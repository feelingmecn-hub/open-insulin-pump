/*
 * Minimal Arduino.h stub for the host (SDL) simulator build.
 *
 * The real ESP32 Arduino core pulls in <freertos/FreeRTOS.h> (via Arduino.h),
 * which does not exist on a plain macOS/Linux host. The shared firmware
 * sources compiled into the simulator (ui_screen.cpp, history_log.cpp,
 * basal_history.cpp, dose_log.cpp, and in LINK mode aaps_dana.cpp /
 * rtc_clock.cpp) only reference a tiny surface of the Arduino API, and the
 * heavyweight ESP-only parts (Preferences / esp_partition / NimBLE) are all
 * guarded behind #ifndef SIMULATOR. So we provide just enough here.
 *
 * This file is placed ahead of any real Arduino core in the include path so
 * that `#include <Arduino.h>` resolves to the stub, never to the ESP32 core.
 */
#ifndef ARDUINO_H_STUB
#define ARDUINO_H_STUB

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <thread>

/* ---- Common Arduino typedefs (harmless on host) ---- */
typedef uint8_t  byte;
typedef bool     boolean;

/* ---- Logic-level constants ---- */
#ifndef LOW
#define LOW   0
#endif
#ifndef HIGH
#define HIGH  1
#endif
#ifndef INPUT
#define INPUT  0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif

/* ---- Timing ---- */
inline uint32_t millis()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline uint64_t micros()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void delay(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline void delayMicroseconds(uint32_t us)
{
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

/* ---- yield() / optimistic_yield() no-ops on host ---- */
inline void yield() {}

/* ---- Serial: 固件源码里大量 Serial.printf/println 调试输出 ----
 * 主机构建把它接到 stdout，但**默认静默**：联调脚本的断言输出才是主角，
 * 几百行 [DANA] 日志会把 PASS/FAIL 淹没。需要看时置环境变量 DANA_VERBOSE=1。 */
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

class HostSerialStub {
    static bool verbose() {
        static const bool v = (std::getenv("DANA_VERBOSE") != nullptr);
        return v;
    }
public:
    void begin(unsigned long = 0) {}
    void flush() {}
    explicit operator bool() const { return true; }
    int printf(const char *fmt, ...) {
        if (!verbose()) return 0;
        va_list ap; va_start(ap, fmt);
        int n = std::vprintf(fmt, ap);
        va_end(ap);
        return n;
    }
    void print(const char *s)   { if (verbose()) std::printf("%s", s); }
    void println(const char *s) { if (verbose()) std::printf("%s\n", s); }
    void println()              { if (verbose()) std::printf("\n"); }
};

inline HostSerialStub Serial;   /* C++17 inline 变量：多 TU 包含也只有一个实例 */

#endif /* ARDUINO_H_STUB */
