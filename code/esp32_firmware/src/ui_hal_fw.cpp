/**
 * ui_hal_fw.cpp — UI 硬件抽象层 · 固件后端 (ESP32-C6 + Arduino)
 *
 * 数据来自 g_pump_state / g_pump_config (真实模块填充)。
 * 动作调用真实硬件模块:
 *   - 大剂量 → motor_enqueue(MOTOR_CMD_BOLUS) + 历史记录 + 今日统计
 *   - 排气   → motor_enqueue(MOTOR_CMD_PRIME) + 状态切换
 *   - 报警清除 → pump_state_clear_alarm() + 历史记录
 *   - 背光   → lcd_display_backlight()
 *   - 模式切换 → 更新 g_pump_state.loop_mode
 *
 * 与模拟器后端 ui_hal_sim.cpp 一一对应, 保证同一份 ui_screen.cpp 双端一致。
 */
#include <Arduino.h>
#include "ui_hal.h"
#include "pump_state.h"
#include "pump_types.h"
#include "config.h"

#include "motor_controller.h"
#include "basal_scheduler.h"
#include "lcd_display.h"
#include "history_log.h"
#include "dose_log.h"      // TBR/排气/回退/清报警 记入剂量追溯日志
#include "basal_history.h" // P2-10b: 基础率执行历史 (方案切换/TBR/模式 记录)
#include "storage.h"
#include "rtc_clock.h"   // 时钟读写

// ---- 内部状态 ----
static bool s_keypad_sound = true;

// ============================================================
// 1. 数据读取 (UI 显示用)
// ============================================================

float ui_hal_glucose_mmol(void)
{
    // CGM 血糖由 BLE 回传写入 g_pump_state.last_glucose_mgdl
    return g_pump_state.last_glucose_mgdl > 0
               ? (float)g_pump_state.last_glucose_mgdl / 18.0f
               : 0.0f;
}

int8_t ui_hal_glucose_trend(void)
{
    return (int8_t)g_pump_state.glucose_trend;   // 已为 5 档显示码 -2..2
}

bool ui_hal_glucose_valid(void)
{
    if (g_pump_state.last_glucose_mgdl == 0) return false;   // 无数据
    if (!rtc_is_set()) return true;                          // 时钟未设置则不做过期判定
    uint32_t now = rtc_unix_now();
    if (now == 0) return true;
    uint32_t age = (now > g_pump_state.last_glucose_time_unix)
                   ? (now - g_pump_state.last_glucose_time_unix) : 0;
    return age <= 600u;                                       // 超 10 分钟判离线
}

uint8_t ui_hal_loop_mode(void)
{
    return g_pump_state.loop_mode;
}

bool ui_hal_loop_connected(void)
{
    return g_pump_state.ble_connected;
}

float ui_hal_tbr_percent(void)
{
    return g_pump_state.tbr_percent;
}

float ui_hal_tbr_rate(void)
{
    return g_pump_state.tbr_rate;
}

float ui_hal_today_total(void)
{
    return (float)g_pump_state.today_units_x100 / 100.0f;
}

void ui_hal_get_clock(int *hh, int *mm)
{
    // 显示本地墙钟 (UTC + 时区偏移), 而非裸 UTC
    uint32_t u = rtc_local_now();
    if (u == 0) { *hh = -1; *mm = -1; return; }   // 未设置
    int y, mo, d, h, mi, s;
    rtc_unix_to_ymdhms(u, &y, &mo, &d, &h, &mi, &s);
    *hh = h; *mm = mi;
}

int ui_hal_basal_count(void)
{
    return BASAL_SLOTS_PER_DAY;  // 24
}

float ui_hal_basal_rate(int idx)
{
    if (idx < 0 || idx >= BASAL_SLOTS_PER_DAY) return 0.0f;
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    return g_pump_config.profiles[prof].slots[idx].rate_uh;
}

bool ui_hal_basal_local_mode(void)
{
    return g_pump_state.loop_mode == 1;  // 1 = 开环(本地档案)
}

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
    storage_save_config(&g_pump_config);  // 落盘持久化当前方案
    // 执行历史: 记录方案切换 (附当前 TBR/速率快照)
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
    float r = quantize_units_grid(rate);   // 吸附 0.1U/h 剂量网格
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

