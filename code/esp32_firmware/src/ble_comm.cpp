/**
 * ble_comm.cpp — BLE GATT 服务 (NimBLE-Arduino)
 *
 * 协议 (与 Android APP / AAPS 对齐, 阶段5 补全):
 *   所有"写"命令统一在 payload 末字节追加 CRC-8/CCITT(poly 0x07, init 0x00)。
 *   - BOLUS : [units_x100 u32 LE][crc]               (5B)  → 大剂量
 *   - BASAL : [rate f32 LE][crc]                       (5B)  → 闭环基础率(AAPS接管)
 *   - TBR   : [percent u8][rate_x100 u16 LE][dur_min u16 LE][crc] (7B) → 临时基础率
 *   - CGM   : [mgdl u16 LE][trend i8][crc]            (5B)  → 手机回传血糖
 *   - CONTROL:[payload u8][crc]                        (2B)  → 0/1/2=设环模式; 0x10=排气(可带ml); 0x11=清报警
 *                                                       0x12=退回; 0x13=标定推出量; 0x14=标定应用系数
 *   通知(泵→手机, 1Hz): SCREEN 推送 20 字节紧凑二进制实时状态(见 notify_pump_state),
 *                       App 解码后自行重画原生虚拟屏, 无需屏镜像/分片/MTU 协商。
 *   远程按键: KEY  [key_event_t u8][crc]            (2B)  → 等同物理按键(ui_screen_key/release)
 */
#include <Arduino.h>
#include <string>
#include <cstring>
#include "ble_comm.h"
#include "config.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "ui_hal.h"   // ui_hal_start_prime / ui_hal_clear_alarm
#include "rtc_clock.h"     // 设置通道: 时间读写
#include "lcd_display.h"   // 设置通道: 背光亮度
#include "history_log.h"   // history_log_event (TBR/排气 事件记录)
#include "dose_log.h"      // BLE 触发的 TBR/大剂量 记入剂量追溯日志
#include "basal_history.h" // P2-10b: BLE 下发的 TBR/环模式 记入基础率执行历史
#include "storage.h"       // storage_save_config (环模式/亮度/方案 持久化)
#include "basal_scheduler.h" // #260 CONTROL 0x18: 基础率验证测试 (全天总量打一次)
#include "ui_screen.h"      // ui_screen_key / ui_screen_release / ui_screen_dump_json（远程按键与镜像屏）

#include <NimBLEDevice.h>
#include "esp_bt.h"             // esp_ble_tx_power_set (省电: BLE 发射功率下调)

#include "esp_task_wdt.h"   // P0-1: 看门狗喂狗

#ifdef USE_AAPS_DANA
#include "aaps_dana.h"   // Dana-i impersonation（方案 B）
#endif

static NimBLEServer        *g_server = nullptr;
static NimBLECharacteristic *g_ch_bolus     = nullptr;
static NimBLECharacteristic *g_ch_basal     = nullptr;
static NimBLECharacteristic *g_ch_tbr       = nullptr;
static NimBLECharacteristic *g_ch_status    = nullptr;
static NimBLECharacteristic *g_ch_iob       = nullptr;
static NimBLECharacteristic *g_ch_reservoir = nullptr;
static NimBLECharacteristic *g_ch_cgm       = nullptr;
static NimBLECharacteristic *g_ch_control   = nullptr;
static NimBLECharacteristic *g_ch_settings  = nullptr;   // 独立 App 设置通道
static NimBLECharacteristic *g_ch_key       = nullptr;   // 远程按键 (WRITE, 伴生 App → 泵)
static NimBLECharacteristic *g_ch_screen    = nullptr;   // 泵屏镜像 (NOTIFY/READ, 泵 → 伴生 App)

static const uint8_t U_SVC[16]   = BLE_SERVICE_PUMP_UUID;
static const uint8_t U_BOLUS[16] = BLE_CHAR_BOLUS_UUID;
static const uint8_t U_BASAL[16] = BLE_CHAR_BASAL_UUID;
static const uint8_t U_TBR[16]   = BLE_CHAR_TBR_UUID;
static const uint8_t U_STATUS[16]= BLE_CHAR_STATUS_UUID;
static const uint8_t U_IOB[16]   = BLE_CHAR_IOB_UUID;
static const uint8_t U_RES[16]   = BLE_CHAR_RESERVOIR_UUID;
static const uint8_t U_CGM[16]   = BLE_CHAR_CGM_UUID;
static const uint8_t U_CONTROL[16]=BLE_CHAR_CONTROL_UUID;
static const uint8_t U_SETTINGS[16]=BLE_CHAR_SETTINGS_UUID;
static const uint8_t U_KEY[16]    = BLE_CHAR_KEY_UUID;
static const uint8_t U_SCREEN[16] = BLE_CHAR_SCREEN_UUID;

