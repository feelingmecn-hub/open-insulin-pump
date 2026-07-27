/**
 * ui_screen.cpp — 全中文 UI 页面状态机 (LVGL, PC/SDL 版)
 *
 * 横屏 320×172, 4 物理按键(上/下/确认/返回)导航。
 * 每帧 ui_screen_refresh() 重建当前页 (clean + draw), 简单且无状态同步问题。
 * 复用了固件的 g_pump_state, 演示数据来自 ui_hal (模拟器后端 = ui_hal_sim)。
 */
#include "ui_screen.h"
#include "pump_state.h"
#include "pump_types.h"
#include "ui_hal.h"
#include "config.h"
#include <cstdio>
#include <cstring>

// ---- 配色 (白底 + 迈世通医疗蓝, 还原最初设计) ----
#define BG      lv_color_hex(0xffffff)   // 纯白设备底
#define PANEL   lv_color_hex(0xffffff)   // 卡片白
#define TEXT    lv_color_hex(0x1f2733)   // 近黑正文
#define TITLE   lv_color_hex(0x0a2a43)   // 深蓝标题 (配医疗蓝)
#define ACCENT  lv_color_hex(0x006bb7)   // 迈世通医疗蓝
#define GREEN   lv_color_hex(0x2e9e4f)   // 正常态绿 (白底可见)
#define YELLOW  lv_color_hex(0xc79100)   // 警示琥珀 (白底可见)
#define RED     lv_color_hex(0xd83a3a)   // 报警红
#define DIM     lv_color_hex(0x8a95a5)   // 灰辅助文字
#define LINE    lv_color_hex(0xd5dce4)   // 分割线/浅边框

#define FONT_MAIN (&lv_font_cn_16)
#define FONT_SM   (&lv_font_cn_12)

// ---- 页面枚举 (ui_set_screen 参数顺序) ----
enum {
    SCR_HOME = 0, SCR_MENU,
    SCR_BASAL,
    SCR_BOLUS_MENU, SCR_BOLUS_NORMAL, SCR_BOLUS_SQUARE, SCR_BOLUS_DUAL, SCR_BOLUS_WIZARD, SCR_BOLUS_MEALS,
    SCR_PRIME,
    SCR_ALARM_LIST, SCR_ALARM_DETAIL,
    SCR_LOOP,
    SCR_SETTINGS
};

// ---- 全局导航状态 ----
static lv_obj_t *s_page = nullptr;     // 页面内容容器 (y0..146)
static int  s_screen = SCR_HOME;
static int  s_sel    = 0;              // 列表/字段选中
static int  s_sel_parent = 0;          // 返回父页时恢复选中

// 大剂量编辑值
static float s_dose      = 1.00f;      // 常规/方波剂量
static float s_dose_imme = 1.00f;      // 双波: 立即量
static float s_dose_sq   = 1.00f;      // 双波: 方波量
static int   s_dur_h     = 1;          // 方波/双波时长(小时)
static float s_wiz_bg    = 6.5f;       // 向导: 血糖 mmol/L
static float s_wiz_carb  = 30.0f;      // 向导: 碳水 g
static int   s_meal_sel  = 0;          // 三餐预设选中
// 基础率模式由 ui_hal_basal_local_mode() 实时反映, 不在此缓存
static int   s_alarm_sel   = 0;

// ---- 文本映射 ----
static const char *trend_str(int8_t t)
{
    switch (t) { case 1: return "上升 ↑"; case -1: return "下降 ↓"; default: return "平稳 →"; }
}
static lv_color_t trend_color(int8_t t)
{
    switch (t) { case 1: return RED; case -1: return GREEN; default: return DIM; }
}
static const char *loop_str(uint8_t m)
{
    switch (m) { case 0: return "闭环中"; case 1: return "开环"; default: return "已暂停"; }
}
static lv_color_t loop_color(uint8_t m)
{
    switch (m) { case 0: return GREEN; case 1: return YELLOW; default: return DIM; }
}
static const char *state_str(uint8_t s)
{
    switch ((pump_state_t)s) {
        case PUMP_STATE_BOOTING:   return "启动中";
        case PUMP_STATE_IDLE:      return "待机";
        case PUMP_STATE_PRIMING:   return "排气中";
        case PUMP_STATE_DELIVERING:return "输注中";
        case PUMP_STATE_BASAL:     return "基础率运行";
        case PUMP_STATE_BOLUS:     return "大剂量中";
        case PUMP_STATE_STOPPING:  return "停止中";
        case PUMP_STATE_ALARM:     return "报警";
        case PUMP_STATE_SLEEP:     return "休眠";
        case PUMP_STATE_ERROR:     return "故障";
        default:                   return "?";
    }
}

