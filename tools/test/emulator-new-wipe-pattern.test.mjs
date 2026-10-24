/** @env pure @kind contract @why 种类 2 的 12 种抹除图案把"步长三档 / 步序相位 / 条带网格锚点 / 每带钳在本带内 / 两块画布的先后"写错时，画面会"少切一条带、多让一道、或把第二块画布铺满整幅"，而执行器照跑 —— 只有对着笔的矩形断言才会红 */
/**
 * tools/test/emulator-new-wipe-pattern.test.mjs —— 种类 2 的 **12 种抹除图案**守卫
 * （`CORE-OPS-SCENE-COMMAND-RECORDS`「12 种抹除图案（条件表）」；实现 = `wipe-pattern.ts`）
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md` §种类 2 的图案段（草稿 `draft-rule.md` / `impl-notes.md`）：
 *   一族 0..3：`步数 = ⌊轴长/S⌋+2`、`步长 = D/步数`、`让出量 = S × 步序`（1/3 步序 +1）；
 *   二族 4..7：`条带数 = ⌊轴长/S⌋+1`、`步长 = D/(S+1)`、每带让出量 = 步序（像素）且**钳在本带内**；
 *   三族 8..11：`步长 = D/(S+条带数)`、第 i 带让出量 = `步序 − i`（非正 ⇒ 第 2 笔空）；
 *   条带网格：最后一带贴远边 / 第 0 带贴远边两种锚点、`余数 = 轴长 mod S`、总覆盖多出 ≤ S−1；
 *   每区域**恰好 2 笔**、**先画布 A 后画布 B**、两笔相接不重叠、12 模式都用两块画布。
 *
 * ★ 本文件是 **pure**：被测的是纯函数，矩形是手写常量；不碰 Pixi / DOM / 游戏安装。
 * ★ 守卫抓的是"实现自造的规则"，这些都不会抛异常：把三档步长写成同一档、把相位 `+1` 写成 `round`、
 *   把条带网格"按 S 均分"（丢掉余数偏移）、漏掉"钳在本带内"、把 `duration = 0` 自行定义成"立即完成"。
 * ★ **不**断言的东西（spec 明确未定 / 不在本层）：矩形之外的可见后果（U2）、空矩形在绘制入口的行为（U3）、
 *   两块画布的领域名（U4）、落点（视野偏移）与混合 / 颜色（这些不随模式变）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { wipePattern } from '../../apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts';

/* ─────────────────────────────── 夹具与独立算式的助手（**不调用**被测代码） ─────────────────────────────── */

/** 一个矩形（右下不含） */
const rect = (left, top, right, bottom) => ({ left, top, right, bottom });

/** 默认夹具：800 × 400 的抹除矩形、分割幅 8、持续时间 1000ms（spec 草稿里的 falsifier 用的就是这套） */
const BASE = { rect: rect(0, 0, 800, 400), split: 8, duration: 1000 };

const widthOf = (r) => r.right - r.left;
const heightOf = (r) => r.bottom - r.top;
const area = (r) => Math.max(0, widthOf(r)) * Math.max(0, heightOf(r));

/** 两个矩形的交集面积（"不重叠"= 0） */
const overlapArea = (a, b) =>
  area(rect(
    Math.max(a.left, b.left),
    Math.max(a.top, b.top),
    Math.min(a.right, b.right),
    Math.min(a.bottom, b.bottom),
  ));

/** 沿**宽**的图案（竖条带 / 左右方向的整幅族）：0,1,4,5,8,9 */
const alongWidth = (mode) => mode % 4 < 2;

/** 该模式的轴长（宽或高） */
const axisLengthOf = (mode, r) => (alongWidth(mode) ? widthOf(r) : heightOf(r));

/** 该模式的**步长**（三档公式，守卫这里独立算一遍） */
const stepOf = (mode, r = BASE.rect, split = BASE.split, duration = BASE.duration) => {
  const length = axisLengthOf(mode, r);
  if (mode <= 3) return duration / (Math.floor(length / split) + 2);
  const stripeCount = Math.floor(length / split) + 1;
  return mode <= 7 ? duration / (split + 1) : duration / (split + stripeCount);
};

/** 条带族的**条带数** */
const stripeCountOf = (mode, r = BASE.rect, split = BASE.split) =>
  Math.floor(axisLengthOf(mode, r) / split) + 1;

