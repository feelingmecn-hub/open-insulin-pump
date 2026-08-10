package com.openloop.pump.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.openloop.pump.data.repository.PreferencesRepository
import com.openloop.pump.ble.ConnectionState
import com.openloop.pump.ble.PumpBleManager
import com.openloop.pump.ble.PumpProtocol
import com.openloop.pump.ble.PumpProtocolSpec
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

/** 基础率方案数据（名称 + 24 段速率 U/h）。 */
data class ProfileData(val name: String, val rates: FloatArray) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as ProfileData
        if (name != other.name) return false
        return rates.contentEquals(other.rates)
    }
    override fun hashCode(): Int {
        var result = name.hashCode()
        result = 31 * result + rates.contentHashCode()
        return result
    }
}

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val prefs: PreferencesRepository,
    private val ble: PumpBleManager
) : ViewModel() {

    // ----------------------------------------------------------
    // App 本地闭环偏好（仅 App 自身编排用，不直接下发泵）
    // ----------------------------------------------------------
    val closedLoop = prefs.closedLoopEnabled
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)
    val isf = prefs.isf.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 50.0)
    val target = prefs.targetGlucose
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 110)
    val maxIob = prefs.maxIob.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 4.0)
    val maxBasal = prefs.maxBasal.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 5.0)
    val carbRatio = prefs.carbRatio
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 10.0)
    val nsUrl = prefs.nightscoutUrl
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), "")
    val appBlePasskey = prefs.blePasskey
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), "")

    fun setLoop(enabled: Boolean) = io { prefs.setLoopParam(enabled = enabled) }
    fun setIsp2(v: Double) = io { prefs.setLoopParam(isf = v) }
    fun setTarget(v: Int) = io { prefs.setLoopParam(target = v) }
    fun setMaxIob(v: Double) = io { prefs.setLoopParam(maxIob = v) }
    fun setMaxBasal(v: Double) = io { prefs.setLoopParam(maxBasal = v) }
    fun setCarbRatio(v: Double) = io { prefs.setLoopParam(carbRatio = v) }
    fun setNightscout(url: String, secret: String) = io { prefs.setNightscout(url, secret) }
    fun setPasskey(pin: String) = io { prefs.setBlePasskey(pin) }

    // ----------------------------------------------------------
    // 泵连接状态
    // ----------------------------------------------------------
    val connectionState: StateFlow<ConnectionState> = ble.connectionState

    // ----------------------------------------------------------
    // 泵系统设置（SETTINGS 通道直读直写，不模拟按键）
    // ----------------------------------------------------------
    private val _brightness = MutableStateFlow(50)
    val brightness: StateFlow<Int> = _brightness.asStateFlow()

    private val _activeProfile = MutableStateFlow(0)
    val activeProfile: StateFlow<Int> = _activeProfile.asStateFlow()

    private val _keypadSound = MutableStateFlow(true)
    val keypadSound: StateFlow<Boolean> = _keypadSound.asStateFlow()

    private val _vibrate = MutableStateFlow(false)
    val vibrate: StateFlow<Boolean> = _vibrate.asStateFlow()

    private val _pumpPasskey = MutableStateFlow("")
    val pumpPasskey: StateFlow<String> = _pumpPasskey.asStateFlow()

    private val _loopMode = MutableStateFlow(0)
    val loopMode: StateFlow<Int> = _loopMode.asStateFlow()

    // 大剂量 / 安全限制
    private val _maxBolusSingle = MutableStateFlow(25f)
    val maxBolusSingle: StateFlow<Float> = _maxBolusSingle.asStateFlow()
    private val _maxBolusPerHour = MutableStateFlow(25f)
    val maxBolusPerHour: StateFlow<Float> = _maxBolusPerHour.asStateFlow()
    private val _maxBasalPerHour = MutableStateFlow(5f)
    val maxBasalPerHour: StateFlow<Float> = _maxBasalPerHour.asStateFlow()

    // 安全参数
    private val _occlusion = MutableStateFlow(700)
    val occlusion: StateFlow<Int> = _occlusion.asStateFlow()
    private val _watchdog = MutableStateFlow(30)
    val watchdog: StateFlow<Int> = _watchdog.asStateFlow()
    private val _overTemp = MutableStateFlow(60f)
    val overTemp: StateFlow<Float> = _overTemp.asStateFlow()

    // 剂量标定系数
    private val _calibration = MutableStateFlow(1.0f)
    val calibration: StateFlow<Float> = _calibration.asStateFlow()

    // 省电: 空闲自动熄屏
    private val _autoDim = MutableStateFlow(true)
    val autoDim: StateFlow<Boolean> = _autoDim.asStateFlow()
    private val _autoDimTimeout = MutableStateFlow(30)
    val autoDimTimeout: StateFlow<Int> = _autoDimTimeout.asStateFlow()

    init {
        // 镜像泵当前环模式（来自 SCREEN 通道 1Hz 推送）
        viewModelScope.launch {
            ble.pumpLiveState.collect { s -> s?.let { _loopMode.value = it.loopMode } }
        }
    }

    /** 连接成功后读取泵全部系统设置，刷新本地显示。 */
    fun loadPumpConfig() {
        viewModelScope.launch { loadAllHubSettings() }
    }

    private suspend fun loadAllHubSettings() {
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_BRIGHTNESS).onSuccess { b ->
            if (b.isNotEmpty()) _brightness.value = b[0].toInt() and 0xFF
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_ACTIVE_PROFILE).onSuccess { b ->
            if (b.isNotEmpty()) _activeProfile.value = b[0].toInt() and 0xFF
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_KEYPAD).onSuccess { b ->
            if (b.isNotEmpty()) _keypadSound.value = (b[0].toInt() and 0xFF) != 0
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_VIBRATE).onSuccess { b ->
            if (b.isNotEmpty()) _vibrate.value = (b[0].toInt() and 0xFF) != 0
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_PASSKEY).onSuccess { b ->
            if (b.size >= 4) _pumpPasskey.value = PumpProtocol.leU32(b, 0).toString()
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_LIMITS).onSuccess { b ->
            if (b.size >= 12) {
                _maxBolusSingle.value = PumpProtocol.leF32(b, 0)
                _maxBolusPerHour.value = PumpProtocol.leF32(b, 4)
                _maxBasalPerHour.value = PumpProtocol.leF32(b, 8)
            }
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_SAFETY).onSuccess { b ->
            if (b.size >= 7) {
                _occlusion.value = PumpProtocol.leU16(b, 0)
                _watchdog.value = b[2].toInt() and 0xFF
                _overTemp.value = PumpProtocol.leF32(b, 3)
            }
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_CALIBRATION).onSuccess { b ->
            if (b.size >= 4) _calibration.value = PumpProtocol.leF32(b, 0)
        }
        ble.requestSettings(PumpProtocolSpec.SET_OP_GET_AUTO_DIM).onSuccess { b ->
            if (b.size >= 3) {
                _autoDim.value = (b[0].toInt() and 0xFF) != 0
                _autoDimTimeout.value = PumpProtocol.leU16(b, 1)
            }
        }
    }

    fun setBrightness(pct: Int) {
        val v = pct.coerceIn(0, 100)
        _brightness.value = v
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_BRIGHTNESS, byteArrayOf(v.toByte())) }
    }

    fun setProfile(p: Int) {
        val v = p.coerceIn(0, 3)
        _activeProfile.value = v
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_ACTIVE_PROFILE, byteArrayOf(v.toByte())) }
    }

    fun setKeypadSound(on: Boolean) {
        _keypadSound.value = on
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_KEYPAD, byteArrayOf((if (on) 1 else 0).toByte())) }
    }

    fun setVibrate(on: Boolean) {
        _vibrate.value = on
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_VIBRATE, byteArrayOf((if (on) 1 else 0).toByte())) }
    }

    fun setPumpPasskey(code: String) {
        val digits = code.filter { it.isDigit() }
        val v = digits.toLongOrNull()?.coerceIn(0, 0xFFFFFFFFL) ?: 0L
        _pumpPasskey.value = v.toString()
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_PASSKEY, u32bytes(v)) }
    }

    fun setLoopMode(m: Int) {
        val v = m.coerceIn(0, 2)
        _loopMode.value = v
        io { ble.sendControl(v) }
    }

    fun setLimit(which: Int, value: Float) {
        when (which) {
            PumpProtocolSpec.LIMIT_SINGLE -> _maxBolusSingle.value = value
            PumpProtocolSpec.LIMIT_PER_HOUR -> _maxBolusPerHour.value = value
            PumpProtocolSpec.LIMIT_MAX_BASAL -> _maxBasalPerHour.value = value
        }
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_LIMIT, byteArrayOf(which.toByte()) + f32bytes(value)) }
    }

    fun setSafety(which: Int, value: Float) {
        val payload = when (which) {
            PumpProtocolSpec.SAFE_OCCLUSION -> byteArrayOf(which.toByte()) + u16bytes(value.toInt())
            PumpProtocolSpec.SAFE_WATCHDOG -> byteArrayOf(which.toByte(), value.toInt().coerceIn(0, 255).toByte())
            else -> byteArrayOf(which.toByte()) + f32bytes(value)
        }
        when (which) {
            PumpProtocolSpec.SAFE_OCCLUSION -> _occlusion.value = value.toInt()
            PumpProtocolSpec.SAFE_WATCHDOG -> _watchdog.value = value.toInt()
            PumpProtocolSpec.SAFE_OVER_TEMP -> _overTemp.value = value
        }
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_SAFETY, payload) }
    }

    fun setCalibration(factor: Float) {
        val f = factor.coerceAtLeast(0.1f)
        _calibration.value = f
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_CALIBRATION, f32bytes(f)) }
    }

    fun setAutoDim(enabled: Boolean, timeoutSec: Int) {
        val t = timeoutSec.coerceIn(5, 600)
        _autoDim.value = enabled
        _autoDimTimeout.value = t
        io {
            ble.requestSettings(
                PumpProtocolSpec.SET_OP_SET_AUTO_DIM,
                byteArrayOf((if (enabled) 1 else 0).toByte()) + u16bytes(t)
            )
        }
    }

    /** 用手机当前时间同步泵 RTC。 */
    fun syncTimeFromPhone() {
        io {
            val now = System.currentTimeMillis() / 1000L
            ble.requestSettings(PumpProtocolSpec.SET_OP_SET_TIME, u32bytes(now))
        }
    }

    // ----------------------------------------------------------
    // 基础率方案编辑器：加载整套 + 单点写入
    // ----------------------------------------------------------
    suspend fun loadProfile(p: Int): ProfileData? {
        val idx = p.coerceIn(0, 3)
        val name = ble.requestSettings(PumpProtocolSpec.SET_OP_GET_PROFILE_NAME, byteArrayOf(idx.toByte()))
            .mapCatching { bytes ->
                val z = bytes.indexOf(0)
                if (z < 0) String(bytes, Charsets.UTF_8) else String(bytes.copyOf(z), Charsets.UTF_8)
            }.getOrNull() ?: "方案${idx + 1}"
        val rates = FloatArray(24)
        for (h in 0..23) {
            ble.requestSettings(
                PumpProtocolSpec.SET_OP_GET_PROFILE_SLOT,
                byteArrayOf(idx.toByte(), h.toByte())
            ).onSuccess { b ->
                if (b.size >= 4) rates[h] = PumpProtocol.leF32(b, 0)
            }
        }
        return ProfileData(name, rates)
    }

    fun setProfileName(p: Int, name: String) {
        val idx = p.coerceIn(0, 3)
        val nb = name.toByteArray(Charsets.UTF_8).copyOfRange(0, minOf(name.length, 31))
        io { ble.requestSettings(PumpProtocolSpec.SET_OP_SET_PROFILE_NAME, byteArrayOf(idx.toByte()) + nb) }
    }

    fun setProfileSlot(p: Int, hour: Int, rate: Float) {
        val idx = p.coerceIn(0, 3)
        val h = hour.coerceIn(0, 23)
        io {
            ble.requestSettings(
                PumpProtocolSpec.SET_OP_SET_PROFILE_SLOT,
                byteArrayOf(idx.toByte(), h.toByte()) + f32bytes(rate)
            )
        }
    }

    // ----------------------------------------------------------
    // 闭环参数编辑器（逐时 ISF / 碳水比 / 目标血糖）
    // ----------------------------------------------------------
    suspend fun loadClKind(kind: Int): FloatArray? {
        val k = kind.coerceIn(0, 2)
        val arr = FloatArray(24)
        for (h in 0..23) {
            ble.requestSettings(
                PumpProtocolSpec.SET_OP_GET_CL_PARAM,
                byteArrayOf(k.toByte(), h.toByte())
            ).onSuccess { b ->
                if (b.size >= 4) arr[h] = PumpProtocol.leF32(b, 0)
            }
        }
        return arr
    }

    fun setClParam(kind: Int, hour: Int, value: Float) {
        val k = kind.coerceIn(0, 2)
        val h = hour.coerceIn(0, 23)
        io {
            ble.requestSettings(
                PumpProtocolSpec.SET_OP_SET_CL_PARAM,
                byteArrayOf(k.toByte(), h.toByte()) + f32bytes(value)
            )
        }
    }

    // ----------------------------------------------------------
    // 泵维护操作（直接发 CONTROL 指令调 ui_hal_*，不模拟按键）
    // ----------------------------------------------------------
    fun primePump(ml: Float = 1.0f) {
        io { ble.sendControl(PumpProtocolSpec.CTRL_CMD_PRIME, ml.coerceIn(0.1f, 200.0f)) }
    }

    fun rewindPump() {
        io { ble.sendControl(PumpProtocolSpec.CTRL_CMD_REWIND) }
    }

    fun clearAlarm() {
        io { ble.sendControl(PumpProtocolSpec.CTRL_CMD_CLEAR_ALARM) }
    }

    fun calibrateDispense(units: Float = 1.0f) {
        io { ble.sendControl(PumpProtocolSpec.CTRL_CMD_CAL_DISPENSE, units.coerceIn(0.1f, 100.0f)) }
    }

    fun applyCalibration(factor: Float) {
        io { ble.sendControl(PumpProtocolSpec.CTRL_CMD_CAL_APPLY, factor.coerceAtLeast(0.1f)) }
    }

    // ----------------------------------------------------------
    // 字节辅助
    // ----------------------------------------------------------
    private fun f32bytes(v: Float): ByteArray {
        val bits = v.toRawBits()
        return byteArrayOf(
            (bits and 0xFF).toByte(),
            ((bits shr 8) and 0xFF).toByte(),
            ((bits shr 16) and 0xFF).toByte(),
            ((bits shr 24) and 0xFF).toByte()
        )
    }

    private fun u32bytes(v: Long): ByteArray {
        return byteArrayOf(
            (v and 0xFF).toByte(),
            ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(),
            ((v shr 24) and 0xFF).toByte()
        )
    }

    private fun u16bytes(v: Int): ByteArray {
        return byteArrayOf((v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte())
    }

    private fun io(block: suspend () -> Unit) = viewModelScope.launch { block() }
}
