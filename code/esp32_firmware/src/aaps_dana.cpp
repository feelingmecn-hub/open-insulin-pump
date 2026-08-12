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
#include <Arduino.h>
#include "aaps_dana.h"

#include <string.h>
#include <cstdlib>
#include "basal_history.h"  // P2-10b: AAPS 接管记入基础率执行历史

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
    /* ★ 必须回报**实际拷贝**长度而非原始长度：调用方只能安全访问 copy 个字节。
     * 之前回报原始 n，配合 48B 的 params 数组，会让 0x53(96B) 的处理逻辑越界读栈。 */
    *out_nparams = copy;
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
    /* code 字段(明文偏移[2])=0x01 表示"已配对/已设置连接密码"，AAPS 据此跳过
     * setConnectionPassword 流程直接发 TIME_INFORMATION。若置 0x00，AAPS 3.x 会进入
     * 连接密码交换分支，而本教学原型未实现该命令(L660 忽略)→ AAPS 卡 ~120s 后断开。
     * 偏移对齐依据：泵回 [ 'O']['K'][code][hwModel][0x00][protocol][key×6]，AAPS 解析出
     * 型号=hwModel、协议=protocol、代码=code，与真机 UI 显示(09/0A/00)完全自洽。 */
    uint8_t params[12] = {
        'O', 'K', 0x01, c->hw_model, 0x00, c->protocol,
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
#include "ui_hal.h"      // bolus_kind_t (BOLUS_DUAL 等, 双波大剂量类型标记)
#include "rtc_clock.h"   // 0x70/0x71 时间同步
#include "storage.h"     // AAPS 下发的配置（0x64/0x66/0x53）持久化

/* FFF0 / FFF1 / FFF2 服务与特征 UUID（标准 Dana BLE GATT）
 * ⚠️ 血泪教训（2026-08-08）：绝对不要用 NimBLEUUID(const uint8_t*, 16) 配「大端手写数组」。
 *   NimBLE 的 16 字节构造函数按 BLE 线序（小端 / LSB first）解析，把
 *   {0x00,0x00,0xff,0xf0,...,0x34,0xfb} 解释成 UUID 后会整体翻转，实际注册出来的是
 *   fb349b5f-8000-0080-0010-0000f0ff0000（已用 macOS bleak 连上泵枚举 GATT 证实）。
 *   而 AAPS BLEComm.findCharacteristic() 是对 "0000fff1-0000-1000-8000-00805f9b34fb"
 *   做**字符串精确匹配**，翻转后永远匹配不上 → 零 SUBSCRIBE → 永远"正在连接"。
 *   正解：Dana 用的本就是 16-bit 蓝牙 SIG 短 UUID，直接用 uint16_t 构造，
 *   NimBLE 会自动展开成标准 Base UUID，零字节序风险。 */
#define DANA_UUID_FFF0  ((uint16_t)0xFFF0)
#define DANA_UUID_FFF1  ((uint16_t)0xFFF1)
#define DANA_UUID_FFF2  ((uint16_t)0xFFF2)

static dana_ctx_t            g_dana_ctx;
static NimBLECharacteristic *g_dana_fff1 = nullptr;
static bool g_fff1_subscribed = false;   // AAPS 写 CCC 使能 FFF1 notify 后置 true

/* ★ 多连接定向投递 (2026-08-08 修复)：
 * onConnect 里保持广播以支持「华为系统连接 + AAPS 连接」并存（NimBLE 最多 3 连接）。
 * 但此前 notify() 不带 connHandle → NimBLE 默认广播给**所有**已订阅连接；同时
 * onDisconnect 无条件重置握手状态机。后果：
 *   ① AAPS 发 PUMP_CHECK 后泵的响应可能被投到系统那条连接 → AAPS 收不到 → 30s 超时；
 *   ② 系统那条连接抖动断开，会把 AAPS 正在进行的握手状态机一起清掉。
 * 修复：记录「实际在跑 Dana 协议的那条连接」(写 FFF2 / 订阅 FFF1 的 peer)，
 *       notify 定向发给它；只有它断开才重置握手状态机。
 * 为 NONE 时退化为旧行为（发给所有订阅者），保证首包/兜底不丢。 */
static uint16_t g_dana_peer_conn = BLE_HS_CONN_HANDLE_NONE;

/* ★ AAPS 配置写入持久化 (2026-08-08)：
 * AAPS「设置配置文件」会连发 0x64(方案号) / 0x66(24 段基础率) / 0x53(24×CIR+24×CF)。
 * 此前泵只回 OK 不落盘 → 下次连接读回的还是旧值，AAPS 判定 profile 不一致，
 * 于是「设置配置文件」按钮反复出现，且闭环用的 CIR/ISF 与手机不同步。
 * 落盘走 Preferences(NVS)，单次可能阻塞几十 ms —— 绝不能在 BLE onWrite 回调内做
 * （与 dana_trace flash 擦除同样的教训）。这里只置 dirty，由 loop 上下文的
 * aaps_dana_pump() 合并延迟写入（连发三条命令合并为一次 NVS 写）。 */
static volatile bool     g_dana_cfg_dirty = false;
static volatile uint32_t g_dana_cfg_dirty_ms = 0;
#define DANA_CFG_SAVE_DELAY_MS 1500u

static inline void dana_cfg_mark_dirty(void)
{
    g_dana_cfg_dirty    = true;
    g_dana_cfg_dirty_ms = (uint32_t)millis();
}

/* 异步发送队列：onWrite 回调只入队，aaps_dana_pump() 在 loop 上下文按 MTU 分包 notify。
 * 原因：NimBLE 在 onWrite 的 GAP 事件回调内同步调用 characteristic->notify() 会被栈丢弃
 *       （ATT 通知不能在接收事件处理中并发发起），这是"连上但 AAPS 永远收不到响应、
 *       2 分钟超时断开"的根因。改为异步后，响应在事件循环空闲时发出即可送达。 */
/* ★ 2026-08-10 大剂量「已输注 0.00U」修复：扩容发送队列。
 * 大剂量期间每段都发一次进度 notify（2U≈20 段 → 约 20 个 Rate + 完成包），
 * 旧 8 槽在 BLE 流控/loop 取包稍慢时极易填满，把**关键的大剂量完成 notify 直接丢弃**
 * → AAPS 永远收不到 bolusDone → 超时记 0.00U。扩到 32 槽留足余量。
 * 2026-08-11 再扩到 64：首次连接 AAPS 会一次性回放全部历史事件(0xC2 回放 N 条记录
 * + 1 个 0xFF 结束符)，历史环最多 48 条 → 最多 49 包；若队列 < 49 溢出会丢弃末尾 0xFF
 * 致 AAPS 死等超时。64 槽可安全承载完整首读。 */
#define DANA_TXQ_SLOTS 64
static uint8_t g_txq[DANA_TXQ_SLOTS][DANA_MAX_PACKET];
static size_t  g_txq_len[DANA_TXQ_SLOTS];
static int     g_txq_head = 0, g_txq_tail = 0;
static bool    g_txq_full = false;
/* 收包重组缓冲：最长的写入命令 0x53 = 96B 参数 → 整包 105B，分 6 片到达。
 * 128B 时「未完成的 105B 包 + 下一片 20B」会触发溢出保护丢整包，故扩到 256B。 */
static uint8_t               g_rxbuf[256];
static size_t                g_rxlen = 0;
static uint32_t              g_dana_passkey = 0;   // 蓝牙配对 passkey (= DANAI_BLE5_KEY)，Dana-i 要求系统层配对

/* ===== BLE 握手追踪日志：存 flash(dana_trace 分区 0x3E0000/128KB)，boot 模式 esptool 导出 =====
 * 定长 32B 记录 ring buffer： [ts:4][dir:1][op:1][len:1][st:1][data:24]
 *   dir: 0=RX(手机→泵原始) 1=TX_ENQ(泵入队响应) 2=TX_SENT(泵实际notify) 3=STATUS 4=ERR
 *   op : 命令 opcode（RX/TX 取包内[4]；STATUS 用约定值 0x00=PUMP_CHECK/0x01=TIME_INFO/0xFE=subscribe）
 *   st : 子状态（STATUS: 1=收到; ERR: 错误码 r）
 * 导出: esptool read_flash 0x3E0000 0x20000 dana_trace.bin  → 每 32B 解一条
 *
 * 主机联调构建(AAPS_DANA_HOST_TEST)没有 esp_flash/Preferences，也不需要 trace
 * （主机侧断言直接看内存状态），故整块用空实现替换。 */
#ifdef AAPS_DANA_HOST_TEST
static bool g_trace_ready = true;
static inline void dana_trace_init(void) {}
static inline void dana_trace_log(uint8_t, uint8_t, uint8_t, const uint8_t *, uint8_t) {}
#else
#include <esp_flash.h>
#include <Preferences.h>
#define DANA_TRACE_ADDR   0x3E0000UL
#define DANA_TRACE_SIZE   0x20000UL
#define DANA_TRACE_MAGIC  0x44414E41UL   // "DANA"
typedef struct { uint32_t ts; uint8_t dir; uint8_t op; uint8_t len; uint8_t st; uint8_t data[24]; } dana_trace_rec_t;
static Preferences g_trace_nvs;
static uint32_t    g_trace_off = 0;
static int32_t     g_trace_lastsec = -1;
static bool        g_trace_ready = false;

static void dana_trace_init(void) {
    g_trace_nvs.begin("dana", false);
    g_trace_off = g_trace_nvs.getUInt("toff", 0);
    uint32_t magic = 0;
    if (esp_flash_read(esp_flash_default_chip, &magic, DANA_TRACE_ADDR, 4) != ESP_OK) magic = 0;
    if (magic != DANA_TRACE_MAGIC) {                       // 首次使用：擦整区并写 magic
        for (uint32_t s = 0; s < DANA_TRACE_SIZE; s += 4096)
            esp_flash_erase_region(esp_flash_default_chip, DANA_TRACE_ADDR + s, 4096);
        magic = DANA_TRACE_MAGIC;
        esp_flash_write(esp_flash_default_chip, &magic, DANA_TRACE_ADDR, 4);
        g_trace_off = 4;
        g_trace_nvs.putUInt("toff", g_trace_off);
    }
    g_trace_lastsec = -1;
    g_trace_ready = true;
}

static void dana_trace_log(uint8_t dir, uint8_t op, uint8_t st, const uint8_t *data, uint8_t len) {
    if (!g_trace_ready) return;   // 未初始化(loop 尚未 init)则不记，避免 BLE 回调里擦 flash 阻塞握手
    if (g_trace_off + sizeof(dana_trace_rec_t) > DANA_TRACE_ADDR + DANA_TRACE_SIZE) return;  // 满则停
    dana_trace_rec_t r;
    r.ts = (uint32_t)millis();
    r.dir = dir; r.op = op; r.st = st;
    r.len = (len > 24) ? 24 : len;
    memset(r.data, 0, 24);
    if (data && r.len) memcpy(r.data, data, r.len);
    uint32_t phys = DANA_TRACE_ADDR + g_trace_off;
    uint32_t sec = phys & ~0xFFFUL;
    if ((int32_t)sec != g_trace_lastsec) {                 // 跨入新扇区则先擦（flash 不能覆写）
        esp_flash_erase_region(esp_flash_default_chip, sec, 4096);
        g_trace_lastsec = (int32_t)sec;
    }
    esp_flash_write(esp_flash_default_chip, &r, phys, sizeof(r));
    g_trace_off += sizeof(r);
    g_trace_nvs.putUInt("toff", g_trace_off);
}
#endif /* AAPS_DANA_HOST_TEST */

/* 调试：十六进制打印收发字节（接 USB CDC 串口可见） */
static void dana_dbg_hex(const char *tag, const uint8_t *d, size_t n) {
    Serial.printf("[DANA] %s (%d): ", tag, (int)n);
    for (size_t i = 0; i < n; i++) Serial.printf("%02X ", d[i]);
    Serial.println();
}

/* 将已构建好的包（可能已二级加密）通过 FFF1 通知发出，>20B 自动分包 */
static void dana_send_raw(const uint8_t *data, size_t len)
{
    if (!g_dana_fff1) return;
    dana_dbg_hex("TX(enq)", data, len);
    dana_trace_log(1, (len > 4 ? data[4] : 0), 0, data, (uint8_t)len);   // TX 入队（泵响应）
    if (g_txq_full) { Serial.println("[DANA] TXQ full, drop"); return; }
    int idx = g_txq_head;
    size_t n = (len > DANA_MAX_PACKET) ? DANA_MAX_PACKET : len;
    memcpy(g_txq[idx], data, n);
    g_txq_len[idx] = n;
    g_txq_head = (g_txq_head + 1) % DANA_TXQ_SLOTS;
    if (g_txq_head == g_txq_tail) g_txq_full = true;
}

/* 在 loop() 上下文调用：把队列中的包按 MTU 分包 notify 发出（需 FFF1 已订阅或已连接）。 */
void aaps_dana_pump(void)
{
    if (!g_dana_fff1) return;
    if (!g_trace_ready) dana_trace_init();   // loop 空闲时初始化(含整区擦除)，不在 BLE 回调里阻塞
    /* 订阅门控：AAPS 写 CCC 使能 FFF1 notify 后置 true；兜底——只要已建立 BLE 连接也发
     * （防止 NimBLE onSubscribe 偶发不触发导致队列永不发、再次 2 分钟超时）。未订阅时
     * notify 会被栈静默丢弃，无害。 */
    if (!g_fff1_subscribed && !g_pump_state.ble_connected) return;
    while (g_txq_head != g_txq_tail) {
        int idx = g_txq_tail;
        const uint8_t *data = g_txq[idx];
        size_t len = g_txq_len[idx];
        size_t off = 0;
        while (off < len) {
            size_t chunk = len - off;
            if (chunk > DANA_MTU_CHUNK) chunk = DANA_MTU_CHUNK;
            g_dana_fff1->setValue(data + off, (uint16_t)chunk);
            /* ★ 定向投递给 Dana 协议对端（AAPS），避免多连接下发错连接。
             * g_dana_peer_conn == BLE_HS_CONN_HANDLE_NONE 时等同旧行为(全体订阅者)。 */
            g_dana_fff1->notify(data + off, chunk, g_dana_peer_conn);
            dana_trace_log(2, (len > 4 ? data[4] : 0), 0, data + off, (uint8_t)chunk);  // TX 实际发出
            off += chunk;
        }
        g_txq_tail = (g_txq_tail + 1) % DANA_TXQ_SLOTS;
        g_txq_full = false;
    }

    /* AAPS 下发的配置延迟落盘：响应先发完（AAPS 有 5s 超时），再写 NVS。
     * 延迟 1.5s 合并 0x64+0x66+0x53 连发，避免一次「设置配置文件」写三遍 flash。 */
    if (g_dana_cfg_dirty &&
        (uint32_t)(millis() - g_dana_cfg_dirty_ms) >= DANA_CFG_SAVE_DELAY_MS) {
        g_dana_cfg_dirty = false;
        storage_save_config(&g_pump_config);
        Serial.println("[DANA] AAPS profile persisted to NVS");
        dana_trace_log(3, 0xF9, 1, nullptr, 0);   // 配置落盘
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

/* ===== AAPS 历史事件历史缓冲 (供 0xC2 APS_HISTORY_EVENTS 回放) =====
 * 记录码对齐 DanaPump.HistoryEntry (常量见 aaps_dana.h DANA_HIST_CODE_*):
 *   TEMP_START=1 TEMP_STOP=2 EXTENDED_START=3 EXTENDED_STOP=4 BOLUS=5 DUAL_BOLUS=6
 * Dana-i (hwModel>=7 → usingUTC=true) 按 UTC 11 字节布局解析 (DanaRSPacketAPSHistoryEvents):
 *   [0..1] id(MSB,1-2000 稳定唯一) [2] recordCode [3..6] Unix秒(MSB,4B)
 *   [7..8] param1(MSB,2B) [9..10] param2(MSB,2B)
 * AAPS 用 pumpId = datetime*2 + id 去重, 故同一记录重读须返回相同 id (id 入环存于记录)。
 * 大剂量/方波立即量: param1=量×100; TBR/方波: param1=百分比或量×100, param2=时长(分)。
 * 已接记录钩子: BOLUS(motor完成) TEMP_START/TEMP_STOP(0xC1/0x60/0x62) EXT_START/EXT_STOP(0x47/0x49,含0x48双波延展量)。 */
typedef struct {
    uint8_t  used;
    uint8_t  code;
    uint32_t ts;     // Unix 秒 (UTC)
    uint16_t p1;     // param1(MSB): 大剂量=量×100; TBR=百分比
    uint16_t p2;     // param2(MSB): TBR=时长(分)
    uint16_t id;     // 1..2000 稳定唯一
} dana_hist_t;
#define DANA_HIST_MAX 48
static dana_hist_t g_hist[DANA_HIST_MAX];
static int         g_hist_head = 0;
static uint16_t    g_hist_id = 1;

static void dana_history_add(uint8_t code, uint32_t ts, uint16_t p1, uint16_t p2)
{
    dana_hist_t *h = &g_hist[g_hist_head];
    h->used = 1;
    h->code = code;
    h->ts   = ts;
    h->p1   = p1;
    h->p2   = p2;
    h->id   = g_hist_id;
    g_hist_id = (g_hist_id >= 2000u) ? 1u : (uint16_t)(g_hist_id + 1u);
    g_hist_head = (g_hist_head + 1) % DANA_HIST_MAX;
}

void aaps_dana_record_bolus(uint32_t ts, uint16_t amount_x100)
{
    dana_history_add(DANA_HIST_CODE_BOLUS, ts, amount_x100, 0);
}

void aaps_dana_record_tbr(uint8_t code, uint32_t ts, uint16_t percent, uint16_t dur_min)
{
    dana_history_add(code, ts, percent, dur_min);
}

/* TBR 历史记录钩子包装: 供 ui_hal 在泵菜单设/取 TBR 时回调, 把事件写入 0xC2 回放缓冲。
 * 让 AAPS 也能看到本地菜单设的 TBR (与 BLE 0x60/0xC1 路径共用同一缓冲, 不重复)。 */
static uint32_t dana_local_now(void);   // 前向声明 (定义见下方 ~638 行)
static void aaps_tbr_hist_cb(uint8_t code, uint16_t percent, uint16_t dur_min)
{
    aaps_dana_record_tbr(code, dana_local_now(), percent, dur_min);
}

/* 把 from(UTC 6 字节: 年-2000,月,日,时,分,秒) 之后的记录逐条回放, 末尾补 0xFF。
 * 每条记录作为独立 RESPONSE(0xC2) 包入队(异步发送), AAPS 缓冲后于 0xFF 统一处理。
 * from 为 AAPS 首读的特例 (0,1,1,0,0,0 → 公元2000) 时 from_ts=0, 回放全部。 */
static void dana_history_replay(const uint8_t *from6)
{
    uint32_t from_ts = 0;
    if (from6 && (from6[0] != 0 || from6[1] != 1 || from6[2] != 1 ||
                  from6[3] != 0 || from6[4] != 0 || from6[5] != 0)) {
        from_ts = rtc_ymdhms_to_unix((int)from6[0] + 2000, (int)from6[1], (int)from6[2],
                                     (int)from6[3], (int)from6[4], (int)from6[5]);
    }
    for (int i = 0; i < DANA_HIST_MAX; i++) {
        dana_hist_t *h = &g_hist[i];
        if (!h->used) continue;
        if (h->ts < from_ts) continue;          // 仅回放 from 之后的增量
        uint8_t rec[11];
        rec[0] = (uint8_t)(h->id >> 8);
        rec[1] = (uint8_t)(h->id & 0xFF);
        rec[2] = h->code;
        rec[3] = (uint8_t)(h->ts >> 24);
        rec[4] = (uint8_t)(h->ts >> 16);
        rec[5] = (uint8_t)(h->ts >> 8);
        rec[6] = (uint8_t)(h->ts & 0xFF);
        rec[7] = (uint8_t)(h->p1 >> 8);
        rec[8] = (uint8_t)(h->p1 & 0xFF);
        rec[9] = (uint8_t)(h->p2 >> 8);
        rec[10] = (uint8_t)(h->p2 & 0xFF);
        dana_send_packet(DANA_TYPE_RESPONSE, DANA_CMD_APS_HISTORY_EVENTS, rec, 11, 1);
    }
    uint8_t end[1] = { 0xFF };
    dana_send_packet(DANA_TYPE_RESPONSE, DANA_CMD_APS_HISTORY_EVENTS, end, 1, 1);
}

/* 泵→手机主动通知（大剂量进度/完成/报警）。命令阶段走 BLE5。 */
void aaps_dana_send_notify(uint8_t notify_opcode, const uint8_t *params, uint8_t nparams)
{
    dana_send_packet(DANA_TYPE_NOTIFY, notify_opcode, params, nparams, 1);
}

/* ---- 大剂量进度/完成/报警主动推送 (P1-6 / P1-7) ----
 * 由 motor_controller (大剂量分段打入时) 与 safety_monitor (报警触发时) 调用。
 * g_dana_fff1 为 null (尚未发起 / 模拟器环境) 时 dana_send_raw 自动 no-op。 */
void aaps_notify_bolus_progress(uint16_t delivered_x100)
{
    uint8_t p[2] = { (uint8_t)(delivered_x100 & 0xFF), (uint8_t)(delivered_x100 >> 8) };
    aaps_dana_send_notify(DANA_NOTIFY_DELIVERY_RATE, p, 2);
}

/* ⚠️ DanaRSPacketNotifyDeliveryComplete.handleMessage 读的是 **2 字节**
 *    (byteArrayToInt(getBytes(data, DATA_START, 2)) / 100.0 = 已输注 U)。
 *    这里若只发 1 字节，AAPS 会数组越界抛异常 → 打药进度条卡死/连接异常。 */
void aaps_notify_bolus_complete(void)
{
    uint16_t delivered = (uint16_t)(g_pump_state.bolus_delivered_x100 & 0xFFFF);
    uint8_t p[2] = { (uint8_t)(delivered & 0xFF), (uint8_t)(delivered >> 8) };
    /* ★ 2026-08-10 修复：大剂量完成 notify 重复投递 3 次。
     * AAPS DanaRS 大剂量完成**完全依赖**此 notify（收到即置 bolusDone，
     * bolusingTreatment.insulin 只在收到时更新），BLE 不稳时单次极易丢失 →
     * 超时记「已输注 0.00U」。重复包值相同，AAPS 幂等处理无害；队列已扩到 32 槽
     * 足够容纳。配合进度 notify 也由 motor 每段发送，送达率显著提高。 */
    for (int i = 0; i < 3; i++)
        aaps_dana_send_notify(DANA_NOTIFY_DELIVERY_COMPLETE, p, 2);
}

void aaps_notify_alarm(uint8_t alarm_code)
{
    uint8_t p[1] = { alarm_code };
    aaps_dana_send_notify(DANA_NOTIFY_ALARM, p, 1);
}

/* ---- 0x48 CGM 误解析已移除 (2026-07-28) ----
 * 经 git 克隆 AAPS master (pump/danars) 逐字节核对: 0x48 真实语义是
 * SET_DUAL_BOLUS (双波大剂量), AAPS 不通过 DanaRS 协议下发实时 CGM 血糖。
 * CGM 屏幕显示请走自定义 BLE 通道 g_ch_cgm (ble_comm.cpp)。 */

/* ---- DanaRS 0x71 SET_PUMP_TIME (AAPS → 泵，本地时间) ----
 *   params[0] : 年 (year - 2000)  [1] 月(1-12)  [2] 日(1-31)
 *   params[3] : 时(0-23)          [4] 分(0-59)  [5] 秒(0-59)
 * 泵内 RTC 存的就是「本地墙钟」，直接写入。 */
static void dana_apply_time(const uint8_t *p, size_t n)
{
    if (n < 6) return;
    uint32_t u = rtc_ymdhms_to_unix((int)p[0] + 2000, (int)p[1], (int)p[2],
                                    (int)p[3], (int)p[4], (int)p[5]);
    if (u != 0) rtc_set_unix(u);
}

/* ============================================================
 * 时间/时区：Dana-i (hwModel≥7) 走 0x78/0x79（UTC + 时区偏移）
 * ------------------------------------------------------------
 * AAPS 侧语义（逐字读 DanaRSPacketOptionGetPumpUTCAndTimeZone +
 * DanaPump.setPumpTime(value, zoneOffset) 推导）：
 *   1) 泵回 7B = UTC 的 [年-2000][月][日][时][分][秒][zoneOffset]
 *   2) AAPS 用**手机本地时区**构造 DateTime(...)，得 millis = UTC_epoch - offset*3600
 *   3) setPumpTime 再 +offset*3600 → pumpTime == 真实 UTC epoch
 *   4) timeDiff = pumpTime - System.currentTimeMillis()
 * ⇒ 泵必须返回**真正的 UTC** 年月日时分秒，否则 timeDiff 会整整差一个时区。
 *
 * ⚠️ 致命分支：|timeDiff| > 1.5 小时 → AAPS 弹「泵时间偏差过大」并
 *    danaPump.reset() + return，**不会**自动下发 0x79 纠正，初始化直接失败。
 *    因此 RTC 从未设置时必须给出一个「大致正确」的兜底值 —— 这里用固件
 *    编译时间（__DATE__/__TIME__，即本机构建时的北京时间）。用户只要在
 *    烧录后不久连接就能落进 1.5h 窗口，之后 AAPS 会用 0x79 精确校时。
 * ============================================================ */

/* 固件编译时刻（本地墙钟）→ unix 秒，作为 RTC 未设置时的兜底 */
/* 泵内本地墙钟（unix 秒），RTC 未设置时回退到固件编译时刻。
 * 编译时刻解析已上移到 rtc_clock.h::rtc_build_time_unix()（固件/模拟器共用，
 * rtc_clock_init 的时间下界保护也用同一个值，避免两处实现漂移）。 */
static uint32_t dana_local_now(void)
{
    uint32_t u = rtc_unix_now();
    return u ? u : rtc_build_time_unix();
}

/* LSB-first 写 16 位（Dana 参数区一律小端） */
static inline void dana_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* float U → U×100 的 uint16（四舍五入 + 饱和） */
static inline uint16_t dana_u100(float u)
{
    if (u <= 0.0f) return 0;
    float v = u * 100.0f + 0.5f;
    return (v > 65535.0f) ? 65535u : (uint16_t)v;
}

/* ============================================================
 * 命令分发：构造命令响应并发送；打药动作接入既有 motor/basal 入口
 * ------------------------------------------------------------
 * ⚠️ 断连根因（2026-08-07 定位，务必牢记）：
 *   AAPS `BLEComm.processMessage()` 的顺序是
 *       message.handleMessage(decryptedBuffer)   ← 先解析
 *       message.setReceived()                    ← 后置「已收到」
 *   一旦 handleMessage 因响应过短而数组越界抛异常，setReceived() 永远不会执行，
 *   `BLEComm.sendMessage()` 里 `message.waitMillis(5000)` 超时后就走
 *       disconnect("Reply not received")
 *   —— 表现就是「正在获取泵设置」约 5~6 秒后断开、无限重连。
 *   所以**每个 opcode 的参数区长度必须 ≥ AAPS 解析器读取的字节数**，
 *   未知命令也要回足够长的零填充，绝不能只回 1 字节。
 *
 * AAPS 3.2.x 初始化序列（DanaRSService.readPumpStatus，hwModel=0x09 ⇒
 * usingUTC=true / profile24=true）：
 *   0xFF → 0x20 → 0x21 → 0x63 → 0x50 → 0x67 → 0x4B → 0x52 → 0x72
 *        → 0x02 → 0x40 → 0x78 [→ 0x79 → 0x78] → 0xC2(loadEvents) → 0x02
 * ============================================================ */
int aaps_dana_handle_command(dana_ctx_t *c, uint8_t opcode,
                             const uint8_t *params, size_t nparams)
{
    /* 最长响应 = 0x52 的 97B 参数区 */
    uint8_t resp[104];
    memset(resp, 0, sizeof(resp));
    uint8_t rn = 1;                 // 默认 1B 结果码 0x00 = OK

    /* 当前小时（用于取当日 CIR/CF/目标），RTC 未设置时退化到 0 点 */
    int cy, cmo, cd, ch, cmi, cs;
    rtc_unix_to_ymdhms(dana_local_now(), &cy, &cmo, &cd, &ch, &cmi, &cs);
    const uint8_t prof = (uint8_t)(g_pump_config.active_profile & (MAX_BASAL_PROFILES - 1));

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

        case DANA_CMD_APS_TBR:           /* 0xC1 闭环 TBR (AAPS: [pct 2B LE][durCode u8]; 150→15min 160→30min) */
            if (nparams >= 3) {
                uint16_t pct = (uint16_t)(params[0] | ((uint16_t)params[1] << 8)); // 0–500
                uint8_t  durCode = params[2];
                uint32_t dur_min = (durCode == 150u) ? 15u : 30u;   // 150→15min, 160→30min, 其余默认30
                float ref = (g_pump_state.current_basal_rate > 0.0f)
                            ? g_pump_state.current_basal_rate : 0.5f;
                g_pump_state.tbr_percent = (float)pct;
                g_pump_state.tbr_rate    = (float)pct / 100.0f * ref;   // 绝对速率 U/h (0%→0)
                g_pump_state.tbr_expiry_ms = millis() + dur_min * 60000UL;
                /* 记 TEMP_START 历史, 供 AAPS 闭环读取时落 TBR 治疗账本 */
                aaps_dana_record_tbr(DANA_HIST_CODE_TEMP_START, dana_local_now(),
                                     pct, (uint16_t)dur_min);
            }
            break;
        case DANA_CMD_SET_TBR: {         /* 0x60 手动 TBR (AAPS: [pct u8][durHours u8]) */
            if (nparams >= 2) {
                uint16_t pct = (uint16_t)params[0];                  // 0–255 (u8)
                uint8_t  durHours = params[1];                       // 小时
                uint32_t dur_min = (durHours > 0u) ? (uint32_t)durHours * 60u : 60u;
                float ref = (g_pump_state.current_basal_rate > 0.0f)
                            ? g_pump_state.current_basal_rate : 0.5f;
                g_pump_state.tbr_percent = (float)pct;
                g_pump_state.tbr_rate    = (float)pct / 100.0f * ref;   // 绝对速率 U/h (0%→0)
                g_pump_state.tbr_expiry_ms = millis() + dur_min * 60000UL;
                /* 记 TEMP_START 历史, 供 AAPS 闭环读取时落 TBR 治疗账本 */
                aaps_dana_record_tbr(DANA_HIST_CODE_TEMP_START, dana_local_now(),
                                     pct, (uint16_t)dur_min);
            }
            break;
        }
        case DANA_CMD_CANCEL_TBR:        /* 0x62 */
            g_pump_state.tbr_percent = 0;
            g_pump_state.tbr_rate    = 0;
            g_pump_state.tbr_expiry_ms = 0;
            /* ★ 2026-08-11: 记 TEMP_STOP 历史, 闭环取消 TBR 时 AAPS 能正确结束 TBR 记录 */
            aaps_dana_record_tbr(DANA_HIST_CODE_TEMP_STOP, dana_local_now(), 0, 0);
            break;

        case DANA_CMD_EXT_BOLUS:         /* 0x47 方波大剂量 */
            if (nparams >= 3) {
                uint16_t amt = (uint16_t)(params[0] | ((uint16_t)params[1] << 8)); // U×100
                uint8_t  halfHours = params[2];                                    // 1–16 → 0.5–8h
                float duration_h = (float)halfHours * 0.5f;
                basal_scheduler_start_extended_bolus((float)amt / 100.0f, duration_h, 0);
                /* ★ 2026-08-12: 记 EXT_START 历史, 供 AAPS 落方波治疗账本 */
                aaps_dana_record_tbr(DANA_HIST_CODE_EXT_START, dana_local_now(),
                                     amt, (uint16_t)(halfHours * 30u));
            }
            break;
        case DANA_CMD_EXT_BOLUS_CANCEL:  /* 0x49 */
            basal_scheduler_cancel_extended_bolus();
            /* ★ 2026-08-12: 记 EXT_STOP 历史, AAPS 能正确结束方波记录 */
            aaps_dana_record_tbr(DANA_HIST_CODE_EXT_STOP, dana_local_now(), 0, 0);
            break;

        /* ---- 0x02 INITIAL_SCREEN_INFORMATION → 17B（protocol≥10 读到 [15]） ----
         * status 位：bit0=暂停 bit2=方波中 bit3=双波中 bit4=TBR 中 */
        case DANA_CMD_INITIAL_SCREEN: {
            uint8_t status = 0;
            /* TBR 进行中: 以 tbr_expiry_ms 在未来判定(含 0% low-temp), 否则 AAPS 验证 isTempBasalInProgress 失败 */
            if (millis() < g_pump_state.tbr_expiry_ms) status |= 0x10;
            if (basal_scheduler_extended_bolus_active()) status |= 0x04;
            resp[0]  = status;
            dana_put16(&resp[1],  (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF)); // 今日总量 U×100
            dana_put16(&resp[3],  (uint16_t)(RESERVOIR_CAPACITY_U * 100u));            // 每日上限 U×100
            dana_put16(&resp[5],  dana_u100(g_pump_state.reservoir_units_left));       // 余量 U×100
            dana_put16(&resp[7],  dana_u100(g_pump_state.current_basal_rate));         // 当前基础率 U/h×100
            resp[9]  = (uint8_t)g_pump_state.tbr_percent;
            resp[10] = g_pump_state.battery_pct;
            dana_put16(&resp[11], dana_u100(g_pump_state.tbr_rate));                   // 方波绝对速率（近似）
            dana_put16(&resp[13], (uint16_t)(g_pump_state.iob_x10000 / 100u));         // IOB U×100
            resp[15] = 0x00;                                                            // errorState = NONE
            resp[16] = 0x00;
            rn = 17;
            break;
        }

        /* ---- 0x26 REVIEW__GET_TODAY_DELIVERY_TOTAL → 2B ---- */
        case DANA_CMD_DAILY_TOTAL:
            dana_put16(&resp[0], (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF));
            rn = 2;
            break;

        /* ---- 0x40 BOLUS__GET_STEP_BOLUS_INFORMATION → 11B ----
         * [0]error [1]bolusType [2..3]initialBolusAmount×100 [4]hour [5]min
         * [6..7]lastBolusAmount×100 [8..9]maxBolus×100 [10]bolusStep×100
         * ⚠️ 旧实现只回 3B（被误当成「大剂量进度查询」），AAPS 读到 [10] 必越界。
         *    bolusStep 上报 10 = 0.1U —— 与本机可靠最小剂量一致（剂量诚实性原则），
         *    AAPS 会据此把下发剂量对齐到 0.1U 栅格。 */
        case DANA_CMD_BOLUS_INFO: {
            uint16_t last = (uint16_t)(g_pump_state.bolus_delivered_x100 & 0xFFFF);
            resp[0] = 0x00;                                   // error = 0（非 0 会 failed）
            resp[1] = motor_bolus_active() ? 0x01 : 0x00;     // bolusType
            dana_put16(&resp[2], last);                       // initialBolusAmount
            // ★ 2026-08-11 修复：回填"上次大剂量时刻"必须用真实发生时间, 不能用当前时钟。
            //   旧实现填 dana_local_now() 的时/分, 导致 AAPS 每次读 0x40 都以为"刚打了针"
            //   (时刻永远=now) → 持续触发 bolus snooze 并污染 IOB 判定。last_bolus_time
            //   在大剂量完成时由 motor_controller 写入 (rtc_unix_now, 与 dana_local_now 同源)。
            if (g_pump_state.last_bolus_time != 0) {
                int _y, _mo, _d, _h, _mi, _s;
                rtc_unix_to_ymdhms(g_pump_state.last_bolus_time, &_y, &_mo, &_d, &_h, &_mi, &_s);
                resp[4] = (uint8_t)_h;                        // 最后一次大剂量时刻（时, 真实）
                resp[5] = (uint8_t)_mi;                       //                    （分, 真实）
            } else {
                resp[4] = 0; resp[5] = 0;
            }
            dana_put16(&resp[6], last);                       // lastBolusAmount
            dana_put16(&resp[8], dana_u100(g_pump_config.max_bolus_single));
            resp[10] = 10;                                    // bolusStep = 0.10U
            rn = 11;
            break;
        }

        case DANA_CMD_SET_DUAL_BOLUS:    /* 0x48 双波大剂量 SET_DUAL_BOLUS (DanaRS)
                                          * 布局 (与 AAPS DanaRS_Packet_APS_Set_Dual_Bolus 一致):
                                          *   params[0..1]: 立即量 (U×100, 2B)
                                          *   params[2..3]: 延展量 (U×100, 2B)
                                          *   params[4]   : 时长 (30 分钟单位, 1-16 → 0.5-8h)
                                          * 立即量走一笔常规大剂量 (motor), 延展量交给
                                          * basal_scheduler 按 duration 时间维铺开 (与方波同机制)。 */
            if (nparams >= 5) {
                uint16_t imme = (uint16_t)(params[0] | ((uint16_t)params[1] << 8));
                uint16_t ext  = (uint16_t)(params[2] | ((uint16_t)params[3] << 8));
                uint8_t  halfHours = params[4];
                float duration_h = (float)halfHours * 0.5f;
                if (imme > 0) {
                    motor_command_t cmd{};
                    cmd.type = MOTOR_CMD_BOLUS;
                    cmd.units_x100 = imme;
                    cmd.kind = BOLUS_DUAL;
                    motor_enqueue(&cmd);
                }
                if (ext > 0) {
                    basal_scheduler_start_extended_bolus((float)ext / 100.0f, duration_h, BOLUS_DUAL);
                    /* ★ 2026-08-12: 双波的延展量记 EXT_START 历史 (立即量已走 BOLUS 记录) */
                    aaps_dana_record_tbr(DANA_HIST_CODE_EXT_START, dana_local_now(),
                                         ext, (uint16_t)(halfHours * 30u));
                }
            }
            break;

        case DANA_CMD_SET_TIME:          /* 0x71 AAPS 下发时间同步（本地时区，6B） */
            dana_apply_time(params, nparams);
            resp[0] = 0x00;              // result = OK
            rn = 1;
            break;

        case DANA_CMD_GET_TIME: {        /* 0x70 AAPS 读取泵时间（本地时区）→ 6B */
            resp[0] = (uint8_t)(cy - 2000);
            resp[1] = (uint8_t)cmo; resp[2] = (uint8_t)cd;
            resp[3] = (uint8_t)ch;  resp[4] = (uint8_t)cmi; resp[5] = (uint8_t)cs;
            rn = 6;
            break;
        }

        /* ---- 0x78 OPTION__GET_PUMP_UTC_AND_TIME_ZONE → 7B ----
         * AAPS 接收端 (GetPumpUTCAndTimeZone.kt:29) 用 Joda DateTime(2000+y, m, d, h, mi, s)
         * **以手机本地时区**解释泵回的墙钟得到 value(毫秒), 再 DanaPump.setPumpTime
         * (kt:79-85) 把 value 加回**手机本地 offset** 得到 pumpTime。
         * ⇒ 这里必须回 **纯 UTC 墙钟**(= 系统时钟本身), 不要减也不要加 offset。
         *   例: 系统时钟 08:12 UTC → 回 08:12 + offset(8); AAPS 用北京解释 08:12 → 00:12 UTC,
         *       再加 8h → 08:12 UTC = 手机 System.currentTimeMillis → timeDiff≈0。
         *   (若此处回本地墙钟 16:12, AAPS 解释 16:12→08:12 UTC, 再加 8h→16:12 UTC, 反而差 +8h。)
         * ⚠️ month/day 为 0 会让 Joda DateTime 抛 IllegalFieldValueException,
         *    进而 setReceived() 不执行 → 5 秒超时断开。dana_local_now() 带兜底保证有效。 */
        case DANA_CMD_GET_UTC_TZ: {
            uint32_t utc = dana_local_now();   // 纯 UTC 秒(未设置时兜底编译时刻)
            int y, mo, d, h, mi, s;
            rtc_unix_to_ymdhms(utc, &y, &mo, &d, &h, &mi, &s);
            resp[0] = (uint8_t)(y - 2000);
            resp[1] = (uint8_t)mo; resp[2] = (uint8_t)d;
            resp[3] = (uint8_t)h;  resp[4] = (uint8_t)mi; resp[5] = (uint8_t)s;
            resp[6] = (uint8_t)(int8_t)rtc_get_zone_offset();   // 有符号小时
            rn = 7;
            break;
        }

        /* ---- 0x79 OPTION__SET_PUMP_UTC_AND_TIME_ZONE ← 7B（真实 UTC + 时区）----
         * AAPS 发送端 (SetPumpUTCAndTimeZone.kt:23) 用 DateTime(time).withZone(UTC) 构造,
         * 故 params[0..5] 已是**真实 UTC** 年月日时分秒。泵内 RTC 也一律存真实 UTC 秒,
         * 不要再叠加 offset (offset 只用于显示/0x78 回本地墙钟)。响应 1B result。 */
        case DANA_CMD_SET_UTC_TZ: {
            if (nparams >= 7) {
                uint32_t utc = rtc_ymdhms_to_unix((int)params[0] + 2000, (int)params[1], (int)params[2],
                                                  (int)params[3], (int)params[4], (int)params[5]);
                int8_t off = (int8_t)params[6];
                rtc_set_zone_offset(off);                 // 记住手机时区
                if (utc != 0) rtc_set_unix(utc);          // 直接存真实 UTC, 不叠加 offset
            }
            resp[0] = 0x00;
            rn = 1;
            break;
        }

        /* ---- 0xFF ETC__KEEP_CONNECTION → 1B（0 = OK，非 0 会 failed）---- */
        case DANA_CMD_KEEP_CONNECTION:
            resp[0] = 0x00;
            rn = 1;
            break;

        /* ---- 0x20 REVIEW__GET_SHIPPING_INFORMATION → 16B ----
         * [0..9] 序列号(ASCII,10) [10..12] 国家(ASCII,3) [13..15] 出厂日期(年-2000,月,日)
         * ⚠️ 日期走 Joda DateTime(y, month, day, 0, 0)：month/day 必须 1..12 / 1..31，
         *    否则抛异常 → AAPS 5 秒超时断开。绝不能留 0。 */
        case DANA_CMD_SHIPPING_INFO: {
            const char *sn = DANAI_DEVICE_NAME;              // "DAN12345AB"，正好 10 字符
            for (int i = 0; i < 10; i++) resp[i] = sn[i] ? (uint8_t)sn[i] : (uint8_t)'0';
            resp[10] = 'K'; resp[11] = 'O'; resp[12] = 'R';  // shippingCountry
            resp[13] = 26; resp[14] = 1; resp[15] = 1;       // 2026-01-01（合法即可）
            rn = 16;
            break;
        }

        /* ---- 0x21 REVIEW__GET_PUMP_CHECK → 3B ----
         * [0] hwModel（0x09 → PumpType.DANA_I，且 usingUTC/profile24 = true）
         * [1] protocol（≥10 → InitialScreen 读 errorState）
         * [2] productCode（<2 会弹 UNSUPPORTED_FIRMWARE 通知） */
        case DANA_CMD_PUMP_CHECK:
            resp[0] = (uint8_t)DANAI_HW_MODEL;
            resp[1] = (uint8_t)DANAI_PROTOCOL;
            resp[2] = 0x02;
            rn = 3;
            break;

        /* ---- 0x22 REVIEW__GET_USER_TIME_CHANGE_FLAG → 1B（data.size<3 会 failed）---- */
        case DANA_CMD_USER_TIME_CHANGE:
            resp[0] = 0x00;
            rn = 1;
            break;

        /* ---- 0x63 BASAL__GET_PROFILE_NUMBER → 1B ----
         * ⚠️ 这个值会被 0x67 用作 pumpProfiles 的行下标，AAPS 数组只有 4 行，必须 0..3。 */
        case DANA_CMD_PROFILE_NUMBER:
            resp[0] = prof;
            rn = 1;
            break;

        /* ---- 0x64 BASAL__SET_PROFILE_NUMBER ← 1B ---- */
        case DANA_CMD_SET_PROFILE_NUMBER:
            if (nparams >= 1 && params[0] < MAX_BASAL_PROFILES &&
                g_pump_config.active_profile != params[0]) {
                g_pump_config.active_profile = params[0];
                dana_cfg_mark_dirty();
                Serial.printf("[DANA] 0x64 active_profile=%u\n", (unsigned)params[0]);
            }
            resp[0] = 0x00;
            rn = 1;
            break;

        /* ---- 0x50 BOLUS__GET_BOLUS_OPTION → 19B ----
         * [0] isExtendedBolusEnabled（**必须 == 1**，否则 EXTENDED_BOLUS_DISABLED + failed）
         * [1] bolusCalculationOption [2] missedBolusConfig [3..18] 4 组错过大剂量时段 */
        case DANA_CMD_BOLUS_OPTION:
            resp[0] = 0x01;      // 方波使能（AAPS 强制要求）
            resp[1] = 0x01;      // 大剂量计算器可用
            resp[2] = 0x00;      // 错过大剂量提醒关闭 → 后面 16B 全 0 即可
            rn = 19;
            break;

        /* ---- 0x67 BASAL__GET_BASAL_RATE → 51B ----
         * [0..1] maxBasal×100  [2] basalStep×100（**必须 == 1**，即 0.01U，
         *        否则 WRONG_BASAL_STEP + failed）  [3..50] 24 段基础率×100
         * 注：basalStep 只是 AAPS 用来做 UI 取整/合法性校验的「协议档位」，
         *     实际下发到电机仍走 dosing.h 的 0.1U 栅格（剂量诚实性原则）。 */
        case DANA_CMD_BASAL_RATE: {
            /* maxBasal 回 0 会让 AAPS 认为泵不允许任何基础率 → 配置文件永远校验失败。
             * 与 0x66 的写入钳制用同一个兜底值，保证读写口径一致。 */
            float maxb = g_pump_config.max_basal_per_hour;
            if (!(maxb > 0.0f)) maxb = MAX_BASAL_RATE;
            dana_put16(&resp[0], dana_u100(maxb));
            resp[2] = 1;                                     // 0.01 U
            for (int i = 0; i < 24; i++)
                dana_put16(&resp[3 + i * 2],
                           dana_u100(g_pump_config.profiles[prof].slots[i].rate_uh));
            rn = 51;
            break;
        }

        case DANA_CMD_GET_PROFILE_BASAL: {     /* 0x65 指定方案 24 段 → 48B */
            uint8_t q = (nparams >= 1 && params[0] < MAX_BASAL_PROFILES) ? params[0] : prof;
            for (int i = 0; i < 24; i++)
                dana_put16(&resp[i * 2], dana_u100(g_pump_config.profiles[q].slots[i].rate_uh));
            rn = 48;
            break;
        }

        /* ---- 0x4B BOLUS__GET_CALCULATION_INFORMATION → 14B ----
         * [0]error(≠0→failed) [1..2]BG [3..4]碳水 [5..6]目标 [7..8]CIR [9..10]CF
         * [11..12]IOB×100 [13]units（0=mg/dL；若报 1=mmol 则 CF/目标/BG 会被 ÷100） */
        case DANA_CMD_CALC_INFO: {
            resp[0] = 0x00;
            dana_put16(&resp[1],  g_pump_state.last_glucose_mgdl);
            dana_put16(&resp[3],  0);
            dana_put16(&resp[5],  g_pump_config.target_glucose[ch]);
            dana_put16(&resp[7],  (uint16_t)(g_pump_config.carb_ratio[ch] + 0.5f));
            dana_put16(&resp[9],  (uint16_t)(g_pump_config.isf[ch] + 0.5f));
            dana_put16(&resp[11], (uint16_t)(g_pump_state.iob_x10000 / 100u));
            resp[13] = 0x00;     // UNITS_MGDL
            rn = 14;
            break;
        }

        /* ---- 0x52 BOLUS__GET_24_CIR_CF_ARRAY → 97B（hwModel≥7 走这条）----
         * [0]units  [1..48] 24×CIR(2B)  [49..96] 24×CF(2B)
         * units 必须 ∈ {0,1}，否则 failed。这是最长的一包 → DANA_MAX_PACKET 必须 ≥106。 */
        case DANA_CMD_24_CIR_CF_ARRAY: {
            resp[0] = 0x00;      // MGDL
            for (int i = 0; i < 24; i++) {
                dana_put16(&resp[1 + i * 2],      (uint16_t)(g_pump_config.carb_ratio[i] + 0.5f));
                dana_put16(&resp[1 + 48 + i * 2], (uint16_t)(g_pump_config.isf[i] + 0.5f));
            }
            rn = 97;
            break;
        }

        /* ---- 0x4E BOLUS__GET_CIR_CF_ARRAY（hw<7 分支）----
         * 本机 hwModel=0x09 ≥7 ⇒ profile24=true，AAPS 只会发 0x52，这条走不到。
         * 但真要收到，它顺序读 language(1)+units(1)+12×CIR(2B)+12×CF(2B) ≈ 50B，
         * 32B 会越界 → 5 秒断连。给 64B 零填充留足余量。 */
        case DANA_CMD_CIR_CF_ARRAY:
            rn = 64;
            break;

        /* ---- 0x72 OPTION__GET_USER_OPTION → 20B ----
         * [0]timeDisplayType(0=24h) [1]buttonScroll [2]beepAndAlarm
         * [3]lcdOnTimeSec（**必须 ≥5**，否则 failed！） [4]backlight [5]language
         * [6]units(0=mg/dL) [7]shutdownHour [8]lowReservoirRate
         * [9..10]cannulaVolume [11..12]refillAmount [13..17]可选语言 [18..19]target
         * data.size≥22（=20B 参数）时才读 target，所以固定回 20B。 */
        case DANA_CMD_USER_OPTION:
            resp[0]  = 0x00;                 // 24 小时制
            resp[1]  = 0x00;                 // 按键滚动关
            resp[2]  = 0x01;                 // 蜂鸣
            resp[3]  = 30;                   // LCD 常亮 30s（≥5）
            resp[4]  = 30;                   // 背光 30s
            resp[5]  = 0x01;                 // 语言
            resp[6]  = 0x00;                 // MGDL
            resp[7]  = 0x00;                 // 不自动关机
            resp[8]  = 20;                   // 低药量阈值 20U
            dana_put16(&resp[9],  0);        // cannulaVolume
            dana_put16(&resp[11], (uint16_t)RESERVOIR_CAPACITY_U);   // refillAmount
            dana_put16(&resp[18], 100);      // target 100 mg/dL
            rn = 20;
            break;

        /* ---- 0x66 BASAL__SET_PROFILE_BASAL_RATE ← 49B ----
         * [0] profileNumber  [1..48] 24 段基础率 ×100（小端 2B/段）
         * 必须真正落盘：AAPS 每次连接都用 0x67/0x65 读回比对，不一致就一直提示
         * 「设置配置文件」；而且泵本地的 basal_scheduler 直接读 g_pump_config，
         * 不写入就等于 AAPS 的配置文件根本没生效（闭环基础率错的）。 */
        case DANA_CMD_SET_PROFILE_BASAL: {
            if (nparams >= 49) {
                uint8_t q = (params[0] < MAX_BASAL_PROFILES) ? params[0] : prof;
                bool changed = false;
                /* ⚠️ 钳制上限必须有安全兜底：若 g_pump_config.max_basal_per_hour 为 0
                 * （旧版 NVS 结构、首次上电未初始化等），直接用它当上限会把 AAPS 下发的
                 * 24 段基础率**全部钳成 0** → 泵完全不给基础率。主机联调实测踩到过。 */
                float cap = g_pump_config.max_basal_per_hour;
                if (!(cap > 0.0f)) cap = MAX_BASAL_RATE;
                for (int i = 0; i < 24; i++) {
                    uint16_t x100 = (uint16_t)(params[1 + i * 2] |
                                               ((uint16_t)params[2 + i * 2] << 8));
                    /* 安全钳制：不接受超过本机最大基础率的下发（防御畸形/恶意包） */
                    float r = (float)x100 / 100.0f;
                    if (r < 0.0f) r = 0.0f;
                    if (r > cap) r = cap;
                    g_pump_config.profiles[q].slots[i].hour = (uint8_t)i;
                    if (g_pump_config.profiles[q].slots[i].rate_uh != r) {
                        g_pump_config.profiles[q].slots[i].rate_uh = r;
                        changed = true;
                    }
                }
                if (changed) {
                    dana_cfg_mark_dirty();
                    Serial.printf("[DANA] 0x66 basal profile %u updated (24 slots)\n", (unsigned)q);
                }
            } else {
                Serial.printf("[DANA] 0x66 short params=%d (need 49), ignored\n", (int)nparams);
            }
            resp[0] = 0x00;
            rn = 1;
            break;
        }

        /* ---- 0x53 BOLUS__SET_24_CIR_CF_ARRAY ← 96B ----
         * [0..47] 24×CIR(2B 小端)  [48..95] 24×CF(2B 小端)
         * 单位与 0x52 读取分支对称：units=MGDL ⇒ CIR 为整数 g/U，CF 为整数 mg/dL·U⁻¹。
         * ⚠️ 曾因 dana_dispatch 的 params[48] 太小把后 48B(CF) 静默截断，已扩到 DANA_MAX_PACKET。 */
        case DANA_CMD_SET_24_CIR_CF: {
            if (nparams >= 96) {
                bool changed = false;
                for (int i = 0; i < 24; i++) {
                    uint16_t cir = (uint16_t)(params[i * 2] | ((uint16_t)params[i * 2 + 1] << 8));
                    uint16_t cf  = (uint16_t)(params[48 + i * 2] | ((uint16_t)params[48 + i * 2 + 1] << 8));
                    if (cir >= 1 && cir <= 300 && g_pump_config.carb_ratio[i] != (float)cir) {
                        g_pump_config.carb_ratio[i] = (float)cir;  changed = true;
                    }
                    if (cf >= 1 && cf <= 1000 && g_pump_config.isf[i] != (float)cf) {
                        g_pump_config.isf[i] = (float)cf;          changed = true;
                    }
                }
                if (changed) {
                    dana_cfg_mark_dirty();
                    Serial.printf("[DANA] 0x53 CIR/CF updated (CIR[0]=%.0f CF[0]=%.0f)\n",
                                  g_pump_config.carb_ratio[0], g_pump_config.isf[0]);
                }
            } else {
                Serial.printf("[DANA] 0x53 short params=%d (need 96), ignored\n", (int)nparams);
            }
            resp[0] = 0x00;
            rn = 1;
            break;
        }

        case DANA_CMD_SET_USER_OPTION:   /* 0x73 */
        case DANA_CMD_SET_USER_TIME_CHANGE_CLEAR: /* 0x23 */
        case DANA_CMD_SET_HISTORY_UPLOAD_MODE:    /* 0x25 */
        case DANA_CMD_SET_HISTORY_SAVE:  /* 0xE0 */
        case DANA_CMD_APS_SET_EVENT_HISTORY: /* 0xC3 */
        case DANA_CMD_SET_BOLUS_OPTION:  /* 0x51 */
        case DANA_CMD_SET_CIR_CF:        /* 0x4F */
            resp[0] = 0x00;              // result = OK
            rn = 1;
            break;

        /* ---- 0x24 REVIEW__GET_MORE_INFORMATION → 定长零填充（本机不产生有效数据）---- */
        case DANA_CMD_MORE_INFO:
            rn = 16;
            break;

        /* ---- 0xC2 APS_HISTORY_EVENTS ----
         * AAPS 逐条接收，首字节 0xFF 代表「最后一条」→ 置 historyDoneReceived。
         * loadEvents() 里是 `while (!historyDoneReceived && isConnected) sleep(100)` 的
         * 死等循环，不回 0xFF 就永远卡在「读取泵历史」直到断连。
         * ★ 修复(2026-08-11)：此前直接回 0xFF 不回放任何记录 → AAPS 读不到 BOLUS/TEMP_START
         *   等事件 → 大剂量控制成功却永远不写治疗账本(IOB 漏记, 闭环低血糖风险)。
         *   现在按 AAPS 下发的 from(UTC 6B) 增量回放历史记录, 末尾补 0xFF。
         *   ⚠️ 仅用 DanaPump.HistoryEntry 已知 code(1/2/5...), 未知 code 会让 AAPS
         *      processMessage 的 fromInt() throw InvalidParameterException 中断整批解析。 */
        case DANA_CMD_APS_HISTORY_EVENTS:
            dana_history_replay(params);   // 回放增量记录 + 末尾 0xFF, 均异步入队
            return 0;                      // 响应已由 replay 发出, 跳过末尾通用发送

        default:
            /* 未识别命令：回足够长的零填充。
             * ⚠️ 绝不能只回 1 字节 —— 任何读到更远偏移的 handleMessage 都会
             *    数组越界抛异常，AAPS 随即 5 秒超时 disconnect。
             * 64B 可覆盖除 0x52(97B) 外所有 AAPS 解析器的最大读取偏移。 */
            rn = 64;
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
            Serial.println("[DANA] PUMP_CHECK rx -> send resp");
            dana_trace_log(3, DANA_OP_PUMP_CHECK, 1, nullptr, 0);   // 收到 PUMP_CHECK
            dana_send_raw(resp, rl);
        } else if (opcode == DANA_OP_TIME_INFO) {
            uint8_t resp[DANA_MAX_PACKET]; size_t rl = 0;
            dana_build_time_info_response(c, resp, &rl);
            c->conn = DANA_CONN_HANDSHAKE_DONE; // 进入命令阶段（BLE5 二级加密启用）
            g_pump_state.dana_paired = true;    // 闭环页据此显示"AAPS 已接管"
            // 执行历史: AAPS 接管 (闭环)
            uint8_t ap = g_pump_config.active_profile; if (ap >= MAX_BASAL_PROFILES) ap = 0;
            basal_history_record(BH_AAPS_TAKEOVER, ap, 0, 0,
                                 (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
            Serial.println("[DANA] TIME_INFO rx -> paired, send resp");
            dana_trace_log(3, DANA_OP_TIME_INFO, 1, nullptr, 0);    // 收到 TIME_INFO
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
        /* ★ 边界检查（g_rxbuf 扩到 256 后必需）：LEN 字节来自对端/可能被误解密成乱码，
         * total 最大可达 262，直接 memcpy 进 pkt[128] 会爆栈。合法包 ≤ 105B(0x53)。
         * 判定畸形则丢掉这两个起始标记字节，让循环重新找下一个 A5A5。 */
        if (total > DANA_MAX_PACKET) {
            Serial.printf("[DANA] bogus LEN=%u (total=%u) -> resync\n",
                          (unsigned)plen, (unsigned)total);
            dana_trace_log(4, 0xEE, plen, g_rxbuf, 8);
            memmove(g_rxbuf, g_rxbuf + 2, g_rxlen - 2);
            g_rxlen -= 2;
            continue;
        }
        if (total > g_rxlen) return;       // 等待后续分片

        uint8_t pkt[DANA_MAX_PACKET];
        memcpy(pkt, g_rxbuf, total);
        memmove(g_rxbuf, g_rxbuf + total, g_rxlen - total);
        g_rxlen -= total;

        /* 参数区必须能装下最大的写入命令：0x53 SET_24_CIR_CF_ARRAY = 96B
         * (24×2B CIR + 24×2B CF)。此前只有 48B，导致后半段 CF 被静默截断。 */
        uint8_t type, opcode, params[DANA_MAX_PACKET];
        size_t  nparams = 0;
        /* 注意：分片已在接收边界解密，此处用 dana_unpack_packet 避免二次解密 */
        int r = dana_unpack_packet(&g_dana_ctx, pkt, total, &type, &opcode,
                                   params, sizeof(params), &nparams);
        if (r == 0) dana_dispatch(&g_dana_ctx, type, opcode, params, nparams);
        else { Serial.printf("[DANA] unpack fail r=%d len=%d\n", r, (int)total);
               dana_trace_log(4, 0, (uint8_t)(r & 0xFF), pkt, (uint8_t)total); }  // 解析失败
    }
}

/* FFF2 写回调（手机→泵命令） */
class DanaChCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
        std::string v = c->getValue();
        if (!v.empty()) {
            /* ★ 写 FFF2 的这条连接就是真正在跑 Dana 协议的对端（AAPS）。
             * 记录它的 connHandle，后续响应定向 notify 回去，杜绝多连接串台。
             * 切换对端时清空收包缓冲，避免半包与新对端的字节流拼接。 */
            uint16_t h = info.getConnHandle();
            if (h != g_dana_peer_conn) {
                Serial.printf("[DANA] peer conn -> %u (was %u)\n",
                              (unsigned)h, (unsigned)g_dana_peer_conn);
                dana_trace_log(3, 0xFA, (uint8_t)(h & 0xFF), nullptr, 0);   // 对端切换
                g_dana_peer_conn = h;
                g_rxlen = 0;
            }
            dana_dbg_hex("RX", (const uint8_t *)v.data(), v.size());
            dana_trace_log(0, 0xFF, 0, (const uint8_t *)v.data(), (uint8_t)v.size());  // RX 原始
            dana_feed_rx((const uint8_t *)v.data(), v.size());
        }
    }
    void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &info, uint16_t subValue) override {
        if (c == g_dana_fff1) {
            g_fff1_subscribed = (subValue != 0);
            uint16_t h = info.getConnHandle();
            if (subValue != 0) {
                /* 订阅 FFF1 的一定是 Dana 客户端（系统连接不会订阅），先行绑定对端，
                 * 让首个 PUMP_CHECK 响应就走定向通道。 */
                if (h != g_dana_peer_conn) { g_dana_peer_conn = h; g_rxlen = 0; }
            } else if (h == g_dana_peer_conn) {
                g_dana_peer_conn = BLE_HS_CONN_HANDLE_NONE;   // 取消订阅：解绑，退回广播式 notify
            }
            Serial.printf("[BLE] FFF1 subscribe=%d conn=%u\n", subValue ? 1 : 0, (unsigned)h);
            dana_trace_log(3, 0xFE, (subValue ? 1 : 0), nullptr, 0);   // 订阅状态变化
        }
    }
};

/* 连接回调：AAPS 接管 server 回调后，连接态只能在此维护
 * （ble_comm.cpp 的 srvCb 已被 setCallbacks 覆盖，故 ble_connected 必须在此设置），
 * ble_task 据此每秒推送 notify（伴生 App 屏镜像 / 状态）。Dana 握手状态机同步清理。 */
class DanaSrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *srv, NimBLEConnInfo &info) override {
        g_pump_state.ble_connected = true;
        Serial.println("[BLE] connected");
        // st: 1=对端已绑定(BOND_BONDED) 0=未绑定(BOND_NONE) —— 直接对应 AAPS connect() 是否会被拦截
        dana_trace_log(3, 0xFD, (uint8_t)(info.isBonded() ? 1 : 0), nullptr, 0);   // BLE GATT 连接建立
        // ★ 根因修复 (2026-08-08, logcat + flash trace 双向实证)：连接建立后必须【继续广播】。
        //   华为/EMUI 对已配对 BLE 设备会自动保持一条系统连接。若此时泵停止广播，
        //   AAPS 的 connectGatt 走白名单方式发起 (bt_stack 实测 init_filter_policy=1，
        //   即必须先扫到广播才能建链)，将永远建不了自己的链路 → 30s 后
        //   bta_gattc_open_fail "Cannot establish Connection" GATT_ERROR(133)
        //   → AAPS 永远停在「正在连接」死循环。
        //   NimBLE 最大 3 连接，继续广播即可让 AAPS 建立第二条独立连接。
        srv->startAdvertising();
        Serial.println("[BLE] keep advertising after connect (multi-conn)");
    }
    void onDisconnect(NimBLEServer *p, NimBLEConnInfo &info, int reason) override {
        uint16_t h = info.getConnHandle();
        /* ★ 多连接修正：NimBLE 在调用本回调前已把该 peer 从连接表移除，
         * 故 getConnectedCount() 即剩余连接数。只有全部断开才算「未连接」。 */
        g_pump_state.ble_connected = (p->getConnectedCount() > 0);
        if (h == g_dana_peer_conn || g_dana_peer_conn == BLE_HS_CONN_HANDLE_NONE) {
            /* 断的是 Dana 协议对端（或尚未绑定对端）→ 重置握手状态机 */
            g_dana_ctx.conn = DANA_CONN_INIT;
            g_rxlen = 0;
            g_dana_peer_conn = BLE_HS_CONN_HANDLE_NONE;
            g_fff1_subscribed = false;
        } else {
            /* 断的是华为系统那条旁路连接 —— 绝不能清掉 AAPS 正在进行的握手 */
            Serial.printf("[BLE] non-Dana conn %u dropped, keep handshake\n", (unsigned)h);
        }
        dana_trace_log(3, 0xFC, (uint8_t)reason, nullptr, 0);   // BLE GATT 断开, st=reason
        p->startAdvertising();                // 断连后重新广播（NimBLE 偶发静默失败，ble_task 有兜底）
        Serial.printf("[BLE] disconnected conn=%u reason=%d left=%d -> startAdvertising()\n",
                      (unsigned)h, reason, (int)p->getConnectedCount());
    }
    /* 蓝牙配对回调（仅当 IO cap 启用 passkey 时触发；当前 NO_IO/Just Works 下不会调用，
     * 保留以兼容若改回 DISPLAY_ONLY 的 passkey 调试场景）。 */
    uint32_t onPassKeyDisplay() override { return g_dana_passkey; }
    void onConfirmPassKey(NimBLEConnInfo &, uint32_t) override { /* Just Works 下不触发 */ }
    void onAuthenticationComplete(NimBLEConnInfo &) override {
        Serial.println("[BLE] paired (Dana-i passkey accepted)");
        dana_trace_log(3, 0xFB, 1, nullptr, 0);   // 配对/绑定完成（AAPS createBond 或手动配对均触发）
    }
};

