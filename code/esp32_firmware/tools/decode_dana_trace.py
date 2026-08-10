#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
decode_dana_trace.py — 把 dana_trace 里的 RX/TX 原始字节还原成 Dana 明文包

背景：固件 dana_trace_log 记录的 op 字段取的是 data[4]，而命令阶段的缓冲
      **已经过 BLE5 二级加密**，所以 trace 里显示的 op（0x33/0x55/0x28…）
      是密文字节，不是真实 opcode。必须离线复刻固件的解密链才能看到真值。

解密链（与 src/aaps_dana.cpp 完全一致）：
  1. dana_decrypt_second_level(buf, key3)   # 命令阶段才有
  2. dana_encode_array_by_sn(buf, devname)  # 设备名 XOR，自逆
  3. 校验 A5A5/AAAA 起、5A5A/EEEE 止、LEN、CRC
  4. 抽取 [3]=TYPE [4]=OPCODE [5..]=PARAMS

⚠️ 实验项目，禁止用于人体。
"""
import struct
import sys

BLE5_MATRIX = bytes([
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
])

DEVNAME = "DAN12345AB"
BLE5KEY = "123456"

# ---- AAPS opcode 名称表（BleEncryption.java 3.2.0.4）----
OP_ENC = {0x00: "PUMP_CHECK", 0x01: "TIME_INFORMATION", 0xD0: "CHECK_PASSKEY",
          0xD1: "PASSKEY_REQUEST", 0xD2: "PASSKEY_RETURN",
          0xF3: "ENC_GET_PUMP_CHECK", 0xF4: "ENC_GET_EASYMENU_CHECK"}
OP_CMD = {
    0x02: "REVIEW__INITIAL_SCREEN_INFORMATION", 0x03: "REVIEW__DELIVERY_STATUS",
    0x04: "REVIEW__GET_PASSWORD",
    0x10: "REVIEW__BOLUS_AVG", 0x11: "REVIEW__BOLUS", 0x12: "REVIEW__DAILY",
    0x13: "REVIEW__PRIME", 0x14: "REVIEW__REFILL", 0x15: "REVIEW__BLOOD_GLUCOSE",
    0x16: "REVIEW__CARBOHYDRATE", 0x17: "REVIEW__TEMPORARY", 0x18: "REVIEW__SUSPEND",
    0x19: "REVIEW__ALARM", 0x1A: "REVIEW__BASAL", 0x1F: "REVIEW__ALL_HISTORY",
    0x20: "REVIEW__GET_SHIPPING_INFORMATION", 0x21: "REVIEW__GET_PUMP_CHECK",
    0x22: "REVIEW__GET_USER_TIME_CHANGE_FLAG", 0x23: "REVIEW__SET_USER_TIME_CHANGE_FLAG_CLEAR",
    0x24: "REVIEW__GET_MORE_INFORMATION", 0x25: "REVIEW__SET_HISTORY_UPLOAD_MODE",
    0x26: "REVIEW__GET_TODAY_DELIVERY_TOTAL",
    0x40: "BOLUS__GET_STEP_BOLUS_INFORMATION", 0x41: "BOLUS__GET_EXTENDED_BOLUS_STATE",
    0x42: "BOLUS__GET_EXTENDED_BOLUS", 0x43: "BOLUS__GET_DUAL_BOLUS",
    0x44: "BOLUS__SET_STEP_BOLUS_STOP", 0x45: "BOLUS__GET_CARB_CALC_INFORMATION",
    0x46: "BOLUS__GET_EXTENDED_MENU_OPTION_STATE", 0x47: "BOLUS__SET_EXTENDED_BOLUS",
    0x48: "BOLUS__SET_DUAL_BOLUS", 0x49: "BOLUS__SET_EXTENDED_BOLUS_CANCEL",
    0x4A: "BOLUS__SET_STEP_BOLUS_START", 0x4B: "BOLUS__GET_CALCULATION_INFORMATION",
    0x4C: "BOLUS__GET_BOLUS_RATE", 0x4D: "BOLUS__SET_BOLUS_RATE",
    0x4E: "BOLUS__GET_CIR_CF_ARRAY", 0x4F: "BOLUS__SET_CIR_CF_ARRAY",
    0x50: "BOLUS__GET_BOLUS_OPTION", 0x51: "BOLUS__SET_BOLUS_OPTION",
    0x52: "BOLUS__GET_24_CIR_CF_ARRAY", 0x53: "BOLUS__SET_24_CIR_CF_ARRAY",
    0x60: "BASAL__SET_TEMPORARY_BASAL", 0x61: "BASAL__TEMPORARY_BASAL_STATE",
    0x62: "BASAL__CANCEL_TEMPORARY_BASAL", 0x63: "BASAL__GET_PROFILE_NUMBER",
    0x64: "BASAL__SET_PROFILE_NUMBER", 0x65: "BASAL__GET_PROFILE_BASAL_RATE",
    0x66: "BASAL__SET_PROFILE_BASAL_RATE", 0x67: "BASAL__GET_BASAL_RATE",
    0x68: "BASAL__SET_BASAL_RATE", 0x69: "BASAL__SET_SUSPEND_ON",
    0x6A: "BASAL__SET_SUSPEND_OFF",
    0x70: "OPTION__GET_PUMP_TIME", 0x71: "OPTION__SET_PUMP_TIME",
    0x72: "OPTION__GET_USER_OPTION", 0x73: "OPTION__SET_USER_OPTION",
    0x74: "OPTION__GET_EASY_MENU_OPTION", 0x75: "OPTION__SET_EASY_MENU_OPTION",
    0x76: "OPTION__GET_EASY_MENU_STATUS", 0x77: "OPTION__SET_EASY_MENU_STATUS",
    0x78: "OPTION__GET_PUMP_UTC_AND_TIME_ZONE", 0x79: "OPTION__SET_PUMP_UTC_AND_TIME_ZONE",
    0x7A: "OPTION__GET_PUMP_TIME_ZONE", 0x7B: "OPTION__SET_PUMP_TIME_ZONE",
    0x80: "REVIEW__GET_PUMP_DEC_RATIO", 0x81: "GENERAL__GET_SHIPPING_VERSION",
    0xC1: "BASAL__APS_SET_TEMPORARY_BASAL", 0xC2: "APS_HISTORY_EVENTS",
    0xC3: "APS_SET_EVENT_HISTORY",
    0xE0: "ETC__SET_HISTORY_SAVE", 0xFF: "ETC__KEEP_CONNECTION",
}
TYPE_NAME = {0x01: "ENC_REQ", 0x02: "ENC_RESP", 0xA1: "COMMAND",
             0xB2: "RESPONSE", 0xC3: "NOTIFY"}


def ble5_key3(key=BLE5KEY):
    return [BLE5_MATRIX[(ord(key[0]) - 48) * 10 + (ord(key[1]) - 48)],
            BLE5_MATRIX[(ord(key[2]) - 48) * 10 + (ord(key[3]) - 48)],
            BLE5_MATRIX[(ord(key[4]) - 48) * 10 + (ord(key[5]) - 48)]]


def switch_lo_hi(b):
    return ((b >> 4) & 0x0F) | ((b << 4) & 0xF0)


def decrypt2(buf, k):
    out = bytearray(len(buf))
    for i, b in enumerate(buf):
        b ^= k[2]
        b = (b + k[1]) & 0xFF
        b = switch_lo_hi(b)
        b = (b - k[0]) & 0xFF
        out[i] = b
    return out


def encrypt2(buf, k):
    out = bytearray(buf)
    if len(out) >= 2 and out[0] == 0xA5 and out[1] == 0xA5:
        out[0] = out[1] = 0xAA
    if len(out) >= 2 and out[-2] == 0x5A and out[-1] == 0x5A:
        out[-2] = out[-1] = 0xEE
    for i, b in enumerate(out):
        b = (b + k[0]) & 0xFF
        b = switch_lo_hi(b)
        b = (b - k[1]) & 0xFF
        b ^= k[2]
        out[i] = b
    return out


def encode_by_sn(buf, devname=DEVNAME):
    out = bytearray(buf)
    coding = [0, 0, 0]
    for i in range(10):
        ch = ord(devname[i])
        if i < 3:
            coding[0] = (coding[0] + ch) & 0xFF
        elif i < 8:
            coding[1] = (coding[1] + ch) & 0xFF
        else:
            coding[2] = (coding[2] + ch) & 0xFF
    n = len(out)
    i = 0
    while i + 5 < n:
        out[i + 3] ^= coding[i % 3]
        i += 1
    return out


def crc16(data, ble5_cmd_phase):
    crc = 0
    for b in data:
        r = ((crc << 8) | (crc >> 8)) & 0xFFFF
        r ^= b
        r ^= (r & 0xFF) >> 4
        r ^= (r << 12) & 0xFFFF
        low = r & 0xFF
        if ble5_cmd_phase:
            r ^= ((low << 4) | ((low >> 3) << 2)) & 0xFFFF
        else:
            r ^= ((low << 3) | ((low >> 2) << 5)) & 0xFFFF
        crc = r & 0xFFFF
    return crc


def unpack(plain, cmd_phase):
    """plain: 已二级解密的字节；返回 (ok, type, opcode, params, err)"""
    if len(plain) < 7:
        return (False, None, None, None, "too short")
    b = encode_by_sn(plain)
    ok_start = (b[0] == 0xA5 and b[1] == 0xA5) or (b[0] == 0xAA and b[1] == 0xAA)
    ok_end = (b[-2] == 0x5A and b[-1] == 0x5A) or (b[-2] == 0xEE and b[-1] == 0xEE)
    if not ok_start or not ok_end:
        return (False, None, None, None,
                "bad marker %02X%02X..%02X%02X" % (b[0], b[1], b[-2], b[-1]))
    ln = b[2]
    if ln + 7 != len(b):
        return (False, None, None, None, "len mismatch len=%d size=%d" % (ln, len(b)))
    c = crc16(b[3:3 + ln], cmd_phase)
    if b[-4] != (c >> 8) or b[-3] != (c & 0xFF):
        return (False, b[3], b[4], bytes(b[5:5 + ln - 2]),
                "CRC bad got=%02X%02X want=%04X" % (b[-4], b[-3], c))
    return (True, b[3], b[4], bytes(b[5:5 + ln - 2]), None)


def opname(t, op):
    if t in (0x01, 0x02):
        return OP_ENC.get(op, "ENC_?")
    return OP_CMD.get(op, "CMD_?")


REC = 32  # [ts:4][dir:1][op:1][len:1][st:1][data:24]
DIRS = {0: "RX ", 1: "TXQ", 2: "TXS", 3: "STA", 4: "ERR"}


def main(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"DANA"[::-1] and struct.unpack("<I", raw[:4])[0] != 0x44414E41:
        print("warn: magic mismatch", raw[:4].hex())
    k = ble5_key3()
    print("BLE5 key3 = %s  (devname=%s key=%s)" % ([hex(x) for x in k], DEVNAME, BLE5KEY))
    print("-" * 100)

    off = 4
    idx = 0
    cmd_phase = False   # 跟随 TIME_INFO 切换
    while off + REC <= len(raw):
        rec = raw[off:off + REC]
        off += REC
        ts, d, op, ln, st = struct.unpack("<IBBBB", rec[:8])
        if ts == 0xFFFFFFFF:
            continue
        data = rec[8:8 + min(ln, 24)]
        tag = DIRS.get(d, "?%d" % d)

        if d == 3:      # 状态
            names = {0xFD: "GATT_CONNECT", 0xFC: "GATT_DISCONNECT", 0xFE: "SUBSCRIBE",
                     0xFB: "BONDED", 0x00: "rx PUMP_CHECK", 0x01: "rx TIME_INFO"}
            print("[%3d] %8.3fs STA %-16s st=%d" %
                  (idx, ts / 1000.0, names.get(op, "op%02X" % op), st))
            if op == 0x01:
                cmd_phase = True
            if op == 0xFC:
                cmd_phase = False
            idx += 1
            continue
        if d == 4:
            print("[%3d] %8.3fs ERR r=%d raw=%s" % (idx, ts / 1000.0, st - 256 if st > 127 else st,
                                                    data.hex(" ")))
            idx += 1
            continue

        # RX / TX 数据帧
        plain = decrypt2(data, k) if cmd_phase else bytearray(data)
        ok, t, o, p, err = unpack(plain, cmd_phase)
        desc = ""
        if ok:
            desc = "%-8s %-38s params=%s" % (TYPE_NAME.get(t, "T%02X" % t),
                                             opname(t, o), p.hex(" ") or "-")
        else:
            # 未解出：可能是分片，或阶段判断错。两种解密都试一下
            alt = decrypt2(data, k) if not cmd_phase else bytearray(data)
            ok2, t2, o2, p2, err2 = unpack(alt, not cmd_phase)
            if ok2:
                desc = "(阶段反转命中) %-8s %-30s params=%s" % (
                    TYPE_NAME.get(t2, "T%02X" % t2), opname(t2, o2), p2.hex(" ") or "-")
            else:
                desc = "解析失败: %s | plain=%s" % (err, bytes(plain).hex(" "))
        print("[%3d] %8.3fs %s len=%2d cipher=%s" % (idx, ts / 1000.0, tag, ln, data.hex(" ")))
        print("      -> %s" % desc)
        idx += 1


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/dana_trace_v7.bin")
