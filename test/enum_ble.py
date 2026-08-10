import asyncio
from bleak import BleakScanner, BleakClient

TARGET = "DAN12345AB"

async def main():
    dev = await BleakScanner.find_device_by_name(TARGET, timeout=8)
    if not dev:
        print("未找到泵"); return
    async with BleakClient(dev.address) as c:
        print("已连接:", dev.address)
        for s in c.services:
            print(f"SRV {s.uuid}")
            for ch in s.characteristics:
                print(f"   CHR {ch.uuid}  props={[p for p in ch.properties]}")

asyncio.run(main())
