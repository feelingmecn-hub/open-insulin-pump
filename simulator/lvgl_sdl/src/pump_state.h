/**
 * pump_state.h — 全局运行状态与配置 (Arduino 框架)
 */
#pragma once

#include "pump_types.h"
#include "config.h"   // 引入 dosing.h: 换算 API 与全部几何常量 (单一真源) 对任何包含本头的 TU 可见

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
// 单位(U) ↔ 微步 统一换算 (与固件端共用 dosing.h 单一真源, 由 config.h 引入)
//   大剂量 / 基础率 / 排气 等所有「打药」路径都必须且只能经以下三函数,
//   0.05U(U-100) = STEPS_PER_005U 自动由 dosing.h 推导。
// ============================================================
