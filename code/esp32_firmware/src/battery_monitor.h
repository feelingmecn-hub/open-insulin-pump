/**
 * battery_monitor.h — 电池监测 (INA226, Arduino)
 */
#pragma once

#include "pump_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void battery_init(void);
void battery_task(void *arg);
