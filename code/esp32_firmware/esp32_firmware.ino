/**
 * esp32_firmware.ino — OpenLoop Insulin Pump 主入口 (Arduino 框架)
 *
 * 框架: Arduino IDE 2.x + ESP32 Board Manager 3.x
 *       + Arduino_GFX + LVGL 9.5.0 + NimBLE-Arduino
 *
 * 所有逻辑运行在 FreeRTOS 任务中 (Arduino-ESP32 内置):
 *   motor / safety / ble / battery / keypad / display
 * loop() 仅空转。
 */
#include <Arduino.h>
#include "HWCDC.h"           // ESP32-C6 原生 USB-CDC 串口: HWCDC(USB Serial/JTAG) 无 OTG 限制, Serial=HWCDCSerial
#include "esp_task_wdt.h"   // P0-1: ESP32 内部任务看门狗 (防任务挂死)
// 注: 固件头文件统一放在 src/ 子目录。Arduino 框架 (IDE/CLI) 的
//     quote-include 不会自动回退搜索 src/, 故此处显式写 src/ 前缀;
//     PlatformIO 与联调模拟器均兼容此写法。
#include "src/config.h"
#include "src/pump_types.h"
#include "src/pump_state.h"
#include "src/ina226.h"
#include "src/lcd_display.h"
#include "src/motor_controller.h"
#include "src/keypad.h"
#include "src/power_manager.h"
#include "src/battery_monitor.h"
#include "src/safety_monitor.h"
#include "src/ble_comm.h"
#include "src/storage.h"
#include "src/history_log.h"
#include "src/dose_log.h"     // 紧凑追加式剂量追溯日志
#include "src/basal_history.h" // 基础率执行历史 (时间轴/用量柱状图)
#include "src/basal_scheduler.h"
#include "src/ui_hal.h"      // UI 硬件抽象 (固件后端)
#include "src/ui_screen.h"   // 全中文 UI 状态机 (与模拟器共用)
#include "src/rtc_clock.h"   // 时钟基准 (持久化 RTC)
#include "src/iob_model.h"   // IOB 衰减模型
#include "src/aaps_dana.h"    // AAPS Dana-i 协议 (异步发送泵 aaps_dana_pump)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LVGL 显示刷新任务 (独占 LVGL 调用, 避免多线程竞争)
static void display_task(void *arg)
{
    for (;;) {
        lcd_display_task();
        esp_task_wdt_reset();   // P0-1: 喂狗 (每 30ms 必到)
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);

    pump_state_init();
    storage_init();
    storage_load_config(&g_pump_config);
    // 恢复环模式偏好 (pump_state_init 无条件置 0=闭环; 若不在这里回填,
    // 用户在 App/泵屏上关掉闭环后一重启又显示"闭环中" —— 用户报告的现象之一)
    if (g_pump_config.loop_mode_pref <= 2) {
        g_pump_state.loop_mode = g_pump_config.loop_mode_pref;
    }
    rtc_clock_init();            // 载入持久化时钟基准 (须在 storage_load_config 之后)
    iob_init();                  // IOB 衰减模型记录表清零
    history_log_init();          // 恢复持久化历史事件
    dose_log_init();             // 恢复持久化剂量追溯日志写指针/计数
    dose_log_append(EVENT_TYPE_POWER_ON, 0, 0, 0);  // 启动溯源标记
    basal_history_init();        // 恢复持久化基础率执行历史 (NVS)

    ina226_init();
    lcd_display_init();          // 内部调用 ui_screen_init() 创建全中文状态屏
    motor_init();
    keypad_init();
    power_init();
    battery_init();
    safety_init();
    ble_init();

    ui_hal_init();               // UI 后端初始化 (按键音等)
    basal_scheduler_init();      // 初始化当前基础率

    // ---- P0-1 内部任务看门狗 ----
    // 订阅"有界循环"的关键任务; basal_scheduler 因 3 分钟长睡不订阅(避免误复位,
    // 且它仅入队 motor 命令、风险低)。motor 任务的队列等待改为有界(见 motor_controller.cpp)。
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = (uint32_t)WATCHDOG_TIMEOUT_S * 1000UL,
        .idle_core_mask = 0,
        .trigger_panic  = true,        // 超时 → 系统 panic + 自动重启
    };
    // 注意: Arduino 框架在 initArduino() 中已初始化过 TWDT。再次 esp_task_wdt_init 会返回
    // ESP_ERR_INVALID_STATE("TWDT already initialized") 而忽略本配置 → 之前 WATCHDOG_TIMEOUT_S
    // 实际未生效, 导致 battery 等任务按 ~5s 间隔喂狗时超过 Arduino 默认短超时, 每 ~12s 触发
    // panic 复位(表现为"设备不停断电")。故: init 失败则用 esp_task_wdt_reconfigure 覆盖超时。
    if (esp_task_wdt_init(&wdt_cfg) != ESP_OK) {
        esp_task_wdt_reconfigure(&wdt_cfg);
    }

    // 创建 FreeRTOS 任务 (捕获句柄用于订阅看门狗)
    TaskHandle_t h_motor=NULL, h_safety=NULL, h_ble=NULL, h_battery=NULL,
                 h_basal=NULL, h_keypad=NULL, h_display=NULL;
    xTaskCreate(motor_task,        "motor",   STACK_SIZE_MOTOR,   NULL, TASK_PRIORITY_MOTOR,   &h_motor);
    xTaskCreate(safety_task,       "safety",  STACK_SIZE_SAFETY,  NULL, TASK_PRIORITY_SAFETY,  &h_safety);
    xTaskCreate(ble_task,          "ble",     STACK_SIZE_BLE,     NULL, TASK_PRIORITY_BLE,     &h_ble);
    xTaskCreate(battery_task,      "battery", STACK_SIZE_BATTERY, NULL, TASK_PRIORITY_BATTERY, &h_battery);
    xTaskCreate(basal_scheduler_task,"basal", STACK_SIZE_BASAL,   NULL, TASK_PRIORITY_BASAL,   &h_basal);
    xTaskCreate(keypad_task,       "keypad",  STACK_SIZE_KEYPAD,  NULL, TASK_PRIORITY_KEYPAD,  &h_keypad);
    xTaskCreate(display_task,      "display", STACK_SIZE_DISPLAY, NULL, TASK_PRIORITY_DISPLAY, &h_display);

    // 订阅看门狗 (basal 不订阅, 见上)
    if (h_motor)   esp_task_wdt_add(h_motor);
    if (h_safety)  esp_task_wdt_add(h_safety);
    if (h_ble)     esp_task_wdt_add(h_ble);
    if (h_battery) esp_task_wdt_add(h_battery);
    if (h_keypad)  esp_task_wdt_add(h_keypad);
    if (h_display) esp_task_wdt_add(h_display);

    g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
    Serial.println("OpenLoop Pump (Arduino) started");
}

void loop()
{
    // 所有实时逻辑在 FreeRTOS 任务中运行；Dana 异步发送队列在此排空
#ifdef USE_AAPS_DANA
    aaps_dana_pump();
#endif
    rtc_clock_tick();   // 每 10 分钟回写时钟基准，防重启后时间倒退触发 AAPS 1.5h 硬门限

    // ---- #263 NVS 延迟落盘 (用户报告"App 一改环模式开关就崩固件"的根因修复) ----
    // NVS 写入会临时关闭 flash cache; 若在 BLE onWrite 回调(中断/蓝牙栈上下文)里同步写,
    // 期间任何未驻留 IRAM 的 BLE ISR 触发即 cache-disabled panic → 复位。
    // 因此三个持久化模块统一改为"回调只标脏, loop() 上下文去抖落盘"。
    // 每拍最多落一个模块 (三者错峰), 避免单次 loop 里连续多次擦写拖长 flash 停顿。
    if (!storage_flush_tick()) {
        if (!basal_history_flush_tick()) {
            history_log_tick();
        }
    }

    delay(1000);
}
