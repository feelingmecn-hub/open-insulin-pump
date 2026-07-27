package com.openloop.pump.ble

/**
 * CRC-8/CCITT 实现 —— 与 ESP32 固件 [ble_gatt_server.cpp] 中 crc8_ccitt() 逐位一致。
 *
 * 多项式 0x07，初始值 0x00，非反射（no reflection）。
 *
 * 固件 C 实现:
 * ```
 * uint8_t crc = 0x00;
 * for (i...) { crc ^= data[i];
 *     for (j=0;j<8;j++) {
 *         if (crc & 0x80) crc = (crc << 1) ^ 0x07;
 *         else            crc <<= 1;
 *     }
 * }
 * ```
 */
object Crc8 {

    /** 计算 data[0..n) 的 CRC-8，返回单字节。 */
    fun ccitt(data: ByteArray, offset: Int = 0, length: Int = data.size - offset): Byte {
        var crc = 0
        val end = offset + length
        for (i in offset until end) {
            crc = crc xor (data[i].toInt() and 0xFF)
            repeat(8) {
                crc = if (crc and 0x80 != 0) {
                    ((crc shl 1) xor 0x07) and 0xFF
                } else {
                    (crc shl 1) and 0xFF
                }
            }
        }
        return crc.toByte()
    }

    /** 校验整帧（末字节为 CRC），返回是否通过。 */
    fun verify(data: ByteArray): Boolean {
        if (data.isEmpty()) return false
        val calc = ccitt(data, 0, data.size - 1)
        return calc == data[data.size - 1]
    }
}
