/** @env pure @kind contract @why 把"转发型指令"实现成空操作（或漏掉请求）不会报错：请求消失了、日志看起来一切正常 */
/**
 * tools/test/emulator-new-forward.test.mjs —— 转发型指令（`60-ops.md` 的 CORE-OPS-FORWARD-MESSAGE）守卫
 *
 * ★ 判据为什么是这些：
 *   - **请求必须发生**：`0x1ca` 不是空操作 —— 漏发请求不报错，只是子系统永远收不到"跳过"；
 *   - **引擎数据不许变**：转发型指令的合同是"不改变脚本可观察的引擎数据"，顺手写一个标量就会
 *     在别的分支上显形；
 *   - **未建模必须留痕**：子系统内部状态没有观察 ⇒ 必须记一条诊断，否则"没建模"与"建模成 0"长得一样。
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

test('★ 0x1ca：把操作数交给消息子系统（请求发生，即使操作数是 0）', () => {
  for (const value of [0, 1, 1234]) {
    const m = machineFor([[0x1ca, [[0, value]]]]);
    m.run();
    assert.deepEqual(m.effects, [{ kind: 'message.request', detail: { entry: 'ReadTextSkip', value } }],
      `op1=${value} 时也必须发出一次请求（空操作实现会得到 []）`);
  }
});

test('★ 0x1ca 不改变脚本可观察的引擎数据（池 / 帧 / 标量 / 状态字都不动）', () => {
  const m = machineFor([[0x1ca, [[0, 5]]]]);
  m.run();
  assert.equal(m.scalars.size, 0, '不许顺手写标量');
  assert.equal(m.flags, 0, '不许碰状态选择字');
  assert.equal(m.globalInt.size + m.globalFloat.size + m.frame.localInt.size + m.frame.localFloat.size, 0,
    '不许写池');
  assert.equal(m.steps, 1, '它是一条正常指令：执行后必须前进');
});

test('★ 0x1ca：子系统未建模必须留诊断（否则"没建模"与"建模成 0"长得一样）', () => {
  const m = machineFor([[0x1ca, [[0, 0]]]]);
  m.run();
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['message-subsystem-unmodeled']);
});
test('★ 0x324：没有操作数，把引擎字段里的对象交给入口（请求发生、数据不变）', () => {
  const m = machineFor([[0x324, []]]);
  m.run();
  assert.deepEqual(m.effects, [{ kind: 'engine.request', detail: { callee: 'sub_453530', holder: 'Engine.d93384' } }]);
  assert.equal(m.scalars.size, 0, '不许顺手写标量（接收者来自引擎字段，不是操作数）');
  assert.equal(m.steps, 1);
});

test('★ 0x32f：按索引清容器里那一格，其它格不动；通知留未建模标记', () => {
  const m = machineFor([[0x32f, [[0, 3]]]]);
  m.scalars.set('Engine.d94385+1', 9);
  m.scalars.set('Engine.d94385+3', 7);
  m.run();
  assert.equal(m.scalars.get('Engine.d94385+3'), 0, '索引 3 的那一格被清');
  assert.equal(m.scalars.get('Engine.d94385+1'), 9, '别的格不许动（按索引清，不是整块清）');
  assert.equal(m.effects.filter((e) => e.kind === 'container.request').length, 1);
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['call-target-unmodeled:sub_49A150']);
});
