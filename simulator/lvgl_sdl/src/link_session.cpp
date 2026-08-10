/* link_session.cpp — 模拟器联调模式的脚本化会话播放器 (实现见 link_session.h)
 *
 * 移植自 test/aaps_link_sim.cpp 的 17 步完整闭环会话。每一步直接驱动真实
 * 固件命令分发代码, 实时改写 g_pump_state —— 模拟器泵屏幕每帧从 g_pump_state
 * 重绘, 画面与命令严格同步。
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#include "link_session.h"

#include "aaps_dana.h"      // 真实协议常量 + 信封 API
#include "pump_state.h"     // g_pump_state / g_pump_config
#include "pump_types.h"     // PUMP_STATE_*
#include "rtc_clock.h"      // rtc_clock_init / rtc_unix_now / rtc_is_set / rtc_unix_to_ymdhms
#include "motor_controller.h"   // motor_bolus_active
#include "basal_scheduler.h"    // basal_scheduler_extended_bolus_active
#include "host_glue.h"      // 假硬件层 + TX 捕获 + host_tx_set/clear_ble5
#include "NimBLEDevice.h"   // 假 NimBLE (aaps_dana_attach 所需)
#include "config.h"
#include "ui_screen.h"   // ui_screen_dump_json (联调控制面板手动操控显示)
#include "ui_hal.h"       // g_last_vib_pat (P3-15 振动观测)
#include "dosing.h"        // units_to_microsteps (电机演示: 已发药 -> 微步)

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

/* 受控字符串 -> JSON 双引号包裹 (内容均为 ASCII/中文, 不含 " \ 与控制字符)。
 * 用 4 槽环形静态缓冲, 允许单次 snprintf 内最多 4 个 json_str 参数并存。 */
static const char *json_str(const char *s)
{
    static char bufs[4][256];
    static int  slot = 0;
    char *buf = bufs[(slot++) & 3];
    int n = (int)strlen(s);
    if (n > 250) n = 250;
    buf[0] = '"';
    memcpy(buf + 1, s, (size_t)n);
    buf[n + 1] = '"';
    buf[n + 2] = '\0';
    return buf;
}

/* ============================================================
 * 协议轨迹 (供 GUI 的"AAPS 发送" / "固件接收" 两个调试窗共用)
 * ============================================================ */
static std::vector<LinkTrace> g_trace;
static std::string            g_last_resp_hex;
static bool                   g_last_rx_ok = false;

static std::string to_hex(const uint8_t *p, size_t n)
{
    static const char *h = "0123456789ABCDEF";
    std::string s;
    char buf[4];
    for (size_t i = 0; i < n; i++) {
        buf[0] = h[p[i] >> 4];
        buf[1] = h[p[i] & 0xF];
        buf[2] = (i + 1 < n) ? ' ' : '\0';
        buf[3] = '\0';
        s += buf;
    }
    return s;
}

static std::string op_name(uint8_t op)
{
    switch (op) {
        case DANA_OP_PUMP_CHECK:      return "PUMP_CHECK";
        case DANA_OP_TIME_INFO:       return "TIME_INFO";
        case DANA_CMD_GET_TIME:       return "GET_TIME";
        case DANA_CMD_SET_TIME:       return "SET_TIME";
        case DANA_CMD_INITIAL_SCREEN: return "INITIAL_SCREEN";
        case DANA_CMD_APS_TBR:        return "APS_TBR";
        case DANA_CMD_SET_TBR:        return "SET_TBR";
        case DANA_CMD_CANCEL_TBR:     return "CANCEL_TBR";
        case DANA_CMD_STEP_BOLUS_START: return "STEP_BOLUS_START";
        case DANA_CMD_STEP_BOLUS_STOP:  return "STEP_BOLUS_STOP";
        case DANA_CMD_EXT_BOLUS:      return "EXT_BOLUS";
        case DANA_CMD_EXT_BOLUS_CANCEL: return "EXT_BOLUS_CANCEL";
        case DANA_CMD_DAILY_TOTAL:    return "DAILY_TOTAL";
        case DANA_CMD_SET_DUAL_BOLUS: return "SET_DUAL_BOLUS";
        default: { char b[16]; snprintf(b, sizeof(b), "OP_0x%02X", op); return b; }
    }
}

static std::string dana_intent(uint8_t op, const uint8_t *p, uint8_t n)
{
    char b[128];
    switch (op) {
        case DANA_OP_PUMP_CHECK: return "握手 PUMP_CHECK (对端身份校验)";
        case DANA_OP_TIME_INFO:  return "握手 TIME_INFO (下发 BLE5 密钥, 完成加密握手)";
        case DANA_CMD_GET_TIME:  return "读取泵时间";
        case DANA_CMD_SET_TIME:
            if (n >= 6) snprintf(b, sizeof(b), "设置泵时间 -> 20%02d-%02d-%02d %02d:%02d:%02d",
                                 p[0], p[1], p[2], p[3], p[4], p[5]);
            else return "设置泵时间";
            return b;
        case DANA_CMD_INITIAL_SCREEN: return "查询初始屏幕 (状态页 17B)";
        case DANA_CMD_APS_TBR:
            if (n >= 1) snprintf(b, sizeof(b), "APS 闭环临时基础率 %d%%", (int)p[0]);
            else return "APS 闭环临时基础率";
            return b;
        case DANA_CMD_SET_TBR:
            if (n >= 1) snprintf(b, sizeof(b), "手动临时基础率 %d%%", (int)p[0]);
            else return "手动临时基础率";
            return b;
        case DANA_CMD_CANCEL_TBR: return "取消临时基础率";
        case DANA_CMD_STEP_BOLUS_START: {
            int ux100 = ((int)p[0] << 8) | (int)p[1];
            snprintf(b, sizeof(b), "步进大剂量 %.2fU", ux100 / 100.0);
            return b;
        }
        case DANA_CMD_STEP_BOLUS_STOP: return "停止大剂量";
        case DANA_CMD_EXT_BOLUS: {
            int ux100 = ((int)p[0] << 8) | (int)p[1];
            float dur = (n >= 3) ? p[2] / 2.0f : 0;
            snprintf(b, sizeof(b), "方波大剂量 %.2fU / %.1fh", ux100 / 100.0, dur);
            return b;
        }
        case DANA_CMD_EXT_BOLUS_CANCEL: return "取消方波大剂量";
        case DANA_CMD_DAILY_TOTAL: return "查询今日总量";
        case DANA_CMD_SET_DUAL_BOLUS: return "双波大剂量 0x48 (教学原型返回 OK, 不污染血糖)";
        default: return "";
    }
}

