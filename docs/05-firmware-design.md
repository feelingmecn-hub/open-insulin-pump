# 固件架构设计

## 1. 固件总体架构

### 1.1 技术栈

| 层级 | 技术 | 说明 |
|------|------|------|
| 开发框架 | Arduino IDE 2.x + ESP32 Board Manager 3.x | 用户态 C++，免 CMake，Waveshare C5/C6 参考示例即 Arduino |
| RTOS | FreeRTOS（Arduino-ESP32 内置） | 多任务调度 |
| 显示 | Arduino_GFX（GFX_Library_for_Arduino）+ LVGL 9.5.0 | ST7789 172×320 驱动与 UI |
| BLE 协议栈 | NimBLE-Arduino | 节省 RAM，与 ESP-IDF NimBLE 协议兼容 |
| 存储 | Preferences（NVS 封装） | 配置、历史记录 |
| 升级 | OTA（ArduinoOTA / HTTP） | 远程升级 |
| 加密 | NimBLE-Arduino 安全配对（Just Works / Passkey）+ 链路层 AES-CCM | BLE 配对加密 |

> **为何选 Arduino 框架？** 项目决策（用户明确要求）：Waveshare ESP32-C6-LCD-1.47 官方 /
> C5 参考示例均为 Arduino；Arduino_GFX + LVGL 9.5.0 对 ST7789 172×320 驱动成熟
> （含 `LCD_X_GAP=34` 列偏移与 `RGB565_SWAPPED` 颜色格式处理）；NimBLE-Arduino 提供与
> ESP-IDF NimBLE 一致的 GATT 协议且占用更小。开发迭代更快，无需维护 CMake/ESP-IDF 工具链。

### 1.2 任务划分

```c
// 任务优先级（数值越大越高）
#define TASK_PRIORITY_SAFETY       10   // 安全监控（最高）
#define TASK_PRIORITY_MOTOR         8   // 电机控制
#define TASK_PRIORITY_BLE           6   // BLE GATT
#define TASK_PRIORITY_BATTERY       5   // 电池监测
#define TASK_PRIORITY_BASAL         5   // 基础率周期调度
#define TASK_PRIORITY_DISPLAY       4   // LCD 显示
#define TASK_PRIORITY_KEYPAD        4   // 4 键按键板
#define TASK_PRIORITY_STORAGE       3   // 历史记录（实际由 history_log 节流落盘，无独立任务）
#define TASK_PRIORITY_OTA           2   // OTA 检查
#define TASK_PRIORITY_IDLE          1   // 空闲任务（最低）

// 任务栈大小
#define STACK_SIZE_SAFETY       4096
#define STACK_SIZE_MOTOR        4096
#define STACK_SIZE_BLE          8192
#define STACK_SIZE_BATTERY      2048
#define STACK_SIZE_BASAL        2048
#define STACK_SIZE_DISPLAY      6144
#define STACK_SIZE_KEYPAD       2048
#define STACK_SIZE_STORAGE      4096
#define STACK_SIZE_OTA          8192
// INA226 电流采样在电机任务内联完成，无独立任务
```

### 1.3 任务通信机制

```
┌────────────┐                ┌────────────┐
│ BLE Task   │ ──── xQueue ──▶│ Motor Task │
└────────────┘                └────────────┘
       │                            │
       ▼                            ▼
┌────────────┐                ┌────────────┐
│ Preferences│ ◀──── mutex ───│ Status Updater│
└────────────┘                └────────────┘
                                      │
                                      ▼
                              ┌────────────┐
                              │ Safety Task│
                              └────────────┘
```

