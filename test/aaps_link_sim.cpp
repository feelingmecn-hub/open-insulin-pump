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
#include "ui_hal.h"         // ui_hal_set_tbr / cancel_tbr / TBR 历史钩子 (P2-9 菜单 TBR 回归)
#include "host_glue.h"      // 假硬件层 (电机/基础率记录, host_state_init)
#include "NimBLEDevice.h"   // 假 NimBLE (host_drain_tx / NimBLEServer)

/* 测试钩子 (由 aaps_dana.cpp 在 AAPS_DANA_HOST_TEST 下定义) */
extern "C" void aaps_dana_feed_rx_test(const uint8_t *data, size_t len);
extern "C" void aaps_dana_reset_test(void);
extern "C" void aaps_dana_host_link_up_test(void);   // 打开发送门控(模拟已连接+已订阅)
/* 注：固件响应走异步发送队列，只有在 loop 上下文调用 aaps_dana_pump()（已在
 * aaps_dana.h 声明）才真正 notify 出去。主机联调没有 loop，必须在每次喂包后
 * 手动泵一次，否则响应滞留队列、host_drain_tx 永远抽不到东西。 */

/* ---------- 统计 ---------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do {                                            \
    if (cond) { g_pass++; printf("  \033[32m[PASS]\033[0m %s\n", name); } \
    else      { g_fail++; printf("  \033[31m[FAIL]\033[0m %s\n", name); } \
} while (0)

/* ---------- 对端(AAPS)上下文, 镜像固件握手状态 ---------- */
static dana_ctx_t c_peer;

/* 最近一次响应解析结果
 * 缓冲区必须 ≥ 最长响应参数区 = 0x52 GET_24_CIR_CF_ARRAY 的 97B，
 * 也要能装下最长请求 0x53 SET_24_CIR_CF_ARRAY 的 96B 参数（整包 105B）。 */
#define SIM_PARAM_CAP  128
#define SIM_PKT_CAP    160
static uint8_t  g_rt_type, g_rt_op;
static uint8_t  g_rt_params[SIM_PARAM_CAP];
static size_t   g_rt_nparams;

