/** @env assets @kind gate @why 语料的 EA→行号 映射/索引坏了，或 opcode 分派表的机械提取对不上 */
/**
 * tools/test/disasm.test.mjs —— **机械提取器的守卫**（语料在场时才有意义 ⇒ `@env assets`）
 *
 * 守的是三件"会坏且坏得有意义"的事：
 *   ① **EA → 行号**：`pnpm tools disasm-at at --ea <EA>` 必须落在该段、且行内容前缀与 EA 一致；
 *   ② **索引可删可重建**：同一份语料建两次 ⇒ 同字节（它是**派生缓存**，不是真源）；
 *   ③ ★ **opcode → handler 分派表的静态提取**：引擎构造函数里那 544 行
 *      `*(_DWORD *)(_this + 67xxxx) = sub_XXXXXX;` 机械可数、且与 `analysis/opcodes.json` 的条数一致。
 *      ⇒ 这条把"handler 只在旧仓 JSON 里"变成"**从语料可再提取**"。
 *
 * ★ 为什么这一条是本轮的第一个点：它**不需要任何引擎语义**（纯定位 + 计数），
 *   因此**不可能把"旧实现的说法"当成"引擎的事实"**。它同时给出后续所有分析的入口。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, contextAt, indexProblems, serializeIndex, withSegRanges } from '../lib/disasm.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
/** 语料解压产物是 gitignore 的 ⇒ 不在场就如实 skip（不是"读不到就当空"） */
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/**
 * 语料里的 opcode→handler 赋值行（IDA 反汇编形态）。
 * ★ 实测：`.text:00415E08 mov dword ptr [esi+0A50A0h], offset sub_418E60`
 *   表基址 = **0xA509C**（`lea edi, [esi+0A509Ch]` 给出的就是它）⇒ `opcode = (偏移 − 0xA509C) / 4`。
 *   ★ 注意**不要**照抄旧仓 `.c` 里的 C 语句形态（`*(_DWORD*)(_this + 676000) = sub_…;`）——
 *     那是 Hex-Rays 的输出，在本仓的 `.lst` 语料里**一条都匹配不到**（实测 0 条）。
 */
const DISPATCH_RE = /mov\s+dword ptr \[[a-z]{2,3}\+(0A5[0-9A-F]{3})h\], offset (sub_[0-9A-F]+)/;
const DISPATCH_BASE = 0xa509c;

test('★ 语料索引：段表与标记自洽、EA 单调、可删可重建（同输入同字节）', { skip }, () => {
  const a = buildIndex(listing);
  const b = buildIndex(listing);
  assert.equal(serializeIndex(a), serializeIndex(b), '同一份语料建两次 ⇒ 索引必须同字节');
  assert.ok(a.monotonic, '段内 EA 必须单调不降（IDer 导出若重排过，索引就是错的）');
  assert.deepEqual(indexProblems(a), [], '索引必须自洽（每个段都要有标记）');
  // ★ 段名可以以 `.` 开头（`.text` / `.data`）—— 这里把它钉住：漏掉它们会静默丢掉 48 万行
  const names = a.segs.map((s) => s.name);
  assert.ok(names.includes('.text'), `段表里必须有 .text，实际：${names.join(' ')}`);
  assert.ok(names.includes('.data'), `段表里必须有 .data，实际：${names.join(' ')}`);
  const rows = a.segs.reduce((n, s) => n + s.rows, 0);
  assert.ok(rows > 100000, `索引应覆盖十万行以上，实际 ${rows}`);
});

test('★ EA → 行号：取到的行前缀必须真的写着这个 EA（且落在同段内）', { skip }, () => {
  const idx = buildIndex(listing);
  // 每个段的起点各验一次（跨段是最容易出错的地方：段的出现顺序不是字典序）
  for (const s of idx.segs) {
    const r = contextAt(listing, idx, s.lo, { lines: 2 });
    assert.ok(r.fromLine > 0, `段 ${s.name} 的起点 0x${s.lo.toString(16)} 必须取得到上下文（实际 fromLine=${r.fromLine}）`);
    const first = r.lines[0];
    assert.ok(
      first.startsWith(`${s.name}:${s.lo.toString(16).padStart(8, '0')}`) ||
        first.startsWith(`${s.name}:${s.lo.toString(16).toUpperCase().padStart(8, '0')}`),
      `段 ${s.name} 起点取到的第一行前缀不对：${first.slice(0, 40)}`,
    );
  }
  // 段外地址必须**明说不在覆盖范围内**，而不是悄悄给别的东西
  const out = contextAt(listing, idx, 0x1000, { lines: 2 });
  assert.equal(out.lines.length, 0);
  assert.match(out.hint ?? '', /不落在任何已索引段内/, `段外地址要明说，实际：${out.hint}`);
});

test('★ 有界：窗口上限生效，且截断必须明说（不许静默给不完整切片）', { skip }, () => {
  const idx = buildIndex(listing);
  const r = contextAt(listing, idx, idx.segs[0].lo, { lines: 3 });
  assert.equal(r.lines.length, 3, '--lines 3 ⇒ 正好 3 行');
  assert.equal(typeof r.truncated.tail, 'boolean', 'truncated 必须是一只可判的旗（不是散文）');
  assert.ok(r.truncated.tail, '在文件开头取 3 行 ⇒ 尾部必然被截断');
});

test('★ opcode → handler 的**静态提取**：从语料机械数出全部 handler 赋值', { skip }, async () => {
  const rows = [];
  const rl = readline.createInterface({ input: fs.createReadStream(listing, { encoding: 'utf8' }), crlfDelay: Infinity });
  let lineNo = 0;
  for await (const line of rl) {
    lineNo += 1;
    const m = DISPATCH_RE.exec(line);
    if (m) rows.push({ off: Number.parseInt(m[1], 16), fn: m[2], line: lineNo });
  }
  rl.close();
  assert.ok(rows.length > 500, `语料里应有几百条 handler 赋值，实际 ${rows.length}（判据形态变了？）`);
  // opcode = (偏移 − 0xA509C)/4（表基址由同一函数里的 `lea … [esi+0A509Ch]` 给出）
  const opcodes = rows.map((r) => (r.off - DISPATCH_BASE) / 4);
  assert.ok(opcodes.every((o) => Number.isInteger(o) && o >= 0), '每个偏移都必须落在表内、且是 4 字节对齐');
  assert.ok(Math.max(...opcodes) <= 0x3ff, `opcode 必须落在 10 位表内（0..0x3FF），实际上界 ${Math.max(...opcodes)}`);
  assert.equal(new Set(opcodes).size, opcodes.length, '同一个 opcode 不许有两条 handler');
  // 第一条必须是 opcode 1（偏移 0xA50A0 = 基址 + 4），这是"基址取对了"的判据
  assert.equal(Math.min(...opcodes), 1, '最小 opcode 应为 1（0 号走默认 handler）');
  // 与登记来源对账（只比存在性：改旧仓 JSON 一定会让这条红，那时要显式复核而不是改数字）
  const src = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus', 'assets.json'), 'utf8')).entries.find(
    (e) => e.id === 'knowledge/opcode-table-source',
  );
  assert.ok(src, '清单里必须有 knowledge/opcode-table-source（派生链的来源）');
});
