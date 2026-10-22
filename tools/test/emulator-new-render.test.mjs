/** @env pure @kind contract @why 渲染请求边界的语义写错了（目标切换的次序 / 失败也写目标 / 混合回退来源 / 离屏文字的三条入口条件与字体来源），而执行器仍然"跑得下去"、请求也照样被记下来 */
/**
 * tools/test/emulator-new-render.test.mjs —— `data/spec/chapters/60-ops-render.md` 三条的守卫
 *
 * 覆盖：
 *   - `0x20d` 渲染目标（纹理槽 / 后缓冲两支；失败路径；**失败不改目标**；取表面 → 设目标 → 归还的次序）；
 *   - `0x33f` 场景混合（读序 2 → 3 →（按需）1 →（按需）1 → 1；绘制项回退；不建默认项；模式不 clamp；
 *     **不发外部请求**、不置脏标记 / animating）；
 *   - `0x204` 离屏文字（三条入口条件；读序 3 → 2 → 4 → 1；数值 → 十进制文本；字体对象的全部状态字段；
 *     `drawText` 返回 false（LockTexture 失败）⇒ 错误通道且不重建字形暂存区）。
 *
 * ★ 判据为什么是这些：
 *   - 0x20d 的"失败不改目标"与"归还先于写目标"写错**不会报错**，只会让目标格停在一个中间值上；
 *   - 0x33f 的回退来源写错（用本条上一次写的颜色）只在颜色格被改过之后才显形；
 *   - 0x204 的三条入口条件写错（先清开关再判 / 空文本也发请求）只有 fake 宿主的调用台账能看见。
 * ★ 本文件只用 fake / headless 宿主，断言的是**请求与引擎数据**，⛔ 不断言像素（spec 不规定像素）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine, effectScope } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { HeadlessRenderHost, NO_COLOR, RENDER_TARGET_BACK_BUFFER } from '../../apps/emulator-new/src/core/render.ts';
import { countEffectsByScope } from '../../apps/emulator-new/src/core/runner.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

/** 合成脚本字节（与其它 emulator-new 守卫同一个写法） */
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

function instrWords(instrs) {
  let n = 0;
  for (const [, ops] of instrs) n += 1 + 2 * ops.length;
  return n;
}

/** 指令之后接一段**内联文本**（存储字节逐字节取反、0xFF 终止），返回 body 与文本的字偏移 */
function scriptWithInlineText(makeInstrs, text) {
  const raw = instrWords(makeInstrs(0));
  const body = script(makeInstrs(raw));
  const bytes = Buffer.alloc(text.length + 1);
  for (let i = 0; i < text.length; i += 1) bytes[i] = text.charCodeAt(i) & 0xff ^ 0xff;
  bytes[text.length] = 0xff;
  return { body: Buffer.concat([body, bytes]), raw };
}

function machineFor(instrs, opts = {}) {
  return new Machine({ body: script(instrs), table, key: KEY, ...opts });
}

/** 默认绘制项（字段集与 handlers 的 defaultItem 一致；本测试只关心当前颜色） */
function drawItem(colorFrom) {
  return {
    flags: 1, textureId: 1, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0],
    start: 0, delay: 0, duration: 0, colorFrom, colorNow: colorFrom, colorTo: -1, unknown: new Map(),
  };
}

function withTexture(m, id, extra = {}) {
  m.scene.textureObjects.set(id, { kind: 'normal', width: 16, height: 16, ...extra });
  return m;
}

function f32bits(value) {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, value, true);
  return view.getUint32(0, true);
}

const names = (host) => host.calls.map((c) => c.call);
const names2 = (host) => host.requests.map((r) => r.kind);

/**
 * fake 渲染宿主：把每次调用记进 `calls`（含调用当时的当前渲染目标，用来钉次序）。
 * ★ 它**不是** headless：每个请求的返回值都可配，用来走失败路径。
 */
class FakeRenderHost {
  constructor(options = {}) {
    this.calls = [];
    this.surfaces = new Map();
    this.backBuffer = { kind: 'fake-back-buffer' };
    this.setTargetResult = options.setTargetResult ?? true;
    this.drawTextResult = options.drawTextResult ?? true;
    this.observe = options.observe ?? null;
  }