// 校验: payload 长度为 len, 末字节为 CRC
static bool pkt_ok(const std::string &v, size_t payload_len)
{
    if (v.size() != payload_len + 1) return false;
    uint8_t crc = crc8_ccitt((const uint8_t *)v.data(), payload_len);
    return crc == (uint8_t)v[payload_len];
}

// 紧凑二进制实时状态推送（替代旧的大 JSON 屏镜像 + 分片方案）：
// 单通知 20 字节 ≤ ATT MTU(23) 载荷上限(20B)，无需分片，App 收到即解码并重画原生虚拟屏。
// 布局（小端，共 20 字节，须与 Android 端 PumpProtocol.parsePumpState 严格对齐）：
//  [0]  magic 0xA1
//  [1]  flags1: b0-3=current_state(0-15)  b4-5=loop_mode(0-3)  b6=keypad_locked  b7=alarm_active
//  [2]  battery_pct (0-100)
//  [3]  alarm_code (alarm_code_t)
//  [4]  glucose_trend + 128 (int8 偏移存储, -2..2 → 126..130; 解码时 -128)
//  [5]  tbr_percent (0=无, 否则百分比)
//  [6]  flags2: b0=ext_bolus_active  b1=step_loss_detected
//  [7]  bolus_progress_pct (0-100)
//  [8-9]  reservoir_units_left × 10        (u16, U×10)
// [10-11] iob_x10000 / 100                (u16, U×100)
// [12-13] current_basal_rate × 100        (u16, U/h ×100)
// [14-15] last_glucose_mgdl               (u16, mg/dL, 0=无)
// [16-17] today_units_x100                (u16, U×100)
// [18-19] clock = HH×60 + MM              (u16, 本地时)
static void notify_pump_state(void)
{
    if (!g_ch_screen) return;
    uint8_t b[20];
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    rtc_unix_to_ymdhms(rtc_unix_now(), &y, &mo, &d, &h, &mi, &s);

    uint8_t flags1 = (uint8_t)(g_pump_state.current_state & 0x0F)
                   | (uint8_t)((g_pump_state.loop_mode & 0x03) << 4)
                   | (uint8_t)(((g_pump_state.keypad_locked ? 1 : 0)) << 6)
                   | (uint8_t)(((g_pump_state.alarm_active ? 1 : 0)) << 7);
    uint8_t flags2 = (uint8_t)((g_pump_state.ext_bolus_active ? 1 : 0))
                   | (uint8_t)(((g_pump_state.step_loss_detected ? 1 : 0)) << 1);
    uint16_t res_x10    = (uint16_t)(g_pump_state.reservoir_units_left * 10u);
    uint16_t iob_x100   = (uint16_t)(g_pump_state.iob_x10000 / 100u);
    uint16_t basal_x100 = (uint16_t)(g_pump_state.current_basal_rate * 100.0f);
    uint16_t glucose    = g_pump_state.last_glucose_mgdl;
    uint16_t today_x100 = (uint16_t)(g_pump_state.today_units_x100 & 0xFFFFu);
    uint16_t clock      = (uint16_t)(h * 60u + mi);

    b[0]  = 0xA1;
    b[1]  = flags1;
    b[2]  = g_pump_state.battery_pct;
    b[3]  = g_pump_state.alarm_code;
    b[4]  = (uint8_t)((int)g_pump_state.glucose_trend + 128);
    b[5]  = (uint8_t)(g_pump_state.tbr_percent > 255 ? 255 : g_pump_state.tbr_percent);
    b[6]  = flags2;
    b[7]  = g_pump_state.bolus_progress_pct;
    b[8]  = (uint8_t)(res_x10 & 0xFF);         b[9]  = (uint8_t)((res_x10 >> 8) & 0xFF);
    b[10] = (uint8_t)(iob_x100 & 0xFF);        b[11] = (uint8_t)((iob_x100 >> 8) & 0xFF);
    b[12] = (uint8_t)(basal_x100 & 0xFF);      b[13] = (uint8_t)((basal_x100 >> 8) & 0xFF);
    b[14] = (uint8_t)(glucose & 0xFF);         b[15] = (uint8_t)((glucose >> 8) & 0xFF);
    b[16] = (uint8_t)(today_x100 & 0xFF);      b[17] = (uint8_t)((today_x100 >> 8) & 0xFF);
    b[18] = (uint8_t)(clock & 0xFF);           b[19] = (uint8_t)((clock >> 8) & 0xFF);

    g_ch_screen->setValue(b, sizeof(b));   // (uint8_t*, size) 重载按长度发送，二进制安全
    g_ch_screen->notify();                 // 未订阅时 NimBLE 自动忽略
}

