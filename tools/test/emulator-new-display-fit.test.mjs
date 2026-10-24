/** @env pure @kind contract @why 显示适配的算术写错（原点不向零截断、1/t 算成 CH/t、T=1/T=2 的缩放取错比值、A!=0 原点非 0、退化出口清 0 而不是保旧值），窗口里内容的位置 / 大小会差几个像素而执行器照跑 */
/**
 * tools/test/emulator-new-display-fit.test.mjs —— 显示适配的**纯函数**守卫
 * （`CORE-SCENE-DISPLAY-FIT`；实现 = `renderer/display-fit.ts`）
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md` 的「显示适配」主公式：
 *   VF 关 1:1 居中 · T=0 / 1 / 2 的分支（原点 / 视口 / 缩放 / 视图矩阵）· A != 0 原点归零 ·
 *   `trunc` 向零 · `half(x) = trunc(x*0.5)` · `canvasW = trunc(canvasH * (1/t))` · 退化出口保持旧值 ·
 *   以及三条不变量（T=0 恒 1:1 · T=1+A=0 缩放恒 1 · A!=0 原点 0）。
 *
 * ★ 纯函数、零依赖（无 Pixi / DOM / node）⇒ 直接 `import` 那个 `.ts`（Node v24 type stripping）。
 * ★ 守卫抓的是"实现自造的算术"：用 `floor` 代替 `trunc`、把 `1/t` 写成 `CH/t`、
 *   T=1 取 `min(kx,ky)` 而不是"被取到的那个比值"、退化出口清 0 —— 这些都**不会抛异常**，只有逐格断言会红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { computeDisplayFit } from '../../apps/emulator-new/src/platform/electron/renderer/display-fit.ts';

/* ─────────────────────────────── 助手 ─────────────────────────────── */

const IDENTITY4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

/** 对角缩放矩阵（列主序） */
const scalingOf = (sx, sy) => [sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

/** 只给 VF 关那支的输入（画布 / 后缓冲） */
const vfOff = (screenW, screenH, canvasW, canvasH) => computeDisplayFit({
  screenW,
  screenH,
  vfOn: false,
  vfType: 0,
  aspectMode: 0,
  refW: 0,
  refH: 0,
  canvasW,
  canvasH,
});

/** VF 开那支的输入（T / A / 参考尺寸） */
const vfOn = ({ screenW = 1280, screenH = 960, vfType, aspectMode = 0, refW = 1920, refH = 1080, canvasW = 1, canvasH = 1, previous } = {}) => computeDisplayFit({
  screenW,
  screenH,
  vfOn: true,
  vfType,
  aspectMode,
  refW,
  refH,
  canvasW,
  canvasH,
  ...(previous ? { previous } : {}),
});

/** 矩阵必须是**列主序** 4×4：只有对角线可以非 0，平移 / 透视格恒 0 */
function assertDiagonalMatrix(matrix, sx, sy) {
  assert.equal(matrix.length, 16, '视图矩阵是 4×4');
  const expected = scalingOf(sx, sy);
  for (let index = 0; index < 16; index += 1) {
    assert.equal(matrix[index], expected[index], `矩阵第 ${index} 格（列主序）`);
  }
}

/* ───────────────────────── 虚拟全屏关：1:1 + 居中 ───────────────────────── */

test('★ VF 关（vfOn = false）：缩放恒 (1,1)、原点 = 画布与后缓冲之差的一半、视图矩阵 = 单位阵', () => {
  const fit = vfOff(1280, 960, 1920, 1080);
  assert.equal(fit.canvasW, 1920, 'VF 关不写画布（画布是调用方摆好的）');
  assert.equal(fit.canvasH, 1080);
  assert.equal(fit.originX, 320, '(1920 - 1280) / 2');
  assert.equal(fit.originY, 60, '(1080 - 960) / 2');
  assert.equal(fit.viewportW, 1280, '视口 = 画布 - 2×原点');
  assert.equal(fit.viewportH, 960);
  assert.equal(fit.scaleX, 1);
  assert.equal(fit.scaleY, 1);
  assert.deepEqual(fit.view, IDENTITY4, 'VF 关：函数不写矩阵 ⇒ 单位阵');
  assert.equal(fit.degenerate, false);
});

test('★ VF 关：画布比后缓冲**小且差为奇数** ⇒ 原点**向零**截断（-11，⛔ 不是 floor 的 -12）', () => {
  // (1000 - 1023) / 2 = -11.5 ⇒ trunc = -11；floor 会给 -12（spec 的算术约定第 1 条）
  const fit = vfOff(1023, 960, 1000, 900);
  assert.equal(fit.originX, -11, '负半数向零 ⇒ -11');
  assert.equal(fit.originY, -30, '(900 - 960) / 2 = -30');
  assert.equal(fit.viewportW, 1022, '视口 = 1000 - 2×(-11) = 1022（不是 1023）');
  assert.equal(fit.viewportH, 960, '900 - 2×(-30) = 960');
});

test('★ VF 关：差为奇数且为正 ⇒ 原点 1.5 截断成 1（⛔ 不是四舍五入的 2）', () => {
  const fit = vfOff(1000, 1000, 1003, 1003);
  assert.equal(fit.originX, 1);
  assert.equal(fit.originY, 1);
  assert.equal(fit.viewportW, 1001, '1003 - 2×1');
});

/* ───────────────────────── T = 0（位置不缩放 / 矩阵单位阵） ───────────────────────── */

test('★ T=0 / A=0 · 高受限（rH <= rW）：画布 = cover(后缓冲, 参考比)，原点在 X、视口宽 = 后缓冲宽', () => {
  // 后缓冲 1280×960、参考 1920×1080 ⇒ t = 0.5625、CH = trunc(720) = 720 < 960 ⇒ 画布 (1706, 960)
  const fit = vfOn({ vfType: 0 });
  assert.equal(fit.canvasW, 1706, 'CW = trunc(960 × (1 / 0.5625)) = 1706');
  assert.equal(fit.canvasH, 960);
  assert.equal(fit.originX, 213, 'trunc((1706 - 1280×1) × 0.5)');
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1280, '1706 - 2×213');
  assert.equal(fit.viewportH, 960);
  assert.equal(fit.scaleX, 1, 'T=0 缩放恒 1');
  assert.equal(fit.scaleY, 1);
  assert.deepEqual(fit.view, IDENTITY4, 'T=0 视图矩阵恒单位阵');
});