  record(call, fields) {
    const entry = { call, ...fields };
    if (this.observe) entry.targetAtCall = this.observe();
    this.calls.push(entry);
    return entry;
  }

  async getRenderSurface(textureId) {
    this.record('getRenderSurface', { textureId });
    return this.surfaces.get(textureId) ?? null;
  }

  async setRenderTarget(surface) {
    this.record('setRenderTarget', { surface });
    return this.setTargetResult;
  }

  async getBackBuffer() {
    this.record('getBackBuffer', {});
    return this.backBuffer;
  }

  async releaseSurface(surface) {
    this.record('releaseSurface', { surface });
  }

  async drawText(request) {
    this.record('drawText', { request, ok: this.drawTextResult });
    return this.drawTextResult; // false = 平台侧加锁 / 取矩形失败（0x204 的失败通道）
  }
}

/* ──────────────────────────── 0x20d 渲染目标 ──────────────────────────── */

test('★ 0x20d 纹理槽：取表面 → 设目标 → 归还 → 才写当前目标（次序不可交换）', async () => {
  const handle = { kind: 'surface', id: 5 };
  let m;
  const host = new FakeRenderHost({ observe: () => m.render.target });
  host.surfaces.set(5, handle);
  m = withTexture(machineFor([[0x20d, [[0, 5]]]], { renderHost: host }), 5);
  await m.run();

  assert.equal(m.render.target, 5, '成功 ⇒ 当前目标 = 槽号');
  assert.deepEqual(names(host), ['getRenderSurface', 'setRenderTarget', 'releaseSurface']);
  assert.equal(host.calls[1].surface, handle);
  assert.equal(host.calls[2].surface, handle, '归还的是同一份表面引用');
  // 归还发生在写当前渲染目标**之前**：两次调用时目标格都还是旧值
  assert.equal(host.calls[1].targetAtCall, RENDER_TARGET_BACK_BUFFER, '设目标时还没写当前目标');
  assert.equal(host.calls[2].targetAtCall, RENDER_TARGET_BACK_BUFFER, '归还时还没写当前目标');
  // 本条不建纹理、不改纹理槽 / 绘制项 / 脏标记
  assert.equal(m.scene.textures.size, 0);
  assert.deepEqual([...m.scene.textureObjects.keys()], [5]);
  assert.equal(m.scene.drawItems.size, 0);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.scene.errorBuffer, '');
});

test('★ 0x20d 取不到渲染表面 ⇒ 写错误通道、当前目标不变、不调 setRenderTarget', async () => {
  const host = new FakeRenderHost();
  const m = withTexture(machineFor([[0x20d, [[0, 5]]]], { renderHost: host }), 5);
  m.render.target = 9; // 原值（不是哨兵）⇒ 才能证明"不变"
  await m.run();

  assert.deepEqual(names(host), ['getRenderSurface']);
  assert.equal(m.render.target, 9, '失败必须发生在写当前目标之前');
  assert.notEqual(m.scene.errorBuffer, '', '不得静默');
});

test('★ 0x20d 设目标失败 ⇒ 仍归还表面、当前目标不变、报错', async () => {
  const handle = { kind: 'surface', id: 5 };
  const host = new FakeRenderHost({ setTargetResult: false });
  host.surfaces.set(5, handle);
  const m = withTexture(machineFor([[0x20d, [[0, 5]]]], { renderHost: host }), 5);
  m.render.target = 3;
  await m.run();

  assert.deepEqual(names(host), ['getRenderSurface', 'setRenderTarget', 'releaseSurface'], '成功 / 失败都归还');
  assert.equal(host.calls[2].surface, handle);
  assert.equal(m.render.target, 3);
  assert.notEqual(m.scene.errorBuffer, '');
});

test('★ 0x20d 槽里没有纹理对象 ⇒ 错误通道，且一次宿主调用都不发', async () => {
  const host = new FakeRenderHost();
  const m = machineFor([[0x20d, [[0, 5]]]], { renderHost: host });
  await m.run();

  assert.deepEqual(host.calls, []);
  assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER, '初值不动（就是后缓冲哨兵）');
  assert.notEqual(m.scene.errorBuffer, '');
});

