#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
link_demo_gui.py — AAPS 蓝牙动态控制 · 联调同步演示控制面板 (四宫格 + 数据模拟)

连接 LVGL SDL 泵模拟器 (SIM_LINK_MODE) 起的 TCP 控制通道 127.0.0.1:18923,
实时四路同步演示:
  ┌─────────────┬──────────────────────────────┐
  │ ① 步进电机  │ ② AAPS 发送数据调试窗        │
  │   推药演示  │   (AAPS -> 泵 的原始 BLE 包)  │
  ├─────────────┼──────────────────────────────┤
  │ ③ 固件接收  │ ④ 胰岛素泵屏幕 UI 实时画面   │
  │   指令调试  │   (canvas 复刻 320x172 横屏) │
  └─────────────┴──────────────────────────────┘

两种模式:
  · 协议演示  — 播放 17 步 AAPS↔泵 完整闭环会话 (握手/时间/基础率/大剂量/方波…)
  · 数据模拟  — 载入血糖曲线 + 24 段基础率 + 餐时, 按真实经过时间用真实固件
               命令路径(STEP_BOLUS / SET_TBR)模拟整套闭环系统运行。

控制通道协议 (simulator/lvgl_sdl/src/link_ipc.cpp):
  接收: {"t":"status",... ,"state":{...}}
        {"t":"reset","total":M}
  发送: "play" / "pause" / "step" / "reset" / "delay <ms>"
        "mode replay|script"  /  "data <json>"
        "key up|down|set|esc"  (手动控制泵屏 4 按键)

