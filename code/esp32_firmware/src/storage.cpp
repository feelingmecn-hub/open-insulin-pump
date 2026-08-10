/**
 * storage.cpp — 配置持久化 (Arduino Preferences)
 *
 * 见 storage.h 顶部说明: 写入改为「标脏 + loop() 去抖落盘」, 避免在 BLE 回调
 * 上下文同步擦写 flash 导致 cache-disabled panic。
 */
#include <Arduino.h>
#include "storage.h"
#include "config.h"
#include "pump_state.h"   // pump_config_apply_default_basal()
#include <Preferences.h>
#include <string.h>

static Preferences g_prefs;

// ---- 延迟落盘状态 ----
static pump_config_t  s_pending;              // 待写快照
static volatile bool  s_dirty      = false;
static volatile bool  s_have_snap  = false;
static uint32_t       s_dirty_ms   = 0;

void storage_init(void)
{
    g_prefs.begin("olp_pump", false);
    s_dirty = false;
    s_have_snap = false;
}

void storage_load_config(pump_config_t *cfg)
{
    size_t n = g_prefs.getBytes("config", cfg, sizeof(pump_config_t));
    if (n == 0) {
        // 无有效存储, 调用方保留默认配置
        return;
    }
    // ⚠️ 向前兼容: 结构体新增字段后 sizeof 变大, 旧存档 n < sizeof(pump_config_t)。
    //    Preferences::getBytes 在 stored_len <= maxLen 时会拷贝 stored_len 字节并返回它,
    //    此时前 n 字节是旧配置、其余字段保持调用方给的默认值 —— 这正是我们要的,
    //    绝不能因为 n != sizeof 就整份丢弃 (那会让用户升级固件后所有基础率方案清零)。
    if (n < sizeof(pump_config_t)) {
        Serial.printf("[STORAGE] 旧版配置 %u/%u 字节, 新增字段用默认值\n",
                      (unsigned)n, (unsigned)sizeof(pump_config_t));
    }
    // 防止历史存档把基础率方案清零导致本地模式无输注:
    // 若当前活动方案的 24 个 slot 全为 0, 重新套用默认方案。
    uint8_t prof = cfg->active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    bool all_zero = true;
    for (int i = 0; i < BASAL_SLOTS_PER_DAY; i++) {
        if (cfg->profiles[prof].slots[i].rate_uh > 0.0f) { all_zero = false; break; }
    }
    if (all_zero) {
        pump_config_apply_default_basal(cfg);
    }
}

void storage_save_config(const pump_config_t *cfg)
{
    if (!cfg) return;
    // 只做内存快照 + 标脏, 真正落盘交给 storage_flush_tick() (loop 上下文)
    memcpy(&s_pending, cfg, sizeof(pump_config_t));
    s_have_snap = true;
    s_dirty     = true;
    s_dirty_ms  = (uint32_t)millis();
}

bool storage_is_dirty(void) { return s_dirty; }

static void storage_do_write(void)
{
    if (!s_have_snap) return;
    g_prefs.putBytes("config", &s_pending, sizeof(pump_config_t));
    s_dirty = false;
}

bool storage_flush_tick(void)
{
    if (!s_dirty) return false;
    uint32_t now = (uint32_t)millis();
    if ((uint32_t)(now - s_dirty_ms) < STORAGE_FLUSH_DEBOUNCE_MS) return false;
    storage_do_write();
    return true;
}

void storage_flush_now(void)
{
    if (!s_dirty) return;
    storage_do_write();
}
