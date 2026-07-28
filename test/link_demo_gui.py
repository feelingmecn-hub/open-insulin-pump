#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
link_demo_gui.py — AAPS 蓝牙动态控制 · 联调同步演示控制面板 (四宫格)

连接 LVGL SDL 泵模拟器 (SIM_LINK_MODE) 起的 TCP 控制通道 127.0.0.1:18923,
实时四路同步演示:
  ┌─────────────┬──────────────────────────────┐
  │ ① 步进电机  │ ② AAPS 发送数据调试窗        │
  │   推药演示  │   (AAPS -> 泵 的原始 BLE 包)  │
  ├─────────────┼──────────────────────────────┤
  │ ③ 固件接收  │ ④ 胰岛素泵屏幕 UI 实时画面   │
  │   指令调试  │   (canvas 复刻 320x172 横屏) │
  └─────────────┴──────────────────────────────┘
  + 左侧 17 步会话列表 (高亮当前 + PASS/FAIL 着色)

控制通道协议 (来自 simulator/lvgl_sdl/src/link_ipc.cpp):
  接收: {"t":"status","idx":N,"total":M,"playing":bool,
         "steps":[...],"trace":[...],"state":{...}}
        {"t":"reset","total":M}
  发送: "play" / "pause" / "step" / "reset" / "delay <ms>"

