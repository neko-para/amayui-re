/** @env pure @kind contract @why 绘制项的消费档写错了（空槽画成占位块、无目标不上报、越界被当"编号非法"、矩形不裁剪或不补位置、位置缩放取整/场景偏移算错、自引用被跳过、位 2 多画一次、源矩形窗口三档/到期写回算错、场景自身变换按条目判错、0x229 不先重置、显示适配矩阵门写反（目标 38 / 纹理目标被缩两次）或固定分辨率模式没短路缩放），画面会错而执行器照跑 */
/**
 * tools/test/emulator-new-scene-draw-plan.test.mjs —— 绘制项消费者的**纯规划器**守卫
 * （`CORE-OPS-TEXTURE-DRAW-CONSUMER`；实现 = `scene-draw-plan.ts`）
 *
 * 判据来自 `data/spec/chapters/60-ops-texture.md` 的第 1–8 步与 5.1 的四步位置加工：
 *   有效位门 / 本轮已消费门 / 纹理解析（空槽 ⇒ 上报 + 跳过）/ 渲染目标（没有 ⇒ 上报 + 跳过）/
 *   4.1 源矩形窗口（三档；只有到期帧写回）/ 裁剪到纹理逻辑矩形（夹空 ⇒ **静默**）/
 *   位置四步（裁剪补偿 → `[20,30)` 缩放 + 半上取整 → 场景偏移两档 → 相加）/
 *   6 附加组派生（只改输入，**不**多发一次请求）/ 6.1 场景自身变换（按键区间乘入）/ 请求；
 *   以及写侧 `0x229`（先重置再写区间 / 平移）与 `0x239`（过「已存在」门才排源矩形窗口）。
 *
 * ★ **不依赖 Pixi / Electron / 游戏安装**：规划器是纯函数，槽表 / 场景对象都是手写对象。
 * ★ 守卫抓的是"实现自造的规则"：空槽画占位块、自引用跳过、把越界编号当"编号非法"、
 *   夹空也上报 —— 这四条都不会让程序抛异常，只有对着 plan 断言才会红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  DEFAULT_STEP_COLOR,
  drawFailedReport,
  planDraw,
} from '../../apps/emulator-new/src/platform/electron/renderer/scene-draw-plan.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';

/* ─────────────────────────────── 假快照 ─────────────────────────────── */

/** 纹理槽 7：64×32 的对象（**逻辑**尺寸，不是表面尺寸） */
const SLOT_7 = { width: 64, height: 32 };

/** 一张只有槽 7 的槽表（`null` = 空槽） */
const slot7Only = (textureId) => (textureId === 7 ? SLOT_7 : null);

/** 一条绘制项（`0x1fb` 写出来的那几格）；默认：整槽大小、位置 (0,0,0)、不透明白 */
const item = (over = {}) => ({
  key: 1,
  flags: 1,
  textureId: 7,
  left: 0,
  top: 0,
  right: 64,
  bottom: 32,
  color: 0xffffffff,
  aux: [0, 0, 0],
  start: 0,
  transform: null,
  group: null,
  sourceWindow: null,
  ...over,
});

/** 没有显示适配上下文时的那一档（`view: null`） */
const plan = (over = {}, extra = {}) =>
  planDraw({
    item: item(over),
    resolveSlot: slot7Only,
    target: { id: -1, present: true },
    view: null,
    now: 0,
    ...extra,
  });

/* ─────────────────────────────── 跳过档 ─────────────────────────────── */

test('★ 空纹理槽 ⇒ 上报（带纹理编号）并跳过：不产出绘制实参（于是调用方没有东西可建，⛔ 不建占位块）', () => {
  const result = plan({}, { resolveSlot: () => null });
  assert.equal(result.skip, 'missing-texture', '空槽必须走"上报 + 跳过"这一档');
  assert.equal(result.report?.category, 'draw-source-missing');
  assert.equal(result.report?.textureId, 7, '上报必须带纹理编号（否则日志定位不到坏条目）');
  assert.match(result.report?.text ?? '', /7/, '说明文本里也要有编号');
  assert.equal(result.draw, null, '⛔ 空槽不许画占位块 / 不许有任何绘制实参');
});

test('★ 没有渲染目标 ⇒ 上报并跳过（与"条目无效"是两条不同的跳过）', () => {
  const result = plan({}, { target: { id: -1, present: false } });
  assert.equal(result.skip, 'no-target');
  assert.equal(result.report?.category, 'draw-target-missing');
  assert.equal(result.report?.textureId, 7);
  assert.equal(result.draw, null);
});

test('★ 源矩形夹到纹理逻辑矩形后为空 ⇒ **静默**跳过：skip 有值、report 为 null', () => {
  // 64×32 的槽，矩形整块落在 (100..200, 0..32) ⇒ 左 ≥ 右
  const result = plan({ left: 100, top: 0, right: 200, bottom: 32 });
  assert.equal(result.skip, 'empty-clip');
  assert.equal(result.report, null, '夹空**不**上报（spec：静默跳过）');
  assert.equal(result.draw, null, '夹空也不建占位物');
});

