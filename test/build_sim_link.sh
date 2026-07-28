#!/usr/bin/env bash
# build_sim_link.sh — 构建"联调演示模式"模拟器 (SIM_LINK_MODE=ON)
#
# 把真实固件命令分发核心 (aaps_dana.cpp / rtc_clock.cpp / host_glue.cpp) 编入
# LVGL SDL 模拟器, 由 link_session 驱动 g_pump_state, 泵屏幕每帧重绘即与 AAPS
# 命令严格同步; 并起 TCP 控制通道 (127.0.0.1:18923) 供 Python 控制面板连接。
#
# 复用已编译的 LVGL (缓存于现有 build 目录的 _deps), 仅需重编模拟器源。
#
# ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
set -e

BUILD=/Users/feelingme/pump_sim_build
SRC=/Users/feelingme/pump_sim/lvgl_sdl   # ASCII 软链, 规避含中文路径

mkdir -p "$BUILD"
cd "$BUILD"
echo ">>> 配置 SIM_LINK_MODE=ON ..."
cmake -DSIM_LINK_MODE=ON "$SRC"
echo ">>> 编译 ..."
cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
echo ">>> 完成. 二进制: $BUILD/simulator"
echo ">>> 运行: $BUILD/simulator   (另开 Python 控制面板 test/link_demo_gui.py)"
# 记录构建模式标记 (供 run_link_demo.sh 判定当前二进制是否为联调版)
touch "$BUILD/.built_link_mode"
rm -f "$BUILD/.built_mock_mode"
echo ">>> 已标记: SIM_LINK_MODE 联调版"
