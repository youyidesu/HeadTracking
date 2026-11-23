/**
  ******************************************************************************
  * @file    esp_link.c
  * @brief   ESP32-C5 链路：SPI 从机（默认）或 UART，固定 30B 帧 + CRC16
  ******************************************************************************
  */
#include "esp_link.h"
#include "app_gimbal.h"
#include <string.h>

#define RING   512

static uint8_t  s_ring[RING];
static volatile uint16_t s_head, s_tail;

static esp_stats_t s_st;
static volatile bool     s_new;
static qp_head_t         s_pending;
static volatile bool     s_ready;

/* -------------------------------------------------------------------------- */
/* CRC16/CCITT (poly 0x1021, init 0xFFFF)                                     */
/* -------------------------------------------------------------------------- */
static uint16_t crc16(const uint8_t *p, uint16_t n)
{
  uint16_t crc = 0xFFFFu;

  while (n--)
  {
    crc ^= (uint16_t)(*p++) << 8;
    for (uint8_t i = 0; i < 8u; i++)
    {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static void __attribute__((unused)) ring_put(const uint8_t *p, uint16_t n)
{
  while (n--)
  {
    uint16_t nxt = (uint16_t)((s_head + 1u) % RING);
    if (nxt != s_tail) { s_ring[s_head] = *p++; s_head = nxt; }
    else               { p++; }
  }
}

static bool ring_peek(uint16_t idx, uint8_t *b)
{
  uint16_t avail = (uint16_t)((s_head + RING - s_tail) % RING);
  if (idx >= avail) return false;
  *b = s_ring[(uint16_t)((s_tail + idx) % RING)];
  return true;
}

/* -------------------------------------------------------------------------- */
/* 后端：SPI 从机                                                              */
/* -------------------------------------------------------------------------- */
#if (ESP_LINK_BACKEND == ESP_BACKEND_SPI) && defined(HAL_SPI_MODULE_ENABLED)

extern SPI_HandleTypeDef hspi1;

/* ── 纯寄存器实现：不用 HAL 的 DMA/IT 收发 ────────────────────────────────
   F411 作 SPI 从机时，HAL 的 TxRx DMA 完成回调实测不可靠（挂几次就停、
   NDTR 永远不归零），而且 DMA 的缓冲指针不跟 CS 复位，容易整帧偏 1 字节。
   这里改成"一个字节一个中断"：每次 RXNE 读一个字节进环形缓冲，同时补一个
   TX 字节。2700 字节/秒的开销可以忽略，但时序完全由硬件决定。 */

#define QP_LEN   ((uint16_t)sizeof(qp_head_t))

static uint8_t s_tx[QP_LEN];          /* MISO 上要发的统计帧 */
static uint8_t s_txidx;               /* 下一个要装进 DR 的字节下标 */
static volatile uint32_t s_last_rx_ms; /* 最近一次收到字节的时刻（判帧间隔） */
static volatile uint8_t  s_rx_seen;    /* ISR 里收到过字节（给任务上下文判帧间隔） */

typedef struct __attribute__((packed))
{
  uint16_t magic;      /* 0x5AA5 */
  uint16_t ver;
  uint32_t frames;     /* F411 累计收到多少帧 */
  uint32_t lost;
  uint32_t crc_err;    /* 高16=yaw应答 低16=pitch应答 */
  uint32_t tick_ms;
  int16_t  yaw_cdeg;   /* 0.1° 单位 */
  int16_t  pitch_cdeg;
  int16_t  yaw_tx;     /* yaw 发送帧数 */
  uint8_t  mode;
  uint8_t  pitch_tx_lo;/* pitch 发送帧数低 8 位 */
  uint16_t crc;
} f411_stat_t;

static volatile uint32_t s_cplt_calls;   /* 收到字节数 */
static uint16_t s_dbg_ack_y, s_dbg_ack_p;
static uint16_t s_dbg_tx_y,  s_dbg_tx_p;
static volatile bool     s_ready;

static void tx_refresh(void)
{
  f411_stat_t t;
  float yp, pp;
  uint16_t ya, pa, ye, pe, ytx, ptx;
  uint8_t ys, ps;

  Gimbal_Diag(&yp, &pp, &ya, &pa, &ye, &pe, &ys, &ps, &ytx, &ptx);
  s_dbg_ack_y = ya; s_dbg_ack_p = pa;
  s_dbg_tx_y  = ytx; s_dbg_tx_p = ptx;

  t.magic        = 0x5AA5u;
  t.ver          = 1u;
  t.frames       = s_st.frames;
  t.lost         = s_st.lost;
  t.crc_err      = ((uint32_t)ya << 16) | pa;
  t.tick_ms      = HAL_GetTick();
  t.yaw_cdeg     = (int16_t)(yp * 10.0f);
  t.pitch_cdeg   = (int16_t)(pp * 10.0f);
  t.yaw_tx       = (int16_t)ytx;
  t.mode         = Gimbal_Mode();
  t.pitch_tx_lo  = (uint8_t)(ptx & 0xFFu);
  t.crc          = crc16((const uint8_t *)&t, (uint16_t)(sizeof(t) - 2u));

  memcpy(s_tx, &t, sizeof(s_tx));       /* 长度一致，正好 30 字节 */
}

/* 每来一个时钟：收一个字节 + 补一个字节。CS 由主机控制，30 字节自然成帧。 */
void SPI1_IRQHandler(void)
{
  while (SPI1->SR & SPI_SR_RXNE)
  {
    uint8_t b = *(volatile uint8_t *)&SPI1->DR;   /* 读 DR 清 RXNE */

    /* 先把下一个字节补进 DR，再做别的。
       1 MHz 下一个字节只有 8µs，ISR 里任何多余调用都可能让 DR 空一拍，
       表现就是"同一个字节被移出两次"。 */
    if (SPI1->SR & SPI_SR_TXE)
    {
      *(volatile uint8_t *)&SPI1->DR = s_tx[s_txidx];
      if (++s_txidx >= QP_LEN) s_txidx = 0u;       /* 每帧正好 30 字节，自然循环 */
    }

    ring_put(&b, 1u);
    s_cplt_calls++;
    s_rx_seen = 1u;                                /* 帧边界判定交给 Esp_Tick */
  }

  if (SPI1->SR & SPI_SR_OVR)            /* 溢出：读 DR 再读 SR 清除 */
  {
    (void)*(volatile uint8_t *)&SPI1->DR;
    (void)SPI1->SR;
  }
}

void Esp_Init(void)
{
  s_head = s_tail = 0;
  s_new  = false;
  s_ready = false;
  memset(&s_st, 0, sizeof(s_st));
  memset(s_tx, 0, sizeof(s_tx));
  s_txidx = 0;

  tx_refresh();

  /* 关掉 HAL 的收发状态机，改用裸寄存器 */
  __HAL_SPI_DISABLE(&hspi1);
  hspi1.State = HAL_SPI_STATE_READY;
  SPI1->CR2 |= SPI_CR2_RXNEIE;          /* 接收非空中断 */
  __HAL_SPI_ENABLE(&hspi1);

  HAL_NVIC_SetPriority(SPI1_IRQn, 1, 0);   /* 最高优先：8µs 一个字节 */
  HAL_NVIC_EnableIRQ(SPI1_IRQn);

  s_ready = true;
}

/* 看门狗：裸寄存器方案不需要重挂，只保持统计帧刷新即可 */
void Esp_Watchdog(void)
{
  if (s_st.frames && (uint32_t)(HAL_GetTick() - s_st.last_tick) > 500u)
  {
    s_st.last_tick = HAL_GetTick();   /* 断流了也不会卡住，仅记录 */
  }
}

/* 保持接口兼容：DMA/回调路径已不用 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) { (void)hspi; }
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)     { (void)hspi; }

bool Esp_Ready(void) { return s_ready; }

/* -------------------------------------------------------------------------- */
/* 后端：UART（备选；只有给 C5 单独留了 UART 时才用）                           */
/* -------------------------------------------------------------------------- */
#elif (ESP_LINK_BACKEND == ESP_BACKEND_UART) && defined(HAL_UART_MODULE_ENABLED)

#define RX_BUF 256
static UART_HandleTypeDef *s_h;
static uint8_t s_dma[RX_BUF];

extern UART_HandleTypeDef huart1;    /* 按实际分配改 */

void Esp_Init(void)
{
  s_h = &huart1;
  s_head = s_tail = 0;
  s_new = false;
  s_ready = false;
  memset(&s_st, 0, sizeof(s_st));

  s_ready = (HAL_UARTEx_ReceiveToIdle_DMA(s_h, s_dma, RX_BUF) == HAL_OK);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if (huart != s_h) return;
  ring_put(s_dma, Size);
  (void)HAL_UARTEx_ReceiveToIdle_DMA(s_h, s_dma, RX_BUF);
}

bool Esp_Ready(void) { return s_ready; }

/* -------------------------------------------------------------------------- */
/* 后端未使能：空实现                                                          */
/* -------------------------------------------------------------------------- */
#else

void Esp_Init(void) { s_ready = false; }
bool Esp_Ready(void) { return false; }
void Esp_Watchdog(void) { /* 后端未使能 */ }

#endif

/* -------------------------------------------------------------------------- */
/* 解析（两端通用）                                                            */
/* -------------------------------------------------------------------------- */
void Esp_Tick(void)
{
  tx_refresh();          /* 1 kHz 刷新统计帧，C5 读到的永远是实时值 */

  /* 帧间隔（58Hz ≈ 17ms）远大于 2ms：空闲超过 2ms 就认为新一帧开始，
     把 TX 指针归零并预装首字节，避免每帧错位 —— 这是 SPI 从机的老问题。 */
  if (s_rx_seen) { s_rx_seen = 0u; s_last_rx_ms = HAL_GetTick(); }

  /* 帧间隔（≥11ms）远大于 2ms：空闲时把 TX 指针归零。
     不动 SPI、不预装 DR —— 帧首会固定多出一个残留字节，
     C5 侧按偏移 1 解析即可，这样偏移是恒定的，不会漂。 */
  if ((uint32_t)(HAL_GetTick() - s_last_rx_ms) > 2u)
  {
    s_txidx = 0u;
  }



  const uint16_t fsz = (uint16_t)sizeof(qp_head_t);
  uint8_t  b;
  uint8_t  tmp[sizeof(qp_head_t)];
  qp_head_t f;

  for (;;)
  {
    if ((uint16_t)((s_head + RING - s_tail) % RING) < fsz) return;

    if (!ring_peek(0, &b)) return;
    if (b != (uint8_t)(QP_MAGIC & 0xFFu))  /* 0xA55A 小端 = 5A A5，首字节 0x5A */
    {
      s_tail = (uint16_t)((s_tail + 1u) % RING);
      continue;
    }

    for (uint16_t i = 0; i < fsz; i++)
    {
      if (!ring_peek(i, &tmp[i])) return;
    }

    memcpy(&f, tmp, fsz);

    if (f.magic != QP_MAGIC || f.ver != QP_VER ||
        crc16((const uint8_t *)&f, (uint16_t)(fsz - 2u)) != f.crc)
    {
      s_st.crc_err++;
      s_tail = (uint16_t)((s_tail + 1u) % RING);
      continue;
    }

    if (s_st.frames && f.seq > s_st.last.seq + 1u)
    {
      s_st.lost += (uint32_t)(f.seq - s_st.last.seq - 1u);
    }
    s_st.frames++;
    s_st.last = f;
    s_st.last_tick = HAL_GetTick();

    s_pending = f;
    s_new = true;

    s_tail = (uint16_t)((s_tail + fsz) % RING);
  }
}

bool Esp_GetHead(qp_head_t *out)
{
  if (!s_new) return false;
  s_new = false;
  *out = s_pending;
  return true;
}

uint32_t Esp_AgeMs(void)
{
  if (s_st.frames == 0u) return 0xFFFFFFFFu;
  return (uint32_t)(HAL_GetTick() - s_st.last_tick);
}

const esp_stats_t *Esp_Stats(void) { return &s_st; }
