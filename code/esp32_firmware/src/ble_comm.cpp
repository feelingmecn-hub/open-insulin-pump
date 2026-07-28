/**
 * ble_comm.cpp — BLE GATT 服务 (NimBLE-Arduino)
 *
 * 协议 (与 Android APP / AAPS 对齐, 阶段5 补全):
 *   所有"写"命令统一在 payload 末字节追加 CRC-8/CCITT(poly 0x07, init 0x00)。
 *   - BOLUS : [units_x100 u32 LE][crc]               (5B)  → 大剂量
 *   - BASAL : [rate f32 LE][crc]                       (5B)  → 闭环基础率(AAPS接管)
 *   - TBR   : [percent u8][rate_x100 u16 LE][dur_min u16 LE][crc] (7B) → 临时基础率
 *   - CGM   : [mgdl u16 LE][trend i8][crc]            (5B)  → 手机回传血糖
 *   - CONTROL:[payload u8][crc]                        (2B)  → 0/1/2=设环模式; 0x10=排气; 0x11=清报警
 *   通知(泵→手机, 1Hz): status / iob / reservoir, status JSON 含 glu/tr/loop/tbr。
 */
#include "ble_comm.h"
#include "config.h"
#include "pump_state.h"
#include "motor_controller.h"
#include "ui_hal.h"   // ui_hal_start_prime / ui_hal_clear_alarm
#include "rtc_clock.h"     // 设置通道: 时间读写
#include "lcd_display.h"   // 设置通道: 背光亮度

#include <NimBLEDevice.h>

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

// 校验: payload 长度为 len, 末字节为 CRC
static bool pkt_ok(const std::string &v, size_t payload_len)
{
    if (v.size() != payload_len + 1) return false;
    uint8_t crc = crc8_ccitt((const uint8_t *)v.data(), payload_len);
    return crc == (uint8_t)v[payload_len];
}

class SrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        g_pump_state.ble_connected = true;
    }
    void onDisconnect(NimBLEServer* p) override {
        g_pump_state.ble_connected = false;
        g_pump_state.dana_paired  = false;   // 断开视为失去 AAPS 接管
        p->startAdvertising();   // 断连后重新广播
    }
};

class ChCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();

        if (c == g_ch_bolus && pkt_ok(v, 4)) {
            uint32_t ux100 = *(uint32_t *)v.data();
            motor_command_t cmd{0};
            cmd.type = MOTOR_CMD_BOLUS;
            cmd.units_x100 = ux100;
            motor_enqueue(&cmd);

        } else if (c == g_ch_basal && pkt_ok(v, 4)) {
            float r = *(float *)v.data();
            g_pump_state.current_basal_rate = r;   // 闭环基础率 (AAPS 接管)

        } else if (c == g_ch_tbr && pkt_ok(v, 5)) {
            uint8_t  percent   = (uint8_t)v[0];
            uint16_t rate_x100 = *(uint16_t *)(v.data() + 1);
            uint16_t dur_min   = *(uint16_t *)(v.data() + 3);
            g_pump_state.tbr_percent    = percent;
            g_pump_state.tbr_rate       = (float)rate_x100 / 100.0f;
            g_pump_state.tbr_expiry_ms  = millis() + (uint32_t)dur_min * 60000UL;
            history_log_event(EVENT_TYPE_TBR, ALARM_NONE,
                             (uint32_t)rate_x100, dur_min);

        } else if (c == g_ch_cgm && pkt_ok(v, 3)) {
            uint16_t mgdl = *(uint16_t *)v.data();
            int8_t   tr   = (int8_t)v[2];
            g_pump_state.last_glucose_mgdl = mgdl;
            g_pump_state.glucose_trend     = tr;   // 伴生 App 直发 5 档显示码 (-2..2)
            g_pump_state.last_glucose_time_unix = rtc_unix_now();  // 标记接收时刻

        } else if (c == g_ch_control && pkt_ok(v, 1)) {
            uint8_t b = (uint8_t)v[0];
            if (b <= 2) {
                g_pump_state.loop_mode = b;        // 0/1/2
                storage_save_config(&g_pump_config);
            } else if (b == 0x10) {
                ui_hal_start_prime();              // 远程触发排气
            } else if (b == 0x11) {
                ui_hal_clear_alarm();              // 远程清报警
            }
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
#ifdef USE_AAPS_DANA
    NimBLEDevice::init(DANAI_DEVICE_NAME);   // AAPS 按设备名正则识别 Dana-i
#else
    NimBLEDevice::init(BLE_DEVICE_NAME);
#endif
    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&srvCb);

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

    g_ch_status = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_STATUS, 16),
                                            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_iob = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_IOB, 16),
                                         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_ch_reservoir = svc->createCharacteristic(NimBLEUUID((uint8_t*)U_RES, 16),
                                               NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    svc->start();

#ifdef USE_AAPS_DANA
    // 挂载 Dana-i FFF0/FFF1/FFF2 服务（与自定义 BLE 服务并存于同一 Server）
    aaps_dana_attach(g_server);
    static const uint8_t U_FFF0[16] = {0x00,0x00,0xff,0xf0, 0x00,0x00,0x10,0x00, 0x80,0x00, 0x00,0x80,0x5f,0x9b,0x34,0xfb};
#endif

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NimBLEUUID((uint8_t*)U_SVC, 16));
#ifdef USE_AAPS_DANA
    adv->addServiceUUID(NimBLEUUID((uint8_t*)U_FFF0, 16));   // AAPS 据此发现 Dana 服务
    adv->setName(DANAI_DEVICE_NAME);
#else
    adv->setName(BLE_DEVICE_NAME);
#endif
    adv->start();
}

void ble_task(void *arg)
{
    for (;;) {
        if (g_pump_state.ble_connected) {
            char buf[64];
            // 状态 notify 扩充: 血糖/趋势/环模式/临时基础率
            snprintf(buf, sizeof(buf),
                     "{\"bat\":%d,\"st\":%d,\"alm\":%d,\"glu\":%d,\"tr\":%d,\"loop\":%d,\"tbr\":%d}",
                     g_pump_state.battery_pct, g_pump_state.current_state, g_pump_state.alarm_code,
                     g_pump_state.last_glucose_mgdl, (int)g_pump_state.glucose_trend,
                     g_pump_state.loop_mode, (int)g_pump_state.tbr_percent);
            if (g_ch_status) { g_ch_status->setValue(buf); g_ch_status->notify(); }

            snprintf(buf, sizeof(buf), "%.2f", g_pump_state.iob_x10000 / 10000.0f);
            if (g_ch_iob) { g_ch_iob->setValue(buf); g_ch_iob->notify(); }

            snprintf(buf, sizeof(buf), "%d", g_pump_state.reservoir_units_left);
            if (g_ch_reservoir) { g_ch_reservoir->setValue(buf); g_ch_reservoir->notify(); }

            // 控制特征值回读当前环模式 (供手机查询)
            if (g_ch_control) g_ch_control->setValue(&g_pump_state.loop_mode, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
