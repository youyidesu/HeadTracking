/**
  ******************************************************************************
  * @file    zdt.h
  * @brief   张大头 ZDT / Emm42 闭环步进 —— UART 协议层（纯逻辑，不依赖 HAL）
  *
  * 帧格式：  [addr] [code] [参数...] [0x6B]
  * 普通应答：[addr] [code] [status] [0x6B]                4 字节
  * 读位置  ：[addr] [0x36] [sign] [v31..v0] [0x6B]        8 字节
  *
  * 出厂默认（本工程按这个算）：地址 1、波特率 115200、细分 16、1.8° 电机
  *   -> 3200 脉冲/圈 = 8.889 脉冲/度
  *
  * 多机接同一条总线时地址必须互不相同，出厂都是 1：
  *   先单独接一台，发 0xAE（设置地址）改成 2，再并联。
  ******************************************************************************
  */
#ifndef __ZDT_H
#define __ZDT_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 尾字节 / 功能码 ---- */
#define ZDT_TAIL            0x6B

#define ZDT_FC_ZERO         0x0A    /* 当前位置清零（注意 0x06 是编码器校准，别混） */
#define ZDT_FC_SET_ADDR     0xAE    /* 设置地址（多机总线必用一次） */
#define ZDT_FC_READ_VERSION 0x1F
#define ZDT_FC_READ_POS     0x36    /* 读实时位置，回 8 字节帧 */
#define ZDT_FC_ENABLE       0xF3
#define ZDT_FC_SPEED        0xF6    /* 速度模式 */
#define ZDT_FC_MOVE         0xFD    /* 位置模式 */
#define ZDT_FC_STOP         0xFE    /* 立即停止 */

/* ---- 子协议字节（这几条命令必须带，漏了判格式错误） ---- */
#define ZDT_PT_ZERO         0x6D
#define ZDT_PT_ENABLE       0xAB
#define ZDT_PT_ESTOP        0x98

/* ---- 方向 / 位置模式 / 同步 ---- */
#define ZDT_DIR_CW          0
#define ZDT_DIR_CCW         1

#define ZDT_MODE_REL_LAST   0       /* 相对上一次目标 */
#define ZDT_MODE_ABS        1       /* 相对零点绝对位置 */
#define ZDT_MODE_REL_NOW    2       /* 相对当前实时位置 */

#define ZDT_SYNC_NOW        0

/* ---- 应答状态码 ---- */
#define ZDT_ST_OK           0x02
#define ZDT_ST_AT_ZERO      0x12
#define ZDT_ST_LIMIT        0x22
#define ZDT_ST_DONE         0x9F
#define ZDT_ST_PARAM_ERR    0xE2
#define ZDT_ST_FMT_ERR      0xEE

/* ---- 本工程地址分配（出厂都是 1，PITCH 必须改） ---- */
#define ZDT_ADDR_YAW        1
#define ZDT_ADDR_PITCH      2

/* 帧构造：返回整帧长度（含 0x6B）。out 缓冲至少 16 字节 */
uint16_t zdt_frame_enable (uint8_t addr, uint8_t *out, bool on);
uint16_t zdt_frame_stop   (uint8_t addr, uint8_t *out);
uint16_t zdt_frame_zero   (uint8_t addr, uint8_t *out);
uint16_t zdt_frame_speed  (uint8_t addr, uint8_t dir, uint16_t rpm,
                           uint8_t accel, uint8_t *out);
uint16_t zdt_frame_move   (uint8_t addr, int32_t pulses, uint16_t rpm,
                           uint8_t accel, uint8_t mode, uint8_t *out);
uint16_t zdt_frame_read   (uint8_t addr, uint8_t *out);
uint16_t zdt_frame_set_addr(uint8_t addr, uint8_t new_addr, bool store, uint8_t *out);

/* ---- 回包解析 ---- */
typedef enum
{
  ZDT_RX_NONE = 0,   /* 数据还不完整，等下一批字节 */
  ZDT_RX_ACK,        /* 普通 4 字节应答 */
  ZDT_RX_POS,        /* 8 字节位置应答 */
  ZDT_RX_SKIP        /* 垃圾字节，调用方丢掉 1 字节重同步 */
} zdt_rx_kind_t;

typedef struct
{
  zdt_rx_kind_t kind;
  uint8_t  addr;
  uint8_t  code;
  uint8_t  status;      /* ACK 帧：状态码 */
  int32_t  pos_raw;     /* POS 帧：原始计数值（带符号） */
  int16_t  enc;         /* POS 帧：折算成 0..65535 一圈 */
} zdt_reply_t;

/* 返回消费掉的字节数（>0）；0 = 数据不够；-1 = 出错（丢弃 1 字节）
 * addr 传本机地址，用于帧头校验；传 0 表示不限地址 */
int zdt_parse(const uint8_t *buf, uint16_t len, uint8_t addr, zdt_reply_t *out);

/* 编码器计数 -> 角度（度）。65536 计数 = 一整圈 */
static inline float zdt_enc_to_deg(int16_t enc)
{
  return (float)((uint16_t)enc) * (360.0f / 65536.0f);
}

#endif /* __ZDT_H */
