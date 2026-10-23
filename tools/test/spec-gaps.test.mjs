/** @env pure @kind gate @why 未成篇 opcode 没有被 audit.gaps[] 精确覆盖（漏报 / 重复 / 形态坏），未成篇义务重新变成"缺口 0"的假象 */
/**
 * tools/test/spec-gaps.test.mjs —— 「未成篇 opcode 的义务落点」守卫（纯文本）
 *
 * 它守四件事：
 * ① **精确划分**：`opcode-table.json` 里 `hasHandler: true` 的每个 opcode，恰好落在
 *    「已写」（规则 `###` 标题里的 `0x…` 数字）或「某个 gap 的 `ops`」之一，不重不漏；
 * ② **落点形态**：gaps 的 `chapter` 存在（已写或 planned）、`ops` 项是 `0x…`、residual 带 `next`；
 *    容器句柄分支（按 switch 结构位置排除的机器分支）已从 `pilot.outOfScope` 移进 gaps；
 * ③ **章首条数**：章首声明「本章 N 条规则」的章节，N 必须等于该文件 `<!-- rule:` 标记数
 *    （声明与标记是同一份账；没声明的章节不检查）；
 * ④ **判别力**：把一条 gap 从内存副本里去掉 / 把一个 opcode 重复登记 / 写坏 `ops` 形态 ⇒ 计算必然报红。
 *
 * ★ 本文件**独立读文件**（不调用 `lib/spec.mjs` 的派生账）：否则"派生"与"断言"是同一份假设，
 *   守卫会恒绿。`lib/spec.mjs` 的 `opcodeAccounting` 只用于最后一条"报告计数与这里一致"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const OPCODE_RE = /^0x[0-9a-fA-F]+$/;
const num = (s) => Number.parseInt(String(s).replace(/^0x/i, ''), 16);
const hex = (n) => `0x${n.toString(16)}`;
const readJson = (rel) => JSON.parse(readFileSync(path.join(ROOT, rel), 'utf8'));

/** 指令表：`hasHandler: true` 的 opcode（数字集合） —— "应当有成篇语义"的闭集 */
export function handlerOpcodes() {
  const raw = readJson('data/spec/format/opcode-table.json');
  const rows = Array.isArray(raw) ? raw : Object.values(raw ?? {});
  return new Set(rows.filter((r) => r?.hasHandler === true).map((r) => r.opcode));
}

/**
 * 已写 opcode：**规则 `###` 标题**里的 `0x…` 数字（跨全部章节）。
 * ★ 只认带 `<!-- rule: -->` 标记的 `###` 小节；正文 / 表格里的数字一律不读。
 * ★ 标题里的 operand type 标签（`0x8003` 一类）会一起收进来，由调用方与 `hasHandler` 求交。
 */
