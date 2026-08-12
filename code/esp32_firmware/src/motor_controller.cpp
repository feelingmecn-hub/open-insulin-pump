/**
 * motor_controller.cpp — DRV8825 步进电机 (Arduino + 硬件定时器 ISR)
 *
 * ★ 全系统唯一的「电机控制入口 ★: 大剂量 / 基础率 / 排气 / JOG / 回退 全部经
 *   execute_command() 分发, 任何模块都不得绕过本文件直接驱动 STEP 引脚。
 *   所有「单位(U)↔微步」换算一律调用 dosing.h 的 units_to_microsteps() 等三函数,
 *   禁止在此处或别处自行拿 STEPS_PER_UNIT 现算。
 */
#include "motor_controller.h"
#include "ina226.h"
#include "pump_state.h"
#include "lcd_display.h"
#include "history_log.h"
#include "dose_log.h"      // 大剂量完成记入剂量追溯日志
#include "storage.h"
#include "rtc_clock.h"     // P1-6 修复: 大剂量完成时写入真实时戳 (rtc_unix_now)
#include "iob_model.h"     // IOB 衰减模型 (替代 iob_x10000 只增不减)

#ifdef USE_AAPS_DANA
#include "aaps_dana.h"     // P1-6/P1-7: 大剂量进度/完成主动推送 (g_dana_fff1 为 null 时自动 no-op)
#endif

#include "esp_task_wdt.h" // P0-1: 看门狗喂狗
#include <driver/gpio.h>
#include <esp32-hal-timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp32-hal-log.h"   // log_i / log_w (调试提示用)

// ---- 全局 ----
QueueHandle_t   g_motor_cmd_queue = nullptr;
SemaphoreHandle_t g_motor_mutex = nullptr;
static TaskHandle_t g_motor_task_handle = nullptr;

static hw_timer_t *g_motor_timer = nullptr;

// 运动状态 (ISR 与任务共享, 用 volatile)
static volatile uint32_t g_step_count   = 0;
static volatile uint32_t g_target_steps = 0;
static volatile uint8_t  g_step_phase   = 0;
static volatile bool     g_motor_running = false;

static uint32_t g_motor_position = 0;   // 当前微步位置
static bool     g_home_set = false;

// 丢步监护
static bool     g_stall_guard_on = false;
static bool     g_step_loss = false;
static bool     g_occlusion = false;   // P0-2: 阻塞(电流过高且应运动)
static uint8_t  g_occ_consec = 0;      // 堵转判定连续采样计数(去抖)
static uint8_t  g_noload_consec = 0;   // 丢步判定连续采样计数(去抖)
static int32_t  g_motor_peak_ma = 0;

// 大剂量分批状态
static volatile bool g_bolus_abort  = false;
static bool          g_bolus_active = false;

// 手动电机控制 (电机测试): 连续点动期间由 STOP 命令/限位触发, 令运动循环立即退出
static volatile bool g_manual_stop  = false;

// 连续点动运行态 (由 motor_task 主循环驱动, 而非在 execute_command 内阻塞):
// active=进行中; dir/speed 为当前方向/速度(可运行中动态改); stop 为停止标志。
static volatile bool      g_manual_jog_active = false;
static volatile motor_dir_t g_manual_jog_dir  = MOTOR_DIR_FORWARD;
static volatile uint16_t  g_manual_jog_speed = MOTOR_MIN_SPEED_HZ;

// ---- 定时器 ISR: 翻转 STEP 计步 ----
static void IRAM_ATTR motor_timer_isr(void)
{
    if (!g_motor_running) return;
    if (g_step_phase == 0) {
        gpio_set_level((gpio_num_t)PIN_MOTOR_STEP, 1);
        g_step_phase = 1;
    } else {
        gpio_set_level((gpio_num_t)PIN_MOTOR_STEP, 0);
        g_step_phase = 0;
        g_step_count++;
        if (g_step_count >= g_target_steps) {
            timerStop(g_motor_timer);   // ESP32 3.x: 停定时器 (替代 timerAlarmDisable)
            g_motor_running = false;
        }
    }
}

