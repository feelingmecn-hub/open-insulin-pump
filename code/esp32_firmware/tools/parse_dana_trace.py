#!/usr/bin/env python3
"""解析 dana_trace.bin：从 ESP32 flash dana_trace 分区 (0x3E0000, 128KB) 导出的 BLE 握手日志。
导出: esptool read_flash 0x3E0000 0x20000 dana_trace.bin
用法: python3 parse_dana_trace.py [dana_trace.bin]
记录格式(定长 32B): [ts:4][dir:1][op:1][len:1][st:1][data:24]
  dir: 0=RX(手机→泵原始) 1=TX_ENQ(泵入队响应) 2=TX_SENT(泵实际notify) 3=STATUS 4=ERR
"""
import sys, struct

REC = 32
DIR = {0: "RX ", 1: "TXQ", 2: "TXS", 3: "STA", 4: "ERR"}
STA = {}


def opname(dir_, op):
    if dir_ == 3:
        if op == 0x00: return "PUMP_CHECK"
        if op == 0x01: return "TIME_INFO"
        if op == 0xFD: return "CONN"
        if op == 0xFC: return "DISC"
        if op == 0xFE: return "SUBSCRIBE"
        if op == 0xFB: return "AUTH"
        return f"STA:0x{op:02X}"
    if dir_ == 4:
        return f"ERR(r={op})"
    return f"op=0x{op:02X}"


def st_note(dir_, op, st):
    """对关键状态给出人话注释，便于快速定位卡点。"""
    if dir_ == 3:
        if op == 0xFD:
            return " [CONN: st=1已绑定→AAPS会继续; st=0未绑定→AAPS connect()直接return false]"
        if op == 0xFB:
            return " [AUTH: 配对/绑定完成]"
        if op == 0xFC:
            return f" [DISC: reason={st}]"
        if op == 0xFE:
            return f" [SUBSCRIBE: st={st}->{'已订阅' if st else '已取消'}]"
    return ""


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "dana_trace.bin"
    with open(path, "rb") as f:
        buf = f.read()
    print(f"file={path} bytes={len(buf)} (expect 131072)")
    cnt = 0
    # 前 4 字节是 magic，记录从 off=4 起按 32 对齐
    for off in range(4, len(buf) - REC + 1, REC):
        rec = buf[off:off + REC]
        if rec == b"\xff" * REC:
            continue
        ts, d, op, ln, st = struct.unpack_from("<IBBBB", rec, 0)
        if ts == 0xFFFFFFFF:
            continue
        data = rec[8:8 + ln]
        hexs = " ".join(f"{b:02X}" for b in data)
        print(f"[{cnt:4d}] t={ts:7d}ms {DIR.get(d, '?'):3s} {opname(d, op):12s} "
              f"st={st} len={ln:2d}  {hexs}{st_note(d, op, st)}")
        cnt += 1
    print(f"--- parsed {cnt} records ---")


if __name__ == "__main__":
    main()
