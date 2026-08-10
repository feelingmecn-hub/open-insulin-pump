package com.openloop.pump.ble

import kotlin.math.roundToInt

/**
 * 泵控制协议编解码层 —— 必须与 ESP32-C6 固件 [ble_comm.cpp] 完全对齐。
 *
 * 设计要点（阶段6 重写，按真实固件行为）：
 *   - 所有"写"命令均在 payload 末字节追加 CRC-8/CCITT(poly 0x07, init 0x00)。
 *   - 写帧不经 opcode 包装，直接传裸值（固件端按特征值含义解析）：
 *       BOLUS  : [units_x100 u32 LE][crc]                  (5B)
 *       BASAL  : [rate f32 LE][crc]                         (5B)  ← 当前基础率(U/h)
 *       TBR    : [percent u8][rate_x100 u16 LE][dur u16 LE][crc] (7B)
 *       CGM    : [mgdl u16 LE][trend i8][crc]              (5B)
 *       CONTROL: [mode_or_cmd u8][crc]                      (2B)
 *       SETTINGS:[op u8][payload...][crc]                   (变长)
 *       KEY    : [key_event_t u8][crc]                      (2B)  0=松开
 *   - 通知(泵→App, 1Hz):
 *       STATUS  : ASCII JSON  {"bat":..,"st":..,"alm":..,"glu":..,"tr":..,"loop":..,"tbr":..}
 *       IOB     : ASCII 文本  "%.2f"
 *       RESERVOIR: ASCII 文本 "%d"
 *       SCREEN  : 20 字节紧凑二进制实时状态 (见 PumpLiveState / parsePumpState)
 */
object PumpProtocol {

    // ============================================================
    // 命令构建（写 → 泵）
    // ============================================================

    /**
     * 大剂量命令 —— 5 字节：[units_x100 u32 LE][crc]
     * 注：固件伴生通道仅支持普通大剂量；方波/时长由泵本地逻辑处理，
     * 此处忽略 durationMin / isExtended（保持调用签名兼容旧 UI）。
     */
    fun buildBolus(units: Double, durationMin: Int = 0, isExtended: Boolean = false): ByteArray {
        require(units in 0.0..PumpProtocolSpec.MAX_BOLUS_UNITS) {
            "大剂量超出范围 0..${PumpProtocolSpec.MAX_BOLUS_UNITS}U"
        }
        val ux100 = (units * 100).roundToInt().coerceAtLeast(0)
        val buf = byteArrayOf(
            (ux100 and 0xFF).toByte(),
            ((ux100 shr 8) and 0xFF).toByte(),
            ((ux100 shr 16) and 0xFF).toByte(),
            ((ux100 shr 24) and 0xFF).toByte(),
            0
        )
        buf[4] = Crc8.ccitt(buf, 0, 4)
        return buf
    }

    /**
     * 当前基础率命令 —— 5 字节：[rate f32 LE][crc]
     * 设置泵当前运行基础率 (U/h)。24 段基础率档案的编辑在泵端完成，
     * App 侧仅做本地规划展示（见 basal 屏）。
     */
    fun buildBasalRate(rateUh: Double): ByteArray {
        require(rateUh in 0.0..PumpProtocolSpec.MAX_BASAL_RATE) { "基础率超出范围" }
        val bits = rateUh.toFloat().toRawBits()
        val buf = byteArrayOf(
            (bits and 0xFF).toByte(),
            ((bits shr 8) and 0xFF).toByte(),
            ((bits shr 16) and 0xFF).toByte(),
            ((bits shr 24) and 0xFF).toByte(),
            0
        )
        buf[4] = Crc8.ccitt(buf, 0, 4)
        return buf
    }

    /**
     * 临时基础率 (TBR) —— 7 字节：[percent u8][rate_x100 u16 LE][dur u16 LE][crc]
     * percent=100 表示按 rate 全量；percent=0 等效取消(归零)。
     */
    fun buildTbr(
        rateUh: Double,
        durationMin: Int,
        percent: Int = PumpProtocolSpec.DEFAULT_TBR_PERCENT
    ): ByteArray {
        require(rateUh in 0.0..PumpProtocolSpec.MAX_BASAL_RATE) { "TBR 速率超出范围" }
        val rateX100 = (rateUh * 100).roundToInt().coerceAtLeast(0).coerceAtMost(65535)
        val p = percent.coerceIn(0, 255)
        val d = durationMin.coerceIn(0, 65535)
        val buf = byteArrayOf(
            p.toByte(),
            (rateX100 and 0xFF).toByte(),
            ((rateX100 shr 8) and 0xFF).toByte(),
            (d and 0xFF).toByte(),
            ((d shr 8) and 0xFF).toByte(),
            0
        )
        buf[5] = Crc8.ccitt(buf, 0, 5)
        return buf
    }

