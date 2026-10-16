#!/usr/bin/env node
/**
 * tools/shape.mjs —— **CLI**：对象 shape 的「访问记录 / 独立复核 / 段落导出 / claims 校验」
 *
 * 经派发器：`pnpm tools shape <access|verify|regions|claims|describe> [args…]`
 * 也可独立跑：`node tools/shape.mjs --regions sub_40ED40 edi`
 *
 * ★ 本域**不认识任何具体家族**（别指望它知道"算数指令"是什么）：输入只是**若干 `sub_xxxxxx`**。
 *   命令 / 口径 / 义务在 `lib/shape-*.mjs`（那边是 schema 的真源）；本文件只做"参数 → 模型 → 打印"。
 *
 * ## 三个落点（都是"判定"，不是"结论"）
 * * `access <sym>`  —— 形状层**助手**的逐 site 访问记录（EA 锚；含 obj / 位移 / 步长 / 索引 / 宽度 / 跳数）
 * * `verify <EA>`   —— **独立复核器**：按 EA 回语料重解一条指令（verified / drifted / unverifiable）
 * * `regions <sym> [reg]` —— **校验器功能①**：机械导出拆分范围（段落 / 类别 / 段首 / 是否被 `call` 打断）
 * * `claims <file>|--sample` —— **校验器功能②**：校验 claims（类型 / 载体 / 角色 / 域 / 逃逸）
 *
 * 读写：**r**（只读语料与 `.c`；本域不写任何文件 —— claims 文件由人 / 模型自己给）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

import { analyzeLines, functionSpans, loadListing, objRootRegs } from './lib/shape-access.mjs';
import { checkClaims, DRILL_MAX_LEVEL, objectLevel, renderReport, sampleModel, VOCAB, REDUNDANT } from './lib/shape-claims.mjs';
import { deriveRegions, insnsOf } from './lib/shape-regions.mjs';
import { checkRow, describeEa, loadRaw } from './lib/shape-verify.mjs';

export const DOMAIN = {
  id: 'shape',
  title: '对象 shape：访问记录 / 独立复核 / 段落导出 / claims 校验（EA 锚；不写任何文件）',
  data: [
    '`corpus/disasm/files/*.{lst,c}`（**只读**语料；解压产物，gitignore）',
    'claims 文件（**由人 / 模型给**，本域只读它；⛔ 不读 `apps/**` —— 那是同层产物）',
  ],
  access: 'r（只读；⛔ 本域没有任何写操作）',
  tool: 'tools/shape.mjs',
};

export const OPERATIONS = [
  { name: 'access', argv: ['--access'], mutates: false, summary: '一个函数的逐 site 访问记录（EA 锚；含 obj / 位移 / 步长 / 索引来源 / 宽度 / 跳数）' },
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '★ **独立复核器**：按 witness EA 回语料重解（verified / drifted / unverifiable；证明 vs 推断按字段）' },
  { name: 'regions', argv: ['--regions'], mutates: false, summary: '★ **功能①**：机械导出某函数某寄存器的拆分范围（段落 / 类别 / 段首形态 / 是否被 call 打断）' },
  { name: 'depth', argv: ['--depth'], mutates: false, summary: '**对象层级分布**（level 1 / 2 / 3+）：`[<sym>]`；用来现跑"下钻上限冻结在层级 2"这个判决（层级 ≥3 单列，不静默丢）' },
  { name: 'claims', argv: ['--claims'], mutates: false, summary: '★ **功能②**：校验 claims 文件（守卫 / G1..G6 / J3 / 双守恒）；`--sample` 用内置样本' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：claims 分节 / 义务 / 两道守卫 / 诚实项（schema 的真源）' },
];

const hex = (n) => (typeof n === 'number' ? `0x${n.toString(16)}` : String(n));

export function describeText() {
  return `shape —— 对象 shape 的观察与校验（汇编层；锚只有 EA）

数据    corpus/disasm/files/*.{lst,c}（只读语料）· claims 文件（人/模型给；本域只读）
读写    r（本域没有任何写操作）
操作    access · verify · regions · depth · claims · describe

分节（claims 文件 = 模型的**判断**，不是转录）
  types      每个类型**按 id 共享一份**：成员 = { off(必填) · width | type+count · kind · name(非键) }；洞不写
  carriers   机器槽 → 类型：kind ∈ arg{fn,reg|off} / local{fn,off} / global{sym}
             ★ 字段**不是**载体 —— 它是容器的成员（写在 types 里）
  roles      { fn, reg, span, role, bound, carryFrom, uses }；span **必须逐字等于** \`regions\` 导出的一个段落
  domains / escapes / identities   可选（本版只做结构检查）
  typesFrom  引用另一个 claims 文件共享类型；**同名类型按成员增量合并**（同 off 本地优先）

义务（红了都要有理由；⛔ 没有一档是"静默通过"）
  G1 类型自洽（成员不重叠 / 不越 size）
  G2 覆盖（两级：容器的成员 + **数组成员的元素内成员**）· G3 步长（数组形态 == size(元素类型)）
  G4 数组边界（末尾地址必须在语料里被引用）· G5/G5' **二级对象下钻**
     （宿主是 ptr + 声明了目标类型 ⇒ 用目标类型的成员表再跑一遍；type=null ⇒ **不透明桶**单列；没声明 ptr ⇒ 红）
  G6 未观察成员（用**形状层观察集**判定：它能解折叠形态；字面量扫描只是下界）
  J3 角色（span == 导出段落，且段内全 ±const/init）
  守恒①（obj0 访问）· 守恒②（二级对象 = 解释 + 不透明 + 拒绝）

两道守卫
  词汇  出现 ${VOCAB.join(' / ')} ⇒ 红（那是 B 层：data/ledger/，走 AGENTS §6 准入门）
  冗余  字段名出现 ${REDUNDANT.join(' / ')} ⇒ 红（这些都能从 EA + 语料机械读出）

诚实项
  · 二级对象**只下钻到层级 2**；层级 ≥3 单列"超出解析深度"（带 EA，不静默丢）
  · 守恒②里"目标类型解释"的那一档只覆盖**声明了 type 的指针成员**；不透明档是不可检查，不是通过
  · 观察集必须由调用方算（本模块不落缓存文件）⇒ 不传就退成字面量下界并**明说**
  · J4/J5/J6 三节本版只做结构检查；J3 只做"span == 导出段落"
`;
}

/** 形状层的 obj0 相对观察集（G6 的正确判据；约 1~2 s） */
function observedSet(lines) {
  const set = new Set();
  const roots = objRootRegs();
  for (const sp of functionSpans(lines)) {
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sp.sym) ?? null, {});
    for (const s of sites) if (s.obj === 'Engine' && s.absDisp !== null) set.add(s.absDisp);
  }
  return set;
}