test('★ 0x20d ≥ 1000 与负数（无符号比较）⇒ 后缓冲那一支，不查纹理槽', async () => {
  for (const slot of [1000, 5000, -1]) {
    const host = new FakeRenderHost();
    const m = machineFor([[0x20d, [[0, slot]]]], { renderHost: host });
    await m.run();

    assert.deepEqual(names(host), ['getBackBuffer', 'setRenderTarget', 'releaseSurface'], `槽号 ${slot}`);
    assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER, `槽号 ${slot} ⇒ 后缓冲哨兵`);
    assert.equal(m.scene.errorBuffer, '');
  }
});

test('★ 0x20d 后缓冲取不到 ⇒ 报错、目标不变（不调 setRenderTarget）', async () => {
  const host = new FakeRenderHost();
  host.backBuffer = null;
  const m = machineFor([[0x20d, [[0, 1000]]]], { renderHost: host });
  m.render.target = 4;
  await m.run();

  assert.deepEqual(names(host), ['getBackBuffer']);
  assert.equal(m.render.target, 4);
  assert.notEqual(m.scene.errorBuffer, '');
});

test('★ 0x20d 没有渲染宿主 ⇒ 按"取不到"处理（写错误通道 + 诊断），⛔ 不假装成功', async () => {
  const m = withTexture(machineFor([[0x20d, [[0, 5]]]]), 5);
  await m.run();

  assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER);
  assert.notEqual(m.scene.errorBuffer, '');
  assert.ok(m.diagnostics.some((d) => d.kind === 'render-host-missing'));
});

/* ──────────────────────────── 0x33f 场景混合 ──────────────────────────── */

test('★ 0x33f 模式原样写入（不 clamp）、颜色 = 低 24 位 + 高 8 位透明度', async () => {
  const m = machineFor([[0x33f, [[0, -7], [0, 0x12], [0, 0x00aabbcc]]]]);
  await m.run();

  assert.equal(m.render.blendMode, -7, '负数模式也照写（⛔ 不 clamp 到枚举）');
  assert.equal(m.render.blendColor, 0x12aabbcc, '透明度占高 8 位');
});

test('★ 0x33f 透明度 > 255 ⇒ 255（不是整字夹取）', async () => {
  const m = machineFor([[0x33f, [[0, 3], [0, 0x1234], [0, 0x00112233]]]]);
  await m.run();
  assert.equal(m.render.blendColor, 0xff112233);
});

test('★ 0x33f 回退来源是**绘制项**的当前颜色（不是本条上次写的颜色）', async () => {
  const m = machineFor([[0x33f, [[0, 3], [0, -1], [0, -1]]]]);
  m.scene.drawItems.set(3, drawItem(0x7f8899aa));
  m.render.blendColor = 0x01020304; // 上一次写的颜色：⛔ 不许当归一化输入
  await m.run();

  assert.equal(m.render.blendMode, 3);
  assert.equal(m.render.blendColor, 0x7f8899aa, '颜色整体回退自绘制项，透明度也回退自它');
});

test('★ 0x33f 绘制项不存在 ⇒ 回退「无颜色」哨兵，且**不建**默认绘制项', async () => {
  const m = machineFor([[0x33f, [[0, 42], [0, -1], [0, -1]]]]);
  m.scene.drawItems.clear();
  await m.run();

  assert.equal(m.scene.drawItems.size, 0, '⛔ 本条不建默认项（与场景 / 颜色族共用口径不同）');
  const sentinel = NO_COLOR;
  const expected = (((sentinel & 0xffffff) | ((sentinel >>> 24) << 24)) >>> 0);
  assert.equal(m.render.blendColor, expected);
});

test('★ 0x33f 不发外部请求、不置脏标记 / animating', async () => {
  const host = new FakeRenderHost();
  const m = machineFor([[0x33f, [[0, 0], [0, 0x80], [0, 0x00112233]]]], { renderHost: host });
  m.scene.dirty = 0;
  m.scene.animating = 0;
  await m.run();

  assert.deepEqual(host.calls, [], '本条由渲染路径稍后读取，⛔ 不调 RenderHost');
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.scene.animating, 0);
});

