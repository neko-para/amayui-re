/** @env pure @kind contract @why 按住派发族（0x102/0x103/0x104/0x105/0xfc）被实现成 0x100 的别名、槽维被抹平、掩码被当成输入字、模数 0 静默兜底，或 0xfc 漏清一张表 / 漏把模数归 0，而 TITLE 仍可能"看起来在跑" */
/**
 * tools/test/emulator-new-input-hold.test.mjs —— `data/spec/chapters/60-ops-input.md` 的守卫
 *
 * 覆盖：
 * - `CORE-INPUT-102-SLOT`：0x102 键 0..31 / 槽 0..9 两个独立上界、越界一槽不写、三元组覆盖、目标 −1
 * - `CORE-INPUT-103-MODULUS`：0x103 模数原样（含 0）、只清**当前帧槽**游标（负模数 ≡ 绝对值）
 * - `CORE-INPUT-105-MASK`：0x105 掩码全 1 照写、不碰输入字
 * - `CORE-OPS-INPUT-104`：反向取最高位 / 上限 0 走无按键编号 / 掩码命中 ⇒ 返回点=自身 + 夹上限（不撤销）/
 *   未登记 ⇒ 撤销 + 推进 1 / 每条出口推进游标 / 模数 0 响亮失败 / 两张表互不可见 /
 *   **游标按帧槽号存**（帧释放 / 重载不复位）· 第一次调用由上下文闸定义成 0 ·
 *   负模数 ≡ 绝对值 · 游标 INT_MAX ∧ 模数 −1 的商溢出响亮失败
 * - `CORE-INPUT-RESET-FC`：0xfc 清**两张表**（所有帧）+ 模数归 0 + 掩码/游标/上限/输入字不变
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';

const table = await loadOpcodeTable();
const KEY = 0x1234abcd;
/** 裸调用 handler 时只要 body 存在（不跑 `run`，内容无关） */
const EMPTY = Buffer.alloc(256);

function blank() {
  return new Machine({ body: EMPTY, table, key: KEY });
}

/** 三个立即数操作数（type 0：`readInt` 按 `raw | 0` 读，负值可直接给） */
function ops(...raws) {
  return raws.map((raw) => ({ type: 0, raw }));
}

function script(instrs) {
  const words = [];
  for (const [opcode, operands] of instrs) {
    words.push(opcode);
    for (const [type, raw] of operands) words.push(type, raw);
  }
  const buf = Buffer.alloc(words.length * 4);
  words.forEach((w, i) => buf.writeUInt32LE(w >>> 0, i * 4));
  return buf;
}

/**
 * 按住游标：按**帧槽号**存在 `Machine.holdCursorBySlot`（⛔ 不是 `Frame` 上的格）；
 * `null` = 引擎不建立初值（⛔ 不是 0）。
 */
function cursorOf(m, slot = m.cur) {
  return m.holdCursorBySlot[slot];
}

test('★ 0x102：键 31 / 槽 9 是上界、越界一槽不写、三元组覆盖、键编号与键总数无关', () => {
  const register = HANDLERS.get(0x102);
  const m = blank();

  register(m, ops(31, 9, 1234));
  assert.equal(m.frame.holdSlots.get(31 * 10 + 9), 1234, '键 31 / 槽 9 是上界，必须能写');
  assert.equal(m.operandCount, 7, '推进格数 = 7（= 2 × 3 + 1）');

  // 目标位置不做检查：−1 是取消哨兵，原样写进格。
  register(m, ops(31, 9, -1));
  assert.equal(m.frame.holdSlots.get(31 * 10 + 9), -1, '目标 −1 原样写入（= 取消登记）');

  // 同一（帧, 键, 槽）重复登记 ⇒ 后一次覆盖前一次。
  register(m, ops(7, 3, 2));
  register(m, ops(7, 3, 9));
  assert.equal(m.frame.holdSlots.get(7 * 10 + 3), 9, '同一三元组后一次覆盖前一次');

  // 键编号越界：抛显示消息型异常，且一个槽都不写。
  assert.throws(() => register(m, ops(32, 0, 5)), /SetKeyJumpEx/, '键编号 32 必须抛');
  assert.equal(m.frame.holdSlots.has(32 * 10), false, '越界键一个槽都不写');
  assert.equal(m.frame.holdSlots.get(7 * 10 + 3), 9, '越界不碰已有表项');

  // 槽号越界：同一句异常文本，且槽 9 仍是原值（若被夹到 9 ⇒ 越界被静默截断）。
  register(m, ops(7, 9, 42));
  assert.throws(() => register(m, ops(7, 10, 5)), /SetKeyJumpEx/, '槽号 10 必须抛');
  assert.equal(m.frame.holdSlots.get(7 * 10 + 9), 42, '槽号越界 ⇒ 槽 9 仍是原值（不截断）');
  assert.equal(m.frame.holdSlots.has(7 * 10 + 10), false, '槽号越界不写那一格');
  assert.equal(m.frame.holdSlots.get(7 * 10 + 3), 9, '槽号越界不碰别的槽');

  // 键编号的检查与键总数无关：键总数 2 时登记键 20 必须允许。
  m.input.keyTotal = 2;
  m.input.keyTotalEstablished = true;
  register(m, ops(20, 0, 4));
  assert.equal(m.frame.holdSlots.get(20 * 10), 4, '键总数 2 + 键 20 ⇒ 不抛、照写');
});

