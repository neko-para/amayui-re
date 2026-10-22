/** @env pure @kind contract @why 纹理族的引擎数据 / 像素加载 / 绘制项来源矩形写错了，而执行器仍然"跑得下去" */
/**
 * tools/test/emulator-new-texture.test.mjs —— 纹理族（0x1f7..0x1fb）的守卫
 *
 * 覆盖 `data/spec/chapters/60-ops-texture.md` 的 6 条规则。用合成脚本字节 + 合成 AGF，不依赖游戏安装。
 *
 * ★ 判据为什么是这些：
 *   - `0x1f9` 的颜色键归一化（负数 ⇒ 0、否则补不透明黑）写错不会报错，只会颜色不对；
 *   - `0x1f9` 的纹理尺寸必须来自解码图像，不是 0×0；
 *   - `0x1f9` 的失败态：解码失败槽为空、上传失败槽里留对象，两者都只把来源写成 -1；
 *   - `0x1f8` 只写来源 / 空白位，**颜色键不能被清**；
 *   - `0x1fa` 只清来源文件、**不清**空白创建标记（清了就把"这份纹理是空白创建的"抹掉）；
 *   - `0x1fb` 的四条边是**来源矩形**，两个浮点是**目标位置**，存成目标矩形不会报错；
 *   - `0x1f7` 的区间是**左闭右开**，且"≤1"是单键路径。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HeadlessRenderHost } from '../../apps/emulator-new/src/core/render.ts';

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

/** 合成一张 2×2、8bpp、带 ACIF alpha 的 AGF（像素索引：左上 1、右上 2、左下 3、右下 0） */
function syntheticAgf() {
  const width = 2;
  const height = 2;
  const stride = 4;
  const meta = Buffer.alloc(1080);
  meta.writeUInt32LE(width, 20);
  meta.writeUInt32LE(height, 24);
  meta.writeInt16LE(8, 30);
  // 调色板：0 黑、1 红、2 绿、3 蓝（B,G,R,0）
  const palette = [[0, 0, 0], [0, 0, 255], [0, 255, 0], [255, 0, 0]];
  palette.forEach(([b, g, r], i) => {
    meta[56 + i * 4] = b;
    meta[56 + i * 4 + 1] = g;
    meta[56 + i * 4 + 2] = r;
    meta[56 + i * 4 + 3] = 0;
  });
  const body = Buffer.alloc(stride * height); // bottom-up
  const rows = [[3, 0], [1, 2]]; // 先写盘上底行，再写顶行
  rows.forEach((row, i) => {
    body[i * stride] = row[0];
    body[i * stride + 1] = row[1];
  });
  const alpha = Buffer.from([255, 128, 64, 0]);
  const acif = Buffer.alloc(36 + alpha.length);
  acif.write('ACIF', 0, 'latin1');
  acif.writeUInt32LE(2, 4);
  acif.writeUInt32LE(alpha.length, 12);
  acif.writeUInt32LE(width, 16);
  acif.writeUInt32LE(height, 20);
  acif.writeUInt32LE(alpha.length, 24);
  acif.writeUInt32LE(alpha.length, 28);
  acif.writeUInt32LE(alpha.length, 32);
  alpha.copy(acif, 36);
  const header = Buffer.alloc(24);
  header.write('ACGF', 0, 'latin1');
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(0, 8);
  header.writeUInt32LE(meta.length, 12);
  header.writeUInt32LE(meta.length, 16);
  header.writeUInt32LE(meta.length, 20);
  const bodyHeader = Buffer.alloc(12);
  bodyHeader.writeUInt32LE(body.length, 4);
  bodyHeader.writeUInt32LE(body.length, 8);
  return Buffer.concat([header, meta, bodyHeader, body, acif]);
}

const AGF = syntheticAgf();

function machineFor(instrs, opts = {}) {
  return new Machine({
    body: script(instrs),
    table,
    key: KEY,
    loadImage: async () => AGF,
    ...opts,
  });
}

