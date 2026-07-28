/**
 * aaps_dana.h — AAPS Dana-i (BLE) impersonation 协议核心
 *
 * ⚠️ 实验项目，禁止用于人体。本文件仅做理论验证 / 教学原型：让设备蓝牙层
 *    伪装成 AAPS 原生支持的 Dana-i 泵（方案 B）。所有打药动作最终仍走
 *    motor_controller / basal_scheduler 与 dosing.h 单一真源。
 *
 * 本头文件包含**纯逻辑**（无 NimBLE / 无 FreeRTOS 依赖），可在宿主（PC）用
 * g++ 直接编译做单测，也可被 ESP32 固件包含。ESP32 专属的 GATT 胶水层在
 * aaps_dana.cpp 中由 `#ifdef USE_AAPS_DANA` 包裹，不影响宿主测试。
 *
 * 算法全部逐字移植自 AAPS 自身 `pump/danars` 模块：
 *   - encryption/BleEncryption.kt（CRC / 设备名 XOR / BLE5 二级加密 / 信封）
 *   - services/BLEComm.kt（握手状态机 / 命令分发 / 分包）
 * 这是确保"被 AAPS 认出"的唯一可靠依据。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 信封常量（与 AAPS DANAR_PACKET__* 一致）
 * ============================================================ */
#define DANA_START_PACKET    0xA5u   // 明文包起始
#define DANA_END_PACKET      0x5Au   // 明文包结束
#define DANA_ENC_START       0xAAu   // BLE5 二级加密后起始标记（替代 A5 A5）
#define DANA_ENC_END         0xEEu   // BLE5 二级加密后结束标记（替代 5A 5A）

#define DANA_TYPE_ENC_REQ    0x01u   // 加密请求
#define DANA_TYPE_ENC_RESP   0x02u   // 加密响应（握手）
#define DANA_TYPE_COMMAND    0xA1u   // 命令
#define DANA_TYPE_RESPONSE   0xB2u   // 命令响应
#define DANA_TYPE_NOTIFY     0xC3u   // 泵→手机通知

/* 加密层 opcode */
#define DANA_OP_PUMP_CHECK        0x00u
#define DANA_OP_TIME_INFO         0x01u
#define DANA_OP_CHECK_PASSKEY     0xD0u
#define DANA_OP_PASSKEY_REQ       0xD1u
#define DANA_OP_PASSKEY_RET       0xD2u

/* 命令层 opcode（单字节，Dana-i / DanaRS-v2 BLE） */
#define DANA_CMD_STEP_BOLUS_START 0x4Au   // 步进大剂量开始
#define DANA_CMD_STEP_BOLUS_STOP  0x44u   // 大剂量停止
#define DANA_CMD_APS_TBR          0xC1u   // APS 临时基础率（闭环专用）
#define DANA_CMD_SET_TBR          0x60u   // 手动临时基础率
#define DANA_CMD_CANCEL_TBR       0x62u   // 取消临时基础率
#define DANA_CMD_EXT_BOLUS        0x47u   // 方波大剂量
#define DANA_CMD_EXT_BOLUS_CANCEL 0x49u   // 取消方波
#define DANA_CMD_INITIAL_SCREEN   0x02u   // 初始屏幕信息（状态查询）
#define DANA_CMD_DAILY_TOTAL      0x26u   // 今日总量查询
#define DANA_CMD_BOLUS_INFO       0x40u   // 大剂量进度查询
#define DANA_CMD_SET_DUAL_BOLUS   0x48u   // 双波大剂量 SET_DUAL_BOLUS (DanaRS)。
                                           // ⚠️ 经 git 克隆 AAPS master 逐字节核对: 0x48 真实语义是
                                           //    双波大剂量, **绝非** CGM 血糖下行! AAPS 当前版本
                                           //    (pump/danars) 并不发送 0x48, 也无 APS 下发实时血糖的命令。
                                           //    本教学原型未实现双波大剂量, 收到 0x48 走 default 返回 OK。
                                           //    CGM 屏幕显示请走自定义 BLE 通道 g_ch_cgm (ble_comm.cpp)。
#define DANA_CMD_GET_TIME         0x70u   // 读取泵时间
#define DANA_CMD_SET_TIME         0x71u   // 设置泵时间

/* 通知 opcode（TYPE = NOTIFY） */
#define DANA_NOTIFY_DELIVERY_COMPLETE 0x01u   // 大剂量结束
#define DANA_NOTIFY_DELIVERY_RATE     0x02u   // 大剂量进度（已输注量）
#define DANA_NOTIFY_ALARM             0x03u   // 报警

#define DANA_MAX_PACKET      64u     // 单包最大字节（含信封）
#define DANA_MTU_CHUNK       20u     // BLE 写/通知分包每片上限

/* ============================================================
 * 安全版本 / 连接状态（镜像 AAPS）
 * ============================================================ */
typedef enum {
    DANA_SEC_DEFAULT = 0,
    DANA_SEC_RSV3    = 1,
    DANA_SEC_BLE5    = 2
} dana_sec_t;

typedef enum {
    DANA_CONN_INIT          = 0,   // 握手前（PUMP_CHECK 阶段）
    DANA_CONN_PUMP_CHECK    = 1,   // PUMP_CHECK 完成、TIME_INFO 前
    DANA_CONN_HANDSHAKE_DONE = 2   // TIME_INFO 完成 → 命令阶段（二级加密启用）
} dana_conn_state_t;

/* BLE5 二级加密查表（secondLvlEncryptionLookupShort / AAPS bleEncryptionMatrix，100B） */
extern const uint8_t DANA_BLE5_MATRIX[100];

