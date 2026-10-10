/** @env pure @kind contract @why 引擎标量堆或"读操作数→写标量"那一批塌了：值丢了或形态写错，而**没人会报错**（后面若有分支读同一个槽，会静默走错路径） */
/**
 * tools/test/emulator-engine-scalars.test.mjs —— **引擎标量堆**与启动链前段那一批的契约
 *
 * ## 它守的是什么（每条都对应一类**静默**坏结果）
 * 1. **默认 0**：引擎把 `Engine+0x5EC9C…0x5ECE8` 一片清零（逐字 `.lst:33974-33994`：连续 20 条
 *    `mov [esi+5EC9Ch..5ECE8h],edi`，`edi = 0`）⇒ 没写过的槽读出来必须是 **0**。
 *    ★ 与 int 池**不同**：池的初值是 `encZero`，**不是** 0。
 *    ★ 台账口径见 `data/ledger/`（subject `Engine+0x5EC9C..0x5ECE8/ctor-zero-fill`）。
 *    ✅ **欠账已补**（需求单 `REQ-01M4FEF7VH1N0JDT5ZVPZS204M`）：那条"回不了语料"的恒真断言，
 *      现在由 `tools/test/emulator-engine-scalars.assets.test.mjs` 承担 —— 它把那 20 条逐字
 *      （含"写的是 0"与"边界就是 0x5ECE8"）钉在 `.lst:33974-33993`（EA `0x415C0F..`）上。
 *      ★ 本文件这几条**保持不变**（纯用例仍不该去读语料）；两条的分工写在这里，免得下次又混。
 * 2. **写进去的读出来还是它**（u32 归一）—— 这一条塌了，前段那批 setter 就白写了。
 * 3. **`form` 要真的生效**：`bool(op1)` 必须**归一成 0/1**（不是原值）。
 * 4. **no-op 要真是 no-op**：`0x1a8` 除了协议写（本模型里自动）什么都不做。
 * 5. **模型引用的标量名必须来自知识层**：否则就是模型自己在编偏移（本仓硬口径）。
 * 6. **快照**：键升序 + 往返逐值相同 + 重复键必须抛（重复 = 静默丢一个槽）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { EngineScalars } from '../../apps/emulator/src/model/engine-scalars.ts';
import { HANDLERS } from '../../apps/emulator/src/vm/ops.ts';
import { ScriptFrame } from '../../apps/emulator/src/vm/machine.ts';
import { GlobalPools } from '../../apps/emulator/src/model/pools.ts';
import { AddressSpace } from '../../apps/emulator/src/model/address-space.ts';
import { ENGINE_SCALAR_BITS, ENGINE_SCALAR_WRITES } from '@amayui/age-format/src/engine/layout.mts';
import { MemoryConfig } from '../../apps/emulator/src/host/config.ts';
import { inlineString } from '../../apps/emulator/src/vm/script.ts';

/** 最小 machine 桩（只需要标量堆 + 记账；与 `emulator-frames.test.mjs` **同形**：槽数组 + `cur`） */
function machineStub() {
  const frame = new ScriptFrame('STUB.BIN', 0, 0, -1);
  return {
    globals: new GlobalPools(0),
    scalars: new EngineScalars(),
    instance: { config: new MemoryConfig(), env: { codecKey: 0 } },
    slots: [frame],
    cur: 0,
    diag: { stopReason: '', steps: 0, oobByKind: new Map() },
    notes: [],
    effects: [],
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
}
const ctxOf = (m, args, opcode) => ({
  machine: m, frame: m.frame,
  script: { name: 'STUB.BIN', headerLen: 0, instructions: [{}], indexByByteOffset: new Map([[0, 0]]) },
  ins: { opcode, name: '', argc: args.length, index: 0, args },
});
const imm = (v) => ({ type: 0, rawData: v >>> 0 });

test('★ 标量堆：默认 0（引擎把这片清零了）、写入读回同一 u32、位操作', () => {
  const s = new EngineScalars();
  assert.equal(s.read('Engine.d97058'), 0, '★ 没写过 ⇒ 0（与 int 池的 encZero **不同**）');
  assert.equal(s.size, 0, '读过不算写过（稀疏：只记被写过的）');

  s.write('Engine.d97058', 0xdeadbeef);
  assert.equal(s.read('Engine.d97058'), 0xdeadbeef);
  s.write('Engine.d97058', -1);
  assert.equal(s.read('Engine.d97058'), 0xffffffff, 'u32 归一（位模式只有一种表示）');

  s.write('Engine.x', 0x10);
  s.setBits('Engine.x', 0x1);
  assert.equal(s.read('Engine.x'), 0x11);
  s.clearBits('Engine.x', 0x1);
  assert.equal(s.read('Engine.x'), 0x10);
  // ★ 位操作作用在**默认值**上也必须成立（没写过 ⇒ 0 ⇒ setBits 得到 mask 本身）
  s.setBits('Engine.never', 0x8000000);
  assert.equal(s.read('Engine.never'), 0x8000000);
});

test('★ 快照：键升序、往返逐值相同、重复键/非 u32 必须抛（重复 = 静默丢一个槽）', () => {
  const s = new EngineScalars();
  s.write('Engine.b', 2);
  s.write('Engine.a', 1);
  const snap = s.snapshot();
  assert.deepEqual(snap.values.map(([k]) => k), ['Engine.a', 'Engine.b'], '键升序 ⇒ 同状态同字节');
  assert.deepEqual(EngineScalars.restore(snap).snapshot(), snap, '往返逐值相同');

  assert.throws(() => EngineScalars.restore({ values: [['x', 1], ['x', 2]] }), /键重复/, '重复键必须抛');
  assert.throws(() => EngineScalars.restore({ values: [['x', -1]] }), /不是 u32/);
  assert.throws(() => EngineScalars.restore({ values: [['', 1]] }), /非法键/);
  assert.throws(() => EngineScalars.restore({}), /不是数组/);
});

test('★ 前段那批：`form` 真的生效（`bool(op1)` 归一，不是原值）+ 写的槽与知识层逐条对上', () => {
  // 知识层说 0x21b 的形态是 bool(op1)
  const boolSlots = ENGINE_SCALAR_WRITES.filter((w) => w.opcode === 0x21b);
  assert.ok(boolSlots.length > 0, '知识层里必须有 0x21b 的登记（没有 ⇒ 这个守卫失去对象）');
  const m1 = machineStub();
  HANDLERS[0x21b](ctxOf(m1, [imm(7)], 0x21b));
  for (const w of boolSlots) {
    assert.equal(m1.scalars.read(w.name), 1, `${w.name}：非 0 输入必须**归一成 1**，不是原值 ${7}`);
  }

  // 形态是 op1 的那些：原值原样写进去
  const rawSlots = ENGINE_SCALAR_WRITES.filter((w) => w.opcode === 0x149 && w.form === 'op1');
  assert.ok(rawSlots.length > 0);
  const m2 = machineStub();
  HANDLERS[0x149](ctxOf(m2, [imm(7)], 0x149));
  for (const w of rawSlots) assert.equal(m2.scalars.read(w.name), 7, `${w.name}：形态 op1 必须写原值`);

  // ★ 每个"写标量"的 handler 写出来的**键集合**必须恰好等于知识层给它的那些名字
  //   （多写 = 模型在编偏移；少写 = 知识层的登记没落地）
  //   ★ 2026-10 补充：**条件副作用**（`ENGINE_SCALAR_BITS`）也算"知识层给的" —— 它们真的落状态了
  //   （此前是 `logged-only` ⇒ 位永远是 0）。下面按"这道门在这个操作数下成不成立"现算应写集合。
  const bitSlotsFor = (opcode, op1) => ENGINE_SCALAR_BITS
    .filter((b) => b.opcode === opcode)
    .filter((b) => (b.when === 'always' ? true : b.when === 'op1!=0' ? op1 !== 0 : op1 === 0))
    .map((b) => b.name);
  const byOpcode = new Map();
  for (const w of ENGINE_SCALAR_WRITES) {
    const list = byOpcode.get(w.opcode) ?? [];
    list.push(w.name);
    byOpcode.set(w.opcode, list);
  }
  for (const [opcode, names] of byOpcode) {
    const m = machineStub();
    // ★ `form: 'unsupported'` 的那几条**故意抛**（引擎明确不支持）⇒ 它们不该在这里被当成"写标量"
    if (ENGINE_SCALAR_WRITES.some((w) => w.opcode === opcode && w.form === 'unsupported')) {
      assert.throws(() => HANDLERS[opcode](ctxOf(m, [imm(3), imm(4)], opcode)), /引擎明确不支持/,
        `0x${opcode.toString(16)} 登记为"引擎不支持" ⇒ 必须抛`);
      assert.deepEqual(m.scalars.snapshot().values, [], '抛了就不该写任何标量');
      continue;
    }
    // ★ 给**两个**操作数：x1a4 是 orm: ''op2''（第二个操作数），只给一个会在桩里读到 undefined
    HANDLERS[opcode](ctxOf(m, [imm(3), imm(4)], opcode));
    const want = [...names, ...bitSlotsFor(opcode, 3)].sort();
    assert.deepEqual(m.scalars.snapshot().values.map(([k]) => k).sort(), want,
      `0x${opcode.toString(16)} 写的槽集合必须恰好是知识层登记的那些（含 ENGINE_SCALAR_BITS 里门成立的那些）`);
  }
});

test('★ `0x1a8` 是**真 no-op**：不碰任何标量、不动池、只留一条 note 都不该有', () => {
  const m = machineStub();
  const before = m.globals.snapshot();
  HANDLERS[0x1a8](ctxOf(m, [], 0x1a8));
  assert.deepEqual(m.scalars.snapshot(), { values: [] }, '不该写任何标量');
  assert.deepEqual(m.globals.snapshot(), before, '不该碰全局池');
  assert.deepEqual(m.effects, [], 'no-op 不该发副作用记录（那会把日志淹掉）');
});

/**
 * ★★ `0x101 poll-input`：**采样并丢弃已积累的输入，再标记"从现在起等输入"**
 * （逐字 `.lst:38259-38266`：`call sub_478090`（采样）→ `and [esi+0AAB44h],0F7FFFFFFh`（先清 bit27
 * 那个"未消费输入"闩锁）→ `mov [edi],0`（再清输入掩码）→ 两个"等输入"字段置 1/0）。
 *
 * ## 这条守卫守的三件事（每一条都对应一种**写反了也看不出来**的实现）
 * 1. **调了一次**（`pollCalls === 1`）：不许"每次问一点"（那会让一次 `poll-input` 吃掉多帧的输入）。
 * 2. **采到的值被丢弃**（`maskDiscarded`/`latchCleared`）：写反成"读一次输入并保存"的实现
 *    —— 即"这次采样留下、以后接着用" —— 会与这两个字段直接冲突。
 * 3. ★ **不许在地址空间里留下这次采样**（`space.snapshot()` 逐值不变）：这一条是**真判据** ——
 *    "保存采样"最自然的落点就是往 `Engine+0xAAB48`（输入掩码）那类格子写一个 u32；
 *    本模型没有那个槽，所以"存起来"这件事一旦写进地址空间，快照当场不同。
 *    ⚠ **已知欠账（不掩盖）**：`maskDiscarded`/`latchCleared` 是**本模型自己的措辞**（效应记录字段），
 *    它们本身是"模型说它丢了" —— 一个把采样留在**别处**（模型外的宿主状态）的实现抓不住。
 *    要真抓住那一类，得先把 `Engine+0xAAB44` / `+0xAAB48` / `+0x777FC` / `+0x77808` 建模
 *    （那是新能力，不在本轮范围）。
 */
test('★ `0x101 poll-input`：采样一次后**丢弃**（不留在任何状态里）、只发一条 modeled 记录', () => {
  const m = machineStub();
  m.space = new AddressSpace();
  let pollCalls = 0;
  m.instance.input = {
    label: 'stub-input',
    poll() { pollCalls += 1; return { buttons: 0x10, wheel: -1, mouseX: 11, mouseY: 22 }; },
  };
  const before = m.space.snapshot();
  HANDLERS[0x101](ctxOf(m, [], 0x101));

  assert.equal(pollCalls, 1, '★ 一次 `poll-input` 只许采样一次（多采 = 会吃掉后面几帧的输入）');
  assert.deepEqual(m.space.snapshot(), before, '★ 采样结果必须**被丢弃** —— 不许留在地址空间的任何格子里');
  assert.deepEqual(m.scalars.snapshot(), { values: [] }, '也不许顺手写标量堆');
  assert.equal(m.effects.length, 1, '只发一条效应记录');
  const e = m.effects[0];
  assert.equal(e.domain, 'input');
  assert.equal(e.action, 'poll');
  assert.equal(e.disposition, 'modeled', '宿主提供了输入源 ⇒ 不是 not-provided');
  assert.equal(e.detail.maskDiscarded, true, '★ 语义是"把已积累的输入丢掉"（⛔ 不是"这次采样留下"）');
  assert.equal(e.detail.latchCleared, true, '★ 未消费输入闩锁（bit27）也必须被清');
});

/**
 * ★★ `0xa0 jcc`：`op1 != 0` ⇒ 跳 `op2`；`op1 == 0` ⇒ 跳 `op3`；**任一支的 `0xFFFFFFFF` = 该支没有目标（落下）**。
 *
 * ## 逐字（`sub_4209B0` @ `0x4209B0`，`.lst:48686-48716`）—— 三处**独立互证**
 * ```
 *   .text:004209CB push 1        / call sub_41BF50   ; 读 op1
 *   .text:004209D6 test eax,eax  / jz loc_4209EA     ; ← ① op1 == 0 走另一支
 *   .text:004209DA push 2        / call sub_41BF50   ; op1 != 0 ⇒ 读 op2
 *   .text:004209E1 cmp eax,0FFFFFFFFh / jz loc_420A42; ← ② 哨兵 ⇒ **不跳**、直接 retn
 *   .text:004209EA loc_4209EA:  push 3 / call sub_41BF50 ; ← ③ op1 == 0 时读 op3（同一条哨兵判据）
 * ```
 * ⇒ 三种形状（非 0 跳 op2 / 为 0 跳 op3 / 哨兵落下）都要**可观测**：
 *    本模型里"跳了"体现为 `frame.ip` 被改写（改过 ⇒ `Machine.step()` 不再 +1），
 *    "落下"体现为 `frame.ip` 不动 + 一条 `jcc-fallthrough` note（"关掉的分支"与"我们算错了 label"必须分得开）。
 */
test('★ `0xa0 jcc`：非 0 跳 op2 / 为 0 跳 op3 / 哨兵**落下**（三支都要可观测）', () => {
  // 一份最小脚本：4 条指令，label 的 raw 就是「(绝对偏移 − headerLen) / 4」
  const mkScript = () => ({
    name: 'JCC.BIN', headerLen: 60, bytes: new Uint8Array(256),
    instructions: [{ byteOffset: 60 }, { byteOffset: 64 }, { byteOffset: 68 }, { byteOffset: 72 }],
    indexByByteOffset: new Map([[60, 0], [64, 1], [68, 2], [72, 3]]),
  });
  const call = (cond, trueLabel, falseLabel) => {
    const m = machineStub();
    const frame = { ip: 1, locals: m.frame.locals };
    const script = mkScript();
    const ins = {
      opcode: 0xa0, name: 'jcc', argc: 3, index: 1,
      args: [imm(cond), imm(trueLabel), imm(falseLabel)],
    };
    HANDLERS[0xa0]({ machine: m, frame, script, ins });
    return { frame, notes: m.notes };
  };

  // ① 条件非 0 ⇒ 跳 **op2**（op3 完全不该被看一眼）
  assert.equal(call(7, 2, 3).frame.ip, 2, '★ 条件非 0 ⇒ 跳 op2');
  // ② 条件为 0 ⇒ 跳 **op3**（★ 这一支写反了会让每条 `if` 都走反，而"程序还在跑"）
  assert.equal(call(0, 2, 3).frame.ip, 3, '★ 条件为 0 ⇒ 跳 op3');
  // ③ 哨兵 `0xFFFFFFFF`（= 那一支"没有目标"）⇒ **落下**（ip 不动）且**留一笔**
  const fallTrue = call(7, 0xffffffff, 3);
  assert.equal(fallTrue.frame.ip, 1, '★ 非 0 支的哨兵 ⇒ 不跳（落下），且 ip 不许被改写');
  assert.ok(fallTrue.notes.some((n) => n.startsWith('jcc-fallthrough')), '★ 落下必须留一笔（与"算错 label"分得开）');
  const fallFalse = call(0, 2, 0xffffffff);
  assert.equal(fallFalse.frame.ip, 1, '★ 为 0 支的哨兵 ⇒ 同样落下');
  assert.ok(fallFalse.notes.some((n) => n.startsWith('jcc-fallthrough')), '为 0 支落下也要留一笔');
  // ④ 非哨兵但**指向一条指令中间** ⇒ 响亮失败（结构性错误，不许猜"最近的那条"）
  //    label raw = 15 ⇒ 绝对偏移 60 + 60 = 120，不在 `indexByByteOffset` 里（那是"指令中间"）
  assert.throws(() => call(7, 15, 3), /不是指令起点/, 'label 落在指令中间必须抛');
});

test('★ `0x2de`：在**宿主字体表**里查名字 ⇒ 下标；未命中 ⇒ **-1**；跳过前导 `@`；没给表 ⇒ 响亮失败', () => {
  const withFonts = (fonts) => {
    const m = machineStub();
    m.instance.fonts = fonts;
    return m;
  };
  const arg = (t, raw) => ({ type: t, rawData: raw });
  const local = { type: 0x9, rawData: 0 };

  // 命中 ⇒ **0 基下标**（引擎逐字 `mov eax,[ebp+var_4]`）
  // ★ 用"往全局串池写一个名字再查"的方式造真实键（走的是同一条**文本**读路径 `readOperandAsText`）。
  const m2 = withFonts(['游ゴシック']);
  m2.globals.write('string', 3003, '游ゴシック');
  HANDLERS[0x2de](ctxOf(m2, [local, arg(5, 3003)], 0x2de));
  const hit = m2.effects.find((e) => e.action === 'font.lookup');
  assert.equal(hit.detail.key, '游ゴシック');
  assert.equal(hit.detail.index, 0, '命中 ⇒ 0 基下标');
  assert.equal(m2.frame.locals.read(9, 0), 0, '结果写回 op1');

  // 未命中 ⇒ -1（⛔ 不是 0）
  const m3 = withFonts(['别的字体']);
  m3.globals.write('string', 3003, '游ゴシック');
  HANDLERS[0x2de](ctxOf(m3, [local, arg(5, 3003)], 0x2de));
  const miss = m3.effects.find((e) => e.action === 'font.lookup');
  assert.equal(miss.detail.index, -1, '★ 未命中 ⇒ -1（引擎逐字 `or eax,-1`）');
  assert.equal(m3.frame.locals.read(9, 0), 0xffffffff, '写回去的是 0xFFFFFFFF（i32 的 -1）');
  assert.ok(String(miss.detail.note).includes('表来自宿主'), '必须写明"表来自宿主"（否则读者会以为 -1 是引擎语义）');

  // 跳过前导 '@'（引擎逐字 `cmp byte ptr [eax],40h / setz cl / add eax,ecx`）
  const m4 = withFonts(['游ゴシック']);
  m4.globals.write('string', 3003, '@游ゴシック');
  HANDLERS[0x2de](ctxOf(m4, [local, arg(5, 3003)], 0x2de));
  assert.equal(m4.effects.find((e) => e.action === 'font.lookup').detail.index, 0, "键首字节是 '@' ⇒ 跳过它再比");

  // ★ 没给表（null）⇒ **响亮失败**（"没能力"与"表是空的"必须分开）
  const m5 = machineStub();
  assert.throws(() => HANDLERS[0x2de](ctxOf(m5, [local, arg(5, 3003)], 0x2de)), /没有提供 `fonts`/);
});

test('★ 配置的字符串两条：`save-string` → `load-string` 往返；**查不到 ⇒ 空串**（不是 0）', () => {
  const m = machineStub();
  const cell = { type: 0x5, rawData: 3003 }; // 全局串池第 3003 格
  // 先往那一格放一个字符串（用 set-string 的写路径：mov 不行，它只处理数值 ⇒ 直接写池）
  m.globals.write('string', 3003, '游ゴシック');
  HANDLERS[0x1a9](ctxOf(m, [cell], 0x1a9)); // save-string
  const w = m.effects.find((e) => e.detail?.opcode === '0x1a9');
  assert.equal(w.detail.key, '\\x0500000bbb', '键 = 类型码 5 的字节 + 8 位十六进制下标');

  // 清掉那一格，再 load-string 读回来（往返）
  m.globals.write('string', 3003, '');
  HANDLERS[0x1aa](ctxOf(m, [cell], 0x1aa));
  assert.equal(m.globals.read('string', 3003), '游ゴシック', '往返：读回来的就是存进去的');
  const r = m.effects.filter((e) => e.action === 'config.read').pop();
  assert.equal(r.detail.value, '游ゴシック');
  assert.equal(r.detail.stored, '游ゴシック', '"有键且值是它" 与 "查不到" 必须能分开');

  // ★ 查不到 ⇒ **空串**（引擎返回一个静态空串的地址，不是 0）
  const m2 = machineStub();
  HANDLERS[0x1aa](ctxOf(m2, [{ type: 0x5, rawData: 9 }, ], 0x1aa));
  const r2 = m2.effects.find((e) => e.action === 'config.read');
  assert.equal(r2.detail.stored, null, '记录要能看出"没有这个键"');
  assert.equal(r2.detail.value, '', '★ 查不到 ⇒ 空串，**不是** 0 也不是 "0"');
  assert.equal(m2.globals.read('string', 9), '', '写回格子的也是空串');
});

test('★ 数组填充两条：`fill-zero` 填的**不是 0**（填 `encZero`）、`set-array-to` 填 `ENC(key,v)`', () => {
  // 键为 0 时 encZero = 0、ENC(v) = v ⇒ 值面上看不出差别；★ 所以这条守卫**同时**断言"记的是 encZero 这件事"
  const m = machineStub();
  const arg = { type: 0x3, rawData: 100 }; // 全局 int 池，第 100 格起
  HANDLERS[0x6c](ctxOf(m, [arg, imm(4)], 0x6c));
  const fx = m.effects.find((e) => e.action === 'array.fill');
  assert.ok(fx, '填数组要留痕');
  assert.equal(fx.detail.count, 4);
  assert.ok(String(fx.detail.filled).includes('encZero'), '★ 记录里必须写明填的是 encZero（助记名骗人，日志不能跟着骗）');
  assert.ok(String(fx.detail.note).includes('不是字面 0'), 'note 要显式点出"不是 0"');
  for (let k = 100; k < 104; k += 1) assert.equal(m.globals.read('int', k), 0, 'int 池里 DEC(encZero) = 0');

  // set-array-to：int 池里写的是 op2 的值（DEC(ENC(v)) = v）
  const m2 = machineStub();
  HANDLERS[0x2d8](ctxOf(m2, [{ type: 0x3, rawData: 200 }, imm(7), imm(3)], 0x2d8));
  for (let k = 200; k < 203; k += 1) assert.equal(m2.globals.read('int', k), 7, 'int 池写 op2 的值');
  // 个数 <= 0 ⇒ 什么都不做（引擎：`if (result > 0)`）
  const m3 = machineStub();
  HANDLERS[0x6c](ctxOf(m3, [{ type: 0x3, rawData: 300 }, imm(0)], 0x6c));
  assert.equal(m3.globals.read('int', 300), null, 'count = 0 ⇒ 一格都不写');
});

test('★ type 2（内联字符串）的读法：文件偏移 = headerLen + 4*raw、^0xFF、cp932、到 0 止', () => {
  // 用**真语料里那一串字节**：`INITCONFIG0.BIN` 偏移 416（= 60 + 4*89）起的「メイリオ」原始字节。
  // 合成一份脚本（只用到 headerLen 与 bytes）=⇒ 纯函数，不需要游戏安装。
  const realBytes = [0x7c, 0x7e, 0x7c, 0xbc, 0x7c, 0x75, 0x7c, 0xb6, 0xff, 0xff, 0xff, 0xff];
  const headerLen = 60;
  const bytes = new Uint8Array(headerLen + 4 * 89 + realBytes.length);
  bytes.set(realBytes, headerLen + 4 * 89);
  const script = { name: 'SYNTH.BIN', headerLen, bytes };

  assert.equal(inlineString(script, 89), 'メイリオ', '真字节 ⇒ cp932 解出「メイリオ」');
  // ★ 偏移口径：headerLen 挪 4 字节 ⇒ 起点偏一格 ⇒ 必须解出别的东西（说明公式真的用了 headerLen 与 4*raw）
  assert.notEqual(inlineString({ ...script, headerLen: 64 }, 89), 'メイリオ', 'headerLen 变了 ⇒ 落点必须跟着变');
  // ★ 没有终止符（`^0xFF` 后为 0）⇒ 抛，而不是一路读到脚本末尾当成一个长字符串
  assert.throws(
    () => inlineString({ name: 'X', headerLen: 0, bytes: Uint8Array.from([0x00, 0x01]) }, 0),
    /读到脚本末尾/,
    '找不到终止符必须响亮失败（地址算错时最容易出这种）',
  );
});

test('★ 对象字段写那一族：**只记欠账**（`applied:false` + 原因），⛔ 不许记成"写成功了"', () => {
  // 0x212：一个字段（+100 ← op2）
  const m1 = machineStub();
  HANDLERS[0x212](ctxOf(m1, [imm(1), imm(0x1234)], 0x212));
  const e1 = m1.effects.find((e) => e.action === 'engine.object-field-write');
  assert.ok(e1, '必须记录这次尝试');
  assert.equal(e1.disposition, 'logged-only', '★ 未建模 ⇒ `logged-only`');
  assert.equal(e1.detail.applied, false, '★★ 对象不在场 ⇒ **写没有发生**（记成 true 就是假装）');
  assert.ok(String(e1.detail.reason).includes('未建模'), '原因要写清楚（否则日志读者会以为写成功了）');
  assert.deepEqual(e1.detail.writes, [{ field: '+100', value: 0x1234 }], '字段偏移与取值要记账');

  // 0x25d：两个字段（+276 ← op2、+280 ← op3）—— 偏移与操作数的**对应**必须钉住
  const m2 = machineStub();
  HANDLERS[0x25d](ctxOf(m2, [imm(2), imm(0xaa), imm(0xbb)], 0x25d));
  const e2 = m2.effects.find((e) => e.action === 'engine.object-field-write');
  assert.deepEqual(e2.detail.writes, [{ field: '+276', value: 0xaa }, { field: '+280', value: 0xbb }],
    'op2 → +276、op3 → +280（顺序照体，不许对调）');
  assert.equal(e2.detail.applied, false);
});

test('★ 配置四条里的整型两条：键是**运行时拼**的（`\\x03` + 8 位十六进制）、查不到 ⇒ 0、写回是 lvalue', () => {
  const m = machineStub();
  // 键的形态：1234 ⇒ \x03 + 000004d2（%8.8x）
  HANDLERS[0x1a2](ctxOf(m, [imm(1234)], 0x1a2));
  const key = '\u0003000004d2';
  assert.equal(m.instance.config.get(key), '1234', '键必须是 类型码字节 + 8 位十六进制（%8.8x）');

  // load-int：**键取自操作数当时的值** ⇒ 先把这个局部格设成 1234（用已实现的 `mov`，走同一条操作数写路径），
  // 再 load-int 它：应当查得到并把值写回同一格。
  const m2 = machineStub();
  m2.instance.config.set(key, '1234');
  const local = { type: 0x9, rawData: 0 };
  HANDLERS[0x55]({ ...ctxOf(m2, [local, imm(1234)], 0x55) }); // mov local0, 1234
  HANDLERS[0x1a3]({ ...ctxOf(m2, [local], 0x1a3) });
  const fx = m2.effects.find((e) => e.action === 'config.read');
  assert.ok(fx, '读配置要留痕');
  assert.equal(fx.detail.key, '\\x03000004d2', '★ 键 = 类型码字节 + 8 位十六进制（`%8.8x`，零填充）');
  assert.equal(fx.detail.value, 1234, '读回来的值');
  assert.ok(String(fx.detail.note).includes('sub_418A30'), '★ 键的取值原语欠账必须写在记录里（不静默）');

  // 查不到 ⇒ 0（引擎逐字：`v4 = v3 ? *v3 : 0`）
  const m3 = machineStub();
  HANDLERS[0x1a3]({ ...ctxOf(m3, [{ type: 0x9, rawData: 1 }], 0x1a3) });
  const fx3 = m3.effects.find((e) => e.action === 'config.read');
  assert.equal(fx3.detail.value, 0, '查不到 ⇒ 0');
  assert.equal(fx3.detail.stored, null, '而且记录要能区分"没有这个键"与"键的值是 0"');
});

test('★ `0x76`/`0x77`：写的是 **bswap24(op1)**（不是原值），且**必须**把 `sub_459F40` 记进欠账', () => {
  const m = machineStub();
  HANDLERS[0x76](ctxOf(m, [imm(0x123456)], 0x76));
  // ★ 形态：低 24 位内字节序倒过来（`b0<<16 | b1<<8 | b2`）—— 写成原值就是"颜色通道反了"而没人报错
  assert.equal(m.scalars.read('Engine.d21664'), 0x563412, 'bswap24(0x123456) = 0x563412');
  HANDLERS[0x77](ctxOf(m, [imm(0xabcdef)], 0x77));
  assert.equal(m.scalars.read('Engine.d21665'), 0xefcdab, 'bswap24(0xabcdef) = 0xefcdab');
  // ★★ `callsAfter`：写完标量之后那次子系统调用**必须**出现在 `engine.forward` 里
  //    —— 少了它，保真欠账会少算，日志显得比实际干净（本仓第一版就是这样漏的）。
  const fwd = m.effects.filter((e) => e.action === 'engine.forward');
  assert.equal(fwd.length, 2, '每条都要记一次未建模的子系统调用');
  assert.ok(fwd.every((e) => e.detail.callee === 'sub_459F40'), '被调符号要写清');
  assert.ok(fwd.every((e) => e.disposition === 'logged-only'), '未建模 ⇒ logged-only');
  assert.ok(fwd.every((e) => e.detail.transfer === 'call'), '★ `0x76`/`0x77` 是 `call`（会返回），不是尾跳');
});

/**
 * ★★ `0x78`/`0x2db` 是**尾跳**（逐字 `jmp sub_459F40`：`.text:0041F47F` / `.text:0042652F`），
 * 而 `0x76`/`0x77` 是 `call`（`.text:0041F3DB`）—— 两者在 `[保真欠账]` 里**必须分得开**。
 *
 * ## 为什么这不是格式洁癖
 * 原先四条 `callsAfter:['sub_459F40']` **长得完全一样** ⇒ 读日志的人分不出"调完会回来接着跑"
 * 与"控制流交给它、本 handler 到此结束"；后者意味着**后面若还有代码，在引擎里根本不会执行**。
 * ★ 实参也必须记：这里 callee 的实参是**子对象槽** `Engine+0x14D30`（`Engine.d21324`），
 *   ⛔ 不是 op1 —— 记成 op1 会把"往那个子对象里写"伪装成"写本次操作数"。
 */
test('★ `0x78` 的欠账看得出是**尾跳**（`transfer: tail`）、`0x76` 是 `call`；实参是子对象槽不是 op1', () => {
  const detailOf = (opcode, args) => {
    const m = machineStub();
    HANDLERS[opcode](ctxOf(m, args, opcode));
    return m.effects.find((e) => e.action === 'engine.forward').detail;
  };
  const tail = detailOf(0x78, [imm(5)]);
  const call = detailOf(0x76, [imm(5)]);
  assert.equal(tail.transfer, 'tail', '★ `0x78` 的体尾是 `jmp sub_459F40`（尾跳，不回来）');
  assert.equal(call.transfer, 'call', '★ `0x76` 的体尾是 `call sub_459F40`（会回来）');
  assert.notEqual(tail.transfer, call.transfer, '★ 两者在欠账里必须分得开（这就是这条用例的全部意义）');
  assert.ok(String(tail.note).includes('尾跳'), 'note 里也要写明（日志读者不必去翻知识层）');
  // ★ 实参：callee 的接收者是子对象槽（没写过 ⇒ 0），**不是** op1 的 5
  assert.deepEqual(tail.argForms, ['Engine.d21324'], '实参形态 = 子对象槽');
  assert.deepEqual(tail.args, [0], '★ 记的是**槽的值**，不是 op1（op1 = 5）');
  assert.deepEqual(call.args, [0]);
  // ★ 2db 与 78 同形（尾跳）
  assert.equal(detailOf(0x2db, [imm(5)]).transfer, 'tail');
  assert.equal(detailOf(0x77, [imm(5)]).transfer, 'call');
});

/**
 * ★★ 转发的**完整实参**：`0x70` 的 callee 收 6 个显式实参，最后一个是**写死的 `0`**；
 * `0x71` 的第 3 个实参是**引擎槽** `Engine.d97055`。
 *
 * 逐字（锚 = EA）：
 * ```
 *   0x41ED30 push 0                 ;; 0x70：最先压 ⇒ 最深的那个实参（callee 的 a7）
 *   0x41ED75 call sub_45D660        ;; 栈上从顶到底 = op1..op5, 0
 *   0x41ED80 sub_41ED80             ;; 0x71：sub_45EC60(Engine+0x14D30, op1, Engine[97055])
 * ```
 * ⇒ callee `sub_45D660` 里 `if (a7 >= 0)` 那条门**恒真**：原先本层只记 5 个操作数，这条分支
 *   在欠账里**完全看不见** ⇒ "永远不会走"与"走了但没记录"变成同一种表现。
 */
test('★ `0x70` 的**常量实参 0** 与接收者槽、`0x71` 的**引擎槽实参**都必须记进欠账（`args` + `argForms`）', () => {
  // ① 0x70：接收者槽 + op1..op5 + 写死的 0
  const m = machineStub();
  HANDLERS[0x70](ctxOf(m, [imm(11), imm(12), imm(13), imm(14), imm(15)], 0x70));
  const d = m.effects.find((e) => e.action === 'engine.forward').detail;
  assert.equal(d.callee, 'sub_45D660');
  assert.equal(d.transfer, 'call', '`0x70` 的 handler 是 `call sub_45D660; … retn`（会回来）');
  assert.deepEqual(d.argForms,
    ['Engine.d21324', 'operand#0', 'operand#1', 'operand#2', 'operand#3', 'operand#4', 'const 0'],
    '★ 每个位置的**形态**都要记（数值里分不出"操作数 0"与"常量 0"）');
  assert.deepEqual(d.args, [0, 11, 12, 13, 14, 15, 0],
    '★ 最后那个 **0 是 handler 写死的**（callee 的 a7）—— 它是 `if (a7 >= 0)` 恒真的原因');
  assert.equal(d.argForms[d.argForms.length - 1], 'const 0', '★ 常量必须落在**最后一个**位置');
  assert.ok(String(d.note).includes('第 6 个显式实参'), 'note 要写明它收 6 个显式实参（否则读者不知道多了什么）');

  // ② 0x71：第 3 个实参是**引擎槽**（值随槽变 —— 这才是"读的是槽不是操作数"的判据）
  const m2 = machineStub();
  m2.scalars.write('Engine.d97055', 0x80000000);
  HANDLERS[0x71](ctxOf(m2, [imm(3)], 0x71));
  const d2 = m2.effects.find((e) => e.action === 'engine.forward').detail;
  assert.deepEqual(d2.argForms, ['Engine.d21324', 'operand#0', 'Engine.d97055'], '实参形态 = 接收者槽 + op1 + 门槽');
  assert.deepEqual(d2.args, [0, 3, 0x80000000], '★ 槽的**当前值**也要记（门就是它）');
  // ★ 槽没写过 ⇒ 0（同一条记录会因此不同 ⇒ 证明记的是槽而不是某个常量）
  const m3 = machineStub();
  HANDLERS[0x71](ctxOf(m3, [imm(3)], 0x71));
  assert.deepEqual(m3.effects.find((e) => e.action === 'engine.forward').detail.args, [0, 3, 0],
    '★ 换成没写过的槽值 ⇒ 记录跟着变（常量就不会变）');

  // ③ 没取证 `callArgs` 的那些（例 0x2f6）：仍按"只有操作数"记（口径：少记 ≠ callee 只收这些）
  const m4 = machineStub();
  HANDLERS[0x2f6](ctxOf(m4, [imm(2)], 0x2f6));
  const d4 = m4.effects.find((e) => e.action === 'engine.forward').detail;
  assert.deepEqual(d4.args, [2]);
  assert.deepEqual(d4.argForms, ['operand#0'], '默认形态 = 操作数（⛔ 不猜常量/槽）');
});

test('★ 标量**数组**族：`0x107` 的索引来自 op1、`0x10b` 的来自 op2（角色互换不许合并实现）', () => {
  const m = machineStub();
  // `0x107`：`Engine[551 + op1] = op2`
  HANDLERS[0x107](ctxOf(m, [imm(3), imm(77)], 0x107));
  assert.equal(m.scalars.read('Engine.d551+3'), 77, '★ 索引必须进键名（否则两次不同的写会互相覆盖）');
  HANDLERS[0x107](ctxOf(m, [imm(4), imm(88)], 0x107));
  assert.equal(m.scalars.read('Engine.d551+4'), 88);
  assert.equal(m.scalars.read('Engine.d551+3'), 77, '两次写互不干扰');
  // 越界 ⇒ 引擎**静默跳过**（不抛），但必须留一笔
  const m2 = machineStub();
  HANDLERS[0x107](ctxOf(m2, [imm(0x20), imm(5)], 0x107));
  assert.deepEqual(m2.scalars.snapshot().values, [], '越界不写');
  assert.ok(m2.notes.some((n) => n.startsWith('scalar-array-skip')), '★ 跳过要可见（不许静默丢弃一次写）');

  // `0x10b`：**角色互换** —— 索引 = op2、值 = op1，表基址 1383
  const m3 = machineStub();
  HANDLERS[0x10b](ctxOf(m3, [imm(9), imm(5)], 0x10b));
  assert.equal(m3.scalars.read('Engine.d1383+5'), 9, '★ 索引来自 op2、值来自 op1');
});

test('★ 派发表**没登记**的 opcode：报"引擎明确不支持"，⛔ 不许与"本批未实现"混为一谈', () => {
  const m = machineStub();
  for (const op of [0x110, 0x111, 0x112]) {
    assert.throws(() => HANDLERS[op](ctxOf(m, [imm(1)], op)), /引擎明确不支持/,
      `0x${op.toString(16)}：派发表没有登记 ⇒ 默认 handler 抛「此命令不支持」—— 走到这里说明**分支走错了**`);
    assert.throws(() => HANDLERS[op](ctxOf(m, [imm(1)], op)), /不是"本批未实现"/, '两种处境必须能分辨');
  }
});

test('★ `0x6 load-frame` 的**帧深上限 40**（越界抛，错误信息照抄引擎那句日文）', () => {
  const m = machineStub();
  // ★ 逐字 `if (v4 >= 40) throw ShowMessage("ファイルの階層が深すぎます．最大は%dです．", 40);`
  assert.throws(() => HANDLERS[0x6](ctxOf(m, [imm(20936), imm(40)], 0x6)), /階層が深すぎます/);
  assert.throws(() => HANDLERS[0x6](ctxOf(m, [imm(20936), imm(999)], 0x6)), /階層が深すぎます/);
  // 39 是允许的（边界）：它会走到 `loadScriptById` —— 桩机没有那个方法，所以这里只断言**不是**深度错误
  assert.throws(
    () => HANDLERS[0x6](ctxOf(m, [imm(20936), imm(39)], 0x6)),
    (e) => !/階層が深すぎます/.test(String(e?.message ?? e)),
    '39 < 40 ⇒ 不该报帧深',
  );
});

test('★ `0x30a` 与 `0x107` 的**越界口径不同**（一个是抛、一个是静默跳过）—— 不许统一', () => {
  const m = machineStub();
  // `0x30a`：表 1969、索引来自 op2（上界 **7**）、值来自 op1（上界 0x1F）
  HANDLERS[0x30a](ctxOf(m, [imm(5), imm(6)], 0x30a));
  assert.equal(m.scalars.read('Engine.d1969+6'), 5, '索引来自 op2、值来自 op1');
  assert.throws(() => HANDLERS[0x30a](ctxOf(m, [imm(5), imm(8)], 0x30a)), /引擎这里抛异常/, '索引 > 7 ⇒ 抛');
  assert.throws(() => HANDLERS[0x30a](ctxOf(m, [imm(0x20), imm(0)], 0x30a)), /引擎这里抛异常/, '值 > 0x1F ⇒ 抛');
  // 对照：`0x107` 同样越界**不抛**（逐字 `if (result <= 0x1F)`，没有异常）
  const m2 = machineStub();
  assert.doesNotThrow(() => HANDLERS[0x107](ctxOf(m2, [imm(0x20), imm(1)], 0x107)), '0x107 越界**不抛**');
  assert.ok(m2.notes.some((n) => n.startsWith('scalar-array-skip')), '但必须留痕');
});

test('★ `0x25b`：写**常量**槽 + 条件转发（条件不成立时**不许**记成欠账）', () => {
  const m = machineStub();
  HANDLERS[0x25b](ctxOf(m, [imm(0x77)], 0x25b));
  assert.equal(m.scalars.read('Engine.d92379'), 2, '★ 常量 2（`form: const`，与操作数无关）');
  assert.equal(m.scalars.read('Engine.d92381'), 0x77, 'op1 进另一个槽');
  assert.equal(m.effects.filter((e) => e.action === 'engine.forward').length, 1,
    '门（Engine.d167990）默认为 0 ⇒ 条件成立 ⇒ 记一笔欠账');
  // ★ 门非 0 ⇒ 引擎**不调** ⇒ 不许记成欠账（否则保真欠账会多算），但要有 note
  const m2 = machineStub();
  m2.scalars.write('Engine.d167990', 1);
  HANDLERS[0x25b](ctxOf(m2, [imm(1)], 0x25b));
  assert.equal(m2.effects.filter((e) => e.action === 'engine.forward').length, 0, '条件不成立 ⇒ 不记欠账');
  assert.ok(m2.notes.some((n) => n.startsWith('forward-skipped')), '但要留一笔（可见）');
});

test('★ `0xfe` **照抄引擎的范围检查**（`op1 > 0x1F` ⇒ 抛，⛔ 不许 clamp/截断）；`0x10c` 的间接写必须留痕', () => {
  const m = machineStub();
  // 合法值：写进 `Engine.d517`
  HANDLERS[0xfe](ctxOf(m, [imm(12)], 0xfe));
  assert.equal(m.scalars.read('Engine.d517'), 12, '合法值照写');
  // ★ 越界：引擎在这里 `throw (aSetkeytotal, 65541)` ⇒ 本层也抛（**不**静默截断成 0x1F）
  assert.throws(() => HANDLERS[0xfe](ctxOf(m, [imm(0x20)], 0xfe)), /超出引擎的范围检查/, '0x20 必须抛');
  assert.equal(m.scalars.read('Engine.d517'), 12, '抛了就不该改状态');

  // `0x10c`：值合法 ⇒ **只记录**（间接写目标未取证），并且规则要看得见
  const m2 = machineStub();
  HANDLERS[0x10c](ctxOf(m2, [imm(3), imm(7)], 0x10c));
  const rec = m2.effects.filter((e) => e.action === 'engine.indirect-write');
  assert.equal(rec.length, 1, '必须留一条（⛔ 不许静默）');
  assert.equal(rec[0].disposition, 'logged-only', '未建模 ⇒ logged-only（会进「保真欠账」）');
  assert.deepEqual([rec[0].detail.slotIndex, rec[0].detail.value], [7, 3], '两个操作数都要记全');
  assert.throws(() => HANDLERS[0x10c](ctxOf(m2, [imm(0x20), imm(0)], 0x10c)), /超出引擎的范围检查/, '越界同样照抄引擎的异常');
});

/**
 * ★★ `0x71` **置** bit27 ⇒ `0x88` 能把它**清掉**（★ 状态真的变，不是只多两条日志）。
 *
 * 逐字（锚 = EA）：`0x71`（`sub_41ED80`）`.text:0041EE8D or dword ptr [ebx+0AAB44h], 8000000h`；
 * `0x88`（`sub_41FAB0`）`.text:0041FAF0 and dword ptr [esi+0AAB44h], 0F7FFFFFFh`（`0xAAB44 / 4 = 174801`）、
 * 真支 `.text:0041FAE4 mov dword ptr [esi+77800h], 1`（`77800h / 4 = 122368`）。
 *
 * ## 它守的三件事（每一条都对应一种"看起来正常"的坏实现）
 * 1. **位真的落状态**：只发 `logged-only` 日志的实现（本仓原先就是）在这里会红 —— 读出来永远是 0。
 * 2. **两条成对**：`0x71` 置 / `0x88`（`op1 == 0`）清**同一格同一位**；把清写成置、或换一位 ⇒ 红。
 * 3. **常量的真支**：`op1 != 0` 时写的是**常量 1**（不是 op1 的原值）⇒ 写原值也红。
 */
test('★ `0x71` 置 bit27 ⇒ `0x88` 能把它清掉（位状态真的变；记录是 `modeled`）', () => {
  const BIT = 0x08000000;
  // ① 置位 ⇒ 位从 0 变 1
  const m = machineStub();
  assert.equal(m.scalars.read('Engine.d174801'), 0, '没跑过 ⇒ 没写过的槽读 0');
  HANDLERS[0x71](ctxOf(m, [imm(1)], 0x71));
  assert.equal(m.scalars.read('Engine.d174801') & BIT, BIT, '★ `0x71` 之后 bit27 必须是 1');
  // ② 清位 ⇒ 位从 1 变 0（★ 这一条就是"成对"的判据：只置不清 / 只清不置 都会红）
  HANDLERS[0x88](ctxOf(m, [imm(0)], 0x88));
  assert.equal(m.scalars.read('Engine.d174801') & BIT, 0, '★ op1 = 0 ⇒ `0x88` 的假支必须把 bit27 清掉');
  assert.equal(m.scalars.read('Engine.d174801'), 0, '清的是**这一位**，不是整格（原先那一位之外没有别的写入）');
  // ③ 真支：`op1 != 0` ⇒ 写**常量 1**（另一格），且**不动** bit27
  const m2 = machineStub();
  HANDLERS[0x88](ctxOf(m2, [imm(7)], 0x88));
  assert.equal(m2.scalars.read('Engine.d122368'), 1, '★ 真支写的是常量 1（⛔ 不是 op1 的原值 7）');
  assert.equal(m2.scalars.read('Engine.d174801') & BIT, 0, '真支**不**置位（条件二选一）');
  // ④ 两条记录都必须是 `modeled`（退回 `logged-only` = 又变成"只记录、不落状态"）
  const bitsOf = (mm) => mm.effects.filter((e) => e.action === 'engine.scalar.bits');
  assert.deepEqual(bitsOf(m).map((e) => e.detail.op), ['set', 'clear'], '置 → 清，两次各留一笔（且顺序照执行序）');
  assert.deepEqual(bitsOf(m2).map((e) => e.detail.op), ['write'], '真支只写常量（不清位）');
  const bits = [...bitsOf(m), ...bitsOf(m2)];
  assert.ok(bits.every((e) => e.disposition === 'modeled'),
    '★ 已建成状态 ⇒ `modeled`（写 `logged-only` 就是"这个位答不出来"那个欠账又回来了）');
  assert.equal(bitsOf(m2)[0].detail.value, 1, '记录里的常量必须是 1（与槽里的值一致）');
  assert.equal(bitsOf(m)[0].detail.slot, 'Engine.d174801', '记录里要写明是哪个槽（否则读者只能去翻代码）');
  // ⑤ 与知识层逐条对上（槽名 / 形态 / 掩码都从表里现取 —— 表改了这里跟着说话）
  const set = ENGINE_SCALAR_BITS.find((b) => b.opcode === 0x71);
  const clear = ENGINE_SCALAR_BITS.find((b) => b.opcode === 0x88 && b.op === 'clear');
  assert.ok(set && clear, '知识层必须同时有"置位"与"清位"两条（成对）');
  assert.equal(set.name, clear.name, '★ 置与清必须是**同一个槽**');
  assert.equal(set.mask, clear.mask, '★ 置与清必须是**同一位**');
  assert.equal(set.op, 'set');
  assert.equal(clear.op, 'clear');
});

test('★ 转发那批：**不建模**但**逐次留痕**（写明被调符号与实参）—— 绝不静默空操作', () => {
  // 0x2f6：argc 1 的转发（实测 SYSTEM4 前 4 条里出现 3 次）
  const m = machineStub();
  HANDLERS[0x2f6](ctxOf(m, [imm(2)], 0x2f6));
  const fx = m.effects.filter((e) => e.action === 'engine.forward');
  assert.equal(fx.length, 1, '转发必须留一条记录');
  assert.equal(fx[0].disposition, 'logged-only', '★ 未建模 ⇒ `logged-only`（不是 `modeled`）');
  assert.deepEqual(fx[0].detail.args, [2], '实参要记下来');
  assert.ok(typeof fx[0].detail.callee === 'string' && fx[0].detail.callee.length > 0, '被调符号要记下来');
  assert.ok(m.notes.some((n) => n.startsWith('forward-not-modelled')), '还要留一条 note（日志里能一眼看到跳过了什么）');
  assert.equal(m.scalars.size, 0, '转发**不**该碰标量堆（那是另一类）');
});