// ---- 底层脉冲: 只驱动定时器跑步, 不动 ENABLE (由调用者管理使能/禁用以降低功耗) ----
static bool manual_limit_hit(motor_dir_t dir);   // 前向声明 (定义见下方)
static bool motor_pulse(motor_dir_t dir, uint32_t steps, uint16_t speed_hz)
{
    if (steps == 0) return true;
    if (speed_hz < 1) speed_hz = 1;
    // 正向机械限位/行程硬上限保护: 覆盖大剂量/排气/标定此前缺失的前限位检测。
    // 到达前限位开关, 或以 g_motor_position 计已超储药器满容量行程(限位开关未接时的兜底) → 不动作。
    // 反向(REVERSE)由调用方(motor_rewind_full / JOG)显式检测, 此处不动以免破坏退药归零逻辑。
    if (dir == MOTOR_DIR_FORWARD) {
        if (manual_limit_hit(MOTOR_DIR_FORWARD)) return false;
        if (g_motor_position >= RESERVOIR_MAX_STEPS) return false;
    }

    gpio_set_level((gpio_num_t)PIN_MOTOR_DIR, dir == MOTOR_DIR_FORWARD ? 1 : 0);

    g_step_count = 0;
    g_target_steps = steps;
    g_step_phase = 0;
    g_motor_running = true;
    g_step_loss = false;
    g_occlusion = false;
    g_motor_peak_ma = 0;
    motor_start_stall_guard();

    // 半步间隔 (us): 1 步 = 2 个半步
    uint32_t half_us = 500000UL / speed_hz;
    if (half_us < 20) half_us = 20;
    timerAlarm(g_motor_timer, half_us, true, 0);   // ESP32 3.x: 设定半步定时(自动重载)
    timerStart(g_motor_timer);                      // ESP32 3.x: 启动定时器 (替代 timerAlarmEnable)

    // 等待完成 (运动期间采样电流做监护)
    while (g_motor_running && !g_manual_stop) {
        motor_stall_guard_tick();
        esp_task_wdt_reset();   // P0-1: 步进忙循环内喂狗 (大剂量可能持续 > 看门狗窗口)
        // 堵转/丢步(INA226): 立即停脉冲, 不等整批完成 —— 顶到限位/管路堵时防丝杆顶死损坏。
        // 去抖后已置位, 无论调试/正式构建都停(调试仅不弹 ALARM)。
        if (g_occlusion || g_step_loss) {
            timerStop(g_motor_timer);
            g_motor_running = false;
            break;
        }
        delay(STALL_SAMPLE_MS);
    }
    if (g_manual_stop) {                 // 手动停止: 立即终止脉冲并收回使能控制权
        timerStop(g_motor_timer);
        g_motor_running = false;
    }
    motor_stop_stall_guard();

    if (dir == MOTOR_DIR_FORWARD) g_motor_position += steps;
    else                          g_motor_position -= steps;

    pump_state_update_motor_current((uint16_t)(g_motor_peak_ma < 0 ? 0 : g_motor_peak_ma));
    pump_state_update_motor_current_peak((uint16_t)(g_motor_peak_ma < 0 ? 0 : g_motor_peak_ma));
    pump_state_set_step_loss(g_step_loss);
    g_pump_state.motor_position = g_motor_position;

    // P0-2: 阻塞优先于丢步判定 (电流过高=管路堵/阻力大/顶死限位; 电流过低=未带载/丢步)
    // ⚠️ 堵转/丢步是物理硬故障(含 INA226 判定顶到机械限位), 无论调试/正式构建都必须停止运动,
    //    否则丝杆会顶死损坏。调试构建(-DMOTOR_DEBUG_UNLOCKED)仅不弹 ALARM, 但仍中止运动。
    if (g_occlusion || g_step_loss) {
#ifdef MOTOR_DEBUG_UNLOCKED
        log_i("[MOTOR][DEBUG] 堵转/丢步提示: 峰值电流 %ld mA (阈值 %u mA) occlusion=%d step_loss=%d",
              (long)g_motor_peak_ma, (unsigned)g_pump_config.occlusion_threshold,
              (int)g_occlusion, (int)g_step_loss);
#endif
#ifndef MOTOR_DEBUG_UNLOCKED
        if (g_occlusion) pump_state_set_alarm(ALARM_OCCLUSION);
        if (g_step_loss)  pump_state_set_alarm(ALARM_STEP_LOSS);
#endif
        return false;   // 堵转/丢步 → 中止运动(安全, 调试模式也中止, 仅不报警)
    }
    return true;
}

// ---- 同步运动 (使能→脉冲→禁用), 用于 JOG / PRIME / REWIND ----
static bool motor_move_sync(motor_dir_t dir, uint32_t steps, uint16_t speed_hz)
{
    if (steps == 0) return true;
    if (speed_hz < 1) speed_hz = 1;
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);   // 使能 (低有效)
    bool ok = motor_pulse(dir, steps, speed_hz);
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
    return ok;
}

