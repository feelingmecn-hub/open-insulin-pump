package com.openloop.pump.ble

import java.util.UUID

/**
 * BLE UUID 与协议常量 —— 必须与 ESP32-C6 固件 [config.h] 完全一致。
 *
 * 基础 UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * （Nordic UART Service 风格，固件以 little-endian 字节数组存储，
 *  对应标准字符串如上）
 *
 * ⚠️ 项目仅保留 AAPS(Dana-i 伪装) 变体固件：泵广播名为 DANAI_DEVICE_NAME
 *   ("DAN12345AB")，但同时挂载自定义伴生服务(本文件 UUID)与 Dana FFF0 服务。
 *   App 扫描该广播名，连接后通过本文件 UUID 的伴生通道控制泵并镜像屏。
 */
object PumpUuids {

    // --- 服务 ---
    val SERVICE = uuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")

    // --- 特征（伴生通道，与 AAPS/Dana 互不干扰）---
    val CHAR_BOLUS     = uuid("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_BASAL     = uuid("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_TBR       = uuid("6E400004-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_STATUS    = uuid("6E400005-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_IOB       = uuid("6E400006-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_RESERVOIR = uuid("6E400007-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_CGM       = uuid("6E400008-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_CONTROL   = uuid("6E400009-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_SETTINGS  = uuid("6E40000A-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_KEY       = uuid("6E40000B-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_SCREEN    = uuid("6E40000C-B5A3-F393-E0A9-E50E24DCCA9E")

    // --- 标准 CCCD (Client Characteristic Configuration Descriptor) ---
    val CCCD = uuid("00002902-0000-1000-8000-00805F9B34FB")

    private fun uuid(s: String): UUID = UUID.fromString(s)
}

/**
 * 协议帧常量（与固件 ble_comm.cpp 对齐）。
 */
object PumpProtocolSpec {
    // 安全上限（与固件 config.h 一致）
    const val MAX_BOLUS_UNITS   = 100.0
    const val MAX_BASAL_RATE    = 5.0       // U/h
    const val MAX_RESERVOIR     = 300       // U
    const val DEFAULT_TBR_PERCENT = 100

    // 远程按键事件码（与固件 pump_types.h key_event_t 一致）
    const val KEY_RELEASE: Byte = 0   // 松手：停止自动重复
    const val KEY_UP:     Byte = 1
    const val KEY_DOWN:   Byte = 2
    const val KEY_SET:    Byte = 3
    const val KEY_ESC:    Byte = 4
    const val KEY_LONG_SET:  Byte = 5   // 长按确认 → 进入菜单/保存原点
    const val KEY_LONG_ESC:  Byte = 6   // 长按取消 → 关机

    // 控制通道命令
    // ⚠️ 2026-08-08 修正: 这三个常量原先标反了(0 写成"开环")。固件真源见
    //    pump_state.cpp / ui_hal_fw.cpp: 0=闭环(AAPS接管), 1=开环(本地档案), 2=暂停。
    //    SettingsScreen 里手写的 listOf(0 to "闭环", 1 to "开环", 2 to "暂停") 是对的,
    //    所以线上行为未受影响; 但常量必须纠正, 免得后续有人照它写出反向指令。
    const val CTRL_MODE_CLOSED = 0      // 闭环(AAPS 接管)
    const val CTRL_MODE_OPEN   = 1      // 开环(本地 24 段档案)
    const val CTRL_MODE_PAUSED = 2      // 暂停
    const val CTRL_CMD_PRIME       = 0x10  // 远程排气（可带 ml 参数）
    const val CTRL_CMD_CLEAR_ALARM = 0x11  // 远程清报警
    const val CTRL_CMD_REWIND      = 0x12  // 远程退回装药
    const val CTRL_CMD_CAL_DISPENSE= 0x13  // 标定：推出测试量（param: units f32）
    const val CTRL_CMD_CAL_APPLY   = 0x14  // 标定：保存系数（param: factor f32）
    const val CTRL_CMD_MANUAL_MOVE = 0x15  // 手动电机控制：[dir u8][steps u32 LE][speed u16 LE]
    const val CTRL_CMD_MANUAL_STOP = 0x16  // 手动停止（连续点动退出）
    const val CTRL_CMD_BASAL_TEST  = 0x18  // 基础率验证测试：把当前方案 24 段总量一次性打出。
                                           //   历史记为"基础率验证"(独立事件类型)，不计入大剂量次数/IOB；
                                           //   用于核对"基础率有没有真写进泵、电机会不会动"，
                                           //   并可对照丝杠实际位移验证步数。

    // 手动电机方向码
    const val MANUAL_DIR_FWD = 0   // 前进（推注方向）
    const val MANUAL_DIR_REV = 1   // 后退（回退方向）

    // 设置通道 op
    const val SET_OP_GET_TIME          = 0x01
    const val SET_OP_SET_TIME          = 0x02  // payload: u32 Unix
    const val SET_OP_GET_BRIGHTNESS    = 0x03
    const val SET_OP_SET_BRIGHTNESS    = 0x04  // payload: u8 0..100
    const val SET_OP_GET_KEYPAD        = 0x05
    const val SET_OP_SET_KEYPAD        = 0x06  // payload: u8 0/1
    const val SET_OP_GET_VIBRATE       = 0x07
    const val SET_OP_SET_VIBRATE       = 0x08  // payload: u8 0/1
    const val SET_OP_GET_PASSKEY       = 0x09
    const val SET_OP_SET_PASSKEY       = 0x0A  // payload: u32
    const val SET_OP_GET_ACTIVE_PROFILE= 0x10
    const val SET_OP_SET_ACTIVE_PROFILE= 0x11  // payload: u8 0..3
    const val SET_OP_GET_PROFILE_NAME  = 0x14  // payload: profile u8 → name[32]
    const val SET_OP_SET_PROFILE_NAME  = 0x15  // payload: profile u8 + name
    const val SET_OP_GET_PROFILE_SLOT  = 0x16  // payload: profile u8, hour u8 → f32
    const val SET_OP_SET_PROFILE_SLOT  = 0x17  // payload: profile u8, hour u8, f32
    const val SET_OP_COMMIT_CONFIG     = 0x19  // 将内存配置一次性落盘 NVS（写 24 段后调一次）
    const val SET_OP_GET_LIMITS        = 0x20  // → 3×f32
    const val SET_OP_SET_LIMIT         = 0x21  // payload: which u8, f32
    const val SET_OP_GET_SAFETY        = 0x22  // → occlusion u16, watchdog u8, over_temp f32
    const val SET_OP_SET_SAFETY        = 0x23  // payload: which u8, value
    const val SET_OP_GET_CL_PARAM      = 0x24  // payload: kind u8, hour u8 → f32
    const val SET_OP_SET_CL_PARAM      = 0x25  // payload: kind u8, hour u8, f32
    const val SET_OP_GET_CALIBRATION   = 0x26  // → f32
    const val SET_OP_SET_CALIBRATION   = 0x27  // payload: f32
    const val SET_OP_GET_AUTO_DIM      = 0x28  // → [u8 enabled][u16 timeout_le]
    const val SET_OP_SET_AUTO_DIM      = 0x29  // payload: [u8 enabled][u16 timeout_le]
    const val SET_OP_GET_MOTOR_POSITION= 0x2A  // → u32 当前电机微步位置（电机测试用）

    // 闭环参数 kind
    const val CL_KIND_ISF         = 0
    const val CL_KIND_CARB_RATIO  = 1
    const val CL_KIND_TARGET_GLU  = 2

    // LIMIT which
    const val LIMIT_SINGLE      = 0
    const val LIMIT_PER_HOUR    = 1
    const val LIMIT_MAX_BASAL   = 2

    // SAFETY which
    const val SAFE_OCCLUSION    = 0
    const val SAFE_WATCHDOG     = 1
    const val SAFE_OVER_TEMP    = 2

    const val SET_OP_GET_DIA_MIN       = 0x12  // 返回 u16 分钟
    const val SET_OP_SET_DIA_MIN       = 0x13  // 编译期固定, 运行时不可改 → 返回 ERR
}
