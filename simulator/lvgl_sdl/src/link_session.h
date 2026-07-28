/* link_session.h — 模拟器联调模式的"脚本化会话播放器"
 *
 * 把 test/aaps_link_sim.cpp 的 17 步完整闭环会话移植为可被模拟器主循环
 * 逐步驱动的播放器。每一步直接调用真实固件命令分发代码
 * (aaps_dana_feed_rx_test / dana_build_packet / dana_unpack_packet), 实时改写
 * g_pump_state —— 模拟器的 LVGL 泵屏幕每帧从 g_pump_state 重绘, 因此画面
 * 与命令严格同步。
 *
 * 本头仅声明 C++ 接口, 供 main.cpp / link_ipc.cpp 使用。
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

struct LinkCheck {
    std::string text;
    bool       ok;
};

struct LinkStep {
    int                 index;    // 1-based
    std::string         title;
    std::vector<LinkCheck> checks;
};

/* 协议轨迹: 一次 AAPS->泵 命令的完整抓包 (供"发送/接收"两个调试窗共用) */
struct LinkTrace {
    int      step_index;     // 所属步骤 (1-based)
    uint8_t  type;           // 发出包类型 (DANA_TYPE_*)
    uint8_t  opcode;         // 发出 opcode
    std::string op_name;     // opcode 可读名 (如 "STEP_BOLUS_START")
    std::string intent;      // 人类意图 (如 "大剂量 2.00U")
    std::string tx_hex;      // AAPS -> 泵 原始字节 (hex)
    bool     crc_ok;         // 固件解包 CRC 校验结果
    std::string rx_info;     // 固件解包结果 (type/op/params)
    std::string action;      // 固件分发动作 (如 "motor_enqueue(2.00U)")
    std::string resp_hex;    // 泵 -> AAPS 回应原始字节 (hex, 空=无回应)
    bool     rejected;       // 被拒绝 (CRC 篡改等, 无回应)
};

namespace linksess {

void init(void);
void reset(void);

// 执行下一步; 返回 false 表示已无更多步骤
bool step(void);

void    set_playing(bool p);
bool    playing(void);
int     index(void);    // 已执行的最后一步 (1-based), 0 = 未开始
int     total(void);    // 总步数

void    set_delay(int ms);   // 自动播放时每步间隔 (ms)
int     delay_ms(void);

// 把全部已执行步骤序列化为 "\"steps\":[...]" JSON 片段 (含 key 与方括号, 不含外层花括号)
void all_steps_json(char *out, size_t cap);

// 把当前完整泵状态序列化为 state JSON 内层字段 (不含 key / 外层花括号)
void snapshot_json(char *out, size_t cap);

// 把全部协议轨迹序列化为 "\"trace\":[...]" JSON 片段 (含 key 与方括号)
void trace_json(char *out, size_t cap);

} // namespace linksess
