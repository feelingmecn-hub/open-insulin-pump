/**
 * ui_screen.h — 全中文 UI 页面状态机 (LVGL, PC/SDL 版)
 *
 * 横屏 320×172, 4 物理按键(上/下/确认/返回)导航。
 * 页面树(参考 docs/UI设计.md):
 *   主状态屏 → 主菜单 → 基础率 / 大剂量 / 排气装药 / 报警 / 闭环 / 系统设置
 * 复用了固件的 g_pump_state, 演示数据来自 mock_hal。
 */
#pragma once
#include "lvgl.h"
#include "pump_types.h"

// 中文字体 (由 lv_font_conv 生成, 见 lv_font_cn_16.c / lv_font_cn_12.c)
LV_FONT_DECLARE(lv_font_cn_16);
LV_FONT_DECLARE(lv_font_cn_12);

void ui_screen_init(void);
void ui_screen_refresh(void);   // 每帧调用: 重建当前页
void ui_screen_key(key_event_t k);

// 截图模式用: 直接跳转到指定页面 (与 ui_screen.cpp 内部 SCR_* 对应)
void ui_set_screen(int s);
int  ui_get_screen(void);

// 页面 ID (供 main 截图切换)
enum ui_screen_id {
    UI_HOME = 0, UI_MENU, UI_BASAL, UI_BOLUS_MENU, UI_BOLUS_NORMAL, UI_BOLUS_SQUARE,
    UI_BOLUS_DUAL, UI_BOLUS_WIZARD, UI_BOLUS_MEALS, UI_PRIME,
    UI_ALARM_LIST, UI_ALARM_DETAIL, UI_LOOP, UI_SETTINGS
};
