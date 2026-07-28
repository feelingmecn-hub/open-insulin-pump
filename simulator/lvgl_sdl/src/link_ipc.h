/* link_ipc.h — 模拟器联调模式的控制通道 (TCP)
 *
 * 在模拟器内起一个 TCP 服务器 (127.0.0.1:18923), 向连接的 GUI 广播状态 JSON,
 * 并接收 play / pause / step / reset / delay 指令驱动 link_session。
 *
 * 协议 (纯文本, 行分隔 '\n'):
 *   模拟器 -> GUI:  {"t":"status","idx":N,"total":M,"playing":B,"steps":[...],"state":{...}}
 *                   {"t":"reset","total":M}
 *    GUI -> 模拟器:  play | pause | step | reset | delay <ms>
 *
 * ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
 */
#pragma once

namespace linkipc {

void start(void);             // 起 TCP 服务器线程 (非阻塞)
void broadcast_status(void);  // 广播当前状态 JSON
void broadcast_reset(void);   // 广播 reset 信号后跟着当前状态

} // namespace linkipc
