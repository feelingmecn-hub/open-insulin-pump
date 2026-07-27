/**
 * dosing.h — 剂量换算「单一真源」(Single Source of Truth)
 *
 * ⚠️ 本文件是全系统唯一允许定义「单位(U) ↔ 微步」换算算法与储药罐几何推导的地方。
 *    - 储药罐类型在 config.h §8.1 用 RESERVOIR_TYPE 选择, 本文件只从「内腔直径」
 *      单一参数推导全部几何 (面积 / 每转体积 / 每单位步数 / 0.05U 步数)。
 *    - 所有打药路径 (大剂量 / 基础率 / 排气 / JOG) 必须且只能调用本文件提供的
 *      units_to_microsteps() / microsteps_to_units() / quantize_units_005()
 *      三个函数, 禁止任何模块自行拿 STEPS_PER_UNIT 现算 (否则取整/标定会不一致)。
 *    - 固件与模拟器共用本文件 (模拟器 CMake 把本目录加入包含路径), 杜绝算法双份。
 *    - 改储药罐类型: 只改 config.h 的 RESERVOIR_TYPE 一个宏, 此处自动重算, 无需动别处。
 *
 * 依赖 (均由 config.h 在 #include "dosing.h" 之前定义):
 *   SYRINGE_DIAMETER_MM, MOTOR_EFFECTIVE_STEPS, LEAD_SCREW_PITCH_MM,
 *   DOSE_CALIBRATION, MIN_DOSE_UNITS
 */

#ifndef DOSING_H
#define DOSING_H

#include <stdint.h>

#ifndef SYRINGE_DIAMETER_MM
#error "dosing.h 必须在 config.h 之后包含: config.h 负责定义 SYRINGE_DIAMETER_MM 等几何基参"
#endif

// ============================================================
// 储药罐几何 — 全部由 SYRINGE_DIAMETER_MM 单一推导, 绝不手算硬编码
// ============================================================
#define SYRINGE_RADIUS_MM     (SYRINGE_DIAMETER_MM / 2.0f)
#define SYRINGE_AREA_MM2      (3.141592653589793f * SYRINGE_RADIUS_MM * SYRINGE_RADIUS_MM)
#define MM_PER_STEP           (LEAD_SCREW_PITCH_MM / (float)MOTOR_EFFECTIVE_STEPS)
#define UL_PER_STEP           (MM_PER_STEP * SYRINGE_AREA_MM2)
#define UNITS_PER_STEP        (UL_PER_STEP / 10.0f)   // U-100: 1U = 10µL
#define STEPS_PER_UNIT        (1.0f / UNITS_PER_STEP)
#define STEPS_PER_005U        ((uint32_t)(STEPS_PER_UNIT * MIN_DOSE_UNITS + 0.5f))
#define UNITS_PER_MICROSTEP   (1.0f / (STEPS_PER_UNIT * DOSE_CALIBRATION))

// ============================================================
// 唯一换算接口 (static inline → 每个 TU 自带一份, 无链接冲突, 全系统行为一致)
// ============================================================
static inline uint32_t units_to_microsteps(float units)
{
    if (units <= 0.0f) return 0;
    // 唯一换算入口: 单位(U) → 微步。DOSE_CALIBRATION 用于实测标定整体缩放。
    return (uint32_t)(units * STEPS_PER_UNIT * DOSE_CALIBRATION + 0.5f);
}

static inline float microsteps_to_units(uint32_t steps)
{
    return (float)steps / (STEPS_PER_UNIT * DOSE_CALIBRATION);
}

// 吸附到 MIN_DOSE_UNITS 最小精度网格 (大剂量命令级安全网)
static inline float quantize_units_005(float units)
{
    if (units <= 0.0f) return 0.0f;
    float q = (float)((int)(units / MIN_DOSE_UNITS + 0.5f)) * MIN_DOSE_UNITS;
    if (q < MIN_DOSE_UNITS) q = MIN_DOSE_UNITS;
    return q;
}

#endif // DOSING_H
