/**
 * @ht/relay —— PC 侧中转
 *
 *   Quest（WebSocket, 16 B 帧） → 这里 → UDP → ESP32-C5:5005
 *
 * 用途：头显不方便直连 C5（或想复用 ControlAnything 页面）时，让 PC 当一跳。
 * 也顺手把帧转给本地 HUD / 日志。
 *
 * 运行：
 *   npm run dev -w @ht/relay
 *   WS_TARGET=192.168.4.1 WS_PORT=8080 UDP_PORT=5005 node src/index.js
 */
import dgram from 'node:dgram';
import { WebSocketServer } from 'ws';

import { decodeWire16, WIRE16_LEN } from '@ht/protocol';

const WS_PORT = Number(process.env.WS_PORT ?? 8080);
const UDP_TARGET = process.env.WS_TARGET ?? '192.168.4.1';
const UDP_PORT = Number(process.env.UDP_PORT ?? 5005);

const udp = dgram.createSocket('udp4');

const wss = new WebSocketServer({ port: WS_PORT, path: '/ws' });
console.log(`[relay] ws://0.0.0.0:${WS_PORT}/ws  →  udp://${UDP_TARGET}:${UDP_PORT}`);

/** @type {Set<import('ws').WebSocket>} */
const clients = new Set();
let frames = 0;
let bad = 0;
let lastReport = Date.now();

wss.on('connection', (ws) => {
  clients.add(ws);
  console.log(`[relay] 客户端接入，当前 ${clients.size}`);

  ws.on('message', (data) => {
    const buf = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer ?? data);

    // 一个报文可以塞 N 个 16 B 帧（拥塞时批量发送）
    if (buf.length === 0 || buf.length % WIRE16_LEN !== 0) {
      bad++;
      return;
    }
    for (let off = 0; off < buf.length; off += WIRE16_LEN) {
      const f = decodeWire16(buf.subarray(off, off + WIRE16_LEN));
      if (!f) { bad++; continue; }
      udp.send(buf.subarray(off, off + WIRE16_LEN), UDP_PORT, UDP_TARGET);
      frames++;
    }
  });

  ws.on('close', () => {
    clients.delete(ws);
    console.log(`[relay] 客户端断开，剩 ${clients.size}`);
  });
});

// 每秒报一次，跟 C5 的串口统计对得上
setInterval(() => {
  const dt = (Date.now() - lastReport) / 1000;
  if (frames || bad) {
    console.log(`[relay] ${(frames / dt).toFixed(0)} 帧/s  坏包 ${bad}  客户端 ${clients.size}`);
  }
  frames = 0;
  bad = 0;
  lastReport = Date.now();
}, 1000).unref();

process.on('SIGINT', () => {
  console.log('\n[relay] 退出');
  udp.close();
  process.exit(0);
});
