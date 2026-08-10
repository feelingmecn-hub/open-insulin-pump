#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pump_web_debug/server.py — 闭环胰岛素泵「可视化 Web 调试后端」
============================================================

无手机 / 无实体按键时，用这台 Mac 的浏览器当调试面板驱动泵：
  - 后端：Python 标准库 http.server + 后台 asyncio 跑 bleak（零额外依赖）
          复用 pump_ble_debug.py 的 UUID / CRC-8 / 指令构造逻辑。
  - 前端：浏览器打开 http://localhost:8080 ，可视化点动电机、看活塞位置动画与实时状态。

运行（仓库隔离 Python，bleak 已装）：
  ~/.workbuddy/binaries/python/envs/default/bin/python3 test/pump_web_debug/server.py

然后浏览器打开 http://localhost:8080

⚠️ 红线：教学原型，严禁接入任何人体！调试请用「水填充空注射器」，剂量勿超量程。
✅ 连续点动(steps=0)已修复：固件异步驱动 + STOP(0x16) 直接置停止标志，停止即时响应，不死锁。
   前端「连续点动」模式可用，再点「■ 停止」即停（或撞限位自动停）。
"""

import asyncio
import json
import struct
import threading
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

from bleak import BleakClient, BleakScanner

# ---- 固件权威 UUID ----
# ⚠️ 关键坑：config.h 的 BLE_CHAR_*_UUID 是「NimBLE 字节序」的 16 字节数组，
# 真实暴露到 GATT 的 UUID 基址是 NUS 变体 6E4000xx-B5A3-F393-E0A9-E50E24DCCA9E
# （config.h:299 注释原文即如此）。绝不能直接把 {0x9E,0xCA,0xDC,0x24,...} 当大端
# 拼成 9ECADC24-0EE5-... 去用，否则 SCREEN 永远找不到。
# 偏移映射（与诊断 GATT 枚举一致）：01=服务 02=bolus 03=basal 04=tbr 05=status
#   06=iob 07=reservoir 08=cgm 09=control 0A=settings 0B=key 0C=screen
SERVICE  = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_CONTROL  = "6e400009-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_SETTINGS = "6e40000a-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_KEY      = "6e40000b-b5a3-f393-e0a9-e50e24dcca9e"
CHAR_SCREEN   = "6e40000c-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME = "DAN12345AB"
STEPS_PER_UNIT = 2178          # 与固件 dosing.h 一致
RESERVOIR_U = 300.0            # 卡式瓶标称容量（用于进度条比例）

KEY_CODE = {"release": 0, "up": 1, "down": 2, "set": 3, "esc": 4,
            "long_set": 5, "long_esc": 6}


def dlog(*a):
    """打印带时间戳的诊断日志到 server 终端（用户明确要求更详细）。"""
    ts = time.strftime("%H:%M:%S")
    print(f"[DIAG {ts}]", *a, flush=True)


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
    return payload + bytes([crc8_ccitt(payload)])


def f_prime(ml: float) -> bytes:
    return frame(bytes([0x10]) + struct.pack('<f', ml))


def f_rewind() -> bytes:
    return frame(bytes([0x12]))


def f_clear() -> bytes:
    return frame(bytes([0x11]))


def f_manual_move(dir_fwd: bool, steps: int, speed: int) -> bytes:
    dirb = 0 if dir_fwd else 1
    return frame(bytes([0x15, dirb])
                 + struct.pack('<I', steps & 0xFFFFFFFF)
                 + struct.pack('<H', speed & 0xFFFF))


def f_stop() -> bytes:
    return frame(bytes([0x16]))


def f_key(k: int) -> bytes:
    return frame(bytes([k & 0xFF]))


def f_get_pos() -> bytes:
    return frame(bytes([0x2A]))


def decode_state(data: bytes) -> dict:
    """解析 SCREEN 推送帧（magic 0xA1, 20 字节）。返回 dict 便于 JSON。"""
    if len(data) < 20 or data[0] != 0xA1:
        return {"raw": data.hex(' '), "valid": False}
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
    return {
        "valid": True,
        "state": states.get(state, state),
        "loop": loops.get(loop, loop),
        "battery": batt,
        "alarm": bool(alarm),
        "alarm_code": acode,
        "trend": trend,
        "tbr": tbr,
        "ext_bolus": bool(ext_bolus),
        "step_loss": bool(step_loss),
        "bolus_progress": prog,
        "reservoir_u": round(res, 1),
        "iob_u": round(iob, 2),
        "basal_uh": round(basal, 2),
        "glucose": glu,
        "today_u": round(today, 2),
        "clock": f"{hh:02d}:{mm:02d}",
        "keypad_lock": bool(keypad),
    }


class BleManager:
    """在后台线程跑自己的 asyncio loop，主线程 HTTP 请求通过 run_coroutine_threadsafe 提交。"""

    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.client = None
        self.connected = False
        self.dev_address = None
        self.state = {"connected": False}
        self.pos = 0
        self.lock = threading.Lock()
        self._stop = False
        self.last_scan = []        # 最近一次扫描到的全部设备（诊断用）
        self.last_discovery = []   # 最近一次连接后枚举到的全部 service/characteristic
        self.chars = {}            # 连接后枚举到的 characteristic 对象（按 uuid 小写缓存）
        t = threading.Thread(target=self._run_loop, daemon=True)
        t.start()

    def _run_loop(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _submit(self, coro, timeout=25):
        fut = asyncio.run_coroutine_threadsafe(coro, self.loop)
        return fut.result(timeout=timeout)

    def _char(self, expected: str):
        """按 uuid 取已发现的 characteristic 对象（连接时已缓存真实对象）。"""
        return self.chars.get(expected.lower())

    # ---- 协程 ----
    async def scan(self):
        """只扫描，列出全部设备（名字/地址/RSSI/广播服务），不连接。供诊断。"""
        devs = await BleakScanner.discover(timeout=8)
        out = []
        for d in devs:
            adv = getattr(d, "advertisement_data", None)
            uu = list(getattr(adv, "service_uuids", []) or []) if adv else []
            out.append({
                "name": d.name or "(无名字)",
                "addr": str(d.address) if d.address else None,
                "rssi": getattr(d, "rssi", None),
                "svc": uu,
            })
            dlog(f"扫描到设备: name={d.name!r} addr={d.address} rssi={getattr(d,'rssi',None)} adv_services={uu}")
        self.last_scan = out
        return {"ok": True, "devices": out}

    async def connect(self):
        if self.connected and self.client and self.client.is_connected:
            return {"ok": True, "msg": "已连接", "diag": {"scan": self.last_scan}}
        # 扫描并匹配（名字含 DAN / 广播含 SERVICE UUID 都算候选；macOS CoreBluetooth 缓存是
        # 连接后「特征找不到」的头号原因，故连接后用 start_notify 触发发现并 dump 真实 GATT，不靠缓存）
        dlog("开始扫描…")
        devs = await BleakScanner.discover(timeout=10)
        scanned = []
        dev = None
        for d in devs:
            adv = getattr(d, "advertisement_data", None)
            uu = list(getattr(adv, "service_uuids", []) or []) if adv else []
            meta = getattr(d, "metadata", None)
            if meta and not uu:
                uu = list((meta or {}).get("uuids", []) or [])
            scanned.append({"name": d.name, "addr": str(d.address) if d.address else None, "rssi": getattr(d, "rssi", None), "svc": uu})
            dlog(f"  设备: name={d.name!r} addr={d.address} rssi={getattr(d,'rssi',None)} adv_services={uu}")
            # 匹配优先级：① 精确名 ② 名字含 DAN ③ 广播含本服务 UUID
            if d.name and DEVICE_NAME.lower() == d.name.lower():
                dev = d
            elif d.name and "dan" in d.name.lower():
                if dev is None:
                    dev = d
            elif SERVICE.lower() in [x.lower() for x in uu]:
                if dev is None:
                    dev = d
        self.last_scan = scanned
        if not dev:
            return {"ok": False,
                    "msg": "未找到泵，确认已上电/蓝牙开/在范围内",
                    "diag": {"scan": scanned}}
        dlog(f"选定设备: name={dev.name!r} addr={dev.address}")
        client = BleakClient(dev.address)
        await client.connect(timeout=20)
        dlog(f"已连接 {dev.address}，开始枚举 GATT 服务…")

        # 触发并获取服务发现（bleak 跨版本兼容）：
        #  - 旧版(0.19+)有 get_services()，connect 后 client.services 亦已填充；
        #  - 新版(3.x)无 get_services()，服务发现由 start_notify/read/write 内部触发。
        # 两步走：先尝试显式 get_services()（无则忽略），再用 SCREEN 订阅触发发现。
        try:
            await client.get_services()
        except (AttributeError, TypeError):
            pass

        # 用 SCREEN 订阅来触发发现（新旧版都会内部枚举 GATT），记录结果
        screen_ok = False
        screen_err = None
        try:
            await client.start_notify(CHAR_SCREEN, self._on_notify)
            screen_ok = True
        except Exception as e:
            screen_err = e
            dlog(f"订阅 SCREEN 失败: {e}")

        # 发现应已完成，dump 设备真实暴露的全部 service / characteristic
        disc = []
        self.chars = {}
        try:
            services = client.services
        except Exception as e:
            dlog(f"读取 client.services 失败: {e}")
            services = None
        if services:
            for s in services:
                chars = []
                for c in s.characteristics:
                    props = [p for p in c.properties]
                    chars.append({"uuid": str(c.uuid), "handle": c.handle, "props": props})
                    self.chars[str(c.uuid).lower()] = c     # 缓存真实对象，后续读写直接用
                    dlog(f"    CHAR {c.uuid}  handle={c.handle}  props={props}")
                disc.append({"uuid": str(s.uuid), "chars": chars})
                dlog(f"  SERVICE {s.uuid}")
        self.last_discovery = disc

        if not screen_ok:
            names = [s["uuid"] for s in disc]
            return {"ok": False,
                    "msg": f"已连接但 GATT 中找不到/订阅不了 SCREEN 特征（{CHAR_SCREEN}）：{screen_err}。"
                           f"设备实际暴露的服务: {names}",
                    "diag": {"scan": scanned, "discovery": disc,
                             "hint": ("设备未暴露任何服务 → macOS GATT 缓存：系统设置→蓝牙『忽略此设备』"
                                      "后重启蓝牙再连。"
                                      if not disc else
                                      "列表里没有 6e40000c-…-e50e24dcca9e（SCREEN）→ 说明运行中的固件 UUID 基址"
                                      "与脚本不一致，需确认烧录的是含伴生 BLE 的固件，且脚本 UUID 与 config.h 对齐"
                                      "（NUS 变体 6E4000xx-B5A3-F393-E0A9-E50E24DCCA9E）。")}}
        self.client = client
        self.connected = True
        with self.lock:
            self.state = {"connected": True, "address": dev.address}
        self.loop.create_task(self._pos_poll())   # 后台每秒轮询电机位置，供可视化实时更新
        dlog(f"订阅 SCREEN 成功，进入就绪。")
        return {"ok": True, "msg": f"已连接 {dev.address}，SCREEN 订阅成功",
                "diag": {"scan": scanned, "discovery": disc}}

    async def _pos_poll(self):
        """连接期间每秒读一次电机位置，更新缓存（可视化活塞实时动）。"""
        ch = self._char(CHAR_SETTINGS)
        while self.connected and self.client and self.client.is_connected:
            try:
                if ch is None:
                    ch = self._char(CHAR_SETTINGS)
                if ch is None:
                    break
                await self.client.write_gatt_char(ch, f_get_pos(), response=True)
                raw = await self.client.read_gatt_char(ch)
                pos = struct.unpack('<I', raw[:4])[0] if len(raw) >= 4 else self.pos
                self.pos = pos
                with self.lock:
                    self.state['motor_pos_steps'] = pos
                    self.state['motor_pos_u'] = round(pos / STEPS_PER_UNIT, 3)
            except Exception:
                pass
            await asyncio.sleep(1)

    async def disconnect(self):
        if self.client:
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.connected = False
        with self.lock:
            self.state = {"connected": False}
        return {"ok": True, "msg": "已断开"}

    def _on_notify(self, _, data):
        d = bytes(data)
        with self.lock:
            st = decode_state(d)
            st["connected"] = True
            self.state = st

    async def _ensure(self):
        if not (self.client and self.client.is_connected):
            raise RuntimeError("未连接")

    async def move(self, dir_fwd: bool, steps: int, speed: int):
        if steps < 0:
            return {"ok": False, "msg": "steps 不能为负；0=连续点动，正整数=定量点动"}
        await self._ensure()
        ch = self._char(CHAR_CONTROL)
        if ch is None:
            return {"ok": False, "msg": f"未找到 CONTROL 特征（{CHAR_CONTROL}）"}
        await self.client.write_gatt_char(ch,
                                          f_manual_move(dir_fwd, steps, speed),
                                          response=True)
        if steps == 0:
            return {"ok": True, "msg": "连续点动已启动；点「停止」或撞限位即停"}
        await asyncio.sleep(0.15)
        return await self.get_pos()

    async def stop(self):
        await self._ensure()
        ch = self._char(CHAR_CONTROL)
        if ch is None:
            return {"ok": False, "msg": f"未找到 CONTROL 特征（{CHAR_CONTROL}）"}
        await self.client.write_gatt_char(ch, f_stop(), response=True)
        return {"ok": True, "msg": "已发送停止"}

    async def prime(self, units: float):
        if not (0 < units <= 200):
            return {"ok": False, "msg": "排气量需在 (0, 200] U"}
        await self._ensure()
        ch = self._char(CHAR_CONTROL)
        if ch is None:
            return {"ok": False, "msg": f"未找到 CONTROL 特征（{CHAR_CONTROL}）"}
        await self.client.write_gatt_char(ch, f_prime(units), response=True)
        return {"ok": True, "msg": f"已排气 {units}U"}

    async def rewind(self):
        await self._ensure()
        ch = self._char(CHAR_CONTROL)
        if ch is None:
            return {"ok": False, "msg": f"未找到 CONTROL 特征（{CHAR_CONTROL}）"}
        await self.client.write_gatt_char(ch, f_rewind(), response=True)
        return {"ok": True, "msg": "已发送退回"}

    async def clear(self):
        await self._ensure()
        ch = self._char(CHAR_CONTROL)
        if ch is None:
            return {"ok": False, "msg": f"未找到 CONTROL 特征（{CHAR_CONTROL}）"}
        await self.client.write_gatt_char(ch, f_clear(), response=True)
        return {"ok": True, "msg": "已清除报警"}

    async def key(self, name: str):
        k = KEY_CODE.get(name)
        if k is None:
            return {"ok": False, "msg": f"未知按键 {name}"}
        await self._ensure()
        ch = self._char(CHAR_KEY)
        if ch is None:
            return {"ok": False, "msg": f"未找到 KEY 特征（{CHAR_KEY}）"}
        await self.client.write_gatt_char(ch, f_key(k), response=True)
        return {"ok": True, "msg": f"已发送按键 {name}"}

    async def get_pos(self):
        await self._ensure()
        ch = self._char(CHAR_SETTINGS)
        if ch is None:
            return {"ok": False, "msg": f"未找到 SETTINGS 特征（{CHAR_SETTINGS}）"}
        await self.client.write_gatt_char(ch, f_get_pos(), response=True)
        raw = await self.client.read_gatt_char(ch)
        pos = struct.unpack('<I', raw[:4])[0] if len(raw) >= 4 else 0
        self.pos = pos
        with self.lock:
            self.state["motor_pos_steps"] = pos
            self.state["motor_pos_u"] = round(pos / STEPS_PER_UNIT, 3)
        return {"ok": True, "motor_pos_steps": pos,
                "motor_pos_u": round(pos / STEPS_PER_UNIT, 3)}


MGR = BleManager()


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def _post(self, fn):
        try:
            length = int(self.headers.get('Content-Length', 0))
            raw = self.rfile.read(length) if length else b'{}'
            try:
                data = json.loads(raw.decode('utf-8')) if raw else {}
            except Exception:
                data = {}
            res = MGR._submit(fn(data))
            self._send(200, res if res is not None else {"ok": True})
        except Exception as e:
            self._send(500, {"ok": False, "msg": f"{type(e).__name__}: {e}"})

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ('/', '/index.html'):
            self._serve_static('index.html', 'text/html; charset=utf-8')
        elif path == '/app.js':
            self._serve_static('app.js', 'application/javascript; charset=utf-8')
        elif path == '/style.css':
            self._serve_static('style.css', 'text/css; charset=utf-8')
        elif path == '/api/state':
            with MGR.lock:
                st = dict(MGR.state)
            st.setdefault('motor_pos_steps', MGR.pos)
            st.setdefault('motor_pos_u', round(MGR.pos / STEPS_PER_UNIT, 3))
            st['reservoir_full_u'] = RESERVOIR_U
            self._send(200, st)
        else:
            self._send(404, {"ok": False, "msg": "not found"})

    def do_POST(self):
        path = urlparse(self.path).path
        m = {
            '/api/connect':  lambda d: MGR.connect(),
            '/api/scan':     lambda d: MGR.scan(),
            '/api/disconnect': lambda d: MGR.disconnect(),
            '/api/move':     lambda d: MGR.move(bool(d.get('fwd')), int(d.get('steps', 0)), int(d.get('speed', 1500))),
            '/api/stop':     lambda d: MGR.stop(),
            '/api/prime':    lambda d: MGR.prime(float(d.get('units', 0))),
            '/api/rewind':   lambda d: MGR.rewind(),
            '/api/clear':    lambda d: MGR.clear(),
            '/api/key':      lambda d: MGR.key(str(d.get('name', ''))),
            '/api/pos':      lambda d: MGR.get_pos(),
        }.get(path)
        if m:
            self._post(m)
        else:
            self._send(404, {"ok": False, "msg": "not found"})

    def _serve_static(self, name, ctype):
        base = os.path.dirname(os.path.abspath(__file__))
        fp = os.path.join(base, 'static', name)
        try:
            with open(fp, 'rb') as f:
                body = f.read()
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):
        pass   # 静默


def main():
    port = int(os.environ.get('PORT', '8080'))
    httpd = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    print(f"泵 Web 调试后端已启动 → 浏览器打开 http://localhost:{port}")
    print("（首次 macOS 会弹蓝牙授权，请允许；系统设置→隐私与安全→蓝牙给终端权限）")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已退出。")


if __name__ == "__main__":
    main()
