import asyncio, threading, time
import serial
from bleak import BleakScanner, BleakClient
from bleak.exc import BleakError

PORT = "COM4"
NAME = "HT-S3-01"
HOLD = 10.0

log, stop = [], False

def reader():
    try:
        s = serial.Serial(PORT, 115200, timeout=0.2)
    except Exception as e:
        log.append(f"[serial] {e}")
        return
    while not stop:
        try:
            c = s.read(512)
            if c:
                log.append(c.decode("utf-8", "replace").rstrip())
        except Exception:
            break
    s.close()

def on_disc(_):
    log.append("[PY] 收到 disconnect 回调")

async def main():
    threading.Thread(target=reader, daemon=True).start()
    await asyncio.sleep(0.4)

    dev = await BleakScanner.find_device_by_name(NAME, timeout=10.0)
    if not dev:
        print("没扫到"); return
    print(f"找到 {dev.address} (RSSI 由系统给)，连接并保持 {HOLD:.0f}s…")

    try:
        async with BleakClient(dev, timeout=20.0, disconnected_callback=on_disc) as cli:
            print("已连接")
            t0 = time.time()
            while time.time() - t0 < HOLD:
                await asyncio.sleep(1)
                print(f"  {time.time()-t0:4.1f}s  connected={cli.is_connected}")
            print("保持结束")
    except BleakError as e:
        print(f"连接失败: {e}")

    await asyncio.sleep(0.5)
    global stop
    stop = True
    await asyncio.sleep(0.3)
    print("\n=== S3 串口 ===")
    for l in log[-25:]:
        print(l)

asyncio.run(main())
