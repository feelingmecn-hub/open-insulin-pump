package com.openloop.pump.domain.model

/**
 * 基础率单槽（1 小时）。
 */
data class BasalSlot(val hour: Int, val rateUh: Double) {
    init {
        require(hour in 0..23) { "小时必须 0..23" }
        require(rateUh in 0.0..5.0) { "基础率 0..5 U/h" }
    }
}

/**
 * 基础率方案（一天 24 槽）。
 */
data class BasalProfile(
    val name: String,
    val slots: List<BasalSlot>
) {
    init {
        require(slots.size == 24) { "必须包含 24 个槽位" }
    }

    /** 当日总基础量 (U)。 */
    val dailyTotalUnits: Double get() = slots.sumOf { it.rateUh }

    /** 当前小时对应的基础率。 */
    fun rateAt(hour: Int): Double =
        slots.firstOrNull { it.hour == hour }?.rateUh ?: 0.0
}
