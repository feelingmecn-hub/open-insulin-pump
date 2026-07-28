/**
 * mock_hal.cpp — 外设桩 (INA226 / 电机 / BLE / CGM / 闭环 模拟)
 *
 * 不碰任何真实硬件, 仅修改 g_pump_state + 演示数据, 让 UI 能展示:
 *   - 主状态屏: CGM 血糖+趋势, 泵状态(基础率/剩余药量/今日总量/IOB/电量)
 *   - 闭环页: AAPS 连接, 临时基础率(TBR)
 *   - 基础率页: 24 段档案
 *   - 时钟
 * 自动演示 + 键盘事件两种模式。
 */
#include "mock_hal.h"
#include "pump_state.h"
#include "pump_types.h"
#include "config.h"

#include <cmath>

static uint32_t g_start_ms = 0;
static bool     g_override = false;

// ---- 演示数据 ----
static float  g_basal[24];
static float  g_glucose = 6.5f;   // mmol/L
static int8_t g_trend   = 0;      // -2 速降 / -1 缓降 / 0 平稳 / +1 缓升 / +2 速升
static float  g_today   = 12.5f;  // 今日总量 U
static int    g_hh = 8, g_mm = 30;
static float  g_tbr_pct   = 0;    // 临时基础率百分比 (0=无)
static float  g_tbr_rate  = 0;    // 临时基础率 U/h

void mock_init(void)
{
    g_start_ms  = 0;
    g_override  = false;
    pump_state_init();

    // 24 段基础率示例 (夜间低, 白天高)
    for (int i = 0; i < 24; i++) g_basal[i] = 0.60f;
    g_basal[6] = 0.90f; g_basal[7] = 1.00f; g_basal[8] = 1.00f;
    g_basal[11] = 1.10f; g_basal[12] = 1.00f; g_basal[13] = 0.95f;
    g_basal[18] = 0.90f; g_basal[19] = 0.80f; g_basal[22] = 0.70f;

    g_glucose = 6.5f; g_trend = 0; g_today = 12.5f;
    g_hh = 8; g_mm = 30; g_pump_state.loop_mode = 0; g_tbr_pct = 0; g_tbr_rate = 0;
}

void mock_tick(uint32_t now_ms)
{
    if (g_start_ms == 0) g_start_ms = now_ms;
    const uint32_t el = now_ms - g_start_ms;

    if (!g_override) {
        if (el < 2000) {
            pump_state_set_state(PUMP_STATE_BOOTING);
            return;
        }

        pump_state_set_state(PUMP_STATE_BASAL);
        g_pump_state.is_primed = true;

        // 当前时段基础率
        int hr = g_hh % 24;
        g_pump_state.current_basal_rate = g_basal[hr];

        // 电量: 每 30s 掉 1%
        int pct = 100 - (int)(el / 30000);
        if (pct < 0) pct = 0;
        g_pump_state.battery_pct = (uint8_t)pct;
        g_pump_state.battery_mv = (uint16_t)(8400 + pct * (12600 - 8400) / 100);

        // 电机电流: 2s 方波
        bool running = ((el / 2000) % 2) == 0;
        g_pump_state.motor_current_ma = running ? MOTOR_RUN_CURRENT_MA : STALL_NOLOAD_MA;
        g_pump_state.bus_power_mw = (uint16_t)(g_pump_state.motor_current_ma * 11 + 60);

        // 储药器: 缓慢消耗
        float used = (el / 3600000.0f) * 0.80f;
        int left = (int)(MAX_RESERVOIR_UNITS - used);
        if (left < 0) left = 0;
        g_pump_state.reservoir_units_left = (uint16_t)left;

        // IOB: 每 90s 来一次 2U 大剂量, 缓慢衰减
        static uint32_t last_bolus = 0;
        if (el - last_bolus > 90000) { last_bolus = el; g_pump_state.iob_x10000 += 20000; }
        if (g_pump_state.iob_x10000 > 0) g_pump_state.iob_x10000 -= 1;

        // 时钟: 演示加速 (1 真实秒 = 6 模拟秒)
        uint32_t simsec = (uint32_t)(el / 1000 * 6) + (8 * 3600 + 30 * 60);
        g_hh = (int)(simsec / 3600) % 24;
        g_mm = (int)(simsec / 60) % 60;

        // CGM: 5 分钟周期正弦波动
        float prev = g_glucose;
        g_glucose = 6.5f + 1.2f * sinf((float)el / 60000.0f * 3.14159f * 2.0f / 5.0f);
        // 由斜率映射为 5 档显示码 (-2..2), 与固件 ui_hal_glucose_trend 契约一致
        float slope = g_glucose - prev;
        if (slope > 0.10f)       g_trend = 2;
        else if (slope > 0.03f)  g_trend = 1;
        else if (slope < -0.10f) g_trend = -2;
        else if (slope < -0.03f) g_trend = -1;
        else                     g_trend = 0;
        g_pump_state.last_glucose_mgdl = (uint16_t)(g_glucose * 18.0f);
        g_pump_state.glucose_trend = g_trend;

        // 闭环
        g_pump_state.ble_connected = true;
        if (g_pump_state.loop_mode == 0) {            // 闭环中: 偶尔给一个临时基础率演示
            g_tbr_pct = (sinf((float)el / 120000.0f) > 0.6f) ? 130.0f : 0.0f;
        } else {
            g_tbr_pct = 0;
        }
        g_tbr_rate = g_basal[hr] * (g_tbr_pct / 100.0f);
    }
}