export function titledOpcodes() {
  const dir = path.join(ROOT, 'data/spec/chapters');
  const out = new Set();
  for (const file of readdirSync(dir).filter((f) => f.endsWith('.md')).sort()) {
    const text = readFileSync(path.join(dir, file), 'utf8');
    for (const part of text.split(/^(?=#{2,3} )/m)) {
      const h = /^###\s+(.+?)\s*$/m.exec(part);
      if (!h || !/<!--\s*rule:/.test(part)) continue;
      for (const m of h[1].matchAll(/0x[0-9a-fA-F]+/g)) out.add(num(m[0]));
    }
  }
  return out;
}

/**
 * 精确划分判据（纯函数）：返回问题清单（空 = 绿）。
 * ★ 这就是守卫的"判别力"所在：坏数据喂进来必须**报出具体 opcode**。
 */
export function partitionProblems({ handler, titled, audit }) {
  const problems = [];
  const owner = new Map(); // opcode → gap id
  for (const g of audit.gaps ?? []) {
    for (const op of g.ops ?? []) {
      if (typeof op !== 'string' || !OPCODE_RE.test(op)) {
        problems.push(`gap ${g.id} 的 ops 项形态非法（要 0x…）：${JSON.stringify(op)}`);
        continue;
      }
      const n = num(op);
      if (owner.has(n)) problems.push(`opcode ${hex(n)} 同时出现在 ${owner.get(n)} 与 ${g.id}`);
      else owner.set(n, g.id);
    }
  }
  for (const n of handler) {
    const written = titled.has(n);
    const inGap = owner.has(n);
    if (written && inGap) problems.push(`opcode ${hex(n)} 既有规则标题覆盖、又在 gap ${owner.get(n)} 里`);
    else if (!written && !inGap) problems.push(`opcode ${hex(n)} 有处理函数，但既没有规则标题覆盖、也不在任何 gap 里`);
  }
  for (const n of owner.keys()) if (!handler.has(n)) problems.push(`gap 里的 opcode ${hex(n)} 不在指令表的 hasHandler 集合里`);
  return problems;
}

/**
 * 章首声明 vs 规则标记数（纯函数）：返回问题清单（空 = 绿）。
 * ★ 只检查**有声明**的章节：正文出现「本章 N 条规则」就必须 N == 该文件的 `<!-- rule:` 标记数；
 *   没声明的章节不检查（不逼所有章节都写声明）。
 * ★ 声明只有这一种写法；⛔ 不认其它措辞（否则守的是"猜句子"而不是"同一份账"）。
 */
export function chapterRuleCountProblems(sections) {
  const problems = [];
  for (const s of sections) {
    const declared = [...String(s.text).matchAll(/本章\s*(\d+)\s*条规则/g)].map((m) => Number(m[1]));
    if (declared.length === 0) continue;
    if (new Set(declared).size > 1) problems.push(`${s.id}：章首声明互相矛盾（${declared.join(' / ')} 条规则）`);
    const marks = [...String(s.text).matchAll(/<!--\s*rule:/g)].length;
    for (const n of new Set(declared)) {
      if (n !== marks) problems.push(`${s.id}：章首声明 ${n} 条规则，实际 ${marks} 个 <!-- rule: --> 标记`);
    }
  }
  return problems;
}

/** 读真源三件套（一次读完，供各用例共用） */
function loadAll() {
  const handler = handlerOpcodes();
  const titled = titledOpcodes();
  const audit = readJson('data/spec/audit.json');
  return { handler, titled, audit };
}

const clone = (v) => JSON.parse(JSON.stringify(v));

test('★ 精确划分：hasHandler = 已写（规则标题里的 opcode）+ gaps 覆盖，不重不漏', () => {
  const { handler, titled, audit } = loadAll();
  assert.ok(handler.size > 0, '指令表里应当有 hasHandler 的 opcode');
  assert.ok((audit.gaps ?? []).length > 0, '未成篇的 opcode 族/段必须登记在 audit.gaps[]（"缺口 0"是假象）');
  const problems = partitionProblems({ handler, titled, audit });
  assert.deepEqual(problems, [], `未成篇 opcode 的划分不成立：\n  - ${problems.join('\n  - ')}`);

  const gapOps = new Set((audit.gaps ?? []).flatMap((g) => (g.ops ?? []).map(num)));
  const written = [...handler].filter((n) => titled.has(n));
  assert.equal(written.length + gapOps.size, handler.size, '已写 + gaps 覆盖必须恰好等于 hasHandler');
});

test('★ 落点形态：chapter 存在（已写或 planned）、ops 是 0x…、residual 带 next；id 不重复', () => {
  const { audit } = loadAll();
  const sections = new Set();
  for (const c of readJson('data/spec/spec.json').chapters ?? []) sections.add(c.id);
  for (const c of audit.planned ?? []) sections.add(c.id);
  const ids = new Set();
  for (const g of audit.gaps ?? []) {
    assert.ok(typeof g.id === 'string' && g.id.trim(), `gap 缺 id：${JSON.stringify(g)}`);
    assert.ok(!ids.has(g.id), `gap id 重复：${g.id}`);
    ids.add(g.id);
    assert.ok(sections.has(g.chapter), `gap ${g.id} 的 chapter 不存在（已写或 planned）：${g.chapter}`);
    assert.ok(typeof g.title === 'string' && g.title.trim(), `gap ${g.id} 缺 title`);
    assert.ok(typeof g.note === 'string' && g.note.trim(), `gap ${g.id} 缺 note（为什么尚未成篇）`);
    if (g.ops !== undefined) {
      assert.ok(Array.isArray(g.ops), `gap ${g.id} 的 ops 必须是数组`);
      for (const op of g.ops) assert.match(op, OPCODE_RE, `gap ${g.id} 的 ops 项形态非法：${JSON.stringify(op)}`);
    }
    assert.ok(Array.isArray(g.residual) && g.residual.length > 0, `gap ${g.id} 必须点名具体义务（residual 非空）`);
    for (const r of g.residual) {
      assert.ok(typeof r.id === 'string' && r.id.trim(), `gap ${g.id} 的残差缺 id`);
      assert.ok(typeof r.what === 'string' && r.what.trim(), `gap ${g.id} 的残差 ${r.id} 缺 what`);
      assert.ok(typeof r.next === 'string' && r.next.trim(), `gap ${g.id} 的残差 ${r.id} 缺 next（残差不是"算了"）`);
    }
  }
});

test('★ 章首「本章 N 条规则」与规则标记数同一份账', () => {
  const chapters = readJson('data/spec/spec.json').chapters ?? [];
  const sections = chapters.map((c) => ({
    id: c.id,
    text: readFileSync(path.join(ROOT, 'data', 'spec', c.file), 'utf8'),
  }));
  assert.ok(
    sections.some((s) => /本章\s*\d+\s*条规则/.test(s.text)),
    '样本前提：至少一章在章首声明了规则条数',
  );
  const problems = chapterRuleCountProblems(sections);
  assert.deepEqual(problems, [], `章首声明与规则标记数不一致：\n  - ${problems.join('\n  - ')}`);

  // 判别力：少一个标记 / 多一个标记 / 两个声明互相矛盾 ⇒ 都必须被点名。
  assert.deepEqual(
    chapterRuleCountProblems([{ id: 'x', text: '本章 2 条规则\n<!-- rule: A -->\n' }]),
    ['x：章首声明 2 条规则，实际 1 个 <!-- rule: --> 标记'],
  );
  assert.deepEqual(
    chapterRuleCountProblems([{ id: 'y', text: '本章 1 条规则\n<!-- rule: A -->\n<!-- rule: B -->\n' }]),
    ['y：章首声明 1 条规则，实际 2 个 <!-- rule: --> 标记'],
  );
  assert.ok(
    chapterRuleCountProblems([{ id: 'z', text: '本章 1 条规则\n本章 2 条规则\n<!-- rule: A -->\n' }]).some((p) =>
      p.includes('互相矛盾'),
    ),
    '同一章两个声明互相矛盾必须报红',
  );
  assert.deepEqual(
    chapterRuleCountProblems([{ id: 'w', text: '没有声明，只有 <!-- rule: A -->\n' }]),
    [],
    '没声明条数的章节不检查（不逼所有章节都写声明）',
  );
});

test('★ 容器句柄分支：已从 pilot.outOfScope 移进 gaps（REQ-01M5AM4M5A5TV4APMPN0QWEA96 第 5 条）', () => {
  const { audit } = loadAll();
  const still = (audit.pilot?.outOfScope ?? []).filter((x) => String(x.what).includes('容器句柄'));
  assert.deepEqual(still, [], '容器句柄分支不能同时留在 outOfScope 与 gaps 里（唯一落点 = gaps[]）');
  const gap = (audit.gaps ?? []).find((g) => g.chapter === 'containers');
  assert.ok(gap, '容器句柄分支必须登记为一条 gap（chapter: containers）');
  assert.match(gap.note, /句柄分支不是跳转表 case/, 'note 必须写清"按 switch 结构位置排除"的理由');
  const unhandled = (gap.residual ?? []).find((r) => r.kind === 'unhandled');
  assert.ok(unhandled?.next?.trim(), '容器句柄 gap 必须有 kind=unhandled 的残差，且带 next');
  for (const tag of ['0x8003', '0x8005', '0x8009', '0x800b']) {
    assert.ok(String(gap.title).toLowerCase().includes(tag), `容器句柄 gap 必须点名类型标签 ${tag}`);
  }
});

test('★ 判别力：去掉一条 gap ⇒ 那条 gap 的 opcode 立刻"无人认领"', () => {
  const { handler, titled, audit } = loadAll();
  const broken = clone(audit);
  const dropped = broken.gaps.find((g) => (g.ops ?? []).length > 0);
  assert.ok(dropped, '样本前提：至少有一条带 ops 的 gap');
  broken.gaps = broken.gaps.filter((g) => g !== dropped);
  const problems = partitionProblems({ handler, titled, audit: broken });
  assert.ok(problems.length > 0, '去掉一条 gap 必须让划分失败（否则守卫是恒绿的）');
  assert.ok(
    problems.some((p) => p.includes(hex(num(dropped.ops[0])))),
    `报错必须点名被丢掉的那个 opcode ${dropped.ops[0]}`,
  );
});

test('★ 判别力：同一 opcode 登记进两个 gap / ops 形态写坏 ⇒ 报红', () => {
  const { handler, titled, audit } = loadAll();
  const withOps = (audit.gaps ?? []).filter((g) => (g.ops ?? []).length > 0);
  assert.ok(withOps.length >= 2, '样本前提：至少两条带 ops 的 gap');

  const dup = clone(audit);
  const first = dup.gaps.find((g) => g.id === withOps[0].id);
  const second = dup.gaps.find((g) => g.id === withOps[1].id);
  first.ops.push(second.ops[0]);
  assert.ok(
    partitionProblems({ handler, titled, audit: dup }).some((p) => p.includes('同时出现在')),
    '同一个 opcode 出现在两条 gap 里必须报红',
  );

  const bad = clone(audit);
  bad.gaps.find((g) => g.id === withOps[0].id).ops[0] = '52';
  assert.ok(
    partitionProblems({ handler, titled, audit: bad }).some((p) => p.includes('形态非法')),
    'ops 项不是 0x… 形态必须报红',
  );
});

test('★ 报告计数与独立划分一致：未成篇 = 表口径总数 − 已写，且由 gaps 覆盖', async () => {
  const { derive, loadSpec, reportText } = await import('../lib/spec.mjs');
  const { handler, titled } = loadAll();
  const model = loadSpec().model;
  const derived = derive(model);
  assert.ok(derived.opcodes?.ok, '指令表在场时派生账必须能算出');
  assert.equal(derived.opcodes.total, handler.size, '派生账的 total 必须等于指令表的 hasHandler 数');
  assert.equal(derived.opcodes.written, [...handler].filter((n) => titled.has(n)).length, '已写数必须等于标题口径的独立计数');
  assert.equal(derived.opcodes.unwritten, handler.size - derived.opcodes.written, '未成篇 = total − 已写');
  assert.equal(derived.opcodes.gapsCovered, derived.opcodes.unwritten, 'gaps 覆盖数必须等于未成篇数');
  assert.equal(derived.opcodes.consistent, true, '派生账必须自洽（无重叠 / 表外 / 重复）');
  const text = reportText(model, { derived });
  assert.ok(
    text.includes(`未成篇 opcode ${derived.opcodes.unwritten}（gaps 覆盖 ${derived.opcodes.gapsCovered}；已写成 ${derived.opcodes.written}）`),
    '报告那一行必须与独立划分一致',
  );
  assert.ok(!text.includes('%'), '报告不许出现百分数');
});
