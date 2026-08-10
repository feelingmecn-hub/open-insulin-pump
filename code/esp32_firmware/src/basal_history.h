/**
 * basal_history.h — 基础率执行历史 (环形记录)
 *
 * 目的: 记录"哪套基础率方案 / 什么模式下 / 是否临时基础率"在何时段被执行,
 *       并附当时累计胰岛素快照, 供 UI "时间轴 + 胰岛素用量轴" 柱状图回看。
 *
 * 与 dose_log 分工:
 *   - dose_log  : 离散关键输注/事件 (大剂量/排气/报警...) 的"剂量追溯"日志
 *   - basal_history: 基础率"执行状态"变更时间线 (方案切换/TBR/模式/AAPS接管)
 *
 * 记录采用 16 字节定长紧凑格式:
 *   ts            uint32  事件 Unix 时间戳
 *   cum_ins_x100  uint32  当时累计胰岛素 ×100 (U), 用于计算时段增量用量
 *   type          uint8   bh_event_type_t
 *   profile       uint8   当时激活方案索引 (0-3, 0xFF=无)
 *   loop_mode     uint8   当时闭环模式 (0/1/2)
 *   _pad          uint8
 *   tbr_percent_x10 uint16 当时 TBR 百分比 ×10 (0=无)
 *   rate_x100     uint16  当时主基础率速率 U/h ×100 (已含 TBR)
 *
 * 容量 BH_MAX_RECORDS=100 (16B×100=1600B, NVS 单 blob ≤1984B 安全)。
 * 模拟器侧不链 Flash/NVS, 用内存环形缓冲 + 预置样例, 保证单一真源 FSM 可编译演示。
 */
#pragma once

#include <cstdint>

// 基础率执行历史记录类型
typedef enum {
    BH_PROFILE_SWITCH = 0x01,   // 切换/选择基础率方案
    BH_TBR_START      = 0x02,   // 临时基础率开始
    BH_TBR_END        = 0x03,   // 临时基础率结束/取消
    BH_MODE_CHANGE    = 0x04,   // 闭环/开环/暂停 模式切换
    BH_AAPS_TAKEOVER  = 0x05,   // AAPS 完成 Dana 握手接管
    BH_BASAL_ACTIVE   = 0x06,   // 基础率正按方案输注 (速率发生变化时打点, 证明"设置已生效")
    BH_BASAL_TEST     = 0x07,   // 基础率验证测试: 一次性打出全天 24 段总量
} bh_event_type_t;

#define BH_MAX_RECORDS 100

typedef struct {
    uint32_t ts;             // Unix 时间戳
    uint32_t cum_ins_x100;   // 当时累计胰岛素 ×100 (U)
    uint8_t  type;           // bh_event_type_t
    uint8_t  profile;        // 当时激活方案索引 (0-3, 0xFF=无)
    uint8_t  loop_mode;      // 当时闭环模式 (0/1/2)
    uint8_t  _pad;
    uint16_t tbr_percent_x10;// 当时 TBR 百分比 ×10 (0=无)
    uint16_t rate_x100;      // 当时主基础率速率 U/h ×100 (已含 TBR)
} basal_history_rec_t;

// 初始化: 固件从 NVS 恢复; 模拟器建立内存环 + 预置样例
void     basal_history_init(void);

// 记录一条 (自动去重: 与最新一条 type+profile+loop+tbr+rate 相同且 <120s 则跳过)
//   内部自动快照 g_pump_state.total_units_x100_delivered 作为 cum_ins。
//
// ⚠️ 2026-08-08: 本函数**只写内存环 + 标脏**, 不再同步写 NVS。
//    原实现每条记录都 putBytes 1600B blob, 若在 NimBLE 回调里被调用,
//    flash cache 关闭窗口过长 → BLE ISR 触发 cache-disabled panic → 复位
//    (即用户报告的"App 一改环模式开关就崩")。落盘改由 basal_history_flush_tick()
//    在 loop() 上下文完成。
void     basal_history_record(uint8_t type, uint8_t profile, uint8_t loop_mode,
                              uint16_t tbr_percent_x10, uint16_t rate_x100);

// 在 loop() (普通任务上下文) 周期调用: 有脏数据且去抖到期则真正落盘 NVS。
// 返回 true 表示本次发生了实际写入。
bool     basal_history_flush_tick(void);

// 已存记录总数
uint32_t basal_history_count(void);

// 读取单条 (index 0 = 最新), 返回 true 成功
bool     basal_history_read(uint32_t index_from_newest, basal_history_rec_t *out);

// 当前"现在"时间戳 (固件=rtc_unix_now; 模拟器=演示基准), 供时间轴末段计算
uint32_t basal_history_now(void);

// 清空 (调试/恢复)
void     basal_history_wipe(void);