test('★ 0x33f 读序 2 → 3 →（按需）1：回退不发生就不读第 1 个', async () => {
  const m = new Machine({ body: Buffer.alloc(0), table, key: KEY });
  const spy = (defs) => {
    const reads = [];
    const ops = defs.map(([type, raw], i) => new Proxy({ type, raw }, {
      get(target, key, receiver) {
        if (key === 'type') reads.push(String(i + 1));
        return Reflect.get(target, key, receiver);
      },
    }));
    return { ops, reads };
  };
  const handler = HANDLERS.get(0x33f);
  assert.ok(handler);

  // op1 = 42（模式）、op2 = 0x12（透明度）、op3 = 颜色：只有两次"按需"都不发生 ⇒ 读序 2,3,1
  let { ops, reads } = spy([[0, 42], [0, 0x12], [0, 0x00aabbcc]]);
  handler(m, ops);
  assert.deepEqual(reads, ['2', '3', '1']);
  assert.equal(m.render.blendMode, 42);

  // 只有透明度 < 0：多读一次第 1 个（回退），再读一次写模式
  m.scene.drawItems.set(42, drawItem(0x40000000));
  ({ ops, reads } = spy([[0, 42], [0, -1], [0, 0x00aabbcc]]));
  handler(m, ops);
  assert.deepEqual(reads, ['2', '3', '1', '1']);

  // 两者都 < 0 ⇒ 两次按需读 + 写模式一次
  ({ ops, reads } = spy([[0, 42], [0, -1], [0, -1]]));
  handler(m, ops);
  assert.deepEqual(reads, ['2', '3', '1', '1', '1']);
});

test('★ 0x33f 三次读第 1 个操作数各用各的值：前两次查表、最后一次写模式', async () => {
  const m = new Machine({ body: Buffer.alloc(0), table, key: KEY });
  // 第 1 个操作数三次读出的 raw 依次是 111（透明度回退）、222（颜色回退）、333（模式）
  const raw = [111, 222, 333];
  let next = 0;
  const op1 = new Proxy({ type: 0, raw: 0 }, {
    get(target, key, receiver) {
      if (key === 'type') return 0;
      if (key === 'raw') return raw[next++] ?? 0;
      return Reflect.get(target, key, receiver);
    },
  });
  m.scene.drawItems.set(111, drawItem(0x7f000000)); // 透明度回退的来源
  m.scene.drawItems.set(222, drawItem(0x00112233)); // 颜色回退的来源
  const ops = [op1, { type: 0, raw: -1 }, { type: 0, raw: -1 }];
  HANDLERS.get(0x33f)(m, ops);

  assert.equal(m.render.blendMode, 333, '模式来自**最后一次**读');
  assert.equal(m.render.blendColor, 0x7f112233, '透明度取自 111、颜色取自 222');
});

test('★ 0x33f 读序：第 2 个先于第 3 个被读（两个都抛时先抛 2 的那个）', async () => {
  const m = machineFor([[0x33f, [[0, 0], [5, 0], [8, 0]]]]);
  await assert.rejects(() => m.run(), /read-int:unsupported-operand-type:0x5/);
});

/* ──────────────────────────── 0x204 离屏文字 ──────────────────────────── */

/** 0x204 的四个操作数：1 = 纹理槽号、2 = x、3 = y、4 = 文本 */
const text204 = (textureId, x, y, textOperand) => [0x204, [[0, textureId], [0, x], [0, y], textOperand]];

test('★ 0x204 成功：drawText = 操作数（坐标 / 文本）+ 字体对象的全部状态字段', async () => {
  const host = new FakeRenderHost();
  host.surfaces.set(5, { kind: 'surface', id: 5 });
  const m = withTexture(machineFor([text204(5, 10, 20, [5, 0])], { renderHost: host }), 5);
  m.globalString.set(0, 'abc');
  m.render.fontObject.font = 'MSGothic';
  m.render.fontObject.size = 32;
  m.render.fontObject.weight = 700;
  m.render.fontObject.color = 0x7f112233;
  m.render.fontObject.shadowColor = 0x00112233;
  m.render.fontObject.drawMode = 2;
  m.render.fontObject.alignMode = 1;
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.deepEqual(names(host), ['getRenderSurface', 'drawText']);
  assert.deepEqual(host.calls[1].request, {
    textureId: 5, x: 10, y: 20, text: 'abc',
    font: 'MSGothic', size: 32, weight: 700, color: 0x7f112233,
    shadowColor: 0x00112233, drawMode: 2, alignMode: 1,
  });
  // 引擎数据：一个内部布尔开关被清 0、字形暂存区被重建；⛔ 其余纹理 / 绘制项 / 脏标记都不动
  assert.equal(m.render.fontObject.switches[0], 0);
  assert.equal(m.render.fontObject.glyphCacheRebuilds, 1);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.scene.drawItems.size, 0);
  assert.deepEqual(m.scene.textureObjects.get(5), { kind: 'normal', width: 16, height: 16 });
  assert.equal(m.scene.errorBuffer, '', '成功路径不写错误通道');
});

