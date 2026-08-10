package com.openloop.pump.ui.motortest

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpBleManager
import com.openloop.pump.ble.PumpProtocol
import com.openloop.pump.ble.PumpProtocolSpec
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

/**
 * 电机手动测试 ViewModel —— 直发 BLE CONTROL 0x15/0x16 与 SETTINGS 0x2A，
 * 绕过剂量记账，仅用于电机/丝杠调试。不推送大剂量、不改储药器/IOB。
 */
@HiltViewModel
class MotorTestViewModel @Inject constructor(
    private val ble: PumpBleManager
) : ViewModel() {

    val connectionState: StateFlow<ConnectionState> = ble.connectionState

    private val _motorPosition = MutableStateFlow(0L)
    val motorPosition: StateFlow<Long> = _motorPosition.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    /** 定量点动：steps>0 走指定步数；steps=0 表示连续点动直到 stop()。 */
    fun move(dir: Int, steps: Long, speedHz: Int) {
        _lastError.value = null
        viewModelScope.launch {
            _busy.value = true
            ble.sendManualMove(dir, steps, speedHz)
                .onFailure { _lastError.value = it.message ?: "发送失败" }
            _busy.value = false
        }
    }

    /** 停止连续点动。 */
    fun stop() {
        _lastError.value = null
        viewModelScope.launch {
            ble.sendManualStop()
                .onFailure { _lastError.value = it.message ?: "发送失败" }
        }
    }

    /** 读取当前电机微步位置（SETTINGS 0x2A）。 */
    fun refreshPosition() {
        _lastError.value = null
        viewModelScope.launch {
            ble.requestSettings(PumpProtocolSpec.SET_OP_GET_MOTOR_POSITION)
                .onSuccess { b -> if (b.size >= 4) _motorPosition.value = PumpProtocol.leU32(b, 0) }
                .onFailure { _lastError.value = it.message ?: "读取失败" }
        }
    }
}
