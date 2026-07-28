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
#include "rtc_clock.h"   // 内联日历互转 (模拟器不链 rtc_clock.cpp)

// 模拟器本地时钟/亮度状态 (独立于固件 rtc_clock, 仅用于演示 UI)
static uint32_t s_mock_unix = 0;     // 0 = 未设置
static uint8_t  s_bright    = 40;
static bool     s_keypad_on = true;

// ---- 数据读取 ----
float   ui_hal_glucose_mmol(void)   { return mock_get_glucose_mmol(); }
int8_t  ui_hal_glucose_trend(void)  { return mock_get_trend(); }
bool    ui_hal_glucose_valid(void)  { return true; }   // 模拟器血糖由 mock 持续刷新, 始终有效
uint8_t ui_hal_loop_mode(void)      { return g_pump_state.loop_mode; }
bool    ui_hal_loop_connected(void) { return mock_loop_connected(); }
float   ui_hal_tbr_percent(void)    { return mock_get_tbr_percent(); }
float   ui_hal_tbr_rate(void)       { return mock_get_tbr_rate(); }
float   ui_hal_today_total(void)    { return mock_get_today_total(); }
void    ui_hal_get_clock(int *hh, int *mm)
{
    if (s_mock_unix == 0) { *hh = -1; *mm = -1; return; }
    int y, mo, d, h, mi, s;
    rtc_unix_to_ymdhms(s_mock_unix, &y, &mo, &d, &h, &mi, &s);
    *hh = h; *mm = mi;
}
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

void ui_hal_set_brightness(uint8_t pct) { if (pct > 100) pct = 100; s_bright = pct; }

bool ui_hal_toggle_keypad_sound(void)
{
    s_keypad_on = !s_keypad_on;
    return s_keypad_on;
}

void ui_hal_init(void) { /* 模拟器无需额外初始化 */ }

// ---- 时钟 ----
bool ui_hal_clock_valid(void) { return s_mock_unix != 0; }
void ui_hal_set_time(uint32_t unix_sec) { s_mock_unix = unix_sec; }
void ui_hal_set_time_ymdhms(int y, int mo, int d, int h, int mi, int s)
{
    s_mock_unix = rtc_ymdhms_to_unix(y, mo, d, h, mi, s);
}

// ---- 显示 / 设置读取 ----
uint8_t ui_hal_get_brightness(void) { return s_bright; }
bool    ui_hal_dana_paired(void)    { return g_pump_state.dana_paired; }
bool    ui_hal_get_keypad_sound(void) { return s_keypad_on; }
void    ui_hal_get_ymdhms(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    if (s_mock_unix == 0) { *y = 2026; *mo = 1; *d = 1; *h = 0; *mi = 0; *s = 0; return; }
    rtc_unix_to_ymdhms(s_mock_unix, y, mo, d, h, mi, s);
}

bool ui_hal_bolus_active(void) { return false; }   // 模拟器无真实电机
void ui_hal_cancel_bolus(void) { /* 模拟器无真实电机, 无需处理 */ }
