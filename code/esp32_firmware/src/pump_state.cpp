/**
 * pump_state.cpp — 全局运行状态与配置 (Arduino 框架)
 */
#include <Arduino.h>
#include "pump_state.h"
#include "config.h"
#include "ui_hal.h"   // P3-13: ui_hal_get_board_temp_c (过温检测源, 双端一致)
#include "dose_log.h"  // 报警触发记入剂量追溯日志
#include "basal_history.h"  // P2-10b: 基础率执行历史
#include "rtc_clock.h" // rtc_is_set (钩入初始执行记录)
#include <string.h>
#include <cstdio>

#ifdef USE_AAPS_DANA
#include "aaps_dana.h"   // P1-7: 报警触发时主动向 AAPS 推送通知 (g_dana_fff1 为 null 时自动 no-op)
#endif

pump_runtime_state_t g_pump_state;
pump_config_t g_pump_config;
float g_dose_calib_factor = DOSE_CALIBRATION;   // P3-14: 运行时标定系数 (默认=1.0)
int   g_last_vib_pat = 0;                        // P3-15: 最近一次振动模式 (供联调观测)

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
    g_pump_config.dose_calibration      = DOSE_CALIBRATION;   // P3-14: 标定系数默认 1.0
    g_dose_calib_factor = g_pump_config.dose_calibration;     // 运行时换算同步

    // 显示 / 用户设置默认
    g_pump_config.display_brightness = 10;   // 默认 10% (省电; ≤50 遵守高温警告)
    g_pump_config.keypad_sound       = 1;    // 默认开
    g_pump_config.vibrate_enabled     = 0;    // P3-15: 振动反馈默认关
    g_pump_config.rtc_base_unix      = 0;    // 未设置时钟
    g_pump_config.auto_dim_enabled   = 1;    // 省电: 空闲自动熄屏默认开
    g_pump_config.auto_dim_timeout_s = 30;   // 省电: 空闲 30s 后熄屏 (背光关闭 + ST7789 休眠)

    // 内置默认基础率方案 (避免本地模式无方案导致 0 输注)
    pump_config_apply_default_basal(&g_pump_config);
    // 内置默认闭环参数 (ISF / 碳水比 / 目标血糖), 避免向导计算除以 0 (P1-8)
    pump_config_apply_default_factors(&g_pump_config);
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

// 亚单位累加器: 避免 0.1U 以下小数反复 floor 丢失
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
    // P0-3: 低药量提前预警(非阻塞) — 剩余≤阈值提示准备换笔芯, 不停止输注
    g_pump_state.reservoir_low_warn =
        (g_pump_state.reservoir_units_left <= RESERVOIR_LOW_WARN_U) ? 1 : 0;
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

void pump_state_update_motor_current_peak(uint16_t ma)
{
    g_pump_state.motor_current_peak_ma = ma;
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
    bool was_active = g_pump_state.alarm_active;
    g_pump_state.alarm_code   = (uint8_t)code;
    g_pump_state.alarm_active = 1;
    g_pump_state.current_state = (uint8_t)PUMP_STATE_ALARM;
    dose_log_append(EVENT_TYPE_ALARM, 0, (uint16_t)code, DOSE_FLAG_ALARM);  // 报警溯源
#ifdef USE_AAPS_DANA
    aaps_notify_alarm((uint8_t)code);   // P1-7: 报警主动推送 AAPS
#endif
    // P3-15: 新报警触发振动反馈 (已由 ui_hal_vibrate 内部按开关拦截)
    if (!was_active) ui_hal_vibrate(VIB_ALARM);
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

// ============================================================
// P3-13 过温检测 (软件钩子)
//   周期性由 UI 层 ui_screen_periodic() 驱动 (真机与模拟器共用同一调用点)。
//   读取 ui_hal_get_board_temp_c() (真机=ESP32 内置传感器, 模拟器=mock),
//   与阈值比较: 接近阈值→非阻塞预警(over_temp_warn), 超过阈值→阻塞报警(ALARM_OVER_TEMP)。
// ============================================================
void thermal_periodic(void)
{
    float t = ui_hal_get_board_temp_c();
    g_pump_state.board_temp_c = t;

    float thr = g_pump_config.over_temp_threshold_c;   // 默认 60°C (config.h)
    const float margin = 3.0f;                          // 预警带: 阈值-3°C 起提示

    if (t >= thr) {
        // 超过阈值: 进入过温报警(阻塞, 暂停输注), 仅在状态切换时触发一次
        if (!(g_pump_state.alarm_active &&
              (alarm_code_t)g_pump_state.alarm_code == ALARM_OVER_TEMP)) {
            pump_state_set_alarm(ALARM_OVER_TEMP);
        }
        g_pump_state.over_temp_warn = 1;
    } else if (t >= thr - margin) {
        // 接近阈值: 非阻塞预警, 不报警
        g_pump_state.over_temp_warn = 1;
    } else {
        // 正常: 清预警; 若此前是过温报警且已降温, 自动解除
        g_pump_state.over_temp_warn = 0;
        if (g_pump_state.alarm_active &&
            (alarm_code_t)g_pump_state.alarm_code == ALARM_OVER_TEMP) {
            pump_state_clear_alarm();
        }
    }
}

// ---- 单位(U) ↔ 微步 统一换算 ----
// 定义已移至 dosing.h (static inline, 单一真源), 本文件不再重复定义。

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
