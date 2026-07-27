/**
 * motor_controller.cpp — DRV8825 步进电机 (Arduino + 硬件定时器 ISR)
 */
#include "motor_controller.h"
#include "ina226.h"
#include "pump_state.h"
#include "lcd_display.h"
#include "history_log.h"
#include "storage.h"

#include <driver/gpio.h>
#include <esp32-hal-timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
static int32_t  g_motor_peak_ma = 0;

// 大剂量分批状态
static volatile bool g_bolus_abort  = false;
static bool          g_bolus_active = false;

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
            timerAlarmDisable(g_motor_timer);
            g_motor_running = false;
        }
    }
}

// ---- 底层脉冲: 只驱动定时器跑步, 不动 ENABLE (由调用者管理使能/禁用以降低功耗) ----
static bool motor_pulse(motor_dir_t dir, uint32_t steps, uint16_t speed_hz)
{
    if (steps == 0) return true;
    if (speed_hz < 1) speed_hz = 1;

    gpio_set_level((gpio_num_t)PIN_MOTOR_DIR, dir == MOTOR_DIR_FORWARD ? 1 : 0);

    g_step_count = 0;
    g_target_steps = steps;
    g_step_phase = 0;
    g_motor_running = true;
    g_step_loss = false;
    g_motor_peak_ma = 0;
    motor_start_stall_guard();

    // 半步间隔 (us): 1 步 = 2 个半步
    uint32_t half_us = 500000UL / speed_hz;
    if (half_us < 20) half_us = 20;
    timerAlarmWrite(g_motor_timer, half_us, true);
    timerAlarmEnable(g_motor_timer);

    // 等待完成 (运动期间采样电流做监护)
    while (g_motor_running) {
        motor_stall_guard_tick();
        delay(STALL_SAMPLE_MS);
    }
    motor_stop_stall_guard();

    if (dir == MOTOR_DIR_FORWARD) g_motor_position += steps;
    else                          g_motor_position -= steps;

    pump_state_update_motor_current((uint16_t)g_motor_peak_ma);
    pump_state_set_step_loss(g_step_loss);
    g_pump_state.motor_position = g_motor_position;

    if (g_step_loss) {
        pump_state_set_alarm(ALARM_STEP_LOSS);
        return false;
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

// ---- 分段大剂量: 每批 0.05U, 段间停顿 + 安全复检, 可中途取消 ----
// 整个大剂量期间保持 ENABLE, 避免段间输液回压把活塞推回 (回灌)。
// 这与真实胰岛素泵的工作方式一致 (Wellion: 0.05U/步, 1s 间隔, ≈3U/min;
// Medtronic 780G: 标准 1.5U/min, 快速 15U/min): 大剂量不是一次连续打完,
// 而是小步推进 + 段间监测, 既能及时发现阻塞, 又能在用户取消时只损失已打部分。
static void motor_deliver_bolus(float total_units, uint8_t kind)
{
    uint32_t total_steps = units_to_microsteps(total_units);
    if (total_steps == 0) return;

    uint32_t seg_steps = units_to_microsteps(BOLUS_SEGMENT_UNITS);
    if (seg_steps == 0) seg_steps = 1;   // 兜底, 避免死循环

    g_bolus_active = true;
    g_bolus_abort  = false;
    g_pump_state.current_state = (uint8_t)PUMP_STATE_BOLUS;

    // 整个大剂量保持电机使能, 防止段间停顿时的输液回压导致回灌
    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);

    uint32_t delivered_steps = 0;
    bool     aborted = false;

    while (delivered_steps < total_steps && !g_bolus_abort) {
        // ---- 段间安全复检 (阻塞/报警/储药器空 → 立即中止剩余) ----
        if (g_pump_state.alarm_code != ALARM_NONE) { aborted = true; break; }
        if (g_step_loss)                           { pump_state_set_alarm(ALARM_STEP_LOSS); aborted = true; break; }
        if (g_pump_state.reservoir_units_left < 1) { pump_state_set_alarm(ALARM_RESERVOIR_EMPTY); aborted = true; break; }

        uint32_t remaining  = total_steps - delivered_steps;
        uint32_t this_steps = (remaining < seg_steps) ? remaining : seg_steps;
        float    this_units = microsteps_to_units(this_steps);

        bool ok = motor_pulse(MOTOR_DIR_FORWARD, this_steps, BOLUS_SPEED_HZ);
        if (!ok) { aborted = true; break; }   // 丢步/堵转已在 pulse 内置报警

        delivered_steps += this_steps;

        // 按实际打入量逐段记账 (取消时只记已打部分, 防止高估已注)
        pump_state_consume_units(this_units);
        uint32_t ux100 = (uint32_t)(this_units * 100.0f + 0.5f);
        g_pump_state.today_units_x100           += ux100;
        g_pump_state.total_units_x100_delivered += ux100;
        g_pump_state.iob_x10000                 += (uint32_t)(this_units * 10000.0f);

        // 段间停顿 (0.05U / 1s ≈ 3U/min, 贴合真实泵); 期间可被取消/报警打断
        if (delivered_steps < total_steps && !g_bolus_abort) {
            vTaskDelay(pdMS_TO_TICKS(BOLUS_SEGMENT_INTERVAL_MS));
        }
    }

    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)
    g_bolus_active = false;

    // ---- 大剂量完成/中止: 记录历史 + 持久化累计统计 ----
    float actual = microsteps_to_units(delivered_steps);
    if (actual > 0.001f) {
        g_pump_config.total_bolus_count++;
        history_log_event(EVENT_TYPE_BOLUS, ALARM_NONE,
                          (uint32_t)(actual * 100.0f + 0.5f), (uint16_t)kind);
        storage_save_config(&g_pump_config);
    }

    if (g_pump_state.current_state == (uint8_t)PUMP_STATE_BOLUS) {
        g_pump_state.current_state = (uint8_t)PUMP_STATE_IDLE;
    }
}

