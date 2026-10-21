/**
 * config.h — ESP32-C6 引脚分配与系统常量 (Arduino 框架)
 *
 * OpenLoop Insulin Pump Firmware (Rev.2 — 3S / 5V 体系)
 * 硬件: Waveshare ESP32-C6-LCD-1.47 + DRV8825 + SM2012 + INA226
 *
 * ⚠️ 开发板: Waveshare ESP32-C6-LCD-1.47 (SKU: 28563)
 *    官方资料: https://docs.waveshare.net/ESP32-C6-LCD-1.47/
 *    LCD 引脚以官方文档为准, 不可更改!
 *
 * 框架: Arduino IDE 2.x + ESP32 Board Manager 3.x
 *       + Arduino_GFX (GFX_Library_for_Arduino) + LVGL 9.5.0 + NimBLE-Arduino
 *
 * 电源树 (Rev.2):
 *   11.1V 3S 锂电池 (SM20 接口) ── 104 + 100µF/16V 滤波
 *        ├─▶ INA226 电流/电压采集 (监测电机丢步/异常/原点)
 *        ├─▶ DRV8825 VMOT  (电机功率, 直接 11.1V)
 *        └─▶ DC-DC 降压 11.1V→5V ──▶ ESP32-C6 / LCD / INA226 / 按键板
 *                                       └─▶ AMS1117-3.3 ──▶ DRV8825 VDD
 *
 * ===== Waveshare ESP32-C6-LCD-1.47 板载资源占用 =====
 *   GPIO  | 功能                | 可否复用
 *   ------|---------------------|----------
 *     1   | USB D+              | ❌ 保留 USB
 *     6   | LCD_MOSI (SPI)      | ❌ 固定
 *     7   | LCD_SCLK (SPI)      | ❌ 固定
 *     8   | RGB_Control(WS2812) | ⚠️ 板载彩灯, 用 rgbLedWriteOrdered 驱动
 *    12   | USB_N (D-)          | ❌ 保留 USB
 *    13   | USB_P (D+)          | ❌ 保留 USB
 *    14   | LCD_CS              | ❌ 固定
 *    15   | LCD_DC              | ❌ 固定
 *    21   | LCD_RST             | ❌ 固定
 *    22   | LCD_BL              | ❌ 固定
 *     4   | TF Card CS          | ✅ 不插 TF 卡即可用
 *     5   | TF Card MISO        | ✅ 不插 TF 卡即可用
 *
 *   ⚠️ I2C 注意: Arduino 默认 Wire 引脚为 GPIO21/22, 但已被 LCD_RST/BL 占用!
 *      INA226 必须显式 Wire.begin(18, 19) 指定 SDA/SCL, 不可用默认。
 *
 * ===== 工程取舍 =====
 *   - DRV8825 的 M0/M1/M2 细分 & nSLEEP 硬件焊死/拉高, 不占 GPIO
 *   - nRESET 硬件 10kΩ 上拉到 3.3V, 不接 MCU (省 1 GPIO)
 *   - DC-DC 使能可硬连线常开; 若需软件控制占用 GPIO17 (UART_RX)
 *   - GPIO2/3 为 strapping pins, 用作限位开关(内部上拉输入)需确保
 *     外部电路不在启动时强拉低电平
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// ============================================================
// 0. LCD 引脚 (Waveshare ESP32-C6-LCD-1.47 固定, 不可更改!)
// ============================================================
// 控制器: ST7789, 分辨率: 172×320, 接口: SPI (4-wire, 无 MISO)
// 来源: https://docs.waveshare.net/ESP32-C6-LCD-1.47/ "接口介绍"
#define PIN_LCD_SCK        7     // SPI Clock
#define PIN_LCD_MOSI       6     // SPI MOSI (Data)
#define PIN_LCD_CS         14    // SPI Chip Select
#define PIN_LCD_DC         15    // Data/Command
#define PIN_LCD_RST        21    // Hardware Reset
#define PIN_LCD_BL         22    // Backlight (LEDC PWM 调光)

#define LCD_H_RES          172
#define LCD_V_RES          320
#define LCD_X_GAP          34    // 172 宽 ST7789 必需列地址偏移, 否则画面错位
#define LCD_Y_GAP          0
#define LCD_SPI_FREQ_HZ    (40 * 1000 * 1000)
#define LCD_ROTATION       3     // landscape

// ============================================================
// 1. DRV8825 步进电机驱动
// ============================================================
// M0/M1/M2 细分: 由实际焊死的硬件电平决定, 必须与下方 MOTOR_MICROSTEPS 完全一致!
//   DRV8825 真值表(M0,M1,M2): L,L,L=1  H,L,L=1/2  L,H,L=1/4  H,H,L=1/8
//                           L,L,H=1/16  H,L,H=1/32  L,H,H=1/32  H,H,H=1/32
//   当前 PCB 设计目标 = H/H/H → 1/32 微步 (不占 GPIO)。
//   ⚠️ 若实际焊的是 H/H/L(=1/8) 而 MOTOR_MICROSTEPS 仍为 32, 每个脉冲实际走 4 倍距离
//      → 给药量被放大 4 倍! 必须改 MOTOR_MICROSTEPS=8 与之匹配(或重新焊成 H/H/H)。
// nSLEEP:        硬件拉高 3.3V 常唤醒 (不占 GPIO)
// nRESET:        硬件 10kΩ 上拉 3.3V (不占 GPIO)
// → 电机仅占 STEP / DIR / ENABLE / nFAULT = 4 引脚 (本板实际空 pad: 9/12/13 + 可选 3)
#define PIN_MOTOR_STEP     9     // STEP 脉冲 (定时器 ISR 翻转)
#define PIN_MOTOR_DIR      12    // DIR 方向 (原 GPIO10, 改接空 pad 12)
#define PIN_MOTOR_ENABLE   13    // ENABLE (低有效) (原 GPIO11, 改接空 pad 13)
#define PIN_MOTOR_nFAULT   16    // FAULT 检测 (输入, 低有效). 可选改接空 pad 3

// ============================================================
// 2. 4 键按键板 (上/下/确认/取消, 内部上拉, 低有效)
// ============================================================
// GPIO4/5 复用 TF 卡引脚 — 不插 TF 卡时完全可用作普通 GPIO
#define PIN_KEY_UP         20
#define PIN_KEY_DOWN       23
#define PIN_KEY_SET        4     // 原 TF_CS (不用TF卡)
#define PIN_KEY_ESC        5     // 原 TF_MISO (不用TF卡)

// ============================================================
// 3. INA226 电流/电压监测 (I2C)
// ============================================================
// ⚠️ 必须 Wire.begin(18, 19), 不可用 Arduino 默认 I2C 引脚 (被 LCD 占用)
#define PIN_INA226_SDA     18
#define PIN_INA226_SCL     19
#define I2C_FREQ_HZ        400000    // I2C Fast Mode

// ============================================================
// 4. 限位开关 (内部上拉, 低有效触发)
// ============================================================
// GPIO2/3 为 strapping pins — 作为输入(内部上拉)一般安全,
// 但需确保外部电路不在启动阶段强拉低
// ⚠️ 本项目未焊接硬件限位开关。ESP32-C6 的 GPIO2/GPIO3 正是 USB-D+/D-（本项目编译开
//    CDCOnBoot=cdc, USB-CDC 占用此二脚），不可用作限位输入。因此原 PIN_LIMIT_FWD/REV 的
//   读取读到的是 USB 差分信号（垃圾值），已于 2026-08-06 移除。
//    限位判定改为 INA226 堵转电流检测：电机顶到机械限位(前/后)或管路堵塞 → 电流持续超阈值
//    → g_occlusion 置位（见 motor_controller.cpp::motor_stall_guard_tick / motor_pulse）。
//   软件兜底：正向 RESERVOIR_MAX_STEPS、反向 REWIND_MAX_STEPS 仍以步数封顶防超行程顶死。

// ============================================================
// 5. 蜂鸣器
// ============================================================
#define PIN_BUZZER         0     // PWM 输出 (LEDC)
#define BUZZER_FREQ_HZ     2400

// ============================================================
// 6. 状态 LED (板载 WS2812 彩灯)
// ============================================================
// GPIO8 是 WS2812 单线 RGB 灯珠 (非普通 GPIO), 必须用
// rgbLedWriteOrdered(8, LED_COLOR_ORDER_RGB, r, g, b) 驱动
#define PIN_LED_STATUS     8     // 板载 WS2812

// ============================================================
// 7. DC-DC 降压使能 (可选)
// ============================================================
// 若 DC-DC 模块有 EN 引脚且需软件控制低功耗关断:
//   GPIO17 默认为 UART_RX, 不用串口接收时可复用
//   若模块无 EN 引脚或硬连线常开, 此定义留空不使用
#define PIN_EN_5V_BUCK     17    // 可选; 不用时悬空

// ============================================================
// 8. 电机参数
// ============================================================
#define MOTOR_STEPS_PER_REV    20      // 每转步数 (SM2012 步距角 18°/步 ⇒ 20 步/转, 见电机规格书 STEP ANGLE 18°/STEP)
#define MOTOR_MICROSTEPS       32      // 固定 1/32 细分 (M0/M1/M2 硬件拉高)
#define MOTOR_EFFECTIVE_STEPS  (MOTOR_STEPS_PER_REV * MOTOR_MICROSTEPS)
                                        // = 640 微步/转 (20步/转 × 1/32)

#define LEAD_SCREW_PITCH_MM   0.5f    // 丝杠导程 mm — 实测确认: M3 × 0.5P 丝杆, 单线导程=0.5mm (与代码一致)

// ============================================================
// 8.1 储药罐类型 (★唯一选择点★: 改这一处即可切换耗材, 几何/换算全自动重算)
//     换算系数绝不在别处手算硬编码 — 全部由 dosing.h 从「内腔直径」单一推导。
// ============================================================
#define RESERVOIR_TYPE_CY13_DANA     1   // CY-13(PHRay)/丹纳原装 3mL 注射器型储药器, 内腔 Φ11.38mm; 用户据产品说明页+丹纳图纸对标核实(满筒3mL↔29.6mm行程自洽)
#define RESERVOIR_TYPE_CARTRIDGE_3ML 2   // 3mL 卡式瓶(笔芯), 内腔 Φ9.65mm
#define RESERVOIR_TYPE               RESERVOIR_TYPE_CY13_DANA   // ← 当前采用

#if   RESERVOIR_TYPE == RESERVOIR_TYPE_CY13_DANA
  #define SYRINGE_DIAMETER_MM   11.38f
  #define RESERVOIR_CAPACITY_U  300
  #define RESERVOIR_TYPE_NAME   "CY-13(PHRay) 3mL 储药器 Φ11.38"
#elif RESERVOIR_TYPE == RESERVOIR_TYPE_CARTRIDGE_3ML
  #define SYRINGE_DIAMETER_MM   9.65f
  #define RESERVOIR_CAPACITY_U  300
  #define RESERVOIR_TYPE_NAME   "3mL 卡式瓶(笔芯)"
#else
  #error "RESERVOIR_TYPE 未选择有效储药罐类型 (见 config.h §8.1)"
#endif

// ---- 剂量网格 / 标定 (全系统) ----
#define MIN_DOSE_UNITS        0.1f    // 最小可靠给药剂量 (U) — 全系统剂量网格 / 单剂量下限。
//   硬件实际精度 ±0.1~0.3U(丝杆背隙为主), 0.1U 为安全可靠下限 (与丹纳原厂大剂量增量一致)。
//   <0.1U 的细量一律不提供输入入口, 仅由内部微步累加器消化 (见 dosing.h 剂量诚实性原则)。
// 剂量标定系数: 实际硬件导程/储药罐内径与标称存在制造偏差, 实测后修正。默认 1.0 (未标定)。
// 该系数作用于唯一换算入口 units_to_microsteps() (见 dosing.h), 全系统剂量随之整体缩放。
#define DOSE_CALIBRATION      1.0f

// ============================================================
// 9. 剂量换算单一真源 (dosing.h)
//     所有「单位(U)↔微步」换算与几何推导都集中在 dosing.h, 由本文件末尾 #include 引入。
//     任何模块禁止自行用 STEPS_PER_UNIT 现算, 统一调用 units_to_microsteps() 等三函数。
// ============================================================



// ---- 大剂量分批打入 (segmented bolus) ----
// 大剂量采用「梯形速度曲线」(循序渐进: 加速 → 匀速 → 减速), 兼顾提速与收尾精度:
//  · 粒度(安全复检/记账精细度)按总量分级 —— ≤1U 全程 0.1U 最细; 1~5U 中段 0.5U; >5U 中段 1.0U。
//  · 末段(剩余≤0.5U 或已进入减速区)恒为 0.1U 最细步 + 缓速, 保证收尾精度。
//  · 步进频率随进度在 600Hz(头尾)↔1800Hz(中段) 间梯形过渡; 段间仅 80ms 复检窗口。
// 剂量精度网格仍是 MIN_DOSE_UNITS(0.1U); 此处仅推送节奏/曲线参数, 可按电机特性微调。
#define BOLUS_GRAN_FINE          0.1f   // 最细粒度: ≤1U 全程 及 末段收尾 (= MIN_DOSE_UNITS 剂量下限)
#define BOLUS_GRAN_MID           0.5f   // 1~5U 中段粒度
#define BOLUS_GRAN_COARSE        1.0f   // >5U 中段粒度
#define BOLUS_TIER1_MAX_UNITS    1.0f   // 总量 ≤1U → 全程最细
#define BOLUS_TIER2_MAX_UNITS    5.0f   // 1U<总量≤5U → 中粒度; >5U → 粗粒度
#define BOLUS_TAIL_UNITS         0.5f   // 末段细步区(绝对): 剩余≤0.5U 强制最细
#define BOLUS_RAMP_UP_FRAC       0.20f  // 前 20% 加速到满速
#define BOLUS_RAMP_DOWN_FRAC     0.22f  // 后 22% 减速(同时进入细步)
#define BOLUS_FAST_HZ            1800   // 中段最高步进频率
#define BOLUS_SLOW_HZ            600    // 头尾缓速频率(>MOTOR_MIN_SPEED_HZ, 保证不失步)
#define BOLUS_SEGMENT_INTERVAL_MS 80    // 段间短暂停顿(复检窗口, 非限速); 调试构建=0
#define MOTOR_MAX_SPEED_HZ    5000
#define MOTOR_MIN_SPEED_HZ    500
#define MOTOR_ACCEL_HZ        2000
#define MOTOR_PULSE_WIDTH_US  50
#define MANUAL_JOG_STEPS      10      // 手动原点设置时每次微动步数

// 回退装药(退到尾部)参数: 反向连续走, 直到后限位开关命中或达安全步数上限。
// 不依赖"已打药量"记账(用户要求"不根据打了多少药计算退多少距离")。
#define REWIND_SPEED_HZ       3000    // 回退速度(空走无药液阻力, 可较快; 远低于 MOTOR_MAX=5000 防丢步)
#define REWIND_CHUNK_STEPS    1000    // 每批步数(小批以便及时检测限位/喂狗)
// 退药/行程安全上限: 储药器 CY-13 3mL(U-100)=300U, 实际行程≈300×STEPS_PER_UNIT(125.84)≈37752 步。
// 取满容量 ×1.15≈45000 步作为"机械限位开关失效时"的兜底自停, 防丝杆顶死。
// ⚠️ 旧值 700000 注释误算为≈321U, 实际≈5563U, 远超储药器行程, 会无限位时疯狂超行程顶死 —— 已修正。
// 若修改 SYRINGE_DIAMETER_MM / STEPS_PER_UNIT, 此值需同步按 300U×新STEPS_PER_UNIT×1.15 调整。
#define REWIND_MAX_STEPS      45000
#define RESERVOIR_MAX_STEPS   45000   // 储药器满容量行程硬上限(供正向运动边界兜底, 与 REWIND_MAX_STEPS 同义)

// DRV8825 电流设置 (I_FS = VREF / (8 * R_SENSE), R_SENSE=0.1Ω)
#define MOTOR_CURRENT_MA      500
#define MOTOR_VREF_MV         400

// ============================================================
// 9. INA226 电流/电压监测参数
// ============================================================
// I2C 地址: A0=A1=GND → 0x40
#define INA226_I2C_ADDR       0x40

// 分流电阻: 20mΩ → 量程 4.096A, 分辨率 125µA/LSB
#define INA226_SHUNT_OHM      0.02f
#define INA226_CURRENT_LSB_A  0.000125f   // 4.096A / 32768 = 125µA
#define INA226_CAL_VALUE      2048        // 0.00512 / (0.000125 * 0.02)

// 电机运行电流基线 (mA) — 由首次校准获得, 用于丢步/阻塞判定
#define MOTOR_RUN_CURRENT_MA  280     // 自由运行稳态电流估计
#define STALL_NOLOAD_MA       80      // 低于此值视为未带动负载/丢步
#define STALL_OVERLOAD_MA     700     // 高于此值视为堵转/阻塞
#define STALL_SAMPLE_MS       5       // 运动期间电流采样间隔
#define STALL_OCCL_CONSEC     5       // 连续 5 个采样(≈25ms)超阈值才判堵转, 滤除启动浪涌/瞬态尖峰
#define STALL_NOLOAD_CONSEC   5       // 连续 5 个采样低于空载阈值才判丢步(去抖)

// ============================================================
// 10. 电池参数 (3S 锂电池)
// ============================================================
#define BATTERY_CELLS         3
#define BATTERY_NOMINAL_MV    11100   // 3S 标称 11.1V
#define BATTERY_FULL_MV       12600   // 满电 12.6V (4.2V × 3)
#define BATTERY_LOW_MV        9600    // 低电 9.6V (3.2V × 3)
#define BATTERY_CRITICAL_MV   9000    // 极低 9.0V (3.0V × 3)
#define BATTERY_CUTOFF_MV     8400    // 关机保护 8.4V (2.8V × 3)

// ============================================================
// 11. 胰岛素参数
// ============================================================
#define INSULIN_CONCENTRATION  100
#define MAX_BOLUS_UNITS        25.0f
#define MAX_BASAL_RATE         5.0f
#define BASAL_TICK_INTERVAL_MS 180000
#define MAX_RESERVOIR_UNITS    RESERVOIR_CAPACITY_U
#define IOB_DURATION_HOURS     4.0f

// ---- 方波/双波延展量: 连续慢滴窗口 ----
// 真实胰岛素泵的方波/延展量是「在 duration 内匀速输注」, 机械上拆成微步(Wellion:
// 步间 ~1s), 而非几分钟一跳。本固件据此把延展量改为细拍连续慢滴:
//   · 每 EXT_BOLUS_WINDOW_MS 计算一次「按方波速率匀速走丝杠」的微投递, 使电机在该
//     窗口内连续运行(而非用 BOLUS_SPEED_HZ 快打后歇几分钟), 贴合真实泵行为。
//   · 该窗口同时作为调度器细拍; 基础率仍按 BASAL_TICK_INTERVAL_MS(3 分钟)窗口投递。
//   · 调小窗口(如 5000~10000)更平滑但队列流量增大; 调大则更省电但连续性略降。
#define EXT_BOLUS_WINDOW_MS   15000   // 细节拍 / 连续慢滴窗口 (ms)
#define EXT_BOLUS_MIN_UNITS   0.005f  // 延展量单次投递下限 (U), 远低于 0.1U 网格以保连续, 防极小量空转

// ---- 基础率速率覆盖 / 执行留痕 (2026-08-08) ----
// BASAL_OVERRIDE_TIMEOUT_MS:
//   伴生 App 通过 BASAL 通道直推的瞬时速率只在此时长内有效, 超时自动回落到
//   g_pump_config.profiles[active] 档案。防止 App 断连后泵被永久钉死在旧速率。
#define BASAL_OVERRIDE_TIMEOUT_MS   (30UL * 60UL * 1000UL)   // 30 分钟
// BASAL_HISTORY_AGG_MS:
//   基础率每 3 分钟投递一小段, 若每段都写 32 条环形历史会瞬间把大剂量记录挤没。
//   故按此窗口**聚合**成一条 EVENT_TYPE_BASAL_RATE 写入历史屏;
//   而 dose_log(35 万条容量) 仍逐段记录, 保证审计可追溯到每一次微投递。
#define BASAL_HISTORY_AGG_MS        (30UL * 60UL * 1000UL)   // 30 分钟聚合一条
// 基础率测试注射的推注速度 (Hz) —— 与大剂量同级, 便于肉眼/卡尺观察行程
#define BASAL_TEST_SPEED_HZ         1200

// ============================================================
// 12. 安全参数
// ============================================================
#define SAFETY_TASK_INTERVAL_MS   1000
#define WATCHDOG_TIMEOUT_S        30   // 任务看门狗超时(秒)。需远大于任何任务的喂狗间隔(如 battery ~5s 采样), 留足余量
#define BLE_TIMEOUT_MS            300000
#define MAX_CONTINUOUS_STEPS      100000
#define OVER_TEMP_THRESHOLD_C     60.0f
#define OVER_TEMP_WARN_C          50.0f   // 过温预警(非阻塞, 接近阈值时提示)
#define OVER_CURRENT_MA           1000    // INA226 总电流过流阈值
#define MAX_PRESSURE_KPA          80

// 低药量提前预警阈值 (U): 剩余≤此值触发非阻塞预警, 提示准备换笔芯 (P0-3)
#define RESERVOIR_LOW_WARN_U      20

// 外部看门狗 TPS3813 的 WDI 喂狗引脚; -1 表示硬件未接线, 此时仅用 ESP32 内部
// esp_task_wdt。若实际 PCB 已焊接 TPS3813, 将其 WDI 接到某个空闲 GPIO 并在此指定。
#ifndef PIN_WATCHDOG_WDI
  #define PIN_WATCHDOG_WDI         -1
#endif

// ============================================================
// 13. 任务参数 (FreeRTOS, Arduino-ESP32 内置)
// ============================================================
#define TASK_PRIORITY_SAFETY    10
#define TASK_PRIORITY_MOTOR     8
#define TASK_PRIORITY_BLE       6
#define TASK_PRIORITY_BATTERY   5
#define TASK_PRIORITY_BASAL     5
#define TASK_PRIORITY_DISPLAY    4
#define TASK_PRIORITY_KEYPAD     4
#define TASK_PRIORITY_STORAGE   3
#define TASK_PRIORITY_OTA       2

#define STACK_SIZE_SAFETY    4096
#define STACK_SIZE_MOTOR     4096
#define STACK_SIZE_BLE       8192
#define STACK_SIZE_BATTERY   2048
#define STACK_SIZE_BASAL     2048
#define STACK_SIZE_DISPLAY   6144
#define STACK_SIZE_KEYPAD    2048
#define STACK_SIZE_STORAGE   4096
#define STACK_SIZE_OTA       8192

// ============================================================
// 14. BLE 参数 (NimBLE-Arduino, 协议与 Android APP 对齐)
// ============================================================
#define BLE_DEVICE_NAME         "OpenLoop-Pump"
#define BLE_MANUFACTURER_NAME   "OpenLoop DIY"
#define BLE_MODEL_NUMBER        "OLP-002"

// 自定义 128-bit UUID 基 (6E400001-B5A3-F393-E0A9-E50E24DCCA9E 变体)
#define BLE_SERVICE_PUMP_UUID       { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x01, 0x00, 0x40, 0x6E }
#define BLE_CHAR_BOLUS_UUID         { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x02, 0x00, 0x40, 0x6E }
#define BLE_CHAR_BASAL_UUID         { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x03, 0x00, 0x40, 0x6E }
#define BLE_CHAR_TBR_UUID           { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x04, 0x00, 0x40, 0x6E }
#define BLE_CHAR_STATUS_UUID        { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x05, 0x00, 0x40, 0x6E }
#define BLE_CHAR_IOB_UUID           { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x06, 0x00, 0x40, 0x6E }
#define BLE_CHAR_RESERVOIR_UUID     { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x07, 0x00, 0x40, 0x6E }
// CGM 血糖回传 (手机/AAPS → 泵): 写入 [mgdl u16][trend i8][crc]
#define BLE_CHAR_CGM_UUID           { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x08, 0x00, 0x40, 0x6E }
// 控制通道 (手机 → 泵): 写入 [loop_mode u8][crc] / 或 [cmd u8][crc]
#define BLE_CHAR_CONTROL_UUID       { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x09, 0x00, 0x40, 0x6E }
// 设置通道 (独立伴生 App ↔ 泵, 与 AAPS/Dana 互不干扰): 读写设备设置
//   命令 [op u8][payload...][crc]; 响应: GET 命令将结果写入本特征, App 读取;
//   SET 命令回 1 字节 ack(0=OK/1=ERR)。op 全集(与伴生 App 严格对齐):
//   时间:      0x01 GET_TIME(u32) / 0x02 SET_TIME(u32 Unix)
//   显示:      0x03 GET_BRIGHTNESS / 0x04 SET_BRIGHTNESS(u8 0..100)
//             0x05 GET_KEYPAD / 0x06 SET_KEYPAD(u8 0/1)
//             0x07 GET_VIBRATE / 0x08 SET_VIBRATE(u8 0/1)
//   配对:      0x09 GET_PASSKEY(u32) / 0x0A SET_PASSKEY(u32)
//   基础率方案:0x10 GET_ACTIVE_PROFILE / 0x11 SET_ACTIVE_PROFILE(u8 0..3)
//             0x14 GET_PROFILE_NAME(profile u8 → name[32])
//             0x15 SET_PROFILE_NAME(profile u8 + name)
//             0x16 GET_PROFILE_SLOT(profile u8, hour u8 → f32)
//             0x17 SET_PROFILE_SLOT(profile u8, hour u8, f32)
//   闭环参数:  0x24 GET_CL_PARAM(kind u8, hour u8 → f32)
//             0x25 SET_CL_PARAM(kind u8, hour u8, f32)  kind:0=ISF 1=碳水比 2=目标血糖
//   限制:      0x20 GET_LIMITS(3×f32) / 0x21 SET_LIMIT(which u8, f32)
//   安全:      0x22 GET_SAFETY(occlusion u16, watchdog u8, over_temp f32)
//             0x23 SET_SAFETY(which u8, value)
//   标定:      0x26 GET_CALIBRATION(f32) / 0x27 SET_CALIBRATION(f32)
//   省电:      0x28 GET_AUTO_DIM([u8 enabled][u16 timeout_le]) / 0x29 SET_AUTO_DIM
//   DIA:       0x12 GET_DIA_MIN(u16) / 0x13 SET_DIA_MIN(只读, 编译期固定)
#define BLE_CHAR_SETTINGS_UUID      { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x0A, 0x00, 0x40, 0x6E }
// 远程按键通道 (独立伴生 App → 泵): 写入 [key_event_t u8][crc] (2B)
//   key 取值见 pump_types.h: 1=UP 2=DOWN 3=SET 4=ESC 5=长按SET 6=长按ESC; 0=松开(release)
//   写入即等同物理按键, 走 ui_screen_key / ui_screen_release 同一路径, 泵屏必然同步。
#define BLE_CHAR_KEY_UUID           { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x0B, 0x00, 0x40, 0x6E }
// 泵屏镜像通道 (泵 → 独立伴生 App): NOTIFY/READ 推送 ui_screen_dump_json()
//   内容与联调控制面板一致, App 据此渲染"与泵一致的虚拟屏 + 4 键遥控"。
#define BLE_CHAR_SCREEN_UUID        { 0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, \
                                      0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, \
                                      0x0C, 0x00, 0x40, 0x6E }

// ============================================================
// 15. AAPS Dana-i impersonation（方案 B，编译期开关）
// ============================================================
// 默认关闭：保持现有自定义 BLE（本地伴生 APP 调试通道）不受影响。
// 启用方式：在 Arduino IDE 编译选项中添加宏 -DUSE_AAPS_DANA。
//   启用后设备蓝牙名变为 DANAI_DEVICE_NAME，并额外暴露 FFF0/FFF1/FFF2 服务，
//   被 AndroidAPS 当作 Dana-i 直接识别与驱动（详见 docs/12）。
// ⚠️ 实验项目，禁止用于人体。
#ifndef USE_AAPS_DANA
  // #define USE_AAPS_DANA   // ← 取消注释（或编译时 -DUSE_AAPS_DANA）以启用
#endif
#ifdef USE_AAPS_DANA
  #ifndef DANAI_DEVICE_NAME
    #define DANAI_DEVICE_NAME  "DAN12345AB"   // 10 字符, 匹配 ^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$
  #endif
  #ifndef DANAI_BLE5_KEY
    #define DANAI_BLE5_KEY     "123456"        // 6 位 ASCII 数字
  #endif
  #define DANAI_HW_MODEL    0x09               // Dana-i (0x0A 亦可)
  #define DANAI_PROTOCOL    0x0A
#endif

// ============================================================
// 16. 振动反馈 (P3-15) — 当前原型无震动马达, 仅预留接口
// ============================================================
// 接了震动马达就把这里改成对应 GPIO; 仍走 ui_hal_vibrate() 统一驱动。
// 设为 -1 表示无硬件, ui_hal_vibrate 不动作(仅记录, 供联调观测)。
#ifndef VIBRATION_PIN
  #define VIBRATION_PIN  (-1)
#endif

#include "dosing.h"

#endif // CONFIG_H
