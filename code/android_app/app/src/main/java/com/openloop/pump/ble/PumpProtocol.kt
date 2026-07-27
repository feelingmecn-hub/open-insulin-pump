package com.openloop.pump.ble

import kotlin.math.roundToInt

/**
 * 泵控制协议编解码层。
 *
 * 所有帧均为小端（little-endian）二进制，末字节为 CRC-8/CCITT，
 * 与 ESP32 固件 [ble_gatt_server.cpp] 一一对应。
 */
object PumpProtocol {

    // ============================================================
    // 命令构建
    // ============================================================

    /**
     * 大剂量命令 —— 8 字节
     * [0] opcode=0x01
     * [1..4] units_x100  (LE u32, 例 500 = 5.00U)
     * [5] duration_min   (方波大剂量时长; 普通为 0)
     * [6] flags          (bit0 = 方波大剂量)
     * [7] CRC-8
     */
    fun buildBolus(units: Double, durationMin: Int = 0, isExtended: Boolean = false): ByteArray {
        require(units in 0.0..PumpProtocolSpec.MAX_BOLUS_UNITS) {
            "大剂量超出范围 0..${PumpProtocolSpec.MAX_BOLUS_UNITS}U"
        }
        val unitsX100 = (units * 100).roundToInt().coerceAtLeast(0)
        val flags = if (isExtended) PumpProtocolSpec.FLAG_EXTENDED.toInt() else 0
        val buf = byteArrayOf(
            PumpProtocolSpec.OPCODE_BOLUS,
            (unitsX100 and 0xFF).toByte(),
            ((unitsX100 shr 8) and 0xFF).toByte(),
            ((unitsX100 shr 16) and 0xFF).toByte(),
            ((unitsX100 shr 24) and 0xFF).toByte(),
            (durationMin and 0xFF).toByte(),
            flags.toByte(),
            0
        )
        buf[7] = Crc8.ccitt(buf, 0, 7)
        return buf
    }

    /**
     * 基础率单槽更新 —— 7 字节
     * [0] opcode=0x02
     * [1] slot         (0..23)
     * [2..5] rate_x1000 (LE u32, rate_uh = rate_x1000/1000)
     * [6] CRC-8
     */
    fun buildBasalSlot(slot: Int, rateUh: Double): ByteArray {
        require(slot in 0..23) { "基础率槽位必须 0..23" }
        require(rateUh in 0.0..PumpProtocolSpec.MAX_BASAL_RATE) { "基础率超出范围" }
        val rateX1000 = (rateUh * 1000).roundToInt().coerceAtLeast(0)
        val buf = byteArrayOf(
            PumpProtocolSpec.OPCODE_BASAL,
            slot.toByte(),
            (rateX1000 and 0xFF).toByte(),
            ((rateX1000 shr 8) and 0xFF).toByte(),
            ((rateX1000 shr 16) and 0xFF).toByte(),
            ((rateX1000 shr 24) and 0xFF).toByte(),
            0
        )
        buf[6] = Crc8.ccitt(buf, 0, 6)
        return buf
    }

    /**
     * 临时基础率 (TBR) 命令 —— 7 字节
     * [0] opcode=0x03
     * [1..4] rate_x1000 (LE u32)
     * [5] duration_min
     * [6] CRC-8
     */
    fun buildTbr(rateUh: Double, durationMin: Int): ByteArray {
        require(rateUh in 0.0..PumpProtocolSpec.MAX_BASAL_RATE) { "TBR 速率超出范围" }
        val rateX1000 = (rateUh * 1000).roundToInt().coerceAtLeast(0)
        val buf = byteArrayOf(
            PumpProtocolSpec.OPCODE_TBR,
            (rateX1000 and 0xFF).toByte(),
            ((rateX1000 shr 8) and 0xFF).toByte(),
            ((rateX1000 shr 16) and 0xFF).toByte(),
            ((rateX1000 shr 24) and 0xFF).toByte(),
            (durationMin and 0xFF).toByte(),
            0
        )
        buf[6] = Crc8.ccitt(buf, 0, 6)
        return buf
    }

    // ============================================================
    // 响应解析
    // ============================================================

    /**
     * 泵状态帧（16 字节，Status 特征的 Read/Notify）。
     *
     * [0] current_state     (pump_state_t)
     * [1] alarm_code        (alarm_code_t)
     * [2..5] delivered_x100 (LE u32, 已注射 U×100)
     * [6..7] reservoir      (LE u16, 剩余药量 U)
     * [8..9] battery_mv     (LE u16)
     * [10] battery_pct      (0..100)
     * [11..14] iob_x100     (LE u32, IOB×100 —— 注意: 此处为 iob_x10000/100)
     * [15] CRC-8
     */
    data class PumpStatus(
        val stateCode: Int,
        val alarmCode: Int,
        val deliveredUnits: Double,
        val reservoirUnits: Int,
        val batteryMv: Int,
        val batteryPct: Int,
        val iobUnits: Double
    ) {
        companion object {
            fun fromBytes(data: ByteArray): PumpStatus? {
                if (data.size < PumpProtocolSpec.STATUS_FRAME_LEN) return null
                if (!Crc8.verify(data)) return null
                val deliveredX100 = leU32(data, 2)
                val reservoir = leU16(data, 6)
                val batteryMv = leU16(data, 8)
                val batteryPct = data[10].toInt() and 0xFF
                val iobX100 = leU32(data, 11)
                return PumpStatus(
                    stateCode = data[0].toInt() and 0xFF,
                    alarmCode = data[1].toInt() and 0xFF,
                    deliveredUnits = deliveredX100 / 100.0,
                    reservoirUnits = reservoir,
                    batteryMv = batteryMv,
                    batteryPct = batteryPct,
                    iobUnits = iobX100 / 100.0
                )
            }
        }
    }

    /** IOB 特征（4 字节）= iob_x10000 (LE u32)。 */
    fun parseIob(data: ByteArray): Double? {
        if (data.size < PumpProtocolSpec.IOB_FRAME_LEN) return null
        return leU32(data, 0) / 10000.0
    }

    /** Reservoir 特征（2 字节）= reservoir_units_left (LE u16, 单位 U)。 */
    fun parseReservoir(data: ByteArray): Int? {
        if (data.size < PumpProtocolSpec.RESERVOIR_FRAME_LEN) return null
        return leU16(data, 0)
    }

    // ============================================================
    // 小端辅助
    // ============================================================

    private fun leU16(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)

    private fun leU32(b: ByteArray, off: Int): Long =
        (b[off].toLong() and 0xFF) or
            ((b[off + 1].toLong() and 0xFF) shl 8) or
            ((b[off + 2].toLong() and 0xFF) shl 16) or
            ((b[off + 3].toLong() and 0xFF) shl 24)
}
