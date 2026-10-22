/** @env pure @kind contract @why 启动前段这簇指令写错（查表规则、标量落点、越界口径、门/事件、entered 记账）不会抛：只会让启动链静默走另一条分支，或"到达 LOGO"变成一句无法验证的话 */
/**
 * tools/test/emulator-new-prologue.test.mjs —— 启动链前段（`CHECKCONFIG` 前后到 `LOGO.BIN` 入口）那一簇：
 *
 * - `0x2de` 字体名查表（宿主表；长度相等 + 逐字节相等；`@` 前缀跳过；未命中 −1）
 * - `0x56 and` / `0x5f gre`（与 `0x5c lt` 同族；有符号比较、结果 0/1）
 * - `0x76/0x77`（`bswap24` 后写同一子系统的两格）、`0x1a4`（成对写）、`0x2ee`（写 + 转发）
 * - `0xfe`（范围检查 ≤0x1f）、`0x10f` / `0x248` / `0x261` / `0x260` / `0x8b`（薄标量）
 * - `0x107/0x10b/0x30a`（按索引写表；**越界口径照抄**：skip / throw）、`0x10c`（间接写，只记录）
 * - `0x135/0x136/0x13f`（位操作；位号 >0x1f 的口径）
 * - `0x143`（事件刷新：长度槽 0、ip += 4、队列空⇒尾调不改变控制流）、`0xae`（存档版本检查的早退分支）
 * - `0x130 load-show-logo`（读引擎初始化置 1 的标志）+ `enteredIds`（"进入过哪些脚本"的记账）
 *
 * ★ 判据为什么是这些：这一簇全部是"**引擎数据的一个格**"级别的写入 —— 写错一格不报错，
 *   只会让后面的分支走另一条（核对表规则/越界口径/门位），或者让"到达 LOGO"无法被机械证明。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { encodeInt, readInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

function machine(opts = {}) {
  return new Machine({ body: Buffer.alloc(4), table: new Map(), key: KEY, ...opts });
}

const call = (m, opcode, ops) => HANDLERS.get(opcode)(m, ops);

test('★★ `0x2de` 字体名查表：0 基下标 / 未命中 −1 / `@` 前缀跳过 / **长度不等不算命中**', async () => {
  const m = machine({ fontNames: ['MS Gothic', 'メイリオ'] });
  call(m, 0x2de, [op(9, 0), op(5, 3003)]); // 空串 ⇒ 未命中
  assert.equal(readInt(m, [op(9, 0)], 1), -1, '空串在表里 ⇒ −1');

  m.globalString.set(3003, 'メイリオ');
  call(m, 0x2de, [op(9, 0), op(5, 3003)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 1, '0 基下标（第二条 ⇒ 1）');

  m.globalString.set(3003, '@MS Gothic');
  call(m, 0x2de, [op(9, 0), op(5, 3003)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 0, '★ 键首字节 `@` 跳过再比');

  m.globalString.set(3003, 'MS');
  call(m, 0x2de, [op(9, 0), op(5, 3003)]);
  assert.equal(readInt(m, [op(9, 0)], 1), -1, '★★ 长度不等 ⇒ 不命中（前缀匹配会得到 0）');
});

test('★ `0x2de`：宿主表为空 ⇒ 一律未命中（就是引擎的回退分支，不是"抛"）', async () => {
  const m = machine({ fontNames: [] });
  m.globalString.set(1, 'whatever');
  call(m, 0x2de, [op(9, 0), op(5, 1)]);
  assert.equal(readInt(m, [op(9, 0)], 1), -1);
});

test('★ `0x56 and` / `0x5f gre`：按 u32 位与 / **有符号** >=，结果 0/1', async () => {
  const m = machine();
  call(m, 0x56, [op(9, 0), op(0, 0b1100), op(0, 0b1010)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 0b1000);
  call(m, 0x5f, [op(9, 1), op(0, -1), op(0, 0)]);
  assert.equal(readInt(m, [op(9, 1)], 1), 0, '−1 >= 0 为假（按无符号会得到 1）');
  call(m, 0x5f, [op(9, 1), op(0, 5), op(0, 5)]);
  assert.equal(readInt(m, [op(9, 1)], 1), 1, '相等 ⇒ 1（`gre` 不是 `gr`）');
});

test('★ `0x76`/`0x77`：写的是 `bswap24(op1)`（不是原值），落点各自一格', async () => {
  const m = machine();
  call(m, 0x76, [op(0, 0x123456)]);
  assert.equal(m.scalars.get('Engine.d21664'), 0x563412);
  call(m, 0x77, [op(0, 0xabcdef)]);
  assert.equal(m.scalars.get('Engine.d21665'), 0xefcdab);
});

test('★ `0x1a4` 成对写：op1 → d21670、op2 → d21671（⛔ 不换位）', async () => {
  const m = machine();
  call(m, 0x1a4, [op(0, 7), op(0, 9)]);
  assert.equal(m.scalars.get('Engine.d21670'), 7);
  assert.equal(m.scalars.get('Engine.d21671'), 9);
});

test('★ `0xfe`：值 > 0x1f ⇒ 抛（引擎自己的范围检查）；否则写 `Engine.d517`', async () => {
  const m = machine();
  call(m, 0xfe, [op(0, 0x1f)]);
  assert.equal(m.scalars.get('Engine.d517'), 0x1f);
  assert.throws(() => call(m, 0xfe, [op(0, 0x20)]), /超出引擎的范围检查/);
});

test('★ `0x107` / `0x10b` / `0x30a`：索引操作数的位置各不相同，越界口径 skip / throw 照抄', async () => {
  const m = machine();
  call(m, 0x107, [op(0, 3), op(0, 11)]); // 0x107：索引 = op1
  assert.equal(m.scalars.get('Engine.d551+3'), 11);
  call(m, 0x10b, [op(0, 12), op(0, 2)]); // 0x10b：索引 = **op2**
  assert.equal(m.scalars.get('Engine.d1383+2'), 12);

  // 0x107 越界 ⇒ 静默跳过（记诊断，不写）
  call(m, 0x107, [op(0, 0x20), op(0, 5)]);
  assert.equal(m.scalars.has('Engine.d551+32'), false, '0x107 越界 ⇒ 引擎静默跳过');
  // 0x30a 越界 ⇒ 抛
  assert.throws(() => call(m, 0x30a, [op(0, 1), op(0, 8)]), /越界/);
  assert.throws(() => call(m, 0x30a, [op(0, 0x20), op(0, 1)]), /越界/);
});

test('★ `0x10c`：值 > 0x1f ⇒ 抛；否则只记录（索引表未取证，⛔ 不猜它是 0）', async () => {
  const m = machine();
  call(m, 0x10c, [op(0, 3), op(0, 2)]);
  assert.equal(m.effects.at(-1)?.kind, 'engine.indirect-write');
  assert.equal(m.scalars.size, 0, '0x10c 不写任何标量（索引表未取证）');
  assert.throws(() => call(m, 0x10c, [op(0, 0x20), op(0, 1)]), /0x1f/);
});

test('★ `0x135`/`0x136`/`0x13f`：置位 / 清位 / 查位（位号在 op3）', async () => {
  const m = machine();
  m.frame.localInt.set(0, encodeInt(0, KEY));
  call(m, 0x135, [op(9, 0), op(0, 3)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 8);
  call(m, 0x136, [op(9, 0), op(0, 3)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 0);
  m.frame.localInt.set(1, encodeInt(0b1010, KEY));
  call(m, 0x13f, [op(9, 2), op(9, 1), op(0, 3)]);
  assert.equal(readInt(m, [op(9, 2)], 1), 1, '第 3 位是 1');
  call(m, 0x13f, [op(9, 2), op(9, 1), op(0, 2)]);
  assert.equal(readInt(m, [op(9, 2)], 1), 0, '第 2 位是 0');
  // 位号越界：写操作不写、查位写 0（引擎口径）
  m.frame.localInt.set(3, encodeInt(-1, KEY));
  call(m, 0x135, [op(9, 3), op(0, 0x20)]);
  assert.equal(readInt(m, [op(9, 3)], 1), -1, '位号 >0x1f ⇒ 不写 op1');
  call(m, 0x13f, [op(9, 4), op(9, 3), op(0, 0x20)]);
  assert.equal(readInt(m, [op(9, 4)], 1), 0);
});

test('★ `0x143` 事件刷新：长度槽清 0、ip += 4（队列空 ⇒ 尾调不改变控制流），并记诊断', async () => {
  const m = machine();
  m.ip = 100;
  call(m, 0x143, []);
  assert.equal(m.ip, 104, '★ 引擎自己在体内加 4 字节；长度槽清 0 ⇒ 分派器不再加');
  assert.equal(m.operandCount, 0);
  assert.ok(m.diagnostics.some((d) => d.kind === 'event-queue-unmodeled:0x143'));
});

test('★ `0xae` 存档版本检查：`Engine.d95780 == 0` ⇒ 早退（什么都不做）；非 0 ⇒ 响亮失败', async () => {
  const m = machine();
  call(m, 0xae, []);
  assert.equal(m.effects.length, 0, '早退分支不写任何东西');
  m.scalars.set('Engine.d95780', 1);
  assert.throws(() => call(m, 0xae, []), /未建模/);
});

test('★★ `0x130` show-logo 标志：初始化置 1（⇒ 启动链会走到 LOGO 的 call-script）；被清后读 0', async () => {
  const m = machine();
  call(m, 0x130, [op(9, 0)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 1, '★ 引擎初始化 `[0x5EB5C] = 1` ⇒ LOGO 分支成立');
  m.showLogo = 0;
  call(m, 0x130, [op(9, 0)]);
  assert.equal(readInt(m, [op(9, 0)], 1), 0);
});

test('★ `0x260` / `0x261`：四格 / 一格的消息子系统标量（op1..op4 的落点不许错位）', async () => {
  const m = machine();
  call(m, 0x260, [op(0, 1), op(0, 2), op(0, 3), op(0, 4)]);
  assert.equal(m.scalars.get('Engine.d80102'), 1);
  assert.equal(m.scalars.get('Engine.d80103'), 2);
  assert.equal(m.scalars.get('Engine.d80104'), 3);
  assert.equal(m.scalars.get('Engine.d80105'), 4);
  call(m, 0x261, [op(0, 9)]);
  assert.equal(m.scalars.get('Engine.d80101'), 9);
});

test('★★ `enteredIds`：`0x3 call-script` 与 `0x8 call-frame` 进入的脚本 id 被记下（"到达 LOGO"可机械断言）', async () => {
  const m = machine();
  m.enterScript(Buffer.alloc(4), 0x51dc); // INITCONFIG.BIN
  assert.deepEqual(m.enteredIds, [0x51dc]);

  const rec = m.loadFrameAt(5, Buffer.alloc(4), 0x5262); // 装载 LOGO 到 5 号槽（**不算进入**）
  assert.deepEqual(m.enteredIds, [0x51dc], '装载≠进入');
  m.callFrameAt(5); // 换帧 ⇒ 进入
  assert.deepEqual(m.enteredIds, [0x51dc, 0x5262], '★ 换帧进入 LOGO 也要记');
  assert.equal(rec.id, 0x5262);
});
