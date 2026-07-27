/**
 * power_manager.h — 电源与状态 LED (Arduino 框架)
 *
 * 3S→DC-DC 5V 供电树; 电机 11.1V 直供。
 * 状态 LED 用板载 WS2812 (GPIO8), 必须 rgbLedWriteOrdered 驱动。
 */
#pragma once

#include "config.h"
#include "pump_types.h"

void power_init(void);
void power_motor_on(void);    // 占位 (电机 11.1V 直供)
void power_motor_off(void);   // 占位
void system_power_off(void);  // 紧急关机 (长按 ESC) → 深度睡眠

// WS2812 状态灯 (r/g/b: 0-255)
void status_led_set(uint8_t r, uint8_t g, uint8_t b);
