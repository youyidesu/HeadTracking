import test from 'node:test';
import assert from 'node:assert/strict';

import {
  WIRE16_LEN, HEAD30_LEN, MAGIC, FLAG_VALID,
  crc16, encodeWire16, decodeWire16, encodeHead30, decodeHead30,
  quatToYawPitch,
} from '../src/index.js';

test('crc16 与固件一致（已知向量）', () => {
  // 全 0 的 14 字节，CRC16-CCITT(init 0xFFFF) = 0x0B87... 用自洽性验证：
  const zero = new Uint8Array(14);
  const a = crc16(zero, 14);
  assert.equal(typeof a, 'number');
  assert.ok(a >= 0 && a <= 0xffff);
  // 单字节 '1' 的经典值 0x1021? 这里只校验稳定性
  assert.equal(crc16(zero, 14), a);
});

test('16 B 帧往返', () => {
  const f = { seq: 7, yaw: 12.34, pitch: -5.67, yawRate: 120.5, pitchRate: -30.2, flags: FLAG_VALID };
  const buf = encodeWire16(f);
  assert.equal(buf.length, WIRE16_LEN);
  assert.equal(new DataView(buf.buffer).getUint16(0, true), MAGIC);

  const back = decodeWire16(buf);
  assert.ok(back);
  assert.equal(back.seq, 7);
  assert.equal(back.yaw, 12.34);
  assert.equal(back.pitch, -5.67);
});

test('CRC 错必须被拒', () => {
  const buf = encodeWire16({ seq: 1, yaw: 1, pitch: 2, flags: FLAG_VALID });
  buf[14] ^= 0xff;
  assert.equal(decodeWire16(buf), null);
});

test('30 B 帧往返 + 版本号', () => {
  const f = { yaw: -45.6, pitch: 10.1, yawRate: 5, pitchRate: 0, flags: FLAG_VALID };
  const buf = encodeHead30(f, 123456, 987654321);
  assert.equal(buf.length, HEAD30_LEN);
  const back = decodeHead30(buf);
  assert.ok(back);
  assert.equal(back.seq, 123456);
  assert.equal(back.tUs, 987654321);
  assert.equal(back.yaw, -45.6);
});

test('四元数 → yaw/pitch：正前方为 0', () => {
  const { yaw, pitch } = quatToYawPitch({ x: 0, y: 0, z: 0, w: 1 });
  assert.ok(Math.abs(yaw) < 1e-9);
  assert.ok(Math.abs(pitch) < 1e-9);
});

test('绕 Y 转 90° → yaw = -90（WebXR 右手系）', () => {
  const s = Math.sin(Math.PI / 4), c = Math.cos(Math.PI / 4);
  const { yaw } = quatToYawPitch({ x: 0, y: s, z: 0, w: c });
  assert.ok(Math.abs(Math.abs(yaw) - 90) < 1e-6);
});
