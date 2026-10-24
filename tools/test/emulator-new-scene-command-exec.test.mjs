/** @env pure @kind contract @why 命令档执行计划器的 op 次序 / 种类闸门写错时，画面会少一块、多清一次、把邻段的条目算进来（同帧两次 / 静默丢画 / 白闪），而像素上看不出原因 —— 只有 op 序列能抓住 */
/**
 * tools/test/emulator-new-scene-command-exec.test.mjs —— **命令档执行计划器**（S2a-1，pure）
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md`（`CORE-SCENE-DRAW-ORDER` / `CORE-OPS-SCENE-COMMAND-RECORDS`）
 * 与切片设计 `.tmp/spec-draft/batch26/s2/slice-plan.md` 的 **S2a** 一节。<br>
 * ★ 这里**只测结构**：执行计划器不画任何东西、不建画布、不写 core / `consumedKeys`。
 *
 * ★ 为什么这些断言会红（每条都对应一种"静默偏掉"，像素上看不出来）：
 *   - 总闸没关死 ⇒ 产品画面在"这一层还没接"的时候就开始变（零回退失效）；
 *   - 种类 0 / 2 漏清一块画布 / 清两次 / 用 B 的清去清 A ⇒ 低档不透明底色会串（上一帧的残影）；
 *   - 区间之下排在覆盖区间之后 ⇒ 同键条目在同帧被画两次，且后一笔压住前一笔；
 *   - 种类 1 的 copy 方向写反（用"画某编号纹理"代替"画布 → 影片纹理"）⇒ 影片纹理被自己的旧内容盖住；
 *   - 种类 1 非首帧仍发 copy ⇒ 每帧都把画布内容糊到影片纹理上（spec 明说之后零发射）；
 *   - 同键条目的平键序错（缺表身份 / 网格跑到辅助表 A 之前）⇒ 同键叠放次序反了；
 *   - 种类 3 凭空多一趟区间之下 ⇒ 键 < 起点的条目被多画一次；
 *   - 种类 2 的 B 段把 A 段的条目也算进来 ⇒ 两块画布内容互相污染；
 *   - 区间之下的条目里出现"键 ≥ 本段起点" ⇒ 与覆盖区间重复绘制（执行层的第二道闸）；
 *   - 设备就绪门静默少画一趟（没有诊断）⇒ 缺的那一块在诊断面板上不可见。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { planSceneCommandPass } from '../../apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts';
import { planCommandExecution } from '../../apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts';

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
const planInput = (over = {}) => ({
  commands: [],
  now: 1000,
  selfKeyWindow: { start: 0, count: 0 },
  tables: { meshes: [], auxA: [], items: [] },
  shaderTier: 2,
  animating: 0,
  forcedComplete: 0,
  ...over,
});

/** 执行计划器输入（★ 总闸默认**开**；只有专门测闸门的用例才关它） */
const execInput = (plan, commands, over = {}) => ({
  plan,
  commands,
  enabled: true,
  deviceReady: true,
  ...over,
});

const exec = (plan, commands, over = {}) => planCommandExecution(execInput(plan, commands, over));
const opsOf = (result, kind) => result.ops.filter((op) => op.kind === kind);
const emitsInto = (result, into) => opsOf(result, 'emit').filter((op) => op.into === into);
const keysOf = (emitOp) => emitOp.entries.map((entry) => entry.key);

/* ──────────────────────────── 总闸 / 空表 ──────────────────────────── */

test('★ 默认关闸：enabled 缺省 / false ⇒ 零 op、零诊断（产品零回退）；空命令表 ⇒ 零 op', () => {
  const empty = planSceneCommandPass(planInput({ commands: [] }));
  assert.deepEqual(exec(empty, []).ops, [], '空命令表 ⇒ 命令档一个请求都不发（spec：空表无动作）');

  const blurPlan = planSceneCommandPass(planInput({
    commands: [command({ kind: 3, commandKey: 7, rangeA: range(10, 5) })],
    tables: { meshes: [10], auxA: [], items: [] },
  }));
  const blurCommands = [{ commandKey: 7, kind: 3, rangeA: range(10, 5), rangeB: range(0, 0) }];
  // 缺省（没有 enabled 这一格）与显式 false 都必须一个 op 都不产
  assert.deepEqual(planCommandExecution({ plan: blurPlan, commands: blurCommands }),
    { ops: [], diagnostics: [] }, '⛔ 缺省不许产 op：这一层今天还没接进渲染步');
  assert.deepEqual(planCommandExecution({ plan: blurPlan, commands: blurCommands, enabled: false }),
    { ops: [], diagnostics: [] }, '⛔ false 不许产 op');
  assert.ok(exec(blurPlan, blurCommands).ops.length > 0, '开了闸才产（否则上一条断言恒真）');
});

