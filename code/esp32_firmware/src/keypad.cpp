/**
 * keypad.cpp — 4 键按键板 (Arduino, 轮询扫描 + 消抖)
 *
 * 短按键通过 ui_screen_key() 驱动全中文 UI 的导航/编辑/确认/返回;
 * 长按 SET = 保存电机原点, 长按 ESC = 关机 (安全相关, 不进菜单)。
 */
#include "keypad.h"
#include "config.h"
#include "ui_screen.h"        // ui_screen_key()

#include "motor_controller.h" // motor_set_home()
#include "power_manager.h"    // system_power_off()

static const uint8_t KEY_PINS[4] = { PIN_KEY_UP, PIN_KEY_DOWN, PIN_KEY_SET, PIN_KEY_ESC };

static uint8_t  g_key_state[4] = { 1, 1, 1, 1 };   // 1=释放(上拉)
static uint32_t g_press_start[4] = { 0, 0, 0, 0 };
static bool     g_long_fired[4]  = { false, false, false, false };

QueueHandle_t g_key_queue = nullptr;

void keypad_init(void)
{
    for (int i = 0; i < 4; i++) {
        pinMode(KEY_PINS[i], INPUT_PULLUP);
    }
    g_key_queue = xQueueCreate(8, sizeof(key_event_t));
}

static void push_event(key_event_t e)
{
    if (g_key_queue) xQueueSend(g_key_queue, &e, 0);
}

// 短按: 喂给 UI 导航状态机 (上/下/确认/返回)
static void handle_short(uint8_t idx)
{
    switch (idx) {
        case 0: ui_screen_key(KEY_UP);   break;  // UP
        case 1: ui_screen_key(KEY_DOWN); break;  // DOWN
        case 2: ui_screen_key(KEY_SET);  break;  // SET (进入/确认)
        case 3: ui_screen_key(KEY_ESC);  break;  // ESC (返回)
        default: break;
    }
}

// 长按: 安全/系统操作, 不进菜单
static void handle_long(uint8_t idx)
{
    switch (idx) {
        case 2: motor_set_home(); break;          // SET 长按 = 存原点
        case 3: system_power_off(); break;        // ESC 长按 = 关机
        default: break;
    }
}

void keypad_task(void *arg)
{
    for (;;) {
        uint32_t now = millis();
        for (int i = 0; i < 4; i++) {
            uint8_t v = digitalRead(KEY_PINS[i]);
            if (v == 0 && g_key_state[i] == 1) {
                // 按下 (下降沿)
                g_key_state[i] = 0;
                g_press_start[i] = now;
                g_long_fired[i] = false;
            } else if (v == 1 && g_key_state[i] == 0) {
                // 释放 (上升沿) → 短按 (未触发过长按)
                g_key_state[i] = 1;
                if (!g_long_fired[i]) {
                    key_event_t e = (key_event_t)(KEY_UP + i);
                    push_event(e);
                    handle_short(i);
                }
            } else if (v == 0 && !g_long_fired[i] && (now - g_press_start[i] > 1000)) {
                // 长按 (1s)
                g_long_fired[i] = true;
                key_event_t e = (i == 2) ? KEY_LONG_SET : (i == 3) ? KEY_LONG_ESC : (key_event_t)(KEY_UP + i);
                push_event(e);
                handle_long(i);
            }
        }
        delay(20);
    }
}

key_event_t keypad_get_event(void)
{
    key_event_t e = KEY_NONE;
    if (g_key_queue) xQueueReceive(g_key_queue, &e, 0);
    return e;
}