// 推送当前泵屏导航状态（屏幕/选中/编辑值）给伴生 App，供其复刻可交互虚拟屏。
// 13 字节二进制单包（≤ MTU 载荷上限），无需分片；未订阅时 NimBLE 自动忽略。
static void notify_pump_nav(void)
{
    if (!g_ch_screen) return;
    uint8_t b[13];
    ui_screen_dump_nav_binary(b, sizeof(b));
    g_ch_screen->setValue(b, sizeof(b));
    g_ch_screen->notify();
}

class SrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        g_pump_state.ble_connected = true;
        ui_hal_mark_activity();   // 省电: 手机连接唤醒屏幕
        Serial.println("[BLE] connected");
    }
    void onDisconnect(NimBLEServer* p, NimBLEConnInfo& info, int reason) override {
        (void)info; (void)reason;
        g_pump_state.ble_connected = false;
        g_pump_state.dana_paired  = false;   // 断开视为失去 AAPS 接管
        p->startAdvertising();   // 断连后重新广播（NimBLE 内偶发静默失败，ble_task 有兜底）
        Serial.printf("[BLE] disconnected reason=%d -> startAdvertising()\n", reason);
    }
};

class ChCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        ui_hal_mark_activity();   // 省电: 任意手机指令都视为活动(唤醒屏幕)
        if (c == g_ch_bolus && pkt_ok(v, 4)) {
            uint32_t ux100 = *(uint32_t *)v.data();
            motor_command_t cmd{};
            cmd.type = MOTOR_CMD_BOLUS;
            cmd.units_x100 = ux100;
            motor_enqueue(&cmd);

        } else if (c == g_ch_basal && pkt_ok(v, 4)) {
            // 伴生 App 直推瞬时基础率。
            // ⚠️ 2026-08-08 修复: 旧代码只写 g_pump_state.current_basal_rate,
            //    而该字段是调度器每拍**回写**的显示量 —— 下一拍就被覆盖回档案值,
            //    等于这条指令根本不生效(表现为"设了基础率电机不动")。
            //    现改写入 basal_override: 调度器读它作为限时覆盖,
            //    BASAL_OVERRIDE_TIMEOUT_MS 后自动回落 24 段档案 (防手机失联后永久跑偏)。
            float r = *(float *)v.data();
            if (r < 0.0f) r = 0.0f;
            if (r > MAX_BASAL_RATE) r = MAX_BASAL_RATE;
            g_pump_state.basal_override_uh    = r;
            g_pump_state.basal_override_ms    = millis();
            g_pump_state.basal_override_valid = 1;
            g_pump_state.current_basal_rate   = r;   // 立即反映到屏幕, 无需等下一拍
            {
                uint8_t ap = g_pump_config.active_profile; if (ap >= MAX_BASAL_PROFILES) ap = 0;
                basal_history_record(BH_BASAL_ACTIVE, ap, g_pump_state.loop_mode, 0,
                                     (uint16_t)(r * 100.0f + 0.5f));
            }

        } else if (c == g_ch_tbr && pkt_ok(v, 5)) {
            uint8_t  percent   = (uint8_t)v[0];
            uint16_t rate_x100 = *(uint16_t *)(v.data() + 1);
            uint16_t dur_min   = *(uint16_t *)(v.data() + 3);
            g_pump_state.tbr_percent    = percent;
            g_pump_state.tbr_rate       = (float)rate_x100 / 100.0f;
            g_pump_state.tbr_expiry_ms  = millis() + (uint32_t)dur_min * 60000UL;
            history_log_event(EVENT_TYPE_TBR, ALARM_NONE,
                             (uint32_t)rate_x100, dur_min);
            dose_log_append(EVENT_TYPE_TBR, (uint32_t)rate_x100, dur_min, DOSE_FLAG_SRC_BLE);
            // 执行历史: TBR 开始 (BLE 来源)
            uint8_t ap = g_pump_config.active_profile; if (ap >= MAX_BASAL_PROFILES) ap = 0;
            basal_history_record(BH_TBR_START, ap, g_pump_state.loop_mode,
                                 (uint16_t)((uint16_t)percent * 10u), rate_x100);

        } else if (c == g_ch_cgm && pkt_ok(v, 3)) {
            uint16_t mgdl = *(uint16_t *)v.data();
            int8_t   tr   = (int8_t)v[2];
            g_pump_state.last_glucose_mgdl = mgdl;
            g_pump_state.glucose_trend     = tr;   // 伴生 App 直发 5 档显示码 (-2..2)
            g_pump_state.last_glucose_time_unix = rtc_unix_now();  // 标记接收时刻

        } else if (c == g_ch_control) {
            // 控制通道：动作类指令（写 → 泵）。帧格式 [op u8][可选 f32 参数][crc]。
            // 伴生 App 在「设置」页直接下发这些指令，无需模拟物理按键。
            if (v.empty()) return;
            uint8_t b = (uint8_t)v[0];
            if (b <= 2) {                                   // 环模式 0/1/2
                if (!pkt_ok(v, 1)) return;
                if (b != g_pump_state.loop_mode) {          // 仅模式真正变化才记录
                    g_pump_state.loop_mode = b;
                    // ⚠️ 环模式必须持久化, 否则重启后 pump_state_init() 一律回 0(闭环),
                    //    表现为"App 里明明关了, 泵屏还显示闭环中"。
                    g_pump_config.loop_mode_pref = b;
                    storage_save_config(&g_pump_config);    // 只标脏, loop() 去抖落盘 (见 #263)
                    uint8_t ap = g_pump_config.active_profile; if (ap >= MAX_BASAL_PROFILES) ap = 0;
                    basal_history_record(BH_MODE_CHANGE, ap, b, 0,
                                         (uint16_t)(g_pump_state.current_basal_rate * 100.0f));
                }
            } else if (b == 0x10) {                          // 排气装药：默认 1.0U，可带 ml 参数
                if (pkt_ok(v, 1)) {
                    ui_hal_start_prime(1.0f);
                } else if (pkt_ok(v, 5)) {
                    float ml = *(const float *)(v.data() + 1);
                    if (ml > 0.0f && ml <= 200.0f) ui_hal_start_prime(ml);
                }
            } else if (b == 0x11) {                          // 清报警
                if (!pkt_ok(v, 1)) return;
                ui_hal_clear_alarm();
            } else if (b == 0x12) {                          // 退回装药（回退活塞到原点，装新储药器）
                if (!pkt_ok(v, 1)) return;
                ui_hal_rewind();
            } else if (b == 0x13) {                          // 标定：推出测试量 (units f32)
                if (!pkt_ok(v, 5)) return;
                float u = *(const float *)(v.data() + 1);
                if (u < 0.1f) u = 1.0f;
                if (u > 100.0f) u = 100.0f;
                ui_hal_calibrate_dispense(u);
                history_log_event(EVENT_TYPE_CALIBRATE, ALARM_NONE, (uint32_t)(u * 100.0f), 0);
                dose_log_append(EVENT_TYPE_CALIBRATE, (uint32_t)(u * 100.0f), 0, DOSE_FLAG_SRC_BLE);
            } else if (b == 0x14) {                          // 标定：保存系数 (factor f32)
                if (!pkt_ok(v, 5)) return;
                float f = *(const float *)(v.data() + 1);
                ui_hal_apply_calibration(f);
                history_log_event(EVENT_TYPE_CALIBRATE, ALARM_NONE, 0, 1);   // sub=1 表示"应用系数"
                dose_log_append(EVENT_TYPE_CALIBRATE, 0, 1, DOSE_FLAG_SRC_BLE);
            } else if (b == 0x15) {                          // 手动电机控制: [dir u8][steps u32 LE][speed u16 LE]
                if (!pkt_ok(v, 7)) return;
                uint8_t  dir   = (uint8_t)v[1] ? MOTOR_DIR_REVERSE : MOTOR_DIR_FORWARD;
                uint32_t steps = *(const uint32_t *)(v.data() + 2);   // steps=0 → 连续点动
                uint16_t speed = *(const uint16_t *)(v.data() + 6);
                if (speed > MOTOR_MAX_SPEED_HZ) speed = MOTOR_MAX_SPEED_HZ;
                ui_hal_manual_move(dir, steps, speed);
            } else if (b == 0x16) {                          // 手动停止 (连续点动退出)
                if (!pkt_ok(v, 1)) return;
                ui_hal_manual_stop();
            } else if (b == 0x18) {                          // #260 基础率验证测试
                // 把当前激活方案 24 段总量一次性打出, 用于验证"基础率到底有没有写进泵、
                // 电机会不会动"。历史记为 EVENT_TYPE_BASAL_TEST, 不计入大剂量/IOB。
                if (!pkt_ok(v, 1)) return;
                float u = basal_scheduler_run_daily_test();
                Serial.printf("[BASAL-TEST] daily total -> %.2fU\n", u);
            }
            notify_pump_state();                            // 立即把最新状态推给 App
            notify_pump_nav();
        } else if (c == g_ch_key && pkt_ok(v, 1)) {
            // 远程按键: 写入 [key_event_t u8][crc]; 0=松开, 1..6=见 pump_types.h
            uint8_t k = (uint8_t)v[0];
            if (k == 0) {
                ui_screen_release();               // 松手: 停止自动重复
            } else if (k >= (uint8_t)KEY_UP && k <= (uint8_t)KEY_LONG_ESC) {
                ui_screen_key((key_event_t)k);     // 等同物理按键 (泵屏必然同步)
            }
            notify_pump_state();                   // 立即把最新状态推给 App (虚拟屏同步)
            notify_pump_nav();                     // 立即把导航状态推给 App (可交互菜单同步)
        } else if (c == g_ch_settings) {
            // 设置通道 (独立伴生 App): [op u8][payload...][crc]
            if (v.size() < 2) return;
            if (!pkt_ok(v, v.size() - 1)) return;   // crc 校验末字节
            uint8_t  op   = (uint8_t)v[0];
            size_t   plen = v.size() - 1;           // op + params 长度
            std::string resp;
            switch (op) {
                case 0x01: { // GET_TIME → u32 Unix (0=未设置)
                    uint32_t u = rtc_unix_now();
                    resp.assign((const char *)&u, 4);
                    break;
                }
                case 0x02: { // SET_TIME → u32 Unix
                    if (plen != 5) { resp.push_back(1); break; }
                    uint32_t u = *(const uint32_t *)(v.data() + 1);
                    rtc_set_unix(u);
                    resp.push_back(0);
                    break;
                }
                case 0x03: resp.push_back(g_pump_config.display_brightness); break; // GET_BRIGHTNESS
                case 0x04: { // SET_BRIGHTNESS → u8
                    if (plen != 2) { resp.push_back(1); break; }
                    uint8_t b = (uint8_t)v[1]; if (b > 100) b = 100;
                    g_pump_config.display_brightness = b;
                    lcd_display_backlight(b);
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x05: resp.push_back(g_pump_config.keypad_sound); break;   // GET_KEYPAD
                case 0x06: { // SET_KEYPAD → u8
                    if (plen != 2) { resp.push_back(1); break; }
                    g_pump_config.keypad_sound = (uint8_t)v[1] ? 1 : 0;
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x10: resp.push_back(g_pump_config.active_profile); break; // GET_ACTIVE_PROFILE
                case 0x11: { // SET_ACTIVE_PROFILE → u8
                    if (plen != 2) { resp.push_back(1); break; }
                    uint8_t p = (uint8_t)v[1]; if (p >= MAX_BASAL_PROFILES) p = 0;
                    g_pump_config.active_profile = p;
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x12: { // GET_DIA_MIN → u16
                    uint16_t dia = (uint16_t)(IOB_DURATION_HOURS * 60.0f);
                    resp.assign((const char *)&dia, 2);
                    break;
                }
                case 0x13: { // SET_DIA_MIN —— DIA 由编译期宏固定, 运行时不可改
                    resp.push_back(1);
                    break;
                }
                // ---- 显示 / 用户设置 ----
                case 0x07: resp.push_back(g_pump_config.vibrate_enabled); break;   // GET_VIBRATE
                case 0x08: { // SET_VIBRATE → u8
                    if (plen != 2) { resp.push_back(1); break; }
                    g_pump_config.vibrate_enabled = (uint8_t)v[1] ? 1 : 0;
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x09: { // GET_PASSKEY → u32
                    uint32_t pk = g_pump_config.ble_passkey;
                    resp.assign((const char *)&pk, 4);
                    break;
                }
                case 0x0A: { // SET_PASSKEY → u32
                    if (plen != 5) { resp.push_back(1); break; }
                    g_pump_config.ble_passkey = *(const uint32_t *)(v.data() + 1);
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                // ---- 基础率方案（名称 + 24 段）----
                case 0x14: { // GET_PROFILE_NAME → profile_idx u8 → name[32]
                    if (plen != 2) { resp.push_back(1); break; }
                    uint8_t pi = (uint8_t)v[1]; if (pi >= MAX_BASAL_PROFILES) { resp.push_back(1); break; }
                    resp.assign(g_pump_config.profiles[pi].name,
                                sizeof(g_pump_config.profiles[pi].name));
                    break;
                }
                case 0x15: { // SET_PROFILE_NAME → profile_idx u8 + name bytes
                    if (plen < 2) { resp.push_back(1); break; }
                    uint8_t pi = (uint8_t)v[1]; if (pi >= MAX_BASAL_PROFILES) { resp.push_back(1); break; }
                    size_t nb = plen - 1;           // 名称字节数 (不含 op/profile_idx)
                    if (nb > 31) nb = 31;
                    memcpy(g_pump_config.profiles[pi].name, v.data() + 2, nb);
                    g_pump_config.profiles[pi].name[nb] = '\0';
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x16: { // GET_PROFILE_SLOT → profile_idx u8, hour u8 → f32 rate
                    if (plen != 3) { resp.push_back(1); break; }
                    uint8_t pi = (uint8_t)v[1]; uint8_t hh = (uint8_t)v[2];
                    if (pi >= MAX_BASAL_PROFILES || hh >= BASAL_SLOTS_PER_DAY) { resp.push_back(1); break; }
                    float r = g_pump_config.profiles[pi].slots[hh].rate_uh;
                    resp.append((const char *)&r, 4);
                    break;
                }
                case 0x17: { // SET_PROFILE_SLOT → profile_idx u8, hour u8, f32 rate
                    if (plen != 7) { resp.push_back(1); break; }
                    uint8_t pi = (uint8_t)v[1]; uint8_t hh = (uint8_t)v[2];
                    if (pi >= MAX_BASAL_PROFILES || hh >= BASAL_SLOTS_PER_DAY) { resp.push_back(1); break; }
                    float r = *(const float *)(v.data() + 3);
                    if (r < 0.0f) r = 0.0f;
                    if (r > MAX_BASAL_RATE) r = MAX_BASAL_RATE;
                    g_pump_config.profiles[pi].slots[hh].rate_uh = r;
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                // ---- 大剂量 / 安全限制 ----
                case 0x20: { // GET_LIMITS → max_bolus_single / per_hour / max_basal (3×f32, 12B)
                    resp.append((const char *)&g_pump_config.max_bolus_single, 4);
                    resp.append((const char *)&g_pump_config.max_bolus_per_hour, 4);
                    resp.append((const char *)&g_pump_config.max_basal_per_hour, 4);
                    break;
                }
                case 0x21: { // SET_LIMIT → which u8 (0=single 1=per_hour 2=max_basal), f32
                    if (plen != 6) { resp.push_back(1); break; }
                    uint8_t which = (uint8_t)v[1];
                    float lv = *(const float *)(v.data() + 2);
                    if (which == 0) g_pump_config.max_bolus_single = (lv < 0.0f) ? 0.0f : lv;
                    else if (which == 1) g_pump_config.max_bolus_per_hour = (lv < 0.0f) ? 0.0f : lv;
                    else if (which == 2) g_pump_config.max_basal_per_hour = (lv < 0.0f) ? 0.0f : lv;
                    else { resp.push_back(1); break; }
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                // ---- 安全参数 ----
                case 0x22: { // GET_SAFETY → occlusion u16, watchdog u8, over_temp f32 (7B)
                    uint16_t occ = g_pump_config.occlusion_threshold;
                    resp.append((const char *)&occ, 2);
                    resp.push_back((uint8_t)g_pump_config.watchdog_timeout_sec);
                    resp.append((const char *)&g_pump_config.over_temp_threshold_c, 4);
                    break;
                }
                case 0x23: { // SET_SAFETY → which u8 (0=occlusion 1=watchdog 2=over_temp), value
                    if (plen != 3 && plen != 6) { resp.push_back(1); break; }
                    uint8_t which = (uint8_t)v[1];
                    if (which == 0) {                     // occlusion u16
                        if (plen != 4) { resp.push_back(1); break; }
                        uint16_t occ = *(const uint16_t *)(v.data() + 2);
                        g_pump_config.occlusion_threshold = occ;
                    } else if (which == 1) {              // watchdog u8
                        if (plen != 3) { resp.push_back(1); break; }
                        g_pump_config.watchdog_timeout_sec = (uint8_t)v[2];
                    } else if (which == 2) {              // over_temp f32
                        if (plen != 6) { resp.push_back(1); break; }
                        g_pump_config.over_temp_threshold_c = *(const float *)(v.data() + 2);
                    } else { resp.push_back(1); break; }
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                // ---- 闭环参数（逐时）----
                case 0x24: { // GET_CL_PARAM → kind u8 (0=isf 1=carb 2=target), hour u8 → f32
                    if (plen != 3) { resp.push_back(1); break; }
                    uint8_t kind = (uint8_t)v[1]; uint8_t hh = (uint8_t)v[2];
                    if (hh >= 24) { resp.push_back(1); break; }
                    float val;
                    if (kind == 0) val = g_pump_config.isf[hh];
                    else if (kind == 1) val = g_pump_config.carb_ratio[hh];
                    else if (kind == 2) val = (float)g_pump_config.target_glucose[hh];
                    else { resp.push_back(1); break; }
                    resp.append((const char *)&val, 4);
                    break;
                }
                case 0x25: { // SET_CL_PARAM → kind u8, hour u8, f32
                    if (plen != 7) { resp.push_back(1); break; }
                    uint8_t kind = (uint8_t)v[1]; uint8_t hh = (uint8_t)v[2];
                    if (hh >= 24) { resp.push_back(1); break; }
                    float val = *(const float *)(v.data() + 3);
                    if (kind == 0) g_pump_config.isf[hh] = val < 0.0f ? 0.0f : val;
                    else if (kind == 1) g_pump_config.carb_ratio[hh] = val < 0.0f ? 0.0f : val;
                    else if (kind == 2) g_pump_config.target_glucose[hh] =
                        (uint16_t)(val < 0.0f ? 0.0f : val);
                    else { resp.push_back(1); break; }
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                // ---- 剂量标定系数 ----
                case 0x26: { // GET_CALIBRATION → f32
                    float cf = g_pump_config.dose_calibration;
                    resp.append((const char *)&cf, 4);
                    break;
                }
                case 0x27: { // SET_CALIBRATION → f32（等同 CONTROL 0x14）
                    if (plen != 5) { resp.push_back(1); break; }
                    float cf = *(const float *)(v.data() + 1);
                    ui_hal_apply_calibration(cf);
                    resp.push_back(0);
                    break;
                }
                // ---- 省电: 空闲自动熄屏 ----
                case 0x28: { // GET_AUTO_DIM → [u8 enabled][u16 timeout_le]
                    resp.push_back((uint8_t)g_pump_config.auto_dim_enabled);
                    uint16_t t = g_pump_config.auto_dim_timeout_s;
                    resp.push_back((uint8_t)(t & 0xFF));
                    resp.push_back((uint8_t)((t >> 8) & 0xFF));
                    break;
                }
                case 0x29: { // SET_AUTO_DIM → [u8 enabled][u16 timeout_le]
                    if (plen != 4) { resp.push_back(1); break; }   // op + u8 + u16 + crc
                    g_pump_config.auto_dim_enabled = (uint8_t)v[1] ? 1 : 0;
                    uint16_t t = (uint16_t)((uint8_t)v[2] | ((uint16_t)(uint8_t)v[3] << 8));
                    if (t < 5) t = 5;            // 下限保护, 避免 0 导致闪烁
                    if (t > 600) t = 600;
                    g_pump_config.auto_dim_timeout_s = t;
                    storage_save_config(&g_pump_config);
                    resp.push_back(0);
                    break;
                }
                case 0x2A: { // GET_MOTOR_POSITION → u32 (当前电机微步位置, 电机测试用)
                    uint32_t pos = g_pump_state.motor_position;
                    resp.assign((const char *)&pos, 4);
                    break;
                }
                default: resp.push_back(1); break;   // 未知 op → ERR
            }
            g_ch_settings->setValue(resp);
        }
    }
};

static SrvCb srvCb;
static ChCb  chCb;

void ble_init(void)
{
    // 不调用 NimBLEDevice::setMTU(): 实测 NimBLE-Arduino 3.1.1 的 setMTU() 在 init() 前调用
    // 会触发 Load access fault 崩溃, 在 init() 后调用又导致后续 adv->start() 静默失败 → 泵不广播。
    // 因此状态推送直接控制在 20 字节以内(单通知载荷上限), 无需分片、无需 MTU 协商。
#ifdef USE_AAPS_DANA
    NimBLEDevice::init(DANAI_DEVICE_NAME);   // AAPS 按设备名正则识别 Dana-i
#else
    NimBLEDevice::init(BLE_DEVICE_NAME);
#endif
    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&srvCb);

    #ifndef DISABLE_COMPANION
    NimBLEService *svc = g_server->createService(NimBLEUUID((uint8_t*)U_SVC, 16));

    g_ch_bolus = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_BOLUS, 16),
                                           NIMBLE_PROPERTY::WRITE);
    g_ch_bolus->setCallbacks(&chCb);
    g_ch_basal = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_BASAL, 16),
                                           NIMBLE_PROPERTY::WRITE);
    g_ch_basal->setCallbacks(&chCb);
    g_ch_tbr = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_TBR, 16),
                                         NIMBLE_PROPERTY::WRITE);
    g_ch_tbr->setCallbacks(&chCb);
    g_ch_cgm = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_CGM, 16),
                                         NIMBLE_PROPERTY::WRITE);
    g_ch_cgm->setCallbacks(&chCb);
    g_ch_control = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_CONTROL, 16),
                                             NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ);
    g_ch_control->setCallbacks(&chCb);

    // 独立伴生 App 设置通道 (与 AAPS/Dana 互不干扰, 始终可用)
    g_ch_settings = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_SETTINGS, 16),
                                              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_settings->setCallbacks(&chCb);

    // 远程按键通道 (伴生 App → 泵): 写入 [key_event_t u8][crc], 等同物理按键
    g_ch_key = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_KEY, 16),
                                         NIMBLE_PROPERTY::WRITE);
    g_ch_key->setCallbacks(&chCb);

    // 实时状态通道 (泵 → 伴生 App): 订阅后按 20 字节二进制推送实时状态 (notify_pump_state),
    // App 端 PumpProtocol.parsePumpState 解码后自行重画原生虚拟屏, 无需屏镜像/分片。
    g_ch_screen = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_SCREEN, 16),
                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_screen->setCallbacks(&chCb);

    g_ch_status = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_STATUS, 16),
                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_iob = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_IOB, 16),
                                         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_reservoir = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_RES, 16),
                                               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    svc->start();
    #endif // DISABLE_COMPANION

