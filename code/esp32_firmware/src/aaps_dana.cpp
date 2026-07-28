/**
 * aaps_dana.cpp — AAPS Dana-i (BLE) impersonation 实现
 *
 * 上半部分（恒编译）：纯协议逻辑，逐字移植 AAPS `pump/danars` 的
 *   BleEncryption.kt / BLEComm.kt。无 NimBLE / FreeRTOS 依赖，可在宿主用 g++ 单测。
 *
 * 下半部分（仅 #ifdef USE_AAPS_DANA，ESP32 固件）：NimBLE GATT 胶水层
 *   （FFF0/FFF1/FFF2 服务、收包重组、命令分发到 motor/basal，状态合成）。
 *
 * ⚠️ 实验项目，禁止用于人体。
 */
#include "aaps_dana.h"

#include <string.h>

/* ============================================================
 * BLE5 二级加密查表（AAPS bleEncryptionMatrix，100 字节）
 * 顺序必须与 AAPS 完全一致，否则二级加密字节不兼容 → AAPS 断开。
 * ============================================================ */
const uint8_t DANA_BLE5_MATRIX[100] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,
    0x67,0x2b,0xfe,0xd7,0xab,0x76,0x6c,0x70,0x48,0x50,
    0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,
    0x9d,0x84,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,
    0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x47,0xf1,
    0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,
    0xaa,0x18,0xbe,0x1b,0x09,0x83,0x2c,0x1a,0x1b,0x6e,
    0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0xa0,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,
    0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xb0,0x54,0xbb,0x16
};

/* ============================================================
 * 上下文初始化
 * ============================================================ */
void dana_ctx_init(dana_ctx_t *c, const char *devname, const char *ble5key,
                   uint8_t hw_model, uint8_t protocol)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < 10 && devname[i]; i++) c->device_name[i] = devname[i];
    c->device_name[10] = 0;
    for (int i = 0; i < 6; i++) c->ble5_key[i] = (uint8_t)ble5key[i];
    /* setBle5Key: kN = matrix[(key[2N]-'0')*10 + (key[2N+1]-'0')] */
    c->ble5_enc_key[0] = DANA_BLE5_MATRIX[(ble5key[0] - '0') * 10 + (ble5key[1] - '0')];
    c->ble5_enc_key[1] = DANA_BLE5_MATRIX[(ble5key[2] - '0') * 10 + (ble5key[3] - '0')];
    c->ble5_enc_key[2] = DANA_BLE5_MATRIX[(ble5key[4] - '0') * 10 + (ble5key[5] - '0')];
    c->hw_model = hw_model;
    c->protocol = protocol;
    c->sec   = DANA_SEC_BLE5;
    c->conn  = DANA_CONN_INIT;
}

/* ============================================================
 * 自定义 Dana CRC-16（移植自 BleEncryption.generateCrc）
 * ============================================================ */
uint16_t dana_crc16(const uint8_t *data, size_t len, dana_sec_t sec, dana_conn_state_t conn)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t  b = data[i];
        uint16_t result = (uint16_t)((crc << 8) | (crc >> 8));   // 16 位字节交换
        result ^= b;
        result ^= (uint16_t)((result & 0xFF) >> 4);
        result ^= (uint16_t)((result << 12) & 0xFFFF);
        uint8_t  low = (uint8_t)(result & 0xFF);
        if (sec == DANA_SEC_BLE5 && conn == DANA_CONN_HANDSHAKE_DONE) {
            /* BLE5 命令阶段分支 */
            result ^= (uint16_t)((uint16_t)(low << 4) | (uint16_t)((low >> 3) << 2));
        } else {
            /* 默认 / RSv3(conn0,1) 分支 */
            result ^= (uint16_t)((uint16_t)(low << 3) | (uint16_t)((low >> 2) << 5));
        }
        crc = (uint16_t)(result & 0xFFFF);
    }
    return crc;
}

/* ============================================================
 * 设备名一级混淆（encodeArrayBySn）
 * 对 bytes[3 .. size-3] 按 codingBytes[i%3] XOR（收发各一次，自逆）
 * ============================================================ */
