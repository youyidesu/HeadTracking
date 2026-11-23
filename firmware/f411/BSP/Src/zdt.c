/**
  ******************************************************************************
  * @file    zdt.c
  * @brief   ZDT / Emm42 帧构造与解析（纯逻辑，可在 PC 上跑单测）
  ******************************************************************************
  */
#include "zdt.h"
#include <string.h>

uint16_t zdt_frame_enable(uint8_t addr, uint8_t *out, bool on)
{
  /* 01 F3 AB 01 00 6B */
  out[0] = addr; out[1] = ZDT_FC_ENABLE; out[2] = ZDT_PT_ENABLE;
  out[3] = on ? 1u : 0u; out[4] = ZDT_SYNC_NOW; out[5] = ZDT_TAIL;
  return 6;
}

uint16_t zdt_frame_stop(uint8_t addr, uint8_t *out)
{
  /* 01 FE 98 00 6B */
  out[0] = addr; out[1] = ZDT_FC_STOP; out[2] = ZDT_PT_ESTOP;
  out[3] = ZDT_SYNC_NOW; out[4] = ZDT_TAIL;
  return 5;
}

uint16_t zdt_frame_zero(uint8_t addr, uint8_t *out)
{
  /* 01 0A 6D 6B */
  out[0] = addr; out[1] = ZDT_FC_ZERO; out[2] = ZDT_PT_ZERO; out[3] = ZDT_TAIL;
  return 4;
}

uint16_t zdt_frame_speed(uint8_t addr, uint8_t dir, uint16_t rpm,
                         uint8_t accel, uint8_t *out)
{
  /* 01 F6 01 05DC 0A 00 6B */
  out[0] = addr; out[1] = ZDT_FC_SPEED; out[2] = dir;
  out[3] = (uint8_t)(rpm >> 8); out[4] = (uint8_t)(rpm & 0xFF);
  out[5] = accel; out[6] = ZDT_SYNC_NOW; out[7] = ZDT_TAIL;
  return 8;
}

uint16_t zdt_frame_move(uint8_t addr, int32_t pulses, uint16_t rpm,
                        uint8_t accel, uint8_t mode, uint8_t *out)
{
  /* 01 FD 01 05DC 00 00007D00 00 00 6B */
  uint8_t  dir = (pulses < 0) ? ZDT_DIR_CCW : ZDT_DIR_CW;
  uint32_t mag = (uint32_t)(pulses < 0 ? -pulses : pulses);

  out[0] = addr; out[1] = ZDT_FC_MOVE; out[2] = dir;
  out[3] = (uint8_t)(rpm >> 8); out[4] = (uint8_t)(rpm & 0xFF);
  out[5] = accel;
  out[6] = (uint8_t)(mag >> 24); out[7] = (uint8_t)(mag >> 16);
  out[8] = (uint8_t)(mag >> 8);  out[9] = (uint8_t)(mag);
  out[10] = mode; out[11] = ZDT_SYNC_NOW; out[12] = ZDT_TAIL;
  return 13;
}

uint16_t zdt_frame_read(uint8_t addr, uint8_t *out)
{
  out[0] = addr; out[1] = ZDT_FC_READ_POS; out[2] = ZDT_TAIL;
  return 3;
}

uint16_t zdt_frame_set_addr(uint8_t addr, uint8_t new_addr, bool store, uint8_t *out)
{
  /* 多机共用一条总线时用：出厂都是 1，先单独接一台改成 2 */
  out[0] = addr; out[1] = ZDT_FC_SET_ADDR; out[2] = 0x00;
  out[3] = new_addr; out[4] = store ? 1u : 0u; out[5] = ZDT_TAIL;
  return 6;
}

int zdt_parse(const uint8_t *buf, uint16_t len, uint8_t addr, zdt_reply_t *out)
{
  if (len < 2) return 0;
  if ((addr != 0) && (buf[0] != addr)) return -1;

  if (buf[1] == ZDT_FC_READ_POS)
  {
    if (len < 8) return 0;
    if (buf[7] != ZDT_TAIL) return -1;

    uint32_t raw = ((uint32_t)buf[3] << 24) | ((uint32_t)buf[4] << 16) |
                   ((uint32_t)buf[5] << 8)  |  (uint32_t)buf[6];
    int32_t v = (buf[2] == 1u) ? -(int32_t)raw : (int32_t)raw;
    int32_t m = v % 65536;

    out->kind    = ZDT_RX_POS;
    out->addr    = buf[0];
    out->code    = buf[1];
    out->status  = 0;
    out->pos_raw = v;
    out->enc     = (int16_t)((m < 0) ? (m + 65536) : m);
    return 8;
  }

  if (len < 4) return 0;
  if (buf[3] != ZDT_TAIL) return -1;

  out->kind    = ZDT_RX_ACK;
  out->addr    = buf[0];
  out->code    = buf[1];
  out->status  = buf[2];
  out->pos_raw = 0;
  out->enc     = 0;
  return 4;
}
