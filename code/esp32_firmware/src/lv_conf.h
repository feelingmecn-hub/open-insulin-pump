/*
 * lv_conf.h — LVGL 9.5.0 配置 (OpenLoop Insulin Pump, Arduino + ESP32-C6)
 *
 * 放置于 sketch 根目录, LVGL 库会自动包含。
 * 显示帧缓冲在 lcd_display.cpp 中用 DMA 内存单独分配。
 */
#if !defined(LV_CONF_INCLUDE_SIMPLE)
#define LV_CONF_INCLUDE_SIMPLE
#endif

/* LVGL 确认哨兵: 让 lv_conf_internal.h 识别本配置已加载 (消除 "failure to include" 提示) */
#ifndef LV_CONF_H
#define LV_CONF_H
#endif

/* 操作系统: LVGL 仅在 display_task 单线程访问, 无需内置 OS 锁 */
#define LV_USE_OS           LV_OS_NONE

/* 颜色深度: ST7789 为 16-bit RGB565 */
#define LV_COLOR_DEPTH      16
/* 每显示实例单独用 RGB565_SWAPPED, 此处不全局交换 */
#define LV_COLOR_16_SWAP    0

/* 内存: C6 有 512KB HP SRAM, 给 LVGL 内部堆 80KB */
#define LV_MEM_SIZE         (80 * 1024)
#define LV_MEM_CUSTOM       0
#define LV_MEM_ADDR         0

/* 字体 (UI 需要中文/英文, 启用 Montserrat 西文 + 默认) */
#define LV_USE_FONT_MONTSERRAT_14   1
#define LV_USE_FONT_MONTSERRAT_16   1
#define LV_USE_FONT_MONTSERRAT_20   1
#define LV_USE_FONT_MONTSERRAT_22   1
#define LV_USE_FONT_MONTSERRAT_24   1
#define LV_FONT_DEFAULT             &lv_font_montserrat_16

/* 计时: 手动 lv_tick_inc() */
#define LV_TICK_CUSTOM       0

/* 日志 (调试时可开启) */
#define LV_USE_LOG           0
#define LV_LOG_LEVEL         LV_LOG_LEVEL_WARN

/* 关闭 demo/example 以节省 flash */
#define LV_USE_DEMO_WIDGETS  0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_EXAMPLES      0

/* 渲染 */
#define LV_DRAW_SW_LAYER_SIMPLE 0
#define LV_USE_DRAW_MASKS    1
#define LV_USE_USER_DATA     1

/* 默认显示缓冲渲染模式 (代码里用 PARTIAL) */
#define LV_RENDER_MODE       LV_RENDER_MODE_PARTIAL

/* 禁用不需要的部件以省空间 (保留常用) */
#define LV_USE_OBJ          1
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BTN          1
#define LV_USE_LABEL        1
#define LV_USE_IMG          1
#define LV_USE_LINE         1
#define LV_USE_LIST         1
#define LV_USE_ROLLER       1
#define LV_USE_SLIDER       1
#define LV_USE_SWITCH       1