/**
 * 第 `k` 步的时刻：取 `步长 × (k + 0.5)`（**半步**，免得浮点误差把 ⌊·⌋ 打偏一格）。
 * ★ 需要"整步"数值的用例单独写 `步长 × k`（实测这些 k 上浮点恰好精确）。
 */
const at = (mode, k, over = {}) => ({ ...BASE, mode, elapsed: stepOf(mode, over.rect, over.split, over.duration) * (k + 0.5), ...over });

/** 结果里第 i 条带的那一对笔（一族只有一对） */
const pairAt = (result, i = 0) => [result.strokes[i * 2], result.strokes[i * 2 + 1]];

/** 两条笔"相接"：交集为空 且 至少共享一条边（零宽 / 零高的退化笔同样成立） */
const touches = (a, b) =>
  overlapArea(a, b) === 0
  && (a.right === b.left || a.left === b.right || a.bottom === b.top || a.top === b.bottom);

/* ─────────────────────────────── 12 模式各一例 ─────────────────────────────── */

test('★ 12 模式各一例：模式 0 用 S=8 / 宽 800 / 经过时间 = 步长×10 ⇒ 让出 80 像素', () => {
  // 每行：模式 / 取第几步 / 看哪一对笔（一族恒 0）/ 该笔对的 A、B 矩形（**手算常量**，不是实现算出来的）
  const cases = [
    { mode: 0, k: 10, band: 0, a: rect(80, 0, 800, 400), b: rect(0, 0, 80, 400) },   // ⌊800/8⌋+2 = 102 步 ⇒ 让出 8×10
    { mode: 1, k: 10, band: 0, a: rect(0, 0, 712, 400), b: rect(712, 0, 800, 400) },   // 相位 +1 ⇒ 让出 8×11
    { mode: 2, k: 5, band: 0, a: rect(0, 40, 800, 400), b: rect(0, 0, 800, 40) },      // ⌊400/8⌋+2 = 52 步 ⇒ 让出 8×5
    { mode: 3, k: 5, band: 0, a: rect(0, 0, 800, 352), b: rect(0, 352, 800, 400) },    // 相位 +1 ⇒ 让出 8×6
    { mode: 4, k: 3, band: 1, a: rect(3, 0, 8, 400), b: rect(0, 0, 3, 400) },          // 第 1 带 [0,8)，让出 3
    { mode: 5, k: 3, band: 0, a: rect(800, 0, 805, 400), b: rect(805, 0, 808, 400) },  // 第 0 带 [800,808) 贴远边
    { mode: 6, k: 3, band: 1, a: rect(0, 3, 800, 8), b: rect(0, 0, 800, 3) },          // 第 1 带 [0,8)，让出 3
    { mode: 7, k: 3, band: 0, a: rect(0, 400, 800, 405), b: rect(0, 405, 800, 408) },  // 第 0 带 [400,408) 贴远边
    { mode: 8, k: 3, band: 1, a: rect(2, 0, 8, 400), b: rect(0, 0, 2, 400) },          // 第 1 带晚 1 ⇒ 让出 3−1
    { mode: 9, k: 3, band: 0, a: rect(800, 0, 805, 400), b: rect(805, 0, 808, 400) },  // 第 0 带晚 0 ⇒ 让出 3
    { mode: 10, k: 3, band: 1, a: rect(0, 2, 800, 8), b: rect(0, 0, 800, 2) },         // 第 1 带晚 1 ⇒ 让出 2
    { mode: 11, k: 3, band: 0, a: rect(0, 400, 800, 405), b: rect(0, 405, 800, 408) }, // 第 0 带晚 0 ⇒ 让出 3
  ];

  for (const { mode, k, band, a, b } of cases) {
    const result = wipePattern(at(mode, k));
    assert.deepEqual(result.diagnostics, [], `模式 ${mode} 不该有诊断`);
    const expectedCount = mode <= 3 ? 2 : 2 * stripeCountOf(mode);
    assert.equal(result.strokes.length, expectedCount, `模式 ${mode} 的笔数：一族 2 笔 / 条带族每带 2 笔`);
    const [first, second] = pairAt(result, band);
    assert.equal(first.canvas, 'A', `模式 ${mode} 的第 ${band} 带必须**先**画布 A`);
    assert.equal(second.canvas, 'B', `模式 ${mode} 的第 ${band} 带必须**后**画布 B`);
    assert.deepEqual(first.rect, a, `模式 ${mode} 画布 A 的矩形错`);
    assert.deepEqual(second.rect, b, `模式 ${mode} 画布 B（让出区）的矩形错`);
  }
});

