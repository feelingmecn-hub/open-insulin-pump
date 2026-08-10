/**
 * ui_screen.h — 全中文 UI 页面状态机 (LVGL)
 *
 * 横屏 320×172, 4 物理按键(上/下/确认/返回)导航。
 * 页面树(参考 docs/UI设计.md):
 *   主状态屏 → 主菜单 → 基础率 / 大剂量 / 排气装药 / 报警 / 闭环 / 系统设置
 *
 * ⚠️ 单一真源(硬规则): 本文件与 code/esp32_firmware/src/ui_screen.cpp 同时为
 * 固件与联调模拟器的唯一来源。模拟器 CMake 直接编译固件侧的 ui_screen.cpp/.h,
 * 禁止在 simulator/lvgl_sdl/src/ 另存副本, 否则联调行为与真实固件会漂移,
 * 且固件可能因缺声明而无法编译。
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
void ui_screen_release(void);   // 按键释放: 停止"按住自动重复"
void ui_screen_periodic(void);  // 每帧调用: 自动重复 + 计时类状态推进 (由 refresh/headless 循环驱动)

// 导出当前界面/选中/编辑态为 JSON 片段 (供联调控制面板手动操控 + 显示)
void ui_screen_dump_json(char *out, size_t cap);
// 二进制导航快照（13 字节，供伴生 App 复刻可交互虚拟屏）
void ui_screen_dump_nav_binary(uint8_t *out, size_t cap);

// 截图模式用: 直接跳转到指定页面 (与 ui_screen.cpp 内部 SCR_* 对应)
void ui_set_screen(int s);
int  ui_get_screen(void);

// 页面 ID (供 main 截图切换; 数值必须与 ui_screen.cpp 的 SCR_* 枚举一一对应)
enum ui_screen_id {
    UI_HOME = 0, UI_MENU, UI_BASAL, UI_BOLUS_MENU, UI_BOLUS_NORMAL, UI_BOLUS_SQUARE,
    UI_BOLUS_DUAL, UI_BOLUS_WIZARD, UI_BOLUS_MEALS, UI_PRIME,
    UI_ALARM_LIST, UI_ALARM_DETAIL, UI_LOOP, UI_SETTINGS,
    UI_CLOCK_SET, UI_ABOUT, UI_HISTORY,
    UI_TBR, UI_PROFILE, UI_MISSED_BOLUS, UI_REWIND_CAL,
    UI_PROFILE_DETAIL, UI_PROFILE_RENAME, UI_BASAL_CHART, UI_BASAL_HISTORY
};
