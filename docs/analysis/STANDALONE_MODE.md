# 两种运行模式：PC 联动 / 头显独立

`config.psd1` 里 `Link.Mode` 一个开关切换。**同一个页面、同一套客户端代码**，只是数据出口不同。

| | `pc` | `standalone` |
|---|---|---|
| 页面由谁提供 | PC（adb reverse → `127.0.0.1`） | **C5** |
| 头显要不要接线 | 要（USB） | **不要** |
| 头姿发给谁 | PC 的 Node → UDP → C5 | **直连 C5 的 WebSocket** |
| 图传 | 有（PC 采集卡 → WebRTC） | **无** |
| 适合 | 整机联调、比赛 | **只调头姿 / 单独验页面** |

---

## 1. 为什么"独立模式"不能有图传

图传链路是 `VRX → HDMI → 采集卡 → PC → 编码 → 头显`，**采集卡在 PC 上**。
头显独立时没有 PC，也就没有采集、没有编码，自然没有画面。

所以独立模式的能力边界：

- ✅ 头姿采样、坐标系转换、HUD、recenter、急停标志、WS 上报、断连重连
- ❌ 视频平面（页面上留一块灰色占位）
- ❌ C5 转发（直接连 C5，不经过 PC）

这对"单独调试头追"完全够用——你要验的是**姿态数值对不对、丢不丢帧、断连保不保护**。

---

## 2. 独立模式怎么跑起来

### 2.1 打独立包（PC 上做一次）

```bat
HeadTracker.cmd cli pack
```

生成 `app\out\standalone\`：

```
bridge.js     客户端
hud.js        左下角小字 HUD
mode.json     { mode: standalone, wsUrl: ws://192.168.4.1/ws, video: false }
```

### 2.2 把包塞进 C5

两条路，看你的固件形态：

| 做法 | 说明 |
|---|---|
| SPIFFS / LittleFS 分区 | 把目录内容烧进文件系统，`httpd` 用 `httpd_resp_set_type` 直接吐文件 |
| 编译进固件（embed） | `EMBED_FILES` 把 js 打进 bin，省分区 |

C5 需要提供两个端点：

```
GET  /                 → index.html + bridge.js + hud.js + mode.json
GET  /ws               → WebSocket，收 16 字节 pose 帧
```

### 2.3 ⚠️ 必须 HTTPS，否则 WebXR 不给你

浏览器只在**安全上下文**下暴露 `navigator.xr`：

| 页面地址 | WebXR |
|---|---|
| `http://192.168.4.1/` | ❌ 不可用 |
| `https://192.168.4.1/`（自签，手动信任） | ✅ 可用 |
| `http://127.0.0.1/`（localhost） | ✅ 可用（但需要 PC 端口映射） |

所以 C5 侧要跑 **TLS + 自签证书**：

1. C5 的 httpd 开 TLS（ESP-IDF `httpd_ssl_start`），证书用自签
2. Quest 浏览器首次访问会警告"不安全" → 点「高级 → 继续前往」
3. 之后该 origin 被视为安全上下文，`navigator.xr` 可用

> 这一步没法绕。要么 C5 出 HTTPS，要么老老实实接线走 PC。

### 2.4 头显操作

1. 头显连 C5 的 WiFi AP
2. 浏览器打开 `https://192.168.4.1/`，忽略证书警告
3. 页面右上角「设置」浮层里确认：模式 = standalone、WS = `wss://192.168.4.1/ws`
   （HTTPS 页面里必须用 **wss**，否则被混合内容拦掉）
4. 开始调试

---

## 3. 客户端怎么判断用哪种模式

`bridge.js` 的优先级：

```
URL 参数  >  localStorage  >  同目录 mode.json  >  默认值
?mode=standalone&ws=wss://192.168.4.1/ws
```

所以同一份代码：

- PC 模式：PC 的页面没有 `mode.json`（或写了 `mode: pc`），走 `ws://127.0.0.1:8080/ws`
- 独立模式：C5 上的 `mode.json` 写着 `standalone`，走 `wss://192.168.4.1/ws`
- 临时覆盖：加个 URL 参数就行，不用改文件

设置浮层里改过之后写进 `localStorage`，下次打开记住。

---

## 4. PC 端在独立模式下的行为

`Link.Mode = standalone` 时：

- `start` **不启动** ffmpeg / MediaMTX / adb reverse，只打印一句指引
- `status` / `doctor` 照常可用（doctor 会提示 C5 连不通，因为此时 PC 不在这个网里）
- `pack` 照常可用（就是用来出独立包的）

这样"开着 PC 但不参与"和"关掉 PC"行为一致，不会有幽灵进程。

---

## 5. 调试清单（独立模式）

| 检查 | 期望 |
|---|---|
| 页面能开（忽略证书） | HUD 出现 |
| `navigator.xr` | 不为 undefined |
| WS 状态 | HUD 第一行变绿 `● ok` |
| 头不动 10 s | yaw/pitch 抖动 < 0.05° |
| 快速甩头 | 数值跟手，无跳变 |
| 断 WiFi 3 s | 云台冻结（F411 侧 150 ms 判定），恢复后自动重连 |
| 按回中 | 当前朝向变成 0°，云台平滑回到 0 |

---

## 6. 与 PC 模式的切换成本

零成本——**改一个下拉框**（GUI 的「运行模式」）或改 `config.psd1` 一行。
两边共用 `bridge.js` / `hud.js`，不存在"两套代码要同步"的问题。
