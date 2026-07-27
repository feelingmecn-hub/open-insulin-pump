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

// 消耗储药器药量 (带亚单位累加器, 避免 0.05U 反复丢失精度)
void pump_state_consume_units(float units);

// ============================================================
// 单位(U) ↔ 微步 统一换算  ← 全系统唯一换算入口
//   推导: 1 转 = 0.5mm(导程), 1 转 = 200 步 × 1/32 微步 = 6400 微步
//         储药器内径 4.5mm → 截面积 ≈15.9mm², 每微步推进 0.5/6400 mm
//         → 每微步体积 ≈1.24e-3 µL → 0.05U(U-100) = STEPS_PER_005U ≈402 微步
//   大剂量 / 基础率 / 排气 等所有「打药」路径都必须经这两个函数,
//   严禁各模块自行用 STEPS_PER_UNIT 现算, 避免精度/取整不一致。
// ============================================================
uint32_t units_to_microsteps(float units);   // 四舍五入到最近整数微步
float   microsteps_to_units(uint32_t steps); // 反算
// 将剂量吸附到 0.05U 最小精度网格 (大剂量命令级安全网)
float   quantize_units_005(float units);

// 给一份合理的内置默认基础率方案 (profile 0 = 0.5 U/h 全天)
void pump_config_apply_default_basal(pump_config_t *cfg);

// CRC-8 (CCITT) 实现
uint8_t crc8_ccitt(const uint8_t *data, size_t len);
