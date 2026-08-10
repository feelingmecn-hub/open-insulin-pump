/**
 * history_log.cpp — 历史事件记录 (内存环形缓冲 + Preferences 持久化)
 *
 * 重启后从 Flash 恢复最近 32 条事件; 新事件写入时按 60s 节流落盘,
 * 避免高频 BLE/基础率事件频繁擦写 Flash。
 *
 * P0-4: 时间戳改用硬件 RTC (rtc_unix_now()); 未设置时钟时回退为 0(未知)。
 * 同时做 host(联调模拟器) 可编译隔离: <Arduino.h>/<Preferences.h>/millis()
 * 仅在 ESP32 固件侧使用, 模拟器侧用轻量单调时钟桩, 保证单一真源 FSM 在
 * 模拟器也能编译并演示历史记录。
 */
#include "history_log.h"
#include "pump_types.h"   // event_type_t / history_event_t
#include "config.h"

#ifndef SIMULATOR
  #include <Arduino.h>
  #include <Preferences.h>
  #include "rtc_clock.h"   // rtc_unix_now() (固件侧真实 RTC)
#else
// 模拟器: 仅用头文件内的纯函数做日历换算, 不链接 rtc_clock.cpp
#include "rtc_clock.h"
#endif

#include <string.h>
#include <cstdio>

#define HISTORY_RING_SIZE 32
#define HISTORY_SAVE_THROTTLE_MS 60000UL

static history_event_t s_ring[HISTORY_RING_SIZE];
static uint8_t s_idx = 0;
static uint8_t s_count = 0;
#ifndef SIMULATOR
static Preferences s_prefs;
#endif
static uint32_t s_last_save_ms = 0;
#ifndef SIMULATOR
static volatile bool s_pending_save = false;   // 有事件待落盘 (延迟到 loop 上下文写)
#endif

// ---- 时间源 (固件=真实RTC; 模拟器=演示用单调时钟) ----
#ifndef SIMULATOR
static uint32_t hlog_now_s(void)  { return rtc_unix_now(); }   // 真实 Unix 秒
static uint32_t hlog_now_ms(void) { return (uint32_t)millis(); }
#else
// 模拟器演示时钟: 每次取时间戳推进 60s, 仅用于排序/显示演示 (避免 32 位溢出)
static uint32_t s_host_sec = 1700000000UL;   // 演示起点 (类 Unix 秒)
static uint32_t hlog_now_s(void)  { s_host_sec += 60; return s_host_sec; }
static uint32_t hlog_now_ms(void) { return s_host_sec; }   // 模拟器未用于节流
#endif

void history_log_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_idx = 0;
    s_count = 0;

#ifndef SIMULATOR
    s_prefs.begin("olp_hist", false);
    size_t n = s_prefs.getBytes("ring", s_ring, sizeof(s_ring));
    if (n == sizeof(s_ring)) {
        s_idx  = s_prefs.getUChar("idx", 0);
        s_count = s_prefs.getUChar("cnt", 0);
        if (s_idx >= HISTORY_RING_SIZE)  s_idx = 0;
        if (s_count > HISTORY_RING_SIZE) s_count = HISTORY_RING_SIZE;
    }
#endif
    s_last_save_ms = 0;   // 强制首次事件即落盘
}

void history_log_event(event_type_t type, uint8_t alarm, uint32_t p1, uint16_t p2)
{
    s_ring[s_idx].timestamp  = hlog_now_s();
    s_ring[s_idx].type       = (uint8_t)type;
    s_ring[s_idx].alarm_code = alarm;
    s_ring[s_idx].param1     = p1;
    s_ring[s_idx].param2     = p2;
    s_idx = (s_idx + 1) % HISTORY_RING_SIZE;
    if (s_count < HISTORY_RING_SIZE) s_count++;

    // ⚠️ 2026-08-08: 不在此处直接落盘。history_log_event 会被 NimBLE 回调 / 电机任务
    //    调用, 而 NVS 擦写会关闭 flash cache, 期间 BLE ISR 访问非 IRAM 代码即 panic。
    //    这里只标"待落盘", 实际写入交给 loop() 上下文的 history_log_tick()。
#ifndef SIMULATOR
    s_pending_save = true;
#endif
}

// loop() 上下文调用: 有待落盘数据且已过节流窗口则真正写 NVS
bool history_log_tick(void)
{
#ifndef SIMULATOR
    if (!s_pending_save) return false;
    uint32_t now = hlog_now_ms();
    if ((uint32_t)(now - s_last_save_ms) < HISTORY_SAVE_THROTTLE_MS) return false;
    history_log_save();
    s_pending_save = false;
    return true;
#else
    return false;
#endif
}

void history_log_save(void)
{
#ifndef SIMULATOR
    s_prefs.putBytes("ring", s_ring, sizeof(s_ring));
    s_prefs.putUChar("idx", s_idx);
    s_prefs.putUChar("cnt", s_count);
    s_last_save_ms = hlog_now_ms();
#endif
}

// ============================================================
// 读取访问器 (供 UI 历史回看屏使用)
// ============================================================
int history_log_count(void)
{
    return (int)s_count;
}

// index_from_newest: 0 = 最新一条
bool history_log_read(int index_from_newest, history_event_t *out)
{
    if (!out) return false;
    if (index_from_newest < 0 || index_from_newest >= (int)s_count) return false;
    int pos = (int)s_idx - 1 - index_from_newest;
    while (pos < 0) pos += HISTORY_RING_SIZE;
    pos %= HISTORY_RING_SIZE;
    *out = s_ring[pos];
    return true;
}
