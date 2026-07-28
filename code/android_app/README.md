# OpenLoop Pump — Android 控制端

开源 DIY 闭环胰岛素泵的手机控制 APP，通过 BLE 5.0 直接控制 ESP32-C6 泵，
并提供闭环算法、CGM 集成、Nightscout 云端同步。

> ⚠️ 本项目为 DIY 开源项目，**非医疗器械**，仅供研究与学习，使用风险自负。

---

## 1. 技术栈

| 层 | 技术 |
|----|------|
| 语言 | Kotlin 1.9 |
| UI | Jetpack Compose (Material 3) + Navigation Compose |
| 架构 | MVVM + Repository + UseCase |
| 依赖注入 | Hilt |
| 本地存储 | Room（治疗/血糖历史）+ DataStore（配置） |
| 网络 | Retrofit + OkHttp（Nightscout） |
| 后台 | WorkManager（定时闭环）+ 前台 Service（BLE 保活） |
| 异步 | Kotlin Coroutines + Flow |

最低支持 Android 8.0 (API 26)。

---

## 2. 目录结构

```
app/src/main/java/com/openloop/pump/
├── OpenLoopApplication.kt          # Hilt 入口 + WorkManager 配置
├── MainActivity.kt                 # 入口 Activity
├── ble/                            # ★ BLE 通信层（对齐固件协议）
│   ├── PumpUuids.kt                #   服务/特征 UUID + 协议常量
│   ├── Crc8.kt                     #   CRC-8/CCITT 算法（与固件一致）
│   ├── ConnectionState.kt          #   连接状态机
│   ├── PumpProtocol.kt             #   命令构建 + 状态解析
│   └── PumpBleManager.kt           #   扫描/连接/读写/Notify/配对
├── domain/
│   ├── model/                      # 领域模型（PumpState, Bolus, Basal, Glucose, Alarm…）
│   └── algorithm/                  # 闭环算法（IOB / 血糖预测 / oref1 简化）
├── data/
│   ├── local/                      # Room 数据库 + DAO + 实体
│   ├── repository/                 # Pump / CGM / Nightscout / Preferences 仓库
│   └── nightscout/                 # Retrofit API + 模型
├── service/                        # 前台保活服务 + 闭环 Worker + 开机自启
├── ui/                             # Compose 界面（dashboard/bolus/basal/history/settings）
└── di/                             # Hilt 模块
```

---

## 3. 与固件协议对接（关键）

APP 的 BLE 协议**必须与 ESP32-C6 固件 `code/esp32_firmware/` 完全一致**：

- **基础 UUID**：`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`（Nordic UART 风格）
- **特征**：BOLUS(`…0002`)、BASAL(`…0003`)、TBR(`…0004`)、STATUS(`…0005`)、IOB(`…0006`)、RESERVOIR(`…0007`)
- **CRC**：所有写帧末字节为 CRC-8/CCITT（poly `0x07`，init `0x00`，非反射），见 `Crc8.kt`
- **写特征需加密链路**（固件启用 MITM + bonding），APP 连接后主动 `createBond()`
- 帧均为小端（little-endian）

详见 `ble/PumpProtocol.kt` 与固件 `ble_gatt_server.cpp`。

---

## 4. 构建

```bash
# 需要 Android SDK (compileSdk 34) + JDK 17
./gradlew assembleDebug        # 生成 debug APK
./gradlew test                 # 运行单元测试（算法验证）
```

首次构建会自动下载 Gradle 8.9 与依赖。

### 权限
- Android 12+：`BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`
- Android 11−：还需 `ACCESS_FINE_LOCATION`（BLE 扫描）
- 网络：`INTERNET`（仅 Nightscout）
- 后台：`FOREGROUND_SERVICE`（connectedDevice 类型）、`RECEIVE_BOOT_COMPLETED`

运行时需向用户申请蓝牙与通知权限。

---

## 5. 使用流程

1. 固件烧录后，泵以 `OpenLoop-Pump` 名称广播。
2. APP 首页自动扫描并连接（或手动"连接/扫描泵"）。
3. 配对（默认 Just Works；若固件设了 passkey，在设置页填入）。
4. 基础率页面编辑 24 槽方案并"应用全部到泵"。
5. 大剂量页面输入剂量（0.05U 步进）→ 确认推注。
6. 设置页开启"自动闭环"，后台每 15 分钟根据 CGM + IOB 调整临时基础率（TBR）。
7. 历史页可同步治疗记录到 Nightscout。

---

## 6. 安全原则

- **闭环只调整临时基础率，绝不自动给大剂量**；大剂量必须用户确认。
- 所有 BLE 命令带 CRC 校验，错误帧被固件拒绝。
- 写特征走加密链路，防止中间人篡改剂量。
- 报警分级（固件 `alarm_code_t`），临界报警在首页红色高亮。