> **Rev.2 新增任务**：`keypad_task`（4 键按键板，20ms 扫描 + 长按判定）与 `display_task`（~33Hz 刷新 LCD 状态屏）。按键板「长按 SET」触发 `motor_set_home()` 原点设置，「长按 ESC」触发深度睡眠关机。
>
> **UI 硬件抽象层（ui_hal，阶段0 引入）**：全中文 UI 状态机 `ui_screen.cpp` 不再直接依赖硬件，所有数据读取与动作都通过 `ui_hal_*` 接口。同一份 `ui_screen.cpp` 在 **PC 模拟器**（`ui_hal_sim.cpp` 后端）与 **真机固件**（`ui_hal_fw.cpp` 后端）下编译运行，彻底解决"模拟器 UI 与固件脱节"问题。模拟器端仅用 `mock_*` 提供演示数据；固件端 `ui_hal_fw.cpp` 把动作接到真实模块：大剂量→`motor_enqueue(MOTOR_CMD_BOLUS)`+历史+落盘、排气→`motor_enqueue(MOTOR_CMD_PRIME)`+状态+历史、报警清除→`pump_state_clear_alarm()`+历史、背光→`lcd_display_backlight()`、模式切换→更新 `loop_mode`+`storage_save_config()`。
>
> **基础率调度器（阶段3 引入）**：`basal_scheduler_task` 每 `BASAL_TICK_INTERVAL_MS`（3 分钟）计算当前应输注基础率并入队 `MOTOR_CMD_BASAL_TICK`。模式：本地档案（读 `g_pump_config` 当前方案整点 slot）/ 闭环（用 BLE 下发的 `current_basal_rate`，AAPS 接管）/ 暂停（0）；临时基础率（TBR）在有效期内优先。每次 tick 同步维护 `today_units_x100`、`total_units_x100_delivered`、储药器扣减与历史事件。



## 2. 状态机设计

### 2.1 顶层状态机

```
                    ┌─────────────┐
        power on ──▶│   BOOTING   │
                    └──────┬──────┘
                           │ init complete
                           ▼
                    ┌─────────────┐
                    │    IDLE     │◀────────────────┐
                    └──────┬──────┘                 │
                           │ command received       │
                           ▼                        │
                    ┌─────────────┐                 │
            ┌──────▶│  PRIMING    │                 │
            │       └──────┬──────┘                 │
            │              │ prime complete         │
            │              ▼                        │
            │       ┌─────────────┐                 │
            │       │  DELIVERING │◀────┐           │
            │       └──────┬──────┘     │           │
            │              │             │           │
            │       ┌──────┴──────┐      │           │
            │       ▼             ▼      │           │
            │  ┌─────────┐  ┌──────────┐  │           │
            │  │ BASAL   │  │  BOLUS   │  │           │
            │  │ mode    │  │  mode    │  │           │
            │  └────┬────┘  └────┬─────┘  │           │
            │       │            │        │           │
            │       └─────┬──────┘        │           │
            │             │               │           │
            │             ▼               │           │
            │       ┌─────────────┐       │           │
            └───────│  COMPLETE   │───────┘           │
                    └─────────────┘                   │
                                                      │
                    ┌─────────────┐                   │
                    │   ALARM     │  ◀────────────────┘
                    │ (任何异常)  │
                    └─────────────┘
```

### 2.2 报警状态

| 报警码 | 含义 | 处理 |
|--------|------|------|
| 0x01 | 电池电量低（< 20%） | 蜂鸣器间歇响，继续工作 |
| 0x02 | 电池电量极低（< 10%） | 蜂鸣器连续响，停止大剂量 |
| 0x03 | 储药器空 | 蜂鸣器 + 停止推注 |
| 0x04 | 阻塞 | 蜂鸣器 + 立即停止 |
| 0x05 | 电机故障（nFAULT） | 立即停止 + 重启 |
| 0x06 | 通信丢失（BLE） | 维持基础率 5 分钟 |
| 0x07 | 推注超时 | 强制停止 |
| 0x08 | 内部温度过高 | 暂停 5 分钟 |
| 0x0F | 电机丢步（INA226 电流异常） | 立即停止 + ALARM_STEP_LOSS |

> 完整 16 个报警码见 [`src/pump_types.h`](code/esp32_firmware/src/pump_types.h)：
> `ALARM_NONE`=0x00、`ALARM_BATTERY_LOW`=0x01 … `ALARM_NVS_ERROR`=0x0C、
> `ALARM_OTA_FAILED`=0x0D、`ALARM_PUMP_STALLED`=0x0E、`ALARM_STEP_LOSS`=0x0F。

