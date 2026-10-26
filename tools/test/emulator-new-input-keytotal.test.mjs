/** @env pure @kind contract @why 0x100 读按帧上限而不是键总数 ⇒ 0xfe 与 0xff 的先后会改变派发结果（真机不会）；键总数初值 7 被实现成 0 则跨实现对照失效 */
/**
 * tools/test/emulator-new-input-keytotal.test.mjs —— `60-ops-input.md` 的
 * CORE-INPUT-WORD（键总数 / 采样）与 CORE-INPUT-KEY-DISPATCH（0x100 的扫描上界）守卫。
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - `0x100` 的扫描上界是**引擎的键总数本身**，不是 `0xff` 抄进帧里的那份快照
 *     ⇒ `0xfe 5 → 0xff → 0xfe 3` 之后，键 4 已经越界、**不许**再派发；
 *     用 `frame.keyLimit`（= 5）的旧实现会派发 ⇒ 第一条断言红。
 *   - 同一脚本把最后一个 `0xfe` 去掉（键总数仍 5）⇒ 键 4 **必须**派发。这一半防"输入字根本没置位"的假绿。
 *   - 引擎构造建立键总数初值 **7**（`Input[259] = 7`）⇒ `0xfe` 之前读它就是 7，不是 UB。
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

function machineFor(instrs, opts = {}) {
  return new Machine({ body: script(instrs), table, key: KEY, tickMs: 1, ...opts });
}

/** 可控输入后端：采样恒返回给定按键位图 */
function inputHost(mask) {
  return {
    sampleKeys: () => mask >>> 0,
    cursor: () => null,
    buttons: () => null,
    nextTouchPoint: () => null,
  };
}

const EXIT = 0x2;

test('★★ 0xfe 5 → 0xff → 0xfe 3 → 0x100：键 4 已越界，不再派发', async () => {
  // 布局（dword 位置）：0..2 = 0xfe 5；3 = 0xff；4..6 = 0xfe 3；7..11 = 0xfb(键 4 → 位置 13)；12 = 0x100；13 = 0x2
  const stale = machineFor([
    [0xfe, [[0, 5]]],
    [0xff, []],
    [0xfe, [[0, 3]]],
    [0xfb, [[0, 4], [0, 13]]],
    [0x100, []],
    [EXIT, []],
  ], { inputHost: inputHost(0b10000) });
  await stale.run();
  assert.equal(stale.input.keyTotal, 3, '键总数 ← 最后一次 0xfe 的 3');
  assert.equal(stale.frame.keyLimit, 5, '0xff 抄下的按帧上限仍是那一次的快照 5（它不跟着变）');
  assert.equal(stale.input.keyDispatches, 0,
    '扫描上界必须是键总数 3 ⇒ 键 4 越界；派发说明用了 frame.keyLimit 的旧快照 5');

  // 同形脚本、去掉第二个 0xfe ⇒ 键总数仍是 5、键 4 在范围内 ⇒ 必须派发（防"输入字清 0"造成的假绿）
  // 布局：0..2 = 0xfe 5；3 = 0xff；4..8 = 0xfb(键 4 → 位置 10)；9 = 0x100；10 = 0x2
  const inRange = machineFor([
    [0xfe, [[0, 5]]],
    [0xff, []],
    [0xfb, [[0, 4], [0, 10]]],
    [0x100, []],
    [EXIT, []],
  ], { inputHost: inputHost(0b10000) });
  await inRange.run();
  assert.equal(inRange.input.keyDispatches, 1, '键总数 5 ⇒ 键 4 在扫描范围内，必须派发（输入字确实置了位）');
  assert.equal(inRange.frame.keyCursor, 5, '派发后游标 = 找到的编号 + 1');
});

test('★ 未 0xfe 时键总数是引擎初值 7', async () => {
  const unset = machineFor([[0x100, []], [0x100, []], [0x1a8, []]]);
  await unset.run();
  assert.equal(unset.input.keyTotal, 7, '构造初值 7');
  assert.equal(unset.diagnostics.filter((d) => d.kind === 'key-total-unset').length, 0,
    '初值 7 是合同，不是 UB；⛔ 不再刷 key-total-unset');

  const sampled = machineFor([[0xff, []]], { inputHost: inputHost(0) });
  await sampled.run();
  assert.equal(sampled.frame.keyLimit, 7, '0xff 把初值 7 抄进按帧扫描上限');

  const established = machineFor([[0xfe, [[0, 2]]], [0xff, []], [0x100, []]], { inputHost: inputHost(0) });
  await established.run();
  assert.equal(established.input.keyTotal, 2, '0xfe 覆盖初值');
  assert.equal(established.frame.keyLimit, 2, '0xff 抄的是覆盖后的键总数');
});
