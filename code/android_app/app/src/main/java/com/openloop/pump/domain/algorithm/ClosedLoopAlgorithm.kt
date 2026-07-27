package com.openloop.pump.domain.algorithm

import kotlin.math.roundToInt

/**
 * 闭环算法（oref1 简化版）。
 *
 * 安全原则：闭环**只调整临时基础率 (TBR)**，绝不自动给出大剂量。
 * 大剂量（校正）必须由用户确认后发起 —— 这是医疗安全底线。
 *
 * 逻辑：
 *  - 预测未来 30 分钟血糖
 *  - 偏高 (> target+30)：提高 TBR（受 maxIOB 与 maxBasal 限制）
 *  - 偏低 (< target−30)：降低/暂停 TBR
 *  - 正常区间：维持当前基础率
 */
class ClosedLoopAlgorithm(
    private val maxBasalRate: Double = 5.0,
    private val maxIob: Double = 4.0,
    private val defaultIsf: Double = 50.0,
    private val targetGlucose: Int = 110
) {

    data class Inputs(
        val glucoseMgdl: Int,
        val iob: Double,
        val currentBasal: Double,
        val isf: Double = defaultIsf,
        val target: Int = targetGlucose
    )

    data class Outputs(
        val tbrRateUh: Double,
        val tbrDurationMin: Int = 30,
        /** 建议大剂量（U）—— 仅作为 UI 推荐，需用户确认。 */
        val recommendedBolus: Double = 0.0
    )

    fun compute(inputs: Inputs): Outputs {
        val predicted = GlucoseForecast.predict(
            inputs.glucoseMgdl, inputs.iob, inputs.isf, 30
        )
        val deviation = predicted - inputs.target

        return when {
            deviation > 30 -> {            // 高血糖 → 提升 TBR
                val correction = (deviation / inputs.isf).coerceAtLeast(0.0)
                val projectedIob = inputs.iob + correction
                val rate = if (projectedIob <= maxIob) {
                    (inputs.currentBasal + correction * 2.0).coerceAtMost(maxBasalRate)
                } else {
                    inputs.currentBasal
                }
                Outputs(rate.roundTo025(), 30)
            }

            deviation < -30 -> {          // 低血糖 → 降低 / 暂停 TBR
                val reduce = (-deviation / inputs.isf).coerceAtLeast(0.0)
                Outputs((inputs.currentBasal - reduce).coerceAtLeast(0.0).roundTo025(), 30)
            }

            else -> Outputs(inputs.currentBasal, 30)   // 正常 → 维持
        }
    }

    /** 计算纯校正大剂量推荐值（需用户确认），不含进食碳水。 */
    fun recommendCorrectionBolus(glucoseMgdl: Int, iob: Double, isf: Double, target: Int): Double {
        val correctionNeeded = glucoseMgdl - target
        if (correctionNeeded <= 0) return 0.0
        val raw = (correctionNeeded / isf) - iob
        return raw.coerceAtLeast(0.0).roundTo025()
    }

    private fun Double.roundTo025(): Double = (this * 40).roundToInt() / 40.0
}