/* ─────────────────────────────── 一族：相位 ─────────────────────────────── */

test('★ 一族：模式 1 / 3 比模式 0 / 2 多一道（相位是 ⌊p⌋+1，不是四舍五入、也不是 ceil）', () => {
  // 从"一步都没走"（elapsed 0）开始：模式 0 / 2 让出 0（第二笔零宽），模式 1 / 3 已经让出一道
  const zero = wipePattern({ ...BASE, mode: 0, elapsed: 0 });
  assert.deepEqual(pairAt(zero)[1].rect, rect(0, 0, 0, 400), '模式 0 在 elapsed 0 时让出量 0 ⇒ 第二笔零宽');
  assert.deepEqual(pairAt(zero)[0].rect, rect(0, 0, 800, 400), '模式 0 在 elapsed 0 时第一笔仍覆盖整幅');

  const one = wipePattern({ ...BASE, mode: 1, elapsed: 0 });
  assert.deepEqual(pairAt(one)[1].rect, rect(792, 0, 800, 400), '模式 1 在 elapsed 0 时已让出 1 道（8 px）');
  assert.deepEqual(pairAt(one)[0].rect, rect(0, 0, 792, 400), '模式 1 的第一笔从右缘退 1 道');

  // 竖直方向同理（轴 = 高）
  const two = wipePattern({ ...BASE, mode: 2, elapsed: 0 });
  assert.deepEqual(pairAt(two)[1].rect, rect(0, 0, 800, 0), '模式 2 在 elapsed 0 时让出量 0');
  const three = wipePattern({ ...BASE, mode: 3, elapsed: 0 });
  assert.deepEqual(pairAt(three)[1].rect, rect(0, 392, 800, 400), '模式 3 在 elapsed 0 时已让出 1 道（从下缘向上）');

  // 双数模式走同一时刻的对比：模式 1 恰好比模式 0 多一道（8 px），⛔ 不是"同宽"
  const step = stepOf(0);
  const mode0 = wipePattern({ ...BASE, mode: 0, elapsed: step * (6 + 0.5) });
  const mode1 = wipePattern({ ...BASE, mode: 1, elapsed: step * (6 + 0.5) });
  const reveal0 = widthOf(pairAt(mode0)[1].rect);
  const reveal1 = widthOf(pairAt(mode1)[1].rect);
  assert.equal(reveal0, 48, '模式 0：第 6 步 ⇒ 6 × 8 = 48 px');
  assert.equal(reveal1, reveal0 + 8, '模式 1 必须比模式 0 **多一道**（相位 +1）');
});

/* ─────────────────────────────── 二族：同相 + 钳位 ─────────────────────────────── */

test('★ 二族：逐像素同相（101 带同时推进），且每条带钳在本带内 —— 步序 > S 后不再增长', () => {
  const result = wipePattern(at(4, 8)); // 步序 8 = S ⇒ 每条带恰好让满
  for (let i = 0; i < stripeCountOf(4); i += 1) {
    const [a, b] = pairAt(result, i);
    assert.equal(widthOf(b.rect), 8, `第 ${i} 带应让满 8 px（同相）`);
    assert.equal(widthOf(a.rect), 0, `第 ${i} 带让满后第一笔退化成零宽（空矩形，U3）`);
    assert.equal(widthOf(a.rect) + widthOf(b.rect), 8, `第 ${i} 带的两笔加起来恰是本带宽 ${8}`);
  }

  // 步序远超 S：clamp 到本带宽 ⇒ 与"刚好让满"逐笔相同（⛔ 不许把让出区画进相邻带）
  const overfilled = wipePattern(at(4, 12));
  assert.deepEqual(overfilled.strokes, result.strokes, '步序 > S 时每带被钳在本带内（不是越界增长）');

  // 未走到 S 时：每条带让出量都等于步序（同相，不等于 0 也不等于 S）
  const partial = wipePattern(at(4, 3));
  for (let i = 0; i < stripeCountOf(4); i += 1) {
    const [a, b] = pairAt(partial, i);
    assert.equal(widthOf(b.rect), 3, `第 ${i} 带让出 3 px`);
    assert.equal(widthOf(a.rect), 5, `第 ${i} 带剩下的 5 px 给画布 A`);
    // 钳在本带内：两笔的并集恰好是一条宽 S 的带
    assert.equal(Math.max(a.rect.right, b.rect.right) - Math.min(a.rect.left, b.rect.left), 8,
      `第 ${i} 带的两笔不越出本带`);
  }
});

