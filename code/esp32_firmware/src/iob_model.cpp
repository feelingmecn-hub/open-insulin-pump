/**
 * iob_model.cpp — IOB 衰减模型实现 (见 iob_model.h)
 */
#include "iob_model.h"
#include "pump_state.h"   // g_pump_state (ext_bolus_* 与 iob_x10000)
#include "pump_types.h"
#include <Arduino.h>      // millis()
#include <string.h>       // memset()

// Walsh IOB 衰减模型 (AAPS 默认 InsulinWalsh): 三角活性曲线, 峰值在 DIA/2
//   活性(t) = (2/DIA)·(t/(DIA/2))         当 0 ≤ t ≤ DIA/2
//   活性(t) = (2/DIA)·((DIA-t)/(DIA/2))   当 DIA/2 ≤ t ≤ DIA
//   IOB(t)  = 2·(t/DIA)²                  当 t ≤ DIA/2
//   IOB(t)  = 1 - 2·((DIA-t)/DIA)²        当 DIA/2 ≤ t ≤ DIA
//   IOB(t)  = 0                           当 t ≥ DIA
// DIA 由 config 的 IOB_DURATION_HOURS 推导; 与 AAPS IOB 数值一致。

// 投递记录: kind=0 即时; kind=1 延展量(含 start/duration 供解析积分)
typedef struct {
    uint8_t  used;
    uint8_t  kind;          // 0=即时 1=延展
    uint32_t uptime_ms;     // 即时: 投递时刻; 延展: 起始时刻
    uint32_t units_x100;    // 总剂量 (U × 100)
    uint32_t duration_ms;   // 延展量总时长 (kind=1)
} iob_rec_t;

static iob_rec_t s_log[IOB_LOG_MAX];
static int       s_ext_idx = -1;   // 当前活动延展量记录在 s_log 中的下标 (-1=无)

void iob_init(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_ext_idx = -1;
}

// 单笔剂量经过 t_min 分钟后的剩余 IOB 比例 [0,1] —— Walsh 三角衰减
float iob_fraction(float t_min)
{
    const float dia = (float)IOB_DIA_MIN;   // DIA (分钟)
    if (t_min <= 0.0f) return 1.0f;         // 刚投递: 全额活性
    if (t_min >= dia)  return 0.0f;         // 超出作用时长: 清零
    float r;
    if (t_min <= dia * 0.5f) {              // 上升段 (峰值前)
        r = t_min / dia;
        return 2.0f * r * r;
    }
    r = (dia - t_min) / dia;                // 下降段 (峰值后)
    return 1.0f - 2.0f * r * r;
}

// 取一条空闲或最旧的记录槽
static int alloc_slot(void)
{
    for (int i = 0; i < IOB_LOG_MAX; i++) {
        if (!s_log[i].used) return i;
    }
    // 全满: 找最旧 (uptime 最小)
    int oldest = 0;
    uint32_t old_t = 0xFFFFFFFFU;
    for (int i = 0; i < IOB_LOG_MAX; i++) {
        if (s_log[i].uptime_ms < old_t) { old_t = s_log[i].uptime_ms; oldest = i; }
    }
    return oldest;
}

void iob_record_bolus(float units)
{
    if (units <= 0.0f) return;
    int i = alloc_slot();
    s_log[i].used       = 1;
    s_log[i].kind       = 0;
    s_log[i].uptime_ms  = (uint32_t)millis();
    s_log[i].units_x100 = (uint32_t)(units * 100.0f + 0.5f);
    s_log[i].duration_ms = 0;
}

void iob_record_extended_start(float total, uint32_t duration_ms, uint32_t start_ms)
{
    if (total <= 0.0f || duration_ms == 0) return;
    int i = alloc_slot();
    s_log[i].used        = 1;
    s_log[i].kind        = 1;
    s_log[i].uptime_ms   = start_ms;
    s_log[i].units_x100  = (uint32_t)(total * 100.0f + 0.5f);
    s_log[i].duration_ms = duration_ms;
    s_ext_idx = i;
}

void iob_record_extended_cancel(uint32_t now_ms, float delivered_units)
{
    if (s_ext_idx < 0 || s_ext_idx >= IOB_LOG_MAX) return;
    iob_rec_t *r = &s_log[s_ext_idx];
    if (!r->used || r->kind != 1) { s_ext_idx = -1; return; }
    // 裁剪为"实际已投递"与"实际已历时", 其余未投递部分不计入 IOB
    uint32_t elapsed = (now_ms > r->uptime_ms) ? (now_ms - r->uptime_ms) : 0;
    if (elapsed > r->duration_ms) elapsed = r->duration_ms;
    r->duration_ms  = elapsed;
    r->units_x100   = (uint32_t)(delivered_units * 100.0f + 0.5f);
    s_ext_idx = -1;
}

void iob_recompute(void)
{
    float iob = 0.0f;
    uint32_t now = (uint32_t)millis();

    for (int i = 0; i < IOB_LOG_MAX; i++) {
        iob_rec_t *r = &s_log[i];
        if (!r->used) continue;

        uint32_t elapsed = (now > r->uptime_ms) ? (now - r->uptime_ms) : 0;
        float emin = (float)elapsed / 60000.0f;

        if (emin > (float)IOB_DIA_MIN) {
            r->used = 0;                       // 超出作用时长, 清除
            if (s_ext_idx == i) s_ext_idx = -1;
            continue;
        }

        if (r->kind == 0) {
            // 即时大剂量: 单笔衰减
            iob += ((float)r->units_x100 / 100.0f) * iob_fraction(emin);
        } else {
            // 延展量: 在 [start, start+D] 内以恒定速率 r=total/D 投递,
            // 当前已投递部分对 IOB 的贡献 = ∫_0^{tEnd} (total/D)·f(tSince-τ) dτ
            float D = (float)r->duration_ms / 60000.0f;
            float T = (float)r->units_x100 / 100.0f;
            if (D > 0.0f && T > 0.0f) {
                float r_per_min = T / D;
                float tSince = emin;
                float tEnd = (tSince < D) ? tSince : D;   // 已投递到的时刻
                for (float tau = 0.0f; tau < tEnd; tau += 1.0f) {
                    float age = tSince - tau;
                    if (age < 0.0f) age = 0.0f;
                    iob += r_per_min * 1.0f * iob_fraction(age);
                }
            }
        }
    }

    g_pump_state.iob_x10000 = (uint32_t)(iob * 10000.0f + 0.5f);
}
