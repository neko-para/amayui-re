/** @env pure @kind contract @why 校验和 / 变换 / 信封写错，真实存档会被引擎拒收，且错误不会在写入时抛 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  buildParams,
  buildSaveBody,
  buildSaveFile,
  buildUncompressedEnvelope,
  crc32NonReflected,
  crc32Reflected,
  transformWords,
  untransformWords,
} from '../../apps/emulator-new/src/core/save-codec.ts';

const ascii = (s) => new TextEncoder().encode(s);

test('★ CRC-32：反射 = 0xCBF43926（标准向量），非反射 = 0xFC891918', () => {
  assert.equal(crc32Reflected(ascii('123456789')), 0xcbf43926);
  assert.equal(crc32NonReflected(ascii('123456789')), 0xfc891918);
});

test('★ 1→2 字变换可逆；偶键 / 余数非 0 ⇒ 抛', () => {
  const src = new Uint32Array([0x11223344, 0xaabbccdd, 0x00000000]);
  const enc = transformWords(src, 0x12345678, 0x1b);
  assert.equal(enc.length, src.length * 2);
  assert.deepEqual([...untransformWords(enc, 0x12345678, 0x1b)], [...src]);

  const zero = transformWords(src, 1, 0);
  assert.throws(() => untransformWords(zero, 1, 0), /除数为 0/);
});

test('★ 未压缩信封：前缀三字 = body 长度、body 原样、参数块两个 CRC 覆盖密文', () => {
  const body = new Uint8Array(32);
  for (let i = 0; i < body.length; i += 1) body[i] = i * 3;
  const cipher = buildUncompressedEnvelope(body, 0x01020304, 0x2f);
  const plain = new Uint8Array(untransformWords(new Uint32Array(cipher.buffer), 0x01020304, 0x2f).buffer);
  const view = new DataView(plain.buffer);
  assert.equal(view.getUint32(0, true), body.length + 12);
  assert.equal(view.getUint32(4, true), body.length + 12);
  assert.equal(view.getUint32(8, true), body.length + 12);
  assert.deepEqual([...plain.slice(12)], [...body]);

  const params = buildParams(cipher, 0x01020304, 0x2f);
  const pv = new DataView(params.buffer);
  assert.equal(pv.getUint32(0, true), cipher.length / 4);
  assert.equal(pv.getUint32(4, true), crc32NonReflected(cipher));
  assert.equal(pv.getUint32(8, true), crc32Reflected(cipher));
  assert.equal(pv.getUint32(12, true), 0x01020304);
  assert.equal(pv.getUint16(16, true), 0x2f);
});

test('★ buildSaveBody / buildSaveFile：帧记录、6 计数、块 0/2/3、尾段、头 +240 与信封', () => {
  const snap = {
    frame: 1,
    records: [
      { status: -1, scriptIndex: 1, ret: [], tableA: -1, tableB: -1 },
      { status: 3, scriptIndex: 2, ret: [7], tableA: 9, tableB: -1 },
    ],
    ints: [10, -2],
    floats: [0x3f800000, 0x40000000],
    strings: ['AB', 'C'],
    version1: 3,
    version2: 20,
  };
  const key = 0x1234abcd;
  const body = buildSaveBody(snap, key);
  const dv = new DataView(body.buffer);
  assert.equal(dv.getUint32(0, true), 1, '主体 +0 = 当前帧');
  const recBase = 1252 + 20000;
  assert.equal(dv.getUint32(recBase + 1044 + 4, true), 2, 'rec[1].脚本文件索引');
  assert.equal(dv.getUint32(recBase + 1044 + 8, true), 1, 'rec[1].返回点数');
  assert.equal(dv.getUint32(recBase + 1044 + 12, true), 7, 'rec[1].返回点');
  assert.equal(dv.getUint32(recBase + 1044 + 1036, true), 9, 'rec[259] 表 A');
  assert.equal(dv.getUint32(recBase + 1044 + 1040, true), 0xffffffff, 'rec[260] 表 B');

  const countBase = recBase + 1044 * 2;
  assert.deepEqual([0, 4, 8, 12, 16, 20].map((o) => dv.getUint32(countBase + o, true)), [2, 0, 2, 2, 0, 0]);

  const file = buildSaveFile(snap, key, 0x01020304, 0x2f);
  assert.equal(new TextDecoder().decode(file.slice(0, 4)), 'S4SD');
  assert.equal(new DataView(file.buffer).getUint32(240, true), body.length + 12);
  assert.equal(new DataView(file.buffer).getUint32(284, true), 3);
  assert.equal(new DataView(file.buffer).getUint32(288, true), 20);
  const cipher = file.slice(312);
  const plain = new Uint8Array(untransformWords(new Uint32Array(cipher.buffer, cipher.byteOffset, cipher.byteLength / 4), 0x01020304, 0x2f).buffer);
  assert.equal(new DataView(plain.buffer).getUint32(0, true), body.length + 12);
  assert.deepEqual([...plain.slice(12)], [...body]);
});
