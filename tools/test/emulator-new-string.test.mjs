/** @env pure @kind contract @why `0x192` 的源/落点或 type 2 的偏移口径写错，会把字体名之类写成一串"看起来正常"的错字节，而启动链只是静默走错分支 */
/**
 * tools/test/emulator-new-string.test.mjs —— 字符串族里**启动链实际用到**的两件事：
 *
 * 1. **type 2（内联字符串）的读法**（取证：台账 `01M4B3DQVB3G390N2E6E2S3Q2T`，判据是
 *    `INITCONFIG0.BIN` 的 5 条 `set-string` 解出字体名）：
 *    文件偏移 = `headerLen + 4×raw`，而 `Machine.body` 已切头 ⇒ **body 偏移 = 4×raw**；
 *    逐字节 `^0xFF`，解出 0 即止。偏移漏掉 `4×` 或少了 `^0xFF`，解出来的字符串就整体错位。
 * 2. **`0x192 set-string`**：把第 2 个操作数的文本写进第 1 个操作数的串池格（`sub_433660`，argc 2）。
 *
 * ★ 判据为什么是这些：这三处（偏移、取反、落点分派）任何一处写错都**不会抛** ——
 *   只会把错误字节写进池/配置，于是后面 `0x1a9` 把错串存进玩家配置，而"跑完了"看起来正常。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ConfigStore } from '../../apps/emulator-new/src/core/config.ts';
import { loadConfigKeys } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { HANDLERS } from '../../apps/emulator-new/src/core/handlers.ts';
import { readText } from '../../apps/emulator-new/src/core/values.ts';
import { slotAddress } from '../../apps/emulator-new/src/core/address.ts';

const KEY = 0x1234abcd;
const op = (type, raw) => ({ type, raw });

/** 造一段 body，并在 `4×raw` 处放内联字符串（逐字节 `^0xFF`，终止符是**原始 0xFF**） */
function inlineBody(raw, text, { terminate = true } = {}) {
  const off = 4 * raw;
  const bytes = Buffer.alloc(off + text.length + 4);
  for (let i = 0; i < text.length; i += 1) bytes[off + i] = text.charCodeAt(i) ^ 0xff;
  if (terminate) bytes[off + text.length] = 0xff;
  return bytes;
}

function machine(body = Buffer.alloc(4), config) {
  return new Machine({ body, table: new Map(), key: KEY, ...(config ? { config } : {}) });
}

test('★ type 2（内联字符串）：body 偏移 = 4×raw、逐字节 ^0xFF、到 0 止', async () => {
  const m = machine(inlineBody(3, 'AB'));
  assert.equal(readText(m, [op(2, 3)], 1), 'AB', 'raw=3 ⇒ body 偏移 12（写成 3 会从别处读）');
  assert.equal(readText(machine(inlineBody(0, 'x')), [op(2, 0)], 1), 'x', 'raw=0 ⇒ 偏移 0');

  assert.throws(
    () => readText(machine(Buffer.alloc(4)), [op(2, 3)], 1),
    /越出脚本体/,
    '偏移越界必须响亮失败（不是空串）',
  );
  assert.throws(
    () => readText(machine(inlineBody(3, 'AB', { terminate: false })), [op(2, 3)], 1),
    /没有终止符/,
    '读完整个 body 都没有终止符 ⇒ 响亮失败（⛔ 不许一路读到末尾）',
  );
});

test('★★ `0x192 set-string`：内联串（op2 type 2）→ 全局串池（op1 type 5）', async () => {
  const m = machine(inlineBody(3, 'AB'));
  HANDLERS.get(0x192)(m, [op(5, 3003), op(2, 3)]);
  assert.equal(m.globalString.get(3003), 'AB', '★ 内联串必须被解出来再写进 op1 指的那一格');
  assert.equal(m.effects.at(-1)?.kind, 'string.set', '要留痕（写外部请求/引擎数据的记录）');
  assert.equal(m.effects.at(-1)?.detail?.opcode, '0x192');

  // 源也可以是串池：op2 type 5（启动链里的另一种形态）
  m.globalString.set(4, 'xyz');
  HANDLERS.get(0x192)(m, [op(5, 3005), op(5, 4)]);
  assert.equal(m.globalString.get(3005), 'xyz', '串池 → 串池的搬运');
});

test('★ `0x192`：落点不是 type 5 ⇒ 响亮失败（不静默写错池）', async () => {
  const m = machine(inlineBody(3, 'AB'));
  assert.throws(
    () => HANDLERS.get(0x192)(m, [op(3, 7), op(2, 3)]),
    /text-unsupported-operand-type:0x3/,
    '局部整型（type 3）当落点必须抛 —— 引擎那条按 op1 type 分派，非串池类会抛',
  );
});

test('★★ `0x192` 的落点 / 源可以是指针族（type 8 / 14）：解引用后写 / 读那条记录', async () => {
  const m = machine(inlineBody(3, 'AB'));
  m.globalString.set(4, 'seed');
  // 指针槽 0 指向 global-string 4 的记录地址（与 `0x61` 落进去的是同一种地址）。
  // ★ 8 = 全局指针池、14 = 当前帧的指针池 —— 两张表不是同一张。
  m.globalStringRef.set(0, slotAddress(5, 4, 0, 0));
  m.frame.localStringPtr.set(0, slotAddress(5, 4, 0, 0));
  m.globalString.set(7, 'abc');
  HANDLERS.get(0x192)(m, [op(14, 0), op(5, 7)]);
  assert.equal(m.globalString.get(4), 'abc', '落点 14 ⇒ 写指针指向的那条记录（不是槽本身）');
  HANDLERS.get(0x192)(m, [op(8, 0), op(5, 7)]);
  assert.equal(m.globalString.get(4), 'abc', '落点 8 同样写穿');
  m.globalString.set(8, '');
  HANDLERS.get(0x192)(m, [op(5, 8), op(14, 0)]);
  assert.equal(m.globalString.get(8), 'abc', '源 14 ⇒ 读指针指向的那条记录');
  assert.equal(readText(m, [op(8, 0)], 1), 'abc', '源 8 / 14 的文本读');
  assert.throws(
    () => HANDLERS.get(0x192)(m, [op(14, 1), op(5, 7)]),
    /地址|0x0/,
    '空指针槽 ⇒ 响亮失败（空指针是 UB，⛔ 不许静默写 0 号记录）',
  );
});

test('★★ 内联串 → `0x192` → `0x1a9` → `0x1aa` 往返：配置里存的必须是解出来的那个串', async () => {
  const s = new ConfigStore(await loadConfigKeys());
  const m = machine(inlineBody(3, 'AB'), s);
  HANDLERS.get(0x192)(m, [op(5, 3003), op(2, 3)]);
  HANDLERS.get(0x1a9)(m, [op(5, 3003)]);
  assert.ok(s.strings.has('\u0005' + '00000bbb'), '字符串键 = 类型码 \\x05 + 8 位十六进制下标（3003 = 0xbbb）');
  // 读回：先把池里的值改掉，load-string 必须把它恢复成配置里那个串
  m.globalString.set(3003, 'zzz');
  HANDLERS.get(0x1aa)(m, [op(5, 3003)]);
  assert.equal(m.globalString.get(3003), 'AB', '★ 内联串 → 配置 → 池 的往返（错一处就会得到空串/乱码）');
});
