/** @env pure @kind contract @why 帧机制塌了：子程序返回落到错的 PC（而"大部分脚本看起来都对"，只在嵌套/多次调用时错开） */
/**
 * tools/test/emulator-frames.test.mjs —— **帧与子程序返回**的契约
 *
 * ## 它守的是什么
 * 引擎有**两种**返回，作用域完全不同；把它们混起来是**静默**的错：
 * | 指令 | 作用域 | 机制 |
 * |---|---|---|
 * | `0x8f call` + `0x05 ret` | **同一帧内** | 每帧自己的返回栈（`帧+0x5EE04` 层数 / `帧+0x5EEA4` 返回表，256 槽） |
 * | `0x03 call-script` + `0x02 exit` | **跨帧** | 深度 ±1 + 记录 `+0x38`（绝对 `0x5D8CC`）caller 回链 |
 * ⇒ 把 `ret` 实现成"弹帧"（本仓**犯过**这个错，见缺陷单 `REQ-01M4AT41GQBN6D7QE0G349NH1R`）时，
 *   单层调用**看起来完全正常**，只有在**嵌套**或**同一帧多次调用**时才错开 —— 所以这条守卫的重点是**两层**。
 *
 * ## ★★ 第二件（2026-10 加）：帧**不是栈，是"槽数组 + `cur`"**
 * `0x6 load-frame`（`sub_41C7C0`）的逐字是**在任意槽 `op2` 上建帧记录、装完恢复旧 `cur`**：
 * `0x41C84E mov [esi+5D884h],ecx`（存旧 cur）· `0x41C854 mov [esi+5D880h],eax`（`cur = op2`）·
 * `0x41C89A call sub_40ED40`（建记录）· `0x41C8A6 mov [esi+5D880h],ecx`（**恢复** cur）。
 * 栈（`push`/`pop` 只动末端）表达不了这件事 —— 而且栈模型把"槽号"与"深度"绑死。
 * ⇒ 下面两条用例把"槽语义"钉住：**记录真的在槽 `op2` 上**（脚本名 / 局部池 / 头 6 个计数）·
 *   **`cur` 不变** · 快照带 **`cur` + 整条 `slots`（含空槽）**且 JSON 往返逐字节相同。
 *
 * 运行：`pnpm test`（纯函数：真的 `ScriptFrame` / 真的 `Machine` + 合成脚本，不需要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ScriptFrame, Machine, FRAME_FLAGS, STATE_PARTITION } from '../../apps/emulator/src/vm/machine.ts';
import { HANDLERS } from '../../apps/emulator/src/vm/ops.ts';
import { GlobalPools } from '../../apps/emulator/src/model/pools.ts';
import { Instance } from '../../apps/emulator/src/host/instance.ts';
import { resolveEnvironment } from '../../apps/emulator/src/host/environment.ts';
import { LayeredFilesystem, MemoryStore } from '../../apps/emulator/src/host/fs.ts';
import { EffectLog } from '../../apps/emulator/src/host/effects.ts';
import { VirtualClock } from '../../apps/emulator/src/host/clock.ts';
import { MemoryConfig } from '../../apps/emulator/src/host/config.ts';

/** 一个最小的脚本桩：label 的 `raw` → 字节偏移（`headerLen + raw*4`）→ 指令下标 */
const scriptStub = () => ({
  name: 'STUB.BIN',
  headerLen: 0,
  instructions: [{}, {}, {}, {}, {}, {}, {}],
  indexByByteOffset: new Map([[0, 0], [4, 1], [8, 2], [12, 3], [16, 4], [20, 5], [24, 6]]),
  notes: [],
});

/**
 * 最小的 machine 桩：真的**槽数组 + `cur`** + 真的全局池（操作数读取要用），其余只记账。
 * ★ 形状必须与 `Machine` 同形（`slots` / `cur` / `frame` = `slots[cur]`）—— 本仓踩过这一脚：
 *   桩自己长成"栈"而实现改了形状，于是"桩上绿"与"实现上对不对"无关。
 */
function machineStub() {
  const frame = new ScriptFrame('STUB.BIN', 0, 0, -1);
  const m = {
    globals: new GlobalPools(0),
    slots: [frame],
    cur: 0,
    diag: { stopReason: '', steps: 0, oobByKind: new Map() },
    notes: /** @type {string[]} */ ([]),
    effects: /** @type {unknown[]} */ ([]),
    get frame() { return this.slots[this.cur]; },
    get depth() { return this.cur + 1; },
    note(kind, detail) { this.notes.push(`${kind}${detail ? `: ${detail}` : ''}`); },
    effect(domain, action, disposition, detail) { this.effects.push({ domain, action, disposition, detail }); },
    pushFrame(name, caller, localCounts = []) {
      const slot = this.cur + 1;
      const f = new ScriptFrame(name, slot, 0, caller, null, localCounts);
      this.slots[slot] = f;
      this.cur = slot;
      return f;
    },
    popFrame() {
      const f = this.slots[this.cur];
      if (!f) throw new Error('没有活动帧：popFrame 没有可弹出的帧');
      this.slots[this.cur] = null;
      this.cur -= 1;
      return f;
    },
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
  const calls = m.depth;

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
  assert.equal(m.depth, calls, 'ret 不许换帧 —— 它与 call 的作用域是**同一帧**');
});

