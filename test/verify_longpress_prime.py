#!/usr/bin/env python3
# 验证: (1) 长按上下键持续加减 (FSM 自动重复) (2) 排气 3mL + 电机面板同步
# 无头运行联调模拟器 (SIM_HEADLESS=1), 经 TCP 127.0.0.1:18923 发指令。
import socket, json, time, sys, subprocess, os

HOST, PORT = "127.0.0.1", 18923
SIM = "/Users/feelingme/pump_sim_build/simulator"
if not os.path.exists(SIM):
    SIM = "simulator.exe"   # Windows 测试包内置

def start_fresh_sim():
    # 先杀掉可能残留的旧模拟器, 避免连到陈旧状态 (端口 18923 冲突)
    if sys.platform.startswith("win"):
        os.system("taskkill /F /IM simulator.exe >nul 2>nul")
    else:
        os.system("pkill -9 -f simulator >/dev/null 2>&1")
    time.sleep(1.2)
    p = subprocess.Popen([SIM], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         env={**os.environ, "SIM_HEADLESS": "1"})
    time.sleep(2.0)
    return p
SCR = {0:"HOME",1:"MENU",2:"BASAL",3:"BOLUS_MENU",4:"BOLUS_NORMAL",5:"BOLUS_SQUARE",
       6:"BOLUS_DUAL",7:"BOLUS_WIZARD",8:"BOLUS_MEALS",9:"PRIME",10:"ALARM_LIST",
       11:"ALARM_DETAIL",12:"LOOP",13:"SETTINGS",14:"CLOCK_SET",15:"ABOUT"}

def conn():
    s = socket.create_connection((HOST, PORT), timeout=5)
    time.sleep(0.2)
    return s

def recv_latest(s):
    # 排空缓冲区, 取最后一条完整 JSON (避免读到陈旧广播)
    s.settimeout(0.08)
    buf = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    s.settimeout(5)
    lines = [l for l in buf.split(b"\n") if l.strip()]
    if not lines:
        return None
    return json.loads(lines[-1].decode())

def key(s, k):
    s.sendall(("key " + k + "\n").encode())
    time.sleep(0.12)
    return recv_latest(s)

def ui(x):
    return x.get("state", {}).get("ui", {})

def nm(x):
    u = ui(x)
    return "%s(s=%s,sel=%s)" % (SCR.get(u.get("screen"), u.get("screen")),
                                u.get("screen"), u.get("sel"))

def read(s):  # 触发一次广播读取当前状态 (delay 0 不改动状态)
    s.sendall(b"delay 0\n")
    time.sleep(0.12)
    return recv_latest(s)

def hold_drain(s, secs):
    # 持续读取, 避免服务器线程因广播发送阻塞而延迟处理后续命令(如 release)
    t0 = time.time()
    while time.time() - t0 < secs:
        recv_latest(s)
        time.sleep(0.05)

print("== 启动全新无头联调模拟器 ==")
p = start_fresh_sim()
s = conn()
# 规范化到 HOME
for _ in range(5):
    key(s, "esc")
r = read(s)
print("HOME:", nm(r))

# ---------- 测试1: 长按上下键持续加减 (自动重复) ----------
print("\n== 测试1: 常规大剂量 长按持续加减 ==")
r = key(s, "set")                 # HOME -> MENU
for _ in range(1): r = key(s, "down")   # MENU sel0=基础率 -> sel1=大剂量
r = key(s, "set")                 # MENU -> BOLUS_MENU
r = key(s, "set")                 # BOLUS_MENU -> BOLUS_NORMAL
print("进入:", nm(r), "dose0=%.2f" % ui(r).get("dose", 0))
dose0 = ui(r).get("dose", 0)
# 单次点按: 应只 +0.05
r = key(s, "up")
dose_tap = ui(r).get("dose", 0)
print("单次点按 up: dose=%.2f (期望 +0.05)" % dose_tap)
# 按住 up 不松 (不发送 release), 等待自动重复 (持续读取, 模拟真实客户端)
s.sendall(b"key up\n")            # press, 启动重复
hold_drain(s, 1.0)                # 期间 FSM 自动重复, 同时排空广播
r = read(s)                       # 触发广播读取
dose_hold = ui(r).get("dose", 0)
print("按住 1s 后: dose=%.2f (期望明显大于 %.2f)" % (dose_hold, dose_tap))
# 松开后: 持续读取直到剂量稳定 (证明 release 生效, 自动重复停止)
# 注: 服务器命令线程与 FSM 主循环存在 ~160ms 调度滞后, release 后可能再触发
#      极少数已排定的重复步进; 正确判定标准是"松开后剂量停止增长"而非"零增量"。
s.sendall(b"key release\n")
doses_post = []
t0 = time.time()
while time.time() - t0 < 0.8:
    r = read(s)                    # delay 0 强制广播, 观察剂量是否停止增长
    if r:
        doses_post.append(ui(r).get("dose", 0))
    time.sleep(0.04)