// 报警清单 (7 种)
typedef struct { alarm_code_t code; const char *name; const char *reason; const char *action; } alarm_def_t;
static const alarm_def_t ALARMS[7] = {
    { ALARM_OCCLUSION,     "阻塞",     "管路阻塞或阻力过大", "检查管路，解除阻塞后确认" },
    { ALARM_BATTERY_LOW,   "低电量",   "电池电压偏低",       "尽快更换电池" },
    { ALARM_RESERVOIR_EMPTY,"低药量",  "储药器药量不足",     "准备更换笔芯" },
    { ALARM_COMM_LOST,     "连接丢失", "与手机/AAPS 通信中断","检查蓝牙连接" },
    { ALARM_PUMP_STALLED,  "电机堵转", "步进电机卡死",       "检查机械结构后复位" },
    { ALARM_STEP_LOSS,     "丢步",     "电机丢步/电流异常",  "检查电机负载" },
    { ALARM_OVER_TEMP,     "过温",     "板载温度过高",       "暂停使用，降温后恢复" },
};
static bool alarm_is_active(int i)
{
    return g_pump_state.alarm_active && (alarm_code_t)g_pump_state.alarm_code == ALARMS[i].code;
}

// ---- 通用 helper: 在 s_page 上放一个文本 label ----
static lv_obj_t *L(int x, int y, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(s_page);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}
// 蓝色标题栏 (白字), 还原最初设计的顶栏风格
static void title(const char *t)
{
    lv_obj_t *hdr = lv_obj_create(s_page);
    lv_obj_set_size(hdr, 320, 18);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, ACCENT, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_t *l = lv_label_create(s_page);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, FONT_MAIN, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, 112, 1);
}

// ============================================================
// 各页面绘制
// ============================================================
static void draw_home(void)
{
    char buf[48];
    int hh, mm; ui_hal_get_clock(&hh, &mm);

    // 蓝色标题栏
    lv_obj_t *hdr = lv_obj_create(s_page);
    lv_obj_set_size(hdr, 320, 18);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, ACCENT, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);

    snprintf(buf, sizeof buf, "%02d:%02d", hh, mm);
    L(4, 2, buf, FONT_SM, lv_color_white());                     // 时钟 (白)
    L(112, 1, "闭环胰岛素泵", FONT_MAIN, lv_color_white());      // 标题 (白)
    snprintf(buf, sizeof buf, "电池 %d%%", g_pump_state.battery_pct);
    L(250, 2, buf, FONT_SM, g_pump_state.battery_pct < 20 ? YELLOW : lv_color_white());  // 电池 (白/低电黄)

    // ---- 左栏: CGM ----
    L(4, 22, "血糖 (CGM)", FONT_SM, DIM);
    snprintf(buf, sizeof buf, "%.1f", ui_hal_glucose_mmol());
    L(4, 40, buf, FONT_MAIN, TITLE);                             // 血糖大字
    L(70, 46, "mmol/L", FONT_SM, DIM);
    L(4, 66, trend_str(ui_hal_glucose_trend()), FONT_SM, trend_color(ui_hal_glucose_trend()));
    snprintf(buf, sizeof buf, "闭环: %s", loop_str(ui_hal_loop_mode()));
    L(4, 90, buf, FONT_SM, loop_color(ui_hal_loop_mode()));

    // ---- 右栏: 泵 ----
    L(168, 22, "基础率", FONT_SM, DIM);
    snprintf(buf, sizeof buf, "%.2f U/h", g_pump_state.current_basal_rate);
    L(168, 40, buf, FONT_MAIN, TEXT);
    L(168, 64, "剩余药量", FONT_SM, DIM);
    snprintf(buf, sizeof buf, "%d U", g_pump_state.reservoir_units_left);
    L(168, 80, buf, FONT_MAIN, TEXT);
    lv_obj_t *bar = lv_bar_create(s_page);
    lv_bar_set_range(bar, 0, MAX_RESERVOIR_UNITS);
    lv_bar_set_value(bar, g_pump_state.reservoir_units_left, LV_ANIM_OFF);
    lv_obj_set_size(bar, 138, 8);
    lv_obj_set_pos(bar, 168, 98);
    lv_obj_set_style_bg_color(bar, LINE, LV_PART_MAIN);         // 浅灰轨道
    lv_obj_set_style_bg_color(bar, ACCENT, LV_PART_INDICATOR); // 蓝色进度

    // ---- 底部信息行 ----
    snprintf(buf, sizeof buf, "今日 %.1f U", ui_hal_today_total());
    L(4, 114, buf, FONT_SM, TEXT);
    snprintf(buf, sizeof buf, "IOB %.2f U", g_pump_state.iob_x10000 / 10000.0f);
    L(168, 114, buf, FONT_SM, TEXT);

    const char *st = g_pump_state.alarm_active ? "⚠ 报警" : state_str(g_pump_state.current_state);
    lv_color_t st_col = g_pump_state.alarm_active ? RED : TEXT;
    if (ui_hal_bolus_active()) {            // 大剂量分段打入进行中
        st = "大剂量注射中… (按 ESC 取消)";
        st_col = ACCENT;
    }
    L(4, 132, st, FONT_SM, st_col);
    L(150, 132, "确认键进入菜单", FONT_SM, DIM);
}

