/** @env pure @kind contract @why 脚本反汇编/重汇编的接口或错误处理坏了（不需要样本也能判） */
/**
 * packages/age-format/test/asm.test.mjs —— ASM（AGE 脚本）的**无需样本**的判据
 *
 * 三条：
 *   ① 指令表只含**格式层四列**（知识层字段不得入库）；
 *   ② 坏输入必须**显式抛错**（不产出"看似成功的文本"）；
 *   ③ 非脚本的 BIN（拿 `SYS4INI.BIN` 的头造）必须被拒绝。
 *
 * ★ **拆过一刀**（本轮测试分级）：原先还有三条"反汇编 → 重汇编逐字节相同 / 确定性 / 极短脚本"——
 *   它们要**原始游戏文件**（不入库）⇒ 属 `@env assets`，已移到 `asm.assets.test.mjs`。
 *   不拆的话，这三条**能在任何机器上跑**的断言会被整包归到 assets 档、默认门禁里就再也看不到它们。
 *
 * ★ 全程纯 JS，不 spawn。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { disassemble, assemble, OPCODE_TABLE } from '../src/asm/index.mts';

test('ASM：指令表只含**格式层**四列（知识层字段不得入库）', () => {
  const table = OPCODE_TABLE;
  assert.ok(Array.isArray(table.entries), '指令表要给出 entries 数组');
  assert.ok(table.entries.length > 500, `指令表条目数应当有几百条，实际 ${table.entries.length}`);
  assert.ok(table.byOpcode instanceof Map, 'byOpcode 是查表入口（argc 决定指令边界）');
  assert.ok(table.byLabel instanceof Map, 'byLabel 供重汇编按助记符查表');

  const ALLOWED = ['opcode', 'argc', 'name', 'aliases'];
  const seen = new Set();
  for (const def of table.entries) {
    // ★ `handler`（opcode → 引擎函数）与 `status`（核对状态）是**引擎逆向知识**，
    //   不是格式层的东西 ⇒ 本表由 `pnpm tools opcodes derive` 机械派生时已丢弃。
    for (const k of Object.keys(def)) {
      assert.ok(ALLOWED.includes(k), `指令表出现非格式层字段 ${k}（知识层字段不得入库）`);
    }
    for (const k of ALLOWED) assert.ok(k in def, `条目缺字段 ${k}`);
    assert.ok(Number.isInteger(def.opcode) && def.opcode >= 0, `opcode 非法：${def.opcode}`);
    assert.ok(!seen.has(def.opcode), `opcode 重复：${def.opcode}`);
    seen.add(def.opcode);
    // argc 决定指令边界（一条 = 4 + argc*8 字节）⇒ 必须是合法小整数
    assert.ok(Number.isInteger(def.argc) && def.argc >= 0 && def.argc <= 255, `argc 非法：${def.argc}`);
    assert.equal(typeof def.name, 'string');
    assert.ok(Array.isArray(def.aliases));
  }
});

test('ASM：坏输入必须显式抛错，而不是产出"看似成功的文本"', () => {
  // 头签名不对（既不是 v4 也不是 v5）⇒ 必须抛
  assert.throws(() => disassemble(Buffer.alloc(64, 0x11)), /header|签名|signature|version/i);
  // 太短 ⇒ 必须抛
  assert.throws(() => disassemble(Buffer.alloc(4)));
});

test('ASM：非脚本的 BIN（SYS4INI.BIN 的头）必须被拒绝', () => {
  // 旧仓对这两个文件同样报 "Could not determine header version!"
  const fake = Buffer.from(`S4IC450 ${'\0'.repeat(64)}`, 'latin1');
  assert.throws(() => disassemble(fake));
});

// `assemble` 在本文件里没有直接用例（它由 asm.assets.test.mjs 的往返判据覆盖）——
// 这里留一个 import 会触发 lint 噪声，所以显式引用一次，证明它确实是本包的公开入口。
test('ASM：入口齐备（`disassemble` / `assemble` 都是函数）', () => {
  assert.equal(typeof disassemble, 'function');
  assert.equal(typeof assemble, 'function');
});
