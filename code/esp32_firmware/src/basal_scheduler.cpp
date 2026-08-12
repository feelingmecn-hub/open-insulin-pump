/**
 * basal_scheduler.cpp — 基础率周期调度器
 */
#include <Arduino.h>
#include "basal_scheduler.h"
#include "config.h"
#include "pump_types.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "history_log.h"
#include "dose_log.h"       // 方波/双波大剂量记入剂量追溯日志
#include "storage.h"
#include "iob_model.h"      // IOB 衰减模型
#include "basal_history.h"  // 基础率执行留痕 (BH_BASAL_ACTIVE / BH_BASAL_TEST)
#include "rtc_clock.h"      // 墙钟整点 (基础率时段必须跟真实时间, 不能跟开机时长)

// 当前整点 (0-23)
//
// ⚠️ 2026-08-08 修复: 原实现用 (millis()/3600000)%24, 那是"开机后经过的小时数",
//    与真实时间毫无关系 —— 开机时刻不同, 同一时刻会落到完全不同的基础率时段,
//    这对 24 段昼夜档案是致命错误。现改用 RTC 墙钟; 仅当时钟完全未设置时
//    才退回运行时长(保证不至于崩, 但此时本来也谈不上时段准确)。
static uint8_t basal_current_hour(void)
{
    uint32_t u = rtc_unix_now();
    if (u == 0) return (uint8_t)((millis() / 3600000UL) % 24UL);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    rtc_unix_to_ymdhms(u, &y, &mo, &d, &h, &mi, &s);
    if (h < 0 || h > 23) h = 0;
    return (uint8_t)h;
}

// 取当前整点应执行的基础率 (U/h)
//
// ⚠️ 2026-08-08 根因修复 —— 用户报告"设置基础率后电机完全不动":
//    旧逻辑在闭环(loop_mode==0)下 base = g_pump_state.current_basal_rate,
//    而 AAPS **从不**直接推送这个字段: 真实 Dana-i 的模型是
//      「泵自己按 24 段档案跑基础率, AAPS 用 0x66 下发档案 + 用 TBR 做增量调节」。
//    于是 AAPS 的 0x66 / 伴生 App 的 SET_PROFILE_SLOT 明明把速率写进了
//    g_pump_config.profiles[...], 闭环却完全不读它 → rate 恒为 0 → 永远不入队
//    MOTOR_CMD_BASAL_TICK → 电机不动。
//    现在闭环与开环统一以「激活方案 × 当前整点」为基础率真源, 与真机行为一致。
static float basal_rate_for_now(void)
{
    // 暂停模式: 0
    if (g_pump_state.loop_mode == 2) return 0.0f;

    // 闭环(0) / 开环(1) 都以当前激活方案的整点段为基础率真源
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;
    uint8_t hour = basal_current_hour();
    float base = g_pump_config.profiles[prof].slots[hour].rate_uh;

    // 伴生 App 通过 BASAL 通道直推的瞬时速率 → 限时覆盖 (超时回落档案)
    if (g_pump_state.basal_override_valid) {
        uint32_t age = (uint32_t)millis() - g_pump_state.basal_override_ms;
        if (age < BASAL_OVERRIDE_TIMEOUT_MS) {
            base = g_pump_state.basal_override_uh;
        } else {
            g_pump_state.basal_override_valid = 0;   // 过期作废, 回落档案
        }
    }

    // 临时基础率优先 (有效期内)
    uint32_t now = millis();
    if (now < g_pump_state.tbr_expiry_ms) {
        base = g_pump_state.tbr_rate;   // tbr_rate 为绝对 U/h (0% low-temp → 0, 闭环低血糖保护生效)
    } else if (now >= g_pump_state.tbr_expiry_ms && g_pump_state.tbr_percent != 0.0f) {
        // 到期, 清除 TBR
        g_pump_state.tbr_percent = 0;
        g_pump_state.tbr_rate    = 0;
        g_pump_state.tbr_expiry_ms = 0;
    }

    if (base < 0.0f) base = 0.0f;
    if (base > MAX_BASAL_RATE) base = MAX_BASAL_RATE;
    return base;
}

// ============================================================
// 方波/双波延展量: 按 duration 时间维铺开 (取代"延展量作一次性第二条大剂量")
// ============================================================

