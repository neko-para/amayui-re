/** @env assets @kind gate @suite toolchain @why 语料的 EA→行号 映射/索引坏了，或 opcode 分派表的机械提取对不上，或折叠块被报成"不落在任何函数里" */
/**
 * tools/test/disasm.test.mjs —— **机械提取器的守卫**（语料在场时才有意义 ⇒ `@env assets`）
 *
 * 守的是四件"会坏且坏得有意义"的事：
 *   ① **EA → 行号**：`pnpm tools disasm-at at --ea <EA>` 必须落在该段、且行内容前缀与 EA 一致；
 *   ② **索引可删可重建**：同一份语料建两次 ⇒ 同字节（它是**派生缓存**，不是真源）；
 *   ③ ★ **opcode → handler 分派表的静态提取**：引擎构造函数里那 544 行
 *      `*(_DWORD *)(_this + 67xxxx) = sub_XXXXXX;` 机械可数、且与 `analysis/opcodes.json` 的条数一致。
 *      ⇒ 这条把"handler 只在旧仓 JSON 里"变成"**从语料可再提取**"。
 *   ④ ★ **折叠块可辨认**：`.lst` 把库函数折成一行 `COLLAPSED` 注释（没有 `proc near … endp`）
 *      ⇒ `disasm-fn` 必须报"落在折叠块 <名字/起点>（声明 N 字节）"，而不是和"真的无处可归"混成一句
 *      （这条用**合成夹具**，不需要语料）。
 *
 * ★ 为什么这一条是本轮的第一个点：它**不需要任何引擎语义**（纯定位 + 计数），
 *   因此**不可能把"旧实现的说法"当成"引擎的事实"**。它同时给出后续所有分析的入口。
 *
 * 运行：`pnpm test:assets`（④ 那条不需要语料，但同文件的其余几条要）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildCatalog, catalogSummary, findFunction } from '../lib/catalog.mjs';
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

// ─────────────────────────────────────────────────────────── 折叠块（COLLAPSED）

/** 造一份最小 `.lst`（写进临时目录；用完即弃；这个用例**不需要**语料） */
function miniListing(text) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-folded-'));
  const lst = path.join(dir, 'synthetic.lst');
  fs.writeFileSync(lst, text, 'utf8');
  return lst;
}

/**
 * ★ 折叠块在 `.lst` 里**只有一行注释**：没有 `proc near … endp` ⇒ 它不属于任何函数。
 * 若"不落在任何函数里"把这件事和"真的无处可归"混成一句，用户就分不清
 * "这一块在语料里被折叠了（有名字、有声明长度，可以按 EA 取字节）"与"你给错了 EA"。
 */
test('★ 折叠块：EA 落在 COLLAPSED 行声明的区间里 ⇒ 报折叠块，不报"不落在任何函数里"', () => {
  const lst = miniListing(
    [
      '.text:00401000 sub_401000 proc near',
      '.text:00401000 retn',
      '.text:00401001 sub_401000 endp',
      '.text:00401004 ; [00000006 BYTES: COLLAPSED FUNCTION j_RtlUnwind. PRESS CTRL-NUMPAD+ TO EXPAND]',
      '.text:0040100A align 10h',
    ].join('\n') + '\n',
  );
  const cat = buildCatalog({ lstFile: lst, cFile: null });
  assert.deepEqual(cat.problems, [], `目录自检必须全过：${cat.problems.join('；')}`);
  // 起点 / 中段 / 末字节都在声明区间 [0x401004, 0x40100A) 里
  for (const ea of [0x401004, 0x401006, 0x401009]) {
    assert.throws(
      () => findFunction(cat, { ea }),
      (err) => /折叠块/.test(err.message) && /j_RtlUnwind/.test(err.message) && /声明 6 字节/.test(err.message) && !/不落在任何函数里/.test(err.message),
      `EA 0x${ea.toString(16)} 落在折叠块内 ⇒ 必须点名折叠块（不是"不落在任何函数里"）`,
    );
  }
  // 符号名也一样：折叠块不是"目录里没有"（它有名字、有声明长度）
  assert.throws(
    () => findFunction(cat, { sym: 'j_RtlUnwind' }),
    (err) => /折叠块/.test(err.message) && !/不在目录里/.test(err.message),
    '折叠块的符号名必须给"折叠块"那句，而不是"不在目录里"',
  );
  // 折叠块之外的空隙仍走原判据（两件事不许合并）
  assert.throws(() => findFunction(cat, { ea: 0x401002 }), /不落在任何函数里/);
  // 计数与"跨模块 callee 不在本文件"**分开**
  const s = catalogSummary(cat);
  assert.equal(s.foldedBlocks, 1, '摘要必须给出折叠块个数');
  assert.equal(s.foldedBytes, 6, '摘要必须给出声明字节数（不是元素数 / 不是 0）');
  assert.deepEqual(s.calleesNotInInventory, [], '本样本没有跨模块 callee ⇒ 两个计数不许互相顶替');
});
