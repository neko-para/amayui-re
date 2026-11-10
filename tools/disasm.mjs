#!/usr/bin/env node
/**
 * tools/disasm.mjs —— **CLI**：反汇编语料的机械提取（EA → 行号 → 有界上下文）
 *
 * 经派发器：`pnpm tools disasm-at <stats|at|span|search|index> [args…]`
 * （★ 域 id 是 **`disasm-at`**；`disasm` 是**语料保真**域，`pnpm tools disasm at` 不存在 ——
 *   需求单与本文件头原先都写成 `disasm`，实测报"域 disasm 没有动作 at"。）
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
  parseCollapsedLine,
  parseEa,
  parseIndexText,
  search,
  serializeIndex,
  spanOfFunction,
  switchTables,
  withSegRanges,
} from './lib/disasm.mjs';
import { sha256File } from './lib/fsx.mjs';
import { parseListingLine } from './lib/lst-format.mjs';
import { DEFAULT_MANIFEST } from './lib/paths.mjs';
import { readBytesAt, readPeSections } from './lib/ledger.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');
const FILES_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
const CACHE_DIR = path.join(REPO_ROOT, '.cache', 'disasm');

const HELP = `tools/disasm.mjs —— 反汇编语料的**定位与切片**（只读语料；不许在大文件里漫游）

  node tools/disasm.mjs --stats                          # 语料盘点：段 / 行数 / EA 范围 / 单调性
  node tools/disasm.mjs --at --ea 0x401000 [--lines 60]   # ★ 取有界上下文（被截断会明说）
  node tools/disasm.mjs --at --ea 0x401000 --bytes 32     # ★ 额外给该 EA 起 32 字节 hex（折叠区间也能取）
  node tools/disasm.mjs --at --ea 0x401000 --fn           # 取它所在函数的区间
  node tools/disasm.mjs --search --match "某串" [--regex] [--limit 50]
  node tools/disasm.mjs --cases --ea 0x41BF50             # ★ 机械枚举函数里的 switch/跳转表（case 数 + 目标）
  node tools/disasm.mjs --span --match "某串"              # 只看命中位置的**行号区间**
  node tools/disasm.mjs --index [--write]                 # 建/刷新派生索引到 .cache/（缺省 dry-run）

★ **逐函数 / 逐定义**那一层（C 体 + 两侧行关联 + 调用链 / 全局变量 / 参数偏移）不在本工具：
  \`pnpm tools disasm-fn show <sym|EA>\`（\`--describe\` 看全部动作）。
★ \`--bytes <n>\` **不依赖外部工具**：EA → PE 节表 → 文件偏移 → 字节（\`lib/ledger.mjs\` 的 \`readPeSections\` / \`eaToFileOffset\` / \`readBytesAt\`）。
  二进制缺省按 \`corpus/assets.json\` 找与 \`.lst\` 同名的入库件，并核对 sha256 == \`.lst\` 头部 \`Input SHA256\`；
  非映射 / 越界 / 落在虚拟尾部 / 跨节全会**响亮报错**（不是给空）。\`--bin <path>\` 可指定二进制。
★ 锚点锚 **EA，不锚行号**：\`EA → 行号\` 由本工具现算；换一次反汇编只重建映射。
★ 索引是**派生缓存**（\`.cache/\`，可删可重建），**不是真源**。
★ 语料**只读**：本工具不改它一个字。
`;

function parseArgs(argv) {
  const out = {
    action: null,
    write: false,
    json: false,
    lines: DEFAULT_MAX_LINES,
    limit: 200,
    regex: false,
    fn: false,
    file: null,
    sym: null,
    bytes: null,
    bin: null,
  };
  const takesValue = new Set(['ea', 'match', 'lines', 'limit', 'seg', 'file', 'sym', 'bytes', 'bin']);
  const ACTIONS = ['stats', 'at', 'span', 'search', 'cases', 'index', 'describe', 'help'];
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
      out[k] = k === 'lines' || k === 'limit' || k === 'bytes' ? Number(v) : v;
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

/**
 * `.lst` 头部的 `; Input SHA256 : <hex>`（只读文件头 8 KB —— 不必扫 18 MB）。
 * ★ 它是"二进制 ↔ 语料"的**跨件锚**：IDA 对它**实际装载的那份文件**算的 sha256
 *   （`tools/test/age-sectfix-artifact.test.mjs` 用同一条锚）。取不到 ⇒ `null`，调用方必须明说。
 */
