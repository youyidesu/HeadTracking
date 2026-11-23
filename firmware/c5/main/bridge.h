#pragma once
#include <stdint.h>

/* 把一段原始报文（1..N 个 16B 帧，或 30B 旧格式）喂给桥接层：
   校验 → 重打包 → SPI 发给 F411。UDP / ESP-NOW / BLE 三条路共用。 */
void bridge_feed(const uint8_t *buf, int len);
