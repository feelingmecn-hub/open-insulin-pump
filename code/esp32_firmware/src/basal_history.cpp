/**
 * basal_history.cpp — 基础率执行历史 (实现见 basal_history.h)
 *
 * 固件: NVS Preferences 单 blob 环形缓冲 (BH_MAX_RECORDS×16B, ≤1984B 安全)。
 * 模拟器: 内存环形缓冲 + 预置样例, 不链 Flash/NVS。
 */
#include "basal_history.h"
#include "pump_types.h"
#include "pump_state.h"   // g_pump_state.total_units_x100_delivered (快照用)
#include "rtc_clock.h"    // rtc_unix_now / rtc_ymdhms_to_unix (双端均有定义)

#ifndef SIMULATOR
  #include <Arduino.h>
  #include <Preferences.h>
#endif

#include <string.h>

#define BH_NS "olp_bh"
#define BH_BLOB_KEY "buf"

// ---- 内存镜像 (固件 NVS 恢复后也放在这里, 读写都走它) ----
static basal_history_rec_t s_buf[BH_MAX_RECORDS];
static uint32_t s_cnt  = 0;   // 已存总数 (≤ BH_MAX_RECORDS)
static uint32_t s_head = 0;   // 下一个写入位置 (环形)

#ifndef SIMULATOR
static Preferences s_prefs;
// 延迟落盘 (见 basal_history.h 说明): record() 只标脏, loop() 里去抖落盘
static volatile bool s_dirty    = false;
static uint32_t      s_dirty_ms = 0;
#define BH_FLUSH_DEBOUNCE_MS 2000UL
#endif

// 模拟器演示基准 "now" (固定日期, 使预置样例与图表自洽, 不受真实时钟影响)
static uint32_t s_sim_now = 0;

// 与最新一条比对去重
static bool bh_dup(uint8_t type, uint8_t profile, uint8_t loop_mode,
                   uint16_t tbr_percent_x10, uint16_t rate_x100)
{
    if (s_cnt == 0) return false;
    // ⚠️ #260: 验证测试是"用户主动触发的一次性动作", 必须条条留痕。
    //    若沿用 2 分钟去重, 用户连按两次对照丝杠行程时会被静默吞掉一条,
    //    直接违背"历史里能明确看到是否生效"的初衷。故永不去重。
    if (type == BH_BASAL_TEST) return false;
    basal_history_rec_t *last = &s_buf[(s_head + BH_MAX_RECORDS - 1) % BH_MAX_RECORDS];
    if (last->type != type) return false;
    if (last->profile != profile) return false;
    if (last->loop_mode != loop_mode) return false;
    if (last->tbr_percent_x10 != tbr_percent_x10) return false;
    if (last->rate_x100 != rate_x100) return false;
    uint32_t now = basal_history_now();
    uint32_t dt = (now >= last->ts) ? (now - last->ts) : 0;
    return dt < 120u;   // 同状态 2 分钟内重复 -> 跳过
}

