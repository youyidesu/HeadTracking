/**
  ******************************************************************************
  * @file    zdt_port.c
  * @brief   ZDT 协议层 -> STM32 HAL UART 适配（一条 UART 可挂多台驱动器）
  ******************************************************************************
  */
#include "zdt_port.h"

#ifdef HAL_UART_MODULE_ENABLED

/* -------------------------------------------------------------------------- */
/* 每条 UART 一个槽：一个单字节落脚点，挂 N 个实例                              */
/* -------------------------------------------------------------------------- */
typedef struct
{
  UART_HandleTypeDef *h;
  uint8_t     one;
  uint8_t     n;
  uint8_t     armed;
  zdt_uart_t *users[ZDT_MAX_PER_UART];
} zdt_slot_t;

static zdt_slot_t s_slots[ZDT_MAX_UART];

static zdt_slot_t *slot_find(UART_HandleTypeDef *h)
{
  for (uint8_t i = 0; i < ZDT_MAX_UART; i++)
  {
    if (s_slots[i].h == h) return &s_slots[i];
  }
  return 0;
}

static zdt_slot_t *slot_get(UART_HandleTypeDef *h)
{
  zdt_slot_t *s = slot_find(h);

  if (s) return s;

  for (uint8_t i = 0; i < ZDT_MAX_UART; i++)
  {
    if (s_slots[i].h == 0)
    {
      s_slots[i].h = h;
      s_slots[i].n = 0;
      s_slots[i].armed = 0;
      return &s_slots[i];
    }
  }
  return 0;
}

static inline void ring_put(zdt_uart_t *u, uint8_t b)
{
  uint16_t nxt = (uint16_t)((u->head + 1u) % ZDT_RX_RING);
  if (nxt != u->tail)
  {
    u->ring[u->head] = b;
    u->head = nxt;
  }
  else
  {
    u->n_err++;              /* 满了丢新字节，不覆盖历史 */
  }
}

static inline bool ring_get(zdt_uart_t *u, uint8_t *b)
{
  if (u->tail == u->head) return false;
  *b = u->ring[u->tail];
  u->tail = (uint16_t)((u->tail + 1u) % ZDT_RX_RING);
  return true;
}

/* -------------------------------------------------------------------------- */
/* 初始化 / 中断                                                              */
/* -------------------------------------------------------------------------- */
void zdt_uart_init(zdt_uart_t *u, UART_HandleTypeDef *huart, uint8_t addr)
{
  zdt_slot_t *s;

  u->huart = huart;
  u->addr  = addr;
  u->head = u->tail = 0;
  u->last_status = 0xFF;
  u->n_ack = u->n_reject = u->n_timeout = u->n_err = 0;

  s = slot_get(huart);
  if (s == 0) return;

  for (uint8_t i = 0; i < s->n; i++)
  {
    if (s->users[i] == u) return;         /* 重复注册 */
  }
  if (s->n < ZDT_MAX_PER_UART) s->users[s->n++] = u;

  if (!s->armed)                          /* 整条 UART 只挂一次中断 */
  {
    s->armed = 1;
    HAL_UART_Receive_IT(huart, &s->one, 1);
  }
}

int zdt_port_on_rx(UART_HandleTypeDef *huart)
{
  zdt_slot_t *s = slot_find(huart);

  if (s == 0 || s->armed == 0) return 0;

  for (uint8_t i = 0; i < s->n; i++) ring_put(s->users[i], s->one);

  HAL_UART_Receive_IT(huart, &s->one, 1);
  return 1;
}

void zdt_port_on_error(UART_HandleTypeDef *huart)
{
  zdt_slot_t *s = slot_find(huart);

  if (s == 0) return;

  for (uint8_t i = 0; i < s->n; i++) s->users[i]->n_err++;

  if (s->armed) HAL_UART_Receive_IT(huart, &s->one, 1);
}

