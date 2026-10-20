/** @env pure @kind gate @why 解码器把头部 / 指令切分 / 代码区上限实现错了（按错的结构写模拟器，后面全部对不上） */
/**
 * tools/test/emulator-new-decode.test.mjs —— **CORE-ISA 的可执行投影**守卫
 *
 * 用**合成字节**验证 `apps/emulator-new/src/decode.ts`：
 * 头 = 4B 签名 + 4B 版本串 + 6×u32 + 子头（长度 + 3 组 len/off），头长 = 32 + 子头长度；
 * 指令 = u32 opcode + argc×(type,raw)；代码区上限 = 第一张表的偏移（dword 下标 ×4）。
 * ★ 真实语料上的结构由 `tools/test/spec-isa.assets.test.mjs` 覆盖（assets 档）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { parseHeader, walk, codeLimit } from '../../apps/emulator-new/src/decode.ts';
import { loadOpcodeTable } from '../../apps/emulator-new/src/spec.ts';

const table = loadOpcodeTable();
const zero = [...table.values()].find((r) => r.argc === 0);
const two = [...table.values()].find((r) => r.argc === 2);

function buildScript(firstOpcode, secondOpcode) {
  const codeWords = 6; // 1 条 argc0（1 字）+ 1 条 argc2（5 字）
  const body = Buffer.alloc((codeWords + 3) * 4); // 代码 6 字 + 三张表各 1 字
  body.writeUInt32LE(firstOpcode, 0);
  body.writeUInt32LE(secondOpcode, 4);
  body.writeUInt32LE(3, 8);
  body.writeUInt32LE(5, 12);
  body.writeUInt32LE(9, 16);
  body.writeUInt32LE(7, 20);
  // 三张表：offsets 是 body 内 dword 下标，首尾相接
  body.writeUInt32LE(1, 24);
  body.writeUInt32LE(1, 28);
  body.writeUInt32LE(1, 32);
  const head = Buffer.alloc(60);
  head.write('SYS4', 0, 'latin1');
  head.write('450 ', 4, 'latin1');
  head.writeUInt32LE(28, 32); // 子头长度
  // 三组 (len, off)：从第 6 个 dword 起，各 1 字
  head.writeUInt32LE(1, 36);
  head.writeUInt32LE(6, 40);
  head.writeUInt32LE(1, 44);
  head.writeUInt32LE(7, 48);
  head.writeUInt32LE(1, 52);
  head.writeUInt32LE(8, 56);
  return Buffer.concat([head, body]);
}

test('★ 头部：4B 签名 + 4B 版本串 + 子头 ⇒ 头长 60；三组表首尾相接', () => {
  assert.ok(zero && two, '规范表里应同时有 argc=0 与 argc=2 的条目');
  const buf = buildScript(zero.opcode, two.opcode);
  const h = parseHeader(buf);
  assert.equal(h.signature, 'SYS4');
  assert.equal(h.version, '450 ');
  assert.equal(h.subHeaderLength, 28);
  assert.equal(h.headerLength, 60);
  assert.deepEqual(h.tables, [
    { len: 1, off: 6 },
    { len: 1, off: 7 },
    { len: 1, off: 8 },
  ]);
  assert.equal(h.tables[1].off, h.tables[0].off + h.tables[0].len);
  assert.equal(h.tables[2].off, h.tables[1].off + h.tables[1].len);
  assert.equal(codeLimit(h, buf.length - h.headerLength), 24);
});

test('★ 切分：按 4 + 8×argc 走；operands 原样取出；越界不猜', () => {
  const buf = buildScript(zero.opcode, two.opcode);
  const h = parseHeader(buf);
  const body = buf.subarray(h.headerLength);
  const limit = codeLimit(h, body.length);
  const r = walk(body, table, limit);
  assert.deepEqual(r.instructions.map((x) => [x.offset, x.opcode, x.argc]), [
    [0, zero.opcode, 0],
    [4, two.opcode, 2],
  ]);
  assert.deepEqual(r.instructions[1].operands, [
    { type: 3, raw: 5 },
    { type: 9, raw: 7 },
  ]);
  assert.equal(r.gap, null);
});

test('★ 未知 opcode：报缺口（不猜 argc、不跳过）', () => {
  const buf = buildScript(0x7fffffff, two.opcode);
  const h = parseHeader(buf);
  const body = buf.subarray(h.headerLength);
  const r = walk(body, table, codeLimit(h, body.length));
  assert.equal(r.instructions.length, 0);
  assert.equal(r.gap?.reason, 'opcode-not-in-table');
  assert.equal(r.gap?.opcode, 0x7fffffff);
  assert.equal(r.gap?.argc, null);
});
