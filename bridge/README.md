# HeadTracking.Bridge

把 **Quest 3 的头姿** 送到 **STM32F411 云台**，可走三条链路（WiFi / ESP-NOW / BLE），
可选旁路 **FPV 图传**。结构参照 ControlAnything 的 monorepo 约定：`apps/*` + `packages/*` + `firmware/*`。

## 目录

```
HeadTracking.Bridge/
├─ apps/
│  ├─ quest/            Quest 3 WebXR 端（取头姿、打包 16 B、HUD）
│  ├─ relay/            PC 中转：WebSocket → UDP → C5
│  └─ console/          PC 桌面控制台（PowerShell/WPF，见 legacy/）
├─ packages/
│  └─ protocol/         16 B 空口帧 + 30 B F411 帧：编解码、CRC、JSON Schema、单测
├─ firmware/            占位：固件开发期仍在 ../esp32c5_test，收尾再迁入
├─ tools/               mockpose.ps1（假头姿源）、ble_latency.py（BLE 延迟测试）
├─ scripts/             构建辅助
└─ docs/                分析文档（链路选型、RF、APK 可行性、决策记录…）
```

## 快速开始

```bash
npm install          # 只跑 Node 部分时装
npm test             # 协议单测（6 项）

npm run dev -w @ht/relay    # PC 中转：Quest → WebSocket → UDP → C5
npm run console             # PC 桌面控制台（图传桥接 + 自检）
```

固件（**开发期不在本仓库内**）：

```bat
cd ..\esp32c5_test
flash.cmd COM11        :: 烧录 + 开串口
```

## 三条链路

| 链路 | 走法 | 单程延迟 | 适合 |
|---|---|---|---|
| **BLE 7.5 ms** | Quest 浏览器 Web Bluetooth → C5 | **7.5 ms**（抖动 ±1.8 ms，实测） | 无 AP、抗干扰最稳 |
| **UDP over WiFi** | C5 自己开 5 GHz AP | 3~15 ms（拥挤时抖动大） | 多设备、要图传 |
| **ESP-NOW** | 控制端 C5（有线接 PC）→ 空中 → 车载 C5 | 2~5 ms | 免 AP、点对点 |

细节：[docs/LINK_OPTIONS.md](docs/LINK_OPTIONS.md)、[docs/WIFI_VS_BLE.md](docs/WIFI_VS_BLE.md)

## 协议一览

```
Quest/PC ──16 B──► C5 ──30 B──► F411
magic/seq/yaw/pitch/rate/flags/crc     magic/ver/flags/seq/t_us/q×4/yaw/pitch/rate/crc
```

两套帧的唯一定义在 [packages/protocol/src/index.js](packages/protocol/src/index.js)，
JSON Schema 在 [packages/protocol/json-schema/](packages/protocol/json-schema)。

C5 固件里那份 C 实现（`../esp32c5_test/main/main.c` 的 `wire16_t` / `qp_head_t`）必须与它逐字节一致。

## 文档

| 文件 | 内容 |
|---|---|
| [docs/DECISIONS.md](docs/DECISIONS.md) | 决策记录（只做头追+图传、standalone、AP 模式…） |
| [docs/LINK_OPTIONS.md](docs/LINK_OPTIONS.md) | 5G UDP / ESP-NOW / 控制端有线 C5 |
| [docs/RF_5G_ANALYSIS.md](docs/RF_5G_ANALYSIS.md) | 赛场 5 GHz 抗干扰分析、信道规划 |
| [docs/WIFI_VS_BLE.md](docs/WIFI_VS_BLE.md) | WiFi 与 BLE 逐项对比 |
| [docs/STANDALONE_MODE.md](docs/STANDALONE_MODE.md) | 头显独立模式 |
| [docs/APK_FEASIBILITY.md](docs/APK_FEASIBILITY.md) | APK 最终形态 + 分部件调试矩阵 |
| [docs/PC_VIDEO_BRIDGE.md](docs/PC_VIDEO_BRIDGE.md) | 图传：采集卡 → 编码 → 浏览器 |
| [docs/QUEST_DOCK_LIMITS.md](docs/QUEST_DOCK_LIMITS.md) | 拓展坞能/不能解决什么 |
| [docs/QUEST3_HEADTRACK.md](docs/QUEST3_HEADTRACK.md) | Quest 头姿取数的十条坑 |
| [docs/CONTROLANYTHING_POSE.md](docs/CONTROLANYTHING_POSE.md) | ControlAnything 取数路径 |
| [docs/INTEGRATION_VIDEO_POSE.md](docs/INTEGRATION_VIDEO_POSE.md) | 图传与头姿合流 |

## 硬件

| 件 | 型号 |
|---|---|
| 头显 | Meta Quest 3 |
| 桥接 | ESP32-C5（WiFi 6 双频 + BLE 5 + 802.15.4） |
| 主控 | STM32F411CEU6 |
| 电机 | 42ZDT28A（YAW）/ 42ZDT40A（PITCH） |

MCU 侧固件与设计文档在仓库外：`../stm32test/`。
