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
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// 初始化 LCD + LVGL, 创建状态屏 UI
void lcd_display_init(void);

// 背光亮度 0-100 (%); 实际建议 ≤50
void lcd_display_backlight(uint8_t percent);

// 在 FreeRTOS display_task 中周期调用: 刷新 LVGL + 更新状态屏
void lcd_display_task(void);