// 已铺开部分记为一次(部分)大剂量事件, 并清除活动标志
static void ext_bolus_log_partial(void)
{
    uint32_t delivered = g_pump_state.ext_bolus_delivered_x100;
    if (delivered > 0) {
        // 延展量取消: 把 IOB 记录裁剪为"实际已投递量", 不夸大未投递部分
        iob_record_extended_cancel(millis(), (float)delivered / 100.0f);
        g_pump_config.total_bolus_count++;
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, delivered,
                          (uint16_t)g_pump_state.ext_bolus_kind);
        dose_log_append(EVENT_TYPE_BOLUS, delivered,
                        (uint16_t)g_pump_state.ext_bolus_kind, DOSE_FLAG_CANCELLED);
        storage_save_config(&g_pump_config);
    }
    g_pump_state.ext_bolus_active = false;
}

// 每个细拍按"已过时间比例"投递一小段, 并按方波速率匀速走丝杠(连续慢滴);
// 时间到收尾。记账(储药器/今日/IOB)在入队时同步进行, 与 motor_deliver_bolus 的
// 分段记账一致, 保证中途取消(或储药器空)只损失未铺开部分。
static void extended_bolus_tick(void)
{
    if (!g_pump_state.ext_bolus_active) return;

    uint32_t now     = millis();
    uint32_t elapsed = (now > g_pump_state.ext_bolus_start_ms)
                           ? (now - g_pump_state.ext_bolus_start_ms) : 0;
    float frac = (g_pump_state.ext_bolus_duration_ms > 0)
                     ? ((float)elapsed / (float)g_pump_state.ext_bolus_duration_ms)
                     : 1.0f;
    if (frac > 1.0f) frac = 1.0f;

    float total     = (float)g_pump_state.ext_bolus_total_x100 / 100.0f;
    float delivered = (float)g_pump_state.ext_bolus_delivered_x100 / 100.0f;
    float target    = total * frac;
    float this_tick = target - delivered;

    // 储药器空 → 中止延展量, 已铺开部分记为部分大剂量事件
    if (g_pump_state.reservoir_units_left < 1) {
        pump_state_set_alarm(ALARM_RESERVOIR_EMPTY);
        ext_bolus_log_partial();
        return;
    }

    // 方波速率 (U/h) → 走丝杠慢速 (steps/s), 使本窗口电机连续运行填满时间,
    // 实现匀速连续慢滴(贴近真实泵 "均匀输注, 步间 ~1s"), 而非大剂量那种分段的梯形曲线。
    float rate_uh     = (g_pump_state.ext_bolus_duration_ms > 0)
                            ? (total / (g_pump_state.ext_bolus_duration_ms / 3600000.0f))
                            : total;   // 退化一次性不会走到这里
    float steps_per_s = (rate_uh / 3600.0f) * (float)STEPS_PER_UNIT;
    uint16_t slow_hz  = (uint16_t)(steps_per_s + 0.5f);
    if (slow_hz < 1) slow_hz = 1;

    // 仅在达到极小下限时才投递, 避免无效微步空转; 远小于 0.1U 网格以保持连续
    if (this_tick >= EXT_BOLUS_MIN_UNITS) {
        // 剂量诚实性原则 (P3-15): 记账用"实际打出量"(吸附整数微步后回读), 不记指令值
        float actual_u;
        quantize_to_actual(this_tick, &actual_u);
        uint32_t ux100 = (uint32_t)(actual_u * 100.0f + 0.5f);
        motor_command_t cmd{};
        cmd.type       = MOTOR_CMD_BOLUS_EXT;
        cmd.units_x100 = ux100;
        cmd.kind       = g_pump_state.ext_bolus_kind;
        cmd.speed_hz   = slow_hz;
        if (motor_enqueue(&cmd)) {
            pump_state_consume_units(actual_u);
            g_pump_state.today_units_x100           += ux100;
            g_pump_state.total_units_x100_delivered += ux100;
            // IOB 由 iob_record_extended_start 统一记录, iob_recompute 衰减 (此处不再累加)
            // 累计「实际投递量」(而非时间目标), 使下一拍 delta 自动对齐时间目标, 避免逐拍舍入漂移
            g_pump_state.ext_bolus_delivered_x100    += ux100;
        }
    }

    // 时间到 → 收尾: 投递剩余零头 + 记一次完整大剂量事件
    if (frac >= 1.0f) {
        float rem = total - (float)g_pump_state.ext_bolus_delivered_x100 / 100.0f;
        if (rem > 0.0005f && g_pump_state.reservoir_units_left >= 1) {
            motor_command_t cmd{};
            cmd.type       = MOTOR_CMD_BOLUS_EXT;
            cmd.units_x100 = (uint32_t)(rem * 100.0f + 0.5f);
            cmd.kind       = g_pump_state.ext_bolus_kind;
            cmd.speed_hz   = slow_hz;
            if (motor_enqueue(&cmd)) {
                pump_state_consume_units(rem);
                uint32_t rx100 = (uint32_t)(rem * 100.0f + 0.5f);
                g_pump_state.today_units_x100           += rx100;
                g_pump_state.total_units_x100_delivered += rx100;
                // IOB 已由 iob_record_extended_start 记录 (延展量记录继续衰减, 此处不再累加)
                g_pump_state.ext_bolus_delivered_x100    = g_pump_state.ext_bolus_total_x100;
            }
        }
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE,
                          g_pump_state.ext_bolus_total_x100,
                          (uint16_t)g_pump_state.ext_bolus_kind);
        dose_log_append(EVENT_TYPE_BOLUS, g_pump_state.ext_bolus_total_x100,
                        (uint16_t)g_pump_state.ext_bolus_kind, 0);
        g_pump_config.total_bolus_count++;
        storage_save_config(&g_pump_config);
        g_pump_state.ext_bolus_active = false;
    }
}