// ---- 分段大剂量: 每批 0.1U(=MIN_DOSE_UNITS 最小剂量), 段间停顿 + 安全复检, 可中途取消 ----
// 整个大剂量期间保持 ENABLE, 避免段间输液回压把活塞推回 (回灌)。
// 这与真实胰岛素泵的工作方式一致 (大剂量最小增量 0.1U/步, 1s 间隔, ≈3U/min;
// Medtronic 780G: 标准 1.5U/min, 快速 15U/min): 大剂量不是一次连续打完,
// 本地浮点钳制 (避免依赖 Arduino constrain 头文件顺序)
static inline float clamp_f(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// 大剂量「梯形速度曲线」规划: 依据已打进度, 返回下一段的(粒度 units, 步进频率 Hz)。
// 设计目标(用户 2026-08-04): 小剂量精细、大剂量提速、收尾减速+细步保证精度。
static void bolus_plan_segment(float total, float delivered,
                               float *out_seg_units, uint16_t *out_speed_hz)
{
    float remaining = total - delivered;
    if (remaining < 0) remaining = 0;
    float p = (total > 0) ? (delivered / total) : 1.0f;     // 进度 0..1

    // ---- 粒度(安全复检/记账精细度) ----
    float gran;
    if (total <= BOLUS_TIER1_MAX_UNITS)      gran = BOLUS_GRAN_FINE;    // ≤1U 全程最细
    else if (total <= BOLUS_TIER2_MAX_UNITS) gran = BOLUS_GRAN_MID;     // 1~5U
    else                                     gran = BOLUS_GRAN_COARSE;  // >5U
    // 收尾细步: 末段绝对区 或 已进入减速区 → 强制最细, 保证收尾精度
    if (remaining <= BOLUS_TAIL_UNITS || p >= (1.0f - BOLUS_RAMP_DOWN_FRAC))
        gran = BOLUS_GRAN_FINE;

    // ---- 速度曲线(加速 → 匀速 → 减速) ----
    float up   = clamp_f(p / BOLUS_RAMP_UP_FRAC,   0.0f, 1.0f);   // 前段加速到满
    float down = clamp_f((1.0f - p) / BOLUS_RAMP_DOWN_FRAC, 0.0f, 1.0f); // 后段减速到缓
    float ramp = (up < down) ? up : down;
    uint16_t speed = (uint16_t)(BOLUS_SLOW_HZ + (BOLUS_FAST_HZ - BOLUS_SLOW_HZ) * ramp);

    *out_seg_units = gran;
    *out_speed_hz  = speed;
}

// 而是小步推进 + 段间监测, 既能及时发现阻塞, 又能在用户取消时只损失已打部分。
// as_basal_test=true: 这是"基础率验证测试注射"(把 24 段总量一次性打出)。
//   物理动作与安全复检完全等同大剂量, 只是**记账口径不同**:
//   记 EVENT_TYPE_BASAL_TEST, 不 ++total_bolus_count, 不进 IOB (它不是治疗剂量,
//   而是为了让用户拿卡尺量行程/看历史来验证基础率设置确实驱动了电机)。
static void motor_deliver_bolus(float total_units, uint8_t kind, bool as_basal_test = false)
{
    uint32_t total_steps = units_to_microsteps(total_units);
    if (total_steps == 0) return;

    // 大剂量按「梯形速度曲线」分段推进: 粒度/步进频率随进度动态调整
    // (起步缓→中段快→收尾减速+细步), 兼顾提速与收尾精度。详见 bolus_plan_segment()。
    // 调试/电机测试构建(MOTOR_DEBUG_UNLOCKED)仅去掉段间停顿(连续推注), 曲线本身不变。
#ifdef MOTOR_DEBUG_UNLOCKED
    const uint32_t seg_interval_ms = 0;
#else
    const uint32_t seg_interval_ms = BOLUS_SEGMENT_INTERVAL_MS;
#endif

    g_bolus_active = true;
    g_bolus_abort  = false;
    g_pump_state.current_state = (uint8_t)PUMP_STATE_BOLUS;
    // P1-6: 大剂量进度/已输注量归零 (供首页显示 + AAPS 0x40 查询/通知)
    g_pump_state.bolus_progress_pct   = 0;
    g_pump_state.bolus_delivered_x100 = 0;

    // 整个大剂量保持电机使能, 防止段间停顿时的输液回压导致回灌
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);

    uint32_t delivered_steps = 0;
    bool     aborted = false;

    while (delivered_steps < total_steps && !g_bolus_abort) {
        // ---- 段间安全复检 (真实硬故障/报警/用户取消/储药器空 → 中止剩余) ----
#ifdef MOTOR_DEBUG_UNLOCKED
        // 调试模式(先调电机): 阻塞/丢步/硬故障/储药器 均不中止大剂量,
        //   仅响应"用户主动取消", 电机可反复无条件执行。正式版(不带宏)恢复全部安全中断。
        if (g_bolus_abort) { aborted = true; break; }
#else
        if (g_pump_state.alarm_code != ALARM_NONE) { aborted = true; break; }
        if (g_occlusion)                           { pump_state_set_alarm(ALARM_OCCLUSION); aborted = true; break; }
        if (g_step_loss)                           { pump_state_set_alarm(ALARM_STEP_LOSS); aborted = true; break; }
        if (g_pump_state.reservoir_units_left < 1) { pump_state_set_alarm(ALARM_RESERVOIR_EMPTY); aborted = true; break; }
#endif

        uint32_t  remaining   = total_steps - delivered_steps;
        float     delivered_u = microsteps_to_units(delivered_steps);
        float     seg_units, this_units;
        uint16_t  seg_speed_hz;
        bolus_plan_segment(total_units, delivered_u, &seg_units, &seg_speed_hz);

        uint32_t seg_steps = units_to_microsteps(seg_units);
        if (seg_steps == 0) seg_steps = 1;   // 兜底, 避免死循环
        uint32_t this_steps = (remaining < seg_steps) ? remaining : seg_steps;
        this_units = microsteps_to_units(this_steps);
        uint32_t ux100      = (uint32_t)(this_units * 100.0f + 0.5f);

        bool ok = motor_pulse(MOTOR_DIR_FORWARD, this_steps, seg_speed_hz);
        if (!ok) { aborted = true; break; }   // 丢步/堵转已在 pulse 内置报警

        delivered_steps += this_steps;

        // P1-6: 更新进度 + 已输注量 (供首页显示 + AAPS 0x40 / DELIVERY_RATE 通知)
        //  ★ 2026-08-11 修复：改用「真实已打微步数」反算，杜绝逐段四舍五入累加漂移。
        //    旧代码 += ux100(每段 round(this_units*100)) 在 ≈20 段后漂移达 0.08U，
        //    导致 AAPS 经 0x40 回读/完成通知得到 9.92U≠请求量→判"输注量不符"报错。
        g_pump_state.bolus_delivered_x100 = (uint32_t)(microsteps_to_units(delivered_steps) * 100.0f + 0.5f);
        g_pump_state.bolus_progress_pct =
            (uint8_t)((uint64_t)delivered_steps * 100u / total_steps);
#ifdef USE_AAPS_DANA
        aaps_notify_bolus_progress((uint16_t)(g_pump_state.bolus_delivered_x100 & 0xFFFF));
#endif

        // 按实际打入量逐段记账 (取消时只记已打部分, 防止高估已注)
        pump_state_consume_units(this_units);
        g_pump_state.today_units_x100           += ux100;
        g_pump_state.total_units_x100_delivered += ux100;
        // 注: IOB 不再此处累加, 由 iob_record_bolus 在整笔完成后统一记录,
        //     并由 iob_recompute 按活性曲线衰减 (见 basal_scheduler_task)。

        // 段间停顿 (调试构建=0 连续推注; 生产=100ms 复检窗口, 仍可取消/报警打断);
        // 节奏已由 config.h 提速, 大剂量整笔约数秒~十余秒完成。
        if (delivered_steps < total_steps && !g_bolus_abort) {
            vTaskDelay(pdMS_TO_TICKS(seg_interval_ms));
        }
    }

    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
    g_bolus_active = false;

    // ---- 大剂量完成/中止: 记录历史 + 持久化累计统计 ----
    float actual = microsteps_to_units(delivered_steps);
    if (actual > 0.001f) {
        // ★ 2026-08-11 修复：完成时把"已输注"对齐到请求量。
        //   微步量化误差 <0.1U（10U→1258 步=9.99U），真实 Dana-i 成功完成也按请求量回报；
        //   若对齐则不报"输注量不符"。中断(aborted)则回报真实已打部分，绝不虚报。
        if (!aborted)
            g_pump_state.bolus_delivered_x100 = (uint32_t)(total_units * 100.0f + 0.5f);
        else
            g_pump_state.bolus_delivered_x100 = (uint32_t)(actual * 100.0f + 0.5f);
        uint32_t ax100 = (uint32_t)(actual * 100.0f + 0.5f);
        if (as_basal_test) {
            // 基础率验证测试: 单独记一条, 明确写入"实际打出量 + 步数", 便于对照卡尺行程。
            //   param2 复用为"实际微步数低 16 位", 1 微步 ≈ 0.00795U (STEPS_PER_UNIT=125.84)
            g_pump_state.bolus_progress_pct = 100;
            history_log_event(EVENT_TYPE_BASAL_TEST, ALARM_NONE, ax100,
                              (uint16_t)(delivered_steps & 0xFFFFu));
            dose_log_append(EVENT_TYPE_BASAL_TEST, ax100,
                            (uint16_t)(delivered_steps & 0xFFFFu), 0);
            g_pump_state.basal_test_running = 0;
            Serial.printf("[BASAL-TEST] 目标 %.2fU → 实打 %.2fU / %u 微步\n",
                          total_units, actual, (unsigned)delivered_steps);
        } else {
            // P1-6: 进度置 100, 并向 AAPS 推送"大剂量完成"通知 (命令阶段走 BLE5)
            g_pump_state.bolus_progress_pct = 100;
#ifdef USE_AAPS_DANA
            aaps_notify_bolus_complete();
#endif
            iob_record_bolus(actual);   // 整笔完成后记录一笔, 交给 iob_recompute 衰减
            g_pump_state.last_bolus_time = rtc_unix_now();   // ★ 真实大剂量完成时刻(UTC秒), 供 0x40 回读/AAPS 用
            /* ★ 2026-08-11: 记 BOLUS 历史, 供 AAPS 经 0xC2 回放后写治疗账本
             *   (AAPS 仅在读到 BOLUS 记录时才 syncBolusWithPumpId → 否则治疗页永远空)。
             *   回报量用最终 bolus_delivered_x100(完成对齐请求量/中断报真实量),
             *   与 AAPS 经 0x40 回读/完成通知所见一致, 便于按量匹配 bolusType。 */
#ifdef USE_AAPS_DANA
            aaps_dana_record_bolus(g_pump_state.last_bolus_time,
                                   (uint16_t)(g_pump_state.bolus_delivered_x100 & 0xFFFF));
#endif
            g_pump_config.total_bolus_count++;
            history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE, ax100, (uint16_t)kind);
            dose_log_append(EVENT_TYPE_BOLUS, ax100, (uint16_t)kind, 0);
        }
        storage_save_config(&g_pump_config);
    } else if (as_basal_test) {
        g_pump_state.basal_test_running = 0;
    }

    if (g_pump_state.current_state == (uint8_t)PUMP_STATE_BOLUS) {
        g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
    }
}