/* ---------- 传输: 把构建好的信封喂给固件, 抽回并解析一个响应 ---------- */
static int feed_and_recv(const uint8_t *pkt, size_t plen, bool expect_resp)
{
    aaps_dana_feed_rx_test(pkt, plen);
    aaps_dana_pump();              // 代替固件 loop：把队列里的响应真正 notify 出去
    if (!expect_resp) return 0;
    uint8_t resp[SIM_PKT_CAP];
    size_t rl = host_drain_tx(resp, sizeof(resp));
    if (rl == 0) return 0;                 // 固件未发响应 (被拒绝)
    uint8_t t, op, pr[SIM_PARAM_CAP];
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
    uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
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
    /* ⚠️ 0x53 SET_24_CIR_CF_ARRAY 的参数区就有 96B，整包 105B —— 旧的 pkt[64]
     * 会让 dana_build_packet 直接失败，这类长写入命令根本发不出去。 */
    uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
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

    /* ---- 3. GET_TIME (时钟未设置, 应回退到「固件编译时刻」) ----
     * ⚠️ 这是有意设计, 不是 bug:
     *   aaps_dana.cpp::dana_local_now() 在 rtc_unix_now()==0 时回退到
     *   rtc_clock.h::rtc_build_time_unix()。原因有二:
     *   ① AAPS 用 Joda DateTime 解析 y/m/d, month=0 或 day=0 会抛
     *      IllegalFieldValueException → setReceived() 不执行 → 5 秒超时断开;
     *   ② 编译时刻比 2000-01-01 更接近真实时间, AAPS 首次同步的时间差更小。
     * 故断言口径 = 「日期必须合法且等于编译时刻」, 而非硬编码 2000-01-01。 */
    printf("\n[3] GET_TIME (0x70) 时钟基准尚未设置 (应回退到固件编译时刻)\n");
    r = send_cmd(DANA_CMD_GET_TIME, NULL, 0);
    CHECK(r == 1, "GET_TIME 收到响应");
    CHECK(g_rt_nparams == 6, "GET_TIME 响应 6 字节");
    {
        int by, bmo, bd, bh, bmi, bs;
        rtc_unix_to_ymdhms(rtc_build_time_unix(), &by, &bmo, &bd, &bh, &bmi, &bs);
        printf("     泵回时间: 20%02d-%02d-%02d %02d:%02d:%02d  (编译时刻 %04d-%02d-%02d %02d:%02d:%02d)\n",
               g_rt_params[0], g_rt_params[1], g_rt_params[2],
               g_rt_params[3], g_rt_params[4], g_rt_params[5],
               by, bmo, bd, bh, bmi, bs);
        CHECK(g_rt_params[1] >= 1 && g_rt_params[1] <= 12 &&
              g_rt_params[2] >= 1 && g_rt_params[2] <= 31,
              "未设时钟时月/日仍合法 (防 AAPS Joda IllegalFieldValueException 断连)");
        CHECK(g_rt_params[0] == (uint8_t)(by - 2000) &&
              g_rt_params[1] == (uint8_t)bmo && g_rt_params[2] == (uint8_t)bd,
              "未设时钟时回退到固件编译时刻 (dana_local_now 兜底生效)");
    }

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

    /* ---- 9. SET_TBR (0x60) 手动 TBR=50% / 1h (AAPS 真实格式: [pct u8][durHours u8]) ---- */
    printf("\n[9] SET_TBR (0x60) 手动 TBR=50%% / 1h (真实 2 字节布局)\n");
    uint8_t tbr2[2] = { 50 & 0xFF, 1 };   // pct=50, durHours=1 -> 60min
    r = send_cmd(DANA_CMD_SET_TBR, tbr2, 2);
    CHECK(r == 1, "SET_TBR 收到响应");
    CHECK(g_pump_state.tbr_percent == 50.0f, "tbr_percent=50");
    CHECK(g_pump_state.tbr_expiry_ms > millis(), "tbr_expiry_ms 在未来(60min)");

    /* ---- 9b. SET_TBR (0x60) 0% low-temp / 1h —— 复现用户实测失败场景 ---- */
    printf("\n[9b] SET_TBR (0x60) 0%% low-temp / 1h (复现 10:44 报错场景)\n");
    uint8_t tbr0[2] = { 0, 1 };   // pct=0, durHours=1
    r = send_cmd(DANA_CMD_SET_TBR, tbr0, 2);
    CHECK(r == 1, "SET_TBR 收到响应");
    CHECK(g_pump_state.tbr_percent == 0.0f, "tbr_percent=0 (low-temp)");
    CHECK(g_pump_state.tbr_rate == 0.0f, "tbr_rate=0 (基础率降到 0)");
    CHECK(g_pump_state.tbr_expiry_ms > millis(),
          "tbr_expiry_ms 在未来 → TBR 位置位, AAPS 验证 isTempBasalInProgress 通过");

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

    /* ---- 15b. AAPS「设置配置文件」写入 → 读回一致性 ----
     * 对应真机现象：AAPS 设备信息页反复出现「设置配置文件」按钮。
     * 根因是泵收到 0x64/0x66/0x53 只回 OK 不落盘，下次连接读回还是旧值。
     * 这里用「写入 → 用对应的读取命令读回 → 逐字节比对」来锁死这条回归。 */
    printf("\n[15b] AAPS 设置配置文件: 0x64/0x66/0x53 写入 -> 0x63/0x67/0x52 读回\n");

    /* (a) 0x64 SET_PROFILE_NUMBER = 2 */
    uint8_t pn[1] = { 2 };
    r = send_cmd(DANA_CMD_SET_PROFILE_NUMBER, pn, 1);
    CHECK(r == 1 && g_rt_params[0] == 0x00, "0x64 SET_PROFILE_NUMBER 回 OK");
    r = send_cmd(DANA_CMD_PROFILE_NUMBER, NULL, 0);
    CHECK(r == 1 && g_rt_nparams == 1 && g_rt_params[0] == 2,
          "0x63 读回方案号 = 2 (写入已生效)");

    /* (b) 0x66 SET_PROFILE_BASAL_RATE: 方案 2, 24 段 = 0.10..0.33 U/h 递增 */
    uint8_t br[49];
    br[0] = 2;
    for (int i = 0; i < 24; i++) {
        uint16_t x100 = (uint16_t)(10 + i);          // 0.10 U/h .. 0.33 U/h
        br[1 + i * 2] = (uint8_t)(x100 & 0xFF);
        br[2 + i * 2] = (uint8_t)(x100 >> 8);
    }
    r = send_cmd(DANA_CMD_SET_PROFILE_BASAL, br, 49);
    CHECK(r == 1 && g_rt_params[0] == 0x00, "0x66 SET_PROFILE_BASAL_RATE(49B) 回 OK");

    r = send_cmd(DANA_CMD_BASAL_RATE, NULL, 0);
    CHECK(r == 1 && g_rt_nparams == 51, "0x67 读回 51B");
    {
        bool basal_ok = (g_rt_nparams == 51);
        for (int i = 0; i < 24 && basal_ok; i++) {
            uint16_t got = (uint16_t)(g_rt_params[3 + i * 2] |
                                      ((uint16_t)g_rt_params[4 + i * 2] << 8));
            if (got != (uint16_t)(10 + i)) {
                printf("      段%d 期望 %u 实得 %u\n", i, (unsigned)(10 + i), (unsigned)got);
                basal_ok = false;
            }
        }
        CHECK(basal_ok, "0x67 读回 24 段基础率与 0x66 写入逐段一致 (持久化生效)");
    }

    /* (c) 0x53 SET_24_CIR_CF_ARRAY: 96B (24×CIR + 24×CF)。
     *     这是整条链路最长的写入包 —— 同时验证 105B 整包的分片重组不被截断。 */
    uint8_t cc[96];
    for (int i = 0; i < 24; i++) {
        uint16_t cir = (uint16_t)(8 + i);            // 8..31 g/U
        uint16_t cf  = (uint16_t)(40 + i * 2);       // 40..86 mg/dL·U⁻¹
        cc[i * 2]          = (uint8_t)(cir & 0xFF);
        cc[i * 2 + 1]      = (uint8_t)(cir >> 8);
        cc[48 + i * 2]     = (uint8_t)(cf & 0xFF);
        cc[48 + i * 2 + 1] = (uint8_t)(cf >> 8);
    }
    r = send_cmd(DANA_CMD_SET_24_CIR_CF, cc, 96);
    CHECK(r == 1 && g_rt_params[0] == 0x00, "0x53 SET_24_CIR_CF_ARRAY(96B) 回 OK");

    r = send_cmd(DANA_CMD_24_CIR_CF_ARRAY, NULL, 0);
    CHECK(r == 1 && g_rt_nparams == 97, "0x52 读回 97B (最长响应包未被截断)");
    {
        bool cc_ok = (g_rt_nparams == 97);
        for (int i = 0; i < 24 && cc_ok; i++) {
            uint16_t gcir = (uint16_t)(g_rt_params[1 + i * 2] |
                                       ((uint16_t)g_rt_params[2 + i * 2] << 8));
            uint16_t gcf  = (uint16_t)(g_rt_params[49 + i * 2] |
                                       ((uint16_t)g_rt_params[50 + i * 2] << 8));
            if (gcir != (uint16_t)(8 + i) || gcf != (uint16_t)(40 + i * 2)) {
                printf("      时段%d 期望 CIR=%u CF=%u 实得 CIR=%u CF=%u\n",
                       i, (unsigned)(8 + i), (unsigned)(40 + i * 2),
                       (unsigned)gcir, (unsigned)gcf);
                cc_ok = false;
            }
        }
        CHECK(cc_ok, "0x52 读回 24×CIR/CF 与 0x53 写入逐段一致 (后 48B 的 CF 未被截断)");
    }

    /* (d) 畸形短包必须被安全忽略，不得把 profile 写坏 */
    uint8_t shortp[8] = { 2, 1, 0, 1, 0, 1, 0, 1 };
    r = send_cmd(DANA_CMD_SET_PROFILE_BASAL, shortp, 8);
    CHECK(r == 1 && g_rt_params[0] == 0x00, "0x66 短包仍回 OK (不断连)");
    r = send_cmd(DANA_CMD_BASAL_RATE, NULL, 0);
    CHECK(r == 1 &&
          (uint16_t)(g_rt_params[3] | ((uint16_t)g_rt_params[4] << 8)) == 10,
          "0x66 短包被忽略, 原 24 段基础率未被破坏 (防御畸形包)");

    /* ---- 16. 安全: 篡改 CRC 的命令必须被拒绝 ---- */
    printf("\n[16] 安全校验 篡改 CRC 的命令被拒绝\n");
    uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
    dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND, DANA_CMD_GET_TIME, NULL, 0, 1);
    pkt[pl - 3] ^= 0xFF;                       // 破坏 CRC 低字节
    aaps_dana_feed_rx_test(pkt, pl);
    aaps_dana_pump();
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

    /* ---- 18. 修复回归: APS_HISTORY_EVENTS (0xC2) 必须回放 BOLUS/TEMP/EXT 记录 ----
     * 根因: 此前固件对 0xC2 只回 0xFF 不回放任何记录 → AAPS 读不到任何泵事件
     *       → 大剂量/临时基础率/方波 控制成功却永不写治疗账本 (IOB 漏记, 闭环低血糖风险)。
     *       用户实测"所有 AAPS 操作泵的记录都看不到"正是此因(0xC2 历史回放缺失)。
     * 此处注入三条历史(一笔 0.1U 大剂量 + 一次 100%/30min TBR + 一次 0.5U/60min 方波),
     * 然后发 0xC2 (from=load-all), 抽回全部响应包逐条解包, 断言:
     *   · 存在 BOLUS 记录(code=5) 且 amount×100 正确、时间戳正确;
     *   · 存在 TEMP_START 记录(code=1) 且百分比/时长正确;
     *   · 存在 EXT_START 记录(code=3) 且 amount×100/时长正确;
     *   · 末尾有 0xFF 结束标记;
     *   · 所有记录 code 均为 AAPS 已知值(否则 processMessage 会抛异常中断整批)。 */
    printf("\n[18] 修复回归: APS_HISTORY_EVENTS (0xC2) 回放 BOLUS/TEMP/EXT 历史\n");
    uint32_t bolus_ts = rtc_unix_now();
    aaps_dana_record_bolus(bolus_ts, 10);                 // 0.1U
    aaps_dana_record_tbr(DANA_HIST_CODE_TEMP_START, bolus_ts + 5, 100, 30);
    aaps_dana_record_tbr(DANA_HIST_CODE_EXT_START, bolus_ts + 10, 50, 60);  // 0.5U/60min 方波
    {
        uint8_t from[6] = { 0, 1, 1, 0, 0, 0 };          // load-all (公元2000)
        uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
        int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
        int br = dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND,
                                   DANA_CMD_APS_HISTORY_EVENTS, from, 6, apply_ble5);
        CHECK(br == 0, "0xC2 请求包构建成功");
        aaps_dana_feed_rx_test(pkt, pl);
        aaps_dana_pump();                                 // 把所有响应包泵入 TX 流

        int n_rec = 0, n_end = 0;
        bool bad_code = false;
        bool found_bolus = false, found_tstart = false, found_ext = false;
        uint8_t rx[SIM_PKT_CAP];
        size_t rl;
        while ((rl = host_drain_tx(rx, sizeof(rx))) > 0) {
            uint8_t t, op, pr[SIM_PARAM_CAP]; size_t np = 0;
            int ur = dana_unpack_packet(&c_peer, rx, rl, &t, &op, pr, sizeof(pr), &np);
            if (ur != 0) { bad_code = true; continue; }
            if (op != DANA_CMD_APS_HISTORY_EVENTS) continue;
            n_rec++;
            if (np == 1 && pr[0] == 0xFF) { n_end++; continue; }
            if (np >= 11) {
                uint8_t code = pr[2];
                /* 本固件仅可能产出 TEMP_START=1 TEMP_STOP=2 BOLUS=5
                 * (及延展/双波 3/4/6); 其余 code 会让 AAPS processMessage 抛异常。 */
                bool known = (code >= 1 && code <= 6);
                if (!known) { bad_code = true; continue; }
                if (code == DANA_HIST_CODE_BOLUS) {
                    uint16_t amt = (uint16_t)((pr[7] << 8) | pr[8]);
                    uint32_t ts  = ((uint32_t)pr[3] << 24) | ((uint32_t)pr[4] << 16) |
                                   ((uint32_t)pr[5] << 8) | pr[6];
                    if (amt == 10 && ts == bolus_ts) found_bolus = true;
                } else if (code == DANA_HIST_CODE_TEMP_START) {
                    uint16_t pct = (uint16_t)((pr[7] << 8) | pr[8]);
                    uint16_t dur = (uint16_t)((pr[9] << 8) | pr[10]);
                    if (pct == 100 && dur == 30) found_tstart = true;
                } else if (code == DANA_HIST_CODE_EXT_START) {
                    uint16_t amt = (uint16_t)((pr[7] << 8) | pr[8]);
                    uint16_t dur = (uint16_t)((pr[9] << 8) | pr[10]);
                    if (amt == 50 && dur == 60) found_ext = true;
                }
            }
        }
        CHECK(!bad_code, "所有 0xC2 记录均为 AAPS 已知 code (无未知 code 致异常)");
        CHECK(found_bolus, "回放含注入的 BOLUS 记录(0.1U, 时间戳正确) ← 修复核心");
        CHECK(found_tstart, "回放含注入的 TEMP_START 记录(100%/30min)");
        CHECK(found_ext, "回放含注入的 EXT_START 记录(0.5U/60min 方波)");
        CHECK(n_end == 1, "回放以 0xFF 结束标记收尾");
    }

    /* ---- 19. P2-9 补全回归: 泵菜单设 TBR 也必须进 0xC2 回放 ----
     * 根因: ui_hal_set_tbr/cancel_tbr 原先只写泵屏历史, 不喂 AAPS 回放缓冲,
     *       → 泵本地菜单设的 TBR AAPS 永远看不到(与 BLE 0x60/0xC1 路径不一致)。
     * 修复: ui_hal_set_tbr/cancel_tbr 经 ui_hal_register_tbr_history_cb 钩子
     *       把 TEMP_START/TEMP_STOP 事件写入 0xC2 回放缓冲。
     * 此处注册"真实记录钩子"(直接落 aaps_dana_record_tbr), 调 ui_hal_set_tbr 模拟
     * 菜单设 120%/45min, 再发 0xC2 回放, 断言 TEMP_START(120%/45min) 真出现在回放里。 */
    printf("\n[19] P2-9 补全: 泵菜单设 TBR 经 ui_hal 钩子进 0xC2 回放\n");
    {
        /* 注册真实记录钩子: 菜单路径的事件直接写入 AAPS 回放缓冲 */
        ui_hal_register_tbr_history_cb([](uint8_t code, uint16_t pct, uint16_t dur) {
            aaps_dana_record_tbr(code, rtc_unix_now(), pct, dur);
        });
        ui_hal_set_tbr(120.0f, 45);        // 模拟菜单设 120% / 45min
        bool found_menu_tstart = false;
        {
            uint8_t from[6] = { 0, 1, 1, 0, 0, 0 };   // load-all
            uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
            int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
            int br = dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND,
                                       DANA_CMD_APS_HISTORY_EVENTS, from, 6, apply_ble5);
            CHECK(br == 0, "0xC2 请求包构建成功(菜单 TBR 回放)");
            aaps_dana_feed_rx_test(pkt, pl);
            aaps_dana_pump();
            uint8_t rx[SIM_PKT_CAP]; size_t rl;
            while ((rl = host_drain_tx(rx, sizeof(rx))) > 0) {
                uint8_t t, op, pr[SIM_PARAM_CAP]; size_t np = 0;
                int ur = dana_unpack_packet(&c_peer, rx, rl, &t, &op, pr, sizeof(pr), &np);
                if (ur != 0) continue;
                if (op != DANA_CMD_APS_HISTORY_EVENTS) continue;
                if (np >= 11 && pr[2] == DANA_HIST_CODE_TEMP_START) {
                    uint16_t pct = (uint16_t)((pr[7] << 8) | pr[8]);
                    uint16_t dur = (uint16_t)((pr[9] << 8) | pr[10]);
                    if (pct == 120 && dur == 45) found_menu_tstart = true;
                }
            }
        }
        CHECK(found_menu_tstart,
              "菜单 ui_hal_set_tbr(120%/45min) → 0xC2 回放含 TEMP_START(120%/45min) ← P2-9 修复核心");
        /* 取消路径同样应记录 TEMP_STOP */
        ui_hal_cancel_tbr();
        bool found_menu_tstop = false;
        {
            uint8_t from[6] = { 0, 1, 1, 0, 0, 0 };
            uint8_t pkt[SIM_PKT_CAP]; size_t pl = 0;
            int apply_ble5 = (c_peer.conn == DANA_CONN_HANDSHAKE_DONE) ? 1 : 0;
            dana_build_packet(&c_peer, pkt, &pl, DANA_TYPE_COMMAND,
                              DANA_CMD_APS_HISTORY_EVENTS, from, 6, apply_ble5);
            aaps_dana_feed_rx_test(pkt, pl);
            aaps_dana_pump();
            uint8_t rx[SIM_PKT_CAP]; size_t rl;
            while ((rl = host_drain_tx(rx, sizeof(rx))) > 0) {
                uint8_t t, op, pr[SIM_PARAM_CAP]; size_t np = 0;
                dana_unpack_packet(&c_peer, rx, rl, &t, &op, pr, sizeof(pr), &np);
                if (op != DANA_CMD_APS_HISTORY_EVENTS) continue;
                if (np >= 3 && pr[2] == DANA_HIST_CODE_TEMP_STOP) found_menu_tstop = true;
            }
        }
        CHECK(found_menu_tstop, "菜单 ui_hal_cancel_tbr() → 0xC2 回放含 TEMP_STOP ← P2-9 修复核心");
        ui_hal_register_tbr_history_cb(nullptr);   // 复位钩子, 避免影响后续交互模式
    }

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

        uint8_t params[SIM_PARAM_CAP];   // 需容纳 0x53 的 96B 参数
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
    aaps_dana_host_link_up_test();   // 模拟 AAPS 已连接 + 已订阅 FFF1, 打开发送门控

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
