/**
 * ble_comm.h — BLE GATT 服务 (NimBLE-Arduino)
 *
 * 复用原协议 UUID (6E400001... 变体), 与 Android APP 对齐。
 * 写入 bolus/basal/tbr 特征值 → 电机命令入队; status/iob/reservoir 定时 notify。
 */
#pragma once

#include "pump_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void ble_init(void);
void ble_task(void *arg);
