#!/usr/bin/env bash
# aaps_auto_reconnect_test.sh —— 无人值守「AAPS↔泵 重连稳定性」测试（零出药风险）
# 用法：手机 USB 连 Mac，在本目录终端执行： bash aaps_auto_reconnect_test.sh
# 原理：adb 强制关/开蓝牙，逼迫 AAPS 反复重连；全程抓 logcat，最后自动判定成败。
set -u
export PATH="$HOME/Library/Android/sdk/platform-tools:$PATH"
TS=$(date +%Y%m%d_%H%M%S)
LOG="/tmp/aaps_reconnect_${TS}.txt"
SUMMARY="/tmp/aaps_reconnect_${TS}_SUMMARY.txt"

echo "==== 清空 logcat 缓冲 ===="
adb logcat -c 2>&1
echo "==== 开始后台抓日志 -> $LOG ===="
( adb logcat -v time > "$LOG" 2>&1 ) &
LOGPID=$!

echo "==== 强制蓝牙 关/开 共 5 轮，逼迫 AAPS 反复重连 ===="
for i in 1 2 3 4 5; do
  echo "-- 第 $i 轮：关蓝牙 --"
  adb shell svc bluetooth disable 2>&1
  sleep 6
  echo "-- 第 $i 轮：开蓝牙 --"
  adb shell svc bluetooth enable 2>&1
  sleep 12   # 留时间给 AAPS 扫描+连接+完成握手
done

echo "==== 停止抓日志 ===="
kill "$LOGPID" 2>/dev/null
sleep 1

echo "==== 自动判定 ===="
CONN_OK=$(grep -c "ENCRYPTION__PUMP_CHECK" "$LOG" 2>/dev/null); CONN_OK=${CONN_OK:-0}
SERV_OK=$(grep -c "onServicesDiscovered"     "$LOG" 2>/dev/null); SERV_OK=${SERV_OK:-0}
STUCK=$(grep -c   "连接卡在 connecting 超时" "$LOG" 2>/dev/null); STUCK=${STUCK:-0}
FAIL_CONN=$(grep -c "conn_complete bda:00:00:00:00:00:00" "$LOG" 2>/dev/null); FAIL_CONN=${FAIL_CONN:-0}

{
  echo "==== AAPS 重连测试结论 ($TS) ===="
  echo "握手成功(ENCRYPTION__PUMP_CHECK) 次数: $CONN_OK"
  echo "服务发现(onServicesDiscovered) 次数:   $SERV_OK"
  echo "卡连接中超时(连接卡在 connecting 超时) 次数: $STUCK"
  echo "建链失败(conn_complete 00:00...) 次数:   $FAIL_CONN"
  if [ "$STUCK" -eq 0 ] && [ "$FAIL_CONN" -eq 0 ] && [ "$CONN_OK" -gt 0 ]; then
    echo "判定: 重连稳定，AAPS 每次都能连上泵并完成握手"
  elif [ "$STUCK" -gt 0 ] || [ "$FAIL_CONN" -gt 0 ]; then
    echo "判定: 出现连接卡顿/失败，需进一步排查（看 $LOG）"
  else
    echo "判定: 未见明确连接/失败日志，可能 AAPS 未触发重连，检查 $LOG"
  fi
  echo "完整日志: $LOG"
} | tee "$SUMMARY"

echo ""
echo "请把上面『判定』那行，以及 $SUMMARY 的内容发给我即可。"