/* ─────────────────────────────── 三族：错相 ─────────────────────────────── */

test('★ 三族：第 i 条带晚 i 个**像素步**（不是晚 i 道）；让出量非正 ⇒ 第 2 笔空', () => {
  const result = wipePattern(at(8, 2)); // 步序 2 ⇒ 第 0 带 2 px、第 1 带 1 px、第 2 带起 0
  const [a0, b0] = pairAt(result, 0);
  const [a1, b1] = pairAt(result, 1);
  const [a2, b2] = pairAt(result, 2);
  assert.equal(widthOf(b0.rect), 2, '第 0 带：让出量 = 步序 − 0 = 2');
  assert.equal(widthOf(b1.rect), 1, '第 1 带：让出量 = 步序 − 1 = 1（晚一个**像素**步，⛔ 不是 8）');
  assert.equal(widthOf(b2.rect), 0, '第 2 带：让出量非正 ⇒ 第 2 笔为空');
  assert.equal(widthOf(a1.rect), 7, '第 1 带的第一笔 = 本带剩下的 7 px');
  assert.equal(widthOf(a2.rect), 8, '第 2 带的第一笔仍覆盖整带（一笔都还没让出）');

  // 与二族同一时刻对比：二族每带同宽，三族第 1 带少 1 px（"错相"的判据）
  const inPhase = wipePattern(at(4, 2));
  assert.equal(widthOf(pairAt(inPhase, 1)[1].rect), 2, '二族第 1 带与第 0 带同宽（同相）');
  assert.equal(widthOf(b1.rect), 1, '三族第 1 带比第 0 带少 1 px（错相）');

  // 竖直方向同样晚 i（模式 10）：第 1 带的让出区从带上缘往下 1 px
  const vertical = wipePattern(at(10, 2));
  assert.deepEqual(pairAt(vertical, 1)[1].rect, rect(0, 0, 800, 1), '模式 10 第 1 带让出 1 px');

  // 让出量随时间推移逐带解锁：步序 4 ⇒ 第 0..3 带各有 4/3/2/1 px，第 4 带起为 0
  const later = wipePattern(at(8, 4));
  const reveals = [0, 1, 2, 3, 4].map((i) => widthOf(pairAt(later, i)[1].rect));
  assert.deepEqual(reveals, [4, 3, 2, 1, 0], '第 i 带的让出量 =「步序 − i」，且非正 = 空');
});

/* ─────────────────────────────── 条带网格：余数 ─────────────────────────────── */

test('★ 条带网格余数：宽 805 / S=8 ⇒ 余数 5、条带数 101；越出量 `S−余数` 落在**动画起点那一侧**', () => {
  const wide = rect(0, 0, 805, 400);
  const split = 8;

  // 4（左→右）：最后一带右缘 = 矩形右缘 ⇒ 第 0 带左缘 = 左 − (S−余数) = −3
  const forward = wipePattern({ ...BASE, mode: 4, rect: wide, split, elapsed: stepOf(4, wide, split) * 0.5 });
  const [firstA] = pairAt(forward, 0);
  assert.equal(firstA.rect.left, -3, '模式 4：第 0 带的左缘越出矩形左 `S − 余数` = 3');
  const [lastA, lastB] = pairAt(forward, stripeCountOf(4, wide, split) - 1);
  assert.equal(Math.max(lastA.rect.right, lastB.rect.right), 805, '模式 4：最后一条带的右缘贴矩形右缘');
  assert.equal(lastA.rect.left, 797, '模式 4：最后一条带宽仍是 S（不出现"半条"）');

  // 5（右→左）：第 0 带左缘 = 右缘 − 余数 = 800 ⇒ 第 0 带右缘越出矩形右 `S − 余数` = 3
  const backward = wipePattern({ ...BASE, mode: 5, rect: wide, split, elapsed: stepOf(5, wide, split) * 0.5 });
  const [backA, backB] = pairAt(backward, 0);
  assert.equal(backA.rect.left, 800, '模式 5：第 0 带左缘 = 右缘 − 余数');
  assert.equal(backB.rect.right, 808, '模式 5：第 0 带右缘越出矩形右 `S − 余数` = 3');

  // ★ 观测判据：最右 5 像素（余数那一段）在 步序 < S−余数 (=3) 时仍属画布 A
  const window = rect(800, 0, 805, 400);
  const clipped = (result) => result.strokes
    .filter((stroke) => stroke.canvas === 'B')
    .reduce((sum, stroke) => sum + overlapArea(stroke.rect, window), 0);
  assert.equal(clipped(wipePattern({ ...BASE, mode: 5, rect: wide, split, elapsed: stepOf(5, wide, split) * 2.5 })), 0,
    '步序 2 < 3 ⇒ 最右 5 像素还在画布 A 里（⛔ 网格没被"按 S 均分"）');
  assert.equal(clipped(wipePattern({ ...BASE, mode: 5, rect: wide, split, elapsed: stepOf(5, wide, split) * 8.5 })), 5 * 400,
    '步序 8 = S ⇒ 第 0 带整条让出，最右 5 像素才翻面');
});

