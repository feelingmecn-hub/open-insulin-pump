package com.openloop.pump.domain.model

import com.openloop.pump.ble.PumpProtocol

/**
 * 泵运行状态枚举 —— 对应固件 pump_types.h 的 pump_state_t。
 */
enum class PumpRunState(val code: Int) {
    BOOTING(0),
    IDLE(1),
    PRIMING(2),
    DELIVERING(3),
    BASAL(4),
    BOLUS(5),
    STOPPING(6),
    ALARM(7),
    SLEEP(8),
    ERROR(9),
    UNKNOWN(-1);

    companion object {
        fun fromCode(code: Int): PumpRunState = entries.firstOrNull { it.code == code } ?: UNKNOWN
    }
}

/**
 * 报警码枚举 —— 对应固件 alarm_code_t。
 */
enum class AlarmCode(val code: Int, val label: String, val critical: Boolean) {
    NONE(0x00, "正常", false),
    BATTERY_LOW(0x01, "电量低 (<20%)", false),
    BATTERY_CRITICAL(0x02, "电量极低 (<10%)", true),
    RESERVOIR_EMPTY(0x03, "储药器空", true),
    OCCLUSION(0x04, "管路阻塞", true),
    MOTOR_FAULT(0x05, "电机故障", true),
    COMM_LOST(0x06, "通信丢失", false),
    OVERFLOW(0x07, "推注超时", true),
    OVER_TEMP(0x08, "过温", true),
    OVER_CURRENT(0x09, "过流", true),
    LIMIT_TRIGGERED(0x0A, "限位触发", true),
    WATCHDOG(0x0B, "看门狗", true),
    NVS_ERROR(0x0C, "存储错误", false),
    OTA_FAILED(0x0D, "OTA 失败", false),
    PUMP_STALLED(0x0E, "泵卡住", true),
    UNKNOWN(-1, "未知", false);

    companion object {
        fun fromCode(code: Int): AlarmCode = entries.firstOrNull { it.code == code } ?: UNKNOWN
    }
}

/** 把 BLE 层原始状态帧映射为领域模型。 */
fun PumpProtocol.PumpStatus.toDomain(): PumpState = PumpState(
    runState = PumpRunState.fromCode(stateCode),
    alarm = AlarmCode.fromCode(alarmCode),
    deliveredUnits = deliveredUnits,
    reservoirUnits = reservoirUnits,
    batteryMv = batteryMv,
    batteryPct = batteryPct,
    iobUnits = iobUnits
)