void basal_scheduler_start_extended_bolus(float units, float duration_h, uint8_t kind)
{
    if (units < MIN_DOSE_UNITS) return;
    if (duration_h <= 0.0f) {
        // 无时长 → 退化为一次性大剂量(与原行为一致)
        motor_command_t cmd{};
        cmd.type       = MOTOR_CMD_BOLUS;
        cmd.units_x100 = (uint32_t)(units * 100.0f + 0.5f);
        cmd.kind       = kind;
        motor_enqueue(&cmd);
        return;
    }
    g_pump_state.ext_bolus_active         = true;
    g_pump_state.ext_bolus_kind           = kind;
    g_pump_state.ext_bolus_total_x100     = (uint32_t)(units * 100.0f + 0.5f);
    g_pump_state.ext_bolus_delivered_x100 = 0;
    g_pump_state.ext_bolus_duration_ms    = (uint32_t)(duration_h * 3600000.0f);
    g_pump_state.ext_bolus_start_ms       = millis();
    // IOB: 记录延展量开始, iob_recompute 按线性投递解析积分 (正确反映输注期间爬升+完成后衰减)
    iob_record_extended_start(units, g_pump_state.ext_bolus_duration_ms,
                              g_pump_state.ext_bolus_start_ms);
}

void basal_scheduler_cancel_extended_bolus(void)
{
    if (g_pump_state.ext_bolus_active) ext_bolus_log_partial();
}

bool basal_scheduler_extended_bolus_active(void)
{
    return g_pump_state.ext_bolus_active;
}

void basal_scheduler_init(void)
{
    // 当前基础率先按本地方案初始化, 避免开局显示 0
    g_pump_state.current_basal_rate = basal_rate_for_now();
}

// ============================================================
// #260 基础率验证测试
//   用户原话: "基础率应该设置一个测试按钮, 测试后把所有时段总和加起来执行打一次,
//              历史记录里可以明确看到是否生效, 也可以对照电机的移动距离来判断"
//   设计要点:
//     · 只是"验证档案是否真的被写进泵里", 因此读的必须是 basal_rate_for_now() 的
//       同一个真源 —— g_pump_config.profiles[active].slots[]。若这里算出 0,
//       说明档案根本没落库, 与"电机不动"是同一个病根, 一次测试即可定性。
//     · 走大剂量的物理路径(分段+安全复检+梯形速度), 但**不**计入大剂量次数/IOB,
//       否则会污染 AAPS 的 IOB 计算 → 闭环误判 → 危险。
//     · 历史用独立事件类型 EVENT_TYPE_BASAL_TEST / BH_BASAL_TEST, 与真实治疗剂量
//       在记录上严格区分, 事后审计不会把测试量当成治疗量。
// ============================================================

float basal_scheduler_daily_total(void)
{
    uint8_t prof = g_pump_config.active_profile;
    if (prof >= MAX_BASAL_PROFILES) prof = 0;

    float sum = 0.0f;
    for (int h = 0; h < BASAL_SLOTS_PER_DAY; h++) {
        float r = g_pump_config.profiles[prof].slots[h].rate_uh;
        if (r < 0.0f) r = 0.0f;                    // 脏数据兜底
        if (r > MAX_BASAL_RATE) r = MAX_BASAL_RATE;
        sum += r;   // 每段固定 1 小时 ⇒ 直接累加 U/h 即得当日总 U
    }
    return sum;
}

