#!/usr/bin/env python3
# 验证: 排气开始后, prime_active 是否会在 ~1.2s 后变为 0 (自动结束)
import socket, json, time, subprocess, os, sys

SIM = "/Users/feelingme/pump_sim_build/simulator"
HOST, PORT = "127.0.0.1", 18923

def conn():
    s = socket.create_connection((HOST, PORT), timeout=5); time.sleep(0.2); return s

def recv_latest(s):
    s.settimeout(0.08); buf = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk: break
            buf += chunk
    except socket.timeout:
        pass
    s.settimeout(5)
    lines = [l for l in buf.split(b"\n") if l.strip()]
    return json.loads(lines[-1].decode()) if lines else None

def key(s, k):
    s.sendall(("key " + k + "\n").encode()); time.sleep(0.12); return recv_latest(s)

def ui(x):
    return (x or {}).get("state", {}).get("ui", {})

def read(s):
    s.sendall(b"delay 0\n"); time.sleep(0.12); return recv_latest(s)

# 先杀掉可能残留的旧模拟器, 避免连到陈旧状态 (端口 18923 冲突)
if sys.platform.startswith("win"):
    os.system("taskkill /F /IM simulator.exe >nul 2>nul")
else:
    os.system("pkill -9 -f simulator >/dev/null 2>&1")
time.sleep(1.2)
p = subprocess.Popen([SIM], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     env={**os.environ, "SIM_HEADLESS": "1"})
time.sleep(3)
s = conn()
for _ in range(5): key(s, "esc")          # 规范化到 HOME
r = read(s); print("HOME screen=", ui(r).get("screen"))
key(s, "set")                            # HOME -> MENU
key(s, "down"); key(s, "down")           # sel -> 2 (排气装药)
key(s, "set")                            # MENU -> PRIME
print("在 PRIME 屏, screen=", ui(r).get("screen"))
key(s, "set")                            # 启动排气
print("== 已发送启动排气 ==")

t0 = time.time()
for i in range(12):
    time.sleep(0.3)
    r = read(s)
    u = ui(r)
    print("t=%.1fs prime_active=%s screen=%s prime_u=%.1f" % (
        time.time()-t0, u.get("prime_active"), u.get("screen"), u.get("prime_u")))
s.close()
try:
    p.terminate(); p.wait(timeout=3)
except Exception:
    try: p.kill()
    except Exception: pass
print("DONE")
