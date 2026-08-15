package com.openloop.pump.ble

import android.util.Log
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

private const val TAG = "PumpBleManager"

/**
 * 泵 BLE 管理器 —— 封装扫描 / 连接 / GATT 读写 / Notify / 配对。
 *
 * 固件要求加密链路（MITM + bonding），因此连接后会主动发起配对；
 * 若固件配置了非零 passkey，需在 [pairingPin] 中设置以完成绑定。
 */
@Singleton
class PumpBleManager @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter: BluetoothAdapter? get() = bluetoothManager.adapter
    private val scanner: BluetoothLeScanner? get() = adapter?.bluetoothLeScanner

    private var gatt: BluetoothGatt? = null

    // 特征缓存
    private var bolusChar: BluetoothGattCharacteristic? = null
    private var basalChar: BluetoothGattCharacteristic? = null
    private var tbrChar: BluetoothGattCharacteristic? = null
    private var statusChar: BluetoothGattCharacteristic? = null
    private var iobChar: BluetoothGattCharacteristic? = null
    private var reservoirChar: BluetoothGattCharacteristic? = null
    private var cgmChar: BluetoothGattCharacteristic? = null
    private var controlChar: BluetoothGattCharacteristic? = null
    private var settingsChar: BluetoothGattCharacteristic? = null
    private var keyChar: BluetoothGattCharacteristic? = null
    private var screenChar: BluetoothGattCharacteristic? = null

    /** 配对 PIN（固件 passkey）。null = Just Works。 */
    var pairingPin: String? = null

    // ---- 对外状态流 ----
    private val _connectionState = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _pumpStatus = MutableStateFlow<PumpProtocol.PumpStatus?>(null)
    val pumpStatus: StateFlow<PumpProtocol.PumpStatus?> = _pumpStatus.asStateFlow()

    private val _iob = MutableStateFlow(0.0)
    val iob: StateFlow<Double> = _iob.asStateFlow()

    /** 连接后自动校时所用的协程作用域（连接回调不在协程内，需自行 launch）。 */
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _reservoir = MutableStateFlow(0)
    val reservoir: StateFlow<Int> = _reservoir.asStateFlow()

    /** 实时状态（SCREEN 通道 20 字节二进制通知，供原生虚拟屏重画 + 派生上面各流）。 */
    private val _pumpLiveState = MutableStateFlow<PumpProtocol.PumpLiveState?>(null)
    val pumpLiveState: StateFlow<PumpProtocol.PumpLiveState?> = _pumpLiveState.asStateFlow()

    private val _pumpNav = MutableStateFlow<PumpProtocol.PumpNav?>(null)
    val pumpNav: StateFlow<PumpProtocol.PumpNav?> = _pumpNav.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    // ---- 异步操作信号 ----
    private var connectDeferred: CompletableDeferred<Boolean>? = null
    private var discoverDeferred: CompletableDeferred<Boolean>? = null
    private var writeDeferred: CompletableDeferred<Boolean>? = null
    private var readDeferred: CompletableDeferred<ByteArray?>? = null
    private val bondListeners = mutableListOf<(Boolean) -> Unit>()

    /**
     * GATT 请求串行化锁。所有写/读特征操作必须串行（蓝牙单连接本就串行），
     * 否则并发写（如 CGM 推送与「应用全部到泵」连续 24 次写入）会互相覆盖
     * writeDeferred/readDeferred，导致某次 await 永不完成、调用方永久挂起。
     */
    private val gattMutex = Mutex()

    /** 单次 GATT 写/读等待上限：超出即判失败，杜绝永久挂起（按钮卡「写入中」的根因）。 */
    private val GATT_IO_TIMEOUT_MS = 8000L

    /**
     * 是否为主动断开。true=用户/服务显式断开（需 close 释放 GATT）；
     * false=链路意外掉线（AAPS 周期断连或信号丢失），同样关闭本端 GATT，
     * 但底层 ACL 由 Android 在 AAPS 仍持有时保留，不拆。
     */
    private var intentionalDisconnect = false

    /** 首次连接成功后缓存的泵 MAC，供后续按需重连（CGM 推送时若链路空闲可重新连上）。 */
    private var targetAddress: String? = null

    /** CGM 推送后的空闲释放计时器：到点且仍空闲则主动断开，把链路让给 AAPS。 */
    private var releaseJob: Job? = null

    /**
     * 推送完 CGM 后保持连接的时长（ms）。超时即主动断开释放链路，
     * 让 AAPS（闭环主控制方）在下个 5 分钟周期独占泵。AAPS 优先级高于伴生 App。
     * 设 5s：足够覆盖写 CHAR_CGM 的 ACK/notify，又不长期霸占 ACL。
     */
    private val RELEASE_DELAY_MS = 5000L

    // ============================================================
    // 扫描
    // ============================================================

    @SuppressLint("MissingPermission")
    fun startScan() {
        val sc = scanner ?: run {
            _connectionState.value = ConnectionState.Error("蓝牙不可用或未开启")
            return
        }
        if (_connectionState.value == ConnectionState.Connecting) return
        _connectionState.value = ConnectionState.Scanning
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            // 不按设备名过滤：ScanFilter.setDeviceName 在部分 Android 10 / EMUI 上按名匹配失败，
            // 导致 onScanResult 永远不回调。改为扫描全部设备，在 onScanResult 中手动比对设备名。
            sc.startScan(emptyList(), settings, scanCallback)
        } catch (e: SecurityException) {
            _connectionState.value = ConnectionState.Error("缺少蓝牙扫描权限")
        }
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        runCatching { scanner?.stopScan(scanCallback) }
        if (_connectionState.value == ConnectionState.Scanning) {
            _connectionState.value = ConnectionState.Disconnected
        }
    }

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val r = result ?: return
            val device = r.device ?: return
            // 优先用 scanRecord 中的设备名：部分 ROM 下 device.name 缓存为空导致漏匹配
            val name = r.scanRecord?.deviceName ?: device.name
            if (name != SCAN_DEVICE_NAME) return
            stopScan()
            connect(device)
        }

        override fun onScanFailed(errorCode: Int) {
            _connectionState.value = ConnectionState.Error("扫描失败 code=$errorCode")
        }
    }

    // ============================================================
    // 连接
    // ============================================================

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        // 必须 autoConnect=false（直连），与 AAPS 的 connectGatt 模式一致——
        // Android 不允许第二个 direct 连接复用 autoConnect=true 建立的 ACL，
        // 若此处用 autoConnect=true，AAPS 的直连会永远拿不到 CONNECTED 而超时。
        // 伴生 App 不常驻：连接仅用于推送 CGM，推完由 releaseJob 主动释放（见 sendCgm）。
        if (_connectionState.value is ConnectionState.Connecting
            || _connectionState.value is ConnectionState.Connected) {
            return
        }
        targetAddress = device.address
        _connectionState.value = ConnectionState.Connecting
        connectDeferred = CompletableDeferred()
        intentionalDisconnect = false
        try {
            gatt = device.connectGatt(
                context, false, gattCallback, BluetoothDevice.TRANSPORT_LE
            )
        } catch (e: SecurityException) {
            _connectionState.value = ConnectionState.Error("连接被拒绝：${e.message}")
            connectDeferred?.complete(false)
        }
    }

    /** 通过 MAC 地址连接。 */
    @SuppressLint("MissingPermission")
    suspend fun connect(address: String): Boolean = withContext(Dispatchers.IO) {
        val dev = adapter?.getRemoteDevice(address) ?: run {
            _connectionState.value = ConnectionState.Error("无效地址")
            return@withContext false
        }
        connect(dev)
        val connected = withTimeoutOrNull(20000) { connectDeferred?.await() } ?: false
        if (!connected) {
            // 首连超时：autoConnect=true 仍在后台持续重试，仅更新 UI 状态，不拆 gatt。
            if (_connectionState.value == ConnectionState.Connecting) {
                _connectionState.value = ConnectionState.Disconnected
            }
            return@withContext false
        }

        // 发起配对以建立加密链路
        if (dev.bondState != BluetoothDevice.BOND_BONDED) {
            registerPairingReceiver()
            dev.createBond()
        }

        // 发现服务
        discoverDeferred = CompletableDeferred()
        val ok = runCatching { gatt?.discoverServices() == true }.getOrDefault(false)
        if (!ok) return@withContext false
        discoverDeferred?.await() ?: false
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        intentionalDisconnect = true
        cancelIdleRelease()
        val g = gatt
        gatt = null
        if (g != null) {
            // 只发起断开；真正的 g.close() 交给 onConnectionStateChange 的 DISCONNECTED 回调执行。
            // 切勿 disconnect() + close() 同步连调——EMUI 上该竞态会把底层 ACL 拆坏，
            // 残留"僵尸链路"常驻（实测 08-13 17:33 起的僵尸 ACL 饿死 AAPS 加密握手近 15h，
            // 且杀进程都清不掉，只能重置手机蓝牙）。
            runCatching { g.disconnect() }
            // 兜底：1s 内若 DISCONNECTED 回调未触发 close，则强制 close，杜绝僵尸 ACL。
            scope.launch {
                delay(1000)
                runCatching { g.close() }
            }
        }
        resetCharacteristics()
        _connectionState.value = ConnectionState.Disconnected
        _pumpStatus.value = null
        _pumpLiveState.value = null
        _iob.value = 0.0
        _reservoir.value = 0
    }

    /** 调度空闲释放：RELEASE_DELAY_MS 后若仍空闲则主动断开，把链路让给 AAPS。 */
    private fun scheduleIdleRelease() {
        cancelIdleRelease()
        releaseJob = scope.launch {
            delay(RELEASE_DELAY_MS)
            // 无条件释放：只要本端仍持有 gatt 就断开。若仅靠 `state is Connected` 判断，
            // 服务发现失败等情况下 state 非 Connected 会漏掉释放，把 ACL 永久占住而饿死 AAPS。
            if (gatt != null) {
                Log.i(TAG, "idle release (yield to AAPS) -> disconnect")
                disconnect()
            }
        }
    }

    /** 取消空闲释放计时（连接活跃/有新推送/显式断开时调用）。 */
    private fun cancelIdleRelease() {
        releaseJob?.cancel()
        releaseJob = null
    }

    private fun resetCharacteristics() {
        bolusChar = null
        basalChar = null
        tbrChar = null
        statusChar = null
        iobChar = null
        reservoirChar = null
        cgmChar = null
        controlChar = null
        settingsChar = null
        keyChar = null
        screenChar = null
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    connectDeferred?.complete(true)
                    connectDeferred = null
                    targetAddress = g.device.address
                    Log.i(TAG, "onConnectionStateChange CONNECTED")
                    // 连接成功后统一发现服务（onScanResult 路径此前不调 discoverServices 会卡 Connecting）
                    runCatching { g.discoverServices() }
                    // 不常驻：连接空闲一段时间后由 releaseJob 主动释放，把链路让给 AAPS。
                    scheduleIdleRelease()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    connectDeferred?.complete(false)
                    connectDeferred = null
                    discoverDeferred?.complete(false)
                    discoverDeferred = null
                    cancelIdleRelease()
                    Log.w(TAG, "onConnectionStateChange DISCONNECTED (intentionalDisconnect=$intentionalDisconnect, status=$status)")
                    // 直连模式下，无论主动/被动断开都关闭本端 GATT（不影响 AAPS 仍持有的 ACL）。
                    runCatching { g.close() }
                    gatt = null
                    resetCharacteristics()
                    _connectionState.value = ConnectionState.Disconnected
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                discoverDeferred?.complete(false)
                discoverDeferred = null
                _connectionState.value = ConnectionState.Error("服务发现失败")
                return
            }
            val svc = g.getService(PumpUuids.SERVICE)
            if (svc == null) {
                discoverDeferred?.complete(false)
                discoverDeferred = null
                _connectionState.value = ConnectionState.Error("未找到 Pump 服务")
                return
            }
            bolusChar = svc.getCharacteristic(PumpUuids.CHAR_BOLUS)
            basalChar = svc.getCharacteristic(PumpUuids.CHAR_BASAL)
            tbrChar = svc.getCharacteristic(PumpUuids.CHAR_TBR)
            statusChar = svc.getCharacteristic(PumpUuids.CHAR_STATUS)
            iobChar = svc.getCharacteristic(PumpUuids.CHAR_IOB)
            reservoirChar = svc.getCharacteristic(PumpUuids.CHAR_RESERVOIR)
            cgmChar = svc.getCharacteristic(PumpUuids.CHAR_CGM)
            controlChar = svc.getCharacteristic(PumpUuids.CHAR_CONTROL)
            settingsChar = svc.getCharacteristic(PumpUuids.CHAR_SETTINGS)
            keyChar = svc.getCharacteristic(PumpUuids.CHAR_KEY)
            screenChar = svc.getCharacteristic(PumpUuids.CHAR_SCREEN)

            enableNotifications(g)
            _connectionState.value = ConnectionState.Connected
            discoverDeferred?.complete(true)
            discoverDeferred = null
            // 一连上就把手机时间写给泵, 自举泵 RTC（AAPS 在 |时差|>1.5h 时拒绝校时,
            // 故必须由本 App 兜底, 否则 AAPS 会卡"大时间差"且无大剂量按钮）。
            scope.launch { runCatching { syncTimeFromPhoneInternal() } }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, char: BluetoothGattCharacteristic, status: Int
        ) {
            val ok = status == BluetoothGatt.GATT_SUCCESS
            writeDeferred?.complete(ok)
            writeDeferred = null
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt, char: BluetoothGattCharacteristic, status: Int
        ) {
            val data = if (status == BluetoothGatt.GATT_SUCCESS) char.value else null
            readDeferred?.complete(data)
            readDeferred = null
        }

        @Deprecated("Deprecated in Java")
        @SuppressLint("MissingPermission")
        override fun onCharacteristicChanged(
            g: BluetoothGatt, char: BluetoothGattCharacteristic
        ) {
            handleNotification(char.uuid, char.value)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt, desc: BluetoothGattDescriptor, status: Int
        ) {
            // Notify 已启用，无需额外动作
        }
    }

    // ============================================================
    // Notify
    // ============================================================

    @SuppressLint("MissingPermission")
    private fun enableNotifications(g: BluetoothGatt) {
        // 仅订阅 SCREEN 通道：固件每 1Hz 推送 20 字节二进制实时状态（单包 ≤ MTU 载荷上限，无需分片）。
        val char = screenChar ?: return
        g.setCharacteristicNotification(char, true)
        val cccd = char.getDescriptor(PumpUuids.CCCD) ?: return
        val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, value)
        } else {
            @Suppress("DEPRECATION")
            cccd.value = value
            g.writeDescriptor(cccd)
        }
    }

    @SuppressLint("MissingPermission")
    private fun handleNotification(uuid: java.util.UUID?, value: ByteArray?) {
        value ?: return
        val u = uuid ?: return
        when (u) {
            // SCREEN：20 字节紧凑二进制实时状态。解码后供原生虚拟屏重画，
            // 并派生出 pumpStatus / iob / reservoir 三个旧流（Dashboard/Bolus/LoopWorker 复用）。
            PumpUuids.CHAR_SCREEN -> {
                // 同一 SCREEN 通道承载两类通知：0xA1=实时状态(20B)，0xB1=导航状态(13B)
                when (value.firstOrNull()?.toInt()?.and(0xFF)) {
                    0xA1 -> {
                        val live = PumpProtocol.parsePumpState(value) ?: return
                        _pumpLiveState.value = live
                        _iob.value = live.iobUnits
                        _reservoir.value = live.reservoirUnits.toInt()
                        _pumpStatus.value = PumpProtocol.PumpStatus(
                            stateCode = live.mode,
                            alarmCode = live.alarmCode,
                            deliveredUnits = 0.0,
                            reservoirUnits = live.reservoirUnits.toInt(),
                            batteryMv = 0,
                            batteryPct = live.batteryPct,
                            iobUnits = live.iobUnits,
                            glucoseMgdl = live.glucoseMgdl,
                            trend = live.glucoseTrend,
                            loopMode = live.loopMode,
                            tbrPercent = live.tbrPercent
                        )
                    }
                    0xB1 -> {
                        _pumpNav.value = PumpProtocol.parsePumpNav(value)
                    }
                }
            }
        }
    }

    // ============================================================
    // 命令 API (suspend)
    // ============================================================

    /** 发送大剂量。 */
    suspend fun sendBolus(
        units: Double, durationMin: Int = 0, isExtended: Boolean = false
    ): Result<Unit> = withContext(Dispatchers.IO) {
        val char = bolusChar ?: return@withContext fail("未连接或未发现 Bolus 特征")
        val payload = runCatching { PumpProtocol.buildBolus(units, durationMin, isExtended) }
            .getOrElse { return@withContext Result.failure(it) }
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /** 设置当前基础率 (U/h)。固件伴生通道只支持设置实时运行速率。 */
    suspend fun setBasalRate(rateUh: Double): Result<Unit> = withContext(Dispatchers.IO) {
        val char = basalChar ?: return@withContext fail("未连接或未发现 Basal 特征")
        val payload = runCatching { PumpProtocol.buildBasalRate(rateUh) }
            .getOrElse { return@withContext Result.failure(it) }
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /**
     * 兼容旧调用：伴生通道仅支持设置当前基础率，slot 参数忽略。
     * 24 段基础率档案的编辑在泵端完成，App 侧仅做本地规划展示。
     */
    suspend fun setBasalSlot(slot: Int, rateUh: Double): Result<Unit> = setBasalRate(rateUh)

    /** 发送临时基础率 (TBR)。 */
    suspend fun setTemporaryBasal(rateUh: Double, durationMin: Int): Result<Unit> =
        withContext(Dispatchers.IO) {
            val char = tbrChar ?: return@withContext fail("未连接或未发现 TBR 特征")
            val payload = runCatching { PumpProtocol.buildTbr(rateUh, durationMin) }
                .getOrElse { return@withContext Result.failure(it) }
            writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
        }

    /**
     * 控制命令：环模式(0/1/2) 或 远程动作 (0x10 排气 / 0x11 清报警 / 0x12 退回 /
     * 0x13 标定推出 / 0x14 标定应用)。param 为可选浮点参数（排气 ml / 标定量或系数），
     * null 表示无参单字节指令。伴生 App「设置」页以此直接下发维护/标定指令，不模拟按键。
     */
    suspend fun sendControl(modeOrCmd: Int, param: Float? = null): Result<Unit> = withContext(Dispatchers.IO) {
        val char = controlChar ?: return@withContext fail("未连接或未发现 Control 特征")
        val payload = PumpProtocol.buildControl(modeOrCmd, param)
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /**
     * 回传血糖 (mg/dl) 与趋势五档码(-2..2) 供泵显示。
     *
     * 链路空闲时按需连接（与 AAPS 共用同一条 ACL，直连模式一致，AAPS 优先级更高）；
     * 推送成功后调度空闲释放，把链路让回 AAPS，避免伴生常驻饿死 AAPS 的直连。
     */
    /**
     * 确保 BLE 链路已连且可写；未连则按配对地址重连（带重试）。
     * 供「应用全部到泵」等批量写操作在开始前对连接做自检，避免出现
     * 24 段静默全失败（GATT 未就绪）才让用户发现没连上。
     * 返回是否就绪。
     */
    suspend fun ensureConnected(): Boolean {
        if (_connectionState.value is ConnectionState.Connected && gatt != null) return true
        val addr = targetAddress ?: run {
            Log.w(TAG, "ensureConnected: 无配对地址")
            return false
        }
        Log.i(TAG, "ensureConnected: 链路空闲，按需连接 $addr")
        repeat(2) { i ->
            if (connect(addr)) return true
            Log.w(TAG, "ensureConnected: 连接失败 #$i，重试")
            delay(500)
        }
        return false
    }

    /** 提交内存配置到 NVS（SET 0x19）：写完 24 段后调用一次，避免每段都落盘引发 NVS 压力。 */
    suspend fun setCommitConfig(): Result<Unit> = withContext(Dispatchers.IO) {
        val r = requestSettings(PumpProtocolSpec.SET_OP_COMMIT_CONFIG, byteArrayOf())
        if (r.isSuccess) Result.success(Unit)
        else Result.failure(r.exceptionOrNull() ?: IllegalStateException("提交配置到 NVS 失败"))
    }

    suspend fun sendCgm(mgdl: Int, trend: Int): Result<Unit> = withContext(Dispatchers.IO) {
        if (_connectionState.value !is ConnectionState.Connected || gatt == null) {
            val addr = targetAddress
            if (addr == null) {
                Log.w(TAG, "sendCgm: 无配对地址，跳过")
                return@withContext Result.failure(Exception("未配对，无法连接泵"))
            }
            Log.i(TAG, "sendCgm: 链路空闲，按需连接 $addr")
            val ok = connect(addr)
            if (!ok) {
                Log.w(TAG, "sendCgm: 连接泵超时/失败")
                return@withContext Result.failure(Exception("连接泵超时/失败"))
            }
        }
        // 重连后 gatt 可能尚未 ready（CONNECTED 回调与栈实际可写状态存在竞态，
        // 实测 CONNECTED 后 ~40ms 内 writeCharacteristic 立即返回 false），写失败退避重试。
        val payload = PumpProtocol.buildCgm(mgdl, trend)
        Log.i(TAG, "sendCgm mgdl=$mgdl trend=$trend -> CHAR_CGM")
        var attempt = 0
        var r: Result<Unit> = Result.failure(Exception("未初始化"))
        do {
            if (attempt > 0) {
                Log.w(TAG, "sendCgm retry #$attempt (prev: ${r.exceptionOrNull()?.message})")
                delay(500)
                // 重试期间链路若被拆（或特征被清空），重新连接
                if (_connectionState.value !is ConnectionState.Connected || gatt == null || cgmChar == null) {
                    val addr = targetAddress ?: return@withContext Result.failure(Exception("未配对，无法连接泵"))
                    if (!connect(addr)) return@withContext Result.failure(Exception("重连失败"))
                }
            }
            val c = cgmChar ?: return@withContext Result.failure(Exception("未连接或未发现 CGM 特征"))
            r = writeWithAck(gatt ?: return@withContext Result.failure(Exception("GATT 未就绪")), c, payload)
            attempt++
        } while (r.isFailure && attempt <= 3)
        if (r.isSuccess) {
            Log.i(TAG, "sendCgm result=OK (attempt $attempt)")
            scheduleIdleRelease()   // 推完延迟释放，让路 AAPS
        } else {
            Log.w(TAG, "sendCgm result=FAIL after $attempt attempts: ${r.exceptionOrNull()?.message}")
        }
        r
    }

    /**
     * 连接后自动把手机当前时间写给泵 RTC（小端 u32 Unix 秒）。
     * 与 SettingsViewModel.syncTimeFromPhone 等价, 但放在 Manager 内便于连接回调直接 launch。
     */
    private suspend fun syncTimeFromPhoneInternal() {
        val now = System.currentTimeMillis() / 1000L
        val payload = byteArrayOf(
            (now and 0xFF).toByte(),
            ((now shr 8) and 0xFF).toByte(),
            ((now shr 16) and 0xFF).toByte(),
            ((now shr 24) and 0xFF).toByte()
        )
        requestSettings(PumpProtocolSpec.SET_OP_SET_TIME, payload)
    }

    /**
     * 设置通道请求：写 [op][payload][crc] 后读取响应，整体在 [gattMutex] 内串行。
     * GET 类 op 返回结果字节；SET 类 op 返回 1 字节 ack (0=OK / 1=ERR)。
     */
    suspend fun requestSettings(op: Int, payload: ByteArray = byteArrayOf()): Result<ByteArray> =
        withContext(Dispatchers.IO) {
            gattMutex.withLock {
                val g = gatt ?: return@withLock fail("GATT 未就绪")
                val char = settingsChar ?: return@withLock fail("未连接或未发现 Settings 特征")
                val req = PumpProtocol.buildSettings(op, payload)
                if (!rawWrite(g, char, req)) return@withLock Result.failure(IOException2("设置写入未确认(超时)"))
                readDeferred = CompletableDeferred()
                val started = runCatching {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        g.readCharacteristic(char)
                        true
                    } else {
                        @Suppress("DEPRECATION")
                        g.readCharacteristic(char)
                    }
                }.getOrDefault(false)
                if (!started) {
                    readDeferred = null
                    return@withLock fail("发起设置读取失败")
                }
                val data = withTimeoutOrNull(GATT_IO_TIMEOUT_MS) { readDeferred!!.await() }
                readDeferred = null
                if (data != null) Result.success(data) else Result.failure(IllegalStateException("设置读取无响应(超时)"))
            }
        }

    /** 读取当前激活基础率方案索引（SET 0x10 → profile u8）。 */
    suspend fun getActiveBasalProfile(): Result<Int> = withContext(Dispatchers.IO) {
        val r = requestSettings(PumpProtocolSpec.SET_OP_GET_ACTIVE_PROFILE, byteArrayOf())
        if (r.isFailure) return@withContext Result.failure(r.exceptionOrNull()!!)
        val b = r.getOrNull() ?: return@withContext Result.failure(IllegalStateException("空响应"))
        Result.success((b.firstOrNull()?.toInt() ?: 0) and 0xFF)
    }

    /**
     * 写入基础率方案单槽（SET 0x17）：payload = [profile u8][hour u8][rate f32 LE]。
     * 真正持久化到泵内 NVS 档案（区别于 setBasalRate 只设实时运行速率），
     * 这样「应用全部到泵」才能把 24 段方案写进泵，runBasalTest 才能核对。
     */
    suspend fun setBasalProfileSlot(profile: Int, hour: Int, rateUh: Double): Result<Unit> =
        withContext(Dispatchers.IO) {
            val payload = byteArrayOf(
                (profile and 0xFF).toByte(),
                (hour and 0xFF).toByte()
            ) + PumpProtocol.f32Le(rateUh.toFloat())
            val r = requestSettings(PumpProtocolSpec.SET_OP_SET_PROFILE_SLOT, payload)
            if (r.isFailure) return@withContext Result.failure(r.exceptionOrNull()!!)
            val ack = r.getOrNull()?.firstOrNull() ?: 0x01.toByte()
            if (ack == 0x00.toByte()) Result.success(Unit)
            else Result.failure(IllegalStateException("泵拒绝写槽(profile=$profile,hour=$hour)"))
        }

    /** 远程按键按下（event 见 PumpProtocolSpec.KEY_*）。等同物理按键，泵屏同步。 */
    suspend fun sendKey(event: Int): Result<Unit> = withContext(Dispatchers.IO) {
        val char = keyChar ?: return@withContext fail("未连接或未发现 Key 特征")
        val payload = PumpProtocol.buildKey(event)
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /** 远程松手：停止"按住自动重复"。 */
    suspend fun releaseKey(): Result<Unit> = sendKey(PumpProtocolSpec.KEY_RELEASE.toInt())

    /**
     * 手动电机控制：前进/后退定量步数，或 steps=0 连续点动直到 STOP。
     * 专用于电机测试（不记账）。dir 见 PumpProtocolSpec.MANUAL_DIR_*。
     */
    suspend fun sendManualMove(dir: Int, steps: Long, speedHz: Int): Result<Unit> = withContext(Dispatchers.IO) {
        val char = controlChar ?: return@withContext fail("未连接或未发现 Control 特征")
        val payload = PumpProtocol.buildManualMove(dir, steps, speedHz)
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /** 手动停止：终止正在进行的连续点动。 */
    suspend fun sendManualStop(): Result<Unit> = withContext(Dispatchers.IO) {
        val char = controlChar ?: return@withContext fail("未连接或未发现 Control 特征")
        val payload = PumpProtocol.buildManualStop()
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /** 主动刷新状态：实时状态由 SCREEN 通道 1Hz 推送，无需主动读取，此处为空操作以保持接口兼容。 */
    suspend fun refreshStatus(): Result<Unit> = Result.success(Unit)

    // ============================================================
    // 底层读写（兼容新旧 API）
    // ============================================================

    /**
     * 底层单次写（不持锁）：置 deferred → 发起写 → 带超时等待 onCharacteristicWrite 完成。
     * 调用方负责用 [gattMutex] 串行化，避免并发写覆盖 deferred。
     */
    private suspend fun rawWrite(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray
    ): Boolean {
        writeDeferred = CompletableDeferred()
        val started = writeCharacteristic(g, char, value)
        if (!started) {
            writeDeferred = null
            return false
        }
        val ok = withTimeoutOrNull(GATT_IO_TIMEOUT_MS) { writeDeferred!!.await() } ?: false
        writeDeferred = null
        return ok
    }

    /** 串行化写（with-response）：返回结果。带超时，永不永久挂起。 */
    private suspend fun writeWithAck(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray
    ): Result<Unit> = gattMutex.withLock {
        if (rawWrite(g, char, value)) Result.success(Unit)
        else Result.failure(IOException2("写入未确认(超时)"))
    }

    @SuppressLint("MissingPermission", "DeprecatedBlockingMethod")
    private fun writeCharacteristic(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray
    ): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(char, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                android.bluetooth.BluetoothStatusCodes.SUCCESS
        } else {
            char.value = value
            g.writeCharacteristic(char)
        }
    }

    // ============================================================
    // 配对处理
    // ============================================================

    private var pairingReceiver: BroadcastReceiver? = null

    @SuppressLint("MissingPermission")
    private fun registerPairingReceiver() {
        if (pairingReceiver != null) return
        pairingReceiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                when (intent?.action) {
                    BluetoothDevice.ACTION_PAIRING_REQUEST -> {
                        val device =
                            intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                        // 若配置了 PIN，自动填充以避免手动输入
                        pairingPin?.let { pin ->
                            device?.setPin(pin.toByteArray())
                            abortBroadcast()
                        }
                    }
                    BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                        val device =
                            intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                        val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
                        if (state == BluetoothDevice.BOND_BONDED) {
                            _connectionState.value = ConnectionState.Bonded
                            bondListeners.forEach { it(true) }
                        } else if (state == BluetoothDevice.BOND_NONE) {
                            bondListeners.forEach { it(false) }
                        }
                    }
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_PAIRING_REQUEST)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        }
        ContextCompat.registerReceiver(
            context, pairingReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED
        )
    }

    fun onBonded(callback: (Boolean) -> Unit) {
        bondListeners.add(callback)
    }

    // ============================================================
    // 工具
    // ============================================================

    private fun <T> fail(msg: String): Result<T> {
        _lastError.value = msg
        return Result.failure(IllegalStateException(msg))
    }

    companion object {
        /** 项目仅保留 AAPS(Dana-i 伪装) 变体固件，泵广播名为 DANAI_DEVICE_NAME。 */
        const val SCAN_DEVICE_NAME = "DAN12345AB"
    }
}

/** 内部 IO 异常别名，避免与标准库同名混淆。 */
private class IOException2(message: String) : Exception(message)