function runAccess(sym) {
  const lines = loadListing();
  const roots = objRootRegs();
  const sp = functionSpans(lines).find((s) => s.sym === sym);
  if (!sp) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sym) ?? null, {});
  process.stdout.write(`# ${sym}（行 ${sp.from + 1}-${sp.to + 1}）  对象根寄存器=${roots.get(sym) ?? '（.c 约定里没有）'}  访问 ${sites.length} 处\n`);
  for (const s of sites) {
    process.stdout.write(`  ${hex(s.ea)}  ${String(s.mn).padEnd(6)} ${s.access} w=${String(s.width).padEnd(7)} base=${String(s.baseKind).padEnd(10)}`
      + ` obj=${String(s.obj ?? '?').padEnd(16)} abs=${s.absDisp === null ? '-' : hex(s.absDisp)}`
      + `${s.stride ? ` stride=${s.stride}` : ''}${s.indexSym ? ` idx=${s.indexSym}` : ''}`
      + `${s.pointeeHost !== null && s.pointeeHost !== undefined ? ` pointee=+${hex(s.pointeeHost)}+${hex(s.pointeeOff)}` : ''}`
      + ` level=${objectLevel(s)}`
      + `${s.problems?.length ? `  ⚠ ${s.problems.join(',')}` : ''}\n`);
  }
  return 0;
}

function runRegions(sym, reg) {
  const lines = loadListing();
  if (!insnsOf(lines, sym)) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const f = insnsOf(lines, sym);
  const regs = reg
    ? [reg.toLowerCase()]
    : [...new Set(f.out.filter((x) => /^e[a-z]{2}$/.test((x.ops[0] ?? '').toLowerCase())).map((x) => x.ops[0].toLowerCase()))];
  process.stdout.write(`# ${sym}：**机械导出的拆分范围**（模型只能引用，不能改）\n`);
  for (const r of regs) {
    const d = deriveRegions(lines, sym, r);
    if (!d || d.writes.length === 0) continue;
    process.stdout.write(`\n## ${r}（写点 ${d.writes.length} 处 ⇒ ${d.regions.length} 个段落）\n`);
    for (const [i, g] of d.regions.entries()) {
      process.stdout.write(`  S${i + 1} [${hex(g.from)}..${hex(g.to)}] 类别=${String(g.cat).padEnd(5)}`
        + ` ${g.allConstOrInit ? '全 ±const/init' : '含非常量形态'}${g.openedByInit ? ' 段首是 init' : ' **段首不是 init**'}`
        + `${g.brokenByCall ? ' ⚠被调用打断' : ''}\n`);
      for (const w of g.writes) process.stdout.write(`        ${hex(w.ea)} 行${w.line}  ${String(w.kind).padEnd(7)} ${w.text}\n`);
    }
  }
  return 0;
}

