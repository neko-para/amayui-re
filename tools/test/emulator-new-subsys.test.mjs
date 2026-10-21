/** @env pure @kind contract @why 0x73 的"0 换成 1"与读序写错都不报错；门槽被当成操作数也只在日志里看出一支请求的实参不对 */
/**
 * tools/test/emulator-new-subsys.test.mjs —— 子系统转发（`60-ops-list.md` 的 CORE-OPS-LIST-SUBSYSTEM-SET73）守卫
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

/** 十个立即数操作数（op1..op10） */
function ops10(values) {
  return values.map((v) => [0, v]);
}

test('★ 0x73：第 10 个操作数为 0 时，固定格写成 1（不是 0）', () => {
  const m = machineFor([[0x73, ops10([1, 2, 3, 4, 5, 6, 7, 8, 9, 0])]]);
  m.run();
  assert.equal(m.scalars.get('Engine.d107528'), 1, '0 必须被换成 1 —— 写原值这条会红');

  const nz = machineFor([[0x73, ops10([1, 2, 3, 4, 5, 6, 7, 8, 9, 42])]]);
  nz.run();
  assert.equal(nz.scalars.get('Engine.d107528'), 42, '非 0 原样写');
});

test('★ 0x73：第 1..9 个操作数按序交给子系统（读序倒序不影响实参顺序）', () => {
  const m = machineFor([[0x73, ops10([10, 20, 30, 40, 50, 60, 70, 80, 90, 0])]]);
  m.run();
  const req = m.effects.find((e) => e.kind === 'subsystem.request');
  assert.deepEqual(req.detail, { callee: 'sub_456430', args: [10, 20, 30, 40, 50, 60, 70, 80, 90] });
});

test('★ 0x73：门槽没建模 ⇒ 请求照发但标 gateModeled:false（不许假装知道门的状态）', () => {
  const m = machineFor([[0x73, ops10([10, 20, 30, 40, 50, 60, 70, 80, 90, 0])]]);
  m.run();
  const vt = m.effects.find((e) => e.kind === 'vtable.request');
  assert.ok(vt, '门槽按 0 处理 ⇒ 会发这一支');
  assert.deepEqual(vt.detail, {
    callee: 'vtable+4', message: 0x21, a: 2 * 70, b: 80, c: 2, gateSlot: 'Engine.d166964', gateModeled: false,
  }, '实参 2×op7 / op8 / 2：把门槽当操作数会读到别的值');
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['call-target-unmodeled:sub_456430']);
});
test('★ 0x78 与 0x2db：各自写**自己的**字段（同形不等于同槽），并各发一次尾跳请求', () => {
  const m = machineFor([[0x78, [[0, 11]]], [0x2db, [[0, 22]]]]);
  m.run();
  assert.equal(m.scalars.get('Engine.d21683'), 11, '0x78 写 +0x55C');
  assert.equal(m.scalars.get('Engine.d71760'), 22, '0x2db 写 +0x313D0（写同一个槽这条会红）');
  assert.deepEqual(m.effects.map((e) => e.detail.field), ['Engine.d21683', 'Engine.d71760']);
  assert.ok(m.effects.every((e) => e.detail.callee === 'sub_459F40'));
});
