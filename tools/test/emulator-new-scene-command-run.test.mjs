/** @env pure @kind contract @why 命令档的 op **派发**写错时，画布会清错 / 清两次 / 拷贝方向反 / 合成顺序反（画面上只表现为"某层时有时无"），而像素看不出原因 —— 只有对着假宿主记下的调用序列能抓住 */
/**
 * tools/test/emulator-new-scene-command-run.test.mjs —— **命令档的执行（S2a-2，pure）**
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md`（`CORE-SCENE-DRAW-ORDER` / `CORE-OPS-SCENE-COMMAND-RECORDS`）
 * 与切片设计 `.tmp/spec-draft/batch26/s2/slice-plan.md` 的 **S2a**（“present 内、收尾扫描前执行 op”）。<br>
 * ★ 这里**不测像素**：`runCommandOps` 只把 op 逐条派发给注入的 `CommandCanvasHost`，守卫记录调用序列。
 *   Pixi 那半边（RenderTexture / blit / 合成）在 `pixi-host.ts`，需要 WebGL ⇒ pure 档测不到。
 *
 * ★ 为什么这些断言会红（每条都对应一种"静默偏掉"）：
 *   - 闸门关着却派发了 op ⇒ 产品画面在"没开闸"时就开始变（零回退失效）；
 *   - 种类闸门失效（未启用的种类仍产 op）⇒ 画布画了但没合成，条目从画面上**消失**；
 *   - `clear` 与 `target` 不成对 / 清错画布 / 清两次 ⇒ 低档不透明底色会串上一帧的残影；
 *   - `copy` 方向写反（own → 画布）⇒ 影片纹理被自己的旧内容盖住；
 *   - `composite` 的源次序反了（先 B 后 A）⇒ 两层叠放关系反了；
 *   - 命令档的 `emit` 丢了命令自己的 `skipExtraTransform` ⇒ 6.1 额外变换被乘两次 / 一次都没乘；
 *   - 命令档画到了后缓冲 ⇒ 会被同帧呈现看见（白闪）。
 *   - `present` op 被派发 ⇒ 命令档自己呈现（一帧两次呈现）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { planSceneCommandPass } from '../../apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts';
import { planCommandExecution, runCommandOps } from '../../apps/emulator-new/src/platform/electron/renderer/scene-command-exec.ts';
import {
  commandExecCommandsOf,
  gateCommandPlanByKinds,
  parseCommandExecKinds,
} from '../../apps/emulator-new/src/platform/electron/renderer/scene-order.ts';

const range = (start, count) => ({ start, count });

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

/**
 * **假宿主**：只记调用序列（不建任何东西）。
 * ★ 这正是"pixi 不可 pure 测试"的绕法：执行层只依赖这个接口。
 */
const fakeHost = () => {
  const calls = [];
  return {
    calls,
    selectTarget: (target, commandKey) => calls.push(['target', target, commandKey]),
    clearCanvas: (target, commandKey) => calls.push(['clear', target, commandKey]),
    emitEntries: (target, entries, skipExtraTransform, commandKey) => calls.push([
      'emit',
      target,
      entries.map((entry) => `${entry.table}#${entry.key}`),
      skipExtraTransform,
      commandKey,
    ]),
    copySurface: (from, to, commandKey) => calls.push(['copy', from, to, commandKey]),
    composite: (sources, into, progress, commandKey) => calls.push(['composite', [...sources], into, progress, commandKey]),
    strokes: (strokes, into, commandKey) => calls.push(['strokes', strokes.map((s) => s.canvas), into, commandKey]),
    mask: (from, into, rect, coeff, commandKey) => calls.push(['mask', from, into, rect, coeff, commandKey]),
  };
};

/* ──────────────────────────── 总闸 / 种类闸门 ──────────────────────────── */