test('★ 有效位为 0 ⇒ 静默不画、不报错；第 16 位（本轮已消费）⇒ 静默不画', () => {
  const inactive = plan({ flags: 0 });
  assert.equal(inactive.skip, 'no-valid');
  assert.equal(inactive.report, null);
  assert.equal(inactive.draw, null);

  const consumed = plan({ flags: 1 | 0x10000 });
  assert.equal(consumed.skip, 'consumed');
  assert.equal(consumed.report, null);
  assert.equal(consumed.draw, null);
});

/* ─────────────────────────────── 位置四步 ─────────────────────────────── */

test('★ 位置档位：纹理编号 20..29、位置 1.4、场景缩放 2 ⇒ x = 3（乘缩放后**半上取整**）', () => {
  // 2 × 1.4 = 2.8 ⇒ +0.5 = 3.3 ⇒ floor = 3（⛔ 不是四舍五入的 3、也不是"不缩放"的 1.4）
  const view = { noCamera: false, scaleX: 2, scaleY: 2, offsetX: 0, offsetY: 0, displayMode: 0, fixedResolution: false, screenForm: 0, viewMatrix: null };
  for (const textureId of [20, 25, 29]) {
    const result = plan({ textureId, left: 0, right: 16, top: 0, bottom: 16, aux: [1.4, 1.4, 0] }, {
      resolveSlot: () => ({ width: 16, height: 16 }),
      target: { id: 5, present: true },
      view,
    });
    assert.equal(result.skip, null);
    assert.deepEqual(result.draw?.position, [3, 3, 0], `编号 ${textureId} 必须落在"缩放 + 半上取整"档`);
  }
});

test('★ 位置档位：其它纹理编号（例 5）、缩放 2 ⇒ x 不被缩放、也不被取整（仍是 1.4）', () => {
  const view = { noCamera: false, scaleX: 2, scaleY: 2, offsetX: 0, offsetY: 0, displayMode: 0, fixedResolution: false, screenForm: 0, viewMatrix: null };
  const result = plan({ textureId: 5, left: 0, right: 16, top: 0, bottom: 16, aux: [1.4, 1.4, 0] }, {
    resolveSlot: () => ({ width: 16, height: 16 }),
    target: { id: 5, present: true },
    view,
  });
  assert.equal(result.skip, null);
  assert.equal(result.draw?.position[0], 1.4, '不在 [20,30) 的编号不缩放、不取整');
  assert.equal(result.draw?.position[1], 1.4);
});

test('★ 裁剪补偿：左 / 上被裁多少，位置就加回多少；右 / 下不补；第三格不动', () => {
  // 矩形 (-5,-3)..(40,20) 夹进 0..64 / 0..32 ⇒ 左 / 上各被裁 5 / 3
  const result = plan({ left: -5, top: -3, right: 40, bottom: 20, aux: [10, 20, 7] });
  assert.equal(result.skip, null);
  assert.deepEqual(result.draw?.rect, { left: 0, top: 0, right: 40, bottom: 20 });
  assert.deepEqual(result.draw?.position, [15, 23, 7], 'x += 5、y += 3，第三格不参与补偿');
});

test('★ 右 / 下越界只夹矩形、不补位置（裁剪补偿只补左 / 上）', () => {
  const result = plan({ left: 0, top: 0, right: 100, bottom: 60, aux: [5, 6, 0] });
  assert.deepEqual(result.draw?.rect, { left: 0, top: 0, right: 64, bottom: 32 });
  assert.deepEqual(result.draw?.position, [5, 6, 0], '右 / 下被裁不补位置');
});