---

## 3. 内存布局

```c
// 内存分配（按 ESP32-C6：512KB SRAM，部分型号支持外接 PSRAM）
// - BLE 协议栈：~80KB
// - GATT 服务：~10KB
// - 任务栈总和：~40KB
// - 历史记录缓冲区：~30KB
// - FreeRTOS 内核：~20KB
// - 余量：~220KB

#define HISTORY_BUFFER_SIZE     (30 * 1024)   // 30KB 历史记录
#define MAX_HISTORY_EVENTS      500           // 最多 500 条事件
#define MAX_BASAL_PROFILES      4             // 4 个基础率方案
#define MAX_BOLUS_PER_DAY       50            // 每天最多 50 次大剂量
```

---

## 4. 数据持久化（Preferences）

使用 Preferences（Arduino-ESP32 的 NVS 封装）保存，`storage.cpp` 通过
`putBytes/getBytes("pump_config", ...)` 读写整块结构体：

```c
// Preferences 键：pump_config
typedef struct {
    // 基础参数
    uint8_t  max_bolus_per_hour;     // 最大大剂量/小时（默认 25U）
    uint8_t  max_basal_per_hour;     // 最大基础率/小时（默认 5U/h）
    float    insulin_concentration;  // U-100 = 100
    uint8_t  ble_pin[6];             // BLE 配对 PIN

    // 当前设置
    uint8_t  active_profile;         // 当前基础率方案
    float    basal_schedule[24][2];   // 24 小时基础率（时间 + 速率）
    float    carb_ratio[24];         // 碳水比例
    float    isf[24];                // 胰岛素敏感系数
    uint16_t target_glucose[24];     // 目标血糖

    // 校准参数
    float    motor_steps_per_unit;   // 电机步数/U
    float    syringe_area_mm2;       // 储药器截面积
    float    lead_screw_pitch_mm;     // 丝杠导程
    uint16_t motor_microstep;        // 细分系数

    // 安全参数
    uint16_t max_pressure_kpa;       // 最大压力（默认 80kPa）
    uint16_t low_battery_threshold;  // 低电阈值
    uint8_t  watchdog_timeout_sec;   // 看门狗超时

    // 统计
    uint32_t total_units_delivered;  // 累计注射单位
    uint32_t total_runtime_hours;    // 累计运行小时
} pump_config_t;

// 命名空间：pump_state（运行时状态）
typedef struct {
    uint8_t   current_state;         // 状态机状态
    uint16_t  reservoir_units_left;  // 剩余药量（U）
    uint16_t  battery_mv;            // 当前电压（mV）
    uint8_t   battery_pct;           // 电池百分比
    uint16_t  last_glucose_mgdl;     // 最近血糖（来自 CGM）
    uint32_t  iob_x100;              // 体内活性胰岛素 × 100（U×100）
    uint32_t  last_bolus_time;       // 上次大剂量时间戳
    uint32_t  last_basal_time;       // 上次基础率时间戳
} pump_state_t;
```

> **阶段4 持久化增强**：
> - **配置**：`storage_save_config()` 在每次大剂量、模式切换、BLE 控制写后调用，写入 Preferences 命名空间 `olp_pump`。`storage_load_config()` 在开机加载时若发现当前活动基础率方案 24 个 slot 全为 0（防止历史存档把方案清零导致本地模式无输注），自动套用 `pump_config_apply_default_basal()` 的默认方案（profile 0 全天 0.5 U/h）。
> - **历史事件**：`history_log.cpp` 用内存环形缓冲（32 条）+ Preferences 命名空间 `olp_hist` 持久化，重启可恢复最近事件；新事件按 **60s 节流**落盘，避免高频 BLE/基础率事件频繁擦写 Flash。
> - **储药器扣减**：`pump_state_consume_units()` 带亚单位累加器，避免 0.05U 这类小数反复 floor 丢失精度；扣减到 0 触发 `ALARM_RESERVOIR_EMPTY`。

