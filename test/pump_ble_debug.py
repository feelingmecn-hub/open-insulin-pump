#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pump_ble_debug.py — 闭环胰岛素泵「无手机 / 无实体按键」BLE 调试客户端
=====================================================================

适用场景
--------
- 忘带安卓手机，或开发板没接 4 键，无法用 App / 物理按键操作泵时，
  用这台 Mac 的蓝牙直接当"伪手机"驱动泵，重点用来调电机。

前提
----
- 泵已烧录含伴生 BLE 服务的固件（本仓库固件默认挂载 NUS 变体 6E4000xx-B5A3-F393-E0A9-E50E24DCCA9E 服务）。
- 编译时带 -DUSE_AAPS_DANA 时，泵广播名为 "DAN12345AB"（脚本默认扫这个名）。
- Python 环境已装 bleak（仓库隔离环境：~/.workbuddy/binaries/python/envs/default）。

用法
----
  python3 test/pump_ble_debug.py scan
  python3 test/pump_ble_debug.py key up          # 发送一次"上"按键（等同物理 UP）
  python3 test/pump_ble_debug.py key down
  python3 test/pump_ble_debug.py key set
  python3 test/pump_ble_debug.py key esc
  python3 test/pump_ble_debug.py key release     # 松手（停自动重复）
  python3 test/pump_ble_debug.py prime 5.0       # 排气 5.0U（≤200）
  python3 test/pump_ble_debug.py rewind          # 退回装药（活塞回原点）
  python3 test/pump_ble_debug.py clear           # 清除报警
  python3 test/pump_ble_debug.py move fwd 4000 1500   # 前进 4000 微步 @1500Hz（定量点动）
  python3 test/pump_ble_debug.py move rev 4000 1500   # 后退
  python3 test/pump_ble_debug.py stop            # 手动停止（对"正在进行的定量移动"无效——
                                                  #   定量移动会自己跑完；仅对连续点动有意义，见下）
  python3 test/pump_ble_debug.py pos             # 读取当前电机微步位置
  python3 test/pump_ble_debug.py monitor 8       # 订阅 SCREEN 推送，解码实时状态 8 秒

✅ "连续点动 (steps=0)" 已修复：固件把连续点动改为由 motor 任务主循环异步驱动、
   STOP(0x16) 直接置停止标志（motor_pulse 内每 5ms 轮询），故停止即时响应、电机任务不死锁。
   用法：move fwd 0 1500 启动连续前进点动，stop 停止；也可运行中再发 move rev 0 改方向。

