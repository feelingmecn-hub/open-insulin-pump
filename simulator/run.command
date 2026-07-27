#!/bin/bash
# 闭环胰岛素泵 LVGL 模拟器 - 一键启动
# 双击本文件即可在 Terminal 中运行模拟器并弹出 SDL 窗口。
# 首次运行若被 macOS 拦截，请到「系统设置 → 隐私与安全性」点「仍要打开」。
# 注意: 项目路径不能含中文 (详见 simulator/lvgl_sdl/README.md)，请用 ASCII 软链构建。
DIR="$(cd "$(dirname "$0")" && pwd)"   # <项目>/simulator
CANDIDATES=(
  "$HOME/pump_sim_build/simulator"
  "$DIR/lvgl_sdl/build/simulator"
)
for BIN in "${CANDIDATES[@]}"; do
  if [ -x "$BIN" ]; then exec "$BIN"; fi
done
osascript -e 'display dialog "模拟器尚未构建。请按 simulator/lvgl_sdl/README.md 用 ASCII 路径构建。" buttons {"好"} default button 1' 2>/dev/null
echo "错误: 未找到模拟器二进制，候选路径:"
printf '  %s\n' "${CANDIDATES[@]}"
sleep 3
exit 1
