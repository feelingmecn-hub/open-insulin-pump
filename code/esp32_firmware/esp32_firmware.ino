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
#include "config.h"
#include "pump_types.h"
#include "pump_state.h"
#include "ina226.h"
#include "lcd_display.h"
#include "motor_controller.h"
#include "keypad.h"
#include "power_manager.h"
#include "battery_monitor.h"
#include "safety_monitor.h"
#include "ble_comm.h"
#include "storage.h"
#include "history_log.h"
#include "basal_scheduler.h"
#include "ui_hal.h"      // UI 硬件抽象 (固件后端)
#include "ui_screen.h"   // 全中文 UI 状态机 (与模拟器共用)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LVGL 显示刷新任务 (独占 LVGL 调用, 避免多线程竞争)
static void display_task(void *arg)
{
    for (;;) {
        lcd_display_task();
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
    history_log_init();          // 恢复持久化历史事件

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

    // 创建 FreeRTOS 任务
    xTaskCreate(motor_task,        "motor",   STACK_SIZE_MOTOR,   NULL, TASK_PRIORITY_MOTOR,   NULL);
    xTaskCreate(safety_task,       "safety",  STACK_SIZE_SAFETY,  NULL, TASK_PRIORITY_SAFETY,  NULL);
    xTaskCreate(ble_task,          "ble",     STACK_SIZE_BLE,     NULL, TASK_PRIORITY_BLE,     NULL);
    xTaskCreate(battery_task,      "battery", STACK_SIZE_BATTERY, NULL, TASK_PRIORITY_BATTERY, NULL);
    xTaskCreate(basal_scheduler_task,"basal", STACK_SIZE_BASAL,   NULL, TASK_PRIORITY_BASAL,   NULL);
    xTaskCreate(keypad_task,       "keypad",  STACK_SIZE_KEYPAD,  NULL, TASK_PRIORITY_KEYPAD,  NULL);
    xTaskCreate(display_task,      "display", STACK_SIZE_DISPLAY, NULL, TASK_PRIORITY_DISPLAY, NULL);

    g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
    Serial.println("OpenLoop Pump (Arduino) started");
}

void loop()
{
    // 所有实时逻辑在 FreeRTOS 任务中运行
    delay(1000);
}
