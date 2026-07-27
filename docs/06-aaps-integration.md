# AAPS（AndroidAPS）闭环集成

## 1. AAPS 工作原理

### 1.1 AAPS 系统组成

```
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│   CGM        │───▶│  AAPS APP    │───▶│  胰岛素泵    │
│ (持续血糖监测)│    │ (闭环算法)   │    │              │
│              │    │              │    │              │
│ • Dexcom G6 │    │ • oref1 算法 │    │ • 接收 TBR   │
│ • Libre 2/3 │    │ • 自动基础率 │    │ • 执行推注   │
│ • Medtronic │    │ • 大剂量建议 │    │              │
└──────────────┘    └──────┬───────┘    └──────────────┘
                           │
                           ▼
                  ┌──────────────┐
                  │  Nightscout  │
                  │  (云端记录)  │
                  └──────────────┘
```

### 1.2 AAPS 通信模型

AAPS 通过 **泵驱动（pump driver）** 模块与泵通信：

```
AAPS Core → Pump Driver Interface → [具体泵实现]
                                          ↓
                                       通信协议
                                          ↓
                                       胰岛素泵
```

目前 AAPS 支持的泵驱动：
- MedtronicPumpDriver（需要 RileyLink 桥）
- DanaRDriver / DanaRSDriver（直接 BLE）
- InsightDriver（直接 BLE）
- OmnipodDriver（需要 RileyLink 或 OAI 桥）
- MedtrumDriver（直接 BLE）
- ComboDriver（BLE）
- [GenericPumpDriver]（待扩展）

---

## 2. 三种集成方案对比

| 方案 | 难度 | 用户体验 | 推荐度 |
|------|------|----------|--------|
| **A. 模拟 Medtronic 协议** | ⭐⭐⭐⭐ | 用户用现成 AAPS | ⭐⭐ |
| **B. 自定义 AAPS 驱动** | ⭐⭐⭐ | 用户用现成 AAPS（需要构建自定义 APK） | ⭐⭐⭐⭐ |
| **C. 仅自研 APP + Nightscout** | ⭐⭐ | 用户用我们的 APP | ⭐⭐⭐ |

### 2.1 方案 A：Medtronic 协议仿真

**原理**：ESP32-C3 仿真老款 Medtronic 泵（如 712/715/722）的 RF 协议，让 AAPS 通过 RileyLink-like 桥连接。

**优点**：AAPS 代码完全不需要改动。

**缺点**：
- 需要逆向 Medtronic 715/722 的 sub-GHz 协议（难度高）
- 需要 RFM69/RFM95 模块（增加成本）
- ESP32-C3 的软件栈很复杂

**结论**：**不推荐**。需要硬件改动（增加 sub-GHz 射频模块），不符合 ESP32-C6 + DRV8825 的简化目标。

### 2.2 方案 B：自定义 AAPS 驱动（推荐）

**原理**：在 AAPS 中新增一个 `OpenPumpDriver`，实现标准 Pump Driver 接口，通过 BLE 直接与我们的泵通信。

**步骤**：

1. **Fork AAPS 仓库**：从 GitHub fork AndroidAPS
2. **实现 Pump Driver 接口**：参考 InsightDriver / DanaRDriver
3. **注册新驱动**：在 Pump Envelope 中添加 OpenPump 选项
4. **实现 BLE 通信**：与 ESP32-C6 的自定义 GATT 服务通信
5. **AAPS 配置**：用户在 AAPS 中选择 "OpenPump" 泵型号
6. **自定义 APK**：需要用户自行构建 APK（gradle build）

**关键 Java/Kotlin 接口**：

```kotlin
// AAPS 的 Pump Interface（简化）
interface PumpInterface {
    val isInitialized: Boolean
    fun connect()
    fun disconnect()
    fun startBolus(units: Double, duration: Duration)
    fun cancelBolus()
    fun setTemporaryBasal(rate: Double, duration: Duration)
    fun cancelTemporaryBasal()
    fun readBasalSchedule(): List<BasalProfileEntry>
    fun writeBasalSchedule(profile: List<BasalProfileEntry>)
    fun readHistory(): List<HistoryEntry>
    fun readPumpStatus(): PumpStatus
    val reservoirUnits: Double
    val batteryLevel: Int
}
```

