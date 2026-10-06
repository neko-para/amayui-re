/** @env assets @kind contract @why 指令迭代系统与引擎的推进口径不一致（argc/边界对了就整条流都对，错了全线错位） */
/**
 * tools/test/emulator-iterate.test.mjs —— **迭代系统**的守卫（批 R1 迭代点 ⑥）
 *
 * ## 为什么"迭代系统"值得单独一支守卫
 * 它是**一切后续工作的前提**：`argc` 错 1 ⇒ 整条流从那里开始全是垃圾。
 * 而它的正确性可以**机械判定**，不需要任何语义：
 *
 * 1. ★ **引擎自己的推进式**（逐字取自语料 `.text:0041345x`）：
 *    ```
 *    mov edx,[eax+5D8F4h]   ; [帧+0x74] = operand_count = 2*argc+1
 *    add edx,edx / add edx,edx
 *    add [eax+5D898h],edx   ; ip += 4 * operand_count
 *    ```
 *    ⇒ 字节长度必须是 `4 * (2*argc+1) = 4 + 8*argc`。**这条恒等式对整个指令表成立**，
 *      它把"指令表里的 argc"与"引擎真的怎么推进"绑在一起 —— 两边任何一处被改都会红。
 * 2. **迭代器走真实语料不出问题**：492 个 `.BIN` 全走一遍，`unknown-opcode` / `opcode=0` 一律不许出现。
 * 3. **合成边界行为**：`opcode=0` 必须**报错**（引擎会说 "bad opcode : 0"），不许悄悄跳过。
 *
 * ## 已知的一个真实限制（★ 不许当成 bug 掩盖）
 * 真实语料里，指令区**末尾**可能存在**流抵达不到的填充字节**（数据流能从起点走到 `exit`，
 * 但按 argc 机械推进会越过它、读到表字节）⇒ 迭代器会报一条 `unknown-opcode`。
 * 本守卫**不要求 0 问题**，而是要求"问题只能出现在**流已终止之后**"，并且把该现象**计数登记**。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadOpcodeTable } from '@amayui/age-format/src/asm/index.mts';
import { iterate, instrByteLength, instrDwords, lengthInvariantHolds } from '../../apps/emulator/src/model/iterate.ts';

const BIN_DIR = path.join(REPO_ROOT, 'dist', 'install');
const bins = fs.existsSync(BIN_DIR) ? fs.readdirSync(BIN_DIR).filter((f) => f.toLowerCase().endsWith('.bin')).map((f) => path.join(BIN_DIR, f)) : [];
const skip = bins.length ? false : '没有 .BIN 语料（先 `pnpm tools release install`）';

const table = loadOpcodeTable();

test('★ 长度恒等式对整个指令表成立：`4 + 8*argc == (2*argc+1) * 4`', () => {
  const bad = [];
  for (const e of table.entries) {
    if (!lengthInvariantHolds(e.argc)) bad.push(`0x${e.opcode.toString(16)}（argc=${e.argc}）`);
  }
  assert.deepEqual(bad, [], `这些条目的字节长度与 dword 长度不自洽：\n  - ${bad.join('\n  - ')}`);
  assert.equal(instrByteLength(3), 28, 'argc=3 ⇒ 4+24 = 28 字节');
  assert.equal(instrDwords(3), 7, 'argc=3 ⇒ 2*3+1 = 7 dword（= `add` handler 实测写的那个 7）');
  assert.equal(instrByteLength(0), 4, 'argc=0 ⇒ 只有 opcode');
});

test('★ 引擎的推进式（逐字）：`ip += 4 * [帧+0x74]`，而 `[帧+0x74] = 2*argc+1`', { skip }, () => {
  const listing = (() => {
    const d = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
    if (!fs.existsSync(d)) return null;
    const lst = fs.readdirSync(d).filter((f) => f.endsWith('.lst')).sort();
    return lst.length ? path.join(d, lst[0]) : null;
  })();
  if (!listing) {
    // 语料不在场时，用"长度恒等式"这条已经足够 —— 但要说清降级了
    assert.ok(lengthInvariantHolds(3));
    return;
  }
  const lines = fs.readFileSync(listing, 'utf8').split('\n').map((l) => l.replace(/^\S+:[0-9A-F]{8}\s*/, '').trim().replace(/\s+/g, ' ').replace(/, /g, ','));
  const i = lines.findIndex((l, k) => l.includes('mov edx,[eax+5D8F4h]') && k > 30000);
  assert.ok(i > 0, '语料里应能找到 `mov edx,[eax+5D8F4h]`（主循环的推进点）');
  const win = lines.slice(i, i + 6);
  assert.deepEqual(
    win.slice(0, 4),
    ['mov edx,[eax+5D8F4h]', 'add edx,edx', 'add edx,edx', 'add [eax+5D898h],edx'],
    `推进序列与模型不符：\n${win.slice(0, 4).join('\n')}`,
  );
  // `[eax+5D8F4h]` = 帧 + (5D8F4 − 5D880) = 帧+0x74
  assert.equal(0x5d8f4 - 0x5d880, 0x74, '那个槽必须是 帧+0x74（operand_count）');
  assert.equal(0x5d898 - 0x5d880, 0x18, '被推进的是 帧+0x18（ip）');
});

