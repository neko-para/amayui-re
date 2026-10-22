/** @env pure @kind contract @why AGF 解码的字节结构 / 行宽 / 行序 / 通道 / alpha / 颜色键写错时，纹理仍"有尺寸"但像素全错 */
/**
 * tools/test/emulator-new-agf.test.mjs —— `30-agf.md` 的解码守卫
 *
 * 合成 AGF（不依赖游戏安装），钉住四件最容易静默错的事：
 *   ① 行宽按 **32 位（4 字节）对齐**（8bpp 1026 宽 ⇒ 1028 字节/行）；
 *   ② 盘上 bottom-up，解出来是 top-down；
 *   ③ 8bpp 调色板从 meta+56 起、24bpp BGR、32bpp BGRA，ACIF alpha 覆盖；
 *   ④ 颜色键在上传期把命中像素整条清 0。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { AgfError, agfStride, applyColorKey, decodeAgf } from '../../apps/emulator-new/src/core/agf.ts';

function buildAgf({ width, height, bpp, body, alpha, version = 2, palette = null }) {
  const stride = agfStride(width, bpp);
  // 允许夹具故意给短 body（坏件测试）；正常用例仍按 stride×height 传
  void stride;
  const meta = Buffer.alloc(1080);
  meta.writeUInt32LE(width, 20);
  meta.writeUInt32LE(height, 24);
  meta.writeInt16LE(bpp, 30);
  if (palette) {
    palette.forEach(([b, g, r], i) => {
      meta[56 + i * 4] = b;
      meta[56 + i * 4 + 1] = g;
      meta[56 + i * 4 + 2] = r;
      meta[56 + i * 4 + 3] = 0;
    });
  }
  const header = Buffer.alloc(24);
  header.write('ACGF', 0, 'latin1');
  header.writeUInt32LE(version, 4);
  header.writeUInt32LE(0, 8);
  header.writeUInt32LE(meta.length, 12);
  header.writeUInt32LE(meta.length, 16);
  header.writeUInt32LE(meta.length, 20);
  const bodyHeader = Buffer.alloc(12);
  bodyHeader.writeUInt32LE(body.length, 4);
  bodyHeader.writeUInt32LE(body.length, 8);
  const parts = [header, meta, bodyHeader, body];
  if (version === 2) {
    const a = alpha ?? Buffer.alloc(width * height);
    const acif = Buffer.alloc(36 + a.length);
    acif.write('ACIF', 0, 'latin1');
    acif.writeUInt32LE(2, 4);
    acif.writeUInt32LE(a.length, 12);
    acif.writeUInt32LE(width, 16);
    acif.writeUInt32LE(height, 20);
    acif.writeUInt32LE(a.length, 24);
    acif.writeUInt32LE(a.length, 28);
    acif.writeUInt32LE(a.length, 32);
    a.copy(acif, 36);
    parts.push(acif);
  }
  return Buffer.concat(parts);
}

const PALETTE = [[0, 0, 0], [0, 0, 255], [0, 255, 0], [255, 0, 0]];

test('★ 8bpp：bottom-up / 调色板 / ACIF alpha 都按规范解成 top-down RGBA', () => {
  const body = Buffer.alloc(8); // 2×2，stride 4；盘上底行 [3,0]，顶行 [1,2]
  body[0] = 3; body[1] = 0;
  body[4] = 1; body[5] = 2;
  const agf = buildAgf({
    width: 2, height: 2, bpp: 8, body, palette: PALETTE,
    alpha: Buffer.from([255, 128, 64, 0]),
  });
  const image = decodeAgf(agf);
  assert.deepEqual([image.width, image.height, image.bpp], [2, 2, 8]);
  assert.deepEqual([...image.rgba], [
    255, 0, 0, 255,      // 左上 = 索引 1 红，alpha 255
    0, 255, 0, 128,      // 右上 = 索引 2 绿，alpha 128
    0, 0, 255, 64,       // 左下 = 索引 3 蓝，alpha 64
    0, 0, 0, 0,          // 右下 = 索引 0 黑，alpha 0
  ]);
});

test('★ 8bpp 宽 1026：行宽按 32 位对齐 ⇒ stride 1028', () => {
  assert.equal(agfStride(1026, 8), 1028, '去掉行对齐这条会红');
  const body = Buffer.alloc(1028);
  body[0] = 1;
  const image = decodeAgf(buildAgf({ width: 1026, height: 1, bpp: 8, body, palette: PALETTE, alpha: Buffer.alloc(1026, 255) }));
  assert.equal(image.rgba.length, 1026 * 4);
});

test('★ 24bpp：BGR → RGBA，且无 ACIF 的 version 1 不读 alpha', () => {
  const body = Buffer.alloc(8); // 2×1，stride 8（6 字节像素 + 2 字节填充）
  // 盘上只有一行；top-down 与 bottom-up 相同。左像素 B=10,G=20,R=30，右 B=40,G=50,R=60
  body.set([10, 20, 30, 40, 50, 60], 0);
  const image = decodeAgf(buildAgf({ width: 2, height: 1, bpp: 24, body, version: 1 }));
  assert.deepEqual([...image.rgba], [30, 20, 10, 255, 60, 50, 40, 255]);
});

test('★ 32bpp：ACIF alpha 覆盖 body 的 alpha', () => {
  const body = Buffer.alloc(8); // 2×1
  body.set([1, 2, 3, 10, 4, 5, 6, 20], 0);
  const image = decodeAgf(buildAgf({
    width: 2, height: 1, bpp: 32, body,
    alpha: Buffer.from([200, 0]),
  }));
  assert.deepEqual([...image.rgba], [3, 2, 1, 200, 6, 5, 4, 0]);
});

test('★ 颜色键：命中 RGB 的像素整条清 0（0 = 黑色键）', () => {
  const rgba = Uint8Array.from([0, 0, 0, 255, 255, 0, 0, 255, 1, 2, 3, 4]);
  applyColorKey(rgba, 0);
  assert.deepEqual([...rgba], [0, 0, 0, 0, 255, 0, 0, 255, 1, 2, 3, 4]);
  applyColorKey(rgba, 0xffff0000);
  assert.deepEqual([...rgba], [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4]);
});

test('★ 坏件：BM / 非法版本 / body 长度不符 / ACIF alpha 短都抛 AgfError', () => {
  const bmp = Buffer.alloc(32);
  bmp.write('BM', 0, 'latin1');
  assert.throws(() => decodeAgf(bmp), (e) => e instanceof AgfError && e.code === 'agf-bmp-unsupported');
  assert.throws(() => decodeAgf(Buffer.from('NOPE0000000000000000000000', 'latin1')), (e) => e instanceof AgfError);
  const badBody = buildAgf({ width: 2, height: 2, bpp: 8, body: Buffer.alloc(4), palette: PALETTE });
  assert.throws(() => decodeAgf(badBody), (e) => e instanceof AgfError && e.code === 'agf-body-size');
  const shortAlpha = buildAgf({
    width: 2, height: 1, bpp: 8, body: Buffer.alloc(4), palette: PALETTE,
    alpha: Buffer.alloc(1),
  });
  assert.throws(() => decodeAgf(shortAlpha), (e) => e instanceof AgfError && e.code === 'agf-alpha-size');
});
