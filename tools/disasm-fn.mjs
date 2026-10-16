#!/usr/bin/env node
/**
 * tools/disasm-fn.mjs —— **CLI**：语料 item 目录与影响面（逐函数 / 逐定义）
 *
 * 经派发器：`pnpm tools disasm-fn <show|list|impact|globals|args|callers|callees|data|catalog|summary|modules|describe> [args…]`
 * 也可独立跑：`node tools/disasm-fn.mjs show sub_42CA50`
 *
 * 模型在 `lib/catalog.mjs`（口径、边来源、诚实项都写在那里）；本文件只做"参数 → 模型 → 打印"。
 * ★ 与 `pnpm tools disasm-at` 的分工：那个是**定位与切片**（"某个 EA 在哪一行"、`--at` / `--cases` / 索引），
 *   这个是**item 层**（"这个函数是什么、谁调它、它碰了哪些全局、它的参数在哪个偏移"）。
 *   旧 `disasm-at --pseudo` 已删除：它每次重扫 5 MB 的 `.c`、且"原型 vs 定义"判据会把 40 个带尾注释的
 *   原型当成定义（给出一行"C 体"而不报错）—— 判据现在只有一份，就是 `lib/catalog.mjs` 的 `readSignature`。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  BASELINE_KEY,
  buildCatalog,
  calleesOf,
  callersOf,
  catalogSummary,
  describe,
  findFunction,
  globalsOf,
  impact,
  listModules,
  resolveModule,
  serializeCatalog,
  whoTouchesGlobal,
} from './lib/catalog.mjs';
import { DEFAULT_MAX_LINES, parseEa } from './lib/disasm.mjs';

export const DOMAIN = {
  // ★ 域 id 与 `disasm-at`（定位与切片）分开：本域动的是**派生的 item 目录**，那个域动的是"EA → 行号"。
  id: 'disasm-fn',
  title: '反汇编语料的 **item 目录与影响面**（逐函数 / 逐定义；两侧关联；调用链 / 全局变量 / 参数偏移）',
  data: [
    '`corpus/disasm/files/*.{lst,c}`（**只读**语料；解压产物，gitignore）',
    '`.cache/disasm/*.catalog.txt`（**派生快照**：一 item 一行；可删可重建）',
  ],
  access: 'r（**只读**语料；只有 `catalog --write` 写 `.cache/`，不入库）',
  tool: 'tools/disasm-fn.mjs',
};

export const OPERATIONS = [
  { name: 'modules', argv: ['--modules'], mutates: false, summary: '列出语料对（键 / `.lst` / `.c` / 哪个是基线）' },
  { name: 'summary', argv: ['--summary'], mutates: false, summary: '目录一览 + 诚实项 + 不变量自检（`catalogProblems`）' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '逐 item 列表：`[--match S] [--limit N] [--no-c] [--sort ea|callers|globals|args]`' },
  { name: 'show', argv: ['--show'], mutates: false, summary: '★ **主入口**：一个函数的全貌（两侧区间 + 参数 + 全局 + 调用关系 + 取址证据）；`--lst` / `--c` / `--both` 选打印哪一侧' },
  { name: 'callers', argv: ['--callers'], mutates: false, summary: '谁调用它（权威边 `call`/`tail`；另有"取址证据"另册）' },
  { name: 'callees', argv: ['--callees'], mutates: false, summary: '它调用谁（权威边 + `.c` 侧补充；间接调用只计数）' },
  { name: 'globals', argv: ['--globals'], mutates: false, summary: '它碰了哪些全局（`--who <名|EA>` 则反过来：谁碰这个全局）' },
  { name: 'args', argv: ['--args'], mutates: false, summary: '参数的栈偏移 / 寄存器（★ 名字优先、顺序兜底的配对，并标出配对依据）' },
  { name: 'impact', argv: ['--impact'], mutates: false, summary: '★ **影响面闭包**：`--up`（谁会到它）/ `--down`（它依赖谁），`--depth N --max N`；**必带下界说明**' },
  { name: 'data', argv: ['--data'], mutates: false, summary: '数据 item（定义）列表：`[--match S] [--limit N]`；`--who <名|EA>` = 谁引用它' },
  { name: 'catalog', argv: ['--catalog'], mutates: true, summary: '打印/落盘**派生快照**到 `.cache/disasm/`（缺省 dry-run）[--write]' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：item 字段 / 边来源 / 不变量 / 诚实项（schema 的真源）' },
];

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');
const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const CACHE_DIR = path.join(REPO_ROOT, '.cache', 'disasm');

const HELP = `tools/disasm-fn —— 语料 **item 目录与影响面**（逐函数 / 逐定义；只读语料）

  node tools/disasm-fn.mjs modules
  node tools/disasm-fn.mjs summary [--module age]
  node tools/disasm-fn.mjs list [--match sub_42] [--limit 30] [--sort callers]
  node tools/disasm-fn.mjs show sub_42CA50            # ★ 主入口：两侧区间 + 参数 + 全局 + 调用关系
  node tools/disasm-fn.mjs show 0x42CA50 --both        # 给 EA 也行（不是函数起点会明说）
  node tools/disasm-fn.mjs show sub_42CA50 --lst       # 换成打印 .lst 那一侧（逐字核验用）
  node tools/disasm-fn.mjs callers sub_415640
  node tools/disasm-fn.mjs globals sub_401100
  node tools/disasm-fn.mjs globals --who dword_559A68  # 反过来：谁碰这个全局（953 个函数）
  node tools/disasm-fn.mjs args sub_401100
  node tools/disasm-fn.mjs impact sub_415640 --up --depth 3      # ★ 影响面（改它会牵扯谁）
  node tools/disasm-fn.mjs impact sub_42CA50 --down --depth 4    # ★ 它依赖谁
  node tools/disasm-fn.mjs data --match off_ [--limit 20]
  node tools/disasm-fn.mjs catalog --write             # 派生快照 → .cache/disasm/<module>.catalog.txt

★ 锚点锚 **EA**：两侧行区间（\`lst=\` / \`c=\`）是**派生值**，换一次导出就全变 —— **不许当锚**。
★ 任何"调用闭包"都只是**下界**：间接调用（\`call eax\` / \`call [esi+4]\` / \`call ds:dword_X\`）解不出目标。
★ 缺事实**报出来**：没有 C 的函数、对不上的 C 定义、跨模块的 callee、只被引用没声明的栈槽 —— 都列出来。
`;

function parseArgs(argv) {
  const out = {
    op: null, module: null, sym: null, ea: null, match: null, limit: 50, lines: DEFAULT_MAX_LINES,
    sort: 'ea', write: false, json: false, noC: false, direction: null, depth: 3, max: 200,
    who: null, side: 'c',
  };
  const OPS = OPERATIONS.map((o) => o.name);
  const takesValue = new Set(['module', 'match', 'limit', 'lines', 'sort', 'depth', 'max', 'who', 'c', 'lst']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') out.op = 'help';
    else if (a === '--json') out.json = true;
    else if (a === '--write') out.write = true;
    else if (a === '--no-c') out.noC = true;
    else if (a === '--up') out.direction = 'up';
    else if (a === '--down') out.direction = 'down';
    else if (a === '--lst') out.side = 'lst';
    else if (a === '--both') out.side = 'both';
    else if (a === '--c') out.side = 'c';
    else if (a.startsWith('--') && OPS.includes(a.slice(2))) out.op = a.slice(2);
    else if (a.startsWith('--')) {
      const key = a.slice(2);
      if (!takesValue.has(key)) throw new Error(`未知参数：${a}（--help 看用法）`);
      const v = argv[i + 1];
      if (v === undefined) throw new Error(`${a} 需要一个值`);
      i += 1;
      if (key === 'lines' || key === 'limit' || key === 'depth' || key === 'max') out[key] = Number.parseInt(v, 10);
      else out[key] = v;
    } else if (out.op === null && OPS.includes(a)) {
      out.op = a; // 位置参数形式：`node tools/disasm-fn.mjs show sub_X`
    } else if (out.sym === null && out.ea === null) {
      // ★ 位置参数：`sub_XXXXXX` / `_WinMain@16` 这类是符号；纯十六进制是 EA
      if (/^(0x)?[0-9A-Fa-f]{6,8}$/.test(a)) out.ea = a;
      else out.sym = a;
    } else throw new Error(`多余的参数：${a}`);
  }
  // `--who` 单独出现时默认问"谁碰这个全局"（`disasm-fn globals --who X`）
  if (out.op === null && out.who) out.op = 'globals';
  return out;
}

/** 载入目录（默认基线那一对；`--module` 选别的） */
function loadCatalog(args) {
  if (args.noC) {
    const mod = resolveModule(FILES_DIR, args.module);
    if (!mod) throw new Error(`找不到语料对 ${JSON.stringify(args.module ?? BASELINE_KEY)}（\`modules\` 看有哪些）`);
    return buildCatalog({ lstFile: mod.lstFile, cFile: null, moduleKey: mod.key });
  }
  const mod = resolveModule(FILES_DIR, args.module);
  if (!mod) {
    const keys = listModules(FILES_DIR).map((m) => m.key).join(' / ');
    throw new Error(`找不到语料对 ${JSON.stringify(args.module ?? BASELINE_KEY)}；现有：${keys || '（corpus/disasm/files/ 里没有 .lst）'}`);
  }
  return buildCatalog({ lstFile: mod.lstFile, cFile: mod.cFile, moduleKey: mod.key });
}

