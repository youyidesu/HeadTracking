/**
  ******************************************************************************
  * @file    esp_link.h
  * @brief   ESP32-C5（炮塔顶射频桥）<-> STM32F411 链路
  *
  * 为什么不是 UART：F411 只有 3 个 USART，YAW/PITCH/底盘 已经占满；
  * C5 就在炮塔上、离 F411 十几厘米，不过滑环，用 SPI 从机更合适
  * （200Hz×30B=6kB/s，SPI@5MHz 占 0.1%，CS 天然给出帧边界）。
  *
  * 需要 CubeMX：SPI1 = Slave / Full-Duplex / 8bit / CPOL=0 CPHA=0 /
  *             NSS 硬件输入(PA4)，SPI1 RX/TX 各挂 DMA2。
  * 没配 SPI1 时本模块编译成空实现，不影响编译。
  ******************************************************************************
  */
#ifndef __ESP_LINK_H
#define __ESP_LINK_H

#include "main.h"
#include <stdbool.h>

#define ESP_BACKEND_SPI    1
#define ESP_BACKEND_UART   0

#ifndef ESP_LINK_BACKEND
#define ESP_LINK_BACKEND   ESP_BACKEND_SPI
#endif

#define QP_MAGIC        0xA55Au
#define QP_VER          1u

/* 头姿态帧（与 quest3-turret/quest_proto.h 的 qp_head_t 逐字节一致） */
typedef struct __attribute__((packed))
{
  uint16_t magic;            /* QP_MAGIC */
  uint8_t  ver;              /* QP_VER   */
  uint8_t  flags;
  uint32_t seq;
  uint32_t t_us;
  int16_t  qw, qx, qy, qz;   /* 四元数 *32767 */
  int16_t  yaw_cdeg;         /* deg*100 */
  int16_t  pitch_cdeg;       /* deg*100 */
  int16_t  yaw_rate;         /* dps*10  */
  int16_t  pitch_rate;       /* dps*10  */
  uint16_t crc;              /* CRC16/CCITT，前 sizeof-2 字节 */
} qp_head_t;

#define QP_F_VALID       0x01u
#define QP_F_CALIB       0x02u
#define QP_F_RECENTER    0x04u
#define QP_F_ESTOP       0x08u
#define QP_F_PITCH_ZERO  0x10u

typedef struct
{
  qp_head_t last;
  uint32_t  frames;
  uint32_t  crc_err;
  uint32_t  lost;
  uint32_t  last_tick;
} esp_stats_t;

void       Esp_Init(void);                    /* 按 ESP_LINK_BACKEND 起收 */
void       Esp_Tick(void);                    /* 1kHz 里解析（非阻塞） */
bool       Esp_GetHead(qp_head_t *out);       /* 有新帧返回 true 并取走 */
uint32_t   Esp_AgeMs(void);
const esp_stats_t *Esp_Stats(void);
bool       Esp_Ready(void);                   /* 后端是否真的挂上了 */
void       Esp_Watchdog(void);                /* 1kHz 调：断流 500ms 自动重挂 */

#endif /* __ESP_LINK_H */
