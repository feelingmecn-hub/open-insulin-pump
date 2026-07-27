package com.openloop.pump.domain.algorithm

import com.openloop.pump.domain.model.Dose
import kotlin.math.exp

/**
 * 体内胰岛素 (IOB) 计算 —— 指数衰减模型。
 *
 * 单笔剂量的剩余活性采用 oref / OpenAPS 常用的指数曲线:
 *   IOB(t) = dose × (1 − (1 + t/τ)·e^(−t/τ))
 * 其中 τ 为作用时间常数（默认 4 小时，对应固件 IOB_DURATION_HOURS）。
 */
object IobCalculator {

    /** 单笔剂量在 [minutesAgo] 分钟后的剩余 IOB (U)。 */
    fun iobForDose(units: Double, minutesAgo: Double, tauHours: Double = 4.0): Double {
        if (minutesAgo <= 0.0) return units
        val t = minutesAgo / 60.0
        val iob = units * (1.0 - (1.0 + t / tauHours) * exp(-t / tauHours))
        return iob.coerceAtLeast(0.0)
    }

    /** 多笔剂量在 [now] 时刻的总 IOB (U)。 */
    fun totalIob(doses: List<Dose>, now: Long, tauHours: Double = 4.0): Double {
        return doses.sumOf { d ->
            val minsAgo = (now - d.timestamp) / 60000.0
            iobForDose(d.units, minsAgo, tauHours)
        }
    }

    /**
     * 基于当前 IOB 与 ISF 估算"活性胰岛素等效血糖影响"
     * （即若不再进食，IOB 还会再降低多少 mg/dL）。
     */
    fun iobGlucoseImpact(iob: Double, isf: Double): Double = iob * isf
}