static std::string action_summary(uint8_t op)
{
    switch (op) {
        case DANA_OP_PUMP_CHECK: return "完成握手 PUMP_CHECK (回 OK + HW_MODEL)";
        case DANA_OP_TIME_INFO:  return "完成握手 TIME_INFO -> 二级加密启用, dana_paired=true";
        case DANA_CMD_GET_TIME:  return "读取硬件 RTC 时间并回传";
        case DANA_CMD_SET_TIME:  return "设置硬件 RTC (rtc_clock 写入)";
        case DANA_CMD_INITIAL_SCREEN: return "回传 17B 状态页 (Dana 标准布局)";
        case DANA_CMD_APS_TBR:   return "设置闭环临时基础率 (改 tbr_*, 不触发电机)";
        case DANA_CMD_SET_TBR:   return "设置手动临时基础率 (改 tbr_*)";
        case DANA_CMD_CANCEL_TBR: return "取消临时基础率 (tbr_percent=0)";
        case DANA_CMD_STEP_BOLUS_START: return "motor_enqueue() 真正下发大剂量 (走 dosing.h 单一换算)";
        case DANA_CMD_STEP_BOLUS_STOP:  return "motor_cancel_bolus() 取消进行中的大剂量";
        case DANA_CMD_EXT_BOLUS: return "basal_scheduler 启动方波 (匀速慢滴铺开)";
        case DANA_CMD_EXT_BOLUS_CANCEL: return "basal_scheduler 取消方波";
        case DANA_CMD_DAILY_TOTAL: return "回传今日累计总量";
        case DANA_CMD_SET_DUAL_BOLUS: return "收到 0x48(双波) -> default 分支返回 OK (安全: 不污染血糖)";
        default: return "分发到对应处理分支";
    }
}

/* 测试钩子 (aaps_dana.cpp 在 AAPS_DANA_HOST_TEST 下定义) */
extern "C" void aaps_dana_feed_rx_test(const uint8_t *data, size_t len);
extern "C" void aaps_dana_reset_test(void);