// ---- #260 基础率验证测试 (直接委托调度器, 保证读的是与实际输注同一个真源) ----
float ui_hal_basal_daily_total(void)
{
    return basal_scheduler_daily_total();
}

float ui_hal_basal_run_test(void)
{
    return basal_scheduler_run_daily_test();
}

// ---- TBR 历史记录钩子 (P2-9 补全): 泵菜单设/取 TBR 时通知 AAPS 协议层写入 0xC2 回放 ----
// 初始为 NULL; aaps_dana_attach() 会注册 aaps_dana_record_tbr 的包装回调。
// 这样泵本地菜单的 TBR 也能进 AAPS 治疗账本, 且不破坏 HAL ↔ 协议层的分层。
static ui_hal_tbr_hist_cb_t s_tbr_hist_cb = NULL;

void ui_hal_register_tbr_history_cb(ui_hal_tbr_hist_cb_t cb)
{
    s_tbr_hist_cb = cb;
}

void ui_hal_set_tbr(float percent, uint32_t duration_min)
{
    if (percent <= 0.0f || duration_min == 0) { ui_hal_cancel_tbr(); return; }
    float ref = (g_pump_state.current_basal_rate > 0.0f)
                ? g_pump_state.current_basal_rate : 0.5f;
    g_pump_state.tbr_percent = percent;
    g_pump_state.tbr_rate    = percent / 100.0f * ref;
    g_pump_state.tbr_expiry_ms = millis() + duration_min * 60000UL;
    history_log_event(EVENT_TYPE_TBR, ALARM_NONE, (uint32_t)percent, (uint16_t)duration_min);
    dose_log_append(EVENT_TYPE_TBR, (uint32_t)percent, (uint16_t)duration_min, 0);
    // 执行历史: TBR 开始
    basal_history_record(BH_TBR_START, g_pump_config.active_profile, g_pump_state.loop_mode,
                         (uint16_t)(percent * 10.0f), (uint16_t)(g_pump_state.tbr_rate * 100.0f));
    // AAPS 0xC2 回放: 把菜单 TBR 也写进历史缓冲 (BLE 路径在 0x60/0xC1 handler 直接记, 此处补菜单路径)
    if (s_tbr_hist_cb) s_tbr_hist_cb(UI_HAL_TBR_EVENT_START, (uint16_t)percent, (uint16_t)duration_min);
}