test('★ 0x1f9 设置纹理：记录 / 解码尺寸 / 上传请求都来自同一张图', async () => {
  const host = new HeadlessRenderHost();
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0x123456]]]], { renderHost: host });
  await m.run();
  assert.deepEqual(m.scene.textures.get(5), { file: 21061, color: 0xff123456, blank: 0 });
  assert.deepEqual(m.scene.textureObjects.get(5), { kind: 'normal', width: 2, height: 2 });
  const upload = host.requests.find((r) => r.kind === 'upload-texture');
  assert.ok(upload, '成功路径必须发一次上传请求');
  assert.deepEqual(
    [upload.request.width, upload.request.height, upload.request.color, upload.request.rgba.length],
    [2, 2, 0xff123456, 16],
    '宽度/高度来自解码图像；颜色键归一化后原样交给平台；RGBA 长度 = w×h×4',
  );
});

test('★ 0x1f9 载入不同尺寸的图像：纹理尺寸跟着图像头走（不是上一次的尺寸）', async () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 1280], [0, 720], [0, 0]]], [0x1f9, [[0, 21061], [0, 7], [0, 0]]]]);
  await m.run();
  assert.deepEqual(m.scene.textureObjects.get(7), { kind: 'normal', width: 2, height: 2 });
});

test('★ 0x1f9 颜色键：负数 ⇒ 0（不是 0xff000000）', async () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, -1]]]]);
  await m.run();
  assert.equal(m.scene.textures.get(5)?.color, 0);
});

test('★ 0x1f9 先清引擎级对象槽（失败路径上对象槽已经空了）', async () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 1]]]]);
  m.scene.objectSlots.set(5, { old: true });
  await m.run();
  assert.equal(m.scene.objectSlots.has(5), false);
});

test('★ 0x1f9 取件失败：只有共享对象槽被清，记录 / 纹理槽不动', async () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0]]]], {
    loadImage: async () => null,
  });
  m.scene.textures.set(5, { file: 999, color: 0x11223344, blank: 1 });
  m.scene.textureObjects.set(5, { kind: 'normal', width: 7, height: 8 });
  m.scene.objectSlots.set(5, { old: true });
  await assert.rejects(() => m.run());
  assert.equal(m.scene.objectSlots.has(5), false, '共享对象槽在读文件前就清掉');
  assert.deepEqual(m.scene.textures.get(5), { file: 999, color: 0x11223344, blank: 1 }, '取件失败不改记录');
  assert.deepEqual(m.scene.textureObjects.get(5), { kind: 'normal', width: 7, height: 8 }, '取件失败不动纹理槽');
});

test('★ 0x1f9 解码失败：旧纹理槽被清空、记录来源 = 无、颜色 / 空白位保持', async () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0x445566]]]], {
    loadImage: async () => Buffer.from([0, 1, 2, 3]),
  });
  m.scene.textures.set(5, { file: 999, color: 0x11223344, blank: 1 });
  m.scene.textureObjects.set(5, { kind: 'normal', width: 7, height: 8 });
  await assert.rejects(() => m.run());
  assert.equal(m.scene.textureObjects.has(5), false, '解码前清旧对象 ⇒ 失败后槽为空');
  assert.deepEqual(m.scene.textures.get(5), { file: -1, color: 0x11223344, blank: 1 });
});

test('★ 0x1f9 上传失败：槽里留下新对象、记录来源 = 无、颜色 / 空白位保持', async () => {
  const host = new HeadlessRenderHost({ uploadTextureSucceeds: false });
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0x445566]]]], { renderHost: host });
  m.scene.textures.set(5, { file: 999, color: 0x11223344, blank: 1 });
  await assert.rejects(() => m.run());
  assert.deepEqual(m.scene.textureObjects.get(5), { kind: 'normal', width: 2, height: 2 });
  assert.deepEqual(m.scene.textures.get(5), { file: -1, color: 0x11223344, blank: 1 });
});

test('★ 0x1f9 没有文件装载器：响亮失败，不伪造成功记录', async () => {
  const m = machineFor([[0x1f9, [[0, 21061], [0, 5], [0, 0]]]], { loadImage: undefined });
  m.scene.textures.set(5, { file: 999, color: 0x11223344, blank: 1 });
  await assert.rejects(() => m.run(), /texture-image-loader-missing/);
  assert.deepEqual(m.scene.textures.get(5), { file: 999, color: 0x11223344, blank: 1 });
});

test('★ 0x1f8 创建纹理：记录 = 无来源文件 / 空白创建 1；种类 3 ⇒ 分割纹理', async () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 1280], [0, 720], [0, 3]]]]);
  await m.run();
  assert.deepEqual(m.scene.textures.get(7), { file: -1, color: 0, blank: 1 });
  assert.deepEqual(m.scene.textureObjects.get(7), { kind: 'divided', width: 1280, height: 720 });
});

