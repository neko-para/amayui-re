/** @env pure @kind contract @why 场景族 / 等待门 / 计时窗的引擎数据写错了（窗字段 / 颜色归一化 / 门判据），而执行器仍然跑得下去 */
/**
 * tools/test/emulator-new-scene.test.mjs —— 场景族（0x202/0x203/0x320/0x322/0x323）+ 等待门（0x21c / 0x101）的守卫
 *
 * 覆盖 `data/spec/chapters/60-ops-scene.md` 的场景族与 `60-ops-wait.md` 的等待/输入，以及 `70-vm.md` 的门与计时窗。
 * 用合成脚本字节，不依赖游戏安装。
 *
 * ★ 判据为什么是这些：
 *   - 颜色归一化（透明度 >255 / <0、颜色 <0）写错不会报错，只会颜色不对；
 *   - `0x202` 有"已存在"门、`0x323` **没有**（真实不对称）—— 抹平它就会让"给不存在的条目排窗"变成另一种行为；
 *   - 窗收尾对称：绘制项与网格**都清**"有窗"位；
 *   - 等待门是"场景还有动画"，不是"表里还有未到期的窗"；门关着时时钟才前进。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { decodeInt, encodeInt } from '../../apps/emulator-new/src/core/values.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

function f32bits(v) {
  const b = new DataView(new ArrayBuffer(4));
  b.setFloat32(0, v, true);
  return b.getUint32(0, true);
}

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

function item(over = {}) {
  return {
    flags: 1, textureId: 0, left: 0, top: 0, right: 0, bottom: 0, aux: [0, 0, 0],
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorNow: -1, colorTo: -1, unknown: new Map(), ...over,
  };
}

function mesh(over = {}) {
  return {
    flags: 1, count: 0, op10: 0, copies: [[], []], colors: [], vbo: [],
    start: 0, delay: 0, duration: 0, colorFrom: -1, colorNow: -1, colorTo: -1, unknown: new Map(), ...over,
  };
}

test('★ 0x202：只有绘制项**已存在**才排窗；颜色 = 不透明黑 | RGB', async () => {
  const m = machineFor([[0x202, [[0, 5], [0, 300], [0, 300], [0, 255], [0, 0xffffff]]]]);
  m.scene.drawItems.set(5, item());
  await m.run();
  const it = m.scene.drawItems.get(5);
  assert.equal(it.flags & 2, 2, '有窗位');
  assert.deepEqual([it.delay, it.duration], [300, 300]);
  assert.equal(it.colorTo >>> 0, 0xffffffff);
  assert.equal(it.start, 0, '起点 0 = 未盖章');
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x202：绘制项**不存在** ⇒ 建默认项，但一个窗字段都不写（"静默"只对颜色成立）', async () => {
  const m = machineFor([[0x202, [[0, 9], [0, 300], [0, 300], [0, 255], [0, 0]]]]);
  await m.run();
  const it = m.scene.drawItems.get(9);
  assert.ok(it, '默认记录被建出来了');
  assert.equal(it.flags & 2, 0, '没有窗位');
});

test('★ 0x203：直接改起色、不排窗；透明度 >255 夹到 255', async () => {
  const m = machineFor([[0x203, [[0, 5], [0, 0], [0, 999], [0, 0x010203]]]]);
  m.scene.drawItems.set(5, item({ colorFrom: -1 }));
  await m.run();
  const it = m.scene.drawItems.get(5);
  assert.equal(it.colorFrom >>> 0, 0xff010203);
  assert.equal(it.flags & 2, 0, '不排窗');
});

test('★ 0x203：透明度 <0 ⇒ 取现有颜色的高字节；颜色 <0 ⇒ 取现有颜色', async () => {
  const m = machineFor([[0x203, [[0, 5], [0, 0], [0, -1], [0, -1]]]]);
  m.scene.drawItems.set(5, item({ colorFrom: 0x11223344 }));
  await m.run();
  assert.equal(m.scene.drawItems.get(5).colorFrom >>> 0, 0x11223344);
});

test('★ 0x320：顶点数 ≤ 0 ⇒ 写错误文本、不建网格', async () => {
  const m = machineFor([[0x320, [
    [0, 2], [0, 0], [0, 0], [0, 0], [3, 0], [3, 4], [0, 0], [0, 0], [0, 0], [0, 0],
  ]]]);
  await m.run();
  assert.equal(m.scene.meshes.size, 0);
  assert.match(m.scene.errorBuffer, /0/);
});

test('★ 0x320：2..8 全按地址读 ⇒ 前两段原样拷贝、第三段合成、4/7/8 交给顶点缓冲对象', async () => {
  // 操作数形状照真实 LOGO：2/3/4/7/8 是浮点池（type 4），5/6 是整型池（type 3）
  const m = machineFor([[0x320, [
    [0, 2], [4, 9], [4, 13], [4, 17],
    [3, 160], [3, 170], [4, 21], [4, 25],
    [0, 4], [0, 7],
  ]]]);
  for (let i = 0; i < 4; i += 1) {
    m.globalFloat.set(9 + i, 0x1000 + i);   // 第 1 段：32 位位模式，逐字原样
    m.globalFloat.set(13 + i, 0x2000 + i);  // 第 2 段
    m.globalInt.set(160 + i, encodeInt(0x11223311 + i, KEY));
    m.globalInt.set(170 + i, encodeInt(0x44556622 + i, KEY));
  }
  await m.run();
  const me = m.scene.meshes.get(2);
  assert.ok(me);
  assert.equal(me.flags & 1, 1, '已存在位');
  assert.equal(me.count, 4);
  assert.equal(me.op10, 7);
  assert.deepEqual(me.copies, [
    [0x1000, 0x1001, 0x1002, 0x1003],
    [0x2000, 0x2001, 0x2002, 0x2003],
  ]);
  assert.deepEqual(me.colors, [
    ((0x11 << 24) | 0x556622) >>> 0,
    ((0x12 << 24) | 0x556623) >>> 0,
    ((0x13 << 24) | 0x556624) >>> 0,
    ((0x14 << 24) | 0x556625) >>> 0,
  ]);
  assert.deepEqual(me.vbo, [
    { pool: 'globalFloat', index: 17 },
    { pool: 'globalFloat', index: 21 },
    { pool: 'globalFloat', index: 25 },
  ]);
  assert.equal(m.scene.dirty, 1);
});

test('★ 0x322：起色 + 按起色烘顶点色（t=0）', async () => {
  const m = machineFor([[0x322, [[0, 2], [0, 0], [0, 255], [0, 0x336699]]]]);
  m.scene.meshes.set(2, mesh({ count: 3, colors: [0, 0, 0] }));
  await m.run();
  const me = m.scene.meshes.get(2);
  assert.equal(me.colorFrom >>> 0, 0xff336699);
  assert.deepEqual(me.colors, [0xff336699, 0xff336699, 0xff336699]);
});

test('★ 0x323：没有"已存在"门（给不存在的网格排窗也照排）', async () => {
  const m = machineFor([[0x323, [[0, 77], [0, 4500], [0, 500], [0, 255], [0, 0]]]]);
  await m.run();
  const me = m.scene.meshes.get(77);
  assert.ok(me, '默认网格被建出来了');
  assert.equal(me.flags & 2, 2, '有窗位（无条件置）');
  assert.deepEqual([me.delay, me.duration], [4500, 500]);
  assert.equal(me.colorTo >>> 0, 0xff000000);
});

test('★ 窗收尾：绘制项与网格都清"有窗"位', async () => {
  const m = machineFor([], { tickMs: 16 });
  m.scene.now = 0;
  m.scene.drawItems.set(1, item({ flags: 3, delay: 0, duration: 100, colorFrom: 0xff000000, colorTo: 0xff00ff00 }));
  m.scene.meshes.set(1, mesh({ flags: 3, delay: 0, duration: 100, colorFrom: 0xff000000, colorTo: 0xff00ff00 }));
  // 第一帧盖章，再走到窗结束
  for (let i = 0; i < 20; i += 1) {
    m.stepScene();
    m.tick();
  }
  const it = m.scene.drawItems.get(1);
  const me = m.scene.meshes.get(1);
  assert.equal(it.flags & 2, 0, '绘制项的"有窗"位被清');
  assert.equal(me.flags & 2, 0, '网格的"有窗"位被清');
  assert.equal(it.colorFrom >>> 0, 0xff00ff00, '起色 = 终色');
  assert.equal(it.colorTo, -1);
});

test('★ 窗进行中：插值写派生色 / 顶点缓冲，不写回当前颜色；半程是线性的', () => {
  const m = machineFor([], { tickMs: 16 });
  m.scene.now = 1;
  m.scene.drawItems.set(1, item({ flags: 3, delay: 0, duration: 500, colorFrom: 0xff000000, colorTo: 0 }));
  m.scene.meshes.set(1, mesh({ flags: 3, delay: 0, duration: 500, colorFrom: 0xff000000, colorTo: 0, colors: [0xff000000] }));
  m.stepScene(); // 在非 0 时刻盖章（start = 1；0 是“未盖章”哨兵）
  m.scene.now = 251;
  m.stepScene();
  const it = m.scene.drawItems.get(1);
  const me = m.scene.meshes.get(1);
  assert.equal(it.colorFrom >>> 0, 0xff000000, '当前颜色在窗进行中保持原值');
  assert.equal(me.colorFrom >>> 0, 0xff000000, '网格当前颜色也保持原值');
  assert.equal(it.colorNow >>> 0, 0x80000000, '绘制项的派生色 = 半程线性值（不是指数衰减）');
  assert.equal(me.colorNow >>> 0, 0x80000000, '网格的派生色 = 半程线性值');
  assert.deepEqual(me.colors, [0x80000000], '网格顶点缓冲每步重烘');
});

test('★ 0x21c 等待门：颜色窗不挡（放行看消息子系统，不看动画窗）', async () => {
  const m = machineFor([
    [0x21c, []],
    [0x2d5, [[4, 5], [1, f32bits(1.5)]]],
  ], { tickMs: 16 });
  // 场景里有一个 100ms 的窗（起点未盖章）——**它不许**把等待位挡住：
  // 语料的自然放行只读消息子系统的计时器 + 场景对象状态字，颜色窗不在其中。
  m.scene.meshes.set(1, mesh({ flags: 3, delay: 0, duration: 100, colorFrom: 0, colorTo: 0xffffffff }));
  const r = await m.run();
  assert.equal(r.steps, 2, 'wait 之后的那条指令被执行了');
  assert.equal(m.flags, 0, '等待位已清');
  assert.equal(m.ticks, 0, '颜色窗还在走也必须立刻放行（不许 tick 空转）');
});

test('★ 0x21c 等待门：场景没有动画 ⇒ 下一个迭代就放行（不空转）', async () => {
  const m = machineFor([
    [0x21c, []],
    [0x2d5, [[4, 5], [1, f32bits(1.5)]]],
  ]);
  const r = await m.run();
  assert.equal(r.steps, 2);
  assert.equal(m.ticks, 0, '没有动画就不该 tick');
});

test('★ 0x101 采样并丢弃：清状态位、输入字归零、两个标量 1/0、外部请求记一条', async () => {
  const m = machineFor([[0x101, []]]);
  m.inputWord = 0xdeadbeef;
  await m.run();
  assert.equal(m.flags & 0x08000000, 0);
  assert.equal(m.inputWord, 0);
  assert.equal(m.scalars.get('pollInput.a'), 1);
  assert.equal(m.scalars.get('pollInput.b'), 0);
  assert.deepEqual(m.effects.map((e) => e.kind), ['input.poll']);
});

test('★ 0x20f 播影片：建对象、置影片位与"有活动对象"、发出播放请求（不等待）', async () => {
  const m = machineFor([[0x20f, [[0, 21063], [0, 42], [0, 131076]]]]);
  m.scene.textureObjects.set(42, { kind: 'normal', width: 1280, height: 720 });
  const r = await m.run({ maxSteps: 1 });
  assert.equal(r.steps, 1);
  const obj = m.scene.objectSlots.get(42);
  assert.equal(obj.kind, 'movie');
  assert.equal(obj.resourceId, 21063);
  assert.equal(obj.opt, 131076);
  assert.equal(obj.started, true);
  assert.equal(m.flags & 0x2000, 0x2000, '影片位被置起（门随后会关）');
  assert.equal(m.scene.hasObjects, 1);
  assert.deepEqual(m.effects.map((e) => e.kind), ['audio.movie.play']);
});

test('★ 0x20f 的绑定门：槽上没有纹理绑定 ⇒ 响亮失败（引擎抛"显示消息"）', async () => {
  const m = machineFor([[0x20f, [[0, 21063], [0, 42], [0, 131076]]]]);
  await assert.rejects(m.run({ maxSteps: 1 }), /movie-binding-missing:42/);
});

test('★ 0x20f 同槽已有对象 ⇒ 不重建（只改配置与启动）', async () => {
  const m = machineFor([[0x20f, [[0, 21063], [0, 42], [0, 1]]]]);
  m.scene.textureObjects.set(42, { kind: 'normal', width: 1, height: 1 });
  m.scene.objectSlots.set(42, { kind: 'movie', slot: 42, resourceId: 1, opt: 0, started: false });
  await m.run({ maxSteps: 1 });
  const obj = m.scene.objectSlots.get(42);
  assert.equal(obj.resourceId, 21063);
  assert.equal(obj.started, true);
});

test('★ 宿主时钟：给了 clock 时 `tick()` 读宿主读数，而不是 `now += tickMs`', () => {
  let now = 1000;
  const m = machineFor([], { clock: () => now });
  now = 1250;
  m.tick();
  assert.equal(m.now, 1250, 'now 必须等于宿主读数');
  assert.equal(m.scene.now, 1250);
  assert.equal(m.ticks, 1);
});

test('★ 宿主时钟 + 等待门：颜色窗不挡，等待位当拍就清', async () => {
  let now = 0;
  const m = machineFor([
    [0x21c, []],
    [0x2d5, [[4, 5], [1, f32bits(1.5)]]],
  ], { clock: () => now });
  m.scene.meshes.set(1, mesh({ flags: 3, delay: 0, duration: 100, colorFrom: 0, colorTo: 0xffffffff }));
  const r = await m.run();
  assert.equal(r.steps, 2);
  assert.equal(m.flags, 0, '等待位已清');
  assert.equal(m.ticks, 0, '颜色窗还在走也必须立刻放行');
});

test('★ 0x344 场景条目：置存在位 + 写值 + 标脏', async () => {
  const m = machineFor([[0x344, [[0, 7], [0, 42]]]]);
  await m.run();
  assert.deepEqual(m.scene.entries.get(7), { present: 1, value: 42 }, '记录 7：存在位 1、第二格 42');
  assert.equal(m.scene.dirty, 1, '场景标脏');
  assert.equal(m.flags, 0, '不置门位');
});
