/**
 * storage.h — 配置持久化 (Arduino Preferences, 替代 ESP-IDF NVS)
 *
 * ⚠️ 2026-08-08 崩溃修复 (伴生 App 改设置 → 固件复位):
 *   NVS(flash) 写入期间会关闭 SPI flash cache。若在 NimBLE 回调 / 高优先级任务
 *   上下文里同步写, 恰好此时 BLE 控制器 ISR 访问未驻留 IRAM 的代码/常量,
 *   就会触发 "Cache disabled but cached memory region accessed" panic → 复位。
 *   现象即用户报告的"App 里一改开关就崩, 重启后仍显示闭环"。
 *
 *   故 storage_save_config() 改为 **只标脏 + 快照**, 真正落盘由 loop() 上下文的
 *   storage_flush_tick() 完成 (去抖 STORAGE_FLUSH_DEBOUNCE_MS)。所有既有调用点
 *   无需改动即自动获得保护, 同时顺带消除高频重复擦写。
 */
#pragma once

#include "pump_types.h"

// 标脏后延迟多久才真正落盘 (期间的连续修改会被合并为一次写)
#define STORAGE_FLUSH_DEBOUNCE_MS 1500UL

void storage_init(void);
void storage_load_config(pump_config_t *cfg);

// 请求保存 (异步): 快照 cfg 并标脏, **不会**立即写 flash。
void storage_save_config(const pump_config_t *cfg);

// 在 loop() (普通任务上下文) 周期调用: 去抖到期则真正落盘。
// 返回 true 表示本次发生了实际写入。
bool storage_flush_tick(void);

// 立即落盘 (仅可在普通任务上下文调用; 关机/关键节点用)
void storage_flush_now(void);

// 是否还有未落盘的修改
bool storage_is_dirty(void);
