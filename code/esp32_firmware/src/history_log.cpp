/**
 * history_log.cpp — 历史事件记录 (内存环形缓冲 + Preferences 持久化)
 *
 * 重启后从 Flash 恢复最近 32 条事件; 新事件写入时按 60s 节流落盘,
 * 避免高频 BLE/基础率事件频繁擦写 Flash。
 */
#include "history_log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#define HISTORY_RING_SIZE 32
#define HISTORY_SAVE_THROTTLE_MS 60000UL

static history_event_t s_ring[HISTORY_RING_SIZE];
static uint8_t s_idx = 0;
static uint8_t s_count = 0;
static Preferences s_prefs;
static uint32_t s_last_save_ms = 0;

void history_log_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_idx = 0;
    s_count = 0;

    s_prefs.begin("olp_hist", false);
    size_t n = s_prefs.getBytes("ring", s_ring, sizeof(s_ring));
    if (n == sizeof(s_ring)) {
        s_idx  = s_prefs.getUChar("idx", 0);
        s_count = s_prefs.getUChar("cnt", 0);
        if (s_idx >= HISTORY_RING_SIZE)  s_idx = 0;
        if (s_count > HISTORY_RING_SIZE) s_count = HISTORY_RING_SIZE;
    }
    s_last_save_ms = 0;   // 强制首次事件即落盘
}

void history_log_event(event_type_t type, uint8_t alarm, uint32_t p1, uint16_t p2)
{
    s_ring[s_idx].timestamp  = (uint32_t)(millis() / 1000UL);  // 运行时秒计数 (无 RTC)
    s_ring[s_idx].type       = (uint8_t)type;
    s_ring[s_idx].alarm_code = alarm;
    s_ring[s_idx].param1     = p1;
    s_ring[s_idx].param2     = p2;
    s_idx = (s_idx + 1) % HISTORY_RING_SIZE;
    if (s_count < HISTORY_RING_SIZE) s_count++;

    // 节流落盘
    uint32_t now = millis();
    if (now - s_last_save_ms >= HISTORY_SAVE_THROTTLE_MS) {
        history_log_save();
    }
}

void history_log_save(void)
{
    s_prefs.putBytes("ring", s_ring, sizeof(s_ring));
    s_prefs.putUChar("idx", s_idx);
    s_prefs.putUChar("cnt", s_count);
    s_last_save_ms = millis();
}
