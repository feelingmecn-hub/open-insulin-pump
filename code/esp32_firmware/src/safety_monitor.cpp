/**
 * safety_monitor.cpp — 安全监控 (丢步/过流/限位/nFAULT/电池)
 */
#include <Arduino.h>
#include "safety_monitor.h"
#include "config.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "ina226.h"
#include "power_manager.h"
#include "esp_task_wdt.h"   // P0-1: 看门狗喂狗

#include <driver/gpio.h>
#include <driver/ledc.h>
#include "esp32-hal-log.h"   // log_w (调试提示用)

static bool g_buzzer_state = false;
static uint32_t g_last_beep_ms = 0;

static void buzzer_set(bool on)
{
    if (on) ledcWrite(PIN_BUZZER, 128);   // 50% duty
    else    ledcWrite(PIN_BUZZER, 0);
}

void safety_init(void)
{
    // 蜂鸣器 LEDC
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num   = (gpio_num_t)PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch);

    // 限位开关: 本项目无硬件限位开关(GPIO2/3 为 ESP32-C6 USB-D+/D-, 被 USB-CDC 占用),
    //   不配置 GPIO 限位引脚。限位判定由 INA226 堵转电流检测(g_occlusion)经 ALARM_OCCLUSION 体现。

    // P0-1: 外部看门狗 TPS3813 的 WDI 喂狗引脚 (未接线时 PIN_WATCHDOG_WDI<0, 跳过)
    if (PIN_WATCHDOG_WDI >= 0) {
        pinMode(PIN_WATCHDOG_WDI, OUTPUT);
        digitalWrite(PIN_WATCHDOG_WDI, 0);
    }
}

// P0-1: 翻转外部看门狗 WDI (仅当引脚已接线)。由 safety_task 每 1s 翻转一次,
// 若 safety 任务挂死则 WDI 停跳 → TPS3813 硬件复位 MCU。
static void wdi_toggle(void)
{
    if (PIN_WATCHDOG_WDI < 0) return;
    static int s_lvl = 0;
    s_lvl ^= 1;
    digitalWrite(PIN_WATCHDOG_WDI, s_lvl);
}

void safety_trigger_alarm(alarm_code_t code)
{
    pump_state_set_alarm(code);
}

void safety_task(void *arg)
{
    for (;;) {
        // 电池
        if (g_pump_state.battery_mv <= BATTERY_CRITICAL_MV)
            safety_trigger_alarm(ALARM_BATTERY_CRITICAL);
        else if (g_pump_state.battery_mv <= BATTERY_LOW_MV)
            safety_trigger_alarm(ALARM_BATTERY_LOW);

        // 电机故障 (nFAULT 低)
        if (gpio_get_level((gpio_num_t)PIN_MOTOR_nFAULT) == 0)
            safety_trigger_alarm(ALARM_MOTOR_FAULT);

        // 限位触发: 无硬件限位开关, 由 INA226 堵转(g_occlusion)经 ALARM_OCCLUSION 体现, 此处不重复检测。

        // 过流 (INA226 总电流)
#ifdef MOTOR_DEBUG_UNLOCKED
        // 调试模式: 过流/丢步仅提示(串口日志), 不报警(不限制), 电机可无条件运行
        if (g_pump_state.battery_current_ma > OVER_CURRENT_MA)
            log_w("[SAFETY][DEBUG] 过流提示(非限制): %ld mA > %d mA",
                  (long)g_pump_state.battery_current_ma, OVER_CURRENT_MA);
        if (motor_step_loss_detected())
            log_w("[SAFETY][DEBUG] 丢步提示(非限制): step_loss 标志置位");
#else
        if (g_pump_state.battery_current_ma > OVER_CURRENT_MA)
            safety_trigger_alarm(ALARM_OVER_CURRENT);
        if (motor_step_loss_detected())
            safety_trigger_alarm(ALARM_STEP_LOSS);
#endif

        // 电机实时电流采样 (INA226): 空闲/运动均刷新, 供 LCD 实时显示 + 调试标定堵转阈值。
        // 运动时 motor_stall_guard_tick 也会高频写同一字段(覆盖), 此处作空闲兜底采样。
        {
            int32_t mc = ina226_read_current_ma();
            pump_state_update_motor_current((uint16_t)(mc < 0 ? 0 : mc));
        }

        // 蜂鸣器急促 (报警时每 500ms 翻转)
        if (g_pump_state.alarm_active) {
            uint32_t now = millis();
            if (now - g_last_beep_ms > 500) {
                g_last_beep_ms = now;
                g_buzzer_state = !g_buzzer_state;
                buzzer_set(g_buzzer_state);
            }
        } else {
            buzzer_set(false);
        }

        // P0-1: 喂内部看门狗 + 翻转外部 WDI (硬件双保险)
        esp_task_wdt_reset();
        wdi_toggle();

        vTaskDelay(pdMS_TO_TICKS(SAFETY_TASK_INTERVAL_MS));
    }
}
