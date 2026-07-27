/**
 * basal_scheduler.cpp — 基础率周期调度器
 */
#include "basal_scheduler.h"
#include "config.h"
#include "pump_types.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "history_log.h"
#include "storage.h"

// 取当前整点应执行的基础率 (U/h)
static float basal_rate_for_now(void)
{
    // 暂停模式: 0
    if (g_pump_state.loop_mode == 2) return 0.0f;

    float base = 0.0f;
    if (g_pump_state.loop_mode == 1) {
        // 本地档案: 读当前方案对应整点的 slot
        uint8_t prof = g_pump_config.active_profile;
        if (prof >= MAX_BASAL_PROFILES) prof = 0;
        uint8_t hour = (uint8_t)((millis() / 3600000UL) % 24UL);
        base = g_pump_config.profiles[prof].slots[hour].rate_uh;
    } else {
        // 闭环: BLE 下发的速率 (AAPS 接管), 未下发则 0
        base = g_pump_state.current_basal_rate;
    }

    // 临时基础率优先 (有效期内)
    uint32_t now = millis();
    if (g_pump_state.tbr_percent > 0 && now < g_pump_state.tbr_expiry_ms) {
        base = g_pump_state.tbr_rate;   // tbr_rate 为绝对 U/h
    } else if (g_pump_state.tbr_percent > 0 && now >= g_pump_state.tbr_expiry_ms) {
        // 到期, 清除 TBR
        g_pump_state.tbr_percent = 0;
        g_pump_state.tbr_rate    = 0;
        g_pump_state.tbr_expiry_ms = 0;
    }

    if (base < 0.0f) base = 0.0f;
    if (base > MAX_BASAL_RATE) base = MAX_BASAL_RATE;
    return base;
}

// ============================================================
// 方波/双波延展量: 按 duration 时间维铺开 (取代"延展量作一次性第二条大剂量")
// ============================================================

// 已铺开部分记为一次(部分)大剂量事件, 并清除活动标志
static void ext_bolus_log_partial(void)
{
    uint32_t delivered = g_pump_state.ext_bolus_delivered_x100;
    if (delivered > 0) {
        g_pump_config.total_bolus_count++;
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, delivered,
                          (uint16_t)g_pump_state.ext_bolus_kind);
        storage_save_config(&g_pump_config);
    }
    g_pump_state.ext_bolus_active = false;
}

// 每个 basal tick 按"已过时间比例"投递一小段; 时间到收尾。
// 记账(储药器/今日/IOB)在入队时同步进行, 与 motor_deliver_bolus 的分段记账一致,
// 保证中途取消(或储药器空)只损失未铺开部分。
static void extended_bolus_tick(void)
{
    if (!g_pump_state.ext_bolus_active) return;

    uint32_t now     = millis();
    uint32_t elapsed = (now > g_pump_state.ext_bolus_start_ms)
                           ? (now - g_pump_state.ext_bolus_start_ms) : 0;
    float frac = (g_pump_state.ext_bolus_duration_ms > 0)
                     ? ((float)elapsed / (float)g_pump_state.ext_bolus_duration_ms)
                     : 1.0f;
    if (frac > 1.0f) frac = 1.0f;

    float total     = (float)g_pump_state.ext_bolus_total_x100 / 100.0f;
    float delivered = (float)g_pump_state.ext_bolus_delivered_x100 / 100.0f;
    float target    = total * frac;
    float this_tick = target - delivered;

    // 储药器空 → 中止延展量, 已铺开部分记为部分大剂量事件
    if (g_pump_state.reservoir_units_left < 1) {
        pump_state_set_alarm(ALARM_RESERVOIR_EMPTY);
        ext_bolus_log_partial();
        return;
    }

    // 仅在达到最小给药精度(0.05U)时才投递, 避免无效微步
    if (this_tick >= MIN_DOSE_UNITS) {
        motor_command_t cmd{0};
        cmd.type       = MOTOR_CMD_BOLUS_EXT;
        cmd.units_x100 = (uint32_t)(this_tick * 100.0f + 0.5f);
        cmd.kind       = g_pump_state.ext_bolus_kind;
        cmd.speed_hz   = BOLUS_SPEED_HZ;
        if (motor_enqueue(&cmd)) {
            pump_state_consume_units(this_tick);
            uint32_t ux100 = (uint32_t)(this_tick * 100.0f + 0.5f);
            g_pump_state.today_units_x100           += ux100;
            g_pump_state.total_units_x100_delivered += ux100;
            g_pump_state.iob_x10000                 += (uint32_t)(this_tick * 10000.0f);
            g_pump_state.ext_bolus_delivered_x100    = (uint32_t)(target * 100.0f + 0.5f);
        }
    }

    // 时间到 → 收尾: 投递剩余零头 + 记一次完整大剂量事件
    if (frac >= 1.0f) {
        float rem = total - (float)g_pump_state.ext_bolus_delivered_x100 / 100.0f;
        if (rem > 0.0005f && g_pump_state.reservoir_units_left >= 1) {
            motor_command_t cmd{0};
            cmd.type       = MOTOR_CMD_BOLUS_EXT;
            cmd.units_x100 = (uint32_t)(rem * 100.0f + 0.5f);
            cmd.kind       = g_pump_state.ext_bolus_kind;
            cmd.speed_hz   = BOLUS_SPEED_HZ;
            if (motor_enqueue(&cmd)) {
                pump_state_consume_units(rem);
                uint32_t rx100 = (uint32_t)(rem * 100.0f + 0.5f);
                g_pump_state.today_units_x100           += rx100;
                g_pump_state.total_units_x100_delivered += rx100;
                g_pump_state.iob_x10000                 += (uint32_t)(rem * 10000.0f);
                g_pump_state.ext_bolus_delivered_x100    = g_pump_state.ext_bolus_total_x100;
            }
        }
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE,
                          g_pump_state.ext_bolus_total_x100,
                          (uint16_t)g_pump_state.ext_bolus_kind);
        g_pump_config.total_bolus_count++;
        storage_save_config(&g_pump_config);
        g_pump_state.ext_bolus_active = false;
    }
}

