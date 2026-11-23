# sansho 云台 BSP —— 接线、模块、参数

## 1. 串口分配（按当前 CubeMX 配置）

| USART | 引脚 | 标签 | 接什么 | 波特率 |
|---|---|---|---|---|
| USART1 | PA9 / PA10 | PITCH_TX / PITCH_RX | **ESP32-C5 射频桥** | **921600，DMA + IDLE** |
| USART6 | PA11 / PA12 | YAW_TX / YAW_RX | **ZDT 多机总线：YAW(地址1) + PITCH(地址2)** | 115200 8N1 |
| USART2 | PA2 / PA3 | RUN_TX / RUN_RX | MG310 四驱底盘板 | 115200 8N1 |

F411 只有 3 个 USART、要挂 4 条链路，所以两轴 ZDT 必须合并到一条多机总线：
Emm42 本来就支持多机（靠地址区分），`zdt_port.c` 也已支持"一条 UART 挂 N 个实例"。

> PITCH 驱动器出厂地址是 1，必须用 `zdt_uart_set_addr(&pitch, 2, true)` 改一次并掉电保存；
> 两条 485/TTL 线并到 USART6 上（YAW 已经在那条线上）。

PA9/PA10 的标签还是 PITCH_TX/RX，那是上一版留下的名字，别按标签接线——**这两根已经归 C5 了**。

时钟：HSE 25 MHz → PLL → **100 MHz**；TIM3 PSC=99/ARR=999 → 1 kHz（目前只做旁路时基，脉冲由驱动器自己发）。

## 2. 模块

| 文件 | 作用 |
|---|---|
| `BSP/Src/zdt.c` / `BSP/Inc/zdt.h` | ZDT 协议帧构造 + 回包解析（纯逻辑，PC 可测） |
| `BSP/Src/zdt_port.c` | 协议层 ↔ HAL UART（中断收 1 字节 + 环形缓冲） |
| `BSP/Src/scurve.c` | 7 段 S 曲线（jerk 限幅），静止→静止 |
| `BSP/Src/app_gimbal.c` | 两轴应用：使能 → 清零 → 演示动作，200 Hz 位置流 |
| `BSP/Src/chassis.c` | MG310 底盘 ASCII 协议（`$Car:` / `$Car_Pwm:` / 读电池） |
| `BSP/Src/bsp_uart.c` | 三个 USART 共用一个 `HAL_UART_RxCpltCallback`，按句柄分发 |
| `tools/host_test/test_zdt_scurve.c` | PC 单测：帧对照手册、S 曲线数值校验 |

## 3. 节拍

```
主循环：
  1 kHz   Gimbal_Tick1ms()   两个 ZDT 的串口收包（非阻塞）
  1 kHz   Esp_Tick()         C5 帧滑窗自同步 + CRC16 校验
  200 Hz  Gimbal_Tick5ms()   S 曲线推进（5ms 步进）
  100 Hz                    位置指令下发（两轴共享 115200 总线，13B/帧阻塞 1.1ms）
  1 Hz    Chassis_Tick1s()   读底盘电池电压
```

`Chassis_Init` 已经把 `$MOTOR_4CH_SET:2!` 发出去，底盘上电默认 MG310 类型。

位置流的技巧：`0xFD` 里的 rpm 参数填**当前剖面的瞬时速度**（`v/6`），
驱动器就只走 5 ms 内该走的那点距离，不会"扑到目标再等"，200 Hz 台阶被抹平。

## 4. 常改的参数

`BSP/Src/app_gimbal.c` 顶部：

```c
#define MOTOR_STEP_DEG   1.8f    /* 换 0.9° 电机改这里 */
#define MICROSTEP        16      /* 驱动器拨码细分 */
static const scurve_lim_t k_lim_yaw   = { 1200.0f, 4000.0f, 40000.0f }; /* 200rpm */
static const scurve_lim_t k_lim_pitch = {  720.0f, 2000.0f, 20000.0f }; /* 120rpm */
static const step_t k_demo[] = { {15,10,300}, {-15,-10,300}, {0,0,600} };
#define DEMO_LOOPS       3
```

`BSP/Src/chassis.c`：

```c
CHASSIS_MOTOR_MG310 = 2      /* $MOTOR_4CH_SET:2! */
```

## 5. 底盘协议（摘自 电机驱动例程/esp32uart.ino）

```
$MOTOR_4CH_SET:<0|1|2>!              0=520电机 1=TT 2=310(MG310)
$MOTOR_4CH_SET_ENCPDER_POLARITY:<0|1>!     注意固件里就拼成 ENCPDER
$Car:<a>,<b>,<c>,<d>!                闭环速度
$Car_Pwm:<a*100>,<b*100>,<c*100>,<d*100>!  开环 PWM
$MOTOR_4CH_READ:battery!             -> $MOTOR_4CH_Battery:<mV>!
$MOTOR_4CH_READ:encoder_20ms!        -> $MOTOR_4CH_Encoder_20ms:a,b,c,d!
$MOTOR_4CH_READ:encoder_total!       -> $MOTOR_4CH_Encoder_Total:a,b,c,d!
```

`$Car:` 的数值单位按例程原样发（例程用 0.1 / 0.5），实际是 m/s 还是归一化要上机试。

## 6. 上电会发生什么

1. `Chassis_Init(&huart2)`：设 310 电机类型 → 速度清零停车
2. `Gimbal_Init()`：等 300 ms → 两轴使能 → 等 200 ms → 两轴位置清零 → 等 200 ms → 开始演示
3. 演示：Yaw +15°/Pitch +10° → Yaw −15°/Pitch −10° → 回原点，循环 3 次，停在原点
4. 演示结束后按 PB0 重放