namespace linksess {

/* 前向声明: 以下函数在文件后部 (AAPS 蓝牙客户端段) 定义; replay 数据模拟引擎
 * 在上方较早就调用它们, 需提前声明以满足 C++ "先声明后使用" 规则。 */
static int  send_cmd(uint8_t opcode, const uint8_t *params, uint8_t nparams);
static int  feed_and_recv(const uint8_t *pkt, size_t plen, bool expect_resp);
static void setup_state(void);

static const int TOTAL = 17;

/* ---------- 对端(AAPS)上下文, 镜像固件握手状态 ---------- */
static dana_ctx_t   c_peer;
static NimBLEServer g_fake_server;   // 持久存在, aaps_dana_attach 存其指针

/* 最近一次响应解析结果 */
static uint8_t  g_rt_type = 0, g_rt_op = 0;
static uint8_t  g_rt_params[64] = {0};
static size_t   g_rt_nparams = 0;

/* 会话状态 */
static std::vector<LinkStep> g_steps;
static bool  g_playing = false;
static int   g_delay_ms = 900;     // 自动播放时每步间隔
static std::mutex g_mtx;

/* ============================================================
 * 数据驱动自由运行 (replay) — 用户载入血糖曲线 + 基础率档案
 * ============================================================ */
struct MealEvt { float t_min; float dose; bool fired; };
static bool                     g_has_dataset = false;
static int                      g_mode = 0;          // 0=脚本 1=数据模拟
static float                    g_sim_min = 0;       // 当前模拟时间(分钟)
static float                    g_replay_dur = 1440; // 总时长(分钟)
static float                    g_speedup = 120;     // 模拟分钟 / 真实秒
static std::vector<std::pair<float,float>> g_glu;    // (t_min, mgdl)
static float                    g_basal[24] = {0.5f};
static std::vector<MealEvt>     g_meals;             // 餐时大剂量
static std::vector<std::pair<float,float>> g_iob;    // (投递剂量U, 投递时刻sim_min)
static float                    g_last_bolus_min = -999.0f;
static bool                     g_suspended = false;
static bool                     g_replay_inited = false;
static float                    g_last_iob_record = -999.0f;

// ---- 极简 JSON 数组解析 (仅解析本项目自有 schema, 字段均为受控数字) ----
static void parse_floats(const char *json, const char *key, float *out, int maxn, int *cnt)
{
    *cnt = 0;
    const char *p = strstr(json, key);
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    const char *q = p + 1;
    while (*q && *q != ']' && *cnt < maxn) {
        while (*q && (*q < '0' || *q > '9') && *q != '-' && *q != '.') {
            if (*q == ']') return; q++;
        }
        if (!*q || *q == ']') return;
        char *e = nullptr;
        float v = (float)strtod(q, &e);
        if (e == q) return;
        q = e;
        out[(*cnt)++] = v;
    }
}

static void parse_pairs(const char *json, const char *key,
                        std::vector<std::pair<float,float>> &out, int maxn)
{
    out.clear();
    const char *p = strstr(json, key);
    if (!p) return;
    p = strstr(p, "[[");
    if (!p) return;
    /* 括号深度扫描: 从 "[["(depth=2) 起, 每个 '[' +1 / ']' -1;
     * depth 归零即到达本数组结束的 "]]", 立即停止 —— 避免把紧随其后的
     * 其它数组(如 meals)误当作本数组的 pair 继续解析。 */
    const char *q = p + 2;
    int depth = 2;
    int cnt = 0;
    while (*q && cnt < maxn) {
        // 跳到第一个数的起点 (同时维护括号深度)
        while (*q) {
            if (*q == '[') { depth++; q++; continue; }
            if (*q == ']') { depth--; q++; if (depth <= 0) return; continue; }
            if (((*q >= '0' && *q <= '9') || *q == '-' || *q == '.')) break;
            q++;
        }
        if (!*q || depth <= 0) return;
        char *e1 = nullptr; float a = (float)strtod(q, &e1); if (e1 == q) return; q = e1;
        // 跳到第二个数的起点 (维护深度)
        while (*q) {
            if (*q == '[') { depth++; q++; continue; }
            if (*q == ']') { depth--; q++; if (depth <= 0) return; continue; }
            if (((*q >= '0' && *q <= '9') || *q == '-' || *q == '.')) break;
            q++;
        }
        if (!*q || depth <= 0) return;
        char *e2 = nullptr; float b = (float)strtod(q, &e2); if (e2 == q) return; q = e2;
        out.push_back({a, b}); cnt++;
    }
}

static float interpolate_glucose(float t)
{
    if (g_glu.empty()) return 0.0f;
    if (t <= g_glu.front().first) return g_glu.front().second;
    if (t >= g_glu.back().first)  return g_glu.back().second;
    for (size_t i = 1; i < g_glu.size(); i++) {
        if (t <= g_glu[i].first) {
            float t0 = g_glu[i-1].first, v0 = g_glu[i-1].second;
            float t1 = g_glu[i].first,   v1 = g_glu[i].second;
            float f = (t1 == t0) ? 0.0f : (t - t0) / (t1 - t0);
            return v0 + (v1 - v0) * f;
        }
    }
    return g_glu.back().second;
}

static void record_iob(float units, float at_min)
{
    if (units <= 0) return;
    g_iob.push_back({units, at_min});
    if (g_iob.size() > 256) g_iob.erase(g_iob.begin());   // 防止无限增长
}

static void recompute_iob(void)
{
    const float DIA = 240.0f;   // 胰岛素活性时长(min) — 与 config IOB_DURATION_HOURS=4h 一致
    float iob = 0;
    for (auto &e : g_iob) {
        float t = g_sim_min - e.second;
        if (t < 0) t = 0;
        float frac = (t >= DIA) ? 0.0f : (1.0f - t / DIA);  // Walsh 三角衰减(线性)
        iob += e.first * frac;
    }
    g_pump_state.iob_x10000 = (uint32_t)(iob * 10000.0f + 0.5f);
}

// 大头剂量字节: {低字节, 高字节, 0} (与 17步 step10 一致)
static void bolus_bytes(float dose, uint8_t out[3])
{
    int ux = (int)(dose * 100.0f + 0.5f);
    out[0] = (uint8_t)(ux & 0xFF);
    out[1] = (uint8_t)((ux >> 8) & 0xFF);
    out[2] = 0;
}

// 数据模拟开始: 先做 Dana 握手(建立加密通道), 复位泵状态
static void replay_handshake(void)
{
    aaps_dana_reset_test();
    host_tx_clear_ble5();
    c_peer.conn = DANA_CONN_INIT;
    setup_state();                       // 复位 g_pump_state + dana 上下文
    uint8_t pkt[64]; size_t pl = 0;
    dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_ENC_REQ, DANA_OP_PUMP_CHECK, NULL, 0, 0);
    feed_and_recv(pkt, pl, true);
    c_peer.conn = DANA_CONN_PUMP_CHECK;
    dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_ENC_REQ, DANA_OP_TIME_INFO, NULL, 0, 0);
    feed_and_recv(pkt, pl, true);
    c_peer.conn = DANA_CONN_HANDSHAKE_DONE;
    host_tx_set_ble5(c_peer.ble5_enc_key);   // TX 侧启用二级加密(与 17步一致)
    g_pump_state.dana_paired = true;
    g_pump_state.last_glucose_mgdl = 0;
    g_pump_state.tbr_percent = 0;
    g_pump_state.tbr_rate = 0;
    g_pump_state.current_state = (uint8_t)PUMP_STATE_BASAL;
}

void set_mode(int m)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_mode = (m == 1) ? 1 : 0;
}

int mode(void) { std::lock_guard<std::mutex> lk(g_mtx); return g_mode; }

float sim_min(void) { std::lock_guard<std::mutex> lk(g_mtx); return g_sim_min; }

bool has_dataset(void) { std::lock_guard<std::mutex> lk(g_mtx); return g_has_dataset; }

void load_dataset(const char *json)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_glu.clear(); g_meals.clear(); g_iob.clear();
    for (int i = 0; i < 24; i++) g_basal[i] = 0.5f;
    int bc = 0;
    parse_floats(json, "\"basal\"", g_basal, 24, &bc);
    parse_pairs(json, "\"glucose\"", g_glu, 20000);
    // meals
    std::vector<std::pair<float,float>> mp;
    parse_pairs(json, "\"meals\"", mp, 256);
    for (auto &m : mp) g_meals.push_back({m.first, m.second, false});
    const char *p = strstr(json, "\"duration\"");
    if (p) { p = strchr(p, ':'); if (p) { char *e = nullptr; float d = (float)strtod(p + 1, &e); if (e != p + 1 && d > 0) g_replay_dur = d; } }
    p = strstr(json, "\"speedup\"");
    if (p) { p = strchr(p, ':'); if (p) { char *e = nullptr; float s = (float)strtod(p + 1, &e); if (e != p + 1 && s > 0) g_speedup = s; } }

    g_has_dataset = !g_glu.empty();
    g_sim_min = 0;
    g_last_bolus_min = -999.0f;
    g_suspended = false;
    g_last_iob_record = -999.0f;
    g_replay_inited = false;     // 下次 play 时重新握手
}