test('★ 关闸（kinds 空）⇒ 零 op、零宿主调用；未启用的种类也不产 op', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 0, commandKey: 99, rangeA: range(20, 10), rangeB: range(40, 10) })],
    tables: { meshes: [20, 25], auxA: [], items: [] },
    shaderTier: 2,
  }));
  assert.ok(plan.commands.length > 0, '夹具前提：规划器给了阶段（否则下面的零 op 是恒真）');
  const commands = [{ commandKey: 99, kind: 0, rangeA: range(20, 10), rangeB: range(40, 10), progress: 0 }];

  // ① 总闸关：enabled 缺省 / false ⇒ 零 op
  assert.deepEqual(planCommandExecution({ plan, commands }).ops, [], '⛔ 缺省不许产 op（产品零回退）');
  // ② 种类闸门空 ⇒ commands 清空 ⇒ 即使 enabled=true 也零 op
  const gatedEmpty = gateCommandPlanByKinds(plan, []);
  assert.deepEqual(gatedEmpty.commands, [], '空列表 ⇒ 一条命令都不进执行计划');
  assert.equal(plan.commands.length, 1, '⛔ 闸门不许改原计划（纯函数）');
  const emptyExec = planCommandExecution({ plan: gatedEmpty, commands, enabled: true });
  // ③ 未启用的种类（只有 3）⇒ 种类 0 的命令不产 op
  const gatedOtherKind = gateCommandPlanByKinds(plan, [3]);
  const otherExec = planCommandExecution({ plan: gatedOtherKind, commands, enabled: true });
  assert.deepEqual(otherExec.ops, [], '⛔ 未启用的种类仍产 op ⇒ 画布画了但没合成（静默丢画）');

  // ④ 零 op ⇒ 一次宿主调用都没有
  for (const exec of [emptyExec, otherExec]) {
    const host = fakeHost();
    const result = runCommandOps(exec.ops, host);
    assert.deepEqual(host.calls, [], '零 op 必须零调用（否则关闸也会动画面）');
    assert.deepEqual(result, { ops: 0, canvasDraws: 0, canvasEntries: 0, skipped: 0 });
  }
});

/* ──────────────────────────── 种类 0：执行次序 ──────────────────────────── */

