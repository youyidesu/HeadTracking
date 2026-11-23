#pragma once

#include <stdint.h>

/* 累计计数（main.c 里更新，状态页读取） */
extern volatile uint32_t g_rx_total;
extern volatile uint32_t g_ok_total;
extern volatile uint32_t g_crc_total;
extern volatile uint32_t g_len_total;
extern volatile uint32_t g_spi_total;

/* 启动 HTTP 状态页：首页每秒自动刷新，/json 给脚本用 */
void http_status_start(const char *ssid, int channel, const char *ip);