test('★ 场景偏移两档：后缓冲 + 显示模式 1 ⇒ 偏移 (0,0)；[20,30) + 后缓冲 ⇒ 整数原样；其它 ⇒ 整数 ÷ 缩放', () => {
  const base = { noCamera: false, scaleX: 2, scaleY: 4, offsetX: 10, offsetY: 8, displayMode: 1, fixedResolution: false, screenForm: 0, viewMatrix: null };
  const rect = { left: 0, top: 0, right: 8, bottom: 8 };
  const slot = () => ({ width: 8, height: 8 });

  // 后缓冲 + 显示模式 1 ⇒ 偏移归零（两个编号都一样）
  const zero = plan({ ...rect, textureId: 5, aux: [0, 0, 0] }, { resolveSlot: slot, target: { id: -1, present: true }, view: base });
  assert.deepEqual(zero.draw?.position, [0, 0, 0], '显示模式 1 + 后缓冲 ⇒ 偏移 (0,0)');

  // 后缓冲 + 显示模式 0：纹理编号在 [20,30) ⇒ 整数原样；否则 ⇒ 整数 ÷ 缩放
  const plain = { ...base, displayMode: 0 };
  const backBand = plan({ ...rect, textureId: 21, aux: [1, 1, 0] }, { resolveSlot: slot, target: { id: -1, present: true }, view: plain });
  // 第 2 步：x = floor(1 × 2 + 0.5) = 2、y = floor(1 × 4 + 0.5) = 4；第 3 步：整数原样 +10 / +8
  assert.deepEqual(backBand.draw?.position, [12, 12, 0], '[20,30) + 后缓冲 ⇒ 偏移 = 整数原样');

  const backOther = plan({ ...rect, textureId: 5, aux: [1, 1, 0] }, { resolveSlot: slot, target: { id: -1, present: true }, view: plain });
  assert.deepEqual(backOther.draw?.position, [1 + 10 / 2, 1 + 8 / 4, 0], '其它编号 ⇒ 偏移 = 整数 ÷ 场景缩放');

  // 其它目标（≥0 且 ≠38）：[20,30) ⇒ 偏移 0；否则 ⇒ 整数 ÷ 缩放
  const texBand = plan({ ...rect, textureId: 21, aux: [1, 1, 0] }, { resolveSlot: slot, target: { id: 5, present: true }, view: plain });
  assert.deepEqual(texBand.draw?.position, [2, 4, 0], '其它目标 + [20,30) ⇒ 偏移 (0,0)（第 2 步的取整仍做）');

  // 保留编号 38：与后缓冲同档
  const reserved = plan({ ...rect, textureId: 21, aux: [1, 1, 0] }, { resolveSlot: slot, target: { id: 38, present: true }, view: plain });
  assert.deepEqual(reserved.draw?.position, [12, 12, 0], '保留编号 38 与后缓冲同档');
});

test('★ 无相机 / 无缩放：缩放按 1、偏移按 0，但 [20,30) 的**取整照做**', () => {
  const view = { noCamera: true, scaleX: 9, scaleY: 9, offsetX: 5, offsetY: 5, displayMode: 0, fixedResolution: false, screenForm: 0, viewMatrix: null };
  const result = plan({ textureId: 21, left: 0, right: 8, top: 0, bottom: 8, aux: [1.4, 1.4, 0] }, {
    resolveSlot: () => ({ width: 8, height: 8 }),
    target: { id: 5, present: true },
    view,
  });
  assert.deepEqual(result.draw?.position, [1, 1, 0], '缩放 1 时 1.4 + 0.5 ⇒ floor = 1；偏移按 0');
});

/* ─────────────────────────────── 自引用 / 越界 ─────────────────────────────── */

test('★ 自引用（源纹理编号 == 渲染目标编号）⇒ 仍然发绘制请求，**不**跳过', () => {
  const normal = plan({}, { target: { id: -1, present: true } });
  const self = plan({}, { target: { id: 7, present: true } });
  assert.equal(self.skip, null, '引擎不比较源与目标 ⇒ 自引用照发请求');
  assert.deepEqual(self.draw, normal.draw, '目标编号不得改变绘制实参（只影响偏移档 / 变换档）');
});

test('★ 越界纹理编号 1000 ⇒ 不出现"编号非法"：读到空槽就上报 + 跳过，读到相邻格（非 0）就照画', () => {
  const outOfRange = plan({ textureId: 1000 }, { resolveSlot: () => null });
  assert.equal(outOfRange.skip, 'missing-texture', '消费路径没有范围检查 ⇒ 越界读到 0 / 空槽就走空槽那一档');
  assert.equal(outOfRange.report?.category, 'draw-source-missing', '⛔ 不许出现"编号非法"这一类');
  assert.equal(outOfRange.report?.textureId, 1000);

  // 越界编号读到渲染对象的相邻状态格（非 0）⇒ 被当纹理对象用（spec 不规定后果）
  const adjacent = plan({ textureId: 1000, left: 0, right: 10, top: 0, bottom: 10 }, {
    resolveSlot: () => ({ width: 10, height: 10 }),
  });
  assert.equal(adjacent.skip, null, '越界编号**不被**拒绝、也不被夹取');
  assert.equal(adjacent.draw?.textureId, 1000, '纹理编号原样交给绘制入口');
});

/* ─────────────────────────────── 附加组（位 2） ─────────────────────────────── */

