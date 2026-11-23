# PC 侧图传桥：采集卡 → 浏览器（喂给 ControlAnything 页面）

> 拓扑：VRX/图传接收 → HDMI → **USB 采集卡 → PC**；PC 同时跑 ControlAnything 的 Node 服务。
> 目标：把图传画面**低延迟地**送进 Quest 浏览器里的那个 WebXR 页面，与头姿同屏。

---

## 1. 先明确：抓"设备"不要抓"窗口"

| 抓法 | 命令 | 额外延迟 | 结论 |
|---|---|---|---|
| **直接抓采集卡**（DShow 设备） | `-f dshow -i video="USB Video"` | 0 | ✅ 用这个 |
| 抓窗口/桌面 | `-f gdigrab -i title=...` | +16~33 ms（DWM 合成）+ 掉帧风险 | ❌ 不要 |

采集卡在 PC 上就是标准 UVC 设备，ffmpeg 的 dshow 后端能直接拿到原始帧，绕过桌面合成。

---

## 2. 三种把画面送进浏览器的方案

| 方案 | 延迟 | 带宽 | 实现难度 | 适用 |
|---|---|---|---|---|
| **MJPEG over HTTP/WS** | 30~60 ms | 1080p60 ≈ 50~100 Mbps | ★☆☆ | 快速验证，但 WiFi 吃不住 |
| **WebRTC（ffmpeg+MediaMTX / GStreamer）** | **60~100 ms** | 8~15 Mbps | ★★☆ | **推荐起步** |
| **WebCodecs + WebSocket**（自建） | **25~50 ms** | 8~15 Mbps | ★★★ | 延迟敏感时的终极档 |

三者都能被 WebXR 页面用成 `THREE.VideoTexture` 或 `<video>` 平面。

---

## 3. 推荐起步方案：ffmpeg → NVENC → MediaMTX → WHEP

### 3.1 采集+编码（PC 上一条命令）

```bat
ffmpeg -f dshow -rtbufsize 64M -video_size 1920x1080 -framerate 60 ^
       -i video="USB Video" ^
       -c:v h264_nvenc -preset p1 -tune ull -rc cbr -b:v 8M -maxrate 8M -bufsize 4M ^
       -g 30 -bf 0 -pix_fmt yuv420p ^
       -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/cam
```

关键参数：
- `-preset p1 -tune ull`：NVENC 最低延迟档
- `-bf 0`：禁用 B 帧（B 帧会引入重排序延迟）
- `-g 30`：2 秒一个关键帧，够快恢复
- `-bufsize 4M`：码率缓冲压小，避免突发延迟

没有 NVENC 就退 `-c:v libx264 -preset ultrafast -tune zerolatency`（720p60 撑得住，1080p60 吃 CPU）。

### 3.2 转 WebRTC（MediaMTX）

`mediamtx.yml`：

```yaml
rtspAddress: :8554
webrtcAddress: :8889
webrtc: yes
webrtcAdditionalHosts: [你的PC局域网IP]
paths:
  cam:
    source: publisher
```

浏览器端用 WHEP 拉流：`http://<PC-IP>:8889/cam/whep`。
MediaMTX 自带播放页（`http://<PC-IP>:8889/cam`）可先验证。

### 3.3 塞进 ControlAnything 页面（three.js）

```js
const video = document.createElement('video');
video.playsInline = true; video.muted = true; video.autoplay = true;
// WHEP 握手后把远端流挂上去（用 mediamtx 的 WHEP js 或手写 RTCPeerConnection）
attachWhep('http://192.168.1.10:8889/cam/whep', video);

const tex = new THREE.VideoTexture(video);
tex.colorSpace = THREE.SRGBColorSpace;
const plane = new THREE.Mesh(
  new THREE.PlaneGeometry(1.6, 0.9),
  new THREE.MeshBasicMaterial({ map: tex })
);
plane.position.set(0, 1.5, -2.0);   // 世界锁定在你想要的位置
scene.add(plane);
```

