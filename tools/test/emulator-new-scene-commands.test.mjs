/** @env pure @kind contract @why 命令记录的各格映射错（矩形少加宽高 / 变体对调 / 时间戳不清 0）、种类 0 请求失败却跳过写表、种类 1 槽空却照写、命令档被假装执行 —— 都只会让绘制顺序与动画门静默偏掉 */
/**
 * tools/test/emulator-new-scene-commands.test.mjs —— **场景命令记录族（写侧）** + 命令档诊断 + 负键口径
 *
 * 判据来自 `data/spec/chapters/60-ops-scene.md`：
 *   - `CORE-OPS-SCENE-COMMAND-RECORDS`：五条写侧（0x223 / 0x24d / 0x24f / 0x250 / 0x251）的
 *     **同键替换**、种类 / 变体、各条自己的格（种类 1 的矩形 / 标志 / 延迟 / 持续、种类 3 的两组起终对）、
 *     **时间戳写表时清 0**、置脏、只有种类 1 置「有动画」、种类 2 的两条校验后果不同、
 *     种类 0 的建 / 换纹理请求**失败也照常写表**（+ 错误通道一条带编号说明 + 槽里留「建失败」对象）、
 *     种类 1 的纹理槽为空 ⇒ **抛显示消息型异常且旧记录不动**、**命令不立即画**；
 *   - `CORE-SCENE-DRAW-ORDER`：负键口径**按表分**（网格 / 辅助表的负键不参与；**绘制项不做符号检查**）。
 *
 * ★ 为什么这些断言会红：
 *   - 同键写成"追加"：表里两条，同一键会被执行两次（渲染时看得出来，其它用例看不出来）；
 *   - 种类 1 的矩形存成 `{left, top, width, height}`：读者手里没有"右 / 下"两格；
 *   - 种类 3 两变体共用一个布局：`0x250` 会把角度读成中心 U；
 *   - 时间戳不清 0：重写命令后**永远不再进"第一帧"**（或被当成"每帧都画"）；
 *   - 种类 0 请求失败就 return：命令表里没有记录（画面上什么都不发生），且错误通道沉默；
 *   - 种类 1 槽空却照写：本该终止的一条命令写进了记录（渲染步读一条已失效的影片纹理）；
 *   - 种类 2 把两条校验合成"非法就拒绝"：类型 = 12 时旧记录会被删、分割幅 = 0 时不会写出 `mode = -1`；
 *   - 种类 1 的「有动画」漏置（或给 0/2/3 也置）：等待门的行为跟着变；
 *   - handler 里顺手发绘制请求：命令被立即画一次（两档顺序无从谈起）；
 *   - 负键一律跳过：**只放一条负键绘制项**时它永远不出现（spec 的纠错点）。
 * ★ 命令档（有命令记录时的第一档）**只规划、不执行**：这里只守"它没有被假装执行"
 *   （两条诊断各报一次：`scene-command-pass-planned` + `scene-command-pass-render-unmodeled`）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { HeadlessRenderHost } from '../../apps/emulator-new/src/core/render.ts';
import {
  SCENE_COMMAND_PASS_PLANNED,
  SCENE_COMMAND_PASS_RENDER_UNMODELED,
  SceneOrderNotes,
  orderedDrawables,
} from '../../apps/emulator-new/src/platform/electron/renderer/scene-order.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

/** 把 [opcode, [[type, raw], ...]] 拼成一段 body（不含头） */
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

function machineFor(instrs, extra = {}) {
  return new Machine({ body: script(instrs), table, key: KEY, ...extra });
}

/**
 * Machine → 命令档规划器要的那份**场景快照**（只填 `scene-order.ts` 的投影读的字段）。
 * ★ 字段与 `session.ts` 的 `snapshot()` 同口径：命令表按命令键升序、区间 A / B 的挪移、
 *   `shaderTier: null`（未建模）、`forcedComplete: 0`（核心没有这一格）。
 */
function sceneOf(machine) {
  const commands = [...machine.scene.commands]
    .sort((a, b) => a[0] - b[0])
    .map(([commandKey, record]) => {
      const twoSegments = record.kind === 0 || record.kind === 2;
      return {
        kind: record.kind,
        commandKey,
        timestamp: record.timestamp,
        delay: record.delay,
        duration: record.duration,
        textureId: record.textureId,
        rangeA: twoSegments
          ? { start: record.segmentA[0], count: record.segmentA[1] }
          : { start: record.rangeStart, count: record.rangeCount },
        rangeB: twoSegments
          ? { start: record.segmentB[0], count: record.segmentB[1] }
          : { start: 0, count: 0 },
        mode: record.mode,
        split: record.split,
        ownerKey: record.ownerKey,
      };
    });
  return {
    commands,
    clock: machine.scene.now,
    selfRecord: { keyStart: 0, keyCount: 0 },
    meshes: [...machine.scene.meshes.keys()].map((key) => ({ key })),
    drawItems: [...machine.scene.drawItems.keys()].map((key) => ({ key })),
    shaderTier: null,
    animating: machine.scene.animating,
    forcedComplete: 0,
  };
}