const hex = (n) => `0x${n.toString(16)}`;
const sourceLine = (cat, side) => {
  const f = side === 'lst' ? cat.lstFile : cat.cFile;
  return f ? `${path.relative(REPO_ROOT, f)}` : '（不在场）';
};

function printItem(cat, found, args) {
  const { item: f, isStart, queriedEa } = found;
  const L = [];
  L.push(`函数        ${f.sym}（起点 ${hex(f.ea)}）${isStart ? '' : `　★ 你给的 ${hex(queriedEa)} 不是函数起点，它属于本函数`}`);
  L.push(`段 / 约定   ${f.seg} / ${f.conv ?? '（无 C，约定未知）'}`);
  L.push(`.lst        ${sourceLine(cat, 'lst')} 行 ${f.fromLine}-${f.toLine}（${f.toLine - f.fromLine + 1} 行）`);
  L.push(`.c          ${f.hasC ? `${sourceLine(cat, 'c')} 行 ${f.c.from}-${f.c.to}（${f.c.to - f.c.from + 1} 行）` : '★ 没有定义（Hex-Rays 没反编译它）—— 只按 .lst 读'}`);
  if (f.convNote) L.push(`★ 约定注记  ${f.convNote}`);
  if (f.pairNotes.length > 0) L.push(`★ 参数注记  ${f.pairNotes.join('；')}`);
  L.push('');
  L.push(`参数        ${f.args.length === 0 ? '（无）' : ''}`);
  for (const a of f.args) {
    const where = a.reg ? `<${a.reg}>` : a.offset === null ? '（偏移不可判定）' : `[ebp${a.offset >= 0 ? '+' : ''}${a.offset}]`;
    L.push(
      `            ${String(a.name).padEnd(16)} ${where.padEnd(16)} ${String(a.width).padStart(2)} 字节` +
        `　${a.cName ?? '（.c 没有对应参数）'}${a.cType ? ` : ${a.cType}` : ''}` +
        `　[配对依据 ${a.pairBy}${a.pairBy === 'ref' ? '；.lst 未声明，由引用推偏移' : ''}]`,
    );
  }
  const localsShown = f.locals.length + f.refOnly.filter((r) => r.kind === 'local').length;
  L.push(`局部        ${localsShown} 个槽（不逐个列；` + (f.locals.length ? `声明 ${f.locals.length}` : '无声明') + `${f.refOnly.some((r) => r.kind === 'local') ? `，另有 ${f.refOnly.filter((r) => r.kind === 'local').length} 个只被引用` : ''}）`);
  const unknown = f.refOnly.filter((r) => r.kind === 'unknown');
  if (unknown.length > 0) L.push(`★ 未解槽    ${unknown.map((u) => `${u.name}×${u.hits}`).join(' ')}（不是 \`arg_XX\`/\`var_XX\` 形态 ⇒ 偏移算不出，不猜）`);
  L.push('');
  L.push(`全局        ${f.globals.length === 0 ? '（没碰全局）' : `${f.globals.length} 个`}`);
  for (const g of f.globals.slice(0, 12)) {
    L.push(`            ${g.name.padEnd(18)} ${g.rw === 'rw' ? '读+写' : g.rw === 'w' ? '写  ' : '读  '} ×${g.hits}　@${hex(g.ea)}`);
  }
  if (f.globals.length > 12) L.push(`            …还有 ${f.globals.length - 12} 个（--json 拿全）`);
  L.push('');
  L.push(`调用        ${f.calleeSyms.length} 个直接目标${f.calleeSyms.length ? `：${f.calleeSyms.slice(0, 8).join(' ')}${f.calleeSyms.length > 8 ? ' …' : ''}` : ''}`);
  const callers = callersOf(cat, f);
  L.push(`调用方      ${callers.length} 个${callers.length ? `：${callers.slice(0, 8).map((c) => `${c.from}(${c.kind})`).join(' ')}${callers.length > 8 ? ' …' : ''}` : ''}`);
  const addr = cat.addrTaken.get(f.sym) ?? [];
  if (addr.length > 0) {
    L.push(`★ 取址证据  ${addr.length} 处 —— 这些是**可能调用方**（动态分派 / 函数指针表），本工具**不猜**：`);
    for (const a of addr.slice(0, 6)) L.push(`            ${a.from}${a.kind === 'table' ? `（表项 @${hex(a.fromEa)}）` : ''} [${a.kind}]`);
    if (addr.length > 6) L.push(`            …还有 ${addr.length - 6} 处`);
  }
  L.push(`间接调用点  ${f.indirectCallSites} 个（本函数体内解不出目标的调用点${f.indirectCallSites ? ' ⇒ 任何"它调用了谁"的清单都只是**下界**' : ''}）`);
  if (f.cExternalCalls.size > 0) L.push(`外部/库调用 ${[...f.cExternalCalls].slice(0, 10).join(' ')}${f.cExternalCalls.size > 10 ? ' …' : ''}（来自 .c）`);
  L.push('');
  L.push(`★ 行区间是**派生值**（不是锚）：换一次导出就全变；锚只锚 EA ${hex(f.ea)}。`);
  return L;
}

