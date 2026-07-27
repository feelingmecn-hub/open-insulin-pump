/**
 * basal_scheduler.h — 基础率周期调度器 (Arduino + FreeRTOS)
 *
 * 每 BASAL_TICK_INTERVAL_MS (默认 3 分钟) 计算当前应输注的基础率并
 * 入队 MOTOR_CMD_BASAL_TICK, 由 motor_controller 实际走丝杠。
 * 支持三种模式:
 *   - 本地档案 (loop_mode==1): 读 g_pump_config 当前方案的整点 slot
 *   - 闭环 (loop_mode==0):     用 BLE 下发的 current_basal_rate (AAPS 接管)
 *   - 暂停 (loop_mode==2):     0
 * 临时基础率 (TBR) 在有效期内的优先级最高。
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