void dana_encode_array_by_sn(uint8_t *buf, size_t size, const char *devname)
{
    uint8_t coding[3] = {0, 0, 0};
    for (int i = 0; i < 10; i++) {
        uint8_t ch = (uint8_t)devname[i];
        if (i < 3)       coding[0] = (uint8_t)(coding[0] + ch);
        else if (i < 8)  coding[1] = (uint8_t)(coding[1] + ch);
        else             coding[2] = (uint8_t)(coding[2] + ch);
    }
    for (size_t i = 0; i + 5 < size; i++) {   // i: 0 .. size-6 → buf[i+3] 至 buf[size-3]
        buf[i + 3] ^= coding[i % 3];
    }
}

/* ============================================================
 * BLE5 二级加密 / 解密（enhancedEncryption == 2）
 * 整包（含已被替换的 AA/EE 标记）逐字节处理。解密后不还原 AA/EE。
 * ============================================================ */
static inline uint8_t dana_switch_lo_hi(uint8_t b)
{
    return (uint8_t)(((b >> 4) & 0x0F) | ((b << 4) & 0xF0));
}

void dana_encrypt_second_level(uint8_t *buf, size_t size, const uint8_t key[3])
{
    if (buf[0] == DANA_START_PACKET && buf[1] == DANA_START_PACKET) {
        buf[0] = DANA_ENC_START; buf[1] = DANA_ENC_START;
    }
    if (size >= 2 && buf[size - 2] == DANA_END_PACKET && buf[size - 1] == DANA_END_PACKET) {
        buf[size - 2] = DANA_ENC_END; buf[size - 1] = DANA_ENC_END;
    }
    for (size_t i = 0; i < size; i++) {
        uint8_t b = buf[i];
        b = (uint8_t)(b + key[0]);
        b = dana_switch_lo_hi(b);
        b = (uint8_t)(b - key[1]);
        b = (uint8_t)(b ^ key[2]);
        buf[i] = b;
    }
}

void dana_decrypt_second_level(uint8_t *buf, size_t size, const uint8_t key[3])
{
    for (size_t i = 0; i < size; i++) {
        uint8_t b = buf[i];
        b = (uint8_t)(b ^ key[2]);
        b = (uint8_t)(b + key[1]);
        b = dana_switch_lo_hi(b);
        b = (uint8_t)(b - key[0]);
        buf[i] = b;
    }
    /* 注意：BLE5 下不还原 AA/EE，解析器同时接受 A5/A5 与 AA/AA 起、5A/5A 与 EE/EE 止 */
}

/* ============================================================
 * 信封构建 / 解析
 * ============================================================ */
int dana_build_packet(dana_ctx_t *c, uint8_t *out, size_t *out_len,
                      uint8_t type, uint8_t opcode,
                      const uint8_t *params, uint8_t nparams,
                      int apply_ble5)
{
    size_t size = 9u + (size_t)nparams;
    if (size > DANA_MAX_PACKET) return -1;

    out[0] = DANA_START_PACKET;
    out[1] = DANA_START_PACKET;
    out[2] = (uint8_t)(2u + nparams);           // LEN = 2 + nParams
    out[3] = type;
    out[4] = opcode;
    for (uint8_t i = 0; i < nparams; i++) out[5u + i] = params[i];

    /* CRC 计算区间 = bytes[3 .. 3+LEN) = [TYPE][OPCODE][PARAMS]（LEN 字节） */
    dana_conn_state_t crcconn = apply_ble5 ? DANA_CONN_HANDSHAKE_DONE : DANA_CONN_INIT;
    uint16_t crc = dana_crc16(&out[3], 2u + nparams, c->sec, crcconn);
    out[5u + nparams]     = (uint8_t)(crc >> 8);
    out[5u + nparams + 1] = (uint8_t)(crc & 0xFF);
    out[5u + nparams + 2] = DANA_END_PACKET;
    out[5u + nparams + 3] = DANA_END_PACKET;

    dana_encode_array_by_sn(out, size, c->device_name);
    if (apply_ble5) dana_encrypt_second_level(out, size, c->ble5_enc_key);

    *out_len = size;
    return 0;
}

/* 内部解包核心：假定 in 已为明文（二级解密由调用方完成）。
 * 仅做：设备名 XOR 解码 + 起止标记/长度校验 + CRC 校验 + 字段抽取。
 * 供 dana_parse_packet（外部信封，先解密再调它）与 dana_feed_rx（接收时已在分片边界解密）复用，
 * 避免二级解密被重复执行（重复解密会让字节变成乱码）。 */