⚠️ 实验项目 / 教学原型, 严禁用于任何人体。真机仅可用空注射器/水验证。
"""
import csv
import json
import math
import os
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, font, messagebox, filedialog

HOST = "127.0.0.1"
PORT = 18923
SIM_BINARY = "/Users/feelingme/pump_sim_build/simulator"
BUILD_SCRIPT = "/Users/feelingme/Desktop/闭环胰岛素泵项目/test/build_sim_link.sh"

# 电机几何 (与固件 config.h / dosing.h 一致, 仅用于演示标度)
STEPS_PER_UNIT = 2178.0          # 1U ≈ 2178 微步
MM_PER_STEP = 0.5 / 6400.0       # 导程 0.5mm / 6400 微步/转
MAX_VIS_U = 6.0                  # 演示用柱塞可视上限 (会话累计 ~5U)

# ---- 配色: 还原固件白底 + 迈世通医疗蓝 (ui_screen.cpp) ----
BG      = "#ffffff"   # 纯白设备底
PANEL_L = "#eef2f6"   # 浅灰卡片底 (面板内浅色)
ACCENT  = "#006bb7"   # 迈世通医疗蓝
TITLE   = "#0a2a43"   # 深蓝标题/正文强调
TEXT    = "#1f2733"   # 近黑正文
GREEN   = "#2e9e4f"   # 正常态绿
YELLOW  = "#c79100"   # 警示琥珀
RED     = "#d83a3a"   # 报警红
DIM     = "#8a95a5"   # 灰辅助文字
LINE    = "#d5dce4"   # 分割线/浅边框
BLUE_L  = "#dce6f0"   # 浅蓝填充 (电机面板)

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


def glucose_color(mmol):
    """按临床范围着色 (白底可见)。"""
    if mmol <= 0:
        return DIM
    if mmol < 3.5:
        return RED
    if mmol < 4.0:
        return YELLOW
    if mmol <= 10.0:
        return TITLE
    if mmol <= 13.5:
        return YELLOW
    return RED


EXAMPLE_CURVE = "[[0,90],[120,110],[300,160],[480,200],[720,150],[960,120],[1200,100],[1440,95]]"
EXAMPLE_BASAL = "0.6,0.6,0.6,0.6,0.6,0.6,0.7,0.7,0.8,0.8,0.9,0.9,0.8,0.8,0.7,0.7,0.6,0.6,0.6,0.6,0.7,0.7,0.6,0.6"
EXAMPLE_MEALS = "[[480,7.0],[1020,4.0]]"

# ---- 泵屏界面枚举 (与 ui_screen.cpp SCR_* / ui_screen.h UI_* 对应) ----
SCREEN = {0: "主状态屏", 1: "主菜单", 2: "基础率", 3: "大剂量", 4: "常规大剂量",
          5: "方波大剂量", 6: "双波大剂量", 7: "向导大剂量", 8: "三餐预设",
          9: "排气与装药", 10: "报警", 11: "报警详情", 12: "闭环", 13: "系统设置",
          14: "设置时间", 15: "关于"}
ALARM_NAMES = ["阻塞", "低电量", "低药量", "连接丢失", "电机堵转", "丢步", "过温"]
ALARM_REASON = ["管路阻塞或阻力过大", "电池电压偏低", "储药器药量不足",
                "与手机/AAPS 通信中断", "步进电机卡死", "电机丢步/电流异常",
                "板载温度过高"]
ALARM_ACTION = ["检查管路，解除阻塞后确认", "尽快更换电池", "准备更换笔芯",
                "检查蓝牙连接", "检查机械结构后复位", "检查电机负载",
                "暂停使用，降温后恢复"]
LOOP_MODE = {0: "闭环中", 1: "开环", 2: "已暂停"}


class LinkGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("闭环胰岛素泵 · AAPS 联调同步演示")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.sock = None
        self.running = True
        self.connected = False
        self.lock = threading.Lock()

        self.last = {"idx": 0, "total": 17, "playing": False,
                     "steps": [], "trace": [], "state": {}}
        self._shown_steps = 0
        self._shown_trace = 0
        self.motor_disp = 0.0
        self.sim_proc = None
        self.sim_build_proc = None

        # 数据模拟相关
        self.mode_var = tk.StringVar(value="protocol")   # protocol / replay
        self.glucose_curve = []      # [[t_min, mgdl], ...]
        self.meals_val = []          # [[t_min, dose_u], ...]
        self.duration_val = 1440
        self.basal_val = [0.6] * 24

        self._build_ui()
        self._net_thread = threading.Thread(target=self._net_loop, daemon=True)
        self._net_thread.start()
        # 键盘手动控制泵屏 (焦点在输入框时不触发, 避免误改数据)
        # 按下发 press, 释放发 release -> 支持"长按持续加减"
        self.root.bind("<KeyPress-Up>", lambda e: self._key_press("up"))
        self.root.bind("<KeyPress-Down>", lambda e: self._key_press("down"))
        self.root.bind("<KeyPress-Return>", lambda e: self._key_press("set"))
        self.root.bind("<KeyPress-KP_Enter>", lambda e: self._key_press("set"))
        self.root.bind("<KeyPress-Escape>", lambda e: self._key_press("esc"))
        self.root.bind("<KeyRelease-Up>", lambda e: self._key_release())
        self.root.bind("<KeyRelease-Down>", lambda e: self._key_release())
        self.root.bind("<KeyRelease-Return>", lambda e: self._key_release())
        self.root.bind("<KeyRelease-KP_Enter>", lambda e: self._key_release())
        self.root.bind("<KeyRelease-Escape>", lambda e: self._key_release())
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

        # 连接状态条 + 模式切换
        top = tk.Frame(self.root)
        top.pack(side=tk.TOP, fill=tk.X, padx=6, pady=4)
        self.conn_var = tk.StringVar(value="● 未连接")
        tk.Label(top, textvariable=self.conn_var, font=mid, fg="#888"
                 ).pack(side=tk.LEFT)
        tk.Button(top, text="▶ 启动模拟器", command=self.launch_sim,
                  font=mid).pack(side=tk.RIGHT, padx=4)
        # 模式切换
        md = tk.Frame(top)
        md.pack(side=tk.RIGHT, padx=10)
        tk.Label(md, text="模式:", font=mid).pack(side=tk.LEFT)
        tk.Radiobutton(md, text="协议演示", variable=self.mode_var, value="protocol",
                       font=mid, command=self.on_mode_change).pack(side=tk.LEFT)
        tk.Radiobutton(md, text="数据模拟", variable=self.mode_var, value="replay",
                       font=mid, command=self.on_mode_change).pack(side=tk.LEFT)

        # 主体: 左列表 + 右 2x2 四宫格
        body = tk.Frame(self.root)
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=6, pady=4)

        # 左: 会话列表 / 回放遥测
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

        # ① 步进电机推药演示 (白底 + 医疗蓝描边)
        f_motor = tk.LabelFrame(grid, text="① 步进电机推药演示 (真实固件换算)",
                                font=mid)
        f_motor.grid(row=0, column=0, sticky="nsew", padx=2, pady=2)
        self.cv_motor = tk.Canvas(f_motor, width=380, height=210,
                                   bg=BG, bd=0, highlightthickness=0)
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

        # ④ 泵屏 UI 实时画面 (白底, 还原固件 ui_screen 配色)
        f_screen = tk.LabelFrame(grid, text="④ 胰岛素泵屏幕 UI (320×172 横屏 · 白底)",
                                 font=mid)
        f_screen.grid(row=1, column=1, sticky="nsew", padx=2, pady=2)
        self.cv = tk.Canvas(f_screen, width=470, height=253,
                            bg=BG, bd=0, highlightthickness=0)
        self.cv.pack(side=tk.TOP, fill=tk.BOTH, expand=True, padx=2, pady=2)

        # ============ 底部: 模式相关控制区 ============
        self.bottom = tk.Frame(self.root)
        self.bottom.pack(side=tk.BOTTOM, fill=tk.X, padx=6, pady=6)

        # ---- 手动控制 (泵屏 4 按键) — 联调中可直接操作泵屏 UI ----
        self.manual_frame = tk.Frame(self.bottom, bd=1, relief=tk.RIDGE)
        self.manual_frame.pack(side=tk.TOP, fill=tk.X, padx=4, pady=4)
        tk.Label(self.manual_frame, text="手动控制 (泵屏按键):",
                 font=small, fg=TITLE).pack(side=tk.LEFT, padx=4)

        def _press(k):
            return lambda e=None: self._send("key " + k)
        def _release():
            return lambda e=None: self._send("key release")

        # 按下即发 press, 松开发 release -> 支持"长按上下键持续加减"(FSM 自动重复)
        for label, k in [("▲ 上", "up"), ("▼ 下", "down"),
                         ("✔ 确认", "set"), ("⏎ 返回", "esc")]:
            b = tk.Button(self.manual_frame, text=label, width=8,
                          bg=("#e3f0fb" if k != "set" else ACCENT),
                          fg=("black" if k != "set" else "white"), font=mid)
            b.bind("<ButtonPress-1>", _press(k))
            b.bind("<ButtonRelease-1>", _release())
            b.pack(side=tk.LEFT, padx=3)
        tk.Label(self.manual_frame, text="(按住 ▲▼ 持续加减 / 键盘 ↑↓ / Enter / Esc)",
                 font=small, fg=DIM).pack(side=tk.LEFT, padx=6)

        # ---- 协议演示控制条 ----
        self.proto_frame = tk.Frame(self.bottom)
        ctl = self.proto_frame
        ctl.pack(side=tk.TOP, fill=tk.X)
        self.play_btn = tk.Button(ctl, text="⏸ 暂停", command=self.toggle_play,
                                  font=big, width=10, bg="#e3f0fb")
        self.play_btn.pack(side=tk.LEFT, padx=3)
        tk.Button(ctl, text="⏭ 单步", command=self.do_step, font=big,
                  width=10, bg="#e3f0fb").pack(side=tk.LEFT, padx=3)
        tk.Button(ctl, text="⟲ 重置", command=self.do_reset, font=big,
                  width=10, bg="#e3f0fb").pack(side=tk.LEFT, padx=3)
        self.delay_var = tk.IntVar(value=900)
        tk.Label(ctl, text="步进延迟", font=mid).pack(side=tk.LEFT, padx=(14, 2))
        self.delay = tk.Scale(ctl, from_=100, to=3000, orient=tk.HORIZONTAL,
                              variable=self.delay_var, length=180,
                              command=self.on_delay, font=small,
                              showvalue=True, resolution=100)
        self.delay.pack(side=tk.LEFT, padx=2)
        self.delay_lbl = tk.Label(ctl, text="900 ms", font=mid)
        self.delay_lbl.pack(side=tk.LEFT, padx=2)

        # ---- 数据模拟控制面板 ----
        self.data_frame = tk.Frame(self.bottom)
        df = self.data_frame
        # 左: 输入区
        inp = tk.LabelFrame(df, text="数据输入 (血糖曲线 / 基础率 / 餐时)", font=mid)
        inp.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=3)
        row1 = tk.Frame(inp)
        row1.pack(side=tk.TOP, fill=tk.X, padx=4, pady=3)
        tk.Label(row1, text="血糖曲线 (mg/dL):", font=small).pack(side=tk.LEFT)
        tk.Button(row1, text="示例", command=self.use_example, font=small,
                  bg="#e3f0fb").pack(side=tk.LEFT, padx=3)
        tk.Button(row1, text="载入CSV", command=self.load_csv, font=small,
                  bg="#e3f0fb").pack(side=tk.LEFT, padx=3)
        self.glucose_text = tk.Text(inp, font=mono, height=3, relief=tk.FLAT,
                                    wrap=tk.WORD, bg="#fbfdff", bd=1)
        self.glucose_text.insert(tk.END, EXAMPLE_CURVE)
        self.glucose_text.pack(side=tk.TOP, fill=tk.X, padx=4, pady=2)
        self.glucose_text.tag_config("hint", foreground=DIM)

        row2 = tk.Frame(inp)
        row2.pack(side=tk.TOP, fill=tk.X, padx=4, pady=2)
        tk.Label(row2, text="基础率(U/h,24段):", font=small).pack(side=tk.LEFT)
        self.basal_var = tk.StringVar(value=EXAMPLE_BASAL)
        tk.Entry(row2, textvariable=self.basal_var, font=mono, width=46,
                 bg="#fbfdff").pack(side=tk.LEFT, padx=3)
        row3 = tk.Frame(inp)
        row3.pack(side=tk.TOP, fill=tk.X, padx=4, pady=2)
        tk.Label(row3, text="餐时([[分,剂量U]]):", font=small).pack(side=tk.LEFT)
        self.meals_var = tk.StringVar(value=EXAMPLE_MEALS)
        tk.Entry(row3, textvariable=self.meals_var, font=mono, width=22,
                 bg="#fbfdff").pack(side=tk.LEFT, padx=3)
        tk.Label(row3, text="时长(min):", font=small).pack(side=tk.LEFT, padx=(8,2))
        self.dur_var = tk.StringVar(value="1440")
        tk.Entry(row3, textvariable=self.dur_var, font=mono, width=7,
                 bg="#fbfdff").pack(side=tk.LEFT, padx=2)
        tk.Label(row3, text="倍速:", font=small).pack(side=tk.LEFT, padx=(8,2))
        self.sp_var = tk.StringVar(value="120")
        tk.Entry(row3, textvariable=self.sp_var, font=mono, width=7,
                 bg="#fbfdff").pack(side=tk.LEFT, padx=2)
        tk.Label(row3, text="(模拟min/真实秒)", font=small, fg=DIM).pack(side=tk.LEFT, padx=2)

        # 右: 曲线图 + 开始按钮 + 状态
        out = tk.Frame(df)
        out.pack(side=tk.LEFT, fill=tk.Y, padx=3)
        self.cv_chart = tk.Canvas(out, width=360, height=130, bg=BG, bd=1,
                                  relief=tk.FLAT, highlightthickness=0)
        self.cv_chart.pack(side=tk.TOP, padx=2, pady=2)
        run = tk.Frame(out)
        run.pack(side=tk.TOP, fill=tk.X, pady=2)
        self.start_btn = tk.Button(run, text="▶ 开始模拟", command=self.start_replay,
                                   font=pick_font(13, bold=True), width=12,
                                   bg=GREEN, fg="white")
        self.start_btn.pack(side=tk.LEFT, padx=3)
        tk.Button(run, text="⏸ 暂停", command=self.toggle_play, font=big,
                  width=8, bg="#e3f0fb").pack(side=tk.LEFT, padx=2)
        tk.Button(run, text="⟲ 重置", command=self.do_reset, font=big,
                  width=8, bg="#e3f0fb").pack(side=tk.LEFT, padx=2)
        self.replay_status = tk.StringVar(value="待载入数据并开始")
        tk.Label(out, textvariable=self.replay_status, font=small, fg=TEXT
                 ).pack(side=tk.TOP, padx=2, pady=1)

        self.on_mode_change()   # 初始显示协议控制条

    # ---------------- 模式切换 ----------------
    def on_mode_change(self):
        m = self.mode_var.get()
        if m == "replay":
            self.proto_frame.pack_forget()
            self.data_frame.pack(side=tk.TOP, fill=tk.X)
            self.step_list.master.config(text="数据模拟回放遥测")
            self._send("mode replay")
            self._draw_chart({})
        else:
            self.data_frame.pack_forget()
            self.proto_frame.pack(side=tk.TOP, fill=tk.X)
            self.step_list.master.config(text="AAPS 命令会话 (17 步)")
            self._send("mode script")

    # ---------------- 数据解析 ----------------
    def _parse_glucose(self, text):
        text = text.strip()
        if not text:
            return []
        # 尝试 JSON 数组
        try:
            obj = json.loads(text)
            if isinstance(obj, list) and obj and isinstance(obj[0], list):
                return [[float(p[0]), float(p[1])] for p in obj]
        except Exception:
            pass
        # 退化为 CSV: 每行 "t,mgdl" 或 "t mgdl"
        pts = []
        for line in text.splitlines():
            line = line.strip().strip("[]")
            if not line:
                continue
            toks = [t for t in line.replace(",", " ").split() if t not in ("", ",", "[", "]")]
            if len(toks) >= 2:
                try:
                    pts.append([float(toks[0]), float(toks[1])])
                except Exception:
                    continue
        return pts

    def _parse_basal(self, text):
        vals = []
        for t in text.replace(",", " ").split():
            try:
                vals.append(float(t))
            except Exception:
                continue
        vals = vals[:24]
        while len(vals) < 24:
            vals.append(vals[-1] if vals else 0.5)
        return vals

    def _parse_meals(self, text):
        text = text.strip()
        if not text:
            return []
        try:
            obj = json.loads(text)
            if isinstance(obj, list):
                return [[float(p[0]), float(p[1])] for p in obj]
        except Exception:
            pass
        return []

    def use_example(self):
        self.glucose_text.delete("1.0", tk.END)
        self.glucose_text.insert(tk.END, EXAMPLE_CURVE)
        self.basal_var.set(EXAMPLE_BASAL)
        self.meals_var.set(EXAMPLE_MEALS)
        self.dur_var.set("1440")
        self.sp_var.set("120")

    def load_csv(self):
        path = filedialog.askopenfilename(
            title="选择血糖 CSV (两列: 时间分钟, 血糖mg/dL)",
            filetypes=[("CSV", "*.csv"), ("文本", "*.txt"), ("全部", "*.*")])
        if not path:
            return
        pts = []
        try:
            with open(path, newline="", encoding="utf-8-sig") as f:
                for row in csv.reader(f):
                    nums = [x for x in row if x.strip() not in ("",)]
                    if len(nums) >= 2:
                        try:
                            pts.append([float(nums[0]), float(nums[1])])
                        except Exception:
                            continue
        except Exception as e:
            messagebox.showerror("读取失败", str(e))
            return
        if not pts:
            messagebox.showwarning("空文件", "未在 CSV 中解析到任意两列数字。")
            return
        self.glucose_text.delete("1.0", tk.END)
        self.glucose_text.insert(tk.END, json.dumps(pts))
        self.glucose_curve = pts
        self._draw_chart({})
        messagebox.showinfo("已载入", f"载入 {len(pts)} 个血糖点。点「开始模拟」运行。")

    def start_replay(self):
        try:
            curve = self._parse_glucose(self.glucose_text.get("1.0", tk.END))
        except Exception as e:
            messagebox.showerror("血糖曲线解析失败", str(e))
            return
        if len(curve) < 2:
            messagebox.showerror("缺少数据", "请先填入或载入血糖曲线 (至少 2 点)。")
            return
        basal = self._parse_basal(self.basal_var.get())
        meals = self._parse_meals(self.meals_var.get())
        try:
            dur = int(float(self.dur_var.get()))
        except Exception:
            dur = 1440
        try:
            sp = float(self.sp_var.get())
        except Exception:
            sp = 120
        if dur <= 0 or sp <= 0:
            messagebox.showerror("参数错误", "时长/倍速必须为正数。")
            return
        self.glucose_curve = curve
        self.meals_val = meals
        self.duration_val = dur
        self.basal_val = basal
        data = {"duration": dur, "speedup": sp, "basal": basal,
                "glucose": curve, "meals": meals}
        self._send("mode replay")
        self._send("data " + json.dumps(data))
        self._send("play")
        self._log_line(self.tx,
            f"[数据模拟] 已载入曲线 {len(curve)} 点 / 基础率 {len(basal)} 段 / "
            f"餐时 {len(meals)} 次 / 时长 {dur}min / 倍速 {sp}x")
        self.replay_status.set("运行中…")

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
        if self.mode_var.get() == "replay":
            st = last.get("state", {})
            sm = st.get("sim_min", 0.0) or 0.0
            dur = self.duration_val or 1440
            pct = min(100.0, sm / dur * 100.0) if dur else 0
            lines = [
                f"回放进度  {pct:5.1f}%   ({sm:.0f}/{dur} min)",
                f"模拟时钟  {sm/60.0:5.1f} h",
                f"当前血糖  {st.get('glucose_mmol',0.0) or 0:.2f} mmol/L",
                f"  趋势    {trend_arrow(st.get('trend',0))}",
                f"IOB      {st.get('iob',0.0) or 0:.2f} U",
                f"今日用量 {st.get('today',0.0) or 0:.2f} U",
                f"剩余药量 {st.get('reservoir',0)} U",
                f"基础率   {st.get('basal_rate',0.0) or 0:.2f} U/h",
                f"临时基础率 {st.get('tbr_pct',0) or 0:.0f}%",
                f"状态     {state_name(st.get('state',1))}",
            ]
            for ln in lines:
                self.step_list.insert(tk.END, ln)
            return
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

    # ---- ④ 泵屏 (白底复刻固件 HOME, ui_screen.cpp 配色) ----
    def _draw_home(self, st):
        cv = self.cv
        cv.delete("all")
        S = 1.47
        W, H = 320 * S, 172 * S
        # 白色设备底
        cv.create_rectangle(0, 0, W, H, fill=BG, outline="")
        # 蓝色标题栏 (医疗蓝 ACCENT)
        cv.create_rectangle(0, 0, W, 18 * S, fill=ACCENT, outline="")
        hh = st.get("clock_h", -1); mm = st.get("clock_m", -1)
        clk = f"{int(hh):02d}:{int(mm):02d}" if hh >= 0 else "--:--"
        cv.create_text(4 * S, 2 * S, anchor="nw", text=clk,
                       fill="#ffffff", font=pick_font(11))
        cv.create_text(W / 2, 1 * S, anchor="n", text="闭环胰岛素泵",
                       fill="#ffffff", font=pick_font(12, bold=True))
        bat = st.get("battery", 0)
        cv.create_text(W - 4 * S, 2 * S, anchor="ne", text=f"电池 {bat}%",
                       fill="#ffffff" if bat >= 20 else YELLOW, font=pick_font(11))
        # ---- 左栏 CGM ----
        cv.create_text(4 * S, 22 * S, anchor="nw", text="血糖 (CGM)",
                       fill=DIM, font=pick_font(11))
        gm = st.get("glucose_mmol", 0.0)
        gvalid = gm and gm > 0
        if gvalid:
            cv.create_text(4 * S, 36 * S, anchor="nw", text=f"{gm:.1f}",
                           fill=glucose_color(gm), font=pick_font(34, bold=True))
            cv.create_text(72 * S, 44 * S, anchor="nw", text="mmol/L",
                           fill=DIM, font=pick_font(11))
            cv.create_text(4 * S, 78 * S, anchor="nw",
                           text=trend_arrow(st.get("trend", 0)),
                           fill=glucose_color(gm), font=pick_font(18, bold=True))
        else:
            cv.create_text(4 * S, 40 * S, anchor="nw", text="CGM 离线",
                           fill=DIM, font=pick_font(13))
        loop = st.get("loop_mode", 0)
        ltxt = {0: "闭环中", 1: "开环", 2: "已暂停"}.get(int(loop), "已暂停")
        lcol = {0: GREEN, 1: YELLOW, 2: DIM}.get(int(loop), DIM)
        paired = st.get("dana_paired", False)
        cv.create_text(4 * S, 100 * S, anchor="nw",
                       text=f"闭环: {ltxt}" + (" · AAPS接管" if paired else ""),
                       fill=lcol, font=pick_font(11))
        # ---- 右栏 泵 ----
        cv.create_text(168 * S, 22 * S, anchor="nw", text="基础率",
                       fill=DIM, font=pick_font(11))
        cv.create_text(168 * S, 36 * S, anchor="nw",
                       text=f"{st.get('basal_rate',0.0) or 0:.2f} U/h",
                       fill=TEXT, font=pick_font(13, bold=True))
        cv.create_text(168 * S, 62 * S, anchor="nw", text="剩余药量",
                       fill=DIM, font=pick_font(11))
        cv.create_text(168 * S, 76 * S, anchor="nw",
                       text=f"{st.get('reservoir',0)} U",
                       fill=TEXT, font=pick_font(13, bold=True))
        # 储药进度条 (蓝)
        bx, by, bw, bh = 168 * S, 94 * S, 138 * S, 8 * S
        cv.create_rectangle(bx, by, bx + bw, by + bh, fill=LINE, outline="")
        res = st.get("reservoir", 0); cap = 300.0
        frac = max(0.0, min(1.0, res / cap))
        cv.create_rectangle(bx, by, bx + bw * frac, by + bh, fill=ACCENT, outline="")
        # ---- 底部信息行 ----
        cv.create_text(4 * S, 124 * S, anchor="nw",
                       text=f"今日 {st.get('today',0.0) or 0:.1f} U",
                       fill=TEXT, font=pick_font(11))
        cv.create_text(168 * S, 124 * S, anchor="nw",
                       text=f"IOB {st.get('iob',0.0) or 0:.2f} U",
                       fill=TEXT, font=pick_font(11))
        # 状态 / 大剂量提示
        if st.get("alarm_active"):
            cv.create_text(4 * S, 144 * S, anchor="nw", text="⚠ 报警",
                           fill=RED, font=pick_font(12, bold=True))
        elif st.get("bolus_active"):
            cv.create_text(4 * S, 144 * S, anchor="nw",
                           text="● 大剂量注射中", fill=ACCENT, font=pick_font(12, bold=True))
        else:
            cv.create_text(4 * S, 144 * S, anchor="nw",
                           text=f"状态: {state_name(st.get('state',1))}",
                           fill=TEXT, font=pick_font(11))
        cv.create_text(150 * S, 144 * S, anchor="nw", text="确认键进入菜单",
                       fill=DIM, font=pick_font(11))

    # ---- ④ 泵屏派发: HOME 富渲染, 其它界面按 screen/sel 渲染 (手动控制可见) ----
    def _refresh_canvas(self, st):
        ui = st.get("ui", {})
        screen = ui.get("screen", 0) if ui else 0
        if screen == 0:
            self._draw_home(st)
        else:
            self._draw_screen(ui, st)

    # 通用菜单列表: items=[(label,value),...], sel=选中索引
    def _menu_list(self, cv, S, items, sel, top_y=24, row_h=18, x=8):
        for i, (lab, val) in enumerate(items):
            y = top_y + i * row_h
            isel = (i == sel)
            cv.create_text(x * S, y * S, anchor="nw",
                           text=("▶ " + lab if isel else "  " + lab),
                           fill=ACCENT if isel else TEXT, font=pick_font(11))
            if val not in ("", None):
                cv.create_text((x + 150) * S, y * S, anchor="nw", text=str(val),
                               fill=ACCENT if isel else DIM, font=pick_font(11))

    # 表单字段: fields=[(label,value,highlight),...]
    def _field(self, cv, S, fields):
        for i, (lab, val, hl) in enumerate(fields):
            y = 30 + i * 30
            cv.create_text(20 * S, y * S, anchor="nw", text=lab,
                           fill=DIM, font=pick_font(11))
            cv.create_text(120 * S, (y - 2) * S, anchor="nw", text=str(val),
                           fill=ACCENT if hl else TEXT, font=pick_font(12, bold=True))

    def _hint(self, cv, S, text, color=DIM):
        cv.create_text(4 * S, 150 * S, anchor="nw", text=text,
                       fill=color, font=pick_font(10))

    # 键盘手动控制 (焦点在输入框时不触发)
    def _key_press(self, k):
        w = self.root.focus_get()
        if w is not None and w.winfo_class() in ("Entry", "Text"):
            return
        self._send("key " + k)

    def _key_release(self):
        w = self.root.focus_get()
        if w is not None and w.winfo_class() in ("Entry", "Text"):
            return
        self._send("key release")

    # 按 screen/sel 实时渲染泵屏 (手动导航可见)
    def _draw_screen(self, ui, st):
        cv = self.cv
        cv.delete("all")
        S = 1.47
        W, H = 320 * S, 172 * S
        cv.create_rectangle(0, 0, W, H, fill=BG, outline="")
        screen = ui.get("screen", 0)
        sel = ui.get("sel", 0)
        name = SCREEN.get(screen, "界面%d" % screen)
        cv.create_rectangle(0, 0, W, 18 * S, fill=ACCENT, outline="")
        cv.create_text(4 * S, 2 * S, anchor="nw", text=name,
                       fill="#ffffff", font=pick_font(12, bold=True))
        cv.create_text(W - 4 * S, 3 * S, anchor="ne", text="手动",
                       fill="#ffffff", font=pick_font(9))

        if screen == 1:    # 主菜单
            items = [("基础率", "%.2f U/h" % st.get("basal_rate", 0)),
                     ("大剂量", "→"), ("排气装药", "→"),
                     ("报警", "1条" if st.get("alarm_active") else "正常"),
                     ("闭环", LOOP_MODE.get(st.get("loop_mode", 0), "已暂停")),
                     ("系统设置", "→")]
            self._menu_list(cv, S, items, sel)
            self._hint(cv, S, "上下选择  确认进入  返回首页")
        elif screen == 13:  # 系统设置
            edit = ui.get("set_edit", 0)
            items = [("日期时间", "已设置" if ui.get("clock_valid", 0) else "未设置"),
                     ("屏幕亮度", "%d%%" % ui.get("brightness", 0) + (" [调]" if edit else "")),
                     ("按键音", "开" if ui.get("keypad", 0) else "关"),
                     ("关于", "→")]
            self._menu_list(cv, S, items, sel)
            if edit:
                self._hint(cv, S, "亮度编辑中: 上下调节  确认/返回完成", ACCENT)
            else:
                self._hint(cv, S, "上下选择  确认进入  返回")
        elif screen == 14:  # 设置时间
            clk = ui.get("clk", [2026, 1, 1, 0, 0])
            fld = ui.get("clk_field", 0)
            labels = ["年", "月", "日", "时", "分", "保存"]
            items = []
            for i in range(6):
                if i < 5:
                    items.append(("%s %02d" % (labels[i], clk[i]), ""))
                else:
                    items.append(("保存 ✔ 保存并返回", ""))
            self._menu_list(cv, S, items, fld, row_h=15)
            self._hint(cv, S, "上下调整  确认下一项  返回取消")
        elif screen == 3:   # 大剂量菜单
            items = [("常规大剂量", "→"), ("方波大剂量", "→"), ("双波大剂量", "→"),
                     ("向导大剂量", "→"), ("三餐预设", "→")]
            self._menu_list(cv, S, items, sel)
            self._hint(cv, S, "上下选择  确认  返回")
        elif screen == 4:   # 常规大剂量
            self._field(cv, S, [("剂量 (U)", "%.2f" % ui.get("dose", 0), True)])
            self._hint(cv, S, "▲▼调整剂量  确认输注  返回")
        elif screen == 5:   # 方波大剂量
            self._field(cv, S, [("剂量", "%.2f U" % ui.get("dose", 0), sel == 0),
                                ("时长", "%d h" % ui.get("dur_h", 1), sel == 1)])
            self._hint(cv, S, "▲▼调整  确认切换/输注  返回")
        elif screen == 6:   # 双波大剂量
            self._field(cv, S, [("立即量", "%.2f U" % ui.get("imme", 0), sel == 0),
                                ("方波量", "%.2f U" % ui.get("sq", 0), sel == 1),
                                ("时长", "%d h" % ui.get("dur_h", 1), sel == 2)])
            self._hint(cv, S, "▲▼调整  确认切换/输注  返回")
        elif screen == 7:   # 向导大剂量
            bg = ui.get("wiz_bg", 6.5); carb = ui.get("wiz_carb", 30)
            sug = max(0.0, (bg - 6.0) / 2.0 + carb / 10.0)
            self._field(cv, S, [("血糖", "%.1f mmol/L" % bg, sel == 0),
                                ("碳水", "%.0f g" % carb, sel == 1)])
            cv.create_text(20 * S, 96 * S, anchor="nw", text="建议: %.2f U" % sug,
                           fill=GREEN, font=pick_font(11, bold=True))
            self._hint(cv, S, "▲▼调整  确认输注  返回")
        elif screen == 8:   # 三餐预设
            defm = [6.0, 8.0, 6.0]
            items = [("%s  默认 %.1f U" % (n, defm[i]), "→")
                     for i, n in enumerate(["早餐", "午餐", "晚餐"])]
            self._menu_list(cv, S, items, sel)
            self._hint(cv, S, "确认按预设输注  返回")
        elif screen == 2:   # 基础率
            local = ui.get("local_mode", 1)
            editing = ui.get("set_edit", 0) == 1
            cv.create_text(8 * S, 22 * S, anchor="nw",
                           text="模式: %s" % ("本地档案" if local else "AAPS 接管"),
                           fill=ACCENT, font=pick_font(11))
            cv.create_text(8 * S, 44 * S, anchor="nw", text="选中段 %02d:00" % sel,
                           fill=TEXT, font=pick_font(12, bold=True))
            cv.create_text(8 * S, 66 * S, anchor="nw",
                           text="当前基础率 %.2f U/h" % ui.get("sel_rate", 0.0),
                           fill=(ACCENT if editing else TEXT), font=pick_font(11, bold=editing))
            cv.create_text(8 * S, 90 * S, anchor="nw", text="共 24 段  ▲▼浏览",
                           fill=DIM, font=pick_font(10))
            if editing:
                self._hint(cv, S, "▲▼调整速率  确认保存  返回取消", ACCENT)
            elif local:
                self._hint(cv, S, "上下选择  确认编辑段  返回")
            else:
                self._hint(cv, S, "AAPS接管 确认切本地档案  返回")
        elif screen == 9:   # 排气与装药
            prime_u = ui.get("prime_u", 1.0)
            prime_active = ui.get("prime_active", 0)
            cv.create_text(12 * S, 26 * S, anchor="nw", text="3mL 注射器 (储药器)",
                           fill=TEXT, font=pick_font(11))
            if prime_active:
                cv.create_text(12 * S, 50 * S, anchor="nw", text="状态: 排气中...",
                               fill=YELLOW, font=pick_font(11, bold=True))
                cv.create_text(12 * S, 72 * S, anchor="nw", text="电机正在推注, 请稍候",
                               fill=DIM, font=pick_font(10))
                self._hint(cv, S, "返回取消排气", YELLOW)
            else:
                cv.create_text(12 * S, 50 * S, anchor="nw",
                               text="排气量: %.1f U" % prime_u,
                               fill=ACCENT, font=pick_font(11, bold=True))
                cv.create_text(12 * S, 72 * S, anchor="nw", text="状态: 待机",
                               fill=DIM, font=pick_font(10))
                self._hint(cv, S, "▲▼调量  确认排气  返回")
        elif screen == 10:  # 报警列表
            code = st.get("alarm_code", 0)
            items = [(ALARM_NAMES[i], "激活" if (st.get("alarm_active") and code == i) else "正常")
                     for i in range(7)]
            self._menu_list(cv, S, items, sel)
            self._hint(cv, S, "确认查看详情  返回")
        elif screen == 11:  # 报警详情
            a = ui.get("alarm_sel", sel)
            if 0 <= a < len(ALARM_NAMES):
                cv.create_text(12 * S, 28 * S, anchor="nw", text="原因:",
                               fill=ACCENT, font=pick_font(11))
                cv.create_text(12 * S, 48 * S, anchor="nw", text=ALARM_REASON[a],
                               fill=TEXT, font=pick_font(11))
                cv.create_text(12 * S, 78 * S, anchor="nw", text="处理:",
                               fill=ACCENT, font=pick_font(11))
                cv.create_text(12 * S, 98 * S, anchor="nw", text=ALARM_ACTION[a],
                               fill=TEXT, font=pick_font(11))
            self._hint(cv, S, "返回列表")
        elif screen == 12:  # 闭环
            conn = st.get("connected")
            paired = st.get("dana_paired")
            cv.create_text(12 * S, 26 * S, anchor="nw", text="AAPS: " + ("已连接" if conn else "断开"),
                           fill=GREEN if conn else RED, font=pick_font(11))
            pc = "已接管" if paired else ("未接管" if conn else "—")
            cv.create_text(12 * S, 48 * S, anchor="nw", text="接管: " + pc,
                           fill=GREEN if paired else (YELLOW if conn else DIM), font=pick_font(11))
            gm = st.get("glucose_mmol", 0)
            cv.create_text(12 * S, 70 * S, anchor="nw",
                           text=("血糖: %.1f mmol/L" % gm if gm > 0 else "血糖: CGM 离线"),
                           fill=TEXT, font=pick_font(11))
            lm = st.get("loop_mode", 0)
            lcol = {0: GREEN, 1: YELLOW, 2: DIM}.get(lm, DIM)
            cv.create_text(12 * S, 92 * S, anchor="nw",
                           text="模式: " + LOOP_MODE.get(lm, "已暂停"),
                           fill=lcol, font=pick_font(11))
            tbr = st.get("tbr_pct", 0)
            cv.create_text(12 * S, 116 * S, anchor="nw",
                           text=(("临时基础率: %.0f%% (%.2f U/h)" % (tbr, st.get("tbr_rate", 0)))
                                 if tbr > 0 else "临时基础率: 无"),
                           fill=YELLOW if tbr > 0 else DIM, font=pick_font(11))
        elif screen == 15:  # 关于
            cv.create_text(12 * S, 26 * S, anchor="nw", text="OpenLoop 闭环胰岛素泵",
                           fill=TITLE, font=pick_font(12, bold=True))
            cv.create_text(12 * S, 50 * S, anchor="nw", text="理论验证 / 教学原型",
                           fill=DIM, font=pick_font(10))
            cv.create_text(12 * S, 74 * S, anchor="nw", text="硬件: ESP32-C6 + DRV8825",
                           fill=TEXT, font=pick_font(11))
            cv.create_text(12 * S, 96 * S, anchor="nw", text="⚠ 严禁用于人体",
                           fill=RED, font=pick_font(11, bold=True))
            self._hint(cv, S, "返回")

    # ---- 数据模拟血糖曲线图 ----
    def _draw_chart(self, st):
        cv = self.cv_chart
        cv.delete("all")
        W, H = 360, 130
        cv.create_rectangle(0, 0, W, H, fill=BG, outline=LINE)
        cv.create_line(0, H - 1, W, H - 1, fill=LINE)
        if not self.glucose_curve:
            cv.create_text(W / 2, H / 2, text="(无血糖曲线 — 点示例或载入CSV)",
                           fill=DIM, font=pick_font(11))
            return
        dur = self.duration_val or 1440
        gmin = min(p[1] for p in self.glucose_curve)
        gmax = max(p[1] for p in self.glucose_curve)
        lo = max(40.0, gmin - 20.0)
        hi = min(400.0, gmax + 20.0)
        if hi <= lo:
            hi = lo + 40.0

        def X(t):
            return (t / dur) * W if dur > 0 else 0

        def Y(v):
            return H - ((v - lo) / (hi - lo)) * H

        # y 网格 + 刻度
        step = 40.0
        g0 = math.ceil(lo / step) * step
        for g in range(int(g0), int(hi) + 1, int(step)):
            y = Y(g)
            cv.create_line(0, y, W, y, fill="#eef2f6")
            cv.create_text(2, y, anchor="w", text=str(g), fill=DIM, font=pick_font(8))
        # 曲线
        pts = [(X(t), Y(v)) for t, v in self.glucose_curve]
        if len(pts) > 1:
            flat = [c for p in pts for c in p]
            cv.create_line(*flat, fill=ACCENT, width=2)
        # 餐时标记
        for mt, md in self.meals_val:
            x = X(mt)
            cv.create_line(x, 0, x, H, fill="#f3c6c6")
            cv.create_text(x, 4, text=f"{md:.0f}U", fill="#c0392b",
                           font=pick_font(8), anchor="n")
        # 当前回放光标
        sm = st.get("sim_min", 0.0) or 0.0
        if sm > 0:
            cx = X(sm)
            cv.create_line(cx, 0, cx, H, fill=GREEN, width=1, dash=(3, 2))
            gm = st.get("glucose_mmol", 0.0) or 0.0
            if gm > 0:
                cy = Y(gm * 18.0)
                cv.create_oval(cx - 3, cy - 3, cx + 3, cy + 3,
                               fill=GREEN, outline="#1f6b32")
            cv.create_text(cx, H - 4, text=f"{sm:.0f}m", fill=GREEN,
                           font=pick_font(8), anchor="s")

    # ---------------- 电机动画 (白底 + 医疗蓝描边) ----------------
    def _anim_tick(self):
        if not self.running:
            return
        with self.lock:
            st = dict(self.last.get("state", {}))
        target = st.get("motor_units", 0.0)
        self.motor_disp += (target - self.motor_disp) * 0.18
        if abs(target - self.motor_disp) < 1e-4:
            self.motor_disp = target
        # 电机运转指示: 大剂量进行中 或 排气中 均点亮
        ui_blk = st.get("ui", {})
        motor_active = bool(st.get("bolus_active", False)) or bool(ui_blk.get("prime_active", False))
        self._draw_motor(self.motor_disp, motor_active)
        if self.mode_var.get() == "replay":
            sm = st.get("sim_min", 0.0) or 0.0
            gm = st.get("glucose_mmol", 0.0) or 0.0
            self.replay_status.set(
                f"运行中  {sm:.0f}min · 血糖 {gm:.2f} mmol/L · "
                f"IOB {st.get('iob',0.0) or 0:.2f}U · 今日 {st.get('today',0.0) or 0:.2f}U")
            self._draw_chart(st)
        self.root.after(60, self._anim_tick)

    def _draw_motor(self, disp_u, bolus_active):
        cv = self.cv_motor
        cv.delete("all")
        W, H = 380, 210
        small = pick_font(10)
        cv.create_rectangle(0, 0, W, H, fill=BG, outline="")
        # --- 步进电机本体 (左) ---
        cv.create_rectangle(18, 60, 92, 150, fill=BLUE_L, outline="#3a4a72")
        cv.create_text(55, 52, text="STEPPER", fill=ACCENT, font=small)
        cx, cy, r = 55, 105, 22
        microsteps = disp_u * STEPS_PER_UNIT
        ang = (microsteps / 32.0) * (2 * math.pi / 200.0)
        for k in range(4):
            a = ang + k * (2 * math.pi / 4.0)
            ex = cx + r * 0.8 * math.cos(a)
            ey = cy + r * 0.8 * math.sin(a)
            cv.create_line(cx, cy, ex, ey, fill=TITLE, width=3)
        cv.create_oval(cx - 4, cy - 4, cx + 4, cy + 4, fill=TITLE, outline="")
        cv.create_oval(cx - r, cy - r, cx + r, cy + r, outline="#56628a", width=1)
        # --- 丝杠 ---
        cv.create_line(92, 105, 205, 105, fill="#7f8ea3", width=3)
        for x in range(100, 205, 8):
            cv.create_line(x, 100, x, 110, fill="#aab6cc", width=1)
        # --- 注射器 (右) ---
        bx0, by0, bx1, by1 = 205, 82, 360, 128
        cv.create_rectangle(bx0, by0, bx1, by1, fill="#eef4fa", outline="#56628a", width=1)
        frac = min(disp_u, MAX_VIS_U) / MAX_VIS_U
        plunger_x = bx0 + 10 + frac * (bx1 - bx0 - 24)
        cv.create_rectangle(plunger_x + 4, by0 + 4, bx1 - 4, by1 - 4,
                            fill="#4aa3ff", outline="")
        cv.create_rectangle(plunger_x, by0 + 2, plunger_x + 5, by1 - 2,
                            fill=TEXT, outline="#9aa6c4")
        cv.create_line(bx1, (by0 + by1) / 2, bx1 + 16, (by0 + by1) / 2,
                       fill="#9aa6c4", width=2)
        cv.create_text((bx0 + bx1) / 2, by1 + 12, text="注射器 (储药器)",
                       fill=DIM, font=small)
        # --- 标签 ---
        ms = int(microsteps)
        mm_travel = microsteps * MM_PER_STEP
        y0 = 158
        cv.create_text(10, y0, anchor="nw", text=f"已发药: {disp_u:.2f} U",
                       fill=GREEN, font=small)
        cv.create_text(10, y0 + 16, anchor="nw", text=f"微步:   {ms}",
                       fill=TEXT, font=small)
        cv.create_text(10, y0 + 32, anchor="nw",
                       text=f"柱塞行程: {mm_travel:.3f} mm", fill=TEXT, font=small)
        col = GREEN if bolus_active else DIM
        cv.create_oval(330, y0 + 4, 342, y0 + 16, fill=col, outline="")
        cv.create_text(346, y0 + 4, anchor="nw",
                       text="运转中" if bolus_active else "静止", fill=col, font=small)
        cv.create_text(346, y0 + 20, anchor="nw",
                       text=f"1U≈{int(STEPS_PER_UNIT)}步", fill=DIM, font=small)

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
        if self._sim_port_busy():
            messagebox.showinfo("提示", "模拟器已在运行 (控制通道 18923 已占用),\n直接连接即可, 无需重复启动。")
            return
        with self.lock:
            if self.connected:
                messagebox.showinfo("提示", "已连接到模拟器, 无需重复启动。")
                return
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
    root.geometry("1240x840")
    try:
        root.option_add("*Font", pick_font(11))
    except Exception:
        pass
    app = LinkGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
