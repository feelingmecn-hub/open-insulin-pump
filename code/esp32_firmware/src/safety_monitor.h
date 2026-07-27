/**
 * safety_monitor.h — 安全监控 (Arduino + FreeRTOS)
 */
#pragma once

#include "pump_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void safety_init(void);
void safety_task(void *arg);
void safety_trigger_alarm(alarm_code_t code);
