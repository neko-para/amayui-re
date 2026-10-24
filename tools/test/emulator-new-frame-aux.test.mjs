/** @env pure @kind contract @why 装载 / 重载帧时不把两条本地块进入目标重置成 −1 ⇒ 新脚本会跳到上一份脚本留下的位置（跨脚本串跑）且不报错 */
/**
 * tools/test/emulator-new-frame-aux.test.mjs —— `60-ops-frame.md` 的
 * CORE-OPS-FRAME-LOCAL-SET-7B / CORE-OPS-FRAME-LOAD-FRAME 的「初值与重置 = −1」守卫。
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - 两格是**按帧**的、初值 **−1**（"未登记"哨兵）；引擎的读者一律与 −1 比较。
 *     模型用 `undefined` 表示未登记 ⇒ 新帧上两格不是 −1，第一条断言红；
 *     `undefined` 还会让 `if (frame.localMain)` 把合法的位置 0 当成"未登记"。
 *   - 帧被**重载**（同一个槽装新脚本）时两格必须重置 ⇒ 上一份脚本登记的 10 / 20 不许留到新帧。
 *   - `0x7b` 仍然覆盖两格（先断言它写进去了，再断言重载把它们清回 −1）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';

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

const NOP = 0x1a8;

test('★ 新装载的帧 / 根帧：两条本地块进入目标都是 −1（未登记哨兵，不是 undefined）', async () => {
  const child = script([[0x2, []]]);
  const root = script([[0x6, [[0, 1], [0, 2]]], [NOP, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? child : null) });
  await m.run({ maxSteps: 1 });

  assert.ok(m.slots[2], '装载落在 2 号槽（防测试自己的布局写错）');
  assert.equal(m.slots[2].frame.localMain, -1, '装载帧把主进入目标重置成 −1');
  assert.equal(m.slots[2].frame.localSub, -1, '装载帧把副进入目标重置成 −1');
  assert.equal(m.frame.localMain, -1, '根帧（freshFrame）同样是未登记');
  assert.equal(m.frame.localSub, -1);
});

test('★★ 重载帧：0x7b 写下的 10 / 20 被重置回 −1（新脚本不许跳到上一份脚本的位置）', async () => {
  // 根：0x6 装 slot 2 → 0x8 切到 2 → 0x6 再装 slot 2 → NOP
  // 子：0x7b(10, 20) → 0x2（槽 2 不相邻于父号 0 ⇒ 返回时不释放，仍留在槽上）
  const child = script([[0x7b, [[0, 10], [0, 20]]], [0x2, []]]);
  const root = script([
    [0x6, [[0, 1], [0, 2]]],
    [0x8, [[0, 2]]],
    [0x6, [[0, 1], [0, 2]]],
    [NOP, []],
  ]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? child : null) });

  // 前四步：装帧 → 换帧 → 0x7b → 0x2（返回根帧）。此刻两格是被 0x7b 写下的值。
  await m.run({ maxSteps: 4 });
  assert.equal(m.slots[2].frame.localMain, 10, '0x7b 仍然覆盖两格（主）');
  assert.equal(m.slots[2].frame.localSub, 20, '0x7b 仍然覆盖两格（副）');

  await m.run();
  assert.equal(m.slots[2].frame.localMain, -1, '重载把主进入目标重置成 −1（留着 10 ⇒ 红）');
  assert.equal(m.slots[2].frame.localSub, -1, '重载把副进入目标重置成 −1（留着 20 ⇒ 红）');
});
