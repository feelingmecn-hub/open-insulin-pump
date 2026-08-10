/**
 * basal_scheduler.h — 基础率周期调度器 (Arduino + FreeRTOS)
 *
 * 每 BASAL_TICK_INTERVAL_MS (默认 3 分钟) 计算当前应输注的基础率并
 * 入队 MOTOR_CMD_BASAL_TICK, 由 motor_controller 实际走丝杠。
 * 基础率真源 (2026-08-08 修正, 对齐真实 Dana-i 行为):
 *   - 闭环 (loop_mode==0) 与 开环 (loop_mode==1) **都**读
 *     g_pump_config.profiles[active_profile].slots[当前墙钟整点].rate_uh。
 *     AAPS 用 Dana 0x66 下发 24 段档案写进这里, 泵自己跑; AAPS 只用 TBR/大剂量做增量。
 *     (旧实现闭环时读 g_pump_state.current_basal_rate —— 那个字段 AAPS 根本不写,
 *      导致"设了基础率电机纹丝不动", 已修复。)
 *   - 暂停 (loop_mode==2): 0
 *   - 伴生 App BASAL 通道直推的速率 = 限时覆盖 (BASAL_OVERRIDE_TIMEOUT_MS 后回落档案)
 *   - 临时基础率 (TBR) 在有效期内优先级最高
 */
#pragma once

#include "pump_types.h"

void basal_scheduler_init(void);
void basal_scheduler_task(void *arg);

// 方波/双波延展量: 按 duration_h 时间维在 basal_scheduler tick 中铺开
// (取代原先"延展量作一次性第二条大剂量入队"的做法, 实现真正的时间维铺开)
void basal_scheduler_start_extended_bolus(float units, float duration_h, uint8_t kind);
void basal_scheduler_cancel_extended_bolus(void);
bool basal_scheduler_extended_bolus_active(void);

// ============================================================
// #260 基础率验证测试 (用户需求: "把所有时段总和加起来执行打一次",
//      历史里能明确看到是否生效, 并可对照电机移动距离核验)
// ============================================================

// 当前激活方案 24 段的全天总量 (U/天)。每段为 1 小时, 故直接累加 U/h 即得 U。
float basal_scheduler_daily_total(void);

// 立即入队一次"基础率验证测试注射": 把 basal_scheduler_daily_total() 一次性打出。
//   · 受单次大剂量上限与剩余药量双重钳制 (配置为 0 时兜底为编译期常量)
//   · 剂量吸附到整数微步后才发, 记账/历史一律用实际打出量
//   · 历史记为 EVENT_TYPE_BASAL_TEST + basal_history 的 BH_BASAL_TEST, 不计入大剂量/IOB
// 返回实际将投递的量 (U); 返回 0 表示未投递 (档案全 0 / 药量不足 / 队列满)。
float basal_scheduler_run_daily_test(void);
