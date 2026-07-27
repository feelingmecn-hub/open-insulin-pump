package com.openloop.pump.domain.model

/**
 * 胰岛素剂量记录 —— 用于 IOB 计算。
 *
 * @param units      剂量 (U)
 * @param timestamp  给药 Unix 毫秒时间
 * @param type       剂量类型（大剂量 / 基础率片段）
 * @param durationMin 持续时长（基础率片段为 3 分钟基础片，大剂量方波为延长时间）
 */
data class Dose(
    val units: Double,
    val timestamp: Long,
    val type: Type,
    val durationMin: Int = 0
) {
    enum class Type { BOLUS, BASAL_SEGMENT }
}