⚠️ 实验项目 / 教学原型, 严禁用于任何人体。真机仅可用空注射器/水验证。
"""
import json
import math
import os
import socket
import subprocess
import sys
import threading
import time

import tkinter as tk
from tkinter import ttk, font, messagebox

HOST = "127.0.0.1"
PORT = 18923
SIM_BINARY = "/Users/feelingme/pump_sim_build/simulator"
BUILD_SCRIPT = "/Users/feelingme/Desktop/闭环胰岛素泵项目/test/build_sim_link.sh"

# 电机几何 (与固件 config.h / dosing.h 一致, 仅用于演示标度)
STEPS_PER_UNIT = 2178.0          # 1U ≈ 2178 微步
MM_PER_STEP = 0.5 / 6400.0       # 导程 0.5mm / 6400 微步/转
MAX_VIS_U = 6.0                  # 演示用柱塞可视上限 (会话累计 ~5U)

TITLE_FONT = None
CJK = ["PingFang SC", "Heiti SC", "Microsoft YaHei", "WenQuanYi Micro Hei",
       "Noto Sans CJK SC", "Arial Unicode MS", "Arial"]
MONO = ["Menlo", "Monaco", "DejaVu Sans Mono", "Courier New", "Courier"]


def pick_font(size, bold=False):
    for f in CJK:
        try:
            font.Font(family=f, size=size)
            return (f, size, "bold" if bold else "normal")
        except Exception:
            continue
    return ("TkDefaultFont", size, "bold" if bold else "normal")


def pick_mono(size):
    for f in MONO:
        try:
            font.Font(family=f, size=size)
            return (f, size)
        except Exception:
            continue
    return ("TkFixedFont", size)


def trend_arrow(t):
    return {0: "→", 1: "↗", 2: "↑", 3: "↘", 4: "↓"}.get(int(t), "→")


def state_name(s):
    return {0: "初始化", 1: "待机", 2: "大剂量", 3: "方波", 4: "排气",
            5: "报警", 6: "输注中", 7: "暂停"}.get(int(s), f"状态{int(s)}")


class LinkGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("闭环胰岛素泵 · AAPS 联调四路同步演示")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.sock = None
        self.running = True
        self.connected = False
        self.lock = threading.Lock()

        self.last = {"idx": 0, "total": 17, "playing": False,
                     "steps": [], "trace": [], "state": {}}
        self._shown_steps = 0
        self._shown_trace = 0
        self.motor_disp = 0.0     # 动画用: 已发药显示值 (tween 到目标)
        self.sim_proc = None      # 本 GUI 拉起的模拟器子进程 (关闭时清理)
        self.sim_build_proc = None

        self._build_ui()
        self._net_thread = threading.Thread(target=self._net_loop, daemon=True)
        self._net_thread.start()
        self.root.after(60, self._anim_tick)

    # ---------------- UI 构建 ----------------
    def _build_ui(self):
        big = pick_font(13, bold=True)
        mid = pick_font(11)
        small = pick_font(10)
        mono = pick_mono(9)

        # 顶部安全声明条
        tk.Label(self.root,
            text="⚠ 实验项目 / 教学原型 · 严禁用于任何人体 (真机仅可用空注射器/水验证)",
            bg="#b71c1c", fg="white", font=pick_font(11, bold=True)
        ).pack(side=tk.TOP, fill=tk.X)

        # 连接状态条
        top = tk.Frame(self.root)
        top.pack(side=tk.TOP, fill=tk.X, padx=6, pady=4)
        self.conn_var = tk.StringVar(value="● 未连接")
        tk.Label(top, textvariable=self.conn_var, font=mid, fg="#888"
                 ).pack(side=tk.LEFT)
        tk.Button(top, text="▶ 启动模拟器", command=self.launch_sim,
                  font=mid).pack(side=tk.RIGHT, padx=4)

        # 主体: 左步骤列表 + 右 2x2 四宫格
        body = tk.Frame(self.root)
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=6, pady=4)

        # 左: 17 步会话列表
        left = tk.LabelFrame(body, text="AAPS 命令会话 (17 步)", font=mid)
        left.pack(side=tk.LEFT, fill=tk.Y, padx=3)
        self.step_list = tk.Listbox(left, font=small, width=30, height=30,
                                    activestyle="none", bd=0)
        self.step_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sl_s = tk.Scrollbar(left, command=self.step_list.yview)
        sl_s.pack(side=tk.RIGHT, fill=tk.Y)
        self.step_list.config(yscrollcommand=sl_s.set)

        # 右: 2x2 四宫格
        grid = tk.Frame(body)
        grid.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=3)
        grid.rowconfigure(0, weight=1); grid.rowconfigure(1, weight=1)
        grid.columnconfigure(0, weight=1); grid.columnconfigure(1, weight=1)

        # ① 步进电机推药演示
        f_motor = tk.LabelFrame(grid, text="① 步进电机推药演示 (真实固件换算)",
                                font=mid)
        f_motor.grid(row=0, column=0, sticky="nsew", padx=2, pady=2)
        self.cv_motor = tk.Canvas(f_motor, width=380, height=210,
                                   bg="#0b1020", bd=0, highlightthickness=0)
        self.cv_motor.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=2, pady=2)

        # ② AAPS 发送数据调试窗
        f_tx = tk.LabelFrame(grid, text="② AAPS 发送数据调试窗 (AAPS → 泵)",
                             font=mid)
        f_tx.grid(row=0, column=1, sticky="nsew", padx=2, pady=2)
        self.tx = tk.Text(f_tx, font=mono, relief=tk.FLAT, state=tk.DISABLED,
                          bg="#0e1526", fg="#7ee7ff", insertbackground="#7ee7ff",
                          wrap=tk.WORD)
        self.tx.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=4, pady=4)
        txs = tk.Scrollbar(f_tx, command=self.tx.yview)
        txs.pack(side=tk.RIGHT, fill=tk.Y)
        self.tx.config(yscrollcommand=txs.set)
        self.tx.tag_config("tx", foreground="#7ee7ff")
        self.tx.tag_config("dim", foreground="#5a6b8c")

        # ③ 固件接收指令调试窗
        f_rx = tk.LabelFrame(grid, text="③ 固件接收指令调试窗 (泵解包 / 分发)",
                             font=mid)
        f_rx.grid(row=1, column=0, sticky="nsew", padx=2, pady=2)
        self.rx = tk.Text(f_rx, font=mono, relief=tk.FLAT, state=tk.DISABLED,
                          bg="#0e1526", fg="#a6e3a1", insertbackground="#a6e3a1",
                          wrap=tk.WORD)
        self.rx.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=4, pady=4)
        rxs = tk.Scrollbar(f_rx, command=self.rx.yview)
        rxs.pack(side=tk.RIGHT, fill=tk.Y)
        self.rx.config(yscrollcommand=rxs.set)
        self.rx.tag_config("rx", foreground="#a6e3a1")
        self.rx.tag_config("bad", foreground="#ff6b6b")
        self.rx.tag_config("dim", foreground="#5a6b8c")

        # ④ 泵屏 UI 实时画面
        f_screen = tk.LabelFrame(grid, text="④ 胰岛素泵屏幕 UI (320×172 横屏)",
                                 font=mid)
        f_screen.grid(row=1, column=1, sticky="nsew", padx=2, pady=2)
        self.cv = tk.Canvas(f_screen, width=470, height=253,
                            bg="#0b1020", bd=0, highlightthickness=0)
        self.cv.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=2, pady=2)

        # 底部控制栏
        ctl = tk.Frame(self.root)
        ctl.pack(side=tk.BOTTOM, fill=tk.X, padx=6, pady=6)
        self.play_btn = tk.Button(ctl, text="⏸ 暂停", command=self.toggle_play,
                                  font=big, width=10)
        self.play_btn.pack(side=tk.LEFT, padx=3)
        tk.Button(ctl, text="⏭ 单步", command=self.do_step, font=big,
                  width=10).pack(side=tk.LEFT, padx=3)
        tk.Button(ctl, text="⟲ 重置", command=self.do_reset, font=big,
                  width=10).pack(side=tk.LEFT, padx=3)

        self.delay_var = tk.IntVar(value=900)
        tk.Label(ctl, text="步进延迟", font=mid).pack(side=tk.LEFT, padx=(14, 2))
        self.delay = tk.Scale(ctl, from_=100, to=3000, orient=tk.HORIZONTAL,
                              variable=self.delay_var, length=180,
                              command=self.on_delay, font=small,
                              showvalue=True, resolution=100)
        self.delay.pack(side=tk.LEFT, padx=2)
        self.delay_lbl = tk.Label(ctl, text="900 ms", font=mid)
        self.delay_lbl.pack(side=tk.LEFT, padx=2)

    # ---------------- 网络 ----------------
    def _net_loop(self):
        while self.running:
            try:
                self._connect()
                self._read_loop()
            except Exception:
                with self.lock:
                    self.connected = False
                self._set_conn("● 未连接 (重试中)", "#888")
                time.sleep(1.5)

    def _connect(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect((HOST, PORT))
        s.settimeout(None)
        with self.lock:
            self.sock = s
            self.connected = True
        self._set_conn("● 已连接  127.0.0.1:18923", "#2e7d32")
        self._log_line(self.tx, "[连接] 已接入模拟器控制通道")

    def _read_loop(self):
        buf = ""
        while self.running:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except Exception:
                raise
            if not data:
                raise ConnectionError("server closed")
            buf += data.decode("utf-8", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                self._on_message(line)

    def _send(self, msg):
        with self.lock:
            if self.sock is None or not self.connected:
                return
            try:
                self.sock.sendall((msg + "\n").encode("utf-8"))
            except Exception:
                pass

    def _on_message(self, line):
        try:
            obj = json.loads(line)
        except Exception:
            return
        if obj.get("t") == "reset":
            with self.lock:
                self.last = {"idx": 0, "total": obj.get("total", 17),
                             "playing": False, "steps": [], "trace": [],
                             "state": {}}
            self._shown_steps = 0
            self._shown_trace = 0
            self.motor_disp = 0.0
            self.root.after(0, self._refresh)
            return
        if obj.get("t") != "status":
            return
        with self.lock:
            self.last = {
                "idx": obj.get("idx", 0),
                "total": obj.get("total", 17),
                "playing": bool(obj.get("playing", False)),
                "steps": obj.get("steps", []),
                "trace": obj.get("trace", []),
                "state": obj.get("state", {}),
            }
        self.root.after(0, self._refresh)

    # ---------------- UI 刷新 ----------------
    def _refresh(self):
        with self.lock:
            last = dict(self.last)
        self._refresh_steps(last)
        self._refresh_debug(last.get("trace", []))
        self._refresh_canvas(last.get("state", {}))
        if last.get("playing"):
            self.play_btn.config(text="⏸ 暂停")
        else:
            self.play_btn.config(text="▶ 播放")

    def _refresh_steps(self, last):
        self.step_list.delete(0, tk.END)
        steps = last.get("steps", [])
        total = last.get("total", 17)
        idx = last.get("idx", 0)
        for i in range(total):
            if i < len(steps):
                title = steps[i].get("title", f"步骤 {i+1}")
                checks = steps[i].get("checks", [])
                ok = all(c.get("ok") for c in checks) if checks else True
                tag = "ok" if ok else "fail"
            else:
                title = f"[步骤 {i+1}] 待执行"
                tag = "pending"
            self.step_list.insert(tk.END, title)
            self.step_list.itemconfig(tk.END, fg=self._color(tag))
            if i == idx and i >= len(steps):
                self.step_list.itemconfig(tk.END, bg="#2a3b66")
        if idx < total:
            self.step_list.selection_clear(0, tk.END)
            self.step_list.selection_set(idx)
            self.step_list.see(idx)

    def _color(self, tag):
        return {"ok": "#36d399", "fail": "#ff5b5b",
                "pending": "#8a93a6"}.get(tag, "#8a93a6")

    def _refresh_debug(self, trace):
        shown = getattr(self, "_shown_trace", 0)
        if len(trace) > shown:
            for t in trace[shown:]:
                self._append_tx(t)
                self._append_rx(t)
            self._shown_trace = len(trace)
        if len(trace) == 0:
            self._shown_trace = 0

    def _append_tx(self, t):
        self.tx.config(state=tk.NORMAL)
        self.tx.insert(tk.END, f"[步{t.get('i')}] {t.get('op')}\n", "tx")
        self.tx.insert(tk.END, f"  → {t.get('intent','')}\n", "tx")
        self.tx.insert(tk.END, f"  TX : {t.get('tx','')}\n", "dim")
        self.tx.insert(tk.END, "\n")
        self.tx.see(tk.END)
        self.tx.config(state=tk.DISABLED)

    def _append_rx(self, t):
        self.rx.config(state=tk.NORMAL)
        rej = t.get("rej", False)
        tag = "bad" if rej else "rx"
        if rej:
            self.rx.insert(tk.END, f"[步{t.get('i')}] {t.get('op')}  ✗ 被拒绝\n", "bad")
        else:
            self.rx.insert(tk.END, f"[步{t.get('i')}] {t.get('op')}\n", "rx")
        self.rx.insert(tk.END, f"  {t.get('rx','')}\n", tag)
        self.rx.insert(tk.END, f"  动作: {t.get('action','')}\n", tag)
        self.rx.insert(tk.END, f"  RESP: {t.get('resp','') or '(无)'}\n", "dim")
        self.rx.insert(tk.END, "\n")
        self.rx.see(tk.END)
        self.rx.config(state=tk.DISABLED)

    def _refresh_canvas(self, st):
        cv = self.cv
        cv.delete("all")
        S = 1.47
        W, H = 320 * S, 172 * S
        f_title = pick_font(20, bold=True)
        f_mid = pick_font(16)
        f_small = pick_font(13)
        cv.create_rectangle(0, 0, W, H, fill="#0b1020", outline="")
        hh = st.get("clock_h", -1); mm = st.get("clock_m", -1)
        clk = f"{int(hh):02d}:{int(mm):02d}" if hh >= 0 else "--:--"
        cv.create_text(10 * S, 8 * S, anchor="nw", text=clk,
                       fill="#cdd6f4", font=f_mid)
        loop = "AAPS 已接管" if st.get("loop_mode", 0) == 0 else "本地模式"
        cv.create_text(W - 10 * S, 8 * S, anchor="ne", text=loop,
                       fill="#89dceb" if st.get("loop_mode", 0) == 0 else "#f9e2af",
                       font=f_small)
        conn = "●BLE" if st.get("connected") else "○BLE"
        cv.create_text(W / 2, 8 * S, anchor="n", text=conn,
                       fill="#a6e3a1" if st.get("connected") else "#f38ba8",
                       font=f_small)
        if st.get("alarm_active"):
            cv.create_rectangle(0, 40 * S, W, 78 * S, fill="#7a1f1f", outline="")
            cv.create_text(W / 2, 50 * S, anchor="n", text="⚠ 报警",
                           fill="#ffb4b4", font=f_mid)
        gm = st.get("glucose_mmol", 0.0)
        if gm:
            cv.create_text(W / 2, 86 * S, anchor="n", text=f"{gm:.1f}",
                           fill="#f9e2af", font=pick_font(46, bold=True))
            cv.create_text(W / 2 + 110 * S, 96 * S, anchor="nw",
                           text="mmol/L", fill="#a6adc8", font=f_small)
            cv.create_text(W / 2, 150 * S, anchor="n",
                           text=trend_arrow(st.get("trend", 0)),
                           fill="#94e2d5", font=pick_font(26, bold=True))
        else:
            cv.create_text(W / 2, 96 * S, anchor="n", text="--",
                           fill="#6c7086", font=pick_font(46, bold=True))
        y = H - 64 * S
        cv.create_text(14 * S, y, anchor="nw",
                       text=f"储药 {st.get('reservoir',0)}U", fill="#cdd6f4", font=f_small)
        cv.create_text(W - 14 * S, y, anchor="ne",
                       text=f"电量 {st.get('battery',0)}%", fill="#cdd6f4", font=f_small)
        y2 = y + 24 * S
        cv.create_text(14 * S, y2, anchor="nw",
                       text=f"IOB {st.get('iob',0.0):.2f}U", fill="#cdd6f4", font=f_small)
        cv.create_text(W - 14 * S, y2, anchor="ne",
                       text=f"今日 {st.get('today',0.0):.2f}U", fill="#cdd6f4", font=f_small)
        tp = st.get("tbr_pct", 0)
        if tp:
            cv.create_text(W / 2, y2, anchor="n",
                           text=f"临时基础率 {tp:.0f}% ({st.get('tbr_rate',0):.2f}U/h)",
                           fill="#fab387", font=f_small)
        if st.get("bolus_active"):
            cv.create_text(W / 2, y, anchor="n", text="● 大剂量进行中",
                           fill="#f38ba8", font=f_small)
        cv.create_text(14 * S, H - 26 * S, anchor="sw",
                       text=f"状态: {state_name(st.get('state',1))}",
                       fill="#a6e3a1", font=f_small)
        cv.create_text(W - 14 * S, H - 26 * S, anchor="se", text="▲ ▼  OK  ESC",
                       fill="#6c7086", font=f_small)

    # ---------------- 电机动画 ----------------
    def _anim_tick(self):
        if not self.running:
            return
        with self.lock:
            st = dict(self.last.get("state", {}))
        target = st.get("motor_units", 0.0)
        # tween 显示值逼近目标, 形成连续"推药"动画
        self.motor_disp += (target - self.motor_disp) * 0.18
        if abs(target - self.motor_disp) < 1e-4:
            self.motor_disp = target
        self._draw_motor(self.motor_disp, st.get("bolus_active", False))
        self.root.after(60, self._anim_tick)

    def _draw_motor(self, disp_u, bolus_active):
        cv = self.cv_motor
        cv.delete("all")
        W, H = 380, 210
        small = pick_font(10)

        # 背景分隔
        cv.create_rectangle(0, 0, W, H, fill="#0b1020", outline="")

        # --- 步进电机本体 (左) ---
        cv.create_rectangle(18, 60, 92, 150, fill="#1b2440", outline="#3a4a72")
        cv.create_text(55, 52, text="STEPPER", fill="#89b4fa", font=small)
        # 转子 (随微步旋转)
        cx, cy, r = 55, 105, 22
        microsteps = disp_u * STEPS_PER_UNIT
        ang = (microsteps / 32.0) * (2 * math.pi / 200.0)  # 200 全步/转
        for k in range(4):  # 4 齿转子
            a = ang + k * (2 * math.pi / 4.0)
            ex = cx + r * 0.8 * math.cos(a)
            ey = cy + r * 0.8 * math.sin(a)
            cv.create_line(cx, cy, ex, ey, fill="#f9e2af", width=3)
        cv.create_oval(cx - 4, cy - 4, cx + 4, cy + 4, fill="#f9e2af", outline="")
        cv.create_oval(cx - r, cy - r, cx + r, cy + r, outline="#56628a", width=1)

        # --- 丝杠 (电机 -> 注射器) ---
        cv.create_line(92, 105, 205, 105, fill="#7f8ea3", width=3)
        for x in range(100, 205, 8):
            cv.create_line(x, 100, x, 110, fill="#4a5878", width=1)

        # --- 注射器 (右) ---
        bx0, by0, bx1, by1 = 205, 82, 360, 128
        cv.create_rectangle(bx0, by0, bx1, by1, fill="#0e1830",
                            outline="#56628a", width=1)
        # 柱塞位置 (随发药推进)
        frac = min(disp_u, MAX_VIS_U) / MAX_VIS_U
        plunger_x = bx0 + 10 + frac * (bx1 - bx0 - 24)
        # 药液柱 (柱塞右侧到针尖)
        cv.create_rectangle(plunger_x + 4, by0 + 4, bx1 - 4, by1 - 4,
                            fill="#4aa3ff", outline="")
        # 柱塞橡胶
        cv.create_rectangle(plunger_x, by0 + 2, plunger_x + 5, by1 - 2,
                            fill="#cdd6f4", outline="#9aa6c4")
        # 针头
        cv.create_line(bx1, (by0 + by1) / 2, bx1 + 16, (by0 + by1) / 2,
                       fill="#9aa6c4", width=2)
        cv.create_text((bx0 + bx1) / 2, by1 + 12, text="注射器 (储药器)",
                       fill="#8a93a6", font=small)

        # --- 标签 ---
        ms = int(microsteps)
        mm_travel = microsteps * MM_PER_STEP
        y0 = 158
        cv.create_text(10, y0, anchor="nw",
                       text=f"已发药: {disp_u:.2f} U", fill="#a6e3a1", font=small)
        cv.create_text(10, y0 + 16, anchor="nw",
                       text=f"微步:   {ms}", fill="#cdd6f4", font=small)
        cv.create_text(10, y0 + 32, anchor="nw",
                       text=f"柱塞行程: {mm_travel:.3f} mm", fill="#cdd6f4", font=small)
        # 电机状态灯
        col = "#36d399" if bolus_active else "#6c7086"
        cv.create_oval(330, y0 + 4, 342, y0 + 16, fill=col, outline="")
        cv.create_text(346, y0 + 4, anchor="nw",
                       text="运转中" if bolus_active else "静止",
                       fill=col, font=small)
        cv.create_text(346, y0 + 20, anchor="nw",
                       text=f"1U≈{int(STEPS_PER_UNIT)}步", fill="#8a93a6", font=small)

    # ---------------- 控制 ----------------
    def toggle_play(self):
        with self.lock:
            playing = self.last.get("playing", False)
        self._send("pause" if playing else "play")

    def do_step(self):
        self._send("step")

    def do_reset(self):
        if not messagebox.askyesno("确认重置", "重置会话将清空所有已执行步骤与泵状态?"):
            return
        self._send("reset")

    def on_delay(self, val):
        try:
            d = int(float(val))
        except Exception:
            return
        self.delay_lbl.config(text=f"{d} ms")
        self._send(f"delay {d}")

    def launch_sim(self):
        # 端口已被占用 -> 模拟器已在跑, 只连不启
        if self._sim_port_busy():
            messagebox.showinfo("提示", "模拟器已在运行 (控制通道 18923 已占用),\n直接连接即可, 无需重复启动。")
            return
        # 已连接 -> 提示
        with self.lock:
            if self.connected:
                messagebox.showinfo("提示", "已连接到模拟器, 无需重复启动。")
                return
        # 二进制必须是联调版 (有构建标记), 否则自动构建, 避免误用 mock 版
        link_mark = "/Users/feelingme/pump_sim_build/.built_link_mode"
        if not os.path.exists(link_mark):
            if not messagebox.askyesno("需要构建联调版",
                    "当前模拟器二进制不是联调演示版 (或缺失)。\n"
                    "是否现在自动构建联调版并启动?\n\n"
                    "(也可直接双击 test/run_link_demo.command 一键启动)"):
                return
            self._build_and_launch()
            return
        self._spawn_sim()

    def _sim_port_busy(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.4)
            s.connect((HOST, PORT))
            s.close()
            return True
        except Exception:
            return False

    def _spawn_sim(self):
        try:
            # 窗口模式: 真实泵屏 SDL 窗口 + GUI 内 canvas 复刻, 双画面并存
            self.sim_proc = subprocess.Popen(
                [SIM_BINARY], cwd="/Users/feelingme/pump_sim_build",
                start_new_session=True)
            self._log_line(self.tx,
                f"[启动] 模拟器进程已发起 (PID {self.sim_proc.pid}), 控制通道将自动连接")
        except Exception as e:
            messagebox.showerror("启动失败", f"无法启动模拟器:\n{e}")

    def _build_and_launch(self):
        self._log_line(self.tx, "[构建] 开始构建联调版模拟器, 请稍候...")
        def run():
            try:
                p = subprocess.Popen(
                    ["bash", BUILD_SCRIPT], stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, start_new_session=True)
                self.sim_build_proc = p
                for line in p.stdout:
                    self._log_line(self.tx, "[构建] " + line.rstrip())
                rc = p.wait()
                if rc == 0:
                    self._log_line(self.tx, "[构建] 完成, 正在启动模拟器...")
                    self.root.after(0, self._spawn_sim)
                else:
                    self._log_line(self.tx, f"[构建] 失败 (返回码 {rc})")
                    self.root.after(0, lambda: messagebox.showerror(
                        "构建失败", f"构建返回 {rc}, 详见窗口日志。"))
            except Exception as e:
                self._log_line(self.tx, f"[构建] 异常: {e}")
        threading.Thread(target=run, daemon=True).start()

    # ---------------- 辅助 ----------------
    def _set_conn(self, text, color):
        self.root.after(0, lambda: self.conn_var.set(text))

    def _log_line(self, widget, text):
        try:
            widget.config(state=tk.NORMAL)
            widget.insert(tk.END, text + "\n")
            widget.see(tk.END)
            widget.config(state=tk.DISABLED)
        except Exception:
            pass

    def on_close(self):
        self.running = False
        # 清理本 GUI 拉起的模拟器子进程 (若仍在运行)
        try:
            if self.sim_proc is not None and self.sim_proc.poll() is None:
                os.killpg(os.getpgid(self.sim_proc.pid), 15)
        except Exception:
            pass
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass
        self.root.destroy()


def main():
    root = tk.Tk()
    root.geometry("1200x760")
    try:
        root.option_add("*Font", pick_font(11))
    except Exception:
        pass
    app = LinkGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
