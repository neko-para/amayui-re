/** @env pure @kind safety @why 翻译 patch 的写路径或锚定坏了 */
/**
 * tools/test/patch.test.mjs — 翻译 patch 的**基建契约**测试
 *
 * 测什么（按 `AGENTS.md` §10 的口径：只测基建，不测业务结论）：
 *   ① **写路径会不会写坏** —— 序列化 ↔ 解析往返等价、一行一个 op、不绿一个字都不写、坏了要回滚；
 *   ② **守卫能不能红** —— `structuralProblems()` 对每条结构不变量都真的能红；
 *   ③ **行序锚定的可再校验性** —— `diffRows()` 产出的 ops 经 `replay()` 之后，
 *      遮蔽 label 应与产物**逐行相同**；`sha8` 对不上时**必须抛**（不许按位置硬套）。
 *
 * 不测什么：真实脚本的重建结果（那是 `pnpm tools patch verify` 的活，需要游戏安装目录在场）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import {
  SPEAKER_FILTER,
  DEFAULT_PATCH,
  DOMAIN,
  VIEW_SCOPES,
  alignRows,
  applyAnchorHunks,
  assemble,
  baseIndexProblems,
  buildBaseIndex,
  canonicalFormProblems,
  canonicalPatch,
  codecContext,
  describe,
  describeText,
  diffRows,
  editRowAtAnchor,
  literalShape,
  loadPatch,
  maskLabels,
  mergeViewManifest,
  mergeStringLiterals,
  parseEditList,
  projectedRows,
  replay,
  rowIndexByLine,
  rowKeyLabel,
  rowsOf,
  savePatch,
  scopeNames,
  serializePatch,
  sha8,
  structuralProblems,
  substituteLiterals,
  symbolize,
  textAtRowKey,
  viewProblems,
  viewToBinText,
} from '../lib/patch.mjs';
import { makeMapper } from '../lib/cn-jp.mjs';

/** 合成一份"反汇编文本"：头部 4 行 + 空行 + 指令/标签行（不需要真的能汇编） */
const dis = (body) =>
  `==Binary Information - do not edit==\nsignature = SYS4450 \nlocal_vars = { f 1 1 6 1 2 }\n====\n\n${body}\n`;

const HEX64 = 'a'.repeat(64);

const docOf = (scripts, subsSha = HEX64) => ({ schemaVersion: 1, subsSha, scripts });
const entryOf = (ops) => ({ baseSha: HEX64, resultSha: 'b'.repeat(64), ops });

// ─────────────────────────────────────────────────────────── ① 行序空间

test('rowsOf：头部 4 行不进序空间；label 定义行被摘出来挂在下一行身上；引用被遮蔽', () => {
  const r = rowsOf(dis('i1f4\nlabel_00000040\nlabel_00000044\ni259 label_00000040\ni258 3 1'));
  assert.equal(r.header.length, 4);
  assert.equal(r.header[1], 'signature = SYS4450 ');
  assert.deepEqual(r.raw, ['i1f4', 'i259 label_00000040', 'i258 3 1']);
  assert.deepEqual(r.masked, ['i1f4', 'i259 label_?', 'i258 3 1']);
  // 两个标签都挂在第 1 行（`i259`）身上
  assert.deepEqual(r.defsBefore[1], [0, 1]);
  assert.deepEqual(r.defsBefore[0], []);
  assert.deepEqual(r.defsBefore[3], []);
  assert.deepEqual([...r.addrToId.keys()].sort(), ['00000040', '00000044']);
});

test('rowsOf：**不要**把 `comment "…"` 当注释滤掉（它是带载荷的指令）', () => {
  const r = rowsOf(dis('i1f4  // 行尾注释会被去掉\ncomment "ループ開始"'));
  assert.deepEqual(r.raw, ['i1f4', 'comment "ループ開始"']);
});

test('symbolize：地址换成符号；未定义的引用要抛（拿不到事实就抛，不猜）', () => {
  const r = rowsOf(dis('label_00000040\ni259 label_00000040'));
  assert.equal(symbolize(r.raw[0], r.addrToId), 'i259 label_100000');
  assert.throws(() => symbolize('i259 label_0000dead', r.addrToId), /未定义的 label 引用/);
});

// ─────────────────────────────────────────────────────────── ③ 锚定的可再校验性

/** 把 diffRows 的产物固化成 patch 的 ops（载荷直接用产物原文，跳过中文映射） */
function opsFrom(a, b) {
  const A = rowsOf(a);
  const B = rowsOf(b);
  const { ops } = diffRows(A.masked, B.masked);
  return ops.map((o) => {
    if (o.op === 'replace-line') return { op: 'replace-line', i: o.i, sha8: sha8(A.masked[o.i]), line: B.raw[o.j] };
    if (o.op === 'delete') return { op: 'delete', i: o.i, sha8: sha8(A.masked[o.i]) };
    return { op: 'insert-after', i: o.i, instr: B.raw[o.j] };
  });
}

test('★ diffRows + replay：打上 patch 后遮蔽 label ⇒ 与产物**逐行相同**', () => {
  const a = dis('i1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1\ni1a7');
  const b = dis('i1f4\ni259\ni258 3 1\ncomment "新的"\ni258 4 1\ni258 5 1\ni1a7\ni1a7');
  const ops = opsFrom(a, b);
  assert.ok(ops.some((o) => o.op === 'insert-after'), '应当识别成插入而不是逐行替换');
  const out = replay(a, ops);
  assert.deepEqual(rowsOf(out.text).masked, rowsOf(b).masked);
});

