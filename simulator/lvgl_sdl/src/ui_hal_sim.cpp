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
#include "basal_history.h" // P2-10b: 基础率执行历史
#include "rtc_clock.h"   // 内联日历互转 (模拟器不链 rtc_clock.cpp)
#include <cstring>      // strncpy/memcpy (模拟器侧无 Arduino.h 自动包含)

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
int     ui_hal_basal_count(void)    { return BASAL_SLOTS_PER_DAY; }
// 模拟器"当前激活基础率"直接读 g_pump_config.profiles[active], 与 fw/link 一致,
// 保证切换方案/编辑 24 段后显示即时同步 (mock 演示数据在 mock_init 注入到 profiles)。
float   ui_hal_basal_rate(int idx)
{
    if (idx < 0 || idx >= BASAL_SLOTS_PER_DAY) return 0.0f;
    uint8_t p = g_pump_config.active_profile;
    if (p >= MAX_BASAL_PROFILES) p = 0;
    return g_pump_config.profiles[p].slots[idx].rate_uh;
}
void    ui_hal_basal_set_rate(int idx, float rate)
{
    if (idx < 0 || idx >= BASAL_SLOTS_PER_DAY) return;
    uint8_t p = g_pump_config.active_profile;
    if (p >= MAX_BASAL_PROFILES) p = 0;
    float r = rate; if (r < 0.0f) r = 0.0f; if (r > 10.0f) r = 10.0f;
    g_pump_config.profiles[p].slots[idx].rate_uh = r;
}
bool    ui_hal_basal_local_mode(void) { return g_pump_state.loop_mode == 1; }
void    ui_hal_set_active_profile(uint8_t idx)
{
    if (idx < MAX_BASAL_PROFILES) {
        g_pump_config.active_profile = idx;
        basal_history_record(BH_PROFILE_SWITCH, idx, g_pump_state.loop_mode,
                             0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
    }
}

uint8_t ui_hal_active_profile(void)
{
    uint8_t p = g_pump_config.active_profile;
    if (p >= MAX_BASAL_PROFILES) p = 0;
    return p;
}

int ui_hal_profile_count(void) { return MAX_BASAL_PROFILES; }

void ui_hal_profile_name(int idx, char *out, size_t n)
{
    if (!out || n == 0) return;
    if (idx < 0 || idx >= MAX_BASAL_PROFILES) { out[0] = '\0'; return; }
    strncpy(out, g_pump_config.profiles[idx].name, n - 1);
    out[n - 1] = '\0';
}

void ui_hal_profile_set_name(int idx, const char *name)
{
    if (idx < 0 || idx >= MAX_BASAL_PROFILES || !name) return;
    strncpy(g_pump_config.profiles[idx].name, name,
            sizeof(g_pump_config.profiles[idx].name) - 1);
    g_pump_config.profiles[idx].name[sizeof(g_pump_config.profiles[idx].name) - 1] = '\0';
}

float ui_hal_profile_basal_rate(int idx, int slot)
{
    if (idx < 0 || idx >= MAX_BASAL_PROFILES) return 0.0f;
    if (slot < 0 || slot >= BASAL_SLOTS_PER_DAY) return 0.0f;
    return g_pump_config.profiles[idx].slots[slot].rate_uh;
}

void ui_hal_profile_set_basal_rate(int idx, int slot, float rate)
{
    if (idx < 0 || idx >= MAX_BASAL_PROFILES) return;
    if (slot < 0 || slot >= BASAL_SLOTS_PER_DAY) return;
    float r = rate; if (r < 0.0f) r = 0.0f; if (r > 10.0f) r = 10.0f;
    g_pump_config.profiles[idx].slots[slot].rate_uh = r;
}

void ui_hal_profile_copy(int dst, int src)
{
    if (dst < 0 || dst >= MAX_BASAL_PROFILES) return;
    if (src < 0 || src >= MAX_BASAL_PROFILES) return;
    if (dst == src) return;
    memcpy(&g_pump_config.profiles[dst], &g_pump_config.profiles[src],
           sizeof(basal_profile_t));
}

void ui_hal_profile_reset(int idx)
{
    if (idx < 0 || idx >= MAX_BASAL_PROFILES) return;
    pump_config_reset_profile(&g_pump_config, idx);
}

// ---- #260 基础率验证测试 (模拟器: 只算账 + 记历史, 无真实电机) ----
float ui_hal_basal_daily_total(void)
{
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    float sum = 0.0f;
    for (int h = 0; h < BASAL_SLOTS_PER_DAY; h++)
        sum += g_pump_config.profiles[prof].slots[h].rate_uh;  // 每段 1h ⇒ 直接累加
    return sum;
}

float ui_hal_basal_run_test(void)
{
    float total = ui_hal_basal_daily_total();
    if (total <= 0.0f) return 0.0f;
    uint8_t ap = g_pump_config.active_profile;
    if (ap >= MAX_BASAL_PROFILES) ap = 0;
    basal_history_record(BH_BASAL_TEST, ap, g_pump_state.loop_mode, 0,
                         (uint16_t)(total * 100.0f + 0.5f));
    return total;
}

void ui_hal_set_tbr(float percent, uint32_t duration_min)
{
    if (percent <= 0.0f || duration_min == 0) { ui_hal_cancel_tbr(); return; }
    float ref = (g_pump_state.current_basal_rate > 0.0f) ? g_pump_state.current_basal_rate : 0.5f;
    g_pump_state.tbr_percent = percent;
    g_pump_state.tbr_rate = percent / 100.0f * ref;
    g_pump_state.tbr_expiry_ms = duration_min * 60000UL;   // 相对时长(模拟器无真实 basal 调度)
    basal_history_record(BH_TBR_START, g_pump_config.active_profile, g_pump_state.loop_mode,
                         (uint16_t)(percent * 10.0f), (uint16_t)(g_pump_state.tbr_rate * 100.0f));
}

void ui_hal_cancel_tbr(void)
{
    if (g_pump_state.tbr_percent != 0) {
        basal_history_record(BH_TBR_END, g_pump_config.active_profile, g_pump_state.loop_mode,
                             0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
    }
    g_pump_state.tbr_percent = 0;
    g_pump_state.tbr_rate = 0;
    g_pump_state.tbr_expiry_ms = 0;
}

// ---- 动作 ----
void ui_hal_deliver_bolus(float total, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units)
{
    (void)kind; (void)duration_h; (void)immediate_units; (void)extended_units;
    mock_deliver_bolus(total);
}

void ui_hal_start_prime(float prime_u)
{
    (void)prime_u;
    pump_state_set_state(PUMP_STATE_PRIMING);
}

// P3-14: 模拟器无真实电机 — 置为 no-op (状态机仍走, 便于演示界面流程)
void ui_hal_rewind(void)            { /* 模拟器无电机, 仅占位 */ }
void ui_hal_rewind_units(float u)   { (void)u; /* 模拟器无电机, 仅占位 */ }
void ui_hal_calibrate_dispense(float u) { (void)u; /* 模拟器无电机, 仅占位 */ }
void ui_hal_apply_calibration(float f) { if (f > 0.0f) g_dose_calib_factor = f; /* 模拟器仅更新运行时因子 */ }

void ui_hal_toggle_basal_mode(void)
{
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
    basal_history_record(BH_MODE_CHANGE, g_pump_config.active_profile, g_pump_state.loop_mode,
                         0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
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

// P3-13: 模拟器板温(mock, 远低于阈值, 仅占位; 过温检测逻辑仍走真实状态机)
float ui_hal_get_board_temp_c(void) { return 30.0f; }

// ---- P3-15: 振动反馈 (模拟器无马达, 仅记录供联调观测) ----
bool ui_hal_get_vibrate_enabled(void) { return g_pump_config.vibrate_enabled != 0; }
void ui_hal_set_vibrate_enabled(bool on)
{
    g_pump_config.vibrate_enabled = on ? 1 : 0;   // 模拟器内存态, 不持久化
}
void ui_hal_vibrate(vib_pattern_t pat)
{
    if (!g_pump_config.vibrate_enabled) return;
    g_last_vib_pat = (int)pat;                    // 记录(供联调观测)
}