---

## 4. 必须做的一件事：给画面打时间码

在 PC 端把时间戳**烧进画面**（左下角小字），用来实测端到端延迟，也给 HUD 对齐提供基准：

```bat
-vf "drawtext=text='%{localtime\:%H\\\\\:%M\\\\\:%S}.%{eif\:mod(t*1000\,1000)\:d\:3}':x=10:y=10:fontsize=24:fontcolor=white"
```

头显这边用 `requestVideoFrameCallback` 拿每帧的 `presentationTime` / `expectedDisplayTime`，
就能算出"这一帧从相机到我眼前花了多少毫秒"，同时把遥测准星按同一时基延迟渲染。

```js
video.requestVideoFrameCallback((now, meta) => {
  // meta.presentationTime / meta.expectedDisplayTime / meta.mediaTime
  hud.setVideoFrameTime(meta.expectedDisplayTime);
  video.requestVideoFrameCallback(arguments.callee);   // 递归（写成具名函数）
});
```

---

## 5. 延迟预算（1080p60，NVENC + MediaMTX）

| 环节 | 典型 |
|---|---|
| 采集卡 UVC 出帧 | 15~30 ms |
| NVENC 编码（p1/ull） | 8~15 ms |
| 网络（5/6 GHz WiFi，单跳） | 5~15 ms |
| 浏览器解码（硬件） | 10~20 ms |
| WebRTC 抖动缓冲 | 10~30 ms |
| Quest 显示 | 15~25 ms |
| **合计** | **65~135 ms** |

想再压，就把 WebRTC 换成 **WebCodecs + WebSocket**（省掉抖动缓冲和解码排队，约 25~50 ms），
代价是自建传输层、丢包要靠自己补。

---

## 6. 与头姿链路的配合

```
视频：VRX → 采集卡 → PC → (WebRTC) → Quest 浏览器
头姿：Quest 浏览器 → (WS) → PC(Node) → (UDP) → C5 → SPI → F411
```

两条流**在 PC 上汇合**，这带来两个好处：
1. PC 同时知道"视频帧时间戳"和"头姿时间戳"，可以做**同源时钟对齐**
2. HUD 准星可以由 PC 统一计算后再下发（或前端自己算），不会两边时基打架

**要不要让 PC 参与对准？** 建议不要——PC 只做转发和测延迟，控制环留在 F411，
HUD 的延迟补偿在前端用 `requestVideoFrameCallback` 的时间戳本地做，少一跳就少一份不确定性。

---

## 7. 带宽与频谱

| 流 | 码率 |
|---|---|
| 视频 1080p60 | 8 Mbps |
| 视频 720p60（降级） | 4 Mbps |
| pose 16 B@90 Hz | 1.4 kB/s |
| 遥测 30 B@50 Hz | 1.5 kB/s |

合计 ≈ 8.02 Mbps。**瓶颈永远是空口竞争**，不是带宽：
- Quest ↔ PC 这段走 5/6 GHz，图传（模拟 5.8G）走 5645~5725
- 6 GHz 可用时优先用它，彻底避开

---

## 8. 落地步骤

1. **只验视频**：`ffmpeg dshow → 文件` → 确认能抓到采集卡、分辨率和帧率对
2. **接 MediaMTX**：浏览器打开 `http://PC:8889/cam` 能看 → 手机拍屏测延迟
3. **接进页面**：VideoTexture 贴到平面 → 确认在 XR 里稳定、不撕裂
4. **加时间码**：跑一遍实测延迟，记录 65~135 ms 落在哪一档
5. **头姿同时跑**：确认两路互不干扰（CPU/GPU/网络）
6. **再谈 WebCodecs**：如果 100 ms 的手感不可接受，再上自建低延迟通道

---

## 9. 风险

