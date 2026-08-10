/**
 * dosing.h — 剂量换算「单一真源」(Single Source of Truth)
 *
 * ⚠️ 本文件是全系统唯一允许定义「单位(U) ↔ 微步」换算算法与储药罐几何推导的地方。
 *    - 储药罐类型在 config.h §8.1 用 RESERVOIR_TYPE 选择, 本文件只从「内腔直径」
 *      单一参数推导全部几何 (面积 / 每转体积 / 每单位步数 / 最小剂量步数)。
 *    - 所有打药路径 (大剂量 / 基础率 / 排气 / JOG) 必须且只能调用本文件提供的
 *      units_to_microsteps() / microsteps_to_units() / quantize_units_grid()
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

// 运行时标定系数 (P3-14): 默认 = DOSE_CALIBRATION(1.0), 实测标定后可被用户覆盖。
// 由 pump_state.cpp(固件/模拟器) 与 host_glue.cpp(主机测试) 各定义一份, 保证链接一致。
extern float g_dose_calib_factor;

// ============================================================
// 储药罐几何 — 全部由 SYRINGE_DIAMETER_MM 单一推导, 绝不手算硬编码
// ============================================================
#define SYRINGE_RADIUS_MM     (SYRINGE_DIAMETER_MM / 2.0f)
#define SYRINGE_AREA_MM2      (3.141592653589793f * SYRINGE_RADIUS_MM * SYRINGE_RADIUS_MM)
#define MM_PER_STEP           (LEAD_SCREW_PITCH_MM / (float)MOTOR_EFFECTIVE_STEPS)
#define UL_PER_STEP           (MM_PER_STEP * SYRINGE_AREA_MM2)
#define UNITS_PER_STEP        (UL_PER_STEP / 10.0f)   // U-100: 1U = 10µL
#define STEPS_PER_UNIT        (1.0f / UNITS_PER_STEP)
#define STEPS_PER_MIN_DOSE_U  ((uint32_t)(STEPS_PER_UNIT * MIN_DOSE_UNITS + 0.5f))  // 最小剂量(=MIN_DOSE_UNITS)对应步数
#define UNITS_PER_MICROSTEP   (1.0f / (STEPS_PER_UNIT * g_dose_calib_factor))

// ============================================================
// 唯一换算接口 (static inline → 每个 TU 自带一份, 无链接冲突, 全系统行为一致)
// ============================================================
static inline uint32_t units_to_microsteps(float units)
{
    if (units <= 0.0f) return 0;
    // 唯一换算入口: 单位(U) → 微步。g_dose_calib_factor 用于实测标定整体缩放 (P3-14)。
    uint32_t s = (uint32_t)(units * STEPS_PER_UNIT * g_dose_calib_factor + 0.5f);
    if (s > 60000u) s = 60000u;   // 纵深防御: 异常标定系数导致单笔步数爆炸(顶死丝杆)时封顶(>300U 行程, 正常剂量永不触发)
    return s;
}

static inline float microsteps_to_units(uint32_t steps)
{
    return (float)steps / (STEPS_PER_UNIT * g_dose_calib_factor);
}

// 吸附到 MIN_DOSE_UNITS(=0.1U) 最小可靠剂量网格 (命令级安全网: 拒绝 <0.1U 细量)
static inline float quantize_units_grid(float units)
{
    if (units <= 0.0f) return 0.0f;
    float q = (float)((int)(units / MIN_DOSE_UNITS + 0.5f)) * MIN_DOSE_UNITS;
    if (q < MIN_DOSE_UNITS) q = MIN_DOSE_UNITS;
    return q;
}

// ============================================================
// 剂量诚实性原则 (P3-15, 安全红线同级) — 单一真源强制约束
// ============================================================
// 电机物理分辨率 = 1 微步 = UNITS_PER_MICROSTEP (当前储药器直径下 ≈0.0079 U, 随 SYRINGE_DIAMETER_MM 变化)。
// 任何剂量最终都是整数个微步打出, 选定剂量与真实打出量的最大偏差为 ±0.5 微步 (≈±0.004 U)。
// 但整机**实际可靠精度**受丝杆背隙/电机角度误差限制仅 ±0.1~0.3U, 故对外承诺的最小剂量下限
// 取 MIN_DOSE_UNITS = 0.1U (见 config.h), 与丹纳原厂大剂量增量一致。本原则要求:
//   ① 绝不直接把"指令剂量"当"已投递量"记账/显示 — 必须经过整数微步吸附再回读实际值;
//   ② UI/蓝牙对外展示的「剂量档位」步进与显示精度不得细于 0.1U (最小可选/可显示剂量=0.1U),
//      物理量本身仍按整数微步记账(仅供内部累加, 不对外宣称该精度);
//   ③ 做不到的精度 (如 <0.1U 网格的细量) 一律不提供输入入口, 只由内部微步累加器消化。
#define DOSE_RESOLUTION_U   UNITS_PER_MICROSTEP   // 真实可达分辨率 (U/微步), 文档化用

// 把"指令剂量"吸附到最近的整数微步, 回传"实际可投递剂量"(U)与微步数。
// 调用方**必须**用返回的 actual_units 记账/显示, 不得再用原始指令值 ——
// 否则会出现"屏上记了 X U、电机实际打了 Y U"的危险不一致。
static inline uint32_t quantize_to_actual(float cmd_units, float *actual_units_out)
{
    uint32_t steps = units_to_microsteps(cmd_units);
    if (actual_units_out) *actual_units_out = microsteps_to_units(steps);
    return steps;
}

#endif // DOSING_H
