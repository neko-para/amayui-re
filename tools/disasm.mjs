#!/usr/bin/env node
/**
 * tools/disasm.mjs —— **CLI**：反汇编语料的机械提取（EA → 行号 → 有界上下文）
 *
 * 经派发器：`pnpm tools disasm <stats|at|span|search|index> [args…]`
 * 也可独立跑：`node tools/disasm.mjs --at --ea 0x401000 --lines 40`
 *
 * 模型在 `lib/disasm.mjs`（口径与"为什么不许漫游"都写在那里）；本文件只做"参数 → 模型 → 输出"。
 * ★ 与 `pnpm tools disasm` 的另外两个动作（`verify` / `build` / `restore`）是**两件事**：
 *   那两个管**语料本身的保真**（转写 / 压缩包 / 反解），本文件**只读**语料做定位与切片。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  DOMAIN,
  DEFAULT_MAX_LINES,
  OPERATIONS,
  buildIndex,
  contextAt,
  describe,
  describeText,
  enclosingFunction,
  indexProblems,
  indexSummary,
  parseEa,
  parseIndexText,
  pseudoOfFunction,
  search,
  serializeIndex,
  spanOfFunction,
  switchTables,
  withSegRanges,
} from './lib/disasm.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');
const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const CACHE_DIR = path.join(REPO_ROOT, '.cache', 'disasm');

const HELP = `tools/disasm.mjs —— 反汇编语料的机械提取（只读语料；不许在大文件里漫游）

  node tools/disasm.mjs --stats                          # 语料盘点：段 / 行数 / EA 范围 / 单调性
  node tools/disasm.mjs --at --ea 0x401000 [--lines 60]   # ★ 取有界上下文（被截断会明说）
  node tools/disasm.mjs --at --ea 0x401000 --fn           # 取它所在函数的区间
  node tools/disasm.mjs --search --match "某串" [--regex] [--limit 50]
  node tools/disasm.mjs --cases --ea 0x41BF50             # ★ 机械枚举函数里的 switch/跳转表（case 数 + 目标）
  node tools/disasm.mjs --pseudo --ea 0x40D500            # ★ 该 EA 所属函数的 Hex-Rays C 体 + .lst 行区间
  node tools/disasm.mjs --pseudo --sym sub_415640 [--lines 200]
  node tools/disasm.mjs --span --match "某串"              # 只看命中位置的**行号区间**
  node tools/disasm.mjs --index [--write]                 # 建/刷新派生索引到 .cache/（缺省 dry-run）

★ 锚点锚 **EA，不锚行号**：\`EA → 行号\` 由本工具现算；换一次反汇编只重建映射。
★ 索引是**派生缓存**（\`.cache/\`，可删可重建），**不是真源**。
★ 语料**只读**：本工具不改它一个字。
`;

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, lines: DEFAULT_MAX_LINES, limit: 200, regex: false, fn: false, file: null, sym: null };
  const takesValue = new Set(['ea', 'match', 'lines', 'limit', 'seg', 'file', 'sym']);
  const ACTIONS = ['stats', 'at', 'span', 'search', 'cases', 'pseudo', 'index', 'describe', 'help'];
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') out.action = 'help';
    else if (a.startsWith('--') && ACTIONS.includes(a.slice(2))) out.action = a.slice(2);
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--regex') out.regex = true;
    else if (a === '--fn') out.fn = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} 需要一个值`);
      out[k] = k === 'lines' || k === 'limit' ? Number(v) : v;
    } else throw new Error(`多余的位置参数：${a}（本工具的参数都是 flag 形态）`);
  }
  if (!out.action) out.action = 'stats';
  return out;
}

/** 语料：默认取 `files/` 里第一个 `.lst`；可用 `--file` 指定（相对仓库根或绝对路径） */
function pickListing(override) {
  if (override) {
    const p = path.isAbsolute(override) ? override : path.join(REPO_ROOT, override);
    if (!fs.existsSync(p)) throw new Error(`--file 不存在：${p}`);
    return p;
  }
  if (!fs.existsSync(FILES_DIR)) {
    throw new Error(
      `语料没解压：${path.relative(REPO_ROOT, FILES_DIR)} 不存在。\n` +
        `（它是 gitignore 的派生物 ⇒ 先 \`pnpm tools disasm build\` 或 \`restore\`。★ 缺事实就报错，不"跳过"。）`,
    );
  }
  const lst = fs.readdirSync(FILES_DIR).filter((f) => f.endsWith('.lst')).sort();
  if (!lst.length) throw new Error(`${path.relative(REPO_ROOT, FILES_DIR)} 里没有 .lst`);
  return path.join(FILES_DIR, lst[0]);
}

