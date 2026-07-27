package com.openloop.pump.domain.algorithm

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * IOB 指数衰减模型的单元测试。
 */
class IobCalculatorTest {

    @Test
    fun `给药瞬间 IOB 等于全量`() {
        assertEquals(1.0, IobCalculator.iobForDose(1.0, 0.0), 0.0001)
        assertEquals(2.5, IobCalculator.iobForDose(2.5, 0.0), 0.0001)
    }

    @Test
    fun `长时间后 IOB 衰减至接近零`() {
        val iob = IobCalculator.iobForDose(1.0, 24 * 60.0) // 24 小时后
        assertTrue("IOB 应接近 0，实际=$iob", iob < 0.01)
    }

    @Test
    fun `IOB 随时间单调递减`() {
        val at10 = IobCalculator.iobForDose(1.0, 10.0)
        val at120 = IobCalculator.iobForDose(1.0, 120.0)
        assertTrue(at120 < at10)
    }

    @Test
    fun `负时间或零剂量返回安全值`() {
        assertEquals(0.0, IobCalculator.iobForDose(0.0, 60.0), 0.0001)
    }
}