test('★ 0x103：模数原样（含 0）、只清当前帧槽游标', () => {
  const setModulus = HANDLERS.get(0x103);
  const m = blank();
  m.hold.mask = 0b1011;
  m.frame.keyLimit = 6;
  m.frame.holdSlots.set(70, 3);

  m.holdCursorBySlot[0] = 5;
  setModulus(m, ops(7));
  assert.equal(m.hold.modulus, 7, '模数原样写入');
  assert.equal(cursorOf(m), 0, '当前帧槽游标 ← 0');
  assert.equal(m.operandCount, 3, '推进格数 = 3（= 2 × 1 + 1）');
  assert.equal(m.hold.mask, 0b1011, '不改掩码');
  assert.equal(m.frame.keyLimit, 6, '不改按键扫描上限');
  assert.equal(m.frame.holdSlots.get(70), 3, '不改按住派发表');

  // 0 照写、不抛（后果延迟到下一次 0x104 的游标推进）。
  setModulus(m, ops(0));
  assert.equal(m.hold.modulus, 0, '模数 0 原样写入（本条不拦）');
  assert.equal(m.diagnostics.some((d) => d.kind === 'hold-modulus-zero:0x103'), true, '0 留诊断');

  // 只清**当前帧槽**：切到帧 B（槽 1，游标 5）后设置模数，槽 0 的游标必须保持原值。
  m.holdCursorBySlot[0] = 3; // 帧 A（槽 0）
  const b = m.loadFrameAt(1, EMPTY);
  m.holdCursorBySlot[1] = 5; // 帧 B（槽 1；就是上面 `b` 那个帧对象）
  m.callFrameAt(1);
  setModulus(m, ops(11));
  assert.equal(m.hold.modulus, 11, '模数是引擎级的（所有帧共用一格）');
  assert.equal(cursorOf(m), 0, '帧 B 的游标被清 0');
  assert.equal(cursorOf(m, 0), 3, '帧 A 的游标保持原值（只清当前帧槽）');
  assert.equal(b.frame, m.frame, '确认清的是槽 1 那个帧对象的游标格（槽号索引）');
});

test('★ 0x105：掩码全 1 照写、不改输入字', () => {
  const setMask = HANDLERS.get(0x105);
  const m = blank();
  m.input.word = 0x12345678;
  m.hold.modulus = 9;

  setMask(m, ops(0xffffffff));
  assert.equal(m.hold.mask >>> 0, 0xffffffff, '掩码全 1 照写、不抛、不截');
  assert.equal((m.hold.mask >>> 31) & 1, 1, '第 31 位也照写');
  assert.equal(m.input.word, 0x12345678, '不改输入字（掩码不是输入字）');
  assert.equal(m.hold.modulus, 9, '不改模数');
  assert.equal(m.operandCount, 3, '推进格数 = 3');

  setMask(m, ops(1 << 5));
  assert.equal(m.hold.mask, 1 << 5, '下一次原样覆盖');
});