test('★ 0x204 请求字段全部来自字体对象状态（缺省值：size 24 / color 0xFFFFFF / shadow 0 / drawMode 1）', async () => {
  const host = new FakeRenderHost();
  host.surfaces.set(5, { kind: 'surface', id: 5 });
  const m = withTexture(machineFor([text204(5, 1, 2, [0, 7])], { renderHost: host }), 5);
  await m.run();

  const request = host.calls.find((c) => c.call === 'drawText')?.request;
  assert.ok(request);
  assert.equal(request.text, '7', '数值操作数 ⇒ 十进制文本');
  // 字体对象的核心没建 ⇒ face name 是 null（⛔ 不补 ''）；其余是已证的默认值
  assert.equal(request.font, null);
  assert.deepEqual(
    [request.size, request.weight, request.color, request.shadowColor, request.drawMode, request.alignMode],
    [24, 0, 0xffffff, 0, 1, 0],
  );
});

test('★ 0x204 入口条件 ①：槽里没有纹理对象 ⇒ 什么都不做（连宿主都不问）', async () => {
  const host = new FakeRenderHost();
  const m = machineFor([text204(5, 10, 20, [5, 0])], { renderHost: host });
  m.globalString.set(0, 'abc');
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.deepEqual(host.calls, []);
  assert.equal(m.render.fontObject.switches[0], 1, '⛔ 不许先清开关再判条件');
  assert.equal(m.render.fontObject.glyphCacheRebuilds, 0);
  assert.equal(m.scene.errorBuffer, '');
});

test('★ 0x204 入口条件 ②：只读询问返回空 ⇒ 不画、不改引擎数据、不报错', async () => {
  const host = new FakeRenderHost(); // surfaces 空 ⇒ getRenderSurface 返回 null
  const m = withTexture(machineFor([text204(5, 10, 20, [5, 0])], { renderHost: host }), 5);
  m.globalString.set(0, 'abc');
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.deepEqual(names(host), ['getRenderSurface'], '询问发了，但没画');
  assert.equal(m.render.fontObject.switches[0], 1);
  assert.equal(m.render.fontObject.glyphCacheRebuilds, 0);
  assert.equal(m.scene.errorBuffer, '');
});

test('★ 0x204 入口条件 ③：文本为空（首字节 0）⇒ 什么都不做，连询问都不发', async () => {
  const host = new FakeRenderHost();
  const m = withTexture(machineFor([text204(5, 10, 20, [5, 0])], { renderHost: host }), 5);
  m.globalString.set(0, '');
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.deepEqual(host.calls, []);
  assert.equal(m.render.fontObject.switches[0], 1);
  assert.equal(m.scene.errorBuffer, '');
});

test('★ 0x204 读序 3 → 2 → 4 → 1：第 3 个先抛、且第 4 个先于第 1 个被读', async () => {
  // 第 3 个是字符串族 ⇒ 读整型抛 0x5；第 2、4、1 个即使也会抛，也轮不到它们
  const first = machineFor([[0x204, [[0, 5], [0, 1], [5, 0], [8, 0]]]]);
  await assert.rejects(() => first.run(), /read-int:unsupported-operand-type:0x5/);

  // 第 3、2 个合法；第 4 个是字符串引用（文本读法不支持）⇒ 抛文本的错；
  // 若实现先读第 1 个（字符串族，读整型也不支持）就会抛 read-int:0x5 ⇒ 这条断言会红。
  const second = machineFor([[0x204, [[5, 0], [0, 1], [0, 2], [8, 0]]]]);
  await assert.rejects(() => second.run(), /text-unsupported-operand-type:0x8/);
});