test('★ 条带网格：余数为 0 时也多算一条带（整条落在矩形之外）', () => {
  // 宽 800 / S=8 ⇒ 余数 0、条带数 101（网格总覆盖 808 = 多出 8）
  const forward = wipePattern(at(4, 0));
  const [forwardA, forwardB] = pairAt(forward, 0);
  assert.equal(forwardA.rect.right <= 0 && forwardB.rect.right <= 0, true,
    '模式 4：第 0 带整条落在矩形左之外（右缘 = 矩形左缘）');
  assert.equal(pairAt(forward, 1)[0].rect.left, 0, '模式 4：第 1 带才贴矩形左缘');

  const backward = wipePattern(at(5, 0));
  const [backA, backB] = pairAt(backward, 0);
  assert.equal(backA.rect.left >= 800 && backB.rect.left >= 800, true,
    '模式 5：第 0 带整条落在矩形右之外（左缘 = 矩形右缘）');
  assert.equal(pairAt(backward, 1)[0].rect.right, 800, '模式 5：第 1 带才收在矩形右缘');
});

/* ─────────────────────────────── 形状：笔数 / 顺序 / 相接 ─────────────────────────────── */

test('★ 每区域恰好 2 笔：一族 2 笔、条带族每带 2 笔，一律先 A 后 B（12 模式全覆盖）', () => {
  for (let mode = 0; mode <= 11; mode += 1) {
    const expectedPairs = mode <= 3 ? 1 : stripeCountOf(mode);
    const result = wipePattern(at(mode, 0));
    assert.equal(result.strokes.length, expectedPairs * 2, `模式 ${mode} 的笔数必须是"每区域 2 笔"`);
    for (let i = 0; i < expectedPairs; i += 1) {
      const [a, b] = pairAt(result, i);
      assert.equal(a.canvas, 'A', `模式 ${mode} 第 ${i} 对的第一笔必须是画布 A`);
      assert.equal(b.canvas, 'B', `模式 ${mode} 第 ${i} 对的第二笔必须是画布 B`);
    }
  }
});

test('★ 两笔相接不重叠：并集 = 该区域、交集 = 0（12 模式 × 若干步）', () => {
  for (let mode = 0; mode <= 11; mode += 1) {
    const pairs = mode <= 3 ? 1 : stripeCountOf(mode);
    // 一族取"让出量 < 轴长"的步（再往后就走出矩形了，见 spec 的"提前走完"）
    const steps = mode <= 3 ? [0, 1, 5, 9] : [0, 1, 3, 9];
    for (const k of steps) {
      const result = wipePattern(at(mode, k));
      for (let i = 0; i < pairs; i += 1) {
        const [a, b] = pairAt(result, i);
        assert.equal(touches(a.rect, b.rect), true, `模式 ${mode} 第 ${i} 对：两笔必须相接且不重叠（k=${k}）`);
        const bandArea = mode <= 3
          ? area(BASE.rect)
          : BASE.split * (alongWidth(mode) ? heightOf(BASE.rect) : widthOf(BASE.rect));
        assert.equal(area(a.rect) + area(b.rect), bandArea, `模式 ${mode} 第 ${i} 对：两笔合起来恰盖住该区域（k=${k}）`);
        if (mode <= 3) {
          assert.equal(area(a.rect) + area(b.rect) === area(BASE.rect), true, '一族：两笔合起来盖住整个抹除矩形');
        }
      }
    }
  }
});

