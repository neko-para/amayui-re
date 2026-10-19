#!/usr/bin/env node
/**
 * tools/spec.mjs —— **CLI**：spec 层（领域本体）的校验 / 派生报告 / 章节 / 反向查询
 *
 * 经派发器：`pnpm tools spec <validate|report|chapters|show|at|effects|describe> [args…]`
 * 也可独立跑：`node tools/spec.mjs --validate`
 *
 * 模型全在 `lib/spec.mjs`（schema / 不变量 / 候选域 / 派生 / 自描述）；本文件只做
 * "参数 → 模型 → 打印"。★ 本域**没有写操作**：规范正文是 `data/spec/chapters/*.md`，
 * 不打包成单文件；校验、报告、查询都是只读。
 *
 * 读写：**r**（章节 / bindings / audit、台账、语料、layout/types）
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { REPO_ROOT } from './lib/paths.mjs';
import { pickListing } from './lib/disasm.mjs';
import { functionSpans } from './lib/shape-access.mjs';
import { derivePilotEffects } from './lib/spec-effects.mjs';
import { effectsForFunction } from './lib/x86-effects.mjs';
import {
  DEFAULT_DIR,
  DISPOSITIONS,
  SPEC_FILE,
  chaptersText,
  checkCorpusBindings,
  derive,
  describe,
  describeText,
  loadLedger,
  loadSpec,
  reportText,
  resolveEvidence,
  validate,
} from './lib/spec.mjs';

export const DOMAIN = {
  id: 'spec',
  title: '引擎 spec（领域本体）：章节 Markdown（分发物）+ manifest + 内部绑定/审计（只读校验与查询）',
  data: [
    '`data/spec/spec.json`（**规范正文**：章节 + 规则：合同 / 状态对应 / 条件 / 可证伪）',
    '`data/spec/bindings.json`（**机器绑定**：站点 EA / 函数 / storage 指针 / 证据）',
    '`data/spec/audit.json`（**范围审计**：试点块 / effect 选择器 / 处理状态 / 残差）',
    '`data/ledger/`（证据：按 id 解析、只认 accepted / proposed）',
    '`corpus/disasm/files/*.lst`（assets 档：EA 锚与转表对账；解压产物，gitignore）',
  ],
  access: 'r（章节 / bindings / audit + 台账 + 语料；不分发单文件，没有写操作）',
  tool: 'tools/spec.mjs',
};

export const OPERATIONS = [
  { name: 'validate', argv: ['--validate'], mutates: false, summary: '不变量（红 = 退出码 1）：形态 / 处理状态 / 绑定 / 证据 / 措辞 / **试点未登记义务 = 0**（条数见 describe）' },
  { name: 'report', argv: ['--report'], mutates: false, summary: '★ 派生处理账：effect 覆盖 / 内部省略 / 残差 / **未登记** + 章节与绑定计数（不给百分数）' },
  { name: 'chapters', argv: ['--chapters'], mutates: false, summary: '整份 spec 的章节骨架 + 每章条目与残差（"整个 spec 到哪一步"的只读视图）' },
  { name: 'show', argv: ['--show'], mutates: false, summary: '一条合同的全貌 + 证据解析结果 + 机器站点（`<id 或唯一前缀>`；`--json` 给机器读）' },
  { name: 'at', argv: ['--at'], mutates: false, summary: '★ 反向查询：某个 EA / 函数符号出现在哪些规则的绑定里（`--at <EA|sub_xxx>`）' },
  { name: 'effects', argv: ['--effects'], mutates: false, summary: '★ 从语料机械枚举 effect：`--pilot`（试点块）或 `<sub_XXXXXX>`（整个函数）；`--json` 给机器读' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：章节 / manifest / bindings / audit 的字段与不变量 / 操作 / 诚实项（schema 唯一真源，支持 `--json`）' },
];

const HELP = `tools/spec.mjs —— spec 层（领域本体）：章节 Markdown（分发物）+ 内部绑定/审计（只读）

  node tools/spec.mjs --validate [--dir data/spec]   # 不变量（含章节规则与 bindings/audit 一一对应）
  node tools/spec.mjs --report                        # 审计账（effect 覆盖 / 内部 / 残差 / 未登记）
  node tools/spec.mjs --effects --pilot [--json]       # 从语料机械枚举试点块的 effect
  node tools/spec.mjs --effects <sub_XXXXXX> [--json]  # 枚举一个函数的全部 effect
  node tools/spec.mjs --chapters                      # 章节骨架（审计视图）
  node tools/spec.mjs --show <id|前缀> [--json]        # 一条规则（规范 + 绑定 + 证据）
  node tools/spec.mjs --at <EA|sub_XXXXXX>             # 反向查询
  node tools/spec.mjs --describe [--json]             # 自描述
`;

const hex = (n) => `0x${n.toString(16)}`;

/** 取 `--flag` 的值（`--file` / `--show` / `--at` 这类） */
function opt(args, name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}

