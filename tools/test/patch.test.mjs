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
  DEFAULT_PATCH,
  DOMAIN,
  OFFICIAL_FILTER,
  canonicalFormProblems,
  canonicalPatch,
  describe,
  diffRows,
  loadPatch,
  mergeStringLiterals,
  replay,
  rowsOf,
  savePatch,
  serializePatch,
  sha8,
  structuralProblems,
  symbolize,
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
    ['脚本名不在官方集口径内', docOf({ 'PLINIT.BIN': entryOf([]) }), /不在官方集口径内/],
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

test('官方集口径只认 SC / SP（含 $N$ 前缀）', () => {
  for (const ok of ['SC0000.BIN', 'SP0131.BIN', '$1$SC0330.BIN', '$5$SP0372.BIN']) assert.ok(OFFICIAL_FILTER.test(ok), ok);
  for (const no of ['PLINIT.BIN', 'EBINIT.BIN', '$2$SKINIT.BIN', 'SYS4INI.BIN', 'SG0010.BIN']) assert.ok(!OFFICIAL_FILTER.test(no), no);
});

// ─────────────────────────────────────────────────────────── ① 写路径

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
});
