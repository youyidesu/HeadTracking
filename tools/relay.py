#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""HT TURRET 图传转接（电脑端）

一次启动，自动完成：
  1. 选视频源（记住上次的选择，防止拉错到笔记本自带摄像头）
  2. 起 devserver（MJPEG / 快照 / 网页）
  3. 检测头显是否连着，自动 adb reverse（这样头显里 127.0.0.1:8080 就是这台电脑）
  4. 检测头显 App 是否在跑，跑起来就提示"可以戴了"
  5. devserver 挂了自动重启；采集卡掉线只提示，不反复重启

用法：
    python relay.py                 # 用上次的配置直接开
    python relay.py --pick          # 重新选视频源
    python relay.py --list          # 只列设备
    python relay.py --port 8080
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, "relay_config.json")
_CANDIDATES = [
    os.path.join(HERE, "..", "apps", "quest-web", "devserver.py"),
    os.path.join(HERE, "..", "apps", "quest", "devserver.py"),
    os.path.join(HERE, "devserver.py"),
]
DEVSERVER = next((p for p in _CANDIDATES if os.path.exists(p)), _CANDIDATES[0])
FFMPEG = os.environ.get(
    "HT_FFMPEG",
    r"C:\Users\loveH\.video-watch\tools\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe",
)
ADB = os.environ.get("HT_ADB", r"D:\i4\i4Tools9\files\adb\adb.exe")
PKG = "com.ht.turret"

DEFAULT = {
    "camera": "UGREEN 95348",
    "port": 8080,
    "width": 1280,
    "height": 720,
    "fps": 60,
    "quality": 6,
    "in_fmt": "yuyv422",     # 这块 UGREEN 只认 yuyv422，选 mjpeg 会零帧
    "serial": "",             # 空 = 用第一个在线设备
}


def load_cfg() -> dict:
    cfg = dict(DEFAULT)
    if os.path.exists(CFG):
        try:
            with open(CFG, encoding="utf-8") as f:
                cfg.update(json.load(f))
        except Exception as e:
            print("  配置读取失败，用默认值:", e)
    return cfg