⚠️ 红线：本泵为教学原型，严禁接入任何人体！调试请用「水填充空注射器」，最大剂量请勿超量程。
"""

import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

# ---- 固件权威 UUID ----
# ⚠️ config.h 的 BLE_CHAR_*_UUID 是 NimBLE 字节序的 16 字节数组，真实 GATT 基址是
# NUS 变体 6E4000xx-B5A3-F393-E0A9-E50E24DCCA9E（config.h:299 注释原文）。
# 偏移：01=服务 09=control 0A=settings 0B=key 0C=screen
SERVICE  = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_CONTROL  = "6e400009-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_SETTINGS = "6e40000a-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_KEY      = "6e40000b-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_SCREEN   = "6e40000c-b5a3-f393-e0a9-e50e24dcca9e"

# 编译带 -DUSE_AAPS_DANA 时广播名
DEVICE_NAME = "DAN12345AB"

# ---- CRC-8/CCITT：poly 0x07, init 0x00（与固件 crc8_ccitt 严格一致）----
def crc8_ccitt(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

def frame(payload: bytes) -> bytes:
    """追加 CRC 尾字节，构成一条完整指令帧。"""
    return payload + bytes([crc8_ccitt(payload)])


# ---- 指令帧构造 ----
def f_prime(ml: float) -> bytes:
    return frame(bytes([0x10]) + struct.pack('<f', ml))

def f_rewind() -> bytes:
    return frame(bytes([0x12]))

def f_clear() -> bytes:
    return frame(bytes([0x11]))

def f_manual_move(dir_fwd: bool, steps: int, speed: int) -> bytes:
    # op=0x15, dir(0=前进/非0=后退), steps u32 LE, speed u16 LE
    dirb = 0 if dir_fwd else 1
    return frame(bytes([0x15, dirb])
                 + struct.pack('<I', steps & 0xFFFFFFFF)
                 + struct.pack('<H', speed & 0xFFFF))

def f_stop() -> bytes:
    return frame(bytes([0x16]))

def f_key(k: int) -> bytes:
    # 0=release 1=UP 2=DOWN 3=SET 4=ESC 5=LONG_SET 6=LONG_ESC
    return frame(bytes([k & 0xFF]))

def f_get_pos() -> bytes:
    return frame(bytes([0x2A]))   # SETTINGS op: GET_MOTOR_POSITION


KEY_NAME = {"up": 1, "down": 2, "set": 3, "esc": 4,
            "long_set": 5, "long_esc": 6, "release": 0}


async def find_device():
    # 直接 discover 后按广播名匹配 (最可靠; 新版 bleak 已删除 BLEDevice.metadata 属性)
    print(f"扫描设备名 {DEVICE_NAME} ...")
    devs = await BleakScanner.discover(timeout=10)
    for d in devs:
        if d.name and DEVICE_NAME.lower() in d.name.lower():
            return d
        # 兼容旧版 bleak: uuids 兜底 (新版本无 metadata, 此处安全访问)
        meta = getattr(d, "metadata", None)
        uu = (meta or {}).get("uuids", []) if meta else []
        if not uu:
            adv = getattr(d, "advertisement_data", None)
            if adv is not None:
                uu = list(getattr(adv, "service_uuids", []) or [])
        if SERVICE.lower() in [x.lower() for x in uu]:
            return d
    return None


async def write_char(client, uuid: str, data: bytes, label: str):
    await client.write_gatt_char(uuid, data, response=True)
    print(f"[→] {label}: {data.hex(' ')}")


async def cmd_scan():
    devs = await BleakScanner.discover(timeout=8)
    print(f"发现 {len(devs)} 个设备：")
    for d in devs:
        name = d.name or "(无名)"
        meta = getattr(d, "metadata", None)
        uuids = (meta or {}).get("uuids", []) if meta else []
        hit = SERVICE.lower() in [u.lower() for u in uuids]
        print(f"  {d.address}  {name}  {'<== 泵' if (name == DEVICE_NAME or hit) else ''}")


async def cmd_key(name: str):
    k = KEY_NAME.get(name)
    if k is None:
        print(f"未知按键 {name}，可选: {list(KEY_NAME)}")
        return
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_KEY, f_key(k), f"KEY {name}")


async def cmd_prime(ml: float):
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_CONTROL, f_prime(ml), f"PRIME {ml}U")


async def cmd_rewind():
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_CONTROL, f_rewind(), "REWIND")


async def cmd_clear():
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_CONTROL, f_clear(), "CLEAR ALARM")


async def cmd_move(dir_str: str, steps: int, speed: int):
    dir_fwd = dir_str.lower() in ("fwd", "forward", "正", "前进")
    if steps < 0:
        print("⚠️ steps 不能为负。正整数=定量点动；0=连续点动（直到 stop/限位）。")
        return
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_CONTROL, f_manual_move(dir_fwd, steps, speed),
                         f"MANUAL MOVE {'FWD' if dir_fwd else 'REV'} "
                         f"{'连续点动' if steps == 0 else str(steps)+'步'} @{speed}Hz")
        if steps == 0:
            print("▶ 连续点动已启动；发 `stop` 命令（或撞限位）即停。")
        else:
            # 读一下位置，确认动了
            await asyncio.sleep(0.2)
            await read_pos(c)


async def cmd_stop():
    async with BleakClient(await find_device_or_exit()) as c:
        await write_char(c, CHAR_CONTROL, f_stop(), "MANUAL STOP")


async def read_pos(client) -> int:
    try:
        await client.write_gatt_char(CHAR_SETTINGS, f_get_pos(), response=True)
        raw = await client.read_gatt_char(CHAR_SETTINGS)
        pos = struct.unpack('<I', raw[:4])[0] if len(raw) >= 4 else 0
        print(f"[←] 电机位置 = {pos} 微步 (≈ {pos/2178.0:.3f}U)")
        return pos
    except Exception as e:
        print(f"[!] 读位置失败: {e}")
        return 0


async def cmd_pos():
    async with BleakClient(await find_device_or_exit()) as c:
        await read_pos(c)


def decode_state(data: bytes):
    if len(data) < 20 or data[0] != 0xA1:
        return f"(非状态帧, {len(data)}B) {data.hex(' ')}"
    f1 = data[1]
    state = f1 & 0x0F
    loop = (f1 >> 4) & 0x03
    keypad = (f1 >> 6) & 1
    alarm = (f1 >> 7) & 1
    batt = data[2]
    acode = data[3]
    trend = data[4] - 128
    tbr = data[5]
    f2 = data[6]
    ext_bolus = f2 & 1
    step_loss = (f2 >> 1) & 1
    prog = data[7]
    res = struct.unpack('<H', data[8:10])[0] / 10.0
    iob = struct.unpack('<H', data[10:12])[0] / 100.0
    basal = struct.unpack('<H', data[12:14])[0] / 100.0
    glu = struct.unpack('<H', data[14:16])[0]
    today = struct.unpack('<H', data[16:18])[0] / 100.0
    clk = struct.unpack('<H', data[18:20])[0]
    hh, mm = divmod(clk, 60)
    states = {0: "空闲", 1: "大剂量", 2: "基础率", 3: "排气", 4: "报警",
              5: "菜单", 6: "关机", 7: "初始化"}
    loops = {0: "关闭", 1: "开环", 2: "闭环", 3: "暂停"}
    return (f"状态={states.get(state,state)} 环={loops.get(loop,loop)} "
            f"电池={batt}% 报警={'是' if alarm else '否'}(code={acode}) "
            f"储药={res}U IOB={iob}U 基础率={basal}U/h 血糖={glu} 今日={today}U "
            f"大剂量进度={prog}% 趋势={trend} TBR={tbr}% 丢步={'是' if step_loss else '否'} "
            f"按键锁={'是' if keypad else '否'} 时钟={hh:02d}:{mm:02d}")


async def cmd_monitor(seconds: float):
    dev = await find_device_or_exit()
    got = []
    def on_notify(_, data):
        b = bytes(data)
        got.append(b)
        print(f"[状态] {decode_state(b)}")
    async with BleakClient(dev) as c:
        try:
            await c.start_notify(CHAR_SCREEN, on_notify)
        except Exception as e:
            print(f"订阅 SCREEN 失败: {e}")
            return
        print(f"已订阅 SCREEN，监听 {seconds}s（可同时另开终端发 move/key）...")
        await asyncio.sleep(seconds)
        await c.stop_notify(CHAR_SCREEN)
    print(f"\n收到 {len(got)} 帧状态推送。")


async def find_device_or_exit():
    dev = await find_device()
    if not dev:
        print("未找到泵，请确认：①开发板已上电 ②蓝牙已开 ③在广播范围内。退出。")
        sys.exit(1)
    print(f"连接 {dev.name or DEVICE_NAME} @ {dev.address} ...")
    return dev


def main():
    import argparse
    ap = argparse.ArgumentParser(description="泵 BLE 调试客户端（无手机/无按键）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="扫描并列出附近 BLE 设备")
    p = sub.add_parser("key", help="发远程按键")
    p.add_argument("name", choices=list(KEY_NAME.keys()))
    p = sub.add_parser("prime", help="排气(装药) 体积U")
    p.add_argument("ml", type=float)
    sub.add_parser("rewind", help="退回装药")
    sub.add_parser("clear", help="清除报警")
    p = sub.add_parser("move", help="点动电机 (steps>0 定量; steps=0 连续点动)")
    p.add_argument("dir", help="fwd/rev")
    p.add_argument("steps", type=int, help="微步数(正整数=定量; 0=连续点动)")
    p.add_argument("speed", type=int, nargs="?", default=1500, help="Hz(默认1500)")
    sub.add_parser("stop", help="手动停止")
    sub.add_parser("pos", help="读电机位置")
    p = sub.add_parser("monitor", help="监听实时状态")
    p.add_argument("secs", type=float, nargs="?", default=8)

    args = ap.parse_args()
    fn = {
        "scan": cmd_scan,
        "key": lambda: cmd_key(args.name),
        "prime": lambda: cmd_prime(args.ml),
        "rewind": cmd_rewind,
        "clear": cmd_clear,
        "move": lambda: cmd_move(args.dir, args.steps, args.speed),
        "stop": cmd_stop,
        "pos": cmd_pos,
        "monitor": lambda: cmd_monitor(args.secs),
    }[args.cmd]
    asyncio.run(fn())


if __name__ == "__main__":
    main()
