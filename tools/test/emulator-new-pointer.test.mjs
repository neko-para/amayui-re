/** @env pure @kind contract @why 指针/地址写错（步长、合成基址、解引用、键换算）不会抛：只会把值写到"另一格"，而脚本与日志看起来都正常 */
/**
 * tools/test/emulator-new-pointer.test.mjs —— **指针族 + 合成地址空间**的守卫
 * （`0x61 lookup-array` / `0x12c` / `0x64` 与它们依赖的取址、解引用、配置键换算）。
 *
 * ★ 判据为什么是这些：
 *   - 地址是**算术**：`base + 元素宽度 × 下标`，下标错一格就写到邻格；字符串族宽度是 **28** 不是 4。
 *   - 指针槽**不编解码**：写进去什么位模式，读出来就是什么（解开引用才解码）。
 *   - 配置键对指针族是**地址换算**：`(地址 − 该族全局池起点) / 元素宽度`；按"槽下标"算会得到另一个键。
 *   - 未映射 / 未对齐 ⇒ **响亮失败**（引擎那里是 UB，不是 0）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/handlers.ts';
import { decodeInt, encodeInt, readInt, writeInt } from '../../apps/emulator-new/src/values.ts';
import { poolBase, slotAddress } from '../../apps/emulator-new/src/address.ts';
import { ConfigStore } from '../../apps/emulator-new/src/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/spec.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

function machine(body = Buffer.alloc(4), config) {
  return new Machine({ body, table: new Map(), key: KEY, ...(config ? { config } : {}) });
}

test('★★ `0x61`：local ptr ← 取址(全局 int 第 10 格) + 4×3（地址写进**指针槽本身**）', () => {
  const m = machine();
  HANDLERS.get(0x61)(m, [op(12, 0), op(3, 10), op(0, 3)]);

  const expect = slotAddress(3, 13);
  assert.equal(m.frame.localPtr.get(0), expect, '★ 槽里是地址 = base + 4×idx');
  assert.equal(expect, poolBase(3) + 4 * 13, '地址口径：池基址 + 元素宽度×下标');

  // 解引用：把目标格写成 42，通过指针读回来（指针槽本身不解码，目标格才解码）
  m.globalInt.set(13, encodeInt(42, KEY));
  assert.equal(readInt(m, [op(12, 0)], 1), 42, '★ 指针读 = 解引用后解码');
});

test('★★ `0x61`：字符串指针（type 14）的步长是 **28**（不是 4）', () => {
  const m = machine();
  HANDLERS.get(0x61)(m, [op(14, 0), op(3, 2), op(0, 5)]);
  const base = slotAddress(3, 2);
  assert.equal(m.frame.localStringPtr.get(0), base + 28 * 5, '★ 28×5（用 4 会得到另一条记录的地址）');
});

test('★★ `0x61`：落点是**值族**（如 type 9）⇒ 抛类型异常（引擎 default 分支）', () => {
  const m = machine();
  assert.throws(() => HANDLERS.get(0x61)(m, [op(9, 0), op(3, 2), op(0, 1)]), /落点必须是引用 \/ 指针族/);
});

test('★ 写指针槽只接受引用 / 指针族 ⇒ 直接写值族是响亮失败', () => {
  const m = machine();
  assert.throws(() => HANDLERS.get(0x61)(m, [op(3, 0), op(3, 2), op(0, 1)]), /落点必须是引用 \/ 指针族/);
});

test('★ 指针解引用：写整型走过指针写到目标格（编码在目标上），读回同值', () => {
  const m = machine();
  m.frame.localPtr.set(1, slotAddress(3, 7));
  writeInt(m, [op(12, 1)], 1, 99);
  assert.equal(decodeInt(m.globalInt.get(7), KEY), 99, '★ 写到的是**目标格**（不是指针槽）');
  assert.equal(m.frame.localPtr.get(1), slotAddress(3, 7), '指针槽的内容没被写操作改掉');

  m.globalInt.set(7, encodeInt(5, KEY));
  assert.equal(readInt(m, [op(12, 1)], 1), 5);
});

test('★ 未映射 / 未对齐的指针 ⇒ 响亮失败（不许补 0）', () => {
  const zero = machine();
  zero.frame.localPtr.set(0, 0);
  assert.throws(() => readInt(zero, [op(12, 0)], 1), /不落在任何池窗口/);

  const mis = machine();
  mis.frame.localPtr.set(0, slotAddress(3, 0) + 2);
  assert.throws(() => readInt(mis, [op(12, 0)], 1), /没对齐到元素边界/);
});

test('★★ 配置键：指针族的键下标 = `(地址 − 全局池起点) / 元素宽度`（不是指针槽下标）', () => {
  const s = new ConfigStore(loadConfigKeys());
  const m = machine(Buffer.alloc(4), s);
  m.frame.localPtr.set(0, slotAddress(3, 10));
  m.globalInt.set(10, encodeInt(42, KEY));

  HANDLERS.get(0x1a2)(m, [op(12, 0)]); // save-int

  const key = '\u0003' + '0000000a';
  assert.ok(s.ints.has(key), `键必须是类型码 + 地址换算的 8 位十六进制（实际键：${[...s.ints.keys()].join(',')}）`);
  assert.equal(s.ints.get(key), 42, '值是**解引用**出来的整数');
});

test('★★ `0x64 copy-local-array`：把脚本体里那块整数逐格 `ENC` 后写到 op1 的地址', () => {
  // body: [raw=1 ⇒ 块在 4 字节处] 块首 = 2 个元素，随后是 7 / 9
  const body = Buffer.alloc(4 + 4 + 8);
  body.writeUInt32LE(2, 4);
  body.writeUInt32LE(7, 8);
  body.writeUInt32LE(9, 12);
  const m = machine(body);
  HANDLERS.get(0x64)(m, [op(9, 5), op(0, 1)]);
  assert.equal(m.frame.localInt.get(5), encodeInt(7, KEY) >>> 0, '格 5 = ENC(7)');
  assert.equal(m.frame.localInt.get(6), encodeInt(9, KEY) >>> 0, '格 6 = ENC(9)');
  assert.equal(m.frame.localInt.get(7), undefined, '★ 只写 count 格');

  // 空块 ⇒ 什么都不做
  const empty = Buffer.alloc(8);
  const m2 = machine(empty);
  HANDLERS.get(0x64)(m2, [op(9, 0), op(0, 1)]);
  assert.equal(m2.frame.localInt.size, 0, 'count = 0 ⇒ 不写');
});

test('★ `0x12c lookup-array-2d`：下标 = 行 × 行宽 + 列（乘在前、加在后）', () => {
  const m = machine();
  HANDLERS.get(0x12c)(m, [op(12, 0), op(3, 0), op(0, 3), op(0, 10), op(0, 4)]);
  assert.equal(m.frame.localPtr.get(0), poolBase(3) + 4 * (3 * 10 + 4), '3×10 + 4 = 34');
});