/** 立即数整数操作数（type 0） */
const ints = (...values) => values.map((raw) => ({ type: 0, raw: raw >>> 0 }));

/** 一个「已存在」的纹理对象（只需可区分；尺寸只为记账） */
const texture = (width = 16, height = 8) => ({ kind: 'normal', width, height });

/** 种类 0（0x223）的 8 个操作数：键、纹理、两段（起点、个数）、延迟、持续 */
const command223 = (key, textureId, startA = 0, countA = 2, startB = 1, countB = 1, delay = 100, duration = 200) =>
  [0x223, [[0, key], [0, textureId], [0, startA], [0, countA], [0, startB], [0, countB], [0, delay], [0, duration]]];

/**
 * 种类 1（0x24d）的 12 个操作数：
 * 键、纹理、区间（起点、个数）、矩形（左、上、宽、高）、标志、影片名表索引、延迟、持续。
 * ★ 第 10 个（影片名表索引）**不进记录**，这里给一个与其它格都不重复的数以便发现"被误写"。
 */
const command24d = (key, textureId, {
  rangeStart = 40, rangeCount = 5,
  left = 100, top = 200, width = 30, height = 40,
  flag = 1, movieName = 917, delay = 11, duration = 12,
} = {}) => [0x24d, [
  [0, key], [0, textureId], [0, rangeStart], [0, rangeCount],
  [0, left], [0, top], [0, width], [0, height],
  [0, flag], [0, movieName], [0, delay], [0, duration],
]];

/**
 * 种类 2（0x24f）的 10 个操作数：
 * 命令键、纹理、区间 A（起点、个数）、区间 B（起点、个数）、类型 / 模式、分割幅、延迟、持续。
 * ★ 逐字段的守卫直接给 1..10 这十个**互异**的值（`command24f` 的默认值只为既有用例保持形状）。
 */
const command24f = (key, mode, split, extra = {}) => {
  const { textureId = 0, startA = 1, countA = 2, startB = 3, countB = 4, delay = 0, duration = 0 } = extra;
  return [0x24f, [
    [0, key], [0, textureId], [0, startA], [0, countA], [0, startB], [0, countB],
    [0, mode], [0, split], [0, delay], [0, duration],
  ]];
};

/**
 * 种类 3 变体 0（0x250）的 10 个操作数：
 * 键、纹理、区间（起点、个数）、长度（起、终）、角度（起、终）、延迟、持续。
 */
const command250 = (key, textureId, {
  rangeStart = 1, rangeCount = 2,
  lengthFrom = 3, angleFrom = 4, lengthTo = 5, angleTo = 6,
  delay = 7, duration = 8,
} = {}) => [0x250, [
  [0, key], [0, textureId], [0, rangeStart], [0, rangeCount],
  [0, lengthFrom], [0, angleFrom], [0, lengthTo], [0, angleTo],
  [0, delay], [0, duration],
]];

/**
 * 种类 3 变体 1（0x251）的 12 个操作数：
 * 键、纹理、区间（起点、个数）、长度（起、终）、中心 U（起、终）、中心 V（起、终）、延迟、持续。
 */
const command251 = (key, textureId, {
  rangeStart = 1, rangeCount = 2,
  lengthFrom = 3, centerUFrom = 4, centerVFrom = 5,
  lengthTo = 6, centerUTo = 7, centerVTo = 8,
  delay = 9, duration = 10,
} = {}) => [0x251, [
  [0, key], [0, textureId], [0, rangeStart], [0, rangeCount],
  [0, lengthFrom], [0, centerUFrom], [0, centerVFrom],
  [0, lengthTo], [0, centerUTo], [0, centerVTo],
  [0, delay], [0, duration],
]];

/* ──────────────────────────── 写侧 ──────────────────────────── */

