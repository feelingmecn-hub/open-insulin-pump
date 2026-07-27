/**
 * power_manager.cpp — 电源与状态 LED (Arduino)
 */
#include "power_manager.h"
#include "motor_controller.h"
#include "esp32-hal-rgb-led.h"
#include "esp_sleep.h"

void power_init(void)
{
    // 5V DC-DC 使能 (若模块 EN 接了 GPIO17; 若硬连线常开则悬空无影响)
    pinMode(PIN_EN_5V_BUCK, OUTPUT);
    digitalWrite(PIN_EN_5V_BUCK, HIGH);
    status_led_set(0, 0, 0);   // 初始灭
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    // 板载 WS2812 单线 RGB 灯珠 (非普通 GPIO)
    rgbLedWriteOrdered(PIN_LED_STATUS, LED_COLOR_ORDER_RGB, r, g, b);
}

void power_motor_on(void)  { /* 电机 11.1V 直供, 无软件开关 */ }
void power_motor_off(void) { /* 占位 */ }

void system_power_off(void)
{
    // 停止电机
    motor_command_t stop_cmd;
    memset(&stop_cmd, 0, sizeof(stop_cmd));
    stop_cmd.type = MOTOR_CMD_STOP;
    motor_enqueue(&stop_cmd);

    // 状态灯红色
    status_led_set(40, 0, 0);

    // 进入深度睡眠, PIN_KEY_ESC 低电平唤醒
    esp_sleep_enable_gpio_wakeup((1ULL << PIN_KEY_ESC), ESP_GPIO_WAKEUP_GPIO_LOW);
    delay(100);
    esp_deep_sleep_start();
}