### 2.3 方案 C：自研 APP + Nightscout 上传（推荐用于快速验证）

**原理**：
- 自研 Android APP 通过 BLE 与泵通信
- APP 上传数据到 Nightscout（云端）
- AAPS 可以从 Nightscout 读取历史数据（只读模式）
- 闭环控制由自研 APP 完成（实现 oref1 简化版）

**优点**：
- 不需要 fork AAPS
- 开发周期短（1-2 个月）
- 我们的 APP 独立运行，AAPS 可选

**缺点**：
- 用户需要切换 APP
- AAPS 闭环功能无法直接用

---

## 3. 推荐实施路线

```
Phase 1（1-2 周）：方案 C 起步
   └── 自研 Android APP（基本控制：基础率/大剂量/历史）
   └── BLE 直连 ESP32-C6
   └── 同步数据到 Nightscout

Phase 2（2-4 周）：闭环算法移植
   └── 在自研 APP 中实现 oref1 简化版
   └── 集成 CGM 数据（xDrip+ 接口）
   └── 自动调整基础率

Phase 3（4-8 周）：AAPS 集成（方案 B）
   └── Fork AAPS 仓库
   └── 实现 OpenPumpDriver
   └── 自定义 APK 构建文档
   └── 用户可选择 "OpenPump" 泵型号
```

---

## 4. OpenPumpDriver 实现要点

### 4.1 BLE 服务与 AAPS 的映射

| AAPS 操作 | ESP32 GATT 命令 |
|-----------|-----------------|
| `startBolus(5U, 0s)` | Write Bolus Command (units=500, duration=0) |
| `setTemporaryBasal(1.5U/h, 30min)` | Write TBR Command (rate=1500, duration=30) |
| `cancelTemporaryBasal()` | Write TBR Command (rate=0, duration=0) |
| `readBasalSchedule()` | Read Basal Schedule Characteristic |
| `readHistory()` | Read History Characteristic (multi-packet) |
| `readPumpStatus()` | Read Pump Status Characteristic |
| `reservoirUnits` | Read Reservoir Level Characteristic |
| `batteryLevel` | Read Battery Level Characteristic (标准 BLE Battery Service) |

### 4.2 AAPS 配置

在 AAPS 配置中（用户构建自定义 APK 后）：

1. 进入 **Config Builder** → **Pump**
2. 选择 **OpenPump**
3. 配对 BLE 设备（首次）
4. 设置最大基础率、最大大剂量
5. 设置基础率方案
6. 启用 AAPS 闭环

### 4.3 关键安全

AAPS 安全机制会检查：
- **Max Bolus**（泵端硬限制 + AAPS 软限制）
- **Max Basal**（同上）
- **maxIOB**（体内最大活性胰岛素，AAPS 算法限制）
- **Watchdog**（通信超时自动停止推注）

我们泵端必须**强制执行**所有限制：
```cpp
// 在 ESP32 固件中
if (cmd.units > pump_config.max_bolus_per_hour * 100) {
    return ERROR_LIMIT_EXCEEDED;
}
```

---

## 5. 通信协议完整性测试

### 5.1 BLE 测试工具

- **nRF Connect**（手机 APP，Nordic 出品，免费）
- **Web Bluetooth API**（浏览器调试）
- **bleak**（Python 库，PC 端自动化测试）

### 5.2 测试用例

