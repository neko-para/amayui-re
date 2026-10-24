/** @env pure @kind contract @why 命令档规划器把命令次序 / 时间戳盖章 / 三表合并序 / 区间之下 / 已消费 / 清表判据写错时，引擎会"少画一批条目、多画一次、或让一次性命令反复执行"—— 这些都不会报错，只有规划器的断言能抓住 */
/**
 * tools/test/emulator-new-scene-command-plan.test.mjs —— **命令档纯规划器**（S0 / S1）
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md`：
 *   - `CORE-SCENE-DRAW-ORDER`：命令键升序、每条命令的前置（时间戳盖章 / 跳过额外变换）、
 *     三表合并序（辅助表 A → 网格 → 绘制项；辅助表 B 不参与）、「区间之下」的三种类口径、
 *     「本轮已消费」、种类 1 的第一帧门、种类 2 的中止、命令表生命周期（无动画 ⇒ 整表清空）；
 *   - `CORE-OPS-SCENE-COMMAND-RECORDS`：四种命令的阶段结构；种类 2 的**抹除矩形来自同键绘制项**
 *     （缺矩形 ⇒ 缺口、不产笔）、`经过时间 = 场景时刻 − (时间戳 + 延迟)`、12 种图案的笔。<br>
 *     ★ 笔只进**规划结构**（`composite.strokes`），⛔ 平台侧一笔都没画。
 *
 * ★ 这里**只测结构，不测像素**：规划器不画任何东西（平台侧执行仍未建模，见
 *   `scene-order.ts` 的 `scene-command-pass-render-unmodeled`）。
 * ★ 为什么这些断言会红（每条都对应一种"静默偏掉"）：
 *   - 遍历不排序 ⇒ 命令执行次序变成插入次序（键 5 的命令排到键 100 后面）；
 *   - 盖章写成"永远盖" / "从不盖" ⇒ 种类 1 的第一帧门每帧都开或永远不开；
 *   - 两段区间合成一段 / 区间之下扫全表 ⇒ 第二块画布吃到第一段的条目（或反之）；
 *   - 能力档 2 仍跑区间之下 ⇒ 同一批条目在同一帧被发两次；
 *   - 区间之下把条目记成已消费 ⇒ 收尾扫描不再画它们（这些键在同一帧只出现一次）；
 *   - 清表判据写成"总是清" / "只看有动画" ⇒ 一次性命令反复执行，或动画中的命令被清掉；
 *   - 种类 2 的抹除矩形取错来源（拿命令自己的格 / 编一个 0 矩形）、经过时间漏减延迟、
 *     `wipePattern` 的未定档被吞掉 ⇒ 合成阶段的笔错位，而"没画"这件事在像素上完全看不出来。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  CANVAS_A,
  CANVAS_B,
  mergeTableKeys,
  planSceneCommandPass,
} from '../../apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts';
import { wipePattern } from '../../apps/emulator-new/src/platform/electron/renderer/wipe-pattern.ts';

const range = (start, count) => ({ start, count });

/** 一条命令快照（字段齐全 = 与 `SceneSnapshot.commands` 同形） */
const command = (over = {}) => ({
  kind: 0,
  commandKey: 10,
  timestamp: 0,
  delay: 0,
  duration: 100,
  textureId: 100,
  rangeA: range(10, 2),
  rangeB: range(0, 0),
  mode: 0,
  split: 0,
  ownerKey: 10,
  ...over,
});

/** 规划器输入（默认：能力档 2、无动画 / 无强制完成、辅助表 A 已建模但空） */
const input = (over = {}) => ({
  commands: [],
  now: 1000,
  selfKeyWindow: { start: 0, count: 0 },
  tables: { meshes: [], auxA: [], items: [] },
  shaderTier: 2,
  animating: 0,
  forcedComplete: 0,
  ...over,
});

const stages = (plan) => plan.commands.flatMap((entry) => entry.phases.map((phase) => phase.stage));
const phasesOf = (plan, stage) => plan.commands.flatMap((entry) => entry.phases).filter((phase) => phase.stage === stage);

/* ──────────────────────────── 遍历序与前置 ──────────────────────────── */

