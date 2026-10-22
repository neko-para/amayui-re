/** @env pure @kind contract @why lea 写成编码地址 / 随机范围 0 不写落点 / 参数格未写就伪造 0 / 命名表命中仍读默认位置 —— 这些都不会抛，只会让脚本静默走错 */
/**
 * tools/test/emulator-new-ops-000.test.mjs —— 0x000–0x0FF 段新增 8 个 handler 的合同守卫：
 * `0x63 lea` / `0x60 random` / `0x74`+`0x7f` 参数格 / `0xd0` 时钟 / `0xa1`+`0xa2`+`0xa3` 命名表。
 *
 * ★ 判据为什么是这些：
 *   - `0x63` 写的是**地址本身**（不编码）；按"写整型"实现会写进一个被编码的地址；
 *   - `0x60` 范围 = 0 时**先写 0 再抛**；顺序反了会让落点保留旧值；
 *   - `0x7f` 在 `0x74` 之前 ⇒ 规范说初值未观察，本实现选择**响亮失败**（不伪造 0）；
 *   - `0xa3` 命中时**不读**第 2 个操作数；读了就会在"默认位置类型非法"的脚本上抛。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { addressOfOperand, decodeInt, encodeInt, readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

function machine(extra = {}) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...extra });
}

test('★ `0x63 lea`：op1 的引用槽 ← 取址(op2) 的**地址本身**（不编码）', () => {
  const m = machine();
  // op2 = 全局整型池第 5 槽；它的地址由取址原语给出
  m.globalInt.set(5, encodeInt(99, KEY) >>> 0);
  const expected = addressOfOperand(m, [op(3, 5)], 1);
  HANDLERS.get(0x63)(m, [op(6, 1), op(3, 5)]);
  assert.equal(m.globalIntRef.get(1), expected, '写进去的是地址，不是编码后的整数');
  assert.notEqual(m.globalIntRef.get(1), encodeInt(expected, KEY) >>> 0, '★ 编码过的地址是另一回事');
});

test('★ `0x63 lea`：落点不是引用族 ⇒ 类型异常（不是静默写整数池）', () => {
  const m = machine();
  assert.throws(() => HANDLERS.get(0x63)(m, [op(9, 0), op(3, 5)]), /不是指针族|类型/);
});

test('★ `0x60 random`：范围 ≠ 0 ⇒ 落点 = 有符号余数；计数器 0..12 循环', () => {
  const m = machine({ random: () => 0.25 }); // bits = 0x40000000（正）
  HANDLERS.get(0x60)(m, [op(3, 1), op(0, 10)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 4, '0x40000000 % 10 = 4');
  assert.equal(m.randomCounter, 1);
  for (let i = 0; i < 12; i += 1) HANDLERS.get(0x60)(m, [op(3, 1), op(0, 10)]);
  assert.equal(m.randomCounter, 0, '第 13 次之后归 0');
});

test('★ `0x60 random`：范围 = 0 ⇒ **先**把落点写 0，**然后**抛', () => {
  const m = machine({ random: () => 0.25 });
  m.globalInt.set(1, encodeInt(77, KEY) >>> 0);
  assert.throws(() => HANDLERS.get(0x60)(m, [op(3, 1), op(0, 0)]), /范围 0/);
  assert.equal(decodeInt(m.globalInt.get(1), KEY), 0, '抛之前落点已经写成 0（旧值 77 必须消失）');
});

test('★ `0x74` / `0x7f`：参数格往返；`0x7f` 先于 `0x74` ⇒ 响亮失败', () => {
  const m = machine();
  m.globalInt.set(1, encodeInt(42, KEY) >>> 0);
  HANDLERS.get(0x74)(m, [op(3, 1)]);
  assert.equal(m.paramCell, 42);
  HANDLERS.get(0x7f)(m, [op(3, 2)]);
  assert.equal(decodeInt(m.globalInt.get(2), KEY), 42, '写回的是同一个整数');
  const fresh = machine();
  assert.throws(() => HANDLERS.get(0x7f)(fresh, [op(3, 2)]), /param-cell-uninitialized/);
});

test('★ `0xd0`：自己读一次宿主时钟（不夹取、不归一）', () => {
  const m = machine({ clock: () => 12345.9 });
  HANDLERS.get(0xd0)(m, [op(3, 1)]);
  assert.equal(readInt(m, [op(3, 1)], 1), 12345, '截断到 32 位整数');
});

test('★ `0xa1` / `0xa2` / `0xa3`：重置 / 绑定 / 命中即跳；命中时**不读**默认位置', () => {
  const m = machine();
  m.globalString.set(1, 'KEY');
  m.globalInt.set(2, encodeInt(7, KEY) >>> 0);

  HANDLERS.get(0xa2)(m, [op(5, 1), op(3, 2)]);
  assert.equal(m.namedTable.get('KEY'), 7);

  // 命中：第 2 个操作数是**浮点**（readInt 会抛）⇒ 若被读就会红
  m.ip = 0;
  HANDLERS.get(0xa3)(m, [op(5, 1), op(1, 0)]);
  assert.equal(m.ip, 28, '位置 7 ⇒ 字节 28');
  assert.equal(m.operandCount, 0, '控制转移后不再自动推进');

  // 未命中：才读默认位置
  m.globalString.set(9, 'MISS');
  m.ip = 0;
  HANDLERS.get(0xa3)(m, [op(5, 9), op(0, 3)]);
  assert.equal(m.ip, 12, '默认位置 3 ⇒ 字节 12');

  HANDLERS.get(0xa1)(m);
  assert.equal(m.namedTable.size, 0, '重置后按键查不到');
});

test('★ `0xa2`：整型键按十进制文本（不是按整型值）', () => {
  const m = machine();
  m.globalInt.set(1, encodeInt(42, KEY) >>> 0);
  HANDLERS.get(0xa2)(m, [op(3, 1), op(0, 5)]);
  assert.equal(m.namedTable.get('42'), 5);
});
