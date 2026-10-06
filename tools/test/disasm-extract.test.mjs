/** @env pure @kind gate @why 语料索引/跳转表提取在**合成语料**上的判据坏了（段名/表体位置/连续性） */
/**
 * tools/test/disasm-extract.test.mjs —— 提取器的**纯函数守卫**（合成语料，不需要真语料）
 *
 * 为什么必须有这一份（与 `disasm.test.mjs` 的分工）：`disasm.test.mjs` 是 `@env assets`，要用 17.6 MB 真语料；
 * 而提取器里最容易错的几件事**在合成语料上就能判**，而且**不依赖机器上有没有解压**：
 *
 *   ① **段名可以以 `.` 开头**（`.text` / `.data`）。这个坑我在同一个文件里犯了**两次**
 *      （`PREFIX_RE` 与索引解析正则都写成 `^([A-Za-z_]…`）⇒ `.text` 40 万行 + `.data` 7.6 万行**整段静默丢失**，
 *      表现是"`--at <该段地址>` 说这个地址不在覆盖范围内"——**听起来像正常的边界提示**。
 *   ② **跳转表体不在函数的行区间里**（它在另一个段、离函数几万行）⇒ 查表必须按全文查。
 *      第一版在函数 span 里查，于是 `cases` 恒为 0。
 *   ③ **表体只有第一行带 `jpt_XXXXXX` 标签**，其余是裸 `dd offset` ⇒ 判据是"首行锚定 + 连续吃"，
 *      不是"逐行找带标签的行"（那样只会数出 1）。
 *   ④ 段的出现顺序**不是字典序**（`.text → .data → seg002 → …`）⇒ `EA → 行号` 只能在**段的切片内**二分。
 *
 * 运行：`pnpm test`（纯函数，零外部依赖）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { buildIndex, contextAt, indexProblems, parseIndexText, serializeIndex, switchTables, withSegRanges } from '../lib/disasm.mjs';

/**
 * 造一份"像 IDA 导出"的小语料：两个段、名字以 `.` 开头、段序非字典序，外加一个跳转表与它的表体。
 * ★ 前缀必须**从列 0 开始**（真实语料就是这样：`PREFIX_RE` 用 `^` 锚定；IDA 不缩进这一列）。
 */
function makeListing() {
  const L = [
    '.text:00401000 ;',
    '.text:00401000 ; Imagebase   : 400000',
    '.text:00401000 ;',
    '.text:00401000 .686p',
    // 函数与一个 switch
    '.text:00401030 sub_401030 proc near',
    '.text:00401030 mov     ecx, [eax-4]',
    '.text:00401035 sub     ecx, 3          ; switch 3 cases',
    '.text:0040103A cmp     ecx, 2',
    '.text:0040103D ja      def_401044      ; jumptable 00401044 default case',
    '.text:00401044 jmp     ds:jpt_401044[ecx*4] ; switch jump',
    '.text:0040104B loc_40104B:             ; jumptable 00401044 case 3',
    '.text:0040104B retn',
    '.text:0040104C loc_40104C:             ; jumptable 00401044 case 4',
    '.text:0040104C retn',
    '.text:0040104D loc_40104D:             ; jumptable 00401044 case 5',
    '.text:0040104D retn',
    '.text:0040104E sub_40104E endp',
    // 另一个段（名字不以点开头），段序在 .text 之后但不是字典序
    'seg002:00550000 db 1',
    'seg002:00550004 db 2',
    // 表体在**同一个 .text 段里但离函数很远**（模拟"不在函数行区间内"）
    '.text:00402000 align 4',
    '.text:00402000 jpt_401044 dd offset loc_40104B ; jump table for switch statement',
    '.text:00402004 dd offset loc_40104C',
    '.text:00402008 dd offset loc_40104D',
    '.text:0040200C align 10h',
  ];
  const p = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-disasm-')), 'fake.lst');
  fs.writeFileSync(p, `${L.join('\n')}\n`);
  return p;
}