// ---- 限位保护: 本项目无硬件限位开关, 限位由 INA226 堵转电流判定 ----
// 电机顶到机械限位(前/后)或管路堵塞 → 电流持续超阈值 → g_occlusion 置位(见 motor_stall_guard_tick)。
// 故"是否到限位"= g_occlusion (INA226 已去抖, 方向无关: 两端顶死都是堵转)。
static bool manual_limit_hit(motor_dir_t)
{
    return g_occlusion;
}

// 全退到电机尾部(回退装药): 反向连续走, 直到后限位开关命中或达安全步数上限。
// 不依赖 g_motor_position 记账(用户明确要求"不根据打了多少药计算退多少距离")。
// 命中后限位即视为活塞退到尾部(装药原点), 完成后将位置记为 0。
static bool motor_rewind_full(uint16_t speed_hz)
{
    if (speed_hz < 1) speed_hz = 1;
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);   // 使能 (低有效)
    uint32_t total = 0;
    bool hit_limit = false;
    while (total < REWIND_MAX_STEPS) {
        if (manual_limit_hit(MOTOR_DIR_REVERSE)) { hit_limit = true; break; }
        // 报警/硬故障: 正式版遇任何报警中途停止; 调试版(仅电机调测)可忽略业务报警继续,
        //   但仍须响应"堵转/丢步"——这是物理顶死信号, 无论如何都要停止硬推, 否则丝杆卡死损坏。
#ifndef MOTOR_DEBUG_UNLOCKED
        if (g_pump_state.alarm_code != ALARM_NONE) break;
#endif
        if (g_occlusion) {
#ifndef MOTOR_DEBUG_UNLOCKED
            pump_state_set_alarm(ALARM_OCCLUSION);
#endif
            break;   // 堵转(顶死) → 永远停止物理运动(调试模式仅提示不报警)
        }
        if (g_step_loss) {
#ifndef MOTOR_DEBUG_UNLOCKED
            pump_state_set_alarm(ALARM_STEP_LOSS);
#endif
            break;
        }
        bool ok = motor_pulse(MOTOR_DIR_REVERSE, REWIND_CHUNK_STEPS, speed_hz);
        if (!ok) break;   // 限位/步数上限触发 → 退出
        total += REWIND_CHUNK_STEPS;
        esp_task_wdt_reset();
    }
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
    if (hit_limit) {
        // 命中后限位 = 活塞退到尾部 = 装药原点
        g_home_set = true;
        g_motor_position = 0;
        g_pump_state.motor_position = 0;
    }
    return hit_limit;
}

