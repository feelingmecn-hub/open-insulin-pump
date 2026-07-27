/**
 * storage.cpp — 配置持久化 (Arduino Preferences)
 */
#include "storage.h"
#include "config.h"
#include "pump_state.h"   // pump_config_apply_default_basal()
#include <Preferences.h>

static Preferences g_prefs;

void storage_init(void)
{
    g_prefs.begin("olp_pump", false);
}

void storage_load_config(pump_config_t *cfg)
{
    size_t n = g_prefs.getBytes("config", cfg, sizeof(pump_config_t));
    if (n != sizeof(pump_config_t)) {
        // 无有效存储, 调用方保留默认配置
        return;
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
    g_prefs.putBytes("config", cfg, sizeof(pump_config_t));
}
