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
// ⚠️ 只可在普通任务/loop 上下文调用, 禁止在 NimBLE 回调里同步调用 (flash cache panic)。
void history_log_save(void);

// loop() 上下文周期调用: 有待落盘事件且已过 60s 节流窗口则真正写入。
// history_log_event() 现在只标脏, 不再自行落盘。
bool history_log_tick(void);

// 读取访问器 (供 UI 历史回看屏): count=总条数; read 按"距最新"索引取出一条
int  history_log_count(void);
bool history_log_read(int index_from_newest, history_event_t *out);
