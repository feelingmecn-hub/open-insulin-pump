#!/bin/bash
export JAVA_HOME=/usr/local/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
cd /Users/feelingme/Desktop/闭环胰岛素泵项目/code/esp32_firmware
~/.local/bin/arduino-cli compile \
  -b "esp32:esp32:esp32c6:PartitionScheme=custom,CDCOnBoot=cdc" \
  --build-property "build.extra_flags=-DUSE_AAPS_DANA -DMOTOR_DEBUG_UNLOCKED -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 -DLV_CONF_INCLUDE_SIMPLE -DESP32" \
  . 2>&1 | tail -8
