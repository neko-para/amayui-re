/** @env pure @kind contract @why 把"注释/未使用"实现成了会读操作数或会写状态的东西（看起来更"有用"，但与引擎不符） */
/**
 * tools/test/emulator-new-noop.test.mjs —— 空操作（0x1a7 / 0x1a8）的守卫
 *
 * 覆盖 `data/spec/chapters/60-ops-wait.md` 的「空操作」：两条都**不读操作数**、不写任何引擎状态，
 * 只让分派器按 `2·argc+1` 推进。
 *
 * ★ 判据为什么是这些：
 *   - `0x1a7` 带一个操作数，但载荷的类型标签**不受支持**时也**不许抛** —— 一抛就说明它真的去读了；
 *   - 两条都**不许**改池 / 场景 / 外部请求 —— "看着像 no-op" 不等于"没有任何写"。
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

test('★ 0x1a7 不读操作数：载荷类型不受支持也不抛，长度 = 4 + 8×1 = 12', async () => {
  const m = new Machine({ body: script([[0x1a7, [[0x6, 0xdead]]]]), table, key: KEY });
  const r = await m.run();
  assert.equal(r.steps, 1);
  assert.equal(r.ip, 12);
  assert.equal(r.stop.kind, 'truncated');
  assert.deepEqual(m.diagnostics, []);
  assert.deepEqual(m.effects, []);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.globalInt.size, 0);
  assert.equal(m.globalFloat.size, 0);
});

test('★ 0x1a8：长度 = 4（argc 0），同样不写任何状态', async () => {
  const m = new Machine({ body: script([[0x1a8, []]]), table, key: KEY });
  const r = await m.run();
  assert.equal(r.steps, 1);
  assert.equal(r.ip, 4);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.globalInt.size + m.globalFloat.size, 0);
});
