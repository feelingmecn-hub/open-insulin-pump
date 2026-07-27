/**
 * ui_hal_sim.cpp — UI 硬件抽象层 · 模拟器后端
 *
 * 数据来自 mock_hal (演示场景), 动作更新 g_pump_state / 调用 mock。
 * 与固件后端 ui_hal_fw.cpp 一一对应, 保证同一份 ui_screen.cpp 双端一致。
 */
#include "ui_hal.h"
#include "mock_hal.h"
#include "pump_state.h"
#include "pump_types.h"

// ---- 数据读取 ----
float   ui_hal_glucose_mmol(void)   { return mock_get_glucose_mmol(); }
int8_t  ui_hal_glucose_trend(void)  { return mock_get_trend(); }
uint8_t ui_hal_loop_mode(void)      { return g_pump_state.loop_mode; }
bool    ui_hal_loop_connected(void) { return mock_loop_connected(); }
float   ui_hal_tbr_percent(void)    { return mock_get_tbr_percent(); }
float   ui_hal_tbr_rate(void)       { return mock_get_tbr_rate(); }
float   ui_hal_today_total(void)    { return mock_get_today_total(); }
void    ui_hal_get_clock(int *hh, int *mm) { mock_get_clock(hh, mm); }
int     ui_hal_basal_count(void)    { return mock_basal_count(); }
float   ui_hal_basal_rate(int idx)  { return mock_basal_rate(idx); }
bool    ui_hal_basal_local_mode(void) { return g_pump_state.loop_mode == 1; }

// ---- 动作 ----
void ui_hal_deliver_bolus(float total, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units)
{
    (void)kind; (void)duration_h; (void)immediate_units; (void)extended_units;
    mock_deliver_bolus(total);
}

void ui_hal_start_prime(void)
{
    pump_state_set_state(PUMP_STATE_PRIMING);
}

void ui_hal_toggle_basal_mode(void)
{
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
}

void ui_hal_clear_alarm(void)
{
    pump_state_clear_alarm();
}

void ui_hal_set_brightness(uint8_t pct) { (void)pct; }

bool ui_hal_toggle_keypad_sound(void)
{
    static bool s_on = true;
    s_on = !s_on;
    return s_on;
}

void ui_hal_init(void) { /* 模拟器无需额外初始化 */ }

bool ui_hal_bolus_active(void) { return false; }   // 模拟器无真实电机
void ui_hal_cancel_bolus(void) { /* 模拟器无真实电机, 无需处理 */ }
