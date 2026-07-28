#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
oracle_aaps.py — AAPS Dana-i (BLE) 协议逐字转译「预言机」

目的：作为黄金基准，验证 C 实现（aaps_dana.cpp）与 AndroidAPS 自身
`pump/danars/encryption/BleEncryption.kt` 字节级一致。本脚本不依赖任何 C，
而是把 AAPS 的 CRC / 设备名 XOR / BLE5 二级加解密 / 信封构建原样移植，
对相同场景输出 over-the-air 字节流；由 run_tests.sh 与 C 测试输出做 diff。

⚠️ 仅供测试/教学，禁止用于真实人体设备。
"""
import sys

# AAPS bleEncryptionMatrix（100 字节，必须与 BleEncryption.kt 完全一致）
BLE5_MATRIX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,
    0x67,0x2b,0xfe,0xd7,0xab,0x76,0x6c,0x70,0x48,0x50,
    0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,
    0x9d,0x84,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,
    0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x47,0xf1,
    0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,
    0xaa,0x18,0xbe,0x1b,0x09,0x83,0x2c,0x1a,0x1b,0x6e,
    0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0xa0,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,
    0xbe,0x39,0x4a,0x4c,0x58,0xcf,0xb0,0x54,0xbb,0x16,
]

START_PACKET = 0xA5
END_PACKET   = 0x5A
ENC_START    = 0xAA
ENC_END      = 0xEE

# 安全版本 / 连接状态
SEC_DEFAULT, SEC_RSV3, SEC_BLE5 = 0, 1, 2
CONN_INIT, CONN_PUMP_CHECK, CONN_HANDSHAKE_DONE = 0, 1, 2


def ble5_enc_key(ble5key):
    k = [int(c) for c in ble5key]
    return [
        BLE5_MATRIX[k[0]*10 + k[1]],
        BLE5_MATRIX[k[2]*10 + k[3]],
        BLE5_MATRIX[k[4]*10 + k[5]],
    ]


def crc16(data, sec, conn):
    """逐字移植 BleEncryption.generateCrc（默认 / BLE5 分支）。"""
    crc = 0
    for b in data:
        result = ((crc << 8) | (crc >> 8)) & 0xFFFF
        result ^= b
        result ^= (result & 0xFF) >> 4
        result ^= (result << 12) & 0xFFFF
        low = result & 0xFF
        # 注意：AAPS 为 result.and(0xFFu).shl(N) —— 仅对底数做 &0xFF，移位结果不截断。
        # 故移位项不得再 &0xFF（否则会丢失 8~11 位），仅末轮对 crc 做 &0xFFFF。
        if sec == SEC_BLE5 and conn == CONN_HANDSHAKE_DONE:
            result ^= (low << 4) | ((low >> 3) << 2)
        else:
            result ^= (low << 3) | ((low >> 2) << 5)
        crc = result & 0xFFFF
    return crc


def encode_array_by_sn(buf, devname):
    """逐字移植 encodeArrayBySn：对 bytes[3 .. size-3] 按 codingBytes[i%3] XOR。"""
    buf = bytearray(buf)
    coding = [0, 0, 0]
    for i in range(10):
        ch = ord(devname[i])
        if i < 3:
            coding[0] = (coding[0] + ch) & 0xFF
        elif i < 8:
            coding[1] = (coding[1] + ch) & 0xFF
        else:
            coding[2] = (coding[2] + ch) & 0xFF
    for i in range(0, len(buf) - 5):
        buf[i + 3] ^= coding[i % 3]
    return bytes(buf)


def switch_lo_hi(b):
    return ((b >> 4) & 0x0F) | ((b << 4) & 0xF0)


def encrypt_second_level(buf, key):
    """逐字移植 encryptSecondLevelPacket（ENCRYPTION_BLE5）。"""
    buf = bytearray(buf)
    if buf[0] == START_PACKET and buf[1] == START_PACKET:
        buf[0] = ENC_START; buf[1] = ENC_START
    if buf[-2] == END_PACKET and buf[-1] == END_PACKET:
        buf[-2] = ENC_END; buf[-1] = ENC_END
    for i in range(len(buf)):
        b = buf[i]
        b = (b + key[0]) & 0xFF
        b = switch_lo_hi(b)
        b = (b - key[1]) & 0xFF
        b = b ^ key[2]
        buf[i] = b
    return bytes(buf)


def build_packet(devname, ble5key, type_, opcode, params, apply_ble5):
    """构建 over-the-air 字节流（getEncryptedPacket + 可选二级加密）。"""
    n = len(params)
    size = 9 + n
    buf = bytearray(size)
    buf[0] = START_PACKET
    buf[1] = START_PACKET
    buf[2] = (2 + n) & 0xFF
    buf[3] = type_ & 0xFF
    buf[4] = opcode & 0xFF
    for i, p in enumerate(params):
        buf[5 + i] = p & 0xFF
    k = ble5_enc_key(ble5key)
    conn = CONN_HANDSHAKE_DONE if apply_ble5 else CONN_INIT
    crc = crc16(bytes(buf[3:3 + (2 + n)]), SEC_BLE5, conn)
    buf[5 + n]     = (crc >> 8) & 0xFF
    buf[5 + n + 1] = crc & 0xFF
    buf[5 + n + 2] = END_PACKET
    buf[5 + n + 3] = END_PACKET
    buf = bytearray(encode_array_by_sn(bytes(buf), devname))
    if apply_ble5:
        buf = bytearray(encrypt_second_level(buf, k))
    return bytes(buf)


def main():
    DEV = "DAN12345AB"
    KEY = "123456"

    def emit(name, type_, opcode, params, apply_ble5):
        pkt = build_packet(DEV, KEY, type_, opcode, params, apply_ble5)
        print("PKT %s %s" % (name, pkt.hex()))

    # 命名场景（与 C 测试 aaps_dana_test.cpp 完全一致）
    emit("PUMP_CHECK_RESP", 0x02, 0x00,
         [ord('O'), ord('K'), 0x00, 0x09, 0x00, 0x0A,
          ord('1'), ord('2'), ord('3'), ord('4'), ord('5'), ord('6')], 0)
    emit("TIME_INFO_RESP", 0x02, 0x01, [ord('O'), ord('K')], 0)
    emit("CMD_BOLUS_RESP", 0xB2, 0x4A, [0x00], 1)
    emit("NOTIFY_RATE", 0xC3, 0x02, [0x2C, 0x01], 1)
    emit("CMD_TBR_PHONE", 0xA1, 0xC1,
         [(150 & 0xFF), (150 >> 8) & 0xFF, 150], 1)

    # 模糊场景：固定种子 LCG（与 C 测试同序），仅用于 build 字节流 diff
    s = 0x12345678
    def lcg():
        nonlocal s
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        return s
    for i in range(200):
        n = lcg() % 9
        fp = [lcg() & 0xFF for _ in range(n)]
        ty = lcg() & 0xFF
        op = lcg() & 0xFF
        pkt = build_packet(DEV, KEY, ty, op, fp, 1)
        print("PKT FUZZ%03d %s" % (i, pkt.hex()))


if __name__ == "__main__":
    main()
