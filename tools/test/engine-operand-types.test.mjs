/** @env assets @kind gate @why 操作数类型分派表的机械提取坏了，或类型表与引擎 case 集不再一致 */
/**
 * tools/test/engine-operand-types.test.mjs —— **操作数类型分派的机械普查**（迭代点 ②：零引擎语义）
 *
 * ## 为什么这一条不需要"引擎语义"
 * 引擎里有两条**取操作数**的原语，各自用一张 `switch` 按 **operand type** 分派。这两个 switch 在语料里
 * 都是 IDA 导出的跳转表 —— **case 数、每个 case 的目标、哪个 case 是 default** 全部是**纯文本可枚举**的。
 * 所以"类型表覆盖得对不对"可以**机械判**，不需要相信任何旧仓结论：
 *
 * | 原语 | EA | 跳转表 | case 数 | 缺席（走 default）|
 * |---|---|---|---|---|
 * | **取址**（算操作数地址） | `0x42AEA0` | `jpt_42AF16` | 12 | 见 `default` 注释点名的 case |
 * | **读值**（按已算地址取值） | `0x41BF50` | `jpt_41BF99` | 14 | **case 8**（`ja def_41BF99 ; jumptable … default case, case 8`）|
 * | float 取址 | `0x42B4B0` | `jpt_42B51F` | 10 | **cases 5, 8, 11** |
 *
 * ★ 三张表的 case 数**互不相等**（12 / 14 / 10）⇒ 引擎对操作数类型的支持集**按用途分三套**，
 *   拿"一张表"代表全部是错的（这正是本轮要机械钉住的东西）。
 *
 * ## 这条守卫红得有意义吗
 * 红 = ① 提取判据失效（语料形态变了），或 ② `packages/age-format` 的类型表与引擎 case 集**真的不一致**。
 * 不是"不许出现某字符串"。
 *
 * 运行：`pnpm test:assets`（要语料）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { buildIndex, spanOfFunction, switchTables } from '../lib/disasm.mjs';
import { getTypeLabel } from '../../packages/age-format/src/asm/types.mjs';

const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const listing = (() => {
  if (!fs.existsSync(FILES_DIR)) return null;
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  return lst.length ? path.join(FILES_DIR, lst[0]) : null;
})();
const skip = listing ? false : '语料未解压（先 `pnpm tools disasm build`）';

/** 三条原语（EA 取自语料，两份镜像在这些 EA 上抽样一致） */
const PRIMITIVES = [
  { ea: 0x42aea0, kind: '取址', expectCases: 12 },
  { ea: 0x41bf50, kind: '读值', expectCases: 14 },
  { ea: 0x42b4b0, kind: 'float 取址', expectCases: 10 },
];

/** 从跳转点往前的小窗口里读出"入口比较 / 索引起点 / 哪些 case 走 default"（语料注释是 IDA 写的，纯文本） */
function entryFacts(lines, jumpLine1) {
  const win = lines.slice(Math.max(0, jumpLine1 - 41), jumpLine1);
  const facts = { bound: null, cases: null, base: 0, defaults: [], joins: [], kind: null };
  for (const l of win) {
    // 形态 A：`cmp <reg>, 0Dh ; switch 14 cases` ⇒ case 值 = 表下标
    const c = /cmp\s+\S+,\s*([0-9A-F]+)h\s*;\s*switch\s+(\d+)\s+cases/.exec(l);
    if (c) {
      facts.bound = Number.parseInt(c[1], 16);
      facts.cases = Number(c[2]);
      facts.kind = 'direct';
    }
    // 形态 B：`sub <reg>, 3 ; switch 12 cases` + 紧随的 `cmp <reg>, 0Bh` ⇒ case 值 = 表下标 + 3
    const sub = /sub\s+\S+,\s*(\d+)\s*;\s*switch\s+(\d+)\s+cases/.exec(l);
    if (sub) {
      facts.base = Number(sub[1]);
      facts.cases = Number(sub[2]);
      facts.kind = 'offset';
    }
    const cb = /cmp\s+\S+,\s*([0-9A-F]+)h\s*$/.exec(l);
    if (cb && facts.kind === 'offset' && facts.bound === null) facts.bound = Number.parseInt(cb[1], 16);
    const d = /ja\s+def_[0-9A-F]+\s*;\s*jumptable\s+[0-9A-F]+\s*default case,?\s*(.*)/.exec(l);
    if (d) facts.defaults = d[1].split(',').map((s) => s.trim()).filter(Boolean);
    const j = /;\s*jumptable\s+[0-9A-F]+\s+case\s+(\d+)/.exec(l);
    if (j) facts.joins.push(Number(j[1]));
  }
  return facts;
}

test('★ 三条原语的跳转表 case 数互不相等（支持集按用途分三套，不是一张表）', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const seen = [];
  for (const p of PRIMITIVES) {
    const tables = switchTables(listing, idx, p.ea, { fileLines: all });
    assert.equal(tables.length, 1, `${p.kind} 原语 0x${p.ea.toString(16)} 应恰好有一条跳转表，实际 ${tables.length}`);
    const t = tables[0];
    assert.equal(t.cases, p.expectCases, `${p.kind} 原语 0x${p.ea.toString(16)} 的 case 数变了：${t.cases} ≠ ${p.expectCases}`);
    seen.push(t.cases);
  }
  assert.equal(new Set(seen).size, 3, `三张表的 case 数必须互不相等（实测 12/14/10），实际 ${seen.join('/')}`);
});

