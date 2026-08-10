/**
 * rtc_clock.h — 设备时钟 (ESP32 硬件 RTC 域, time()/settimeofday, 可手动/App 设置)
 *
 * 实现见 rtc_clock.cpp (固件独占编译; 模拟器不编此文件)。
 *
 * 说明: ESP32-C6 内置 RTC 域提供 64 位微秒基准, 由芯片 RTC 持续计时
 *   (light/deep sleep 均不停摆), 系统 time()/settimeofday() 直接映射到该硬件 RTC,
 *   返回真实 Unix 秒, 无 millis() 的 49 天回绕问题。
 *   无备份电池: 整机完全掉电后 RTC 域清零, 故开机从 storage 的
 *   g_pump_config.rtc_base_unix 恢复 (rtc_clock_init 时 settimeofday)。
 *   后续若加 DS3231 等外置 RTC 或 SNTP, 只需替换 rtc_clock_init/rtc_unix_now 的实现。
 *
 * 时间基准 (rtc_base_unix) 与"显示/App 设置"统一存放于 g_pump_config, 因此本地 UI
 * 与独立手机 App (自定义 BLE 设置通道) / Dana 0x71 SET_TIME 修改的是同一份数据, 互不冲突。
 */
#pragma once

#include <cstdint>
#include <cstring>

// 初始化: 从 g_pump_config.rtc_base_unix 载入基准, 并记录当前 millis()
//
// ⚠️ 内含"时间下界"保护: 恢复出来的基准若早于固件编译时刻, 一律视为陈旧,
//    改用编译时刻。理由见 rtc_clock_tick 的说明 —— 设备时间不可能早于
//    固件被编译出来的那一刻, 而陈旧基准会让 AAPS 判 |timeDiff| > 1.5h 直接放弃。
void rtc_clock_init(void);

// 当前 Unix 秒; 返回 0 表示"未设置"
uint32_t rtc_unix_now(void);

// 是否已设置时间
bool rtc_is_set(void);

// 设置时间 (同时持久化到 storage)
void rtc_set_unix(uint32_t unix_sec);

// 周期性回写时钟基准 (主循环调用, 内部自带节流)
//
// ⚠️ 为什么必须有它 (2026-08-08):
//    本机无备份电池, 掉电后 RTC 域清零, 开机只能从 g_pump_config.rtc_base_unix 恢复。
//    若该基准只在"用户设置时间"那一刻写入, 每次重启时钟都会**倒退回上次设置的时刻**
//    —— 停机一天就差一天。而 AAPS(DanaRSService.readPumpStatus) 在
//    |timeDiff| > 1.5h 时会 runAlarm(largetimediff) + danaPump.reset() 并直接 return,
//    **不会**下发 0x79 去纠正 → 初始化就此失败、反复重连。
//    故这里每隔一段时间回写一次, 把重启后的时间倒退量限制在该间隔以内。
void rtc_clock_tick(void);

// 日历 <-> Unix 互转 (UTC, 简化格里高利)
//   声明为 inline, 使模拟器后端(ui_hal_sim.cpp)无需链接 rtc_clock.cpp 即可复用,
//   保证固件与模拟器算法完全一致。
static inline bool     rtc_is_leap_year(int y) { return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0); }
static inline int      rtc_days_in_month(int y, int mo)
{
    static const uint16_t dpm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (mo == 2 && rtc_is_leap_year(y)) return 29;
    if (mo >= 1 && mo <= 12) return dpm[mo - 1];
    return 30;
}
static inline uint32_t rtc_ymdhms_to_unix(int y, int mo, int d, int h, int mi, int s)
{
    if (y < 1970) y = 1970;
    if (mo < 1) mo = 1; if (mo > 12) mo = 12;
    uint32_t days = 0;
    for (int yy = 1970; yy < y; yy++) days += rtc_is_leap_year(yy) ? 366 : 365;
    for (int mm = 1; mm < mo; mm++)  days += rtc_days_in_month(y, mm);
    days += (uint32_t)(d > 0 ? d - 1 : 0);
    return days * 86400UL + (uint32_t)h * 3600UL + (uint32_t)mi * 60UL + (uint32_t)s;
}
/* 固件编译时刻 (本机时区墙钟) 的 Unix 秒 —— 作为"时间不可能早于此"的下界。
 * header-only, 固件与模拟器共用; 各翻译单元展开的 __DATE__/__TIME__ 可能差几秒, 无碍。 */
static inline uint32_t rtc_build_time_unix(void)
{
    static const char kMon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;          /* "Aug  8 2026" */
    const char *t = __TIME__;          /* "15:27:31"    */
    int mo = 1;
    for (int i = 0; i < 12; i++)
        if (memcmp(kMon + i * 3, d, 3) == 0) { mo = i + 1; break; }
    int day = (d[4] == ' ' ? 0 : (d[4] - '0') * 10) + (d[5] - '0');
    int yr  = (d[7]-'0')*1000 + (d[8]-'0')*100 + (d[9]-'0')*10 + (d[10]-'0');
    return rtc_ymdhms_to_unix(yr, mo, day,
                              (t[0]-'0')*10 + (t[1]-'0'),
                              (t[3]-'0')*10 + (t[4]-'0'),
                              (t[6]-'0')*10 + (t[7]-'0'));
}

static inline void rtc_unix_to_ymdhms(uint32_t u, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    int year = 1970;
    uint32_t days = u / 86400UL;
    int sod = (int)(u % 86400UL);
    while (days >= (rtc_is_leap_year(year) ? 366U : 365U)) {
        days -= (rtc_is_leap_year(year) ? 366U : 365U); year++;
    }
    int month = 1;
    while (days >= (uint32_t)rtc_days_in_month(year, month)) {
        days -= (uint32_t)rtc_days_in_month(year, month); month++;
    }
    *y = year; *mo = month; *d = (int)days + 1;
    *h = sod / 3600; *mi = (sod % 3600) / 60; *s = sod % 60;
}