test('★ 0x104：反向取最高位、上限 0 走无按键编号', () => {
  const dispatch = HANDLERS.get(0x104);

  // (a) 同时按住 3 与 7 ⇒ 先派发 7（反向 = 编号最高）。
  const m = blank();
  m.ip = 4; // 本条在 dword 1；返回点 = ip/4 + 1 = 2（掩码未置起）
  m.hold.modulus = 10;
  m.input.keyTotal = 8;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 8;
  m.input.word = (1 << 3) | (1 << 7);
  m.frame.holdSlots.set(7 * 10 + 0, 2); // 游标 0：键 7 → 位置 2
  m.frame.holdSlots.set(3 * 10 + 0, 3); // 游标 0：键 3 → 位置 3
  dispatch(m, []);
  const dispatched = m.effects.filter((e) => e.kind === 'input.hold.dispatch');
  assert.equal(dispatched.length, 1, '只派发一个');
  assert.equal(dispatched[0].detail.index, 7, '反向：先派发编号最高的 7');
  assert.equal(m.ip, 8, '执行位置 ← 目标 2（dword）');
  assert.equal(m.operandCount, 0, '控制转移 ⇒ 推进格数 0');
  assert.deepEqual(m.frame.retStack, [2], '掩码未置起 ⇒ 返回点 = 下一条');
  assert.equal(cursorOf(m), 1, '每条出口推进游标');

  // (b) 上限 0 ⇒ 不扫描；编号 = 键总数（哪怕输入字里键 2 置起）。
  const z = blank();
  z.ip = 4;
  z.hold.modulus = 5;
  z.input.keyTotal = 5;
  z.input.keyTotalEstablished = true;
  z.frame.keyLimit = 0;
  z.input.word = 1 << 2;
  z.frame.holdSlots.set(5 * 10 + 0, 4); // "无按键"编号 = 键总数 5
  dispatch(z, []);
  const zEffects = z.effects.filter((e) => e.kind === 'input.hold.dispatch');
  assert.equal(zEffects.length, 1, '上限 0 ⇒ 走无按键表项');
  assert.equal(zEffects[0].detail.index, 5, '编号 = 键总数（不是实际按下的 2）');
  assert.equal(z.ip, 16, '跳到目标 4');
  assert.equal(zEffects.some((e) => e.detail.index === 2), false, '不得派发键 2');
});

test('★ 0x104：掩码命中夹上限 + 返回点=自身；未登记撤销但仍夹', () => {
  const dispatch = HANDLERS.get(0x104);

  // (a) 掩码置起键 7、槽已登记 ⇒ 返回点 = 本条自身，上限夹到 7。
  const m = blank();
  m.ip = 4;
  m.hold.modulus = 10;
  m.hold.mask = 1 << 7;
  m.input.keyTotal = 8;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 8;
  m.input.word = 1 << 7;
  m.frame.holdSlots.set(7 * 10 + 0, 3);
  dispatch(m, []);
  assert.deepEqual(m.frame.retStack, [1], '掩码命中 ⇒ 返回点 = 本条自身（ip/4 = 1）');
  assert.equal(m.frame.keyLimit, 7, '按键扫描上限已夹到键号');
  assert.equal(m.ip, 12, '跳到目标 3');

  // (b) 掩码未置起 ⇒ 返回点 = 下一条、上限不变。
  const plain = blank();
  plain.ip = 4;
  plain.hold.modulus = 10;
  plain.hold.mask = 0;
  plain.input.keyTotal = 8;
  plain.input.keyTotalEstablished = true;
  plain.frame.keyLimit = 8;
  plain.input.word = 1 << 7;
  plain.frame.holdSlots.set(7 * 10 + 0, 3);
  dispatch(plain, []);
  assert.deepEqual(plain.frame.retStack, [2], '掩码为 0 ⇒ 返回点 = 下一条');
  assert.equal(plain.frame.keyLimit, 8, '掩码为 0 ⇒ 不夹上限');

  // (c) 掩码置起但槽未登记 ⇒ 撤销返回点、推进 1，**但夹取不回滚**。
  const miss = blank();
  miss.ip = 4;
  miss.hold.modulus = 10;
  miss.hold.mask = 1 << 7;
  miss.input.keyTotal = 8;
  miss.input.keyTotalEstablished = true;
  miss.frame.keyLimit = 8;
  miss.input.word = 1 << 7;
  dispatch(miss, []);
  assert.deepEqual(miss.frame.retStack, [], '未登记 ⇒ 返回栈顶 −1（撤销）');
  assert.equal(miss.operandCount, 1, '推进格数留在 1');
  assert.equal(miss.ip, 4, '不跳');
  assert.equal(miss.frame.keyLimit, 7, '夹取发生在查表之前 ⇒ 未登记也已经是键号');
  assert.equal(miss.effects.filter((e) => e.kind === 'input.hold.dispatch').length, 0);
});