/** 种类 0 / 2 的共用夹具：区间 A [20,30) / 区间 B [40,50)，能力档 1（区间之下发生） */
const twoCanvasFixture = (kind) => {
  const ownerKey = kind === 2 ? 30 : 10;
  const plan = planSceneCommandPass(planInput({
    commands: [command({
      kind,
      commandKey: 99,
      ownerKey,
      split: kind === 2 ? 10 : 0,
      duration: 100,
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
  const commands = [{ commandKey: 99, kind, rangeA: range(20, 10), rangeB: range(40, 10), progress: 0 }];
  return { plan, commands };
};

test('★ 种类 0：派发次序 = A 趟（target/clear/两条 emit）→ B 趟 → target own → composite[A,B]；clear 与 target 成对', () => {
  const { plan, commands } = twoCanvasFixture(0);
  const exec = planCommandExecution({
    plan: gateCommandPlanByKinds(plan, [0]),
    commands,
    enabled: true,
    deviceReady: true,
  });
  const host = fakeHost();
  const result = runCommandOps(exec.ops, host);

  assert.deepEqual(host.calls, [
    ['target', 'canvasA', 99],
    ['clear', 'canvasA', 99],
    ['emit', 'canvasA', ['mesh#5', 'items#10', 'items#15'], false, 99],
    ['emit', 'canvasA', ['mesh#20', 'mesh#25'], false, 99],
    ['target', 'canvasB', 99],
    ['clear', 'canvasB', 99],
    ['emit', 'canvasB', ['mesh#5', 'items#10', 'items#15', 'items#30'], false, 99],
    ['emit', 'canvasB', ['mesh#45'], false, 99],
    ['target', 'own', 99],
    ['composite', ['canvasA', 'canvasB'], 'own', 0, 99],
  ], '⛔ 次序 / 目标 / 条目 / 源次序任意一处偏掉，画面上都只表现为"某层时有时无"');

  // clear 与 target **成对**：每个 clear 的前一次调用必须是同一块画布的 target
  host.calls.forEach((call, index) => {
    if (call[0] !== 'clear') return;
    assert.deepEqual(host.calls[index - 1]?.slice(0, 2), ['target', call[1]], '⛔ 清的不是刚切过去的那块画布');
  });
  assert.deepEqual(host.calls.filter((call) => call[0] === 'clear').map((call) => call[1]),
    ['canvasA', 'canvasB'], '⛔ 漏清 / 清两次：低档不透明底色会串上一帧的残影');
  // 合成是最后一个调用，两源 = 两块画布 → 命令自己的纹理
  assert.deepEqual(host.calls.at(-1), ['composite', ['canvasA', 'canvasB'], 'own', 0, 99]);
  // 统计口径：10 个 op / 其中 4 次画布 emit / 10 条条目（A：3+2、B：4+1）
  assert.deepEqual(result, { ops: 10, canvasDraws: 4, canvasEntries: 10, skipped: 0 });
});

/* ──────────────────────────── 种类 1：copy 方向 ──────────────────────────── */

test('★ 种类 1：copy 方向 = 画布 A → own（⛔ 不可反），且只在首帧派发', () => {
  const first = planSceneCommandPass(planInput({
    commands: [command({ kind: 1, commandKey: 5, timestamp: 0, rangeA: range(10, 2), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 11], auxA: [], items: [] },
    shaderTier: 1,
  }));
  const commands = [{ commandKey: 5, kind: 1, rangeA: range(10, 2), rangeB: range(0, 0) }];
  const exec = planCommandExecution({
    plan: gateCommandPlanByKinds(first, [1]),
    commands,
    enabled: true,
    deviceReady: true,
  });
  const host = fakeHost();
  runCommandOps(exec.ops, host);
  assert.deepEqual(host.calls.at(-1), ['copy', 'canvasA', 'own', 5],
    '⛔ 方向反了：用命令自己的纹理去盖画布（影片纹理被自己的旧内容盖住）');
  assert.equal(host.calls.filter((call) => call[0] === 'copy').length, 1, '首帧恰好一次整幅拷贝');

  // 非首帧：规划器只标记 ⇒ 零 op、零调用
  const later = planSceneCommandPass(planInput({
    commands: [command({ kind: 1, commandKey: 5, timestamp: 1000, rangeA: range(10, 2), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 11], auxA: [], items: [] },
    shaderTier: 1,
    now: 1016,
  }));
  const laterExec = planCommandExecution({
    plan: gateCommandPlanByKinds(later, [1]),
    commands,
    enabled: true,
    deviceReady: true,
  });
  const laterHost = fakeHost();
  runCommandOps(laterExec.ops, laterHost);
  assert.deepEqual(laterHost.calls, [], '⛔ 非首帧仍拷 ⇒ 每帧把画布内容糊到影片纹理上');
});

/* ──────────────────────────── 种类 3：单源合成 ──────────────────────────── */

test('★ 种类 3：单源合成（sources 只有画布 A），且没有区间之下那一趟', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 3, commandKey: 7, rangeA: range(10, 5), rangeB: range(0, 0) })],
    tables: { meshes: [5, 10, 12], auxA: [], items: [] },
    shaderTier: 0,
  }));
  const exec = planCommandExecution({
    plan: gateCommandPlanByKinds(plan, [3]),
    commands: [{ commandKey: 7, kind: 3, rangeA: range(10, 5), rangeB: range(0, 0) }],
    enabled: true,
    deviceReady: true,
  });
  const host = fakeHost();
  runCommandOps(exec.ops, host);
  assert.deepEqual(host.calls.at(-1), ['composite', ['canvasA'], 'own', 0, 7], '⛔ 类型 3 是单源合成');
  assert.equal(host.calls.filter((call) => call[0] === 'clear').length, 1, '只清画布 A');
});

/* ──────────────────────────── skipExtraTransform / 目标闭集 ──────────────────────────── */

test('★ 命令档的 emit 带命令自己的 skipExtraTransform；op 目标只在 36/37 与 own，不出现后缓冲', () => {
  /** 键窗 [20,25)：区间 A 起点 20 落在窗内 ⇒ planned.skipExtraTransform = true */
  const inWindow = planSceneCommandPass(planInput({
    commands: [command({ kind: 0, commandKey: 99, rangeA: range(20, 10), rangeB: range(40, 10) })],
    tables: { meshes: [5, 20, 25, 45], auxA: [], items: [10, 15, 30, 50] },
    selfKeyWindow: { start: 20, count: 5 },
    shaderTier: 1,
  }));
  const commands = [{ commandKey: 99, kind: 0, rangeA: range(20, 10), rangeB: range(40, 10) }];
  const exec = planCommandExecution({ plan: inWindow, commands, enabled: true, deviceReady: true });
  const emits = exec.ops.filter((op) => op.kind === 'emit');
  assert.ok(emits.length > 0, '夹具前提：有 emit 可查');
  assert.ok(emits.every((op) => op.skipExtraTransform === true),
    '⛔ 丢了命令自己的标志 ⇒ 6.1 额外变换被乘两次（收尾扫描恒 false，两条路必须各自带自己的值）');

  // 默认键窗为空 ⇒ 恒 false（命令档不跳过）
  const { plan } = twoCanvasFixture(0);
  const noWindow = planCommandExecution({ plan, commands, enabled: true, deviceReady: true });
  const noWindowEmits = noWindow.ops.filter((op) => op.kind === 'emit');
  assert.ok(noWindowEmits.every((op) => op.skipExtraTransform === false));

  // 目标闭集：⛔ 命令档不许画到后缓冲
  const targets = new Set(['canvasA', 'canvasB', 'own']);
  for (const op of exec.ops) {
    const used = op.kind === 'target' || op.kind === 'clear' ? [op.target]
      : op.kind === 'emit' ? [op.into]
        : op.kind === 'copy' ? [op.from, op.to]
          : op.kind === 'composite' ? [...op.sources, op.into]
            : [];
    for (const target of used) assert.ok(targets.has(target), `⛔ op 目标 ${target} 落到了后缓冲 / 非法目标`);
  }
  assert.equal(exec.ops.some((op) => op.kind === 'present'), false, '本层不产 present（呈现归 present()）');
});

/* ──────────────────────────── present op / 投影 ──────────────────────────── */

test('★ `present` op 不被派发（⛔ 命令档自己呈现）；`commandExecCommandsOf` 只投 kind / range', () => {
  const host = fakeHost();
  const result = runCommandOps(
    [{ kind: 'present', target: 'own', commandKey: 1 }],
    host,
  );
  assert.deepEqual(host.calls, [], '⛔ 命令档自己 present ⇒ 一帧两次呈现 / 画到不该画的地方');
  assert.deepEqual(result, { ops: 0, canvasDraws: 0, canvasEntries: 0, skipped: 1 });

  // 投影：只给执行器要的三样（⛔ 不把整条快照塞过去，也别补默认值）
  const commandSnapshot = {
    ...command({ kind: 2, commandKey: 42, rangeA: range(20, 10), rangeB: range(40, 10) }),
    rect: { left: 1, top: 2, right: 3, bottom: 4 },
    flag: 1,
    mask: 255,
  };
  const scene = { commands: [commandSnapshot], clock: 0 };
  const projected = commandExecCommandsOf(scene);
  assert.deepEqual(projected, [{
    commandKey: 42,
    kind: 2,
    rangeA: range(20, 10),
    rangeB: range(40, 10),
    progress: 0,
    rect: { left: 1, top: 2, right: 3, bottom: 4 },
    flag: 1,
    mask: 255,
  }], '⛔ 投影多一格 / 少一格都会让执行器与规划器按不同的输入对齐');
  for (const entry of projected) {
    assert.deepEqual(Object.keys(entry).sort(), ['commandKey', 'flag', 'kind', 'mask', 'progress', 'rangeA', 'rangeB', 'rect'],
      '只投执行器认识的格 + 进度 / 遮罩');
  }
});

test('★ 种类 0 的进度透传到 composite；种类 3 单源也用同一进度', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 0, commandKey: 99, rangeA: range(20, 10), rangeB: range(40, 10) })],
    tables: { meshes: [20], auxA: [], items: [] },
    shaderTier: 2,
  }));
  const exec = planCommandExecution({
    plan: gateCommandPlanByKinds(plan, [0]),
    commands: [{ commandKey: 99, kind: 0, rangeA: range(20, 10), rangeB: range(40, 10), progress: 0.5 }],
    enabled: true,
  });
  const host = fakeHost();
  runCommandOps(exec.ops, host);
  assert.deepEqual(host.calls.at(-1), ['composite', ['canvasA', 'canvasB'], 'own', 0.5, 99],
    '退路合成第二笔 alpha 必须是进度（不是恒 1）');
});