test('★ T=0 / A=0 · 宽受限（rH > rW）：原点在 Y、视口高 = 后缓冲高', () => {
  // 后缓冲 1280×960、参考 1280×1080 ⇒ t = 0.84375、CH = 1080 >= 960 ⇒ 画布 (1280, 1080)
  const fit = vfOn({ vfType: 0, refW: 1280, refH: 1080 });
  assert.equal(fit.canvasW, 1280);
  assert.equal(fit.canvasH, 1080);
  assert.equal(fit.originX, 0);
  assert.equal(fit.originY, 60, 'trunc((1080 - 960×1) × 0.5)');
  assert.equal(fit.viewportW, 1280);
  assert.equal(fit.viewportH, 960);
  assert.deepEqual(fit.view, IDENTITY4);
});

test('★ A != 0：原点恒 (0,0)、画布 = 后缓冲（T=0/1），视口高 = 后缓冲高', () => {
  const fit = vfOn({ vfType: 0, aspectMode: 1 });
  assert.equal(fit.canvasW, 1280);
  assert.equal(fit.canvasH, 960);
  assert.equal(fit.originX, 0);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1280);
  assert.equal(fit.viewportH, 960);
  assert.equal(fit.scaleX, 1);
  assert.deepEqual(fit.view, IDENTITY4);
});

/* ───────────────────────── T = 1（缩放恒 1 + 矩阵 = Scaling） ───────────────────────── */

test('★ T=1 / A=0：缩放恒 (1,1)（I2），视图矩阵 = Scaling(ky = 1080/960 = 1.125)', () => {
  const fit = vfOn({ vfType: 1 });
  assert.equal(fit.canvasW, 1706, '画布与 T=0 同一份');
  assert.equal(fit.canvasH, 960);
  assert.equal(fit.originX, 213);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1280);
  assert.equal(fit.viewportH, 960);
  assert.equal(fit.scaleX, 1, 'I2：T=1 且 A=0 ⇒ 缩放恒 1（取到的比值恰为 1）');
  assert.equal(fit.scaleY, 1);
  assertDiagonalMatrix(fit.view, 1.125, 1.125);
});

