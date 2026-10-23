/** @env pure @kind contract @why TITLE 的 Version 数字靠 0x23b 画：源格子 / 目标步长 / 前导零 / 三种对齐任一算错 ⇒ 数字错位、重叠或整段消失 */
/**
 * tools/test/emulator-new-cg-digit.test.mjs —— `0x23b`（CG 数字条）几何的纯函数守卫
 *
 * ★ 为什么这条守卫存在：TITLE 的 `Version 0.00.0000` **不是**预烘焙位图 ——
 *   `draw-texture 76/77` 的源矩形 `1342,811,12,15` 是**小数点**格子；七位数字来自
 *   `i2da`（记录）+ 三条 `i23b`（位数 1 / 2 / 4）。把 76/77 当数字或把 `0x23b` 留在
 *   未建模白名单，画面就只剩 `Version . .`（两个点）。
 *
 * 夹具是 TITLE.BIN 的真实记录：`i2da 0 4 54c 32b c f 2 (local-int 40d)`，
 * 其中 `sub (local-int 40d) 0 3` ⇒ 字距 = -3；三条 `i23b` 的 x = 962 / 975 / 997。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { cgDigitItems, cgDigitRecordOf } from '../../apps/emulator-new/src/core/cg-digit.ts';

/** TITLE.BIN 的 `0x2da` 记录：纹理 4 / x0=0x54c / y0=0x32b / 12×15 / 字内空隙 2 / 字距 -3 */
const TITLE_RECORD = [4, 0x54c, 0x32b, 12, 15, 2, -3];

test('★ TITLE 记录：0x54c=1356 是数字格起点、14 是源步长、9 是目标步长（12+(-3)）', () => {
  const rec = cgDigitRecordOf(TITLE_RECORD);
  assert.deepEqual(
    [rec.textureId, rec.x0, rec.y0, rec.cellWidth, rec.cellHeight, rec.innerGap, rec.advanceGap],
    [4, 1356, 811, 12, 15, 2, -3],
    '0x2da 七格的顺序是 [纹理槽, x0, y0, 单字宽, 字高, 字内空隙, 字距]',
  );
  assert.equal(rec.cellWidth + rec.innerGap, 14, '源格子步长 = 单字宽 + 字内空隙（数字 0,1,2… 在 1356,1370,1384…）');
  assert.equal(rec.cellWidth + rec.advanceGap, 9, '目标步长 = 单字宽 + 字距（TITLE 的字距是 -3）');
});

test('★ TITLE 的三条 0x23b（位数 1/2/4，flags=1）：七位数字的键与位置', () => {
  const seg1 = cgDigitItems(TITLE_RECORD, 0x6f, 0, 1, 1, 962, 247);
  const seg2 = cgDigitItems(TITLE_RECORD, 0x70, 0, 2, 1, 975, 247);
  const seg3 = cgDigitItems(TITLE_RECORD, 0x72, 0, 4, 1, 997, 247);

  assert.equal(seg1.length + seg2.length + seg3.length, 7, '1+2+4 位全部画出（flags bit0 = 补前导零）');
  assert.deepEqual([...seg1, ...seg2, ...seg3].map((it) => it.handle), [111, 112, 113, 114, 115, 116, 117],
    '绘制键从 op1 起连续编号；118/119 是两个小数点的静态绘制项');
  assert.deepEqual(
    [...seg1, ...seg2, ...seg3].map((it) => it.dstX),
    // 右对齐：个位在 x + k*9（k = 位数-1 → 0），所以每段从右往左读
    [962, 984, 975, 1024, 1015, 1006, 997],
    '个位在段的最右格，高位向左；段首 x 是字段左边界',
  );
  assert.deepEqual(
    [...seg1, ...seg2, ...seg3].map((it) => it.dstY),
    Array(7).fill(247),
  );
  for (const item of [...seg1, ...seg2, ...seg3]) {
    assert.equal(item.textureId, 4, '数字条纹理槽来自记录 [0]');
    assert.deepEqual([item.srcLeft, item.srcTop, item.srcRight, item.srcBottom], [1356, 811, 1368, 826],
      '补零时每一位都是数字 0 的格子（x0=1356，宽 12，高 15）');
  }
});

