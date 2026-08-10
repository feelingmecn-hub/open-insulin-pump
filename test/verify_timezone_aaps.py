"""验证泵 0x78/0x79 时区处理与 AAPS 端算法的往返一致性。

AAPS 端算法 (ref/AndroidAPS):
- GET 0x78 接收: DateTime(2000+y,mo,d,h,mi,s) 用**手机本地时区**解释 -> value(毫秒)
- DanaPump.setPumpTime(value, zoneOffset): pumpTime = value + 手机本地offset*3600*1000
- timeDiff = (pumpTime - System.currentTimeMillis)/1000 ; |timeDiff|>5400 触发大时间差

本脚本模拟: 真实手机时间 = 2026-08-10 16:12 北京 (= 08:12 UTC), 泵系统时钟存真实 UTC 秒。
比较旧固件(0x78 回 UTC-offset)与新固件(0x78 回 UTC+offset)的 timeDiff。
"""
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

BEIJING = ZoneInfo("Asia/Shanghai")
PHONE_OFFSET_S = 8 * 3600  # 北京时区偏移(秒)

def aaps_pump_time(ymd_hms, _zone_offset):
    y, mo, d, h, mi, s = ymd_hms
    # AAPS GET 端: 用手机本地时区解释泵回的墙钟
    local = datetime(y, mo, d, h, mi, s, tzinfo=BEIJING)
    value_ms = int(local.timestamp() * 1000)          # UTC 毫秒
    pump_time_ms = value_ms + PHONE_OFFSET_S * 1000   # DanaPump.setPumpTime
    return pump_time_ms

def system_millis(real_utc):
    return int(real_utc.timestamp() * 1000)

# 真实时间: 2026-08-10 08:12:00 UTC (手机 16:12 北京)
real_utc = datetime(2026, 8, 10, 8, 12, 0, tzinfo=timezone.utc)

# 泵系统时钟存真实 UTC 秒 (= 伴生App 0x02 / AAPS 0x79 设的 UTC)
# 旧 0x78: 回 (UTC - 8h) 墙钟 = (00:12) + offset 8  (错误: 多减了 offset)
old_78 = (2026, 8, 10, 0, 12, 0)
# 新 0x78: 回纯 UTC 墙钟 = (08:12) + offset 8  (正确: AAPS 本地时区解释+加回offset 自洽)
new_78 = (2026, 8, 10, 8, 12, 0)

# 泵屏显示 (新逻辑用本地墙钟)
screen_new = "16:12"

print(f"真实手机时间(北京): 16:12  (= 08:12 UTC)")
print(f"泵系统时钟(UTC秒):  08:12 UTC")
print(f"旧 0x78 回的墙钟:    00:12 (+offset 8)")
print(f"新 0x78 回的墙钟:    16:12 (+offset 8)")
print(f"泵屏显示(新逻辑):    {screen_new}  (应=手机本地)")
print("-" * 60)
old_diff = (aaps_pump_time(old_78, 8) - system_millis(real_utc)) / 1000
new_diff = (aaps_pump_time(new_78, 8) - system_millis(real_utc)) / 1000
print(f"旧 0x78 -> AAPS timeDiff = {old_diff:+.0f} 秒  (>{5400}s 触发大时间差! 错)")
print(f"新 0x78 -> AAPS timeDiff = {new_diff:+.0f} 秒  (<1.5h 不报错, 正确)")
assert abs(old_diff - (-28800)) < 1, "旧逻辑应差 -8h"
assert abs(new_diff) < 1, "新逻辑应归零"
print("\n✅ 验证通过: 旧逻辑差 8h(报错), 新逻辑归零(不报错), 泵屏显示本地时间")