float basal_scheduler_run_daily_test(void)
{
    // 已有测试在跑 → 不重复入队 (避免连点按钮叠加打药)
    if (g_pump_state.basal_test_running) return 0.0f;

    float total = basal_scheduler_daily_total();
    if (total <= 0.0f) return 0.0f;     // 档案全 0: 正是要暴露的问题, 不打药

    // 单次上限钳制。⚠️ 配置字段为 0(旧 NVS/未初始化)时必须兜底为编译期常量,
    //    否则"安全检查"反而把剂量静默清零 —— 见 MEMORY 里 0x66 的同类教训。
    float cap = g_pump_config.max_bolus_single;
    if (!(cap > 0.0f)) cap = MAX_BOLUS_UNITS;
    if (total > cap) total = cap;

    // 剩余药量钳制
    float left = (float)g_pump_state.reservoir_units_left;
    if (total > left) total = left;

    if (total < MIN_DOSE_UNITS) return 0.0f;

    // 剂量诚实性: 吸附到整数微步, 记账/历史一律用回读的实际量
    float actual_u = 0.0f;
    uint32_t steps = quantize_to_actual(total, &actual_u);
    if (steps == 0 || actual_u < MIN_DOSE_UNITS) return 0.0f;
    uint32_t ux100 = (uint32_t)(actual_u * 100.0f + 0.5f);

    motor_command_t cmd{};
    cmd.type       = MOTOR_CMD_BASAL_TEST;
    cmd.units_x100 = ux100;
    cmd.speed_hz   = BASAL_TEST_SPEED_HZ;
    cmd.rate_uh    = 0.0f;
    cmd.kind       = 0;

    g_pump_state.basal_test_units_x100 = ux100;
    if (!motor_enqueue(&cmd)) {
        g_pump_state.basal_test_units_x100 = 0;
        return 0.0f;                     // 队列满
    }

    // 立即在基础率执行历史上打点: 即使随后电机被安全模块拦下,
    // 也能看到"测试被触发过 + 当时档案总量是多少", 便于定位是"没落库"还是"没走动"。
    uint8_t ap = g_pump_config.active_profile;
    if (ap >= MAX_BASAL_PROFILES) ap = 0;
    basal_history_record(BH_BASAL_TEST, ap, g_pump_state.loop_mode, 0,
                         (uint16_t)(actual_u * 100.0f + 0.5f));

    return actual_u;
}

