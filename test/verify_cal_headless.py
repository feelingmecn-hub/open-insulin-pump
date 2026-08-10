#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P3-14 无头标定验证：导航到 回退/标定 屏，把实测体积设为 0.90U 并保存，
检查 dose_calibration 从 1.0 -> ~1.111 (1/0.90)。"""
import socket, subprocess, time, json, sys, os

SIM = "/Users/feelingme/pump_sim_build/simulator"
HOST, PORT = "127.0.0.1", 18923
KEY_SEQ = [
    ("set",  "home -> menu"),
    ("down", "menu sel 0->1"),
    ("down", "menu sel 1->2"),
    ("down", "menu sel 2->3"),
    ("down", "menu sel 3->4"),
    ("down", "menu sel 4->5"),
    ("down", "menu sel 5->6"),
    ("down", "menu sel 6->7"),
    ("down", "menu sel 7->8"),
    ("set",  "menu -> rewind_cal (sel=0)"),
    ("down", "rewind_cal sel 0->1"),
    ("down", "rewind_cal sel 1->2 (measured field)"),
    ("set",  "enter edit (set_edit=1)"),
    ("down", "measured 1.00 -> 0.95"),
    ("down", "measured 0.95 -> 0.90"),
    ("set",  "exit edit (set_edit=0)"),
    ("down", "rewind_cal sel 2->3 (apply field)"),
    ("set",  "APPLY calibration (factor=1/0.90)"),
]

def drain(sock, timeout=1.5):
    sock.settimeout(timeout)
    last = None
    raw = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            raw += chunk
    except socket.timeout:
        pass
    # 取出最后一条 status 行
    for line in raw.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if line.startswith("{\"t\":\"status\""):
            last = line
    return last

def state_of(line):
    if not line:
        return None
    try:
        o = json.loads(line)
        return o.get("state", {})
    except Exception:
        return None

def send_key(sock, k):
    # 模拟真实"按下即松开": 立即施加一次, 再发 release 终止自动重复(避免 300ms 后连发)
    sock.sendall(("key %s\n" % k).encode())
    if k in ("up", "down"):
        sock.sendall(b"key release\n")
    time.sleep(0.12)
    return drain(sock)

def main():
    os.environ["SIM_HEADLESS"] = "1"
    proc = subprocess.Popen([SIM], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            env=dict(os.environ))
    time.sleep(2.0)
    try:
        sock = socket.create_connection((HOST, PORT), timeout=5)
        time.sleep(0.3)
        init = drain(sock, 1.5)
        s0 = state_of(init)
        print("INIT: screen=%s sel=%s dose_calibration=%s" % (
            s0.get("ui",{}).get("screen"), s0.get("ui",{}).get("sel"), s0.get("dose_calibration")))
        last_state = s0
        for k, desc in KEY_SEQ:
            st = send_key(sock, k)
            s = state_of(st)
            if s:
                last_state = s
            scr = last_state.get("ui",{}).get("screen") if last_state else "?"
            sel = last_state.get("ui",{}).get("sel") if last_state else "?"
            ed = last_state.get("ui",{}).get("set_edit") if last_state else "?"
            dc = last_state.get("dose_calibration") if last_state else "?"
            print("[%s] %-32s screen=%s sel=%s edit=%s dc=%s" % (k, desc, scr, sel, ed, dc))
        # 静置，再读一次确保最终态
        time.sleep(1.0)
        final = drain(sock, 2.0)
        sf = state_of(final)
        if sf:
            last_state = sf
        scr = last_state.get("ui",{}).get("screen")
        sel = last_state.get("ui",{}).get("sel")
        dc = last_state.get("dose_calibration")
        print("\nRESULT: screen=%s sel=%s dose_calibration=%s" % (scr, sel, dc))
        if dc is not None and abs(dc - 1.11111) < 0.01:
            print("PASS: dose_calibration = %.5f (expected ~1.11111)" % dc)
            return 0
        else:
            print("FAIL: dose_calibration = %s (expected ~1.11111)" % dc)
            return 1
    finally:
        try: sock.close()
        except: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except: proc.kill()

if __name__ == "__main__":
    sys.exit(main())
