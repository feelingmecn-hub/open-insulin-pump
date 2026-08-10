import asyncio
from bleak import BleakScanner

async def main():
    print("扫描 6 秒附近 BLE 设备 ...")
    devs = await BleakScanner.discover(timeout=6.0)
    print(f"发现 {len(devs)} 个设备：")
    found = False
    for d in devs:
        name = d.name or "(无名称)"
        tag = "  <== 我们的泵?" if "DAN12345AB" in name.upper() else ""
        print(f"  {d.address}  {name}{tag}")
        if "DAN12345AB" in name.upper():
            found = True
    if not found:
        print("未扫描到 DAN12345AB —— 泵可能未上电/关机/超出范围")

asyncio.run(main())