#ifdef USE_AAPS_DANA
    // 挂载 Dana-i FFF0/FFF1/FFF2 服务（与自定义 BLE 服务并存于同一 Server）
    aaps_dana_attach(g_server);
#endif

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    // 广播包(Advertising Data)上限 31 字节, 两个服务 UUID 无法同时放进广播包:
    //   FFF0 是 16-bit(占 4B), 6E400001 是 128-bit(占 18B); 任一叠加设备名 + flags 即逼近上限。
    // 策略(⚠️顺序敏感, 见下方注释):
    //   ① 设备名 / FFF0       → 进【广播包】  (AAPS 按广播包里的设备名正则 DAN12345AB 识别 Dana-i)
    //   ② 6E400001 128-bit    → 进【扫描响应包 Scan Response】(额外 31B, 伴生 App 靠它发现)
    //
    // ⚠️ 致命顺序陷阱: NimBLE 的 setName() 一旦 enableScanResponse(true) 已生效, 会把设备名
    //    塞进【扫描响应】而非广播包 (NimBLEAdvertising.cpp:519-523)。名字一旦跑到扫描响应,
    //    AAPS 在广播阶段读不到设备名 → 无法按正则识别 → 连接失败。
    //    故必须: 先 setName() → 再 enableScanResponse(true) → FFF0 → 最后 6E400001(128-bit 因
    //    广播包放不下自动溢入扫描响应, 与 m_scanResp 无关, 仅取决于容量)。
