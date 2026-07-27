package com.openloop.pump.domain.algorithm

import kotlin.math.roundToInt

/**
 * 血糖预测 —— 基于当前血糖、IOB 与 ISF 的简单线性/衰减预测。
 *
 * 简化假设：IOB 在未来 [minutes] 分钟内按匀速降低血糖（乘以时间比例）。
 * 生产实现应结合近期血糖趋势 (CGM slope) 做更精确的 AR 预测。
 */
object GlucoseForecast {

    /**
     * 预测 [minutes] 分钟后的血糖 (mg/dL)。
     *
     * @param currentMgdl 当前血糖
     * @param iob         当前 IOB (U)
     * @param isf         胰岛素敏感系数 (mg/dL per U)
     * @param minutes     预测时长（默认 30 分钟）
     */
    fun predict(currentMgdl: Int, iob: Double, isf: Double, minutes: Int = 30): Int {
        val drop = iob * isf * (minutes / 60.0)
        return (currentMgdl - drop).roundToInt().coerceAtLeast(40)
    }
}