void basal_scheduler_start_extended_bolus(float units, float duration_h, uint8_t kind)
{
    if (units < MIN_DOSE_UNITS) return;
    if (duration_h <= 0.0f) {
        // 无时长 → 退化为一次性大剂量(与原行为一致)
        motor_command_t cmd{0};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);
        cmd.kind       = kind;
        motor_enqueue(&cmd);
        return;
    }
    g_pump_state.ext_bolus_active         = true;
    g_pump_state.ext_bolus_kind           = kind;
    g_pump_state.ext_bolus_total_x100     = (uint32_t)(units * 100.0f + 0.5f);
    g_pump_state.ext_bolus_delivered_x100 = 0;
    g_pump_state.ext_bolus_duration_ms    = (uint32_t)(duration_h * 3600000.0f);
    g_pump_state.ext_bolus_start_ms       = millis();
}

void basal_scheduler_cancel_extended_bolus(void)
{
    if (g_pump_state.ext_bolus_active) ext_bolus_log_partial();
}

bool basal_scheduler_extended_bolus_active(void)
{
    return g_pump_state.ext_bolus_active;
}

void basal_scheduler_init(void)
{
    // 当前基础率先按本地方案初始化, 避免开局显示 0
    g_pump_state.current_basal_rate = basal_rate_for_now();
}

void basal_scheduler_task(void *arg)
{
    (void)arg;
    for (;;) {
        float rate = basal_rate_for_now();
        g_pump_state.current_basal_rate = rate;   // 反映给状态屏/蓝牙

        // 本 tick 应输注的药量: rate * 间隔 / 3600
        float units = rate * (BASAL_TICK_INTERVAL_MS / 3600000.0f);
        if (units > 0.0005f) {
            motor_command_t cmd{0};
            cmd.type      = MOTOR_CMD_BASAL_TICK;
            cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);
            cmd.rate_uh   = rate;
            motor_enqueue(&cmd);

            // 统计 + 历史 + 储药器
            g_pump_state.today_units_x100          += cmd.units_x100;
            g_pump_state.total_units_x100_delivered += cmd.units_x100;
            pump_state_consume_units(units);
            history_log_event(EVENT_TYPE_BASAL_RATE, ALARM_NONE,
                              cmd.units_x100, (uint16_t)(rate * 100.0f));
        }

        // ---- 方波/双波延展量: 按时间比例在每 tick 铺开一小段 ----
        extended_bolus_tick();

        vTaskDelay(pdMS_TO_TICKS(BASAL_TICK_INTERVAL_MS));
    }
}