test('★ 数字点位置：76/77 的源矩形 1342,811,12,15 是小数点格子，不是数字 0', () => {
  // 数字 0 的格子从 1356 起；1342..1354 在真实纹理里只有小数点（见 SO004.AGF 的 x=1346..1350）。
  const zero = cgDigitItems(TITLE_RECORD, 118, 0, 1, 1, 969, 247)[0];
  assert.equal(zero.srcLeft, 1356, '数字 0 从记录 x0=1356 起，而不是 1342');
  assert.notEqual(zero.srcLeft, 1342, '1342 是小数点格子的起点；把它当数字是缺陷的成因之一');
});

test('★ 前导零：只有右对齐（无 bit1/bit2）才看 flags bit0', () => {
  // 值 5、字段 3 位：个位永远画；十位/百位在 flags bit0=0 时跳过。
  const right = cgDigitItems(TITLE_RECORD, 10, 5, 3, 0, 100, 7);
  assert.deepEqual(right.map((it) => it.handle), [10], '只画个位 ⇒ 绘制键也从 10 起连续编号');
  assert.equal(right[0].dstX, 100 + 2 * 9, '个位在第 3 格（k=2）');
  assert.equal(right[0].srcLeft, 1356 + 14 * 5, '源格子按数字 5 取（1356+70=1426）');

  const zeroFilled = cgDigitItems(TITLE_RECORD, 10, 5, 3, 1, 100, 7);
  assert.deepEqual(zeroFilled.map((it) => it.handle), [10, 11, 12]);
  assert.deepEqual(zeroFilled.map((it) => it.srcLeft), [1356 + 14 * 5, 1356, 1356],
    '建项顺序是个位 → 高位：个位取数字 5，补零的高位取数字 0');

  // ★ 高位非零但中间位为 0：value=1000、4 位 ⇒ 四位全画（最高位的 1 不能被"前导零"逻辑吃掉）。
  //   删掉条件里的 `remaining !== 0` 会只画 3 位（"000"），这条必须红。
  const thousand = cgDigitItems(TITLE_RECORD, 20, 1000, 4, 0, 100, 7);
  assert.equal(thousand.length, 4, '1000 的四位都要画');
  assert.deepEqual(thousand.map((it) => it.handle), [20, 21, 22, 23]);
  assert.deepEqual(thousand.map((it) => it.srcLeft), [1356, 1356, 1356, 1356 + 14], '个/十/百位是 0，千位是 1');
  assert.equal(thousand[3].dstX, 100, '千位落在字段最左格');

  // 居中 / 左对齐分支不补前导零（引擎 0x424AA9 / 0x424BE4 只判 k==last || 剩余值 != 0）。
  assert.equal(cgDigitItems(TITLE_RECORD, 10, 5, 3, 2, 100, 7).length, 1, '居中：仍只有个位');
  assert.equal(cgDigitItems(TITLE_RECORD, 10, 5, 3, 4, 100, 7).length, 1, '左对齐：仍只有个位');
});

test('★ 对齐：居中 / 左对齐的 x 与右对齐不同（同一数值、同一字段）', () => {
  // 值 5、3 位：digitCount=1、last=2、advance=9。
  const right = cgDigitItems(TITLE_RECORD, 0, 5, 3, 0, 100, 7)[0];
  const centered = cgDigitItems(TITLE_RECORD, 0, 5, 3, 2, 100, 7)[0];
  const left = cgDigitItems(TITLE_RECORD, 0, 5, 3, 4, 100, 7)[0];
  assert.equal(right.dstX, 118, '右对齐：k=2 ⇒ 100+18');
  assert.equal(centered.dstX, 109, '居中：100+18-trunc((2-1+1)*9/2)=100+18-9');
  assert.equal(left.dstX, 100, '左对齐：最高位在字段左边界（dc-1-(last-k)=0）');
});

test('★ digits <= 0 ⇒ 不建项（引擎 0x424A70 的 js 分支）', () => {
  assert.deepEqual(cgDigitItems(TITLE_RECORD, 5, 123, 0, 1, 10, 20), []);
  assert.deepEqual(cgDigitItems(TITLE_RECORD, 5, 123, -1, 1, 10, 20), []);
});
