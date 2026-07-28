#!/usr/bin/env bash
# run_tests.sh — 编译并运行 aaps_dana 宿主单测，并与 AAPS 预言机做字节流 diff
#
# 验证：C 实现（aaps_dana.cpp）与 AAPS 自身 BleEncryption.kt（oracle_aaps.py 逐字转译）
# 对相同场景输出的 over-the-air 字节流完全一致 → 证明 Dana-i BLE 协议字节级兼容。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
BIN="$HERE/aaps_dana_test"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "==> [1/4] 编译 C 宿主单测（g++，不定义 USE_AAPS_DANA → 仅纯逻辑）"
g++ -std=c++17 -I"$SRC" "$HERE/aaps_dana_test.cpp" "$SRC/aaps_dana.cpp" -o "$BIN"
echo "    编译成功: $BIN"

echo "==> [2/4] 运行 C 单测（assertions + 黄金字节流）"
"$BIN" | tee "$TMP/c_full.txt"
if grep -q "SOME_ASSERTS_FAILED" "$TMP/c_full.txt"; then
    echo "!!! C 断言失败，终止。" >&2; exit 1
fi

echo "==> [3/4] 运行 AAPS 预言机（Python 转译）"
python3 "$HERE/oracle_aaps.py" | tee "$TMP/py_full.txt" >/dev/null

echo "==> [4/4] 抽取 PKT 行并 diff（C vs AAPS 预言机）"
grep '^PKT ' "$TMP/c_full.txt" | sort -k1 > "$TMP/c_pkt.txt"
grep '^PKT ' "$TMP/py_full.txt" | sort -k1 > "$TMP/py_pkt.txt"

if diff -q "$TMP/c_pkt.txt" "$TMP/py_pkt.txt" >/dev/null; then
    echo ""
    echo "✅ PASS：C 实现与 AAPS BleEncryption 字节级一致（$(wc -l < "$TMP/c_pkt.txt") 个场景全部匹配）。"
    echo "   涵盖：握手包(PUMP_CHECK/TIME_INFO) + 命令响应 + 通知 + 200 随机命令包。"
    echo "   下一步：在 Arduino IDE 以 -DUSE_AAPS_DANA 编译烧录，用真实 AndroidAPS 配对验证。"
    exit 0
else
    echo ""
    echo "❌ FAIL：C 实现与 AAPS 预言机字节流不一致！差异如下：" >&2
    diff "$TMP/c_pkt.txt" "$TMP/py_pkt.txt" >&2 || true
    exit 1
fi
