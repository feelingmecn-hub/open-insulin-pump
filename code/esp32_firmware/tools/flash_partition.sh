#!/usr/bin/env bash
# 闭环胰岛素泵 — ESP32-C6 分区烧录脚本（保留 NVS / bonding）
#
# ⚠️ 实验项目 / 教学原型，严禁用于任何人体。
#
# 用法:
#   ./tools/flash_partition.sh                 # 自动探测 /dev/cu.usbmodem* 并烧录
#   ./tools/flash_partition.sh /dev/cu.usbmodemXXXX   # 指定端口
#
# 设计要点（与「merged.bin 烧 0x0 会清 NVS」的坑对应）:
#   仅烧录三段「程序分区」，绝不烧 merged.bin 到 0x0：
#     bootloader  -> 0x0
#     partitions  -> 0x8000
#     app         -> 0x10000
#   NVS(0x9000) / otadata(0xe000) / dose_log / dana_trace 保持不变，
#   因此 AAPS bonding、剂量追溯日志、BLE 握手追踪全部保留，无需重新配对。
#
# 前置: pip install esptool（本机已装于托管 venv: ~/.workbuddy/binaries/python/envs/default）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build_out/v10_debug}"

# --- 解析 esptool ---
if command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL="esptool.py"
elif [ -x "$HOME/.workbuddy/binaries/python/envs/default/bin/esptool" ]; then
  ESPTOOL="$HOME/.workbuddy/binaries/python/envs/default/bin/esptool"
else
  ESPTOOL="python3 -m esptool"
fi

# --- 探测端口 ---
if [ $# -ge 1 ] && [[ "$1" == /dev/* ]]; then
  PORT="$1"
else
  PORTS=(/dev/cu.usbmodem*)
  if [ ${#PORTS[@]} -eq 0 ]; then
    echo "❌ 未找到 /dev/cu.usbmodem* — 请把泵置入 boot 模式并用 USB 数据线连接 Mac。" >&2
    exit 1
  fi
  PORT="${PORTS[0]}"
  if [ ${#PORTS[@]} -gt 1 ]; then
    echo "⚠️  发现多个串口，默认用第一个: $PORT（如需指定请传参）" >&2
  fi
fi

BL="$BUILD/esp32_firmware.ino.bootloader.bin"
PT="$BUILD/esp32_firmware.ino.partitions.bin"
APP="$BUILD/esp32_firmware.ino.bin"

for f in "$BL" "$PT" "$APP"; do
  if [ ! -f "$f" ]; then
    echo "❌ 缺少固件镜像: $f" >&2
    echo "   先用 build_v4.sh 或 arduino-cli 编译到 $BUILD" >&2
    exit 1
  fi
done

echo ">>> 端口: $PORT"
echo ">>> 固件目录: $BUILD"
echo ">>> 仅烧程序分区（保留 NVS/bond/dose_log）..."

$ESPTOOL --chip esp32c6 --port "$PORT" --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash 0x0   "$BL" \
             0x8000 "$PT" \
             0x10000 "$APP"

echo "✅ 烧录完成。设备已硬复位开机，bonding 与日志保留。"
echo "   下一步: 手机 AAPS 正常模式连泵，adb logcat 验证菜单 TBR / 大剂量落账本。"
