#!/usr/bin/env bash
# 闭环胰岛素泵 — AAPS 蓝牙动态控制模拟联调 编译/运行脚本
#
# 在主机 (PC/macOS) 上编译"真实固件命令分发代码" (aaps_dana.cpp, 开 USE_AAPS_DANA),
# 用假 NimBLE + 假电机/基础率 桩件驱动, 由 AAPS 蓝牙客户端模拟器跑完整闭环会话。
#
# 用法:
#   ./test/run_link_sim.sh            # 跑脚本化完整会话
#   ./test/run_link_sim.sh -i         # 脚本化 + 进入交互 REPL
#
# ⚠️ 实验项目 / 教学原型, 严禁用于任何人体。
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/code/esp32_firmware/src"
HOST="$ROOT/test/host"
BUILD="/tmp/aaps_link_sim_build"

mkdir -p "$BUILD"

# 优先使用系统 C++ 编译器 (macOS: clang++ via `c++`)
CXX="${CXX:-c++}"
echo ">>> 使用编译器: $($CXX --version 2>&1 | head -1)"

echo ">>> 编译 aaps_dana.cpp (真实固件, USE_AAPS_DANA)"
$CXX -std=c++17 -DUSE_AAPS_DANA -DAAPS_DANA_HOST_TEST \
     -I"$SRC" -I"$HOST/inc" \
     -c "$SRC/aaps_dana.cpp" -o "$BUILD/aaps_dana.o"

echo ">>> 编译 rtc_clock.cpp (真实固件, 主机联调用内存模拟 RTC)"
$CXX -std=c++17 -DAAPS_DANA_HOST_TEST -I"$SRC" -I"$HOST/inc" \
     -c "$SRC/rtc_clock.cpp" -o "$BUILD/rtc_clock.o"

echo ">>> 编译 host_glue.cpp (假硬件层 + TX 捕获)"
$CXX -std=c++17 -DHOST_GLUE_OWNS_STATE -I"$SRC" -I"$HOST/inc" \
     -c "$HOST/host_glue.cpp" -o "$BUILD/host_glue.o"

echo ">>> 编译 aaps_link_sim.cpp (AAPS 蓝牙客户端模拟器 + 联调脚本)"
$CXX -std=c++17 -DUSE_AAPS_DANA -DAAPS_DANA_HOST_TEST \
     -I"$SRC" -I"$HOST/inc" \
     -c "$ROOT/test/aaps_link_sim.cpp" -o "$BUILD/aaps_link_sim.o"

echo ">>> 链接"
$CXX "$BUILD/aaps_dana.o" "$BUILD/rtc_clock.o" "$BUILD/host_glue.o" "$BUILD/aaps_link_sim.o" \
     -o "$BUILD/aaps_link_sim"

echo ">>> 运行"
"$BUILD/aaps_link_sim" "$@"
