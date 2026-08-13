package com.openloop.pump.data.repository

import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpBleManager
import com.openloop.pump.ble.PumpProtocol
import com.openloop.pump.domain.model.BolusRequest
import kotlinx.coroutines.flow.StateFlow
import javax.inject.Inject
import javax.inject.Singleton

/**
 * 泵仓库 —— 包装 [PumpBleManager]，向 UI / ViewModel 暴露统一接口。
 *
 * 领域状态映射（toDomain）在 ViewModel 中完成，保持仓库层无领域依赖之外的最小转换。
 */
@Singleton
class PumpRepository @Inject constructor(
    private val ble: PumpBleManager
) {
    val connectionState: StateFlow<ConnectionState> = ble.connectionState
    val pumpStatusFlow: StateFlow<PumpProtocol.PumpStatus?> = ble.pumpStatus
    val iob: StateFlow<Double> = ble.iob
    val reservoir: StateFlow<Int> = ble.reservoir
    /** 实时状态（SCREEN 通道 20 字节二进制），供原生虚拟屏重画。 */
    val pumpLiveState: StateFlow<PumpProtocol.PumpLiveState?> = ble.pumpLiveState

    fun startScan() = ble.startScan()
    fun stopScan() = ble.stopScan()

    suspend fun connect(address: String): Boolean {
        val ok = ble.connect(address)
        if (ok) ble.pairingPin = null // 默认 Just Works；如需 PIN 由设置页写入
        return ok
    }

    fun disconnect() = ble.disconnect()

    /** 发送大剂量，并可在外部落库 / 同步。 */
    suspend fun deliverBolus(req: BolusRequest) =
        ble.sendBolus(req.units, req.durationMin, req.isExtended)

    suspend fun updateBasalSlot(slot: Int, rateUh: Double) =
        ble.setBasalSlot(slot, rateUh)

    suspend fun setTemporaryBasal(rateUh: Double, durationMin: Int) =
        ble.setTemporaryBasal(rateUh, durationMin)

    suspend fun refreshStatus() = ble.refreshStatus()

    /**
     * 回传血糖给泵（mg/dL + 五档趋势码 -2..2），供泵屏显示用。
     * AAPS 协议不下发血糖，此路是泵获取 CGM 值的唯一来源（伴生 App 收 xDrip 后转发）。
     * 纯显示数据，无安全/计费影响。
     */
    suspend fun sendCgm(mgdl: Int, trend: Int) = ble.sendCgm(mgdl, trend)

    /**
     * 基础率验证测试（CONTROL 0x18）：让泵把当前激活方案 24 段的总量一次性打出。
     *
     * 用途：确认「基础率到底有没有写进泵、泵会不会真的驱动电机」。
     * 泵侧把它记为独立事件「基础率验证」，**不计入**大剂量次数与 IOB，
     * 历史条目附带实际微步数，可与丝杠位移互相印证。
     */
    suspend fun runBasalTest() =
        ble.sendControl(com.openloop.pump.ble.PumpProtocolSpec.CTRL_CMD_BASAL_TEST)
}