int dana_unpack_packet(dana_ctx_t *c, const uint8_t *in, size_t in_len,
                       uint8_t *out_type, uint8_t *out_opcode,
                       uint8_t *out_params, size_t params_cap, size_t *out_nparams)
{
    if (in_len < 7 || in_len > DANA_MAX_PACKET) return -1;

    uint8_t buf[DANA_MAX_PACKET];
    memcpy(buf, in, in_len);
    size_t size = in_len;

    /* 设备名 XOR 解码（自逆） */
    dana_encode_array_by_sn(buf, size, c->device_name);

    /* 起始/结束标记宽松校验（接受 A5/A5 与 AA/AA；5A/5A 与 EE/EE） */
    int ok_start = (buf[0]==DANA_START_PACKET && buf[1]==DANA_START_PACKET) ||
                   (buf[0]==DANA_ENC_START   && buf[1]==DANA_ENC_START);
    int ok_end   = (buf[size-2]==DANA_END_PACKET && buf[size-1]==DANA_END_PACKET) ||
                   (buf[size-2]==DANA_ENC_END     && buf[size-1]==DANA_ENC_END);
    if (!ok_start || !ok_end) return -2;

    uint8_t len = buf[2];
    if ((size_t)len + 7u != size) return -2;

    /* CRC 校验（分支随 conn 自动选择） */
    dana_conn_state_t crcconn = (c->conn == DANA_CONN_HANDSHAKE_DONE)
                                ? DANA_CONN_HANDSHAKE_DONE : DANA_CONN_INIT;
    uint16_t crc = dana_crc16(&buf[3], len, c->sec, crcconn);
    if (buf[size - 4] != (uint8_t)(crc >> 8) || buf[size - 3] != (uint8_t)(crc & 0xFF))
        return -3;

    *out_type    = buf[3];
    *out_opcode  = buf[4];
    size_t n = (size_t)len - 2u;                 // 参数个数
    size_t copy = (n > params_cap) ? params_cap : n;
    for (size_t i = 0; i < copy; i++) out_params[i] = buf[5u + i];
    *out_nparams = n;
    return 0;
}

int dana_parse_packet(dana_ctx_t *c, const uint8_t *in, size_t in_len,
                      uint8_t *out_type, uint8_t *out_opcode,
                      uint8_t *out_params, size_t params_cap, size_t *out_nparams)
{
    if (in_len < 7 || in_len > DANA_MAX_PACKET) return -1;

    uint8_t buf[DANA_MAX_PACKET];
    memcpy(buf, in, in_len);
    size_t size = in_len;

    /* 命令阶段（握手完成）先整体二级解密（解外部加密信封信）；握手阶段不解密。
     * 对齐 AAPS BLEComm.readDataParsing：先 decryptSecondLevelPacket 再 addToReadBuffer/重组。 */
    if (c->conn == DANA_CONN_HANDSHAKE_DONE)
        dana_decrypt_second_level(buf, size, c->ble5_enc_key);

    return dana_unpack_packet(c, buf, size, out_type, out_opcode,
                              out_params, params_cap, out_nparams);
}

/* ============================================================
 * 握手响应构造（握手阶段，不做二级加密）
 * ============================================================ */
int dana_build_pump_check_response(dana_ctx_t *c, uint8_t *out, size_t *out_len)
{
    /* 解密后 14B = [0x02][0x00]['O']['K'][?][HW_MODEL][?][PROTOCOL][key×6] */
    uint8_t params[12] = {
        'O', 'K', 0x00, c->hw_model, 0x00, c->protocol,
        c->ble5_key[0], c->ble5_key[1], c->ble5_key[2],
        c->ble5_key[3], c->ble5_key[4], c->ble5_key[5]
    };
    return dana_build_packet(c, out, out_len, DANA_TYPE_ENC_RESP, DANA_OP_PUMP_CHECK,
                             params, 12, 0);
}

int dana_build_time_info_response(dana_ctx_t *c, uint8_t *out, size_t *out_len)
{
    /* 解密后 4B = [0x02][0x01]['O']['K']；AAPS 收到即置 isConnected=true */
    uint8_t params[2] = { 'O', 'K' };
    return dana_build_packet(c, out, out_len, DANA_TYPE_ENC_RESP, DANA_OP_TIME_INFO,
                             params, 2, 0);
}