void basal_scheduler_task(void *arg)
{
    (void)arg;

    // 小数微步累加器 (P3-15): 基础率每窗口应走微步含小数, 直接取整会永久漂移。
    // 这里把小数尾跨窗口累加, 满 1 步才投递, 保证长期速率精确 = rate×时间 (±1 步)。
    static float s_basal_frac_steps = 0.0f;
    // #258 执行留痕用的聚合器 (见下方投递分支注释)
    static uint32_t s_basal_agg_x100      = 0;   // 聚合窗口内累计实际打出量 ×100
    static uint32_t s_basal_agg_since_ms  = 0;   // 聚合窗口起点
    static uint16_t s_last_logged_rate_x100 = 0xFFFF;  // 上次已打点的速率(变化才记)
    // 调度器细拍 = min(延展量窗口, 基础率窗口): 延展量连续慢滴用细拍, 基础率仍按
    // BASAL_TICK_INTERVAL_MS(3 分钟)窗口每 basal_div 个细拍投递一次。
    const uint32_t loop_ms = (EXT_BOLUS_WINDOW_MS > 0 && EXT_BOLUS_WINDOW_MS < BASAL_TICK_INTERVAL_MS)
                                 ? EXT_BOLUS_WINDOW_MS : BASAL_TICK_INTERVAL_MS;
    const uint32_t basal_div = (loop_ms > 0) ? (BASAL_TICK_INTERVAL_MS / loop_ms) : 1;

    uint32_t tick = 0;
    for (;;) {
        float rate = basal_rate_for_now();
        g_pump_state.current_basal_rate = rate;   // 反映给状态屏/蓝牙

        // 基础率: 每 basal_div 个细拍投递一次 3 分钟窗口量。
        // ⚠️ 剂量诚实性原则 (P3-15): 禁止把 0.025U 这类碎量直接取整成 units_x100 发走
        //    (那样会永久 +20% 漂移, 且记账用指令值而非实际打出量)。
        //    改用"小数微步累加器": 每窗口应走微步(含小数)累加, 满 1 步才投递,
        //    记账用 microsteps_to_units(实际步数) —— 长期速率精确 = rate×时间 (±1 步)。
        if (tick % basal_div == 0) {
            float units = rate * (BASAL_TICK_INTERVAL_MS / 3600000.0f);
            if (units > 0.0005f) {
                float steps_f = units * (float)STEPS_PER_UNIT * g_dose_calib_factor;
                s_basal_frac_steps += steps_f;
                uint32_t n_steps = (uint32_t)s_basal_frac_steps;   // floor 整数微步
                if (n_steps > 0) {
                    s_basal_frac_steps -= (float)n_steps;          // 小数尾留待下窗口续投
                    float actual_u = microsteps_to_units(n_steps); // 实际打出的精确量
                    motor_command_t cmd{};
                    cmd.type       = MOTOR_CMD_BASAL_TICK;
                    cmd.units_x100 = (uint32_t)(actual_u * 100.0f + 0.5f);
                    cmd.rate_uh    = rate;
                    if (motor_enqueue(&cmd)) {
                        // 记账用"实际打出量", 与电机真正执行的一致 (绝不记指令值)
                        g_pump_state.today_units_x100           += cmd.units_x100;
                        g_pump_state.total_units_x100_delivered += cmd.units_x100;
                        pump_state_consume_units(actual_u);
                        g_pump_state.last_basal_time = rtc_unix_now();

                        // ---- #258 基础率执行留痕 (用户: "历史里只看得到大剂量") ----
                        // ① dose_log: 每一段微投递都逐条记 (8B/条, 35 万条容量, 可完整审计)
                        dose_log_append(EVENT_TYPE_BASAL_RATE, cmd.units_x100,
                                        (uint16_t)(rate * 100.0f + 0.5f), 0);
                        // ② history_log(32 条环形, 泵屏"历史记录"页): 按 BASAL_HISTORY_AGG_MS
                        //    聚合成一条, 否则 3 分钟一条会把大剂量记录瞬间挤没。
                        s_basal_agg_x100 += cmd.units_x100;
                        uint32_t nowms = (uint32_t)millis();
                        if (s_basal_agg_since_ms == 0) s_basal_agg_since_ms = nowms;
                        if ((uint32_t)(nowms - s_basal_agg_since_ms) >= BASAL_HISTORY_AGG_MS) {
                            history_log_event(EVENT_TYPE_BASAL_RATE, ALARM_NONE,
                                              s_basal_agg_x100,
                                              (uint16_t)(rate * 100.0f + 0.5f));
                            s_basal_agg_x100     = 0;
                            s_basal_agg_since_ms = nowms;
                        }
                        // ③ basal_history: 速率**变化**时打一个点, 让"设置是否生效"
                        //    在时间轴上一眼可见 (同速率由 bh_dup + 本地比较双重去重)
                        uint16_t rx100 = (uint16_t)(rate * 100.0f + 0.5f);
                        if (rx100 != s_last_logged_rate_x100) {
                            uint8_t ap = g_pump_config.active_profile;
                            if (ap >= MAX_BASAL_PROFILES) ap = 0;
                            basal_history_record(BH_BASAL_ACTIVE, ap, g_pump_state.loop_mode,
                                                 (uint16_t)(g_pump_state.tbr_percent * 10.0f),
                                                 rx100);
                            s_last_logged_rate_x100 = rx100;
                        }
                    }
                }
            } else {
                // 速率归零 (暂停 / 该时段档案为 0): 收尾未落账的聚合量并打一个 0 点,
                // 使历史上"什么时候停的输注"同样可见, 而不是悄无声息地断掉。
                if (s_basal_agg_x100 > 0) {
                    history_log_event(EVENT_TYPE_BASAL_RATE, ALARM_NONE, s_basal_agg_x100, 0);
                    s_basal_agg_x100     = 0;
                    s_basal_agg_since_ms = (uint32_t)millis();
                }
                if (s_last_logged_rate_x100 != 0) {
                    uint8_t ap = g_pump_config.active_profile;
                    if (ap >= MAX_BASAL_PROFILES) ap = 0;
                    basal_history_record(BH_BASAL_ACTIVE, ap, g_pump_state.loop_mode, 0, 0);
                    s_last_logged_rate_x100 = 0;
                }
            }
        }

        // ---- 方波/双波延展量: 细拍连续慢滴(每窗口按方波速率匀速走丝杠) ----
        extended_bolus_tick();

        // 周期重算 IOB (按活性曲线衰减), 写 g_pump_state.iob_x10000
        iob_recompute();

        tick++;
        vTaskDelay(pdMS_TO_TICKS(loop_ms));
    }
}
