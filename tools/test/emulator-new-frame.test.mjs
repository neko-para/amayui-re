/** @env pure @kind contract @why 0x8f/0x5 的"位置"口径、返回点算式、长度槽清零任一写错，执行流都会静默跳到别处（不报错，只是跑错脚本） */
/**
 * tools/test/emulator-new-frame.test.mjs —— 帧与调用（`15-frame.md` 的 CORE-FRAME-CALL / CORE-FRAME-RET）守卫
 *
 * ★ 判据为什么是这些（每条都能被"写错"弄红）：
 *   - **位置不是字节偏移**：`0x8f` 的操作数是 dword 序号 ⇒ 目标 4 必须落到 **byte 16**；
 *     把 `4 × 位置` 写成 `位置` 会落到 byte 4；
 *   - **返回点 = 下一条指令**：压进栈的必须是 `本指令位置 + 1 + 2×argc`（= `位置 + 3`），
 *     压成本指令位置会跳回自己（死循环）—— 这儿直接断言栈里的数；
 *   - **控制转移不自动推进**：跳转后分派器不能再按长度槽加一次。断言"精确的 ip"：多推进一次就多 12（call）/ 4（ret）；
 *   - **哨兵 −1**：不压栈、不跳，照常推进 —— 把它当普通位置会算出 `ip = −4`；
 *   - **空栈不许静默**：返回栈空时引擎的后续未取证 ⇒ 实现必须响亮失败（静默 exit 会让"跑完了"和"跑飞了"长得一样）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { decodeInt } from '../../apps/emulator-new/src/core/values.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;

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

function machineFor(instrs) {
  return new Machine({ body: script(instrs), table, key: KEY });
}

/** 一条"什么都不改"的指令（0x1a8，长度 4）—— 用来铺格子 */
const NOP = 0x1a8;

test('★ 0x8f：位置 × 4 = 字节偏移', async () => {
  // ip0 call(→位置 4 = byte 16)、byte 16 ret；返回点 = 位置 3 = byte 12（那条 NOP）
  const m = machineFor([[0x8f, [[0, 4]]], [NOP, []], [0x5, []]]);
  const r = await m.run({ stopAtOpcode: 0x5 });
  assert.equal(r.steps, 1);
  assert.equal(m.ip, 16, '位置 4 ⇒ 字节 16（把"位置就是字节"实现在这里会得到 4）');
  assert.deepEqual(m.frame.retStack, [3], '返回点 = 0/4 + 1 + 2×1 = 3（压成 0 会跳回自己）');
});

test('★ 0x8f 的控制转移不自动推进：ip 必须正好 16（多推进一次会变成 28）', async () => {
  const m = machineFor([[0x8f, [[0, 4]]], [NOP, []], [0x5, []]]);
  await m.run({ stopAtOpcode: 0x5 });
  assert.equal(m.ip, 16);
  assert.equal(m.steps, 1);
});

test('★ 0x5：弹出返回点并跳回（不是"退出"，也不是弹帧），且不自动推进', async () => {
  const m = machineFor([[0x5, []]]);
  m.frame.retStack = [2];
  const r = await m.run();
  assert.equal(m.ip, 8, '返回位置 2 ⇒ 字节 8（多推进一次会变成 12）');
  assert.equal(r.steps, 1);
  assert.deepEqual(m.frame.retStack, [], '弹出后栈里不许还留着它');
});

test('★ 0x8f 哨兵 −1：不压栈、不跳，照常推进到下一条', async () => {
  const m = machineFor([[0x8f, [[0, 0xffffffff]]], [NOP, []]]);
  const r = await m.run();
  assert.equal(m.ip, 16, '长度槽留着 ⇒ 推进 4 × 3 = 12（把 −1 当位置会算出 ip = −4）');
  assert.deepEqual(m.frame.retStack, [], '哨兵不许留栈');
  assert.equal(r.stop.kind, 'truncated');
});

test('★ 0x5 空栈：响亮失败（不许静默 exit —— 那会让"跑完"与"跑飞"长得一样）', async () => {
  const m = machineFor([[0x5, []]]);
  await assert.rejects(m.run(), /ret-with-empty-stack/);
});

