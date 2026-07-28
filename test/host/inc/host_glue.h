/* Host glue — 主机联调测试用的"假硬件"层接口。
 * 实现位于 host_glue.cpp; 由 AAPS 模拟器(aaps_link_sim.cpp)与桩件共同使用。
 *
 * ⚠️ 实验项目, 禁止用于人体。
 */
#pragma once

#include <cstdint>
#include <cstddef>

/* 电机调用记录 (验证大剂量/取消是否真正下发) */
struct host_motor_log_t {
    int      enqueue_count;      // motor_enqueue 调用次数
    uint32_t last_units_x100;    // 最近一次大剂量单位 ×100
    int      cancel_count;       // motor_cancel_bolus 调用次数
    int      active_bolus;       // 进行中的大剂量计数 (enqueue - cancel, 下限0)
    uint32_t delivered_units_x100; // 累计已下发剂量 (单位 ×100), 供电机推药演示
};

/* 基础率/方波调用记录 */
struct host_basal_log_t {
    int   ext_start_count;       // basal_scheduler_start_extended_bolus 次数
    float last_ext_units;        // 最近方波总量 (U)
    float last_ext_dur_h;        // 最近方波时长 (h)
    int   ext_cancel_count;      // basal_scheduler_cancel_extended_bolus 次数
};

extern host_motor_log_t g_host_motor;
extern host_basal_log_t g_host_basal;

/* 累计已下发剂量 (单位 ×100), 供模拟器电机推药演示读取 */
uint32_t motor_delivered_units_x100(void);

/* millis() 由 Arduino 框架在真机提供; 主机测试在 host_glue.cpp 实现。 */
uint32_t millis(void);

void host_reset_logs(void);

/* 配置/状态默认值初始化 (供 main 调用) */
void host_state_init(void);

/* ============================================================
 * TX 捕获的 BLE5 二级解密控制（镜像固件 dana_feed_rx 的接收边界解密）
 * ------------------------------------------------------------
 * 固件命令阶段(握手完成)的响应是 BLE5 加密信封，其 LEN 字节也是密文。
 * 若不先解密就按 LEN 重组，会读错长度、抽不出完整包。
 * 故在收到每个分片(对应 NimBLE notify 的 setValue)时，若 BLE5 已启用，
 * 先整体二级解密再追加到 TX 流，host_drain_tx 即可按明文 LEN 重组。
 *   host_tx_set_ble5(key)   : 握手完成后调用，传入 3 字节 BLE5 密钥
 *   host_tx_clear_ble5()    : 断连/重置握手时调用，恢复不解密
 * ============================================================ */
void host_tx_set_ble5(const uint8_t key[3]);
void host_tx_clear_ble5(void);
