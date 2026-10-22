/** @env pure @kind contract @why 连写 N 格 / 跳转 / 比较写错（填字面 0、算错格数、哨兵当位置、比较反向）都不会抛：只会让后面每个分支静默走错 */
/**
 * tools/test/emulator-new-array-fill.test.mjs —— 启动链 `INITCONFIG4.BIN` 用到的那一小簇：
 * `0x6c fill-zero` / `0x2d8 set-array-to` / `0x8c jmp` / `0x5c lt`。
 *
 * ★ 判据为什么是这些：
 *   - `fill-zero` 的助记名骗人：它填的是 **`ENC(key, 0)`**，不是字面 0 ⇒ 判据必须断言**裸位模式**；
 *   - 连写是"从 op1 的**地址**起 `count` 个 dword" ⇒ 判据必须看**邻居**（多写一格/少写一格都要红）；
 *   - `jmp` 的 **−1 哨兵**是"没有目标、原地落下" ⇒ 当成位置会算出 `ip = −4`；
 *   - `lt` 是**有符号**比较且结果写 0/1 ⇒ 反向或按无符号比较，后面的循环次数就错了。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { decodeInt, encodeInt, readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

function machine() {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY });
}

const encZero = encodeInt(0, KEY) >>> 0;

test('★★ `0x6c fill-zero`：从地址起填 count 个 dword，填的是 **ENC(key,0)**（不是字面 0），邻居不动', async () => {
  const m = machine();
  assert.notEqual(encZero, 0, '非零 key 下 ENC(key,0) ≠ 0（否则这条判据抓不到"填 0"）');
  // 局部 int 池（type 9）：下标 3 / 7 放哨兵值，填 [4..6]
  m.frame.localInt.set(3, encodeInt(7, KEY) >>> 0);
  m.frame.localInt.set(7, encodeInt(9, KEY) >>> 0);

  HANDLERS.get(0x6c)(m, [op(9, 4), op(0, 3)]);

  for (const i of [4, 5, 6]) {
    assert.equal(m.frame.localInt.get(i), encZero, `格 ${i} 必须是 ENC(key,0) 的**位模式**（不是 0）`);
    assert.equal(decodeInt(m.frame.localInt.get(i), KEY), 0, '解码之后才是 0');
  }
  assert.equal(m.frame.localInt.get(3), encodeInt(7, KEY) >>> 0, '起始下标之前的格子不许被动');
  assert.equal(m.frame.localInt.get(7), encodeInt(9, KEY) >>> 0, '★ 越过 count 的格子不许被动（多写一格会红）');
});

test('★ `0x6c`：count ≤ 0 ⇒ 一格都不写（引擎的循环不进入）', async () => {
  const m = machine();
  HANDLERS.get(0x6c)(m, [op(9, 2), op(0, 0)]);
  assert.equal(m.frame.localInt.size, 0, 'count = 0');
  HANDLERS.get(0x6c)(m, [op(9, 2), op(0, -1)]);
  assert.equal(m.frame.localInt.size, 0, 'count = −1');
});

test('★ `0x6c`：落点是**浮点池**（type 10）时写的是裸位模式（不做数值转换）', async () => {
  const m = machine();
  HANDLERS.get(0x6c)(m, [op(10, 0), op(0, 2)]);
  assert.equal(m.frame.localFloat.get(0), encZero, '浮点池里存的就是 ENC(key,0) 那个 dword');
  assert.equal(m.frame.localFloat.get(1), encZero);
});

test('★★ `0x2d8 set-array-to`：填 **ENC(key, op2)**（不是 op2 原值）', async () => {
  const m = machine();
  HANDLERS.get(0x2d8)(m, [op(9, 1), op(0, 7), op(0, 2)]);
  assert.equal(m.frame.localInt.get(1), encodeInt(7, KEY) >>> 0, '格 1 = 编码后的 7');
  assert.equal(m.frame.localInt.get(2), encodeInt(7, KEY) >>> 0, '格 2 = 编码后的 7');
  assert.equal(m.frame.localInt.get(3), undefined, '★ count = 2 ⇒ 第 3 格不许被动');
  assert.equal(readInt(m, [op(9, 1)], 1), 7, '读回来是 7（编码/解码互逆）');
});

test('★ `0x8c jmp`：op1 = 位置 ⇒ ip = 4×位置、长度槽清 0；**−1 哨兵 ⇒ 原地落下**', async () => {
  const m = machine();
  HANDLERS.get(0x8c)(m, [op(0, 27)]);
  assert.equal(m.ip, 108, '位置 27 ⇒ 字节 108（把位置当字节会得到 27）');
  assert.equal(m.operandCount, 0, '控制转移后分派器不许再推进');

  const s = machine();
  HANDLERS.get(0x8c)(s, [op(0, -1)]);
  assert.equal(s.ip, 0, '★ 哨兵 −1 ⇒ 不跳（当成位置会算出 ip = −4）');
  assert.equal(s.operandCount, 0, '哨兵路径不写长度槽 —— 本桩初值就是 0，这里只确认它没被写成"跳转"');
});

test('★ `0x5c lt`：op1 ← (int32(op2) < int32(op3)) ? 1 : 0（有符号）', async () => {
  const m = machine();
  const w = (k) => [op(9, k)];
  HANDLERS.get(0x5c)(m, [op(9, 0), op(0, 3), op(0, 5)]);
  assert.equal(readInt(m, w(0), 1), 1, '3 < 5 ⇒ 1');
  HANDLERS.get(0x5c)(m, [op(9, 0), op(0, 5), op(0, 3)]);
  assert.equal(readInt(m, w(0), 1), 0, '5 < 3 ⇒ 0（反向就会得到 1）');
  HANDLERS.get(0x5c)(m, [op(9, 0), op(0, -1), op(0, 0)]);
  assert.equal(readInt(m, w(0), 1), 1, '−1 < 0 ⇒ 1（按无符号比较会得到 0）');
});

test('★ 连写/取址的操作数类型不支持 ⇒ 响亮失败（不是静默写错池）', async () => {
  const m = machine();
  assert.throws(() => HANDLERS.get(0x6c)(m, [op(5, 3), op(0, 1)]), /address:unsupported-operand-type:0x5/);
  assert.throws(() => HANDLERS.get(0x2d8)(m, [op(5, 3), op(0, 1), op(0, 1)]), /address:unsupported-operand-type:0x5/);
});
