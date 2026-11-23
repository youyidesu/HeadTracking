# 从零点亮：分步流程

按这个顺序调，每一步都有明确判据，出错能立刻定位在哪一层。

---

## 第 1 步：F411 单独跑通（不接 C5）

**烧录**

```bash
STM32_Programmer_CLI -c port=SWD -w release/f411/stm32test.elf -rst
```

**期望现象**

```
1. 电机使能（手拧有阻力）
2. 清零（静止）
3. pitch 缓慢向一侧转，每次 2°，最多 30°
4. 撞到挡块 → 回退 6°
5. 再往反方向退 18° → 停住   ← 这是"正角度"
6. 进入演示摆动
```

**判据**

| 现象 | 结论 |
|---|---|
| 完整走了 3~6 | ✅ 校准正常 |
| 一点不动 | 电机没电 / 驱动没上电 |
| 转一下马上停 | 编码器没反馈（或被判堵转） |
| 一直转不停 | 限位没接 / 编码器没反馈 |

---

## 第 2 步：C5 与 F411 的 SPI

**C5 烧 WiFi 或 BLE 固件都行**，只看串口打印：

```
I bridge: SPI2 主机 1.0 MHz, mode0, CS=GPIO10
I bridge: F411 DEMO | 收 36 帧 | 丢 12 | 应答 61/0 | pos 0.4° / 0.0°
```

**判据**

| 现象 | 原因 |
|---|---|
| `收 0 帧` | **magic 字节序错**（必须 `5A A5`）或 SPI 没接 |
| `收 N 帧` 但 `pos` 乱跳 | 传输长度不对（应为 31 B）或结构体对齐不一致 |
| `mode 0x08` | 校准跑完但没撞到限位 → 电机侧问题 |

---

## 第 3 步：头显 → C5

**BLE 模式**（`HT_MODE 2`）：

```
I ble_test: 广播中：C5-BLE-TEST
I ble_test: 已连接 (handle=0)，初始间隔 7.50 ms
I bridge: 收 90/s | 转发 90/s
```

**判据**：`收/转发` 都是 90/s 左右就通了。

---

## 第 4 步：云台跟随

头显转头，云台跟着转。

| 现象 | 改哪里 |
|---|---|
| 方向反 | App 的 `yawinv=1`（或在 F411 改 `YAW_SIGN=-1.0f`） |
| 转过大角度绕一整圈 | F411 必须用**连续累计角**（`wrap180` 累加），不能用绝对角 |
| 跟不上 / 抖动 | 提高发送频率（`sendHz`）或检查 BLE 连接间隔 |

---

## 第 5 步：App 界面

- 能看见面板、有准星 → 渲染 OK
- 准星压到按钮变绿、扣扳机有 `[App] 点击 xxx` → 输入 OK
- 背景能看到现实 → 透视 OK

**判据**：`adb pull /sdcard/Android/data/com.ht.turret/files/app.log` 里有：

```
[App] 图传源: 有线(adb reverse)
[App] MJPEG(TCP) connected=True frames=… soi=… loadFail=0
```

---

## 第 6 步：图传

```powershell
# PC 端
cd web
python devserver.py 8080

# 头显侧隧道
adb reverse tcp:8080 tcp:8080
```

配置里：

```properties
vsrc=2
```

**判据**：`frames` 每秒涨 30+，画面出现。

| 现象 | 原因 |
|---|---|
| `connected=False` | 隧道没建（重插 USB 后要重跑 `adb reverse`） |
| `bytes=0` | 服务端没帧（采集卡没信号 / 输入格式错） |
| `soi` 涨但 `frames=0` | 解码失败（JPEG 不完整） |
| 服务端 `No JPEG data found` | 采集卡输入格式要 `yuyv422`，不是 `mjpeg` |