/** 打印正文（有界；截断**明说**） */
function printBody(cat, f, side, lines) {
  const L = [];
  if (side === 'lst' || side === 'both') {
    const all = fs.readFileSync(cat.lstFile, 'utf8').split('\n');
    const from = f.fromLine;
    const to = Math.min(f.toLine, from + lines - 1);
    L.push(`--- .lst ${from}-${to}${to < f.toLine ? `（★ 截断：本节共 ${f.toLine - f.fromLine + 1} 行，只给前 ${to - from + 1} 行 —— --lines 调大）` : ''} ---`);
    for (let i = from; i <= to; i += 1) L.push(`${String(i).padStart(7)}  ${all[i - 1] ?? ''}`);
    L.push('');
  }
  if (!f.hasC) {
    if (side !== 'lst') L.push('（没有 C 体可给 —— 用 `--lst` 读 .lst；★ 这不代表"这段没有逻辑"）');
    return L;
  }
  if (side === 'c' || side === 'both') {
    const all = fs.readFileSync(cat.cFile, 'utf8').split('\n');
    const from = f.c.from;
    const to = Math.min(f.c.to, from + lines - 1);
    L.push(`--- .c ${from}-${to}${to < f.c.to ? `（★ 截断：本函数共 ${f.c.to - f.c.from + 1} 行，只给前 ${to - from + 1} 行 —— --lines 调大）` : ''} ---`);
    for (let i = from; i <= to; i += 1) L.push(`${String(i).padStart(7)}  ${all[i - 1] ?? ''}`);
    L.push('');
    L.push('★ C 是 Hex-Rays 的**改写**（类型 / 变量名 / 结构都是它的推断）⇒ 结论必须回 .lst 核验，锚记函数起点 EA。');
  }
  return L;
}

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.op === null) {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.op === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.op === 'describe') {
    const d = describe();
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ domain: DOMAIN, ...d, operations: OPERATIONS }, null, 2)}\n`);
      return 0;
    }
    const L = [`# ${d.file} —— 自描述（schema 的真源；文档不复述）`, '', `用途：${d.purpose}`, `★ 这里**不**做：${d.notHere}`, ''];
    L.push('## item 字段');
    for (const [k, v] of d.itemFields) L.push(`* \`${k}\` —— ${v}`);
    L.push('', `## 边来源（闭集合）`, ...d.edgeKinds.map((k) => `* \`${k}\``));
    L.push('', '## 不变量（含"谁在守它"）');
    for (const i of d.invariants) L.push(`${i.id}. ${i.text}　—　${i.enforcedBy}`);
    L.push('', '## ★ 必须一起读的诚实项');
    for (const h of d.honesty) L.push(`* ${h}`);
    L.push('', '## 操作');
    for (const o of OPERATIONS) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'modules') {
    const mods = listModules(FILES_DIR);
    if (mods.length === 0) throw new Error(`corpus/disasm/files/ 里没有任何 .lst（先 \`pnpm tools disasm build\` 或解压 zip）`);
    if (args.json) {
      process.stdout.write(`${JSON.stringify(mods, null, 2)}\n`);
      return 0;
    }
    for (const m of mods) {
      process.stdout.write(`${m.key.padEnd(12)} ${m.baseline ? '★ 基线' : '       '} ${path.relative(REPO_ROOT, m.lstFile)}${m.cFile ? ' + .c' : '（**没有 .c**）'}\n`);
    }
    return 0;
  }

  const cat = loadCatalog(args);

  // ★ `--who` 问的是"谁碰这个全局/数据 item" ⇒ **不需要**先定位一个函数（旧实现在这里抛错过）
  if (args.who) {
    const isEa = /^(0x)?[0-9A-Fa-f]{4,8}$/.test(args.who);
    const w = whoTouchesGlobal(cat, { name: isEa ? null : args.who, ea: isEa ? parseEa(args.who) : null });
    const d = w.item;
    const L = [`数据 item   ${w.name}${d ? `（@${hex(d.ea)}，${d.dir}，约 ${d.sizeBytes} 字节，.lst 行 ${d.fromLine}-${d.toLine}${d.c ? `；.c 行 ${d.c.line}` : ''}）` : '（语料里没有这个名字的数据标签）'}`];
    if (d && d.tableTargets.length > 0) L.push(`表项        ${d.tableTargets.join(' ')}（函数指针表 / 跳转表）`);
    L.push(`引用它的函数 ${w.users.length} 个${w.users.length ? '：' : ''}`);
    for (const u of w.users.slice(0, args.limit)) L.push(`            ${u.sym.padEnd(16)} ${u.rw} ×${u.hits}　@${hex(u.ea)}`);
    if (w.users.length > args.limit) L.push(`            …还有 ${w.users.length - args.limit} 个（--limit 调大）`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.op === 'summary') {
    const s = catalogSummary(cat);
    if (args.json) {
      process.stdout.write(`${JSON.stringify(s, null, 2)}\n`);
      return 0;
    }
    const L = [
      `语料        ${s.module}（${s.lst}${s.c ? ' + ' + s.c : '（没有 .c）'}）`,
      `sha256      lst ${s.sha.lst.slice(0, 16)}…${s.sha.c ? `  c ${s.sha.c.slice(0, 16)}…` : ''}`,
      '',
      `函数        ${s.functions}（有 C ${s.withC}；★ 没有 C ${s.withoutC} —— 那是"Hex-Rays 没反编译"，不是"没有逻辑"）`,
      `数据 item   ${s.dataItems}（带 EA 的自动名 + 可读名标签）`,
      `调用点      ${s.callSitesTotal} 个；其中**解不出目标**的 ${s.indirectCallSites} 个（⇒ 闭包是下界）`,
      `边          ${s.callEdges} 条去重后的直接边；函数指针表项 ${s.tableEdges}；被取址目标 ${s.addrTakenTargets} 个`,
      `C 侧        ${s.cDefs} 个定义，对不上 .lst 的 ${s.cDefsUnmatched}；数据定义 ${s.cDataDefs}（挂上 ${s.cDataMatched}，对不上 ${s.cDataUnmatched}）`,
      `跨模块      callee 不在本文件里的 ${s.calleesNotInInventory.length} 个${s.calleesNotInInventory.length ? `：${s.calleesNotInInventory.slice(0, 6).join(' ')}…` : ''}`,
      '',
      `不变量自检  ${s.problems.length === 0 ? '✅ 全过' : `❌ ${s.problems.length} 条`}`,
    ];
    for (const p of s.problems.slice(0, 10)) L.push(`  - ${p}`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.op === 'catalog') {
    const out = path.join(CACHE_DIR, `${cat.module}.catalog.txt`);
    const text = serializeCatalog(cat);
    if (!args.write) {
      process.stdout.write(`dry-run：会写 ${path.relative(REPO_ROOT, out)}（${(text.length / 1024).toFixed(0)} KB，${cat.functions.length} 函数 + ${cat.data.length} 数据 item）\n加 --write 落盘。\n`);
      return 0;
    }
    fs.mkdirSync(CACHE_DIR, { recursive: true });
    fs.writeFileSync(out, text);
    const back = fs.readFileSync(out, 'utf8');
    if (back !== text) throw new Error('写后回读不一致 —— 已中止（磁盘问题？）');
    process.stdout.write(`已写 ${path.relative(REPO_ROOT, out)}（${(text.length / 1024).toFixed(0)} KB）；回读复验 ✅\n`);
    return 0;
  }

  if (args.op === 'list') {
    let rows = cat.functions.slice();
    if (args.noC) rows = rows.filter((f) => !f.hasC);
    if (args.match) rows = rows.filter((f) => f.sym.includes(args.match));
    const sortKey = {
      ea: (f) => f.ea,
      callers: (f) => -(cat.callers.get(f.sym)?.length ?? 0),
      globals: (f) => -f.globals.length,
      args: (f) => -f.args.length,
    }[args.sort];
    if (!sortKey) throw new Error(`--sort 只认 ea|callers|globals|args（给的是 ${args.sort}）`);
    rows.sort((a, b) => (sortKey(a) === sortKey(b) ? a.ea - b.ea : sortKey(a) - sortKey(b)));
    const shown = rows.slice(0, args.limit);
    if (args.json) {
      process.stdout.write(`${JSON.stringify(shown.map((f) => ({ ea: f.ea, sym: f.sym, lst: [f.fromLine, f.toLine], c: f.c ? [f.c.from, f.c.to] : null, args: f.args.length, globals: f.globals.length, callers: cat.callers.get(f.sym)?.length ?? 0, indirect: f.indirectCallSites })), null, 2)}\n`);
      return 0;
    }
    const L = [`共 ${rows.length} 个 item${shown.length < rows.length ? `；★ 只列前 ${shown.length} 个（--limit 调大）` : ''}`];
    L.push('EA        sym             lst 区间        C 区间          参 全局 调用方 间接');
    for (const f of shown) {
      L.push(
        `${hex(f.ea).padEnd(9)} ${f.sym.padEnd(15)} ${`${f.fromLine}-${f.toLine}`.padEnd(14)} ` +
          `${(f.c ? `${f.c.from}-${f.c.to}` : '★ 无 C').padEnd(14)} ` +
          `${String(f.args.length).padStart(2)} ${String(f.globals.length).padStart(4)} ${String(cat.callers.get(f.sym)?.length ?? 0).padStart(6)} ${String(f.indirectCallSites).padStart(4)}`,
      );
    }
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.op === 'data') {
    let rows = cat.data.slice();
    if (args.match) rows = rows.filter((d) => d.name.includes(args.match) || (d.c && d.c.raw.includes(args.match)));
    const shown = rows.slice(0, args.limit);
    if (args.json) {
      process.stdout.write(`${JSON.stringify(shown.map((d) => ({ ea: d.ea, name: d.name, dir: d.dir, lst: [d.fromLine, d.toLine], size: d.sizeBytes, table: d.tableTargets.length, cLine: d.c?.line ?? null })), null, 2)}\n`);
      return 0;
    }
    const L = [`共 ${rows.length} 个数据 item${shown.length < rows.length ? `；★ 只列前 ${shown.length} 个（--limit 调大）` : ''}`];
    L.push('EA        name                类型   .lst 行       约字节 表项 .c 行');
    for (const d of shown) {
      L.push(
        `${hex(d.ea).padEnd(9)} ${d.name.padEnd(19)} ${d.dir.padEnd(5)} ${`${d.fromLine}-${d.toLine}`.padEnd(14)} ` +
          `${String(d.sizeBytes).padStart(5)} ${String(d.tableTargets.length).padStart(4)} ${d.c ? String(d.c.line) : '-'}`,
      );
    }
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  // 其余操作都需要定位一个函数
  const found = findFunction(cat, { sym: args.sym, ea: args.ea === null ? null : parseEa(args.ea) });
  const f = found.item;

  if (args.op === 'show') {
    if (args.json) {
      process.stdout.write(`${JSON.stringify({
        ...f,
        callees: calleesOf(cat, f),
        callers: callersOf(cat, f),
        addrEvidence: cat.addrTaken.get(f.sym) ?? [],
        globals: f.globals,
      }, null, 2)}\n`);
      return 0;
    }
    const L = printItem(cat, found, args);
    L.push(...printBody(cat, f, args.side, args.lines));
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'callers') {
    const rows = callersOf(cat, f);
    const addr = cat.addrTaken.get(f.sym) ?? [];
    const L = [`${f.sym} 的调用方（**权威边**：.lst 里的 call/jmp）：${rows.length} 个`];
    for (const r of rows) L.push(`  ${r.from.padEnd(16)} ${r.kind.padEnd(5)} ×${r.hits}　@${hex(r.fromEa)}`);
    if (addr.length > 0) {
      L.push('');
      L.push(`★ 另有 ${addr.length} 处"取址证据"（**可能调用方**，动态分派 ⇒ 本工具不猜它会不会真调到）：`);
      for (const a of addr) L.push(`  ${a.from.padEnd(16)} ${a.kind}${a.kind === 'table' ? ` @${hex(a.fromEa)}` : ''}`);
    }
    L.push('');
    L.push(`★ 本函数体内**解不出目标**的调用点 ${f.indirectCallSites} 个；闭包因此只是**下界**。`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'callees') {
    const rows = calleesOf(cat, f);
    const L = [`${f.sym} 调用了谁（**权威边** + \`.c\` 侧补充）：`];
    L.push('  .lst 权威边：');
    for (const r of rows) L.push(`    ${r.to.padEnd(16)} ${r.kind.padEnd(5)} ×${r.hits}${r.ea === null ? '　★ 不在本文件（跨模块 / 导入）' : ''}`);
    if (f.cCalls.size > 0) {
      const lstSet = new Set(rows.map((r) => r.to));
      const extra = [...f.cCalls.keys()].filter((s) => !lstSet.has(s));
      L.push(`  .c 侧点名 ${f.cCalls.size} 个${extra.length ? `；其中 ${extra.length} 个 .lst 里没有（Hex-Rays 内联/折叠造成的差，**不合并**）：${extra.slice(0, 8).join(' ')}` : '（与 .lst 一致）'}`);
    }
    if (f.cAddrTaken.size > 0) L.push(`  .c 侧**取址**（不是调用）：${[...f.cAddrTaken].slice(0, 8).join(' ')}`);
    L.push(`  ★ 解不出目标的调用点 ${f.indirectCallSites} 个 ⇒ 上面这份清单是**下界**。`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'globals') {
    const g = globalsOf(cat, f);
    const L = [`${f.sym} 碰过的全局：${g.length} 个（读/写按**操作数位置**判定，不是数据流分析）`];
    for (const x of g) L.push(`  ${x.name.padEnd(20)} ${x.rw === 'rw' ? '读+写' : x.rw === 'w' ? '写  ' : '读  '} ×${x.hits}　@${hex(x.ea)}`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'args') {
    const L = [`${f.sym}（${f.conv ?? '约定未知'}）的参数与偏移`];
    if (f.convNote) L.push(`★ ${f.convNote}`);
    L.push('  名字              位置              宽  .c 参数        配对依据');
    for (const a of f.args) {
      const where = a.reg ? `<${a.reg}>` : a.offset === null ? '（不可判定）' : `[ebp${a.offset >= 0 ? '+' : ''}${a.offset}]`;
      L.push(`  ${String(a.name).padEnd(17)} ${where.padEnd(17)} ${String(a.width).padStart(2)}  ${(a.cName ?? '-').padEnd(14)} ${a.pairBy}${a.pairBy === 'ref' ? '（.lst 未声明，由引用推偏移）' : ''}`);
    }
    if (f.pairNotes.length > 0) L.push(`★ ${f.pairNotes.join('；')}`);
    L.push('★ 配对规则：**先按名字**（两边共用同一份数据库 ⇒ 名字常常逐字相同），剩下的**按顺序**；两边各多出来的明说。');
    L.push('★ 跨侧比**结构偏移**要小心：`.c` 是十进制（`this + 280`）、`.lst` 是十六进制（`[esi+118h]`）—— 同一个偏移。');
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }
  if (args.op === 'impact') {
    const direction = args.direction ?? 'up';
    const r = impact(cat, f, { direction, depth: args.depth, maxNodes: args.max });
    if (args.json) {
      process.stdout.write(`${JSON.stringify(r, null, 2)}\n`);
      return 0;
    }
    const L = [
      `影响面（${direction === 'up' ? '**谁会到它**（改它的影响面）' : '**它依赖谁**'}）根：${f.sym} @${hex(f.ea)}，深度 ${r.depth}`,
      `命中 ${r.nodes.length} 个节点 / ${r.edges.length} 条边${r.truncated ? `　★ **截断**（--max ${args.max} 到顶，不是"就这些"）` : ''}`,
    ];
    const byDepth = new Map();
    for (const n of r.nodes) {
      if (!byDepth.has(n.depth)) byDepth.set(n.depth, []);
      byDepth.get(n.depth).push(n);
    }
    for (const [d, ns] of [...byDepth].sort((a, b) => a[0] - b[0])) {
      L.push(`  深度 ${d}（${ns.length}）：${ns.slice(0, 12).map((n) => n.sym).join(' ')}${ns.length > 12 ? ' …' : ''}`);
    }
    L.push('');
    L.push('★ 这份闭包是**下界**：');
    L.push(`  · 命中的函数体内共有 ${r.honesty.indirectInReached} 个**解不出目标**的调用点（全语料 ${r.honesty.indirectInCorpus} 个）`);
    if (r.honesty.addrEvidence.length > 0) {
      L.push(`  · 根函数另有 ${r.honesty.addrEvidence.length} 处**取址证据**（动态分派 ⇒ 可能调用方，本工具不猜）：${r.honesty.addrEvidence.slice(0, 5).map((a) => a.from).join(' ')}`);
    }
    if (args.direction === null) L.push('  · 缺省方向是 `--up`（谁会到它）；要"它依赖谁"用 `--down`。');
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  process.stderr.write(`未知动作：${args.op}\n${HELP}`);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    process.exitCode = 2;
  }
}
