# APK 最终形态的可行性 + 现在的分部件调试方案

> **2026-11-15 决策（D-001）**：最终形态只做「头追 + 图传」，放弃一切需要相机的功能
> （SAM 点选 / Passthrough Camera API）。本文 §三 的相机权限分析因此降级为历史备查，
> 不再是阻塞项。见 [DECISIONS.md](DECISIONS.md)。
>
> 上半部分：把整机做成一个脱离 PC 的 APK，技术上行不行、代价多大（**现在不做**）。
> 下半部分：在 APK 之前，怎么把每一段单独验通（**现在就能做**）。

---

# 一、APK 最终形态可行性

## 1.1 结论先说

**可行，而且比 WebXR 方案更适合当最终形态**——因为 APK 能同时做到
"UVC 看画面" + "读头姿"，而这恰恰是浏览器做不到的那件事（见 `QUEST_DOCK_LIMITS.md` §3.①）。

代价是**换一整套渲染/交互层**（WebXR → Unity/OpenXR），
但**下游（C5 → SPI → F411）一行都不用改**，因为 16 B 帧格式已经定死了。

## 1.2 APK 相比现在的 WebXR 方案强在哪

| 能力 | WebXR 页面 | 原生 APK |
|---|---|---|
| 读头姿 | `getViewerPose`（每帧，预测值） | `OVRPlugin.GetNodePose` + **角速度** |
| 显示 UVC 画面 | ❌ 浏览器拿不到 | ✅ Camera2 `LENS_FACING_EXTERNAL` |
| 传输 | WebSocket（还要 PC 中转） | **裸 UDP 直发 C5** |
| 端到端延迟 | 60~150 ms（IP 视频） | **40~75 ms**（UVC 直采） |
| 后台能力 | 切应用即挂起 | 前台 Service 可维持（受限但存在） |
| 部署 | 开网页 | 装 APK（sideload） |

## 1.3 要换什么、不用换什么

**要换（新写）**

| 层 | 现在 | APK 版 |
|---|---|---|
| 渲染 | three.js + WebXR | Unity 2022 LTS + Meta XR SDK（或原生 OpenXR NDK） |
| 姿态源 | `XRFrame.getViewerPose` | `OVRPlugin.GetNodePose(EyeCenter, Step.Render)` |
| 视频 | PC 采集 → WebRTC → `<video>` | UVC 采集卡 → Camera2 → Texture → 平面 |
| 传输 | WebSocket → PC → UDP | Android `DatagramSocket` → C5 |
| HUD | `hud.js`（CanvasTexture） | Unity Canvas / TextMeshPro 贴在同一块平面 |

**不用换（直接复用）**

- 16 B 帧格式（magic / seq / yaw / pitch / rate / flags / crc）
- 坐标系与符号约定（`yaw=atan2(-f.x,-f.z)`）
- C5 固件（收 UDP → SPI）
- F411 全套（协议解析、S 曲线、断线保护、停靠）
- `quest/bridge.js` 里除 `start()` 以外的逻辑（重连策略、限速、recenter）

> 换句话说：**换的是"取样那一层"，下游一行不改**。

## 1.4 技术风险（按严重度）

| # | 风险 | 说明 | 缓解 |
|---|---|---|---|
| 1 | **采集卡兼容性** | 必须是标准 UVC；MJPG 1080p60 最稳，YUY2 会吃满带宽 | 先用手上这块 UGREEN 实测 |
| 2 | **UVC 采集卡在 APK 里可用性** | 标准 Android `CAMERA` + USB Host；官方 HDMI Link 已证明可行 | 先用样例 APK 枚举设备 |
| 3 | **热降频** | 1080p60 解码 + 渲染 + WiFi ≈ 12~15 W，Quest 3 约 20~30 min 后降频 | 720p60 兜底；加散热；比赛分段 |
| 4 | **USB 单口** | UVC 占了口，充电/有线网要挂坞 | 65 W PD 坞，1080p60 + 百兆网 |
| 5 | **构建分发** | 需要开发者模式 + 签名 APK；上架要走 Meta 审核 | 比赛用 sideload 即可 |
| 6 | **权限** | 只需常规 `CAMERA` + USB Host；**不涉及** Passthrough Camera API 的特殊政策 | 声明即可，无审批 |