void replay_tick(float real_dt_ms)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_has_dataset || !g_playing) return;
    if (!g_replay_inited) { replay_handshake(); g_replay_inited = true; }

    float dt_min = (real_dt_ms / 1000.0f) * g_speedup;
    g_sim_min += dt_min;
    if (g_sim_min >= g_replay_dur) { g_sim_min = g_replay_dur; g_playing = false; }

    // ---- 血糖插值 + 趋势 ----
    float mgdl = interpolate_glucose(g_sim_min);
    g_pump_state.last_glucose_mgdl    = (uint16_t)(mgdl + 0.5f);
    g_pump_state.last_glucose_time_unix = rtc_unix_now();
    float mgdl_prev = interpolate_glucose(g_sim_min - 5.0f);
    float slope = mgdl - mgdl_prev;
    g_pump_state.glucose_trend = (slope > 8) ? 2 : (slope > 2) ? 1
                                : (slope < -8) ? -2 : (slope < -2) ? -1 : 0;

    // ---- 基础率连续输注 (按当前小时 + TBR) ----
    int hour = ((int)(g_sim_min / 60)) % 24;
    float base = (hour >= 0 && hour < 24) ? g_basal[hour] : 0.5f;
    if (g_pump_state.tbr_percent > 0 && g_pump_state.tbr_expiry_ms
        && millis() > g_pump_state.tbr_expiry_ms) {
        g_pump_state.tbr_percent = 0; g_pump_state.tbr_rate = 0;
    }
    float tbr_mult = (g_pump_state.tbr_percent <= 0.0f) ? 0.0f
                   : (g_pump_state.tbr_percent / 100.0f);
    float rate = base * tbr_mult;
    g_pump_state.current_basal_rate = rate;
    float deliv = rate * (dt_min / 60.0f);   // 本 tick 基础率投递量(U)
    if (deliv > 0) {
        uint32_t du = (uint32_t)(deliv * 100.0f + 0.5f);
        g_host_motor.delivered_units_x100 += du;
        g_pump_state.today_units_x100     += du;
        if (g_pump_state.reservoir_units_left > du)
            g_pump_state.reservoir_units_left -= (uint16_t)(du / 100u + ((du % 100u) ? 1 : 0));
        else
            g_pump_state.reservoir_units_left = 0;
        // 基础率按 15 模拟分钟粒度记入 IOB(避免事件爆炸)
        if (g_sim_min - g_last_iob_record >= 15.0f) {
            record_iob(deliv, g_sim_min);
            g_last_iob_record = g_sim_min;
        }
    }

    // ---- 虚拟 AAPS: 餐时大剂量 (走真实固件命令路径) ----
    for (auto &m : g_meals) {
        if (!m.fired && g_sim_min >= m.t_min && g_sim_min < m.t_min + dt_min) {
            uint8_t b[3]; bolus_bytes(m.dose, b);
            int r = send_cmd(DANA_CMD_STEP_BOLUS_START, b, 3);
            if (r > 0) {
                uint32_t du = (uint32_t)(m.dose * 100.0f + 0.5f);
                g_host_motor.delivered_units_x100 += du;
                g_pump_state.today_units_x100     += du;
                if (g_pump_state.reservoir_units_left > du)
                    g_pump_state.reservoir_units_left -= (uint16_t)(du/100u + ((du%100u)?1:0));
                else g_pump_state.reservoir_units_left = 0;
                record_iob(m.dose, g_sim_min);
                g_last_bolus_min = g_sim_min;
            }
            m.fired = true;
        }
    }

    // ---- 虚拟 AAPS: 高血糖纠正 (ISF=2) ----
    float mmol = mgdl / 18.0f;
    if (mmol > 10.0f && (g_sim_min - g_last_bolus_min) > 90.0f) {
        float corr = (mmol - 6.0f) / 2.0f;
        if (corr > 10.0f) corr = 10.0f;
        corr = quantize_units_grid(corr);
        if (corr >= 0.1f) {   // 0.1U 最小剂量下限: 量化后 corr 为 0.1 的倍数, 0 即无剂量
            uint8_t b[3]; bolus_bytes(corr, b);
            int r = send_cmd(DANA_CMD_STEP_BOLUS_START, b, 3);
            if (r > 0) {
                uint32_t du = (uint32_t)(corr * 100.0f + 0.5f);
                g_host_motor.delivered_units_x100 += du;
                g_pump_state.today_units_x100     += du;
                record_iob(corr, g_sim_min);
                g_last_bolus_min = g_sim_min;
            }
        }
    }

    // ---- 虚拟 AAPS: 低血糖暂停 / 恢复 (TBR 0% / 100%) ----
    if (mmol < 4.0f && !g_suspended) {
        uint8_t tbr[3] = { 0, 0, 0 };
        send_cmd(DANA_CMD_SET_TBR, tbr, 3);
        g_suspended = true;
    } else if (mmol > 5.0f && g_suspended) {
        uint8_t tbr[3] = { 100 & 0xFF, 0, 0 };
        send_cmd(DANA_CMD_SET_TBR, tbr, 3);
        g_suspended = false;
    }

    recompute_iob();

    // ---- 状态机 ----
    bool bolusing = (g_sim_min - g_last_bolus_min) < 10.0f;
    g_pump_state.current_state = bolusing ? (uint8_t)PUMP_STATE_BOLUS
                                          : (uint8_t)PUMP_STATE_BASAL;
}


/* ============================================================
 * AAPS 蓝牙客户端 (与 aaps_link_sim.cpp 一致)
 * ============================================================ */
static int feed_and_recv(const uint8_t *pkt, size_t plen, bool expect_resp)
{
    aaps_dana_feed_rx_test(pkt, plen);
    if (!expect_resp) return 0;
    uint8_t resp[128];
    size_t rl = host_drain_tx(resp, sizeof(resp));
    if (rl == 0) { g_last_resp_hex.clear(); g_last_rx_ok = false; return 0; }
    uint8_t t, op, pr[64];
    size_t np = 0;
    /* 固件响应已由 host_tx_push 在分片边界整体二级解密 (g_tx_ble5 启用后),
     * 故用 dana_unpack_packet (不再二次解密)。 */
    int r = dana_unpack_packet(&c_peer, resp, rl, &t, &op, pr, sizeof(pr), &np);
    g_rt_type = t; g_rt_op = op; g_rt_nparams = np;
    memcpy(g_rt_params, pr, np);
    g_last_resp_hex = to_hex(resp, rl);
    g_last_rx_ok = (r == 0);
    return (r == 0) ? 1 : -2;
}

