# 两个 MCU 固件（例程 + 出厂配置）

这套系统的两个固件各自独立成工程，互相之间只靠 **SPI（30 字节帧）** 和
**一份协议定义** 耦合。下面是可以直接照抄的配置和烧录方式。

```
头显 ──wss──► ESP32-C5 ──SPI 1MHz──► STM32F411 ──UART──► 42ZDT ×2
                 │                        │
                 └────◄── MISO 遥测 ◄──────┘
```

---

## 1. ESP32-C5 · 射频桥

**目录**：`D:\project\esp32c5_test`

| 项 | 值 |
|---|---|
| 芯片 | ESP32-C5（RISC-V, 240 MHz） |
| IDF | v5.5.6 |
| Flash | 4 MB，单 app 大分区（0x1C0000） |
| 固件大小 | 约 1.01 MB（分区剩 45%） |

### 关键配置（`main/main.c` 顶部）

```c
#define BRIDGE_LINK_UDP        0     /* WiFi/UDP 关闭 */
#define BRIDGE_ENABLE_HTTP     1     /* 自带 HTTPS 页面 + WebSocket */
#define BRIDGE_ENABLE_BLE_TEST 0     /* BLE 关闭（与 WiFi 抢射频） */
#define BRIDGE_BAND            2     /* 2.4 GHz */
#define BRIDGE_WIFI_MODE       1     /* AP 模式 */
#define AP_SSID                "HT-TURRET"
#define AP_PASS                "turret1234"
#define AP_CHANNEL             6
```

### SPI 引脚（主机）

| 信号 | GPIO | 接到 F411 |
|---|---|---|
| SCK | 6 | PA5 |
| MOSI | 7 | PA7 |
| CS | 10 | PA4 |
| MISO | 4 | PA6 |
| GND | GND | GND |

`SPI_FREQ_HZ = 1000000`（1 MHz）、mode 0、**CS 前后各留 1 个时钟余量**：

```c
.cs_ena_pretrans  = 1,
.cs_ena_posttrans = 1,
```

### 传输长度是 31 字节，不是 30

```c
.length = 8 * 31,      /* 从机帧首会多一个残留字节，解析时按偏移跳过 */
```

`f411_parse()` 会在前 3 个字节里找 `A5 5A`（F411 统计帧的 magic 小端），
找到就按它对齐。**不要改成 30**，会整帧偏移。

### 页面托管

`web/xr_test.html` 由 `tools/build_web_assets.py` 打包成 `main/web_page.h`：

```bat
python tools\build_web_assets.py     :: 页面 + 证书 → .h
idf.py build
idf.py -p COM10 flash
```

页面地址：`https://192.168.4.1/xr`（自签证书，头显点一次「继续前往」）。

### 证书

`main/certs/server_cert.pem` + `server_key.pem`，生成脚本 `main/certs/gen.ps1`。
生成器会自动**补 NUL 结尾**（mbedTLS 要求）并**去掉 `EC PARAMETERS` 段**——
这两点是踩过的坑，改生成脚本时别丢。

---

## 2. STM32F411 · 云台控制

**目录**：`D:\project\stm32test`

| 项 | 值 |
|---|---|
| 芯片 | STM32F411xE，96 MHz |
| 工具链 | `arm-none-eabi-gcc 14.3.1`（STM32Cube 自带） |
| 固件大小 | 27.5 KB / 512 KB，RAM 3.8 KB / 128 KB |

### 外设分配（全占满，没有空闲串口）

| 外设 | 引脚 | 用途 | 波特率 |
|---|---|---|---|
| USART6 | PA11/PA12 | YAW 步进 | 115200 |
| USART1 | PA9/PA10 | PITCH 步进 | 115200 |
| USART2 | PA2/PA3 | 底盘 MG310 | 115200 |
| SPI1 | PA4/PA5/PA6/PA7 | C5（从机） | mode 0 |

