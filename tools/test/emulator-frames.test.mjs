/** @env pure @kind contract @why 帧机制塌了：子程序返回落到错的 PC（而"大部分脚本看起来都对"，只在嵌套/多次调用时错开） */
/**
 * tools/test/emulator-frames.test.mjs —— **帧与子程序返回**的契约
 *
 * ## 它守的是什么
 * 引擎有**两种**返回，作用域完全不同；把它们混起来是**静默**的错：
 * | 指令 | 作用域 | 机制 |
 * |---|---|---|
 * | `0x8f call` + `0x05 ret` | **同一帧内** | 每帧自己的返回栈（`帧+0x5EE04` 层数 / `帧+0x5EEA4` 返回表，256 槽） |
 * | `0x03 call-script` + `0x02 exit` | **跨帧** | 深度 ±1 + `帧+0x5D8CC` caller 回链 |
 * ⇒ 把 `ret` 实现成"弹帧"（本仓**犯过**这个错，见缺陷单 `REQ-01M4AT41GQBN6D7QE0G349NH1R`）时，
 *   单层调用**看起来完全正常**，只有在**嵌套**或**同一帧多次调用**时才错开 —— 所以这条守卫的重点是**两层**。
 *
 * 运行：`pnpm test`（纯函数：真的 `ScriptFrame` + 最小的 machine 桩，不需要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ScriptFrame, Machine, FRAME_FLAGS } from '../../apps/emulator/src/vm/machine.ts';
import { HANDLERS } from '../../apps/emulator/src/vm/ops.ts';
import { GlobalPools } from '../../apps/emulator/src/model/pools.ts';

/** 一个最小的脚本桩：label 的 `raw` → 字节偏移（`headerLen + raw*4`）→ 指令下标 */
const scriptStub = () => ({
  name: 'STUB.BIN',
  headerLen: 0,
  instructions: [{}, {}, {}, {}, {}, {}, {}],
  indexByByteOffset: new Map([[0, 0], [4, 1], [8, 2], [12, 3], [16, 4], [20, 5], [24, 6]]),
  notes: [],
});

/** 最小的 machine 桩：真的帧栈 + 真的全局池（操作数读取要用），其余只记账 */
function machineStub() {
  const frame = new ScriptFrame('STUB.BIN', 0, 0, -1);
  const m = {
    globals: new GlobalPools(0),
    frames: [frame],
    diag: { stopReason: '', steps: 0, oobByKind: new Map() },
    notes: /** @type {string[]} */ ([]),
    effects: /** @type {unknown[]} */ ([]),
    get frame() { return this.frames[this.frames.length - 1]; },
    note(kind, detail) { this.notes.push(`${kind}${detail ? `: ${detail}` : ''}`); },
    effect(domain, action, disposition, detail) { this.effects.push({ domain, action, disposition, detail }); },
    pushFrame(name, caller) { const f = new ScriptFrame(name, this.frames.length, 0, caller); this.frames.push(f); return f; },
    popFrame() { return this.frames.pop(); },
  };
  return m;
}

const ctxOf = (machine, args, opcode) => ({
  machine,
  frame: machine.frame,
  script: scriptStub(),
  ins: { opcode, name: '', argc: args.length, index: machine.frame.ip, args },
});
const imm = (v) => ({ type: 0, rawData: v >>> 0 });

test('★ 两层子程序返回：`call` 压「调用点的下一条」、`ret` 弹回（两种返回不许混）', () => {
  const m = machineStub();
  const f = m.frame;
  const calls = m.frames.length;

  // ip=0: call → label 4（字节偏移 16 ⇒ 下标 4）
  f.ip = 0;
  HANDLERS[0x8f](ctxOf(m, [imm(4)], 0x8f));
  assert.equal(f.returnStack.length, 1, 'call 必须压一个返回点');
  assert.equal(f.returnStack[0], 1, '压的是**调用点的下一条**（引擎压 `(PC−基址)>>2 + 3`，换算过来就是 ip+1）');
  assert.equal(f.ip, 4, 'call 把 PC 重定向到 label');

  // ip=4: 再 call → label 3（偏移 12 ⇒ 下标 3）—— 这就是"第二层"
  f.ip = 4;
  HANDLERS[0x8f](ctxOf(m, [imm(3)], 0x8f));
  assert.deepEqual(f.returnStack, [1, 5], '第二层压的是 5');
  assert.equal(f.ip, 3);

  // ip=3: ret ⇒ 回到 5
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(f.ip, 5, '第一层 ret 回到内层调用点的下一条');
  assert.deepEqual(f.returnStack, [1]);

  // ip=5: ret ⇒ 回到 1
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(f.ip, 1, '第二层 ret 回到外层调用点的下一条');
  assert.deepEqual(f.returnStack, [], '返回栈必须清空');

  // ★ `ret` **不换帧**（换成 `exit` 才会）
  assert.equal(m.frames.length, calls, 'ret 不许换帧 —— 它与 call 的作用域是**同一帧**');
});