function listingLines() {
  const lst = pickListing(path.join(REPO_ROOT, 'corpus', 'disasm', 'files'));
  return lst ? { file: lst, lines: fs.readFileSync(lst, 'utf8').split('\n') } : null;
}

function lineOfEa(ea) {
  const l = listingLines();
  if (!l) return null;
  const want = `:${ea.toString(16).toUpperCase().padStart(8, '0')} `;
  return l.lines.find((x) => x.includes(want)) ?? null;
}

/**
 * 一个 EA 属于哪个函数（语料在场时才给；不在场就明说）。
 * ★ 判据：取 `ea <= target` 里**起点最大**的那个函数，再用下一个函数的起点当上界 ——
 *   不能拿 `endp` 那一行（它通常没有 EA），否则除函数首行外一律判不到。
 */
function enclosingFn(ea) {
  const l = listingLines();
  if (!l) return null;
  const spans = functionSpans(l.lines).filter((s) => Number.isFinite(s.ea)).sort((a, b) => a.ea - b.ea);
  let hit = null;
  for (const s of spans) {
    if (s.ea > ea) break;
    hit = s;
  }
  if (!hit) return null;
  const i = spans.indexOf(hit);
  const next = spans[i + 1];
  return next ? (ea < next.ea ? hit.sym : null) : hit.sym;
}

function eaOfEnd(span, lines) {
  const m = /^\.text:([0-9A-F]{8}) /.exec(lines[span.to] ?? '');
  return m ? Number.parseInt(m[1], 16) : span.ea;
}

