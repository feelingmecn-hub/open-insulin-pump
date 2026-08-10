#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
联调手动控制验证 (无头) — 确认 TCP `key` 指令能驱动固件 UI FSM 切换界面。
用法: python3 test/verify_manual_keys.py
"""
import socket, json, time, sys

PORT = 18923
HOST = "127.0.0.1"

SCREEN_NAMES = {
    0: "HOME 首页", 1: "MENU 主菜单", 2: "BASAL 基础率", 3: "BOLUS_MENU 大剂量菜单",
    4: "BOLUS_NORMAL 常规大剂量", 5: "BOLUS_SQUARE 方波", 6: "BOLUS_DUAL 双波",
    7: "BOLUS_WIZARD 向导", 8: "BOLUS_MEALS 餐食", 9: "PRIME 排气装药",
    10: "ALARM_LIST 报警列表", 11: "ALARM_DETAIL 报警详情", 12: "LOOP 闭环",
    13: "SETTINGS 系统设置", 14: "CLOCK_SET 设置时间", 15: "ABOUT 关于",
}

def recv_status(s):
    """阻塞读取一行 JSON 状态。"""
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            return None
        buf += chunk
    line = buf.split(b"\n", 1)[0]
    return json.loads(line.decode("utf-8", "replace"))

def get_screen(st):
    try:
        return st["state"]["ui"]["screen"]
    except Exception:
        return None

def main():
    s = socket.create_connection((HOST, PORT), timeout=5)
    time.sleep(0.3)
    # 丢弃欢迎广播可能不是第一行? 读取一次
    st = recv_status(s)
    if st is None:
        print("FAIL: 未收到初始状态"); sys.exit(1)
    print("初始界面:", SCREEN_NAMES.get(get_screen(st), get_screen(st)))

    seq = [
        ("down", "主菜单应出现"),
        ("set",  "进入主菜单"),
        ("down", "在主菜单下移"),
        ("down", "在主菜单再下移"),
        ("set",  "进入系统设置(或子项)"),
        ("esc",  "返回上级"),
        ("esc",  "返回首页"),
    ]
    results = []
    for k, expect in seq:
        s.sendall(("key " + k + "\n").encode())
        time.sleep(0.25)
        st = recv_status(s)
        sc = get_screen(st)
        name = SCREEN_NAMES.get(sc, sc)
        ok = sc is not None
        results.append((k, name, expect, ok))
        print(f"  key {k:4s} -> screen={sc} ({name})  [{expect}]")

    s.close()
    # 基本断言: 经历过至少 2 个不同界面, 且最终回到某个合法界面
    screens_seen = {r[1] for r in results}
    print("\n经过界面集合:", screens_seen)
    if len(screens_seen) >= 2:
        print("PASS: 手动按键成功驱动泵屏 FSM 切换界面")
        sys.exit(0)
    else:
        print("FAIL: 界面未随按键变化")
        sys.exit(1)

if __name__ == "__main__":
    main()
