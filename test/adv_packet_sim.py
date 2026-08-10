#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
离线模拟 NimBLE-Arduino 的 BLE 广播包/扫描响应包打包逻辑,
验证"先 setName → 再 enableScanResponse → FFF0 → 6E400001"顺序下:
  - 广播包(advData) 应含 设备名(0x09) + FFF0(0x03), ≤31B  → AAPS 能发现
  - 扫描响应(scanData) 应含 6E400001(0x07),          ≤31B  → 伴生 App 能发现

仅用于交付前自检, 不依赖任何硬件/ESP32 工具链。
"""
UUID128 = bytes.fromhex("6E400001B5A3F393E0A9E50E24DCCA9E")  # 伴生 App 服务
NAME_AAPS = "DAN12345AB"
NAME_RAW  = "OpenLoop-Pump"


class AdvData:
    def __init__(self, with_flags=True):
        self.payload = bytearray([0x02, 0x01, 0x06]) if with_flags else bytearray()

    def _fit(self, struct):
        return len(self.payload) + len(struct) <= 31

    def add(self, struct):
        if self._fit(struct):
            self.payload += struct
            return True
        return False


def simulate(name, enable_scan_response_first: bool):
    """
    enable_scan_response_first=True  → 复现旧 bug: 先 enableScanResponse 再 setName
    enable_scan_response_first=False → 正确顺序: 先 setName 再 enableScanResponse
    """
    adv = AdvData()          # 广播包, 带 flags
    scan = AdvData(False)    # 扫描响应, 不带 flags
    m_scan_resp = False

    def setName(n):
        nonlocal m_scan_resp
        s = bytes([1 + len(n), 0x09]) + n.encode()
        if m_scan_resp:
            return scan.add(s)     # NimBLEAdvertising::setName: m_scanResp 时优先进 scan
        return adv.add(s)

    def addUUID16(u):
        s = bytes([0x03, 0x03, u & 0xFF, (u >> 8) & 0xFF])
        if not adv.add(s):
            scan.add(s)

    def addUUID128(b):
        s = bytes([0x11, 0x07]) + b
        if not adv.add(s):          # NimBLEAdvertising::addServiceUUID: adv 放不下才转 scan
            scan.add(s)

    if enable_scan_response_first:
        m_scan_resp = True
        addUUID128(UUID128)
        setName(name)
        addUUID16(0xFFF0)
    else:
        setName(name)               # 先设名字(此时 m_scan_resp=false → 进广播包)
        m_scan_resp = True          # 再开扫描响应
        addUUID16(0xFFF0)           # FFF0 → 广播包
        addUUID128(UUID128)         # 128-bit → 广播放不下 → 自动进扫描响应

    return adv.payload, scan.payload


def report(tag, adv, scan):
    has_name = (0x09 in adv)
    has_fff0 = (0x03 in adv) and bytes([0xF0, 0xFF]) in adv
    has_128  = (0x07 in scan)
    print(f"\n=== {tag} ===")
    print(f"  广播包 advData  ({len(adv):2d}B) hex: {adv.hex()}")
    print(f"  扫描响应 scanData({len(scan):2d}B) hex: {scan.hex()}")
    print(f"  [AAPS]   广播包含设备名={has_name}  含FFF0={has_fff0}  → AAPS可发现={has_name and has_fff0}")
    print(f"  [伴生App] 扫描响应含6E400001={has_128}  → 伴生App可发现={has_128}")
    print(f"  越界检查: advData≤31={len(adv)<=31}  scanData≤31={len(scan)<=31}")


if __name__ == "__main__":
    a, s = simulate(NAME_AAPS, enable_scan_response_first=True)
    report("旧顺序(复现 bug): 先 enableScanResponse → setName", a, s)
    a, s = simulate(NAME_AAPS, enable_scan_response_first=False)
    report("新顺序(修复): 先 setName → enableScanResponse → FFF0 → 6E400001", a, s)
    a, s = simulate(NAME_RAW, enable_scan_response_first=False)
    report("非AAPS模式(OpenLoop-Pump)新顺序", a, s)