test('★ 0x5 栈顶是哨兵 −1：不跳，只前进 4 字节（不是跳去 −4）', async () => {
  const m = machineFor([[0x5, []]]);
  m.frame.retStack = [-1];
  const r = await m.run();
  assert.equal(m.ip, 4, '长度槽留 1 ⇒ 前进 4（把 −1 当位置会算出 ip = −4）');
  assert.equal(r.stop.kind, 'truncated');
  assert.deepEqual(m.frame.retStack, [], '哨兵被弹出，不许留在栈里');
});

test('★ 嵌套调用：返回栈后进先出（返回点 3 / 6 依次弹出）', async () => {
  const m = machineFor([[0x8f, [[0, 6]]], [NOP, []], [0x5, []]]);
  await m.run({ stopAtOpcode: 0x5 });
  assert.deepEqual(m.frame.retStack, [3], '第一条 call 压 3');
  m.frame.retStack.push(6);
  await m.run({ stopAtOpcode: 0x5 });
  assert.deepEqual(m.frame.retStack, [3, 6], '后进');
});
test('★ 0xa0 条件跳转：非 0 走 op2、为 0 走 op3、目标 −1 不跳', async () => {
  // 布局：a0 长 7 字（0..6），NOP 在字 7/8/9；目标 7 = 字节 28、8 = 字节 32
  const m = machineFor([[0xa0, [[0, 1], [0, 7], [0, 8]]], [0x1a8, []], [0x1a8, []], [0x1a8, []]]);
  await m.run({ stopAtOpcode: 0x1a8 });
  assert.equal(m.ip, 28, '条件非 0 ⇒ 用第 2 个操作数（7 ⇒ 字节 28）');

  const zero = machineFor([[0xa0, [[0, 0], [0, 7], [0, 8]]], [0x1a8, []], [0x1a8, []], [0x1a8, []]]);
  await zero.run({ stopAtOpcode: 0x1a8 });
  assert.equal(zero.ip, 32, '条件为 0 ⇒ 用第 3 个操作数（8 ⇒ 字节 32）');

  const sentinel = machineFor([[0xa0, [[0, 1], [0, 0xffffffff], [0, 3]]]]);
  const r = await sentinel.run();
  assert.equal(sentinel.ip, 28, '目标 −1 ⇒ 不跳，长度槽留着 ⇒ 推进 4×7 = 28');
  assert.equal(r.stop.kind, 'truncated');
});
test('★ 0x3 / 0x2 多脚本', async () => {
  const child = script([[0x2, []]]);
  const root = script([[0x3, [[0, 1]]], [0x1a8, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? child : null) });
  const r = await m.run();
  assert.equal(r.steps, 3, '根@0 → 子@0 → 根@12：三条（多推一次会变 4）');
  assert.equal(r.stop.kind, 'truncated', '根@12 那条执行完就该到底（说明返回点确实是 12）');
  assert.equal(m.cur, 0, '回到根帧（0 号槽）');
  assert.equal(m.slots[1], null, '子帧槽已释放（显式空槽，不是"洞"）');
  // ★ 槽复用：下一次换脚本仍落在 1 号槽（帧号 = 父号 + 1，与"曾经占过几个槽"无关）
  m.enterScript(script([[NOP, []]]));
  assert.equal(m.cur, 1, '★ 复用 1 号槽（不是"新开一个 2 号"）');
  assert.equal(m.slots[0]?.body, root, '根帧记录不动');
});

test('★ 0x3 局部池隔离：子脚本写自己的槽 0 = 7，父脚本读同号槽必须读到自己的 1', async () => {
  const NOP = 0x1a8;
  const child = script([[0x50, [[9, 0], [0, 7], [0, 0]]], [0x2, []]]);
  // 根：槽0 = 1；调子；返回后 槽1 = 槽0 + 0
  const root = script([
    [0x50, [[9, 0], [0, 1], [0, 0]]],
    [0x3, [[0, 1]]],
    [0x50, [[9, 1], [9, 0], [0, 0]]],
    [NOP, []],
  ]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? child : null) });
  await m.run();
  assert.equal(decodeInt(m.frame.localInt.get(1), KEY), 1, '父池的槽 0 必须还是 1（写 7 说明池被共享）');
  assert.equal(m.cur, 0, '回到根帧');
  assert.equal(m.slots[1], null, '子帧槽已释放');
});