test('★ 命令键升序：输入乱序也按命令键（不是插入次序）执行', () => {
  const plan = planSceneCommandPass(input({
    commands: [
      command({ kind: 3, commandKey: 100, rangeA: range(0, 0) }),
      command({ kind: 3, commandKey: 5, rangeA: range(0, 0) }),
      command({ kind: 3, commandKey: 40, rangeA: range(0, 0) }),
    ],
  }));
  assert.deepEqual(plan.commands.map((entry) => entry.commandKey), [5, 40, 100],
    '⛔ 不许按插入次序（命令表是有序表，遍历序是命令键升序）');
});

test('★ 时间戳盖章：为 0 ⇒ 盖成 now（输出 stamps），非 0 保持原值', () => {
  const plan = planSceneCommandPass(input({
    now: 1000,
    commands: [
      command({ kind: 1, commandKey: 5, timestamp: 0, rangeA: range(10, 2) }),
      command({ kind: 3, commandKey: 6, timestamp: 700, rangeA: range(10, 2) }),
    ],
  }));
  assert.deepEqual(plan.stamps, [{ commandKey: 5, timestamp: 1000 }],
    '只有"输入是 0"的那条被盖章（⛔ 不是每次都盖）');
  assert.deepEqual(plan.commands.map((entry) => entry.timestamp), [1000, 700]);
});

test('★ 种类 1 的第一帧门：判据是"盖章后的时间戳 === now"（时钟不前进仍算首帧）', () => {
  const first = planSceneCommandPass(input({
    now: 1000,
    commands: [command({ kind: 1, commandKey: 5, timestamp: 0, rangeA: range(10, 2) })],
    tables: { meshes: [10], auxA: [], items: [] },
  }));
  assert.equal(first.commands[0].firstFrame, true, '刚盖章 ⇒ 首帧');
  assert.deepEqual(stages(first), ['covered', 'copy', 'mask-blend'], '首帧：覆盖区间 → 拷贝 → 遮罩混合');

  // 时钟**没有前进**：已盖章的命令仍落回首帧支 —— 这是相等判据的后果，⛔ 不许"修"成别的判据
  const sameClock = planSceneCommandPass(input({
    now: 1000,
    commands: [command({ kind: 1, commandKey: 5, timestamp: 1000, rangeA: range(10, 2) })],
    tables: { meshes: [10], auxA: [], items: [] },
  }));
  assert.equal(sameClock.commands[0].firstFrame, true, '时间戳 == now ⇒ 首帧（相等，不是"首次"）');
  assert.deepEqual(stages(sameClock), ['covered', 'copy', 'mask-blend']);

  // 时钟前进：零发射只标记，但遮罩混合同样发生（每帧一次）
  const later = planSceneCommandPass(input({
    now: 1016,
    commands: [command({ kind: 1, commandKey: 5, timestamp: 1000, rangeA: range(10, 2) })],
    tables: { meshes: [10], auxA: [], items: [] },
  }));
  assert.equal(later.commands[0].firstFrame, false, '时间戳 != now ⇒ 之后');
  assert.deepEqual(stages(later), ['mark', 'mask-blend'], '之后：只标记（零发射）→ 遮罩混合');
  assert.equal(phasesOf(later, 'mark')[0].draw, false, '只标记支一个绘制请求都不发');
});

/* ──────────────────────────── 空表 / 清表 ──────────────────────────── */

test('★ 空命令表：全空、无诊断、不产生任何副作用（不标脏）', () => {
  const plan = planSceneCommandPass(input({
    commands: [],
    tables: { meshes: [1, 2], auxA: null, items: [3] },
    animating: 1,
  }));
  assert.deepEqual(plan.commands, []);
  assert.deepEqual(plan.consumedKeys, []);
  assert.deepEqual(plan.stamps, []);
  assert.deepEqual(plan.diagnostics, [], '空表 ⇒ 命令档无动作，连"缺口"都没有');
  assert.equal(plan.clearTable, false, '「有动画」= 1 ⇒ 不清表（清表判据仍照公式算）');
});

