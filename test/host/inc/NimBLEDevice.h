/* Host (PC) stub for NimBLE-Arduino — 仅提供 aaps_dana.cpp 编译/链接所需的类与符号。
 *
 * 关键点: 固件 dana_send_raw() 通过 FFF1 特征 setValue()+notify() 把响应包发出来。
 * 本桩把每次 setValue() 的字节追加到全局 TX 流, 由 host_drain_tx() 重组出"完整 Dana 信封"
 * (逻辑与固件 dana_feed_rx 完全对称), 供 AAPS 模拟器侧解析校验。
 *
 * ⚠️ 实验项目, 禁止用于人体。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

/* ---------- TX 捕获 (固件 → AAPS 的响应) ---------- */
void host_tx_push(const uint8_t *data, size_t len);

/* 从已捕获的 TX 流中抽出一个完整 Dana 信封 (A5A5/AAAA 起, 5A5A/EEEE 止)。
 * 返回抽出的字节数 (0 = 当前无可抽出的完整包)。out 容量需 ≥ cap。 */
size_t host_drain_tx(uint8_t *out, size_t cap);

/* ---------- 最小 NimBLE 类型 ---------- */
struct NimBLEConnInfo { /* opaque */ };

/* millis() 由 Arduino 框架在真机提供; 主机测试用 host_glue.cpp 实现。
 * 此声明仅存在于本 HOST 桩头 (真机不使用此文件), 不影响固件。 */
uint32_t millis(void);

enum NIMBLE_PROPERTY : uint32_t {
    READ    = 0x02,
    WRITE   = 0x08,
    NOTIFY  = 0x10,
    WRITE_NR = 0x08,
};

struct NimBLEUUID {
    uint8_t bytes[16];
    NimBLEUUID() { for (size_t i = 0; i < 16; i++) bytes[i] = 0; }
    NimBLEUUID(const uint8_t *b, size_t n) {
        for (size_t i = 0; i < 16; i++) bytes[i] = (i < n) ? b[i] : 0;
    }
};

class NimBLECharacteristicCallbacks {
public:
    virtual ~NimBLECharacteristicCallbacks() {}
    virtual void onWrite(class NimBLECharacteristic *, NimBLEConnInfo &) {}
};

class NimBLEServerCallbacks {
public:
    virtual ~NimBLEServerCallbacks() {}
    virtual void onConnect(class NimBLEServer *, NimBLEConnInfo &) {}
    virtual void onDisconnect(class NimBLEServer *) {}
};

class NimBLECharacteristic {
public:
    void setCallbacks(NimBLECharacteristicCallbacks *) {}
    void setValue(const uint8_t *data, uint16_t len) { host_tx_push(data, (size_t)len); }
    void setValue(const std::string &v) { host_tx_push((const uint8_t *)v.data(), v.size()); }
    void notify() {}
    std::string getValue() { return std::string(); }
};

class NimBLEService {
public:
    NimBLECharacteristic *createCharacteristic(const NimBLEUUID &, uint32_t) {
        return new NimBLECharacteristic();
    }
    void start() {}
};

class NimBLEServer {
public:
    void setCallbacks(NimBLEServerCallbacks *) {}
    NimBLEService *createService(const NimBLEUUID &) { return new NimBLEService(); }
};
