/**
 * pump_state.h — 全局运行状态与配置 (Arduino 框架)
 */
#pragma once

#include "pump_types.h"
#include "config.h"   // 引入 dosing.h: 换算 API 与全部几何常量 (单一真源) 对任何包含本头的 TU 可见
#include <cstdio>     // snprintf (pump_config_reset_profile 内联定义需用)

// 实时状态 (所有任务读写, display 仅读)
extern pump_runtime_state_t g_pump_state;
// 持久配置 (Preferences)
extern pump_config_t g_pump_config;

void pump_state_init(void);

void pump_state_update_battery(uint16_t mv, uint8_t pct);
void pump_state_update_motor_current(uint16_t ma);
void pump_state_update_motor_current_peak(uint16_t ma);   // 峰值电流(单次运动期间)
void pump_state_update_bus_power(uint16_t mw);
void pump_state_set_step_loss(bool lost);
void pump_state_set_alarm(alarm_code_t code);
void pump_state_clear_alarm(void);
void pump_state_set_state(pump_state_t s);

// P3-13 过温检测: 周期性调用, 读板温并与阈值比较(预警/报警状态机)
void thermal_periodic(void);

// 消耗储药器药量 (带亚单位累加器, 避免 0.1U 以下小数反复 floor 丢失精度)
void pump_state_consume_units(float units);

// ============================================================
// 单位(U) ↔ 微步 统一换算  ← 全系统唯一换算入口
//   换算算法与全部几何推导集中在 dosing.h (单一真源), 由 config.h 自动引入,
//   不再于本文件重复声明/定义。大剂量 / 基础率 / 排气 等所有「打药」路径都必须
//   且只能经 units_to_microsteps() / microsteps_to_units() / quantize_units_grid()
//   三函数, 严禁各模块自行用 STEPS_PER_UNIT 现算, 避免精度/取整不一致。
// ============================================================

// 给一份合理的内置默认基础率方案 (profile 0 = 0.5 U/h 全天)
void pump_config_apply_default_basal(pump_config_t *cfg);
// 仅重置第 p 套方案为内置默认 (名称+24段), 不触碰其余方案
// 定义为 static inline: 与 pump_config_apply_default_factors 同理, 固件与主机联调桩
// (host_glue.cpp / 模拟器 ui_hal_sim / ui_hal_link) 各自拥有一份副本, 避免链接缺失。
static inline void pump_config_reset_profile(pump_config_t *cfg, int p)
{
    if (!cfg || p < 0 || p >= MAX_BASAL_PROFILES) return;
    const float def_rate = 0.5f;   // U/h, 全天恒定
    snprintf(cfg->profiles[p].name, sizeof(cfg->profiles[p].name),
             p == 0 ? "默认" : "方案%d", p + 1);
    for (int i = 0; i < BASAL_SLOTS_PER_DAY; i++) {
        cfg->profiles[p].slots[i].hour    = (uint8_t)i;
        cfg->profiles[p].slots[i].rate_uh = def_rate;
    }
}

// 给一份合理的闭环参数默认 (ISF / 碳水比 / 目标血糖), 避免向导计算除以 0 (P1-8)
// 定义为 static inline: 固件(pump_state.cpp)与主机联调桩(host_glue.cpp, 不链接 pump_state.cpp)
// 都能各自拥有一份副本, 避免重复符号 / 链接缺失。
static inline void pump_config_apply_default_factors(pump_config_t *cfg)
{
    if (!cfg) return;
    // 默认闭环参数 (mg/dL 体系, 与 AAPS 口径一致):
    //   ISF=40 mg/dL/U, CR=10 g/U, 目标血糖=100 mg/dL (全天恒定, 可在 UI/App 调整)
    for (int i = 0; i < 24; i++) {
        cfg->isf[i]            = 40.0f;
        cfg->carb_ratio[i]     = 10.0f;
        cfg->target_glucose[i] = 100;
    }
}

// CRC-8 (CCITT) 实现
uint8_t crc8_ccitt(const uint8_t *data, size_t len);