test('★ diffRows + replay：整段删除 / 整段插入 / 首尾边界都对得上', () => {
  const a = dis('i1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1');
  const b = dis('i259\ni258 4 1\ni258 5 1');
  assert.deepEqual(rowsOf(replay(a, opsFrom(a, b)).text).masked, rowsOf(b).masked);

  const c = dis('i1f4\ni1a7\ni1a7\ni259\ni258 3 1\ni258 4 1\ni258 5 1\ni1a7');
  assert.deepEqual(rowsOf(replay(a, opsFrom(a, c)).text).masked, rowsOf(c).masked);

  // 首部插入：i 会取到 -1
  const d = dis('i1a7\ni1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1');
  const opsD = opsFrom(a, d);
  assert.ok(opsD.some((o) => o.op === 'insert-after' && o.i === -1), '首部插入应当落成 i = -1');
  assert.deepEqual(rowsOf(replay(a, opsD).text).masked, rowsOf(d).masked);
});

test('replay：label 定义跟着被替换的行一起搬（否则会悬空）', () => {
  const a = dis('i1f4\nlabel_00000040\ni259 label_00000040');
  const b = dis('i1f4\nlabel_00000040\ncomment "换了"\ni259 label_00000040');
  const out = replay(a, opsFrom(a, b));
  const rows = rowsOf(out.text);
  // 定义行**必须被认出来**（符号是 6~7 位 hex，不能要求 8 位）
  assert.deepEqual(rows.raw, ['i1f4', 'comment "换了"', 'i259 label_100000']);
  assert.deepEqual(rows.defsBefore[2], [0], '定义必须挂在 i259 那一行身上');
  assert.equal(rows.defsBefore.flat().length, 1, '只该有一个定义');
});

test('rowsOf：**符号** label（6~7 位 hex）也算定义行 —— 打上 patch 的文本必须还能被解析', () => {
  const rows = rowsOf(dis('i1f4\nlabel_100000\ni259 label_100000\nlabel_900001\nnop'));
  assert.deepEqual(rows.raw, ['i1f4', 'i259 label_100000', 'nop']);
  assert.deepEqual(rows.defsBefore, [[], [0], [1], []]);
});

test('replay：`def` 放出**局部 label 定义**；同一个符号被定义两次 ⇒ 抛', () => {
  const a = dis('i1f4\ni259');
  const out = replay(a, [{ op: 'insert-after', i: 0, instr: 'jmp label_900001', def: ['label_900001'] }]);
  const rows = rowsOf(out.text);
  assert.deepEqual(rows.raw, ['i1f4', 'jmp label_900001', 'i259'], '定义行不进序空间（与基线 id 空间无关）');
  assert.deepEqual(rows.defsBefore[1], [0], '局部定义必须挂在插入的那一行身上');
  assert.ok(out.text.includes('label_900001\njmp label_900001'), '定义行必须在指令行之前');
  assert.throws(
    () => replay(a, [
      { op: 'insert-after', i: 0, instr: 'jmp label_900001', def: ['label_900001'] },
      { op: 'insert-after', i: 1, instr: 'nop', def: ['label_900001'] },
    ]),
    /定义了两次/,
  );
});

test('diffRows：行数账必须平（删/插/替之后的产物行数 = 原行数 − 删 + 插）', () => {
  const a = dis('i1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1\ni1a7\njmp label_00000008');
  const b = dis('i1f4\ncomment "甲"\ni258 3 1\ni258 5 1\ni1a7\ncomment "乙"\ncomment "丙"\njmp label_00000008');
  const A = rowsOf(a);
  const B = rowsOf(b);
  const { ops, align } = diffRows(A.masked, B.masked);
  const tally = { delete: 0, 'insert-after': 0, 'replace-line': 0 };
  for (const o of ops) if (o.op in tally) tally[o.op] += 1;
  assert.equal(A.masked.length - tally.delete + tally['insert-after'], B.masked.length, '行数账不平 ⇒ diff 丢了行');
  // 对齐图：每条匹配都必须真的"值相同"，且覆盖所有"既没被插入、也没被替换"的产物行
  for (const [j, i] of align) assert.equal(B.masked[j], A.masked[i], `对齐图把产物第 ${j} 行对到了不相等的基线第 ${i} 行`);
  assert.equal(align.size, B.masked.length - tally['insert-after'] - tally['replace-line']);
});

test('diffRows：远处的单行巧合不算数（否则会把局部小改动判成"删一大段 + 插一大段"）', () => {
  // 中间夹了一行与很后面某行相同的内容：正确的对齐只该动中间那几行
  const a = dis('i1f4\ni259 label_00000004\ni258 7 1\ni258 8 1\ni1a7');
  const b = dis('i1f4\ni259 label_00000004\ni258 9 1\ni1a7');
  const { ops } = diffRows(rowsOf(a).masked, rowsOf(b).masked);
  assert.ok(ops.length <= 3, `op 数应当很小，实际 ${ops.length}：${JSON.stringify(ops)}`);
});

test('replay：`sha8` 对不上 ⇒ **抛**（基线换过就不许按位置硬套）', () => {
  const a = dis('i1f4\ni259');
  const ops = [{ op: 'replace-line', i: 1, sha8: 'deadbeef', line: 'comment "x"' }];
  assert.throws(() => replay(a, ops), /与基线\*\*对不上\*\*/);
  // 关掉摘要校验时能过（诊断用），但仍然要落在合法位置上
  assert.equal(rowsOf(replay(a, ops, { verifySha: false }).text).masked[1], 'comment "x"');
});

test('replay：行序越界 ⇒ 抛（不静默截断）', () => {
  const a = dis('i1f4\ni259');
  assert.throws(() => replay(a, [{ op: 'delete', i: 5, sha8: sha8('i259') }]), /行序越界/);
  assert.throws(() => replay(a, [{ op: 'insert-after', i: 9, instr: 'i1a7' }]), /行序越界/);
});

// ─────────────────────────────────────────────────────────── ② 保卫能不能红