static int send_enc(uint8_t enc_op, const uint8_t *params, uint8_t nparams)
{
    uint8_t pkt[64]; size_t pl = 0;
    int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
    if (dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_ENC_REQ, enc_op,
                          params, nparams, apply_ble5) != 0) return -1;
    LinkTrace tr;
    tr.step_index = (int)g_steps.size() + 1;
    tr.type = DANA_TYPE_ENC_REQ;
    tr.opcode = enc_op;
    tr.op_name = op_name(enc_op);
    tr.intent = dana_intent(enc_op, params, nparams);
    tr.tx_hex = to_hex(pkt, pl);
    tr.crc_ok = false;
    tr.rejected = false;
    int rec = feed_and_recv(pkt, pl, true);
    if (rec > 0) {
        tr.crc_ok = true;
        tr.rx_info = "解包 OK · 握手信封 (type=ENC_REQ 0x01) · CRC 通过";
        tr.action = action_summary(enc_op);
        tr.resp_hex = g_last_resp_hex;
        if (enc_op == DANA_OP_PUMP_CHECK)     c_peer.conn = DANA_CONN_PUMP_CHECK;
        else if (enc_op == DANA_OP_TIME_INFO) c_peer.conn = DANA_CONN_HANDSHAKE_DONE;
    } else {
        tr.rejected = true;
        tr.rx_info = "CRC 校验失败 -> 丢弃, 无响应";
        tr.action = "无响应 (被拒绝)";
    }
    g_trace.push_back(tr);
    return rec;
}

static int send_cmd(uint8_t opcode, const uint8_t *params, uint8_t nparams)
{
    uint8_t pkt[64]; size_t pl = 0;
    int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
    if (dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND, opcode,
                          params, nparams, apply_ble5) != 0) return -1;
    LinkTrace tr;
    tr.step_index = (int)g_steps.size() + 1;
    tr.type = DANA_TYPE_COMMAND;
    tr.opcode = opcode;
    tr.op_name = op_name(opcode);
    tr.intent = dana_intent(opcode, params, nparams);
    tr.tx_hex = to_hex(pkt, pl);
    tr.crc_ok = false;
    tr.rejected = false;
    int rec = feed_and_recv(pkt, pl, true);
    if (rec > 0) {
        tr.crc_ok = true;
        tr.rx_info = "解包 OK · type=COMMAND(0xA1) · CRC 通过 · 回响 opcode";
        tr.action = action_summary(opcode);
        tr.resp_hex = g_last_resp_hex;
    } else {
        tr.rejected = true;
        tr.rx_info = "CRC 校验失败 -> 丢弃, 无响应";
        tr.action = "无响应 (被拒绝)";
    }
    g_trace.push_back(tr);
    return rec;
}

/* ============================================================
 * 会话步骤
 * ============================================================ */
#define LCHECK(cond, name) do { LinkCheck _c; _c.text = (name); _c.ok = (cond); s.checks.push_back(_c); } while (0)

static void step1(LinkStep &s)  // 握手 PUMP_CHECK
{
    int r = send_enc(DANA_OP_PUMP_CHECK, NULL, 0);
    LCHECK(r == 1, "PUMP_CHECK 收到固件响应");
    LCHECK(c_peer.conn == DANA_CONN_PUMP_CHECK, "对端握手态 -> PUMP_CHECK");
    LCHECK(g_rt_type == DANA_TYPE_ENC_RESP, "响应类型 = ENC_RESP");
    LCHECK(g_rt_op == DANA_OP_PUMP_CHECK, "响应 opcode = PUMP_CHECK");
    LCHECK(g_pump_state.dana_paired == false, "握手未完成时 dana_paired 仍为 false");
}

static void step2(LinkStep &s)  // 握手 TIME_INFO
{
    int r = send_enc(DANA_OP_TIME_INFO, NULL, 0);
    LCHECK(r == 1, "TIME_INFO 收到固件响应");
    LCHECK(c_peer.conn == DANA_CONN_HANDSHAKE_DONE, "对端握手态 -> HANDSHAKE_DONE");
    LCHECK(g_pump_state.dana_paired == true, "固件 dana_paired=true (闭环页显示 'AAPS 已接管')");
    host_tx_set_ble5(c_peer.ble5_enc_key);   // 握手完成: TX 捕获侧启用二级解密
}

static void step3(LinkStep &s)  // GET_TIME (未设时钟)
{
    int r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    LCHECK(r == 1, "GET_TIME 收到响应");
    LCHECK(g_rt_nparams == 6, "GET_TIME 响应 6 字节");
    LCHECK(g_rt_params[0] == 0 && g_rt_params[1] == 1 && g_rt_params[2] == 1,
           "未设时钟时回 2000-01-01 00:00:00");
}

static void step4(LinkStep &s)  // SET_TIME
{
    uint8_t tset[6] = { 26, 7, 28, 13, 30, 0 };
    int r = send_cmd(DANA_CMD_SET_TIME, tset, 6);
    LCHECK(r == 1, "SET_TIME 收到响应");
    LCHECK(rtc_is_set(), "固件 rtc_is_set()=true (硬件 RTC 已被设置)");
    LCHECK(rtc_unix_now() >= 1780000000u, "rtc_unix_now() 返回合理 Unix 秒 (>2026)");
}

static void step5(LinkStep &s)  // GET_TIME 复核
{
    int r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    LCHECK(r == 1, "GET_TIME 收到响应");
    LCHECK(g_rt_params[0] == 26 && g_rt_params[1] == 7 && g_rt_params[2] == 28 &&
          g_rt_params[3] == 13 && g_rt_params[4] == 30,
          "GET_TIME 回显与 SET_TIME 一致 (年-2000=26,月=7,日=28,时=13,分=30)");
}

