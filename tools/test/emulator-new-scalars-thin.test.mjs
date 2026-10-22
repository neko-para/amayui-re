/** @env pure @kind contract @why 0x149/0x21b/0x252/0x88 的写槽、0/1 归一与分支写错都不会报错，只会让引擎里那几格的值静默错掉 */
/**
 * tools/test/emulator-new-scalars-thin.test.mjs —— 引擎级标量薄写（`60-ops-scalar.md` 的 CORE-OPS-SCALAR-THIN / PAIR-88）守卫
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - **写的是哪个格**：槽名与知识层（`ENGINE_SCALAR_WRITES`）里的稳定身份一致；
 *     写错格子不报错、只在有人读它时才表现成"另一套状态"；
 *   - **0x21b 的归一**：非 0 必须得到 1（不是原值、不是 −1）；
 *   - **0x88 的两支互斥**：非 0 不碰状态字、为 0 不写第三格；
 *   - **清位是读-改-写**：只清第 27 位，别的位必须原样留下（写成整字 0 会踩掉别的位）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { REPO_ROOT } from '../lib/paths.mjs';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

/** 把 [opcode, [[type, raw], ...]] 拼成一段 body（不含头） */
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

test('★ 0x149 / 0x21b / 0x252：各写自己的那个格（0x21b 归一到 0/1）', async () => {
  const m = machineFor([
    [0x149, [[0, 1000]]],
    [0x252, [[0, 3]]],
  ]);
  await m.run();
  assert.equal(m.scalars.get('Engine.d97058'), 1000, '0x149 原样写');
  assert.equal(m.scalars.get('Engine.d92323'), 3, '0x252 原样写');

  for (const [input, want] of [[5, 1], [0, 0], [-3, 1], [0x7fffffff, 1]]) {
    const b = machineFor([[0x21b, [[0, input]]]]);
    await b.run();
    assert.equal(b.scalars.get('Engine.d166965'), want, `0x21b 输入 ${input} ⇒ ${want}`);
  }
});

test('★ 三条的槽名必须是知识层里的稳定身份（写错槽名在这条会红）', async () => {
  const layout = fs.readFileSync(path.join(REPO_ROOT, 'packages/age-format/src/engine/layout.mts'), 'utf8');
  const m = machineFor([[0x149, [[0, 1]]], [0x21b, [[0, 1]]], [0x252, [[0, 1]]]]);
  await m.run();
  for (const name of m.scalars.keys()) {
    assert.match(name, /^Engine\.d\d+$/, `槽名形态不对：${name}`);
    assert.ok(layout.includes(`'${name}'`), `知识层没有这个稳定身份：${name}`);
  }
  assert.deepEqual([...m.scalars.keys()].sort(), ['Engine.d166965', 'Engine.d92323', 'Engine.d97058']);
});

test('★ 0x88 非 0 支：两个格都写 op1，第三格写常量 1，状态字不动', async () => {
  // 门非 0 时 run 不执行指令 ⇒ 直接调 handler；先把第 27 位置起，验证"非 0 支不许碰它"
  const m = machineFor([]);
  m.flags = 0x08000000;
  const handler = m.handlers.get(0x88);
  handler(m, [{ type: 0, raw: 7 }]);
  assert.equal(m.scalars.get('Engine.d1415'), 7);
  assert.equal(m.scalars.get('Engine.d97050'), 7);
  assert.equal(m.scalars.get('Engine.d122368'), 1, '非 0 ⇒ 第三格 ← 常量 1（不是 op1）');
  assert.equal(m.flags, 0x08000000, '非 0 支不许碰状态选择字');
});

test('★ 0x88 为 0 支：两个格写 0，只清状态字第 27 位，不写第三格', async () => {
  // ① 门关着时这条指令**根本不执行**（run 在 flags≠0 时只服务状态位）⇒ 这本身是证据。
  //    ★ 门位不服务时 run 现在**返回 Stop**（`kind:'gate'`）而不是抛：那是"这里没建模"，不是执行出错。
  const gated = machineFor([[0x88, [[0, 0]]]]);
  gated.flags = 0x08000000 | 0x00000200;
  const stop = (await gated.run()).stop;
  assert.equal(stop.kind, 'gate', '门非 0 时不许执行指令');
  assert.equal(stop.kind === 'gate' ? stop.bit : 0, 0x08000000 | 0x00000200, '报告里带上未服务的门位');

  // ② 直接调 handler 才测得准「为 0」支
  const direct = machineFor([]);
  direct.flags = 0x08000000 | 0x00000200;
  const handler = direct.handlers.get(0x88);
  handler(direct, [{ type: 0, raw: 0 }]);
  assert.equal(direct.scalars.get('Engine.d1415'), 0);
  assert.equal(direct.scalars.get('Engine.d97050'), 0);
  assert.equal(direct.scalars.has('Engine.d122368'), false, '为 0 支不许写第三格');
  assert.equal(direct.flags, 0x00000200, '只清第 27 位，别的位必须原样留下');
});