test('★ 位 2 = 1：只发**一次**请求，且颜色 / 源矩形 / 变换至少一项被改（不是第二次绘制）', () => {
  const group = {
    colorStart: 100,
    colorPeriod: 200,
    colorEnd: 0x00000000, // 透明黑：与不透明白逐字节混合 ⇒ 颜色必然变
    rectStart: 0,
    rectPeriod: 1000,
    stepX: 4,
    stepY: 2,
    lastStepIndex: 0,
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 3, 0, 1],
  };
  // now = 200 ⇒ elapsed 100 / period 200 ⇒ 三角波权重 1（峰）⇒ 颜色 = 终色
  const withGroup = plan({ flags: 1 | 0x4, left: 10, top: 10, right: 40, bottom: 20, group }, { now: 200 });
  const withoutGroup = plan({ left: 10, top: 10, right: 40, bottom: 20 });

  assert.equal(withGroup.skip, null);
  assert.ok(withGroup.draw, '位 2 仍然**只**产出一次绘制实参');
  assert.equal(withGroup.draw.rect.left, 14, '源矩形按步进偏移（左右同加）');
  assert.equal(withGroup.draw.rect.right, 44);
  assert.equal(withGroup.draw.rect.top, 12, '上下同加');
  assert.equal(withGroup.draw.color, 0x00000000, '颜色朝附加组终色混合（三角波峰 ⇒ 终色）');
  assert.equal(withGroup.draw.transform.valid, true, '位 2 把「本步变换有效」强制为 1');
  assert.notDeepEqual(withGroup.draw.transform.matrix, withoutGroup.draw.transform.matrix, '附加组变换乘进本步变换');
  assert.notDeepEqual(withGroup.draw, withoutGroup.draw);
  // 跨步进：now=200、起点 0、周期 1000 ⇒ 步号 0 == lastStepIndex ⇒ 不标脏
  assert.equal(withGroup.markDirty, false);
});

test('★ 位 2 = 1 且跨步进 ⇒ 场景标脏（本条唯一的标脏来源）；不跨步 ⇒ 不标脏', () => {
  const group = {
    colorStart: 0, colorPeriod: 0, colorEnd: null,
    rectStart: 0, rectPeriod: 100, stepX: 0, stepY: 0,
    lastStepIndex: 0,
    transform: null,
  };
  const crossed = plan({ flags: 1 | 0x4, group }, { now: 250 });
  assert.equal(crossed.markDirty, true, 'now=250 / 周期 100 ⇒ 步号 2 ≠ 上次 0 ⇒ 标脏');
  const same = plan({ flags: 1 | 0x4, group }, { now: 50 });
  assert.equal(same.markDirty, false, '同一段步号不标脏');
});

test('★ 位 2 缺附加组字段 ⇒ 仍只画一次，并记缺口（⛔ 不编常数）', () => {
  const result = plan({ flags: 1 | 0x4, group: null });
  assert.equal(result.skip, null);
  assert.ok(result.draw);
  assert.ok(result.notes.includes('group-unmodeled'), '缺字段要在诊断里明说');
  assert.equal(result.draw.color, DEFAULT_STEP_COLOR, '没有附加组终色 ⇒ 本步颜色不动');
});

/* ─────────────────────────────── 其它 ─────────────────────────────── */

test('★ 无颜色（-1）⇒ 本步颜色按 spec 的默认「不透明白」交给绘制入口', () => {
  const result = plan({ color: -1 });
  assert.equal(result.draw?.color, DEFAULT_STEP_COLOR);
  assert.equal(result.draw?.color, 0xffffffff);
});

test('drawFailedReport：绘制入口失败的上报**也带纹理编号**（spec 第 7 条）', () => {
  const report = drawFailedReport(42, '纹理源不可用');
  assert.equal(report.category, 'draw-failed');
  assert.equal(report.textureId, 42);
  assert.match(report.text, /42/);
  assert.match(report.text, /纹理源不可用/);
});

/* ────────────────────── 4.1 源矩形窗口（窗口排上后的三档） ────────────────────── */

/** 一个 6 格 / 每行 3 格的窗口：步序号 5 ⇒ 第 2 列、第 1 行 */
const window6x3 = (over = {}) => ({ delay: 0, duration: 100, steps: 6, perRow: 3, switchWord: 1, ...over });

test('★ 4.1 到期帧 ⇒ 写回**末帧**矩形（步序号 = 步进总数 − 1；走不完的推进不落表）', () => {
  // 基准 10×10、起点 1000、延迟 0、持续 100 ⇒ now=1101 到期（相等算窗内）；末帧 step=5 ⇒ 第 2 列 / 第 1 行
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3() }, { now: 1101 });
  assert.equal(result.skip, null);
  assert.deepEqual(result.draw?.rect, { left: 20, top: 10, right: 30, bottom: 20 }, 'Δx = 宽 × (5 mod 3) = 20、Δy = 高 × floor(5/3) = 10');
  assert.deepEqual(result.rectWriteBack, { left: 20, top: 10, right: 30, bottom: 20 }, '到期那一档才把结果写回条目四条边');
  assert.equal(result.markDirty, true, '有窗且走到这一段 ⇒ 逐帧标脏');
  assert.equal(result.notes.includes('rect-window-divide-by-zero'), false);
});

test('★ 4.1 推进中 ⇒ 只改**本帧实参**、不写回，但仍标脏', () => {
  // now=1050、起点 1000、持续 100、6 格 ⇒ step = floor(50×6/100) = 3 ⇒ 第 0 列 / 第 1 行
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3() }, { now: 1050 });
  assert.equal(result.skip, null);
  assert.deepEqual(result.draw?.rect, { left: 0, top: 10, right: 10, bottom: 20 }, '推进中按步序号平移本帧实参');
  assert.equal(result.rectWriteBack, null, '推进中**不**写回（否则记录逐帧漂走）');
  assert.equal(result.markDirty, true, '推进中也要逐帧标脏');
});