```python
# 使用 bleak 测试 ESP32-C6 泵 (阶段5 协议: 自定义 128-bit UUID + CRC-8/CCITT)
import asyncio
from bleak import BleakClient

PUMP_ADDRESS = "XX:XX:XX:XX:XX:XX"
# 自定义 Pump Control Service 下的特征 UUID (base 6E400001-B5A3-F393-E0A9-E50E24DCCA9E)
BOLUS_CHAR_UUID = "9ECA-DC240EE5-A9E0-93F3-A3B50100406E".replace("-", "")  # 实际见 config.h BLE_CHAR_BOLUS_UUID
STATUS_CHAR_UUID = "9ECA-DC240EE5-A9E0-93F3-A3B50100406E".replace("-", "")  # 实际见 BLE_CHAR_STATUS_UUID

def crc8_ccitt(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

async def test_bolus():
    async with BleakClient(PUMP_ADDRESS) as client:
        await client.connect()
        print(f"Connected: {client.is_connected}")

        # 推注 5.00U 测试: payload=units_x100(4B LE) + crc
        payload = (500).to_bytes(4, 'little')   # 5.00U -> 500 (注意: 示例中 0.5U=50, 5U=500)
        cmd = payload + bytes([crc8_ccitt(payload)])
        await client.write_gatt_char(BOLUS_CHAR_UUID, cmd)

        await asyncio.sleep(2)

        # 读取/订阅 status (JSON: {"bat":..,"st":..,"alm":..,"glu":..,"tr":..,"loop":..,"tbr":..})
        status = await client.read_gatt_char(STATUS_CHAR_UUID)
        print(f"Status: {status.decode(errors='replace')}")

asyncio.run(test_bolus())
```

> ⚠️ 上述 UUID 为示意，真实 128-bit UUID 见 `code/esp32_firmware/src/config.h` 的
> `BLE_CHAR_*_UUID` 宏（base `9ECA-DC24-0EE5-A9E0-93F3-A3B5-0100-406E`，特征末 2 字节区分）。
> 阶段5 起所有写命令统一在 payload 末字节附加 CRC-8/CCITT(poly 0x07, init 0x00)，
> 格式见 `docs/05-firmware-design.md §5.2`。**Android APP 的 BLE 层需同步更新**到该
> 二进制+CRC 格式（原文档早期示例的 16-bit UUID + opcode 写法已废弃）。

---

## 6. 自研 APP 设计

### 6.1 APP 架构

```
┌──────────────────────────────────────┐
│  Android UI (Jetpack Compose)        │
├──────────────────────────────────────┤
│  ViewModels (MVVM)                    │
├──────────────────────────────────────┤
│  Domain Layer (Use Cases)             │
│  - BolusUseCase                       │
│  - BasalUseCase                       │
│  - HistoryUseCase                     │
│  - AlarmUseCase                       │
├──────────────────────────────────────┤
│  Data Layer                           │
│  - BLE Manager (BluetoothGatt)        │
│  - Pump Repository                    │
│  - Nightscout Sync (Retrofit)         │
│  - CGM Bridge (xDrip+ API)            │
├──────────────────────────────────────┤
│  Algorithm Layer                      │
│  - oref1 simplified                   │
│  - IOB (Insulin on Board)             │
│  - Basal Adjustment                   │
└──────────────────────────────────────┘
```

### 6.2 关键技术

| 技术 | 用途 |
|------|------|
| Kotlin | 主要语言 |
| Jetpack Compose | UI |
| Hilt | 依赖注入 |
| Coroutines + Flow | 异步 |
| Room | 本地数据库 |
| Retrofit + OkHttp | Nightscout API |
| Bluetooth Low Energy API | 泵通信 |
| WorkManager | 后台同步 |

### 6.3 关键功能

1. **基础率设置**：24 小时可调，0.05U 步进
2. **大剂量**：0.05U 步进，0-25U 范围，延长推注支持
3. **历史记录**：查看推注历史（推送同步 Nightscout）
4. **CGM 数据**：通过 xDrip+ API 接收
5. **闭环控制**：oref1 简化算法
6. **报警**：电池低、储药器空、阻塞、CGM 异常

---

## 7. Nightscout 集成

### 7.1 上传数据格式（Nightscout API v3）

```json
{
    "date": 1719432000000,
    "dateString": "2026-06-25T12:00:00.000Z",
    "sgv": 120,
    "direction": "Flat",
    "trend": 4,
    "type": "sgv"
}
```