void ui_hal_cancel_tbr(void)
{
    if (g_pump_state.tbr_percent != 0) {
        basal_history_record(BH_TBR_END, g_pump_config.active_profile, g_pump_state.loop_mode,
                             0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
    }
    g_pump_state.tbr_percent = 0;
    g_pump_state.tbr_rate    = 0;
    g_pump_state.tbr_expiry_ms = 0;
    // AAPS 0xC2 回放: 菜单取消 TBR 也写停止事件
    if (s_tbr_hist_cb) s_tbr_hist_cb(UI_HAL_TBR_EVENT_STOP, 0, 0);
}

// P3-13: 板载温度读取 (过温检测源)
float ui_hal_get_board_temp_c(void)
{
    // ---- 真机硬件勾子 (需 ESP32 内置温度传感器) ----
    // Arduino-ESP32 / ESP-IDF ≥ 4.4 可用下列 driver; 把 #if 0 改 #if 1 即启用:
    //   #include "driver/temperature_sensor.h"
    //   static temperature_sensor_handle_t s_ts = nullptr; static bool s_inited = false;
    //   if (!s_inited) {
    //       temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    //       if (temperature_sensor_install(&c, &s_ts) == ESP_OK && s_ts) temperature_sensor_enable(s_ts);
    //       s_inited = true;
    //   }
    //   if (s_ts) { float t = 0; if (temperature_sensor_get_celsius(s_ts, &t) == ESP_OK && t > 0) return t; }
    // ---- 回退(教学原型): 以电机电流做保守板温估算, 用于演示过温预警/报警状态机 ----
    float t = 38.0f;
    if (g_pump_state.motor_current_ma > 0)
        t += (float)g_pump_state.motor_current_ma / 200.0f;
    return t;
}

// ============================================================
// 2. 动作 (菜单触发, 真实生效)
// ============================================================

void ui_hal_deliver_bolus(float total_units, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units)
{
    (void)total_units;

    // 安全限制
    if (immediate_units < 0.01f && extended_units < 0.01f) return;
    if (immediate_units > g_pump_config.max_bolus_single) immediate_units = g_pump_config.max_bolus_single;
    if (extended_units  > g_pump_config.max_bolus_single) extended_units  = g_pump_config.max_bolus_single;

    // 吸附到 0.1U 最小剂量网格 (全系统统一剂量精度, 见 quantize_units_grid)
    immediate_units = quantize_units_grid(immediate_units);
    extended_units  = quantize_units_grid(extended_units);

    uint8_t k = (uint8_t)kind;

    // 立即量 → 入队电机命令 (记账/历史/储药器扣减由 motor_controller 分段完成时统一处理,
    // 以支持中途取消只损失已打部分; 此处只负责校验 + 入队)
    if (immediate_units > 0.001f) {
        motor_command_t cmd{};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(immediate_units * 100.0f + 0.5f);
        cmd.kind       = k;
        motor_enqueue(&cmd);
    }

    // 方波/双波延展量 → 交给 basal_scheduler 按 duration_h 时间维铺开
    // (不再作为一次性大剂量入队; 总量正确 + 时序正确 + 可中途取消)
    if (extended_units > 0.001f) {
        basal_scheduler_start_extended_bolus(extended_units, duration_h, k);
    }
}

void ui_hal_start_prime(float prime_u)
{
    pump_state_set_state(PUMP_STATE_PRIMING);

    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_PRIME;
    // prime_u 已是胰岛素单位(U); 转为电机推注量(units_x100 = U*100)驱动丝杠
    uint32_t u = (uint32_t)(prime_u * 100.0f + 0.5f);
    if (u < 1) u = 1;
    cmd.units_x100 = u;
    motor_enqueue(&cmd);

    history_log_event(EVENT_TYPE_PRIME, ALARM_NONE, 0, 0);
    dose_log_append(EVENT_TYPE_PRIME, 0, 0, 0);
}

// P3-14: 回退装药 (退活塞装新笔芯) —— 全退到电机尾部(后限位), 不依赖已打药量
void ui_hal_rewind(void)
{
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_REWIND;
    cmd.units_x100 = 0;   // 0 → motor_controller 全退到后限位(尾部)
    motor_enqueue(&cmd);
    history_log_event(EVENT_TYPE_REWIND, ALARM_NONE, 0, 0);
    dose_log_append(EVENT_TYPE_REWIND, 0, 0, 0);
}

// P3-14: 手动退药 —— 按指定 U 退, 由用户自行判断退多少合适
void ui_hal_rewind_units(float units)
{
    if (units < 0.1f) units = 0.1f;
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_REWIND;
    cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);   // >0 → motor_controller 按指定 U 退
    motor_enqueue(&cmd);
    history_log_event(EVENT_TYPE_REWIND, ALARM_NONE, (uint32_t)(units * 100.0f), 0);
    dose_log_append(EVENT_TYPE_REWIND, (uint32_t)(units * 100.0f), 0, 0);
}

// P3-14: 剂量标定出测试量 (默认 1.0U), 供用户实测后计算标定系数
void ui_hal_calibrate_dispense(float units)
{
    if (units < 0.1f) units = 1.0f;   // 默认 1.0U 参考体积
    motor_command_t cmd{};
    cmd.type = MOTOR_CMD_CALIBRATE;
    cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);
    motor_enqueue(&cmd);
}

// P3-14: 保存标定系数(实测/指令)到配置并持久化, 同步运行时换算因子
void ui_hal_apply_calibration(float factor)
{
    if (factor <= 0.0f || factor > 2.0f) factor = DOSE_CALIBRATION;   // 防 0/负/异常放大(异常系数会令 units_to_microsteps 步数爆炸顶死丝杆)
    g_pump_config.dose_calibration = factor;
    g_dose_calib_factor = factor;
    storage_save_config(&g_pump_config);
}

// 完全手动电机控制 (电机测试): 直接驱动丝杠前进/后退, 不记账(不改储药器/IOB/今日统计)。
// steps=0 → 连续点动直到 ui_hal_manual_stop() / 限位命中; steps>0 → 定量步数。
void ui_hal_manual_move(uint8_t dir, uint32_t steps, uint16_t speed_hz)
{
    motor_command_t cmd{};
    cmd.type  = MOTOR_CMD_MANUAL;
    cmd.dir   = (dir == MOTOR_DIR_REVERSE) ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
    cmd.steps = steps;
    cmd.speed_hz = speed_hz;
    motor_enqueue(&cmd);
}