test('★ structuralProblems 对每条结构不变量都能红', () => {
  const good = docOf({ 'SC0000.BIN': entryOf([{ op: 'replace-line', i: 1, sha8: 'deadbeef', line: 'comment "x"' }]) });
  assert.deepEqual(structuralProblems(good), []);

  const cases = [
    ['schemaVersion 不是 1', { ...good, schemaVersion: 2 }, /schemaVersion/],
    ['subsSha 形态非法', { ...good, subsSha: 'zz' }, /subsSha/],
    ['键不是 .BIN 名字', docOf({ 'PLINIT.TXT': entryOf([]) }), /键必须是 \.BIN 名字/],
    ['baseSha 形态非法', docOf({ 'SC0000.BIN': { ...entryOf([]), baseSha: 'x' } }), /baseSha/],
    ['resultSha 形态非法', docOf({ 'SC0000.BIN': { ...entryOf([]), resultSha: null } }), /resultSha/],
    ['op 枚举之外', docOf({ 'SC0000.BIN': entryOf([{ op: 'rename', i: 1 }]) }), /op 非法/],
    ['行序递减', docOf({ 'SC0000.BIN': entryOf([
      { op: 'delete', i: 5, sha8: 'deadbeef' },
      { op: 'delete', i: 2, sha8: 'deadbeef' },
    ]) }), /非递减/],
    ['同一行重叠', docOf({ 'SC0000.BIN': entryOf([
      { op: 'replace-line', i: 3, sha8: 'deadbeef', line: 'x' },
      { op: 'delete', i: 3, sha8: 'deadbeef' },
    ]) }), /重叠/],
    ['replace-line 缺 line', docOf({ 'SC0000.BIN': entryOf([{ op: 'replace-line', i: 3, sha8: 'deadbeef', line: '' }]) }), /line/],
    ['inset-after 的 i < -1', docOf({ 'SC0000.BIN': entryOf([{ op: 'insert-after', i: -2, instr: 'x' }]) }), /不得小于 -1/],
    ['delete 带 line', docOf({ 'SC0000.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef', line: 'x' }]) }), /不该带/],
    ['schema 之外的多余字段', docOf({ 'SC0000.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef', why: 'x' }]) }), /多余字段/],
    ['delete 带 def', docOf({ 'SC0000.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef', def: ['label_900001'] }]) }), /不该带 def/],
    ['def 不是数组', docOf({ 'SC0000.BIN': entryOf([{ op: 'insert-after', i: 3, instr: 'nop', def: 'label_900001' }]) }), /非空数组/],
    ['def 里的符号形态非法', docOf({ 'SC0000.BIN': entryOf([{ op: 'insert-after', i: 3, instr: 'nop', def: ['900001'] }]) }), /不是 label 形态/],
  ];
  for (const [why, doc, re] of cases) {
    const bad = structuralProblems(doc);
    assert.ok(bad.length > 0, `「${why}」应当红，实际全绿`);
    assert.ok(bad.some((b) => re.test(b)), `「${why}」的红点不对：${bad.join(' | ')}`);
  }
});

test('mergeStringLiterals：只换字符串常量，其余原样（BIN 里的占位写法 → patch 里的中文）', () => {
  const built = 'set-string (global-string f17) "要返回標題画面龜？"';
  const truth = 'set-string (global-string f17) "要返回标题画面吗？"';
  assert.equal(mergeStringLiterals(built, truth), truth);
  // 没有字符串的行原样返回（被引用的 label 地址**不该**被换掉）
  assert.equal(mergeStringLiterals('call label_0003e0a4', 'call label_100000'), 'call label_0003e0a4');
  // 字面量个数不一致 ⇒ 不动它，但要计数（宁可少换一处，也不拼出一行怪东西）
  const stats = { stringSubstitutions: 0, stringMismatch: 0 };
  assert.equal(mergeStringLiterals('i1f4 "甲"', 'i1f4 "乙" "丙"', stats), 'i1f4 "甲"');
  assert.equal(stats.stringMismatch, 1);
});

test('viewToBinText：人用中文改视图，汇编前要落成 BIN 写法；头部 4 行原样', () => {
  const m = makeMapper({ 这: '這' }); // `这` 编不进 cp932，`這` 编得进 ⇒ 正好考映射这条规则
  const view = '==Binary Information - do not edit==\nsignature = SYS4450 \nlocal_vars = { f 1 1 6 1 2 }\n====\n\ni1a7 "这是"\nret';
  const out = viewToBinText(view, m).split('\n');
  assert.equal(out[1], 'signature = SYS4450 ');
  assert.equal(out[5], 'i1a7 "這是"');
  assert.equal(out[6], 'ret');
  // 指令名与操作数形态不动
  assert.equal(out[5].slice(0, 5), 'i1a7 ');
});

test('★ 规范形态：脚本名按字典序、一行一个 op、键序重排也得出同一份文本', () => {
  const doc = docOf({
    'SP0131.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef' }]),
    'SC0000.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef' }]),
    '$1$SC0330.BIN': entryOf([]),
  });
  const text = serializePatch(doc);
  const keys = [...text.matchAll(/^  "((?:[^"\\]|\\.)*)": \{$/gm)].map((m) => JSON.parse(`"${m[1]}"`));
  assert.deepEqual(keys, [...keys].sort(), '键必须按字典序输出');
  assert.deepEqual(keys, ['$1$SC0330.BIN', 'SC0000.BIN', 'SP0131.BIN'], '`$`(0x24) 排在字母前');
  // ★ 顺序与**输入的键序无关**：把对象反过来喂进去，输出必须逐字节相同
  const reversed = { ...doc, scripts: Object.fromEntries(Object.entries(doc.scripts).reverse()) };
  assert.equal(serializePatch(reversed), text);
  // 一行一个 op
  assert.equal(text.split('\n').filter((l) => l.trimStart().startsWith('{"op"')).length, 2);
});

