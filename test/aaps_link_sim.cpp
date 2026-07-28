/* AAPS 蓝牙客户端模拟器 + 固件动态控制联调
 * ============================================================
 * 这是"模拟联调"的主程序: 在主机上把固件 aaps_dana.cpp 的命令分发逻辑
 * (真实代码, 编译期开 USE_AAPS_DANA) 当作"被 AAPS 蓝牙控制的泵", 由本程序
 * 扮演 AndroidAPS 蓝牙客户端, 经 Dana BLE 信封层 (build→feed→固件重组→dispatch→
 * handle_command→构造响应→FFF1 发出→本程序解析) 跑一条完整闭环会话,
 * 并对每个命令后的固件状态变化做断言。
 *
 * 默认: 跑一条脚本化的完整会话 (握手→时间同步→状态/基础率/大剂量/方波/回归/篡改拒绝)。
 *       带 -i 参数: 进入交互 REPL, 可手动发任意 opcode+参数, 实时看固件状态。
 *
 * 编译见 test/run_link_sim.sh。
 *
 * ⚠️ 实验项目, 禁止用于人体。本程序仅做理论验证/教学原型联调。
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "aaps_dana.h"      // 真实协议常量 + 信封 API (extern "C")
#include "pump_state.h"     // g_pump_state (真实结构体)
#include "rtc_clock.h"      // rtc_clock_init / rtc_unix_now / rtc_is_set (真实固件实现)
#include "host_glue.h"      // 假硬件层 (电机/基础率记录, host_state_init)
#include "NimBLEDevice.h"   // 假 NimBLE (host_drain_tx / NimBLEServer)

/* 测试钩子 (由 aaps_dana.cpp 在 AAPS_DANA_HOST_TEST 下定义) */
extern "C" void aaps_dana_feed_rx_test(const uint8_t *data, size_t len);
extern "C" void aaps_dana_reset_test(void);

/* ---------- 统计 ---------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do {                                            \
    if (cond) { g_pass++; printf("  \033[32m[PASS]\033[0m %s\n", name); } \
    else      { g_fail++; printf("  \033[31m[FAIL]\033[0m %s\n", name); } \
} while (0)

/* ---------- 对端(AAPS)上下文, 镜像固件握手状态 ---------- */
static dana_ctx_t c_peer;

/* 最近一次响应解析结果 */
static uint8_t  g_rt_type, g_rt_op;
static uint8_t  g_rt_params[64];
static size_t   g_rt_nparams;

/* ---------- 传输: 把构建好的信封喂给固件, 抽回并解析一个响应 ---------- */
static int feed_and_recv(const uint8_t *pkt, size_t plen, bool expect_resp)
{
    aaps_dana_feed_rx_test(pkt, plen);
    if (!expect_resp) return 0;
    uint8_t resp[128];
    size_t rl = host_drain_tx(resp, sizeof(resp));
    if (rl == 0) return 0;                 // 固件未发响应 (被拒绝)
    uint8_t t, op, pr[64];
    size_t np = 0;
    /* 注意：固件响应已由 host_tx_push 在分片边界整体二级解密（g_tx_ble5 启用后），
     * 故此处用 dana_unpack_packet（不再二次解密），否则会解成乱码。 */
    int r = dana_unpack_packet(&c_peer, resp, rl, &t, &op, pr, sizeof(pr), &np);
    g_rt_type = t; g_rt_op = op; g_rt_nparams = np;
    memcpy(g_rt_params, pr, np);
    return (r == 0) ? 1 : -2;            // 1=解析成功, -2=CRC/结构错误
}

/* 发送握手类信封 (ENC_REQ, PUMP_CHECK / TIME_INFO) */
static int send_enc(uint8_t enc_op, const uint8_t *params, uint8_t nparams)
{
    uint8_t pkt[64]; size_t pl = 0;
    int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
    if (dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_ENC_REQ, enc_op,
                          params, nparams, apply_ble5) != 0) return -1;
    int rec = feed_and_recv(pkt, pl, true);
    if (rec > 0) {
        if (enc_op == DANA_OP_PUMP_CHECK)      c_peer.conn = DANA_CONN_PUMP_CHECK;
        else if (enc_op == DANA_OP_TIME_INFO)  c_peer.conn = DANA_CONN_HANDSHAKE_DONE;
    }
    return rec;
}

/* 发送命令类信封 (命令阶段, BLE5) */
static int send_cmd(uint8_t opcode, const uint8_t *params, uint8_t nparams)
{
    uint8_t pkt[64]; size_t pl = 0;
    int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
    if (dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND, opcode,
                          params, nparams, apply_ble5) != 0) return -1;
    return feed_and_recv(pkt, pl, true);
}

