/**
 * motor_controller.cpp — DRV8825 步进电机 (Arduino + 硬件定时器 ISR)
 */
#include "motor_controller.h"
#include "ina226.h"
#include "pump_state.h"
#include "lcd_display.h"

#include <driver/gpio.h>
#include <esp32-hal-timer.h>

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

// ---- 同步运动 (调用者须持 g_motor_mutex) ----
static bool motor_move_sync(motor_dir_t dir, uint32_t steps, uint16_t speed_hz)
{
    if (steps == 0) return true;
    if (speed_hz < 1) speed_hz = 1;

    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 0);   // 使能 (低有效)
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

    gpio_set_level((gpio_num_t)PIN_MOTOR_ENABLE, 1);   // 禁用 (省电)

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

// ---- 执行命令 ----
static void execute_command(const motor_command_t *cmd)
{
    uint32_t steps = cmd->steps;
    uint16_t speed = cmd->speed_hz ? cmd->speed_hz : MOTOR_MAX_SPEED_HZ;

    switch (cmd->type) {
        case MOTOR_CMD_BOLUS: {
            float units = cmd->units_x100 / 100.0f;
            steps = (uint32_t)(units * STEPS_PER_UNIT);
            motor_move_sync(MOTOR_DIR_FORWARD, steps, speed);
            g_pump_state.reservoir_units_left =
                (uint16_t)(g_pump_state.reservoir_units_left > (units + 0.005f)
                           ? g_pump_state.reservoir_units_left - (uint16_t)(units * 100) / 100.0f
                           : 0);
            break;
        }
        case MOTOR_CMD_BASAL_TICK:
            // 基础率每 BASAL_TICK_INTERVAL_MS 推注的小步
            motor_move_sync(MOTOR_DIR_FORWARD, steps, speed);
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