// ---- 执行命令 ----
static void execute_command(const motor_command_t *cmd)
{
    uint32_t steps = cmd->steps;
    uint16_t speed = cmd->speed_hz ? cmd->speed_hz : MOTOR_MAX_SPEED_HZ;

    // 进入新命令: 若正在进行连续点动, 且本次不是"启动连续点动", 先终止之,
    // 避免连续点动与本次步进指令并发驱动同一定时器造成竞态/状态混乱。
    if (g_manual_jog_active && !(cmd->type == MOTOR_CMD_MANUAL && cmd->steps == 0)) {
        g_manual_jog_active = false;
        g_manual_stop = false;
        gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用
    }
    // 清除"手动停止"标志, 避免残留标志误伤后续(大剂量/排气等)运动。
    g_manual_stop = false;

    switch (cmd->type) {
        case MOTOR_CMD_BOLUS:
            // 唯一换算入口: 单位(U) → 微步, 再由 motor_deliver_bolus 分段打入
            motor_deliver_bolus(cmd->units_x100 / 100.0f, cmd->kind);
            break;
        case MOTOR_CMD_BASAL_TEST:
            // 基础率验证测试: 与大剂量同一物理路径(分段/梯形曲线/段间安全复检),
            // 仅记账口径不同 (见 motor_deliver_bolus 的 as_basal_test 说明)
            g_pump_state.basal_test_running = 1;
            motor_deliver_bolus(cmd->units_x100 / 100.0f, cmd->kind, /*as_basal_test=*/true);
            break;
        case MOTOR_CMD_BASAL_TICK:
            // 基础率每 BASAL_TICK_INTERVAL_MS 推注的小步: 同样经唯一换算入口,
            // 不能再用 cmd->steps (调度器只填 units_x100, steps 恒为 0 → 原 bug 不动电机)
            motor_move_sync(MOTOR_DIR_FORWARD, units_to_microsteps(cmd->units_x100 / 100.0f), speed);
            break;
        case MOTOR_CMD_BOLUS_EXT:
            // 方波/双波延展量的一次性微投递: 同样经唯一换算入口走丝杠。
            // 记账(储药器/今日/IOB)由 basal_scheduler 在入队时同步完成, 此处仅物理推注。
            motor_move_sync(MOTOR_DIR_FORWARD, units_to_microsteps(cmd->units_x100 / 100.0f), speed);
            break;
        case MOTOR_CMD_PRIME:
            // 按排气体积(units_x100 U)换算步数驱动丝杠; 无体积参数时沿用默认 2000 步
            {
                uint32_t prime_steps = (cmd->units_x100 > 0)
                    ? units_to_microsteps(cmd->units_x100 / 100.0f)
                    : 2000;
                motor_move_sync(MOTOR_DIR_FORWARD, prime_steps, speed);
            }
            g_pump_state.is_primed = true;
            break;
        case MOTOR_CMD_REWIND: {
            // units_x100==0 → 全退到尾部(后限位), 不依赖已打药量;
            // units_x100>0  → 按指定 U 退(用户手动判断退多少)。
            uint16_t rw_speed = cmd->speed_hz ? cmd->speed_hz : REWIND_SPEED_HZ;
            if (cmd->units_x100 == 0) {
                motor_rewind_full(rw_speed);
            } else {
                uint32_t rw_steps = units_to_microsteps(cmd->units_x100 / 100.0f);
                if (rw_steps > 0) motor_move_sync(MOTOR_DIR_REVERSE, rw_steps, rw_speed);
            }
            break;
        }
        case MOTOR_CMD_CALIBRATE:
            // P3-14: 剂量标定 — 推出已知指令体积(默认 1.0U)的测试量, 供用户实测后计算标定系数。
            // 物理推注同样经唯一换算入口; 实测体积由 UI 经 ui_hal_calibrate_dispense 下发。
            motor_move_sync(MOTOR_DIR_FORWARD, units_to_microsteps(cmd->units_x100 / 100.0f), speed);
            break;
        case MOTOR_CMD_STOP:
            timerStop(g_motor_timer);   // ESP32 3.x: 停定时器 (替代 timerAlarmDisable)
            g_motor_running = false;
            g_manual_stop = true;       // 同时终止正在进行的连续点动
            gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);
            break;
        case MOTOR_CMD_MANUAL: {
            // 完全手动电机控制 (电机测试): 前进/后退定量步数, 或 steps=0 连续点动直到 STOP/限位。
            motor_dir_t dir = (cmd->dir == MOTOR_DIR_REVERSE) ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
            uint16_t sp = cmd->speed_hz ? cmd->speed_hz : MOTOR_MIN_SPEED_HZ;
            if (steps == 0) {
                // 连续点动: 仅置运行态, execute_command 立即返回; 实际步进由 motor_task
                // 主循环以小批驱动(每批轮询 g_manual_stop/限位), 故 STOP 即时响应, 且电机任务
                // 不被独占(可继续收命令/喂看门狗/运行中改方向速度), 彻底消除"连续点动卡死电机任务"的死锁。
                g_manual_jog_dir   = dir;
                g_manual_jog_speed = sp;
                g_manual_stop      = false;
                if (!g_manual_jog_active) {
                    g_manual_jog_active = true;
                    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);   // 使能 (结束时由主循环禁用)
                }
                // 运行中再次收到 steps=0: 直接改方向/速度即可(主循环读全局), 无需停止重启。
            } else {
                // 定量: 限位命中则不动作; 同步执行 (有界时长, 立即返回)
                g_manual_stop = false;
                gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);   // 使能
                if (!manual_limit_hit(dir)) motor_pulse(dir, steps, sp);
                gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
            }
            break;
        }
        case MOTOR_CMD_CANCEL_BOLUS:
            g_bolus_abort = true;
            break;
        default:
            break;
    }
}