    /**
     * 控制命令。
     * 无参（默认）：2 字节 [cmd u8][crc]（环模式 0/1/2；0x10 排气默认1U / 0x11 清报警 / 0x12 退回）。
     * 带参：6 字节 [cmd u8][param f32 LE][crc]（0x10 排气带 ml / 0x13 标定推出量 / 0x14 标定系数）。
     */
    fun buildControl(modeOrCmd: Int, param: Float? = null): ByteArray {
        return if (param == null) {
            val buf = byteArrayOf((modeOrCmd and 0xFF).toByte(), 0)
            buf[1] = Crc8.ccitt(buf, 0, 1)
            buf
        } else {
            val bits = param.toRawBits()
            val buf = byteArrayOf(
                (modeOrCmd and 0xFF).toByte(),
                (bits and 0xFF).toByte(),
                ((bits shr 8) and 0xFF).toByte(),
                ((bits shr 16) and 0xFF).toByte(),
                ((bits shr 24) and 0xFF).toByte(),
                0
            )
            buf[5] = Crc8.ccitt(buf, 0, 5)
            buf
        }
    }

    /** CGM 血糖回传 —— 5 字节：[mgdl u16 LE][trend i8][crc]（trend 取 -2..2 五档显示码）。 */
    fun buildCgm(mgdl: Int, trend: Int): ByteArray {
        val m = mgdl.coerceIn(0, 65535)
        val t = (trend.coerceIn(-128, 127)).toByte()
        val buf = byteArrayOf(
            (m and 0xFF).toByte(),
            ((m shr 8) and 0xFF).toByte(),
            t,
            0
        )
        buf[3] = Crc8.ccitt(buf, 0, 3)
        return buf
    }

    /** 设置通道命令 —— 变长：[op u8][payload...][crc]。 */
    fun buildSettings(op: Int, payload: ByteArray = byteArrayOf()): ByteArray {
        val buf = ByteArray(1 + payload.size + 1)
        buf[0] = (op and 0xFF).toByte()
        payload.copyInto(buf, 1)
        buf[buf.size - 1] = Crc8.ccitt(buf, 0, buf.size - 1)
        return buf
    }

    /** 小端读 f32（从响应字节 b 的 off 处）。 */
    fun leF32(b: ByteArray, off: Int): Float =
        Float.fromBits(
            (b[off].toInt() and 0xFF)
                or ((b[off + 1].toInt() and 0xFF) shl 8)
                or ((b[off + 2].toInt() and 0xFF) shl 16)
                or ((b[off + 3].toInt() and 0xFF) shl 24)
        )

    /** 小端读 u16。 */
    fun leU16(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)

    /** 小端读 u32。 */
    fun leU32(b: ByteArray, off: Int): Long =
        (leU16(b, off).toLong() and 0xFFFF) or ((leU16(b, off + 2).toLong() and 0xFFFF) shl 16)

    /**
     * 远程按键命令 —— 2 字节：[key_event_t u8][crc]。
     * event=0 表示松开(停止自动重复)；1..6 见 PumpProtocolSpec.KEY_*。
     */
    fun buildKey(event: Int): ByteArray {
        val buf = byteArrayOf((event and 0xFF).toByte(), 0)
        buf[1] = Crc8.ccitt(buf, 0, 1)
        return buf
    }

    /**
     * 手动电机控制命令 —— 8 字节：[0x15][dir u8][steps u32 LE][speed u16 LE][crc]。
     * dir 见 PumpProtocolSpec.MANUAL_DIR_*；steps=0 表示连续点动直到 STOP；speed=0 用默认低速。
     * 专用于电机测试，不记账（不改储药器/IOB）。必须与固件 ble_comm.cpp CONTROL 0x15 对齐。
     */
    fun buildManualMove(dir: Int, steps: Long, speedHz: Int): ByteArray {
        val d = (dir and 0xFF).toByte()
        val s = steps.coerceIn(0L, 0xFFFFFFFFL)
        val sp = speedHz.coerceIn(0, 65535)
        val buf = byteArrayOf(
            PumpProtocolSpec.CTRL_CMD_MANUAL_MOVE.toByte(),
            d,
            (s and 0xFF).toByte(),
            ((s shr 8) and 0xFF).toByte(),
            ((s shr 16) and 0xFF).toByte(),
            ((s shr 24) and 0xFF).toByte(),
            (sp and 0xFF).toByte(),
            ((sp shr 8) and 0xFF).toByte(),
            0
        )
        buf[8] = Crc8.ccitt(buf, 0, 8)
        return buf
    }

