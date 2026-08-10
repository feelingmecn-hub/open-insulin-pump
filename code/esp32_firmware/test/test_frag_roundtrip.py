#!/usr/bin/env python3
# 镜像固件 ble_frag_notify(C++) 与 App reassembleFragments(Kotlin) 的分片/重组协议，
# 做逻辑级往返 sanity check。屏显 JSON 为 ASCII/UTF-8，故按字节拆分无歧义。
import sys

FRAG_PAYLOAD = 19

def firmware_frag(s: str):
    """返回分片列表，每片为 bytes: [header][payload...]"""
    data = s.encode("utf-8")
    total = len(data)
    off = 0
    seq = 0
    out = []
    while off < total:
        n = total - off
        if n > FRAG_PAYLOAD:
            n = FRAG_PAYLOAD
        hdr = seq & 0x7F
        if off + n >= total:
            hdr |= 0x80
        pkt = bytes([hdr]) + data[off:off+n]
        out.append(pkt)
        off += n
        seq += 1
    return out

class AppReassembler:
    def __init__(self):
        self.bufs = {}

    def feed(self, uuid, pkt: bytes):
        """返回完成后的字符串，否则 None"""
        if not pkt:
            return None
        header = pkt[0] & 0xFF
        is_final = (header & 0x80) != 0
        payload = pkt[1:]
        key = uuid
        sb = self.bufs.setdefault(key, [])
        # 新帧起点（seq==0）丢弃残留
        if (header & 0x7F) == 0:
            sb.clear()
        sb.append(payload)
        if not is_final:
            return None
        text = b"".join(sb).decode("utf-8")
        sb.clear()
        return text

def check(name, cond):
    print(("PASS" if cond else "FAIL") + " - " + name)
    if not cond:
        sys.exit(1)

# 1) 单帧
s1 = '{"a":1}'
p = firmware_frag(s1)
check("single-frag count==1", len(p) == 1)
r = AppReassembler()
check("single-frag roundtrip", r.feed("X", p[0]) == s1)

# 2) 多帧（>1 片）
s2 = '{"screen":"home","battery":87,"reservoir":142,"iob":1.25,"basal":0.8,"status":"running"}'
p = firmware_frag(s2)
check("multi-frag count>1", len(p) >= 2)
r = AppReassembler()
out = None
for pkt in p:
    out = r.feed("X", pkt)
check("multi-frag roundtrip", out == s2)

# 3) 大消息（约 15 帧）
s3 = ("{" + ",".join(f'"k{i}":"{"x"*20}' for i in range(60)) + "}")  # ~ 1.5KB
p = firmware_frag(s3)
check("large-frag count>=10", len(p) >= 10)
r = AppReassembler()
out = None
for pkt in p:
    out = r.feed("X", pkt)
check("large-frag roundtrip", out == s3)

# 4) 丢片自愈（真实场景）：BLE 通知有序，但偶发丢片时，下一帧 seq==0 会丢弃残留缓冲，
#    自动恢复。模拟：消息A 缺末片（只发前几片），随后消息B 完整下发 → 应只产出 B。
pA = firmware_frag(s2)
pB = firmware_frag(s3)
r = AppReassembler()
out = None
for pkt in pA[:-1]:           # A 的末片丢失
    out = r.feed("X", pkt)
check("drop-frame yields None (A incomplete)", out is None)
for pkt in pB:                # B 完整，seq==0 重置缓冲
    out = r.feed("X", pkt)
check("recover-after-drop (B ok)", out == s3)

# 5) 同一 UUID 连续两帧（真实场景）：seq==0 重置缓冲，前后帧互不污染。
pA = firmware_frag(s2)
pB = firmware_frag(s3)
r = AppReassembler()
out = None
for pkt in pA:
    out = r.feed("X", pkt)
check("consecutive-A ok", out == s2)
for pkt in pB:
    out = r.feed("X", pkt)
check("consecutive-B ok (buffer reset)", out == s3)

# 6) 缺末片（最后一片丢失）→ 不应产出
p = firmware_frag(s2)
r = AppReassembler()
out = None
for pkt in p[:-1]:
    out = r.feed("X", pkt)
check("missing-final yields None", out is None)

print("ALL FRAG ROUNDTRIP TESTS PASSED")
