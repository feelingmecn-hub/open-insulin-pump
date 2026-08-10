#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P3-15 无头振动验证: 振动默认关 -> 按键不记录; 设置开启 -> 按键记录 vib_last=1(VIB_KEY)."""
import socket, subprocess, time, json, sys, os

SIM = "/Users/feelingme/pump_sim_build/simulator"
HOST, PORT = "127.0.0.1", 18923

def drain(sock, timeout=1.2):
    sock.settimeout(timeout)
    raw = b""; last = None
    try:
        while True:
            c = sock.recv(65536)
            if not c: break
            raw += c
    except socket.timeout:
        pass
    for line in raw.decode("utf-8","replace").splitlines():
        line = line.strip()
        if line.startswith("{\"t\":\"status\""):
            last = line
    return last

def state_of(line):
    try: return json.loads(line).get("state", {}) if line else None
    except Exception: return None

def send_key(sock, k):
    sock.sendall(("key %s\n" % k).encode())
    if k in ("up","down"): sock.sendall(b"key release\n")
    time.sleep(0.12)
    return drain(sock)

def main():
    os.environ["SIM_HEADLESS"] = "1"
    proc = subprocess.Popen([SIM], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ))
    time.sleep(2.0)
    try:
        sock = socket.create_connection((HOST, PORT), timeout=5)
        time.sleep(0.3)
        st = drain(sock, 1.5)
        s = state_of(st)
        print("INIT: vib_last=%s" % s.get("vib_last"))
        # 1) 振动默认关: 在菜单按一次 down, 不应记录
        s = state_of(send_key(sock, "down"))
        v0 = s.get("vib_last")
        print("after key (vibrate OFF): vib_last=%s (expect 0)" % v0)
        # 回到 home: esc
        send_key(sock, "esc")  # menu -> home
        # 进菜单
        send_key(sock, "set")  # home -> menu sel=0
        for _ in range(6): send_key(sock, "down")   # sel 0->6 (设置)
        send_key(sock, "set")  # menu -> settings sel=0
        for _ in range(6): send_key(sock, "down")   # sel 0->6 (振动反馈)
        send_key(sock, "set")  # 切换振动开 (此时 vibrate 仍关, 该次 key 不记录)
        # 2) 振动已开: 再按一次 down, 应记录 VIB_KEY=1
        s = state_of(send_key(sock, "down"))
        v1 = s.get("vib_last")
        print("after key (vibrate ON):  vib_last=%s (expect 1)" % v1)
        ok = (v0 == 0 and v1 == 1)
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try: sock.close()
        except: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except: proc.kill()

if __name__ == "__main__":
    sys.exit(main())