test('★ 4.1 延迟内：开关字第 1 位 = 1 ⇒ 原样；= 0 ⇒ 塌成空（都**不**写回）', () => {
  // now=1050、起点 1000、延迟 100 ⇒ now ≤ 起点+延迟 ⇒ 延迟内
  const keep = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3({ delay: 100, switchWord: 2 }) }, { now: 1050 });
  assert.equal(keep.skip, null);
  assert.deepEqual(keep.draw?.rect, { left: 0, top: 0, right: 10, bottom: 10 }, '位 1 = 1 ⇒ 矩形原样');
  assert.equal(keep.rectWriteBack, null);

  const collapse = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3({ delay: 100, switchWord: 0 }) }, { now: 1050 });
  assert.equal(collapse.skip, 'empty-clip', '位 1 = 0 ⇒ 右←左、下←上 ⇒ 夹空（静默）');
  assert.equal(collapse.report, null);
  assert.equal(collapse.rectWriteBack, null, '延迟内不写回');
  assert.equal(collapse.markDirty, true);
});

test('★ 4.1 到期 + 开关字第 0 位 = 0 ⇒ 塌空，并**仍然**把结果（空矩形）写回', () => {
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3({ switchWord: 0 }) }, { now: 1101 });
  assert.equal(result.skip, 'empty-clip');
  assert.deepEqual(result.rectWriteBack, { left: 0, top: 0, right: 0, bottom: 0 }, 'spec：到期那一档把结果写回（空矩形也是结果）');
});

test('★ 4.1 持续时间 ≤ 0 ⇒ 整段不跑：矩形原样、不写回、**不**标脏', () => {
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3({ duration: 0 }) }, { now: 1100 });
  assert.equal(result.skip, null);
  assert.deepEqual(result.draw?.rect, { left: 0, top: 0, right: 10, bottom: 10 });
  assert.equal(result.rectWriteBack, null);
  assert.equal(result.markDirty, false, '没跑过 4.1 ⇒ 不标脏');
});

test('★ 4.1 每行步数 = 0 且持续时间 > 0 ⇒ 诊断 + 跳过该档（不夹取、不出 NaN）', () => {
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3({ perRow: 0 }) }, { now: 1050 });
  assert.ok(result.notes.includes('rect-window-divide-by-zero'), '整数除零后果 spec 不规定 ⇒ 记缺口');
  assert.deepEqual(result.draw?.rect, { left: 0, top: 0, right: 10, bottom: 10 }, '该档跳过 ⇒ 矩形保持原样（⛔ 不按 1 除）');
  assert.equal(result.rectWriteBack, null);
  assert.equal(result.markDirty, true, '窗口本身仍然活着');
});

test('★ 4.1 无「有窗」位 ⇒ 4.1 整段不跑（窗口字段即使有值也不生效）', () => {
  const result = plan({ flags: 1, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3() }, { now: 1100 });
  assert.deepEqual(result.draw?.rect, { left: 0, top: 0, right: 10, bottom: 10 });
  assert.equal(result.rectWriteBack, null);
  assert.equal(result.markDirty, false);
});

test('★ 4.1 场景「强制完成」档 = 1 ⇒ 不看时钟也算到期', () => {
  const result = plan({ flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3() }, { now: 1000, forcedComplete: 1 });
  assert.deepEqual(result.rectWriteBack, { left: 20, top: 10, right: 30, bottom: 20 }, '强制完成 ⇒ 到期档 ⇒ 末帧写回');
});

/* ────────────────────── 6.1 场景自身变换（作用键区间） ────────────────────── */

/** 一个只含平移的额外变换（列主序：(5, 7)） */
const SELF_MATRIX = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 7, 0, 1];

test('★ 6.1 条目键落在区间且计数非 0 ⇒ 乘入场景自身变换并把「本步变换有效」置 1', () => {
  const result = plan({ key: 10 }, { selfTransform: { keyStart: 10, keyCount: 3, matrix: SELF_MATRIX } });
  assert.equal(result.draw?.transform.valid, true, 'spec：乘入并把有效置 1');
  assert.equal(result.draw?.transform.matrix[12], 5, '额外变换真的乘进了本步变换');

  const unmodeled = plan({ key: 10 }, { selfTransform: { keyStart: 10, keyCount: 3, matrix: null } });
  assert.equal(unmodeled.draw?.transform.valid, true, '矩阵未建模也照样置有效（用单位阵）');
  assert.ok(unmodeled.notes.includes('self-transform-matrix-unmodeled'), '未建模的派生要记缺口');
});

