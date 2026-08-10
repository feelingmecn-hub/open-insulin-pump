package com.openloop.pump.domain.model

/**
 * 大剂量请求（用户发起）。
 *
 * @param units       剂量 (U)，精度 0.1U
 * @param isExtended  是否为方波大剂量（延长推注）
 * @param durationMin 方波时长（分钟）
 * @param note        备注（餐前 / 校正 等）
 */
data class BolusRequest(
    val units: Double,
    val isExtended: Boolean = false,
    val durationMin: Int = 0,
    val note: String = "",
    val timestamp: Long = System.currentTimeMillis()
) {
    init {
        require(units > 0) { "剂量必须为正" }
        require(units <= 100.0) { "单次大剂量不可超过 100U" }
        require(durationMin in 0..600) { "方波时长超出范围" }
    }
}