static DanaChCb  g_dana_ch_cb;
static DanaSrvCb g_dana_srv_cb;

/* 在已有 NimBLEServer 上挂载 Dana FFF0/FFF1/FFF2 服务 */
void aaps_dana_attach(void *server)
{
    NimBLEServer *srv = (NimBLEServer *)server;
    /* 蓝牙配对（2026-08-07 修正，方向反转）：
     * 之前误把 sm_bonding 关成 0（setSecurityAuth(false,false,false)），以为能绕开
     * 加密不对称——这是错的，且正是连不上的真正根因。Android 持久化 bond
     * （bondState=BONDED）的前提是**从机也要请求 bonding**；从机 sm_bonding=0 时
     * Android 认为本次配对「不持久化」，华为/AAPS 看到的 bondState 永远是 BOND_NONE →
     * AAPS 走 BLEComm.kt:133 的 createBond()+sleep(10s)+return false 死循环，永远到不了
     * connectGatt/订阅/PUMP_CHECK。这和 flash trace 的 CONN+AUTH+DISC 却零 SUBSCRIBE 完全吻合。
     * 修正：恢复从机 bonding=true（LESC），让 Android 持久化 bond；NimBLE 已通过
     * ble_store_config_init() 把 LTK 落 NVS，连接时加密对称。Dana-i 真正安全在应用层
     * DANAI_BLE5_KEY 握手，BLE 层只需 bonding 成功即可，AAPS 不校验链路层 MITM/SC 类型。 */
    /* 蓝牙安全（2026-08-07 修正 v4 — bonding + legacy pairing，关 LESC）：
     * 必须 sm_bonding=1：让配对响应带 BOND 标志，Android 才会持久化配对（实测 sm_bonding=0
     * 时重启后泵变"可用设备"不持久化）。但 AAPS 在 STATE_CONNECTED 立即 discoverServices()，
     * 若链路层用 LESC(sc=true) 加密，Android 10 的 onEncryptionChanged 常不触发 → AAPS 卡
     * "正在连接"。改用 legacy pairing(sc=false)：加密协商走传统 SMP，onEncryptionChanged 可靠
     * 触发，AAPS 顺利走完 discoverServices→订阅FFF1→发 PUMP_CHECK。Dana 真正安全在应用层
     * DANAI_BLE5_KEY 握手，BLE 层只需 bonding 成功即可，AAPS 不校验链路层 MITM/SC 类型。 */
    NimBLEDevice::setSecurityAuth(true, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
    g_dana_passkey = (uint32_t)atoi(DANAI_BLE5_KEY);  // 仅作兼容保留，NO_IO 下不触发回调
    dana_ctx_init(&g_dana_ctx, DANAI_DEVICE_NAME, DANAI_BLE5_KEY,
                  DANAI_HW_MODEL, DANAI_PROTOCOL);

    srv->setCallbacks(&g_dana_srv_cb);

    NimBLEService *svc = srv->createService(NimBLEUUID(DANA_UUID_FFF0));

    /* FFF1：泵→手机（READ + NOTIFY），响应/通知通道 */
    g_dana_fff1 = svc->createCharacteristic(NimBLEUUID(DANA_UUID_FFF1),
                                           NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_dana_fff1->setCallbacks(&g_dana_ch_cb);
    /* FFF2：手机→泵（WRITE + WRITE_NR，无响应写）。
     * ⚠️ 关键：AAPS 用 WRITE_TYPE_NO_RESPONSE 写 FFF2（见 docs §6.1）。
     * 若只声明 NIMBLE_PROPERTY::WRITE（带响应写），NimBLE 会拒绝 no-response
     * 写（ATT Write Not Permitted），泵收不到 PUMP_CHECK → AAPS 一直"正在连接"。
     * 同时保留 WRITE 以兼容个别用带响应写的客户端。 */
    NimBLECharacteristic *fff2 = svc->createCharacteristic(NimBLEUUID(DANA_UUID_FFF2),
                                                          NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    fff2->setCallbacks(&g_dana_ch_cb);

    svc->start();

    /* P2-9 补全: 把 TBR 历史记录钩子注册给 HAL, 使泵菜单设/取 TBR 也能进 0xC2 回放。 */
    ui_hal_register_tbr_history_cb(aaps_tbr_hist_cb);
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
    g_dana_peer_conn = BLE_HS_CONN_HANDLE_NONE;
}

/* 模拟「AAPS 已连接并订阅 FFF1」：打开 aaps_dana_pump() 的发送门控。
 * 真机由 NimBLE 的 onConnect/onSubscribe 回调置位，主机桩不产生这些事件，
 * 必须显式打开，否则响应永远滞留在发送队列里、host_drain_tx 抽不到东西。 */
extern "C" void aaps_dana_host_link_up_test(void)
{
    g_fff1_subscribed = true;
    g_pump_state.ble_connected = true;
    g_dana_peer_conn = 0;      // 假定对端 connHandle = 0
}
#endif

#endif /* USE_AAPS_DANA */
