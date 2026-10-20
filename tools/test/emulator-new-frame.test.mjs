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

import { loadOpcodeTable } from '../../apps/emulator-new/src/spec.ts';
import { Machine } from '../../apps/emulator-new/src/machine.ts';
import { decodeInt } from '../../apps/emulator-new/src/values.ts';

const table = loadOpcodeTable();
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

test('★ 0x8f：位置 × 4 = 字节偏移', () => {
  // ip0 call(→位置 4 = byte 16)、byte 16 ret；返回点 = 位置 3 = byte 12（那条 NOP）
  const m = machineFor([[0x8f, [[0, 4]]], [NOP, []], [0x5, []]]);
  const r = m.run({ stopAtOpcode: 0x5 });
  assert.equal(r.steps, 1);
  assert.equal(m.ip, 16, '位置 4 ⇒ 字节 16（把"位置就是字节"实现在这里会得到 4）');
  assert.deepEqual(m.frame.retStack, [3], '返回点 = 0/4 + 1 + 2×1 = 3（压成 0 会跳回自己）');
});

test('★ 0x8f 的控制转移不自动推进：ip 必须正好 16（多推进一次会变成 28）', () => {
  const m = machineFor([[0x8f, [[0, 4]]], [NOP, []], [0x5, []]]);
  m.run({ stopAtOpcode: 0x5 });
  assert.equal(m.ip, 16);
  assert.equal(m.steps, 1);
});

test('★ 0x5：弹出返回点并跳回（不是"退出"，也不是弹帧），且不自动推进', () => {
  const m = machineFor([[0x5, []]]);
  m.frame.retStack = [2];
  const r = m.run();
  assert.equal(m.ip, 8, '返回位置 2 ⇒ 字节 8（多推进一次会变成 12）');
  assert.equal(r.steps, 1);
  assert.deepEqual(m.frame.retStack, [], '弹出后栈里不许还留着它');
});

test('★ 0x8f 哨兵 −1：不压栈、不跳，照常推进到下一条', () => {
  const m = machineFor([[0x8f, [[0, 0xffffffff]]], [NOP, []]]);
  const r = m.run();
  assert.equal(m.ip, 16, '长度槽留着 ⇒ 推进 4 × 3 = 12（把 −1 当位置会算出 ip = −4）');
  assert.deepEqual(m.frame.retStack, [], '哨兵不许留栈');
  assert.equal(r.stop.kind, 'truncated');
});

test('★ 0x5 空栈：响亮失败（不许静默 exit —— 那会让"跑完"与"跑飞"长得一样）', () => {
  const m = machineFor([[0x5, []]]);
  assert.throws(() => m.run(), /ret-with-empty-stack/);
});

test('★ 0x5 栈顶是哨兵 −1：不跳，只前进 4 字节（不是跳去 −4）', () => {
  const m = machineFor([[0x5, []]]);
  m.frame.retStack = [-1];
  const r = m.run();
  assert.equal(m.ip, 4, '长度槽留 1 ⇒ 前进 4（把 −1 当位置会算出 ip = −4）');
  assert.equal(r.stop.kind, 'truncated');
  assert.deepEqual(m.frame.retStack, [], '哨兵被弹出，不许留在栈里');
});

test('★ 嵌套调用：返回栈后进先出（返回点 3 / 6 依次弹出）', () => {
  const m = machineFor([[0x8f, [[0, 6]]], [NOP, []], [0x5, []]]);
  m.run({ stopAtOpcode: 0x5 });
  assert.deepEqual(m.frame.retStack, [3], '第一条 call 压 3');
  m.frame.retStack.push(6);
  m.run({ stopAtOpcode: 0x5 });
  assert.deepEqual(m.frame.retStack, [3, 6], '后进');
});
test('★ 0xa0 条件跳转：非 0 走 op2、为 0 走 op3、目标 −1 不跳', () => {
  // 布局：a0(cond=1,op2=5,op3=6) @0（长 28）→ 位置 7 = 字节 28 … 用简单布局：目标 2 = 字节 8
  const m = machineFor([[0xa0, [[0, 1], [0, 2], [0, 3]]], [0x1a8, []], [0x1a8, []], [0x1a8, []]]);
  m.run();
  assert.equal(m.ip, 8, '条件非 0 ⇒ 用第 2 个操作数（2 ⇒ 字节 8）');

  const zero = machineFor([[0xa0, [[0, 0], [0, 2], [0, 3]]], [0x1a8, []], [0x1a8, []], [0x1a8, []]]);
  zero.run();
  assert.equal(zero.ip, 12, '条件为 0 ⇒ 用第 3 个操作数（3 ⇒ 字节 12）');

  const sentinel = machineFor([[0xa0, [[0, 1], [0, 0xffffffff], [0, 3]]]]);
  const r = sentinel.run();
  assert.equal(sentinel.ip, 28, '目标 −1 ⇒ 不跳，长度槽留着 ⇒ 推进 4×7 = 28');
  assert.equal(r.stop.kind, 'truncated');
});
test('★ 0x3 / 0x2 多脚本', () => {
  const child = script([[0x2, []]]);
  const root = script([[0x3, [[0, 1]]], [0x1a8, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: (id) => (id === 1 ? child : null) });
  const r = m.run();
  assert.equal(r.steps, 3, '根@0 → 子@0 → 根@12：三条（多推一次会变 4）');
  assert.equal(r.stop.kind, 'truncated', '根@12 那条执行完就该到底（说明返回点确实是 12）');
  assert.equal(m.frames.length, 0, '子帧已弹回');
  assert.equal(m.frames.length, 0);
});

test('★ 0x3 局部池隔离：子脚本写自己的槽 0 = 7，父脚本读同号槽必须读到自己的 1', () => {
  const NOP = 0x1a8;
  const child = script([[0x50, [[9, 0], [0, 7], [0, 0]]], [0x2, []]]);
  // 根：槽0 = 1；调子；返回后 槽1 = 槽0 + 0
  const root = script([
    [0x50, [[9, 0], [0, 1], [0, 0]]],
    [0x3, [[0, 1]]],
    [0x50, [[9, 1], [9, 0], [0, 0]]],
    [NOP, []],
  ]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: (id) => (id === 1 ? child : null) });
  m.run();
  assert.equal(decodeInt(m.frame.localInt.get(1), KEY), 1, '父池的槽 0 必须还是 1（写 7 说明池被共享）');
  assert.equal(m.frames.length, 0);
});

test('★ 0x9 全局重置：丢全部帧并以 id 0 重装根脚本（不是返回父帧、不是 halt）', () => {
  const child = script([[0x9, []]]);
  const root = script([[0x3, [[0, 1]]], [0x1a8, []], [0x1a8, []]]);
  const m = new Machine({ body: root, table, key: KEY, loadScript: (id) => (id === 0 ? root : id === 1 ? child : null) });
  const r = m.run({ maxSteps: 4 });
  assert.equal(m.frames.length, 0, '0x9 之后不许还挂着帧');
  assert.equal(m.ip === 0 || r.stop.kind === 'step-limit', true, '根脚本从头开始（不是 12、不是 halted）');
  assert.equal(m.halted, false);
});

test('★ 0x3 深度上限：40 层时报错（F≥39 就报「階層が深すぎます」）', () => {
  const self = script([[0x3, [[0, 1]]]]);
  const m = new Machine({ body: self, table, key: KEY, loadScript: () => self });
  assert.throws(() => m.run({ maxSteps: 50 }), /call-script-too-deep/);
});
