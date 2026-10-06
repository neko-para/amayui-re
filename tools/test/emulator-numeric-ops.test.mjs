/** @env assets @kind contract @why 纯数值指令族的成员/handler/出现次数与语料不一致（族边界被改坏） */
/**
 * tools/test/emulator-numeric-ops.test.mjs —— **纯数值指令族**的守卫（批 R1 迭代点 ⑤）
 *
 * 三条独立判据：
 *   ① **handler 逐条对账**：模型里每个 handler 都要与语料**现算**出来的分派表一致
 *      （`mov dword ptr [esi+0A50xxh], offset sub_XXXXXX`，基址 `0xA509C`）。
 *   ② **argc 与指令表一致**：argc 决定指令边界（`p += 4 + 8*argc`）⇒ 它错了整条流都会错位。
 *   ③ **静态出现次数可复算**：在 `.BIN` 语料上按 argc 走一遍数出来。★ 这批数里**有 9 个是 0** ——
 *      "语料里一条都没有"是很强的判据（动这些指令只影响推理、不影响产物）。
 *
 * ★ 本守卫**故意不判语义**（`add` 到底是不是加法）：那需要逐条读 handler 体，属下一步；
 *   语义未解的条目在模型里标了 `semanticsUnknown`，守卫只确保它们**没被偷偷写成已知**。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

import { REPO_ROOT } from '../lib/paths.mjs';
import { NUMERIC_OPS, SEMANTICS_UNKNOWN, UNUSED_IN_CORPUS } from '../../apps/emulator/src/model/numeric-ops.ts';
// ★ handler 名是**逆向观察**（哪段代码实现了它）⇒ 来自知识层，不在模拟器里。
//   本守卫就是"布局/观察 ↔ 语义"的对账方：拿知识层的 handler 表回语料现算复核。
import { OPCODE_HANDLERS } from '@amayui/age-format/src/engine/handlers.mts';

const DISPATCH_BASE = 0xa509c;
const RE_DISPATCH = /mov\s+dword ptr \[[a-z]{2,3}\+(0A5[0-9A-F]{3})h\], offset (sub_[0-9A-F]+)/;

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const BIN_DIR = path.join(REPO_ROOT, 'dist', 'install');
const bins = fs.existsSync(BIN_DIR) ? fs.readdirSync(BIN_DIR).filter((f) => f.toLowerCase().endsWith('.bin')).map((f) => path.join(BIN_DIR, f)) : [];
const skip = listing && bins.length ? false : '语料未解压或没有 .BIN（先 `pnpm tools disasm build` / `pnpm tools release install`）';

/** 现算分派表（opcode → handler） */
async function dispatchTable() {
  const map = new Map();
  const rl = readline.createInterface({ input: fs.createReadStream(listing, { encoding: 'utf8' }), crlfDelay: Infinity });
  for await (const line of rl) {
    const m = RE_DISPATCH.exec(line);
    if (m) map.set((Number.parseInt(m[1], 16) - DISPATCH_BASE) / 4, m[2]);
  }
  rl.close();
  return map;
}

/** 指令表（argc 的真源） */
const table = (() => {
  const p = path.join(REPO_ROOT, 'packages', 'age-format', 'src', 'asm', 'instruction-set.json');
  const raw = JSON.parse(fs.readFileSync(p, 'utf8'));
  return new Map(Object.values(raw).map((e) => [Number(e.opcode), e]));
})();

test('★ 纯数值族：每条 handler 必须与语料现算的分派表**逐条一致**', { skip }, async () => {
  const disp = await dispatchTable();
  assert.ok(disp.size > 500, `分派表应当有几百条，实际 ${disp.size}`);
  const bad = [];
  for (const op of NUMERIC_OPS) {
    const real = disp.get(op.opcode);
    if (!real) bad.push(`0x${op.opcode.toString(16)}（${op.name ?? '?'}）在语料分派表里没有`);
    else if (real.toLowerCase() !== OPCODE_HANDLERS[op.opcode]?.toLowerCase()) bad.push(`0x${op.opcode.toString(16)}：观察层记的 ${OPCODE_HANDLERS[op.opcode]} ≠ 语料现算的 ${real}`);
  }
  assert.deepEqual(bad, [], `handler 与语料不一致（改模型或改语料都要先复核）：\n  - ${bad.join('\n  - ')}`);
});

