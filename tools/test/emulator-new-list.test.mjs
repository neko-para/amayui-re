/** @env pure @kind contract @why 0x70 的字段配对写反 / 写错组 / 忘了登记标记都不会报错，只会让列表项拿到另一对数 */
/**
 * tools/test/emulator-new-list.test.mjs —— 列表项（`60-ops-list.md` 的 CORE-OPS-LIST-SET）守卫
 *
 * ★ 判据：三对字段 ← (op2, op3)、一对 ← (op4, op5)、登记标记 = 1、别的组不动；
 *   组号 0 是"用默认组"（未定）⇒ 必须留诊断而不是写第 0 组。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/spec.ts';
import { Machine } from '../../apps/emulator-new/src/machine.ts';

const table = loadOpcodeTable();
const KEY = 0x1234abcd;

function script(instrs) {
  const words = [];
  for (const [opcode, ops] of instrs) {
    words.push(opcode);
    for (const [type, raw] of ops) words.push(type, raw);
  }
  const buf = Buffer.alloc(words.length * 4);
  words.forEach((w, i) => buf.writeUInt32LE(w >>> 0, i * 4));
  return buf;
}

function machineFor(instrs) {
  return new Machine({ body: script(instrs), table, key: KEY });
}

test('★ 0x70：三对字段 ← (op2, op3)，一对 ← (op4, op5)，登记标记 = 1', () => {
  const m = machineFor([[0x70, [[0, 1], [0, 880], [0, 148], [0, 190], [0, 557]]]]);
  m.run();
  const rec = m.ui.records.get(1);
  assert.ok(rec, '组 1 必须建出记录');
  assert.deepEqual(
    { f14: rec.f14, f18: rec.f18, f24: rec.f24, f28: rec.f28, f7c: rec.f7c, f80: rec.f80 },
    { f14: 880, f18: 148, f24: 880, f28: 148, f7c: 880, f80: 148 },
    '三对字段都 ← (op2=880, op3=148)（把两对写反会得到 148/880）',
  );
  assert.deepEqual({ f0c: rec.f0c, f10: rec.f10 }, { f0c: 190, f10: 557 },
    '第四对 ← (op4=190, op5=557)');
  assert.equal(rec.registered, 1, '登记标记必须置 1');
});

test('★ 0x70 只写该组：别的组一字不动', () => {
  const m = machineFor([[0x70, [[0, 2], [0, 1], [0, 2], [0, 3], [0, 4]]]]);
  m.run();
  assert.equal(m.ui.records.has(1), false, '组 1 不许被建/被写');
  assert.equal(m.ui.records.get(2).f14, 1);
});

test('★ 0x70 组号 0：不许写"第 0 组"，只留诊断（默认组是谁未定）', () => {
  const m = machineFor([[0x70, [[0, 0], [0, 1], [0, 2], [0, 3], [0, 4]]]]);
  m.run();
  assert.equal(m.ui.records.size, 0, '组号 0 是"用默认组"，不是"第 0 组"');
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['ui-default-slot-unmodeled']);
});

test('★ 0x70 的内部行为要留痕：一次列表请求 + 一条未建模标记', () => {
  const m = machineFor([[0x70, [[0, 3], [0, 1], [0, 2], [0, 3], [0, 4]]]]);
  m.run();
  assert.deepEqual(m.effects,
    [{ kind: 'ui.list.request', detail: { slot: 3, a3: 1, a4: 2, a5: 3, a6: 4 } }]);
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['ui-list-internals-unmodeled']);
});
test('★ 0x79 与 0x1c1：各写自己那一对字段（同形不同落点）', () => {
  const m = machineFor([[0x79, [[0, 1], [0, 11], [0, 22]]], [0x1c1, [[0, 1], [0, 33], [0, 44]]]]);
  m.run();
  const rec = m.ui.records.get(1);
  assert.deepEqual({ f1c: rec.f1c, f20: rec.f20 }, { f1c: 11, f20: 22 }, '0x79 写 +0x1C/+0x20');
  assert.deepEqual({ f24: rec.f24, f28: rec.f28 }, { f24: 33, f28: 44 }, '0x1c1 写 +0x24/+0x28');
});

test('★ 0x79 组号 0：只留诊断，不写第 0 组', () => {
  const m = machineFor([[0x79, [[0, 0], [0, 1], [0, 2]]]]);
  m.run();
  assert.equal(m.ui.records.size, 0);
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['ui-default-slot-unmodeled']);
});
test('★ 0x212 / 0x213 / 0x25d：写已有记录各自的字段；记录不存在 ⇒ 无动作（不创建）', () => {
  const m = machineFor([[0x70, [[0, 1], [0, 1], [0, 2], [0, 3], [0, 4]]],
    [0x212, [[0, 1], [0, 77]]],
    [0x213, [[0, 1], [0, 88], [0, 99]]],
    [0x25d, [[0, 1], [0, 111], [0, 222]]]]);
  m.run();
  const rec = m.ui.records.get(1);
  assert.equal(rec.f64, 77, '0x212 → +0x64');
  assert.deepEqual({ f68: rec.f68, f6c: rec.f6c }, { f68: 88, f6c: 99 }, '0x213 → +0x68/+0x6C');
  assert.deepEqual({ f114: rec.f114, f118: rec.f118 }, { f114: 111, f118: 222 }, '0x25d → +0x114/+0x118');

  const empty = machineFor([[0x212, [[0, 5], [0, 1]]]]);
  empty.run();
  assert.equal(empty.ui.records.size, 0, '记录不存在时不许顺手建一条空记录');
});
test('★ 0x303：写 +0x120/+0x124；记录不存在时**响亮失败**（与 0x212 那族不同）', () => {
  const m = machineFor([[0x70, [[0, 2], [0, 1], [0, 2], [0, 3], [0, 4]]], [0x303, [[0, 2], [0, 55], [0, 66]]]]);
  m.run();
  const rec = m.ui.records.get(2);
  assert.deepEqual({ f120: rec.f120, f124: rec.f124 }, { f120: 55, f124: 66 });

  const empty = machineFor([[0x303, [[0, 7], [0, 1], [0, 2]]]]);
  assert.throws(() => empty.run(), /list-record-missing:7/, '这条不许静默跳过缺失记录');
});
