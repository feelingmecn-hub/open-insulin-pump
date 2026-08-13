package com.openloop.pump.ble

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
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.resume

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
     * 是否为主动断开。true=用户/服务显式断开（需 close 释放 GATT）；
     * false=链路意外掉线，此时 autoConnect=true 会让 Android 后台自动重连，
     * 切勿 close()，否则取消 autoConnect 且拆掉与 AAPS 共用的 ACL。
     */
    private var intentionalDisconnect = false

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
        // 同手机上与 AAPS 共存：用 autoConnect=true 建立后台常驻连接，
        // 不与 AAPS 的直连(connectGatt autoConnect=false)抢同一条 ACL——
        // 两个 App 都直连会在连接建立/服务发现/绑定的瞬间互相竞态，间歇性互相挤掉。
        if (_connectionState.value is ConnectionState.Connecting
            || _connectionState.value is ConnectionState.Connected) {
            return
        }
        _connectionState.value = ConnectionState.Connecting
        connectDeferred = CompletableDeferred()
        intentionalDisconnect = false
        try {
            gatt = device.connectGatt(
                context, true, gattCallback, BluetoothDevice.TRANSPORT_LE
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
        runCatching {
            gatt?.disconnect()
            gatt?.close()
        }
        gatt = null
        resetCharacteristics()
        _connectionState.value = ConnectionState.Disconnected
        _pumpStatus.value = null
        _pumpLiveState.value = null
        _iob.value = 0.0
        _reservoir.value = 0
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
                    // 连接成功后统一发现服务（onScanResult 路径此前不调 discoverServices 会卡 Connecting）
                    runCatching { g.discoverServices() }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    connectDeferred?.complete(false)
                    connectDeferred = null
                    discoverDeferred?.complete(false)
                    discoverDeferred = null
                    if (intentionalDisconnect) {
                        runCatching { g.close() }
                        gatt = null
                        resetCharacteristics()
                        _connectionState.value = ConnectionState.Disconnected
                    } else {
                        // 非主动断开：autoConnect=true 已让 Android 后台自动重连，
                        // 切勿 close()（会取消 autoConnect），仅更新状态并把特征置空，
                        // 保留 gatt 句柄供重连复用。这样即使 AAPS 周期断连把 ACL 瞬断，
                        // 伴生 App 也不拆链，AAPS 下次直连可稳定复用同一 ACL。
                        resetCharacteristics()
                        _connectionState.value = ConnectionState.Disconnected
                    }
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

    /** 回传血糖 (mg/dl) 与趋势五档码(-2..2)，供泵显示。 */
    suspend fun sendCgm(mgdl: Int, trend: Int): Result<Unit> = withContext(Dispatchers.IO) {
        val char = cgmChar ?: return@withContext fail("未连接或未发现 CGM 特征")
        val payload = PumpProtocol.buildCgm(mgdl, trend)
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
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
     * 设置通道请求：写 [op][payload][crc] 后读取响应。
     * GET 类 op 返回结果字节；SET 类 op 返回 1 字节 ack (0=OK / 1=ERR)。
     */
    suspend fun requestSettings(op: Int, payload: ByteArray = byteArrayOf()): Result<ByteArray> =
        withContext(Dispatchers.IO) {
            val g = gatt ?: return@withContext fail("GATT 未就绪")
            val char = settingsChar ?: return@withContext fail("未连接或未发现 Settings 特征")
            val req = PumpProtocol.buildSettings(op, payload)
            val w = writeWithAck(g, char, req)
            if (w.isFailure) return@withContext Result.failure(w.exceptionOrNull()!!)
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
                return@withContext fail("发起设置读取失败")
            }
            val data = readDeferred?.await()
            readDeferred = null
            if (data != null) Result.success(data) else Result.failure(IllegalStateException("设置读取无响应"))
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

    private suspend fun writeWithAck(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray
    ): Result<Unit> {
        writeDeferred = CompletableDeferred()
        val started = writeCharacteristic(g, char, value)
        if (!started) {
            writeDeferred = null
            return Result.failure(IllegalStateException("发起写入失败"))
        }
        val ok = try { writeDeferred!!.await() } catch (_: Exception) { false }
        writeDeferred = null
        return if (ok) Result.success(Unit) else Result.failure(IOException2("写入未确认"))
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
