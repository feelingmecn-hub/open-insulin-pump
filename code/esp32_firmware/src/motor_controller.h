/**
 * motor_controller.h — DRV8825 步进电机控制 (Arduino + 硬件定时器 ISR)
 *
 * 脉冲由 ESP32 硬件定时器 ISR 翻转 STEP 引脚生成 (替代 ESP-IDF RMT)。
 * 运动命令通过 FreeRTOS 队列串行执行, 运动期间用 INA226 电流做丢步/堵转监护。
 */
#pragma once

#include "pump_types.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern QueueHandle_t g_motor_cmd_queue;
extern SemaphoreHandle_t g_motor_mutex;

void motor_init(void);
void motor_task(void *arg);

// 手动微动 (按键板调用, 内部加互斥保护)
bool motor_jog(motor_dir_t dir, uint16_t steps);
// 设置当前位置为原点 (按键板长按 SET)
void motor_set_home(void);
// 通用入队 (BLE / 基础率任务调用)
bool motor_enqueue(const motor_command_t *cmd);
// 取消正在进行的大剂量 (置 abort 标志, 当前段完成后停止)
void motor_cancel_bolus(void);
// 大剂量是否正在进行
bool motor_bolus_active(void);

// 丢步/堵转监护
void motor_start_stall_guard(void);
void motor_stall_guard_tick(void);
void motor_stop_stall_guard(void);
bool motor_step_loss_detected(void);