/* ============================================================
 * ESP32 / NimBLE 胶水层（仅固件构建开启 USE_AAPS_DANA）
 * ============================================================ */
#ifdef USE_AAPS_DANA

#include <NimBLEDevice.h>
#include "pump_state.h"
#include "motor_controller.h"
#include "basal_scheduler.h"
#include "rtc_clock.h"   // 0x70/0x71 时间同步

/* FFF0 / FFF1 / FFF2 服务与特征 UUID（标准 Dana BLE GATT） */
static const uint8_t U_FFF0[16] = {0x00,0x00,0xff,0xf0, 0x00,0x00,0x10,0x00, 0x80,0x00, 0x00,0x80,0x5f,0x9b,0x34,0xfb};
static const uint8_t U_FFF1[16] = {0x00,0x00,0xff,0xf1, 0x00,0x00,0x10,0x00, 0x80,0x00, 0x00,0x80,0x5f,0x9b,0x34,0xfb};
static const uint8_t U_FFF2[16] = {0x00,0x00,0xff,0xf2, 0x00,0x00,0x10,0x00, 0x80,0x00, 0x00,0x80,0x5f,0x9b,0x34,0xfb};

static dana_ctx_t            g_dana_ctx;
static NimBLECharacteristic *g_dana_fff1 = nullptr;
static uint8_t               g_rxbuf[128];
static size_t                g_rxlen = 0;

/* 将已构建好的包（可能已二级加密）通过 FFF1 通知发出，>20B 自动分包 */
static void dana_send_raw(const uint8_t *data, size_t len)
{
    if (!g_dana_fff1) return;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > DANA_MTU_CHUNK) chunk = DANA_MTU_CHUNK;
        g_dana_fff1->setValue(data + off, (uint16_t)chunk);
        g_dana_fff1->notify();
        off += chunk;
    }
}

/* 构建并发送一个响应/通知包 */
static void dana_send_packet(uint8_t type, uint8_t opcode,
                             const uint8_t *params, uint8_t nparams, int ble5)
{
    uint8_t buf[DANA_MAX_PACKET];
    size_t  bl = 0;
    if (dana_build_packet(&g_dana_ctx, buf, &bl, type, opcode, params, nparams, ble5) == 0)
        dana_send_raw(buf, bl);
}

/* 泵→手机主动通知（大剂量进度/完成/报警）。命令阶段走 BLE5。 */
void aaps_dana_send_notify(uint8_t notify_opcode, const uint8_t *params, uint8_t nparams)
{
    dana_send_packet(DANA_TYPE_NOTIFY, notify_opcode, params, nparams, 1);
}

/* ---- 0x48 CGM 误解析已移除 (2026-07-28) ----
 * 经 git 克隆 AAPS master (pump/danars) 逐字节核对: 0x48 真实语义是
 * SET_DUAL_BOLUS (双波大剂量), AAPS 不通过 DanaRS 协议下发实时 CGM 血糖。
 * CGM 屏幕显示请走自定义 BLE 通道 g_ch_cgm (ble_comm.cpp)。 */

/* ---- DanaRS 0x71 设置泵时间 (AAPS → 泵) ----
 * 布局依据 DanaRS 协议 DanaRS_Packet_APS_Time:
 *   params[0] : 年 (year - 2000, 1 字节)
 *   params[1] : 月 (1-12)   params[2] : 日 (1-31)
 *   params[3] : 时 (0-23)   params[4] : 分 (0-59)  params[5] : 秒 (0-59)
 * ⚠️ 若你的 AAPS 版本发送 2 字节完整年或不同顺序, 请调整此处。 */
static void dana_apply_time(const uint8_t *p, size_t n)
{
    if (n < 6) return;
    int y  = (int)p[0] + 2000;
    int mo = (int)p[1];
    int d  = (int)p[2];
    int h  = (int)p[3];
    int mi = (int)p[4];
    int s  = (int)p[5];
    uint32_t u = rtc_ymdhms_to_unix(y, mo, d, h, mi, s);
    if (u != 0) rtc_set_unix(u);
}

