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
// 设置第 idx 段基础率速率 U/h (本地档案模式可写; AAPS接管模式由 AAPS 下发)
void    ui_hal_basal_set_rate(int idx, float rate);
// 切换当前基础率方案 (0-3) (P2-10); 固件侧持久化, 模拟器为内存态
void    ui_hal_set_active_profile(uint8_t idx);
// 当前激活方案索引 (0-3)
uint8_t ui_hal_active_profile(void);
// 方案总数 (恒为 MAX_BASAL_PROFILES)
int     ui_hal_profile_count(void);
// 读取第 idx 套方案名称到 out (截断到 n-1)
void    ui_hal_profile_name(int idx, char *out, size_t n);
// 设置第 idx 套方案名称 (落盘)
void    ui_hal_profile_set_name(int idx, const char *name);
// 读取第 idx 套方案第 slot 段速率 U/h (直接读写 g_pump_config.profiles, 三后端一致)
float   ui_hal_profile_basal_rate(int idx, int slot);
// 设置第 idx 套方案第 slot 段速率 U/h (落盘, 吸附 0.1U 剂量网格)
void    ui_hal_profile_set_basal_rate(int idx, int slot, float rate);
// 复制第 src 套方案到 dst (名称+24段, 落盘)
void    ui_hal_profile_copy(int dst, int src);
// 重置第 idx 套方案为内置默认 (落盘)
void    ui_hal_profile_reset(int idx);
// ---- #260 基础率验证测试 ----
// 当前激活方案 24 段的全天总量 (U/天), 供确认页显示"将注射多少"
float   ui_hal_basal_daily_total(void);
// 立即把全天总量一次性打出 (走大剂量物理路径, 历史记 EVENT_TYPE_BASAL_TEST,
// 不计入大剂量次数/IOB)。返回实际下发量 U; 0 = 未下发(档案全 0/药量不足/队列满)
float   ui_hal_basal_run_test(void);
// 设置临时基础率(TBR): percent(0-500), duration_min(0=取消) (P2-9)
void    ui_hal_set_tbr(float percent, uint32_t duration_min);
// 取消临时基础率 (percent/rate 归零)
void    ui_hal_cancel_tbr(void);

// ---- TBR 历史记录回调钩子 (P2-9 补全: 泵菜单设/取 TBR 也必须进 AAPS 0xC2 回放) ----
// 背景: AAPS 只在 0xC2 历史回放里看到 TBR 事件。原实现只在 BLE 的 0x60/0xC1/0x62
//       handler 里调 aaps_dana_record_tbr, 而泵本地菜单(SCR_TBR)经 ui_hal_set_tbr/
//       cancel_tbr 设 TBR 时只写泵屏历史, 不喂 AAPS 回放缓冲 → AAPS 看不到菜单 TBR。
// 做法: HAL 层暴露一个钩子, 由 AAPS/Dana 协议层(aaps_dana.cpp)在初始化时注册;
//       ui_hal_set_tbr/cancel_tbr 在落状态时顺带触发钩子。HAL 不反向依赖协议层。
// 回调参数 code: 1=TEMP_START / 2=TEMP_STOP (必须与 aaps_dana DANA_HIST_CODE_TEMP_* 一致)
// 中性常量, 避免 HAL 反向依赖 Dana 协议层头文件。
#define UI_HAL_TBR_EVENT_START 1u   // = DANA_HIST_CODE_TEMP_START
#define UI_HAL_TBR_EVENT_STOP  2u   // = DANA_HIST_CODE_TEMP_STOP
typedef void (*ui_hal_tbr_hist_cb_t)(uint8_t code, uint16_t percent, uint16_t dur_min);
void    ui_hal_register_tbr_history_cb(ui_hal_tbr_hist_cb_t cb);

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

// 开始排气装药 (复位 + 充注); prime_u = 排气量(单位 U, 胰岛素泵以 U 计, 非 mL)
void ui_hal_start_prime(float prime_u);
// P3-14: 回退装药 (退活塞装新笔芯) + 剂量标定出测试量
void ui_hal_rewind(void);                       // 全退到尾部(后限位), 装新储药器
void ui_hal_rewind_units(float units);         // 手动退药: 按指定 U 退(用户自行判断退多少)
void ui_hal_calibrate_dispense(float units);   // 推出标定测试量(默认 1.0U) 供实测
void ui_hal_apply_calibration(float factor);  // 保存标定系数(实测/指令)并持久化
// 切换 本地档案 / AAPS接管 基础率模式
void ui_hal_toggle_basal_mode(void);
// 清除当前报警
void ui_hal_clear_alarm(void);
// 完全手动电机控制 (电机测试): dir 0=前进 1=后退; steps=0 表示连续点动直到停止;
// speed_hz=0 用默认低速。仅调试/电机测试用, 不记账(不改变储药器/IOB)。
void ui_hal_manual_move(uint8_t dir, uint32_t steps, uint16_t speed_hz);
// 停止正在进行的手动点动 (连续点动退出)
void ui_hal_manual_stop(void);
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
// 标记"用户/BLE 有活动", 用于空闲自动熄屏唤醒 (固件 lcd_display 实现; 模拟器为 no-op)
void    ui_hal_mark_activity(void);
bool    ui_hal_dana_paired(void);       // AAPS 是否完成 Dana 握手接管
bool    ui_hal_get_keypad_sound(void);  // 当前按键音开关
void    ui_hal_get_ymdhms(int *y, int *mo, int *d, int *h, int *mi, int *s); // 当前日期(未设置给默认)
float   ui_hal_get_board_temp_c(void);   // P3-13: 板载温度(°C), 固件=ESP32内置传感器, 模拟器=模拟值

// ---- 振动反馈 (P3-15) ----
// 当前原型无震动马达, 此处仅预留统一接口: 真机有马达时只需在 ui_hal_fw 内按
// VIBRATION_PIN 驱动 GPIO 即可, 调用点(ui_screen_key / 报警)无需改动。
typedef enum {
    VIB_NONE = 0,
    VIB_KEY,          // 按键反馈
    VIB_ALARM,        // 报警触发
    VIB_BOLUS_DONE,   // 大剂量/排气完成
    VIB_CONFIRM       // 确认/保存操作
} vib_pattern_t;

// 触发一次振动 (内部按 g_pump_config.vibrate_enabled 开关, 未开启直接返回)
void    ui_hal_vibrate(vib_pattern_t pat);
// 振动反馈开关读取/设置 (设置会持久化, 真机写 NVS, 模拟器为内存态)
bool    ui_hal_get_vibrate_enabled(void);
void    ui_hal_set_vibrate_enabled(bool on);

// 最近一次振动模式 (供联调快照观测; 0 = 无)。由各后端 ui_hal_vibrate 写入。
extern int g_last_vib_pat;

// 后端初始化 (固件侧可在此初始化缓存/默认值)
void ui_hal_init(void);