test('★ commandExecCommandsOf 的进度：未开始 0 / 进行中线性 / 到期 1', () => {
  const make = (over) => ({ commands: [{ ...command({ kind: 0, commandKey: 1 }), ...over }], clock: over.clock });
  const at = (timestamp, delay, duration, clock) => {
    const scene = make({ timestamp, delay, duration });
    scene.clock = clock;
    return commandExecCommandsOf(scene)[0].progress;
  };
  assert.equal(at(100, 50, 100, 100), 0, '未开始（now <= ts+delay）');
  assert.equal(at(100, 50, 100, 200), 0.5, '进行中线性');
  assert.equal(at(100, 50, 100, 250), 1, '到期 = 1（不 clamp 到中间值）');
  assert.equal(at(100, 50, 0, 200), 1, 'duration <= 0 ⇒ 到期档');
});

test('★ 种类 3 的进度不夹取：已过为负时按同一公式外推，到期才 1', () => {
  const scene = (clock) => ({ commands: [{ ...command({ kind: 3, commandKey: 1, timestamp: 100, delay: 50, duration: 100 }) }], clock });
  assert.equal(commandExecCommandsOf(scene(100))[0].progress, -0.5, '未开始也不夹成 0');
  assert.equal(commandExecCommandsOf(scene(150))[0].progress, 0);
  assert.equal(commandExecCommandsOf(scene(200))[0].progress, 0.5);
  assert.equal(commandExecCommandsOf(scene(250))[0].progress, 1);
});