/* 命令分发：构造命令响应并发送；打药动作接入既有 motor/basal 入口 */
int aaps_dana_handle_command(dana_ctx_t *c, uint8_t opcode,
                             const uint8_t *params, size_t nparams)
{
    uint8_t resp[18];
    uint8_t rn = 1;
    resp[0] = 0x00;  // 默认 OK

    switch (opcode) {
        case DANA_CMD_STEP_BOLUS_START: {  /* 0x4A 步进大剂量 */
            if (nparams >= 3) {
                uint16_t amt = (uint16_t)(params[0] | ((uint16_t)params[1] << 8)); // U×100
                uint8_t  speed = params[2];
                (void)speed;  // 注射速度映射（教学原型暂用默认）
                motor_command_t cmd{};
                cmd.type = MOTOR_CMD_BOLUS;
                cmd.units_x100 = amt;
                motor_enqueue(&cmd);
            }
            break;
        }
        case DANA_CMD_STEP_BOLUS_STOP:    /* 0x44 */
            motor_cancel_bolus();
            break;

        case DANA_CMD_APS_TBR:           /* 0xC1 闭环临时基础率 */
        case DANA_CMD_SET_TBR: {         /* 0x60 手动临时基础率 */
            if (nparams >= 3) {
                uint16_t pct = (uint16_t)(params[0] | ((uint16_t)params[1] << 8)); // 0–500
                uint8_t  durCode = params[2];
                uint32_t dur_min = (durCode == 150u) ? 15u : (durCode == 160u) ? 30u : 0u;
                float ref = (g_pump_state.current_basal_rate > 0.0f)
                            ? g_pump_state.current_basal_rate : 0.5f;
                g_pump_state.tbr_percent = (float)pct;
                g_pump_state.tbr_rate    = (float)pct / 100.0f * ref;   // 绝对速率 U/h
                g_pump_state.tbr_expiry_ms = millis() + dur_min * 60000UL;
            }
            break;
        }
        case DANA_CMD_CANCEL_TBR:        /* 0x62 */
            g_pump_state.tbr_percent = 0;
            g_pump_state.tbr_rate    = 0;
            g_pump_state.tbr_expiry_ms = 0;
            break;

        case DANA_CMD_EXT_BOLUS:         /* 0x47 方波大剂量 */
            if (nparams >= 3) {
                uint16_t amt = (uint16_t)(params[0] | ((uint16_t)params[1] << 8)); // U×100
                uint8_t  halfHours = params[2];                                    // 1–16 → 0.5–8h
                float duration_h = (float)halfHours * 0.5f;
                basal_scheduler_start_extended_bolus((float)amt / 100.0f, duration_h, 0);
            }
            break;
        case DANA_CMD_EXT_BOLUS_CANCEL:  /* 0x49 */
            basal_scheduler_cancel_extended_bolus();
            break;

        case DANA_CMD_INITIAL_SCREEN: {  /* 0x02 状态查询（17B） */
            uint16_t daily = (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF);
            uint16_t maxd  = (uint16_t)(RESERVOIR_CAPACITY_U * 100u);
            uint16_t resv  = (uint16_t)(g_pump_state.reservoir_units_left * 100u);
            uint16_t basal = (uint16_t)(g_pump_state.current_basal_rate * 100.0f);
            uint16_t iob   = (uint16_t)(g_pump_state.iob_x10000 / 100u);
            resp[0] = 0x00;                                  // status: 未暂停/无 TBR 标志位
            resp[1] = (uint8_t)(daily & 0xFF); resp[2] = (uint8_t)(daily >> 8);
            resp[3] = (uint8_t)(maxd & 0xFF);  resp[4] = (uint8_t)(maxd >> 8);
            resp[5] = (uint8_t)(resv & 0xFF);  resp[6] = (uint8_t)(resv >> 8);
            resp[7] = (uint8_t)(basal & 0xFF); resp[8] = (uint8_t)(basal >> 8);
            resp[9] = (uint8_t)g_pump_state.tbr_percent;
            resp[10] = g_pump_state.battery_pct;
            resp[11] = 0x00; resp[12] = 0x00;               // 方波绝对速率（简化）
            resp[13] = (uint8_t)(iob & 0xFF); resp[14] = (uint8_t)(iob >> 8);
            resp[15] = 0x00;                                 // 错误状态
            resp[16] = 0x00;                                 // 保留
            rn = 17;
            break;
        }
        case DANA_CMD_DAILY_TOTAL: {     /* 0x26 今日总量（2B） */
            uint16_t daily = (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF);
            resp[0] = (uint8_t)(daily & 0xFF); resp[1] = (uint8_t)(daily >> 8);
            rn = 2;
            break;
        }
        case DANA_CMD_BOLUS_INFO:        /* 0x40 大剂量进度 */
            break;

        case DANA_CMD_SET_DUAL_BOLUS:    /* 0x48 双波大剂量: 教学原型未实现, 返回 OK */
            break;

        case DANA_CMD_SET_TIME:          /* 0x71 AAPS 下发时间同步 */
            dana_apply_time(params, nparams);
            break;

        case DANA_CMD_GET_TIME: {        /* 0x70 AAPS 读取泵时间 */
            uint32_t u = rtc_unix_now();
            int y, mo, d, h, mi, s;
            if (u == 0) { y = 2000; mo = 1; d = 1; h = 0; mi = 0; s = 0; }
            else rtc_unix_to_ymdhms(u, &y, &mo, &d, &h, &mi, &s);
            resp[0] = (uint8_t)(y - 2000);
            resp[1] = (uint8_t)mo; resp[2] = (uint8_t)d;
            resp[3] = (uint8_t)h;  resp[4] = (uint8_t)mi; resp[5] = (uint8_t)s;
            rn = 6;
            break;
        }
        default:
            /* 其余未识别命令: 教学原型返回 OK */
            rn = 1;
            break;
    }

    /* 命令响应 TYPE=RESPONSE(0xB2)，命令阶段走 BLE5 二级加密 */
    dana_send_packet(DANA_TYPE_RESPONSE, opcode, resp, rn, 1);
    return 0;
}

