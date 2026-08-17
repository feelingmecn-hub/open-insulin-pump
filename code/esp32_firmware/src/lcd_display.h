/**
 * lcd_display.h — 1.47" ST7789 LCD 驱动 (Arduino_GFX + LVGL 9.5.0)
 *
 * 框架: Arduino + GFX_Library_for_Arduino + LVGL 9.5.0
 * 引脚: 见 config.h (SCK=7/MOSI=6/CS=14/DC=15/RST=21/BL=22)
 *
 * ⚠️ 172 宽 ST7789 必须 LCD_X_GAP=34 列偏移, 否则画面横向错位
 * ⚠️ 颜色格式 RGB565_SWAPPED (屏要求字节交换)
 * ⚠️ 背光亮度控制在 50% 以内 (Waveshare 高温警告), 默认 40%
 */
#pragma once

#include <Arduino.h>
#include "config.h"
// ⚠️ GFX 库的 ESP32 数据总线类 (Arduino_ESP32SPI 等) 守卫为
//   #if defined(ESP32) && (CONFIG_IDF_TARGET_ESP32 || ... || CONFIG_IDF_TARGET_ESP32C6 ...)
// 但 Arduino-ESP32 3.x 在 C6 上只定义 ESP32C6 / CONFIG_IDF_TARGET_ESP32C6, 不定义
// legacy 的 ESP32 宏, 导致整类(含其 .cpp 实现)被跳过而链接失败。
// 解决: 整个构建必须带 -D ESP32 (见 flash 工具与下方编译命令的 build.extra_flags),
// 核心与 NimBLE/LVGL 均以 CONFIG_IDF_TARGET_* 区分芯片, 不受此宏影响。
#include <Arduino_GFX_Library.h>
#include <databus/Arduino_ESP32SPI.h>
#include <lvgl.h>

// 初始化 LCD + LVGL, 创建状态屏 UI
void lcd_display_init(void);

// 背光亮度 0-100 (%); 实际建议 ≤50
void lcd_display_backlight(uint8_t percent);

// 在 FreeRTOS display_task 中周期调用: 刷新 LVGL + 更新状态屏
void lcd_display_task(void);

// 开机画面 (独立 splash, 不进 ui_screen 状态机, 不影响联调模拟器单一真源):
// 显示项目名/版本/自检项, 停留 hold_ms 后销毁并交回 ui_screen_init() 建主界面。
void lcd_display_boot_screen(int hold_ms);
