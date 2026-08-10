/**
 * battery_monitor.cpp — 电池监测 (INA226 + 3S 放电曲线)
 */
#include <Arduino.h>
#include "battery_monitor.h"
#include "config.h"
#include "ina226.h"
#include "pump_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"   // P0-1: 看门狗喂狗

static uint8_t pct_from_mv(uint16_t mv)
{
    if (mv >= BATTERY_FULL_MV)    return 100;
    if (mv <= BATTERY_CRITICAL_MV) return 0;
    if (mv >= BATTERY_NOMINAL_MV) {
        // 12600..11100 → 100..50
        return (uint8_t)(50 + (mv - BATTERY_NOMINAL_MV) * 50UL /
                          (BATTERY_FULL_MV - BATTERY_NOMINAL_MV));
    }
    // 11100..9000 → 50..0
    return (uint8_t)((mv - BATTERY_CRITICAL_MV) * 50UL /
                     (BATTERY_NOMINAL_MV - BATTERY_CRITICAL_MV));
}

void battery_init(void)
{
    // INA226 已在 ina226_init() 初始化
}

void battery_task(void *arg)
{
    for (;;) {
        esp_task_wdt_reset();   // P0-1: 喂狗
        ina226_telemetry_t tel;
        if (ina226_read(&tel)) {
            uint8_t pct = pct_from_mv(tel.bus_voltage_mv);
            pump_state_update_battery(tel.bus_voltage_mv, pct);
            pump_state_update_bus_power((uint16_t)tel.power_mw);
            g_pump_state.battery_current_ma =
                (uint16_t)(tel.current_ma < 0 ? -tel.current_ma : tel.current_ma);
        }
        // 5 秒采样周期, 但拆成 5 段(每段 1s)持续喂狗, 避免单次长延迟错过看门狗窗口
        for (int i = 0; i < 5; ++i) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
    }
}