/* 收包重组 + 分发 */
static void dana_dispatch(dana_ctx_t *c, uint8_t type, uint8_t opcode,
                          const uint8_t *params, size_t nparams)
{
    if (type == DANA_TYPE_ENC_REQ) {
        if (opcode == DANA_OP_PUMP_CHECK) {
            uint8_t resp[DANA_MAX_PACKET]; size_t rl = 0;
            dana_build_pump_check_response(c, resp, &rl);
            c->conn = DANA_CONN_PUMP_CHECK;     // 仍走默认 CRC 分支
            dana_send_raw(resp, rl);
        } else if (opcode == DANA_OP_TIME_INFO) {
            uint8_t resp[DANA_MAX_PACKET]; size_t rl = 0;
            dana_build_time_info_response(c, resp, &rl);
            c->conn = DANA_CONN_HANDSHAKE_DONE; // 进入命令阶段（BLE5 二级加密启用）
            g_pump_state.dana_paired = true;    // 闭环页据此显示"AAPS 已接管"
            dana_send_raw(resp, rl);
        }
        /* BLE5 简化流程不含 passkey 交换，其余加密请求忽略 */
    } else if (type == DANA_TYPE_COMMAND) {
        aaps_dana_handle_command(c, opcode, params, nparams);
    }
}

