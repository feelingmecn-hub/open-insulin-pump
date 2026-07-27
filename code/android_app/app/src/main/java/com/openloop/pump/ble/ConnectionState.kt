package com.openloop.pump.ble

/**
 * BLE 连接状态机（APP 侧）。
 */
sealed interface ConnectionState {
    /** 未连接 */
    data object Disconnected : ConnectionState

    /** 正在扫描 */
    data object Scanning : ConnectionState

    /** 正在建立 GATT 连接 */
    data object Connecting : ConnectionState

    /** 已连接，服务已发现，可通信 */
    data object Connected : ConnectionState

    /** 已配对（加密链路已建立） */
    data object Bonded : ConnectionState

    /** 错误 */
    data class Error(val reason: String) : ConnectionState
}
