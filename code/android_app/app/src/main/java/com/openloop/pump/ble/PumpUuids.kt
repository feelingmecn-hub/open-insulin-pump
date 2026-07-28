package com.openloop.pump.ble

import java.util.UUID

/**
 * BLE UUID 与协议常量 —— 必须与 ESP32-C6 固件 [config.h] 完全一致。
 *
 * 基础 UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * （Nordic UART Service 风格，固件以 little-endian 字节数组存储，
 *  对应标准字符串如上）
 */
object PumpUuids {

    // --- 服务 ---
    val SERVICE = uuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")

    // --- 特征 ---
    val CHAR_BOLUS     = uuid("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_BASAL     = uuid("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_TBR       = uuid("6E400004-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_STATUS    = uuid("6E400005-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_IOB       = uuid("6E400006-B5A3-F393-E0A9-E50E24DCCA9E")
    val CHAR_RESERVOIR = uuid("6E400007-B5A3-F393-E0A9-E50E24DCCA9E")

    // --- 标准 CCCD (Client Characteristic Configuration Descriptor) ---
    val CCCD = uuid("00002902-0000-1000-8000-00805F9B34FB")

    private fun uuid(s: String): UUID = UUID.fromString(s)
}

/**
 * 协议帧常量（与固件 ble_gatt_server.cpp 对齐）。
 */
object PumpProtocolSpec {
    // 操作码
    const val OPCODE_BOLUS: Byte = 0x01
    const val OPCODE_BASAL: Byte = 0x02
    const val OPCODE_TBR:   Byte = 0x03

    // 帧长度
    const val BOLUS_CMD_LEN    = 8
    const val BASAL_CMD_LEN    = 7
    const val TBR_CMD_LEN      = 7
    const val STATUS_FRAME_LEN = 16
    const val IOB_FRAME_LEN    = 4
    const val RESERVOIR_FRAME_LEN = 2

    // 大剂量 flags
    const val FLAG_EXTENDED: Byte = 0x01   // 方波大剂量

    // 安全上限（与固件 config.h 一致）
    const val MAX_BOLUS_UNITS   = 25.0
    const val MAX_BASAL_RATE    = 5.0       // U/h
    const val MAX_RESERVOIR     = 300       // U
}