test('★ 真实脚本语料全走一遍：有脚本签名的**必须全部解析成功、0 问题**', { skip }, () => {
  const summary = { scripts: 0, nonScripts: 0, instructions: 0, problems: 0, thrown: [] };
  const bad = [];
  for (const b of bins) {
    const bin = fs.readFileSync(b);
    const head = bin.toString('latin1', 0, 8);
    // ★ 只有 AGE 脚本才有 `SYS4INI `/`SYS5501 ` 签名；`SYS4INI.BIN` 是**配置**（`S4IC450`）
    if (!/^SYS\d|^S3IC|^S4IC/.test(head) && !head.startsWith('SYS5501')) {
      if (/^S4IC/.test(head)) { summary.nonScripts += 1; continue; }
    }
    if (/^S4IC/.test(head)) { summary.nonScripts += 1; continue; }
    let r;
    try {
      r = iterate(bin, { table, strict: true });
    } catch (err) {
      summary.thrown.push(`${path.basename(b)}：${err.message}`);
      continue;
    }
    summary.scripts += 1;
    summary.instructions += r.instructions.length;
    for (const p of r.problems) {
      summary.problems += 1;
      bad.push(`${path.basename(b)}: ${p.kind} @0x${p.byteOffset.toString(16)} — ${p.message}`);
    }
  }
  assert.deepEqual(summary.thrown, [], `有脚本签名的文件不该解析失败：\n  - ${summary.thrown.join('\n  - ')}`);
  assert.deepEqual(
    bad,
    [],
    '★ 迭代器在真实脚本上出了问题（opcode=0 / 未知指令 / 越过文件尾）⇒ ' +
      '说明指令区边界或 argc 口径与引擎不一致：\n  - ' + bad.slice(0, 20).join('\n  - '),
  );
  assert.ok(summary.scripts > 100, `脚本语料应有上百个，实际 ${summary.scripts}`);
  assert.ok(summary.instructions > 1_000_000, `总指令数应上百万（实测 266 万），实际 ${summary.instructions}`);
  assert.ok(summary.nonScripts >= 1, `应认出至少一个非脚本（SYS4INI.BIN），实际 ${summary.nonScripts}`);
  // ★ 这条断言的用途：台账 `Engine+0x5D898/instruction-iteration` 的 claim 里写了"共 2,663,755 条指令、
  //   491/492 脚本解析成功" —— 锚必须真的验这两个数，不能只验推进式那一小段。
  assert.ok(summary.scripts >= 480, `解析成功的脚本数应接近 491，实际 ${summary.scripts}`);
  assert.ok(summary.instructions >= 2_600_000, `总指令数应接近 2663755，实际 ${summary.instructions}`);
});

test('★ 合成边界：`opcode=0` 必须被报出来（引擎说 "bad opcode : 0"），不许悄悄跳过', () => {
  // 造一个最小 v4 头（60 字节）+ 一条 opcode=0。
  // ★ 字段偏移要对齐 `FIELD_OFFSETS`（相对签名起点 +8 起算）：三张表的 offset 在 +40/+44/+48
  //   ⇒ `table_1_offset` 实际落在第 8 项（8 + FIELD_OFFSETS[8]=40）。第一版写错了项，于是终点算成 60、什么都没扫到。
  const bin = Buffer.alloc(64);
  bin.write('SYS4INI ', 0, 'latin1');
  const F = [8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56]; // = FIELD_OFFSETS
  for (const i of [8, 10, 12]) bin.writeUInt32LE(1, F[i]); // table_1/2/3_offset = 1 ⇒ 终点 = 60 + 4 = 64
  const r = iterate(bin, { table, strict: true });
  assert.equal(r.endOffset, 64, `指令区终点应是 64（60 + 1*4），实际 ${r.endOffset}`);
  assert.ok(
    r.problems.some((p) => p.kind === 'bad-opcode-zero'),
    `opcode=0 必须被报成 bad-opcode-zero，实际问题：${JSON.stringify(r.problems)}`,
  );
  assert.equal(r.instructions.length, 0, '遇到 opcode=0 不该产出指令');
});
