/**
 * aaps_dana_test.cpp — aaps_dana 纯逻辑宿主单测（g++ 直接编译，无需 ESP32）
 *
 * 验证目标：
 *   1) 信封构建 / 解析在握手与命令阶段均可往返一致；
 *   2) 200 个随机命令包 build+parse 往返无失真；
 *   3) 与 oracle_aaps.py（AAPS BleEncryption 逐字转译）对相同场景输出的
 *      over-the-air 字节流完全一致（由 run_tests.sh diff 校验）。
 *
 * 编译：g++ -std=c++17 -I../src aaps_dana_test.cpp ../src/aaps_dana.cpp -o aaps_dana_test
 * ⚠️ USE_AAPS_DANA 不定义 → 仅编译纯逻辑，ESP32 胶水层被排除。
 */
#include "aaps_dana.h"
#include <stdio.h>
#include <string.h>

static uint32_t g_s = 0x12345678u;
static uint32_t lcg(void) { g_s = g_s * 1103515245u + 12345u; return g_s; }

static int    g_failures = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; }   \
        else        { printf("ok:   %s\n", msg); }                  \
    } while (0)

static void hexprint(const char *name, const uint8_t *b, size_t n)
{
    printf("PKT %s ", name);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

int main(void)
{
    dana_ctx_t c;
    dana_ctx_init(&c, "DAN12345AB", "123456", 0x09, 0x0A);
    uint8_t buf[DANA_MAX_PACKET];
    size_t  bl;

    /* 场景 1: PUMP_CHECK 响应（握手，无二级加密） */
    CHECK(dana_build_pump_check_response(&c, buf, &bl) == 0, "build PUMP_CHECK resp");
    hexprint("PUMP_CHECK_RESP", buf, bl);
    c.conn = DANA_CONN_INIT;
    uint8_t t = 0, o = 0, p[16]; size_t np = 0;
    CHECK(dana_parse_packet(&c, buf, bl, &t, &o, p, 16, &np) == 0, "parse PUMP_CHECK resp");
    CHECK(t == DANA_TYPE_ENC_REQ && o == DANA_OP_PUMP_CHECK, "PUMP_CHECK type/opcode");
    CHECK(np == 12 && p[0] == 'O' && p[1] == 'K' && p[3] == 0x09 && p[5] == 0x0A,
          "PUMP_CHECK payload (OK/HW_MODEL/PROTOCOL)");
    CHECK(p[6] == '1' && p[7] == '2' && p[8] == '3' && p[9] == '4' && p[10] == '5' && p[11] == '6',
          "PUMP_CHECK ble5key echoed");

    /* 场景 2: TIME_INFORMATION 响应（握手，无二级加密） */
    CHECK(dana_build_time_info_response(&c, buf, &bl) == 0, "build TIME_INFO resp");
    hexprint("TIME_INFO_RESP", buf, bl);
    c.conn = DANA_CONN_INIT;
    CHECK(dana_parse_packet(&c, buf, bl, &t, &o, p, 16, &np) == 0, "parse TIME_INFO resp");
    CHECK(t == DANA_TYPE_ENC_REQ && o == DANA_OP_TIME_INFO, "TIME_INFO type/opcode");
    CHECK(np == 2 && p[0] == 'O' && p[1] == 'K', "TIME_INFO payload (OK)");

    /* 场景 3: 命令响应 0x4A（命令阶段，BLE5 二级加密） */
    uint8_t p3[1] = { 0x00 };
    CHECK(dana_build_packet(&c, buf, &bl, DANA_TYPE_RESPONSE, DANA_CMD_STEP_BOLUS_START,
                            p3, 1, 1) == 0, "build BOLUS resp (BLE5)");
    hexprint("CMD_BOLUS_RESP", buf, bl);
    c.conn = DANA_CONN_HANDSHAKE_DONE;   // 命令阶段
    CHECK(dana_parse_packet(&c, buf, bl, &t, &o, p, 16, &np) == 0, "parse BOLUS resp (BLE5)");
    CHECK(t == DANA_TYPE_RESPONSE && o == DANA_CMD_STEP_BOLUS_START && np == 1 && p[0] == 0,
          "BOLUS resp roundtrip");

    /* 场景 4: NOTIFY 大剂量进度 0x02（命令阶段，BLE5） */
    uint8_t p4[2] = { 0x2C, 0x01 };   // 已输注 300 = 3.00U
    CHECK(dana_build_packet(&c, buf, &bl, DANA_TYPE_NOTIFY, DANA_NOTIFY_DELIVERY_RATE,
                            p4, 2, 1) == 0, "build NOTIFY rate (BLE5)");
    hexprint("NOTIFY_RATE", buf, bl);
    c.conn = DANA_CONN_HANDSHAKE_DONE;
    CHECK(dana_parse_packet(&c, buf, bl, &t, &o, p, 16, &np) == 0, "parse NOTIFY rate (BLE5)");
    CHECK(t == DANA_TYPE_NOTIFY && o == DANA_NOTIFY_DELIVERY_RATE &&
          np == 2 && p[0] == 0x2C && p[1] == 0x01, "NOTIFY rate roundtrip");

    /* 场景 5: 手机→泵命令 0xC1（命令阶段，BLE5）build + parse 往返 */
    uint8_t p5[3] = { (uint8_t)(150 & 0xFF), (uint8_t)(150 >> 8), 150 };
    CHECK(dana_build_packet(&c, buf, &bl, DANA_TYPE_COMMAND, DANA_CMD_APS_TBR,
                            p5, 3, 1) == 0, "build TBR cmd (BLE5)");
    hexprint("CMD_TBR_PHONE", buf, bl);
    c.conn = DANA_CONN_HANDSHAKE_DONE;
    CHECK(dana_parse_packet(&c, buf, bl, &t, &o, p, 16, &np) == 0, "parse TBR cmd (BLE5)");
    CHECK(t == DANA_TYPE_COMMAND && o == DANA_CMD_APS_TBR &&
          np == 3 && p[0] == p5[0] && p[1] == p5[1] && p[2] == p5[2],
          "TBR cmd roundtrip (pct/durCode)");

    /* 模糊测试：200 个随机命令包 build + parse 往返 */
    int fuzz_fail = 0;
    for (int i = 0; i < 200; i++) {
        uint8_t n   = (uint8_t)(lcg() % 9);
        uint8_t fp[8];
        for (uint8_t j = 0; j < n; j++) fp[j] = (uint8_t)(lcg() & 0xFF);
        uint8_t ty = (uint8_t)(lcg() & 0xFF);
        uint8_t op = (uint8_t)(lcg() & 0xFF);
        if (dana_build_packet(&c, buf, &bl, ty, op, fp, n, 1) != 0) { fuzz_fail++; continue; }
        c.conn = DANA_CONN_HANDSHAKE_DONE;
        uint8_t fp2[8]; size_t np2; uint8_t t2, o2;
        if (dana_parse_packet(&c, buf, bl, &t2, &o2, fp2, 8, &np2) != 0) { fuzz_fail++; continue; }
        if (t2 != ty || o2 != op || np2 != n) { fuzz_fail++; continue; }
        for (uint8_t j = 0; j < n; j++) if (fp2[j] != fp[j]) { fuzz_fail++; break; }
    }
    CHECK(fuzz_fail == 0, "fuzz 200 command packets build+parse roundtrip");

    /* 黄金字节流（与 oracle_aaps.py 对齐）：重置种子后复现同一 LCG 序列 */
    g_s = 0x12345678u;
    for (int i = 0; i < 200; i++) {
        uint8_t n   = (uint8_t)(lcg() % 9);
        uint8_t fp[8];
        for (uint8_t j = 0; j < n; j++) fp[j] = (uint8_t)(lcg() & 0xFF);
        uint8_t ty = (uint8_t)(lcg() & 0xFF);
        uint8_t op = (uint8_t)(lcg() & 0xFF);
        dana_build_packet(&c, buf, &bl, ty, op, fp, n, 1);
        printf("PKT FUZZ%03d ", i);
        for (size_t k = 0; k < bl; k++) printf("%02x", buf[k]);
        printf("\n");
    }

    if (g_failures == 0) printf("ALL_ASSERTS_PASS\n");
    else                 printf("SOME_ASSERTS_FAILED %d\n", g_failures);
    return g_failures ? 1 : 0;
}
