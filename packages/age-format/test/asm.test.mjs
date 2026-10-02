/**
 * packages/age-format/test/asm.test.mjs —— ASM（AGE 脚本）的**往返判据**：反汇编 → 重汇编必须逐字节相同
 *
 * ★ 样本不入库（原始游戏文件）：`loadSample` 拿不到就**跳过**。
 * ★ 全程纯 JS，不 spawn。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { disassemble, assemble, loadOpcodeTable } from '../src/asm/index.mjs';
import { loadSample, fileOf, sha256 } from './samples.mjs';

const sample = loadSample('assets/samples-asm');
if (!sample) console.log('[ASM] 跳过：样本不在场（原始游戏文件不入库，见 corpus/assets/samples.md）');
const skip = sample ? false : '样本不在场（游戏文件不入库）';

const NAMES = ['$1$OFINIT.BIN', 'READICON.BIN', 'PLINIT.BIN', 'INFOFA.BIN'];

test('ASM：四个样本 反汇编 → 重汇编 逐字节相同', { skip }, () => {
  for (const name of NAMES) {
    const f = fileOf(sample, name);
    const text = disassemble(f.buf);
    assert.ok(text.length > 0, `${name} 反汇编不该为空`);
    const back = assemble(text);
    assert.equal(back.length, f.buf.length, `${name} 重汇编长度必须一致（原始 ${f.buf.length}）`);
    assert.ok(back.equals(f.buf), `${name} 重汇编必须逐字节相同`);
    assert.equal(sha256(back), f.sha256, `${name} 与清单登记 sha256 一致`);
  }
});

test('ASM：反汇编是确定性的（同输入两次 ⇒ 同文本）', { skip }, () => {
  const f = fileOf(sample, 'PLINIT.BIN');
  assert.equal(disassemble(f.buf), disassemble(Buffer.from(f.buf)));
  const t1 = disassemble(f.buf);
  assert.equal(assemble(t1).toString('hex'), assemble(t1).toString('hex'));
});

test('ASM：指令表只含**格式层**四列（知识层字段不得入库）', () => {
  const table = loadOpcodeTable();
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

test('ASM：极短脚本也不崩（最小的那个样本只有 14 行反汇编）', { skip }, () => {
  const f = fileOf(sample, '$1$OFINIT.BIN');
  const text = disassemble(f.buf);
  assert.ok(text.split('\n').length >= 1);
  assert.ok(assemble(text).equals(f.buf));
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
