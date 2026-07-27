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
// M0/M1/M2 细分: 硬件焊死 H/H/L = 1/32 微步 (不占 GPIO)
// nSLEEP:        硬件拉高 3.3V 常唤醒 (不占 GPIO)
// nRESET:        硬件 10kΩ 上拉 3.3V (不占 GPIO)
// → 电机仅占 STEP / DIR / ENABLE / nFAULT = 4 引脚
#define PIN_MOTOR_STEP     9     // STEP 脉冲 (定时器 ISR 翻转)
#define PIN_MOTOR_DIR      10    // DIR 方向
#define PIN_MOTOR_ENABLE   11    // ENABLE (低有效)
#define PIN_MOTOR_nFAULT   16    // FAULT 检测 (输入, 低有效)

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
#define PIN_LIMIT_FWD      2     // 前进方向限位
#define PIN_LIMIT_REV      3     // 后退方向限位

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
#define MOTOR_STEPS_PER_REV    200     // 每转步数 (1.8° 步距角)
#define MOTOR_MICROSTEPS       32      // 固定 1/32 细分 (M0/M1/M2 硬件拉高)
#define MOTOR_EFFECTIVE_STEPS  (MOTOR_STEPS_PER_REV * MOTOR_MICROSTEPS)
                                        // = 6400 微步/转

#define LEAD_SCREW_PITCH_MM   0.5f    // 丝杠导程 mm (默认保守估计, 需实测)

#define SYRINGE_DIAMETER_MM   8.65f   // 储药器(标准3ml注射器型储药器/丹纳PH300·优泵CY-13兼容)内腔直径 mm
#define SYRINGE_RADIUS_MM     (SYRINGE_DIAMETER_MM / 2.0f)
#define SYRINGE_AREA_MM2      (3.1415926f * SYRINGE_RADIUS_MM * SYRINGE_RADIUS_MM)
                                        // ≈ 58.8 mm² (π·(8.65/2)²); 实测后微调 DOSE_CALIBRATION

#define MM_PER_STEP           (LEAD_SCREW_PITCH_MM / MOTOR_EFFECTIVE_STEPS)
#define UL_PER_STEP           (MM_PER_STEP * SYRINGE_AREA_MM2)
#define UNITS_PER_STEP        (UL_PER_STEP / 10.0f)
#define STEPS_PER_UNIT        (1.0f / UNITS_PER_STEP)
#define STEPS_PER_005U        ((uint16_t)(STEPS_PER_UNIT * 0.05f))
                                        // = 109 微步 (0.05U; 理论 108.907, 取整 109)
#define MIN_DOSE_UNITS        0.05f   // 最小给药精度 (U) — 全系统剂量网格

// 剂量标定系数: 实际硬件导程/笔芯内径与标称存在制造偏差, 实测后修正。
// 例: 实测打出 0.05U 实际为 0.051U → 标定 = 0.051/0.05 = 1.02。默认 1.0 (未标定)。
// 该系数作用于唯一换算入口 units_to_microsteps(), 全系统剂量随之整体缩放。
#define DOSE_CALIBRATION      1.0f

// ---- 大剂量分批打入 (segmented bolus) ----
// 真实胰岛素泵以「步进 + 段间停顿」方式给大剂量 (Wellion: 0.05U/步, 1s 间隔, ≈3U/min;
// Medtronic 780G: 标准 1.5U/min, 快速 15U/min)。本固件采用同样策略:
// 每批推 0.05U (最小精度网格), 段间停顿并复检安全 (阻塞/报警/储药器空), 支持中途取消。
#define BOLUS_SEGMENT_UNITS       MIN_DOSE_UNITS   // 每批 0.05U
#define BOLUS_SEGMENT_INTERVAL_MS 1000             // 段间停顿 1s → 约 3U/min
#define BOLUS_SPEED_HZ            500              // 单批脉冲频率 (109 微步 ≈ 0.22s; 段间 1s 停顿主导速率≈3U/min)

#define MOTOR_MAX_SPEED_HZ    5000
#define MOTOR_MIN_SPEED_HZ    500
#define MOTOR_ACCEL_HZ        2000
#define MOTOR_PULSE_WIDTH_US  50
#define MANUAL_JOG_STEPS      10      // 手动原点设置时每次微动步数

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
#define MAX_RESERVOIR_UNITS    300
#define IOB_DURATION_HOURS     4.0f

// ============================================================
// 12. 安全参数
// ============================================================
#define SAFETY_TASK_INTERVAL_MS   1000
#define WATCHDOG_TIMEOUT_S        10
#define BLE_TIMEOUT_MS            300000
#define MAX_CONTINUOUS_STEPS      100000
#define OVER_TEMP_THRESHOLD_C     60.0f
#define OVER_CURRENT_MA           1000    // INA226 总电流过流阈值
#define MAX_PRESSURE_KPA          80

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

#endif // CONFIG_H