/* ─────────────────────────────── 未定档与非法档 ─────────────────────────────── */

test('★ duration = 0 ⇒ 返回空 + `wipe-duration-zero`（⛔ 不自行定义成"立即完成"）', () => {
  for (const duration of [0, -1]) {
    for (const mode of [0, 4, 8]) {
      const result = wipePattern({ ...BASE, mode, elapsed: 500, duration });
      assert.deepEqual(result.strokes, [], `duration=${duration}（模式 ${mode}）：spec 保持未定 ⇒ 一笔都不画`);
      assert.deepEqual(result.diagnostics, ['wipe-duration-zero'], `duration=${duration}（模式 ${mode}）必须记未定诊断`);
    }
  }
});

test('★ split ≤ 0 ⇒ 返回空 + `wipe-split-zero`（写入侧已夹到 ≥1，走到这里说明调用方没夹）', () => {
  for (const split of [0, -3]) {
    const result = wipePattern({ ...BASE, mode: 0, split, elapsed: 0 });
    assert.deepEqual(result.strokes, [], `split=${split}：步数 / 条带数无从算 ⇒ 不画`);
    assert.deepEqual(result.diagnostics, ['wipe-split-zero']);
  }
  // 两档同时坏 ⇒ 两条诊断都在（顺序 = 判定顺序），仍然一笔都不画
  assert.deepEqual(wipePattern({ ...BASE, mode: 0, split: 0, duration: 0, elapsed: 0 }), {
    strokes: [],
    diagnostics: ['wipe-split-zero', 'wipe-duration-zero'],
  });
});

test('★ 类型 < 0 ⇒ 一笔都不画（正常产物、不记诊断）；类型 > 11 ⇒ 空 + `wipe-mode-out-of-range`', () => {
  const negative = wipePattern({ ...BASE, mode: -1, elapsed: 500 });
  assert.deepEqual(negative.strokes, [], '类型 < 0（分割幅 < 1 那条错误记录）落不到任何图案');
  assert.deepEqual(negative.diagnostics, [], '类型 < 0 是**正常**产物 ⇒ 不是"未定档"，不记诊断');

  const beyond = wipePattern({ ...BASE, mode: 12, elapsed: 500 });
  assert.deepEqual(beyond.strokes, [], '类型 > 11 一笔都不画');
  assert.deepEqual(beyond.diagnostics, ['wipe-mode-out-of-range'], '写入侧已校验 ⇒ 出现即为上游坏数据');
});

/* ─────────────────────────────── 三档步长互不相同 ─────────────────────────────── */

test('★ 三档步长不是同一档：同一时刻三族的让出量按各自的 step 走', () => {
  // 505ms、S=8、800×400：一族步长 1000/102、二族 1000/9、三族 1000/109
  // ★ 取 505 而不是 500：500 ÷ (1000/102) 落在整数边界上（浮点下界 50.999…），不适合作判据
  const elapsed = 505;

  const whole = wipePattern({ ...BASE, mode: 0, elapsed });
  assert.equal(widthOf(pairAt(whole)[1].rect), 408, '一族：⌊505 ÷ (1000/102)⌋ = 51 步 ⇒ 让出 51×8 = 408');

  const inPhase = wipePattern({ ...BASE, mode: 4, elapsed });
  assert.equal(widthOf(pairAt(inPhase, 0)[1].rect), 4, '二族：⌊505 ÷ (1000/9)⌋ = 4 **像素**（⛔ 不是 4 道 = 32）');

  const staggered = wipePattern({ ...BASE, mode: 8, elapsed });
  assert.equal(widthOf(pairAt(staggered, 47)[1].rect), 8, '三族：步序 ⌊505 ÷ (1000/109)⌋ = 55 ⇒ 第 47 带已钳满 8');
  assert.equal(widthOf(pairAt(staggered, 48)[1].rect), 7, '三族：第 48 带 7 px（每带晚 1）');
  assert.equal(widthOf(pairAt(staggered, 54)[1].rect), 1, '三族：第 54 带 1 px');
  assert.equal(widthOf(pairAt(staggered, stripeCountOf(8) - 1)[1].rect), 0, '三族：最后一带还没开始让出');
});