// ---- 公共 API ----
void motor_init(void)
{
    gpio_reset_pin((gpio_num_t)PIN_MOTOR_STEP);
    gpio_reset_pin((gpio_num_t)PIN_MOTOR_DIR);
    gpio_reset_pin((gpio_num_t)PIN_MOTOR_ENABLE);
    gpio_reset_pin((gpio_num_t)PIN_MOTOR_nFAULT);
    gpio_set_direction((gpio_num_t)PIN_MOTOR_STEP,    GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_MOTOR_DIR,     GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_MOTOR_ENABLE,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_MOTOR_nFAULT,  GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_MOTOR_nFAULT, GPIO_PULLUP_ONLY);
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 默认禁用

    // 限位开关: 本项目未焊接硬件限位开关(GPIO2/3 为 ESP32-C6 USB-D+/D-, 被 USB-CDC 占用,
    //   不可作限位输入)。限位判定改由 INA226 堵转电流检测(g_occlusion), 见 motor_stall_guard_tick。
    //   故此处不再配置 GPIO 限位引脚, 避免把 USB 差分脚当 GPIO 读(读到垃圾值)。

    g_motor_timer = timerBegin(1000000);   // 1 MHz (ESP32 3.x: 参数为计数频率)
    timerAttachInterrupt(g_motor_timer, &motor_timer_isr);   // ESP32 3.x: 2 参, 无 edge 标志

    g_motor_cmd_queue = xQueueCreate(8, sizeof(motor_command_t));
    g_motor_mutex = xSemaphoreCreateMutex();
}