// ---- 执行命令 ----
static void execute_command(const motor_command_t *cmd)
{
    uint32_t steps = cmd->steps;
    uint16_t speed = cmd->speed_hz ? cmd->speed_hz : MOTOR_MAX_SPEED_HZ;

    switch (cmd->type) {
        case MOTOR_CMD_BOLUS:
            // 唯一换算入口: 单位(U) → 微步, 再由 motor_deliver_bolus 分段打入
            motor_deliver_bolus(cmd->units_x100 / 100.0f, cmd->kind);
            break;
        case MOTOR_CMD_BASAL_TICK:
            // 基础率每 BASAL_TICK_INTERVAL_MS 推注的小步: 同样经唯一换算入口,
            // 不能再用 cmd->steps (调度器只填 units_x100, steps 恒为 0 → 原 bug 不动电机)
            motor_move_sync(MOTOR_DIR_FORWARD, units_to_microsteps(cmd->units_x100 / 100.0f), speed);
            break;
        case MOTOR_CMD_PRIME:
            motor_move_sync(MOTOR_DIR_FORWARD, steps ? steps : 2000, speed);
            g_pump_state.is_primed = true;
            break;
        case MOTOR_CMD_REWIND:
            motor_move_sync(MOTOR_DIR_REVERSE, steps ? steps : g_motor_position, speed);
            break;
        case MOTOR_CMD_STOP:
            timerAlarmDisable(g_motor_timer);
            g_motor_running = false;
            gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);
            break;
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

    g_motor_timer = timerBegin(1000000);   // 1 MHz
    timerAttachInterrupt(g_motor_timer, &motor_timer_isr, true);

    g_motor_cmd_queue = xQueueCreate(8, sizeof(motor_command_t));
    g_motor_mutex = xSemaphoreCreateMutex();
}

void motor_task(void *arg)
{
    motor_command_t cmd;
    for (;;) {
        if (xQueueReceive(g_motor_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
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

void motor_start_stall_guard(void)
{
    g_stall_guard_on = true;
    g_motor_peak_ma = 0;
}

void motor_stall_guard_tick(void)
{
    if (!g_stall_guard_on) return;
    int32_t cur = ina226_read_current_ma();
    if (cur > g_motor_peak_ma) g_motor_peak_ma = cur;
    // 电流过低=未带载/丢步; 过高=堵转 (仅在明显偏离时判定)
    if (cur < STALL_NOLOAD_MA && g_step_count > 50) g_step_loss = true;
    if (cur > STALL_OVERLOAD_MA)                    g_step_loss = true;
}

void motor_stop_stall_guard(void)
{
    g_stall_guard_on = false;
}

bool motor_step_loss_detected(void)
{
    return g_step_loss;
}