test('★ clearTable = 「有动画」=== 0 && 「强制完成」=== 0', () => {
  for (const [animating, forcedComplete, expected] of [
    [0, 0, true],
    [1, 0, false],
    [0, 1, false],
    [1, 1, false],
  ]) {
    const plan = planSceneCommandPass(input({
      commands: [command({ kind: 3, commandKey: 1, rangeA: range(0, 0) })],
      animating,
      forcedComplete,
    }));
    assert.equal(plan.clearTable, expected, `animating=${animating} forcedComplete=${forcedComplete}`);
  }
});

/* ──────────────────────────── 区间之下 ──────────────────────────── */

test('★ 区间之下跳过另一段：B 段起点更低时，A 的区间之下必须排除尚未消费的 B 段条目', () => {
  const plan = planSceneCommandPass(input({
    commands: [command({
      kind: 2,
      commandKey: 99,
      ownerKey: 10,
      rangeA: range(40, 10),
      rangeB: range(20, 10),
    })],
    tables: { meshes: [5, 25, 45], auxA: [], items: [10, 30, 50] },
    shaderTier: 1,
  }));
  const below = phasesOf(plan, 'below-range');
  assert.deepEqual(below[0].tableKeys, [5, 10, 30], 'A 的区间之下（键 < 40）必须排除 B 段 [20,30) 的 25，但保留不在 B 段里的 30');
  assert.deepEqual(below[1].tableKeys, [5, 10], 'B 的区间之下（键 < 20）不涉及 A 段');
});

test('★ 种类 0 / 2 的区间之下：每块画布一趟、跳过另一段、不置已消费', () => {
  for (const kind of [0, 2]) {
    const plan = planSceneCommandPass(input({
      commands: [command({
        kind,
        commandKey: 99,
        ownerKey: 10,
        rangeA: range(20, 10),
        rangeB: range(40, 10),
      })],
      tables: {
        meshes: [5, 20, 25, 45],
        auxA: [],
        items: [10, 15, 30, 50],
      },
      shaderTier: 1,
    }));
    const belowRange = phasesOf(plan, 'below-range');
    assert.equal(belowRange.length, 2, `kind=${kind}：两块画布各一趟区间之下`);
    assert.deepEqual(belowRange.map((phase) => phase.target), [CANVAS_A, CANVAS_B], '画布 36 先于 37');
    assert.deepEqual(belowRange.map((phase) => phase.segment), ['A', 'B']);
    assert.deepEqual(belowRange[0].tableKeys, [5, 10, 15],
      '区间之下 A = 键 < 20 且不在区间 B [40,50) 里（三表合并序：网格 → 辅助表 A → 绘制项）');
    assert.deepEqual(belowRange[1].tableKeys, [5, 10, 15, 30],
      '区间之下 B = 键 < 40 且不在区间 A [20,30) 里（键 30 不在区间 A ⇒ 参与）');
    for (const phase of belowRange) assert.equal(phase.consumed, false, '区间之下**不**置已消费');

    const covered = phasesOf(plan, 'covered');
    assert.deepEqual(covered[0].tableKeys, [20, 25], '区间 A 只喂第一块画布');
    assert.deepEqual(covered[1].tableKeys, [45], '区间 B 只喂第二块画布（50 不在 [40,50) 内）');
    assert.deepEqual(plan.consumedKeys, [20, 25, 45], '只有覆盖区间置已消费');
  }
});