test('★ 五条只写命令表：种类 / 变体正确、置脏、推进按 2·argc+1、且一条绘制请求都不发', async () => {
  const host = new HeadlessRenderHost();
  const m = machineFor([
    command223(10, 7),
    command24d(20, 21),
    // ★ 种类 2 写表前会按第 2 个操作数发建 / 换纹理请求；这里给它一个**已存在**的槽，
    //   于是这条用例的 `host.requests` 仍然只可能来自绘制（= 空）。
    command24f(30, 3, 2, { textureId: 7 }),
    command250(40, 100),
    command251(50, 200),
  ], { renderHost: host });
  // 四条读纹理槽的命令各给一个**已存在**的对象（格式比较不在本用例范围 ⇒ 平台宿主只记诊断）
  m.scene.textureObjects.set(7, texture());
  m.scene.textureObjects.set(21, texture());
  m.scene.textureObjects.set(100, texture());
  m.scene.textureObjects.set(200, texture());
  const r = await m.run();
  assert.equal(r.steps, 5, '五条都被分派执行（推进不是 2·argc+1 会跳飞 / 重叠）');
  assert.equal(r.stop.kind, 'truncated');
  assert.equal(m.scene.commands.size, 5, '五个不同的命令键 ⇒ 五条记录');
  assert.deepEqual(
    [10, 20, 30, 40, 50].map((key) => m.scene.commands.get(key)?.kind),
    [0, 1, 2, 3, 3],
  );
  assert.deepEqual(
    [10, 20, 30, 40, 50].map((key) => m.scene.commands.get(key)?.timestamp),
    [0, 0, 0, 0, 0],
    '写表时时间戳一律清 0（五条都一样）',
  );
  assert.equal(m.scene.commands.get(40).variant, 0, '0x250 = 种类 3 变体 0');
  assert.equal(m.scene.commands.get(50).variant, 1, '0x251 = 种类 3 变体 1');
  const rec223 = m.scene.commands.get(10);
  assert.equal(rec223.textureId, 7, '种类 0 的第 2 个操作数是纹理 id');
  assert.deepEqual(rec223.segmentA, [0, 2], '种类 0 的操作数 3 / 4 是一段（起点、个数）');
  assert.deepEqual(rec223.segmentB, [1, 1], '种类 0 的操作数 5 / 6 是另一段');
  assert.deepEqual([rec223.delay, rec223.duration], [100, 200]);
  assert.equal(m.scene.dirty, 1, '写成功后置场景脏');
  assert.deepEqual(host.requests, [], '⛔ 命令不立即画：五条 handler 都不发绘制请求');
});

test('★ 种类 1：12 个操作数正确落字段（矩形右 / 下 = 5+7 / 6+8、flag = 9、延迟 11、持续 12）', async () => {
  const m = machineFor([]);
  m.scene.textureObjects.set(21, texture());
  await HANDLERS.get(0x24d)(m, ints(5, 21, 40, 5, 100, 200, 30, 40, 1, 917, 11, 12));
  const rec = m.scene.commands.get(5);
  assert.ok(rec, '槽非空 ⇒ 写记录');
  assert.equal(rec.kind, 1);
  assert.equal(rec.textureId, 21, '第 2 个操作数是影片纹理编号');
  assert.deepEqual([rec.rangeStart, rec.rangeCount], [40, 5], '第 3 / 4 个是覆盖区间的起点 / 个数');
  assert.deepEqual(
    rec.rect,
    { left: 100, top: 200, right: 130, bottom: 240 },
    '右 / 下 = 左 + 第 7、上 + 第 8（宽高是**加出来的**）',
  );
  assert.deepEqual(
    Object.keys(rec.rect).sort(),
    ['bottom', 'left', 'right', 'top'],
    '⛔ 记录里没有宽 / 高两格（不许存成 {width, height}）',
  );
  assert.equal(rec.flag, 1, '第 9 个操作数是那个取 0 / 1 的标志');
  assert.deepEqual([rec.delay, rec.duration], [11, 12], '延迟 / 持续是第 11 / 12 个');
  assert.equal(rec.timestamp, 0, '写表时时间戳清 0（「第一帧」判据）');
  assert.equal(m.scene.dirty, 1, '写成功 ⇒ 置脏');
  assert.equal(m.scene.animating, 1, '种类 1 还置「有动画」（与「脏」是两个格）');
});