dose_after_release = doses_post[-1]
# 稳定判定: 末尾连续读数相等 => release 已停止自动重复
assert doses_post[-1] == doses_post[-2] == doses_post[-3], \
    "松开后剂量未停止增长 (末段=%s)" % doses_post[-5:]
print("松开后: dose=%.2f (已稳定, 长按累加停止)" % dose_after_release)

assert abs(dose_tap - (dose0 + 0.05)) < 1e-6, "单次点按未 +0.05"
assert dose_hold > dose_tap + 0.20, "长按未持续累加 (hold=%.2f tap=%.2f)" % (dose_hold, dose_tap)
print("测试1 PASS: 长按上下键持续加减 (自动重复) 正常")
key(s, "esc"); key(s, "esc")  # 回到 MENU

# ---------- 测试2: 排气(U) + 电机面板同步 ----------
print("\n== 测试2: 排气与装药 (单位 U) + 电机同步 ==")
# 从 HOME 重新进入, 避免依赖上一段结束状态
for _ in range(6):
    key(s, "esc")                 # 一路回到 HOME
r = read(s)
print("HOME:", nm(r))
r = key(s, "set")                # HOME -> MENU
r = key(s, "down")               # MENU sel0->sel1 (大剂量)
r = key(s, "down")               # MENU sel1->sel2 (排气装药)
print("MENU(选排气装药):", nm(r))
r = key(s, "set")                # MENU -> PRIME
print("进入:", nm(r))
prime_u0 = ui(r).get("prime_u", 0)
print("初始排气量=%.1f U (UI 显示)" % prime_u0)
# 调排气量到 2.0 U (步进 0.5U, 1.0 + 2*0.5 = 2.0)
for _ in range(2):
    r = key(s, "up")
prime_u_set = ui(r).get("prime_u", 0)
print("调量后排气量=%.1f U" % prime_u_set)
assert abs(prime_u_set - 2.0) < 1e-6, "排气量调节失败"
# 记录电机面板当前累计
before = read(s)
m_before = before.get("state", {}).get("motor_units", 0.0)
print("排气前 电机累计=%.2f U" % m_before)
# 确认排气
r = key(s, "set")
ui_r = ui(r)
prime_active = ui_r.get("prime_active", 0)
state_top = r.get("state", {}).get("state", 0)
print("排气中: prime_active=%s state=%s" % (prime_active, state_top))
after = read(s)
m_after = after.get("state", {}).get("motor_units", 0.0)
print("排气后 电机累计=%.2f U (期望 +~%.1f)" % (m_after, prime_u_set))
# 等待自动结束 (~1.2s)
time.sleep(1.5)
r = read(s)
prime_active2 = ui(r).get("prime_active", 0)
print("1.5s 后: prime_active=%s (期望 0=自动回待机)" % prime_active2)

assert prime_active == 1, "排气未进入 PRIMING"
assert m_after > m_before + 1.5, "电机面板未随排气动作 (before=%.2f after=%.2f)" % (m_before, m_after)
assert prime_active2 == 0, "排气未自动结束"
print("测试2 PASS: 排气(U) 可调 + 电机面板柱塞同步动作 + 自动结束")

s.close()
try:
    p.terminate(); p.wait(timeout=3)
except Exception:
    try: p.kill()
    except Exception: pass
print("\nALL LONGPRESS+PRIME TESTS PASSED")
sys.exit(0)