test('★ `ret` 空栈 ⇒ 什么都不做（引擎逐字：`cmp edx,-1 / jz locret` ⇒ 直接 retn）', () => {
  const m = machineStub();
  const f = m.frame;
  f.ip = 7;
  const before = m.depth;
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(f.ip, 7, 'PC 不动');
  assert.equal(m.depth, before, '帧数不动（既不是 exit、也不是崩）');
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
  assert.equal(m.depth, 1, 'call 不压帧');
  HANDLERS[0x05](ctxOf(m, [], 0x05));
  assert.equal(m.depth, 1, 'ret 不弹帧');
  // 跨帧那一对：exit 才是"回到调用层" —— 顶层（caller=-1）时是程序退出
  assert.equal(f.caller, -1, '顶层帧的 caller 是 −1（`exit` 据此判定程序退出）');
  assert.throws(() => HANDLERS[0x02](ctxOf(m, [], 0x02)), /顶层脚本 exit/, '顶层 exit = 程序退出信号，不是 no-op');

  // ★★ `exit`（非顶层）走 `Machine.popFrame()`：弹掉的是**槽 `cur`**、`cur` 减一，
  //    ⛔ 不许再有人裸改 `slots`（那会让 `popFrame` 变成零调用点的死方法 —— 本仓上一版就是这样）。
  const m2 = machineStub();
  const child = m2.pushFrame('CHILD.BIN', 0);
  assert.equal(m2.cur, 1, 'pushFrame 落在**下一个槽**（`slots[++cur]`）');
  assert.equal(child.cur, 1, '记录知道自己是第几号槽');
  HANDLERS[0x02](ctxOf(m2, [], 0x02));
  assert.equal(m2.cur, 0, '★ 非顶层 exit 必须把 cur 退回去');
  assert.equal(m2.slots[1], null, '★ 退掉的那一槽必须被显式清空（不是留下一条过期记录）');
  assert.equal(m2.frame.scriptName, 'STUB.BIN', '活动帧回到调用者');
});

test('★ 返回栈进快照（否则"恢复后重跑到第 N 帧"会在返回点上静默分叉）', () => {
  const m = machineStub();
  m.frame.ip = 0;
  HANDLERS[0x8f](ctxOf(m, [imm(2)], 0x8f));
  const snap = m.frame.snapshot();
  assert.deepEqual(Object.keys(snap).sort(), ['caller', 'cur', 'flags', 'ip', 'localCounts', 'locals', 'returnStack', 'scriptName', 'waitReason', 'waitSinceMs'].sort());
  assert.deepEqual(snap.returnStack, [1], '返回栈在快照里');
  // 标志位常量也在场（wait 门那一位）
  assert.equal(FRAME_FLAGS.WAIT_GATE, 0x400);
  assert.equal(typeof Machine, 'function');
});

// ─────────────────────────────────────────────── ★★ 槽语义（`0x6 load-frame`；2026-10）

/** 造一个**真 `Machine`**（不是假桩）：只注入最小宿主服务（不读盘、不碰语料） */
function realMachine() {
  const { env } = resolveEnvironment({
    instanceId: 'frames-guard',
    installRoot: { label: 'x', identity: 'x' },
    userRoot: { label: 'y', identity: 'y' },
  });
  const fsys = new LayeredFilesystem({
    label: 'x', sources: [new MemoryStore('src', 'x')], writable: new MemoryStore('w', 'y'),
  });
  return new Machine(new Instance({
    env, fs: fsys, effects: new EffectLog(), clock: new VirtualClock(0), config: new MemoryConfig(),
  }));
}

/**
 * 一份**合成**的 `LoadedScript`：`loadFrameAt` 只读 `name` 与头的 6 个 local 声明
 * ⇒ 不必造真字节（真字节那条路由 assets 档的启动链守卫跑，见 `emulator-headless-logo.assets.test.mjs`）。
 * ★ 6 个数是实测的 `DRAWTOOLTIP.BIN` 头（`localVarCounts` 的输出），按池序摆。
 */