> PA11/PA12 与 USB 复用，本工程**未启用 USB**，可以安全占用。

### SPI 从机实现（重要）

**不用 HAL 的 DMA**，改成逐字节中断：

```c
void SPI1_IRQHandler(void)
{
  while (SPI1->SR & SPI_SR_RXNE) {
    uint8_t b = *(volatile uint8_t *)&SPI1->DR;
    if (SPI1->SR & SPI_SR_TXE)                       /* 先补 TX，再干别的 */
      *(volatile uint8_t *)&SPI1->DR = s_tx[s_txidx];
    if (++s_txidx >= QP_LEN) s_txidx = 0u;
    ring_put(&b, 1u);
  }
  if (SPI1->SR & SPI_SR_OVR) { (void)SPI1->DR; (void)SPI1->SR; }
}
```

原因：从机模式下 HAL 的 `TxRxCplt` 回调实测不可靠（挂几次就停、NDTR 不归零），
DMA 缓冲指针不跟 CS 复位，会整帧偏移。1 MHz 下每字节 8 µs，ISR 必须**先补
DR** 再做缓冲和计数，否则同一个字节会被移出两次。

**同步字节是 `0x5A`**：

```c
if (b != (uint8_t)(QP_MAGIC & 0xFFu))   /* QP_MAGIC=0xA55A，小端首字节 0x5A */
```

### 上电时序

```
等 300ms → 使能两轴 → 等 200ms → 清零 → pitch 限位自校准 → 演示/跟随
```

限位自校准（`HOME_*` 宏）：

```
+2°/20ms 往外扫 → 撞到挡块（驱动器 0x22 或编码器连续 100ms 不动）
→ 退回 6° → 发 0x0A 把这里设成原点
```

正向 55° 没碰到挡块就换负向再扫；两边都没有就放弃，沿用上电位置。

### 轴方向

```c
#define YAW_SIGN     (+1.0f)
#define PITCH_SIGN   (-1.0f)    /* 实测俯仰反向 */
```

换电机、换齿轮箱、整机倒装时**只改这两个数**，不要动跟随公式。

### 烧录

```powershell
$env:PATH = "C:\Users\loveH\AppData\Local\stm32cube\bundles\gnu-tools-for-stm32\14.3.1+st.2\bin;" + $env:PATH
cd D:\project\stm32test
& "C:\Users\loveH\AppData\Local\stm32cube\bundles\ninja\1.13.2+st.1\bin\ninja.exe" -C build\Debug
& "C:\Users\loveH\AppData\Local\stm32cube\bundles\programmer\2.23.0\bin\STM32_Programmer_CLI.exe" `
  -c port=SWD mode=UR -w build\Debug\stm32test.elf -v -rst
```

SWD 只需三根：`SWDIO(PA13)` / `SWCLK(PA14)` / `GND`。**不要接 ST-Link 的 3.3V**，
会和外部电源打架。

---

## 3. 协议（两端唯一事实来源）

16 字节空口帧（浏览器 → C5）：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u16 | magic `0xA55A`（小端 `5A A5`） |
| 2 | u16 | seq |
| 4 | i16 | yaw ×100（0.01°） |
| 6 | i16 | pitch ×100 |
| 8 | i16 | yaw_rate ×10（0.1 dps） |
| 10 | i16 | 保留 |
| 12 | u16 | flags（0x01 VALID / 0x04 RECENTER / 0x08 ESTOP） |
| 14 | u16 | CRC16-CCITT（前 14 字节） |

30 字节控制帧（C5 → F411）和 30 字节遥测帧（F411 → C5）见
`stm32test/BSP/Inc/esp_link.h` 与 `esp32c5_test/main/main.c` 里的 `qp_head_t` /
`f411_stat_t`，两处字段顺序必须逐字节一致。

CRC 是 **CRC16-CCITT，poly 0x1021，init 0xFFFF**，两端实现相同。
