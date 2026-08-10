#!/bin/bash
# flash_macos.command — 闭环胰岛素泵固件一键烧录 (macOS)
# 底层依赖 Arduino CLI；首次运行会自动安装 arduino-cli + esp32 板包 + 依赖库。
#
# ⚠️ 安全红线（务必遵守）：
#     本产品为「教学 / 理论验证原型」，不是医疗器械，严禁用于任何人体！
#     真机仅可用「空注射器 + 水」验证机械动作，不可装入胰岛素或连接人体。
#
# 用法：把本文件放在 code/esp32_firmware/flash_tools/ 下，双击即可。
#       若从终端运行：bash flash_macos.command
#
# 注：本脚本用于「从源码重新编译并烧录」。如果只是想烧录、不想在本机装
#     工具链，请直接用 build_out/release/ 里的预编译 bin + 浏览器 ESP Web
#     Flasher 烧录（步骤见 docs/13-烧录指南.md §2，最省事）。

set -e

# 定位到固件根目录（esp32_firmware.ino 所在），无论脚本从哪启动
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$FIRMWARE_DIR"

clear
echo "============================================================"
echo "   闭环胰岛素泵 · 固件一键烧录工具 (macOS)"
echo "------------------------------------------------------------"
echo "   [警告] 教学原型：仅限「空注射器 + 水」验证机械动作"
echo "   [警告] 严禁用于人体 / 严禁装入胰岛素"
echo "============================================================"
echo "   固件目录: $FIRMWARE_DIR"
echo ""

# 1) 定位 / 安装 arduino-cli
ARD=""
for p in arduino-cli "$HOME/.local/bin/arduino-cli" "/usr/local/bin/arduino-cli" "/tmp/bin/arduino-cli" "$FIRMWARE_DIR/flash_tools/arduino-cli"; do
  if command -v "$p" >/dev/null 2>&1 || [ -x "$p" ]; then ARD="$p"; break; fi
done
if [ -z "$ARD" ]; then
  echo "[信息] 未检测到 arduino-cli，正在自动安装到 $HOME/.local/bin ..."
  mkdir -p "$HOME/.local/bin"
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh -s -- -b "$HOME/.local/bin" || {
    echo "[错误] arduino-cli 自动安装失败。请手动安装: https://arduino.github.io/arduino-cli/ 后重跑本脚本。"; exit 1; }
  ARD="$HOME/.local/bin/arduino-cli"
fi
export PATH="$(dirname "$ARD"):$PATH"
echo "[OK] arduino-cli: $($ARD version 2>&1 | head -1)"

# 2) 安装 esp32 板包 (含 ESP32-C6)
if ! "$ARD" core list 2>/dev/null | grep -q "esp32:esp32"; then
  echo "[信息] 安装 esp32 板包 (3.1.1)，首次需下载工具链，请稍候..."
  "$ARD" config init --additional-urls "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json" >/dev/null 2>&1 || true
  "$ARD" core update-index
  "$ARD" core install esp32:esp32@3.1.1
fi
echo "[OK] esp32 板包就绪"

# 3) 安装依赖库 (GFX / LVGL 9.5.0 / NimBLE-Arduino)
"$ARD" lib install "GFX Library for Arduino" "LVGL@9.5.0" "NimBLE-Arduino" >/dev/null 2>&1 || true
echo "[OK] 依赖库就绪 (GFX / LVGL 9.5.0 / NimBLE-Arduino)"

# 4) 选择固件变体
echo ""
echo "请选择固件变体："
echo "   1) 默认（自定义 BLE，本地调试通道）"
echo "   2) AAPS Dana-i 伪装（被 AndroidAPS 当作 Dana-i 驱动）"
read -p "输入 1 或 2 [默认 1]：" VARIANT || true
FQBN="esp32:esp32:esp32c6:PartitionScheme=custom,CDCOnBoot=cdc"
# 基础宏: 显式开启 USB-CDC-On-Boot(=1) 并让 lv_conf.h 被 LVGL 正确包含。
# 注: 用 build.extra_flags 统一传入会覆盖 FQBN menu 的 .esp32 子项, 故 USB_CDC 需手动带上。
if [ "$VARIANT" = "2" ]; then
  EXTRA='--build-property build.extra_flags="-DUSE_AAPS_DANA -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32"'
  echo "   -> AAPS Dana-i 变体"
else
  EXTRA='--build-property build.extra_flags="-DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32"'
  echo "   -> 默认变体"
fi

# 5) 检测 USB 串行端口
PORTS=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true)
if [ -z "$PORTS" ]; then
  echo ""
  echo "[错误] 未发现 USB 串行端口。请确认："
  echo "   1) 用一根「数据线」连接开发板 USB-C 口到电脑；"
  echo "   2) 开发板已上电（USB 插入即上电）；"
  echo "   3) 端口应出现在 /dev/cu.usb*（如 /dev/cu.usbmodem1234）。"
  echo "   然后重新运行本脚本。"
  exit 1
fi
PORT=$(echo "$PORTS" | head -1)
N=$(echo "$PORTS" | wc -l | tr -d ' ')
if [ "$N" -gt 1 ]; then
  echo ""
  echo "发现多个串行端口："
  echo "$PORTS"
  read -p "请输入要使用的端口完整路径：" PORT || true
fi
echo "[信息] 使用端口: $PORT"

# 6) 编译 + 烧录
echo ""
echo ">>> 开始编译并烧录 (FQBN=$FQBN) ..."
echo "    （首次编译会下载 ESP32 工具链与库，可能需数分钟，请耐心等待）"
"$ARD" compile -b "$FQBN" $EXTRA --upload -p "$PORT" "$FIRMWARE_DIR"

# 7) 可选串口监视器
echo ""
read -p "烧录完成。是否打开串口监视器 (115200) 查看启动日志？[y/N]：" MON || true
if [ "$MON" = "y" ] || [ "$MON" = "Y" ]; then
  echo ">>> 打开串口监视器 (Ctrl+C 退出) ..."
  "$ARD" monitor -p "$PORT" -b "$FQBN" --config baudrate=115200 || true
fi

echo ""
echo "============================================================"
echo "   完成。请移步 docs/13-烧录指南.md 查看空机验证步骤。"
echo "   [警告] 再次提醒：教学原型，仅空注射器+水，严禁人体！"
echo "============================================================"
