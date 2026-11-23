# 决策记录（Decision Log）

> 每条都写清"定了什么 / 为什么 / 影响哪些文件"。改主意时不要删旧条目，追加新的并标注取代关系。

---

## D-001 · 2026-11-15 · 最终形态只做「头追 + 图传」，放弃一切需要相机的功能

**决定**

Quest 端最终只保留两件事：

1. **头姿上报**（16 B 帧 → C5 → F411）
2. **FPV 图传显示**（UVC 采集卡画面铺在视野里）

**放弃**

- ControlAnything 的 SAM 3.1 点选 / 物体分割 / 生成式控件
- Meta Passthrough Camera API（透视相机取帧）
- 任何"看懂环境"的视觉功能

**理由**

- 云台要的是**头姿**，不是相机。SAM 那套是为了"用手指点真实设备建控制卡片"，
  和云台瞄准没有关系。
- 去掉相机后，最大的政策/兼容风险（Passthrough Camera API 的 v74+ 限制、
  Device User Data 合规、必须真机、必须 passthrough 应用）**全部消失**。
- APK 里只剩 UVC（标准 Android `CAMERA` 权限 + USB Host），没有特殊权限。

**影响**

| 受到影响的东西 | 变成 |
|---|---|
| `docs/APK_FEASIBILITY.md` §3 | 相机权限分析降级为"历史备查"，不再是阻塞项 |
| APK 技术栈 | Unity + Meta XR SDK 基础 VR 场景，**不需要 MRUK / PCA / passthrough** |
| 权限清单 | `CAMERA`（UVC 用）+ `USB Host`，无特殊 Meta 权限 |
| 风险表 | 删掉"Camera2 外部相机可用性"里的 passthrough 部分；保留采集卡兼容性与热降频 |
| 工作量 | 3.5~4 人日 → **3~3.5 人日**（少掉 SAM 相关的联调） |
| 现有代码 | 无改动。`bridge.js` / `hud.js` / 16 B 帧 / C5 / F411 全部照旧 |

**仍然成立的前提**

- 图传仍必须走 **UVC 采集卡**（浏览器拿不到 UVC，所以最终形态是 APK 而不是网页）
- 头显只有一个前台应用 → **UVC 视频和网页二选一**，这也是要出 APK 的原因

---

## D-002 · 2026-11-15 · 现阶段只做分部件调试，不写 APK

**决定**：按 `docs/APK_FEASIBILITY.md` 第二部分的 6 段矩阵逐段验证，
APK 等协议在真机上跑通后再写。

**理由**：APK 会把"视频 + 姿态"的形态锁死，而这俩链路现在都还没上机验证过。

---

## D-003 · 2026-11-15 · 头显端支持 standalone 模式

**决定**：`Link.Mode = standalone` 时头显直连 C5，不经过 PC，也没有图传。

**理由**：便于单独调头追，不被 PC 链路拖住。细节见 `docs/STANDALONE_MODE.md`。

---

## D-004 · 2026-11-21 · C5 只跑 BLE，WiFi 全关

**决定**：ESP32-C5 固件默认 `BRIDGE_LINK_UDP = 0`、`BRIDGE_ENABLE_HTTP = 0`，
只做 BLE 外设 + SPI 转发。网页改由 PC 托管（adb reverse + `http://127.0.0.1:8080`）。

**依据（实测）**

| 配置 | 能否扫到 | RTT p50 | 抖动 | 最大 |
|---|---|---|---|---|
| WiFi 5G AP + BLE | ❌ | — | — | — |
| WiFi 2.4G AP + BLE | ❌ | — | — | — |
| **纯 BLE** | ✅ | **14.93 ms** | **0.33 ms** | **15.97 ms** |

WiFi 一开，BLE 广播就抢不到空口——日志说"广播中"，但空中什么都没有。
换天线无效，去掉 WiFi 立刻正常，所以是射频共存问题，不是天线或代码问题。

**影响**

- C5 不再提供热点/网页；`HT-TURRET` AP 与 `https://192.168.4.1/xr` 停用
- 头显页面走 PC：`npx serve apps/quest -l 8080` + `adb reverse tcp:8080 tcp:8080`
- 以后做 APK 时，页面内嵌，连 PC 都不需要
- UDP / ESP-NOW 代码保留在固件里，只是默认关闭（改两个宏即可恢复）

**未解决的**：C5 若将来要同时提供网页和 BLE，需要换方案（双芯片、或 PC 常驻）。