test('★ 纯数值族：argc 必须与指令表一致（argc 错了整条指令流都会错位）', () => {
  const bad = [];
  for (const op of NUMERIC_OPS) {
    const e = table.get(op.opcode);
    if (!e) { bad.push(`0x${op.opcode.toString(16)} 不在指令表里`); continue; }
    if (Number(e.argc) !== op.argc) bad.push(`0x${op.opcode.toString(16)}：模型 argc=${op.argc} ≠ 指令表 ${e.argc}`);
    if ((e.name ?? null) !== op.name) bad.push(`0x${op.opcode.toString(16)}：模型 name=${JSON.stringify(op.name)} ≠ 指令表 ${JSON.stringify(e.name)}`);
  }
  assert.deepEqual(bad, [], `argc/name 与指令表不一致：\n  - ${bad.join('\n  - ')}`);
});

test('★ 静态出现次数可复算：0 次的那批必须真的是 0（不是"没数到"）', { skip }, () => {
  const argc = new Map([...table].map(([op, e]) => [op, Number(e.argc)]));
  const count = new Map();
  for (const b of bins) {
    const buf = fs.readFileSync(b);
    const isVer5 = buf.toString('latin1', 0, 6).includes('SYS5');
    let p = isVer5 ? 64 : 56;
    while (p + 4 <= buf.length) {
      const op = buf.readUInt32LE(p);
      const a = argc.get(op);
      if (a === undefined) { p += 4; continue; }
      count.set(op, (count.get(op) ?? 0) + 1);
      p += 4 + 8 * a;
      if (p > buf.length) break;
    }
  }
  const zeros = NUMERIC_OPS.filter((o) => (count.get(o.opcode) ?? 0) === 0).map((o) => o.opcode);
  assert.deepEqual(
    zeros.sort((a, b) => a - b),
    [...UNUSED_IN_CORPUS].sort((a, b) => a - b),
    '模型声明的"语料里 0 次"与现算结果不一致（语料变了，或模型里的数抄错了）',
  );
  // 模型里的 staticUses 也逐个复核（它是一份**可复算的观察**，不是说明文字）
  const bad = [];
  for (const op of NUMERIC_OPS) {
    const real = count.get(op.opcode) ?? 0;
    if (real !== op.staticUses) bad.push(`0x${op.opcode.toString(16)}：模型 ${op.staticUses} ≠ 现算 ${real}`);
  }
  assert.deepEqual(bad, [], `staticUses 与现算不一致：\n  - ${bad.join('\n  - ')}`);
  assert.ok(bins.length > 100, `.BIN 语料应当有上百个，实际 ${bins.length}`);
});

test('★ 语义未解的条目**不许**在模型里被写成已知（防止"顺手补语义"）', () => {
  for (const op of SEMANTICS_UNKNOWN) {
    const e = NUMERIC_OPS.find((o) => o.opcode === op);
    assert.ok(e, `0x${op.toString(16)} 应在族里`);
    assert.equal(e.semanticsUnknown, true, `0x${op.toString(16)} 必须标 semanticsUnknown`);
    assert.equal(e.semantics, undefined, `0x${op.toString(16)} 不许带 semantics 文本（旧仓是"仅映射"，我们不许猜测）`);
  }
  assert.ok(SEMANTICS_UNKNOWN.length >= 10, '语义未解的条目应当有一批（实测 12 条）');
});

test('★ 族边界：只搬运不做算术的那些**不许**混进来', async () => {
  const { NOT_IN_FAMILY } = await import('../../apps/emulator/src/model/numeric-ops.ts');
  const inFamily = new Set(NUMERIC_OPS.map((o) => o.opcode));
  const overlap = NOT_IN_FAMILY.opcodes.filter((o) => inFamily.has(o));
  assert.deepEqual(overlap.map((o) => `0x${o.toString(16)}`), [], '边界外的 opcode 出现在了纯数值族里');
  assert.ok(NOT_IN_FAMILY.reason.length > 0, '边界要写清理由（不是一份没有解释的名单）');
});
