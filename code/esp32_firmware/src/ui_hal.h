/**
 * ui_hal.h — UI 与硬件解耦抽象层接口
 *
 * 目的: 让同一份 ui_screen.cpp 既能跑在 PC/SDL 模拟器(后端=ui_hal_sim.cpp,
 *       数据来自 mock_hal), 也能跑在 ESP32 真机(后端=ui_hal_fw.cpp, 数据来自
 *       真实模块与 g_pump_state)。ui_screen 只依赖本接口, 不感知后端来源。
 *
 * 两类接口:
 *   1) 数据读取: UI 绘制时调用, 返回当前显示所需的值。
 *   2) 动作:    菜单确认时调用, 在真机上真实生效(电机/报警/背光...)。
 *
 * 两个后端分别在 simulator/lvgl_sdl/src/ui_hal_sim.cpp 与
 * code/esp32_firmware/src/ui_hal_fw.cpp 实现。
 */
#pragma once

#include <cstdint>
#include "pump_types.h"

// ============================================================
// 1. 数据读取 (UI 显示用)
// ============================================================

// 当前血糖 mmol/L (来自 CGM / AAPS 回传)
float   ui_hal_glucose_mmol(void);
// 血糖趋势: -2 速降 / -1 缓降 / 0 平稳 / +1 缓升 / +2 速升 (5 档)
int8_t  ui_hal_glucose_trend(void);
// 血糖是否有效 (有数据且未过期): false → UI 显"CGM 离线/无数据"
bool    ui_hal_glucose_valid(void);
// 闭环模式: 0 闭环(AAPS接管) / 1 开环(本地档案) / 2 暂停
uint8_t ui_hal_loop_mode(void);
// AAPS / 手机 连接状态
bool    ui_hal_loop_connected(void);
// 临时基础率百分比 (0 = 无 TBR)
float   ui_hal_tbr_percent(void);
// 临时基础率速率 U/h
float   ui_hal_tbr_rate(void);
// 今日累计注射总量 U
float   ui_hal_today_total(void);
// 设备时钟 (24h)
void    ui_hal_get_clock(int *hh, int *mm);
// 基础率段数 (通常为 24)
int     ui_hal_basal_count(void);
// 第 idx 段基础率速率 U/h
float   ui_hal_basal_rate(int idx);
// 是否本地档案模式 (true=本地基础率方案, false=AAPS接管)
bool    ui_hal_basal_local_mode(void);

// ============================================================
// 2. 动作 (菜单触发, 真实生效)
// ============================================================

typedef enum {
    BOLUS_NORMAL = 0,   // 常规大剂量
    BOLUS_SQUARE,       // 方波大剂量
    BOLUS_DUAL,         // 双波大剂量
    BOLUS_WIZARD,       // 向导大剂量
    BOLUS_MEAL          // 三餐预设
} bolus_kind_t;

/**
 * 触发一次大剂量。
 * @param total_units    总剂量 U (立即量 + 延展量)
 * @param kind           大剂量类型
 * @param duration_h     方波/双波时长(小时), 常规/向导/三餐为 0
 * @param immediate_units 立即输注量 U (常规=全部, 双波=立即部分)
 * @param extended_units  延展(方波)量 U (常规=0, 双波=方波部分)
 */
void ui_hal_deliver_bolus(float total_units, bolus_kind_t kind,
                          float duration_h, float immediate_units, float extended_units);

// 开始排气装药 (复位 + 充注)
void ui_hal_start_prime(void);
// 切换 本地档案 / AAPS接管 基础率模式
void ui_hal_toggle_basal_mode(void);
// 清除当前报警
void ui_hal_clear_alarm(void);
// 设置背光亮度 0-100 (%)
void ui_hal_set_brightness(uint8_t pct);
// 切换按键音开关 (返回切换后的状态)
bool ui_hal_toggle_keypad_sound(void);

// 大剂量是否正在进行 (供 UI 显示"注射中"与"ESC 取消")
bool ui_hal_bolus_active(void);
// 取消正在进行的大剂量 (只损失已打部分, 剩余停止)
void ui_hal_cancel_bolus(void);

// ---- 时钟 (RTC 风格) ----
// 时钟是否已设置 (false = 显示"未设置")
bool    ui_hal_clock_valid(void);
// 设置时间 (Unix 秒)
void    ui_hal_set_time(uint32_t unix_sec);
// 设置时间 (日历)
void    ui_hal_set_time_ymdhms(int y, int mo, int d, int h, int mi, int s);

// ---- 显示 / 设置读取 ----
uint8_t ui_hal_get_brightness(void);    // 当前背光亮度 0-100
bool    ui_hal_dana_paired(void);       // AAPS 是否完成 Dana 握手接管
bool    ui_hal_get_keypad_sound(void);  // 当前按键音开关
void    ui_hal_get_ymdhms(int *y, int *mo, int *d, int *h, int *mi, int *s); // 当前日期(未设置给默认)

// 后端初始化 (固件侧可在此初始化缓存/默认值)
void ui_hal_init(void);