test('★ 6.1 条目键不在区间 / 计数为 0 ⇒ **不**乘（valid 保持 0）', () => {
  const outside = plan({ key: 20 }, { selfTransform: { keyStart: 10, keyCount: 3, matrix: SELF_MATRIX } });
  assert.equal(outside.draw?.transform.valid, false, '键不在 [10, 13) ⇒ 不乘');
  assert.equal(outside.draw?.transform.matrix[12], 0);

  // 区间是左闭右开：末键 = 起点 + 计数（13）**不**在区间内
  const boundary = plan({ key: 13 }, { selfTransform: { keyStart: 10, keyCount: 3, matrix: SELF_MATRIX } });
  assert.equal(boundary.draw?.transform.valid, false, '键 == 起点 + 计数 ⇒ 不乘（右开）');

  const zeroCount = plan({ key: 10 }, { selfTransform: { keyStart: 10, keyCount: 0, matrix: SELF_MATRIX } });
  assert.equal(zeroCount.draw?.transform.valid, false, '计数为 0 ⇒ 不派生额外变换');
});

test('★ 6.1 命令档传「跳过」⇒ 即使键在区间也**不**乘（接口参数；命令档执行仍未建模）', () => {
  const skipped = plan({ key: 10 }, { selfTransform: { keyStart: 10, keyCount: 3, matrix: SELF_MATRIX }, skipExtraTransform: true });
  assert.equal(skipped.draw?.transform.valid, false);
  assert.equal(skipped.draw?.transform.matrix[12], 0);
});

/* ────────────────────── 0x229 / 0x239（核心写侧） ────────────────────── */

const KEY = 0x1234abcd;
const int = (value) => ({ type: 0, raw: value >>> 0 });
const float = (value) => {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, value, true);
  return { type: 1, raw: view.getUint32(0, true) };
};
const machine = () => new Machine({ body: new Uint8Array(0), table: new Map(), key: KEY });

test('★ 0x229：连写两条不同区间 ⇒ 第一条**不残留**；三元平移写入；置脏', async () => {
  const handler = HANDLERS.get(0x229);
  assert.ok(handler, '0x229 必须在指令表里（不是缺省处理器）');
  const m = machine();

  await handler(m, [int(10), int(3), float(1.5), float(2.5), float(3.5)]);
  assert.equal(m.scene.selfRecord.keyStart, 10);
  assert.equal(m.scene.selfRecord.keyCount, 3);
  assert.deepEqual(m.scene.selfRecord.translate, { x: 1.5, y: 2.5, z: 3.5 }, '三个浮点原值写进三元平移');
  assert.equal(m.scene.dirty, 1, '写成功 ⇒ 场景标脏');

  m.scene.dirty = 0;
  await handler(m, [int(20), int(5), float(7), float(8), float(9)]);
  assert.equal(m.scene.selfRecord.keyStart, 20, '先重置 ⇒ 第一条的区间不得残留');
  assert.equal(m.scene.selfRecord.keyCount, 5);
  assert.deepEqual(m.scene.selfRecord.translate, { x: 7, y: 8, z: 9 });
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x229 先重置再读：三元平移读到坏操作数而抛 ⇒ 上一次的记录已经不在了（键区间已按本次写入）', async () => {
  const handler = HANDLERS.get(0x229);
  const m = machine();
  await handler(m, [int(10), int(3), float(1.5), float(2.5), float(3.5)]);
  // 第 3 个操作数改成"未写过的全局浮点格" ⇒ 读三元平移时抛（Machine 默认 uninitializedFloat='throw'）
  assert.throws(
    () => handler(m, [int(20), int(5), { type: 4, raw: 99 }, float(8), float(9)]),
    /uninitialized-float-slot:99/,
  );
  assert.deepEqual(m.scene.selfRecord.translate, { x: 0, y: 0, z: 0 }, '重置发生在读取之前 ⇒ 上一次的平移不得残留');
  assert.equal(m.scene.selfRecord.keyStart, 20, '键区间在平移读取之前就写（spec 次序）');
  assert.equal(m.scene.selfRecord.keyCount, 5);
});

test('★ 0x239：过「已存在」门才写源矩形窗口五格 + 清共用窗起点；门不过 ⇒ 一个字段都不写', async () => {
  const m = machine();
  await HANDLERS.get(0x1fb)(m, [int(1), int(7), int(0), int(0), int(10), int(20), int(0), int(0)]);
  const drawn = m.scene.drawItems.get(1);
  drawn.start = 500; // 假装颜色窗已经盖过章
  await HANDLERS.get(0x239)(m, [int(1), int(30), int(200), int(6), int(3), int(1)]);
  assert.deepEqual(drawn.sourceWindow, { delay: 30, duration: 200, steps: 6, perRow: 3, switchWord: 1 });
  assert.equal(drawn.flags & 2, 2, '置首字第 1 位（有窗）');
  assert.equal(drawn.start, 0, '窗起点与颜色窗共用 ⇒ 清 0（由派生盖章）');
  assert.equal(drawn.fields.get(0x48), 30, '按偏移原样存的兼容视图也照写');
  assert.equal(drawn.fields.get(0x5c), 200);
  assert.equal(drawn.fields.get(0x238), 6);
  assert.equal(drawn.fields.get(0x23c), 3);
  assert.equal(drawn.fields.get(0x234), 1);

  const gated = machine();
  await HANDLERS.get(0x239)(gated, [int(2), int(30), int(200), int(6), int(3), int(1)]);
  const fresh = gated.scene.drawItems.get(2);
  assert.ok(fresh, '门不过也建默认记录');
  assert.equal(fresh.flags & 2, 0);
  assert.equal(fresh.sourceWindow, null, '门不过 ⇒ 源矩形窗口一格都不写');
  assert.equal(fresh.fields.size, 0);
  assert.equal(gated.scene.dirty, 0, '门不过 ⇒ 也不标脏 / 不置动画');
});