/* ---------- 状态快照 ---------- */
static void dump_state(void)
{
    printf("    泵状态: TBR%%=%.0f  TBR_Uh=%.3f  IOB_x10000=%u  today_x100=%u  "
           "dana_paired=%d  ext_active=%d  glucose_mgdl=%u\n",
           g_pump_state.tbr_percent, g_pump_state.tbr_rate, g_pump_state.iob_x10000,
           g_pump_state.today_units_x100, g_pump_state.dana_paired,
           g_pump_state.ext_bolus_active, g_pump_state.last_glucose_mgdl);
}

/* ============================================================
 * 脚本化完整会话
 * ============================================================ */
static void run_scripted(void)
{
    printf("\n========== AAPS ↔ 固件 模拟联调 (脚本化完整会话) ==========\n");

    /* ---- 1. 握手 PUMP_CHECK ---- */
    printf("\n[1] 握手 PUMP_CHECK (设备名 DAN12345AB, BLE5)\n");
    int r = send_enc(DANA_OP_PUMP_CHECK, NULL, 0);
    CHECK(r == 1, "PUMP_CHECK 收到固件响应");
    CHECK(c_peer.conn == DANA_CONN_PUMP_CHECK, "对端握手态 -> PUMP_CHECK");
    CHECK(g_rt_type == DANA_TYPE_ENC_RESP, "响应类型 = ENC_RESP");
    CHECK(g_rt_op == DANA_OP_PUMP_CHECK, "响应 opcode = PUMP_CHECK");
    CHECK(g_pump_state.dana_paired == false, "握手未完成时 dana_paired 仍为 false");

    /* ---- 2. 握手 TIME_INFO ---- */
    printf("\n[2] 握手 TIME_INFO (完成二级加密握手)\n");
    r = send_enc(DANA_OP_TIME_INFO, NULL, 0);
    CHECK(r == 1, "TIME_INFO 收到固件响应");
    CHECK(c_peer.conn == DANA_CONN_HANDSHAKE_DONE, "对端握手态 -> HANDSHAKE_DONE");
    CHECK(g_pump_state.dana_paired == true, "固件 dana_paired=true (闭环页据此显示 'AAPS 已接管')");
    /* 握手完成：开启 TX 捕获侧的 BLE5 二级解密，使后续命令响应能按明文 LEN 重组 */
    host_tx_set_ble5(c_peer.ble5_enc_key);

    /* ---- 3. GET_TIME (时钟未设置, 应回 2000-01-01) ---- */
    printf("\n[3] GET_TIME (0x70) 时钟基准尚未设置\n");
    r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    CHECK(r == 1, "GET_TIME 收到响应");
    CHECK(g_rt_nparams == 6, "GET_TIME 响应 6 字节");
    CHECK(g_rt_params[0] == 0 && g_rt_params[1] == 1 && g_rt_params[2] == 1,
          "未设时钟时回 2000-01-01 00:00:00");

    /* ---- 4. SET_TIME (0x71) 设为 2026-07-28 13:30:00 ---- */
    printf("\n[4] SET_TIME (0x71) -> 2026-07-28 13:30:00\n");
    uint8_t tset[6] = { 26, 7, 28, 13, 30, 0 };
    r = send_cmd(DANA_CMD_SET_TIME, tset, 6);
    CHECK(r == 1, "SET_TIME 收到响应");
    CHECK(rtc_is_set(), "固件 rtc_is_set()=true (硬件 RTC 已被设置)");
    CHECK(rtc_unix_now() >= 1780000000u, "rtc_unix_now() 返回合理 Unix 秒 (>2026)");

    /* ---- 5. GET_TIME 复核 ---- */
    printf("\n[5] GET_TIME 复核 (应回 2026-07-28 13:30:00)\n");
    r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    CHECK(r == 1, "GET_TIME 收到响应");
    CHECK(g_rt_params[0] == 26 && g_rt_params[1] == 7 && g_rt_params[2] == 28 &&
          g_rt_params[3] == 13 && g_rt_params[4] == 30,
          "GET_TIME 回显与 SET_TIME 一致 (年-2000=26,月=7,日=28,时=13,分=30)");

    /* ---- 6. INITIAL_SCREEN (0x02) 状态查询 ---- */
    printf("\n[6] INITIAL_SCREEN (0x02) 状态查询\n");
    r = send_cmd(DANA_CMD_INITIAL_SCREEN, NULL, 0);
    CHECK(r == 1, "INITIAL_SCREEN 收到响应");
    CHECK(g_rt_nparams == 17, "INITIAL_SCREEN 响应 17 字节 (Dana 标准布局)");
    dump_state();

    /* ---- 7. APS_TBR (0xC1) 闭环临时基础率 100% / 30min ---- */
    printf("\n[7] APS_TBR (0xC1) 闭环 TBR=100%% / 30min\n");
    host_reset_logs();
    uint8_t tbr[3] = { 100 & 0xFF, 0, 160 };   // pct=100, durCode=160 -> 30min
    r = send_cmd(DANA_CMD_APS_TBR, tbr, 3);
    CHECK(r == 1, "APS_TBR 收到响应");
    CHECK(g_pump_state.tbr_percent == 100.0f, "tbr_percent=100");
    CHECK(g_pump_state.tbr_rate > 0.49f && g_pump_state.tbr_rate < 0.51f,
          "tbr_rate≈0.5 U/h (参考基础率 0.5 × 100%)");
    CHECK(g_pump_state.tbr_expiry_ms > millis(), "tbr_expiry_ms 在未来");
    CHECK(g_host_motor.enqueue_count == 0, "APS_TBR 未误触发电机(仅改基础率)");

    /* ---- 8. CANCEL_TBR (0x62) ---- */
    printf("\n[8] CANCEL_TBR (0x62)\n");
    r = send_cmd(DANA_CMD_CANCEL_TBR, NULL, 0);
    CHECK(r == 1, "CANCEL_TBR 收到响应");
    CHECK(g_pump_state.tbr_percent == 0.0f, "tbr_percent 归零");
    CHECK(g_pump_state.tbr_rate == 0.0f, "tbr_rate 归零");

    /* ---- 9. SET_TBR (0x60) 手动 TBR=50% / 30min ---- */
    printf("\n[9] SET_TBR (0x60) 手动 TBR=50%% / 30min\n");
    uint8_t tbr2[3] = { 50 & 0xFF, 0, 160 };
    r = send_cmd(DANA_CMD_SET_TBR, tbr2, 3);
    CHECK(r == 1, "SET_TBR 收到响应");
    CHECK(g_pump_state.tbr_percent == 50.0f, "tbr_percent=50");

    /* ---- 10. STEP_BOLUS_START (0x4A) 2.0U ---- */
    printf("\n[10] STEP_BOLUS_START (0x4A) 大剂量 2.0U\n");
    host_reset_logs();
    uint8_t bol[3] = { 200 & 0xFF, 0, 0 };   // 200 = 2.00U, speed=0(默认)
    r = send_cmd(DANA_CMD_STEP_BOLUS_START, bol, 3);
    CHECK(r == 1, "STEP_BOLUS_START 收到响应");
    CHECK(g_host_motor.enqueue_count == 1, "motor_enqueue 被调用 1 次 (大剂量真正下发)");
    CHECK(g_host_motor.last_units_x100 == 200, "下发剂量=2.00U (200×100)");

    /* ---- 11. STEP_BOLUS_STOP (0x44) ---- */
    printf("\n[11] STEP_BOLUS_STOP (0x44)\n");
    r = send_cmd(DANA_CMD_STEP_BOLUS_STOP, NULL, 0);
    CHECK(r == 1, "STEP_BOLUS_STOP 收到响应");
    CHECK(g_host_motor.cancel_count == 1, "motor_cancel_bolus 被调用 (取消进行中的大剂量)");

    /* ---- 12. EXT_BOLUS (0x47) 方波 3U / 2h ---- */
    printf("\n[12] EXT_BOLUS (0x47) 方波 3U / 2h\n");
    host_reset_logs();
    uint8_t ext[3] = { 300 & 0xFF, (300 >> 8) & 0xFF, 4 };   // 300=3.00U (LE: lo=0x2C,hi=0x01), halfHours=4 -> 2h
    r = send_cmd(DANA_CMD_EXT_BOLUS, ext, 3);
    CHECK(r == 1, "EXT_BOLUS 收到响应");
    CHECK(g_host_basal.ext_start_count == 1, "basal_scheduler_start_extended_bolus 被调用");
    CHECK(g_host_basal.last_ext_units == 3.0f, "方波总量=3.0U");
    CHECK(g_host_basal.last_ext_dur_h == 2.0f, "方波时长=2.0h");
    CHECK(g_pump_state.ext_bolus_active == true, "固件 ext_bolus_active=true");

    /* ---- 13. EXT_BOLUS_CANCEL (0x49) ---- */
    printf("\n[13] EXT_BOLUS_CANCEL (0x49)\n");
    r = send_cmd(DANA_CMD_EXT_BOLUS_CANCEL, NULL, 0);
    CHECK(r == 1, "EXT_BOLUS_CANCEL 收到响应");
    CHECK(g_host_basal.ext_cancel_count == 1, "basal_scheduler_cancel_extended_bolus 被调用");
    CHECK(g_pump_state.ext_bolus_active == false, "固件 ext_bolus_active=false");

    /* ---- 14. DAILY_TOTAL (0x26) ---- */
    printf("\n[14] DAILY_TOTAL (0x26)\n");
    r = send_cmd(DANA_CMD_DAILY_TOTAL, NULL, 0);
    CHECK(r == 1, "DAILY_TOTAL 收到响应");
    CHECK(g_rt_nparams == 2, "DAILY_TOTAL 响应 2 字节");
    CHECK(((uint16_t)g_rt_params[0] | ((uint16_t)g_rt_params[1] << 8)) ==
          (uint16_t)(g_pump_state.today_units_x100 & 0xFFFF),
          "DAILY_TOTAL 与固件今日总量一致");

    /* ---- 15. 回归: SET_DUAL_BOLUS (0x48) 不应污染血糖 ---- */
    printf("\n[15] 回归校验 SET_DUAL_BOLUS (0x48)\n");
    printf("     注: 经 git 克隆 AAPS master 逐字节核对, 0x48 真实语义是双波大剂量,\n");
    printf("          绝非 CGM 血糖! 若误解析为血糖, 参数[0]=0x00,[1]=0x20 会被当 8192 mg/dL.\n");
    host_reset_logs();
    uint8_t dual[3] = { 0x00, 0x20, 0x03 };  // 若被误当血糖 -> last_glucose_mgdl=0x2000=8192
    r = send_cmd(DANA_CMD_SET_DUAL_BOLUS, dual, 3);
    CHECK(r == 1, "SET_DUAL_BOLUS 收到响应(教学原型返回 OK)");
    CHECK(g_pump_state.last_glucose_mgdl == 0,
          "血糖未被 0x48 参数污染 (last_glucose_mgdl 仍为 0, 安全!)");
    CHECK(g_rt_op == DANA_CMD_SET_DUAL_BOLUS, "响应 opcode 正确回显 0x48");
    dump_state();

    /* ---- 16. 安全: 篡改 CRC 的命令必须被拒绝 ---- */
    printf("\n[16] 安全校验 篡改 CRC 的命令被拒绝\n");
    uint8_t pkt[64]; size_t pl = 0;
    dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND, DANA_CMD_GET_TIME, NULL, 0, 1);
    pkt[pl - 3] ^= 0xFF;                       // 破坏 CRC 低字节
    aaps_dana_feed_rx_test(pkt, pl);
    uint8_t junk[128];
    size_t rl = host_drain_tx(junk, sizeof(junk));
    CHECK(rl == 0, "篡改 CRC 的命令 -> 固件无响应 (完整性保护生效)");

    /* 紧随其后发一个合法命令, 确认状态机未被破坏 */
    r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    CHECK(r == 1, "篡改后发合法命令仍正常响应 (状态机未损坏)");

    /* ---- 17. 收尾: 再查 INITIAL_SCREEN 确认整体一致 ---- */
    printf("\n[17] 收尾 INITIAL_SCREEN 复核\n");
    r = send_cmd(DANA_CMD_INITIAL_SCREEN, NULL, 0);
    CHECK(r == 1 && g_rt_nparams == 17, "INITIAL_SCREEN 仍返回 17 字节标准布局");
    dump_state();

    printf("\n========== 脚本化会话结束 ==========\n");
}

