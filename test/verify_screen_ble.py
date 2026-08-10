import asyncio, sys
from bleak import BleakClient, BleakScanner

TARGET = "DAN12345AB"
# 固件真实 GATT 基址是 NUS 变体（config.h:299）：01=服务 0C=screen
SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
SCREEN  = "6e40000c-b5a3-f393-e0a9-e50e24dcca9e"

async def main():
    print("扫描", TARGET, "...")
    dev = await BleakScanner.find_device_by_name(TARGET, timeout=8)
    if not dev:
        print("未找到泵，退出")
        return
    print("找到:", dev.address)

    got = []
    def on_notify(_, data):
        got.append(bytes(data))
        print(f"[通知] 长度={len(data)} : {bytes(data).decode(errors='replace')}")

    async with BleakClient(dev.address) as c:
        print("已连接。MTU 协商由系统处理。")
        try:
            await c.start_notify(SCREEN, on_notify)
        except Exception as e:
            print("订阅 SCREEN 失败:", e)
            return
        print("已订阅 SCREEN，等待泵推送 (6s) ...")
        await asyncio.sleep(6)
        await c.stop_notify(SCREEN)

    print("\n==== 汇总 ====")
    if got:
        longest = max(got, key=len)
        print(f"收到 {len(got)} 帧; 最长 {len(longest)} 字节")
        txt = longest.decode(errors='replace')
        print("最长帧内容:", txt)
        # 判定是否为完整 JSON：以 '}' 结尾且无截断
        if txt.strip().startswith('{') and txt.strip().endswith('}'):
            print("✅ 完整 JSON 已收到（MTU 足以承载 SCREEN 镜像）")
        else:
            print("⚠️ 收到但疑似截断（可能 Mac CoreBluetooth MTU 较小；手机端 512 不受影响）")
    else:
        print("❌ 未收到任何 SCREEN 通知")

asyncio.run(main())