void basal_history_init(void)
{
    s_cnt = 0; s_head = 0;
    memset(s_buf, 0, sizeof(s_buf));

#ifndef SIMULATOR
    s_prefs.begin(BH_NS, false);
    s_cnt  = s_prefs.getUInt("cnt", 0);
    s_head = s_prefs.getUInt("head", 0);
    size_t got = s_prefs.getBytes(BH_BLOB_KEY, s_buf, sizeof(s_buf));
    if (got != sizeof(s_buf)) { s_cnt = 0; s_head = 0; }   // 校验失败重置
    if (s_cnt > BH_MAX_RECORDS) s_cnt = BH_MAX_RECORDS;
    if (s_head >= BH_MAX_RECORDS) s_head = 0;
#else
    // ---- 模拟器: 预置样例 (过去 ~12h, 方案切换 + 一次 TBR) ----
    s_sim_now = rtc_ymdhms_to_unix(2026, 7, 30, 12, 0, 0);  // 演示基准 12:00
    uint32_t base = s_sim_now;
    // 累积胰岛素示例 (随方案/时长变化, 仅用于演示柱状图高低)
    uint32_t cum = 0;
    // 08:00 起, 方案1 (默认昼夜)
    cum += 240;  // 4h × 0.6U/h ≈ 2.4U
    s_buf[s_head++] = { base - 4*3600, cum, BH_PROFILE_SWITCH, 0, 1, 0, 0, 60 };
    s_cnt++;
    // 10:00 切到方案2 (工作日)
    cum += 130;
    s_buf[s_head++] = { base - 2*3600, cum, BH_PROFILE_SWITCH, 1, 1, 0, 0, 90 };
    s_cnt++;
    // 10:30 来一次 TBR 130%
    cum += 60;
    s_buf[s_head++] = { base - 90*60, cum, BH_TBR_START, 1, 1, 0, 1300, 117 };
    s_cnt++;
    // 11:30 TBR 结束
    cum += 90;
    s_buf[s_head++] = { base - 30*60, cum, BH_TBR_END, 1, 1, 0, 0, 90 };
    s_cnt++;
    // 11:45 AAPS 接管 (闭环)
    cum += 40;
    s_buf[s_head++] = { base - 15*60, cum, BH_AAPS_TAKEOVER, 1, 0, 0, 0, 90 };
    s_cnt++;
    // 11:50 模式切回开环本地档案
    cum += 15;
    s_buf[s_head++] = { base - 10*60, cum, BH_MODE_CHANGE, 1, 1, 0, 0, 90 };
    s_cnt++;
    g_pump_state.total_units_x100_delivered = cum;   // 让"现在"快照与样例衔接
#endif
}

void basal_history_record(uint8_t type, uint8_t profile, uint8_t loop_mode,
                          uint16_t tbr_percent_x10, uint16_t rate_x100)
{
    if (bh_dup(type, profile, loop_mode, tbr_percent_x10, rate_x100)) return;

    basal_history_rec_t rec;
    rec.ts             = basal_history_now();
    rec.cum_ins_x100   = (uint32_t)g_pump_state.total_units_x100_delivered;
    rec.type           = type;
    rec.profile        = profile;
    rec.loop_mode      = loop_mode;
    rec._pad           = 0;
    rec.tbr_percent_x10 = tbr_percent_x10;
    rec.rate_x100      = rate_x100;

    s_buf[s_head] = rec;
    s_head = (s_head + 1) % BH_MAX_RECORDS;
    if (s_cnt < BH_MAX_RECORDS) s_cnt++;

#ifndef SIMULATOR
    // ⚠️ 不在此处写 NVS: 本函数可能由 NimBLE 回调触发, flash 擦写会关 cache → panic。
    //    仅标脏, 由 basal_history_flush_tick() 在 loop() 上下文落盘。
    s_dirty    = true;
    s_dirty_ms = (uint32_t)millis();
#endif
}

bool basal_history_flush_tick(void)
{
#ifndef SIMULATOR
    if (!s_dirty) return false;
    if ((uint32_t)((uint32_t)millis() - s_dirty_ms) < BH_FLUSH_DEBOUNCE_MS) return false;
    s_prefs.putBytes(BH_BLOB_KEY, s_buf, sizeof(s_buf));
    s_prefs.putUInt("cnt", s_cnt);
    s_prefs.putUInt("head", s_head);
    s_dirty = false;
    return true;
#else
    return false;
#endif
}

uint32_t basal_history_count(void) { return s_cnt; }

bool basal_history_read(uint32_t index_from_newest, basal_history_rec_t *out)
{
    if (!out || index_from_newest >= s_cnt) return false;
    // 逻辑索引: head-1 是最新
    int idx = (int)((s_head + BH_MAX_RECORDS - 1 - index_from_newest) % BH_MAX_RECORDS);
    *out = s_buf[idx];
    return true;
}

uint32_t basal_history_now(void)
{
#ifdef SIMULATOR
    return s_sim_now;   // 模拟器演示基准时钟 (basal_history_init 已置, 不依赖真实 RTC)
#else
    uint32_t n = rtc_unix_now();
    return n;
#endif
}

void basal_history_wipe(void)
{
    s_cnt = 0; s_head = 0;
    memset(s_buf, 0, sizeof(s_buf));
#ifndef SIMULATOR
    s_prefs.putBytes(BH_BLOB_KEY, s_buf, sizeof(s_buf));
    s_prefs.putUInt("cnt", 0);
    s_prefs.putUInt("head", 0);
#endif
}