---

## 5. BLE GATT 服务设计

### 5.1 服务结构

```
固件 `ble_comm.cpp`（NimBLE-Arduino）实现一个自定义 **Pump Control Service**，
UUID 基为 `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`（与 Android APP 完全一致，见 README §5.3）：

```
┌────────────────────────────────────────────────────────────┐
│ Pump Control Service (128-bit, base 6E400001…CA9E)          │
│  - Bolus Command   (…0002, Write)        大剂量命令          │
│  - Basal Schedule  (…0003, Write)        闭环基础率 (AAPS 接管)│
│  - TBR Command     (…0004, Write)        临时基础率          │
│  - Pump Status     (…0005, Notify)       泵状态 (JSON)       │
│  - Insulin On Board(…0006, Notify)       IOB                 │
│  - Reservoir Level (…0007, Notify)       剩余药量            │
│  - CGM Write       (…0008, Write)        手机回传血糖        │
│  - Control         (…0009, Write+Read)   环模式/排气/清报警  │
└────────────────────────────────────────────────────────────┘
```

> 标准 Device Information Service (0x180A) / Battery Service (0x180F) 可选追加；
> 当前固件版本聚焦 Pump Control Service 的 8 个 characteristic。
```

### 5.2 命令协议（二进制格式，统一 CRC-8）

所有"写"命令在 payload 末字节追加 **CRC-8/CCITT（poly 0x07, init 0x00）**，
固件 `ble_comm.cpp` 用 `pkt_ok()` 校验长度与 CRC，校验失败直接丢弃。

**Bolus Command** (Write, 5 bytes)：
```
Offset  Size  Field           Description
0       4     units_x100      Bolus amount × 100 (e.g., 500 = 5.00U), little-endian
4       1     crc8            CRC-8 over bytes [0..3]
```

**Basal Command** (Write, 5 bytes)：闭环基础率（AAPS 接管时写入 `current_basal_rate`）
```
Offset  Size  Field           Description
0       4     rate_f32        Rate in U/h, float32 little-endian
4       1     crc8            CRC-8 over bytes [0..3]
```

**TBR Command** (Write, 7 bytes)：临时基础率
```
Offset  Size  Field           Description
0       1     percent         TBR 百分比 (e.g. 50 = 50%)
1       2     rate_x100       TBR 绝对速率 × 100 (U/h × 100), uint16 LE
3       2     duration_min    TBR 持续分钟, uint16 LE
5       1     crc8            CRC-8 over bytes [0..4]
```

**CGM Write** (Write, 5 bytes)：手机/AAPS 回传血糖（泵无独立 CGM 采集时由手机转发）
```
Offset  Size  Field           Description
0       2     mgdl            Blood glucose in mg/dL, uint16 LE
2       1     trend           CGM 趋势箭头: -1 降 / 0 平 / +1 升
3       1     crc8            CRC-8 over bytes [0..2]
```

**Control Command** (Write, 2 bytes)：环模式 / 远程动作
```
Offset  Size  Field           Description
0       1     payload         0/1/2 = 设 loop_mode(闭环/本地/暂停); 0x10 = 远程排气; 0x11 = 远程清报警
1       1     crc8            CRC-8 over byte [0]
Read 时回读当前 loop_mode (1 byte)。
```

**Pump Status** (Read+Notify, JSON, 1Hz)：
```
{"bat":<pct>,"st":<state>,"alm":<code>,"glu":<mgdl>,"tr":<trend>,"loop":<mode>,"tbr":<percent>}
```


### 5.3 安全

- BLE Just Works 配对（无 PIN）— 默认
- BLE Passkey 配对（6 位 PIN）— 推荐
- 所有 GATT 写操作需要 AES-128-CCM 加密
- BLE 连接超时：30 秒无通信 → 维持基础率但报警

---

## 6. FreeRTOS 任务伪代码

### 6.1 BLE GATT 任务