#ifdef USE_AAPS_DANA
    adv->setName(DANAI_DEVICE_NAME);             // ← 必须在 enableScanResponse 之前: 名字进广播包
#else
    adv->setName(BLE_DEVICE_NAME);
#endif
    adv->enableScanResponse(true);              // 之后 6E400001 会溢入扫描响应并真正发出
#ifdef USE_AAPS_DANA
    adv->addServiceUUID(NimBLEUUID((uint16_t)0xFFF0));     // FFF0 16-bit → 广播包 (flags3+name12+4=19B<31)
#endif
    adv->addServiceUUID(NimBLEUUID((uint8_t*)U_SVC, 16));  // 6E400001 128-bit → 广播放不下(19+18>31) → 自动进扫描响应
#ifdef USE_AAPS_DANA
    Serial.printf("[BLE] advData=%u scanData=%u (AAPS 应在广播包看到名字+FFF0, 扫描响应看到 6E400001)\n",
                  adv->getAdvertisementData().getPayload().size(),
                  adv->getScanData().getPayload().size());
#endif
    adv->start();
    // 省电: BLE 发射功率温和下调 (默认约 +9dBm → 0dBm). 体戴距离足够, 范围略降但显著省电.
    // 若连接变弱/断续, 删此行即可恢复默认功率.
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N0);
    Serial.println("[BLE] advertising start called");
}