test('★ 0x9 全局重置：丢全部帧、清空三个全局池、状态选择字归 0，并以 id 0 重装根脚本', async () => {
  const child = script([[0x9, []]]);
  const root = script([[0x3, [[0, 1]]], [0x1a8, []], [0x1a8, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 0 ? root : id === 1 ? child : null) });
  m.globalInt.set(3, 12345);
  m.globalFloat.set(4, 0x3f800000);
  m.globalString.set(5, 'x');
  const r = await m.run({ maxSteps: 4 });
  assert.equal(m.cur, 0, '0x9 之后当前帧号回 0（根帧）');
  assert.equal(m.slots.filter(Boolean).length, 1, '0x9 之后只留 0 号槽那一帧（其余全释放）');
  assert.equal(m.ip === 0 || r.stop.kind === 'step-limit', true, '根脚本从头开始（不是 12、不是 halted）');
  assert.equal(m.halted, false);
  assert.equal(m.globalInt.size, 0, '整型池清回初值（decode 后为 0）');
  assert.equal(m.globalFloat.size, 0, '浮点池清 0');
  assert.equal(m.globalString.size, 0, '字符串池清空');
});

test('★ 0x3 深度上限：按**帧号**判 —— 当前帧号 39 时再换脚本就报错', async () => {
  const self = script([[0x3, [[0, 1]]]]);
  const m = new Machine({ body: self, table, key: KEY, loadScript: async () => self });
  await assert.rejects(m.run({ maxSteps: 50 }), /深度超限/);
});

// ─────────────────────────────────────── ★★ 帧 = 40 槽 + `cur`（不是 LIFO 栈；2026-10）
//
// 逐字（锚 = EA）：`0x2` 的正面支是"当前帧号 ← **父帧号**" + **有条件释放**（父号 + 1 == 当前号）
// + 从父号起**向下跳过空槽**；`0x6` 往**任意槽**写记录而 `cur` 不动；`0x8` 切到**任意已装载槽**。
// 栈（只动末端、`cur -= 1`）在这些情形下**看起来也正常**，所以下面每条都构造"父帧号 ≠ 栈顶下一格"。

test('★★ 跨槽返回：父帧号 ≠ 当前帧号 − 1 ⇒ `0x2` 回到**父帧号**，且**不释放**目标帧', async () => {
  const childBody = script([[0x2, []]]);
  const rootBody = script([[NOP, []]]);
  const m = new Machine({ body: rootBody, table, key: KEY });
  const rec = m.loadFrameAt(3, childBody);
  m.callFrameAt(3); // 父帧号 ← 0（调用者），当前帧号 ← 3
  assert.equal(rec.parent, 0, '换帧把父帧号写成调用者');
  assert.equal(m.cur, 3);

  const ok = m.returnToCaller();

  assert.equal(ok, true, '有父帧 ⇒ 不是"整个执行结束"');
  assert.equal(m.cur, 0, '★ 回到**父帧号 0**（`cur -= 1` 会回到 2 —— 那一槽可能空着/装着别的脚本）');
  assert.equal(m.slots[3], rec, '★★ 父号 + 1 ≠ 当前号 ⇒ 目标帧**不许**被释放（它还装载着）');
});

test('★★ `0x6` 装载帧：记录落在**指定槽**、当前帧号**一动都不动**', async () => {
  const loaded = script([[NOP, []]]);
  const root = script([[0x6, [[0, 1], [0, 4]]], [NOP, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? loaded : null) });
  await m.run({ maxSteps: 1 });

  assert.equal(m.cur, 0, '★★ 装载前后当前帧号相同（改了就是"顺手把当前帧换掉"）');
  assert.equal(m.slots[4]?.body, loaded, '★ 记录真的在槽 4 上（不是"活动链末端"）');
  assert.equal(m.slots[4]?.parent, 0, '父帧号 ← 装载时正在执行的帧号');
  assert.equal(m.slots[4]?.ip, 0, '新记录从它的开头开始');
  assert.equal(m.ip, 20, '`0x6` 照常推进（长度槽 5 ⇒ 4×5 = 20 字节）');
});