static void draw_menu(void)
{
    title("主菜单");
    const char *names[6] = { "基础率", "大剂量", "排气装药", "报警", "闭环", "系统设置" };
    char v[6][24];
    snprintf(v[0], sizeof v[0], "%.2f U/h", g_pump_state.current_basal_rate);
    strcpy(v[1], "→");
    strcpy(v[2], "→");
    snprintf(v[3], sizeof v[3], g_pump_state.alarm_active ? "1 条" : "正常");
    snprintf(v[4], sizeof v[4], "%s", loop_str(ui_hal_loop_mode()));
    strcpy(v[5], "→");

    for (int i = 0; i < 6; i++) {
        int y = 26 + i * 19;
        bool sel = (i == s_sel);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", names[i]);
        L(8, y, line, FONT_MAIN, sel ? ACCENT : TEXT);
        L(220, y, v[i], FONT_MAIN, sel ? ACCENT : DIM);
    }
    L(40, 134, "上下选择  确认进入  返回首页", FONT_SM, DIM);
}

static void draw_basal(void)
{
    title("基础率");
    L(8, 20, ui_hal_basal_local_mode() ? "模式: 本地档案" : "模式: AAPS 接管",
      FONT_SM, ACCENT);

    int cnt = ui_hal_basal_count();
    int top = s_sel - 3;
    if (top < 0) top = 0;
    if (top > cnt - 7) top = cnt - 7;
    if (top < 0) top = 0;
    for (int r = 0; r < 7; r++) {
        int idx = top + r;
        if (idx >= cnt) break;
        int y = 40 + r * 14;
        bool sel = (idx == s_sel);
        char line[40];
        snprintf(line, sizeof line, "%s %02d:00  %.2f U/h", sel ? "▶" : " ", idx, ui_hal_basal_rate(idx));
        L(12, y, line, FONT_SM, sel ? ACCENT : TEXT);
    }
    L(40, 134, "上下选择  确认切换模式  返回", FONT_SM, DIM);
}

static void draw_bolus_menu(void)
{
    title("大剂量");
    const char *names[5] = { "常规大剂量", "方波大剂量", "双波大剂量", "向导大剂量", "三餐预设" };
    for (int i = 0; i < 5; i++) {
        int y = 30 + i * 21;
        bool sel = (i == s_sel);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", names[i]);
        L(16, y, line, FONT_MAIN, sel ? ACCENT : TEXT);
    }
    L(60, 134, "上下选择  确认  返回", FONT_SM, DIM);
}

static void draw_bolus_normal(void)
{
    title("常规大剂量");
    L(20, 30, "剂量 (U)", FONT_SM, DIM);
    char buf[24];
    snprintf(buf, sizeof buf, "%.2f", s_dose);
    L(120, 28, buf, FONT_MAIN, ACCENT);
    L(20, 70, "▲▼ 调整剂量  确认输注  返回", FONT_SM, DIM);
    L(20, 100, "单次最大 25.00 U", FONT_SM, DIM);
}