static void step6(LinkStep &s)  // INITIAL_SCREEN 状态查询
{
    int r = send_cmd(DANA_CMD_INITIAL_SCREEN, NULL, 0);
    LCHECK(r == 1, "INITIAL_SCREEN 收到响应");
    LCHECK(g_rt_nparams == 17, "INITIAL_SCREEN 响应 17 字节 (Dana 标准布局)");
}

static void step7(LinkStep &s)  // APS_TBR 100% / 30min
{
    host_reset_logs();
    uint8_t tbr[3] = { 100 & 0xFF, 0, 160 };
    int r = send_cmd(DANA_CMD_APS_TBR, tbr, 3);
    LCHECK(r == 1, "APS_TBR 收到响应");
    LCHECK(g_pump_state.tbr_percent == 100.0f, "tbr_percent=100");
    LCHECK(g_pump_state.tbr_rate > 0.49f && g_pump_state.tbr_rate < 0.51f,
           "tbr_rate≈0.5 U/h (参考基础率 0.5 × 100%)");
    LCHECK(g_pump_state.tbr_expiry_ms > millis(), "tbr_expiry_ms 在未来");
    LCHECK(g_host_motor.enqueue_count == 0, "APS_TBR 未误触发电机(仅改基础率)");
}

static void step8(LinkStep &s)  // CANCEL_TBR
{
    int r = send_cmd(DANA_CMD_CANCEL_TBR, NULL, 0);
    LCHECK(r == 1, "CANCEL_TBR 收到响应");
    LCHECK(g_pump_state.tbr_percent == 0.0f, "tbr_percent 归零");
    LCHECK(g_pump_state.tbr_rate == 0.0f, "tbr_rate 归零");
}

static void step9(LinkStep &s)  // SET_TBR 50% / 30min
{
    uint8_t tbr2[3] = { 50 & 0xFF, 0, 160 };
    int r = send_cmd(DANA_CMD_SET_TBR, tbr2, 3);
    LCHECK(r == 1, "SET_TBR 收到响应");
    LCHECK(g_pump_state.tbr_percent == 50.0f, "tbr_percent=50");
}

static void step10(LinkStep &s)  // STEP_BOLUS_START 2.0U
{
    host_reset_logs();
    uint8_t bol[3] = { 200 & 0xFF, 0, 0 };
    int r = send_cmd(DANA_CMD_STEP_BOLUS_START, bol, 3);
    LCHECK(r == 1, "STEP_BOLUS_START 收到响应");
    LCHECK(g_host_motor.enqueue_count == 1, "motor_enqueue 被调用 1 次 (大剂量真正下发)");
    LCHECK(g_host_motor.last_units_x100 == 200, "下发剂量=2.00U (200×100)");
}

static void step11(LinkStep &s)  // STEP_BOLUS_STOP
{
    int r = send_cmd(DANA_CMD_STEP_BOLUS_STOP, NULL, 0);
    LCHECK(r == 1, "STEP_BOLUS_STOP 收到响应");
    LCHECK(g_host_motor.cancel_count == 1, "motor_cancel_bolus 被调用 (取消进行中的大剂量)");
}

static void step12(LinkStep &s)  // EXT_BOLUS 方波 3U / 2h
{
    host_reset_logs();
    uint8_t ext[3] = { 300 & 0xFF, (300 >> 8) & 0xFF, 4 };
    int r = send_cmd(DANA_CMD_EXT_BOLUS, ext, 3);
    LCHECK(r == 1, "EXT_BOLUS 收到响应");
    LCHECK(g_host_basal.ext_start_count == 1, "basal_scheduler_start_extended_bolus 被调用");
    LCHECK(g_host_basal.last_ext_units == 3.0f, "方波总量=3.0U");
    LCHECK(g_host_basal.last_ext_dur_h == 2.0f, "方波时长=2.0h");
    LCHECK(g_pump_state.ext_bolus_active == true, "固件 ext_bolus_active=true");
}

static void step13(LinkStep &s)  // EXT_BOLUS_CANCEL
{
    int r = send_cmd(DANA_CMD_EXT_BOLUS_CANCEL, NULL, 0);
    LCHECK(r == 1, "EXT_BOLUS_CANCEL 收到响应");
    LCHECK(g_host_basal.ext_cancel_count == 1, "basal_scheduler_cancel_extended_bolus 被调用");
    LCHECK(g_pump_state.ext_bolus_active == false, "固件 ext_bolus_active=false");
}

static void step14(LinkStep &s)  // DAILY_TOTAL
{
    int r = send_cmd(DANA_CMD_DAILY_TOTAL, NULL, 0);
    LCHECK(r == 1, "DAILY_TOTAL 收到响应");
    LCHECK(g_rt_nparams == 2, "DAILY_TOTAL 响应 2 字节");
    LCHECK(((uint16_t)g_rt_params[0] | ((uint16_t)g_rt_params[1] << 8)) ==
          (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF),
          "DAILY_TOTAL 与固件今日总量一致");
}

static void step15(LinkStep &s)  // 回归: SET_DUAL_BOLUS 不污染血糖
{
    host_reset_logs();
    uint8_t dual[3] = { 0x00, 0x20, 0x03 };
    int r = send_cmd(DANA_CMD_SET_DUAL_BOLUS, dual, 3);
    LCHECK(r == 1, "SET_DUAL_BOLUS 收到响应(教学原型返回 OK)");
    LCHECK(g_pump_state.last_glucose_mgdl == 0,
          "血糖未被 0x48 参数污染 (last_glucose_mgdl 仍为 0, 安全!)");
    LCHECK(g_rt_op == DANA_CMD_SET_DUAL_BOLUS, "响应 opcode 正确回显 0x48");
}