test('★ 读值原语的**缺席者**：case 8 走 default（= 抛异常），与取址原语的 case 集不同', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const read = switchTables(listing, idx, 0x41bf50, { fileLines: all })[0];
  const facts = entryFacts(all, read.insnLine);
  assert.equal(facts.bound, 0xd, `读值原语的入口比较应是 0Dh，实际 ${facts.bound === null ? 'null' : facts.bound.toString(16)}`);
  assert.equal(facts.cases, 14, `语料自己写着 14 cases，实际 ${facts.cases}`);
  assert.deepEqual(facts.defaults, ['case 8'], `读值原语的 default case 应是 case 8，实际 ${JSON.stringify(facts.defaults)}`);
  // 表里那个 default 项必须落在下标 8（机械一致：case 值 = 表下标，因为入口用 `cmp … 0Dh` + `ja` 直接索引）
  const defIdx = read.targets.findIndex((g) => g.loc.startsWith('def_'));
  assert.equal(defIdx, 8, `default 项应落在表下标 8，实际 ${defIdx}`);
});

test('★ 取址原语：`sub ecx, 3` + `cmp ecx, 0Bh` ⇒ case **3..14**，且 default **一个都不占**', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const addr = switchTables(listing, idx, 0x42aea0, { fileLines: all })[0];
  const facts = entryFacts(all, addr.insnLine);
  assert.equal(facts.kind, 'offset', `取址原语应是"减去基点再比较"的形态，实际 ${facts.kind}`);
  assert.equal(facts.base, 3, `索引起点应是 3（case 3 对应表下标 0），实际 ${facts.base}`);
  assert.equal(facts.bound, 0xb, `上界应是 0Bh ⇒ case 3..14，实际 ${facts.bound === null ? 'null' : facts.bound.toString(16)}`);
  assert.deepEqual(facts.defaults, [], '取址原语的 default 不占任何 case（0/1/2 由入口前的 cmp/jz 另行处理）');
  // 12 项 ⇒ 3..14 正好 12 个；表首项必须指到 case 3 的落地标签（IDA 会在那一行标 `case 3`）
  assert.equal(addr.cases, 12);
  const firstCaseLine = all[addr.targets[0].atLine - 1 + 0];
  assert.ok(firstCaseLine, '表首项应能定位');
  const landing = all.findIndex((l, k) => k > addr.insnLine && l.includes(`${addr.targets[0].loc}:`) && /case 3\b/.test(l));
  assert.ok(landing > 0, `表下标 0 应落到标注 \`case 3\` 的标签 ${addr.targets[0].loc}（语料里找不到）`);
});

test('★ 类型表 vs 引擎 case 集：取址原语枚举的每个 type 都必须有标签（现在这里会红）', { skip }, () => {
  const idx = buildIndex(listing);
  const all = fs.readFileSync(listing, 'utf8').split('\n');
  const addr = switchTables(listing, idx, 0x42aea0, { fileLines: all })[0];
  assert.equal(addr.cases, 12, `取址原语的 case 数变了：${addr.cases}`);

  // 取址原语入口用 `cmp … 0Eh` + `ja`（边界 = 14）⇒ 表覆盖 case 0..13，其中走 default 的 case 见注释
  const facts = entryFacts(all, addr.insnLine);
  const noLabel = [];
  for (let v = 0; v <= 0xd; v += 1) {
    try {
      getTypeLabel(v);
    } catch (err) {
      noLabel.push(`type ${v}（0x${v.toString(16)}）：${err.message}`);
    }
  }
  assert.deepEqual(
    noLabel,
    [],
    '★ 引擎的取址原语会分派这些 type，而 `packages/age-format` 的类型表给不出标签 ⇒ ' +
      '反汇编器遇到它们要么抛、要么把 operand 解错：\n  - ' + noLabel.join('\n  - ') +
      `\n（取址原语的入口比较：${facts.bound === null ? '未取到' : `cmp … ${facts.bound.toString(16)}h`}；` +
      `走 default 的 case：${JSON.stringify(facts.defaults)}）`,
  );
});

test('★ 类型表的逆映射与正映射必须同构（`getType(getTypeLabel(v)) === v`）', { skip }, async () => {
  const { getType } = await import('../../packages/age-format/src/asm/types.mjs');
  const bad = [];
  for (let v = 0; v <= 0xd; v += 1) {
    let label;
    try {
      label = getTypeLabel(v);
    } catch {
      continue; // 上面那条守卫已经在报"给不出标签"
    }
    if (label === '') continue; // 无标签形态（标量 / 字符串 / 数组各有写法）
    const back = getType(label);
    if (back !== v) bad.push(`type ${v} → "${label}" → ${back}`);
  }
  assert.deepEqual(bad, [], `正/逆映射不同构（重汇编会把 type 写错）：\n  - ${bad.join('\n  - ')}`);
});