test('★ 0x1f8 种类 ≠ 3 ⇒ 普通纹理（只有两分支，不是枚举）', async () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 4], [0, 4], [0, 0]]]]);
  await m.run();
  assert.equal(m.scene.textureObjects.get(7)?.kind, 'normal');
});

test('★ 0x1f8 只写来源 / 空白位：颜色键不能被清零', async () => {
  const m = machineFor([[0x1f8, [[0, 7], [0, 4], [0, 4], [0, 0]]]]);
  m.scene.textures.set(7, { file: 123, color: 0x11223344, blank: 0 });
  await m.run();
  assert.deepEqual(m.scene.textures.get(7), { file: -1, color: 0x11223344, blank: 1 });
});

test('★ 0x1f8 平台创建失败：错误通道有文本、对象仍留在槽里（尺寸不可用）', async () => {
  const host = new HeadlessRenderHost({ createTextureSucceeds: false });
  const m = machineFor([[0x1f8, [[0, 7], [0, 4], [0, 4], [0, 0]]]], { renderHost: host });
  await m.run();
  assert.notEqual(m.scene.errorBuffer, '');
  assert.deepEqual(m.scene.textureObjects.get(7), { kind: 'normal', width: 0, height: 0 });
});

test('★ 0x1fa 释放纹理：来源文件 → 无、槽清空，但**空白创建标记不动**', async () => {
  const m = machineFor([[0x1fa, [[0, 7]]]]);
  m.scene.textures.set(7, { file: 21061, color: 0xff, blank: 1 });
  m.scene.textureObjects.set(7, { kind: 'normal', width: 1, height: 1 });
  await m.run();
  assert.deepEqual(m.scene.textures.get(7), { file: -1, color: 0xff, blank: 1 });
  assert.equal(m.scene.textureObjects.has(7), false);
});

test('★ 0x1fb 绘制纹理：四条边是 left/top/宽/高算出的边，两个浮点是另外两个位', async () => {
  const m = machineFor([[0x1fb, [
    [0, 200000], [0, 42],
    [0, 100], [0, 50], [0, 1280], [0, 720],
    [0, 10], [0, 20],
  ]]]);
  await m.run();
  const item = m.scene.drawItems.get(200000);
  assert.ok(item);
  assert.equal(item.flags & 1, 1, '有效位');
  assert.equal(item.textureId, 42);
  assert.deepEqual([item.left, item.top, item.right, item.bottom], [100, 50, 1380, 770],
    '右/下 = 来源 left/top + 宽/高（不是目标宽高）');
  assert.deepEqual(item.aux, [10, 20, 0], '第 7/8 个操作数 = 目标位置 x/y');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1fb 不检查纹理是否存在（画不存在的纹理照常记账）', async () => {
  const m = machineFor([[0x1fb, [[0, 200001], [0, 999], [0, 1], [0, 2], [0, 3], [0, 4], [0, 0], [0, 0]]]]);
  await m.run();
  assert.equal(m.scene.drawItems.get(200001)?.textureId, 999);
});

test('★ 0x1f7 拆离：count ≤ 1 是单键；删到东西才置脏', async () => {
  const m = machineFor([[0x1f7, [[0, 200000], [0, 1]]]]);
  m.scene.drawItems.set(200000, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  m.scene.drawItems.set(200001, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  m.scene.dirty = 0;
  await m.run();
  assert.equal(m.scene.drawItems.has(200000), false);
  assert.equal(m.scene.drawItems.has(200001), true, '单键路径只删一个');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1f7 拆离：count > 1 删**左闭右开** [键, 键+个数)', async () => {
  const m = machineFor([[0x1f7, [[0, 200000], [0, 2]]]]);
  for (const key of [200000, 200001, 200002]) {
    m.scene.drawItems.set(key, { flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0] });
  }
  await m.run();
  assert.deepEqual([...m.scene.drawItems.keys()].sort((a, b) => a - b), [200002], '200002 不在区间里');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x1f7 什么都没删 ⇒ 脏标记不动（不是"执行过就置脏"）', async () => {
  const m = machineFor([[0x1f7, [[0, 123], [0, 1]]]]);
  m.scene.dirty = 0;
  await m.run();
  assert.equal(m.scene.dirty, 0);
});