static void step16(LinkStep &s)  // 篡改 CRC 的命令必须被拒绝
{
    uint8_t pkt[64]; size_t pl = 0;
    dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND, DANA_CMD_GET_TIME, NULL, 0, 1);
    pkt[pl - 3] ^= 0xFF;                       // 破坏 CRC 低字节
    aaps_dana_feed_rx_test(pkt, pl);
    // 协议轨迹: 记录这帧被篡改的包 (固件应拒绝)
    {
        LinkTrace tr;
        tr.step_index = (int)g_steps.size() + 1;
        tr.type = DANA_TYPE_COMMAND;
        tr.opcode = DANA_CMD_GET_TIME;
        tr.op_name = op_name(DANA_CMD_GET_TIME);
        tr.intent = dana_intent(DANA_CMD_GET_TIME, NULL, 0);
        tr.tx_hex = to_hex(pkt, pl);
        tr.crc_ok = false;
        tr.rejected = true;
        tr.rx_info = "CRC 校验失败 -> 丢弃, 无响应 (完整性保护生效)";
        tr.action = "无响应 (被拒绝)";
        g_trace.push_back(tr);
    }
    uint8_t junk[128];
    size_t rl = host_drain_tx(junk, sizeof(junk));
    LCHECK(rl == 0, "篡改 CRC 的命令 -> 固件无响应 (完整性保护生效)");

    int r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    LCHECK(r == 1, "篡改后发合法命令仍正常响应 (状态机未损坏)");
}

static void step17(LinkStep &s)  // 收尾 INITIAL_SCREEN 复核
{
    int r = send_cmd(DANA_CMD_INITIAL_SCREEN, NULL, 0);
    LCHECK(r == 1 && g_rt_nparams == 17, "INITIAL_SCREEN 仍返回 17 字节标准布局");
}

#undef LCHECK

typedef void (*step_fn_t)(LinkStep &);
static const char  *g_titles[TOTAL] = {
    "[1] 握手 PUMP_CHECK",
    "[2] 握手 TIME_INFO (完成二级加密握手)",
    "[3] GET_TIME (时钟基准尚未设置)",
    "[4] SET_TIME -> 2026-07-28 13:30:00",
    "[5] GET_TIME 复核",
    "[6] INITIAL_SCREEN 状态查询",
    "[7] APS_TBR 闭环临时基础率 100% / 30min",
    "[8] CANCEL_TBR 取消临时基础率",
    "[9] SET_TBR 手动临时基础率 50% / 30min",
    "[10] STEP_BOLUS_START 大剂量 2.0U",
    "[11] STEP_BOLUS_STOP 停止大剂量",
    "[12] EXT_BOLUS 方波 3U / 2h",
    "[13] EXT_BOLUS_CANCEL 取消方波",
    "[14] DAILY_TOTAL 今日总量查询",
    "[15] 回归 SET_DUAL_BOLUS 不污染血糖",
    "[16] 安全 篡改CRC命令被拒绝",
    "[17] 收尾 INITIAL_SCREEN 复核",
};
static const step_fn_t g_steps_fn[TOTAL] = {
    step1, step2, step3, step4, step5, step6, step7, step8, step9,
    step10, step11, step12, step13, step14, step15, step16, step17,
};

/* ============================================================
 * 状态初始化 (host 测试与模拟器共用同一套起始状态)
 * ============================================================ */
static void setup_state(void)
{
    pump_state_init();
    g_pump_state.current_basal_rate    = 0.5f;   // 基础率 0.5 U/h (默认档案)
    g_pump_state.reservoir_units_left = 300;    // 满储药器
    g_pump_state.battery_pct          = 100;
    g_pump_state.ble_connected        = true;   // 模拟已 BLE 连接
    g_pump_state.loop_mode            = 0;      // 闭环 (AAPS 接管)
    g_pump_state.current_state        = (uint8_t)PUMP_STATE_IDLE;
    g_pump_state.dana_paired          = false;
    g_pump_state.iob_x10000           = 0;
    g_pump_state.today_units_x100     = 0;
    g_pump_state.tbr_percent          = 0;
    g_pump_state.tbr_rate             = 0;
    g_pump_state.tbr_expiry_ms        = 0;
    g_pump_state.ext_bolus_active     = false;
    g_pump_state.last_glucose_mgdl    = 0;
    g_pump_state.glucose_trend        = 0;

    rtc_clock_init();                            // 从 g_pump_config.rtc_base_unix(=0) 恢复 -> 未设置
    dana_ctx_init(&c_peer, "DAN12345AB", "123456", 0x09, 0x0A);
    aaps_dana_attach(&g_fake_server);

    host_reset_logs();                           // 会话(重)开始: 调用记录归零
    g_host_motor.delivered_units_x100 = 0;       // 累计发药量归零 (电机演示柱塞回到起点)
}

void init(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    setup_state();
    g_steps.clear();
    g_trace.clear();
    g_playing = false;
    g_rt_nparams = 0;
}

void reset(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    aaps_dana_reset_test();        // 复位固件握手态
    host_tx_clear_ble5();
    c_peer.conn = DANA_CONN_INIT;
    setup_state();
    g_steps.clear();
    g_trace.clear();
    g_playing = false;
    g_rt_nparams = 0;
    // 数据模拟状态复位 (保留已载入的数据集, 便于重跑)
    g_sim_min = 0;
    g_iob.clear();
    g_suspended = false;
    g_last_bolus_min = -999.0f;
    g_last_iob_record = -999.0f;
    g_replay_inited = false;
}

bool step(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if ((int)g_steps.size() >= TOTAL) return false;
    LinkStep s;
    s.index = (int)g_steps.size() + 1;
    s.title = g_titles[g_steps.size()];
    g_steps_fn[g_steps.size()](s);
    g_steps.push_back(s);
    return (int)g_steps.size() < TOTAL;
}

void set_playing(bool p) { std::lock_guard<std::mutex> lk(g_mtx); g_playing = p; }
bool playing(void)       { std::lock_guard<std::mutex> lk(g_mtx); return g_playing; }
int  index(void)         { std::lock_guard<std::mutex> lk(g_mtx); return (int)g_steps.size(); }
int  total(void)         { return TOTAL; }

void set_delay(int ms)   { if (ms >= 0) { std::lock_guard<std::mutex> lk(g_mtx); g_delay_ms = ms; } }
int  delay_ms(void)      { std::lock_guard<std::mutex> lk(g_mtx); return g_delay_ms; }

