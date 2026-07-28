/* Host glue — 主机联调测试用的"假硬件"实现。
 *
 * 作用:
 *   1. 提供固件 aaps_dana.cpp (USE_AAPS_DANA) 在主机侧需要的全局状态与桩函数:
 *        - g_pump_state / g_pump_config (真实结构体, 来自 pump_types.h)
 *        - motor_enqueue / motor_cancel_bolus (记录调用, 不真走电机)
 *        - basal_scheduler_start/cancel_extended_bolus (记录 + 简单状态反射)
 *        - storage_save_config (no-op)
 *        - millis() (主机单调时钟)
 *   2. 实现 NimBLE 桩的 TX 捕获 (host_tx_push / host_drain_tx)。
 *
 * 这样编译出来的固件命令分发代码是**真实固件代码**, 只是运行在主机内存里,
 * 由 AAPS 模拟器经 BLE 信封层驱动 —— 即"模拟联调"。
 *
 * ⚠️ 实验项目, 禁止用于人体。
 */
#include "pump_state.h"     // 真实 g_pump_state / g_pump_config 声明 + config/dosing
#include "motor_controller.h"
#include "basal_scheduler.h"
#include "storage.h"
#include "history_log.h"     // ui_hal_link 模式需要的桩 (history_log.h 不依赖 Arduino)
#include "aaps_dana.h"       // dana_decrypt_second_level (协议层, 宿主可用)
#include "host_glue.h"

#include <chrono>

/* ============================================================
 * 全局状态 (真实结构体, 与固件完全一致)
 * ------------------------------------------------------------
 * 双归属:
 *   - 主机联调测试 (test/run_link_sim.sh, 定义 HOST_GLUE_OWNS_STATE):
 *       pump_state.cpp 不编入, 这里定义 g_pump_state / g_pump_config。
 *   - 模拟器联调模式 (SIM_LINK_MODE): pump_state.cpp 已定义它们,
 *       这里改为 extern 声明, 避免重复符号。
 * ============================================================ */
#ifdef HOST_GLUE_OWNS_STATE
pump_runtime_state_t g_pump_state;
pump_config_t        g_pump_config;
#else
extern pump_runtime_state_t g_pump_state;
extern pump_config_t        g_pump_config;
#endif

/* ============================================================
 * 调用记录 (供测试断言)
 * ============================================================ */
host_motor_log_t g_host_motor;
host_basal_log_t g_host_basal;

void host_reset_logs(void)
{
    g_host_motor.enqueue_count = 0;
    g_host_motor.last_units_x100 = 0;
    g_host_motor.cancel_count = 0;
    g_host_motor.active_bolus = 0;
    /* 注意: delivered_units_x100 (累计发药) 不在此清零 —— 它是跨步单调量,
     * 仅由会话(重)开始 setup_state() 归零, 这样电机演示柱塞不会回跳。 */
    g_host_basal.ext_start_count = 0;
    g_host_basal.last_ext_units = 0;
    g_host_basal.last_ext_dur_h = 0;
    g_host_basal.ext_cancel_count = 0;
}

void host_state_init(void)
{
    memset(&g_pump_state, 0, sizeof(g_pump_state));
    memset(&g_pump_config, 0, sizeof(g_pump_config));

    g_pump_state.current_basal_rate   = 0.5f;   // 基础率 0.5 U/h (默认档案)
    g_pump_state.battery_pct         = 100;
    g_pump_state.reservoir_units_left = 300;    // 满储药器
    g_pump_state.iob_x10000          = 0;
    g_pump_state.today_units_x100    = 0;
    g_pump_state.current_state       = 1;       // PUMP_STATE_IDLE
    g_pump_state.battery_mv          = 11100;
    g_pump_state.ble_connected       = true;    // 模拟已 BLE 连接
    g_pump_state.loop_mode           = 0;       // 闭环 (AAPS 接管)

    host_reset_logs();
}

/* ============================================================
 * 电机桩 (记录意图, 不真走硬件)
 * ============================================================ */
bool motor_enqueue(const motor_command_t *cmd)
{
    g_host_motor.enqueue_count++;
    if (g_host_motor.active_bolus < 65535) g_host_motor.active_bolus++;
    if (cmd) {
        g_host_motor.last_units_x100 = cmd->units_x100;
        if (g_host_motor.delivered_units_x100 + cmd->units_x100 > g_host_motor.delivered_units_x100)
            g_host_motor.delivered_units_x100 += cmd->units_x100;   // 累计已下发 (溢出保护)
    }
    return true;
}

void motor_cancel_bolus(void)
{
    g_host_motor.cancel_count++;
    if (g_host_motor.active_bolus > 0) g_host_motor.active_bolus--;
}

/* 累计已下发剂量 (单位 ×100), 供模拟器电机推药演示读取 */
uint32_t motor_delivered_units_x100(void)
{
    return g_host_motor.delivered_units_x100;
}