test('★ 4.1 相等（now == 终点）算窗内 ⇒ 不写回、仍标脏；只有 now > 终点才到期', () => {
  const result = plan(
    { flags: 1 | 2, start: 1000, left: 0, top: 0, right: 10, bottom: 10, sourceWindow: window6x3() },
    { now: 1100 },
  );
  assert.equal(result.skip, null);
  assert.equal(result.rectWriteBack, null, '相等算窗内 ⇒ 不是到期帧，⛔ 不写回');
  assert.equal(result.markDirty, true);
});

/* ═════════════ 5.1 末段：两张显示适配矩阵的乘入门（`CORE-SCENE-DISPLAY-FIT`） ═════════════ */

/**
 * ★ 这一节是**纠错守卫**：spec 5.1 的门是
 *   视图矩阵 ⇐ `F==0 && (D<0 || D==39) && tex∉[20,30)`；
 *   逆缩放   ⇐ `F==0 && D>=0 && D!=39 && tex∈[20,30)`。
 *   旧实现写成"`D>=0 && D!=39` ⇒ 乘视图矩阵"（正好反了）⇒ 目标 38 / 纹理目标会被缩两次。
 * ★ 两张矩阵取值可辨识：视图矩阵对角线 3、逆缩放矩阵对角线 1/s（s = 2 ⇒ 0.5）。
 */
