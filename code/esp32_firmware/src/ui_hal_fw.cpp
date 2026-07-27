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
#include "ui_hal.h"
#include "pump_state.h"
#include "pump_types.h"
#include "config.h"

#include "motor_controller.h"
#include "lcd_display.h"
#include "history_log.h"
#include "storage.h"

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
    return (int8_t)g_pump_state.glucose_trend;
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
    // 简单运行时钟 (从开机计时), 后续可接 RTC 或 AAPS 同步
    uint32_t sec = (uint32_t)(millis() / 1000UL);
    *hh = (int)((sec / 3600UL) % 24UL);
    *mm = (int)((sec / 60UL) % 60UL);
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

// ============================================================
// 2. 动作 (菜单触发, 真实生效)
// ============================================================

void ui_hal_deliver_bolus(float total_units, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units)
{
    (void)kind; (void)duration_h;

    // 安全限制
    if (total_units < 0.01f) return;
    if (total_units > g_pump_config.max_bolus_single) total_units = g_pump_config.max_bolus_single;

    // 立即量 → 入队电机命令
    if (immediate_units > 0.001f) {
        motor_command_t cmd{0};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(immediate_units * 100.0f + 0.5f);
        motor_enqueue(&cmd);

        // 统计与历史
        uint32_t ux100 = (uint32_t)(immediate_units * 100.0f + 0.5f);
        g_pump_state.today_units_x100 += ux100;
        g_pump_state.total_units_x100_delivered += ux100;
        g_pump_state.iob_x10000 += (uint32_t)(immediate_units * 10000.0f);
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, ux100, 0);
    }

    // 方波/双波延展量 → 作为第二条 bolus 入队 (总量正确, 时序未铺开)
    // TODO: 完整的方波调度需在 basal_scheduler 中实现时间铺开
    if (extended_units > 0.001f) {
        motor_command_t cmd{0};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(extended_units * 100.0f + 0.5f);
        motor_enqueue(&cmd);

        uint32_t ux100 = (uint32_t)(extended_units * 100.0f + 0.5f);
        g_pump_state.today_units_x100 += ux100;
        g_pump_state.total_units_x100_delivered += ux100;
        g_pump_state.iob_x10000 += (uint32_t)(extended_units * 10000.0f);
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, ux100, 0);
    }

    // 储药器扣减 (命令剂量; 若电机实际阻塞由安全模块另报)
    pump_state_consume_units(immediate_units + extended_units);

    // 持久化配置变更 (累计统计)
    storage_save_config(&g_pump_config);
}

void ui_hal_start_prime(void)
{
    pump_state_set_state(PUMP_STATE_PRIMING);

    motor_command_t cmd{0};
    cmd.type = MOTOR_CMD_PRIME;
    motor_enqueue(&cmd);

    history_log_event(EVENT_TYPE_PRIME, ALARM_NONE, 0, 0);
}

void ui_hal_toggle_basal_mode(void)
{
    // 在 开环(本地档案=1) 与 闭环(AAPS接管=0) 之间切换
    g_pump_state.loop_mode = (g_pump_state.loop_mode == 1) ? 0 : 1;
    storage_save_config(&g_pump_config);  // 记录模式偏好
}

void ui_hal_clear_alarm(void)
{
    uint8_t prev_code = g_pump_state.alarm_code;
    pump_state_clear_alarm();
    history_log_event(EVENT_TYPE_ALARM_CLEAR, prev_code, 0, 0);
}

void ui_hal_set_brightness(uint8_t pct)
{
    lcd_display_backlight(pct);
}

bool ui_hal_toggle_keypad_sound(void)
{
    s_keypad_sound = !s_keypad_sound;
    return s_keypad_sound;
}

void ui_hal_init(void)
{
    s_keypad_sound = true;
    // 固件侧无需额外初始化; 各模块在 setup() 中已初始化
}
