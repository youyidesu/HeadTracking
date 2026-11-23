/**
 * @ht/protocol —— 两条帧格式的唯一事实来源
 *
 *  wire16   16 B    Quest/PC → C5（UDP / ESP-NOW / BLE 都用它）
 *  head30   30 B    C5 → F411（SPI，qp_head_t）
 *
 * 两端实现必须和这里逐字节一致：
 *   · Quest 端  apps/quest/src/bridge.js
 *   · PC   端  apps/relay/src/index.js
 *   · C5       firmware/esp32c5/main/main.c
 *   · F411     stm32test/BSP/Inc/esp_link.h
 */

export const WIRE16_LEN = 16;
export const HEAD30_LEN = 30;
export const MAGIC = 0xa55a;
export const QP_VER = 1;

/** flags 位 */
export const FLAG_VALID = 0x01;
export const FLAG_RECENTER = 0x04;
export const FLAG_ESTOP = 0x08;

/* ─────────────────────────── CRC16/CCITT ─────────────────────────── */

/**
 * CRC16-CCITT（poly 0x1021, init 0xFFFF, 不反转）
 * @param {Uint8Array} buf
 * @param {number} [len]
 * @returns {number}
 */
export function crc16(buf, len = buf.length) {
  let crc = 0xffff;
  for (let i = 0; i < len; i++) {
    crc ^= buf[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/* ─────────────────────────── 16 B 空口帧 ─────────────────────────── */

/**
 * @typedef {Object} PoseFrame
 * @property {number} seq      0..65535
 * @property {number} yaw      单位度，可负
 * @property {number} pitch    单位度，可负
 * @property {number} yawRate  单位度/秒
 * @property {number} pitchRate
 * @property {number} flags    位掩码
 */

/**
 * @param {PoseFrame} f
 * @returns {Uint8Array}
 */
export function encodeWire16(f) {
  const b = new Uint8Array(WIRE16_LEN);
  const v = new DataView(b.buffer);
  v.setUint16(0, MAGIC, true);
  v.setUint16(2, f.seq & 0xffff, true);
  v.setInt16(4, clampI16(Math.round(f.yaw * 100)), true);
  v.setInt16(6, clampI16(Math.round(f.pitch * 100)), true);
  v.setInt16(8, clampI16(Math.round((f.yawRate ?? 0) * 10)), true);
  v.setInt16(10, clampI16(Math.round((f.pitchRate ?? 0) * 10)), true);
  v.setUint16(12, f.flags ?? FLAG_VALID, true);
  v.setUint16(14, crc16(b, 14), true);
  return b;
}

/**
 * 解析并校验；失败返回 null（magic 或 CRC 不对）
 * @param {Uint8Array} buf
 * @returns {PoseFrame|null}
 */
export function decodeWire16(buf) {
  if (buf.length !== WIRE16_LEN) return null;
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (v.getUint16(0, true) !== MAGIC) return null;
  if (crc16(buf, 14) !== v.getUint16(14, true)) return null;
  return {
    seq: v.getUint16(2, true),
    yaw: v.getInt16(4, true) / 100,
    pitch: v.getInt16(6, true) / 100,
    yawRate: v.getInt16(8, true) / 10,
    pitchRate: v.getInt16(10, true) / 10,
    flags: v.getUint16(12, true),
  };
}

/* ─────────────────────────── 30 B F411 帧 ─────────────────────────── */

/**
 * C5 把 16 B 重打包成 F411 期望的 qp_head_t
 * @param {PoseFrame} f
 * @param {number} seq32    32 位序号（F411 用它判丢帧）
 * @param {number} tUs      C5 单调时钟，微秒
 * @returns {Uint8Array}
 */
export function encodeHead30(f, seq32, tUs) {
  const b = new Uint8Array(HEAD30_LEN);
  const v = new DataView(b.buffer);
  v.setUint16(0, MAGIC, true);
  v.setUint8(2, QP_VER);
  v.setUint8(3, (f.flags ?? FLAG_VALID) & 0xff);
  v.setUint32(4, seq32 >>> 0, true);
  v.setUint32(8, tUs >>> 0, true);
  v.setInt16(12, 0, true); // qw
  v.setInt16(14, 0, true); // qx
  v.setInt16(16, 0, true); // qy
  v.setInt16(18, 0, true); // qz
  v.setInt16(20, clampI16(Math.round(f.yaw * 100)), true);
  v.setInt16(22, clampI16(Math.round(f.pitch * 100)), true);
  v.setInt16(24, clampI16(Math.round((f.yawRate ?? 0) * 10)), true);
  v.setInt16(26, clampI16(Math.round((f.pitchRate ?? 0) * 10)), true);
  v.setUint16(28, crc16(b, 28), true);
  return b;
}

/**
 * @param {Uint8Array} buf
 * @returns {null | {seq:number,tUs:number,yaw:number,pitch:number,yawRate:number,pitchRate:number,flags:number}}
 */
export function decodeHead30(buf) {
  if (buf.length !== HEAD30_LEN) return null;
  const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (v.getUint16(0, true) !== MAGIC) return null;
  if (v.getUint8(2) !== QP_VER) return null;
  if (crc16(buf, 28) !== v.getUint16(28, true)) return null;
  return {
    flags: v.getUint8(3),
    seq: v.getUint32(4, true),
    tUs: v.getUint32(8, true),
    yaw: v.getInt16(20, true) / 100,
    pitch: v.getInt16(22, true) / 100,
    yawRate: v.getInt16(24, true) / 10,
    pitchRate: v.getInt16(26, true) / 10,
  };
}

/* ─────────────────────────── 姿态 → 欧拉角 ─────────────────────────── */

/**
 * WebXR/OpenXR 四元数 → 我们协议用的 yaw/pitch（度）
 * WebXR 右手系：+Y 上、-Z 前。yaw 右转为正，pitch 抬头为正。
 * @param {{x:number,y:number,z:number,w:number}} q
 * @returns {{yaw:number,pitch:number}}
 */
export function quatToYawPitch(q) {
  const f = rotateVec([q.x, q.y, q.z, q.w], [0, 0, -1]);
  return {
    yaw: (Math.atan2(-f[0], -f[2]) * 180) / Math.PI,
    pitch: (Math.asin(clamp(f[1], -1, 1)) * 180) / Math.PI,
  };
}

/** @param {number[]} q @param {number[]} v */
export function rotateVec(q, v) {
  const [x, y, z, w] = q;
  const ix = w * v[0] + y * v[2] - z * v[1];
  const iy = w * v[1] + z * v[0] - x * v[2];
  const iz = w * v[2] + x * v[1] - y * v[0];
  const iw = -x * v[0] - y * v[1] - z * v[2];
  return [
    ix * w + iw * -x + iy * -z - iz * -y,
    iy * w + iw * -y + iz * -x - ix * -z,
    iz * w + iw * -z + ix * -y - iy * -x,
  ];
}

function clamp(v, a, b) {
  return v < a ? a : v > b ? b : v;
}
function clampI16(v) {
  return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}
