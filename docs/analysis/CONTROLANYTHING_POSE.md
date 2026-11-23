# 从 ControlAnything 里抓 Quest3 头追数据 —— 分析

> ControlAnything：Meta Quest 3 上的 WebXR 应用（论文 arXiv:2611.08321，仓库 `Apolotary/controlanything`）。
> 用户在头显里用手指点选真实设备，由笔记本上的 coding agent 现场写 connector，把空间控件或
> "每帧手部程序"绑定到设备上。**它不是 Unity 工程，是浏览器里的 three.js WebXR 页面 + 笔记本上的 Node 服务。**

---

## 1. 它的架构决定了数据在哪、怎么出

```
Quest3 浏览器（three.js WebXR immersive-ar 页面）
        │  WebSocket（https + 自签证书，页面由笔记本提供）
        ▼
笔记本 Node 服务（模型密钥 / agent / connector host）
        │
        ▼
executors（Web MIDI / Web Bluetooth / AppleScript）→ 真实设备
```

来自论文 §4.1（原文要点）：*"The headset client is a three.js WebXR immersive-ar application in
Quest Browser. The server, a Node program on the wearer's laptop, holds the model keys, the agent
and the connector host behind a WebSocket hub."*
仓库依赖里能看到 `selfsigned`、`ws`、`three`，印证了"自签 HTTPS + WebSocket"这条路径。

**结论 1：头姿数据在客户端的 `XRFrame.getViewerPose()` 里**，每帧一次，出帧率 = 显示刷新率（72/90/120）。
**结论 2：浏览器不能发裸 UDP**，往外只能 WebSocket / WebRTC。
**结论 3：ControlAnything 必须有一台笔记本在环**（模型密钥和 connector 都在那儿），所以走它的通道，
头姿天然要经过 PC 一跳。

---

## 2. 三条可行路径

| 方案 | 链路 | 延迟 | 改动量 | 适用 |
|---|---|---|---|---|
| **A. 借它的 WebSocket**（推荐先用） | Quest →(WS)→ Node →(UDP)→ C5 →(SPI)→ F411 | +2~8 ms（PC 一跳） | 客户端 ~25 行 + 服务端 ~15 行 | 已有 ControlAnything，最快验证 |
| **B. 客户端第二条 WS 直连 C5** | Quest →(WS)→ C5 →(SPI)→ F411 | 最低 | 客户端 ~25 行；C5 要跑 WS 服务 | 想要低延迟、且 C5 能做 WS |
| **C. 抛开 ControlAnything，自建最小页面** | Quest →(WS)→ C5 →(SPI)→ F411 | 最低 | C5 托管页面 + 90 行 JS | 只要头追，不要它那些功能 |

**方案 A 的现实优势**：ControlAnything 已经解决了 **secure context**（WebXR 只在 https / localhost 下可用，
局域网 `http://192.168.x.x` 直接不给 `navigator.xr`）。自建页面要么配自签证书 + 在 Quest 里装 CA，
要么用 `adb reverse` 映射 localhost——都是额外麻烦。

---

## 3. 方案 A 的具体改法

### 3.1 客户端：加一个 PoseTap（每帧读头姿）

```js
// 挂在这个页面的 renderer.setAnimationLoop 回调里（ControlAnything 的主循环）
const _tap = {
  ws: null, seq: 0, zero: null,                       // zero = Quaternion，recenter 用
  open(url) { this.ws = new WebSocket(url); this.ws.binaryType = 'arraybuffer'; },
  recenter(q) { this.zero = q.slice(); },
  push(q) {
    if (!this.ws || this.ws.readyState !== 1) return;
    const z = this.zero;
    // rel = inverse(zero) * q
    const rel = z ? mulQuat(invQuat(z), q) : q;
    // WebXR: 右手系, Y up, -Z 前 → 前向量
    const f = rotateVec(rel, [0, 0, -1]);
    const yaw   = Math.atan2(-f[0], -f[2]) * 180 / Math.PI;   // 右转为正，需上机验符号
    const pitch = Math.asin(clamp(f[1], -1, 1)) * 180 / Math.PI;

    const b = new ArrayBuffer(16);
    const v = new DataView(b);
    v.setUint16(0, 0xA55A, true);
    v.setUint16(2, this.seq++ & 0xFFFF, true);
    v.setInt16(4, Math.round(yaw * 100), true);
    v.setInt16(6, Math.round(pitch * 100), true);
    v.setInt16(8, 0, true);                                  // yaw_rate，用相邻帧差分或留 0
    v.setInt16(10, 0, true);                                 // pitch_rate
    v.setUint16(12, 0, true);                                // flags
    v.setUint16(14, crc16(b, 14), true);
    this.ws.send(b);                                         // 16B，比 30B 更省
  }
};
```