| 风险 | 说明 | 对策 |
|---|---|---|
| NVENC 不可用（核显/老卡） | 1080p60 软编会掉帧 | 降到 720p60 + ultrafast |
| MediaMTX 的 WHEP 在 Quest 浏览器兼容性 | 极少数版本有 SDP 解析差异 | 先用自带播放页验证 |
| WiFi 抖动导致花屏 | 关键帧间隔 2 s，恢复要 2 s | 把 `-g` 降到 15，或开 WebRTC 的 NACK/FEC |
| 采集卡 USB 带宽不足 | 1080p60 MJPG 需要 USB 3.0 | 确认采集卡走 USB 3.0 口 |

---

## 10. 拓扑修正：Quest 也用 USB 接 PC

前面按"Quest 走 WiFi 连 PC"算的。如果 **Quest 用 USB-C 直连 PC**（不是 Link/Air Link，
只是拿来当网线用），整条链路会干净得多：

```
VRX → HDMI → 采集卡 → PC ──USB-C── Quest3 浏览器
                       │
                       └── WiFi/UDP ──► ESP32-C5 → SPI → F411
```

### 10.1 三个立刻到手的好处

| 好处 | 说明 |
|---|---|
| **secure context 免费解决** | `adb reverse tcp:8080 tcp:8080`，头显浏览器直接开 `http://127.0.0.1:8080`，localhost 属于"可信来源"，**WebXR 直接可用**，不用自签证书、不用装 CA |
| **Quest 侧零 WiFi** | 5 GHz 全留给 PC→C5 那一跳，不再和 5.8G 图传、2.4G 遥控抢空口 |
| **视频延迟更稳** | USB 链路 ~1 ms 且无抖动，WebRTC 的抖动缓冲可以压到最小 |

### 10.2 具体怎么配

```bat
:: 1) 头显开发者模式 + USB 调试打开，USB 连上 PC
adb devices                       :: 应能看到设备
:: 2) 把 PC 的端口映射进头显的 localhost
adb reverse tcp:8080 tcp:8080     :: ControlAnything 页面
adb reverse tcp:8889 tcp:8889     :: MediaMTX 的 WHEP
:: 3) 头显浏览器打开
::    http://127.0.0.1:8080        （页面 + WS 走这条）
::    http://127.0.0.1:8889/cam    （视频）
```

页面里的 WS 地址写 `ws://127.0.0.1:8080/ws`，WHEP 写 `http://127.0.0.1:8889/cam/whep`——
全部走 USB，一根线解决。

### 10.3 代价

- **活动范围被线限制**（操作手离 PC 不能超过线长）→ 调试/比赛台架阶段完全可接受
- USB-C 被占，头显**没法同时接 UVC 采集卡**——但我们本来就把采集放在 PC 上了
- `adb reverse` 需要 adb 常驻；PC 侧可写成开机自启的脚本

### 10.4 两条路可以切换

| 场景 | 建议 |
|---|---|
| 台架调试 / 比赛现场（操作手坐 PC 边） | **USB + localhost**（本文 §10） |
| 需要自由走动 | 退回 WiFi，用自签 HTTPS（`selfsigned`）提供页面 |

### 10.5 修正后的延迟账

| 环节 | USB 方案 | WiFi 方案 |
|---|---|---|
| 视频：采集卡 | 15~30 ms | 15~30 ms |
| 视频：NVENC | 8~15 ms | 8~15 ms |
| 视频：传输 | **~1 ms** | 5~15 ms |
| 视频：解码+抖动缓冲 | 20~35 ms | 25~50 ms |
| 显示 | 15~25 ms | 15~25 ms |
| **视频合计** | **60~105 ms** | 65~135 ms |
| **pose 链路** | **~1 ms**（USB） | 5~15 ms（WiFi） |

pose 那条几乎白送——90 Hz 的 16 B 帧走 USB localhost，延迟可以忽略。
