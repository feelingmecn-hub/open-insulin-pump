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
#include "iob_model.h"      // IOB 衰减模型

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
        // 延展量取消: 把 IOB 记录裁剪为"实际已投递量", 不夸大未投递部分
        iob_record_extended_cancel(millis(), (float)delivered / 100.0f);
        g_pump_config.total_bolus_count++;
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, delivered,
                          (uint16_t)g_pump_state.ext_bolus_kind);
        storage_save_config(&g_pump_config);
    }
    g_pump_state.ext_bolus_active = false;
}

// 每个细拍按"已过时间比例"投递一小段, 并按方波速率匀速走丝杠(连续慢滴);
// 时间到收尾。记账(储药器/今日/IOB)在入队时同步进行, 与 motor_deliver_bolus 的
// 分段记账一致, 保证中途取消(或储药器空)只损失未铺开部分。
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

    // 方波速率 (U/h) → 走丝杠慢速 (steps/s), 使本窗口电机连续运行填满时间,
    // 实现匀速连续慢滴(贴近真实泵 "均匀输注, 步间 ~1s"), 而非用 BOLUS_SPEED_HZ 快打后歇几分钟。
    float rate_uh     = (g_pump_state.ext_bolus_duration_ms > 0)
                            ? (total / (g_pump_state.ext_bolus_duration_ms / 3600000.0f))
                            : total;   // 退化一次性不会走到这里
    float steps_per_s = (rate_uh / 3600.0f) * (float)STEPS_PER_UNIT;
    uint16_t slow_hz  = (uint16_t)(steps_per_s + 0.5f);
    if (slow_hz < 1) slow_hz = 1;

    // 仅在达到极小下限时才投递, 避免无效微步空转; 远小于 0.05U 网格以保持连续
    if (this_tick >= EXT_BOLUS_MIN_UNITS) {
        motor_command_t cmd{0};
        cmd.type       = MOTOR_CMD_BOLUS_EXT;
        cmd.units_x100 = (uint32_t)(this_tick * 100.0f + 0.5f);
        cmd.kind       = g_pump_state.ext_bolus_kind;
        cmd.speed_hz   = slow_hz;
        if (motor_enqueue(&cmd)) {
            pump_state_consume_units(this_tick);
            uint32_t ux100 = (uint32_t)(this_tick * 100.0f + 0.5f);
            g_pump_state.today_units_x100           += ux100;
            g_pump_state.total_units_x100_delivered += ux100;
            // IOB 由 iob_record_extended_start 统一记录, iob_recompute 衰减 (此处不再累加)
            // 累计「实际投递量」(而非时间目标), 使下一拍 delta 自动对齐时间目标, 避免逐拍舍入漂移
            g_pump_state.ext_bolus_delivered_x100    += ux100;
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
            cmd.speed_hz   = slow_hz;
            if (motor_enqueue(&cmd)) {
                pump_state_consume_units(rem);
                uint32_t rx100 = (uint32_t)(rem * 100.0f + 0.5f);
                g_pump_state.today_units_x100           += rx100;
                g_pump_state.total_units_x100_delivered += rx100;
                // IOB 已由 iob_record_extended_start 记录 (延展量记录继续衰减, 此处不再累加)
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
    // IOB: 记录延展量开始, iob_recompute 按线性投递解析积分 (正确反映输注期间爬升+完成后衰减)
    iob_record_extended_start(units, g_pump_state.ext_bolus_duration_ms,
                              g_pump_state.ext_bolus_start_ms);
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
    // 调度器细拍 = min(延展量窗口, 基础率窗口): 延展量连续慢滴用细拍, 基础率仍按
    // BASAL_TICK_INTERVAL_MS(3 分钟)窗口每 basal_div 个细拍投递一次。
    const uint32_t loop_ms = (EXT_BOLUS_WINDOW_MS > 0 && EXT_BOLUS_WINDOW_MS < BASAL_TICK_INTERVAL_MS)
                                 ? EXT_BOLUS_WINDOW_MS : BASAL_TICK_INTERVAL_MS;
    const uint32_t basal_div = (loop_ms > 0) ? (BASAL_TICK_INTERVAL_MS / loop_ms) : 1;

    uint32_t tick = 0;
    for (;;) {
        float rate = basal_rate_for_now();
        g_pump_state.current_basal_rate = rate;   // 反映给状态屏/蓝牙

        // 基础率: 每 basal_div 个细拍投递一次 3 分钟窗口量
        if (tick % basal_div == 0) {
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
        }

        // ---- 方波/双波延展量: 细拍连续慢滴(每窗口按方波速率匀速走丝杠) ----
        extended_bolus_tick();

        // 周期重算 IOB (按活性曲线衰减), 写 g_pump_state.iob_x10000
        iob_recompute();

        tick++;
        vTaskDelay(pdMS_TO_TICKS(loop_ms));
    }
}