test('★ T=1 / A != 0：视图矩阵 = Scaling(kx, ky) = (1.5, 1.125)，缩放 = 画布比值 (1,1)', () => {
  const fit = vfOn({ vfType: 1, aspectMode: 1 });
  assert.equal(fit.originX, 0);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1280);
  assert.equal(fit.viewportH, 960);
  assert.equal(fit.scaleX, 1);
  assert.equal(fit.scaleY, 1);
  assertDiagonalMatrix(fit.view, 1.5, 1.125);
});

/* ───────────────────────── T = 2（画布 = 参考尺寸，缩放 = min 比值） ───────────────────────── */

test('★ T=2 / A=0：画布 = 参考尺寸；缩放 = min(kx, ky) = 1.125；原点 = trunc((1920 - 1280×1.125)/2) = 240', () => {
  const fit = vfOn({ vfType: 2 });
  assert.equal(fit.canvasW, 1920);
  assert.equal(fit.canvasH, 1080);
  assert.equal(fit.originX, 240);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1440, '1920 - 2×240');
  assert.equal(fit.viewportH, 1080);
  assert.equal(fit.scaleX, 1.125);
  assert.equal(fit.scaleY, 1.125);
  assertDiagonalMatrix(fit.view, 1.125, 1.125);
});

test('★ T=2 / A != 0：缩放 = (kx, ky) = (1.5, 1.125)、原点 (0,0)、视口 = 参考尺寸、矩阵 = Scaling(1.5, 1.125)', () => {
  const fit = vfOn({ vfType: 2, aspectMode: 1 });
  assert.equal(fit.canvasW, 1920);
  assert.equal(fit.canvasH, 1080);
  assert.equal(fit.originX, 0);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 1920);
  assert.equal(fit.viewportH, 1080);
  assert.equal(fit.scaleX, 1.5);
  assert.equal(fit.scaleY, 1.125);
  assertDiagonalMatrix(fit.view, 1.5, 1.125);
});

/* ───────────────────────── 舍入：half / 1/t ───────────────────────── */

test('★ half(x) = trunc(x × 0.5)：1.875 截断成 1（⛔ 四舍五入会给 2）', () => {
  // 后缓冲 5×4、参考 5×1、T=2 ⇒ ky = 0.25 <= kx = 1：
  // ox = trunc((5 - 5×0.25) × 0.5) = trunc(1.875) = 1（round 会给 2、floor 也是 1 但这里只钉向零口径）
  const fit = vfOn({ screenW: 5, screenH: 4, vfType: 2, refW: 5, refH: 1 });
  assert.equal(fit.originX, 1);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 3, '5 - 2×1');
  assert.equal(fit.viewportH, 1);
  assert.equal(fit.scaleX, 0.25, 'min(kx, ky) = ky = 1/4');
  assertDiagonalMatrix(fit.view, 0.25, 0.25);
});

test('★ canvasW = trunc(canvasH × (1/t))：算的是 1/t 再乘（边界上差 1 像素）', () => {
  // 后缓冲 1920×770、参考 1921×770 ⇒ t = 770/1921、CH = trunc(1920×t) = 769 < 770
  // ⇒ CH = 770、CW = trunc(770 × (1921/770)) = 1920；若写成 trunc(770 / t) 会得到 1921
  const fit = vfOn({ screenW: 1920, screenH: 770, vfType: 0, refW: 1921, refH: 770 });
  assert.equal(fit.canvasH, 770);
  assert.equal(fit.canvasW, 1920, '1/t 再乘 ⇒ 1920（⛔ 不是 CH / t 的 1921）');
  assert.equal(fit.originX, 0);
  assert.equal(fit.viewportW, 1920, '画布宽跟着 1920 走（写成 CH/t 时这里会是 1921）');
  assert.equal(fit.viewportH, 770);
});

/* ───────────────────────── 退化出口（T ∉ {0,1,2}） ───────────────────────── */

test('★ 退化出口（T=3）：只把视图矩阵置单位阵，原点 / 视口 / 缩放**保持旧值**（⛔ 不是清 0）', () => {
  const previous = { originX: 7, originY: 9, viewportW: 100, viewportH: 50, scaleX: 2, scaleY: 3 };
  const fit = vfOn({ vfType: 3, canvasW: 111, canvasH: 222, previous });
  assert.equal(fit.degenerate, true, '退化出口要能被调用方识别（否则会把旧值当新值覆盖）');
  assert.equal(fit.canvasW, 111, '画布保持旧值（函数不写它）');
  assert.equal(fit.canvasH, 222);
  assert.equal(fit.originX, 7, '原点保持旧值');
  assert.equal(fit.originY, 9);
  assert.equal(fit.viewportW, 100);
  assert.equal(fit.viewportH, 50);
  assert.equal(fit.scaleX, 2);
  assert.equal(fit.scaleY, 3);
  assert.deepEqual(fit.view, IDENTITY4, '退化出口只写矩阵（= 单位阵）');
});