test('★ 发布物：data/translations/patch.json 处于规范形态（否则 git diff 会混进无意义噪声）', () => {
  if (!fs.existsSync(DEFAULT_PATCH)) return; // 首次提取前还不存在
  assert.deepEqual(canonicalFormProblems(DEFAULT_PATCH), []);
  // 顺序本身也复核一遍：文本里的键序 == 字典序，且每条条目的行序非递减
  const doc = loadPatch(DEFAULT_PATCH);
  const keys = Object.keys(doc.scripts);
  assert.deepEqual(keys, [...keys].sort());
  for (const [name, e] of Object.entries(doc.scripts)) {
    for (let k = 1; k < e.ops.length; k += 1) {
      assert.ok(e.ops[k].i >= e.ops[k - 1].i, `${name}: ops[${k}] 的行序递减了`);
    }
  }
});

test('范围口径：patch 的键只要求是 `.BIN` 名字；「标注集」只是标签、不是范围', () => {
  // ★ 曾经的错：把旧仓 `annotate-speaker.js` 的正则（SC/SP）当成"有译文的脚本集合"，
  //   于是漏掉了非 SC/SP 的 247 支真译文。⇒ 结构校验不再按名字筛，只要求是 .BIN。
  assert.deepEqual(structuralProblems(docOf({ 'SG0010.BIN': entryOf([]), 'SN0000.BIN': entryOf([]) })), []);
  assert.ok(SPEAKER_FILTER.test('SC0000.BIN') && SPEAKER_FILTER.test('$1$SC0330.BIN') && SPEAKER_FILTER.test('SP0131.BIN'));
  for (const no of ['SG0010.BIN', 'SN0000.BIN', 'PLINIT.BIN', 'BIINIT.BIN', '$2$SKINIT.BIN']) {
    assert.ok(!SPEAKER_FILTER.test(no), `${no} 不该落进"标注集"`);
  }
});

// ─────────────────────────────────────────────────────────── ① 写路径

/**
 * ★ "整支删空"型条目**一个都不该有**。
 * 曾经旧仓有一支（`$1$IMINIT.BIN`：工具链把整支 body 弄丢，见 `REQ-01M3YF5120V9WXB7NF36GVP52G`）——
 * 迁移时**跳过**了它，所以发布物里本来就不该有这种条目。
 * ⇒ 这条守卫**没有例外清单**：以后任何一支再出现这种形态，直接红。
 */
test('★ 发布物：没有"整支删空"型条目（那种形态不像译文）', () => {
  if (!fs.existsSync(DEFAULT_PATCH)) return;
  const doc = loadPatch(DEFAULT_PATCH);
  const suspicious = Object.entries(doc.scripts)
    .filter(([, e]) => e.ops.length >= 50 && e.ops.every((o) => o.op === 'delete'))
    .map(([n, e]) => `${n}（${e.ops.length} 条 op 全是 delete）`);
  assert.deepEqual(suspicious, [], `有"整支删空"型条目 —— 先确认它是译文还是产物异常：\n  - ${suspicious.join('\n  - ')}`);
});

test('replay：条目带 `header` 时用它替掉基线那 4 行（头部**不是**不变量）', () => {
  const a = dis('i1f4\ni259');
  const header = ['==Binary Information - do not edit==', 'signature = SYS4450 ', 'local_vars = { 1 1 1 1 1 1 }', '===='];
  const out = replay(a, [{ op: 'delete', i: 1, sha8: sha8(rowsOf(a).masked[1]) }], { header });
  assert.deepEqual(rowsOf(out.text).header, header);
  assert.deepEqual(out.header, header);
  // 不带 header ⇒ 一律取基线的
  assert.deepEqual(rowsOf(replay(a, []).text).header, rowsOf(a).header);
});

test('structuralProblems：`header` 必须是 4 个字符串', () => {
  const good = docOf({ 'SC0000.BIN': { ...entryOf([]), header: ['a', 'b', 'c', 'd'] } });
  assert.deepEqual(structuralProblems(good), []);
  for (const bad of [['a'], 'abc', ['a', 'b', 'c'], [1, 2, 3, 4]]) {
    const doc = docOf({ 'SC0000.BIN': { ...entryOf([]), header: bad } });
    assert.ok(structuralProblems(doc).some((b) => /header/.test(b)), `${JSON.stringify(bad)} 应当红`);
  }
});

test('序列化 ↔ 解析往返等价，且**一行一个 op**（可 diff 是硬需求）', () => {
  const doc = docOf({
    'SP0131.BIN': entryOf([{ op: 'insert-after', i: 0, instr: 'comment "x"' }]),
    'SC0000.BIN': entryOf([
      { op: 'replace-line', i: 7, sha8: 'deadbeef', line: 'i1a7 "中文"' },
      { op: 'delete', i: 9, sha8: 'cafebabe' },
    ]),
  });
  const text = serializePatch(doc);
  // 脚本名排序（确定 → diff 稳定）
  assert.ok(text.indexOf('"SC0000.BIN"') < text.indexOf('"SP0131.BIN"'));
  const opsLines = text.split('\n').filter((l) => l.trimStart().startsWith('{"op"'));
  assert.equal(opsLines.length, 3, '每个 op 恰好占一行');
  assert.deepEqual(canonicalPatch(JSON.parse(text)), canonicalPatch(doc));
});

