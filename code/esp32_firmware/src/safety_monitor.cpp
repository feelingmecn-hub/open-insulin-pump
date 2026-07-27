/**
 * safety_monitor.cpp — 安全监控 (丢步/过流/限位/nFAULT/电池)
 */
#include "safety_monitor.h"
#include "config.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "ina226.h"
#include "power_manager.h"

#include <driver/gpio.h>
#include <driver/ledc.h>

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

    pinMode(PIN_LIMIT_FWD, INPUT_PULLUP);
    pinMode(PIN_LIMIT_REV,  INPUT_PULLUP);
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

        // 限位触发
        if (digitalRead(PIN_LIMIT_FWD) == 0 || digitalRead(PIN_LIMIT_REV) == 0)
            safety_trigger_alarm(ALARM_LIMIT_TRIGGERED);

        // 过流 (INA226 总电流)
        if (g_pump_state.battery_current_ma > OVER_CURRENT_MA)
            safety_trigger_alarm(ALARM_OVER_CURRENT);

        // 丢步 (INA226 电流监护)
        if (motor_step_loss_detected())
            safety_trigger_alarm(ALARM_STEP_LOSS);

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

        vTaskDelay(pdMS_TO_TICKS(SAFETY_TASK_INTERVAL_MS));
    }
}
