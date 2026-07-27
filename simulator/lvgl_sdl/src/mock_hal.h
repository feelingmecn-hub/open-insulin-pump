/**
 * mock_hal.h — 外设桩 (INA226 / 电机 / BLE 模拟)
 *
 * PC/SDL 模拟器没有真实硬件, 这里用纯逻辑驱动 g_pump_state,
 * 让 UI 能演示不同状态机/报警/电量/电机电流的变化。
 *
 * 用法:
 *   mock_init()        — 启动时调用一次
 *   mock_tick(now_ms)  — 主循环每帧调用, 自动推进演示场景
 *   mock_event('a')    — 键盘触发特定状态/报警 (见 mock_hal.cpp)
 */
#pragma once
#include <cstdint>

void mock_init(void);
void mock_tick(uint32_t now_ms);
void mock_event(char c);

// ---- 演示数据读取接口 (模拟器专用, 不进固件) ----
float  mock_get_glucose_mmol(void);   // 当前血糖 mmol/L
int8_t mock_get_trend(void);          // -1 下降, 0 平稳, 1 上升
uint8_t mock_loop_mode(void);         // 0 闭环中, 1 开环, 2 暂停
bool   mock_loop_connected(void);
float  mock_get_tbr_percent(void);     // 0 = 无临时基础率
float  mock_get_tbr_rate(void);        // 临时基础率 U/h
float  mock_get_today_total(void);     // 今日总量 U
void   mock_get_clock(int *hh, int *mm);
int    mock_basal_count(void);         // 24
float  mock_basal_rate(int idx);       // 第 idx 段速率 U/h
void   mock_deliver_bolus(float units);// 触发一次大剂量演示