test('savePatch：结构不绿 ⇒ **一个字都不写**（连已有文件都不碰）', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'patch-test-'));
  const p = path.join(dir, 'patch.json');
  const okDoc = docOf({ 'SC0000.BIN': entryOf([{ op: 'delete', i: 3, sha8: 'deadbeef' }]) });
  assert.equal(savePatch(okDoc, p).ok, true);
  const before = fs.readFileSync(p, 'utf8');

  const badDoc = docOf({ 'SC0000.BIN': entryOf([{ op: 'rename', i: 3 }]) });
  const res = savePatch(badDoc, p);
  assert.equal(res.ok, false);
  assert.match(res.reason, /预验未通过/);
  assert.equal(fs.readFileSync(p, 'utf8'), before, '红灯时文件必须原样');
  // 落盘的那份必须能被重新读出来（且与写前等价）
  assert.deepEqual(canonicalPatch(loadPatch(p)), canonicalPatch(okDoc));
  fs.rmSync(dir, { recursive: true, force: true });
});

test('savePatch：`_doc` 由工具拥有（手写的 _doc 会被规范成指向自描述的那一句）', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'patch-test-'));
  const p = path.join(dir, 'patch.json');
  const doc = { ...docOf({ 'SC0000.BIN': entryOf([]) }), _doc: '手写的会漂' };
  assert.equal(savePatch(doc, p).ok, true);
  assert.match(loadPatch(p)._doc, /tools patch describe/);
  fs.rmSync(dir, { recursive: true, force: true });
});

test('自描述可用：字段 / 不变量（含谁在守）/ 操作都在，且域声明与 cli 的地图一致', () => {
  const d = describe();
  assert.equal(d.file, 'data/translations/patch.json');
  for (const need of ['scripts[].baseSha', 'scripts[].resultSha', 'ops[].i', 'ops[].sha8', 'ops[].line', 'ops[].instr', 'ops[].def']) {
    assert.ok(d.fields.some((f) => f.name === need), `字段表少了 ${need}`);
  }
  assert.ok(d.invariants.length >= 5);
  for (const inv of d.invariants) assert.ok(inv.enforcedBy && inv.enforcedBy.length > 0, `不变量 ${inv.id} 没说"谁在守它"`);
  assert.match(d.writePath, /唯一写入口/);
  assert.equal(DOMAIN.id, 'patch');
  assert.ok(DOMAIN.data.some((x) => x.includes('patch.json')));
  // ★ 视图的范围口径必须出现在自描述里（它是"检索少报"那类事故的唯一防线，不能只活在代码注释里）
  assert.deepEqual(d.views.scopes, VIEW_SCOPES);
  assert.equal(d.views.defaultScope, 'all');
  for (const need of ['where', 'kinds', 'scopeNote', 'manifest', 'howToSearch']) assert.ok(d.views[need], `自描述少了 views.${need}`);
  assert.ok(describeText(d).includes('## 视图'), 'describeText 里要有「视图」一节');
});

// ─────────────────────────────────────────────────────────── 视图：范围 / 清单 / 配对

test('★ 视图范围：`all` / `patch` / `annotated` 是**三个不同的集合**（混过一次，代价是检索静默少报）', () => {
  const all = ['SC0000.BIN', 'SP0131.BIN', 'SG0010.BIN', 'SN0000.BIN', 'PLINIT.BIN', '$1$SC0330.BIN'];
  const doc = { scripts: { 'SC0000.BIN': {}, 'PLINIT.BIN': {} } };
  assert.deepEqual(scopeNames('all', { all, doc }), all, 'all = 基线根里全部可反汇编脚本');
  assert.deepEqual(scopeNames('patch', { all, doc }), ['PLINIT.BIN', 'SC0000.BIN'], 'patch 只看"有变更的"');
  assert.deepEqual(scopeNames('annotated', { all, doc }), ['SC0000.BIN', 'SP0131.BIN', '$1$SC0330.BIN']);
  // ★ 三者互不相等：任何"用 annotated 当视图范围"的写法都会被这条抓住
  assert.notDeepEqual(scopeNames('annotated', { all, doc }), scopeNames('patch', { all, doc }));
  assert.equal(scopeNames('annotated', { all, doc }).length, 3, 'annotated 只是"旧管线标注过的那批"');
  assert.throws(() => scopeNames('nope', { all, doc }), /不认识的视图范围/);
  // patch 里有基线根不认识的键 ⇒ 抛（不静默按名字生成一个查不到的视图）
  assert.throws(() => scopeNames('patch', { all, doc: { scripts: { 'XX.BIN': {} } } }), /不在基线根/);
});