export function listingInputSha256(listingFile) {
  const fd = fs.openSync(listingFile, 'r');
  try {
    const head = Buffer.alloc(8192);
    const n = fs.readSync(fd, head, 0, head.length, 0);
    const m = /;\s*Input SHA256\s*:\s*([0-9A-Fa-f]{64})/.exec(head.subarray(0, n).toString('latin1'));
    return m ? m[1].toUpperCase() : null;
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * `--bytes` 读的二进制：`--bin` 优先；否则按 `corpus/assets.json` 找**与 `.lst` 同名**的入库件。
 *
 * ★ 不硬编码 `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`：那是清单记的**事实**，
 *   写死等于给它造第二个真源（清单改了路径，这里会静默读旧件）。
 * ★ 找不到 / 找到多个都**报错**，不猜 —— 猜错的表现是"字节看起来正常，其实来自别的二进制"。
 *
 * @returns {{path:string, source:string}} `source` = `'--bin'` 或清单条目 id
 */
export function resolveReferenceBinary(listingFile, override = null) {
  if (override) {
    const abs = path.isAbsolute(override) ? override : path.join(REPO_ROOT, override);
    if (!fs.existsSync(abs)) throw new Error(`--bin 指定的文件不存在：${abs}`);
    return { path: abs, source: '--bin' };
  }
  const base = path.basename(listingFile).replace(/\.lst$/i, '');
  if (!fs.existsSync(DEFAULT_MANIFEST)) {
    throw new Error(
      `清单不在：${path.relative(REPO_ROOT, DEFAULT_MANIFEST)} —— --bytes 靠它把 .lst 对到二进制（也可以 --bin <path>）`,
    );
  }
  const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const named = (manifest.entries ?? []).filter(
    (e) => typeof e.dest === 'string' && path.posix.basename(e.dest).toLowerCase() === base.toLowerCase(),
  );
  const onDisk = named
    .map((e) => ({ id: e.id, abs: path.join(REPO_ROOT, e.dest) }))
    .filter((h) => fs.existsSync(h.abs));
  if (onDisk.length === 0) {
    throw new Error(
      `清单里没有与 ${base} 同名、且已落盘的二进制（候选条目：${named.map((e) => e.id).join(' / ') || '无'}）` +
        `⇒ 用 --bin <path> 指定`,
    );
  }
  if (onDisk.length > 1) {
    throw new Error(
      `清单里有 ${onDisk.length} 个与 ${base} 同名的落盘件` +
        `（${onDisk.map((h) => `${h.id}=${path.relative(REPO_ROOT, h.abs)}`).join(' / ')}）⇒ 用 --bin <path> 指定，不猜`,
    );
  }
  return { path: onDisk[0].abs, source: onDisk[0].id };
}

/**
 * `--at --bytes` 的一次性装配：解析二进制 → **核对 sha256（二进制 == `.lst` 说的那一份）**
 * → PE 节表 → 文件偏移 → 字节 → 折叠行声明的长度。
 *
 * ★ sha256 不符 ⇒ **抛**：那是"语料与二进制不是同一份"（清单/语料问题），不是取字节的问题；
 *   继续读会给出**看起来对**的字节，却属于另一个版本 —— 比读不到更糟。
 *
 * @param {string} listingFile `.lst` 路径
 * @param {string|null} eaLine 该 EA 在语料里的那一行（用于读折叠声明；`--fn` 时可能为 null）
 * @param {number} ea VA
 * @param {number} n 正整数
 * @param {string|null} binOverride `--bin`
 */
export function readPeBytesBlock(listingFile, eaLine, ea, n, binOverride = null) {
  const ref = resolveReferenceBinary(listingFile, binOverride);
  const want = listingInputSha256(listingFile);
  if (!want) {
    throw new Error(`.lst 头部没有 \`; Input SHA256 :\`（${path.relative(REPO_ROOT, listingFile)}）⇒ 无法证明二进制与语料同源`);
  }
  const got = sha256File(ref.path).toUpperCase();
  if (got !== want) {
    throw new Error(
      `二进制与语料**不是同一份**：${path.relative(REPO_ROOT, ref.path)} sha256=${got} ≠ .lst 头部 Input SHA256=${want}` +
        ` ⇒ 停下（先查语料 / 清单；不在这里硬读）`,
    );
  }
  const buf = fs.readFileSync(ref.path);
  const pe = readPeSections(buf);
  const hit = readBytesAt(buf, pe, ea, n);
  return {
    ea,
    n,
    bytes: hit.bytes,
    fileOffset: hit.fileOffset,
    offsetInSection: hit.offsetInSection,
    sectionName: hit.sectionName,
    sectionIndex: hit.sectionIndex,
    binary: path.relative(REPO_ROOT, ref.path),
    source: ref.source,
    sha256: got.toLowerCase(),
    fold: eaLine ? parseCollapsedLine(eaLine) : null,
  };
}

/** `readPeBytesBlock` 的人读形态（stdout；**只在 `--bytes` 时**才被调用 ⇒ 旧输出不受影响） */
function peBytesText(b) {
  const L = [
    '',
    `PE 原始字节  0x${b.ea.toString(16)} 起 ${b.n} 字节 → 文件偏移 0x${b.fileOffset.toString(16)}` +
      `（段 ${b.sectionName}，段内 +0x${b.offsetInSection.toString(16)}；**来自 PE 原始字节（折叠区间也能取）**）`,
    `             二进制 ${b.binary}（sha256 == .lst 头部 Input SHA256 ✓${b.source === '--bin' ? '；--bin 指定' : `；清单条目 ${b.source}`}）`,
  ];
  for (let i = 0; i < b.bytes.length; i += 16) {
    const row = b.bytes.subarray(i, i + 16);
    L.push(`  ${(b.ea + i).toString(16).padStart(8, '0')}  ${[...row].map((x) => x.toString(16).toUpperCase().padStart(2, '0')).join(' ')}`);
  }
  if (b.fold && b.fold.declaredBytes !== null) {
    const d = b.fold.declaredBytes;
    const diff = b.n - d;
    const cmp =
      diff === 0
        ? '本次读的**正好是整段**'
        : diff < 0
          ? `本次 ${b.n} 字节**在区间内**（前缀）`
          : `**本次越过区间末尾 ${diff} 字节**（取到的尾部已不属于这个折叠区间）`;
    L.push(`★ 折叠行    0x${b.ea.toString(16)} 声明 ${d} 字节（0x${d.toString(16)}）⇒ ${cmp}`);
  } else if (b.fold) {
    L.push(`★ 折叠行    0x${b.ea.toString(16)} 是折叠/孤立块，但**这行没有声明字节数**（不是 0 —— 只是没写）`);
  }
  return `${L.join('\n')}\n`;
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
    if (args.bytes !== null && (!Number.isInteger(args.bytes) || args.bytes <= 0)) {
      throw new Error(`--bytes 需要正整数（拿到 ${JSON.stringify(args.bytes)}；例：--bytes 32）`);
    }
    if (args.bin !== null && args.bytes === null) {
      throw new Error('--bin 只与 --bytes 一起用（它指定的是取字节用的二进制）');
    }
    const targetEa = parseEa(args.ea);
    const ctx = args.fn
      ? enclosingFunction(file, idx, args.ea)
      : contextAt(file, idx, args.ea, { lines: args.lines, seg: args.seg });
    // ★ 折叠声明只从"**该 EA 那一行**"读：`--fn` 的窗口首行是 `proc near` 头，不是这个 EA 的行
    const firstLine = ctx.lines?.[0] ?? null;
    const firstParsed = firstLine ? parseListingLine(firstLine) : null;
    const eaLine = firstParsed && firstParsed.ea === targetEa ? firstLine : null;
    let peBlock = null;
    let peError = null;
    if (args.bytes !== null) {
      try {
        peBlock = readPeBytesBlock(file, eaLine, targetEa, args.bytes, args.bin);
      } catch (err) {
        peError = err;
      }
    }
    const failed = peError !== null;
    if (args.json) {
      const obj = { ...ctx, file: path.relative(REPO_ROOT, file) };
      if (args.bytes !== null) {
        obj.pe = peBlock
          ? {
              ea: peBlock.ea,
              n: peBlock.n,
              fileOffset: peBlock.fileOffset,
              offsetInSection: peBlock.offsetInSection,
              section: peBlock.sectionName,
              binary: peBlock.binary,
              source: peBlock.source,
              sha256: peBlock.sha256,
              hex: peBlock.bytes.toString('hex').toUpperCase(),
              fold: peBlock.fold ? { declaredBytes: peBlock.fold.declaredBytes } : null,
            }
          : { error: { code: peError.code ?? 'error', message: peError.message } };
      }
      process.stdout.write(`${JSON.stringify(obj, null, 2)}\n`);
      return failed ? 1 : 0;
    }
    const head = [
      `语料        ${path.relative(REPO_ROOT, file)}（sha256 ${idx.sha256.slice(0, 16)}…）`,
      `EA          0x${targetEa.toString(16)}${ctx.seg ? `　段 ${ctx.seg}` : ''}`,
      `行窗口      ${ctx.fromLine}-${ctx.toLine}（${ctx.lines.length} 行）`,
    ];
    if (ctx.truncated?.head || ctx.truncated?.tail) {
      head.push(`★ 截断      ${ctx.truncated.head ? '窗口起点不在文件头 ' : ''}${ctx.truncated.tail ? '窗口终点不在文件尾（可能没取全）' : ''}`.trim());
    }
    if (ctx.hint) head.push(`★ 提示      ${ctx.hint}`);
    process.stdout.write(`${head.join('\n')}\n\n${ctx.lines.map((l, i) => `${String(ctx.fromLine + i).padStart(7)}  ${l}`).join('\n')}\n`);
    if (peBlock) process.stdout.write(peBytesText(peBlock));
    if (peError) process.stderr.write(`✗ PE 取字节失败（响亮）：${peError.message}\n`);
    return failed ? 1 : 0;
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