/* ──────────────────────────── 种类 0：两块画布 ──────────────────────────── */

/** 种类 0 / 2 的共用夹具：区间 A [20,30) / 区间 B [40,50)，能力档 1（区间之下发生） */
const twoCanvasFixture = (kind) => {
  const ownerKey = kind === 2 ? 30 : 10; // 种类 2 要求绘制项表里有一条同键条目
  const plan = planSceneCommandPass(planInput({
    commands: [command({
      kind,
      commandKey: 99,
      ownerKey,
      rangeA: range(20, 10),
      rangeB: range(40, 10),
    })],
    tables: {
      meshes: [5, 20, 25, 45],
      auxA: [],
      items: [10, 15, 30, 50],
      itemRects: new Map([[30, { left: 0, top: 0, right: 8, bottom: 8 }]]),
    },
    shaderTier: 1,
  }));
  const commands = [{ commandKey: 99, kind, rangeA: range(20, 10), rangeB: range(40, 10) }];
  return { plan, commands };
};

test('★ 种类 0：画布 A / B 各切目标 + 清一次，区间之下在覆盖区间之前，composite 在最后且两源', () => {
  const { plan, commands } = twoCanvasFixture(0);
  const result = exec(plan, commands);

  assert.deepEqual(result.ops.map((op) => op.kind),
    ['target', 'clear', 'emit', 'emit', 'target', 'clear', 'emit', 'emit', 'target', 'composite'],
    '次序 = A 趟 → B 趟 → 合成');

  // ① 两块画布**各清一次**、A 先于 B
  assert.deepEqual(opsOf(result, 'clear').map((op) => op.target), ['canvasA', 'canvasB'],
    '⛔ 漏清 / 清两次 / 清错画布：低档不透明底色会串上一帧的残影');
  // ② 每个 clear 前一个 op 是**同一块画布**的 target（用前先清）
  result.ops.forEach((op, index) => {
    if (op.kind !== 'clear') return;
    assert.equal(result.ops[index - 1]?.kind, 'target');
    assert.equal(result.ops[index - 1]?.target, op.target, '切目标与清空必须同一块画布');
  });

  // ③ 区间之下在覆盖区间之前（每块画布各一趟，次序 = 键升序 + 平键序）
  assert.deepEqual(emitsInto(result, 'canvasA').map(keysOf), [[5, 10, 15], [20, 25]],
    'A：区间之下 [5,10,15]（键 < 20，跳过 B 段）→ 覆盖区间 [20,25]');
  assert.deepEqual(emitsInto(result, 'canvasB').map(keysOf), [[5, 10, 15, 30], [45]],
    'B：区间之下 [5,10,15,30]（键 < 40，跳过 A 段 [20,30) 里的 20/25）→ 覆盖区间 [45]');

  // ④ 合成是最后一个 op，且两源 = 两块画布 → 命令自己的纹理
  const last = result.ops.at(-1);
  assert.equal(last.kind, 'composite', '⛔ 合成之后不许再画：那会落在 own 之外的目标上');
  assert.deepEqual(last.sources, ['canvasA', 'canvasB']);
  assert.equal(last.into, 'own');
  assert.equal(result.ops.at(-2).kind, 'target');
  assert.equal(result.ops.at(-2).target, 'own', '合成前先切回自己的纹理');
});

/* ──────────────────────────── 种类 1：影片覆盖 ──────────────────────────── */

/** 种类 1 夹具：时间戳 0（首帧）、区间 [10,12)、能力档 1 ⇒ 区间之下 [5] */
const movieFixture = (over = {}) => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 1, commandKey: 5, timestamp: 0, rangeA: range(10, 2), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 11], auxA: [], items: [] },
    shaderTier: 1,
    ...over,
  }));
  const commands = [{ commandKey: 5, kind: 1, rangeA: range(10, 2), rangeB: range(0, 0) }];
  return { plan, commands };
};