const IDENTITY4 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const VIEW_MATRIX = [3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
const scalingOf = (sx, sy) => [sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

/** 一张可辨识的场景视图（缩放 2 / 偏移 10,8 / 显示模式 0 / 占满屏态 F=0） */
const viewOf = (over = {}) => ({
  noCamera: false,
  scaleX: 2,
  scaleY: 2,
  offsetX: 10,
  offsetY: 8,
  displayMode: 0,
  fixedResolution: false,
  screenForm: 0,
  viewMatrix: VIEW_MATRIX,
  ...over,
});

/** 8×8 的槽（位置档用得到） */
const slot8 = () => ({ width: 8, height: 8 });
const smallItem = (over = {}) => ({ left: 0, top: 0, right: 8, bottom: 8, ...over });

test('★ 矩阵门：后缓冲（D=-1）+ 非 [20,30) ⇒ 乘**视图矩阵**（恰好一张）', () => {
  const result = plan({ textureId: 7 }, { target: { id: -1, present: true }, view: viewOf() });
  assert.equal(result.skip, null);
  assert.equal(result.adaptMatrixCount, 1, '每笔最多乘一张（互斥门）');
  assert.deepEqual(result.draw.transform.matrix, VIEW_MATRIX, '视图矩阵被乘进本步变换');
});

test('★ 矩阵门：后缓冲 + [20,30) ⇒ **两张都不乘**（逆缩放只给"非后缓冲、非 39"的目标）', () => {
  const result = plan(smallItem({ textureId: 25 }), {
    resolveSlot: slot8,
    target: { id: -1, present: true },
    view: viewOf(),
  });
  assert.equal(result.adaptMatrixCount, 0, 'D=-1 落在视图那侧、但 [20,30) 把它挡掉 ⇒ 两张都不乘');
  assert.deepEqual(result.draw.transform.matrix, IDENTITY4);
});

test('★ 矩阵门：目标 38（画布）+ 非 [20,30) ⇒ **两张都不乘**（⛔ 旧实现在这里乘了视图矩阵）', () => {
  const result = plan({ textureId: 7 }, { target: { id: 38, present: true }, view: viewOf() });
  assert.equal(result.adaptMatrixCount, 0, '目标 38 不乘视图矩阵');
  assert.deepEqual(result.draw.transform.matrix, IDENTITY4);
  assert.equal(result.draw.transform.valid, false);
});

test('★ 矩阵门：目标 38（画布）+ [20,30) ⇒ 乘**逆缩放**矩阵', () => {
  const result = plan(smallItem({ textureId: 20 }), {
    resolveSlot: slot8,
    target: { id: 38, present: true },
    view: viewOf(),
  });
  assert.equal(result.adaptMatrixCount, 1);
  assert.deepEqual(result.draw.transform.matrix, scalingOf(0.5, 0.5));
});

test('★ 矩阵门：其它纹理目标（D=7）⇒ 非 [20,30) 不乘、[20,30) 乘逆缩放（"其它目标不乘视图矩阵"）', () => {
  const plain = plan({ textureId: 7 }, { target: { id: 7, present: true }, view: viewOf() });
  assert.equal(plain.skip, null);
  assert.equal(plain.adaptMatrixCount, 0, '纹理目标不乘视图矩阵');
  assert.deepEqual(plain.draw.transform.matrix, IDENTITY4);

  const band = plan(smallItem({ textureId: 29 }), { resolveSlot: slot8, target: { id: 7, present: true }, view: viewOf() });
  assert.equal(band.adaptMatrixCount, 1);
  assert.deepEqual(band.draw.transform.matrix, scalingOf(0.5, 0.5), '逆缩放 = 1/2（把位置上乘过的 s 抵回去）');
});

test('★ 矩阵门：保留目标 39 与后缓冲**同档**（非 [20,30) ⇒ 视图矩阵；[20,30) ⇒ 两张都不乘）', () => {
  const outside = plan({ textureId: 7 }, { target: { id: 39, present: true }, view: viewOf() });
  assert.equal(outside.adaptMatrixCount, 1);
  assert.deepEqual(outside.draw.transform.matrix, VIEW_MATRIX, 'D == 39 ⇒ 乘视图矩阵');

  const inside = plan(smallItem({ textureId: 21 }), { resolveSlot: slot8, target: { id: 39, present: true }, view: viewOf() });
  assert.equal(inside.adaptMatrixCount, 0, 'D == 39 不乘逆缩放矩阵（两张门互补、且 39 落在视图那侧）');
  assert.deepEqual(inside.draw.transform.matrix, IDENTITY4);
});

test('★ 矩阵门：屏幕形态 F != 0（窗口态）⇒ 两张都不乘、偏移恒 0（位置不被折回）', () => {
  const windowed = viewOf({ screenForm: 1 });
  for (const [target, textureId] of [[-1, 7], [-1, 25], [38, 7], [38, 25], [7, 7], [7, 25], [39, 7]]) {
    const result = plan(smallItem({ textureId }), { resolveSlot: slot8, target: { id: target, present: true }, view: windowed });
    assert.equal(result.skip, null);
    assert.equal(result.adaptMatrixCount, 0, `F=1 时 D=${target} / tex=${textureId} 不许乘任何适配矩阵`);
    assert.deepEqual(result.draw.transform.matrix, IDENTITY4);
  }
  // 偏移：窗口态恒 0（spec「折回画布坐标」），即使快照里带着非 0 原点
  const offset = plan(smallItem({ textureId: 5, aux: [0, 0, 0] }), {
    resolveSlot: slot8,
    target: { id: 7, present: true },
    view: windowed,
  });
  assert.deepEqual(offset.draw.position, [0, 0, 0], '窗口态偏移恒 0（⛔ 不按原点折回）');
});

test('★ 固定分辨率模式（display:ScreenMode != 0）：取值器把缩放短路成 1、原点成 0，**矩阵仍乘一次**', () => {
  const fixed = viewOf({ fixedResolution: true });
  // 纹理编号 21 在 [20,30) 档：正常会被乘 scaleX=2 并加偏移 (10,8)；固定分辨率 ⇒ 只取整、不加偏移
  const result = plan(smallItem({ textureId: 21, aux: [1.4, 1.4, 0] }), {
    resolveSlot: slot8,
    target: { id: 5, present: true },
    view: fixed,
  });
  assert.deepEqual(result.draw.position, [1, 1, 0], '缩放按 1（floor(1.4 + 0.5) = 1）、偏移按 0');
  // 逆缩放矩阵退化 = 单位阵，但**仍然乘一次**（数值无影响 ⇒ 只能靠次数观测）
  assert.equal(result.adaptMatrixCount, 1, '固定分辨率模式下逆缩放矩阵仍然乘一次');
  assert.deepEqual(result.draw.transform.matrix, IDENTITY4);

  // 非 [20,30) 的视图矩阵照乘（固定分辨率不改变门）
  const view = plan({ textureId: 7 }, { target: { id: -1, present: true }, view: fixed });
  assert.equal(view.adaptMatrixCount, 1);
  assert.deepEqual(view.draw.transform.matrix, VIEW_MATRIX);
});

test('★ 没有视图矩阵取值（view.viewMatrix = null）⇒ 不乘并记缺口（⛔ 不拿单位阵假装乘过）', () => {
  const result = plan({ textureId: 7 }, { target: { id: -1, present: true }, view: viewOf({ viewMatrix: null }) });
  assert.equal(result.adaptMatrixCount, 0);
  assert.deepEqual(result.draw.transform.matrix, IDENTITY4);
  assert.ok(result.notes.includes('transform-view-missing'), '缺取值要能在诊断里看见');
});