export async function main(argv = process.argv.slice(2)) {
  const args = argv.filter((a) => a !== '');
  const dir = opt(args, '--dir') ?? DEFAULT_DIR;
  const bundle = loadSpec({ dir });
  const spec = bundle.model;
  const relFile = `${bundle.files.spec} + ${bundle.files.bindings} + ${bundle.files.audit}`;

  if (args.includes('--describe')) {
    thisOut(args.includes('--json') ? JSON.stringify(describe(), null, 2) + '\n' : describeText());
    return 0;
  }

  // 证据解析要台账；台账永远在本仓（不是 assets 档）
  const ledger = loadLedger();

  if (args.includes('--validate')) {
    const { checks, problems, derived, coverage } = validate(bundle, { ledger });
    const L = [`spec validate —— ${relFile}（不变量条数见 describe）`, ''];
    for (const c of checks) L.push(`${c.problems.length === 0 ? '✅' : '❌'} #${c.id} ${c.text}`);
    if (problems.length) {
      L.push('');
      L.push(`不变量失败 ${problems.length} 条：`);
      for (const p of problems) L.push(`  - [#${p.check}] ${p.where} —— ${p.why}`);
    }
    L.push('');
    L.push(`试点闭合：块 ${derived.pilotSites} · effect ${derived.effects ?? '（语料不在场）'} · ` +
      `covered ${derived.covered ?? '—'} · internal ${derived.internal ?? '—'} · residual ${derived.residualEffects ?? '—'} · ` +
      `**未登记 ${derived.unregistered.length}**${derived.unregistered.length ? `（${derived.unregistered.map((e) => e.id).join(', ')}）` : ''}`);
    thisOut(`${L.join('\n')}\n`);

    // 站点锚回语料核（assets 档：语料不在场就明说，不静默当绿）
    const b = checkCorpusBindings(spec);
    if (!b.ok && b.why) {
      thisOut(`语料侧对账：跳过（${b.why}）—— 这不等于"语料对上了"\n`);
    } else {
      thisOut(`语料侧对账：${b.problems.length === 0 ? '✅ 站点 EA 都在指令行首' : `❌ ${b.problems.length} 处不一致`}\n`);
      for (const x of b.problems) thisOut(`  - ${x}\n`);
      if (b.problems.length) problems.push(...b.problems.map((why) => ({ check: 11, where: 'corpus', why })));
    }
    return problems.length ? 1 : 0;
  }

  if (args.includes('--effects')) {
    const isPilot = args.includes('--pilot');
    let effects;
    if (isPilot) {
      const r = derivePilotEffects(spec);
      if (!r.ok) { thisErr(`无法枚举试点 effect：${r.why}\n`); return 2; }
      effects = r.effects;
    } else {
      const sym = args[args.indexOf('--effects') + 1];
      if (!sym || !/^sub_[0-9A-F]{6}$/.test(sym)) {
        thisErr('--effects 需要 `--pilot` 或函数符号（sub_XXXXXX）\n');
        return 2;
      }
      const l = listingLines();
      if (!l) { thisErr('语料未解压\n'); return 2; }
      const r = effectsForFunction(l.lines, sym);
      if (!r.ok) { thisErr(`${r.why}\n`); return 2; }
      effects = r.effects;
    }
    if (args.includes('--json')) { thisOut(`${JSON.stringify(effects, null, 2)}\n`); return 0; }
    const byKind = {};
    for (const e of effects) byKind[e.kind] = (byKind[e.kind] ?? 0) + 1;
    const L = [`effect 枚举（机械；语料 = .lst；${isPilot ? '试点块' : '函数'}）`, ''];
    L.push(`effect  ${effects.length}（${Object.entries(byKind).map(([k, n]) => `${k}=${n}`).join(' · ')}）`);
    L.push('');
    for (const e of effects) L.push(`  ${e.block ?? e.site}  ${e.site}  ${e.op.padEnd(6)} ${e.kind.padEnd(12)} ${e.detail}`);
    thisOut(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.includes('--report')) {
    const { derived, weak } = validate(bundle, { ledger });
    thisOut(reportText(spec, { file: relFile, derived, weak }));
    return 0;
  }

  if (args.includes('--chapters')) {
    thisOut(chaptersText(spec));
    return 0;
  }

  if (args.includes('--show')) {
    const ref = args[args.indexOf('--show') + 1];
    if (!ref) {
      thisErr('--show 需要一个 id 或唯一前缀\n');
      return 2;
    }
    const hit = spec.entries.filter((e) => e.id.startsWith(ref) || e.id.endsWith(ref));
    const gapHit = (spec.gaps ?? []).filter((g) => g.id.startsWith(ref) || g.id.endsWith(ref));
    if (hit.length + gapHit.length !== 1) {
      const all = [...hit.map((e) => e.id), ...gapHit.map((g) => g.id)];
      thisErr(`--show 命中 ${all.length} 条（要唯一）：${all.join(', ') || '(无)'}\n`);
      return 2;
    }
    if (gapHit.length === 1) {
      const g = gapHit[0];
      const L = [`# ${g.id} —— ${g.title}`, '', `章节        ${g.chapter}`, `审计缺口    ${g.note}`];
      if ((g.sites ?? []).length) { L.push('', '机器站点'); for (const x of g.sites) L.push(`  · ${x.ea} —— ${x.what}`); }
      if ((g.residual ?? []).length) { L.push('', '残差'); for (const r of g.residual) L.push(`  [${r.kind}] ${r.id}：${r.what}\n      下一步：${r.next}`); }
      thisOut(`${L.join('\n')}\n`);
      return 0;
    }
    const e = hit[0];
    if (args.includes('--json')) {
      thisOut(`${JSON.stringify({ ...e, evidence: (e.binding?.evidence ?? []).map((ev) => ({ ev, ...resolveEvidence(ev, { ledger }) })) }, null, 2)}\n`);
      return 0;
    }
    const L = [];
    L.push(`# ${e.id} —— ${e.title}`);
    L.push('');
    L.push(`章节        ${e.chapter}`);
    L.push(`状态        ${e.status} · ${e.disposition}`);
    L.push('');
    L.push('规范正文（与分发产物同文）');
    for (const line of String(e.text ?? '').split('\n')) L.push(`  ${line}`);
    if ((e.falsifiers ?? []).length) { L.push(''); L.push(`可证伪      ${e.falsifiers.join(' · ')}`); }
    if ((e.covers ?? []).length) L.push(`覆盖 effect  ${e.covers.map((x) => JSON.stringify(x)).join(' · ')}`);
    L.push('');
    L.push('机器绑定');
    for (const s of e.binding?.sites ?? []) {
      const line = lineOfEa(Number.parseInt(String(s.ea).replace(/^0x/i, ''), 16));
      L.push(`  · ${s.ea} ${line ? line.replace(/^\s+/, '') : '（语料不在场 / 行首不在）'}`);
      L.push(`      ${s.what}`);
    }
    for (const f of e.binding?.functions ?? []) L.push(`  · 函数 ${f}`);
    for (const st of e.binding?.storage ?? []) L.push(`  · storage ${st}`);
    L.push('');
    L.push('证据（现算有效状态）');
    for (const ev of e.binding?.evidence ?? []) {
      const r = resolveEvidence(ev, { ledger });
      L.push(`  ${r.ok ? '✅' : '❌'} ${ev} —— ${r.ok ? (r.kind === 'ledger' ? `effective=${r.effective}` : '用例存在') : r.why}`);
    }
    if ((e.residual ?? []).length) {
      L.push('');
      L.push('残差');
      for (const r of e.residual) L.push(`  [${r.kind}] ${r.id}：${r.what}\n      下一步：${r.next}${r.candidate ? `\n      候选：${r.candidate}` : ''}`);
    }
    thisOut(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.includes('--at')) {
    const ref = args[args.indexOf('--at') + 1];
    if (!ref) {
      thisErr('--at 需要一个 EA（0x…）或函数符号（sub_XXXXXX）\n');
      return 2;
    }
    const L = [`反向查询 ${ref}`, ''];
    if (/^sub_[0-9A-F]{6}$/.test(ref)) {
      const hits = spec.entries.filter((e) => (e.binding?.functions ?? []).includes(ref));
      L.push(`函数绑定：${hits.length} 条`);
      for (const e of hits) L.push(`  · ${e.id}（${e.disposition}）`);
      const l = listingLines();
      const span = l ? functionSpans(l.lines).find((s) => s.sym === ref) : null;
      if (span) L.push(`  语料：${hex(span.ea)}..${hex(eaOfEnd(span, l.lines))}`);
    } else {
      const ea = Number.parseInt(ref.replace(/^0x/i, ''), 16);
      if (!Number.isFinite(ea)) {
        thisErr(`--at 认不出：${ref}\n`);
        return 2;
      }
      const hits = spec.entries.filter((e) => (e.binding?.sites ?? []).some((s) => Number.parseInt(String(s.ea).replace(/^0x/i, ''), 16) === ea));
      const gapHits = (spec.gaps ?? []).filter((g) => (g.sites ?? []).some((x) => Number.parseInt(String(x.ea).replace(/^0x/i, ''), 16) === ea));
      L.push(`站点绑定：${hits.length} 条`);
      for (const g of gapHits) {
        const x = g.sites.find((s) => Number.parseInt(String(s.ea).replace(/^0x/i, ''), 16) === ea);
        L.push(`  · ${g.id}（审计缺口）—— ${x.what}`);
      }
      for (const e of hits) {
        const s = e.binding.sites.find((x) => Number.parseInt(String(x.ea).replace(/^0x/i, ''), 16) === ea);
        L.push(`  · ${e.id}（${e.disposition}）—— ${s.what}`);
      }
      const line = lineOfEa(ea);
      L.push(`  语料：${line ? line.replace(/^\s+/, '') : '（不在场 / 不是指令行首）'}`);
      const fn = enclosingFn(ea);
      L.push(`  所属函数：${fn ?? '（语料不在场时判不了）'}`);
      if (fn) {
        const fnHits = spec.entries.filter((e) => (e.binding?.functions ?? []).includes(fn));
        L.push(`函数级绑定：${fnHits.length} 条`);
        for (const e of fnHits) L.push(`  · ${e.id}（${e.disposition}）—— ${e.title}`);
        const gapFn = (spec.gaps ?? []).filter((g) => (g.functions ?? []).includes(fn));
        if (gapFn.length) {
          L.push(`审计缺口里：${gapFn.length} 条`);
          for (const g of gapFn) L.push(`  · ${g.id} —— ${g.title}`);
        }
      }
    }
    thisOut(`${L.join('\n')}\n`);
    return 0;
  }

  thisErr(HELP);
  return 2;
}

function thisOut(s) {
  process.stdout.write(s);
}
function thisErr(s) {
  process.stderr.write(s);
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
      process.exitCode = 2;
    });
}
