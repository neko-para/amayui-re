/** @env pure @kind contract @why 纹理族的引擎数据写错了（记录字段 / 槽 / 绘制项四条边 / 脏标志），而执行器仍然"跑得下去" */
/**
 * tools/test/emulator-new-texture.test.mjs —— 纹理族（0x1f7..0x1fb）的守卫
 *
 * 覆盖 `data/spec/chapters/60-ops.md` 的纹理族 5 条规则。用合成脚本字节，不依赖游戏安装。
 *
 * ★ 判据为什么是这些：
 *   - `0x1f9` 的颜色键归一化（负数 ⇒ 0、否则补不透明黑）写错不会报错，只会颜色不对；
 *   - `0x1fa` 只清来源文件、**不清**空白创建标记（清了就把"这份纹理是空白创建的"抹掉）；
 *   - `0x1fb` 的矩形存的是**四条边**（右/下 = x+宽 / y+高），存成宽高不会报错；
 *   - `0x1f7` 的区间是**左闭右开**，且"≤1"是单键路径。
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

test('★ 0x1f9 设置纹理：记录 = 文件号 / 归一化颜色键 / 空白创建 0；槽上有普通纹理', () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0x123456]]]]);
  m.run();
  assert.deepEqual(m.scene.textures.get(5), { file: 21061, color: 0xff123456, blank: 0 });
  assert.equal(m.scene.textureObjects.get(5)?.kind, 'normal');
});

test('★ 0x1f9 颜色键：负数 ⇒ 0（不是 0xff000000）', () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, -1]]]]);
  m.run();
  assert.equal(m.scene.textures.get(5)?.color, 0);
});

test('★ 0x1f9 先清引擎级对象槽（失败路径上对象槽已经空了）', () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 1]]]]);
  m.scene.objectSlots.set(5, { old: true });
  m.run();
  assert.equal(m.scene.objectSlots.has(5), false);
});

test('★ 0x1f8 创建纹理：记录 = 无来源文件 / 空白创建 1；种类 3 ⇒ 分割纹理', () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 1280], [0, 720], [0, 3]]]]);
  m.run();
  assert.deepEqual(m.scene.textures.get(7), { file: -1, color: 0, blank: 1 });
  assert.deepEqual(m.scene.textureObjects.get(7), { kind: 'divided', width: 1280, height: 720 });
});

test('★ 0x1f8 种类 ≠ 3 ⇒ 普通纹理（只有两分支，不是枚举）', () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 4], [0, 4], [0, 0]]]]);
  m.run();
  assert.equal(m.scene.textureObjects.get(7)?.kind, 'normal');
});

test('★ 0x1fa 释放纹理：来源文件 → 无、槽清空，但**空白创建标记不动**', () => {
  const m = machineFor([[0x1fa, [[0, 7]]]]);
  m.scene.textures.set(7, { file: 21061, color: 0xff, blank: 1 });
  m.scene.textureObjects.set(7, { kind: 'normal', width: 1, height: 1 });
  m.run();
  assert.deepEqual(m.scene.textures.get(7), { file: -1, color: 0xff, blank: 1 });
  assert.equal(m.scene.textureObjects.has(7), false);
});

test('★ 0x1fb 绘制纹理：矩形存**四条边**（右 = x+宽、下 = y+高），不是宽高', () => {
  const m = machineFor([[0x1fb, [
    [0, 200000], [0, 42],
    [0, 100], [0, 50], [0, 1280], [0, 720],
    [0, 10], [0, 20],
  ]]]);
  m.run();
  const item = m.scene.drawItems.get(200000);
  assert.ok(item);
  assert.equal(item.flags & 1, 1, '有效位');
  assert.equal(item.textureId, 42);
  assert.deepEqual([item.left, item.top, item.right, item.bottom], [100, 50, 1380, 770],
    '右/下 = x+宽 / y+高（不是宽高）');
  assert.deepEqual(item.aux, [10, 20, 0]);
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1fb 不检查纹理是否存在（画不存在的纹理照常记账）', () => {
  const m = machineFor([[0x1fb, [[0, 200001], [0, 999], [0, 1], [0, 2], [0, 3], [0, 4], [0, 0], [0, 0]]]]);
  m.run();
  assert.equal(m.scene.drawItems.get(200001)?.textureId, 999);
});

test('★ 0x1f7 拆离：count ≤ 1 是单键；删到东西才置脏', () => {
  const m = machineFor([[0x1f7, [[0, 200000], [0, 1]]]]);
  m.scene.drawItems.set(200000, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  m.scene.drawItems.set(200001, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  m.scene.dirty = 0;
  m.run();
  assert.equal(m.scene.drawItems.has(200000), false);
  assert.equal(m.scene.drawItems.has(200001), true, '单键路径只删一个');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1f7 拆离：count > 1 删**左闭右开** [键, 键+个数)', () => {
  const m = machineFor([[0x1f7, [[0, 200000], [0, 2]]]]);
  for (const key of [200000, 200001, 200002]) {
    m.scene.drawItems.set(key, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  }
  m.run();
  assert.deepEqual([...m.scene.drawItems.keys()].sort((a, b) => a - b), [200002], '200002 不在区间里');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1f7 什么都没删 ⇒ 脏标记不动（不是"执行过就置脏"）', () => {
  const m = machineFor([[0x1f7, [[0, 123], [0, 1]]]]);
  m.scene.dirty = 0;
  m.run();
  assert.equal(m.scene.dirty, 0);
});