void ble_task(void *arg)
{
    static uint32_t last_adv_kick = 0;
    for (;;) {
        esp_task_wdt_reset();   // P0-1: 喂狗
        if (g_pump_state.ble_connected) {
            // 紧凑二进制实时状态推送（替代旧的大 JSON + 分片屏镜像）：
            // 单通知 20 字节 ≤ MTU 载荷上限，App 收到即解码重画虚拟屏，无需分片。
            notify_pump_state();
            // 导航状态推送（屏幕/选中/编辑值），App 据此复刻可交互菜单。
            notify_pump_nav();

            // 控制特征值回读当前环模式 (供手机查询)
            if (g_ch_control) g_ch_control->setValue(&g_pump_state.loop_mode, 1);
        }

        // ★ 广播保活 (2026-08-08 根因修复)：必须【无论是否已有连接】都保持可发现。
        //   旧逻辑只在断连时才重启广播 → 一旦华为系统对已配对设备占住一条连接，
        //   泵就永久停止广播；AAPS 的 connectGatt 走白名单发起(需扫到广播才建链)，
        //   于是永远连不上，30s 超时报 GATT_ERROR(133)，UI 卡在「正在连接」。
        //   NimBLE 支持最多 3 条连接，持续广播才能让 AAPS 建立自己的那一条。
        {
            uint32_t now = (uint32_t)millis();
            if (now - last_adv_kick > 3000) {
                NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
                if (adv && !adv->isAdvertising()) {
                    adv->start();
                    Serial.printf("[BLE] adv keepalive restart (connected=%d)\n",
                                  g_pump_state.ble_connected ? 1 : 0);
                }
                last_adv_kick = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