## 1.5 工作量估算

| 任务 | 人日 |
|---|---|
| Unity 工程 + Meta XR SDK 起架子 + 串流平面 | 0.5 |
| UVC 接入（Camera2 external → Texture） | 1.0 ~ 1.5 |
| 姿态取样 + UDP 发送（复用 16 B 帧） | 0.5 |
| HUD + 设置面板（recenter / 急停 / 限速） | 0.5 |
| 联调 + 热/延迟测试 | 1.0 |
| **合计** | **3 ~ 3.5 人日**（D-001 去掉 SAM 相关后） |

## 1.6 建议的推进节奏

```
现在          分部件调试（见下方第二部分）        ← 你在这一步
阶段一        头显端页面在 standalone 模式跑通
阶段二        C5 固件：UDP + SPI 打通
阶段三        F411 上机：空载 → 带载
阶段四        才开始写 APK（把已验证的姿势/协议搬进去）
```

**先不要在 APK 上花时间**——APK 会锁死"视频+姿态"的形态，
而这两条链路现在都还没在真机上验过一次。先验协议，后写壳。

---

# 二、现在的分部件调试方案

## 2.1 拆成 6 段，每段独立可验

| # | 段 | 怎么单独验 | 需要什么 | 判据 |
|---|---|---|---|---|
| 1 | **视频链路** | `HeadTracker.cmd` → 启动 → 浏览器开 `http://127.0.0.1:8889/cam` | 采集卡 + MediaMTX | 有画面、延迟 <150 ms |
| 2 | **头显页面** | standalone 模式，连任意 WS 服务 | 头显 + WiFi | HUD 出字、无报错 |
| 3 | **pose 上行** | `tools\mockpose.ps1` 假造头姿 | 无需头显 | C5 收到 16 B 帧 |
| 4 | **C5 转发** | C5 收 UDP → SPI | 逻辑分析仪/示波器 | SPI 上有 16 B 突发 |
| 5 | **F411 解析** | ST-Link Live Watch `Esp_Stats()->frames` | 调试器 | frames 涨、crc_err=0 |
| 6 | **执行机构** | F411 演示 → 停靠 | 电源 + 电机 | 转到 HOME 停住 |

**关键：3 号工具（mockpose）让"没有头显也能验完 C5→F411→电机"。**

## 2.2 mockpose.ps1 用法

```powershell
# 静止 0°，验零漂/配平
.\tools\mockpose.ps1 -Still

# ±60° 正弦，验跟随平滑度
.\tools\mockpose.ps1 -Target 192.168.4.1

# ±30° 阶跃，验方向对不对、有没有过冲
.\tools\mockpose.ps1 -Target 192.168.4.1 -Step 30

# 带急停标志，验停车保护
.\tools\mockpose.ps1 -Target 192.168.4.1 -EStop

# 90 Hz 跑 30 秒，数丢帧
.\tools\mockpose.ps1 -Rate 90 -Seconds 30
```

帧格式与 `quest/bridge.js` **完全一致**（magic 0xA55A / 16 B / CRC16-CCITT），
所以它替换掉的正是"头显"这一段，其余链路原样参与。

## 2.3 已经验证过的

| 项 | 结果 |
|---|---|
| mockpose 发的字节 | 16 B、magic `0xA55A`、seq、flags `0x01`、yaw 正常 |
| CRC16 实现 | 与 `bridge.js`、F411 `esp_link.c` 同一套参数 |
| 视频链路命令 | ffmpeg + NVENC + MediaMTX 配置就绪（未装 MediaMTX） |

## 2.4 建议的调试顺序

1. **先 1 号**（视频）——完全不碰头显和单片机，最容易出结果
2. **再 5/6 号**（F411 + 电机）——用调试器直接喂 `Esp_Stats()`，确认解析和运动
3. **然后 3 号**（mockpose）——把 1、5、6 串起来，此时链路上没有任何"未验证的部件"
4. **最后 2 号**（头显页面）——它只负责"采样 + 发帧"，前面都通了它就没有未知数