void motor_task(void *arg)
{
    motor_command_t cmd;
    for (;;) {
        esp_task_wdt_reset();   // P0-1: 喂狗 (空闲/连续点动均定期喂)

        // ---- 连续点动由本主循环驱动: 小批步进 + 轮询停止/限位, 不阻塞命令处理 ----
        if (g_manual_jog_active) {
            // 优先处理点动期间到达的命令(如改方向/速度、或停止), 避免堆积
            if (xQueueReceive(g_motor_cmd_queue, &cmd, 0) == pdTRUE) {
                xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
                g_pump_state.current_state = (uint8_t)PUMP_STATE_DELIVERING;
                execute_command(&cmd);   // 内部会对非"启动连续点动"命令先停掉当前 jog
                if (g_pump_state.current_state == (uint8_t)PUMP_STATE_DELIVERING)
                    g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
                xSemaphoreGive(g_motor_mutex);
                continue;
            }
            // 无新命令: 继续本批步进 (motor_pulse 内置每 5ms 轮询 g_manual_stop, STOP 即时响应)
            if (g_manual_stop || manual_limit_hit(g_manual_jog_dir)) {
                g_manual_jog_active = false;            // 退出连续点动
                g_manual_stop      = false;
                gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
            } else {
                // 取互斥锁后再步进, 与命令分支/其它任务(motor_jog)串行, 避免并发驱动定时器竞态
                xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
                bool ok = motor_pulse(g_manual_jog_dir, 200, g_manual_jog_speed);
                xSemaphoreGive(g_motor_mutex);
                esp_task_wdt_reset();
                // 生产固件: 阻塞/丢步返回 false → 终止连续点动 (调试模式恒返回 true, 仅提示);
                // 与旧实现一致, 避免对着堵塞管路硬推。
                if (!ok) {
                    g_manual_jog_active = false;
                    g_manual_stop      = false;
                    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);
                }
            }
            continue;   // 不阻塞在队列上, 直接进入下一轮(优先继续点动/复检)
        }

        // 队列等待改为"有界等待"(= 看门狗窗口一半), 避免空闲阻塞导致误复位
        if (xQueueReceive(g_motor_cmd_queue, &cmd,
                          pdMS_TO_TICKS(WATCHDOG_TIMEOUT_S * 500UL)) == pdTRUE) {
            xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
            g_pump_state.current_state = (uint8_t)PUMP_STATE_DELIVERING;
            execute_command(&cmd);
            if (g_pump_state.current_state == (uint8_t)PUMP_STATE_DELIVERING) {
                g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
            }
            xSemaphoreGive(g_motor_mutex);
        }
    }
}

