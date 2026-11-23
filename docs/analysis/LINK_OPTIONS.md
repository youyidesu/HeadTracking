# 链路选型：5G UDP / ESP-NOW / 控制端有线 C5

> 固件已经**同时支持** UDP 和 ESP-NOW，改 `main.c` 顶部两个宏即可切换。
> 本文说明两者能干什么、不能干什么，以及控制端加一块 C5 怎么摆。

---

## 1. 三个方案

| | A. UDP over 5G WiFi | B. ESP-NOW（控制端 C5 有线接 PC） | C. 运行时切换 |
|---|---|---|---|
| 拓扑 | Quest/PC → AP → 车载 C5 | PC ─USB─ 控制端 C5 ─2.4G─ 车载 C5 | 两套都编进去 |
| 延迟 | 3~15 ms（含 AP 排队） | **2~5 ms**（无 AP、无 IP 栈） | — |
| 距离 | 30~80 m（视 AP） | 100~200 m（视天线） | — |
| 频谱 | 5 GHz | **可切 5 GHz / 2.4 GHz** | — |
| 依赖 | 需要 AP（或 C5 自己开热点） | 免 AP | — |
| 头显能否直连 | ✅（C5 开 AP，5G） | ❌（头显没有 ESP-NOW） | — |

---

## 2. 两条硬约束（必须先知道）

### 2.1 C5 的 5G 只支持 20 MHz 带宽

ESP32-C5 是双频 WiFi 6，但 **5 GHz 只跑 20 MHz 信道**。
如果现场 AP 把 5G 设成 40/80 MHz，C5 会连不上或配网失败。

**做法**：把 AP 的 5G 锁死在 20 MHz（或干脆用 C5 自己开 AP，默认就是 20 MHz）。

### 2.2 ESP-NOW 可以跑 5 GHz（C5 专属能力）

**更正**：ESP-NOW 协议本身没有"频段"参数，它跟着 WiFi 射频走。
C5 是双频芯片，用 `esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)` 把射频切到 5G，
再用 `esp_wifi_set_channel(36, ...)` 定信道，**ESP-NOW 就在 5 GHz 上收发**。

IDF v5.5.6 里的证据：

```c
esp_wifi.h:1771   esp_err_t esp_wifi_set_band_mode(wifi_band_mode_t band_mode);
esp_wifi_types.h  WIFI_BAND_MODE_5G_ONLY = 2,
esp_now.h         uint8_t channel;   /* 0 = 跟随当前，36~165 都放得下 */
```

所以三条路可以并存（仍然同一射频，**同时只能一种**）：
- ESP-NOW @ 2.4G：免 AP，抗图传但不能抗遥控
- **ESP-NOW @ 5G**：免 AP + 抗遥控 + 避开 5.8G（选 ch36-48）← 最理想
- UDP @ 5G：需要 AP，但可以多设备

> ⚠️ 待实测：`libespnow.a` 是否真的接受 5G 信道，API 层没有禁止，但要两块 C5 对打验证。

---

## 3. 控制端放一块 C5 的价值

```
PC ──USB(CDC-ACM 921600)──► 控制端 C5 ──ESP-NOW──► 车载 C5 ──SPI──► F411
```

| 好处 | 说明 |
|---|---|
| **PC 不需要 WiFi** | 有线接 C5，桌面上不用配网络，也不会被现场 AP 干扰 |
| **只剩一跳无线** | 抖动只存在于 ESP-NOW 段，比"Quest→AP→PC→AP→C5"少两跳 |
| **可做双角色** | 控制端 C5 同时开 AP 给头显连（standalone 模式），一机两用 |
| **带宽够** | 90 Hz × 16 B = **1.4 kB/s**，921600 串口占用 1.5% |

USB 侧用 ESP-IDF 的 TinyUSB CDC-ACM 即可，PC 上就是一个 COM 口。

---

## 4. ESP-NOW 的几个实现要点

| 项 | 建议 |
|---|---|
| 包长 | ≤250 B；现在是 16 B，**可以批量打包**（一次 15 帧 = 240 B）减少发送次数 |
| 配对 | 调试用**广播**（免配对）；正式用**配对 + LMK 加密**，防止收到别人的帧 |
| 重传 | 默认无重传；丢帧由 F411 的 150 ms 冻结 / 1 s 停车兜底 |
| 信道 | 必须和接收端一致（`peer.channel = 0` 表示跟随当前） |
| 与遥控共存 | 走 2.4G 时避开遥控常用信道；若冲突严重，切回 5G UDP |

---

## 5. 推荐

| 场景 | 用哪个 |
|---|---|
| **台架调试 / 比赛（操作手在 PC 边）** | **B：ESP-NOW** —— 延迟最低、不依赖现场 AP |
| 需要头显直连（standalone） | **A：5G UDP**（C5 开 AP） |
| 现场 AP 可靠、且要连多台设备 | A |
| 2.4G 遥控干扰严重 | A（5G 避开） |

固件切换方式：

```c
#define BRIDGE_LINK_UDP     1     /* 5G/2.4G WiFi，走 IP */
#define BRIDGE_LINK_ESPNOW  0     /* 2.4G 点对点 */
```

两个都编译通过（实测：UDP 版 940 KB，ESP-NOW 版 935 KB）。

---

## 6. 已加进固件的东西

| 改动 | 目的 |
|---|---|
| `esp_wifi_set_ps(WIFI_PS_NONE)` | 关省电，否则延迟从几 ms 抖到几十 ms |
| 收包逻辑抽成 `handle_frame()` | UDP 与 ESP-NOW 共用同一条解析/重打包路径 |
| 统计每秒打印 `收/转发/crc错/长度错/SPI错` | 分段调试时看这一行就知道卡在哪 |
| 16 B → 30 B 重打包 | F411 侧（已冻结）不用改 |

---

## 7. 还没做、但值得排进计划的

1. **控制端 C5 固件**：USB CDC 收 PC 的 16 B → ESP-NOW 转发（车载侧已有接收）
2. **批量打包**：ESP-NOW 一次发多帧，降低协议开销
3. **链路质量回报**：车载 C5 把 RSSI/丢包率塞进遥测，PC 侧显示（可以画在 HUD 上）
4. **自动切换**（方案 C）：ESP-NOW 连续丢帧 → 切 5G UDP，稳定后再切回