/** 索引缓存：命中且**自洽**就用它，否则现建（**不写盘**，除非 `--index --write`） */
function loadOrBuildIndex(file) {
  const cacheFile = path.join(CACHE_DIR, `${path.basename(file)}.idx.txt`);
  if (fs.existsSync(cacheFile)) {
    const txt = fs.readFileSync(cacheFile, 'utf8');
    const parsed = parseIndexText(txt);
    // ★ 廉价校验用「大小 + 行数」而不是 sha256（每次现算太重）；不一致就重建
    //   —— 宁可重建，也不可用陈旧索引（陈旧索引会给出**看起来对、其实错**的行号）。
    // ★ 还要**自洽校验**：只看"非空"不够 —— 一份读过不回来的索引（段名漏匹配）会让
    //   `.text` 的地址统统报"不在覆盖范围内"，而那提示听起来像正常的边界情况。
    const st = fs.statSync(file);
    const consistent = parsed.bytes === st.size && parsed.lines && indexProblems({ segs: parsed.segs, marks: parsed.marks }).length === 0;
    if (consistent) {
      return {
        file,
        bytes: st.size,
        sha256: parsed.sha256,
        lines: parsed.lines,
        imagebase: parsed.imagebase,
        // ★ 从缓存读回来的段表也要补 marks 下标区间（不补的话 `--at` 在非 `.text` 段上静默给错）
        segs: withSegRanges(parsed.segs, parsed.marks),
        marks: parsed.marks,
        monotonic: true,
        fromCache: cacheFile,
      };
    }
  }
  return { ...buildIndex(file), fromCache: null };
}

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }

  const file = pickListing(args.file);

  if (args.action === 'index') {
    const idx = buildIndex(file);
    const text = serializeIndex(idx);
    const target = path.join(CACHE_DIR, `${path.basename(file)}.idx.txt`);
    const L = [
      `语料        ${path.relative(REPO_ROOT, file)}（${idx.lines} 行 / ${(idx.bytes / 1048576).toFixed(1)} MB）`,
      `段          ${idx.segs.map((s) => `${s.name}=${s.rows} 行/EAs 0x${s.lo.toString(16)}-0x${s.hi.toString(16)}`).join(' · ')}`,
      `标记        ${idx.marks.length} 条（只在 EA 变化时压一条 ⇒ 稀疏表）`,
      `单调性      ${idx.monotonic ? '✓ 段内 EA 单调不降' : '✗ **不单调**（IDA 导出重排过？索引会错！）'}`,
      `落点        ${path.relative(REPO_ROOT, target)}`,
      `体积        ${(Buffer.byteLength(text) / 1024).toFixed(0)} KB`,
    ];
    if (!args.write) {
      process.stdout.write(`${L.join('\n')}\n\n（dry-run）加 --write 落盘。★ 它是**派生缓存**，随时可删。\n`);
      return idx.monotonic ? 0 : 1;
    }
    fs.mkdirSync(CACHE_DIR, { recursive: true });
    fs.writeFileSync(target, text);
    const back = fs.readFileSync(target, 'utf8');
    if (back !== text) {
      process.stderr.write(`✗ 写后回读复验失败，已删除：${target}\n`);
      fs.rmSync(target, { force: true });
      return 1;
    }
    process.stdout.write(`${L.join('\n')}\n\n已写入      ${target}\n`);
    return idx.monotonic ? 0 : 1;
  }

  const idx = loadOrBuildIndex(file);

  if (args.action === 'stats') {
    const s = indexSummary(idx);
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ ...s, file: path.relative(REPO_ROOT, s.file) }, null, 2)}\n`);
      return 0;
    }
    const L = [
      `语料        ${path.relative(REPO_ROOT, file)}`,
      `体积         ${(idx.bytes / 1048576).toFixed(1)} MB / ${idx.lines} 行`,
      `sha256       ${idx.sha256}`,
      `Imagebase    ${idx.imagebase === null ? '?' : `0x${idx.imagebase.toString(16)}`}`,
      `索引来源     ${idx.fromCache ? path.relative(REPO_ROOT, idx.fromCache) : '**现建**（未落盘；要缓存跑 --index --write）'}`,
      `单调性       ${idx.monotonic ? '✓' : '✗ **不单调**'}`,
      '',
      `${'段'.padEnd(10)} ${'行数'.padStart(8)} ${'EA 范围'.padEnd(24)} 行号范围`,
    ];
    for (const s of idx.segs) {
      L.push(`${s.name.padEnd(10)} ${String(s.rows).padStart(8)} ${`0x${s.lo.toString(16)}-0x${s.hi.toString(16)}`.padEnd(24)} ${s.firstLine}-${s.lastLine}`);
    }
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.action === 'search' || args.action === 'span') {
    const r = search(file, { match: args.match, regex: args.regex, limit: args.limit });
    if (args.action === 'span') {
      const bySeg = new Map();
      for (const h of r.hits) {
        if (!h.seg) continue;
        const cur = bySeg.get(h.seg);
        if (!cur) bySeg.set(h.seg, { lo: h.line, hi: h.line, loEa: h.ea, hiEa: h.ea });
        else {
          cur.hi = h.line;
          cur.hiEa = h.ea;
        }
      }
      const L = [`命中 ${r.total} 处${r.truncated ? `（只看了前 ${r.shown} 处 ⇒ 区间可能偏窄，调 --limit）` : ''}`, ''];
      for (const [seg, w] of [...bySeg].sort()) L.push(`${seg}  行 ${w.lo}-${w.hi}  EA 0x${w.loEa.toString(16)}-0x${w.hiEa.toString(16)}`);
      process.stdout.write(`${L.join('\n')}\n`);
      return 0;
    }
    if (args.json) {
      process.stdout.write(`${JSON.stringify(r, null, 2)}\n`);
      return 0;
    }
    const L = [`命中 ${r.total} 处${r.truncated ? `　★ 只显示前 ${r.shown} 处（--limit 可调）` : ''}`, ''];
    for (const h of r.hits) L.push(`${String(h.line).padStart(7)}  ${h.seg ?? '?'}:${h.ea === null ? '?' : h.ea.toString(16).padStart(8, '0')}  ${h.text.trim().slice(0, 140)}`);
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.action === 'at') {
    if (!args.ea) throw new Error('--at 需要 --ea <EA>（例：--ea 0x401000）');
    const ctx = args.fn
      ? enclosingFunction(file, idx, args.ea)
      : contextAt(file, idx, args.ea, { lines: args.lines, seg: args.seg });
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ ...ctx, file: path.relative(REPO_ROOT, file) }, null, 2)}\n`);
      return 0;
    }
    const head = [
      `语料        ${path.relative(REPO_ROOT, file)}（sha256 ${idx.sha256.slice(0, 16)}…）`,
      `EA          0x${parseEa(args.ea).toString(16)}${ctx.seg ? `　段 ${ctx.seg}` : ''}`,
      `行窗口      ${ctx.fromLine}-${ctx.toLine}（${ctx.lines.length} 行）`,
    ];
    if (ctx.truncated?.head || ctx.truncated?.tail) {
      head.push(`★ 截断      ${ctx.truncated.head ? '窗口起点不在文件头 ' : ''}${ctx.truncated.tail ? '窗口终点不在文件尾（可能没取全）' : ''}`.trim());
    }
    if (ctx.hint) head.push(`★ 提示      ${ctx.hint}`);
    process.stdout.write(`${head.join('\n')}\n\n${ctx.lines.map((l, i) => `${String(ctx.fromLine + i).padStart(7)}  ${l}`).join('\n')}\n`);
    return 0;
  }

  if (args.action === 'cases') {
    if (!args.ea) throw new Error('--cases 需要 --ea <EA>（函数内任一地址）');
    const all = fs.readFileSync(file, 'utf8').split('\n');
    const tables = switchTables(file, idx, args.ea, { fileLines: all });
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ ea: parseEa(args.ea), tables }, null, 2)}\n`);
      return 0;
    }
    const span = spanOfFunction(file, idx, args.ea);
    const L = [
      `语料        ${path.relative(REPO_ROOT, file)}`,
      `函数        行 ${span.fromLine}-${span.toLine}（${span.lines[0]?.trim().slice(0, 60)}）`,
      `跳转表      ${tables.length} 个`,
    ];
    for (const t of tables) {
      L.push('');
      L.push(`jpt_${t.jptAt.toString(16).toUpperCase()}　表在 ${t.tableSeg} 行 ${t.tableLine}　→ **${t.cases} 个 case**`);
      for (const g of t.targets) L.push(`  [${String(g.i).padStart(2)}] → ${g.loc}`);
    }
    L.push('');
    L.push('★ 表里的**下标**只是序号：真正的 case 值要看跳转点前后的 `cmp`/`sub` 归零式。');
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  if (args.action === 'pseudo') {
    if (!args.ea && !args.sym) throw new Error('--pseudo 需要 --ea <EA> 或 --sym sub_XXXXXX');
    const r = pseudoOfFunction(file, idx, { ea: args.ea, sym: args.sym, lines: args.lines });
    if (args.json) {
      process.stdout.write(`${JSON.stringify({ ...r, c: r.c ? { ...r.c, body: undefined } : null }, null, 2)}\n`);
      return 0;
    }
    const L = [
      `函数        ${r.sym}（起点 0x${r.fnEa.toString(16)}）${r.isFunctionStart === false ? '　★ 你给的 EA 不是函数起点' : ''}`,
      `lst         行 ${r.lst.fromLine}-${r.lst.toLine}（${r.lst.lines} 行）`,
    ];
    if (r.note) L.push(`★ 提示      ${r.note}`);
    if (!r.c) {
      L.push('');
      L.push('（没有 C 体可给 —— 按上面的 .lst 行区间读，或 `--at --ea 0x… --fn`）');
      process.stdout.write(`${L.join('\n')}\n`);
      return 0;
    }
    L.push(`C 体        ${path.relative(REPO_ROOT, r.c.file)} 行 ${r.c.fromLine}-${r.c.toLine}（共 ${r.c.bodyLines} 行）`);
    if (r.c.truncated) L.push(`★ 截断      只给了前 ${r.c.body.length} 行（--lines 调大；别把"没给"当成"没有"）`);
    L.push('');
    L.push(r.c.body.map((l, i) => `${String(r.c.fromLine + i).padStart(7)}  ${l}`).join('\n'));
    L.push('');
    L.push('★ C 是 Hex-Rays 的**改写**（类型 / 变量名 / 结构都是它的推断）⇒ 结论必须回 .lst 核验，锚记函数起点 EA。');
    process.stdout.write(`${L.join('\n')}\n`);
    return 0;
  }

  process.stderr.write(`未知动作：${args.action}\n${HELP}`);
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
