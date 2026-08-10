/**
 * ui_screen.cpp — 全中文 UI 页面状态机 (LVGL, PC/SDL 版)
 *
 * 横屏 320×172, 4 物理按键(上/下/确认/返回)导航。
 * 每帧 ui_screen_refresh() 重建当前页 (clean + draw), 简单且无状态同步问题。
 * 复用了固件的 g_pump_state, 演示数据来自 ui_hal (模拟器后端 = ui_hal_sim)。
 */
#include <Arduino.h>
#include "ui_screen.h"
#include "pump_state.h"
#include "pump_types.h"
#include "ui_hal.h"
#include "config.h"
#include "history_log.h"   // P0-4: 历史事件读取 (旧, 保留供兼容)
#include "basal_history.h"  // 基础率执行历史 (时间轴/用量柱状图)
#include "dose_log.h"       // 紧凑剂量追溯日志 (主日志查看器)
#include "rtc_clock.h"     // rtc_unix_to_ymdhms (头部内联, 双端共用)
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
    SCR_SETTINGS, SCR_CLOCK_SET, SCR_ABOUT,
    SCR_HISTORY,         // P0-4: 历史事件回看
    SCR_TBR,             // P2-9: 本地临时基础率(TBR) 设置/取消
    SCR_PROFILE,         // P2-10: 基础率方案切换
    SCR_MISSED_BOLUS,    // P2-12: 错过大剂量提醒 查看/清除
    SCR_REWIND_CAL,     // P3-14: 回退装药 + 剂量标定
    SCR_PROFILE_DETAIL, // #188: 方案详情(切换/重命名/编辑/预览/记录/复制/重置)
    SCR_PROFILE_RENAME, // #188: 方案重命名字符编辑器
    SCR_BASAL_CHART,    // #189: 24h 基础率预览柱状图
    SCR_BASAL_HISTORY   // #189: 基础率执行历史(时间轴+用量柱状图)
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

    // 基础率单段编辑: 速率步进与上下限 (U/h)
    static const float BASAL_RATE_STEP = 0.1f;
    static const float BASAL_RATE_MIN  = 0.0f;
    static const float BASAL_RATE_MAX  = 10.0f;
    static float s_edit_rate = 0.0f;   // 编辑态临时值 (SET 提交 / ESC 取消)

    // P2-9: 本地临时基础率(TBR) 编辑态
    static int   s_tbr_pct   = 100;    // 百分比 (0=取消, 10-500 步长 10)
    static int   s_tbr_dur_30 = 0;     // 时长 (30 分钟单位, 0-48 → 0-24h; 0=取消)
    // P2-10: 基础率方案切换选中
    static int   s_profile_sel = 0;
    // P3-14: 剂量标定实测体积输入(U)
    static float s_cal_measured = 1.0f;
    // P3-14: 手动退药量输入(U)
    static float s_rewind_units = 5.0f;

    // #188: 档案管理 / 时间轴
    static int   s_edit_profile = 0;        // SCR_BASAL / SCR_BASAL_CHART 当前编辑/预览的方案索引
    static int   s_basal_parent  = SCR_MENU;// SCR_BASAL 返回父页
    static const int PROF_DETAIL_N = 8;     // SCR_PROFILE_DETAIL 菜单项数 (含 #260 验证测试)
    static int   s_prof_detail_sel = 0;     // SCR_PROFILE_DETAIL 子菜单选中
    // 0=菜单 1=复制选择 2=重置确认 3=验证测试确认(#260) 4=测试已下发提示 5=测试未下发(报因)
    static int   s_prof_action = 0;
    static int   s_copy_dst = 0;            // 复制目标方案索引
    static float s_prof_test_units = 0.0f;  // #260 上次验证测试实际下发量 (U)
    // 重命名字符编辑器: 调色板(空格+数字+字母+常用字)
    static const char *NAME_CHARS[] = {
        " ", "0","1","2","3","4","5","6","7","8","9",
        "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
        "昼","夜","周","工","作","日","末","运","动","常","休","假","旅","高","低","糖","饭","敏","世","通","泵"
    };
    #define NAME_CHAR_N ((int)(sizeof(NAME_CHARS)/sizeof(NAME_CHARS[0])))
    static int   s_name_chars[31];
    static int   s_name_len = 0;
    static int   s_name_cur = 0;            // 0..s_name_len(追加槽)..s_name_len+1(保存槽)
    static int   s_name_append = 0;         // 追加槽待插入字符的调色板索引
    // #189: 执行历史时间轴筛选 (0=本方案 1=全部)
    static int   s_hist_filter = 0;

// 时钟设置页编辑状态
static int  s_clk_field = 0;          // 0=年 1=月 2=日 3=时 4=分 5=保存
static int  s_clk_y = 2026, s_clk_mo = 1, s_clk_d = 1, s_clk_h = 0, s_clk_mi = 0;

// 系统设置页: 亮度编辑态 (SET 进入/退出, 期间上下调亮度)
static int  s_set_edit = 0;

// ---- 排气/装药 (prime) 状态 ----
static float s_prime_u    = 1.0f;        // 排气量 (U), 0.5~10.0, 步进0.5U, 可▲▼调节
static uint32_t s_prime_start = 0;       // PRIMING 起始 tick (用于自动结束)

// ---- 按住自动重复 (长按上下键持续加减) ----
static key_event_t s_rep_key   = KEY_NONE;  // 当前按住方向 (仅 UP/DOWN)
static uint32_t    s_rep_at    = 0;         // 下次重复触发 tick
static uint32_t    s_rep_count = 0;         // 重复次数 (用于加速)

// ---- 文本映射 ----
static const char *trend_str(int8_t t)
{
    switch (t) {
        case 2:  return "速升 ↑↑";
        case 1:  return "缓升 ↗";
        case -1: return "缓降 ↘";
        case -2: return "速降 ↓↓";
        default: return "平稳 →";
    }
}
static lv_color_t trend_color(int8_t t)
{
    switch (t) {
        case 2:  return RED;     // 快速上升 (警示)
        case 1:  return YELLOW;  // 缓升
        case -1: return GREEN;   // 缓降
        case -2: return GREEN;   // 速降
        default: return DIM;     // 平稳
    }
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

// ---- 方案重命名: 名称编解码 (调色板索引 <-> UTF-8 串) ----
static void name_decode(const char *src)
{
    s_name_len = 0;
    const char *p = src;
    while (*p && s_name_len < 31) {
        unsigned char c = (unsigned char)*p;
        int w = 1;
        if (c >= 0xF0) w = 4; else if (c >= 0xE0) w = 3; else if (c >= 0xC0) w = 2;
        char ch[5] = {0};
        memcpy(ch, p, w);
        int idx = 0;   // 未命中 -> 空格
        for (int k = 1; k < NAME_CHAR_N; k++) { if (strcmp(NAME_CHARS[k], ch) == 0) { idx = k; break; } }
        s_name_chars[s_name_len++] = idx;
        p += w;
    }
    s_name_cur = s_name_len;
}
static void name_encode(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < s_name_len; i++)
        strncat(out, NAME_CHARS[s_name_chars[i]], cap - strlen(out) - 1);
}
// 带光标括号渲染: cur<len 时高亮该字; 否则末尾光标
static void name_render(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < s_name_len; i++) {
        if (i == s_name_cur) strncat(out, "[", cap - strlen(out) - 1);
        strncat(out, NAME_CHARS[s_name_chars[i]], cap - strlen(out) - 1);
        if (i == s_name_cur) strncat(out, "]", cap - strlen(out) - 1);
    }
    if (s_name_cur >= s_name_len) strncat(out, "▏", cap - strlen(out) - 1);
}