// 停止正在进行的手动点动 (连续点动退出)
void ui_hal_manual_stop(void)
{
    motor_manual_stop();
}

void ui_hal_toggle_basal_mode(void)
{
    // 在 开环(本地档案=1) 与 闭环(AAPS接管=0) 之间切换
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
    // ⚠️ 2026-08-08: 之前只 storage_save_config 了配置, 但 loop_mode 压根不在配置里,
    //    重启后 pump_state_init() 无条件置 0 → 屏幕又回"闭环中"。必须写入持久化字段。
    g_pump_config.loop_mode_pref = g_pump_state.loop_mode;
    storage_save_config(&g_pump_config);  // 记录模式偏好 (标脏, loop() 去抖落盘)
    // 执行历史: 模式切换
    basal_history_record(BH_MODE_CHANGE, g_pump_config.active_profile, g_pump_state.loop_mode,
                         0, (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
}

void ui_hal_clear_alarm(void)
{
    uint8_t prev_code = g_pump_state.alarm_code;
    pump_state_clear_alarm();
    history_log_event(EVENT_TYPE_ALARM_CLEAR, prev_code, 0, 0);
    dose_log_append(EVENT_TYPE_ALARM_CLEAR, 0, (uint16_t)prev_code, DOSE_FLAG_ALARM);
}

void ui_hal_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    g_pump_config.display_brightness = pct;   // 持久化, 供 App/UI 读取同一份
    lcd_display_backlight(pct);
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
    lcd_display_backlight(g_pump_config.display_brightness);  // 开机应用存储的亮度
    // 固件侧其余初始化在各模块 setup() 中完成
}

// ---- P3-15: 振动反馈 ----
bool ui_hal_get_vibrate_enabled(void) { return g_pump_config.vibrate_enabled != 0; }

void ui_hal_set_vibrate_enabled(bool on)
{
    g_pump_config.vibrate_enabled = on ? 1 : 0;
    storage_save_config(&g_pump_config);   // 持久化偏好
}

void ui_hal_vibrate(vib_pattern_t pat)
{
    if (!g_pump_config.vibrate_enabled) return;   // 开关关闭直接返回
    g_last_vib_pat = (int)pat;                    // 记录(供联调观测)
#if VIBRATION_PIN >= 0
    // 真机有马达: 按模式驱动 GPIO 脉冲(次数/占空不同)。
    // 注: 中断安全起见, 实际固件应在非 ISR 上下文调用; 当前调用点
    // (ui_screen_key / pump_state_set_alarm) 均在任务上下文, 安全。
    int pulses = 1;
    if (pat == VIB_ALARM)       pulses = 3;
    else if (pat == VIB_BOLUS_DONE) pulses = 2;
    else if (pat == VIB_CONFIRM)     pulses = 1;
    for (int i = 0; i < pulses; i++) {
        digitalWrite(VIBRATION_PIN, HIGH);
        delay(60);
        digitalWrite(VIBRATION_PIN, LOW);
        if (i + 1 < pulses) delay(80);
    }
#else
    // 当前原型无震动马达: 仅记录, 不动作。
    (void)pat;
#endif
}

// ---- 时钟 ----
bool ui_hal_clock_valid(void)    { return rtc_is_set(); }
void ui_hal_set_time(uint32_t unix_sec) { rtc_set_unix(unix_sec); }
void ui_hal_set_time_ymdhms(int y, int mo, int d, int h, int mi, int s)
{
    // 用户输入的是本地墙钟, 转成真实 UTC 存储 (内部时钟一律存 UTC 秒)
    uint32_t local = rtc_ymdhms_to_unix(y, mo, d, h, mi, s);
    int32_t off = (int32_t)rtc_get_zone_offset() * 3600;
    int32_t u = (int32_t)local - off;
    if (u < 0) u = 0;
    rtc_set_unix((uint32_t)u);
}

// ---- 显示 / 设置读取 ----
uint8_t ui_hal_get_brightness(void) { return g_pump_config.display_brightness; }
bool    ui_hal_dana_paired(void)    { return g_pump_state.dana_paired; }
bool    ui_hal_get_keypad_sound(void) { return s_keypad_sound; }
void    ui_hal_get_ymdhms(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    // 读取本地墙钟供"设置时间"界面显示/编辑
    uint32_t u = rtc_local_now();
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