test('★ 0x204 第 4 个按文本读：数值型 ⇒ 十进制（整数 / 负数 / 浮点）', async () => {
  const cases = [
    { instrs: [[0x55, [[3, 1], [0, 42]]], text204(5, 0, 0, [3, 1])], expected: '42' },
    { instrs: [text204(5, 0, 0, [0, -7])], expected: '-7' },
    { instrs: [[0x2d5, [[4, 1], [1, f32bits(1.5)]]], text204(5, 0, 0, [4, 1])], expected: '1.5' },
  ];
  for (const { instrs, expected } of cases) {
    const host = new FakeRenderHost();
    host.surfaces.set(5, { kind: 'surface', id: 5 });
    const m = withTexture(machineFor(instrs, { renderHost: host }), 5);
    await m.run();
    const request = host.calls.find((c) => c.call === 'drawText')?.request;
    assert.ok(request, `期望发出一次绘制请求（期望文本 "${expected}"）`);
    assert.equal(request.text, expected);
  }
});

test('★ 0x204 第 4 个按文本读：内联文本（类型 2，存储字节逐字节取反）', async () => {
  const host = new FakeRenderHost();
  host.surfaces.set(5, { kind: 'surface', id: 5 });
  const { body, raw } = scriptWithInlineText(
    (r) => [text204(5, 0, 0, [2, r])],
    'abc',
  );
  const m = new Machine({ body, table, key: KEY, renderHost: host });
  withTexture(m, 5);
  await m.run();

  const request = host.calls.find((c) => c.call === 'drawText')?.request;
  assert.ok(request);
  assert.equal(request.text, 'abc');
  assert.equal(raw > 0, true);
});

test('★ 0x204 drawText 返回 false（LockTexture 失败）⇒ 写错误通道、不重建字形暂存区', async () => {
  const host = new FakeRenderHost({ drawTextResult: false });
  host.surfaces.set(5, { kind: 'surface', id: 5 });
  const m = withTexture(machineFor([text204(5, 0, 0, [5, 0])], { renderHost: host }), 5);
  m.globalString.set(0, 'abc');
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.deepEqual(names(host), ['getRenderSurface', 'drawText']);
  assert.notEqual(m.scene.errorBuffer, '', '失败必须上报，不得静默');
  assert.equal(m.render.fontObject.switches[0], 0, '请求第 1 步（清开关）在加锁之前');
  assert.equal(m.render.fontObject.glyphCacheRebuilds, 0, '失败时不重建字形暂存区');
  assert.equal(m.scene.dirty, 0);
});

test('★ 0x204 不改纹理槽 / 绘制项 / 脏标记 / 池 / 帧 / 列表', async () => {
  const host = new FakeRenderHost();
  host.surfaces.set(5, { kind: 'surface', id: 5 });
  const m = withTexture(machineFor([text204(5, 3, 4, [5, 0])], { renderHost: host }), 5);
  m.globalString.set(0, 'x');
  m.globalInt.set(9, 123);
  const globalsBefore = [...m.globalInt.entries()];
  const localsBefore = [...m.frame.localInt.entries()];
  const listsBefore = [...m.ui.records.keys()];
  await m.run();

  assert.deepEqual([...m.globalInt.entries()], globalsBefore);
  assert.deepEqual([...m.frame.localInt.entries()], localsBefore);
  assert.deepEqual([...m.ui.records.keys()], listsBefore);
  assert.equal(m.scene.textures.size, 0);
  assert.equal(m.scene.drawItems.size, 0);
  assert.equal(m.scene.dirty, 0);
  assert.equal(m.effects.length, 0, '渲染请求只走 RenderHost，不另记一份 effects');
  assert.ok(host.calls.some((c) => c.call === 'drawText'), '绘制请求确实发出去了（不是被入口条件挡住）');
});

test('★ 0x204 没有渲染宿主 ⇒ 入口条件 ② 不成立（什么都不做，不报错）', async () => {
  const m = withTexture(machineFor([text204(5, 0, 0, [5, 0])]), 5);
  m.globalString.set(0, 'abc');
  m.render.fontObject.switches = [1, 1];
  await m.run();

  assert.equal(m.render.fontObject.switches[0], 1);
  assert.equal(m.scene.errorBuffer, '');
  assert.ok(m.diagnostics.some((d) => d.kind === 'render-host-missing'));
});

