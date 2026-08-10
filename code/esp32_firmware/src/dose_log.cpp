/**
 * dose_log.cpp — 紧凑追加式剂量追溯日志 (实现见 dose_log.h)
 *
 * 持久化到 partitions.csv 的 dose_log 分区 (追加写, 永不覆盖);
 * 写指针/计数/时间戳基准/锚点存 NVS (olp_dlog 命名空间)。
 * 模拟器侧不链接 Flash/NVS, 全部桩化为 no-op, 保证单一真源 FSM 可编译演示。
 */
#include "dose_log.h"
#include "config.h"
#include "rtc_clock.h"   // rtc_unix_now()  (固件与模拟器均有定义)

#ifndef SIMULATOR
  #include <Arduino.h>
  #include <Preferences.h>
  #include "esp_partition.h"
#endif

#include <string.h>

// NVS 元数据键
#define DL_NS        "olp_dlog"
#define DL_ANCHOR_EVERY 1000U   // 每写入 1000 条打一个时间锚点, 限制读取扫描量

#ifndef SIMULATOR
static const esp_partition_t *s_part = nullptr;
static Preferences s_prefs;
#endif

static uint32_t s_off      = 0;     // dose_log 分区内写偏移 (字节)
static uint32_t s_cnt      = 0;     // 总记录数
static uint32_t s_base_ts  = 0;     // 首条记录重建后 Unix 秒 (读起点, 锚点基准)
static uint32_t s_last_ts  = 0;     // 上一条记录真实 Unix 秒 (计算 delta 用)
static uint32_t s_recon_ts = 0;     // 重建运行时间戳 (按写入的取整 delta 推进, 非墙钟)
static uint32_t s_anchor_idx = 0;   // 最近锚点记录索引
static uint32_t s_anchor_ts  = 0;   // 最近锚点重建 Unix 秒 (== s_recon_ts 当时值)
static bool     s_full     = false;

void dose_log_init(void)
{
    s_off = s_cnt = s_base_ts = s_last_ts = s_recon_ts = 0;
    s_anchor_idx = s_anchor_ts = 0;
    s_full = false;

#ifndef SIMULATOR
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x99, "dose_log");
    s_prefs.begin(DL_NS, false);
    s_off        = s_prefs.getUInt("off", 0);
    s_cnt        = s_prefs.getUInt("cnt", 0);
    s_base_ts    = s_prefs.getUInt("base", 0);
    s_last_ts    = s_prefs.getUInt("last", 0);
    s_recon_ts   = s_prefs.getUInt("rts", 0);
    s_anchor_idx = s_prefs.getUInt("aidx", 0);
    s_anchor_ts  = s_prefs.getUInt("ats", 0);
    s_full       = s_prefs.getUChar("full", 0) != 0;
    // 校验偏移合法性 (应为 8 的倍数且不超过分区)
    if (s_part && (s_off % DOSE_LOG_REC_SIZE != 0 || s_off > s_part->size))
        { s_off = 0; }
#endif
}

void dose_log_append(event_type_t type, uint32_t amount_x100, uint16_t param2, uint8_t flags)
{
#ifndef SIMULATOR
    if (!s_part || s_full) return;

    uint32_t now = rtc_unix_now();
    uint16_t delta = 0;
    if (s_base_ts != 0 && s_last_ts != 0) {
        int64_t d = (int64_t)now - (int64_t)s_last_ts;
        if (d < 0) d = 0;
        delta = (d / 60 > 65535) ? 65535 : (uint16_t)(d / 60);
    }

    // 钳制 amount 到 uint16 (655.35U, 远超任何合理单次剂量)
    if (amount_x100 > 0xFFFF) amount_x100 = 0xFFFF;

    uint8_t rec[DOSE_LOG_REC_SIZE];
    rec[0] = (uint8_t)(((flags & 0x0F) << 4) | (type & 0x0F));
    rec[1] = (uint8_t)(amount_x100 & 0xFF);
    rec[2] = (uint8_t)((amount_x100 >> 8) & 0xFF);
    rec[3] = (uint8_t)(delta & 0xFF);
    rec[4] = (uint8_t)((delta >> 8) & 0xFF);
    rec[5] = (uint8_t)(param2 & 0xFF);
    rec[6] = (uint8_t)((param2 >> 8) & 0xFF);
    rec[7] = 0;

    if (esp_partition_write(s_part, s_off, rec, DOSE_LOG_REC_SIZE) != ESP_OK) return;

    bool first = (s_base_ts == 0);
    if (first) s_base_ts = now;
    s_last_ts = now;
    // 重建运行时间戳: 严格按"写入记录的取整 delta"推进, 与读取端累加一致,
    // 绝不可用真实墙钟 now (否则整数分钟取整会逐条累积漂移).
    if (first) s_recon_ts = now;
    else       s_recon_ts += (uint32_t)delta * 60UL;
    s_off += DOSE_LOG_REC_SIZE;
    s_cnt += 1;
    if (s_off + DOSE_LOG_REC_SIZE > s_part->size) s_full = true;

    // 时间锚点: 首条锚到重建基准; 之后每 DL_ANCHOR_EVERY 条刷新一次,
    // 限制读取扫描量 (≤ DL_ANCHOR_EVERY 条)。锚点存"上一记录的重建时间戳",
    // 读取端从锚点索引累加该记录 delta 后恰好到达锚点记录本身 (避免 off-by-one 漂移).
    uint32_t anchor_base = s_recon_ts - (uint32_t)delta * 60UL;
    if (first) {
        s_anchor_idx = 0;
        s_anchor_ts  = anchor_base;   // first: delta=0 -> = base_ts
    } else if (s_cnt % DL_ANCHOR_EVERY == 0) {
        s_anchor_idx = s_cnt - 1;
        s_anchor_ts  = anchor_base;
    }

    s_prefs.putUInt("off", s_off);
    s_prefs.putUInt("cnt", s_cnt);
    s_prefs.putUInt("base", s_base_ts);
    s_prefs.putUInt("last", s_last_ts);
    s_prefs.putUInt("rts", s_recon_ts);
    s_prefs.putUInt("aidx", s_anchor_idx);
    s_prefs.putUInt("ats", s_anchor_ts);
    s_prefs.putUChar("full", s_full ? 1 : 0);
#else
    (void)type; (void)amount_x100; (void)param2; (void)flags;
#endif
}

