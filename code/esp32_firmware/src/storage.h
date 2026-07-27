/**
 * storage.h — 配置持久化 (Arduino Preferences, 替代 ESP-IDF NVS)
 */
#pragma once

#include "pump_types.h"

void storage_init(void);
void storage_load_config(pump_config_t *cfg);
void storage_save_config(const pump_config_t *cfg);
