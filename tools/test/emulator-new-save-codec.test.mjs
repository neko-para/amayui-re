/** @env pure @kind contract @why 校验和 / 变换 / 信封 / 块布局写错，真实存档会被引擎拒收，且错误不会在写入时抛 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  buildParams,
  buildSaveBody,
  buildSaveFile,
  buildUncompressedEnvelope,
  crc32NonReflected,
  crc32Reflected,
  parseSaveBody,
  parseSaveEnvelope,
  SaveFormatError,
  transformWords,
  untransformWords,
} from '../../apps/emulator-new/src/core/save-codec.ts';
import { decodeInt } from '../../apps/emulator-new/src/core/values.ts';

const ascii = (s) => new TextEncoder().encode(s);

/** 主体里块区的三个基点（只按 `FMT-SAVE-CODEC` 的偏移公式算，不复用实现里的常量）。 */
const layout = (snap) => {
  const recBase = 1252 + (snap.version1 === 3 ? 20000 : 0);
  const countBase = recBase + 1044 * (snap.frame + 1);
  const blocksBase = countBase + 24;
  const capWords = Math.ceil(snap.strings.reduce((n, s) => n + s.length + 1, 0) / 4);
  const block0 = 4 * snap.ints.length;
  const block1 = 4 * snap.floats.length;
  const block2 = 4 + 4 * capWords;
  return { recBase, countBase, blocksBase, capWords, block0, block1, block2, tailBase: blocksBase + block0 + block1 + block2 };
};

const words = (dv, base, n) => Array.from({ length: n }, (_, i) => dv.getUint32(base + 4 * i, true));

/**
 * 两帧记录（`FMT-SAVE-CODEC`）：`rec[0]` = **父帧号**（根 −1、子 = 父帧号），
 * 不是旧合同里的"状态码"；`tableA` / `tableB` 是表 1 / 表 2 的**下标**（这里只验字节落点，取值由
 * `emulator-new-save-tables.test.mjs` 从真实帧区算出来）。
 */
const snapshot = (over = {}) => ({
  frame: 1,
  records: [
    { parent: -1, scriptIndex: 1, ret: [], tableA: -1, tableB: -1 },
    { parent: 0, scriptIndex: 2, ret: [7], tableA: 9, tableB: -1 },
  ],
  ints: [10, -2],
  floats: [0x3f800000, 0x40000000],
  strings: ['AB', 'C'],
  version1: 3,
  version2: 20,
  ...over,
});

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

test('★ 6 个计数槽：c0/c1/c2 = 整型 / 浮点 / 字符串个数，c3/c4/c5 = 0（引用三池未建模）', () => {
  const snap = snapshot();
  const { countBase } = layout(snap);
  const dv = new DataView(buildSaveBody(snap, 0x1234abcd).buffer);
  assert.deepEqual(words(dv, countBase, 6), [2, 2, 2, 0, 0, 0], 'c0..c5 按序：值族三池有模型、引用三池恒 0');
});

test('★ 块 1 = 浮点 raw（与输入逐位一致）；块 3 长度为 0（尾段紧接字符串段）', () => {
  const key = 0x1234abcd;
  const snap = snapshot();
  const { blocksBase, block0, block1, block2, capWords, tailBase } = layout(snap);
  const body = buildSaveBody(snap, key);
  const dv = new DataView(body.buffer);

  // 块 0：整型值池（按值解码回来与输入一致）
  assert.deepEqual(words(dv, blocksBase, 2).map((w) => decodeInt(w, key)), [10, -2], '块 0 = 解码后的整型值');
  // 块 1：浮点值池 —— 浮点写在**块 1**，不是块 3
  assert.deepEqual(words(dv, blocksBase + block0, 2), [0x3f800000, 0x40000000], '块 1 = 浮点 raw 32 位');
  // 块 2：`u32 capWords` + NUL 结尾字节池
  assert.equal(capWords, 2);
  assert.equal(dv.getUint32(blocksBase + block0 + block1, true), capWords, '块 2 首字 = 已用字数');
  assert.deepEqual(
    [...body.slice(blocksBase + block0 + block1 + 4, blocksBase + block0 + block1 + block2)],
    [...ascii('AB\0C\0\0\0\0')],
    '块 2 字节池：字符串连续、NUL 结尾、尾部补 0',
  );
  // 块 3 / 4 / 5 长度 0 ⇒ 尾段长度字段就在块 2 之后
  assert.equal(dv.getUint32(tailBase, true), 740, '尾段首字 = 740 长度字段（块 3 起没有多余字）');
  assert.equal(dv.getUint32(tailBase + 4, true), 0, '场景记录条数 N = 0');
  assert.equal(body.length, tailBase + 756 + (2 + 16463) * 4, 'slack = c2 + 16463 + 3N 个字');
});

test('★ version2 < 20：浮点计数照写、块 1 留 4×c1 个 0 字（合法空洞），后续块位置不变', () => {
  const key = 0x1234abcd;
  const snap = snapshot({ version2: 19 });
  const { countBase, blocksBase, block0, block1, block2, tailBase } = layout(snap);
  const body = buildSaveBody(snap, key);
  const dv = new DataView(body.buffer);

  assert.deepEqual(words(dv, countBase, 6), [2, 2, 2, 0, 0, 0], '计数无条件写（内容被门控 ≠ 计数消失）');
  assert.deepEqual(words(dv, blocksBase + block0, 2), [0, 0], '块 1 仍是 4×c1 长的洞，内容为 0');
  assert.equal(dv.getUint32(blocksBase + block0 + block1, true), 2, '块 2 起点不因留洞而移动');
  assert.equal(dv.getUint32(tailBase, true), 740, '尾段起点不因留洞而移动');
  assert.equal(body.length, layout(snapshot()).tailBase + 756 + (2 + 16463) * 4, '总长与 version2 ≥ 20 时逐字节相同');
  assert.equal(block2, 4 + 4 * 2);
});