test('★ 种类 1：copy 方向 = 画布 A → own，且只在首帧产 op（之后零发射）', () => {
  const first = movieFixture();
  assert.equal(first.plan.commands[0].firstFrame, true, '夹具前提：首帧');
  const result = exec(first.plan, first.commands);

  const copy = opsOf(result, 'copy');
  assert.equal(copy.length, 1, '首帧恰好一次整幅拷贝');
  assert.equal(copy[0].from, 'canvasA', '源 = 离屏画布 A');
  assert.equal(copy[0].to, 'own', '目标 = 命令自己的纹理（种类 1 是影片纹理）');
  assert.equal(result.ops.at(-1).kind, 'copy', '拷贝是这条命令最后一个 op（遮罩混合不在本轮）');
  assert.deepEqual(emitsInto(result, 'canvasA').map(keysOf), [[5], [10, 11]],
    '首帧：区间之下 [5] → 覆盖区间 [10,11]');

  // ★ 之后（时间戳 ≠ now）：只标记支 ⇒ 零 op（不是"少发一笔"）
  const later = movieFixture({
    now: 1016,
    commands: [command({ kind: 1, commandKey: 5, timestamp: 1000, rangeA: range(10, 2), rangeB: range(0, 0) })],
  });
  assert.equal(later.plan.commands[0].firstFrame, false, '夹具前提：非首帧');
  assert.deepEqual(exec(later.plan, later.commands), { ops: [], diagnostics: [] },
    '⛔ 非首帧仍发 copy 会把画布内容每帧糊到影片纹理上');
});

test('★ 设备就绪门：种类 1 的区间之下被挡住时记缺口，覆盖区间与拷贝照发；无区间之下时不记', () => {
  const { plan, commands } = movieFixture();
  const blocked = exec(plan, commands, { deviceReady: false });
  assert.deepEqual(emitsInto(blocked, 'canvasA').map(keysOf), [[10, 11]],
    '区间之下被挡 ⇒ 只剩覆盖区间那一趟');
  assert.deepEqual(blocked.diagnostics,
    [{ kind: 'scene-command-exec-device-not-ready', commandKey: 5 }],
    '⛔ 少画一趟必须留痕（不许静默）');
  assert.equal(opsOf(blocked, 'copy').length, 1, '就绪门只挡区间之下，挡不住拷贝');

  // 能力档 2：规划器根本不产区间之下 ⇒ 就绪门没有可挡的 ⇒ 不记诊断
  const tier2 = movieFixture({ shaderTier: 2 });
  const noGate = exec(tier2.plan, tier2.commands, { deviceReady: false });
  assert.deepEqual(noGate.diagnostics, [], '没有区间之下这一趟 ⇒ 与就绪门无关');
});

/* ──────────────────────────── 平键序 / entries 形状 ──────────────────────────── */

test('★ 同键条目的平键序 = 辅助表 A → 网格 → 绘制项（阶段 entries 与 emit 都保序）', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 0, commandKey: 5, rangeA: range(10, 3), rangeB: range(0, 0) })],
    tables: { meshes: [10, 12], auxA: [10], items: [10, 11] },
    shaderTier: 2,
  }));
  const covered = plan.commands[0].phases.find((phase) => phase.stage === 'covered' && phase.segment === 'A');

  // 阶段条目：同键 10 的三条按 辅助表 A(0) → 网格(1) → 绘制项(2)；表身份是字符串名
  assert.deepEqual(covered.entries.map((entry) => [entry.key, entry.table]),
    [[10, 'auxA'], [10, 'mesh'], [10, 'items'], [11, 'items'], [12, 'mesh']],
    '⛔ 平键序错 / 丢表身份：同键叠放次序反了，而 `tableKeys` 老形状下这条根本写不出来');
  // 旧形状兼容：tableKeys 必须与 entries 逐项同序（两个形状不许漂）
  assert.deepEqual(covered.tableKeys, covered.entries.map((entry) => entry.key));
  // 只有 covered / below-range 两阶段带 entries
  for (const phase of plan.commands[0].phases) {
    if (phase.stage === 'covered' || phase.stage === 'below-range') {
      assert.ok(Array.isArray(phase.entries), `${phase.stage} 必须带 entries`);
    } else {
      assert.equal('entries' in phase, false, `${phase.stage} 不该有 entries（本轮只给两阶段）`);
    }
  }

  // 执行层原样保序，且**拷贝**（不改规划器的数组）
  const result = exec(plan, [{ commandKey: 5, kind: 0, rangeA: range(10, 3), rangeB: range(0, 0) }]);
  const emitA = emitsInto(result, 'canvasA')[0];
  assert.deepEqual(emitA.entries.map((entry) => [entry.key, entry.table]),
    covered.entries.map((entry) => [entry.key, entry.table]), 'emit 的条目次序必须与阶段一致');
  assert.notEqual(emitA.entries, covered.entries, '⛔ 执行层不许把规划器的数组直接交出去（会被改到）');
});