### 7.2 推注事件上传

```json
{
    "eventType": "Bolus",
    "insulin": 5.0,
    "date": 1719432000000,
    "notes": "Meal bolus",
    "device": "OpenPump-001"
}
```

### 7.3 自研 APP 上传示例

```kotlin
class NightscoutUploader @Inject constructor(
    private val api: NightscoutApi
) {
    suspend fun uploadBolus(units: Double, timestamp: Long, notes: String = "") {
        val entry = Treatments(
            eventType = "Bolus",
            insulin = units,
            date = timestamp,
            notes = notes,
            device = "OpenPump-${getDeviceId()}"
        )
        api.postTreatment(entry)
    }
}
```

---

## 8. 闭环算法（自研 APP）

### 8.1 简化 oref1 算法

```
输入：
  BG_current = 当前血糖（mg/dL）
  BG_target = 目标血糖（默认 110 mg/dL）
  ISF = 胰岛素敏感系数（默认 50 mg/dL/U）
  IOB = 体内活性胰岛素（U）
  basal_rate = 当前基础率（U/h）

输出：
  basal_adjustment = 临时基础率调整

算法：
  delta = BG_current - BG_target
  expected_drop = IOB * ISF  # IOB 预期降糖
  predicted_BG = BG_current - expected_drop
  
  if predicted_BG > BG_target:
      # 需要更多胰岛素
      required_extra = (predicted_BG - BG_target) / ISF
      # 限制：maxIOB
      if IOB + required_extra < MAX_IOB:
          adjustment_rate = basal_rate + required_extra * (60 / TBR_DURATION)
      else:
          adjustment_rate = basal_rate  # 受限于 maxIOB
  elif predicted_BG < BG_target:
      # 需要更少胰岛素
      required_reduce = (BG_target - predicted_BG) / ISF
      adjustment_rate = basal_rate - required_reduce * (60 / TBR_DURATION)
  else:
      adjustment_rate = basal_rate
  
  # 限制：0 < rate < max_basal
  adjustment_rate = clamp(adjustment_rate, 0, MAX_BASAL)
  
  setTemporaryBasal(adjustment_rate, duration=30min)
```

### 8.2 IOB 计算

```kotlin
fun calculateIOB(bolusHistory: List<BolusRecord>, currentTime: Long): Double {
    val DIA = 5 * 60 * 60.0  // 5 小时胰岛素活性时间（秒）
    var iob = 0.0

    for (bolus in bolusHistory) {
        val t = currentTime - bolus.time  // 时间差（秒）
        if (t < 0 || t > DIA) continue

        // 指数衰减（简化版）
        val remainingFraction = exp(-t / DIA)
        iob += bolus.units * remainingFraction
    }

    return iob
}
```

---

## 9. 实际使用流程（用户视角）

```
用户日常使用：
1. 早晨：起床，查看 APP 状态（CGM、IOB、剩余药量、电池）
2. 餐前：在 APP 中输入碳水 → APP 计算建议大剂量 → 用户确认
3. APP 推注：BLE 发送命令 → 泵执行 → APP 显示结果
4. 日常：APP 后台运行，每 5 分钟接收 CGM 数据，自动调整基础率
5. 睡前：APP 显示当日统计（总剂量、TIR 时间在 70-180）

异常情况：
- APP 推送通知（电池低、储药器空、阻塞）
- 泵端蜂鸣器报警
- 自动切换到 OpenAPS 安全模式（仅基础率 0U/h）
```

---

## 10. 风险与免责声明

⚠️ **强烈警告**：
1. 本项目的 AAPS 集成**未经医疗认证**
2. oref1 算法简化版本**未经验证**
3. 严禁直接用于人类胰岛素注射
4. 强烈建议保留商业胰岛素泵作为备份
5. 所有使用者必须接受糖尿病教育，理解 APS 系统的局限

> 推荐：初期使用本泵时，**仅作为研究/教学工具**，注射生理盐水用于测试。