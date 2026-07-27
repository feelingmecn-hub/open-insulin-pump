/**
 * pump_state.h — 全局运行状态与配置 (Arduino 框架)
 */
#pragma once

#include "pump_types.h"

// 实时状态 (所有任务读写, display 仅读)
extern pump_runtime_state_t g_pump_state;
// 持久配置 (Preferences)
extern pump_config_t g_pump_config;

void pump_state_init(void);

void pump_state_update_battery(uint16_t mv, uint8_t pct);
void pump_state_update_motor_current(uint16_t ma);
void pump_state_update_bus_power(uint16_t mw);
void pump_state_set_step_loss(bool lost);
void pump_state_set_alarm(alarm_code_t code);
void pump_state_clear_alarm(void);
void pump_state_set_state(pump_state_t s);

// CRC-8 (CCITT) 实现
uint8_t crc8_ccitt(const uint8_t *data, size_t len);

// ============================================================
// 单位(U) ↔ 微步 统一换算 (与固件端保持一致, 0.5mm/rev · 1/32 微步 · 9.65mm 内腔(标准3ml笔芯))
//   大剂量 / 基础率 / 排气 等所有「打药」路径都必须经这两个函数,
//   0.05U(U-100) = STEPS_PER_005U ≈88 微步。
// ============================================================
uint32_t units_to_microsteps(float units);   // 四舍五入到最近整数微步
float   microsteps_to_units(uint32_t steps); // 反算
float   quantize_units_005(float units);     // 吸附到 0.05U 最小精度网格
