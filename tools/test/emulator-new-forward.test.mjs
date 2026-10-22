/** @env pure @kind contract @why 把"转发型指令"实现成空操作（或漏掉请求）不会报错：请求消失了、日志看起来一切正常 */
/**
 * tools/test/emulator-new-forward.test.mjs —— 转发型指令（`60-ops-forward.md` 的 CORE-OPS-FORWARD-MESSAGE）守卫
 *
 * ★ 判据为什么是这些：
 *   - **请求必须发生**：`0x1ca` 不是空操作 —— 漏发请求不报错，只是子系统永远收不到"跳过"；
 *   - **引擎数据不许变**：转发型指令的合同是"不改变脚本可观察的引擎数据"，顺手写一个标量就会
 *     在别的分支上显形；
 *   - **未建模必须留痕**：子系统内部状态没有观察 ⇒ 必须记一条诊断，否则"没建模"与"建模成 0"长得一样。
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

function machineFor(instrs, config = undefined) {
  return new Machine({ body: script(instrs), table, key: KEY, config });
}

function configWithReadTextSkip() {
  return new ConfigStore({
    defaults: [],
    ini: [],
    persist: [{ section: 'message', key: 'ReadTextSkip', internal: 'message:ReadTextSkip', kind: 'int' }],
  });
}

test('★ 0x1ca：把操作数原样写进命名键 message:ReadTextSkip（0 也写）', async () => {
  for (const value of [0, 1, 1234, -1]) {
    const config = configWithReadTextSkip();
    config.setInt('message:ReadTextSkip', 7);
    const m = machineFor([[0x1ca, [[0, value]]]], config);
    await m.run();
    assert.equal(config.getInt('message:ReadTextSkip'), value, `op1=${value} 必须原样写入（含 0）`);
    assert.deepEqual(m.effects, [], '这是已建模的配置写，不再发 message.request');
    assert.deepEqual(m.diagnostics, [], '命名键已建模，不再留 message 未建模诊断');
  }
});

test('★ 0x1ca 不改变脚本可观察的引擎数据（池 / 帧 / 标量 / 状态字都不动）', async () => {
  const m = machineFor([[0x1ca, [[0, 5]]]], configWithReadTextSkip());
  await m.run();
  assert.deepEqual([...m.scalars.keys()].filter((k) => k !== 'Engine.d166964'), [], '不许顺手写别的标量（DrawMode 缓存是构造期播种）');
  assert.equal(m.flags, 0, '不许碰状态选择字');
  assert.equal(m.globalInt.size + m.globalFloat.size + m.frame.localInt.size + m.frame.localFloat.size, 0,
    '不许写池');
  assert.equal(m.steps, 1, '它是一条正常指令：执行后必须前进');
});

test('★ 0x1ca：命名键在可持久词表里 ⇒ serializeIni 写回 message.ReadTextSkip', async () => {
  const config = configWithReadTextSkip();
  const m = machineFor([[0x1ca, [[0, 42]]]], config);
  await m.run();
  assert.match(Buffer.from(config.serializeIni()).toString('latin1'), /\[message\]\r\nReadTextSkip=42\r\n/);
});
test('★ 0x324：没有操作数，把引擎字段里的对象交给入口（请求发生、数据不变）', async () => {
  const m = machineFor([[0x324, []]]);
  await m.run();
  assert.deepEqual(m.effects, [{ kind: 'engine.request', detail: { callee: 'sub_453530', holder: 'Engine.d93384' } }]);
  assert.equal(m.scalars.size, 0, '不许顺手写标量（接收者来自引擎字段，不是操作数）');
  assert.equal(m.steps, 1);
});

test('★ 0x32f：按索引清容器里那一格，其它格不动；通知留未建模标记', async () => {
  const m = machineFor([[0x32f, [[0, 3]]]]);
  m.scalars.set('Engine.d94385+1', 9);
  m.scalars.set('Engine.d94385+3', 7);
  await m.run();
  assert.equal(m.scalars.get('Engine.d94385+3'), 0, '索引 3 的那一格被清');
  assert.equal(m.scalars.get('Engine.d94385+1'), 9, '别的格不许动（按索引清，不是整块清）');
  assert.equal(m.effects.filter((e) => e.kind === 'container.request').length, 1);
  assert.deepEqual(m.diagnostics.map((d) => d.kind), ['call-target-unmodeled:sub_49A150']);
});