/* 进行中的大剂量计数 (供 ui_hal_fw 的 ui_hal_bolus_active) */
bool motor_bolus_active(void)
{
    return g_host_motor.active_bolus > 0;
}

/* ============================================================
 * 基础率/方波桩 (记录 + 简单状态反射, 便于 INITIAL_SCREEN 等读回)
 * ============================================================ */
void basal_scheduler_start_extended_bolus(float units, float duration_h, uint8_t kind)
{
    g_host_basal.ext_start_count++;
    g_host_basal.last_ext_units = units;
    g_host_basal.last_ext_dur_h = duration_h;
    g_pump_state.ext_bolus_active        = true;
    g_pump_state.ext_bolus_kind          = kind;
    g_pump_state.ext_bolus_total_x100    = (uint32_t)(units * 100.0f);
    g_pump_state.ext_bolus_delivered_x100 = 0;
    g_pump_state.ext_bolus_duration_ms   = (uint32_t)(duration_h * 3600000.0f);
    g_pump_state.ext_bolus_start_ms      = millis();
}

void basal_scheduler_cancel_extended_bolus(void)
{
    g_host_basal.ext_cancel_count++;
    g_pump_state.ext_bolus_active = false;
}

/* 方波是否进行中 (供 ui_hal_fw 的 ui_hal_bolus_active) */
bool basal_scheduler_extended_bolus_active(void)
{
    return g_pump_state.ext_bolus_active;
}

/* ---- 以下为 ui_hal_fw.cpp 在模拟器联调模式下需要的无操作桩 ---- */
void lcd_display_backlight(uint8_t) { /* 模拟器无真实背光硬件 */ }

void history_log_event(event_type_t, uint8_t, uint32_t, uint16_t) { /* 模拟器不落盘历史 */ }

/* ============================================================
 * 存储桩
 * ============================================================ */
void storage_save_config(const pump_config_t *) { /* no-op on host */ }

/* ============================================================
 * millis() — 主机单调毫秒时钟
 * ============================================================ */
static std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();
uint32_t millis(void)
{
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_epoch).count();
    return (uint32_t)ms;
}

/* ============================================================
 * TX 捕获 (NimBLE 桩 setValue → 这里)
 * ============================================================ */
static uint8_t  g_txbuf[8192];
static size_t   g_txlen = 0;

/* BLE5 二级解密控制：握手完成后对固件响应分片先解密再追加（对齐固件 dana_feed_rx） */
static bool     g_tx_ble5 = false;
static uint8_t  g_tx_key[3] = {0, 0, 0};

void host_tx_set_ble5(const uint8_t key[3])
{
    g_tx_ble5 = true;
    g_tx_key[0] = key[0]; g_tx_key[1] = key[1]; g_tx_key[2] = key[2];
}

void host_tx_clear_ble5(void)
{
    g_tx_ble5 = false;
}

void host_tx_push(const uint8_t *data, size_t len)
{
    /* 接收边界先整体二级解密（命令阶段固件响应为 BLE5 加密信封，
     * LEN 字节也是密文；先解密才能按明文 LEN 重组完整包）。
     * 逐字节变换与位置无关，按 BLE 分片逐片解密等价于整包解密。 */
    uint8_t chunk[DANA_MAX_PACKET];
    size_t  cl = (len > sizeof(chunk)) ? sizeof(chunk) : len;
    memcpy(chunk, data, cl);
    if (g_tx_ble5) dana_decrypt_second_level(chunk, cl, g_tx_key);

    if (g_txlen + cl <= sizeof(g_txbuf)) {
        memcpy(g_txbuf + g_txlen, chunk, cl);
        g_txlen += cl;
    }
}

size_t host_drain_tx(uint8_t *out, size_t cap)
{
    if (g_txlen < 7) return 0;

    /* 找起始标记 (A5 A5 或 AA AA) */
    size_t i = 0;
    bool found = false;
    for (; i + 1 < g_txlen; i++) {
        if ((g_txbuf[i] == 0xA5 && g_txbuf[i + 1] == 0xA5) ||
            (g_txbuf[i] == 0xAA && g_txbuf[i + 1] == 0xAA)) {
            found = true;
            break;
        }
    }
    if (!found) { g_txlen = 0; return 0; }
    if (i > 0) { memmove(g_txbuf, g_txbuf + i, g_txlen - i); g_txlen -= i; }

    uint8_t plen = g_txbuf[2];
    size_t  total = (size_t)plen + 7u;
    if (total > g_txlen) return 0;          // 分包未到齐

    size_t copy = (total > cap) ? cap : total;
    memcpy(out, g_txbuf, copy);
    memmove(g_txbuf, g_txbuf + total, g_txlen - total);
    g_txlen -= total;
    return copy;
}
