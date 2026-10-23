/** @env assets @kind contract @why `.STH` 的格式合同（BMP 头字段 / 声明长度差 14 / 行宽对齐）与真机产物脱节 ⇒ 读者按错的字节结构写解码器，缩略图读不回来 */
/**
 * tools/test/spec-save-sth.assets.test.mjs —— `FMT-SAVE-STH` 的**真实样本**守卫
 *
 * 判据来自 `corpus/fixtures/SAVE*.STH` 本身（真机产物），不是从实现反推：
 * ① 两段头：`BM` / 14 字节文件头 / 40 字节信息头 / 无调色板 / 无尾段；
 * ② 声明长度（像素字节数 + 40）与实际长度（像素字节数 + 54）**差 14** —— 这是引擎写侧的行为，
 *    ⛔ 不许在样本上"修正"；
 * ③ 宽高 / 位深 / 压缩 / `biSizeImage` / 平面数 / 每行 4 字节对齐。
 * ★ **行序（自下而上）不在此断言**：样本里没有任何"方向标记"能机械判出首行是画面顶还是底
 *   （四份缩略图的上下两半亮度、UI 位置都随场景变），把它写成断言只会得到一条"看起来在验方向"的恒真判据。
 *   行序的锚是引擎写侧的倒序行指针与读侧的翻正（见 `bindings.json` 的 `FMT-SAVE-STH`），不是本文件。
 * ★ 行末填充同理不可断言：四份样本宽 320 ⇒ `320 × 3 = 960` 已 4 字节对齐，样本里没有填充字节。
 * ★ 样本缺席（或只有 LFS 指针）⇒ skip，不是红。
 * ★ 合成侧的空值/失败路径在 `emulator-new-ops-100-200.test.mjs`；本条只负责"真实字节"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const FIXTURES = path.join(REPO_ROOT, 'corpus', 'fixtures');
const NAMES = ['SAVE76.STH', 'SAVE77.STH', 'SAVE78.STH', 'SAVE79.STH'];

/** LFS 指针只有一百多字节；它和"文件不在场"一样，不能当真样本用。 */
const present = (name) => {
  const file = path.join(FIXTURES, name);
  return fs.existsSync(file) && fs.statSync(file).size > 4096 ? file : null;
};
const skip = present(NAMES[0]) ? false : 'corpus/fixtures/SAVE76.STH 不在场（或只是 LFS 指针）';

const u16 = (b, o) => b.readUInt16LE(o);
const u32 = (b, o) => b.readUInt32LE(o);
const i32 = (b, o) => b.readInt32LE(o);

/** 按 `FMT-SAVE-STH` 的判据读一份 `.STH` 的头部（偏移只活在本守卫里，正文不写偏移）。 */
function head(file) {
  const b = fs.readFileSync(file);
  return {
    bytes: b,
    type: b.subarray(0, 2).toString('latin1'),
    declaredSize: u32(b, 2),
    reserved1: u16(b, 6),
    reserved2: u16(b, 8),
    pixelStart: u32(b, 10),
    infoSize: u32(b, 14),
    width: i32(b, 18),
    height: i32(b, 22),
    planes: u16(b, 26),
    bitCount: u16(b, 28),
    compression: u32(b, 30),
    sizeImage: u32(b, 34),
    xPelsPerMeter: u32(b, 38),
    yPelsPerMeter: u32(b, 42),
    colorsUsed: u32(b, 46),
    colorsImportant: u32(b, 50),
  };
}

/** 每行字节数 = 宽 × 位深按 32 位对齐（`4` 字节对齐）。 */
const strideOf = (h) => Math.ceil((h.width * h.bitCount) / 32) * 4;

test('★ SAVE76.STH：BMP 头字段与声明/实际长度', { skip }, () => {
  const h = head(present(NAMES[0]));
  const stride = strideOf(h);
  const pixels = stride * h.height;

  assert.equal(h.type, 'BM', '文件头类型');
  assert.equal(h.infoSize, 40, '信息头长度');
  assert.equal(h.width, 320, '宽');
  assert.equal(h.height, 180, '高（正数 ⇒ 自下而上）');
  assert.equal(h.planes, 1, '平面数');
  assert.equal(h.bitCount, 24, '位深');
  assert.equal(h.compression, 0, '压缩 = BI_RGB');
  assert.equal(h.sizeImage, 0, '图像字节数字段 = 0');
  assert.equal(h.colorsUsed, 0, '调色板计数 = 0（24 位无调色板）');
  assert.equal(h.colorsImportant, 0);
  assert.equal(h.xPelsPerMeter, 0);
  assert.equal(h.yPelsPerMeter, 0);
  assert.equal(h.reserved1, 0);
  assert.equal(h.reserved2, 0);
  assert.equal(h.pixelStart, 54, '像素数据起始 = 两段头长');

  // ★ 这是引擎写侧的行为：声明「像素字节数 + 40」，真实长度却是「像素字节数 + 54」。
  assert.equal(h.declaredSize, 40 + pixels, '声明的文件长度 = 像素字节数 + 40（不是 +54）');
  assert.equal(h.bytes.length, 54 + pixels, '真实长度 = 54 + 像素字节数');
  assert.equal(h.bytes.length - h.declaredSize, 14, '声明与实际差 14 字节');

  // 行按 4 字节对齐：320 × 3 = 960 已对齐 ⇒ 本样本无行末填充。
  assert.equal(stride, 960);
  assert.equal(stride % 4, 0, '行字节数 4 字节对齐');
  assert.equal(pixels, 172800);
});

test('★ 四个槽的 .STH 结构一致：声明长度恒为像素 + 40、实际恒为像素 + 54', { skip }, () => {
  for (const name of NAMES) {
    const file = present(name);
    if (!file) {
      assert.fail(`${name} 缺席（同目录其它样本在场 ⇒ 这个样本被删了）`);
    }
    const h = head(file);
    const pixels = strideOf(h) * h.height;
    assert.equal(h.type, 'BM', `${name} 类型`);
    assert.equal(h.declaredSize, 40 + pixels, `${name} 声明长度`);
    assert.equal(h.bytes.length, 54 + pixels, `${name} 实际长度`);
    assert.equal(h.bitCount, 24, `${name} 位深`);
  }
});

test('★ `.STH` 文件在像素块之后结束（无尾段 / 无调色板）', { skip }, () => {
  const h = head(present(NAMES[0]));
  const header = 54;
  // 信息头之后、像素之前没有调色板：像素第一行不是全 0 的 4×256 调色板结构。
  assert.equal(h.pixelStart, header, '像素数据起始紧接两段头');
  // 像素块长度 = 行字节数 × 高；文件不得多出尾段。
  assert.equal(h.bytes.length - h.pixelStart, strideOf(h) * h.height);
});