test('★ 种类 1：纹理槽为空 ⇒ 抛显示消息型异常终止本条，旧记录一格不动（⛔ 不是错误通道上报）', async () => {
  const m = machineFor([]);
  m.scene.textureObjects.set(21, texture());
  await HANDLERS.get(0x24d)(m, ints(5, 21, 40, 5, 100, 200, 30, 40, 1, 917, 11, 12));
  const before = m.scene.commands.get(5);
  assert.ok(before);

  m.scene.textureObjects.delete(21);
  m.scene.dirty = 0;
  m.scene.animating = 0;
  await assert.rejects(
    async () => HANDLERS.get(0x24d)(m, ints(5, 21, 0, 0, 0, 0, 0, 0, 0, 0, 7, 8)),
    /テクスチャが確保されていません/,
    '槽空 ⇒ 引擎抛显示消息型异常',
  );
  assert.equal(m.scene.commands.size, 1, '⛔ 不建新记录');
  assert.equal(m.scene.commands.get(5), before, '旧记录同一个对象、一格不动（抛在删旧记录之前）');
  assert.deepEqual(
    [before.textureId, before.rangeStart, before.rangeCount, before.flag, before.delay, before.duration],
    [21, 40, 5, 1, 11, 12],
    '旧记录的格没被第二次调用改写',
  );
  assert.equal(m.scene.errorBuffer, '', '⛔ 不是错误通道上报（与种类 0 的失败路径不同）');
  assert.equal(m.scene.dirty, 0, '抛在写表之前 ⇒ 不置脏');
  assert.equal(m.scene.animating, 0, '也不置「有动画」');
});