static void dana_feed_rx(const uint8_t *data, size_t len)
{
    /* 接收边界先整体二级解密（对齐 AAPS BLEComm.readDataParsing：
     * 先 decryptSecondLevelPacket 再 addToReadBuffer/重组）。
     * 原因：BLE5 二级加密覆盖整包（含 LEN 字节与起止标记），若不先解密，
     * 重组阶段读到的 LEN 是乱码，永远凑不齐一个完整包，命令到不了 dana_dispatch。
     * 逐字节变换与位置无关（仅首尾标记经替换后亦可由逆运算还原），
     * 故按 BLE 分片逐片解密再拼接是正确且等价的。 */
    uint8_t chunk[DANA_MAX_PACKET];
    size_t  cl = (len > sizeof(chunk)) ? sizeof(chunk) : len;
    memcpy(chunk, data, cl);
    if (g_dana_ctx.conn == DANA_CONN_HANDSHAKE_DONE)
        dana_decrypt_second_level(chunk, cl, g_dana_ctx.ble5_enc_key);

    if (g_rxlen + cl > sizeof(g_rxbuf)) g_rxlen = 0;  // 溢出保护
    memcpy(g_rxbuf + g_rxlen, chunk, cl);
    g_rxlen += cl;

    while (g_rxlen >= 7) {
        /* 查找起始标记（解密后应为 A5 A5；容错也接受 AA AA） */
        size_t i = 0; int found = 0;
        for (; i + 1 < g_rxlen; i++) {
            if ((g_rxbuf[i] == DANA_START_PACKET && g_rxbuf[i+1] == DANA_START_PACKET) ||
                (g_rxbuf[i] == DANA_ENC_START    && g_rxbuf[i+1] == DANA_ENC_START)) {
                found = 1; break;
            }
        }
        if (!found) { g_rxlen = 0; return; }
        if (i > 0) { memmove(g_rxbuf, g_rxbuf + i, g_rxlen - i); g_rxlen -= i; }

        /* 此时 LEN 已解密为明文，可正确判断是否收齐分包 */
        uint8_t plen = g_rxbuf[2];
        size_t  total = (size_t)plen + 7u;
        if (total > g_rxlen) return;       // 等待后续分片

        uint8_t pkt[DANA_MAX_PACKET];
        memcpy(pkt, g_rxbuf, total);
        memmove(g_rxbuf, g_rxbuf + total, g_rxlen - total);
        g_rxlen -= total;

        uint8_t type, opcode, params[48];
        size_t  nparams = 0;
        /* 注意：分片已在接收边界解密，此处用 dana_unpack_packet 避免二次解密 */
        int r = dana_unpack_packet(&g_dana_ctx, pkt, total, &type, &opcode,
                                   params, sizeof(params), &nparams);
        if (r == 0) dana_dispatch(&g_dana_ctx, type, opcode, params, nparams);
    }
}

/* FFF2 写回调（手机→泵命令） */
class DanaChCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
        std::string v = c->getValue();
        if (!v.empty()) dana_feed_rx((const uint8_t *)v.data(), v.size());
    }
};

/* 连接回调：断连后清理握手状态（与现有自定义 BLE 行为一致） */
class DanaSrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, NimBLEConnInfo &) override {
        /* 不覆盖 g_pump_state.ble_connected（自定义 BLE 已管理） */
    }
    void onDisconnect(NimBLEServer *) override {
        g_dana_ctx.conn = DANA_CONN_INIT;   // 重置握手状态机
        g_rxlen = 0;
    }
};

static DanaChCb  g_dana_ch_cb;
static DanaSrvCb g_dana_srv_cb;

/* 在已有 NimBLEServer 上挂载 Dana FFF0/FFF1/FFF2 服务 */
void aaps_dana_attach(void *server)
{
    NimBLEServer *srv = (NimBLEServer *)server;
    dana_ctx_init(&g_dana_ctx, DANAI_DEVICE_NAME, DANAI_BLE5_KEY,
                  DANAI_HW_MODEL, DANAI_PROTOCOL);

    srv->setCallbacks(&g_dana_srv_cb);

    NimBLEService *svc = srv->createService(NimBLEUUID((uint8_t *)U_FFF0, 16));

    /* FFF1：泵→手机（READ + NOTIFY），响应/通知通道 */
    g_dana_fff1 = svc->createCharacteristic(NimBLEUUID((uint8_t *)U_FFF1, 16),
                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    /* FFF2：手机→泵（WRITE，无响应写） */
    NimBLECharacteristic *fff2 = svc->createCharacteristic(NimBLEUUID((uint8_t *)U_FFF2, 16),
                                                          NIMBLE_PROPERTY::WRITE);
    fff2->setCallbacks(&g_dana_ch_cb);

    svc->start();
}

/* ============================================================
 * 主机联调测试钩子 (仅 AAPS_DANA_HOST_TEST 编译期启用)
 * 暴露固件内部的 dana_feed_rx, 让 AAPS 模拟器把 BLE 字节流直接喂入
 * 真实的收包重组 + 命令分发路径 (等价于 NimBLE FFF2 写回调)。
 * ============================================================ */
#ifdef AAPS_DANA_HOST_TEST
extern "C" void aaps_dana_feed_rx_test(const uint8_t *data, size_t len)
{
    dana_feed_rx(data, len);
}

/* 复位固件握手状态机 (模拟断连), 供交互模式重新握手 */
extern "C" void aaps_dana_reset_test(void)
{
    g_dana_ctx.conn = DANA_CONN_INIT;
    g_rxlen = 0;
}
#endif

#endif /* USE_AAPS_DANA */