test('★ 种类 3 没有区间之下：单段 + 合成（能力档 0 也不发生）', () => {
  const plan = planSceneCommandPass(input({
    commands: [command({ kind: 3, commandKey: 5, rangeA: range(10, 5), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 12, 30], auxA: [], items: [] },
    shaderTier: 0,
  }));
  assert.deepEqual(stages(plan), ['covered', 'composite'], '结构上就没有"区间之下"这一趟');
  assert.deepEqual(phasesOf(plan, 'covered')[0].tableKeys, [10, 12]);
});

test('★ 能力档 2 抑制区间之下（种类 0 / 1 / 2）；档为 null 时按 ≥2 跑并记缺口', () => {
  for (const kind of [0, 1, 2]) {
    const plan = planSceneCommandPass(input({
      commands: [command({ kind, commandKey: 5, rangeA: range(10, 5), rangeB: range(20, 5) })],
      tables: { meshes: [5], auxA: [], items: [5] },
      shaderTier: 2,
    }));
    assert.equal(phasesOf(plan, 'below-range').length, 0, `kind=${kind} 能力档 2 ⇒ 区间之下不发生`);
  }

  const unknown = planSceneCommandPass(input({
    commands: [command({ kind: 3, commandKey: 5, rangeA: range(10, 5) })],
    shaderTier: null,
  }));
  assert.equal(phasesOf(unknown, 'below-range').length, 0, 'null 按 ≥2 跑（区间之下不发生）');
  assert.ok(unknown.diagnostics.some((entry) => entry.kind === 'scene-command-tier-unmodeled'),
    '但必须显式记账（⛔ 不把 null 当 2 这个结论）');
});

/* ──────────────────────────── 已消费 / 合并序 ──────────────────────────── */

test('★ consumed 标记：覆盖区间置位、区间之下不置；consumedKeys 是收尾扫描要跳过的键', () => {
  const plan = planSceneCommandPass(input({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(10, 3), rangeB: range(0, 0) })],
    tables: { meshes: [10, 12], auxA: [], items: [10, 11] },
    shaderTier: 1,
  }));
  const covered = phasesOf(plan, 'covered')[0];
  assert.deepEqual(covered.tableKeys, [10, 10, 11, 12],
    '同键多条目各算一次（平键序的单键输出看不出 A/网格，见下一条 typed 用例）');
  assert.equal(covered.consumed, true);
  assert.deepEqual(plan.consumedKeys, [10, 11, 12], '升序去重（第 16 位是"条目作废"，不是"键作废"）');
});

/* ──────────────────────────── 键窗粗判 ──────────────────────────── */

test('★ 键窗粗判只看覆盖区间起点：起点在窗外但跨进窗内 ⇒ 窗内条目仍乘额外变换', () => {
  const inside = planSceneCommandPass(input({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(12, 3) })],
    selfKeyWindow: { start: 10, count: 5 },
  }));
  assert.equal(inside.commands[0].skipExtraTransform, true,
    '起点 12 ∈ [10, 15) ⇒ 命令档跳过额外变换');

  const crossing = planSceneCommandPass(input({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(5, 10) })],
    selfKeyWindow: { start: 10, count: 5 },
  }));
  assert.equal(crossing.commands[0].skipExtraTransform, false,
    '起点 5 在窗外（区间跨进窗内）⇒ 标志为 false，窗内条目仍乘额外变换（⛔ 不是"区间相交"）');

  const emptyWindow = planSceneCommandPass(input({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(10, 3) })],
    selfKeyWindow: { start: 0, count: 0 },
  }));
  assert.equal(emptyWindow.commands[0].skipExtraTransform, false, '键窗计数 0 ⇒ 区间为空 ⇒ 恒 false');
});

/* ──────────────────────────── 种类 2 的中止 ──────────────────────────── */

test('★ 种类 2 缺同键绘制项 ⇒ 清持续时间 + 中止整档（其后的命令一条都不规划）', () => {
  const plan = planSceneCommandPass(input({
    commands: [
      command({ kind: 2, commandKey: 5, ownerKey: 777 }),
      command({ kind: 3, commandKey: 9, rangeA: range(0, 0) }),
    ],
  }));
  assert.equal(plan.commands.length, 1, '⛔ 不是 continue：后面的命令不执行');
  assert.deepEqual(plan.commands[0].phases, [], '中止发生在任何切目标之前 ⇒ 没有任何阶段');
  assert.equal(plan.commands[0].abort, true);
  assert.ok(plan.diagnostics.some((entry) => entry.kind === 'scene-command-wipe-rect-missing'
    && entry.commandKey === 5), '必须显式记账"缺那条绘制项"');
  assert.deepEqual(plan.consumedKeys, [], '中止命令没有覆盖任何条目');
});

/* ──────────────────────────── 种类 2 的抹除笔（只规划、不画） ──────────────────────────── */

/** 抹除矩形（800 × 400；条带族用 S=8 ⇒ ⌊800/8⌋+1 = 101 条带） */
const WIPE_RECT = { left: 0, top: 0, right: 800, bottom: 400 };

