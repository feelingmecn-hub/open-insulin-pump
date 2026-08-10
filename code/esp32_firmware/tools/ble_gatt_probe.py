#!/usr/bin/env python3
"""
BLE 外设 GATT 真机自检工具（macOS / Linux）

用途：绕开手机与 AAPS，用第三方 BLE 客户端直连泵，把泵**实际注册出来**的
GATT 服务表原样打印出来，并断言 Dana 的 FFF0/FFF1/FFF2 UUID 与属性正确。

为什么必须有这个工具（2026-08-08 血泪教训）：
    固件里 `NimBLEUUID(const uint8_t*, 16)` 按 BLE 线序（小端）解析字节数组，
    如果照人类可读顺序写成大端数组，注册出来的 UUID 会**整体翻转**：
        期望  0000fff0-0000-1000-8000-00805f9b34fb
        实际  fb349b5f-8000-0080-0010-0000f0ff0000
    这种错误：看源码看不出来、泵侧 trace 也记不到、手机只表现为"连不上"。
    唯一可靠的发现手段就是第三方客户端直连枚举。

依赖：
    /Users/feelingme/.workbuddy/binaries/python/envs/default/bin/pip install bleak

用法：
    python3 ble_gatt_probe.py                 # 默认找 DAN12345AB
    python3 ble_gatt_probe.py --name DAN12345AB
    python3 ble_gatt_probe.py --scan-only     # 只扫描，列出周边所有 BLE 设备
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEFAULT_NAME = "DAN12345AB"

# Dana-i 标准 GATT 契约：AAPS BLEComm.findCharacteristic() 对这些字符串做精确匹配
EXPECT = {
    "service": "0000fff0-0000-1000-8000-00805f9b34fb",
    "fff1": ("0000fff1-0000-1000-8000-00805f9b34fb", {"read", "notify"}),
    "fff2": ("0000fff2-0000-1000-8000-00805f9b34fb", {"write", "write-without-response"}),
}
CCCD = "00002902-0000-1000-8000-00805f9b34fb"


async def scan_only(timeout: float) -> int:
    print(f"扫描 {timeout:.0f} 秒 ...\n")
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (dev, adv) in sorted(found.items(), key=lambda kv: -kv[1][1].rssi):
        name = adv.local_name or dev.name or "(no name)"
        print(f"{addr}  rssi={adv.rssi:>4}  name={name}")
        if adv.service_uuids:
            print(f"      adv services = {adv.service_uuids}")
    print(f"\n共 {len(found)} 个设备")
    return 0


async def probe(name: str, timeout: float) -> int:
    print(f"查找 {name} ...")
    dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
    if not dev:
        print(f"❌ 未扫描到 {name}")
        print("   排查：泵是否上电？是否已被占满连接而停止广播？（连接后必须继续广播）")
        return 2

    print(f"✅ 扫描到: {dev.address}")
    print("建立 GATT 连接 ...")
    async with BleakClient(dev, timeout=25.0) as cli:
        print(f"✅ 已连接\n")
        print("--- 实际注册的 GATT 服务表 ---")

        seen_chars = {}
        for svc in cli.services:
            print(f"SERVICE {svc.uuid}")
            for ch in svc.characteristics:
                props = set(ch.properties)
                seen_chars[ch.uuid.lower()] = props
                print(f"   CHAR {ch.uuid}  props={sorted(props)}")
                for d in ch.descriptors:
                    print(f"       DESC {d.uuid}")

        print("\n--- Dana 契约校验 ---")
        ok = True

        svc_uuids = {s.uuid.lower() for s in cli.services}
        if EXPECT["service"] in svc_uuids:
            print(f"✅ 服务 {EXPECT['service']}")
        else:
            ok = False
            print(f"❌ 缺少服务 {EXPECT['service']}")
            rev = [u for u in svc_uuids if u.startswith("fb349b5f")]
            if rev:
                print(f"   ⚠️ 发现字节序翻转的 UUID {rev[0]}")
                print("      → 固件用了大端手写数组 + NimBLEUUID(ptr,16)；")
                print("        改用 NimBLEUUID((uint16_t)0xFFF0) 即可修复。")

        for key in ("fff1", "fff2"):
            uuid, need = EXPECT[key]
            props = seen_chars.get(uuid)
            if props is None:
                ok = False
                print(f"❌ 缺少特征 {uuid}")
            elif not need.issubset(props):
                ok = False
                print(f"❌ {key.upper()} 属性不足: 有 {sorted(props)}，需要 {sorted(need)}")
            else:
                print(f"✅ {key.upper()} {uuid} props={sorted(props)}")

        fff1_uuid = EXPECT["fff1"][0]
        has_cccd = any(
            d.uuid.lower() == CCCD
            for s in cli.services
            for ch in s.characteristics
            if ch.uuid.lower() == fff1_uuid
            for d in ch.descriptors
        )
        if has_cccd:
            print(f"✅ FFF1 带 CCCD(2902)，AAPS 可写描述符使能 notify")
        else:
            ok = False
            print(f"❌ FFF1 缺 CCCD(2902)，AAPS 无法订阅")

        print("\n" + ("🎉 全部通过：AAPS 能找到并订阅 FFF1" if ok else "💥 校验失败，见上方 ❌"))
        return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="BLE 外设 GATT 真机自检")
    ap.add_argument("--name", default=DEFAULT_NAME, help=f"设备名 (默认 {DEFAULT_NAME})")
    ap.add_argument("--timeout", type=float, default=15.0, help="扫描超时秒数")
    ap.add_argument("--scan-only", action="store_true", help="只扫描，不连接")
    args = ap.parse_args()

    coro = scan_only(args.timeout) if args.scan_only else probe(args.name, args.timeout)
    return asyncio.run(coro)


if __name__ == "__main__":
    sys.exit(main())