test('★★ `0x8` 换帧 + `0x2` 跨槽返回：切到已装载帧、从它开头执行、回来时**不释放**它', async () => {
  const target = script([[0x2, []]]);
  const root = script([[0x8, [[0, 5]]], [NOP, []]]);
  const m = new Machine({ body: root, table, key: KEY });
  const rec = m.loadFrameAt(5, target);
  rec.frame.retStack.push(9); // 目标帧"上次"留下的返回点：换帧不清
  rec.ip = 8;                 // 上次停在哪：换帧必须把它拨回 0

  const r = await m.run();

  assert.equal(r.steps, 3, '0x8 → 目标帧的 0x2 → 根@12 的 NOP（多一步说明返回点错位）');
  assert.equal(m.cur, 0, '★ 回到调用者（父帧号 0）');
  assert.equal(m.slots[5], rec, '★ 不相邻 ⇒ 目标帧仍装载着（再切回去还在）');
  assert.deepEqual(rec.frame.retStack, [9], '★★ 换帧**不清**目标帧的返回栈');
  assert.equal(rec.parent, 0, '目标帧的父帧号 ← 调用者');
  assert.equal(rec.ip, 0, '★ 换帧把目标帧的指令指针拨回它的开头（预置 8）');
});

test('★★ `0x8` 目标帧未装载 ⇒ 报错（文本含帧号），且当前帧不变', async () => {
  const m = new Machine({ body: script([[0x8, [[0, 7]]]]), table, key: KEY });
  await assert.rejects(
    m.run(),
    /この階層にはファイルが読み込まれていません．Depth=7/,
    '★ 报错文本照抄引擎（并带上目标帧号）',
  );
  assert.equal(m.cur, 0, '★ 报错后当前帧号仍是调用者');
});

test('★★ 深度上限按**帧号**判：先装到 39 号槽再切过去 ⇒ 再换脚本就报错（不是按"已有几帧"）', async () => {
  const self = script([[0x3, [[0, 1]]]]);
  const m = new Machine({ body: self, table, key: KEY, loadScript: async () => self });
  m.loadFrameAt(39, self);
  m.callFrameAt(39);
  assert.equal(m.slots.filter(Boolean).length, 2, '槽数组里只有 2 个非空槽（按"已有几帧"判会放过）');
  assert.equal(m.cur, 39);
  await assert.rejects(m.run({ maxSteps: 5 }), /深度超限/, '★ 当前帧号 39 ⇒ 再换脚本超限');
});

test('★★ `0x8` 的父帧号 ← **切进来的那一帧**（不是"当初装载它的那一帧"）', async () => {
  const target = script([[0x2, []]]);
  const child = script([[0x8, [[0, 4]]], [0x2, []]]); // 槽 1：切到槽 4，再结束自己
  const root = script([[0x3, [[0, 1]]], [NOP, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: async (id) => (id === 1 ? child : null) });
  const rec = m.loadFrameAt(4, target); // 装载者是槽 0
  assert.equal(rec.parent, 0, '装载时父帧号 ← 装载者');

  await m.run();

  assert.equal(rec.parent, 1, '★ 父帧号 ← **切帧者**（槽 1），不是装载者（槽 0）');
  assert.equal(m.cur, 0, '槽 4 的 0x2 回槽 1，槽 1 的 0x2 回槽 0');
  assert.equal(m.steps, 5, '0x3 → 槽1 的 0x8 → 槽4 的 0x2 → 槽1 的 0x2 → 根@12 的 NOP');
});

test('★ key=0：只在 Machine 构造期记一次 codec-key-placeholder（不是每条 0x6 一条）', async () => {
  const child = script([]);
  const body = script([[0x6, [[0, 1], [0, 1]]], [0x6, [[0, 1], [0, 2]]]]);
  const zero = new Machine({ body, table, key: 0, loadScript: async () => child });
  await zero.run();
  assert.equal(zero.diagnostics.filter((d) => d.kind === 'codec-key-placeholder').length, 1);
  assert.equal(zero.diagnostics.filter((d) => d.kind.startsWith('codec-key-placeholder:')).length, 0);

  const nonzero = new Machine({ body, table, key: KEY, loadScript: async () => child });
  await nonzero.run();
  assert.equal(nonzero.diagnostics.filter((d) => d.kind.startsWith('codec-key-placeholder')).length, 0);
});

test('★ 不给 key ⇒ Machine 运行期生成非 0 值（正常入口不需要用户输入 key）', async () => {
  const m = new Machine({ body: script([]), table });
  assert.notEqual(m.key, 0, '运行期生成的 key 必须非 0');
  assert.equal(m.diagnostics.filter((d) => d.kind === 'codec-key-placeholder').length, 0);
});