    /** 手动停止（连续点动退出）—— 2 字节：[0x16][crc]。 */
    fun buildManualStop(): ByteArray {
        val buf = byteArrayOf(PumpProtocolSpec.CTRL_CMD_MANUAL_STOP.toByte(), 0)
        buf[1] = Crc8.ccitt(buf, 0, 1)
        return buf
    }

    // ============================================================
    // 响应解析（泵 → App 通知，全部为 ASCII 文本）
    // ============================================================

    /**
     * 泵状态（STATUS 特征通知/读取的 ASCII JSON）。
     * 保留旧字段以兼容既有 UI；reservoir/iob/delivered 由各自独立通道推送，此处置 0。
     */
    data class PumpStatus(
        val stateCode: Int,
        val alarmCode: Int,
        val deliveredUnits: Double,   // 固件 status 帧不再提供, 默认 0
        val reservoirUnits: Int,      // 由 reservoir 通道单独推送 (此处 0)
        val batteryMv: Int,           // 固件 status 仅给 pct, 默认 0
        val batteryPct: Int,
        val iobUnits: Double,         // 由 iob 通道单独推送 (此处 0)
        val glucoseMgdl: Int,         // 新增: status.glu
        val trend: Int,               // 新增: status.tr (-2..2)
        val loopMode: Int,            // 新增: status.loop (0/1/2)
        val tbrPercent: Int           // 新增: status.tbr
    ) {
        val glucoseMmol: Double
            get() = if (glucoseMgdl > 0) glucoseMgdl / 18.01559 else 0.0

        companion object {
            fun fromString(text: String): PumpStatus? = runCatching {
                PumpStatus(
                    stateCode = optInt(text, "st"),
                    alarmCode = optInt(text, "alm"),
                    deliveredUnits = 0.0,
                    reservoirUnits = 0,
                    batteryMv = 0,
                    batteryPct = optInt(text, "bat"),
                    iobUnits = 0.0,
                    glucoseMgdl = optInt(text, "glu"),
                    trend = optInt(text, "tr"),
                    loopMode = optInt(text, "loop"),
                    tbrPercent = optInt(text, "tbr")
                )
            }.getOrNull()
        }
    }

    /** STATUS 特征（ASCII JSON）。 */
    fun parseStatus(data: ByteArray): PumpStatus? =
        runCatching { String(data, Charsets.UTF_8) }.getOrNull()?.let { PumpStatus.fromString(it) }

    // ============================================================
    // 实时状态（SCREEN 特征通知，20 字节紧凑二进制）
    // 布局与固件 ble_comm.cpp::notify_pump_state 严格对齐（小端）。
    // ============================================================

    /**
     * 泵实时状态 —— 固件每 1Hz 通过 SCREEN 通道推送的紧凑二进制包解码结果。
     * App 据此**自行重画**原生虚拟屏（不再镜像 LVGL 屏），彻底规避大 JSON / 分片 / MTU 问题。
     */
    data class PumpLiveState(
        val mode: Int,              // pump_state_t: 0启动 1待机 2排气 3输注 4基础率 5大剂量 6停止 7报警 8休眠 9故障
        val loopMode: Int,          // 0 闭环(AAPS接管) / 1 开环 / 2 暂停
        val keypadLocked: Boolean,
        val alarmActive: Boolean,
        val batteryPct: Int,        // 0-100
        val alarmCode: Int,         // alarm_code_t, 0=无
        val glucoseMgdl: Int,       // mg/dL, 0=无
        val glucoseTrend: Int,      // -2..2 五档
        val tbrPercent: Int,        // 0=无
        val extBolusActive: Boolean,
        val stepLoss: Boolean,
        val bolusProgressPct: Int,  // 0-100
        val reservoirUnits: Double, // 剩余药量 U
        val iobUnits: Double,       // IOB U
        val basalRateUh: Double,    // 当前基础率 U/h
        val todayUnits: Double,     // 今日累计注射 U
        val clockMin: Int           // HH*60+MM
    ) {
        val glucoseMmol: Double
            get() = if (glucoseMgdl > 0) glucoseMgdl / 18.01559 else 0.0
        val clockText: String
            get() = "%02d:%02d".format(clockMin / 60, clockMin % 60)
    }

