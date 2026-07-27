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

    suspend fun refreshStatus() = ble.readStatus()
}
