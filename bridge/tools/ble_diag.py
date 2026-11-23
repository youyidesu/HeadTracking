import asyncio, threading, time, sys
import serial
from bleak import BleakScanner, BleakClient

PORT = "COM4"
NAME = "S3-BLE-TEST"

log = []
stop = False

def reader():
    try:
        s = serial.Serial(PORT, 115200, timeout=0.2)
    except Exception as e:
        log.append(f"[serial] 打不开 {PORT}: {e}")
        return
    while not stop:
        try:
            c = s.read(512)
            if c:
                log.append("[S3] " + c.decode("utf-8", "replace").rstrip("\r\n"))
        except Exception:
            break
    s.close()

async def main():
    t = threading.Thread(target=reader, daemon=True)
    t.start()
    await asyncio.sleep(0.5)

    print("扫描…")
    dev = await BleakScanner.find_device_by_name(NAME, timeout=10.0)
    if not dev:
        print("没扫到"); return
    print(f"找到 {dev.address}，尝试连接…")

    try:
        async with BleakClient(dev, timeout=20.0) as cli:
            print("连接成功，服务表：")
            for s in cli.services:
                print("  ", s.uuid)
    except Exception as e:
        print(f"连接失败: {type(e).__name__}: {e}")

    await asyncio.sleep(1.5)
    global stop
    stop = True
    await asyncio.sleep(0.3)
    print("\n=== 期间 S3 的串口输出 ===")
    for l in log[-40:]:
        print(l)

asyncio.run(main())