    /** 解码固件 20 字节二进制实时状态；非法包(长度不足/魔数不符)返回 null。 */
    fun parsePumpState(data: ByteArray): PumpLiveState? {
        if (data.size < 20) return null
        if (data[0].toInt() and 0xFF != 0xA1) return null
        val f1 = data[1].toInt() and 0xFF
        val f2 = data[6].toInt() and 0xFF
        val u16 = { off: Int ->
            ((data[off + 1].toInt() and 0xFF) shl 8) or (data[off].toInt() and 0xFF)
        }
        return PumpLiveState(
            mode = f1 and 0x0F,
            loopMode = (f1 shr 4) and 0x03,
            keypadLocked = (f1 and 0x40) != 0,
            alarmActive = (f1 and 0x80) != 0,
            batteryPct = data[2].toInt() and 0xFF,
            alarmCode = data[3].toInt() and 0xFF,
            glucoseMgdl = u16(14),
            glucoseTrend = (data[4].toInt() and 0xFF) - 128,
            tbrPercent = data[5].toInt() and 0xFF,
            extBolusActive = (f2 and 0x01) != 0,
            stepLoss = (f2 and 0x02) != 0,
            bolusProgressPct = data[7].toInt() and 0xFF,
            reservoirUnits = u16(8) / 10.0,
            iobUnits = u16(10) / 100.0,
            basalRateUh = u16(12) / 100.0,
            todayUnits = u16(16) / 100.0,
            clockMin = u16(18)
        )
    }

    // ============================================================
    // 导航状态（SCREEN 特征通知，13 字节紧凑二进制，魔数 0xB1）
    // 布局与固件 ui_screen.cpp::ui_screen_dump_nav_binary 严格对齐。
    // 用于 App 复刻与泵屏幕一致的可交互菜单（屏幕/选中/编辑值）。
    // ============================================================

    /**
     * 泵屏导航状态 —— 固件每次屏幕变化 / 1Hz 推送。
     * v0..v3 含义随 screen 变化（见 PumpMenu 各屏定义）：
     *   大剂量普通/方波: v0=剂量_x100(U) 方波/双波 v1=时长h
     *   双波: v0=立即_x100 v1=方波_x100 v2=时长h
     *   向导: v0=血糖_x10(mmol) v1=碳水_g
     *   餐时: v0=三餐选中项
     *   基础率: v0=速率_x100(U/h)
     *   TBR: v0=百分比_x10 v1=时长(30min单位)
     *   排气: v0=排气量_x10(U)
     *   时钟设置: v0=当前编辑字段值
     */
    data class PumpNav(
        val screen: Int,           // ui_screen_id (0..24)
        val sel: Int,              // 当前选中项索引
        val edit: Boolean,         // 是否处于编辑态(SET 已进入)
        val primeActive: Boolean,  // 正在排气中
        val clockValid: Boolean,   // 时钟已设置
        val keypadSound: Boolean,  // 按键音开关
        val v0: Int,
        val v1: Int,
        val v2: Int,
        val v3: Int
    )

    /** 解码固件 13 字节二进制导航；非法包返回 null。 */
    fun parsePumpNav(data: ByteArray): PumpNav? {
        if (data.size < 13) return null
        if (data[0].toInt() and 0xFF != 0xB1) return null
        val u16 = { off: Int ->
            ((data[off + 1].toInt() and 0xFF) shl 8) or (data[off].toInt() and 0xFF)
        }
        val f = data[4].toInt() and 0xFF
        return PumpNav(
            screen = data[1].toInt() and 0xFF,
            sel = data[2].toInt() and 0xFF,
            edit = (data[3].toInt() and 0xFF) != 0,
            primeActive = (f and 0x01) != 0,
            clockValid = (f and 0x02) != 0,
            keypadSound = (f and 0x04) != 0,
            v0 = u16(5),
            v1 = u16(7),
            v2 = u16(9),
            v3 = u16(11)
        )
    }

    // ============================================================
    // JSON 文本辅助（轻量，避免引入额外依赖；服务于 PumpStatus.fromString）
    // ============================================================

    private fun optInt(json: String, key: String): Int {
        val m = Regex("\"$key\"\\s*:\\s*(-?\\d+)").find(json) ?: return 0
        return m.groupValues[1].toIntOrNull() ?: 0
    }

    private fun optFloat(json: String, key: String): Double {
        val m = Regex("\"$key\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)").find(json) ?: return 0.0
        return m.groupValues[1].toDoubleOrNull() ?: 0.0
    }
}
