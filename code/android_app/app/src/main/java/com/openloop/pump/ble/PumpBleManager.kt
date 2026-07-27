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
import android.bluetooth.le.ScanFilter
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

    /** 配对 PIN（固件 passkey）。null = Just Works。 */
    var pairingPin: String? = null

    // ---- 对外状态流 ----
    private val _connectionState = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _pumpStatus = MutableStateFlow<PumpProtocol.PumpStatus?>(null)
    val pumpStatus: StateFlow<PumpProtocol.PumpStatus?> = _pumpStatus.asStateFlow()

    private val _iob = MutableStateFlow(0.0)
    val iob: StateFlow<Double> = _iob.asStateFlow()

    private val _reservoir = MutableStateFlow(0)
    val reservoir: StateFlow<Int> = _reservoir.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    // ---- 异步操作信号 ----
    private var connectDeferred: CompletableDeferred<Boolean>? = null
    private var discoverDeferred: CompletableDeferred<Boolean>? = null
    private var writeDeferred: CompletableDeferred<Boolean>? = null
    private var readDeferred: CompletableDeferred<ByteArray?>? = null
    private val bondListeners = mutableListOf<(Boolean) -> Unit>()

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
        val filter = ScanFilter.Builder()
            .setDeviceName(SCAN_DEVICE_NAME)
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        try {
            sc.startScan(listOf(filter), settings, scanCallback)
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
            val device = result?.device ?: return
            if (device.name != SCAN_DEVICE_NAME) return
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
        _connectionState.value = ConnectionState.Connecting
        connectDeferred = CompletableDeferred()
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
        val connected = connectDeferred?.await() ?: false
        if (!connected) return@withContext false

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
        runCatching {
            gatt?.disconnect()
            gatt?.close()
        }
        gatt = null
        resetCharacteristics()
        _connectionState.value = ConnectionState.Disconnected
        _pumpStatus.value = null
    }

    private fun resetCharacteristics() {
        bolusChar = basalChar = tbrChar = statusChar = iobChar = reservoirChar = null
    }

    private val gattCallback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    connectDeferred?.complete(true)
                    connectDeferred = null
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    connectDeferred?.complete(false)
                    connectDeferred = null
                    discoverDeferred?.complete(false)
                    discoverDeferred = null
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

            enableNotifications(g)
            _connectionState.value = ConnectionState.Connected
            discoverDeferred?.complete(true)
            discoverDeferred = null

            // 主动拉取一次状态
            readStatusSuspended()
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
        listOfNotNull(statusChar, iobChar, reservoirChar).forEach { char ->
            g.setCharacteristicNotification(char, true)
            val cccd = char.getDescriptor(PumpUuids.CCCD) ?: return@forEach
            val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeDescriptor(cccd, value)
            } else {
                @Suppress("DEPRECATION")
                cccd.value = value
                g.writeDescriptor(cccd)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun handleNotification(uuid: java.util.UUID?, value: ByteArray?) {
        value ?: return
        when (uuid) {
            PumpUuids.CHAR_STATUS -> PumpProtocol.PumpStatus.fromBytes(value)
                ?.let { _pumpStatus.value = it }
            PumpUuids.CHAR_IOB -> PumpProtocol.parseIob(value)?.let { _iob.value = it }
            PumpUuids.CHAR_RESERVOIR -> PumpProtocol.parseReservoir(value)
                ?.let { _reservoir.value = it }
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

    /** 更新基础率单槽。 */
    suspend fun setBasalSlot(slot: Int, rateUh: Double): Result<Unit> = withContext(Dispatchers.IO) {
        val char = basalChar ?: return@withContext fail("未连接或未发现 Basal 特征")
        val payload = runCatching { PumpProtocol.buildBasalSlot(slot, rateUh) }
            .getOrElse { return@withContext Result.failure(it) }
        writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
    }

    /** 发送临时基础率 (TBR)。 */
    suspend fun setTemporaryBasal(rateUh: Double, durationMin: Int): Result<Unit> =
        withContext(Dispatchers.IO) {
            val char = tbrChar ?: return@withContext fail("未连接或未发现 TBR 特征")
            val payload = runCatching { PumpProtocol.buildTbr(rateUh, durationMin) }
                .getOrElse { return@withContext Result.failure(it) }
            writeWithAck(gatt ?: return@withContext fail("GATT 未就绪"), char, payload)
        }

    /** 读取一次泵状态。 */
    suspend fun readStatus(): Result<PumpProtocol.PumpStatus> = withContext(Dispatchers.IO) {
        val g = gatt ?: return@withContext fail("GATT 未就绪")
        val char = statusChar ?: return@withContext fail("未连接或未发现 Status 特征")
        readDeferred = CompletableDeferred()
        val started = runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.readCharacteristic(char) == BluetoothGatt.GATT_SUCCESS
            } else {
                @Suppress("DEPRECATION")
                g.readCharacteristic(char)
            }
        }.getOrDefault(false)
        if (!started) {
            readDeferred = null
            return@withContext fail("发起读取失败")
        }
        val data = readDeferred?.await()
        readDeferred = null
        val status = data?.let { PumpProtocol.PumpStatus.fromBytes(it) }
        if (status != null) {
            _pumpStatus.value = status
            Result.success(status)
        } else {
            Result.failure(IllegalStateException("状态解析失败或 CRC 校验不通过"))
        }
    }

    private fun readStatusSuspended() {
        // 在发现服务后异步触发一次读取（一次性 fire-and-forget，会自行结束）
        CoroutineScope(SupervisorJob() + Dispatchers.IO).launch { readStatus() }
    }

    // ============================================================
    // 底层读写（兼容新旧 API）
    // ============================================================

    private suspend fun writeWithAck(
        g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray
    ): Result<Unit> = suspendCancellableCoroutine { cont ->
        writeDeferred = CompletableDeferred()
        val started = writeCharacteristic(g, char, value)
        if (!started) {
            writeDeferred = null
            cont.resume(Result.failure(IllegalStateException("发起写入失败")))
            return@suspendCancellableCoroutine
        }
        cont.invokeOnCancellation { writeDeferred = null }
        val ok = runCatching { writeDeferred!!.await() }.getOrDefault(false)
        writeDeferred = null
        cont.resume(if (ok) Result.success(Unit) else Result.failure(IOException2("写入未确认")))
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
        const val SCAN_DEVICE_NAME = "OpenLoop-Pump"
    }
}

/** 内部 IO 异常别名，避免与标准库同名混淆。 */
private class IOException2(message: String) : Exception(message)
