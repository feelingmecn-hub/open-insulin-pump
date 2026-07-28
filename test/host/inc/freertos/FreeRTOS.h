/* Host (PC) stub for FreeRTOS — 仅提供 aaps_dana.cpp / motor_controller.h 编译所需的类型。
 * 不实现任何 RTOS 行为; 联调测试在单线程内顺序驱动, 无需真实任务/队列。 */
#pragma once
#include <cstddef>
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
#define portMAX_DELAY ((uint32_t)0xFFFFFFFF)