void all_steps_json(char *out, size_t cap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    int n = (int)g_steps.size();
    int off = snprintf(out, cap, "\"steps\":[");
    for (int i = 0; i < n && (size_t)off < cap - 1; i++) {
        const LinkStep &s = g_steps[i];
        off += snprintf(out + off, cap - off,
                        "%s{\"i\":%d,\"title\":%s,\"checks\":[",
                        (i ? "," : ""),
                        s.index, json_str(s.title.c_str()));
        for (size_t j = 0; j < s.checks.size(); j++) {
            off += snprintf(out + off, cap - off,
                            "%s{\"t\":%s,\"ok\":%s}",
                            (j ? "," : ""),
                            json_str(s.checks[j].text.c_str()),
                            s.checks[j].ok ? "true" : "false");
        }
        off += snprintf(out + off, cap - off, "]}");
    }
    snprintf(out + off, cap - off, "]");
}

void trace_json(char *out, size_t cap)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string s = "\"trace\":[";
    for (size_t i = 0; i < g_trace.size(); i++) {
        const LinkTrace &t = g_trace[i];
        if (i) s += ",";
        /* 字段均为受控 ASCII/中文, 无引号/反斜杠, 直接包裹安全 */
        std::string e = "{\"i\":" + std::to_string(t.step_index)
            + ",\"op\":\"" + t.op_name + "\""
            + ",\"intent\":\"" + t.intent + "\""
            + ",\"tx\":\"" + t.tx_hex + "\""
            + ",\"crc_ok\":" + (t.crc_ok ? "true" : "false")
            + ",\"rx\":\"" + t.rx_info + "\""
            + ",\"action\":\"" + t.action + "\""
            + ",\"resp\":\"" + t.resp_hex + "\""
            + ",\"rej\":" + (t.rejected ? "true" : "false") + "}";
        s += e;
    }
    s += "]";
    snprintf(out, cap, "%s", s.c_str());
}

void snapshot_json(char *out, size_t cap)
{
    std::lock_guard<std::mutex> lk(g_mtx);

    float glucose = (g_pump_state.last_glucose_mgdl > 0)
                        ? (float)g_pump_state.last_glucose_mgdl / 18.0f : 0.0f;
    int hh = -1, mm = -1;
    if (rtc_is_set()) {
        uint32_t u = rtc_unix_now();
        if (u != 0) {
            int y, mo, d, h, mi, s;
            rtc_unix_to_ymdhms(u, &y, &mo, &d, &h, &mi, &s);
            hh = h; mm = mi;
        }
    }
    bool bolus_active = motor_bolus_active() || basal_scheduler_extended_bolus_active();

    /* 电机演示: 累计已发药 -> 微步 (走 dosing.h 单一真源) */
    uint32_t m_delivered_x100 = motor_delivered_units_x100();
    float    m_units    = m_delivered_x100 / 100.0f;
    uint32_t m_microsteps = units_to_microsteps(m_units);

    char nb[48];
    auto num = [&](const char *fmt, double v) {
        snprintf(nb, sizeof(nb), fmt, v);
        return std::string(nb);
    };
    auto tf = [](bool b) { return std::string(b ? "true" : "false"); };
    std::string s;
    s.reserve(512);
    s += "\"glucose_mmol\":"; s += num("%.2f", glucose);
    s += ",\"trend\":";       s += std::to_string((int)g_pump_state.glucose_trend);
    s += ",\"clock_h\":";      s += std::to_string(hh);
    s += ",\"clock_m\":";      s += std::to_string(mm);
    s += ",\"reservoir\":";    s += std::to_string((int)g_pump_state.reservoir_units_left);
    s += ",\"battery\":";      s += std::to_string((int)g_pump_state.battery_pct);
    s += ",\"iob\":";          s += num("%.2f", g_pump_state.iob_x10000 / 10000.0f);
    s += ",\"today\":";        s += num("%.2f", g_pump_state.today_units_x100 / 100.0f);
    s += ",\"tbr_pct\":";      s += num("%.0f", g_pump_state.tbr_percent);
    s += ",\"tbr_rate\":";     s += num("%.3f", g_pump_state.tbr_rate);
    s += ",\"loop_mode\":";    s += std::to_string((unsigned)g_pump_state.loop_mode);
    s += ",\"connected\":";    s += tf(g_pump_state.ble_connected);
    s += ",\"dana_paired\":";  s += tf(g_pump_state.dana_paired);
    s += ",\"bolus_active\":"; s += tf(bolus_active);
    s += ",\"ext_active\":";   s += tf(g_pump_state.ext_bolus_active);
    s += ",\"state\":";        s += std::to_string((unsigned)g_pump_state.current_state);
    s += ",\"alarm_active\":"; s += tf(g_pump_state.alarm_active);
    s += ",\"alarm_code\":";   s += std::to_string((unsigned)g_pump_state.alarm_code);
    s += ",\"over_temp_warn\":"; s += tf(g_pump_state.over_temp_warn);
    s += ",\"board_temp_c\":";   s += num("%.1f", g_pump_state.board_temp_c);
    s += ",\"basal_rate\":";   s += num("%.2f", g_pump_state.current_basal_rate);
    s += ",\"dose_calibration\":"; s += num("%.3f", g_pump_config.dose_calibration);
    s += ",\"vib_last\":";       s += std::to_string(g_last_vib_pat);   // P3-15: 最近振动模式(0=无)
    s += ",\"motor_units\":";  s += num("%.2f", m_units);
    s += ",\"motor_microsteps\":"; s += std::to_string((unsigned)m_microsteps);
    s += ",\"sim_min\":";      s += num("%.1f", g_sim_min);
    s += ",\"mode\":";         s += std::to_string(g_mode);
    s += ",\"has_data\":";     s += tf(g_has_dataset);
    // 当前泵屏 UI 导航/编辑态 (界面/选中/表单值/亮度/时钟), 供联调面板手动操控显示
    char uibuf[320];
    ui_screen_dump_json(uibuf, sizeof(uibuf));
    s += ",\"ui\":";
    s += uibuf;
    if (cap > 0) {
        size_t m = (s.size() < cap - 1) ? s.size() : (cap - 1);
        memcpy(out, s.data(), m);
        out[m] = '\0';
    }
}

} // namespace linksess