const synthScript = (name, counts) => ({
  name,
  bytes: new Uint8Array(0),
  headerLen: 60,
  header: {
    fields: {
      local_integer_1: counts[0], local_floats: counts[1], local_strings_1: counts[2],
      local_integer_2: counts[3], unknown_data: counts[4], local_strings_2: counts[5],
    },
  },
  instructions: [],
  indexByByteOffset: new Map(),
  notes: [],
});

const DRAWTIP_COUNTS = [351, 1, 2, 6, 1, 3];

test('★★ `0x6 load-frame` 的槽语义：记录建在**指定槽 `op2`** 上，而 `cur` **一动都不动**', () => {
  const m = realMachine();
  const root = m.pushFrame('ROOT.BIN', -1, [1, 0, 0, 0, 0, 0]);
  assert.equal(m.cur, 0, '★ 根脚本落在槽 0（`pushFrame` = `slots[++cur]`）');
  assert.equal(m.slots[0], root);

  const script = synthScript('DRAWTOOLTIP.BIN', DRAWTIP_COUNTS);
  const curBefore = m.cur;
  const rec = m.loadFrameAt(26, script);

  // ① 记录真的在**槽 26** 上（⛔ 不是"活动链的末端" —— 栈模型下这里什么都不会有）
  assert.equal(m.slots[26], rec, '★ 槽 op2 上必须真的有帧记录');
  assert.equal(rec.cur, 26, '记录知道自己是第几号槽');
  assert.equal(rec.scriptName, 'DRAWTOOLTIP.BIN', '脚本名要对得上');
  assert.deepEqual([...rec.localCounts], DRAWTIP_COUNTS, '★ 头那 6 个 local 计数按**池序**落在记录上');
  assert.equal(rec.locals.key, root.locals.key, '同一把 codec key');
  assert.notEqual(rec.locals, root.locals, '★ 每个槽的记录有**自己**的 local 池（栈模型给不了第二个池）');

  // ② ★★ `cur` 没变 —— 这正是引擎"临时切 cur、装完切回来"的**净效果**
  assert.equal(m.cur, curBefore, '★★ load-frame 不许改 cur（改了就是"顺手把当前帧换掉"）');
  assert.equal(m.frame, root, '活动帧还是原来那一份');
  assert.equal(m.root, root, '槽 0 还是根脚本（`root` 不许被 26 号槽顶掉）');

  // ③ 槽是**一整条**数组：中间是显式 `null`（不是"洞"）—— 否则快照往返说不清
  assert.equal(m.slots.length, 27);
  for (let i = 1; i < 26; i += 1) assert.equal(m.slots[i], null, `槽 ${i} 必须是显式空槽`);

  // ④ 越界槽响亮失败（`op2 >= 40` 那条**引擎的**检查在 `ops.ts`；这里守的是模型数组的界）
  assert.throws(() => m.loadFrameAt(40, script), /不在 0\.\.39 之内/);
  assert.throws(() => m.loadFrameAt(-1, script), /不在 0\.\.39 之内/);
});

test('★★ 帧快照带 `cur` 与**整条 slots（含空槽）**，且 JSON 往返逐字节相同', () => {
  const m = realMachine();
  m.pushFrame('ROOT.BIN', -1, [1, 0, 0, 0, 0, 0]);
  m.loadFrameAt(26, synthScript('DRAWTOOLTIP.BIN', DRAWTIP_COUNTS));
  const snap = m.snapshot();
  // 顶层键 = 分区表里归 `engine` 的那几个（`emulator-state-partition` 守卫也核这条，两处会一起红）
  assert.deepEqual(Object.keys(snap).sort(), Object.keys(STATE_PARTITION.Machine).filter((k) => STATE_PARTITION.Machine[k] === 'engine').sort());
  assert.equal(snap.cur, 0, '★ 快照要带 `cur`');
  assert.equal(snap.slots.length, 27, '★ 快照要带**整条** `slots`（含空槽）');
  assert.equal(snap.slots[0].scriptName, 'ROOT.BIN');
  assert.equal(snap.slots[1], null, '空槽在快照里是显式 `null`');
  assert.equal(snap.slots[26].scriptName, 'DRAWTOOLTIP.BIN', '★ 不在活动链上的槽也要进快照（丢掉 = 恢复后预装记录凭空消失）');
  assert.deepEqual(snap.slots[26].localCounts, DRAWTIP_COUNTS, '槽里的计数一起进快照');
  // ★ 判据：JSON 往返**逐字节相同**（空槽要在往返里活下来）
  const text = JSON.stringify(snap);
  assert.equal(JSON.stringify(JSON.parse(text)), text, '★ JSON 往返必须逐字节相同');
  assert.deepEqual(JSON.parse(text).slots, snap.slots, '往返后逐槽相同');
});