/* ──────────────────────────── 种类 3：单段 ──────────────────────────── */

test('★ 种类 3：单段（结构上没有区间之下）+ 单源合成，且只用画布 A', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 3, commandKey: 7, rangeA: range(10, 5), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 12, 30], auxA: [], items: [] },
    shaderTier: 0, // 最宽的闸：种类 3 仍然不产区间之下
  }));
  const result = exec(plan, [{ commandKey: 7, kind: 3, rangeA: range(10, 5), rangeB: range(0, 0) }]);

  assert.deepEqual(result.ops.map((op) => op.kind),
    ['target', 'clear', 'emit', 'target', 'composite']);
  const emitted = emitsInto(result, 'canvasA');
  assert.equal(emitted.length, 1, '⛔ 种类 3 凭空多一趟区间之下 ⇒ 键 < 起点的条目被多画一次');
  assert.deepEqual(keysOf(emitted[0]), [10, 12], '只画覆盖区间 [10,15) 里的条目');
  assert.equal(opsOf(result, 'clear').length, 1, '只清画布 A');
  const last = result.ops.at(-1);
  assert.equal(last.kind, 'composite');
  assert.deepEqual(last.sources, ['canvasA'], '单源合成（不是两块画布）');
  assert.deepEqual(result.diagnostics, [{ kind: 'scene-command-effect-unmodeled', commandKey: 7 }],
    '种类 3 只记“效果技法未建模”，不记别的缺口');
});

/* ──────────────────────────── 种类 2：B 段跳过 A 段 ──────────────────────────── */

test('★ 种类 2：B 段的区间之下不含 A 段的条目（两块画布不许互相污染）', () => {
  const { plan, commands } = twoCanvasFixture(2);
  const result = exec(plan, commands);

  const emitsB = emitsInto(result, 'canvasB');
  assert.deepEqual(emitsB.map(keysOf), [[5, 10, 15, 30], [45]],
    'B：区间之下 = 键 < 40 且**不在 A 段 [20,30) 里**（20/25 不许进；30 在区间外 ⇒ 照常参与）');
  const inARange = (key) => key >= 20 && key < 30;
  assert.deepEqual(emitsB[0].entries.filter((entry) => inARange(entry.key)), [],
    '⛔ B 段的区间之下混进 A 段条目 ⇒ 两块画布内容互相污染');
  // 同一条命令的 A 段不受影响
  assert.deepEqual(emitsInto(result, 'canvasA').map(keysOf), [[5, 10, 15], [20, 25]]);
  assert.deepEqual(result.diagnostics, []);
});

/* ──────────────────────────── 执行层的第二道闸 ──────────────────────────── */

test('★ 区间之下越界的条目被丢掉并记诊断（键 ≥ 本段起点的条目不许被画两次）', () => {
  const { plan, commands } = twoCanvasFixture(0);
  const belowA = plan.commands[0].phases.find((phase) => phase.stage === 'below-range' && phase.segment === 'A');
  // 人为把一条"区间 A 里"的条目塞进区间之下（规划器不该这么给，但执行层必须拒绝）
  belowA.entries = [...belowA.entries, { key: 25, table: 'mesh' }];
  belowA.tableKeys = belowA.entries.map((entry) => entry.key);

  const result = exec(plan, commands);
  assert.deepEqual(emitsInto(result, 'canvasA').map(keysOf), [[5, 10, 15], [20, 25]],
    '越界的 25 被丢掉（否则它会先被区间之下画一次、再被覆盖区间画一次）');
  assert.ok(result.diagnostics.some((entry) => entry.kind === 'scene-command-exec-below-range-out-of-range'
    && entry.commandKey === 99), '丢条目必须留痕');
});

/* ──────────────────────────── 对齐缺口 ──────────────────────────── */

test('★ 对齐缺口：输入少一条 / 种类对不上 ⇒ 不猜、不产 op，并记诊断', () => {
  const { plan, commands } = twoCanvasFixture(0);

  const missing = exec(plan, []);
  assert.deepEqual(missing.ops, [], '没有 kind / range ⇒ ⛔ 不猜种类');
  assert.deepEqual(missing.diagnostics,
    [{ kind: 'scene-command-exec-command-unmodeled', commandKey: 99 }]);

  const mismatch = exec(plan, [{ ...commands[0], kind: 3 }]);
  assert.deepEqual(mismatch.ops, [], '两边种类对不上 ⇒ ⛔ 不猜哪边对');
  assert.deepEqual(mismatch.diagnostics,
    [{ kind: 'scene-command-exec-kind-mismatch', commandKey: 99 }]);
});
