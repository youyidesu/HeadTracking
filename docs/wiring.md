# 接线与供电

## 总览

```
       ┌──────────────┐        SPI         ┌──────────────┐
       │  ESP32-C5    │◄──────────────────►│  STM32F411   │
       │  (桥接)      │  CS/SCK/MOSI/MISO  │  (云台控制)  │
       └──────┬───────┘                    └──────┬───────┘
              │ BLE / WiFi                        │ UART
              │                                   ↓
        Quest 3                            42ZDT 闭环步进 ×2
```

## SPI（C5 主机 → F411 从机）

| 信号 | C5 引脚 | 说明 |
|---|---|---|
| CS   | GPIO10 | 片选，低有效 |
| SCK  | GPIO6  | 1 MHz，mode 0 |
| MOSI | GPIO7  | C5 → F411（姿态帧） |
| MISO | GPIO4  | F411 → C5（遥测帧） |

**注意**：C5 侧每次传输 **31 字节**（从机帧首会多一个残留字节），解析时按偏移找 magic `A5 5A`。

## 步进驱动（F411 → 42ZDT）

| 轴 | 说明 |
|---|---|
| yaw | 无限位，可连续旋转 |
| pitch | **带机械挡块**，上电自动校准 |

42ZDT 走 UART，默认 115200 8N1，地址 `YAW_ADDR` / `PITCH_ADDR`（见 `firmware/f411/BSP/Inc/zdt_port.h`）。

## 供电

| 设备 | 供电 | 备注 |
|---|---|---|
| C5 | **5V** | ⚠️ 外部 3V3 与 USB 同时接 → LDO 被反灌，认不到串口 |
| F411 | USB / ST-Link 5V | |
| ST-Link | —— | **只接 SWDIO / SWCLK / GND**，不要接 3V3 |
| 步进驱动 | 炮塔独立电池 | 独立开关，不充电 |

## 调试串口

- C5 的 COM 口常被 `idf_monitor` 占用，烧录前先关掉（`Stop-Process -Name python`）
- ⚠️ **打开 C5 的串口会触发 DTR/RTS 跳变 → C5 复位**。测试 BLE/WiFi 时不要同时读串口，否则会看到“莫名其妙掉线”
