#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""devserver.py —— Quest 调试用的极简静态服务器

为什么不用 `python -m http.server` / `npx serve`：
  · npx serve 会把 /x.html 301 到 /x，浏览器会把这条重定向缓存到天荒地老
  · http.server 对无扩展名的文件返回 application/octet-stream，
    浏览器拿到就"下载"而不是渲染

这个服务器：
  · 永远不重定向
  · .html 和无扩展名一律返回 text/html; charset=utf-8
  · 所有响应带 Cache-Control: no-store，改完刷新即生效

用法：
    python devserver.py [端口] [目录]
"""
import io
import http.server
import os
import socketserver
import subprocess
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
ROOT = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(os.path.abspath(__file__))
HOST = os.environ.get('HT_HOST', '0.0.0.0')   # 头显要能访问，默认绑所有网卡

# ─────────── MJPEG 串流：ffmpeg 抓摄像头 → multipart/x-mixed-replace ───────────
FFMPEG = os.environ.get(
    "HT_FFMPEG",
    r"C:\Users\loveH\.video-watch\tools\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe",
)
CAMERA = os.environ.get("HT_CAMERA", "UGREEN 95348")   # HDMI 采集卡（图传）
OUT_W = int(os.environ.get("HT_OUT_W", "1920"))        # 输出宽
OUT_H = int(os.environ.get("HT_OUT_H", "1080"))        # 输出高
OUT_FPS = int(os.environ.get("HT_OUT_FPS", "60"))      # 输出帧率
QUALITY = os.environ.get("HT_Q", "5")                  # mjpeg 质量 2(最好)~8
IN_FMT = os.environ.get("HT_IN_FMT", "mjpeg")          # 采集卡输入格式：mjpeg 或 yuyv422

_latest = {"jpeg": None, "seq": 0, "t": 0.0}
_lock = threading.Lock()
_ff = {"proc": None}


def _ffmpeg_worker():
    # rtbufsize 必须给大：1080p 采集卡的实时缓冲默认只有几 MB，会疯狂丢帧
    cmd = [
        FFMPEG, "-hide_banner", "-loglevel", "error",
        "-rtbufsize", "512M",
        "-f", "dshow",
    ]

    # 采集卡的输入格式：实测这块 UGREEN 95348 只有 rawvideo/yuyv422 能稳定出帧，
    # 选 mjpeg 会报 "No JPEG data found in image"。
    if IN_FMT == "yuyv422":
        cmd += ["-vcodec", "rawvideo", "-pix_fmt", "yuyv422"]
    else:
        cmd += ["-vcodec", "mjpeg"]

    cmd += [
        "-video_size", "1920x1080",
        "-framerate", "60",
        "-i", "video=" + CAMERA,
        "-vf", "scale=%d:%d" % (OUT_W, OUT_H),
        "-r", str(OUT_FPS),
        "-f", "mjpeg", "-q:v", QUALITY, "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, bufsize=0)
    _ff["proc"] = proc
    buf = b""
    while True:
        chunk = proc.stdout.read(4096)
        if not chunk:
            break
        buf += chunk
        while True:
            a = buf.find(b"\xff\xd8")
            b = buf.find(b"\xff\xd9", a + 2) if a >= 0 else -1
            if a < 0 or b < 0:
                break
            jpg = buf[a:b + 2]
            buf = buf[b + 2:]
            with _lock:
                _latest["jpeg"] = jpg
                _latest["seq"] += 1
                _latest["t"] = time.time()


# ─────────── 无采集卡时的合成画面（用于验证图传链路） ───────────
_synth_n = 0
_synth_lock = threading.Lock()


NO_SIGNAL = {"img": None}


def _no_signal_jpeg(w=1280, h=720):
    """超过 2 秒没新帧时返回的占位图，避免客户端把旧帧当成实时画面。"""
    if NO_SIGNAL["img"]:
        return NO_SIGNAL["img"]
    try:
        from PIL import Image, ImageDraw
    except Exception:
        return None
    img = Image.new("RGB", (w, h), (18, 18, 22))
    d = ImageDraw.Draw(img)
    for y in range(0, h, 40):
        d.line([(0, y), (w, y)], fill=(30, 30, 36))
    d.text((w // 2 - 90, h // 2 - 30), "NO SIGNAL", fill=(240, 80, 80))
    d.text((w // 2 - 130, h // 2 + 4), "check HDMI / VRX power", fill=(200, 200, 200))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=70)
    NO_SIGNAL["img"] = buf.getvalue()
    return NO_SIGNAL["img"]


def _synth_jpeg(w=640, h=360):
    """画一张带编号和十字线的测试图，证明 App → HTTP → 贴图 这条链是通的。"""
    global _synth_n
    try:
        from PIL import Image, ImageDraw
    except Exception:
        return None

    with _synth_lock:
        _synth_n += 1
        n = _synth_n

    img = Image.new("RGB", (w, h), (16, 22, 34))
    d = ImageDraw.Draw(img)

    # 网格
    for x in range(0, w, 40):
        d.line([(x, 0), (x, h)], fill=(30, 44, 62))
    for y in range(0, h, 40):
        d.line([(0, y), (w, y)], fill=(30, 44, 62))

    # 中心十字
    d.line([(w // 2 - 40, h // 2), (w // 2 + 40, h // 2)], fill=(90, 230, 140), width=3)
    d.line([(w // 2, h // 2 - 40), (w // 2, h // 2 + 40)], fill=(90, 230, 140), width=3)

    # 计数方块（每帧移位，肉眼能看出在刷新）
    bx = 40 + (n * 7) % (w - 80)
    d.rectangle([bx, h - 70, bx + 50, h - 20], fill=(245, 190, 80))

    d.text((16, 14), "HT TURRET  TEST PATTERN", fill=(215, 230, 245))
    d.text((16, 34), "frame %d" % n, fill=(90, 230, 140))

    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=70)
    return buf.getvalue()


def _ensure_ffmpeg():
    p = _ff["proc"]
    if p is None or p.poll() is not None:
        threading.Thread(target=_ffmpeg_worker, daemon=True).start()


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def guess_type(self, path):
        base, ext = os.path.splitext(path)
        if ext.lower() in ('', '.html', '.htm'):
            return 'text/html; charset=utf-8'
        return super().guess_type(path)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        super().end_headers()

    def do_POST(self):
        # 页面回传诊断结果：POST /report  body=文本
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n).decode('utf-8', 'replace') if n else ''
        with open(os.path.join(ROOT, 'report.log'), 'a', encoding='utf-8') as f:
            f.write(body + "\n---\n")
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(b'ok')

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.end_headers()

    def _mjpeg(self):
        _ensure_ffmpeg()
        # 等第一帧（最多 2.5 s）：采集卡没插就返回 503，页面会留空
        for _ in range(125):
            with _lock:
                if _latest["jpeg"]:
                    break
            time.sleep(0.02)
        else:
            self.send_error(503, "capture device not available")
            return

        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        last = -1
        try:
            while True:
                with _lock:
                    seq, jpg = _latest["seq"], _latest["jpeg"]
                if jpg is None or seq == last:
                    # 超过 2 秒没有新帧：改发 NO SIGNAL 占位图（每秒一次）
                    if time.time() - _latest.get("t", 0) > 2.0 and seq == last:
                        ns = _no_signal_jpeg()
                        if ns:
                            self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                            self.wfile.write(b"Content-Length: " + str(len(ns)).encode() + b"\r\n\r\n")
                            self.wfile.write(ns)
                            self.wfile.write(b"\r\n")
                            time.sleep(1.0)
                            continue
                    time.sleep(0.005)
                    continue
                last = seq
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                self.wfile.write(b"Content-Length: " + str(len(jpg)).encode() + b"\r\n\r\n")
                self.wfile.write(jpg)
                self.wfile.write(b"\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path.startswith("/stream.mjpg"):
            return self._mjpeg()
        if self.path.startswith("/snapshot.jpg"):
            _ensure_ffmpeg()
            jpg = None
            for _ in range(200):
                with _lock:
                    jpg = _latest["jpeg"]
                if jpg:
                    break
                time.sleep(0.02)
            if not jpg:
                jpg = _synth_jpeg()          # 没有采集卡就用合成图
            if not jpg:
                self.send_error(503, "camera not ready")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(jpg)))
            self.end_headers()
            self.wfile.write(jpg)
            return
        # 目录 / 或已存在的无扩展名文件都走默认处理；其余 404 由父类负责
        super().do_GET()

    def log_message(self, fmt, *args):
        print(f"[{self.address_string()}] {fmt % args}")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == '__main__':
    os.chdir(ROOT)
    with Server((HOST, PORT), Handler) as httpd:
        print(f'devserver: http://{HOST}:{PORT}/  (root={ROOT})')
        httpd.serve_forever()