// ---- 基础率执行历史事件名 ----
static const char *bh_name(uint8_t t)
{
    switch (t) {
        case BH_PROFILE_SWITCH: return "切方案";
        case BH_TBR_START:      return "临时率";
        case BH_TBR_END:        return "临时率止";
        case BH_MODE_CHANGE:    return "模式切换";
        case BH_AAPS_TAKEOVER:  return "AAPS接管";
        case BH_BASAL_ACTIVE:   return "基础率生效";   // #258 速率变化打点
        case BH_BASAL_TEST:     return "验证测试";     // #260 全天量一次性打出
        default:                return "事件";
    }
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

// 1px 分隔线 (用于列表与页脚之间, 防止重叠)
static void hline(int x0, int x1, int y, lv_color_t c)
{
    lv_obj_t *ln = lv_obj_create(s_page);
    lv_obj_set_size(ln, x1 - x0, 1);
    lv_obj_set_pos(ln, x0, y);
    lv_obj_set_style_bg_color(ln, c, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_set_style_pad_all(ln, 0, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
}

// 页脚提示: 分隔线(y=126) + 文字(y=130), 固定在列表下方, 不与菜单选项重叠
static void footer_hint(int x, const char *t, lv_color_t c)
{
    hline(4, 316, 126, LINE);
    L(x, 130, t, FONT_SM, c);
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

    // 时钟: 未设置显示 "--:--"
    if (hh < 0) snprintf(buf, sizeof buf, "--:--");
    else        snprintf(buf, sizeof buf, "%02d:%02d", hh, mm);
    L(4, 2, buf, FONT_SM, lv_color_white());                     // 时钟 (白)
    L(112, 1, "闭环胰岛素泵", FONT_MAIN, lv_color_white());      // 标题 (白)
    snprintf(buf, sizeof buf, "电池 %d%%", g_pump_state.battery_pct);
    L(250, 2, buf, FONT_SM, g_pump_state.battery_pct < 20 ? YELLOW : lv_color_white());  // 电池 (白/低电黄)

    // ---- 左栏: CGM ----
    L(4, 22, "血糖 (CGM)", FONT_SM, DIM);
    float gm = ui_hal_glucose_mmol();
    bool gvalid = ui_hal_glucose_valid();
    if (!gvalid) {
        L(4, 40, "CGM 离线", FONT_MAIN, DIM);                   // 无数据 / 过期 >10min
    } else {
        snprintf(buf, sizeof buf, "%.1f", gm);
        L(4, 40, buf, FONT_MAIN, TITLE);                        // 血糖大字 (mmol/L)
    }
    L(70, 46, "mmol/L", FONT_SM, DIM);
    if (!gvalid) {
        L(4, 66, "--", FONT_SM, DIM);
    } else {
        L(4, 66, trend_str(ui_hal_glucose_trend()), FONT_SM, trend_color(ui_hal_glucose_trend()));
    }
    snprintf(buf, sizeof buf, "闭环: %s", loop_str(ui_hal_loop_mode()));
    L(4, 90, buf, FONT_SM, loop_color(ui_hal_loop_mode()));

    // ---- 右栏: 泵 ----
    L(168, 22, "基础率", FONT_SM, DIM);
    snprintf(buf, sizeof buf, "%.2f U/h", g_pump_state.current_basal_rate);
    L(168, 40, buf, FONT_MAIN, TEXT);
    L(168, 64, "剩余药量", FONT_SM, DIM);
    snprintf(buf, sizeof buf, "%d U", g_pump_state.reservoir_units_left);
    lv_color_t res_col = g_pump_state.reservoir_low_warn ? YELLOW : TEXT;
    L(168, 80, buf, FONT_MAIN, res_col);
    if (g_pump_state.reservoir_low_warn) {
        L(168, 64, "⚠ 剩余药量", FONT_SM, YELLOW);   // P0-3: 低药量预警(非阻塞)
    }
    lv_obj_t *bar = lv_bar_create(s_page);
    lv_bar_set_range(bar, 0, MAX_RESERVOIR_UNITS);
    lv_bar_set_value(bar, g_pump_state.reservoir_units_left, LV_ANIM_OFF);
    lv_obj_set_size(bar, 138, 8);
    lv_obj_set_pos(bar, 168, 98);
    lv_obj_set_style_bg_color(bar, LINE, LV_PART_MAIN);         // 浅灰轨道
    lv_obj_set_style_bg_color(bar, ACCENT, LV_PART_INDICATOR); // 蓝色进度

    // ---- 底部信息行 ----
    // ---- 电机实时电流 (调试: 实时显示 INA226 电流, 对照堵转阈值) ----
    {
        uint16_t mc  = g_pump_state.motor_current_ma;
        uint16_t thr = g_pump_config.occlusion_threshold;
        bool occl = (thr > 0) && (mc >= thr);
        snprintf(buf, sizeof buf, "电流 %d mA%s", mc, occl ? " 堵转!" : "");
        L(168, 108, buf, FONT_SM, occl ? RED : (mc > 0 ? GREEN : DIM));
    }
    snprintf(buf, sizeof buf, "今日 %.1f U", ui_hal_today_total());
    L(4, 122, buf, FONT_SM, TEXT);
    snprintf(buf, sizeof buf, "IOB %.2f U", g_pump_state.iob_x10000 / 10000.0f);
    L(168, 122, buf, FONT_SM, TEXT);

    static char s_status[48];
    const char *st;
    lv_color_t st_col = TEXT;
    if (g_pump_state.alarm_active) {
        st = "⚠ 报警";
        st_col = RED;
    } else if (ui_hal_bolus_active()) {     // 大剂量分段打入进行中
        snprintf(s_status, sizeof s_status, "大剂量注射中 %d%% (ESC取消)",
                 g_pump_state.bolus_progress_pct);
        st = s_status;
        st_col = ACCENT;
    } else {
        st = state_str(g_pump_state.current_state);
    }
    L(4, 132, st, FONT_SM, st_col);
    if (g_pump_state.keypad_locked)
        L(150, 132, "已锁: 按确认解锁", FONT_SM, YELLOW);
    else
        L(150, 132, "确认键进入菜单", FONT_SM, DIM);

    // P2-11 按键锁 / P2-12 错过大剂量 / P3-13 过温 底部徽标
    char badge[48] = "";
    if (g_pump_state.keypad_locked) strcat(badge, "已锁 ");
    if (g_pump_state.missed_bolus)   strcat(badge, "错过剂量! ");
    if (g_pump_state.over_temp_warn) strcat(badge, "过温!");
    if (badge[0]) L(4, 146, badge, FONT_SM, YELLOW);
}

static void draw_menu(void)
{
    title("主菜单");
    const char *names[9] = { "基础率", "大剂量", "排气装药", "报警", "闭环", "临时基础率", "历史记录", "系统设置", "回退/标定" };
    char v[9][24];
    snprintf(v[0], sizeof v[0], "%.2f U/h", g_pump_state.current_basal_rate);
    strcpy(v[1], "→");
    strcpy(v[2], "→");
    snprintf(v[3], sizeof v[3], g_pump_state.alarm_active ? "1 条" : "正常");
    snprintf(v[4], sizeof v[4], "%s", loop_str(ui_hal_loop_mode()));
    snprintf(v[5], sizeof v[5], g_pump_state.tbr_percent > 0 ? "%.0f%%" : "无", g_pump_state.tbr_percent);
    snprintf(v[6], sizeof v[6], "%d 条", history_log_count());
    strcpy(v[7], "→");
    snprintf(v[8], sizeof v[8], "%.3f", g_pump_config.dose_calibration);

    // 滚动窗口: 仅画可见行, 选中项保持可视 (主菜单 9 项)。无底部通用页脚提示(已移除冗余),
    // 列表铺满全屏; vis=7 末行 y<=124, 在 s_page(146) 内。
    const int cnt = 9, vis = 7;
    int top = s_sel - (vis - 1);
    if (top < 0) top = 0;
    if (top > cnt - vis) top = cnt - vis;
    if (top < 0) top = 0;
    for (int r = 0; r < vis; r++) {
        int idx = top + r;
        if (idx >= cnt) break;
        int y = 22 + r * 17;
        bool sel = (idx == s_sel);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", names[idx]);
        L(8, y, line, FONT_MAIN, sel ? ACCENT : TEXT);
        L(220, y, v[idx], FONT_MAIN, sel ? ACCENT : DIM);
    }
}

static void draw_basal(void)
{
    char nm[32];
    ui_hal_profile_name(s_edit_profile, nm, sizeof nm);
    bool active = (s_edit_profile == (int)g_pump_config.active_profile);
    char tbuf[40];
    if (active)
        snprintf(tbuf, sizeof tbuf, "基础率 · %s ★当前", nm);
    else
        snprintf(tbuf, sizeof tbuf, "基础率 · %s (未启用)", nm);
    title(tbuf);

    int cnt = BASAL_SLOTS_PER_DAY;
    int top = s_sel - 3;
    if (top < 0) top = 0;
    if (top > cnt - 7) top = cnt - 7;
    if (top < 0) top = 0;
    for (int r = 0; r < 6; r++) {
        int idx = top + r;
        if (idx >= cnt) break;
        int y = 40 + r * 14;
        bool sel = (idx == s_sel);
        float rate = (s_set_edit && idx == s_sel)
                     ? s_edit_rate
                     : ui_hal_profile_basal_rate(s_edit_profile, idx);
        char line[40];
        snprintf(line, sizeof line, "%s %02d:00  %.2f U/h", sel ? "▶" : " ", idx, rate);
        L(12, y, line, FONT_SM, sel ? ACCENT : TEXT);
    }
    if (s_set_edit)
        footer_hint(12, "▲▼ 调整速率  确认保存  返回取消", ACCENT);
    else if (active && !ui_hal_basal_local_mode())
        footer_hint(12, "AAPS接管 确认切本地档案", DIM);
    else
        footer_hint(12, "上下选择  确认编辑段  返回", DIM);
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
    snprintf(buf, sizeof buf, "%.1f", s_dose);
    L(120, 28, buf, FONT_MAIN, ACCENT);
    L(20, 70, "▲▼ 调整剂量  确认输注  返回", FONT_SM, DIM);
    L(20, 100, "单次最大 25.00 U", FONT_SM, DIM);
}

static void draw_bolus_square(void)
{
    title("方波大剂量");
    char buf[32];
    snprintf(buf, sizeof buf, "%s 剂量  %.1f U", s_sel == 0 ? "▶" : " ", s_dose);
    L(20, 30, buf, FONT_MAIN, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 时长  %d h", s_sel == 1 ? "▶" : " ", s_dur_h);
    L(20, 58, buf, FONT_MAIN, s_sel == 1 ? ACCENT : TEXT);
    L(20, 96, "▲▼ 调整  确认切换/输注  返回", FONT_SM, DIM);
}

static void draw_bolus_dual(void)
{
    title("双波大剂量");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 立即量  %.1f U", s_sel == 0 ? "▶" : " ", s_dose_imme);
    L(16, 26, buf, FONT_SM, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 方波量  %.1f U", s_sel == 1 ? "▶" : " ", s_dose_sq);
    L(16, 50, buf, FONT_SM, s_sel == 1 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 时长    %d h", s_sel == 2 ? "▶" : " ", s_dur_h);
    L(16, 74, buf, FONT_SM, s_sel == 2 ? ACCENT : TEXT);
    L(16, 104, "▲▼ 调整  确认切换/输注  返回", FONT_SM, DIM);
}

// P1-8: 向导建议剂量 = (血糖-目标)/ISF + 碳水/CR
//   全部查 g_pump_config 的当前小时档 (替换原先硬编码 目标6.0/ISF 2.0/CR 10),
//   与目标血糖/ISF/碳水比单位一致 (mg/dL 体系, 与 AAPS 口径对齐)。
static float wizard_suggest(float bg_mmol, float carb_g)
{
    int hour = 12;   // 默认档 (默认值全天一致, slot 任意即可)
#ifndef SIMULATOR
    if (rtc_is_set()) hour = (int)((rtc_unix_now() / 3600U) % 24U);
#endif
    float target = (float)g_pump_config.target_glucose[hour];
    float isf    = g_pump_config.isf[hour];
    float cr     = g_pump_config.carb_ratio[hour];
    if (isf    <= 0.0f) isf    = 40.0f;   // 安全兜底 (未配置)
    if (cr     <= 0.0f) cr     = 10.0f;
    if (target <= 0.0f) target = 100.0f;
    float bg_mgdl    = bg_mmol * 18.018f;            // mmol/L → mg/dL
    float correction = (bg_mgdl - target) / isf;     // 校正量 U
    float food       = carb_g / cr;                  // 餐食量 U
    float sug = correction + food;
    if (sug < 0.0f) sug = 0.0f;
    return sug;
}

static void draw_bolus_wizard(void)
{
    title("向导大剂量");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 血糖  %.1f mmol/L", s_sel == 0 ? "▶" : " ", s_wiz_bg);
    L(16, 26, buf, FONT_SM, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 碳水  %.0f g", s_sel == 1 ? "▶" : " ", s_wiz_carb);
    L(16, 50, buf, FONT_SM, s_sel == 1 ? ACCENT : TEXT);
    // 建议剂量 = (血糖-目标)/ISF + 碳水/CR (目标/ISF/CR 查 g_pump_config 当前小时档)
    float sug = wizard_suggest(s_wiz_bg, s_wiz_carb);
    if (sug < 0) sug = 0;
    sug = quantize_units_grid(sug);          // 吸附 0.1U 最小剂量网格, 与实投一致
    snprintf(buf, sizeof buf, "建议: %.1f U", sug);
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
    L(12, 26, "3mL 注射器 (储药器)", FONT_SM, TEXT);
    bool priming = (g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING);
    if (priming) {
        L(12, 50, "状态: 排气中...", FONT_SM, YELLOW);
        L(12, 74, "电机正在推注, 请稍候", FONT_SM, DIM);
        L(40, 124, "返回取消排气", FONT_SM, DIM);
    } else {
        char vol[40];
        snprintf(vol, sizeof vol, "排气量: %.1f U", s_prime_u);
        L(12, 50, vol, FONT_SM, ACCENT);
        L(12, 74, "状态: 待机", FONT_SM, DIM);
        L(40, 124, "▲▼调量  确认排气  返回", FONT_SM, DIM);
    }
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
    // 区分"已连接"与"AAPS 完成 Dana 握手接管"
    const char *pair = ui_hal_dana_paired() ? "已接管" : (ui_hal_loop_connected() ? "未接管" : "—");
    lv_color_t pc = ui_hal_dana_paired() ? GREEN : (ui_hal_loop_connected() ? YELLOW : DIM);
    L(12, 44, "接管:", FONT_SM, DIM);
    L(80, 44, pair, FONT_SM, pc);
    if (ui_hal_glucose_valid()) {
        snprintf(buf, sizeof buf, "血糖: %.1f mmol/L", ui_hal_glucose_mmol());
    } else {
        snprintf(buf, sizeof buf, "血糖: CGM 离线");
    }
    L(12, 50, buf, FONT_SM, TEXT);
    L(12, 72, ui_hal_glucose_valid() ? trend_str(ui_hal_glucose_trend()) : "--",
      FONT_SM, ui_hal_glucose_valid() ? trend_color(ui_hal_glucose_trend()) : DIM);
    snprintf(buf, sizeof buf, "模式: %s", loop_str(ui_hal_loop_mode()));
    L(12, 94, buf, FONT_SM, loop_color(ui_hal_loop_mode()));
    float tbr = ui_hal_tbr_percent();
    if (tbr > 0) {
        snprintf(buf, sizeof buf, "临时基础率: %.0f%% (%.2f U/h)", tbr, ui_hal_tbr_rate());
        L(12, 116, buf, FONT_SM, YELLOW);
    } else {
        L(12, 116, "临时基础率: 无", FONT_SM, DIM);
    }
    L(100, 134, "确认切换模式  返回", FONT_SM, DIM);
}

static void draw_settings(void)
{
    title("系统设置");
    const char *names[8] = { "日期时间", "屏幕亮度", "按键音", "基础率方案", "按键锁", "错过大剂量", "振动反馈", "关于" };
    char vals[8][24];
    snprintf(vals[0], sizeof vals[0], "%s", ui_hal_clock_valid() ? "已设置" : "未设置");
    if (s_set_edit)
        snprintf(vals[1], sizeof vals[1], "%d%% [调]", ui_hal_get_brightness());
    else
        snprintf(vals[1], sizeof vals[1], "%d%%", ui_hal_get_brightness());
    snprintf(vals[2], sizeof vals[2], "%s", ui_hal_get_keypad_sound() ? "开" : "关");
    snprintf(vals[3], sizeof vals[3], "方案%d", (int)g_pump_config.active_profile + 1);
    snprintf(vals[4], sizeof vals[4], "%s", g_pump_state.keypad_locked ? "已锁" : "未锁");
    snprintf(vals[5], sizeof vals[5], "%s", g_pump_state.missed_bolus ? "1 条" : "无");
    snprintf(vals[6], sizeof vals[6], "%s", ui_hal_get_vibrate_enabled() ? "开" : "关");   // P3-15
    strcpy(vals[7], "→");
    // 滚动窗口: 仅画可见行, 选中项始终保持在可视区内。无底部通用页脚提示(已移除冗余),
    // 列表铺满全屏; vis=7 末行 y<=112, 在 s_page(146) 内。
    const int cnt = 8, vis = 7;
    int top = s_sel - (vis - 1);
    if (top < 0) top = 0;
    if (top > cnt - vis) top = cnt - vis;
    if (top < 0) top = 0;
    for (int r = 0; r < vis; r++) {
        int idx = top + r;
        if (idx >= cnt) break;
        int y = 22 + r * 15;
        bool sel = (idx == s_sel);
        char line[32];
        snprintf(line, sizeof line, "%s %s", sel ? "▶" : " ", names[idx]);
        L(16, y, line, FONT_MAIN, sel ? ACCENT : TEXT);
        L(220, y, vals[idx], FONT_MAIN, sel ? ACCENT : DIM);
    }
    if (s_set_edit)
        footer_hint(36, "亮度编辑中: 上下调节  确认/返回完成", ACCENT);
}

static void draw_clock_set(void)
{
    title("设置时间");
    const char *labels[6] = { "年", "月", "日", "时", "分", "保存" };
    int vals[5] = { s_clk_y, s_clk_mo, s_clk_d, s_clk_h, s_clk_mi };
    char buf[16];
    for (int i = 0; i < 6; i++) {
        int y = 24 + i * 17;
        bool sel = (i == s_clk_field);
        if (i < 5) snprintf(buf, sizeof buf, "%s %02d", labels[i], vals[i]);
        else       snprintf(buf, sizeof buf, "%s %s", labels[i], "✔ 保存并返回");
        L(40, y, buf, FONT_MAIN, sel ? ACCENT : TEXT);
    }
    L(100, 134, "上下调整  确认下一项  返回取消", FONT_SM, DIM);
}

static void draw_about(void)
{
    title("关于");
    L(12, 26, "OpenLoop 闭环胰岛素泵", FONT_MAIN, TITLE);
    L(12, 50, "理论验证 / 教学原型", FONT_SM, DIM);
    L(12, 74, "硬件: ESP32-C6 + DRV8825", FONT_SM, TEXT);
    L(12, 96, "⚠ 严禁用于人体", FONT_SM, RED);
    char tbuf[40];
    snprintf(tbuf, sizeof tbuf, "板温 %.1f°C (阈 %.0f°C)", g_pump_state.board_temp_c, g_pump_config.over_temp_threshold_c);
    L(12, 116, tbuf, FONT_SM, g_pump_state.over_temp_warn ? YELLOW : DIM);
    L(100, 134, "返回", FONT_SM, DIM);
}

// P2-9: 本地临时基础率(TBR) 设置/取消
static void draw_tbr(void)
{
    title("临时基础率");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 百分比  %d%%", s_sel == 0 ? "▶" : " ", s_tbr_pct);
    L(16, 26, buf, FONT_SM, s_sel == 0 ? ACCENT : TEXT);
    int mins = s_tbr_dur_30 * 30;
    char dstr[16];
    if (mins == 0)            strcpy(dstr, "取消(无)");
    else if (mins % 60 == 0)  snprintf(dstr, sizeof dstr, "%d 小时", mins / 60);
    else                      snprintf(dstr, sizeof dstr, "%d 分钟", mins);
    snprintf(buf, sizeof buf, "%s 时长    %s", s_sel == 1 ? "▶" : " ", dstr);
    L(16, 50, buf, FONT_SM, s_sel == 1 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 应用", s_sel == 2 ? "▶" : " ");
    L(16, 78, buf, FONT_MAIN, s_sel == 2 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 取消临时基础率", s_sel == 3 ? "▶" : " ");
    L(16, 102, buf, FONT_MAIN, s_sel == 3 ? ACCENT : TEXT);
    float ref  = (g_pump_state.current_basal_rate > 0.0f) ? g_pump_state.current_basal_rate : 0.5f;
    float rate = s_tbr_pct / 100.0f * ref;
    snprintf(buf, sizeof buf, "≈ %.2f U/h", rate);
    L(16, 126, buf, FONT_SM, DIM);
    footer_hint(16, "▲▼ 调整  确认执行  返回", DIM);
}

// P2-10: 基础率方案切换
// P2-10 / #188: 基础率方案列表 (确认进入方案详情)
static void draw_profile(void)
{
    title("基础率方案");
    char buf[40], nm[32];
    for (int i = 0; i < MAX_BASAL_PROFILES; i++) {
        int y = 30 + i * 22;
        bool sel = (i == s_sel);
        bool act = (i == (int)g_pump_config.active_profile);
        ui_hal_profile_name(i, nm, sizeof nm);
        snprintf(buf, sizeof buf, "%s %s 方案%d", sel ? "▶" : " ", act ? "★" : " ", i + 1);
        L(16, y, buf, FONT_MAIN, sel ? ACCENT : (act ? GREEN : TEXT));
        snprintf(buf, sizeof buf, "%s  %.2fU/h", nm, ui_hal_profile_basal_rate(i, 12));
        L(150, y, buf, FONT_SM, DIM);
    }
    L(16, 132, "▲▼ 选择  确认详情  返回", FONT_SM, DIM);
}

// #188: 方案详情子菜单
static void draw_profile_detail(void)
{
    char nm[32];
    ui_hal_profile_name(s_edit_profile, nm, sizeof nm);
    char tbuf[40];
    snprintf(tbuf, sizeof tbuf, "方案%d · %s%s", s_edit_profile + 1, nm,
             (s_edit_profile == (int)g_pump_config.active_profile) ? " ★" : "");
    title(tbuf);
    const char *items[PROF_DETAIL_N] = {
        "① 设为当前方案", "② 重命名", "③ 编辑24段基础率",
        "④ 24h 预览图",   "⑤ 执行记录", "⑥ 复制本方案", "⑦ 重置为默认",
        "⑧ 验证测试·打全天量"
    };
    // 8 项在 172px 高的屏上按 14px 行距排, 末项仍留出底部提示行
    for (int idx = 0; idx < PROF_DETAIL_N; idx++) {
        int y = 20 + idx * 14;
        bool sel = (idx == s_prof_detail_sel);
        L(12, y, items[idx], FONT_SM, sel ? ACCENT : TEXT);
    }
    if (s_prof_action == 1) {       // 复制: 选择目标
        snprintf(tbuf, sizeof tbuf, "复制到: 方案%d (▲▼选 确认复制 ESC取消)",
                 s_copy_dst + 1);
        L(12, 136, tbuf, FONT_SM, YELLOW);
    } else if (s_prof_action == 2) { // 重置确认
        L(12, 136, "确认重置为默认? 再按确认 ESC取消", FONT_SM, RED);
    } else if (s_prof_action == 3) { // #260 验证测试确认 (会真的打药, 必须二次确认)
        snprintf(tbuf, sizeof tbuf, "将一次性注射 %.1fU! 再按确认 ESC取消",
                 ui_hal_basal_daily_total());
        L(12, 136, tbuf, FONT_SM, RED);
    } else if (s_prof_action == 4) { // 测试已下发, 显示实际量
        snprintf(tbuf, sizeof tbuf, "已下发测试 %.1fU, 见执行记录", s_prof_test_units);
        L(12, 136, tbuf, FONT_SM, GREEN);
    } else if (s_prof_action == 5) { // #260 未能下发: 必须说明原因
        // ⚠️ 这里恰恰是最有价值的诊断信号 —— "档案总量为 0" 就等价于
        //    "基础率设置压根没落到泵里", 与用户报的"电机不动"是同一个病根。
        //    若这里静默退回菜单, 用户只会以为按钮坏了, 反而丢掉了关键线索。
        if (ui_hal_basal_daily_total() <= 0.0f) {
            L(12, 136, "全天总量为0! 基础率未写入泵", FONT_SM, RED);
        } else {
            L(12, 136, "未下发: 余量/上限不足或电机忙", FONT_SM, YELLOW);
        }
    } else {
        L(12, 136, "▲▼ 选择  确认执行  返回", FONT_SM, DIM);
    }
}

// #188: 方案重命名字符编辑器
static void draw_profile_rename(void)
{
    char tbuf[40];
    snprintf(tbuf, sizeof tbuf, "重命名 方案%d", s_edit_profile + 1);
    title(tbuf);
    char shown[64];
    name_render(shown, sizeof shown);
    L(8, 22, "名称:", FONT_SM, DIM);
    L(64, 20, shown, FONT_MAIN, TITLE);
    char a1[24];
    snprintf(a1, sizeof a1, "＋追加 [%s]", NAME_CHARS[s_name_append]);
    int y = 54;
    bool sa = (s_name_cur == s_name_len);
    bool ss = (s_name_cur == s_name_len + 1);
    L(12, y,    a1, FONT_SM, sa ? ACCENT : DIM);
    L(12, y+18, "✔ 保存并退出", FONT_SM, ss ? ACCENT : DIM);
    snprintf(tbuf, sizeof tbuf, "字库 %d/%d", s_name_append + 1, NAME_CHAR_N);
    L(190, y, tbuf, FONT_SM, DIM);
    L(12, 134, "上下选字/项  确认  返回退格", FONT_SM, DIM);
}

// #189: 24h 基础率预览柱状图
static void draw_basal_chart(void)
{
    char nm[32];
    ui_hal_profile_name(s_edit_profile, nm, sizeof nm);
    char tbuf[40];
    snprintf(tbuf, sizeof tbuf, "24h 预览 · 方案%d %s", s_edit_profile + 1, nm);
    title(tbuf);
    float mx = 1.0f;
    for (int h = 0; h < 24; h++) {
        float r = ui_hal_profile_basal_rate(s_edit_profile, h);
        if (r > mx) mx = r;
    }
    int x0 = 8, x1 = 312, y0 = 130, hgt = 92;
    snprintf(tbuf, sizeof tbuf, "%.1fU/h", mx);
    L(x0, 20, tbuf, FONT_SM, DIM);
    int bw = (x1 - x0) / 24;
    for (int i = 0; i < 24; i++) {
        float r = ui_hal_profile_basal_rate(s_edit_profile, i);
        int bh = (int)(r / mx * hgt);
        if (bh < 0) bh = 0;
        int bx = x0 + i * bw;
        lv_obj_t *bar = lv_obj_create(s_page);
        lv_obj_set_size(bar, bw - 1, bh);
        lv_obj_set_pos(bar, bx, y0 - bh);
        lv_obj_set_style_bg_color(bar, ACCENT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        if (i % 6 == 0) {
            char hb[8]; snprintf(hb, sizeof hb, "%02d", i);
            L(bx, y0 + 2, hb, FONT_SM, DIM);
        }
    }
    L(12, 134, "▲▼ 切换方案  返回", FONT_SM, DIM);
}

// #189: 基础率执行历史 (时间轴 + 用量轴 柱状图)
static void draw_basal_history(void)
{
    char nm[32];
    ui_hal_profile_name(s_edit_profile, nm, sizeof nm);
    char tbuf[40];
    snprintf(tbuf, sizeof tbuf, "执行记录 · %s", (s_hist_filter == 0) ? nm : "全部方案");
    title(tbuf);
    uint32_t n = basal_history_count();
    if (n == 0) {
        L(20, 60, "（暂无执行记录）", FONT_SM, DIM);
        L(60, 134, "返回", FONT_SM, DIM);
        return;
    }
    basal_history_rec_t recs[64];
    uint32_t m = (n < 64) ? n : 64;
    for (uint32_t i = 0; i < m; i++) basal_history_read(i, &recs[i]);
    uint32_t now = basal_history_now();
    // 段: recs[i] 起始时段, 结束于 recs[i-1].ts(或 now); 过滤本方案
    struct { uint32_t ins_x100; uint32_t dur_min; uint8_t type; uint8_t profile; uint16_t rate_x100; uint32_t ts; } segs[64];
    int ns = 0;
    for (uint32_t i = 0; i < m && ns < 64; i++) {
        if (s_hist_filter == 0 && recs[i].profile != (uint8_t)s_edit_profile) continue;
        uint32_t end = (i == 0) ? now : recs[i-1].ts;
        int32_t dur = (int32_t)(end - recs[i].ts);
        if (dur < 0) dur = 0;
        uint32_t ins;
        if (i == 0)   // 进行中时段: 速率估算用量
            ins = (uint32_t)((recs[i].rate_x100 / 100.0f) * (dur / 60.0f) * 100.0f);
        else
            ins = recs[i-1].cum_ins_x100 - recs[i].cum_ins_x100;
        segs[ns].ins_x100 = ins;
        segs[ns].dur_min  = (uint32_t)dur;
        segs[ns].type     = recs[i].type;
        segs[ns].profile  = recs[i].profile;
        segs[ns].rate_x100= recs[i].rate_x100;
        segs[ns].ts       = recs[i].ts;
        ns++;
    }
    if (ns == 0) {
        L(20, 60, "本方案暂无执行记录", FONT_SM, DIM);
        L(60, 134, "确认看全部  返回", FONT_SM, DIM);
        return;
    }
    uint32_t mx = 1;
    for (int i = 0; i < ns; i++) if (segs[i].ins_x100 > mx) mx = segs[i].ins_x100;
    int x0 = 8, x1 = 312, y0 = 120, hgt = 84;
    int slotw = (x1 - x0) / ns;
    for (int i = 0; i < ns; i++) {
        int bh = (int)((uint64_t)segs[i].ins_x100 * hgt / mx);
        if (segs[i].ins_x100 == 0) bh = 2;
        int bx = x0 + i * slotw;
        lv_color_t col = ACCENT;
        if (segs[i].type == BH_TBR_START)         col = YELLOW;
        else if (segs[i].type == BH_AAPS_TAKEOVER) col = GREEN;
        else if (segs[i].type == BH_MODE_CHANGE)  col = DIM;
        lv_obj_t *bar = lv_obj_create(s_page);
        lv_obj_set_size(bar, slotw - 1, bh);
        lv_obj_set_pos(bar, bx, y0 - bh);
        lv_obj_set_style_bg_color(bar, col, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        if (ns <= 12 || i % 2 == 0) {
            int yy = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
            rtc_unix_to_ymdhms(segs[i].ts, &yy, &mo, &d, &hh, &mm, &ss);
            char hb[8]; snprintf(hb, sizeof hb, "%02d:%02d", hh, mm);
            L(bx, y0 + 2, hb, FONT_SM, DIM);
        }
    }
    snprintf(tbuf, sizeof tbuf, "%s · %d段 · 柱高=用量",
             (s_hist_filter == 0) ? nm : "全部方案", ns);
    L(8, 20, tbuf, FONT_SM, DIM);
    L(8, 134, "确认切换本方案/全部  返回", FONT_SM, DIM);
}

// P2-12: 错过大剂量提醒 查看/清除
static void draw_missed_bolus(void)
{
    title("错过大剂量");
    if (g_pump_state.missed_bolus) {
        L(16, 34, "⚠ 有待处理提醒", FONT_MAIN, YELLOW);
        L(16, 64, "曾收到大剂量请求但未能", FONT_SM, TEXT);
        L(16, 86, "输注(报警 / 药量不足)", FONT_SM, TEXT);
        L(16, 106, "确认 → 清除提醒", FONT_MAIN, ACCENT);
    } else {
        L(16, 60, "无错过的大剂量", FONT_MAIN, GREEN);
    }
    footer_hint(16, "确认清除  返回", DIM);
}

// P3-14: 回退装药 + 剂量标定
static void draw_rewind_cal(void)
{
    title("回退/标定");
    char buf[40];
    snprintf(buf, sizeof buf, "%s 回退装药(退到尾部)", s_sel == 0 ? "▶" : " ");
    L(16, 22, buf, FONT_MAIN, s_sel == 0 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 手动退药  %.1f U", s_sel == 1 ? "▶" : " ", s_rewind_units);
    L(16, 40, buf, FONT_MAIN, s_sel == 1 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 标定: 推出 1.0U 测试量", s_sel == 2 ? "▶" : " ");
    L(16, 58, buf, FONT_MAIN, s_sel == 2 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 实测体积  %.1f U", s_sel == 3 ? "▶" : " ", s_cal_measured);
    L(16, 76, buf, FONT_MAIN, s_sel == 3 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "%s 计算并保存系数", s_sel == 4 ? "▶" : " ");
    L(16, 94, buf, FONT_MAIN, s_sel == 4 ? ACCENT : TEXT);
    snprintf(buf, sizeof buf, "当前系数 %.3f (指令/实测)", g_pump_config.dose_calibration);
    L(16, 112, buf, FONT_SM, DIM);
    footer_hint(16, s_set_edit ? "▲▼ 调整  确认执行  返回" : "▲▼ 选择  确认执行  返回", DIM);
}

// P0-4: 历史事件回看
static const char *event_name(uint8_t t)
{
    switch ((event_type_t)t) {
        case EVENT_TYPE_BOLUS:       return "大剂量";
        case EVENT_TYPE_BASAL_RATE:  return "基础率";
        case EVENT_TYPE_TBR:         return "临时基础率";
        case EVENT_TYPE_PRIME:       return "排气";
        case EVENT_TYPE_REWIND:      return "回退";
        case EVENT_TYPE_ALARM:       return "报警";
        case EVENT_TYPE_ALARM_CLEAR: return "清报警";
        case EVENT_TYPE_RESERVOIR:   return "储药器";
        case EVENT_TYPE_BATTERY:     return "电池";
        case EVENT_TYPE_POWER_ON:    return "开机";
        case EVENT_TYPE_POWER_OFF:   return "关机";
        case EVENT_TYPE_CALIBRATE:   return "校准";
        case EVENT_TYPE_BASAL_TEST:  return "基础率验证";   // #260
        default:                     return "事件";
    }
}
static const char *alarm_name_by_code(uint8_t code)
{
    for (int i = 0; i < 7; i++)
        if ((uint8_t)ALARMS[i].code == code) return ALARMS[i].name;
    return "报警";
}
// #189: 剂量追溯日志查看器 (主日志 = dose_log)
static void draw_history(void)
{
    title("剂量追溯日志");
    uint32_t n = dose_log_count();
    if (n == 0) {
        L(20, 50, "（暂无记录）", FONT_SM, DIM);
        L(60, 134, "返回", FONT_SM, DIM);
        return;
    }
    dose_log_entry_t evs[200];
    uint32_t k = dose_log_read_recent(200, evs);   // 旧 -> 新
    if (s_sel > (int)k - 1) s_sel = (int)k - 1;
    if (s_sel < 0)          s_sel = 0;
    char buf[48];
    for (int r = 0; r < 6; r++) {
        int idx = s_sel + r;
        if (idx >= (int)k) break;
        dose_log_entry_t *e = &evs[idx];
        int y = 26 + r * 18;
        int yy, mo, d, hh, mm, ss;
        rtc_unix_to_ymdhms(e->timestamp, &yy, &mo, &d, &hh, &mm, &ss);
        const char *tn = event_name(e->type);
        // 突出显示: 报警红 / 大剂量蓝 / 取消琥珀 / 其它常规
        lv_color_t col = TEXT;
        if (e->flags & DOSE_FLAG_ALARM)       col = (e->type == EVENT_TYPE_ALARM) ? RED : DIM;
        else if (e->type == EVENT_TYPE_BOLUS) col = ACCENT;
        else if (e->type == EVENT_TYPE_BASAL_TEST) col = GREEN;   // #260 与治疗剂量视觉区分
        else if (e->flags & DOSE_FLAG_CANCELLED) col = YELLOW;
        bool sel = (idx == s_sel);
        snprintf(buf, sizeof buf, "%02d:%02d %s%s", hh, mm, tn,
                 (e->flags & DOSE_FLAG_SRC_BLE) ? "↪" : "");
        L(8, y, buf, FONT_SM, sel ? ACCENT : col);
        // 右栏关键量
        if (e->type == EVENT_TYPE_ALARM || e->type == EVENT_TYPE_ALARM_CLEAR)
            snprintf(buf, sizeof buf, "%s", alarm_name_by_code((uint8_t)e->param2));
        else if (e->type == EVENT_TYPE_TBR) {
            if (e->amount_x100 <= 500)
                snprintf(buf, sizeof buf, "%.0f%% %dmin", (double)e->amount_x100, e->param2);
            else
                snprintf(buf, sizeof buf, "%.2fU/h", e->amount_x100 / 100.0f);
        } else if (e->type == EVENT_TYPE_BASAL_TEST) {
            // #260: param2 = 实际走的微步数, 用户可据此对照丝杠位移核验
            snprintf(buf, sizeof buf, "%.1fU/%d步", e->amount_x100 / 100.0f, e->param2);
        } else if (e->type == EVENT_TYPE_BOLUS || e->type == EVENT_TYPE_BASAL_RATE
                   || e->type == EVENT_TYPE_PRIME)
            snprintf(buf, sizeof buf, "%.2f U", e->amount_x100 / 100.0f);
        else
            buf[0] = '\0';
        L(200, y, buf, FONT_SM, sel ? ACCENT : DIM);
    }
    snprintf(buf, sizeof buf, "共 %u 条  上下选择  返回", n);
    L(20, 134, buf, FONT_SM, DIM);
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
        case SCR_CLOCK_SET:    draw_clock_set(); break;
        case SCR_ABOUT:        draw_about(); break;
        case SCR_HISTORY:      draw_history(); break;   // #189 dose_log 查看器
        case SCR_TBR:          draw_tbr(); break;        // P2-9
        case SCR_PROFILE:      draw_profile(); break;    // P2-10 / #188
        case SCR_MISSED_BOLUS: draw_missed_bolus(); break; // P2-12
        case SCR_REWIND_CAL:   draw_rewind_cal(); break;    // P3-14
        case SCR_PROFILE_DETAIL: draw_profile_detail(); break; // #188
        case SCR_PROFILE_RENAME: draw_profile_rename(); break; // #188
        case SCR_BASAL_CHART:  draw_basal_chart(); break;   // #189 24h 预览
        case SCR_BASAL_HISTORY: draw_basal_history(); break; // #189 执行历史
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
        case 0:
            s_edit_profile = ui_hal_active_profile();   // 从菜单进: 编辑当前激活方案
            s_basal_parent  = SCR_MENU;
            enter_child(SCR_BASAL);
            break;
        case 1: enter_child(SCR_BOLUS_MENU); break;
        case 2: enter_child(SCR_PRIME); break;
        case 3: enter_child(SCR_ALARM_LIST); break;
        case 4: enter_child(SCR_LOOP); break;
        case 5: enter_child(SCR_HISTORY); break;   // P0-4: 历史回看
        case 6: enter_child(SCR_SETTINGS); break;
        case 7: enter_child(SCR_TBR); break;
        case 8: enter_child(SCR_REWIND_CAL); break;   // P3-14: 回退/标定
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
            list_key(k, 9, SCR_HOME, on_menu_enter);
            break;

        case SCR_BASAL:
            if (s_set_edit) {
                // 编辑选中段的速率: 上下调, 确认保存, 返回取消
                if (k == KEY_UP) {
                    s_edit_rate += BASAL_RATE_STEP;
                    if (s_edit_rate > BASAL_RATE_MAX) s_edit_rate = BASAL_RATE_MAX;
                } else if (k == KEY_DOWN) {
                    s_edit_rate -= BASAL_RATE_STEP;
                    if (s_edit_rate < BASAL_RATE_MIN) s_edit_rate = BASAL_RATE_MIN;
                } else if (k == KEY_SET) {
                    ui_hal_profile_set_basal_rate(s_edit_profile, s_sel, s_edit_rate); // 保存该段
                    s_set_edit = 0;
                } else if (k == KEY_ESC) {
                    s_set_edit = 0;                              // 取消(不改写)
                }
                break;
            }
            // 列表态: 上下选段, 返回父页
            list_key(k, BASAL_SLOTS_PER_DAY, s_basal_parent, nullptr);
            if (k == KEY_SET) {
                // 仅当该方案为当前激活 且 AAPS 接管时, 需先切回本地档案才能编辑
                if (s_edit_profile == (int)g_pump_config.active_profile
                    && !ui_hal_basal_local_mode()) {
                    ui_hal_toggle_basal_mode();
                } else {
                    s_edit_rate = ui_hal_profile_basal_rate(s_edit_profile, s_sel);
                    s_set_edit = 1;                          // 进入编辑
                }
            }
            break;

        case SCR_BOLUS_MENU:
            list_key(k, 5, SCR_MENU, on_bolus_menu_enter);
            break;

        case SCR_BOLUS_NORMAL:
            if (k == KEY_UP)        s_dose += 0.1f;
            else if (k == KEY_DOWN) s_dose = (s_dose > 0.1f) ? s_dose - 0.1f : 0.1f;
            else if (k == KEY_SET)  { ui_hal_deliver_bolus(s_dose, BOLUS_NORMAL, 0, s_dose, 0); back_to(SCR_MENU); }
            else if (k == KEY_ESC)  back_to(SCR_MENU);
            break;

        case SCR_BOLUS_SQUARE:
            // 确认 = 在 剂量/时长 间切换; 仅停在最后一个字段(时长)时, 确认才输注。
            // (旧逻辑: 首按确认既切到时长又立即输注 -> 单键误注胰岛素, 已修复)
            if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP || k == KEY_DOWN) {
                if (s_sel == 0) {
                    if (k == KEY_UP)      s_dose += 0.1f;
                    else if (k == KEY_DOWN) s_dose = (s_dose > 0.1f) ? s_dose - 0.1f : 0.1f;
                } else {
                    if (k == KEY_UP)      s_dur_h = (s_dur_h < 8) ? s_dur_h + 1 : 8;
                    else if (k == KEY_DOWN) s_dur_h = (s_dur_h > 0) ? s_dur_h - 1 : 0;
                }
            } else if (k == KEY_SET) {
                if (s_sel < 1)      s_sel++;                                  // 切到下一字段
                else { ui_hal_deliver_bolus(s_dose, BOLUS_SQUARE, (float)s_dur_h, 0, s_dose); back_to(SCR_MENU); }
            }
            break;

        case SCR_BOLUS_DUAL:
            // 确认 = 在 立即量/方波量/时长 间切换; 仅停在最后字段(时长)时输注。
            if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP || k == KEY_DOWN) {
                if (s_sel == 0) {
                    if (k == KEY_UP)      s_dose_imme += 0.1f;
                    else if (k == KEY_DOWN) s_dose_imme = (s_dose_imme > 0.1f) ? s_dose_imme - 0.1f : 0.1f;
                } else if (s_sel == 1) {
                    if (k == KEY_UP)      s_dose_sq += 0.1f;
                    else if (k == KEY_DOWN) s_dose_sq = (s_dose_sq > 0.1f) ? s_dose_sq - 0.1f : 0.1f;
                } else {
                    if (k == KEY_UP)      s_dur_h = (s_dur_h < 8) ? s_dur_h + 1 : 8;
                    else if (k == KEY_DOWN) s_dur_h = (s_dur_h > 0) ? s_dur_h - 1 : 0;
                }
            } else if (k == KEY_SET) {
                if (s_sel < 2)      s_sel++;                                  // 切到下一字段 (勿在切换时即输注)
                else { ui_hal_deliver_bolus(s_dose_imme + s_dose_sq, BOLUS_DUAL, (float)s_dur_h, s_dose_imme, s_dose_sq); back_to(SCR_MENU); }
            }
            break;

        case SCR_BOLUS_WIZARD:
            // 确认 = 在 血糖/碳水 间切换; 仅停在最后字段(碳水)时输注。
            if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP || k == KEY_DOWN) {
                if (s_sel == 0) {
                    if (k == KEY_UP)      s_wiz_bg += 0.1f;
                    else if (k == KEY_DOWN) s_wiz_bg = (s_wiz_bg > 3.0f) ? s_wiz_bg - 0.1f : 3.0f;
                } else {
                    if (k == KEY_UP)      s_wiz_carb += 1.0f;
                    else if (k == KEY_DOWN) s_wiz_carb = (s_wiz_carb > 0) ? s_wiz_carb - 1.0f : 0;
                }
            } else if (k == KEY_SET) {
                if (s_sel < 1)      s_sel++;                                  // 切到下一字段
                else {
                    float sug = wizard_suggest(s_wiz_bg, s_wiz_carb);
                    if (sug < 0) sug = 0;
                    sug = quantize_units_grid(sug);   // 0.1U 最小剂量网格, 与显示一致
                    ui_hal_deliver_bolus(sug, BOLUS_WIZARD, 0, sug, 0); back_to(SCR_MENU);
                }
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
            if (g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING) {
                // 排气中: 仅可取消
                if (k == KEY_ESC) {
                    pump_state_set_state(PUMP_STATE_IDLE);
                    s_prime_start = 0;
                }
            } else {
                if      (k == KEY_UP)   s_prime_u = (s_prime_u < 10.0f) ? s_prime_u + 0.5f : 10.0f;
                else if (k == KEY_DOWN) s_prime_u = (s_prime_u > 0.5f) ? s_prime_u - 0.5f : 0.5f;
                else if (k == KEY_SET)  { ui_hal_start_prime(s_prime_u); s_prime_start = lv_tick_get(); }
                else if (k == KEY_ESC)  back_to(SCR_MENU);
            }
            break;

        case SCR_ALARM_LIST:
            list_key(k, 7, SCR_MENU, []() { s_alarm_sel = s_sel; enter_child(SCR_ALARM_DETAIL); });
            break;

        case SCR_ALARM_DETAIL:
            if (k == KEY_SET || k == KEY_ESC) back_to(SCR_ALARM_LIST);
            break;

        case SCR_LOOP:
            if (k == KEY_SET) ui_hal_toggle_basal_mode();   // 切换 闭环(AAPS接管)/开环(本地档案)
            else if (k == KEY_ESC) back_to(SCR_MENU);
            break;

        case SCR_SETTINGS:
            if (s_set_edit) {
                // 亮度编辑态: 上下调亮度, 确认/返回退出编辑 (停留在亮度项)
                if (k == KEY_UP) {
                    uint8_t b = ui_hal_get_brightness();
                    if (b <= 95) ui_hal_set_brightness((uint8_t)(b + 5));
                } else if (k == KEY_DOWN) {
                    uint8_t b = ui_hal_get_brightness();
                    if (b >= 5) ui_hal_set_brightness((uint8_t)(b - 5));
                } else if (k == KEY_SET || k == KEY_ESC) {
                    s_set_edit = 0;
                }
                break;
            }
            // 非编辑态: 上下键正常在 8 个设置项间导航
            if (k == KEY_SET && s_sel == 0) {
                int s; ui_hal_get_ymdhms(&s_clk_y, &s_clk_mo, &s_clk_d,
                                         &s_clk_h, &s_clk_mi, &s);
                s_clk_field = 0;
                enter_child(SCR_CLOCK_SET);
            } else if (k == KEY_SET && s_sel == 1) {
                s_set_edit = 1;            // 进入亮度编辑态
            } else if (k == KEY_SET && s_sel == 2) {
                ui_hal_toggle_keypad_sound();
            } else if (k == KEY_SET && s_sel == 3) {
                s_set_edit = 0;                         // 清空可能残留的编辑态
                s_profile_sel = (int)g_pump_config.active_profile;
                enter_child(SCR_PROFILE);               // 基础率方案切换
                s_sel = s_profile_sel;                  // 高亮当前方案
            } else if (k == KEY_SET && s_sel == 4) {
                g_pump_state.keypad_locked = g_pump_state.keypad_locked ? 0 : 1;  // 按键锁切换
            } else if (k == KEY_SET && s_sel == 5) {
                enter_child(SCR_MISSED_BOLUS);  // 错过大剂量提醒
            } else if (k == KEY_SET && s_sel == 6) {
                ui_hal_set_vibrate_enabled(!ui_hal_get_vibrate_enabled());   // P3-15: 振动反馈开关
            } else if (k == KEY_SET && s_sel == 7) {
                enter_child(SCR_ABOUT);
            } else if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP) {
                s_sel = (s_sel + 7) % 8;
            } else if (k == KEY_DOWN) {
                s_sel = (s_sel + 1) % 8;
            }
            break;

        case SCR_CLOCK_SET:
            if (k == KEY_ESC) { back_to(SCR_SETTINGS); break; }
            if (k == KEY_SET) {
                if (s_clk_field < 5) s_clk_field++;        // 下一项
                else {                                     // field 5 = 保存
                    ui_hal_set_time_ymdhms(s_clk_y, s_clk_mo, s_clk_d,
                                          s_clk_h, s_clk_mi, 0);
                    back_to(SCR_SETTINGS);
                }
                break;
            }
            // 上下调整当前字段
            if (s_clk_field == 0)      s_clk_y  = (k==KEY_UP) ? (s_clk_y<2099?s_clk_y+1:2099) : (s_clk_y>2020?s_clk_y-1:2020);
            else if (s_clk_field == 1) s_clk_mo = (k==KEY_UP) ? (s_clk_mo%12)+1 : (s_clk_mo==1?12:s_clk_mo-1);
            else if (s_clk_field == 2) s_clk_d  = (k==KEY_UP) ? (s_clk_d%31)+1 : (s_clk_d==1?31:s_clk_d-1);
            else if (s_clk_field == 3) s_clk_h  = (k==KEY_UP) ? (s_clk_h+1)%24 : (s_clk_h+23)%24;
            else if (s_clk_field == 4) s_clk_mi = (k==KEY_UP) ? (s_clk_mi+1)%60 : (s_clk_mi+59)%60;
            break;

        case SCR_ABOUT:
            if (k == KEY_SET || k == KEY_ESC) back_to(SCR_SETTINGS);
            break;

        case SCR_HISTORY: {   // #189: dose_log 查看器
            uint32_t n = dose_log_count();
            if (n <= 0) { if (k == KEY_ESC) back_to(SCR_MENU); break; }
            if (k == KEY_UP)        s_sel = (s_sel + (int)n - 1) % (int)n;
            else if (k == KEY_DOWN) s_sel = (s_sel + 1) % (int)n;
            else if (k == KEY_SET || k == KEY_ESC) back_to(SCR_MENU);
            break;
        }

        case SCR_TBR: {   // P2-9: 本地临时基础率 (TBR) 设置/取消
            if (s_set_edit) {
                // 编辑值态: 上下调整, 确认/返回保留并退出编辑
                if (k == KEY_UP) {
                    if (s_sel == 0)      s_tbr_pct   = (s_tbr_pct   < 500) ? s_tbr_pct + 10 : 500;
                    else if (s_sel == 1) s_tbr_dur_30 = (s_tbr_dur_30 < 48) ? s_tbr_dur_30 + 1 : 48;
                } else if (k == KEY_DOWN) {
                    if (s_sel == 0)      s_tbr_pct   = (s_tbr_pct   > 0) ? s_tbr_pct - 10 : 0;
                    else if (s_sel == 1) s_tbr_dur_30 = (s_tbr_dur_30 > 0) ? s_tbr_dur_30 - 1 : 0;
                } else if (k == KEY_SET || k == KEY_ESC) {
                    s_set_edit = 0;
                }
                break;
            }
            // 导航态: 上下选字段, 确认执行, 返回菜单
            if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP) {
                s_sel = (s_sel + 3) % 4;
            } else if (k == KEY_DOWN) {
                s_sel = (s_sel + 1) % 4;
            } else if (k == KEY_SET) {
                if (s_sel == 0)      s_set_edit = 1;   // 进入百分比编辑
                else if (s_sel == 1) s_set_edit = 1;   // 进入时长编辑
                else if (s_sel == 2) { ui_hal_set_tbr((float)s_tbr_pct, (uint32_t)s_tbr_dur_30 * 30); back_to(SCR_MENU); }
                else if (s_sel == 3) { ui_hal_cancel_tbr(); back_to(SCR_MENU); }
            }
            break;
        }

        case SCR_PROFILE: {   // #188: 基础率方案列表 -> 进入详情
            int n = MAX_BASAL_PROFILES;
            if (k == KEY_ESC) {
                back_to(SCR_SETTINGS);
            } else if (k == KEY_UP) {
                s_sel = (s_sel + n - 1) % n;
            } else if (k == KEY_DOWN) {
                s_sel = (s_sel + 1) % n;
            } else if (k == KEY_SET) {
                s_edit_profile = s_sel;        // 进入该方案详情
                s_prof_detail_sel = 0;
                s_prof_action = 0;
                s_hist_filter = 0;
                enter_child(SCR_PROFILE_DETAIL);
            }
            break;
        }

        case SCR_PROFILE_DETAIL: {   // #188: 方案详情子菜单
            if (s_prof_action == 1) {        // 复制: 选择目标
                if (k == KEY_UP || k == KEY_DOWN) {
                    int step = (k == KEY_UP) ? -1 : 1;
                    int nxt = s_copy_dst;
                    do { nxt = (nxt + step + MAX_BASAL_PROFILES) % MAX_BASAL_PROFILES; }
                    while (nxt == s_edit_profile);   // 跳过自身 (4 方案必有其它目标)
                    s_copy_dst = nxt;
                } else if (k == KEY_SET) {
                    ui_hal_profile_copy(s_copy_dst, s_edit_profile);
                    s_prof_action = 0;
                } else if (k == KEY_ESC) {
                    s_prof_action = 0;
                }
                break;
            }
            if (s_prof_action == 2) {        // 重置确认
                if (k == KEY_SET) {
                    ui_hal_profile_reset(s_edit_profile);
                    s_prof_action = 0;
                } else if (k == KEY_ESC) {
                    s_prof_action = 0;
                }
                break;
            }
            if (s_prof_action == 3) {        // #260 验证测试确认 (真会打药)
                if (k == KEY_SET) {
                    s_prof_test_units = ui_hal_basal_run_test();
                    // 返回 0 不能静默退回菜单 —— 那是"档案没落库"的关键信号, 必须显式报因
                    s_prof_action = (s_prof_test_units > 0.0f) ? 4 : 5;
                } else if (k == KEY_ESC) {
                    s_prof_action = 0;
                }
                break;
            }
            if (s_prof_action == 4 || s_prof_action == 5) {  // 结果提示: 任意键回菜单
                s_prof_action = 0;
                break;
            }
            if (k == KEY_ESC) {
                back_to(SCR_PROFILE);
            } else if (k == KEY_UP) {
                s_prof_detail_sel = (s_prof_detail_sel + PROF_DETAIL_N - 1) % PROF_DETAIL_N;
            } else if (k == KEY_DOWN) {
                s_prof_detail_sel = (s_prof_detail_sel + 1) % PROF_DETAIL_N;
            } else if (k == KEY_SET) {
                switch (s_prof_detail_sel) {
                    case 0:  // 设为当前方案
                        ui_hal_set_active_profile((uint8_t)s_edit_profile);
                        back_to(SCR_PROFILE);
                        break;
                    case 1:  // 重命名
                        { char nm[32]; ui_hal_profile_name(s_edit_profile, nm, sizeof nm);
                          name_decode(nm); s_name_append = 0; s_name_cur = s_name_len;
                          s_screen = SCR_PROFILE_RENAME; }
                        break;
                    case 2:  // 编辑24段基础率
                        s_basal_parent = SCR_PROFILE_DETAIL;
                        s_screen = SCR_BASAL;
                        s_sel = 0; s_set_edit = 0;
                        break;
                    case 3:  // 24h 预览图
                        s_screen = SCR_BASAL_CHART;
                        break;
                    case 4:  // 执行记录
                        s_hist_filter = 0;
                        s_screen = SCR_BASAL_HISTORY;
                        break;
                    case 5:  // 复制本方案
                        s_copy_dst = (s_edit_profile + 1) % MAX_BASAL_PROFILES;
                        s_prof_action = 1;
                        break;
                    case 6:  // 重置为默认
                        s_prof_action = 2;
                        break;
                    case 7:  // #260 验证测试: 全天 24 段总量一次性打出
                        // 仅对"当前激活方案"有意义 —— 调度器跑的就是它;
                        // 若光标停在非激活方案上, 先提示切换, 避免测出来的量对不上实际输注。
                        if (s_edit_profile != (int)g_pump_config.active_profile) {
                            ui_hal_set_active_profile((uint8_t)s_edit_profile);
                        }
                        s_prof_action = 3;      // 进入二次确认
                        break;
                }
            }
            break;
        }

        case SCR_PROFILE_RENAME: {   // #188: 重命名字符编辑器
            if (k == KEY_UP || k == KEY_DOWN) {
                if (s_name_cur < s_name_len) {
                    // 改当前字
                    s_name_chars[s_name_cur] = (s_name_chars[s_name_cur]
                        + (k==KEY_UP?NAME_CHAR_N-1:1)) % NAME_CHAR_N;
                } else if (s_name_cur == s_name_len) {
                    // 追加槽: 调待插入字
                    s_name_append = (s_name_append + (k==KEY_UP?NAME_CHAR_N-1:1)) % NAME_CHAR_N;
                }
                // 保存槽: 上下无效
            } else if (k == KEY_SET) {
                if (s_name_cur < s_name_len) {
                    s_name_cur++;                       // 移到下一字
                } else if (s_name_cur == s_name_len) {  // 追加槽
                    if (s_name_len < 31) {
                        s_name_chars[s_name_len] = s_name_append;
                        s_name_len++;
                        s_name_cur = s_name_len;        // 停在新字(可继续调/追加)
                        s_name_append = 0;
                    } else {
                        // 已满: 直接保存退出
                        char nm[40]; name_encode(nm, sizeof nm);
                        ui_hal_profile_set_name(s_edit_profile, nm);
                        back_to(SCR_PROFILE_DETAIL);
                    }
                } else {                                // 保存槽
                    char nm[40]; name_encode(nm, sizeof nm);
                    ui_hal_profile_set_name(s_edit_profile, nm);
                    back_to(SCR_PROFILE_DETAIL);
                }
            } else if (k == KEY_ESC) {
                if (s_name_cur < s_name_len) {          // 退格删除当前字
                    for (int i = s_name_cur; i < s_name_len - 1; i++)
                        s_name_chars[i] = s_name_chars[i+1];
                    s_name_len--;
                    if (s_name_cur > 0) s_name_cur--;
                } else if (s_name_cur == s_name_len && s_name_len > 0) {
                    s_name_cur = s_name_len - 1;        // 退回上一字
                } else {
                    back_to(SCR_PROFILE_DETAIL);        // 空名: 取消
                }
            }
            break;
        }

        case SCR_BASAL_CHART: {   // #189: 24h 预览 (▲▼ 切换方案)
            int n = MAX_BASAL_PROFILES;
            if (k == KEY_ESC) {
                back_to(SCR_PROFILE_DETAIL);
            } else if (k == KEY_UP) {
                s_edit_profile = (s_edit_profile + n - 1) % n;
            } else if (k == KEY_DOWN) {
                s_edit_profile = (s_edit_profile + 1) % n;
            }
            break;
        }

        case SCR_BASAL_HISTORY: {   // #189: 执行历史时间轴
            if (k == KEY_ESC) {
                back_to(SCR_PROFILE_DETAIL);
            } else if (k == KEY_SET) {
                s_hist_filter = s_hist_filter ? 0 : 1;  // 本方案 <-> 全部
            }
            // 上下键保留(备用滚动); 当前段数有限无需滚动
            break;
        }

        case SCR_MISSED_BOLUS:   // P2-12: 错过大剂量 查看/清除
            if (k == KEY_ESC) {
                back_to(SCR_SETTINGS);
            } else if (k == KEY_SET) {
                g_pump_state.missed_bolus = 0;   // 清除提醒 (停留本屏, 徽标同步消失)
            }
            break;

        case SCR_REWIND_CAL:   // P3-14: 回退装药 + 剂量标定
            if (s_set_edit) {
                // 编辑态: 上下调整当前编辑项(手动退药量 或 实测体积), 确认执行/退出编辑
                if (s_sel == 1) {        // 手动退药量: 确认即退该量并退出编辑
                    if (k == KEY_UP)        s_rewind_units = (s_rewind_units < 300.0f) ? s_rewind_units + 0.1f : 300.0f;
                    else if (k == KEY_DOWN) s_rewind_units = (s_rewind_units > 0.1f) ? s_rewind_units - 0.1f : 0.1f;
                    else if (k == KEY_SET) { ui_hal_rewind_units(s_rewind_units); s_set_edit = 0; }   // 执行并退出
                    else if (k == KEY_ESC) s_set_edit = 0;                                        // 取消(不退)
                } else {                 // 实测体积: 仅调整并保留, 退出编辑后才计算
                    if (k == KEY_UP)        s_cal_measured = (s_cal_measured < 2.0f) ? s_cal_measured + 0.1f : 2.0f;
                    else if (k == KEY_DOWN) s_cal_measured = (s_cal_measured > 0.1f) ? s_cal_measured - 0.1f : 0.1f;
                    else if (k == KEY_SET || k == KEY_ESC) s_set_edit = 0;
                }
                break;
            }
            if (k == KEY_ESC) {
                back_to(SCR_MENU);
            } else if (k == KEY_UP) {
                s_sel = (s_sel + 4) % 5;
            } else if (k == KEY_DOWN) {
                s_sel = (s_sel + 1) % 5;
            } else if (k == KEY_SET) {
                if (s_sel == 0)      ui_hal_rewind();                                  // 回退装药(退到尾部)
                else if (s_sel == 1) s_set_edit = 1;                                   // 进入手动退药量编辑
                else if (s_sel == 2) ui_hal_calibrate_dispense(1.0f);                  // 标定出 1.0U 测试量
                else if (s_sel == 3) s_set_edit = 1;                                   // 进入实测体积编辑
                else if (s_sel == 4) {                                                 // 计算并保存系数
                    float factor = (s_cal_measured > 0.001f) ? (1.0f / s_cal_measured) : 1.0f;
                    ui_hal_apply_calibration(factor);
                }
            }
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
    ui_screen_periodic();          // 自动重复 + 计时状态推进
    lv_obj_clean(s_page);
    draw_current();
}

// 每帧调用: 处理"按住自动重复"与计时类状态(如排气自动结束)
void ui_screen_periodic(void)
{
    uint32_t now = lv_tick_get();

    // ---- 按住自动重复 (仅 UP/DOWN) ----
    if (s_rep_key == KEY_UP || s_rep_key == KEY_DOWN) {
        if (now >= s_rep_at) {
            handle_key(s_rep_key);             // 重复施加同方向调整
            s_rep_count++;
            uint32_t interval = (s_rep_count < 6) ? 90 : 40;  // 前 6 次慢(90ms), 之后加速(40ms)
            s_rep_at = now + interval;
        }
    }

    // ---- 排气中: 约 1.2s 后自动回到待机 ----
    if (g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING && s_prime_start) {
            if (now - s_prime_start > 1200) {
            pump_state_set_state(PUMP_STATE_IDLE);
            s_prime_start = 0;
        }
    }

    // ---- P3-13: 过温检测 (读板温 + 预警/报警状态机) ----
    thermal_periodic();
}

void ui_screen_key(key_event_t k)
{
    // P2-11: 按键锁生效时, 拦截所有导航键, 仅允许"确认"键解锁 (防误触输注/改设置)
    if (g_pump_state.keypad_locked) {
        if (k == KEY_SET) g_pump_state.keypad_locked = 0;  // 按确认解锁
        return;                                            // 上/下/返回一律忽略
    }
    // 任何非 UP/DOWN 的按键都会终止自动重复 (避免误触持续)
    if (k == KEY_UP || k == KEY_DOWN) {
        s_rep_key   = k;
        s_rep_at    = lv_tick_get() + 300;     // 初始延迟 300ms 后才开始重复
        s_rep_count = 0;
    } else {
        s_rep_key = KEY_NONE;
    }
    ui_hal_vibrate(VIB_KEY);                   // P3-15: 按键反馈 (开关关闭时 HAL 内部直接返回)
    handle_key(k);                              // 立即施加一次
}

void ui_screen_release(void)
{
    s_rep_key = KEY_NONE;                       // 松开按键: 停止自动重复
}

void ui_set_screen(int s) { s_screen = s; s_sel = 0; }
int  ui_get_screen(void)  { return s_screen; }

// 导出当前界面导航/编辑态 -> JSON 片段 (无外层花括号), 供联调控制面板
// (link_demo_gui.py) 实时渲染菜单/设置/时钟等界面, 实现手动操控可见。
void ui_screen_dump_json(char *out, size_t cap)
{
    int hh, mm; ui_hal_get_clock(&hh, &mm);
    float sel_rate = 0.0f;
    if (s_screen == SCR_BASAL)
        sel_rate = s_set_edit ? s_edit_rate : ui_hal_profile_basal_rate(s_edit_profile, s_sel);
    snprintf(out, cap,
        "{\"screen\":%d,\"sel\":%d,\"set_edit\":%d,\"sel_rate\":%.2f,\"local_mode\":%d,"
        "\"clk_field\":%d,\"clk\":[%d,%d,%d,%d,%d],"
        "\"dose\":%.2f,\"dur_h\":%d,\"imme\":%.2f,\"sq\":%.2f,"
        "\"wiz_bg\":%.1f,\"wiz_carb\":%.0f,\"alarm_sel\":%d,"
        "\"brightness\":%d,\"clock_valid\":%d,\"keypad\":%d,"
        "\"prime_u\":%.1f,\"prime_active\":%d,"
        "\"edit_profile\":%d,\"prof_detail\":%d,\"hist_filter\":%d}",
        s_screen, s_sel, s_set_edit, sel_rate,
        ui_hal_basal_local_mode() ? 1 : 0, s_clk_field,
        s_clk_y, s_clk_mo, s_clk_d, s_clk_h, s_clk_mi,
        s_dose, s_dur_h, s_dose_imme, s_dose_sq,
        s_wiz_bg, s_wiz_carb, s_alarm_sel,
        (int)ui_hal_get_brightness(), ui_hal_clock_valid() ? 1 : 0,
        ui_hal_get_keypad_sound() ? 1 : 0,
        s_prime_u,
        g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING ? 1 : 0,
        s_edit_profile, s_prof_detail_sel, s_hist_filter);
}

// 二进制导航快照（供伴生 App 复刻可交互虚拟屏）：13 字节，单通知 ≤ MTU 载荷上限，无需分片。
// [0xB1][screen][sel][edit][flags][v0:2][v1:2][v2:2][v3:2]
// v0..v3 按当前屏幕填入最相关的数值（单位见各屏注释），App 端按 screen 解读并渲染对应菜单。
void ui_screen_dump_nav_binary(uint8_t *b, size_t cap)
{
    if (cap < 13) return;
    uint16_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    switch (s_screen) {
        case SCR_BOLUS_NORMAL: v0 = (uint16_t)(s_dose * 100); break;
        case SCR_BOLUS_SQUARE: v0 = (uint16_t)(s_dose * 100); v1 = (uint16_t)s_dur_h; break;
        case SCR_BOLUS_DUAL:   v0 = (uint16_t)(s_dose_imme * 100);
                                v1 = (uint16_t)(s_dose_sq * 100);
                                v2 = (uint16_t)s_dur_h; break;
        case SCR_BOLUS_WIZARD: v0 = (uint16_t)(s_wiz_bg * 10); v1 = (uint16_t)s_wiz_carb; break;
        case SCR_BOLUS_MEALS:  v0 = (uint16_t)s_meal_sel; break;   // 仅传选中项, 预设值本地
        case SCR_BASAL:        v0 = (uint16_t)((s_set_edit ? s_edit_rate
                                            : ui_hal_profile_basal_rate(s_edit_profile, s_sel)) * 100); break;
        case SCR_TBR:          v0 = (uint16_t)(s_tbr_pct * 10); v1 = (uint16_t)s_tbr_dur_30; break;
        case SCR_PRIME:        v0 = (uint16_t)(s_prime_u * 10); break;
        case SCR_CLOCK_SET: {
            int f = s_clk_field;
            int val = (f == 0) ? s_clk_y : (f == 1) ? s_clk_mo : (f == 2) ? s_clk_d
                              : (f == 3) ? s_clk_h : (f == 4) ? s_clk_mi : 0;
            v0 = (uint16_t)val; break;
        }
        default: break;
    }
    b[0] = 0xB1;
    b[1] = (uint8_t)s_screen;
    b[2] = (uint8_t)s_sel;
    b[3] = (uint8_t)(s_set_edit ? 1 : 0);
    b[4] = (uint8_t)((g_pump_state.current_state == (uint8_t)PUMP_STATE_PRIMING ? 1 : 0)
                    | (ui_hal_clock_valid() ? 2 : 0)
                    | (ui_hal_get_keypad_sound() ? 4 : 0));
    b[5] = (uint8_t)(v0 & 0xFF); b[6] = (uint8_t)(v0 >> 8);
    b[7] = (uint8_t)(v1 & 0xFF); b[8] = (uint8_t)(v1 >> 8);
    b[9] = (uint8_t)(v2 & 0xFF); b[10] = (uint8_t)(v2 >> 8);
    b[11] = (uint8_t)(v3 & 0xFF); b[12] = (uint8_t)(v3 >> 8);
}