test('★ 0x104：游标每出口推进、模数 0 响亮失败、两张表互不可见', () => {
  const dispatch = HANDLERS.get(0x104);

  // (a) 连续两次（都不命中）⇒ 两次的游标不同（按模数回绕）。
  const m = blank();
  m.ip = 4;
  m.hold.modulus = 7;
  m.input.keyTotal = 0;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 0;
  m.input.word = 0;
  dispatch(m, []);
  assert.equal(cursorOf(m), 1, '第一次出口后游标 = 1（0 + 1 mod 7）');
  dispatch(m, []);
  assert.equal(cursorOf(m), 2, '第二次出口后游标 = 2（两次不同）');
  assert.equal(m.input.word, 0, '本条不改输入字');
  assert.equal(m.hold.mask, 0, '本条不改掩码');
  assert.equal(m.hold.modulus, 7, '本条不改模数');

  // (b) 模数 0 ⇒ 显式失败（⛔ 不按 0 / 不按 1 / 不用默认模数兜底）。
  const zero = blank();
  zero.hold.modulus = 0;
  assert.throws(() => dispatch(zero, []), /input-hold: modulus is 0/, '模数 0 必须响亮失败');

  // (c) 两张表互不可见：0xfb 登记的键不被 0x104 命中。
  const t = blank();
  t.ip = 4;
  t.hold.modulus = 10;
  t.input.keyTotal = 8;
  t.input.keyTotalEstablished = true;
  t.frame.keyLimit = 8;
  t.input.word = 1 << 7;
  t.frame.keyCursor = 4;
  t.frame.keyJumps.set(7, 2); // 只登记按键回调表
  dispatch(t, []);
  assert.equal(t.effects.filter((e) => e.kind === 'input.hold.dispatch').length, 0, '0xfb 的表对 0x104 不可见');
  assert.equal(t.ip, 4, '不跳');
  assert.deepEqual(t.frame.retStack, [], '未登记 ⇒ 撤销');
  assert.equal(t.frame.keyCursor, 4, '不改 0x100 的按键扫描游标（两套游标）');

  // (d) 写进表的 −1 = 取消登记（引擎哨兵）：必须走"未登记"，⛔ 不能当成位置 −1 跳。
  const cancel = blank();
  cancel.ip = 4;
  cancel.hold.modulus = 10;
  cancel.input.keyTotal = 8;
  cancel.input.keyTotalEstablished = true;
  cancel.frame.keyLimit = 8;
  cancel.input.word = 1 << 7;
  cancel.frame.holdSlots.set(7 * 10 + 0, -1);
  dispatch(cancel, []);
  assert.equal(cancel.effects.filter((e) => e.kind === 'input.hold.dispatch').length, 0, '−1 = 未登记');
  assert.equal(cancel.ip, 4, '不跳到位置 −1');
  assert.deepEqual(cancel.frame.retStack, [], '撤销返回点');
});

test('★ 0x104：游标按帧槽号存 —— 帧释放 / 重载后从原值继续（不是 null、不是 0）', () => {
  const dispatch = HANDLERS.get(0x104);
  const m = blank();
  m.hold.modulus = 7;
  m.input.keyTotal = 0;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 0;
  m.input.word = 0;
  dispatch(m, []); // 第一次：上下文闸把槽 0 定义成 0，随后推进 → 1
  dispatch(m, []); // 编号不变 ⇒ 不重置，1 → 2
  assert.equal(cursorOf(m), 2, '先推进到非 0');

  // (a) 重载同一槽（换成新 Frame 对象）⇒ 游标不复位（陈旧但是定义值）。
  const oldFrame = m.frame;
  const reloaded = m.loadFrameAt(0, EMPTY);
  assert.notEqual(reloaded.frame, oldFrame, '重载确实换了 Frame 对象');
  assert.equal(cursorOf(m), 2, '帧重载不复位游标');
  reloaded.frame.keyLimit = 0;
  dispatch(m, []);
  assert.equal(cursorOf(m), 3, '重载后从原值 2 继续 → 3（不是 null、不是 0）');

  // (b) 子帧槽被释放（结束当前脚本）后再装回同一槽 ⇒ 游标仍是旧值。
  m.enterScript(EMPTY); // 槽 1
  assert.equal(m.cur, 1);
  assert.equal(cursorOf(m), null, '槽 1 从没被 0x104 写过');
  m.hold.contextKey = -1; // 让上下文闸先把槽 1 定义成 0
  m.input.word = 0;
  m.input.keyTotal = 0;
  m.frame.keyLimit = 0;
  dispatch(m, []); // 槽 1：0 → 1
  dispatch(m, []); // 1 → 2
  assert.equal(cursorOf(m), 2);
  m.returnToCaller();
  assert.equal(m.slots[1], null, '槽 1 已被释放');
  assert.equal(cursorOf(m, 1), 2, '释放不清游标');
  m.loadFrameAt(1, EMPTY);
  m.callFrameAt(1);
  m.input.word = 0;
  m.input.keyTotal = 0;
  m.frame.keyLimit = 0;
  dispatch(m, []); // 编号仍是 0（== contextKey）⇒ 闸不动作，2 → 3
  assert.equal(cursorOf(m), 3, '重装同一槽后从原值继续（按槽号，不是按帧对象）');
});

