#!/usr/bin/env python3
# 验证固件分片(19B载荷+1B头) 与 App 重组逻辑 的往返正确性。
# 复刻 firmware ble_frag_notify 的分片 与 App reassembleFragments 的重组。
import sys

FRAG_PAYLOAD = 19  # 1B 头 + 19B 载荷 = 20B = (MTU23 - 3)

def firmware_fragment(json_str: str):
    """模拟固件 ble_frag_notify：返回按特征分片的多个通知字节串(list[bytes])。"""
    total = len(json_str)
    off = 0
    seq = 0
    chunks = []
    while off < total:
        n = min(total - off, FRAG_PAYLOAD)
        hdr = seq & 0x7F
        if off + n >= total:
            hdr |= 0x80  # 末片
        pkt = bytes([hdr]) + json_str[off:off + n].encode("utf-8")
        chunks.append(pkt)
        off += n
        seq += 1
    return chunks

def app_reassemble(chunks, want_uuid="SCREEN"):
    """模拟 App reassembleFragments：按 UUID 累积，末片到达时返回完整字符串。"""
    frag_bufs = {}
    for value in chunks:
        header = value[0] & 0xFF
        is_final = (header & 0x80) != 0
        payload = value[1:]
        key = want_uuid
        sb = frag_bufs.setdefault(key, [])
        if (header & 0x7F) == 0:
            sb.clear()
        sb.extend(payload)
        if not is_final:
            continue
        text = b"".join(sb).decode("utf-8")
        sb.clear()
        return text
    return None  # 未收到末片

def main():
    # 模拟 ui_screen_dump_json 的典型输出（与固件 ScreenSnapshot 字段对应）
    samples = [
        '{"screen":0,"sel":1,"set_edit":0,"title":"主屏","big":"0.00","unit":"U","clk":[12,30],"line1":"基础率 0.50","line2":"剩余 300U","warn":0,"bolus":0,"prime":0,"tbr":100,"hist":0,"histf":0}',
        '{"screen":3,"sel":0,"set_edit":1,"title":"大剂量","big":"2.50","unit":"U","clk":[8,5],"line1":"确认?","line2":"","warn":0,"bolus":1,"prime":0,"tbr":100,"hist":0,"histf":0}',
        '{"bat":100,"st":0,"alm":0,"glu":0,"tr":0,"loop":0,"tbr":0}',  # status
        'short',
    ]
    ok = True
    for i, s in enumerate(samples):
        chunks = firmware_fragment(s)
        # 验证每片 <= 20 字节
        for c in chunks:
            if len(c) > 20:
                print(f"[FAIL] 样本{i}: 片长 {len(c)} > 20"); ok = False
        got = app_reassemble(chunks)
        if got != s:
            print(f"[FAIL] 样本{i}: 重组不符\n  原: {s}\n  得: {got}")
            ok = False
        else:
            print(f"[OK] 样本{i}: {len(s)}B -> {len(chunks)}片, 重组一致")
    # 丢片测试：丢弃中间一片，应返回 None（不产出错误 JSON）
    s = samples[0]
    chunks = firmware_fragment(s)
    dropped = chunks[:1] + chunks[2:]  # 丢第2片
    got = app_reassemble(dropped)
    if got is not None:
        print(f"[FAIL] 丢片测试: 应返回 None, 实际 {got!r}")
        ok = False
    else:
        print("[OK] 丢片测试: 返回 None (未产出残缺 JSON)")
    print("RESULT:", "ALL PASS" if ok else "HAS FAILURES")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
