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

#include <cstring>      // strncpy/memcpy (模拟器侧无 Arduino.h 自动包含)
#include "motor_controller.h"
#include "basal_scheduler.h"
#include "rtc_clock.h"
#include "storage.h"        // storage_save_config (主机为 no-op)
#include "basal_history.h"  // P2-10b: 基础率执行历史
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

void ui_hal_basal_set_rate(int idx, float rate)
{
    if (idx < 0 || idx >= BASAL_SLOTS_PER_DAY) return;
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    // 吸附到 0.1U/h 剂量网格 (与全系统剂量精度一致), 并夹在安全范围
    float r = quantize_units_grid(rate);
    if (r < 0.0f)  r = 0.0f;
    if (r > 10.0f) r = 10.0f;   // 单段速率安全上限
    g_pump_config.profiles[prof].slots[idx].rate_uh = r;
    storage_save_config(&g_pump_config);  // 落盘持久化
}

void ui_hal_set_active_profile(uint8_t idx)
{
    if (idx >= MAX_BASAL_PROFILES) idx = 0;
    g_pump_config.active_profile = idx;
    storage_save_config(&g_pump_config);  // 落盘持久化当前方案 (联调桩为 no-op)
    basal_history_record(BH_PROFILE_SWITCH, idx, g_pump_state.loop_mode,
                         0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
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
    storage_save_config(&g_pump_config);
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
    float r = quantize_units_grid(rate);
    if (r < 0.0f)  r = 0.0f;
    if (r > 10.0f) r = 10.0f;
    g_pump_config.profiles[idx].slots[slot].rate_uh = r;
    storage_save_config(&g_pump_config);
}

void ui_hal_profile_copy(int dst, int src)
{
    if (dst < 0 || dst >= MAX_BASAL_PROFILES) return;
    if (src < 0 || src >= MAX_BASAL_PROFILES) return;
    if (dst == src) return;
    memcpy(&g_pump_config.profiles[dst], &g_pump_config.profiles[src],
           sizeof(basal_profile_t));
    storage_save_config(&g_pump_config);
}

void ui_hal_profile_reset(int idx)
{
    if (idx < 0 || idx >= MAX_BASAL_PROFILES) return;
    pump_config_reset_profile(&g_pump_config, idx);
    storage_save_config(&g_pump_config);
}

// ---- #260 基础率验证测试 (联调桩: 算账 + 记历史, 电机由联调引擎另行模拟) ----
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
    g_pump_state.tbr_expiry_ms = duration_min * 60000UL;   // 相对时长(联调桩无真实 basal 调度)
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

    immediate_units = quantize_units_grid(immediate_units);
    extended_units  = quantize_units_grid(extended_units);
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

void ui_hal_start_prime(float prime_u)
{
    pump_state_set_state(PUMP_STATE_PRIMING);
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_PRIME;
    // prime_u 已是胰岛素单位(U); 转为电机推注量(units_x100 = U*100), 使联调电机面板①柱塞同步动作
    uint32_t u = (uint32_t)(prime_u * 100.0f + 0.5f);
    if (u < 1) u = 1;
    cmd.units_x100 = u;
    motor_enqueue(&cmd);
}

// P3-14: 回退装药 + 剂量标定出测试量 (联调模式: 经电机命令, 电机面板①柱塞同步)
void ui_hal_rewind(void)
{
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_REWIND;
    cmd.units_x100 = 0;   // 0 → 全退到尾部(后限位)
    motor_enqueue(&cmd);
    // 注: 联调后端不依赖 history_log 框架, 历史落盘由真机后端负责
}
void ui_hal_rewind_units(float units)
{
    if (units < 0.1f) units = 0.1f;
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_REWIND;
    cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);   // >0 → 按指定 U 退
    motor_enqueue(&cmd);
}
void ui_hal_calibrate_dispense(float units)
{
    if (units < 0.1f) units = 1.0f;
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_CALIBRATE;
    cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);
    motor_enqueue(&cmd);
}
void ui_hal_apply_calibration(float factor)
{
    if (factor > 0.0f) {
        g_dose_calib_factor = factor;              // 联调: 更新运行时换算因子
        g_pump_config.dose_calibration = factor;   // 同步配置, 供快照/显示读取
        storage_save_config(&g_pump_config);       // 主机为 no-op
    }
}

void ui_hal_toggle_basal_mode(void)
{
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
    storage_save_config(&g_pump_config);   // 主机为 no-op
    basal_history_record(BH_MODE_CHANGE, g_pump_config.active_profile, g_pump_state.loop_mode,
                         0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
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
void    ui_hal_mark_activity(void)   { /* 模拟器无真实背光熄屏, no-op */ }
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

// P3-13: 联调板温(mock, 可由 TCP "thermal N.N" 注入以演示过温预警/报警)
static float s_link_board_temp_c = 35.0f;
float ui_hal_get_board_temp_c(void) { return s_link_board_temp_c; }
void  link_set_board_temp_c(float c) { s_link_board_temp_c = c; }   // 供 link_ipc 注入

// ---- P3-15: 振动反馈 (联调无马达, 仅记录供 TCP 快照观测) ----
bool ui_hal_get_vibrate_enabled(void) { return g_pump_config.vibrate_enabled != 0; }
void ui_hal_set_vibrate_enabled(bool on)
{
    g_pump_config.vibrate_enabled = on ? 1 : 0;   // 联调内存态
    storage_save_config(&g_pump_config);          // 主机为 no-op
}
void ui_hal_vibrate(vib_pattern_t pat)
{
    if (!g_pump_config.vibrate_enabled) return;
    g_last_vib_pat = (int)pat;                    // 记录(供联调观测)
}