test('★ `ret` 空栈 ⇒ 什么都不做（引擎逐字：`cmp edx,-1 / jz locret` ⇒ 直接 retn）', () => {
  const m = machineStub();
  const f = m.frame;
  f.ip = 7;
  const before = m.frames.length;
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(f.ip, 7, 'PC 不动');
  assert.equal(m.frames.length, before, '帧数不动（既不是 exit、也不是崩）');
  assert.ok(m.notes.some((n) => n.startsWith('ret-empty')), '空栈要留痕（"没得返"与"返到 0"必须分得开）');
});

test('★ 哨兵 `-1`：`jmp` / `call` 都**静默落下**（与引擎同形，不许抛）', () => {
  const m1 = machineStub();
  m1.frame.ip = 3;
  assert.doesNotThrow(() => HANDLERS[0x8c](ctxOf(m1, [imm(0xffffffff)], 0x8c)), 'jmp −1 不许抛');
  assert.equal(m1.frame.ip, 3, 'jmp −1 ⇒ PC 不动');
  assert.ok(m1.notes.some((n) => n.startsWith('jmp-no-label')));

  const m2 = machineStub();
  m2.frame.ip = 3;
  assert.doesNotThrow(() => HANDLERS[0x8f](ctxOf(m2, [imm(0xffffffff)], 0x8f)), 'call −1 不许抛');
  assert.equal(m2.frame.returnStack.length, 0, 'call −1 ⇒ **不压**（引擎是先压后撤，净效果等于不压）');
  assert.equal(m2.frame.ip, 3, 'call −1 ⇒ PC 不动');
});

test('★ `jcc`：条件非 0 落 op2、为 0 落 op3；目标为哨兵 ⇒ 该支落下', () => {
  // 非 0、op2 有目标 ⇒ 跳 op2
  const a = machineStub();
  a.frame.ip = 0;
  HANDLERS[0xa0](ctxOf(a, [imm(1), imm(5), imm(2)], 0xa0));
  assert.equal(a.frame.ip, 5, '条件非 0 ⇒ 跳 op2');

  // 为 0 ⇒ 跳 op3
  const b = machineStub();
  b.frame.ip = 0;
  HANDLERS[0xa0](ctxOf(b, [imm(0), imm(5), imm(2)], 0xa0));
  assert.equal(b.frame.ip, 2, '条件为 0 ⇒ 跳 op3');

  // 实测站点形态：op2 = 哨兵 ⇒ 非 0 时"落下"（这正是 SYSTEM4 上 show-logo 那条的形态）
  const c = machineStub();
  c.frame.ip = 4;
  HANDLERS[0xa0](ctxOf(c, [imm(1), imm(0xffffffff), imm(2)], 0xa0));
  assert.equal(c.frame.ip, 4, 'op2 是哨兵 ⇒ 非 0 时落下（PC 不动）');
});

test('★ `call` / `ret` 是**同帧**，`call-script` / `exit` 才是**跨帧** —— 四条的帧效果必须互不混淆', () => {
  const m = machineStub();
  assert.equal(typeof HANDLERS[0x8f], 'function');
  assert.equal(typeof HANDLERS[0x05], 'function');
  assert.equal(typeof HANDLERS[0x03], 'function');
  assert.equal(typeof HANDLERS[0x02], 'function');
  // 同帧那一对：只动 returnStack 与 ip
  const f = m.frame;
  f.ip = 0;
  HANDLERS[0x8f](ctxOf(m, [imm(2)], 0x8f));
  assert.equal(m.frames.length, 1, 'call 不压帧');
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(m.frames.length, 1, 'ret 不弹帧');
  // 跨帧那一对：exit 才是"回到调用层" —— 顶层（caller=-1）时是程序退出
  assert.equal(f.caller, -1, '顶层帧的 caller 是 −1（`exit` 据此判定程序退出）');
  assert.throws(() => HANDLERS[0x02](ctxOf(m, [], 0x02)), /顶层脚本 exit/, '顶层 exit = 程序退出信号，不是 no-op');
});

test('★ 返回栈进快照（否则"恢复后重跑到第 N 帧"会在返回点上静默分叉）', () => {
  const m = machineStub();
  m.frame.ip = 0;
  HANDLERS[0x8f](ctxOf(m, [imm(2)], 0x8f));
  const snap = m.frame.snapshot();
  assert.deepEqual(Object.keys(snap).sort(), ['caller', 'cur', 'flags', 'ip', 'locals', 'returnStack', 'scriptName', 'waitReason', 'waitSinceMs'].sort());
  assert.deepEqual(snap.returnStack, [1], '返回栈在快照里');
  // 标志位常量也在场（wait 门那一位）
  assert.equal(FRAME_FLAGS.WAIT_GATE, 0x400);
  assert.equal(typeof Machine, 'function');
});