uint32_t dose_log_count(void) { return s_cnt; }
bool     dose_log_full(void)  { return s_full; }

// 内部: 把"距最新的 index_from_newest"单条解出
// 扫描起点: 目标索引在锚点之后 -> 从锚点快累加(常见); 否则(跨锚点, 请求很老)从首条 base_ts 全量累加
static bool dl_decode_from(int target_from_newest, dose_log_entry_t *out)
{
#ifndef SIMULATOR
    if (!s_part || target_from_newest < 0 || (uint32_t)target_from_newest >= s_cnt) return false;
    int target_idx = (int)s_cnt - 1 - target_from_newest;
    int scan_from = (target_idx >= (int)s_anchor_idx) ? (int)s_anchor_idx : 0;
    uint32_t ts   = (target_idx >= (int)s_anchor_idx) ? s_anchor_ts : s_base_ts;

    uint8_t rec[DOSE_LOG_REC_SIZE];
    for (int idx = scan_from; idx <= target_idx; idx++) {
        if (esp_partition_read(s_part, (size_t)idx * DOSE_LOG_REC_SIZE, rec, DOSE_LOG_REC_SIZE) != ESP_OK)
            return false;
        uint16_t d = (uint16_t)(rec[3] | (rec[4] << 8));
        ts += (uint32_t)d * 60UL;
        if (idx == target_idx) {
            out->type        = rec[0] & 0x0F;
            out->flags       = (rec[0] >> 4) & 0x0F;
            out->amount_x100 = (uint32_t)(rec[1] | (rec[2] << 8));
            out->param2      = (uint16_t)(rec[5] | (rec[6] << 8));
            out->timestamp   = ts;
            return true;
        }
    }
    return false;
#else
    (void)target_from_newest; (void)out;
    return false;
#endif
}

bool dose_log_read(int index_from_newest, dose_log_entry_t *out)
{
    if (!out) return false;
    return dl_decode_from(index_from_newest, out);
}

uint32_t dose_log_read_recent(uint32_t max_n, dose_log_entry_t *out)
{
    if (!out || max_n == 0) return 0;
#ifndef SIMULATOR
    if (!s_part || s_cnt == 0) return 0;
    uint32_t start_idx = (s_cnt > max_n) ? (s_cnt - max_n) : 0;
    // 扫描起点: 目标窗口起点在锚点之后 -> 从锚点快累加(常见, O(≤1000));
    //          否则(请求 > ~1000 条, 跨越锚点)从首条 base_ts 全量累加(O(n), 偶发审计).
    int      scan_from = (start_idx >= s_anchor_idx) ? (int)s_anchor_idx : 0;
    uint32_t ts        = (start_idx >= s_anchor_idx) ? s_anchor_ts : s_base_ts;

    uint32_t fill = 0;
    uint8_t rec[DOSE_LOG_REC_SIZE];
    for (uint32_t idx = (uint32_t)scan_from; idx < s_cnt; idx++) {
        if (esp_partition_read(s_part, (size_t)idx * DOSE_LOG_REC_SIZE, rec, DOSE_LOG_REC_SIZE) != ESP_OK)
            break;
        uint16_t d = (uint16_t)(rec[3] | (rec[4] << 8));
        ts += (uint32_t)d * 60UL;
        if (idx >= start_idx) {   // 仅发射目标窗口内的记录, 前的锚点段只用于累加时间戳
            dose_log_entry_t e;
            e.type        = rec[0] & 0x0F;
            e.flags       = (rec[0] >> 4) & 0x0F;
            e.amount_x100 = (uint32_t)(rec[1] | (rec[2] << 8));
            e.param2      = (uint16_t)(rec[5] | (rec[6] << 8));
            e.timestamp   = ts;
            out[fill++] = e;
            if (fill >= max_n) break;
        }
    }
    return fill;
#else
    (void)max_n;
    return 0;
#endif
}

void dose_log_wipe(void)
{
#ifndef SIMULATOR
    if (s_part) esp_partition_erase_range(s_part, 0, s_part->size);
    s_off = s_cnt = s_base_ts = s_last_ts = s_recon_ts = 0;
    s_anchor_idx = s_anchor_ts = 0;
    s_full = false;
    s_prefs.putUInt("off", 0);
    s_prefs.putUInt("cnt", 0);
    s_prefs.putUInt("base", 0);
    s_prefs.putUInt("last", 0);
    s_prefs.putUInt("rts", 0);
    s_prefs.putUInt("aidx", 0);
    s_prefs.putUInt("ats", 0);
    s_prefs.putUChar("full", 0);
#endif
}
