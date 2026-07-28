/**
 * iob_model.h — IOB (体内剩余活性胰岛素) 衰减模型
 *
 * 设计目标: 修复原"iob_x10000 只增不减"的语义错误。
 *   - 每笔大剂量投递后记录 (剂量, 投递时刻);
 *   - 周期性调用 iob_recompute() 按胰岛素活性曲线计算当前 IOB, 写入 g_pump_state.iob_x10000;
 *   - 方波/双波延展量按"线性投递"解析式积分 (而非整笔当作瞬时), 保证
 *     投递期间 IOB 随时间爬升、完成后继续按曲线衰减、最终归零, 无跳变。
 *
 * 曲线: Walsh 三角衰减 (AAPS 默认 InsulinWalsh 模型), 峰值在 DIA/2; DIA 由 config 的
 *   IOB_DURATION_HOURS 推导 (默认 4h = 240min)。与 AAPS IOB 数值一致。
 */
#pragma once

#include <cstdint>
#include "config.h"   // IOB_DURATION_HOURS

#define IOB_DIA_MIN        ((uint32_t)(IOB_DURATION_HOURS * 60.0f))
#define IOB_LOG_MAX        48

// 初始化 (清空投递记录)
void iob_init(void);

// 记录一笔"即时"大剂量 (剂量 units U, 于 millis() 此刻投递)
void iob_record_bolus(float units);

// 记录一笔延展量(方波/双波)开始: 总量 total U, 时长 duration_ms, 起始 millis start_ms
//   之后由 iob_recompute 持续按线性投递解析积分, 完成/取消无需额外处理(见 cancel)
void iob_record_extended_start(float total, uint32_t duration_ms, uint32_t start_ms);

// 延展量取消: 把记录裁剪为"实际已投递量"与"实际已历时", 保证 IOB 不夸大未投递部分
void iob_record_extended_cancel(uint32_t now_ms, float delivered_units);

// 周期性重算 g_pump_state.iob_x10000 (考虑 DIA + 活动/历史延展量)
void iob_recompute(void);

// 纯函数: 单笔剂量经过 t_min 分钟后的剩余 IOB 比例 [0,1]
float iob_fraction(float t_min);