/* ──────────────────────────── 渲染状态初值 / headless 宿主 ──────────────────────────── */

test('★ 渲染状态初值：目标 = 后缓冲哨兵、混合模式 0、颜色无颜色、字体对象 = 已证默认值', () => {
  const m = new Machine({ body: Buffer.alloc(0), table, key: KEY });
  assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER);
  assert.equal(m.render.blendMode, 0);
  assert.equal(m.render.blendColor, NO_COLOR);
  const font = m.render.fontObject;
  assert.equal(font.font, null, '核心没有建字体对象 ⇒ face name 未建立');
  assert.equal(font.size, 24);
  assert.equal(font.weight, 0, '0x2BD 的二选一常量；初值未取证');
  assert.equal(font.color, 0xffffff, 'Font+0x550 主色（0x4655FB 逐字证）');
  assert.equal(font.shadowColor, 0, 'Font+0x554 副色（0x465605）');
  assert.equal(font.drawMode, 1, 'Font+0x55C（0x465611）');
  assert.equal(font.alignMode, 0, 'Font+0x313D0 的占位（初值未取证）');
  assert.equal(font.renderState, 0);
  assert.equal(font.drawPath, 0);
  assert.deepEqual(font.switches, [0, 0]);
  assert.equal(font.glyphCacheRebuilds, 0);
  assert.equal(m.renderHost, undefined);
});

test('★ headless 宿主：显式无后端（只记账，不产生像素）', async () => {
  const host = new HeadlessRenderHost();
  assert.equal(host.backend, 'headless');
  assert.equal('pixels' in host, false, '⛔ headless 没有像素面');

  const m = withTexture(machineFor([
    text204(5, 2, 3, [5, 0]),
    [0x20d, [[0, 5]]],
    [0x20d, [[0, 1000]]],
  ], { renderHost: host }), 5);
  m.globalString.set(0, 'hi');
  await m.run();

  assert.deepEqual(host.requests.map((r) => r.kind), [
    'get-render-surface', 'draw-text',
    'get-render-surface', 'set-render-target', 'release-surface',
    'get-back-buffer', 'set-render-target', 'release-surface',
  ]);
  const drawn = host.requests.find((r) => r.kind === 'draw-text')?.request;
  assert.equal(drawn?.text, 'hi');
  assert.deepEqual(
    [drawn?.size, drawn?.color, drawn?.shadowColor, drawn?.drawMode],
    [24, 0xffffff, 0, 1],
    'headless 也按契约携带字体对象状态（仍然只是记账）',
  );
  assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER, '最后一次切到后缓冲');
  // 只记账：纹理对象原样（没有像素被写进去）
  assert.deepEqual(m.scene.textureObjects.get(5), { kind: 'normal', width: 16, height: 16 });
});

test('★ headless 也可以显式地"取不到表面" / "设目标失败"（走失败路径）', async () => {
  const noSurface = new HeadlessRenderHost({ hasSurface: () => false });
  const m = withTexture(machineFor([[0x20d, [[0, 5]]]], { renderHost: noSurface }), 5);
  await m.run();
  assert.equal(m.render.target, RENDER_TARGET_BACK_BUFFER);
  assert.notEqual(m.scene.errorBuffer, '');

  const noTarget = new HeadlessRenderHost({ setRenderTargetSucceeds: false });
  const m2 = withTexture(machineFor([[0x20d, [[0, 5]]]], { renderHost: noTarget }), 5);
  await m2.run();
  assert.equal(m2.render.target, RENDER_TARGET_BACK_BUFFER);
  assert.deepEqual(names2(noTarget), ['get-render-surface', 'set-render-target', 'release-surface']);

  const noDraw = new HeadlessRenderHost({ drawTextSucceeds: false });
  const m3 = withTexture(machineFor([text204(5, 0, 0, [5, 0])], { renderHost: noDraw }), 5);
  m3.globalString.set(0, 'abc');
  await m3.run();
  assert.notEqual(m3.scene.errorBuffer, '', 'headless 也能显式走失败路径（只影响返回值）');
  assert.equal(m3.render.fontObject.glyphCacheRebuilds, 0);
});

