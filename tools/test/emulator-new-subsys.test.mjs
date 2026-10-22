/** @env pure @kind contract @why 0x73 的"0 换成 1"与读序写错都不报错；门槽被当成操作数也只在日志里看出一支请求的实参不对 */
/**
 * tools/test/emulator-new-subsys.test.mjs —— 子系统转发（`60-ops-list.md` 的 CORE-OPS-LIST-SUBSYSTEM-SET73）守卫
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { ConfigStore } from '../../apps/emulator-new/src/core/config.ts';

const table = await loadOpcodeTable();
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

test('★ 0x73：第 10 个操作数为 0 时，固定格写成 1（不是 0）', async () => {
  const m = machineFor([[0x73, ops10([1, 2, 3, 4, 5, 6, 7, 8, 9, 0])]]);
  await m.run();
  assert.equal(m.scalars.get('Engine.d107656'), 1, '0 必须被换成 1 —— 写原值这条会红');

  const nz = machineFor([[0x73, ops10([1, 2, 3, 4, 5, 6, 7, 8, 9, 42])]]);
  await nz.run();
  assert.equal(nz.scalars.get('Engine.d107656'), 42, '非 0 原样写');
});

test('★ 0x73：第 1..9 个操作数写十格（不是转发请求）', async () => {
  const m = machineFor([[0x73, ops10([9, 20, 30, 40, 50, 60, 70, 80, 90, 0])]]);
  await m.run();
  const rec = m.ui.records.get(9);
  assert.deepEqual(
    { f3c: rec.f3c, f40: rec.f40, f44: rec.f44, f48: rec.f48, f4c: rec.f4c, f50: rec.f50, f54: rec.f54, f58: rec.f58, f5c: rec.f5c, f60: rec.f60 },
    { f3c: 40, f40: 50, f44: 60, f48: 120, f4c: 140, f50: 20, f54: 30, f58: 1, f5c: 90, f60: 90 },
  );
});

test('★ 0x73：门槽没建模 ⇒ 请求照发但标 gateModeled:false（不许假装知道门的状态）', async () => {
  const m = machineFor([[0x73, ops10([9, 20, 30, 40, 50, 60, 70, 80, 90, 0])]]);
  await m.run();
  const vt = m.effects.find((e) => e.kind === 'vtable.request');
  assert.ok(vt, '门槽按 0 处理 ⇒ 会发这一支');
  assert.deepEqual(vt.detail, {
    callee: 'vtable+4', message: 0x21, a: 2 * 70, b: 80, c: 2, gateSlot: 'Engine.d166964', gateModeled: false,
  }, '实参 2×op7 / op8 / 2：把门槽当操作数会读到别的值');
  assert.deepEqual(m.diagnostics.map((d) => d.kind), [], 'sub_456430 的十格已建模，不再留未建模诊断');
});

test('★ 0x73：renderer-init 把 set:DrawMode=1 播种进门槽 ⇒ 不发第二请求', async () => {
  const config = new ConfigStore({
    defaults: [{ indexKey: 'DRAWMODE', internal: 'set:DrawMode', kind: 'int' }],
    ini: [],
    persist: [],
  });
  config.setInt('set:DrawMode', 1);
  const m = new Machine({ body: script([[0x73, ops10([9, 20, 30, 40, 50, 60, 70, 80, 90, 0])]]), table, key: KEY, config });
  await m.run();
  assert.equal(m.scalars.get('Engine.d166964'), 1);
  assert.equal(m.effects.some((e) => e.kind === 'vtable.request'), false, '门=1 ⇒ 不发 0x21 请求');
});
test('★ 0x78 与 0x2db：各自写**自己的**字段（同形不等于同槽），并各发一次尾跳请求', async () => {
  const m = machineFor([[0x78, [[0, 11]]], [0x2db, [[0, 22]]]]);
  await m.run();
  assert.equal(m.scalars.get('Engine.d21667'), 11, '0x78 写 +0x55C');
  assert.equal(m.scalars.get('Engine.d71744'), 22, '0x2db 写 +0x313D0（写同一个槽这条会红）');
  assert.deepEqual(m.effects.map((e) => e.detail.field), ['Engine.d21667', 'Engine.d71744']);
  assert.ok(m.effects.every((e) => e.detail.callee === 'sub_459F40'));
});
