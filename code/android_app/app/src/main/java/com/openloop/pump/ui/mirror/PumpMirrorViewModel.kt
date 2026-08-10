package com.openloop.pump.ui.mirror

import androidx.lifecycle.ViewModel
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpBleManager
import com.openloop.pump.ble.PumpProtocol
import com.openloop.pump.ble.PumpProtocolSpec
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * 虚拟泵屏 ViewModel —— 订阅泵屏镜像快照，并把 4 键操作透传到 KEY 通道。
 *
 * 与泵上 4 个物理按键同一路径 (ui_screen_key / ui_screen_release)，
 * 因此 App 上操作后泵屏必然同步。
 */
@HiltViewModel
class PumpMirrorViewModel @Inject constructor(
    private val ble: PumpBleManager
) : ViewModel() {

    val connectionState: StateFlow<ConnectionState> = ble.connectionState
    val pumpLiveState: StateFlow<PumpProtocol.PumpLiveState?> = ble.pumpLiveState
    val pumpNav: StateFlow<PumpProtocol.PumpNav?> = ble.pumpNav

    fun ensureConnected() {
        val s = ble.connectionState.value
        if (s !is ConnectionState.Connected && s !is ConnectionState.Connecting
            && s !is ConnectionState.Bonded && s !is ConnectionState.Scanning
        ) {
            ble.startScan()
        }
    }

    /**
     * 按键按下。suspend 直到送达固件；调用方须保证 press 先于 release 到达，
     * 否则 release 抢先 → 固件认为按键一直按住 → ui_screen_key 自动重复乱跳。
     */
    suspend fun pressKey(event: Int): Result<Unit> = ble.sendKey(event)

    /** 松手：停止"按住自动重复"。suspend 直到送达。 */
    suspend fun releaseKey(): Result<Unit> = ble.releaseKey()

    // 便捷常量（与固件 key_event_t / PumpProtocolSpec 一致）
    val KEY_UP get() = PumpProtocolSpec.KEY_UP.toInt()
    val KEY_DOWN get() = PumpProtocolSpec.KEY_DOWN.toInt()
    val KEY_SET get() = PumpProtocolSpec.KEY_SET.toInt()
    val KEY_ESC get() = PumpProtocolSpec.KEY_ESC.toInt()
    val KEY_LONG_SET get() = PumpProtocolSpec.KEY_LONG_SET.toInt()
    val KEY_LONG_ESC get() = PumpProtocolSpec.KEY_LONG_ESC.toInt()
}
