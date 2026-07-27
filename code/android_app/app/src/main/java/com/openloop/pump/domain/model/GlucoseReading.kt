package com.openloop.pump.domain.model

/**
 * CGM 血糖读数。
 *
 * @param mgdl   血糖值 (mg/dL)
 * @param trend  趋势箭头
 */
data class GlucoseReading(
    val mgdl: Int,
    val trend: Trend = Trend.FLAT,
    val timestamp: Long = System.currentTimeMillis()
) {
    /** mmol/L (U-100 换算: 1 mmol/L = 18.0182 mg/dL)。 */
    val mmolL: Double get() = mgdl / 18.0182

    enum class Trend {
        RISING_FAST,
        RISING,
        FLAT,
        FALLING,
        FALLING_FAST,
        UNKNOWN;

        companion object {
            fun fromString(s: String?): Trend = when (s?.uppercase()) {
                "RISING_FAST", "DoubleUp" -> RISING_FAST
                "RISING", "SingleUp" -> RISING
                "FLAT", "Flat" -> FLAT
                "FALLING", "SingleDown" -> FALLING
                "FALLING_FAST", "DoubleDown" -> FALLING_FAST
                else -> UNKNOWN
            }
        }
    }
}