test('★ 0x104：第一次调用（contextKey 初值 −1）⇒ 当前槽游标定义为 0，不报 UB', () => {
  const dispatch = HANDLERS.get(0x104);
  const m = blank();
  assert.equal(m.hold.contextKey, -1, '模拟"进程里第一次"');
  assert.equal(cursorOf(m), null, '引擎不建立游标初值');
  m.hold.modulus = 10;
  m.input.keyTotal = 7;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 0; // 不扫描 ⇒ 编号 = 键总数 7
  m.frame.holdSlots.set(7 * 10 + 0, 4); // 只登记在槽 0 ⇒ 第一次必须命中
  assert.doesNotThrow(() => dispatch(m, []), '第一次 0x104 不得因"游标未定义"抛');
  const dispatched = m.effects.filter((e) => e.kind === 'input.hold.dispatch');
  assert.equal(dispatched.length, 1, '命中槽 0');
  assert.equal(dispatched[0].detail.slot, 0, '上下文闸把当前槽的游标定义成 0');
  assert.equal(m.ip, 16, '跳过到目标 4');
  assert.equal(cursorOf(m), 1, '出口推进 0 → 1');
  assert.equal(m.diagnostics.some((d) => d.kind === 'hold-cursor-uninitialized'), false, '不记 UB 诊断');

  // 残值路径：编号 == 静态格 ⇒ 上下文闸不动作 ⇒ 该槽仍是 null ⇒ 响亮失败（⛔ 不是 0）。
  const residual = blank();
  residual.hold.modulus = 10;
  residual.hold.contextKey = 7;
  residual.input.keyTotal = 7;
  residual.input.keyTotalEstablished = true;
  residual.frame.keyLimit = 0;
  assert.throws(() => dispatch(residual, []), /uninitialized/, '残值路径仍响亮失败');
});

test('★ 0x104：负模数 ≡ 绝对值（模数 −7 的槽号序列与 7 相同）', () => {
  const dispatch = HANDLERS.get(0x104);

  /** 模数给定时连续 7 次派发命中的槽号序列（键总数 0 ⇒ 编号恒 0，7 个槽全登记） */
  function slotsFor(modulus) {
    const m = blank();
    m.hold.modulus = modulus;
    m.input.keyTotal = 0;
    m.input.keyTotalEstablished = true;
    m.frame.keyLimit = 0;
    m.input.word = 0;
    for (let slot = 0; slot < 7; slot += 1) m.frame.holdSlots.set(slot, 100 + slot);
    const seen = [];
    for (let i = 0; i < 7; i += 1) {
      dispatch(m, []);
      const last = m.effects.filter((e) => e.kind === 'input.hold.dispatch').at(-1);
      seen.push(last.detail.slot);
    }
    return { seen, cursor: cursorOf(m) };
  }

  const positive = slotsFor(7);
  const negative = slotsFor(-7);
  assert.deepEqual(negative.seen, positive.seen, '−7 与 7 的槽号序列逐点相同（JS % 与 idiv 同语义）');
  assert.deepEqual(positive.seen, [0, 1, 2, 3, 4, 5, 6], '槽号 0..6 各一次');
  assert.equal(positive.cursor, 0, '7 次推进回到 0');
  assert.equal(negative.cursor, 0, '−7 也回到 0');
});