主循环里：

```js
renderer.setAnimationLoop((time, frame) => {
  const pose = frame.getViewerPose(refSpace);              // refSpace: 'local-floor' 或 'local'
  if (pose) {
    const o = pose.transform.orientation;                   // {x,y,z,w}
    _tap.push([o.x, o.y, o.z, o.w]);
  }
  // …原来的渲染
});
```

**注意**：`getViewerPose` 拿到的是**预测到显示时刻**的姿态（和 Unity 的 `GetNodePose` 同性质），
补偿逻辑见 `docs/QUEST3_HEADTRACK.md` §2/§3。

### 3.2 服务端：Node 转成 UDP 发给 C5

```js
import dgram from 'node:dgram';
const sock = dgram.createSocket('udp4');

wss.on('connection', ws => ws.on('message', (buf) => {
  if (buf.length === 16 && buf.readUInt16LE(0) === 0xA55A) {
    sock.send(buf, 5005, '192.168.4.1');      // C5 的 IP
  }
}));
```

C5 侧收 16 B → 补齐成 `qp_head_t` 30 B（或直接改协议用 16 B）→ SPI 发给 F411。

**建议直接改协议为 16 B**：`magic/seq/yaw/pitch/yaw_rate/pitch_rate/flags/crc`，
四元数在云台侧没用（我们只用 yaw/pitch + 角速度）。F411 的 `esp_link.c` 改一个结构体即可。

---

## 4. 坐标与符号（最容易翻车的地方）

| 系 | 手性 | 上 | 前 | 备注 |
|---|---|---|---|---|
| WebXR | 右手 | +Y | **-Z** | `local-floor` 原点在地面 |
| 我们的协议 | —— | —— | —— | `yaw=atan2(-f.x,-f.z)`、`pitch=asin(f.y)`，单位 0.01° |

上机第一件事就是**验符号**：头向右转（+yaw）→ 云台应同向；低头 → pitch 减小。
反了就在这一层改号，别去改驱动器方向。

---

## 5. 延迟预算（方案 A）

| 环节 | 典型 |
|---|---|
| WebXR 预测提前量 | 20~40 ms（会白送补偿，见 QUEST3_HEADTRACK §2） |
| 浏览器 → Node（同机/同网 WS） | 1~3 ms |
| Node → C5（UDP over WiFi） | 2~8 ms |
| C5 → F411（SPI 16 B） | <0.1 ms |
| F411 控制环 + ZDT | 2~3 ms |
| **合计（扣掉预测补偿后）** | **约 5~15 ms** |

比"Quest 直连 C5"多一跳，但对于 90 Hz、以姿势跟随为目标的云台完全够用。

---

## 6. 落地顺序（建议）

1. **先在 ControlAnything 里打点**：`console.log` 每帧的 yaw/pitch，确认数值合理（静止抖动 <0.05°）
2. 加 PoseTap + Node 转发，用 Wireshark 抓 UDP 确认帧率和 seq 连续
3. C5 侧收 UDP → SPI（或先把 UDP 收下来用串口打印，验证链路）
4. F411 侧 `esp_link` 改 16 B 结构体 → `frames` 开始涨 → 进入 TRACK
5. 最后再接 S 曲线/安全逻辑（已经写好了）

---

## 7. 与既有结论的衔接

- F411 端**不需要任何改动**除了协议长度（16 B）：`Esp_Tick()` 按 magic+CRC 滑窗同步，
  长度变了只改一个结构体常量
- 90 Hz 全帧发送的带宽结论不变：16 B × 90 = 1.4 kB/s，SPI 占用 0.02%
- 断连保护阈值（150 ms 冻结 / 1 s 停车）对方案 A 的抖动完全够
