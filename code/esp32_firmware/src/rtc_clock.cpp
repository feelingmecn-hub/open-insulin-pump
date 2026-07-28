/**
 * rtc_clock.cpp — 设备时钟实现 (见 rtc_clock.h)
 *
 * P2 改造 (2026-07-28): 改用 ESP32 硬件 RTC 域时钟, 取代原先 millis() 累加方案。
 *
 *   旧方案缺陷:
 *     - millis() 是 32 位毫秒计数器, 约 49.7 天回绕归零, 回绕瞬间时间倒退;
 *     - 进入 deep/light sleep 时 millis() 停摆, 唤醒后时间严重偏差;
 *     - 本质是"开机相对计时", 非真实日历时钟。
 *
 *   新方案:
 *     - ESP32 内置 RTC 域提供 64 位微秒基准, 由芯片 RTC 持续计时
 *       (light/deep sleep 均不停摆), 系统 time()/settimeofday() 直接映射
 *       到该硬件 RTC, 返回真实 Unix 秒, 无 49 天回绕。
 *     - 仍无备份电池: 整机完全掉电后 RTC 域清零, 故开机从 storage 的
 *       g_pump_config.rtc_base_unix 恢复 (rtc_clock_init 时 settimeofday),
 *       运行中每次 rtc_set_unix 持续持久化同一份基准。
 *     - 与本地设置 / 独立手机 App / Dana 0x71 SET_TIME 共用同一入口
 *       rtc_set_unix, 写入的是同一份 g_pump_config, 互不冲突。
 *
 *   注: ESP32 RTC 晶振存在 ppm 级温漂, 长期运行可能有秒级偏差, 对教学原型可接受;
 *       若需高精度, 可加 NTP(SNTP) 或 DS3231 外置 RTC, 仅替换本文件实现即可。
 */
#include "rtc_clock.h"
#include "config.h"
#include "pump_state.h"   // g_pump_config (持久化基准)
#include "storage.h"
#include <time.h>         // time(), time_t
#include <sys/time.h>     // settimeofday(), struct timeval

// ---- 内部状态 ----
static bool s_set = false;   // 是否已设置过有效时间 (硬件 RTC 已写入基准)

// 将系统时钟 (硬件 RTC 域) 设为指定 Unix 秒
#if defined(AAPS_DANA_HOST_TEST)
/* 主机联调：普通用户无 root, settimeofday 会 EPERM 静默失败, 时间无法真正写入系统时钟,
 * 导致固件 rtc_set_unix 看似成功、GET_TIME 却读到真实墙钟。为保证 0x70/0x71 往返一致
 * (仅联调验证用, 不进真机), 这里用内存中的"模拟 RTC"。 */
static uint32_t s_host_unix = 0;
static void sys_set_unix(uint32_t unix_sec) { s_host_unix = unix_sec; }
uint32_t rtc_unix_now(void)
{
    if (!s_set) return 0;             // 未设置 → 调用方据此判"时间无效"
    return s_host_unix;
}
#else
static void sys_set_unix(uint32_t unix_sec)
{
    struct timeval tv;
    tv.tv_sec  = (time_t)unix_sec;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

uint32_t rtc_unix_now(void)
{
    if (!s_set) return 0;             // 未设置 → 调用方据此判"时间无效"
    time_t t = time(NULL);
    if (t < (time_t)1000000000) return 0;   // 2001-09 之前视为未校准, 防异常值
    return (uint32_t)t;
}
#endif

void rtc_clock_init(void)
{
    // 开机从持久化基准恢复 (无备份电池时, 这是唯一时间来源)
    if (g_pump_config.rtc_base_unix != 0) {
        sys_set_unix(g_pump_config.rtc_base_unix);
        s_set = true;
    } else {
        s_set = false;   // 等待用户在设置页 / App / Dana 0x71 设定
    }
}

bool rtc_is_set(void)
{
    return s_set;
}

void rtc_set_unix(uint32_t unix_sec)
{
    if (unix_sec < 1000000000U) return;     // 拒绝明显非法值 (2001 年前)
    sys_set_unix(unix_sec);
    s_set = true;
    g_pump_config.rtc_base_unix = unix_sec;
    storage_save_config(&g_pump_config);    // 持久化, 掉电前/重启后恢复
}

// 日历 <-> Unix 互转见 rtc_clock.h (inline, 与模拟器共用)
