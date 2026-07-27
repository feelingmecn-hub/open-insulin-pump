/**
 * pump_state.cpp — 全局运行状态与配置 (Arduino 框架)
 */
#include "pump_state.h"
#include "config.h"
#include <string.h>

pump_runtime_state_t g_pump_state;
pump_config_t g_pump_config;

void pump_state_init(void)
{
    memset(&g_pump_state, 0, sizeof(g_pump_state));
    memset(&g_pump_config, 0, sizeof(g_pump_config));

    g_pump_state.current_state = (uint8_t)PUMP_STATE_BOOTING;
    g_pump_state.reservoir_units_left = MAX_RESERVOIR_UNITS;
    g_pump_state.battery_pct = 100;
    g_pump_state.battery_mv = BATTERY_FULL_MV;
    g_pump_state.motor_max_position = (uint32_t)(STEPS_PER_UNIT * MAX_RESERVOIR_UNITS);
    g_pump_state.loop_mode = 0;            // 默认闭环(AAPS接管)
    g_pump_state.today_units_x100 = 0;
    g_pump_state.tbr_percent = 0;
    g_pump_state.tbr_rate = 0;
    g_pump_state.tbr_expiry_ms = 0;

    // 默认配置
    g_pump_config.insulin_concentration = INSULIN_CONCENTRATION;
    g_pump_config.max_bolus_per_hour    = MAX_BOLUS_UNITS;
    g_pump_config.max_basal_per_hour    = MAX_BASAL_RATE;
    g_pump_config.max_bolus_single      = MAX_BOLUS_UNITS;
    g_pump_config.occlusion_threshold   = 700;
    g_pump_config.watchdog_timeout_sec  = WATCHDOG_TIMEOUT_S;
    g_pump_config.over_temp_threshold_c = OVER_TEMP_THRESHOLD_C;
    g_pump_config.motor_steps_per_unit  = STEPS_PER_UNIT;
    g_pump_config.syringe_area_mm2      = SYRINGE_AREA_MM2;
    g_pump_config.lead_screw_pitch_mm   = LEAD_SCREW_PITCH_MM;
    g_pump_config.motor_microstep       = MOTOR_MICROSTEPS;
    g_pump_config.units_per_ml          = INSULIN_CONCENTRATION;
}

void pump_state_update_battery(uint16_t mv, uint8_t pct)
{
    g_pump_state.battery_mv  = mv;
    g_pump_state.battery_pct = pct;
}

void pump_state_update_motor_current(uint16_t ma)
{
    g_pump_state.motor_current_ma = ma;
}

void pump_state_update_bus_power(uint16_t mw)
{
    g_pump_state.bus_power_mw = mw;
}

void pump_state_set_step_loss(bool lost)
{
    g_pump_state.step_loss_detected = lost;
}

void pump_state_set_alarm(alarm_code_t code)
{
    g_pump_state.alarm_code   = (uint8_t)code;
    g_pump_state.alarm_active = 1;
    g_pump_state.current_state = (uint8_t)PUMP_STATE_ALARM;
}

void pump_state_clear_alarm(void)
{
    g_pump_state.alarm_active = 0;
    g_pump_state.alarm_code   = ALARM_NONE;
    if (g_pump_state.current_state == (uint8_t)PUMP_STATE_ALARM) {
        g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
    }
}

void pump_state_set_state(pump_state_t s)
{
    g_pump_state.current_state = (uint8_t)s;
}

// CRC-8 (CCITT): poly 0x07, init 0x00
uint8_t crc8_ccitt(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
            else            crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}