static void draw_bolus_square(void)
{
    title("方波大剂量");
    char buf[32];
    snprintf(buf, sizeof buf, "%s 剂量  %.2f U", s_sel == 0 ? "▶" : " ", s_dose);
    L(20, 30, buf, FONT_MAIN, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 时长  %d h", s_sel == 1 ? "▶" : " ", s_dur_h);
    L(20, 58, buf, FONT_MAIN, s_sel == 1 ? ACCENT : TEXT);
    L(20, 96, "▲▼ 调整  确认切换/输注  返回", FONT_SM, DIM);
}

static void draw_bolus_dual(void)
{
    title("双波大剂量");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 立即量  %.2f U", s_sel == 0 ? "▶" : " ", s_dose_imme);
    L(16, 26, buf, FONT_SM, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 方波量  %.2f U", s_sel == 1 ? "▶" : " ", s_dose_sq);
    L(16, 50, buf, FONT_SM, s_sel == 1 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 时长    %d h", s_sel == 2 ? "▶" : " ", s_dur_h);
    L(16, 74, buf, FONT_SM, s_sel == 2 ? ACCENT : TEXT);
    L(16, 104, "▲▼ 调整  确认切换/输注  返回", FONT_SM, DIM);
}

static void draw_bolus_wizard(void)
{
    title("向导大剂量");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 血糖  %.1f mmol/L", s_sel == 0 ? "▶" : " ", s_wiz_bg);
    L(16, 26, buf, FONT_SM, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 碳水  %.0f g", s_sel == 1 ? "▶" : " ", s_wiz_carb);
    L(16, 50, buf, FONT_SM, s_sel == 1 ? ACCENT : TEXT);
    // 建议剂量 = (血糖-目标)/ISF + 碳水/CR  (目标6.0, ISF=2.0, CR=10)
    float sug = (s_wiz_bg - 6.0f) / 2.0f + s_wiz_carb / 10.0f;
    if (sug < 0) sug = 0;
    snprintf(buf, sizeof buf, "建议: %.2f U", sug);
    L(16, 78, buf, FONT_MAIN, GREEN);
    L(16, 108, "▲▼ 调整  确认输注  返回", FONT_SM, DIM);
}

static void draw_bolus_meals(void)
{
    title("三餐预设");
    const char *names[3] = { "早餐", "午餐", "晚餐" };
    static const float def[3] = { 6.0f, 8.0f, 6.0f };
    char buf[32];
    for (int i = 0; i < 3; i++) {
        int y = 34 + i * 30;
        bool sel = (i == s_sel);
        snprintf(buf, sizeof buf, "%s %s  默认 %.1f U", sel ? "▶" : " ", names[i], def[i]);
        L(20, y, buf, FONT_MAIN, sel ? ACCENT : TEXT);
    }
    L(60, 134, "确认按预设输注  返回", FONT_SM, DIM);
}

static void draw_prime(void)
{
    title("排气与装药");
    L(12, 28, "1. 安装 1mL 笔芯", FONT_SM, TEXT);
    L(12, 50, "2. 螺杆自动复位", FONT_SM, TEXT);
    L(12, 72, "3. 自动排气充注", FONT_SM, TEXT);
    L(12, 96, g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING
              ? "状态: 排气中..." : "状态: 待机", FONT_SM,
              g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING ? YELLOW : DIM);
    L(40, 124, "确认开始排气  返回", FONT_SM, DIM);
}

static void draw_alarm_list(void)
{
    title("报警");
    for (int i = 0; i < 7; i++) {
        int y = 26 + i * 15;
        bool sel = (i == s_sel);
        bool act = alarm_is_active(i);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", ALARMS[i].name);
        lv_color_t col = act ? RED : (sel ? ACCENT : TEXT);
        L(16, y, line, FONT_SM, col);
        L(150, y, act ? "激活" : "正常", FONT_SM, act ? RED : DIM);
    }
    L(60, 134, "确认查看详情  返回", FONT_SM, DIM);
}

static void draw_alarm_detail(void)
{
    const alarm_def_t *a = &ALARMS[s_alarm_sel];
    title(a->name);
    L(12, 28, "原因:", FONT_SM, ACCENT);
    L(12, 48, a->reason, FONT_SM, TEXT);
    L(12, 78, "处理:", FONT_SM, ACCENT);
    L(12, 98, a->action, FONT_SM, TEXT);
    L(60, 134, "返回列表", FONT_SM, DIM);
}

static void draw_loop(void)
{
    title("闭环");
    char buf[40];
    L(12, 26, "AAPS:", FONT_SM, DIM);
    L(80, 26, ui_hal_loop_connected() ? "已连接" : "断开", FONT_SM,
      ui_hal_loop_connected() ? GREEN : RED);
    snprintf(buf, sizeof buf, "血糖: %.1f mmol/L", ui_hal_glucose_mmol());
    L(12, 50, buf, FONT_SM, TEXT);
    L(12, 72, trend_str(ui_hal_glucose_trend()), FONT_SM, trend_color(ui_hal_glucose_trend()));
    snprintf(buf, sizeof buf, "模式: %s", loop_str(ui_hal_loop_mode()));
    L(12, 94, buf, FONT_SM, loop_color(ui_hal_loop_mode()));
    float tbr = ui_hal_tbr_percent();
    if (tbr > 0) {
        snprintf(buf, sizeof buf, "临时基础率: %.0f%% (%.2f U/h)", tbr, ui_hal_tbr_rate());
        L(12, 116, buf, FONT_SM, YELLOW);
    } else {
        L(12, 116, "临时基础率: 无", FONT_SM, DIM);
    }
    L(100, 134, "返回", FONT_SM, DIM);
}

static void draw_settings(void)
{
    title("系统设置");
    const char *names[4] = { "日期时间", "屏幕亮度", "按键音", "关于" };
    static const char *vals[4] = { "→", "50%", "开", "→" };
    for (int i = 0; i < 4; i++) {
        int y = 32 + i * 22;
        bool sel = (i == s_sel);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", names[i]);
        L(16, y, line, FONT_MAIN, sel ? ACCENT : TEXT);
        L(220, y, vals[i], FONT_MAIN, sel ? ACCENT : DIM);
    }
    L(100, 134, "返回", FONT_SM, DIM);
}

// ============================================================
// 绘制分派
// ============================================================
static void draw_current(void)
{
    switch (s_screen) {
        case SCR_HOME:         draw_home(); break;
        case SCR_MENU:         draw_menu(); break;
        case SCR_BASAL:        draw_basal(); break;
        case SCR_BOLUS_MENU:   draw_bolus_menu(); break;
        case SCR_BOLUS_NORMAL: draw_bolus_normal(); break;
        case SCR_BOLUS_SQUARE: draw_bolus_square(); break;
        case SCR_BOLUS_DUAL:   draw_bolus_dual(); break;
        case SCR_BOLUS_WIZARD: draw_bolus_wizard(); break;
        case SCR_BOLUS_MEALS:  draw_bolus_meals(); break;
        case SCR_PRIME:        draw_prime(); break;
        case SCR_ALARM_LIST:   draw_alarm_list(); break;
        case SCR_ALARM_DETAIL: draw_alarm_detail(); break;
        case SCR_LOOP:         draw_loop(); break;
        case SCR_SETTINGS:     draw_settings(); break;
        default:               draw_home(); break;
    }
}

// ============================================================
// 导航辅助
// ============================================================
static void enter_child(int child) { s_sel_parent = s_sel; s_sel = 0; s_screen = child; }
static void back_to(int parent)    { s_sel = s_sel_parent; s_screen = parent; }

// 列表通用键: count 项, 父页 parent, 进入回调 on_enter
static void list_key(key_event_t k, int count, int parent, void (*on_enter)(void))
{
    if (k == KEY_UP)        s_sel = (s_sel + count - 1) % count;
    else if (k == KEY_DOWN) s_sel = (s_sel + 1) % count;
    else if (k == KEY_SET)  { if (on_enter) on_enter(); }
    else if (k == KEY_ESC)  back_to(parent);
}

// ============================================================
// 按键分派
// ============================================================
static void on_menu_enter(void)
{
    switch (s_sel) {
        case 0: enter_child(SCR_BASAL); break;
        case 1: enter_child(SCR_BOLUS_MENU); break;
        case 2: enter_child(SCR_PRIME); break;
        case 3: enter_child(SCR_ALARM_LIST); break;
        case 4: enter_child(SCR_LOOP); break;
        case 5: enter_child(SCR_SETTINGS); break;
    }
}
static void on_bolus_menu_enter(void)
{
    switch (s_sel) {
        case 0: enter_child(SCR_BOLUS_NORMAL); break;
        case 1: enter_child(SCR_BOLUS_SQUARE); break;
        case 2: enter_child(SCR_BOLUS_DUAL); break;
        case 3: enter_child(SCR_BOLUS_WIZARD); break;
        case 4: enter_child(SCR_BOLUS_MEALS); break;
    }
}

static void handle_key(key_event_t k)
{
    // 大剂量分段打入进行中时, ESC 优先取消 (只损失已打部分, 剩余停止)
    if (k == KEY_ESC && ui_hal_bolus_active()) {
        ui_hal_cancel_bolus();
        return;
    }
    switch (s_screen) {
        case SCR_HOME:
            if (k == KEY_SET) enter_child(SCR_MENU);
            break;

        case SCR_MENU:
            list_key(k, 6, SCR_HOME, on_menu_enter);
            break;

        case SCR_BASAL:
            list_key(k, ui_hal_basal_count(), SCR_MENU, nullptr);
            if (k == KEY_SET) ui_hal_toggle_basal_mode();   // 确认切换模式
            break;

        case SCR_BOLUS_MENU:
            list_key(k, 5, SCR_MENU, on_bolus_menu_enter);
            break;

        case SCR_BOLUS_NORMAL:
            if (k == KEY_UP)        s_dose += 0.05f;
            else if (k == KEY_DOWN) s_dose = (s_dose > 0.05f) ? s_dose - 0.05f : 0.05f;
            else if (k == KEY_SET)  { ui_hal_deliver_bolus(s_dose, BOLUS_NORMAL, 0, s_dose, 0); back_to(SCR_MENU); }
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            break;

        case SCR_BOLUS_SQUARE:
            if (k == KEY_SET)       s_sel = (s_sel + 1) % 2;     // 在 剂量/时长 间切换
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            else if (s_sel == 0) {
                if (k == KEY_UP)      s_dose += 0.05f;
                else if (k == KEY_DOWN) s_dose = (s_dose > 0.05f) ? s_dose - 0.05f : 0.05f;
            } else {
                if (k == KEY_UP)      s_dur_h = (s_dur_h < 8) ? s_dur_h + 1 : 8;
                else if (k == KEY_DOWN) s_dur_h = (s_dur_h > 0) ? s_dur_h - 1 : 0;
            }
            if (k == KEY_SET && s_sel == 1) { ui_hal_deliver_bolus(s_dose, BOLUS_SQUARE, (float)s_dur_h, 0, s_dose); back_to(SCR_MENU); }
            break;

        case SCR_BOLUS_DUAL:
            if (k == KEY_SET)       s_sel = (s_sel + 1) % 3;
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            else if (s_sel == 0) {
                if (k == KEY_UP)      s_dose_imme += 0.05f;
                else if (k == KEY_DOWN) s_dose_imme = (s_dose_imme > 0.05f) ? s_dose_imme - 0.05f : 0.05f;
            } else if (s_sel == 1) {
                if (k == KEY_UP)      s_dose_sq += 0.05f;
                else if (k == KEY_DOWN) s_dose_sq = (s_dose_sq > 0.05f) ? s_dose_sq - 0.05f : 0.05f;
            } else {
                if (k == KEY_UP)      s_dur_h = (s_dur_h < 8) ? s_dur_h + 1 : 8;
                else if (k == KEY_DOWN) s_dur_h = (s_dur_h > 0) ? s_dur_h - 1 : 0;
            }
            if (k == KEY_SET && s_sel == 2) { ui_hal_deliver_bolus(s_dose_imme + s_dose_sq, BOLUS_DUAL, (float)s_dur_h, s_dose_imme, s_dose_sq); back_to(SCR_MENU); }
            break;

        case SCR_BOLUS_WIZARD:
            if (k == KEY_SET)       s_sel = (s_sel + 1) % 2;
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            else if (s_sel == 0) {
                if (k == KEY_UP)      s_wiz_bg += 0.1f;
                else if (k == KEY_DOWN) s_wiz_bg = (s_wiz_bg > 3.0f) ? s_wiz_bg - 0.1f : 3.0f;
            } else {
                if (k == KEY_UP)      s_wiz_carb += 1.0f;
                else if (k == KEY_DOWN) s_wiz_carb = (s_wiz_carb > 0) ? s_wiz_carb - 1.0f : 0;
            }
            if (k == KEY_SET && s_sel == 1) {
                float sug = (s_wiz_bg - 6.0f) / 2.0f + s_wiz_carb / 10.0f;
                if (sug < 0) sug = 0;
                ui_hal_deliver_bolus(sug, BOLUS_WIZARD, 0, sug, 0); back_to(SCR_MENU);
            }
            break;

        case SCR_BOLUS_MEALS: {
            static const float def[3] = { 6.0f, 8.0f, 6.0f };
            if (k == KEY_UP)        s_sel = (s_sel + 2) % 3;
            else if (k == KEY_DOWN) s_sel = (s_sel + 1) % 3;
            else if (k == KEY_SET)  { ui_hal_deliver_bolus(def[s_sel], BOLUS_MEAL, 0, def[s_sel], 0); back_to(SCR_MENU); }
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            break;
        }

        case SCR_PRIME:
            if (k == KEY_SET)  ui_hal_start_prime();   // 后端: 固件入队 PRIME+状态+历史; 模拟器置 PRIMING
            else if (k == KEY_ESC) back_to(SCR_MENU);
            break;

        case SCR_ALARM_LIST:
            list_key(k, 7, SCR_MENU, []() { s_alarm_sel = s_sel; enter_child(SCR_ALARM_DETAIL); });
            break;

        case SCR_ALARM_DETAIL:
            if (k == KEY_SET || k == KEY_ESC) back_to(SCR_ALARM_LIST);
            break;

        case SCR_LOOP:
            if (k == KEY_ESC) back_to(SCR_MENU);
            break;

        case SCR_SETTINGS:
            if (k == KEY_UP)        s_sel = (s_sel + 3) % 4;
            else if (k == KEY_DOWN) s_sel = (s_sel + 1) % 4;
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            // 确认: 亮度/按键音 仅演示切换, 不进子页
            else if (k == KEY_SET && s_sel == 1) ui_hal_set_brightness(50);   // 亮度演示
            else if (k == KEY_SET && s_sel == 2) ui_hal_toggle_keypad_sound(); // 按键音
            break;

        default: break;
    }
}

// 屏幕按钮回调
static void btn_cb(lv_event_t *e)
{
    key_event_t k = (key_event_t)(intptr_t)lv_event_get_user_data(e);
    handle_key(k);
}

// ============================================================
// 初始化 / 刷新 / 外部接口
// ============================================================
void ui_screen_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_page = lv_obj_create(scr);
    lv_obj_set_size(s_page, 320, 146);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);

    // 底部 4 个常驻控制按钮 (上 / 下 / 确认 / 返回)
    const char *bt[4] = { "上", "下", "确认", "返回" };
    key_event_t bk[4] = { KEY_UP, KEY_DOWN, KEY_SET, KEY_ESC };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(scr);
        lv_obj_set_size(b, 76, 22);
        lv_obj_set_pos(b, 4 + i * 80, 148);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xeef2f6), 0);        // 浅灰按钮
        lv_obj_set_style_bg_color(b, ACCENT, LV_STATE_PRESSED);         // 按下变医疗蓝
        lv_obj_set_style_border_color(b, LINE, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_t *lb = lv_label_create(b);
        lv_label_set_text(lb, bt[i]);
        lv_obj_set_style_text_font(lb, FONT_SM, 0);
        lv_obj_set_style_text_color(lb, TEXT, 0);                      // 深色文字
        lv_obj_set_style_text_color(lb, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_center(lb);
        lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)bk[i]);
    }
    s_screen = SCR_HOME; s_sel = 0;
}

void ui_screen_refresh(void)
{
    lv_obj_clean(s_page);
    draw_current();
}

void ui_screen_key(key_event_t k) { handle_key(k); }

void ui_set_screen(int s) { s_screen = s; s_sel = 0; }
int  ui_get_screen(void)  { return s_screen; }