test('★ 种类 3：0x250 / 0x251 的变体、起终对、延迟持续位置都不同；写表前先发建纹理请求；时间戳 = 0', async () => {
  const host = new HeadlessRenderHost();
  const m = machineFor([], { renderHost: host });
  await HANDLERS.get(0x250)(m, ints(50, 500, 1, 2, 3, 4, 5, 6, 7, 8));
  await HANDLERS.get(0x251)(m, ints(51, 501, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  assert.deepEqual(
    host.requests.map((request) => [request.kind, request.request?.textureId]),
    [['create-texture', 500], ['create-texture', 501]],
    '两条都在写表**之前**按第 2 个操作数发建纹理请求（槽空）',
  );

  const v0 = m.scene.commands.get(50);
  assert.equal(v0.kind, 3);
  assert.equal(v0.variant, 0, '0x250 = 变体 0（滑移模糊）');
  assert.equal(v0.textureId, 500, '第 2 个 = 纹理编号');
  assert.deepEqual([v0.rangeStart, v0.rangeCount], [1, 2], '第 3 / 4 个 = 区间起点 / 个数');
  assert.deepEqual(v0.rampLength, [3, 5], '变体 0 的长度对 = 第 5 → 第 7');
  assert.deepEqual(v0.rampAngle, [4, 6], '变体 0 的角度对 = 第 6 → 第 8');
  assert.deepEqual(v0.rampCenterU, [0, 0], '变体 0 不用中心 U ⇒ 两格 0');
  assert.deepEqual(v0.rampCenterV, [0, 0], '变体 0 不用中心 V ⇒ 两格 0');
  assert.deepEqual([v0.delay, v0.duration], [7, 8], '变体 0 的延迟 / 持续 = 第 9 / 10');
  assert.equal(v0.timestamp, 0, '写表时时间戳清 0');

  const v1 = m.scene.commands.get(51);
  assert.equal(v1.kind, 3);
  assert.equal(v1.variant, 1, '0x251 = 变体 1（缩放模糊）');
  assert.equal(v1.textureId, 501);
  assert.deepEqual([v1.rangeStart, v1.rangeCount], [1, 2]);
  assert.deepEqual(v1.rampLength, [3, 6], '变体 1 的长度对 = 第 5 → 第 8');
  assert.deepEqual(v1.rampCenterU, [4, 7], '变体 1 的中心 U 对 = 第 6 → 第 9');
  assert.deepEqual(v1.rampCenterV, [5, 8], '变体 1 的中心 V 对 = 第 7 → 第 10');
  assert.deepEqual(v1.rampAngle, [0, 0], '变体 1 不用角度 ⇒ 两格 0');
  assert.deepEqual([v1.delay, v1.duration], [9, 10], '变体 1 的延迟 / 持续 = 第 11 / 12');
  assert.equal(v1.timestamp, 0);

  assert.notDeepEqual(
    [v0.rampLength, v0.rampAngle, v0.delay],
    [v1.rampLength, v1.rampAngle, v1.delay],
    '⛔ 两变体不许共用一套布局（否则 0x250 会把角度读成中心 U）',
  );
  assert.equal(m.scene.dirty, 1, '写成功 ⇒ 置脏');
  assert.equal(m.scene.animating, 0, '种类 3 写表时**不**置「有动画」（那由推进决定）');
});

/* ──────────────────────────── 种类 0 的建 / 换纹理请求与失败路径 ──────────────────────────── */

test('★ 0x223 的槽空 ⇒ 请求建纹理；格式不符 ⇒ 请求换纹理；格式相符 ⇒ 一个请求都不发', async () => {
  const empty = machineFor([command223(10, 7)], { renderHost: new HeadlessRenderHost() });
  await empty.run();
  assert.deepEqual(
    empty.renderHost.requests.map((request) => request.kind),
    ['create-texture'],
    '槽空 ⇒ 建纹理请求',
  );
  assert.equal(empty.scene.commands.size, 1, '请求之后照常写命令记录');

  const mismatch = machineFor([command223(10, 7)], {
    renderHost: new HeadlessRenderHost({ textureFormatMatches: () => false }),
  });
  mismatch.scene.textureObjects.set(7, texture());
  await mismatch.run();
  assert.deepEqual(
    mismatch.renderHost.requests.map((request) => request.kind),
    ['texture-format-matches', 'replace-texture'],
    '槽非空且平台说格式不符 ⇒ 换纹理请求',
  );

  const coincident = machineFor([command223(10, 7)], {
    renderHost: new HeadlessRenderHost({ textureFormatMatches: () => true }),
  });
  coincident.scene.textureObjects.set(7, texture());
  await coincident.run();
  assert.deepEqual(
    coincident.renderHost.requests.map((request) => request.kind),
    ['texture-format-matches'],
    '格式相符 ⇒ 一个建 / 换请求都不发（因此也不会产生失败上报）',
  );
  assert.equal(coincident.scene.errorBuffer, '', '相符路径不写错误通道');

  const noHost = machineFor([command223(10, 7)]);
  noHost.scene.textureObjects.set(7, texture());
  await noHost.run();
  assert.equal(noHost.scene.commands.size, 1, '没有宿主也照常写表');
  assert.ok(
    noHost.diagnostics.some((d) => d.kind === 'scene-command-texture-format-unmodeled'),
    '没有宿主 ⇒ 记诊断，⛔ 不静默',
  );
});

test('★ 种类 0 的请求失败：记录照写、场景照脏、错误通道一条带编号说明（不带脚本位置）、槽里留「建失败」对象', async () => {
  const host = new HeadlessRenderHost({ createTextureSucceeds: false });
  const m = machineFor([], { renderHost: host });
  m.scene.textureObjects.set(7, texture(4, 4));
  // 先用同键写一条旧记录（纹理 7 的槽在、平台不提供格式比较 ⇒ 只记诊断、不发请求）
  await HANDLERS.get(0x223)(m, ints(9, 7, 0, 2, 1, 1, 100, 200));
  const old = m.scene.commands.get(9);
  assert.equal(old.textureId, 7);
  assert.equal(m.scene.errorBuffer, '', '前一次没有请求 ⇒ 错误通道还是空的');

  // 第二次：空槽纹理 123 ⇒ 建纹理请求失败。顺手验证**请求发生在删旧 / 写表之前**。
  const create = host.createTexture.bind(host);
  let recordDuringRequest;
  host.createTexture = async (request) => {
    recordDuringRequest = m.scene.commands.get(9);
    return create(request);
  };
  m.scene.dirty = 0;
  await HANDLERS.get(0x223)(m, ints(9, 123, 0, 2, 1, 1, 300, 400));

  assert.equal(recordDuringRequest, old, '请求发出时命令表里还是旧记录 ⇒ 请求在删旧记录之前');
  const record = m.scene.commands.get(9);
  assert.ok(record, '⚠ 请求失败**不阻止**写表');
  assert.notEqual(record, old, '旧记录被删、新记录被写（同键是替换）');
  assert.deepEqual([record.textureId, record.delay, record.duration], [123, 300, 400], '新记录写的是第二次的格');
  assert.equal(m.scene.dirty, 1, '请求失败也照样置脏');
  assert.match(m.scene.errorBuffer, /TEXTURE=123/, '错误通道收到一条**带纹理编号**的说明');
  assert.match(m.scene.errorBuffer, /テクスチャ作成に失敗/, '说明文本来自引擎的建纹理失败格式串');
  assert.doesNotMatch(
    m.scene.errorBuffer,
    /行目\)|\(\S+：\d+/,
    '⛔ 不带脚本位置前缀（那是绘制阶段的上报入口，与这条不同）',
  );
  const object = m.scene.textureObjects.get(123);
  assert.ok(object, '槽里**仍然留下一个对象**（「建失败」⇒ 槽不是空的）');
  assert.equal(object.creationFailed, true, '对象带着「建失败」标记');
});

test('★ 种类 0 失败后再次同编号写入：槽非空且格式相符 ⇒ 不再发请求（判据退化为「格式是否相符」）', async () => {
  const host = new HeadlessRenderHost({ createTextureSucceeds: false, textureFormatMatches: () => true });
  const m = machineFor([command223(1, 55), command223(2, 55)], { renderHost: host });
  const r = await m.run();
  assert.equal(r.steps, 2);
  assert.deepEqual(
    host.requests.map((request) => request.kind),
    ['create-texture', 'texture-format-matches'],
    '第一次建失败；第二次槽非空 ⇒ 只问格式，相符 ⇒ 一个请求都不发',
  );
  assert.equal(
    host.requests.filter((request) => request.kind === 'create-texture').length,
    1,
    '⛔ 不许"槽里有对象就无条件重建"',
  );
  assert.ok(m.scene.commands.has(1) && m.scene.commands.has(2), '两条记录都写了（失败不阻止写表）');
  assert.match(m.scene.errorBuffer, /TEXTURE=55/, '失败上报只有第一次那一条');
  assert.equal(m.scene.textureObjects.get(55).creationFailed, true);
});

/* ──────────────────────────── 时间戳 ──────────────────────────── */

test('★ 时间戳：写表后为 0；重写同一命令键重新清 0（下一次遍历又回到「第一帧」）', async () => {
  const m = machineFor([]);
  await HANDLERS.get(0x250)(m, ints(5, 1, 1, 2, 3, 4, 5, 6, 7, 8));
  const before = m.scene.commands.get(5);
  assert.equal(before.timestamp, 0, '写表后清 0');

  // 模拟命令档遍历时盖章（命令档本身未建模；这里只改那一格）
  before.timestamp = 123_456;
  await HANDLERS.get(0x250)(m, ints(5, 1, 1, 2, 3, 4, 5, 6, 7, 8));
  const after = m.scene.commands.get(5);
  assert.notEqual(after, before, '同键是替换（新记录对象）');
  assert.equal(after.timestamp, 0, '重写同一命令键 ⇒ 时间戳重新清 0');
});

/* ──────────────────────────── 同键替换 / 表边界 ──────────────────────────── */

test('★ 同键连写两次 ⇒ 命令表只一条，且记录是第二次的（替换，不是追加）', async () => {
  const m = machineFor([command250(5, 111), command250(5, 777)]);
  await m.run();
  assert.equal(m.scene.commands.size, 1, '同键是替换：表里不该有两条');
  assert.equal(m.scene.commands.get(5).kind, 3);
  assert.equal(m.scene.commands.get(5).textureId, 777, '留下的是第二次写进去的那些格');
});

test('★ 0x1f6 只清四张同键表：命令表**不动**（spec：清空范围是四张表）', async () => {
  const m = machineFor([command250(5, 1), [0x1f6, []]]);
  await m.run();
  assert.ok(m.scene.commands.get(5), '命令表是第五张表，不在 0x1f6 的清空范围');
  assert.equal(m.scene.dirty, 0, '0x1f6 的既有语义仍然成立（脏清 0）');
});

/* ──────────────────────────── 种类 2（0x24f 抹除命令） ──────────────────────────── */

test('★ 种类 2：十个操作数逐字段落记录（1..10 互异值，反"操作数错位"）', async () => {
  const m = machineFor([]);
  m.scene.animating = 0;
  m.scene.dirty = 0;
  await HANDLERS.get(0x24f)(m, ints(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
  const record = m.scene.commands.get(1);
  assert.ok(record, '校验通过 ⇒ 写记录');
  assert.equal(record.kind, 2, '种类 = 2');
  assert.equal(record.timestamp, 0, '写表时时间戳清 0');
  assert.equal(record.ownerKey, 1, '第 1 个操作数 = 命令键，**另记进记录**（命令档用它找抹除矩形）');
  assert.equal(record.textureId, 2, '第 2 个 = 命令自己的纹理编号');
  assert.deepEqual(record.segmentA, [3, 4], '第 3 / 4 个 = 区间 A（起点、个数）');
  assert.deepEqual(record.segmentB, [5, 6], '第 5 / 6 个 = 区间 B —— ⛔ 两段不许合并');
  assert.equal(record.mode, 7, '第 7 个 = 类型 / 模式');
  assert.equal(record.split, 8, '第 8 个 = 分割幅');
  assert.deepEqual([record.delay, record.duration], [9, 10], '第 9 / 10 个 = 延迟 / 持续时间');
  assert.notDeepEqual(record.segmentA, record.segmentB, '⛔ 两段区间是两条独立的格');
  assert.equal(m.scene.dirty, 1, '写成功 ⇒ 置脏');
  assert.equal(m.scene.animating, 0, '⛔ 种类 2 **不**置「有动画」（与种类 1 不同）');
});

test('★ 种类 2：类型 ∉ [0, 11]（含负数）⇒ 拒绝且**旧记录一格不动**；表空时也不建记录', async () => {
  const m = machineFor([command24f(9, 3, 2, { delay: 11, duration: 12 })]);
  await m.run();
  const before = m.scene.commands.get(9);
  assert.ok(before);
  m.scene.errorBuffer = '';
  m.scene.dirty = 0;

  // 同一个键、非法类型（−1 与 12 走**同一个**错误支）
  for (const [mode, why] of [[-1, '负数不许放行'], [12, '上界 11 之外']]) {
    await HANDLERS.get(0x24f)(m, ints(9, 200, 30, 40, 50, 60, mode, 5, 70, 80));
    assert.equal(m.scene.commands.get(9), before, `${why}：记录对象都不许换（⛔ 不建、⛔ 不删旧）`);
    assert.deepEqual(
      [before.mode, before.split, before.textureId, before.delay, before.duration],
      [3, 2, 0, 11, 12],
      `${why}：旧记录一格不动`,
    );
    assert.equal(m.scene.errorBuffer, 'タイプが不正です', `${why}：错误通道是「タイプが不正です」`);
    assert.equal(m.scene.dirty, 0, `${why}：没写表 ⇒ 不置脏`);
  }
  assert.equal(m.scene.commands.size, 1);

  // 表本来是空的时候同样"不建记录"（不是"建了一条默认记录"）
  const empty = machineFor([command24f(9, 12, 2)]);
  await empty.run();
  assert.equal(empty.scene.commands.size, 0, '类型越界 ⇒ 不建记录');
  assert.equal(empty.scene.dirty, 0);
});

test('★ 种类 2：分割幅 = 0 ⇒ 夹成 1、类型 = −1、延迟与持续时间清 0，且**照常写表**', async () => {
  const m = machineFor([]);
  m.scene.animating = 0;
  m.scene.errorBuffer = '';
  // 延迟 / 持续时间给**非 0** 的输入：清 0 必须写在记录里（不是"默认就是 0"）
  await HANDLERS.get(0x24f)(m, ints(9, 2, 3, 4, 5, 6, 7, 0, 11, 12));
  const record = m.scene.commands.get(9);
  assert.ok(record, '⚠ 这一支是**照常写**（与类型越界不同）');
  assert.equal(record.mode, -1, '类型被记成 −1（⇒ 渲染侧会置「强制完成」；本层未建模，见记录注释）');
  assert.equal(record.split, 1, '分割幅被夹成 1');
  assert.deepEqual([record.delay, record.duration], [0, 0], '延迟与持续时间**清 0**');
  assert.equal(record.ownerKey, 9);
  assert.equal(record.textureId, 2, '其它格仍按操作数写');
  assert.match(m.scene.errorBuffer, /分割幅/, '错误通道 =「分割幅が０以下です」');
  assert.doesNotMatch(m.scene.errorBuffer, /タイプが不正/, '这一支**跳过**类型校验');
  assert.equal(m.scene.dirty, 1);
  assert.equal(m.scene.animating, 0, '且不置「有动画」');
});

test('★ 种类 2：纹理槽空 ⇒ 建纹理请求发在写表**之前**；成功也不置「有动画」', async () => {
  const host = new HeadlessRenderHost();
  const m = machineFor([], { renderHost: host });
  const create = host.createTexture.bind(host);
  let recordDuringRequest;
  host.createTexture = async (request) => {
    recordDuringRequest = m.scene.commands.get(5);
    return create(request);
  };
  await HANDLERS.get(0x24f)(m, ints(5, 77, 1, 2, 3, 4, 6, 2, 30, 40));
  assert.deepEqual(
    host.requests.map((request) => [request.kind, request.request?.textureId]),
    [['create-texture', 77]],
    '槽空 ⇒ 按第 2 个操作数发一次建纹理请求',
  );
  assert.equal(recordDuringRequest, undefined, '请求发出时记录还没落表 ⇒ 请求在删旧 / 写表之前');
  assert.ok(m.scene.commands.get(5), '请求之后照常写表');
  assert.equal(m.scene.animating, 0, '种类 2 不置「有动画」');

  // 类型越界：一条纹理请求都不发（请求在两条校验**之后**）
  const bad = machineFor([], { renderHost: new HeadlessRenderHost() });
  await HANDLERS.get(0x24f)(bad, ints(5, 77, 1, 2, 3, 4, 12, 2, 30, 40));
  assert.deepEqual(bad.renderHost.requests, [], '⛔ 非法类型不建纹理（引擎也不会）');
  assert.equal(bad.scene.commands.size, 0);
});

test('★ 只有种类 1 置「有动画」；种类 0 / 2 / 3 不置', async () => {
  // ★ 直接用 handler：`Machine.run` 的下一轮 `stepScene()` 会把「有动画」清 0（窗模型），
  //   跑完整段脚本会观测不到刚写的值。
  const kind1 = machineFor([]);
  kind1.scene.textureObjects.set(10, texture());
  await HANDLERS.get(0x24d)(kind1, ints(5, 10, 11, 12, 13, 14, 15, 16, 1, 17, 18, 19));
  assert.equal(kind1.scene.animating, 1, '种类 1 ⇒ 有动画 = 1');
  assert.equal(kind1.scene.dirty, 1, '「脏」与「有动画」是两个格，两个都要置');

  for (const [opcode, ops, what] of [
    [0x223, ints(5, 7, 0, 1, 1, 1, 100, 200), '0x223（种类 0）'],
    [0x24f, ints(5, 0, 1, 2, 3, 4, 3, 2, 0, 0), '0x24f（种类 2）'],
    [0x250, ints(5, 1, 2, 3, 4, 5, 6, 7, 8, 9), '0x250（种类 3 变体 0）'],
    [0x251, ints(5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11), '0x251（种类 3 变体 1）'],
  ]) {
    const m = machineFor([]);
    m.scene.textureObjects.set(1, texture());
    m.scene.textureObjects.set(7, texture());
    await HANDLERS.get(opcode)(m, ops);
    assert.equal(m.scene.animating, 0, `${what} 不置「有动画」`);
    assert.equal(m.scene.dirty, 1, `${what} 仍然置脏`);
  }
});

/* ──────────────────────────── 命令档的诊断（S0/S1：只规划、不执行） ──────────────────────────── */

test('★ 命令表非空 ⇒ 诊断出现一次：先「已规划」再「仍未执行 / 未画」', async () => {
  const m = machineFor([command250(10, 1)]);
  await m.run();
  const notes = new SceneOrderNotes();
  const scene = sceneOf(m);
  assert.deepEqual(notes.next({ ...scene, commands: [] }), [], '空命令表 ⇒ 命令档无动作，没有诊断');
  const first = notes.next(scene);
  assert.equal(first.length, 2, '两条诊断：planned + render-unmodeled');
  assert.ok(first[0].startsWith(SCENE_COMMAND_PASS_PLANNED), '第 1 条要报"命令数 / 阶段数 / 覆盖键数"');
  assert.match(first[0], /命令 1 条 \/ 阶段 2 段 \/ 覆盖键 \d+ 个/, '摘要必须带三个数');
  assert.ok(first[1].startsWith(SCENE_COMMAND_PASS_RENDER_UNMODELED), '第 2 条显式记账"仍未真的画"');
  assert.deepEqual(notes.next(sceneOf(m)), [], '同一条诊断只报一次（不是每帧一条）');
});

/* ──────────────────────────── 负键口径（按表分） ──────────────────────────── */

const item = (key) => ({ key });
const mesh = (key) => ({ key });
const shape = (scene) => orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`);

test('★ 只放一条负键绘制项（三张辅助表都空）⇒ 它**仍被发出**', () => {
  assert.deepEqual(shape({ drawItems: [item(-7)], meshes: [] }), ['item#-7'],
    '绘制项是最后候选，键不做符号检查（spec 的纠错点）');
});

test('★ 负键网格仍然不发出（网格按 0 哨兵处理）', () => {
  assert.deepEqual(shape({ drawItems: [], meshes: [mesh(-7)] }), [],
    '⛔ 不许把绘制项的"负键照发"套到网格上');
});

test('★ 负键绘制项与正键条目混排时不改平键序：负键最前、同键网格先于绘制项', () => {
  assert.deepEqual(
    shape({ drawItems: [item(3), item(-1), item(3)], meshes: [mesh(3), mesh(-2)] }),
    ['item#-1', 'mesh#3', 'item#3', 'item#3'],
  );
});
