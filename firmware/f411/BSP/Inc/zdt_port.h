/**
  ******************************************************************************
  * @file    zdt_port.h
  * @brief   ZDT 协议层 -> STM32 HAL UART 适配
  *
  * 支持**一条 UART 挂多台驱动器**（ZDT 多机总线）：init 时各传自己的地址，
  * 收到一个字节就喂给这条 UART 上所有实例，由 zdt_parse 按地址过滤。
  * 这样 F411 的三个 USART 才能挤出四条链路
  * （C5 + YAW/PITCH 共总线 + 底盘）。
  ******************************************************************************
  */
#ifndef __ZDT_PORT_H
#define __ZDT_PORT_H

#include "main.h"
#include "zdt.h"

#ifdef HAL_UART_MODULE_ENABLED

#define ZDT_RX_RING      64
#define ZDT_MAX_UART      4     /* 最多几条 UART 挂 ZDT */
#define ZDT_MAX_PER_UART  2     /* 每条 UART 最多几台驱动器 */

typedef struct
{
  UART_HandleTypeDef *huart;
  uint8_t  addr;

  uint8_t  ring[ZDT_RX_RING];
  volatile uint16_t head, tail;

  volatile uint8_t  last_status;
  volatile uint32_t n_ack, n_reject, n_timeout, n_err;
  volatile uint32_t n_tx;        /* 发送尝试次数 */
  volatile uint32_t n_tx_err;    /* HAL 返回非 OK 的次数 */

  /* 遥测：最近一次 0x36 读回的位置（闭环编码器计数） */
  volatile int32_t  pos_raw;
  volatile uint16_t pos_seq;      /* 每次读回自增，上层可判新 */
} zdt_uart_t;

void zdt_uart_init(zdt_uart_t *u, UART_HandleTypeDef *huart, uint8_t addr);

/* 由 bsp_uart.c 的中断分发调用：1 = 这条 UART 归 ZDT 管 */
int  zdt_port_on_rx(UART_HandleTypeDef *huart);
void zdt_port_on_error(UART_HandleTypeDef *huart);

void zdt_uart_poll(zdt_uart_t *u);           /* 1kHz 里调，非阻塞 */

/* 单发帧（阻塞 <= 2ms @115200） */
void zdt_uart_send(zdt_uart_t *u, const uint8_t *frame, uint16_t len);

/* 常用命令 */
void zdt_uart_enable(zdt_uart_t *u, bool on);
void zdt_uart_speed(zdt_uart_t *u, int16_t rpm);          /* 正负表方向 */
void zdt_uart_move(zdt_uart_t *u, int32_t pulses, uint16_t rpm,
                   uint8_t accel, uint8_t mode);
void zdt_uart_stop(zdt_uart_t *u);
void zdt_uart_request_pos(zdt_uart_t *u);   /* 发 0x36，回包进 pos_raw */
void zdt_uart_set_addr(zdt_uart_t *u, uint8_t new_addr, bool store);

/* 读编码器位置（阻塞版，<=timeout_ms）。成功后 *enc = 0..65535 一圈 */
bool zdt_uart_read_pos(zdt_uart_t *u, int16_t *enc, uint16_t timeout_ms);

#endif /* HAL_UART_MODULE_ENABLED */
#endif /* __ZDT_PORT_H */