/* -------------------------------------------------------------------------- */
/* 收包解析                                                                    */
/* -------------------------------------------------------------------------- */
void zdt_uart_poll(zdt_uart_t *u)
{
  uint8_t buf[16];
  uint8_t b;
  zdt_reply_t rp;

  for (;;)
  {
    uint16_t n = 0;
    while (n < sizeof(buf) && ring_get(u, &b)) buf[n++] = b;
    if (n == 0) return;

    int used = zdt_parse(buf, n, u->addr, &rp);

    if (used > 0)
    {
      if (rp.kind == ZDT_RX_ACK)
      {
        u->last_status = rp.status;
        if (rp.status == ZDT_ST_OK || rp.status == ZDT_ST_DONE ||
            rp.status == ZDT_ST_AT_ZERO || rp.status == ZDT_ST_LIMIT)
          u->n_ack++;
        else
          u->n_reject++;
      }
      else
      {
        u->n_ack++;
        u->pos_raw = rp.pos_raw;    /* 记下闭环位置 */
        u->pos_seq++;
      }

      for (uint16_t i = (uint16_t)used; i < n; i++) ring_put(u, buf[i]);
      continue;
    }

    if (used == 0)                        /* 不够一帧，原样塞回等后续 */
    {
      for (uint16_t i = 0; i < n; i++) ring_put(u, buf[i]);
      return;
    }

    u->n_err++;                           /* 垃圾字节：丢 1 个重同步 */
    for (uint16_t i = 1; i < n; i++) ring_put(u, buf[i]);
  }
}

/* -------------------------------------------------------------------------- */
/* 发送                                                                       */
/* -------------------------------------------------------------------------- */
void zdt_uart_send(zdt_uart_t *u, const uint8_t *frame, uint16_t len)
{
  /* 统计"尝试发送"次数：HAL 的返回值只影响 tx_err，不再影响 n_tx。
     之前用 HAL_OK 才自增，导致 pitch 那路明明发出去了却显示 0。 */
  u->n_tx++;
  if (HAL_UART_Transmit(u->huart, (uint8_t *)frame, len, 5) != HAL_OK) u->n_tx_err++;
}

/* 读实时位置：回 8 字节帧，非阻塞，结果在 pos_raw */
void zdt_uart_request_pos(zdt_uart_t *u)
{
  uint8_t f[4];
  uint16_t n = zdt_frame_read(u->addr, f);
  zdt_uart_send(u, f, n);
}

void zdt_uart_enable(zdt_uart_t *u, bool on)
{
  uint8_t f[8];
  uint16_t n = zdt_frame_enable(u->addr, f, on);
  zdt_uart_send(u, f, n);
}

void zdt_uart_speed(zdt_uart_t *u, int16_t rpm)
{
  uint8_t f[8];
  uint8_t dir = (rpm < 0) ? ZDT_DIR_CCW : ZDT_DIR_CW;
  uint16_t mag = (uint16_t)(rpm < 0 ? -rpm : rpm);
  uint16_t n = zdt_frame_speed(u->addr, dir, mag, 10, f);
  zdt_uart_send(u, f, n);
}

void zdt_uart_move(zdt_uart_t *u, int32_t pulses, uint16_t rpm,
                   uint8_t accel, uint8_t mode)
{
  uint8_t f[16];
  uint16_t n = zdt_frame_move(u->addr, pulses, rpm, accel, mode, f);
  zdt_uart_send(u, f, n);
}

void zdt_uart_stop(zdt_uart_t *u)
{
  uint8_t f[8];
  uint16_t n = zdt_frame_stop(u->addr, f);
  zdt_uart_send(u, f, n);
}

void zdt_uart_set_addr(zdt_uart_t *u, uint8_t new_addr, bool store)
{
  uint8_t f[8];
  uint16_t n = zdt_frame_set_addr(u->addr, new_addr, store, f);
  zdt_uart_send(u, f, n);
}

bool zdt_uart_read_pos(zdt_uart_t *u, int16_t *enc, uint16_t timeout_ms)
{
  uint8_t f[8], b, buf[8];
  uint8_t  n = 0;
  uint32_t t0 = HAL_GetTick();
  zdt_reply_t rp;

  u->tail = u->head;
  uint16_t len = zdt_frame_read(u->addr, f);
  zdt_uart_send(u, f, len);

  while ((HAL_GetTick() - t0) < timeout_ms)
  {
    if (!ring_get(u, &b)) continue;
    buf[n++] = b;

    int used = zdt_parse(buf, n, u->addr, &rp);
    if (used == 8 && rp.kind == ZDT_RX_POS)
    {
      *enc = rp.enc;
      u->n_ack++;
      return true;
    }
    if (used < 0)
    {
      for (uint8_t i = 1; i < n; i++) buf[i - 1] = buf[i];
      n--;
    }
    if (n >= sizeof(buf)) n = 0;
  }

  u->n_timeout++;
  return false;
}

#endif /* HAL_UART_MODULE_ENABLED */
