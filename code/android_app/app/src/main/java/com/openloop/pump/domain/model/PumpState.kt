package com.openloop.pump.domain.model

/**
 * 泵领域状态（UI / 业务逻辑使用）。
 */
data class PumpState(
    val runState: PumpRunState,
    val alarm: AlarmCode,
    val deliveredUnits: Double,
    val reservoirUnits: Int,
    val batteryMv: Int,
    val batteryPct: Int,
    val iobUnits: Double
) {
    val hasCriticalAlarm: Boolean get() = alarm.critical
    val batteryLow: Boolean get() = batteryPct in 1..20
    val reservoirLow: Boolean get() = reservoirUnits <= 20
}
