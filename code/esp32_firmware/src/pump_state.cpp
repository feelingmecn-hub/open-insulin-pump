/**
 * pump_state.cpp — 全局运行状态与配置 (Arduino 框架)
 */
#include "pump_state.h"
#include "config.h"
#include <string.h>
#include <cstdio>

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

    // 内置默认基础率方案 (避免本地模式无方案导致 0 输注)
    pump_config_apply_default_basal(&g_pump_config);
}

void pump_config_apply_default_basal(pump_config_t *cfg)
{
    if (!cfg) return;
    const float def_rate = 0.5f;   // U/h, 全天恒定 (原型用, 可在 UI/App 调整)
    for (uint8_t p = 0; p < MAX_BASAL_PROFILES; p++) {
        snprintf(cfg->profiles[p].name, sizeof(cfg->profiles[p].name),
                 p == 0 ? "默认" : "方案%d", p + 1);
        for (int i = 0; i < BASAL_SLOTS_PER_DAY; i++) {
            cfg->profiles[p].slots[i].hour    = (uint8_t)i;
            cfg->profiles[p].slots[i].rate_uh = def_rate;
        }
    }
}

// 亚单位累加器: 避免 0.05U 这种小数反复 floor 丢失
static float s_reservoir_frac = 0.0f;

void pump_state_consume_units(float units)
{
    if (units <= 0.0f) return;
    s_reservoir_frac += units;
    uint16_t whole = (uint16_t)s_reservoir_frac;
    if (whole > 0) {
        if (whole >= g_pump_state.reservoir_units_left) {
            whole = g_pump_state.reservoir_units_left;
            s_reservoir_frac = 0.0f;
            if (g_pump_state.reservoir_units_left == 0) {
                pump_state_set_alarm(ALARM_RESERVOIR_EMPTY);
            }
        }
        g_pump_state.reservoir_units_left -= whole;
        s_reservoir_frac -= whole;
    }
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

// ---- 单位(U) ↔ 微步 统一换算 (全系统唯一入口) ----
// 见 pump_state.h 头部推导: 0.5mm/rev · 1/32 微步 · 4.5mm 内径
uint32_t units_to_microsteps(float units)
{
    if (units <= 0.0f) return 0;
    // 唯一换算入口: 单位(U) → 微步。DOSE_CALIBRATION 用于实测标定整体缩放。
    float steps = units * STEPS_PER_UNIT * DOSE_CALIBRATION;
    return (uint32_t)(steps + 0.5f);        // 四舍五入, 误差 < 1 微步 ≈ 0.00012U
}

float microsteps_to_units(uint32_t steps)
{
    return (float)steps / (STEPS_PER_UNIT * DOSE_CALIBRATION);
}

// 吸附到 0.05U 最小精度网格: 0.05 的整数倍, 且不低于 MIN_DOSE_UNITS
float quantize_units_005(float units)
{
    if (units <= 0.0f) return 0.0f;
    float q = (float)((int)(units / MIN_DOSE_UNITS + 0.5f)) * MIN_DOSE_UNITS;
    if (q < MIN_DOSE_UNITS) q = MIN_DOSE_UNITS;
    return q;
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
