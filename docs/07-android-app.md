# 手机 APP 设计

## 1. APP 概览

| 属性 | 规格 |
|------|------|
| 平台 | Android 8.0+ (API 26+) |
| 名称建议 | **LoopKit** 或 **OpenPump** |
| 语言 | Kotlin（首选）/ Java |
| UI | Jetpack Compose |
| 最低 RAM | 2GB |
| 存储 | 50MB |
| 网络 | Wi-Fi/4G（仅 Nightscout 同步） |

---

## 2. 主要功能

### 2.1 用户界面（首页 Dashboard）

```
┌─────────────────────────────────┐
│ LoopKit                  ⚙️ 设置 │
├─────────────────────────────────┤
│                                 │
│   ╭───────────────────────╮     │
│   │                       │     │
│   │     血糖 6.8 mmol/L   │     │ ← 大字号显示
│   │         120 mg/dL     │     │
│   │       ↗ 上升趋势       │     │ ← CGM 数据
│   │                       │     │
│   ╰───────────────────────╯     │
│                                 │
│   IOB (体内胰岛素)：1.2 U       │
│   基础率：1.0 U/h (自动调整)    │
│                                 │
│   ╭─────────╮  ╭─────────╮     │
│   │ 🔋 78%  │  │ 💧 124U  │     │ ← 电池和药量
│   ╰─────────╯  ╰─────────╯     │
│                                 │
│   [大剂量]  [历史]  [餐前]      │
│                                 │
└─────────────────────────────────┘
```

### 2.2 大剂量输入界面

```
┌─────────────────────────────────┐
│  ← 返回                  大剂量  │
├─────────────────────────────────┤
│                                 │
│       输入剂量                  │
│                                 │
│   ╭─────────────╮               │
│   │    5.00     │ U             │
│   ╰─────────────╯               │
│                                 │
│   延长推注？  [ ○ 否 ] [ ● 是 ]  │
│   延长时间：[ 30 ] 分钟          │
│                                 │
│   备注：___________________      │
│                                 │
│   [取消]            [确认推注]  │
│                                 │
└─────────────────────────────────┘
```

### 2.3 基础率设置

```
┌─────────────────────────────────┐
│  ← 返回            基础率方案    │
├─────────────────────────────────┤
│                                 │
│  方案：[ ● 日常 ] [ ○ 周末 ]     │
│                                 │
│  时间        速率 (U/h)         │
│  ──────────────────────         │
│  00:00 - 06:00    0.8           │
│  06:00 - 08:00    1.0           │
│  08:00 - 12:00    1.2           │
│  12:00 - 14:00    0.9           │
│  14:00 - 18:00    1.0           │
│  18:00 - 22:00    1.3           │
│  22:00 - 24:00    0.8           │
│                                 │
│  总日剂量：22.8 U               │
│                                 │
│  [ + 添加时间段 ]                │
│  [保存]                          │
│                                 │
└─────────────────────────────────┘
```

### 2.4 历史记录

```
┌─────────────────────────────────┐
│  ← 返回             历史记录     │
├─────────────────────────────────┤
│                                 │
│  今天（2026-07-25）              │
│  ──────────────────────────     │
│  12:30  🔵 大剂量 5.0U (餐前)   │
│  12:00  ⚪ 基础率 1.0U/h (开始) │
│  11:30  📊 血糖 6.8 mmol/L      │
│  10:00  ⚪ 基础率 0.8U/h (开始) │
│  08:00  🟢 自动调整 +0.2U/h     │
│  ...                            │
│                                 │
│  昨天 (2026-07-24)              │
│  ...                            │
│                                 │
│  [同步到 Nightscout]              │
└─────────────────────────────────┘
```

---

## 3. BLE 通信模块

### 3.1 BLE 扫描与连接