test('★ 0x104：游标 = 0x7fffffff ∧ 模数 = −1 ⇒ 商溢出响亮失败（hold-cursor-divide-overflow）', () => {
  const dispatch = HANDLERS.get(0x104);
  const m = blank();
  m.hold.modulus = -1;
  m.hold.contextKey = 0; // 不让上下文闸清掉注入的游标
  m.input.keyTotal = 0;
  m.input.keyTotalEstablished = true;
  m.frame.keyLimit = 0;
  m.input.word = 0;
  m.holdCursorBySlot[m.cur] = 0x7fffffff;
  assert.throws(() => dispatch(m, []), /overflows int32/, 'INT_MAX + 1 再 idiv −1 必须响亮失败');
  assert.equal(
    m.diagnostics.some((d) => d.kind === 'hold-cursor-divide-overflow'),
    true,
    '诊断名 = hold-cursor-divide-overflow',
  );
  assert.equal(m.holdCursorBySlot[m.cur], 0x7fffffff, '⛔ 不许算出 2147483648，也不许静默写成 0');
});

test('★ 0xfc：清两张表（所有帧）+ 模数 0 + 掩码/游标/上限/输入字不变', () => {
  const reset = HANDLERS.get(0xfc);
  const m = blank();
  m.hold.modulus = 7;
  m.hold.mask = 0b101;
  m.frame.keyJumps.set(4, 7);
  m.frame.holdSlots.set(70, 3);
  m.frame.keyLimit = 8;
  m.holdCursorBySlot[0] = 3;
  m.input.word = 0x10;

  const other = m.loadFrameAt(1, EMPTY);
  other.frame.keyJumps.set(5, 9);
  other.frame.holdSlots.set(50, 4);
  m.holdCursorBySlot[1] = 2;

  reset(m, []);
  assert.equal(m.operandCount, 1, '推进格数 = 1');
  assert.equal(m.hold.modulus, 0, '模数 ← 0（有意的复位）');
  assert.equal(m.frame.keyJumps.size, 0, '当前帧按键回调表被清');
  assert.equal(m.frame.holdSlots.size, 0, '当前帧按住派发表被清');
  assert.equal(other.frame.keyJumps.size, 0, '其它帧的按键回调表也被清（整张表）');
  assert.equal(other.frame.holdSlots.size, 0, '其它帧的按住派发表也被清（整张表）');
  assert.equal(m.hold.mask, 0b101, '掩码不被清');
  assert.equal(cursorOf(m), 3, '游标不被清');
  assert.equal(cursorOf(m, 1), 2, '其它帧的游标也不被清');
  assert.equal(m.frame.keyLimit, 8, '按键扫描上限不被清');
  assert.equal(m.input.word, 0x10, '输入字不被清');
  assert.equal(m.input.keyTotalEstablished, false, '键总数不被写');

  // 复位之后必须重新设模数：否则下一次 0x104 在游标推进处除零。
  assert.throws(() => HANDLERS.get(0x104)(m, []), /input-hold: modulus is 0/, '复位后 0x104 必须响亮失败');
});

test('★ 脚本里跑：0x102 推进 7、0x103 推进 3、0x104 跳走再返回', async () => {
  const setup = new Machine({
    body: script([
      [0x102, [[0, 7], [0, 3], [0, 2]]],
      [0x103, [[0, 10]]],
      [0x2, []],
    ]),
    table,
    key: KEY,
  });
  await setup.run();
  assert.equal(setup.frame.holdSlots.get(7 * 10 + 3), 2, '0x102 登记（键 7, 槽 3）→ 2');
  assert.equal(setup.hold.modulus, 10, '0x103 模数');
  assert.equal(setup.steps, 3, '两条指令各按自己的推进格数走到 exit');

  // 0x104 派发：目标是 0x5（ret）⇒ 回到"下一条"再 exit。
  const run = new Machine({
    body: script([[0x104, []], [0x2, []], [0x5, []]]),
    table,
    key: KEY,
  });
  run.input.keyTotal = 8;
  run.input.keyTotalEstablished = true;
  run.input.word = 1 << 7;
  run.hold.modulus = 10;
  run.frame.keyLimit = 8;
  run.frame.holdSlots.set(7 * 10 + 0, 2);
  const result = await run.run();
  assert.equal(run.input.keyDispatches, 1, '按键回调派发记账共用');
  assert.equal(run.hold.dispatches, 1, '按住派发记账');
  assert.deepEqual(result.stop, { kind: 'exit' }, '回调返回后继续到 exit');
  assert.equal(run.steps, 3, '0x104 + ret + exit');
});

test('★ 五个 handler 都在表里（防重复键覆盖 / 漏注册）', () => {
  for (const opcode of [0xfc, 0x102, 0x103, 0x104, 0x105]) {
    assert.equal(HANDLERS.get(opcode) !== undefined, true, `opcode 0x${opcode.toString(16)} 必须有 handler`);
  }
});
