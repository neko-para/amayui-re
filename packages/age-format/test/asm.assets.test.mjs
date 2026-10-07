/** @env assets @kind contract @why 脚本反汇编→重汇编不再逐字节相同（原始游戏文件被换/坏了） */
/**
 * packages/age-format/test/asm.assets.test.mjs —— ASM 的**往返判据**（要原始游戏文件，不入库）
 *
 * ★ 样本不入库：`loadSample` 拿不到就**如实 skip**（`{ skip }`，不是裸 return —— 那会被记成 pass）。
 *   来源与哈希口径见 `corpus/assets/samples.md`。
 * ★ 从 `asm.test.mjs` 拆出来的原因（本轮测试分级）：这三条要**游戏文件**在场，属 `@env assets`。
 *
 * 判据一句话：`assemble(disassemble(bin))` 必须与 `bin` **逐字节相同** —— 这是"BIN 是文本的可逆像"的证人。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { disassemble, assemble, bytesEqual } from '../src/asm/index.mts';
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
    assert.ok(bytesEqual(back, f.buf), `${name} 重汇编必须逐字节相同`);
    assert.equal(sha256(back), f.sha256, `${name} 与清单登记 sha256 一致`);
  }
});

test('ASM：反汇编是确定性的（同输入两次 ⇒ 同文本）', { skip }, () => {
  const f = fileOf(sample, 'PLINIT.BIN');
  assert.equal(disassemble(f.buf), disassemble(Buffer.from(f.buf)));
  const t1 = disassemble(f.buf);
  // ★ 这里原先写的是 `assemble(t1).toString('hex')` —— `assemble()` 现在返回 **`Uint8Array`**，
  //   而 `Uint8Array.prototype.toString()` 是 `"1,2,3"`（不是 hex）⇒ 那样写会**静默退化**成
  //   "自己和自己比逗号串"（仍然绿，但不再是比较字节）。见 `src/asm/bytes.mts` 头注陷阱 1。
  const hexOf = (b) => Buffer.from(b).toString('hex');
  assert.equal(hexOf(assemble(t1)), hexOf(assemble(t1)));
});

test('ASM：极短脚本也不崩（最小的那个样本只有 14 行反汇编）', { skip }, () => {
  const f = fileOf(sample, '$1$OFINIT.BIN');
  const text = disassemble(f.buf);
  assert.ok(text.split('\n').length >= 1);
  assert.ok(bytesEqual(assemble(text), f.buf));
});