```kotlin
class PumpBleManager @Inject constructor(
    @ApplicationContext private val context: Context
) {
    private var gatt: BluetoothGatt? = null
    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    // 特征 UUID（与 ESP32 GATT 服务对应）
    companion object {
        val PUMP_SERVICE_UUID = UUID.fromString("00001a2b-0000-1000-8000-00805f9b34fb")
        val BOLUS_CHAR_UUID = UUID.fromString("00002a50-0000-1000-8000-00805f9b34fb")
        val STATUS_CHAR_UUID = UUID.fromString("00002a53-0000-1000-8000-00805f9b34fb")
        // ... 其他特征
    }

    suspend fun connect(pumpAddress: String) = withContext(Dispatchers.IO) {
        val device = BluetoothAdapter.getDefaultAdapter()
            .getRemoteDevice(pumpAddress)

        gatt = device.connectGatt(context, false, gattCallback)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _connectionState.value = ConnectionState.CONNECTED
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _connectionState.value = ConnectionState.DISCONNECTED
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                // 启用 Notify
                val statusChar = g.getService(PUMP_SERVICE_UUID)
                    ?.getCharacteristic(STATUS_CHAR_UUID)
                g.setCharacteristicNotification(statusChar, true)
                // 设置 CCCD 描述符
                statusChar?.getDescriptor(CCCD_UUID)?.let { desc ->
                    desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(desc)
                }
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, char: BluetoothGattCharacteristic) {
            // 处理 Notify 数据
            handleNotification(char.value)
        }
    }

    suspend fun sendBolus(units: Double, durationMin: Int = 0): Boolean {
        val cmd = buildBolusCommand(units, durationMin)
        val char = gatt?.getService(PUMP_SERVICE_UUID)?.getCharacteristic(BOLUS_CHAR_UUID)
            ?: return false

        return withContext(Dispatchers.IO) {
            char.value = cmd
            gatt?.writeCharacteristic(char) ?: false
        }
    }

    private fun buildBolusCommand(units: Double, durationMin: Int): ByteArray {
        val unitsX100 = (units * 100).toInt()
        return ByteBuffer.allocate(8)
            .order(ByteOrder.LITTLE_ENDIAN)
            .put(0x01.toByte())                    // opcode: bolus
            .putInt(unitsX100)                     // units × 100
            .put(durationMin.toByte())             // duration
            .put(0x00.toByte())                    // flags
            .put(computeCrc8().toByte())           // CRC-8
            .array()
    }

    // CRC-8 实现
    private fun computeCrc8(): Int {
        // 简单 CRC-8/MAXIM 实现
        return 0x00  // TODO
    }
}
```

---

## 4. Nightscout 同步模块

### 4.1 API 接口

```kotlin
interface NightscoutApi {
    @POST("treatments")
    suspend fun postTreatment(@Body treatment: Treatment): Response<Unit>

    @GET("entries.json")
    suspend fun getEntries(
        @Query("count") count: Int = 10,
        @Query("find[date][$gte]") from: Long? = null
    ): List<GlucoseEntry>

    @GET("status.json")
    suspend fun getStatus(): NightscoutStatus
}
```

### 4.2 Retrofit 配置

```kotlin
@Module
@InstallIn(SingletonComponent::class)
object NetworkModule {

    @Provides
    @Singleton
    fun provideRetrofit(): Retrofit {
        val client = OkHttpClient.Builder()
            .addInterceptor { chain ->
                val request = chain.request().newBuilder()
                    .addHeader("API-SECRET", BuildConfig.NS_API_SECRET)
                    .build()
                chain.proceed(request)
            }
            .build()

        return Retrofit.Builder()
            .baseUrl(SharedPrefs.getNightscoutUrl())
            .client(client)
            .addConverterFactory(GsonConverterFactory.create())
            .build()
    }
}
```

---

## 5. 闭环算法实现（自研 oref1）

```kotlin
class ClosedLoopAlgorithm @Inject constructor() {

    data class Inputs(
        val glucoseMgdl: Int,
        val glucoseTrend: Trend,
        val iob: Double,                // U
        val basalRate: Double,          // U/h
        val targetGlucose: Int = 110,
        val isf: Int = 50,              // mg/dL/U
        val maxIOB: Double = 4.0        // U
    )

    data class Outputs(
        val tbrRate: Double,            // U/h
        val tbrDurationMin: Int = 30,
        val recommendedBolus: Double = 0.0  // U
    )

    fun compute(inputs: Inputs): Outputs {
        val bg = inputs.glucoseMgdl
        val target = inputs.targetGlucose
        val isf = inputs.isf
        val iob = inputs.iob

        // 预计 30 分钟后的血糖
        val predictedDrop = iob * isf * (30.0 / 60.0)  // 简化
        val predictedBG = bg - predictedDrop

        return if (predictedBG > target + 30) {
            // 高血糖：增加基础率
            val correction = (predictedBG - target) / isf
            val adjustedIOB = iob + correction
            val newRate = if (adjustedIOB < inputs.maxIOB) {
                inputs.basalRate + correction * 2
            } else {
                inputs.basalRate  // 受 maxIOB 限制
            }
            Outputs(
                tbrRate = newRate.coerceAtMost(MAX_BASAL_RATE),
                tbrDurationMin = 30
            )
        } else if (predictedBG < target - 30) {
            // 低血糖：减少基础率
            val reduce = (target - predictedBG) / isf
            val newRate = (inputs.basalRate - reduce).coerceAtLeast(0.0)
            Outputs(
                tbrRate = newRate,
                tbrDurationMin = 30
            )
        } else {
            // 正常范围：维持
            Outputs(
                tbrRate = inputs.basalRate,
                tbrDurationMin = 30
            )
        }
    }

    companion object {
        const val MAX_BASAL_RATE = 5.0
    }
}
```

