#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_latency.py —— C5 蓝牙连接能力 + 往返延迟测试

对端：esp32c5_test 里 ble_test.c 起的 GATT 服务
    服务 FFF0 / 写入 FFF1 / 回显通知 FFF2

用法：
    python ble_latency.py                      # 默认找 C5-BLE-TEST，连上打 200 包
    python ble_latency.py --count 500 --hz 90  # 按 90Hz 打 500 包（模拟真实帧率）
    python ble_latency.py --scan               # 只扫描，列出附近的 BLE 设备

输出：每包 RTT，最后给 min/avg/p50/p95/max，以及 RTT/2（单程估计）
"""
import argparse
import asyncio
import statistics
import sys
import time

from bleak import BleakClient, BleakScanner

SVC = "0000fff0-0000-1000-8000-00805f9b34fb"
CH_RX = "0000fff1-0000-1000-8000-00805f9b34fb"   # write
CH_TX = "0000fff2-0000-1000-8000-00805f9b34fb"   # notify


async def scan_all():
    print("扫描 8 秒…")
    devs = await BleakScanner.discover(timeout=8.0)
    for d in devs:
        print(f"  {d.address}  {d.name!r}")
    print(f"共 {len(devs)} 个设备")


async def run(name: str, count: int, hz: float, verbose: bool):
    print(f"查找 {name} …")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (ad.local_name or "") == name, timeout=10.0)
    if dev is None:
        print(f"没找到 {name}（确认 C5 已上电、BLE 测试已编译进去）")
        return 1

    print(f"连接 {dev.address} …")
    rtts: list[float] = []
    evt = asyncio.Event()
    last = {"t": 0.0}

    def on_notify(_char, data: bytearray):
        now = time.perf_counter()
        if last["t"]:
            rtts.append((now - last["t"]) * 1000.0)   # ms
        evt.set()

    async with BleakClient(dev, timeout=15.0) as cli:
        print(f"已连接，MTU 由系统协商；开始 {count} 包 @ {hz:.0f} Hz")
        await cli.start_notify(CH_TX, on_notify)

        period = 1.0 / hz if hz > 0 else 0.0
        payload = bytearray(16)
        t_start = time.perf_counter()

        for i in range(count):
            payload[0] = i & 0xFF
            payload[1] = (i >> 8) & 0xFF
            evt.clear()
            last["t"] = time.perf_counter()
            try:
                await cli.write_gatt_char(CH_RX, payload, response=False)
            except Exception as e:
                print(f"写入失败: {e}")
                break
            try:
                await asyncio.wait_for(evt.wait(), timeout=1.0)
            except asyncio.TimeoutError:
                print(f"第 {i} 包超时（没收到回显）")
                break
            if verbose and (i % 10 == 0):
                print(f"  #{i:<4} RTT {rtts[-1]:6.2f} ms")
            if period:
                await asyncio.sleep(max(0.0, t_start + (i + 1) * period - time.perf_counter()))

        await cli.stop_notify(CH_TX)

    if not rtts:
        print("没有收到任何回显——检查 UUID 或服务是否启动")
        return 1

    rtts_sorted = sorted(rtts)
    p = lambda q: rtts_sorted[min(len(rtts_sorted) - 1, int(len(rtts_sorted) * q))]
    print("\n── 结果 ─────────────────────────────")
    print(f"  样本      {len(rtts)}")
    print(f"  RTT min   {min(rtts):6.2f} ms")
    print(f"  RTT avg   {statistics.mean(rtts):6.2f} ms")
    print(f"  RTT p50   {p(0.50):6.2f} ms")
    print(f"  RTT p95   {p(0.95):6.2f} ms")
    print(f"  RTT max   {max(rtts):6.2f} ms")
    print(f"  抖动(std) {statistics.pstdev(rtts):6.2f} ms")
    print(f"  单程估计  {statistics.mean(rtts) / 2:6.2f} ms（RTT/2，含对端处理）")
    print("─────────────────────────────────────")
    print("  参考：BLE 连接间隔 7.5ms 时 RTT 通常 8~20ms；")
    print("        如果 RTT 稳定在 30ms 以上，多半是系统把间隔强制到 30ms 了。")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="C5-BLE-TEST")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--hz", type=float, default=20.0)
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.scan:
        asyncio.run(scan_all())
        return 0

    try:
        return asyncio.run(run(a.name, a.count, a.hz, a.verbose))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