test('★ 段名以 `.` 开头也要被索引（漏掉 = 整段静默丢失，且提示听起来像正常边界）', () => {
  const file = makeListing();
  const idx = buildIndex(file);
  const names = idx.segs.map((s) => s.name);
  assert.deepEqual(names.sort(), ['.text', 'seg002'].sort(), `段表必须含 .text 与 seg002，实际 ${names.join(' ')}`);
  assert.deepEqual(indexProblems(idx), [], '索引必须自洽（每个段都要有标记）');
  // 每个段的起点都要取得到上下文，且首行前缀写着这个 EA
  for (const s of idx.segs) {
    const r = contextAt(file, idx, s.lo, { lines: 1 });
    assert.ok(r.fromLine > 0, `段 ${s.name} 的起点必须取得到上下文`);
    assert.ok(r.lines[0].includes(s.lo.toString(16).padStart(8, '0')), `首行应写着该 EA：${r.lines[0]}`);
  }
});

test('★ 段内二分：段的文件顺序不是字典序（.text 之后是 seg002），跨段查询不许串味', () => {
  const file = makeListing();
  const idx = buildIndex(file);
  // 段内可查
  assert.ok(contextAt(file, idx, 0x401030, { lines: 1 }).fromLine > 0, '.text 内应可查');
  assert.ok(contextAt(file, idx, 0x550004, { lines: 1 }).fromLine > 0, 'seg002 内应可查');
  // 段间空洞必须**明说**，不许拿邻段的最近一条顶上
  const hole = contextAt(file, idx, 0x500000, { lines: 1 });
  assert.equal(hole.lines.length, 0);
  assert.match(hole.hint ?? '', /不落在任何已索引段内/, `段间空洞要明说，实际：${hole.hint}`);
  // ★ 显式点名一个段、而 EA 不在它范围内时，要报"不在该段范围内"（两条提示语义不同，别混）
  const over = contextAt(file, idx, 0x550100, { lines: 1, seg: 'seg002' });
  assert.equal(over.lines.length, 0);
  assert.match(over.hint ?? '', /不在段 seg002 的范围内/, `点名段但越界要明说，实际：${over.hint}`);
  // 而不点名时，超出所有段 ⇒ 走"不在任何段内"
  const over2 = contextAt(file, idx, 0x550100, { lines: 1 });
  assert.match(over2.hint ?? '', /不落在任何已索引段内/, `实际：${over2.hint}`);
});

test('★ 跳转表：表体在函数区间**之外**也要找到，且只有首行带标签（连续吃裸 dd offset）', () => {
  const file = makeListing();
  const idx = buildIndex(file);
  const tables = switchTables(file, idx, 0x401030);
  assert.equal(tables.length, 1, `应恰好一条跳转表，实际 ${tables.length}`);
  const t = tables[0];
  assert.equal(t.jptAt, 0x401044);
  assert.equal(t.cases, 3, `case 数应为 3（表体 3 行，只有首行带 jpt_ 标签），实际 ${t.cases}`);
  assert.deepEqual(t.targets.map((g) => g.loc), ['loc_40104B', 'loc_40104C', 'loc_40104D']);
  // 表体确实在函数 endp（行 17）之后 ⇒ 证明"必须按全文查"
  assert.ok(t.tableLine > 17, `表体应在函数区间之外，实际行 ${t.tableLine}`);
});

test('★ 索引落盘/读回同构（派生缓存的性质：可删可重建、且读回来判据不丢）', () => {
  const file = makeListing();
  const a = buildIndex(file);
  const text = serializeIndex(a);
  assert.equal(serializeIndex(buildIndex(file)), text, '同一份语料两次建索引必须同字节');
  const back = parseIndexText(text);
  assert.equal(back.marks.length, a.marks.length, '标记数要一致');
  assert.deepEqual(back.segs.map((s) => s.name), a.segs.map((s) => s.name), '段表要一致（含以 . 开头的段名）');
  // 从缓存读回来的段表也要能补出 marks 下标区间（不补 ⇒ 非 .text 段的查询会静默给错）
  const segs = withSegRanges(back.segs, back.marks);
  for (const s of segs) assert.ok(s.markLo >= 0 && s.markHi >= s.markLo, `段 ${s.name} 应补出标记区间`);
  const idx2 = { file, lines: back.lines, bytes: back.bytes, sha256: back.sha256, imagebase: back.imagebase, segs, marks: back.marks, monotonic: true };
  assert.ok(contextAt(file, idx2, 0x550004, { lines: 1 }).fromLine > 0, '从缓存读回的索引也要能查 seg002');
});