test('★ 种类 2：有抹除笔时产 strokes（A→B 顺序保序），不再产 composite', () => {
  const { plan, commands } = twoCanvasFixture(2);
  const exec = planCommandExecution({
    plan: gateCommandPlanByKinds(plan, [2]),
    commands: commands.map((c) => ({ ...c, progress: 0 })),
    enabled: true,
    deviceReady: true,
  });
  const host = fakeHost();
  runCommandOps(exec.ops, host);
  const last = host.calls.at(-1);
  assert.equal(last[0], 'strokes', '有笔时不许再用 composite 冒充图案');
  assert.ok(last[1].length >= 2, '至少一族两笔');
  assert.equal(last[1][0], 'A', '每带先 A');
  assert.equal(last[1][1], 'B', '后 B');
});

test('★ 种类 1：首帧 copy 后接 mask；非首帧只有 mask（不再 copy）', () => {
  const plan = planSceneCommandPass(planInput({
    commands: [command({ kind: 1, commandKey: 5, timestamp: 0, rangeA: range(10, 2), rangeB: range(0, 0) })],
    tables: { meshes: [], auxA: [], items: [10] },
    shaderTier: 2,
  }));
  const cmd = { commandKey: 5, kind: 1, rangeA: range(10, 2), rangeB: range(0, 0), progress: 0, rect: { left: 0, top: 0, right: 8, bottom: 8 }, flag: 1, mask: 255 };
  const first = planCommandExecution({ plan: gateCommandPlanByKinds(plan, [1]), commands: [cmd], enabled: true });
  const host = fakeHost();
  runCommandOps(first.ops, host);
  const types = host.calls.map((c) => c[0]);
  assert.ok(types.includes('copy') && types.indexOf('copy') < types.indexOf('mask'), '首帧 copy 之后必须接 mask');
  assert.equal(host.calls.at(-1)[4], 254 / 255, 'flag=1 的均匀系数（255→254/255）');

  const later = planCommandExecution({ plan: gateCommandPlanByKinds(plan, [1]), commands: [{ ...cmd, progress: 0 }], enabled: true });
  // 直接把 plan 的 firstFrame 翻成 false 不现实；用 timestamp 非本帧的场景再规划一次
  const planLater = planSceneCommandPass(planInput({
    commands: [command({ kind: 1, commandKey: 5, timestamp: 1, rangeA: range(10, 2), rangeB: range(0, 0) })],
    tables: { meshes: [], auxA: [], items: [10] },
    shaderTier: 2,
    now: 2,
  }));
  const execLater = planCommandExecution({ plan: gateCommandPlanByKinds(planLater, [1]), commands: [cmd], enabled: true });
  const host2 = fakeHost();
  runCommandOps(execLater.ops, host2);
  assert.ok(host2.calls.every((c) => c[0] !== 'copy'), '非首帧不许 copy');
  assert.equal(host2.calls.at(-1)[0], 'mask', '非首帧仍要遮罩混合');
});

test('★ 种类 0 走退路时记 effect-unmodeled；种类 2 有笔不记', () => {
  const k0 = twoCanvasFixture(0);
  const e0 = planCommandExecution({ plan: gateCommandPlanByKinds(k0.plan, [0]), commands: k0.commands.map((c) => ({ ...c, progress: 0 })), enabled: true, deviceReady: true });
  assert.ok(e0.diagnostics.some((d) => d.kind === 'scene-command-effect-unmodeled' && d.commandKey === 99), '种类 0 必须记效果未建模');

  const k2 = twoCanvasFixture(2);
  const e2 = planCommandExecution({ plan: gateCommandPlanByKinds(k2.plan, [2]), commands: k2.commands.map((c) => ({ ...c, progress: 0 })), enabled: true, deviceReady: true });
  assert.ok(!e2.diagnostics.some((d) => d.kind === 'scene-command-effect-unmodeled'), '种类 2 有笔时不该报影响 0/3 的缺口');
});

test('★ parseCommandExecKinds：只收 0..3、去重、未知丢弃；空 ⇒ 零回退', () => {
  assert.deepEqual(parseCommandExecKinds(null), []);
  assert.deepEqual(parseCommandExecKinds(''), []);
  assert.deepEqual(parseCommandExecKinds('0,1,2'), [0, 1, 2]);
  assert.deepEqual(parseCommandExecKinds('3,3,9,-1,x,1.5'), [3]);
  assert.deepEqual(parseCommandExecKinds(' 0 , 7 '), [0]);
});
