/**
 * basal_scheduler.cpp — 基础率周期调度器
 */
#include "basal_scheduler.h"
#include "config.h"
#include "pump_types.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "history_log.h"

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

        vTaskDelay(pdMS_TO_TICKS(BASAL_TICK_INTERVAL_MS));
    }
}
