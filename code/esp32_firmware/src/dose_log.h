/**
 * dose_log.h — 紧凑追加式剂量追溯日志 (持久化, 永不覆盖)
 *
 * 设计目标: 用闲置 Flash 空间 (partitions.csv 的 dose_log 分区, ~2.69MB)
 * 记录"重要参数"级的实际输注/关键事件, 供安全审计与溯源。
 *
 * 记录采用 8 字节定长紧凑格式 (小端):
 *   byte0   : type(低4位) | flags(高4位)
 *   byte1-2 : amount_x100 (uint16) 实际推注量 ×100 (U); 非剂量事件为 0
 *   byte3-4 : delta_min    (uint16) 距上一条记录的分钟数 (时间戳增量编码)
 *   byte5-6 : param2       (uint16) 辅助参数 (TBR 百分比/时长, 基础率×100 等)
 *   byte7   : reserved
 *
 * 时间戳增量编码: 相邻事件通常只差几分钟, 用 2 字节存"距上一条的分钟差"
 * 代替 4 字节绝对时间, 平均压缩显著; 首条记录 delta 记为 0, 绝对时间存于
 * NVS 元数据 base_ts。读取时从 base_ts 累加各条 delta 复原绝对 Unix 秒。
 *
 * 容量 (8字节/条, 2.69MB): ~35 万条
 *   - 只记离散关键事件(大剂量/方波/TBR/排气/报警/开关机, ~7条/天) → ≈137 年 (永久)
 *   - 若改为每3分钟基础率分片也记 → ≈2 年
 *
 * 模拟器侧: 不链接 Flash/NVS, 全部桩化为 no-op, 保证单一真源 FSM 可编译演示。
 */
#pragma once

#include <cstdint>
#include "pump_types.h"   // event_type_t

#define DOSE_LOG_REC_SIZE 8

// flags 位定义 (byte0 高4位)
#define DOSE_FLAG_SRC_BLE    0x01   // 1 = 由 BLE/AAPS 触发; 0 = 本地/UI
#define DOSE_FLAG_ALARM      0x02   // 报警类事件
#define DOSE_FLAG_CANCELLED  0x04   // 被取消/仅部分投递

typedef struct {
    uint8_t   type;        // event_type_t (低4位有效)
    uint8_t   flags;
    uint32_t  amount_x100; // 实际推注量 ×100 (U)
    uint16_t  param2;      // 辅助参数
    uint32_t  timestamp;   // 解码后绝对 Unix 秒 (读取时由 base_ts + 累加 delta 复原)
} dose_log_entry_t;

// 初始化: 从 NVS 恢复写指针/计数/时间戳基准
void    dose_log_init(void);

// 追加一条追溯记录 (不覆盖既有记录; 分区写满后置 full 标志停止)
//   type       : event_type_t
//   amount_x100: 实际推注量 ×100
//   param2     : 辅助参数
//   flags      : DOSE_FLAG_*
void    dose_log_append(event_type_t type, uint32_t amount_x100, uint16_t param2, uint8_t flags);

// 已存记录总数
uint32_t dose_log_count(void);

// 是否写满
bool    dose_log_full(void);

// 读取最近 max_n 条 (out[0..k-1] 由旧到新), 返回实际填充条数 k
uint32_t dose_log_read_recent(uint32_t max_n, dose_log_entry_t *out);

// 按"距最新"索引读取单条 (index 0 = 最新); 仅供偶尔审计, 内部单次扫描
bool    dose_log_read(int index_from_newest, dose_log_entry_t *out);

// 清空日志 (调试/恢复用)
void    dose_log_wipe(void);