/**
 * **对象层级分布**：level=1 对象就是 obj0；level=2 对象是"obj0 的成员里读出的指针"；3+ = 再往下。
 * ★ 层级 = `baseHop + 1`（`baseHop` 是**基址值的来路**，不是层级）—— 差这一层会让"层级 3"整类
 *   （实测 812 处）从"超出解析深度"里漏掉，而上限冻结在层级 2 时那正是必须被发现的东西。
 */
function runDepth(sym) {
  const lines = loadListing();
  const roots = objRootRegs();
  const all = functionSpans(lines);
  const fns = sym ? all.filter((s) => s.sym === sym) : all;
  if (!fns.length) { process.stderr.write(`找不到函数 ${sym}\n`); return 2; }
  const hist = new Map();
  let total = 0;
  for (const sp of fns) {
    const { sites } = analyzeLines(lines, sp.from, sp.to, roots.get(sp.sym) ?? null, {});
    for (const s of sites) {
      if (s.access === 'a') continue;                                   // 地址计算不算访问
      if (s.obj !== 'Engine' && !String(s.obj).startsWith('Engine+0x')) continue;
      const lvl = objectLevel(s);
      hist.set(lvl, (hist.get(lvl) ?? 0) + 1);
      total += 1;
    }
  }
  const withinCap = [...hist].filter(([k]) => k <= DRILL_MAX_LEVEL).reduce((a, [, v]) => a + v, 0);
  process.stdout.write(`# 对象层级分布${sym ? `（${sym}）` : '（全语料）'}：obj0 族访问 ${total} 处\n`);
  process.stdout.write('  层级含义：1 = 对象就是 obj0；2 = 对象是"obj0 的成员里读出的指针"；3+ = 指针的指针\n');
  for (const [k, v] of [...hist].sort((a, b) => a[0] - b[0])) process.stdout.write(`  level ${String(k).padEnd(3)} ${String(v).padStart(6)}  (${total ? ((v / total) * 100).toFixed(1) : '0'}%)\n`);
  process.stdout.write(`  ★ 下钻上限冻结在 level=${DRILL_MAX_LEVEL}：覆盖 ${withinCap}/${total}；层级 >${DRILL_MAX_LEVEL} 的由 \`claims\` 单列"超出解析深度"（带 EA），⛔ 不静默丢\n`);
  return 0;
}

function runClaims(target) {
  const lines = loadListing();
  let model;
  let baseDir = process.cwd();
  let title = 'claims-check';
  if (target === '--sample' || target === undefined) {
    model = sampleModel();
    title = 'claims-check（内置样本）';
  } else {
    const file = path.resolve(target);
    model = JSON.parse(fs.readFileSync(file, 'utf8'));
    baseDir = path.dirname(file);
    title = `claims-check：${path.basename(file)}`;
  }
  const rep = checkClaims(model, { lines, baseDir, observed: observedSet(lines) });
  process.stdout.write(renderReport(rep, { title }));
  return rep.findings.length ? 1 : 0;
}

export async function main(argv = process.argv.slice(2)) {
  const [flag, ...rest] = argv;
  const pos = rest.filter((a) => !a.startsWith('--'));
  switch (flag) {
    case '--access': return runAccess(pos[0]);
    case '--verify': {
      const ea = Number.parseInt(pos[0] ?? '', 16);
      const raw = loadRaw();
      const d = ea ? describeEa(raw, ea) : null;
      process.stdout.write(`${JSON.stringify(d ?? checkRow(raw, {}), null, 2)}\n`);
      return d?.ok === false ? 1 : 0;
    }
    case '--regions': return runRegions(pos[0], pos[1]);
    case '--depth': return runDepth(pos[0]);
    case '--claims': return runClaims(pos[0]);
    case '--describe': process.stdout.write(describeText()); return 0;
    default:
      process.stderr.write(`未知参数：${argv.join(' ')}\n可用：--access <sym> · --verify <EA> · --regions <sym> [reg] · --claims <file>|--sample · --describe\n`);
      return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
if (isDirectRun) {
  main()
    .then((code) => { process.exitCode = code ?? 0; })
    .catch((err) => { process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`); process.exitCode = 2; });
}
