#!/usr/bin/env bash
# ============================================================================
#  run_link_demo.sh  —  AAPS 蓝牙动态控制 · 联调四路同步演示 · 一键启动器
#
#  自动完成: (1) 构建联调版模拟器 (若二进制缺失或源码有更新)
#            (2) 启动模拟器 (本机会弹出真实泵屏 SDL 窗口; 无显示器则无窗)
#            (3) 等待 TCP 控制通道 127.0.0.1:18923 就绪
#            (4) 启动 Python 控制面板 GUI (四宫格同步演示)
#            (5) GUI 关闭后自动清理模拟器进程
#
#  用法:
#    bash test/run_link_demo.sh              # 常规一键启动
#    FORCE=1 bash test/run_link_demo.sh      # 强制重新构建
#    HEADLESS=1 bash test/run_link_demo.sh   # 无窗模式 (仅 GUI 内的泵屏复刻)
#    SIM_WINDOW=1 bash test/run_link_demo.sh # 强制 SDL 窗口 (默认桌面会话即此)
#    SKIP_GUI=1  bash test/run_link_demo.sh  # 仅启动模拟器, 不启 GUI (CI/沙箱验证用)
#
#  ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。真机仅可用空注射器/水验证。
# ============================================================================
set -u

PROJ="/Users/feelingme/Desktop/闭环胰岛素泵项目"
BUILD="$PROJ/test/build_sim_link.sh"
BIN="/Users/feelingme/pump_sim_build/simulator"
GUI="$PROJ/test/link_demo_gui.py"
PORT=18923

cd "$PROJ" || { echo "无法进入项目目录 $PROJ"; exit 1; }

echo "============================================================"
echo " AAPS 联调四路同步演示 · 一键启动"
echo " (①步进电机推药 ②AAPS发送 ③固件接收 ④泵屏UI 实时同步)"
echo "============================================================"

# ---- 0) 检查 GUI 依赖 (tkinter) ----
if [ "${SKIP_GUI:-0}" != "1" ]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "错误: 未找到 python3 (GUI 依赖)。请先安装 Python 3。"
    exit 1
  fi
  if ! python3 -c "import tkinter" >/dev/null 2>&1; then
    echo "错误: 当前 python3 缺少 tkinter (GUI 依赖)。"
    echo "       macOS 自带 python3 通常已含; 或执行: brew install python-tk"
    exit 1
  fi
fi

# ---- 1) 确保联调版二进制存在且为最新 ----
need_build=0
if [ ! -x "$BIN" ]; then
  need_build=1
  echo "[1/4] 未找到模拟器二进制, 需要构建"
elif [ "${FORCE:-0}" = "1" ]; then
  need_build=1
  echo "[1/4] FORCE=1, 强制重新构建"
else
  # 比较源码最新修改时间 vs 二进制修改时间
  src_mt=$(find "$PROJ/simulator" "$PROJ/code/esp32_firmware/src" "$PROJ/test/host" \
           \( -name '*.cpp' -o -name '*.h' -o -name 'CMakeLists.txt' \) -print0 2>/dev/null \
           | xargs -0 -I{} stat -f '%m' {} 2>/dev/null | sort -n | tail -1)
  bin_mt=$(stat -f '%m' "$BIN" 2>/dev/null || echo 0)
  if [ -n "$src_mt" ] && [ "$src_mt" -gt "$bin_mt" ]; then
    need_build=1
    echo "[1/4] 检测到源码已更新, 重新构建"
  else
    echo "[1/4] 二进制已是最新, 跳过构建"
  fi
fi

if [ "$need_build" -eq 1 ]; then
  echo "[1/4] 构建联调版模拟器 (首次约 1-2 分钟, 后续秒级) ..."
  if ! bash "$BUILD"; then
    echo "错误: 模拟器构建失败, 中止。详见上方输出。"
    exit 1
  fi
fi

# ---- 2) 启动模拟器 (后台) ----
# 决定窗口 / 无窗模式
MODE_HEADLESS=0
if [ "${HEADLESS:-0}" = "1" ]; then
  MODE_HEADLESS=1
elif [ -z "${DISPLAY:-}" ] && [ -z "${SSH_CONNECTION:-}" ]; then
  # 无显示器环境 (罕见, 桌面会话一般有 DISPLAY)
  MODE_HEADLESS=1
fi
if [ "${SIM_WINDOW:-0}" = "1" ]; then
  MODE_HEADLESS=0
fi

echo "[2/4] 启动模拟器 ($([ "$MODE_HEADLESS" = 1 ] && echo 无窗 || echo SDL窗口)) ..."
if [ "$MODE_HEADLESS" = "1" ]; then
  SIM_HEADLESS=1 "$BIN" > /tmp/sim_link_demo.log 2>&1 &
else
  "$BIN" > /tmp/sim_link_demo.log 2>&1 &
fi
SIM_PID=$!
echo "      模拟器 PID=$SIM_PID  (日志: /tmp/sim_link_demo.log)"

# ---- 3) 等待控制通道端口 ----
echo "[3/4] 等待控制通道 127.0.0.1:$PORT ..."
wait_port() {
  python3 - "$PORT" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
for _ in range(60):
    try:
        s = socket.socket(); s.settimeout(0.3)
        s.connect(('127.0.0.1', p)); s.close(); sys.exit(0)
    except Exception:
        time.sleep(0.3)
sys.exit(1)
PY
}
if ! wait_port; then
  echo "错误: 控制通道未在 18 秒内就绪。模拟器日志:"
  tail -n 25 /tmp/sim_link_demo.log
  kill "$SIM_PID" 2>/dev/null
  exit 1
fi
echo "      控制通道已就绪 ✓"

# ---- 清理钩子 ----
cleanup() {
  if [ -n "${SIM_PID:-}" ] && kill -0 "$SIM_PID" 2>/dev/null; then
    kill -TERM "$SIM_PID" 2>/dev/null
    kill -TERM "-$SIM_PID" 2>/dev/null   # 进程组 (模拟器内部线程)
  fi
}
trap cleanup EXIT INT TERM

# ---- 4) 启动 GUI ----
if [ "${SKIP_GUI:-0}" = "1" ]; then
  echo "[4/4] SKIP_GUI=1: 仅保持模拟器运行, 按 Ctrl-C 退出"
  wait "$SIM_PID"
else
  echo "[4/4] 启动控制面板 GUI ..."
  echo "      (演示时点击 ▶ 播放, 看四路同步; 关闭窗口即自动停止模拟器)"
  python3 "$GUI"
fi

echo "演示结束, 已清理模拟器进程。"