/* 运行上下文 */
typedef struct {
    char     device_name[11];     // 10 字符 + NUL（必须匹配 ^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$）
    uint8_t  ble5_key[6];         // 6 位 ASCII 数字
    uint8_t  ble5_enc_key[3];     // 由 ble5_key 经矩阵推导
    uint8_t  hw_model;            // 0x09 / 0x0A = Dana-i
    uint8_t  protocol;            // 协议号
    dana_sec_t        sec;        // 固定 BLE5
    dana_conn_state_t conn;       // 握手状态机
} dana_ctx_t;

/* 初始化上下文（派生 BLE5 密钥、重置状态机） */
void dana_ctx_init(dana_ctx_t *c, const char *devname, const char *ble5key,
                   uint8_t hw_model, uint8_t protocol);

/* ============================================================
 * 纯算法（逐字移植 AAPS BleEncryption.kt）
 * ============================================================ */

/* 自定义 Dana CRC-16。sec/conn 决定末级混淆分支：
 *   - BLE5 且 conn==HANDSHAKE_DONE → 分支 B: (low<<4)|((low>>3)<<2)
 *   - 其余（默认 / RSv3 conn0,1）→ 分支 A: (low<<3)|((low>>2)<<5)            */
uint16_t dana_crc16(const uint8_t *data, size_t len, dana_sec_t sec, dana_conn_state_t conn);

/* 设备名一级混淆（encodeArrayBySn）：对 bytes[3 .. size-3] 按 codingBytes[i%3] XOR。
 * 收发两端各施加一次（XOR 自逆）。 */
void dana_encode_array_by_sn(uint8_t *buf, size_t size, const char *devname);

/* BLE5 二级加密 / 解密（enhancedEncryption==2）：整包（含已被替换的 AA/EE 标记）逐字节处理。
 * 解密后**不**还原 AA/EE，解析器同时接受 A5/A5 与 AA/AA 起、5A/5A 与 EE/EE 止。 */
void dana_encrypt_second_level(uint8_t *buf, size_t size, const uint8_t key[3]);
void dana_decrypt_second_level(uint8_t *buf, size_t size, const uint8_t key[3]);

/* ============================================================
 * 信封编解码
 * ============================================================ */

/* 构建 outgoing 包：out 需 ≥ 9 + nparams。
 * apply_ble5 != 0 → 在信封 + 设备名 XOR 之后施加 BLE5 二级加密（命令阶段）。
 * CRC 分支随 apply_ble5 自动选择（握手=默认分支，命令=BLE5 分支）。 */
int dana_build_packet(dana_ctx_t *c, uint8_t *out, size_t *out_len,
                      uint8_t type, uint8_t opcode,
                      const uint8_t *params, uint8_t nparams,
                      int apply_ble5);

/* 解析 incoming 包（原始 UART 字节，命令阶段可能已二级加密）：
 *   c->conn==HANDSHAKE_DONE → 先二级解密；再设备名 XOR 解码；长度校验；
 *   CRC 校验（分支随 conn 自动）；提取 [type][opcode][params]。
 * 返回 0 成功，<0 失败（长度/CRC 错误等）。 */
int dana_parse_packet(dana_ctx_t *c, const uint8_t *in, size_t in_len,
                      uint8_t *out_type, uint8_t *out_opcode,
                      uint8_t *out_params, size_t params_cap, size_t *out_nparams);

/* dana_unpack_packet: 内部解包核心，假定 in 已为明文（二级解密由调用方完成）。
 * 仅做设备名 XOR 解码 + 长度/起止标记校验 + CRC 校验 + 字段抽取。
 * 供两条路径复用：
 *   - dana_parse_packet（外部加密信封，先解密再调它）
 *   - 主机联调 TX 捕获（host_tx_push 已在分片边界解密，解包时不应二次解密）。 */
int dana_unpack_packet(dana_ctx_t *c, const uint8_t *in, size_t in_len,
                       uint8_t *out_type, uint8_t *out_opcode,
                       uint8_t *out_params, size_t params_cap, size_t *out_nparams);

/* ============================================================
 * 握手响应构造（握手阶段，不做二级加密）
 * ============================================================ */
/* PUMP_CHECK 响应：解密后 14B = [0x02][0x00]['O']['K'][?][HW_MODEL][?][PROTOCOL][key×6]
 * AAPS processConnectResponse 据此判 hwModel∈{0x09,0x0A} 后发 TIME_INFORMATION。 */
int dana_build_pump_check_response(dana_ctx_t *c, uint8_t *out, size_t *out_len);

/* TIME_INFORMATION 响应：解密后 4B = [0x02][0x01]['O']['K']。
 * AAPS processEncryptionResponse(BLE5) 收到即置 isConnected=true → 握手完成。 */
int dana_build_time_info_response(dana_ctx_t *c, uint8_t *out, size_t *out_len);

#ifdef __cplusplus
}
#endif

/* ============================================================
 * ESP32 专属胶水层声明（仅在固件构建 USE_AAPS_DANA 时可用）
 * ============================================================ */
#ifdef USE_AAPS_DANA
#ifdef __cplusplus
extern "C" {
#endif

/* 在已有 NimBLEServer 上挂载 FFF0/FFF1/FFF2 服务与回调。
 * 由 ble_comm.cpp 的 ble_init() 在宏开启时调用。 */
void aaps_dana_attach(void *server);

/* 处理一个已解析的命令（opcode + params），构造响应并通过 FFF1 通知发出。
 * 返回响应包是否成功入队发送。 */
int aaps_dana_handle_command(dana_ctx_t *c, uint8_t opcode,
                             const uint8_t *params, size_t nparams);

#ifdef __cplusplus
}
#endif
#endif /* USE_AAPS_DANA */