// ------------------------------------------------------------
// Getter (供 UI 读取演示数据)
// ------------------------------------------------------------
float  mock_get_glucose_mmol(void) { return g_glucose; }
int8_t mock_get_trend(void)        { return g_trend; }
uint8_t mock_loop_mode(void)       { return g_pump_state.loop_mode; }
bool   mock_loop_connected(void)   { return g_pump_state.ble_connected; }
float  mock_get_tbr_percent(void)  { return g_tbr_pct; }
float  mock_get_tbr_rate(void)     { return g_tbr_rate; }
float  mock_get_today_total(void)  { return g_today; }
void   mock_get_clock(int *hh, int *mm) { *hh = g_hh; *mm = g_mm; }
int    mock_basal_count(void)      { return 24; }
float  mock_basal_rate(int idx)    { return (idx >= 0 && idx < 24) ? g_basal[idx] : 0; }

void mock_deliver_bolus(float units)
{
    g_override = true;
    pump_state_set_state(PUMP_STATE_BOLUS);
    g_pump_state.iob_x10000 += (uint32_t)(units * 10000);
    g_today += units;
    g_pump_state.motor_current_ma = MOTOR_RUN_CURRENT_MA;
    g_pump_state.bus_power_mw = (uint16_t)(g_pump_state.motor_current_ma * 11 + 60);
}

// ------------------------------------------------------------
// 键盘事件 — 手动触发特定状态/报警, 用于定向测试 UI
//   a  触发报警(泵卡住)   c  清除报警        s  切换丢步
//   b  大剂量+1U          i  IDLE           p  PRIMING
//   e  ERROR              r  恢复自动演示    l  切换闭环模式
// ------------------------------------------------------------
void mock_event(char c)
{
    g_override = true;
    switch (c) {
        case 'a': pump_state_set_alarm(ALARM_PUMP_STALLED); break;
        case 'c': pump_state_clear_alarm(); break;
        case 's': pump_state_set_step_loss(!g_pump_state.step_loss_detected); break;
        case 'b': mock_deliver_bolus(1.0f); break;
        case 'i': pump_state_clear_alarm(); pump_state_set_state(PUMP_STATE_IDLE); break;
        case 'p': pump_state_set_state(PUMP_STATE_PRIMING); break;
        case 'e': pump_state_set_state(PUMP_STATE_ERROR); break;
        case 'l': g_pump_state.loop_mode = (uint8_t)((g_pump_state.loop_mode + 1) % 3); break;
        case 'r': g_override = false; break;
        default: break;
    }
}