/* ──────────────────────────── 迭代头泵点（CORE-VM-LOOP 的接线缝） ──────────────────────────── */

test('★ onIteration：每轮迭代头调一次；缺省完全不调（既有行为不变）', async () => {
  const plain = machineFor([[0x1a7, [[0, 0]]], [0x1a7, [[0, 0]]]]);
  const plainResult = await plain.run();
  assert.equal(plainResult.steps, 2);
  assert.equal(plainResult.stop.kind, 'truncated');

  let pumps = 0;
  const pumped = machineFor([[0x1a7, [[0, 0]]], [0x1a7, [[0, 0]]]], {
    onIteration: () => { pumps += 1; },
  });
  const pumpedResult = await pumped.run();
  assert.equal(pumpedResult.steps, 2, '泵点不改执行结果');
  assert.equal(pumps, pumpedResult.steps + 1, '每个执行轮 + 最后报缺口的轮各泵一次');
});

test('★ onIteration 在**门之前**：门关着的那一轮也泵，且泵完仍继续本迭代', async () => {
  /** @type {number[]} */
  const flagsSeen = [];
  /** @type {Machine | undefined} */
  let m;
  m = machineFor([[0x21c, []], [0x1a7, [[0, 0]]]], {
    onIteration: () => { flagsSeen.push(m.flags); },
  });
  const result = await m.run();

  assert.equal(result.steps, 2, '等待位在门里被清 ⇒ 后面的指令照常执行');
  assert.equal(m.flags, 0, '等待位已清（本层只有等待位）');
  assert.ok(flagsSeen.some((f) => f !== 0), '门关着（状态字非 0）的那一轮也泵过 ⇒ 泵点在门之前');
  assert.ok(flagsSeen.length > result.steps, '门迭代不执行指令，但也算一轮迭代');
});

test('★ 字体状态维护指令写进字体对象（0x1a5 / 0x75 / 0x76 / 0x77 / 0x78 / 0x2db / 0x2bd）', async () => {
  const m = machineFor([
    [0x1a5, [[5, 1]]],
    [0x75, [[0, 32]]],
    [0x76, [[0, 0x112233]]],
    [0x77, [[0, 0x445566]]],
    [0x78, [[0, 2]]],
    [0x2db, [[0, 1]]],
    [0x2bd, [[0, 1]]],
  ]);
  m.globalString.set(1, 'Yu Gothic');
  await m.run();
  const f = m.render.fontObject;
  assert.equal(f.font, 'Yu Gothic', '0x1a5 写 face name');
  assert.equal(f.size, 32, '0x75 写字号');
  assert.equal(f.color, 0x332211, '0x76 写主色（低 24 位字节倒序）');
  assert.equal(f.shadowColor, 0x665544, '0x77 写副色（低 24 位字节倒序）');
  assert.equal(f.drawMode, 2, '0x78 写绘制模式');
  assert.equal(f.drawPath, 2, '0x78 同时是绘制入口选择格');
  assert.equal(f.alignMode, 1, '0x2db 写对位模式');
  assert.equal(f.weight, 700, '0x2bd 非 0 ⇒ 700');
});


test('★ 效果分级：内部记账 / 外部请求 / 未建模，CLI trace 只打后两类', () => {
  assert.equal(effectScope('string.set'), 'internal');
  assert.equal(effectScope('array.lookup'), 'internal');
  assert.equal(effectScope('engine.scalar.write'), 'internal');
  assert.equal(effectScope('audio.playing.query'), 'external');
  assert.equal(effectScope('texture.load.request'), 'external');
  assert.equal(effectScope('engine.forward'), 'unmodeled');
  assert.equal(effectScope('subsystem.request'), 'unmodeled');
  assert.equal(effectScope('some.new.request'), 'external');
  const scopes = countEffectsByScope([
    { kind: 'string.set', detail: {} },
    { kind: 'audio.playing.query', detail: {} },
    { kind: 'engine.forward', detail: {} },
  ]);
  assert.deepEqual(scopes.effectCounts, { 'audio.playing.query': 1, 'engine.forward': 1 });
  assert.deepEqual(scopes.internalEffectCounts, { 'string.set': 1 });
});
