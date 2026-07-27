/**
 * keypad.h — 4 键按键板 (上/下/确认/取消)
 *
 * 按键板自扫描: keypad_task 检测短按/长按并直接调用电机/电源函数。
 * 同时把事件入队 (g_key_queue) 供其他逻辑可选读取。
 */
#pragma once

#include "pump_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern QueueHandle_t g_key_queue;

void keypad_init(void);
void keypad_task(void *arg);
key_event_t keypad_get_event(void);   // 备用: 从队列取事件 (非阻塞)