test('★ version1 = 2 的容器：无论 version2 都不写浮点内容（也没有 20000 专有副块）', () => {
  const key = 0x1234abcd;
  const snap = snapshot({ version1: 2, version2: 20 });
  const { recBase, blocksBase, block0, block1, tailBase } = layout(snap);
  const body = buildSaveBody(snap, key);
  const dv = new DataView(body.buffer);

  assert.equal(recBase, 1252, '版本 1 ≠ 3 ⇒ 无 20000 字节专有副块');
  assert.deepEqual(words(dv, blocksBase + block0, 2), [0, 0], '版本 1 = 2 ⇒ 浮点内容门控同样生效');
  assert.equal(dv.getUint32(tailBase, true), 740);
  assert.equal(block1, 8);
});

test('★ buildSaveBody / buildSaveFile：帧记录、尾段、头 +240 与信封', () => {
  const snap = snapshot();
  const key = 0x1234abcd;
  const { recBase } = layout(snap);
  const body = buildSaveBody(snap, key);
  const dv = new DataView(body.buffer);
  assert.equal(dv.getUint32(0, true), 1, '主体 +0 = 当前帧');
  assert.equal(dv.getUint32(recBase, true), 0xffffffff, '帧记录[0].父帧号 = −1（根帧）');
  assert.equal(dv.getUint32(recBase + 1044, true), 0, '帧记录[1].父帧号 = 0（父帧号，不是状态码）');
  assert.equal(dv.getUint32(recBase + 1044 + 4, true), 2, '帧记录[1].脚本文件索引');
  assert.equal(dv.getUint32(recBase + 1044 + 8, true), 1, '帧记录[1].返回点数');
  assert.equal(dv.getUint32(recBase + 1044 + 12, true), 7, '帧记录[1].返回点');
  assert.equal(dv.getUint32(recBase + 1044 + 1036, true), 9, '帧记录 +1036 = 表 1 下标');
  assert.equal(dv.getUint32(recBase + 1044 + 1040, true), 0xffffffff, '帧记录 +1040 = 表 2 下标（−1）');

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

test('★ parseSaveEnvelope：buildSaveFile 往返；body 与 buildSaveBody 逐字节相同', () => {
  const snapshot = {
    frame: 0,
    records: [{ parent: -1, scriptIndex: 0, ret: [], tableA: -1, tableB: -1 }],
    ints: [1, -2, 300],
    floats: [0, 0x3f800000],
    strings: ['abc', ''],
    version1: 3,
    version2: 20,
    textureRecords: [{ file: 7, color: 0x123456, flag0: 1, flag1: 0, blank: 0 }],
  };
  const intKey = 0x2468ace;
  const file = buildSaveFile(snapshot, intKey, 0x11223344, 0x1235);
  const parsed = parseSaveEnvelope(file);
  assert.equal(parsed.version1, 3);
  assert.equal(parsed.version2, 20);
  assert.deepEqual(parsed.body, buildSaveBody(snapshot, intKey));
});

test('★ parseSaveEnvelope：坏魔数 / 短文件 / CRC 被改都按 code 拒绝', () => {
  const snapshot = { frame: 0, records: [{ parent: -1, scriptIndex: 0, ret: [], tableA: -1, tableB: -1 }], ints: [], floats: [], strings: [], version1: 3, version2: 20 };
  const file = buildSaveFile(snapshot, 1, 0x11223344, 0x1235);
  const badMagic = file.slice();
  badMagic[0] = 0x58;
  assert.throws(() => parseSaveEnvelope(badMagic), (e) => e instanceof SaveFormatError && e.code === 'save-magic');
  assert.throws(() => parseSaveEnvelope(file.slice(0, 100)), (e) => e instanceof SaveFormatError && e.code === 'save-short');
  const badCrc = file.slice();
  badCrc[296] ^= 0xff;
  assert.throws(() => parseSaveEnvelope(badCrc), (e) => e instanceof SaveFormatError && e.code === 'save-crc1');
});

test('★ parseSaveBody：帧记录 / 纹理记录 / 三池往返；v1≠3 时没有纹理表', () => {
  const snapshot = {
    frame: 2,
    records: [
      { parent: -1, scriptIndex: 0, ret: [], tableA: -1, tableB: -1 },
      { parent: 0, scriptIndex: 11, ret: [3, 9], tableA: 4, tableB: -1 },
      { parent: 1, scriptIndex: 12, ret: [], tableA: -1, tableB: 7 },
    ],
    ints: [1, -2, 300],
    floats: [0, 0x3f800000],
    strings: ['abc', '', 'x'],
    version1: 3,
    version2: 20,
    textureRecords: [{ file: 7, color: 0x123456, flag0: 1, flag1: 0, blank: 0 }],
  };
  const intKey = 0x2468ace;
  const body = buildSaveBody(snapshot, intKey);
  const parsed = parseSaveBody(body, 3, intKey);
  assert.equal(parsed.frame, 2);
  assert.deepEqual(parsed.records, snapshot.records);
  assert.deepEqual(parsed.ints, snapshot.ints);
  assert.deepEqual(parsed.floats, snapshot.floats);
  assert.deepEqual(parsed.strings, snapshot.strings);
  assert.deepEqual(parsed.textureRecords[0], snapshot.textureRecords[0]);
  assert.equal(parsed.textureRecords.length, 1000);

  const v2 = { ...snapshot, version1: 2 };
  const parsed2 = parseSaveBody(buildSaveBody(v2, intKey), 2, intKey);
  assert.deepEqual(parsed2.textureRecords, [], 'v1≠3 没有 20000 B 纹理表');
});
