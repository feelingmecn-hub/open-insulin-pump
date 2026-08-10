#!/bin/bash
# Dana-only + v4 固件：验证"我们自己的伴生 BLE 是否干扰 AAPS 握手"(H2 假设)
# 与 build_v4.sh 的唯一区别：额外 -DDISABLE_COMPANION —— 屏蔽自定义 NUS 伴生服务，
#   使 GATT server 上只挂 Dana 的 FFF0/FFF1/FFF2，隔离"我们自己蓝牙"的变量。
# 安全配置仍为 v4：sm_bonding=1 + 关 LESC(legacy pairing)，过 AAPS 的 BOND_BONDED 门。
export JAVA_HOME=/usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
cd /Users/feelingme/Desktop/闭环胰岛素泵项目/code/esp32_firmware
~/.local/bin/arduino-cli compile \
  -b "esp32:esp32:esp32c6:PartitionScheme=custom,CDCOnBoot=cdc" \
  --build-property "build.extra_flags=-DUSE_AAPS_DANA -DDISABLE_COMPANION -DMOTOR_DEBUG_UNLOCKED -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32" \
  . 2>&1 | tail -8