这个顺序的好处：**每一步失败都能一眼定位**，不会出现"整套不通、不知道哪一段"的局面。

---

# 三、APK 到底能不能拿到 ControlAnything 需要的相机权限？

**先说结论：权限能拿到，但"ControlAnything 本身"拿不到。别把这两件事混在一起。**

## 3.1 先分清两个"相机"

| 用途 | 谁需要 | 现在怎么拿 | APK 里怎么拿 |
|---|---|---|---|
| **点选真实物体**（SAM 3.1 分割） | ControlAnything 的 UI | 浏览器 WebXR `camera-access` | Meta Passthrough Camera API |
| **看 FPV 图传** | 炮塔操作手 | PC 采集卡 → IP → `<video>` | UVC 采集卡（Camera2 external） |
| **头姿** | 云台 | `getViewerPose` | `OVRPlugin.GetNodePose` |

你的云台项目**只需要后两个**。第一个（passthrough 相机）纯粹是 ControlAnything 用来
"把手指指向的设备建个卡片"的交互手段——**不做这个功能，云台照样工作**。

## 3.2 Meta 的 Passthrough Camera API：能用，但有条件

| 条件 | 说明 |
|---|---|
| **Horizon OS ≥ v74** | Camera2 支持从 v74 起；v83 起多一个 1280×1280 档位 |
| **应用必须是 passthrough 应用** | 纯 VR 应用拿不到；要开透视 |
| **OVR Manager → Enabled Passthrough Camera Access** | 在 Camera Rig 上勾选 |
| **加 `PassthroughCameraAccess` 组件**（MRUK） | 然后 `WebCamTexture` 就能读到帧 |
| **必须真机** | XR Simulator 不支持，没法在编辑器里跑 |
| **数据属性** | 相机帧被归为 **Device User Data**，外传要遵守 Meta 的开发者数据政策 |

**开发者模式不影响这个**——开发者模式只决定能不能 sideload / adb，跟相机权限无关。

结论：**APK 拿 passthrough 帧在技术上成立**（官方 sample `Unity-PassthroughCameraApiSamples` 就是干这个的）。

## 3.3 但"把 ControlAnything 塞进 APK"这条路不通

ControlAnything 是**浏览器里的 WebXR 页面**（three.js + `immersive-ar`）。
把它装进 APK 只有两种做法，都不成立：

| 做法 | 结果 |
|---|---|
| 用 WebView 包一层 | ❌ **WebView 不支持 immersive WebXR**，`navigator.xr` 拿不到沉浸会话，页面根本进不了 AR 模式 |
| 用 Unity 重写 | ✅ 能跑，但那就**不是 ControlAnything 了**——SAM 推理、agent 交互、控件生成全要重做 |

所以"APK 拿相机" ≠ "APK 跑 ControlAnything"。

## 3.4 三条路，按时髦程度排序

| 路线 | 有没有 PC | 有没有 ControlAnything | 相机 | 工作量 |
|---|---|---|---|---|
| **A. standalone WebXR**（页面托管到 C5） | ❌ 无 PC | ✅ 原样保留 | 浏览器 `camera-access` | 小（已做 pack） |
| **B. 自己写 Unity APK** | ❌ 无 PC | ❌ 只有头追+图传 | PCA + UVC | 3.5~4 人日 |
| **C. 现在的 PC 联动** | ✅ 有 PC | ✅ | 浏览器 | 已完成 |

**要"脱离 PC 又保留 ControlAnything"，选 A，不是 B。**
A 的唯一门槛是 C5 得用 HTTPS 提供页面（见 `STANDALONE_MODE.md` §2.3）。

**要"最快、最低延迟、不要 ControlAnything 那些花活"，选 B。**

## 3.5 给你的建议

1. 先确认你**到底要不要 SAM 点选**这个功能。云台要的是头姿，不是相机。
2. 不要 → 直接走 A 或 B，相机权限的事根本不用管。
3. 要 → 只能留在 WebXR（A 或 C），因为 Unity 重写等于放弃 ControlAnything 的全部交互。
4. 无论哪条，**先把手上的分部件调试做完**（本文第二部分），别在权限上纠结——那是最后一步的最后一公里。