test('★ 退化出口且没有上一轮 ⇒ 中性值（原点 0 / 视口 = 画布 / 缩放 1），仍标 degenerate', () => {
  const fit = vfOn({ vfType: -1, canvasW: 640, canvasH: 480 });
  assert.equal(fit.degenerate, true);
  assert.equal(fit.originX, 0);
  assert.equal(fit.originY, 0);
  assert.equal(fit.viewportW, 640);
  assert.equal(fit.viewportH, 480);
  assert.equal(fit.scaleX, 1);
  assert.equal(fit.scaleY, 1);
  assert.deepEqual(fit.view, IDENTITY4);
});

/* ───────────────────────── 三条不变量（随机样例） ───────────────────────── */

test('★ 不变量：A=0 ⇒ vw = 画布宽 - 2×原点；A!=0 ⇒ 原点 (0,0) 且 T=0/1 画布高 = 后缓冲高；T=1+A=0 ⇒ 缩放 1', () => {
  // 线性同余（确定性 ⇒ 失败可复现），不用 Math.random
  let seed = 0x2f6e2b1;
  const next = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed;
  };
  for (let round = 0; round < 300; round += 1) {
    const screenW = 320 + (next() % 3000);
    const screenH = 240 + (next() % 2000);
    const refW = 320 + (next() % 4000);
    const refH = 240 + (next() % 3000);
    const vfType = next() % 3;
    const aspectMode = next() % 2;
    const fit = vfOn({ screenW, screenH, vfType, aspectMode, refW, refH });
    const label = `round ${round}: ${screenW}x${screenH} / ref ${refW}x${refH} / T=${vfType} A=${aspectMode}`;

    assert.equal(fit.canvasW > 0, true, `${label} 画布宽必须为正`);
    assert.equal(fit.canvasH > 0, true, `${label} 画布高必须为正`);
    if (aspectMode === 0) {
      assert.equal(fit.viewportW, fit.canvasW - 2 * fit.originX, `${label} 视口宽 = 画布宽 - 2×原点X`);
      assert.equal(fit.viewportH, fit.canvasH - 2 * fit.originY, `${label} 视口高 = 画布高 - 2×原点Y`);
    } else {
      assert.equal(fit.originX, 0, `${label} A!=0 ⇒ 原点 X = 0`);
      assert.equal(fit.originY, 0, `${label} A!=0 ⇒ 原点 Y = 0`);
      if (vfType !== 2) assert.equal(fit.canvasH, screenH, `${label} A!=0 且 T=0/1 ⇒ 画布高 = 后缓冲高`);
    }
    if (vfType === 0) {
      assert.equal(fit.scaleX, 1, `${label} T=0 ⇒ 缩放 X 恒 1`);
      assert.equal(fit.scaleY, 1, `${label} T=0 ⇒ 缩放 Y 恒 1`);
      assert.deepEqual(fit.view, IDENTITY4, `${label} T=0 ⇒ 视图矩阵恒单位阵`);
    }
    if (vfType === 1 && aspectMode === 0) {
      assert.equal(fit.scaleX, 1, `${label} T=1 且 A=0 ⇒ 缩放 X 恒 1`);
      assert.equal(fit.scaleY, 1, `${label} T=1 且 A=0 ⇒ 缩放 Y 恒 1`);
    }
    assert.equal(fit.view.length, 16, `${label} 视图矩阵是 4×4`);
    for (let index = 0; index < 16; index += 1) {
      const diagonal = index === 0 || index === 5 || index === 10 || index === 15;
      if (!diagonal) assert.equal(fit.view[index], 0, `${label} 矩阵非对角格 ${index} 必须为 0（只有缩放）`);
    }
    assert.equal(fit.view[10], 1, `${label} 第三轴不缩放`);
    assert.equal(fit.view[15], 1, `${label} 齐次分量`);
  }
});
