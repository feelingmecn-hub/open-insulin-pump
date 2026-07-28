#!/usr/bin/env bash
# build_sim_mock.sh — 构建"标准 mock 演示模式"模拟器 (SIM_LINK_MODE=OFF)
#
# 用于回归: 确认联调模式改动未破坏原有 mock 模拟器 (mock 数据自动演示)。
# 与 build_sim_link.sh 共用同一 build 目录, 仅切换 SIM_LINK_MODE 选项,
# 复用已编译的 LVGL, 仅重编模拟器源。
#
# ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
set -e

BUILD=/Users/feelingme/pump_sim_build
SRC=/Users/feelingme/pump_sim/lvgl_sdl   # ASCII 软链, 规避含中文路径

mkdir -p "$BUILD"
cd "$BUILD"
echo ">>> 配置 SIM_LINK_MODE=OFF ..."
cmake -DSIM_LINK_MODE=OFF "$SRC"
echo ">>> 编译 ..."
cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
echo ">>> 完成. 二进制: $BUILD/simulator (mock 模式)"
# 记录构建模式标记 (供 run_link_demo.sh 判定; mock 标记存在时一键脚本会重建联调版)
touch "$BUILD/.built_mock_mode"
rm -f "$BUILD/.built_link_mode"
echo ">>> 已标记: 标准 mock 模式"
