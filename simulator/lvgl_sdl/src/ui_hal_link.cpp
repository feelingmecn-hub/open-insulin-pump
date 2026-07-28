/* ui_hal_link.cpp — UI 硬件抽象层 · 模拟器联调模式后端 (主机兼容)
 *
 * 与 ui_hal_fw.cpp 行为一致 (数据来自 g_pump_state / g_pump_config / rtc_*),
 * 但**不依赖 Arduino / LCD / history_log 框架** —— 动作直接调用主机桩
 * (motor_enqueue / basal_scheduler_* / pump_state_*), 供 SIM_LINK_MODE 在 PC 上编译运行。
 *
 * 这样模拟器联调模式与真机共用同一份"状态真理" (g_pump_state), 泵屏幕即真实反映
 * AAPS 命令效果。
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#include "ui_hal.h"
#include "pump_state.h"
#include "pump_types.h"
#include "config.h"

#include "motor_controller.h"
#include "basal_scheduler.h"
#include "rtc_clock.h"
#include "storage.h"        // storage_save_config (主机为 no-op)
#include "host_glue.h"      // 主机桩: motor_enqueue / motor_cancel_bolus / 等

#include <cstring>

// ---- 内部状态 ----
static bool s_keypad_sound = true;

// ============================================================
// 1. 数据读取 (UI 显示用)
// ============================================================

float ui_hal_glucose_mmol(void)
{
    return g_pump_state.last_glucose_mgdl > 0
               ? (float)g_pump_state.last_glucose_mgdl / 18.0f
               : 0.0f;
}

int8_t ui_hal_glucose_trend(void)
{
    return (int8_t)g_pump_state.glucose_trend;
}

bool ui_hal_glucose_valid(void)
{
    if (g_pump_state.last_glucose_mgdl == 0) return false;
    if (!rtc_is_set()) return true;
    uint32_t now = rtc_unix_now();
    if (now == 0) return true;
    uint32_t age = (now > g_pump_state.last_glucose_time_unix)
                   ? (now - g_pump_state.last_glucose_time_unix) : 0;
    return age <= 600u;
}

uint8_t ui_hal_loop_mode(void)        { return g_pump_state.loop_mode; }
bool    ui_hal_loop_connected(void)   { return g_pump_state.ble_connected; }
float   ui_hal_tbr_percent(void)      { return g_pump_state.tbr_percent; }
float   ui_hal_tbr_rate(void)         { return g_pump_state.tbr_rate; }
float   ui_hal_today_total(void)      { return (float)g_pump_state.today_units_x100 / 100.0f; }

void ui_hal_get_clock(int *hh, int *mm)
{
    uint32_t u = rtc_unix_now();
    if (u == 0) { *hh = -1; *mm = -1; return; }
    int y, mo, d, h, mi, s;
    rtc_unix_to_ymdhms(u, &y, &mo, &d, &h, &mi, &s);
    *hh = h; *mm = mi;
}

int ui_hal_basal_count(void)          { return BASAL_SLOTS_PER_DAY; }

float ui_hal_basal_rate(int idx)
{
    if (idx < 0 || idx >= BASAL_SLOTS_PER_DAY) return 0.0f;
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    return g_pump_config.profiles[prof].slots[idx].rate_uh;
}

bool ui_hal_basal_local_mode(void)    { return g_pump_state.loop_mode == 1; }

// ============================================================
// 2. 动作 (菜单触发, 经主机桩真实生效到 g_pump_state)
// ============================================================

void ui_hal_deliver_bolus(float total_units, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units)
{
    (void)total_units;
    if (immediate_units < 0.01f && extended_units < 0.01f) return;
    if (immediate_units > g_pump_config.max_bolus_single) immediate_units = g_pump_config.max_bolus_single;
    if (extended_units  > g_pump_config.max_bolus_single) extended_units  = g_pump_config.max_bolus_single;

    immediate_units = quantize_units_005(immediate_units);
    extended_units  = quantize_units_005(extended_units);
    uint8_t k = (uint8_t)kind;

    if (immediate_units > 0.001f) {
        motor_command_t cmd{};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(immediate_units * 100.0f + 0.5f);
        cmd.kind       = k;
        motor_enqueue(&cmd);
    }
    if (extended_units > 0.001f) {
        basal_scheduler_start_extended_bolus(extended_units, duration_h, k);
    }
}

void ui_hal_start_prime(void)
{
    pump_state_set_state(PUMP_STATE_PRIMING);
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_PRIME;
    motor_enqueue(&cmd);
}

void ui_hal_toggle_basal_mode(void)
{
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
    storage_save_config(&g_pump_config);   // 主机为 no-op
}

void ui_hal_clear_alarm(void)
{
    pump_state_clear_alarm();
}

void ui_hal_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    g_pump_config.display_brightness = pct;
    storage_save_config(&g_pump_config);
}

bool ui_hal_toggle_keypad_sound(void)
{
    s_keypad_sound = !s_keypad_sound;
    return s_keypad_sound;
}

void ui_hal_init(void)
{
    s_keypad_sound = (g_pump_config.keypad_sound != 0);
}

// ---- 时钟 ----
bool    ui_hal_clock_valid(void)    { return rtc_is_set(); }
void    ui_hal_set_time(uint32_t unix_sec) { rtc_set_unix(unix_sec); }
void    ui_hal_set_time_ymdhms(int y, int mo, int d, int h, int mi, int s)
{
    rtc_set_unix(rtc_ymdhms_to_unix(y, mo, d, h, mi, s));
}

// ---- 显示 / 设置读取 ----
uint8_t ui_hal_get_brightness(void) { return g_pump_config.display_brightness; }
bool    ui_hal_dana_paired(void)    { return g_pump_state.dana_paired; }
bool    ui_hal_get_keypad_sound(void) { return s_keypad_sound; }
void    ui_hal_get_ymdhms(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    uint32_t u = rtc_unix_now();
    if (u == 0) { *y = 2026; *mo = 1; *d = 1; *h = 0; *mi = 0; *s = 0; return; }
    rtc_unix_to_ymdhms(u, y, mo, d, h, mi, s);
}

bool ui_hal_bolus_active(void)
{
    return motor_bolus_active() || basal_scheduler_extended_bolus_active();
}

void ui_hal_cancel_bolus(void)
{
    motor_cancel_bolus();
    basal_scheduler_cancel_extended_bolus();
}
