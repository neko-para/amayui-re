/** @env pure @kind contract @why 0x71 的"位置写帧字段"漏写 / 写错（写下一条、写成字节偏移）都不会报错，只会让帧里那格静默错掉 */
/**
 * tools/test/emulator-new-text.test.mjs —— 文本请求（`60-ops-list.md` 的 CORE-OPS-LIST-TEXT-REQUEST）守卫
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

test('★ 0x71：把**本条**指令的位置写进帧字段（不是下一条、不是字节偏移）', () => {
  // 先放一条 0x1a8（4 字节），让 0x71 的位置 = 1，能区分"本条"与"下一条"
  const m = machineFor([[0x1a8, []], [0x71, [[0, 3]]]]);
  m.run();
  assert.equal(m.frame.position, 1, '位置 = 本条 ip / 4 = 1（写成下一条会得到 4；写成字节会得到 4）');
});

test('★ 0x71：请求里第 3 个实参是引擎槽（不许伪装成操作数）', () => {
  const m = machineFor([[0x71, [[0, 3]]]]);
  m.run();
  assert.deepEqual(m.effects, [{
    kind: 'text.request',
    detail: { slot: 3, engineSlot: 'Engine.d97055', engineSlotModeled: false },
  }]);
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['text-subsystem-unmodeled']);
});

test('★ 0x71 不许顺手置状态位（条件未取证 ⇒ 置错会让门立刻关上）', () => {
  const m = machineFor([[0x71, [[0, 1]]]]);
  m.run();
  assert.equal(m.flags, 0, '本版不写那次有条件的状态位写；门必须仍然开着');
});