def save_cfg(cfg: dict) -> None:
    with open(CFG, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    print("  已保存:", CFG)


# ───────────────────────── 设备枚举 ─────────────────────────

def list_video_devices() -> list[str]:
    """用 ffmpeg 列出 DirectShow 视频设备名"""
    if not os.path.exists(FFMPEG):
        print("  找不到 ffmpeg:", FFMPEG)
        return []
    try:
        out = subprocess.run(
            [FFMPEG, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"],
            capture_output=True, text=True, timeout=20,
        )
        text = out.stderr or ""
    except Exception as e:
        print("  ffmpeg 调用失败:", e)
        return []

    # 这个 ffmpeg 构建的输出形如：
    #   "UGREEN 95348" (video)
    #   "OBS Virtual Camera" (none)
    #   "数字音频接口 (UGREEN 95348)" (audio)
    names = []
    for line in text.splitlines():
        m = re.search(r'"([^"]+)"\s*\((video|audio|none)\)', line)
        if not m:
            continue
        name, kind = m.group(1), m.group(2)
        if kind == 'video' and name not in names:
            names.append(name)
    return names


def pick_device(cfg: dict) -> dict:
    devs = list_video_devices()
    if not devs:
        print("  ⚠ 没找到任何视频设备（采集卡插了吗？）")
        return cfg

    print("\n可用视频源：")
    for i, d in enumerate(devs, 1):
        mark = " ←当前" if d == cfg["camera"] else ""
        print(f"  [{i}] {d}{mark}")

    try:
        raw = input("\n选择编号（回车=保持当前）: ").strip()
    except EOFError:
        print("\n  （非交互环境，保持当前选择）")
        raw = ""
    if raw.isdigit() and 1 <= int(raw) <= len(devs):
        cfg["camera"] = devs[int(raw) - 1]
        print("  已选择:", cfg["camera"])
    save_cfg(cfg)
    return cfg


# ───────────────────────── adb ─────────────────────────

def adb(args: list[str], timeout=8) -> str:
    try:
        out = subprocess.run([ADB] + args, capture_output=True, text=True, timeout=timeout)
        return (out.stdout or "") + (out.stderr or "")
    except Exception:
        return ""


def pick_serial(cfg: dict) -> str:
    txt = adb(["devices"])
    serials = []
    for line in txt.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            serials.append(parts[0])
    if not serials:
        return ""
    if cfg.get("serial") in serials:
        return cfg["serial"]
    cfg["serial"] = serials[0]
    save_cfg(cfg)
    return cfg["serial"]


def ensure_reverse(serial: str, port: int) -> bool:
    if not serial:
        return False
    adb(["-s", serial, "reverse", f"tcp:{port}", f"tcp:{port}"])
    lst = adb(["-s", serial, "reverse", "--list"])
    return f"tcp:{port}" in lst


def app_running(serial: str) -> bool:
    if not serial:
        return False
    return bool(adb(["-s", serial, "shell", f"pidof {PKG}"]).strip())


# ───────────────────────── devserver ─────────────────────────

def start_devserver(cfg: dict) -> subprocess.Popen | None:
    if not os.path.exists(DEVSERVER):
        print("  找不到 devserver.py:", DEVSERVER)
        return None

    env = dict(os.environ)
    env.update({
        "HT_CAMERA": cfg["camera"],
        "HT_OUT_W": str(cfg["width"]),
        "HT_OUT_H": str(cfg["height"]),
        "HT_OUT_FPS": str(cfg["fps"]),
        "HT_Q": str(cfg["quality"]),
        "HT_IN_FMT": cfg["in_fmt"],
        "HT_FFMPEG": FFMPEG,
    })
    try:
        p = subprocess.Popen(
            [sys.executable, DEVSERVER, str(cfg["port"])],
            cwd=os.path.dirname(DEVSERVER), env=env,
        )
        return p
    except Exception as e:
        print("  devserver 启动失败:", e)
        return None


def probe_stream(port: int) -> str:
    """拉一小段流，看有没有帧"""
    try:
        import urllib.request
        req = urllib.request.urlopen(f"http://127.0.0.1:{port}/snapshot.jpg", timeout=6)
        data = req.read()
        if len(data) > 2000:
            return f"OK {len(data)} B"
        return f"数据偏小 {len(data)} B"
    except Exception as e:
        return "失败: " + str(e)[:60]


# ───────────────────────── 主循环 ─────────────────────────

def main() -> int:
    try:
        sys.stdout.reconfigure(line_buffering=True)   # 双击运行时也能实时看到状态
    except Exception:
        pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--pick", action="store_true", help="重新选择视频源")
    ap.add_argument("--list", action="store_true", help="只列设备")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--camera", default="")
    args = ap.parse_args()

    if args.list:
        for i, d in enumerate(list_video_devices(), 1):
            print(f"  [{i}] {d}")
        return 0

    cfg = load_cfg()
    if args.port:
        cfg["port"] = args.port
    if args.camera:
        cfg["camera"] = args.camera

    if args.pick:
        cfg = pick_device(cfg)
    elif not os.path.exists(CFG):
        save_cfg(cfg)          # 首次运行：先落盘默认值，不打断启动
        print("  首次运行，已写入默认配置（--pick 可改视频源）")
    else:
        print("  视频源:", cfg["camera"], "（--pick 可重选）")

    port = cfg["port"]
    print(f"\n启动 devserver :{port}  源={cfg['camera']}  {cfg['width']}x{cfg['height']}@{cfg['fps']}")
    proc = start_devserver(cfg)
    if proc is None:
        return 1

    time.sleep(4)
    print("  自检:", probe_stream(port))

    serial = ""
    last_state = ""
    while True:
        try:
            if proc.poll() is not None:
                print("  ⟳ devserver 退出了，重启")
                proc = start_devserver(cfg)
                time.sleep(3)
                continue

            serial = pick_serial(cfg)
            if serial:
                ok = ensure_reverse(serial, port)
                running = app_running(serial)
                state = f"设备 {serial} | 反向隧道 {'OK' if ok else '失败'} | App {'运行中' if running else '未启动'}"
                if running and state != last_state:
                    state += "  → 可以戴上头显了"
            else:
                state = "未检测到头显（插 USB 数据线）"

            if state != last_state:
                print(" ", time.strftime("%H:%M:%S"), state)
                last_state = state

            time.sleep(3)
        except KeyboardInterrupt:
            print("\n  退出，关闭 devserver")
            try:
                proc.terminate()
            except Exception:
                pass
            return 0


if __name__ == "__main__":
    sys.exit(main())