/** 一条种类 2 命令（同键绘制项 = 42；默认模式 0 / 分割幅 8 / 持续 1000ms） */
const wipeCommand = (over = {}) => command({
  kind: 2,
  commandKey: 100,
  ownerKey: 42,
  mode: 0,
  split: 8,
  delay: 0,
  duration: 1000,
  ...over,
});

/** 规划器输入（默认带同键绘制项 42 的矩形；`over` 可换命令 / 表） */
const wipeInput = (over = {}) => input({
  commands: [wipeCommand()],
  tables: { meshes: [], auxA: [], items: [42], itemRects: new Map([[42, WIPE_RECT]]) },
  ...over,
});

const compositeOf = (plan) => phasesOf(plan, 'composite')[0];

test('★ 种类 2 + 同键绘制项矩形 ⇒ 合成阶段挂上抹除笔（一族 2 笔 / 条带族 条带数×2），且与 wipePattern 直接调用逐笔一致', () => {
  const now = 1400;
  const timestamp = 1000;
  const delay = 100;
  for (const mode of [0, 4, 8]) {
    const plan = planSceneCommandPass(input({
      now,
      commands: [wipeCommand({ timestamp, delay, mode })],
      tables: { meshes: [], auxA: [], items: [42], itemRects: new Map([[42, WIPE_RECT]]) },
    }));
    const composite = compositeOf(plan);
    assert.equal(composite.stage, 'composite');
    assert.equal(composite.target, 'own', '抹除结果落在命令自己的纹理上');
    assert.equal(composite.draw, true, '合成这一趟照常发请求（笔只是它内部的结构）');

    const stripes = Math.floor((WIPE_RECT.right - WIPE_RECT.left) / 8) + 1; // 101
    assert.equal(composite.strokes.length, mode <= 3 ? 2 : 2 * stripes,
      `模式 ${mode}：一族恰好 2 笔、条带族每带 2 笔`);

    const direct = wipePattern({
      mode,
      rect: WIPE_RECT,
      split: 8,
      // 经过时间 = now − (时间戳 + 延迟)；漏掉延迟就是 400 而不是 300
      elapsed: now - (timestamp + delay),
      duration: 1000,
    });
    assert.deepEqual(composite.strokes, direct.strokes,
      `模式 ${mode}：笔必须与 wipePattern 直接调用逐笔一致（⛔ 不在规划器里重算一遍图案）`);
    assert.deepEqual(plan.diagnostics, [], `模式 ${mode}：参数合法 ⇒ 一条抹除诊断都没有`);
  }

  // ★ 判据：经过时间**必须**减掉延迟（否则同一时刻会多让出一道）
  const plan = planSceneCommandPass(input({
    now,
    commands: [wipeCommand({ timestamp, delay, mode: 0 })],
    tables: { meshes: [], auxA: [], items: [42], itemRects: new Map([[42, WIPE_RECT]]) },
  }));
  const withoutDelay = wipePattern({ mode: 0, rect: WIPE_RECT, split: 8, elapsed: now - timestamp, duration: 1000 });
  assert.notDeepEqual(compositeOf(plan).strokes, withoutDelay.strokes,
    '⛔ 经过时间 = now − 时间戳 是错的（延迟也要减）');

  // ★ 刚盖章的帧（时间戳由 0 盖成 now）：经过时间 = −延迟（负区按 spec 外推，⛔ 不夹取）
  const fresh = planSceneCommandPass(input({
    now,
    commands: [wipeCommand({ timestamp: 0, delay: 250, mode: 0 })],
    tables: { meshes: [], auxA: [], items: [42], itemRects: new Map([[42, WIPE_RECT]]) },
  }));
  assert.deepEqual(fresh.stamps, [{ commandKey: 100, timestamp: now }], '时间戳 0 ⇒ 盖成本帧场景时刻');
  assert.deepEqual(compositeOf(fresh).strokes,
    wipePattern({ mode: 0, rect: WIPE_RECT, split: 8, elapsed: -250, duration: 1000 }).strokes,
    '盖章后的经过时间 = now − (now + 延迟) = −延迟');
});