test('★ 视图清单：逐脚本记账（指纹 + 覆盖了哪几侧），陈旧要报出"为什么"与"怎么修"', () => {
  const entries = { 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r1' }, 'PLINIT.BIN': { baseSha: 'aa', resultSha: 'aa' } };
  const manifest = mergeViewManifest(null, { scope: 'all', kinds: ['data', 'src'], entries, subsSha: 'subs', full: true });
  const dir = 'dist/views';
  const doc = (scripts) => ({ scripts });

  // ① 新鲜：patch 条目的两个指纹与清单一致；无条目的那支"基线 == 产物"
  assert.deepEqual(
    viewProblems({ manifest, dir, kinds: ['data', 'src'], names: ['SC0000.BIN', 'PLINIT.BIN'], subsSha: 'subs', doc: doc({ 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r1' } }) }).problems,
    [],
  );

  // ② patch 条目变了 ⇒ 点名那一支，并给出**只重建它**的命令
  const stale = viewProblems({ manifest, dir, kinds: ['src'], names: ['SC0000.BIN'], subsSha: 'subs', doc: doc({ 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r2' } }) });
  assert.equal(stale.ok, false);
  assert.ok(stale.problems.some((p) => /比 patch 旧/.test(p)), stale.problems.join(' | '));
  assert.ok(stale.problems.some((p) => /SC0000\.BIN/.test(p)));

  // ③ 只生成了 data、却要搜 src ⇒ 点名"缺哪一侧"；缺整支脚本也要红
  const dataOnly = mergeViewManifest(null, { scope: 'all', kinds: ['data'], entries, subsSha: 'subs', full: true });
  const missKind = viewProblems({ manifest: dataOnly, dir, kinds: ['src'], names: ['SC0000.BIN'], subsSha: 'subs', doc: doc({ 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r1' } }) });
  assert.ok(missKind.problems.some((p) => /不含请求的那一侧/.test(p)), '少一侧要红');
  const missName = viewProblems({ manifest, dir, kinds: ['src'], names: ['NOPE.BIN'], subsSha: 'subs', doc: doc({}) });
  assert.ok(missName.problems.some((p) => /没有这 1 支脚本的视图/.test(p)));
  assert.match(missName.fix, /patch view/);

  // ④ 字典换了 ⇒ 红（换字典会让 src 的字符串和盘上那份不一样）
  assert.ok(viewProblems({ manifest, dir, kinds: ['src'], names: ['PLINIT.BIN'], subsSha: 'SUBS2', doc: doc({}) }).problems.some((p) => /字典指纹变了/.test(p)));

  // ⑤ 无变更的那支：现在 patch 里却有条目 ⇒ 也陈旧；清单不存在 ⇒ 报"先生成"
  assert.equal(viewProblems({ manifest, dir, kinds: ['src'], names: ['PLINIT.BIN'], subsSha: 'subs', doc: doc({ 'PLINIT.BIN': { baseSha: 'aa', resultSha: 'bb' } }) }).ok, false);
  assert.equal(viewProblems({ manifest: null, dir, kinds: ['src'], names: [], subsSha: 's', doc: doc({}) }).ok, false);

  // ⑥ **增量记账**：部分生成只动自己那一格；字典换了则全部作废（不给旧记账背书）
  const merged = mergeViewManifest(manifest, { scope: 'all', kinds: ['src'], entries: { 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r9' } }, subsSha: 'subs', full: false });
  assert.deepEqual(merged.scripts['SC0000.BIN'].kinds, ['src'], '这一次只做了 src ⇒ 只声称 src');
  assert.deepEqual(merged.scripts['PLINIT.BIN'].kinds, ['data', 'src'], '没动的那支保持原记录');
  assert.deepEqual(
    Object.keys(mergeViewManifest(manifest, { scope: 'all', kinds: ['data'], entries: { 'SC0000.BIN': { baseSha: 'b1', resultSha: 'r1' } }, subsSha: 'SUBS2', full: false }).scripts),
    ['SC0000.BIN'],
    '字典换过 ⇒ 旧记账一律作废（只保留这一次真的生成了的那些）',
  );
  assert.deepEqual(Object.keys(mergeViewManifest(manifest, { scope: 'all', kinds: ['data'], entries, subsSha: 'subs', full: true }).scripts).length, 2, '全量生成 = 整个替换');
});

test('★ 编辑清单（锚寻址）：`-` 必须与当前内容逐字相同；改字面量 / 插一行 / 删一行各映到对应 op', () => {
  // 行序空间：`dis()` 是 4 行头部 + 空行 + 正文 ⇒ 正文的行序从 0 开始
  const view = dis('i1f4\ncomment "原生标记"\nset-string (global-string f17) "旧中文"\nshow-text 0 "第二行"');
  const B = rowsOf(view);
  const entryOf2 = (ops) => ({ baseSha: 'x'.repeat(64), resultSha: 'y'.repeat(64), ops });

  // ① 改字面量：锚 2 = `set-string … "旧中文"` 那一行
  const one = applyAnchorHunks({
    baseMasked: B.masked,
    entry: entryOf2([{ op: 'replace-line', i: 2, sha8: sha8(B.masked[2]), line: 'set-string (global-string f17) "旧中文"' }]),
    hunks: parseEditList('SC.BIN 2\n- set-string (global-string f17) "旧中文"\n+ set-string (global-string f17) "新中文"'),
  });
  assert.equal(one.entry.ops.find((o) => o.i === 2).line, 'set-string (global-string f17) "新中文"');
  assert.equal(one.entry.ops.find((o) => o.i === 2).sha8, sha8(B.masked[2]), '锚与摘要都不动');
  assert.deepEqual(one.report.map((r) => [r.verb, r.created]), [['replace', false]]);

  // ② 还没翻译的行（没有条目）⇒ 新建一条 replace-line，sha8 由基线现算
  const created = applyAnchorHunks({
    baseMasked: B.masked,
    entry: entryOf2([]),
    hunks: parseEditList('SC.BIN 3\n- show-text 0 "第二行"\n+ show-text 0 "第二行改"'),
  });
  assert.equal(created.entry.ops.length, 1);
  assert.deepEqual(created.entry.ops[0], { op: 'replace-line', i: 3, sha8: sha8(B.masked[3]), line: 'show-text 0 "第二行改"' });
  assert.equal(created.report[0].created, true);

  // ③ 插一行（挂在锚 3 之后）＋ 再删掉它（行键 `3+1`）—— 行键会随着插入整体后移
  const ins = applyAnchorHunks({ baseMasked: B.masked, entry: entryOf2([]), hunks: parseEditList('SC.BIN 3\n+ end-text-line 0') });
  assert.deepEqual(ins.entry.ops[0], { op: 'insert-after', i: 3, instr: 'end-text-line 0' });
  assert.equal(textAtRowKey(B.masked, ins.entry.ops, 3, 1), 'end-text-line 0');
  const del = applyAnchorHunks({
    baseMasked: B.masked,
    entry: ins.entry,
    hunks: parseEditList('SC.BIN 3+1\n- end-text-line 0'),
  });
  assert.equal(del.entry.ops.length, 0, '删掉刚插的那一行 ⇒ 条目又空了');
  assert.equal(del.report[0].verb, 'delete');

  // ④ 删掉基线行 ⇒ delete op（原来那条 replace-line 先撤掉）
  const delBase = applyAnchorHunks({
    baseMasked: B.masked,
    entry: entryOf2([{ op: 'replace-line', i: 2, sha8: sha8(B.masked[2]), line: 'set-string (global-string f17) "旧中文"' }]),
    hunks: parseEditList('SC.BIN 2\n- set-string (global-string f17) "旧中文"'),
  });
  assert.deepEqual(delBase.entry.ops, [{ op: 'delete', i: 2, sha8: sha8(B.masked[2]) }]);

  // ⑤ 护栏：绑定对不上 / 行键不存在 / 块替换要拒绝并指向 `patch edit`
  const throwsWith = (fn, needle) => {
    let msg = '';
    try { fn(); } catch (e) { msg = e.message; }
    assert.ok(msg.includes(needle), `期望报错里含 ${needle}，实际：${msg || '(没抛)'}`);
  };
  const H = (t) => parseEditList(t);
  throwsWith(
    () => applyAnchorHunks({ baseMasked: B.masked, entry: entryOf2([]), hunks: H('SC.BIN 2\n- 别的内容\n+ x') }),
    '期望的当前内容对不上',
  );
  throwsWith(
    () => applyAnchorHunks({ baseMasked: B.masked, entry: entryOf2([]), hunks: H('SC.BIN 2+3\n- x\n+ y') }),
    '在当前 src 视图里不存在',
  );
  throwsWith(() => H('SC.BIN 2\n- a\n- b\n+ c\n+ d'), '不在 `set` 的词汇里');
  throwsWith(() => H('SC.BIN 2\n- a\n- b'), '一次最多删一行');
  throwsWith(() => H('SC.BIN 2\n+ a\n+ b'), '一次最多插一行');
  throwsWith(() => H('SC.BIN 2\n+ 新内容\n- 旧内容'), '`-` 行必须集中在 `+` 行之前');
  throwsWith(() => H('- 没有头行\n+ x'), '先给头行');
  throwsWith(() => H('SC.BIN 2'), '既没有');
  throwsWith(() => H('# 只有注释'), '一条编辑都没有');
  // 只改字面量才允许：动结构要走渲染+反解
  throwsWith(
    () => applyAnchorHunks({ baseMasked: B.masked, entry: entryOf2([]), hunks: H('SC.BIN 3\n- show-text 0 "第二行"\n+ comment "第二行"') }),
    '只改引号里的字面量',
  );
});

test('★ merge on read：`projectedRows` 与真视图的行空间一致（src = base 行 ∖ 被 replace/delete ∪ op 载荷）', () => {
  const a = dis('i1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1');
  const A = rowsOf(a);
  const ops = [
    { op: 'replace-line', i: 1, sha8: sha8(A.masked[1]), line: 'comment "换成中文"' },
    { op: 'insert-after', i: 2, instr: 'end-text-line 0' },
    { op: 'delete', i: 3, sha8: sha8(A.masked[3]) },
  ];
  const { rows } = projectedRows(A.masked, ops);
  assert.deepEqual(rows.map((r) => r.text), ['i1f4', 'comment "换成中文"', 'i258 3 1', 'end-text-line 0', 'i258 5 1']);
  assert.deepEqual(rows.map((r) => r.kind), ['same', 'replace', 'same', 'insert', 'same']);
  // 与重放出来的行空间逐行相同（**除了 label 地址**：投影沿用基线那份，真视图用重建后的）
  const replayed = rowsOf(replay(a, ops).text);
  assert.equal(replayed.raw.length, rows.length);
  assert.deepEqual(replayed.masked, rows.map((r) => maskLabels(r.text)));
  // 行键：锚 2 那一行 + 挂在它后面的第 1 条插入行
  assert.equal(textAtRowKey(A.masked, ops, 2, 0), 'i258 3 1');
  assert.equal(textAtRowKey(A.masked, ops, 2, 1), 'end-text-line 0');
  assert.equal(textAtRowKey(A.masked, ops, 3, 0), null, '被 delete 的行在 src 视图里没有');
  assert.equal(rowKeyLabel(2, 1), 'i=2+1');
  // 锚 + k 直改（快路径）：只动载荷，锚与摘要不动
  const { entry } = editRowAtAnchor({ baseMasked: A.masked, entry: { ops }, anchor: 1, newText: 'comment "再换一次"' });
  assert.equal(entry.ops.find((o) => o.i === 1).line, 'comment "再换一次"');
  assert.equal(entry.ops.find((o) => o.i === 1).sha8, sha8(A.masked[1]));
});

test('★ 基线索引：只依赖不可变的东西（基线 + codec）⇒ 永不陈旧；codec 一变就红', () => {
  // 假根：只实现 `names()` / `resolve()` / 那三个廉价指纹要用的字段。
  // ★ 字节要是**真的 AGE 脚本**（`allScriptNames` 按头部签名判定）⇒ 用汇编器造两份。
  const binA = assemble(dis('i1f4\ni259'));
  const binB = assemble(dis('i1f4\ni258 3 1'));
  const bufs = { 'SC0000.BIN': binA, 'SP0001.BIN': binB };
  const loose = new Map(Object.keys(bufs).map((n) => [n, path.join(os.tmpdir(), n)]));
  const root = {
    dir: os.tmpdir(),
    loose,
    indices: [],
    names: () => new Set(loose.keys()),
    resolve: (n) => (bufs[n] ? { buf: bufs[n], from: `loose:${n}` } : null),
  };
  const realStat = fs.statSync;
  fs.statSync = ((p) => (typeof p === 'string' && /SC0000|SP0001/.test(p) ? { size: 1, mtimeMs: 1 } : realStat(p)));
  try {
    const idx = buildBaseIndex({ root, subsSha: 'subs', codecSha: 'codec1' });
    assert.deepEqual(idx.scripts, ['SC0000.BIN', 'SP0001.BIN']);
    assert.equal(idx.entries['SC0000.BIN'].bytes, binA.length);
    assert.ok(idx.baselineKey);
    assert.deepEqual(baseIndexProblems({ index: idx, root, codecSha: 'codec1' }).problems, []);
    // codec 变了（改汇编器 / 指令表）⇒ 旧索引里的文本不再担保能重建出同样字节
    assert.ok(baseIndexProblems({ index: idx, root, codecSha: 'codec2' }).problems.some((x) => /codec 指纹变了/.test(x)));
    assert.match(baseIndexProblems({ index: idx, root, codecSha: 'codec2' }).fix, /patch index --write/);
    assert.equal(baseIndexProblems({ index: null, root, codecSha: 'codec1' }).ok, false);
  } finally {
    fs.statSync = realStat;
  }
  // codec 指纹本身：64 位 hex，且覆盖到指令表（文件清单里有它）
  const ctx = codecContext();
  assert.match(ctx.codecSha, /^[0-9a-f]{64}$/);
  assert.ok(ctx.files.some((f) => f === 'instruction-set.json'), `codec 指纹要覆盖指令表：${ctx.files.join(', ')}`);
});

test('★ `find --edits --to` 的填充口径：只动引号里的字面量、跳过 `comment`、不动行尾注释', () => {
  const view = dis(
    ['i1f4  // 行尾注释里也有 赫塔 字样',
      'comment "▼G1 赫塔标记"',
      'set-string (global-string f17) "赫塔雷斯之戒"',
      'display-furigana 0 "赫塔" "ヘタ"',
      'label_00000040',
      'show-text 0 "去吧"'].join('\n'),
  );
  const { text, changes } = substituteLiterals(view, [{ from: '赫塔', to: '废柴' }]);
  assert.ok(text.includes('i1f4  // 行尾注释里也有 赫塔 字样'), '行尾注释不碰');
  assert.ok(text.includes('comment "▼G1 赫塔标记"'), 'comment 是原文标记，一律跳过');
  assert.ok(text.includes('set-string (global-string f17) "废柴雷斯之戒"'));
  assert.ok(text.includes('display-furigana 0 "废柴" "ヘタ"'), '两个参数都是文本 ⇒ 都换');
  assert.ok(text.includes('label_00000040'));
  assert.equal(changes.length, 2);
  // ★ 机械填充**会误伤同形词**（这就是"填好只是省事、仍然要逐条看"的原因）：
  const mangled = substituteLiterals(view, [{ from: '赫塔雷斯', to: '废柴雷斯' }]);
  assert.ok(mangled.text.includes('"废柴雷斯之戒"'), '按整词替换才是对的');
  const over = substituteLiterals(view, [{ from: '赫塔', to: '废柴雷斯' }]);
  assert.ok(over.text.includes('"废柴雷斯雷斯之戒"'), '按词根替换会把已有后缀叠加出来 ⇒ 必须人看');
  // 多对替换按顺序叠加；正则写法也能用
  const multi = substituteLiterals(view, [{ from: '赫塔', to: '废柴' }, { from: '废柴雷斯', to: '废柴' }]);
  assert.ok(multi.text.includes('"废柴之戒"'), multi.text);
  const rx = substituteLiterals(view, [{ from: '赫[塔太]', to: 'X' }], { regex: true });
  assert.ok(rx.text.includes('"X雷斯之戒"'), rx.text);
});

test('★ 配对（对齐）：`replace` / `insert` / `delete` 之后，src 的每一行都还有基线身份', () => {
  const a = dis('i1f4\ni259\ni258 3 1\ni258 4 1\ni258 5 1\ni1a7');
  const b = dis('i1f4\ni259\ni258 3 1\ncomment "插进来的"\ni258 4 1\ni1a7'); // 删一行、插一行
  const A = rowsOf(a);
  const B = rowsOf(b);
  const ops = opsFrom(a, b);
  const { rows, dropped } = alignRows(A.raw, ops);
  const R = rowsOf(replay(a, ops).text);
  assert.equal(rows.length, R.raw.length, 'src 的行数必须与重放出来的行数相同');
  // 非插入行：src 的内容仍然等于它所锚的基线行（`replace` 是"换了"，锚还是那一行）
  for (const r of rows) {
    if (r.op !== 'insert') continue;
    assert.ok(r.base >= -1, '插入行要挂在某一行后面');
  }
  assert.ok(rows.some((r) => r.op === 'insert'), '要认出插入行');
  assert.equal(dropped.length, 1, '要认出被删的那一行');
  // 行序空间的下标与 `rowsOf` 完全一致（否则检索报出的行号会对不上 patch 的锚）
  const src = a.split('\n');
  const idx = rowIndexByLine(a);
  assert.equal(idx.filter((x) => x !== null).length, A.raw.length);
  for (const [i, r] of idx.entries()) {
    if (r === null) continue;
    assert.equal(src[i].replace(/\s+\/\/.*$/, '').trim(), A.raw[r], `第 ${i + 1} 行应当对应行序 ${r}`);
  }
  // 没有 ops 的脚本：一一对应（视图 = 基线）
  const id = alignRows(A.raw, []);
  assert.deepEqual(id.rows.map((r) => r.base), A.raw.map((_, i) => i));
  // 越界的 op ⇒ 抛（不让"错位的 patch"悄悄产出一份错位的检索结果）
  assert.throws(() => alignRows(A.raw, [{ op: 'replace-line', i: 99 }]), /行序越界/);
});
