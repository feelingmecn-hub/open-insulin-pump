/**
 * history_log.h — 历史事件记录 (内存环形缓冲, 重启丢失)
 *
 * Arduino 下简化实现; 如需持久化可后续改用 Preferences/LittleFS。
 */
#pragma once

#include "pump_types.h"

void history_log_init(void);
void history_log_event(event_type_t type, uint8_t alarm, uint32_t p1, uint16_t p2);

// 持久化: 把内存环形缓冲写入 Preferences (olp_hist 命名空间)
// 写入频率由 history_log_event 内部节流 (≤1 次/60s), 也可手动调用。
void history_log_save(void);