test('★ 种类 2：duration = 0 ⇒ 记 scene-command-wipe-duration-zero、合成阶段笔为空（⛔ 不自行定义成"立即完成"）', () => {
  for (const duration of [0, -1]) {
    const plan = planSceneCommandPass(wipeInput({ commands: [wipeCommand({ duration })] }));
    assert.deepEqual(plan.diagnostics, [{ kind: 'scene-command-wipe-duration-zero', commandKey: 100 }],
      `duration=${duration}：spec 保持未定 ⇒ 必须显式记账`);
    assert.deepEqual(compositeOf(plan).strokes, [],
      `duration=${duration}：算过但这一帧一笔都不发（不是"没算过"）`);
  }
});

test('★ 种类 2：分割幅 = 0 ⇒ scene-command-wipe-split-zero（写入侧已夹到 ≥1 ⇒ 出现即上游没夹）', () => {
  for (const split of [0, -3]) {
    const plan = planSceneCommandPass(wipeInput({ commands: [wipeCommand({ split })] }));
    assert.deepEqual(plan.diagnostics, [{ kind: 'scene-command-wipe-split-zero', commandKey: 100 }]);
    assert.deepEqual(compositeOf(plan).strokes, [], '步数 / 条带数无从算 ⇒ 不产笔');
  }
});

test('★ 种类 2：同键条目在表里但没有矩形 ⇒ scene-command-wipe-rect-unmodeled 且不产笔（⛔ 不编矩形）', () => {
  // 两种"没有矩形"：整份投影缺失 / 投影里没有这一键
  for (const tables of [
    { meshes: [], auxA: [], items: [42] },
    { meshes: [], auxA: [], items: [42], itemRects: new Map() },
  ]) {
    const plan = planSceneCommandPass(input({ commands: [wipeCommand()], tables }));
    assert.deepEqual(plan.diagnostics, [{ kind: 'scene-command-wipe-rect-unmodeled', commandKey: 100 }],
      '条目在表里 ⇒ 不是 rect-missing；矩形没有 ⇒ 是 rect-unmodeled（两条缺口不许合并）');
    assert.equal('strokes' in compositeOf(plan), false, '⛔ 不许挂一个编出来的笔（连空数组都不挂）');
    assert.equal(plan.commands[0].abort, false, '矩形没建模 ≠ 缺条目：不中止整档');
    assert.deepEqual(stages(plan), ['covered', 'covered', 'composite'], '结构照常规划出来');
  }

  // `strokes` 是种类 2 专用：种类 0 的合成阶段没有这一格
  const kind0 = planSceneCommandPass(input({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(10, 2) })],
  }));
  assert.equal('strokes' in compositeOf(kind0), false, '⛔ 别给种类 0 也挂抹除笔');
});

test('★ 种类 2：类型 = 12 ⇒ scene-command-wipe-mode-out-of-range 且不产笔；类型 < 0 是正常产物（不产笔、不记诊断）', () => {
  const beyond = planSceneCommandPass(wipeInput({ commands: [wipeCommand({ mode: 12 })] }));
  assert.deepEqual(beyond.diagnostics, [{ kind: 'scene-command-wipe-mode-out-of-range', commandKey: 100 }]);
  assert.deepEqual(compositeOf(beyond).strokes, [], '落不到任何图案 ⇒ 一笔都不画');

  const negative = planSceneCommandPass(wipeInput({ commands: [wipeCommand({ mode: -1 })] }));
  assert.deepEqual(negative.diagnostics, [],
    '类型 < 0（分割幅 < 1 那条错误记录）是**正常**产物 ⇒ 不是未定档，不记诊断');
  assert.deepEqual(compositeOf(negative).strokes, [], '但它同样一笔都不画');
});

test('★ 三表合并序：同键时辅助表 A 在网格之前（typed 表秩 0/1/2）—— 引擎比较链是 A→网格→绘制项', () => {
  const entries = mergeTableKeys({ auxA: [5], meshes: [5], items: [5] });
  assert.deepEqual(entries.map((entry) => [entry.key, entry.table]), [[5, 0], [5, 1], [5, 2]]);
  const mixed = mergeTableKeys({ auxA: [5], meshes: [5, 9], items: [5, 7] });
  assert.deepEqual(mixed.map((entry) => entry.key), [5, 5, 5, 7, 9]);
});