bool motor_jog(motor_dir_t dir, uint16_t steps)
{
    if (steps == 0) return false;
    if (g_motor_mutex == nullptr) return false;
    xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
    bool ok = motor_move_sync(dir, steps, MOTOR_MIN_SPEED_HZ);
    xSemaphoreGive(g_motor_mutex);
    return ok;
}

void motor_set_home(void)
{
    if (g_motor_mutex) xSemaphoreTake(g_motor_mutex, portMAX_DELAY);
    g_home_set = true;
    g_motor_position = 0;
    g_pump_state.motor_position = 0;
    if (g_motor_mutex) xSemaphoreGive(g_motor_mutex);
}

bool motor_enqueue(const motor_command_t *cmd)
{
    if (!g_motor_cmd_queue) return false;
    return xQueueSend(g_motor_cmd_queue, cmd, 0) == pdTRUE;
}

void motor_cancel_bolus(void)
{
    if (g_bolus_active) g_bolus_abort = true;
}

bool motor_bolus_active(void)
{
    return g_bolus_active;
}

void motor_manual_stop(void)
{
    // BLE CONTROL 0x16: 立即令任何进行中的手动点动(含连续点动)退出运动循环。
    // 不依赖命令队列(连续点动会独占队列), 直接置标志; 运动循环每 chunk 轮询此标志。
    g_manual_stop = true;
}

void motor_start_stall_guard(void)
{
    g_stall_guard_on = true;
    g_motor_peak_ma = 0;
    g_occlusion = false;
    g_occ_consec = 0;
    g_noload_consec = 0;
}

void motor_stall_guard_tick(void)
{
    if (!g_stall_guard_on) return;
#ifdef MOTOR_DEBUG_UNLOCKED
    // 测试阶段 INA226 可能未焊接/未接: 无电流采样数据时不判堵转/丢步, 也不置
    // step_loss/occlusion, 否则电机将永远被中止、且 LCD 电流被 0 误导。
    // 接上芯片(init 自检通过 -> g_ina226_online=true)后自动恢复完整保护。
    if (!g_ina226_online) {
        g_occlusion = false;
        g_step_loss = false;
        return;
    }
#endif
    int32_t cur = ina226_read_current_ma();
    if (cur > g_motor_peak_ma) g_motor_peak_ma = cur;
    pump_state_update_motor_current((uint16_t)(cur < 0 ? 0 : cur));   // 实时电流(供 LCD/调试观察)
    // P0-2: 电流过低=未带载/丢步; 电流过高(超阈值)=管路阻塞/堵转/顶死限位。
    // 无压力传感器, 电流法为"尽力而为"的阻塞检测(阈值来自 config.occlusion_threshold)。
    // 连续 STALL_*_CONSEC 个采样超阈值才置位, 滤除启动浪涌与瞬态尖峰, 避免误中止运动。
    if (cur > (int32_t)g_pump_config.occlusion_threshold) {
        if (++g_occ_consec >= STALL_OCCL_CONSEC) g_occlusion = true;
    } else {
        g_occ_consec = 0;
    }
    if (cur < STALL_NOLOAD_MA && g_step_count > 50) {
        if (++g_noload_consec >= STALL_NOLOAD_CONSEC) g_step_loss = true;
    } else {
        g_noload_consec = 0;
    }
}

void motor_stop_stall_guard(void)
{
    g_stall_guard_on = false;
}

bool motor_step_loss_detected(void)
{
    return g_step_loss;
}