/* ============================================================
 * 交互 REPL: 手动发任意 opcode + 参数, 实时看固件状态
 *   用法: 每行 "opcode_hex [param_hex ...]"  e.g.  c1 64 00 a0
 *         hs      重新握手
 *         reset    复位固件握手态
 *         quit     退出
 * ============================================================ */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_bytes(const std::string &s, uint8_t *out, size_t *n, size_t cap)
{
    std::vector<int> v;
    for (size_t i = 0; i < s.size(); ) {
        while (i < s.size() && s[i] == ' ') i++;
        if (i >= s.size()) break;
        int hi = hexval(s[i]);
        if (hi < 0) return false;
        if (i + 1 >= s.size()) return false;
        int lo = hexval(s[i + 1]);
        if (lo < 0) return false;
        v.push_back(hi * 16 + lo);
        i += 2;
    }
    if (v.size() > cap) return false;
    for (size_t i = 0; i < v.size(); i++) out[i] = (uint8_t)v[i];
    *n = v.size();
    return true;
}

static void run_interactive(void)
{
    printf("\n========== AAPS 模拟器 交互模式 ==========\n");
    printf("  命令格式: <opcode_hex> [param_hex ...]   例如: c1 64 00 a0  (APS TBR 100%%/30min)\n");
    printf("  特殊指令: hs=重新握手  reset=复位固件握手态  quit=退出\n");
    printf("  已知 opcode: 02=状态 26=今日总量 40=大剂量进度 44=停大剂量 47=方波 49=停方波\n");
    printf("              4a=大剂量 60=手动TBR 62=取消TBR c1=APS_TBR 70=读时间 71=设时间 48=双波\n\n");

    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;

        if (s == "quit" || s == "exit") break;
        if (s == "hs") {
            aaps_dana_reset_test();
            host_tx_clear_ble5();
            c_peer.conn = DANA_CONN_INIT;
            int r1 = send_enc(DANA_OP_PUMP_CHECK, NULL, 0);
            int r2 = send_enc(DANA_OP_TIME_INFO, NULL, 0);
            if (r2 == 1) host_tx_set_ble5(c_peer.ble5_enc_key);
            printf("  握手: PUMP_CHECK=%s TIME_INFO=%s dana_paired=%d\n",
                   r1 == 1 ? "OK" : "ERR", r2 == 1 ? "OK" : "ERR", g_pump_state.dana_paired);
            continue;
        }
        if (s == "reset") {
            aaps_dana_reset_test();
            host_tx_clear_ble5();
            c_peer.conn = DANA_CONN_INIT;
            printf("  固件握手态已复位\n");
            continue;
        }

        /* 解析 opcode + 参数 */
        size_t sp = s.find(' ');
        std::string opstr = (sp == std::string::npos) ? s : s.substr(0, sp);
        std::string parstr = (sp == std::string::npos) ? "" : s.substr(sp + 1);

        int oh = hexval(opstr.size() >= 1 ? opstr[0] : ' ');
        int ol = (opstr.size() >= 2) ? hexval(opstr[1]) : -1;
        if (oh < 0 || ol < 0) { printf("  opcode 解析失败\n"); continue; }
        uint8_t opcode = (uint8_t)(oh * 16 + ol);

        uint8_t params[64];
        size_t np = 0;
        if (!parstr.empty() && !parse_hex_bytes(parstr, params, &np, sizeof(params))) {
            printf("  参数解析失败\n");
            continue;
        }

        int r = send_cmd(opcode, params, (uint8_t)np);
        if (r == 1) {
            printf("  响应: type=0x%02X op=0x%02X nparams=%zu  params=",
                   g_rt_type, g_rt_op, g_rt_nparams);
            for (size_t i = 0; i < g_rt_nparams; i++) printf("%02X ", g_rt_params[i]);
            printf("\n");
        } else if (r == 0) {
            printf("  固件无响应 (可能被拒绝 / 该命令无回包)\n");
        } else {
            printf("  响应 CRC/结构校验失败\n");
        }
        dump_state();
    }
    printf("========== 交互模式结束 ==========\n");
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char **argv)
{
    printf("============================================================\n");
    printf(" 闭环胰岛素泵固件 — AAPS 蓝牙动态控制模拟联调\n");
    printf(" ⚠️ 实验项目 / 教学原型, 严禁用于任何人体 (含使用者本人)!\n");
    printf("============================================================\n");

    host_state_init();
    rtc_clock_init();                       // 与固件一致: 从 g_pump_config.rtc_base_unix 恢复 (本机=0, 未设置)

    /* 初始化对端(AAPS)上下文: 与固件 DANAI_* 完全一致 */
    dana_ctx_init(&c_peer, "DAN12345AB", "123456", 0x09, 0x0A);

    /* 挂载 Dana GATT 服务 (假 NimBLE), 初始化固件 g_dana_ctx */
    NimBLEServer fake_server;
    aaps_dana_attach(&fake_server);

    bool interactive = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-i" || std::string(argv[i]) == "--interactive")
            interactive = true;
    }

    run_scripted();

    if (interactive) run_interactive();

    printf("\n============================================================\n");
    printf(" 联调结果: \033[32mPASS=%d\033[0m  \033[31mFAIL=%d\033[0m  (合计 %d)\n",
           g_pass, g_fail, g_pass + g_fail);
    if (g_fail == 0)
        printf(" ✅ 全部通过 — 固件命令分发逻辑与 AAPS Dana 协议一致, 可烧录联调。\n");
    else
        printf(" ❌ 存在失败项, 需排查固件逻辑。\n");
    printf("============================================================\n");
    return (g_fail == 0) ? 0 : 1;
}