---

## 6. CGM 数据接收（xDrip+ 集成）

xDrip+ 是 Android 上流行的 CGM 数据广播 APP。它通过本地 UDP 广播或内容提供者共享数据：

```kotlin
class CGMBridge @Inject constructor(@ApplicationContext private val context: Context) {

    fun observeCGM(): Flow<GlucoseReading> = callbackFlow {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent?.action == "com.eveningoutpost.dexdrip.BgEstimate") {
                    val glucose = intent.getDoubleExtra("estimated_glucose_mgdl", 0.0).toInt()
                    val trend = intent.getStringExtra("trend") ?: "Flat"
                    val timestamp = intent.getLongExtra("timestamp", System.currentTimeMillis())
                    trySend(GlucoseReading(glucose, trend, timestamp))
                }
            }
        }
        context.registerReceiver(
            receiver,
            IntentFilter("com.eveningoutpost.dexdrip.BgEstimate"),
            Context.RECEIVER_EXPORTED
        )
        awaitClose { context.unregisterReceiver(receiver) }
    }
}
```

---

## 7. 关键依赖

```gradle
// build.gradle.kts (Module: app)
dependencies {
    // Kotlin & Coroutines
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.0")

    // AndroidX & Compose
    implementation("androidx.core:core-ktx:1.13.1")
    implementation(platform("androidx.compose:compose-bom:2024.06.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.0")

    // Hilt (DI)
    implementation("com.google.dagger:hilt-android:2.51")
    kapt("com.google.dagger:hilt-compiler:2.51")
    implementation("androidx.hilt:hilt-navigation-compose:1.2.0")

    // Room (数据库)
    implementation("androidx.room:room-runtime:2.6.1")
    implementation("androidx.room:room-ktx:2.6.1")
    kapt("androidx.room:room-compiler:2.6.1")

    // Retrofit + OkHttp (网络)
    implementation("com.squareup.retrofit2:retrofit:2.11.0")
    implementation("com.squareup.retrofit2:converter-gson:2.11.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.squareup.okhttp3:logging-interceptor:4.12.0")

    // WorkManager (后台同步)
    implementation("androidx.work:work-runtime-ktx:2.9.0")
    implementation("androidx.hilt:hilt-work:1.2.0")
    kapt("androidx.hilt:hilt-compiler:1.2.0")

    // DataStore (配置)
    implementation("androidx.datastore:datastore-preferences:1.1.1")
}
```

---

## 8. 关键权限

```xml
<!-- AndroidManifest.xml -->
<uses-permission android:name="android.permission.BLUETOOTH" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />
<uses-permission android:name="android.permission.WAKE_LOCK" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
```

---

## 9. 架构图

```
┌──────────────────────────────────────────────────────────┐
│                    Android APP                            │
│  ┌────────────────────────────────────────────────────┐  │
│  │                  UI Layer (Compose)                │  │
│  │  Dashboard │ Bolus │ Basal │ History │ Settings   │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│                          ▼                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │              ViewModel Layer (MVVM)                │  │
│  │  DashboardVM │ BolusVM │ BasalVM │ HistoryVM        │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│                          ▼                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │              Domain Layer (Use Cases)              │  │
│  │  DeliverBolus │ ScheduleBasal │ SyncNightscout      │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│                          ▼                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │              Data Layer (Repositories)             │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────┐ │  │
│  │  │ BLE Manager  │  │ Nightscout   │  │  CGM     │ │  │
│  │  │              │  │ API          │  │  Bridge  │ │  │
│  │  └──────────────┘  └──────────────┘  └──────────┘ │  │
│  │  ┌──────────────┐  ┌──────────────┐               │  │
│  │  │ Local DB     │  │ Preferences  │               │  │
│  │  │ (Room)       │  │ (DataStore)  │               │  │
│  │  └──────────────┘  └──────────────┘               │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                                │
│                          ▼                                │
│  ┌────────────────────────────────────────────────────┐  │
│  │              Algorithm Layer                        │  │
│  │  ClosedLoop │ IOB │ GlucoseForecast                │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
                            │
                            │ BLE 5.0
                            ▼
                  ┌──────────────────────┐
                  │  ESP32-C6 Pump       │
                  └──────────────────────┘
```

---

## 10. 测试计划

| 测试 | 工具 | 频率 |
|------|------|------|
| 单元测试 | JUnit5 + MockK | 每次提交 |
| UI 测试 | Compose Test | 每次发布 |
| BLE 模拟测试 | MockGatt | 开发期 |
| 集成测试 | AndroidX Test | 每版本 |
| 端到端测试 | Espresso | 月度 |