```cpp
void ble_task(void* param) {
    // NimBLE-Arduino 初始化
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCb());

    NimBLEService* pSvc = pServer->createService(pumpServiceUUID);
    // 创建 6 个 characteristic：bolus/basal/tbr 可写, status/iob/reservoir 可通知
    pSvc->start();
    NimBLEDevice::startAdvertising();

    while (1) {
        if (pServer->getConnectedCount() > 0) {
            // 每 1s 打包 g_pump_state 经 Notify 上行
            notify_status();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### 6.2 电机控制任务

```cpp
void motor_task(void* param) {
    motor_command_t cmd;
    TickType_t last_basal_time = 0;
    TickType_t last_wakeup = xTaskGetTickCount();

    while (1) {
        // 等待 100ms 或命令
        if (xQueueReceive(motor_queue, &cmd, pdMS_TO_TICKS(100))) {
            // 处理命令
            switch (cmd.type) {
                case CMD_BOLUS:
                    deliver_bolus(cmd.units_x100);
                    break;
                case CMD_BASAL_TICK:
                    deliver_basal_step();
                    break;
                case CMD_PRIME:
                    prime(cmd.volume);
                    break;
                case CMD_STOP:
                    emergency_stop();
                    break;
                case CMD_REWIND:
                    rewind(cmd.steps);
                    break;
            }
        }

        // 每 3 分钟执行一次基础率检查
        TickType_t now = xTaskGetTickCount();
        if (now - last_basal_time >= pdMS_TO_TICKS(180000)) {
            // 根据当前时间和基础率方案计算下一步推注量
            float rate = get_current_basal_rate();
            uint32_t steps = units_to_microsteps(rate / 20.0f);  // 0.05U, 经唯一换算入口
            motor_command_t basal_cmd = {CMD_BASAL_TICK, steps, 0, 0};
            // 直接执行（不通过队列）
            deliver_motor_steps(steps, MOTOR_FORWARD);
            last_basal_time = now;
        }

        // 喂狗
        motor_task_wdt_feed();

        // 让出 CPU
        taskYIELD();
    }
}
```

### 6.3 安全监控任务（最高优先级）

```cpp
void safety_task(void* param) {
    TickType_t last_check = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // 1. 电池监测（3S 锂电池，阈值见 config.h）
        uint16_t battery_mv = read_battery_voltage();
        if (battery_mv < 9000) {  // < 9.0V 极低
            trigger_alarm(ALARM_BATTERY_CRITICAL);
        } else if (battery_mv < 9600) {  // < 9.6V 低
            trigger_alarm(ALARM_BATTERY_LOW);
        }

        // 1b. 电机丢步监护（INA226 电流）
        if (motor_step_loss_detected()) {
            trigger_alarm(ALARM_STEP_LOSS);
            emergency_stop();
        }

        // 2. 储药器监测
        uint16_t reservoir = get_reservoir_units();
        if (reservoir == 0) {
            trigger_alarm(ALARM_RESERVOIR_EMPTY);
        }

        // 3. 限位开关
        if (digitalRead(LIMIT_FWD_PIN) == LOW) {
            emergency_stop();
            trigger_alarm(ALARM_OVERLOAD);
        }

        // 4. BLE 连接监测
        if (!ble_is_connected() && (now - last_ble_event) > pdMS_TO_TICKS(300000)) {
            trigger_alarm(ALARM_COMM_LOST);
        }

        // 5. DRV8825 故障
        if (digitalRead(FAULT_PIN) == LOW) {
            trigger_alarm(ALARM_MOTOR_FAULT);
            emergency_stop();
        }

        // 6. 温度监测
        float temp = temperatureRead();
        if (temp > 60.0) {
            trigger_alarm(ALARM_OVER_TEMP);
            pause_pump(300);  // 暂停 5 分钟
        }

        // 喂狗
        safety_task_wdt_feed();

        vTaskDelay(pdMS_TO_TICKS(1000));  // 1 秒检查周期
    }
}
```

---

## 6.1 步进电机控制精度与剂量换算

### 6.1.1 关键参数（已用真实几何推导 + 程序验证）

| 参数 | 值 | 说明 |
|------|----|----|
| 丝杠导程 | 0.5 mm/转 | 电机转 1 圈推动活塞前进 0.5mm |
| 步进角 | 1.8°（200 步/转） | 全步 |
| 细分 | 1/32（硬件拉高 M0/M1/M2） | 固定 |
| **有效微步/转** | **6400** | 200 × 32 |
| 储药器（3ml 注射器型）内腔直径 | 8.65 mm | 截面积 ≈ 58.8 mm² |
| 每转排开体积 | 29.38 µL | 面积 × 导程 |
| U-100 浓度 | 100 U / mL | 1U = 10µL |
| **微步/单位 (STEPS_PER_UNIT)** | **≈ 2178** | 6400 / 2.938U |
| **每微步对应** | **≈ 0.000459 U (0.00459 µL)** | 远细于 0.05U 最小精度 |
| **0.05U 微步数** | **109 微步**（理论 108.9，取整 109） | 全系统最小剂量网格 |

> 结论：**打 0.05U = 109 微步**，实际剂量 0.05006U（误差 +0.12%，远低于医疗泵 ±15% 要求）。
> 机械分辨率 0.000459U 比 0.05U 精细约 109 倍，因此「控制精度」绰绰有余；真正决定
> 「绝对精度」的是丝杠导程与笔芯内径的**实测值**——见 6.1.3 标定系数。
>
> （下表为当前 `RESERVOIR_TYPE = RESERVOIR_TYPE_CY13_DANA` 的取值；切换储药罐类型后
> 由 `dosing.h` 自动重算，无需手工改动任何数字。）

### 6.1.2 单一真源：储药罐类型 + 换算模块（防精度漂移）

- **储药罐类型在 `config.h` 用 `RESERVOIR_TYPE` 唯一选择**（如 `RESERVOIR_TYPE_CY13_DANA` / `RESERVOIR_TYPE_CARTRIDGE_3ML`）。切换耗材只改这一个宏，几何与换算全自动重算，绝不允许多处硬编码。
- **全部「单位(U)↔微步」换算算法与几何推导集中在 `dosing.h`（单一真源）**：它仅从「内腔直径」推导截面积 / 每转体积 / `STEPS_PER_UNIT` / `STEPS_PER_005U`，并定义以下三函数。固件与模拟器**共用同一份** `dosing.h`，杜绝算法双份。
- 全系统（大剂量 / 基础率 / 排气）禁止任何模块各自拿 `STEPS_PER_UNIT` 现算，必须统一走：

```c
// dosing.h — 单位(U) ↔ 微步 唯一换算 (static inline, 单一真源)
uint32_t units_to_microsteps(float units);   // 四舍五入, 误差 < 1 微步
float    microsteps_to_units(uint32_t steps);
float    quantize_units_005(float units);    // 吸附到 0.05U 网格
```

### 6.1.3 剂量标定系数 `DOSE_CALIBRATION`

实测硬件与标称几何存在制造偏差。上电后用量筒/天平标定，修正该系数（默认 1.0）：

```
实测打出 0.05U 实际为 0.051U  →  DOSE_CALIBRATION = 0.051 / 0.05 = 1.02
```

该系数作用于 `units_to_microsteps()`，全系统剂量随之整体缩放，无需改多处常量。

## 6.2 大剂量分段打入（Segmented Bolus）—— 对标真实胰岛素泵

### 6.2.1 为什么不能一次连续打完

搜证主流胰岛素泵给出的一致做法（非一次性连续推注，而是小步推进 + 段间停顿）：

- **Wellion MICRO-PUMP**：Bolus Steps **0.05U/步**，Volume per Step 0.5µL，**段间间隔 1s**，
  Infusion Speed **3 U/min**；阻塞时平均已打约 2.5U 才报警。
- **Medtronic MiniMed 780G**：Bolus Speed 标准 **1.5 U/min**、快速 **15 U/min**；
  编程步进 0.025 / 0.05 / 0.1U；Max bolus 25U。
- **通用安全逻辑**：段间复检压力/电流（阻塞）、报警、储药器空；用户可中途取消，
  只损失已打部分——这是防止「过量输注」的关键工程手段。

### 6.2.2 本固件实现（`motor_controller.cpp`）

- 大剂量不再由 `motor_move_sync()` 一次跑完，而是 `motor_deliver_bolus()` 循环：
  - 每批 **`BOLUS_SEGMENT_UNITS` = 0.05U（109 微步）**；
  - 段间 `vTaskDelay(BOLUS_SEGMENT_INTERVAL_MS=1000)` 停顿 ≈ **3 U/min**（贴合 Wellion）；
  - 每段前复检 `alarm_code` / 丢步 / `reservoir_units_left < 1`，异常即中止剩余；
  - 阻塞监测由 INA226 电流（`motor_stall_guard_tick()`）在每步内标记；
  - **整个大剂量期间保持电机 ENABLE**，避免段间输液回压把活塞推回（回灌）；
  - 按「实际打入量」逐段记账（储药器 / 今日 / 累计 / IOB），取消时只扣已打部分；
  - 完成/中止后写一条 `EVENT_TYPE_BOLUS` 历史并 `storage_save_config()` 持久化累计。
- 新增 `MOTOR_CMD_CANCEL_BOLUS` 与 `motor_cancel_bolus()`：UI 在**任何页面按 ESC** 即可
  取消正在打入的大剂量（首页底部显示「大剂量注射中… (按 ESC 取消)」）；BLE 控制通道亦可触发。
- 基础率（0.5U/h → 每 3 分钟仅 ~0.025U ≈ 201 微步）本身已极慢，等价于「自然分段」，无需改动。

### 6.2.3 方波 / 双波大剂量：按时间维铺开（Extended Bolus Time-Spread）

- **入口**：`ui_hal_deliver_bolus()`（固件后端 `ui_hal_fw.cpp`）把「立即量」作为一次性
  `MOTOR_CMD_BOLUS` 入队；把「延展量」交给 `basal_scheduler_start_extended_bolus()`，
  **不再作为第二条一次性大剂量入队**。
- **驱动**：`basal_scheduler_task()` 每 `BASAL_TICK_INTERVAL_MS`（3 分钟）调用
  `extended_bolus_tick()`，按「已过时间比例」计算本 tick 应铺开的量
  `target = total × (elapsed / duration_ms)`，与已铺开量之差即本 tick 投递量。
- **电机路径**：每个 tick 的微投递封装为 `MOTOR_CMD_BOLUS_EXT`，仍经 `execute_command()`
  这一全系统唯一电机入口，换算统一走 `dosing.h`；记账（储药器/今日/累计/IOB）在入队时同步完成，
  与 `motor_deliver_bolus()` 的分段记账一致——中途取消或储药器空只损失未铺开部分。
- **收尾**：时间到（`frac ≥ 1`）投递剩余零头，并写**一条** `EVENT_TYPE_BOLUS` 历史事件
  （与「立即量」分开记，双波 = 立即 + 延展两条事件），`total_bolus_count++` 持久化。
- **取消 / 安全**：`ui_hal_cancel_bolus()` 同时取消立即量（`motor_cancel_bolus()`）与延展量
  （`basal_scheduler_cancel_extended_bolus()`）；`ui_hal_bolus_active()` 在任一进行中均返回真。
  延展量投递前复检 `reservoir_units_left < 1`，空则中止并记为部分事件。
- **退化**：`duration_h ≤ 0` 时退化为一次性大剂量（与原行为一致），不进 EXT 路径。
- 状态暴露于 `g_pump_state.ext_bolus_*`（活动标志 / 类型 / 总量 / 已铺开 / 时长 / 起始），
  UI 与 BLE 可据此显示「方波注射中」进度。

---

## 7. 校准流程

### 7.1 初次上电校准

1. **空白注射器校准**（零位）：
   - 推杆完全后退到限位开关
   - 记录当前位置为"零点"

2. **充满注射器**：
   - 用户安装新笔芯
   - APP 触发"换药"流程
   - 推杆前进到限位（前端）
   - 记录当前位置
   - 计算总行程对应的药量（笔芯容量）

3. **精度验证**：
   - 推注 0.05U 若干次
   - 用天平称量验证

### 7.2 定期校准

- 每次更换笔芯后自动重新校准
- 每月一次精度自检

---

## 8. OTA 升级

### 8.1 升级流程

```
┌────────┐                      ┌────────┐
│  Server│ ── HTTPS GET ──────▶ │  Pump  │
│        │     firmware.bin     │        │
│        │ ◀── 200 OK ─────────│        │
│        │                      │        │
│        │                      │ 校验 SHA-256
│        │                      │ 写 OTA 分区
│        │                      │ 校验新分区
│        │                      │ 切换启动
└────────┘                      └────────┘
```

### 8.2 安全要求

- HTTPS + 证书验证
- 固件签名验证（ECDSA P-256）
- 防回滚（版本号必须递增）

---

## 9. 开发与调试工具

| 工具 | 用途 |
|------|------|
| Arduino IDE 2.x + ESP32 Board Manager 3.x | 主开发框架 |
| Arduino 串口监视器 / minicom | 串口日志监控 |
| ESP32 Flash Download Tool / Arduino 上传 | 固件烧录 |
| nRF Connect（手机 APP） | BLE 调试 |
| Wireshark + BT HCI snoop | BLE 协议分析 |
| Logic Analyzer | GPIO 时序分析（STEP/DIR） |
| 示波器 | 电机信号、PWM、VREF |

---

## 10. Rev.2 新增模块：INA226 监护与 4 键按键板

### 10.1 INA226 电流/电压监护

`ina226.cpp` 经 I2C（SDA=GPIO18, SCL=GPIO19, 400kHz）读取总线电压（1.25mV/LSB）、
分流电流（125µA/LSB）与功率。校准寄存器 = 2048（20mΩ 分流，量程 4.096A）。
电机运动期间每 5ms 采样一次，用于：

- 丢步检测：电流 < 80mA 视为未带载/丢步 → `ALARM_STEP_LOSS`
- 堵转检测：电流 > 700mA 视为机械阻塞 → 停止推注
- 过流检测：总电流 > 1A → `ALARM_OVER_CURRENT`

接入位置：`motor_controller.cpp` 的 `motor_move_sync()` 内联调用
`motor_start_stall_guard() / motor_stall_guard_tick() / motor_stop_stall_guard()`
（运动期间每 5ms 采样 INA226），由 `safety_monitor.cpp` 调用
`motor_step_loss_detected()` 判定丢步。

### 10.2 4 键按键板

`keypad.cpp` 扫描 GPIO20/23/4/5（上/下/确认/取消，复用 TF 卡引脚，内部上拉，20ms 周期，
消抖 2 拍，长按 150 拍 = 1.5s）：

- 短按 UP/DOWN/SET/ESC：经 `ui_screen_key()` 驱动全中文 UI 状态机导航（上/下移动、确认进入/确认、返回），**不再直接 jog 电机**（动作由 UI 经 `ui_hal_*` 后端落到真实模块）
- 长按 SET：保存原点（`motor_set_home()`）
- 长按 ESC：进入深度睡眠关机（`power_manager` 深度睡眠，GPIO5 即 PIN_KEY_ESC 作唤醒源）

> 线程安全：LVGL 对象只在 `display_task`（`ui_screen_refresh` / 按钮回调）中创建/销毁；`keypad_task` 仅通过 `ui_screen_key()` 修改纯状态变量（`s_screen`/`s_sel`/`s_dose` 等）与调用 `ui_hal_*`（均不触碰 LVGL），因此不存在跨任务并发访问 LVGL 的竞争。

按键事件经 `g_key_queue` 队列上报主循环与 `motor_task`。