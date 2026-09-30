#!/usr/bin/env node
/**
 * tools/disasm-recode.mjs — 反汇编语料的**无损转写 + 保真断言**（§1.8.1）
 *
 * 规则不是这里发明的：它沿用旧仓**已经定案并实测过**的口径 ——
 *   · `docs-new/03-engine/agerc-module.md` §4（转写规则 + 半角假名前导字节 bug 的修正）
 *   · `.tmp/convert_to_utf8.py`（引擎本体那份的逐行字节解码方法）
 *   · `.tmp/convert_agerc_utf8.py`（AGERC 那份：加 UTF-16LE 宽串规则 + 上述修正）
 * 本文件是它们的 **Node 重写**（本仓口径：除 emulator 外一律 `.mjs`，且不依赖 Python/iconv）。
 *
 * ★ 铁的纪律（不许做的事）：**不做任何符号改写** ——
 *   ❌ 禁止 `::` → `__`、❌ 禁止 `this` → `_this`、❌ 禁止折叠空格或重命名符号。
 *   （旧仓 `sanitize_symbols.py` 就是这么把语料搞坏的：同一份语料 raw 的 `::` 4754 / `this` 36753
 *     → `_utf8` 只剩 3 / 1 ⇒ 基于字符串层的结论全不可信。见 docs/00-origin/decisions.md §4。）
 *
 * 逐行规则：
 *   1. 纯 ASCII 行 ⇒ 原样。
 *   2. 含 `; File Name   :` 的行 ⇒ **整行按 GBK**（IDA 用运行机器的 ANSI 码页写路径，`CC EC BD 59` = `天結`）。
 *   3. 其余 ⇒ 按 **CP932 的字节结构**逐字节解：
 *        · `0xA1..0xDF` 是**单字节**半角假名，**永远不是前导字节**（旧仓的 bug 就在这里：
 *          盲试两字节会把 `B1 8F` 配成汉字，`ｱ` 变成 Latin-1 `±`）；
 *        · `A1 F4..FE` 是 IDA 的 GBK 箭头字形（◆□■△←↑↓〓）；
 *        · CP932 解不出的**单字节保留原码位**（IDA 的 `; '\x80'` 字节字面量 ⇒ U+0080）。
 *   4. **本批不启用 UTF-16LE 宽串规则** —— 见下方"为什么"。
 *
 * ★ 为什么不启用宽串规则（与旧仓的差异，实测）：
 *   旧仓那两份 `_utf8.*` 的 UTF-16LE 宽串是**汉化版**的产物（`显示消息窗口(`）。
 *   本轮的 4 个文件来自**原版**二进制，实测宽串上下文里**一个真宽串都没有**：
 *   `dwTypeData` 下的高字节串按 UTF-16LE 解是乱码（`꿒낾…`），按 CP932 解才是正文（`ﾒｯｾｰｼﾞｳｲﾝﾄﾞｳを表示する(&O)`）。
 *   ⇒ 断言 #8 把这个差异**机械化**：宽串上下文里若出现「CP932 可解」且「UTF-16LE 也像正经文本」的歧义串，
 *     断言直接红，逼人裁决（而不是静默按某一边解）。
 *
 * 用法（经派发器：`pnpm tools disasm <verify|build|restore|describe> [args…]`）：
 *   node tools/disasm-recode.mjs --verify [--staging <dir>] [--json]     # 只断言，不落盘（缺省）
 *   node tools/disasm-recode.mjs --build  [--out-dir <dir>] [--zip <path>] # 转写落盘 + 确定性 zip
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { decodeCp932Run, decodeGbkLine, encodeBack, revCp932 } from './lib/cp932.mjs';
import { sha256buf } from './lib/fsx.mjs';
import { DEFAULT_OUT_DIR, DEFAULT_STAGING, DEFAULT_ZIP, REPO_ROOT } from './lib/paths.mjs';
import { buildZip, readZip } from './lib/zip.mjs';

export { DEFAULT_OUT_DIR, DEFAULT_STAGING, DEFAULT_ZIP, REPO_ROOT };

/** 入库的 4 个文件（顺序即 zip 内顺序，固定 ⇒ 打包确定） */
export const FILES = [
  { name: 'AGE.EXE__dumped.sectfix.EXE.c', derivedFrom: 'binary/age-sectfix' },
  { name: 'AGE.EXE__dumped.sectfix.EXE.lst', derivedFrom: 'binary/age-sectfix' },
  { name: 'AGERC.DLL.c', derivedFrom: 'binary/agerc-debug-unpacked' },
  { name: 'AGERC.DLL.lst', derivedFrom: 'binary/agerc-debug-unpacked' },
];

/** 工具层的自我声明：**我动哪片数据、有哪些操作**（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'disasm',
  title: '反汇编语料（4 个 UTF-8 文件，一个 LFS zip）',
  data: ['corpus/disasm/disasm-*.zip', 'corpus/disasm/files/（gitignore 的解压读区）', '.staging/（投递原件；中转区，可随时丢）'],
  access: 'r：verify ／ w：build（产出 zip + 解压区）、restore（只写 .staging/）',
  tool: 'tools/disasm-recode.mjs',
};

export const OPERATIONS = [
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '保真断言（原件在就按原件；不在就由 zip 反解 + 清单 sha256 自证）' },
  { name: 'build', argv: ['--build'], mutates: true, summary: '转写落盘 + 打确定性 zip（需要 .staging/ 里的原件）' },
  { name: 'restore', argv: ['--restore'], mutates: true, summary: '由 zip 反解回投递原件（默认写回 .staging/）' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：数据 / 断言目录 / 操作' },
];

/** 断言目录（人读的静态列表；运行时逐文件逐条打印的细节不在此） */
export const ASSERTIONS = [
  [1, '源文件是干净的 CRLF（LF 数 == CRLF 数，无孤立 CR）'],
  [2, '行数不变'],
  [3, 'LF 数不变'],
  [4, '输出是 LF-only（无任何 CR）'],
  [5, '★ 逐行反解回字节 == 源字节（最强的一条：没有字符被丢、被换、被合并）'],
  [6, '纯 ASCII 行原样'],
  [7, '输出无 U+FFFD'],
  [8, '编码判别器：CP932 产物含假名、整文件按 GBK 解不含假名'],
  [9, '★ 宽串守卫：宽串上下文里的高歧义串必须红，不许静默选边'],
];

/** 自描述（`--describe`）：数据 / 断言目录 / 操作 / 写入口 */
export function describe() {
  return {
    domain: DOMAIN,
    assertions: ASSERTIONS.map(([id, text]) => ({ id, text, enforcedBy: '本工具的 `--verify`（也由 `pnpm tools corpus validate` 的 #7 触发）' })),
    operations: OPERATIONS,
    writePath: '入库件只由 `build` 产出；`verify` 只读；`restore` 只写 `.staging/`（中转区，不是来源）。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.domain.id} —— ${d.domain.title}（自描述）`);
  L.push('');
  L.push('## 我动哪片数据');
  for (const x of d.domain.data) L.push(`* ${x}`);
  L.push(`* 读写：${d.domain.access}`);
  L.push(`* 工具：\`${d.domain.tool}\``);
  L.push('');
  L.push('## 断言目录');
  for (const a of d.assertions) L.push(`${a.id}. ${a.text}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools ${d.domain.id} ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}

const FILE_NAME_MARKER = '; File Name   :';
const WIDE_MARKERS = ['(LPWSTR)', 'dwTypeData', 'text "UTF-16LE"', 'L"'];

// ───────────────────────────────────────────────── CP932 / GBK 编解码（纯工具在 lib/cp932.mjs）
// ★ 纯工具在 lib/cp932.mjs（不认识任何语料规则）；本文件只决定**哪一行该怎么解**。
// ───────────────────────────────────────────────── 逐行转写

/**
 * 线性扫描引号串（**不用正则**：`(?:(?!\1).)*` 这类回引号模式在 28 MB 清单上会把
 * V8 的正则回溯栈打爆 —— 实测 `RangeError: Maximum call stack size exceeded`）。
 */
export function quoteRuns(s) {
  const out = [];
  let i = 0;
  while (i < s.length) {
    const q = s[i];
    if (q !== '"' && q !== "'") {
      i += 1;
      continue;
    }
    const start = i + 1;
    let j = start;
    while (j < s.length) {
      if (q === '"' && s[j] === '\\') {
        j += 2;
        continue;
      }
      if (s[j] === q) break;
      j += 1;
    }
    if (j >= s.length) break; // 未闭合 ⇒ 丢弃（与旧行为一致）
    out.push(s.slice(start, j));
    i = j + 1;
  }
  return out;
}

export function transcodeLine(body, stats = {}) {
  if (!body.some((b) => b >= 0x80)) return { text: body.toString('ascii'), mode: 'ascii' };
  const asLatin = body.toString('latin1');
  if (asLatin.includes(FILE_NAME_MARKER)) {
    stats.gbkLines = (stats.gbkLines ?? 0) + 1;
    return { text: decodeGbkLine(body, stats), mode: 'gbk-line' };
  }
  return { text: decodeCp932Run(body, stats), mode: 'cp932' };
}

export function transcode(buf) {
  const stats = { fallback: 0, gbkArrows: 0, gbkLines: 0 };
  const rawLines = buf.toString('latin1').split('\n');
  const outLines = [];
  const bodies = [];
  for (const ln of rawLines) {
    const body = ln.endsWith('\r') ? Buffer.from(ln.slice(0, -1), 'latin1') : Buffer.from(ln, 'latin1');
    bodies.push(body);
    outLines.push(transcodeLine(body, stats).text);
  }
  return { text: outLines.join('\n'), bodies, outLines, rawLines, stats };
}

// ───────────────────────────────────────────────── 断言

const KANA = (t) => (t.match(/[\u3040-\u30ff\uff66-\uff9f]/g) ?? []).length;
const FFFD = (t) => (t.match(/\uFFFD/g) ?? []).length;
const COLON2 = (t) => (t.match(/::/g) ?? []).length;
const THIS = (t) => (t.match(/\bthis\b/g) ?? []).length;

function literalMultiset(text) {
  return quoteRuns(text).sort();
}

/** 宽串歧义守卫（断言 #8）：宽串上下文里的高字节引号串，两边都"说得通" ⇒ 歧义 */
function wideAmbiguity(bodies) {
  const found = [];
  for (const body of bodies) {
    if (!body.some((b) => b >= 0x80)) continue;
    const line = body.toString('latin1');
    const marker = WIDE_MARKERS.find((m) => line.includes(m));
    if (!marker) continue;
    for (const inner of quoteRuns(line)) {
      const run = Buffer.from(inner, 'latin1');
      if (!run.some((b) => b >= 0x80)) continue;
      const asCp932 = decodeCp932Run(run);
      let asWide = null;
      try {
        const even = run.length % 2 ? Buffer.concat([run, Buffer.from([0])]) : run;
        asWide = new TextDecoder('utf-16le', { fatal: true }).decode(even);
      } catch {
        asWide = null;
      }
      const cp932Ok = FFFD(asCp932) === 0;
      const wideOk =
        asWide !== null &&
        asWide.length > 0 &&
        !/[\uD800-\uDFFF]/.test(asWide) &&
        [...asWide].every((c) => c.charCodeAt(0) >= 0x20 && c.charCodeAt(0) < 0x7f ? true : /[\u4e00-\u9fff\u3000-\u30ff\uff01-\uff5e]/.test(c)) &&
        /[\u4e00-\u9fff]/.test(asWide);
      found.push({ marker, bytes: run, asCp932, asWide, cp932Ok, wideOk, ambiguous: cp932Ok && wideOk });
    }
  }
  return found;
}

/** 对单个文件跑全部断言 */
export function checkFile(srcAbs) {
  const buf = fs.readFileSync(srcAbs);
  const { text, bodies, outLines, stats } = transcode(buf);
  const checks = [];
  const push = (label, ok, detail = '') => checks.push({ label, ok, detail });

  const srcLf = (buf.toString('latin1').match(/\n/g) ?? []).length;
  const srcCrlf = (buf.toString('latin1').match(/\r\n/g) ?? []).length;

  push('源文件是干净的 CRLF（LF 数 == CRLF 数，无孤立 CR）', srcLf === srcCrlf && srcCrlf > 0, `LF=${srcLf} CRLF=${srcCrlf}`);
  push('行数不变', outLines.length === bodies.length, `${outLines.length} vs ${bodies.length}`);
  push('LF 数不变', (text.match(/\n/g) ?? []).length === srcLf, `${(text.match(/\n/g) ?? []).length} vs ${srcLf}`);
  push('输出是 LF-only（无任何 CR）', !text.includes('\r'));

  // ★ 主断言：逐行反解回**逐字节相同**
  let badRt = 0;
  let firstBad = '';
  for (let i = 0; i < bodies.length; i += 1) {
    const gbkLine = bodies[i].toString('latin1').includes(FILE_NAME_MARKER) && bodies[i].some((b) => b >= 0x80);
    const back = encodeBack(outLines[i], { gbk: gbkLine });
    if (back === null || Buffer.compare(back, bodies[i]) !== 0) {
      badRt += 1;
      if (!firstBad) firstBad = `L${i + 1}`;
    }
  }
  push(`逐行反解回字节 == 源字节（${bodies.length} 行；含 GBK 行用 GBK 表反解）`, badRt === 0, badRt ? `${badRt} 行不可逆，首例 ${firstBad}` : '全部逐字节相同');

  // ASCII 行逐字节等价（上面已覆盖，单列一条便于一眼看到）
  const asciiLines = bodies.filter((b) => !b.some((x) => x >= 0x80)).length;
  const asciiOk = bodies.every((b, i) => b.some((x) => x >= 0x80) || outLines[i] === b.toString('ascii'));
  push(`纯 ASCII 行原样（${asciiLines} 行）`, asciiOk);

  push('输出无 U+FFFD', FFFD(text) === 0, `U+FFFD=${FFFD(text)}`);

  // 编码判别器（旧仓口径：假名计数才是判别器）
  let gbkKana = -1;
  try {
    gbkKana = KANA(new TextDecoder('gbk').decode(buf));
  } catch {
    gbkKana = -1;
  }
  const cp932Kana = KANA(text);
  push('编码判别器：CP932 产物含假名，而整文件按 GBK 解不含假名', cp932Kana > 0 && gbkKana === 0, `cp932 假名=${cp932Kana} / gbk 假名=${gbkKana}`);

  // 宽串歧义守卫
  const wide = wideAmbiguity(bodies);
  const amb = wide.filter((w) => w.ambiguous);
  push(
    `宽串守卫：宽串上下文的高字节串不得两边都说得通（本轮共 ${wide.length} 处，全部 CP932-only）`,
    amb.length === 0,
    amb.length ? `${amb.length} 处歧义：${amb.map((a) => `${a.marker} ${[...a.bytes].map((b) => b.toString(16)).join(' ')}`).join(' | ')}` : wide.map((w) => `${w.marker}:${w.asCp932.slice(0, 12)}…`).join(' '),
  );

  const nonAsciiLines = bodies.filter((b) => b.some((x) => x >= 0x80)).length;
  const rt = revCp932();
  return {
    name: path.basename(srcAbs),
    ok: checks.every((c) => c.ok),
    checks,
    stats: {
      bytes: buf.length,
      lines: bodies.length,
      crlf: srcCrlf,
      nonAsciiLines,
      asciiLines,
      gbkLines: stats.gbkLines ?? 0,
      gbkArrows: stats.gbkArrows ?? 0,
      fallbackChars: stats.fallback ?? 0,
      wideRuns: wide.length,
      colonColon: COLON2(text),
      thisWord: THIS(text),
      literals: literalMultiset(text),
      reverseDupChars: rt.dups.size,
    },
  };
}

export function verifyAll(stagingDir = DEFAULT_STAGING, { zip = DEFAULT_ZIP } = {}) {
  // ── 模式 A：投递原件还在 `.staging/` ⇒ 拿**原件**当基准跑全部断言，
  //           并额外用清单里记录的 sha256 给"原件本身"上锚（否则只能证明"转写无损"，
  //           证明不了"这份原件还是当初那一份"）。 ──
  if (FILES.every((f) => fs.existsSync(path.join(stagingDir, f.name)))) {
    const expected = expectedOriginHashes();
    const results = FILES.map((f) => {
      const abs = path.join(stagingDir, f.name);
      const r = checkFile(abs);
      const act = sha256buf(fs.readFileSync(abs));
      const exp = expected.get(f.name);
      const ok = exp !== undefined && exp === act;
      r.checks.push({
        label: '投递原件 sha256 == 清单里记录的原件（真实性锚）',
        ok,
        detail: exp === undefined ? '清单里没有该件的 sha256' : `${act.slice(0, 12)}… vs 清单 ${exp.slice(0, 12)}…`,
      });
      r.ok = r.ok && ok;
      return r;
    });
    return { mode: 'staging', stagingDir, results, ok: results.every((r) => r.ok) };
  }

  // ── 模式 B：原件已不在（`.staging/` 是用完即弃的中转区，fresh clone 本来就没有）
  //           ⇒ 由 zip **反解回原件**，并用清单里记录的**原件 sha256** 自证。
  //           这不是"放宽"：反解是逐字节的（断言 #5 的同一个原语），比对的是外部锚（清单里的 sha256）。
  if (!fs.existsSync(zip)) {
    const results = FILES.map((f) => ({
      name: f.name,
      ok: false,
      checks: [
        {
          label: '投递原件或 zip 至少有一个可用',
          ok: false,
          detail: `既没有 ${stagingDir} 下的原件，也没有 ${zip} ⇒ 无法证明任何保真性质`,
        },
      ],
      stats: null,
    }));
    return { mode: 'missing', results, ok: false };
  }

  const expected = expectedOriginHashes();
  const restored = restoreOriginals(zip);
  const results = restored.map((r) => {
    const exp = expected.get(r.name);
    const act = sha256buf(r.bytes);
    const ok = exp !== undefined && exp === act;
    return {
      name: r.name,
      ok,
      checks: [
        {
          label: '原件已不在 ⇒ 由 zip 反解，sha256 必须等于清单里记录的原件（fixtures/raw-source-* 口径）',
          ok,
          detail: exp === undefined ? '清单里没有该文件的原件 sha256' : `反解 ${act.slice(0, 12)}… / 清单 ${exp.slice(0, 12)}…`,
        },
      ],
      stats: { bytes: r.bytes.length, restoredFromZip: true, expectedSha256: exp ?? null, actualSha256: act },
    };
  });
  return { mode: 'restored', zip, results, ok: results.every((r) => r.ok) };
}

// ───────────────────────────────────────────────── 从 zip 反解回原件
// ★ zip 纯工具（读写、确定性）在 lib/zip.mjs；编解码在 lib/cp932.mjs。

/** 把 zip 里的 4 个 UTF-8 文件反解回**逐字节相同**的原件（CRLF 也一并还原） */
export function restoreOriginals(zipPath = DEFAULT_ZIP) {
  const zip = readZip(fs.readFileSync(zipPath));
  const CRLF = Buffer.from('\r\n');
  return FILES.map((f) => {
    const data = zip.get(f.name);
    if (data === undefined) throw new Error(`zip 里没有 ${f.name}`);
    const chunks = [];
    data
      .toString('utf8')
      .split('\n')
      .forEach((ln, i) => {
        if (i > 0) chunks.push(CRLF);
        const back = encodeBack(ln, { gbk: ln.includes(FILE_NAME_MARKER) });
        if (back === null) throw new Error(`${f.name}: 有字符无法反解回字节（第 ${i + 1} 行）`);
        chunks.push(back);
      });
    return { name: f.name, bytes: Buffer.concat(chunks) };
  });
}

/**
 * 清单里登记的"**投递原件** sha256"（按文件名索引）。
 * ★ 只取 `root === 'staging'` 的 origin：同名文件在别的条目里也存在
 *   （旧仓 `engine/AGERC.DLL.c` vs 本次投递的 `AGERC.DLL.c`），不加限定就会张冠李戴（实测踩过）。
 */
export function expectedOriginHashes(manifestPath = path.join(REPO_ROOT, 'corpus', 'assets.json')) {
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const map = new Map();
  for (const e of manifest.entries ?? []) {
    for (const o of e.origin ?? []) {
      if (o.root !== 'staging') continue;
      if (typeof o.sha256 === 'string') map.set(path.basename(o.path), o.sha256);
    }
  }
  return map;
}

// ───────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: 'verify', staging: DEFAULT_STAGING, outDir: null, zip: DEFAULT_ZIP, json: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--verify') out.action = 'verify';
    else if (a === '--build') out.action = 'build';
    else if (a === '--restore') out.action = 'restore';
    else if (a === '--describe') out.action = 'describe';
    else if (a === '--staging') out.staging = path.resolve(argv[++i]);
    else if (a === '--out-dir') out.outDir = path.resolve(argv[++i]);
    else if (a === '--zip') out.zip = path.resolve(argv[++i]);
    else if (a === '--json') out.json = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
  }
  return out;
}

const MODE_LINE = {
  staging: '基准：`.staging/` 里的**投递原件**（拿原件跑全部断言）',
  restored: '基准：原件已不在 ⇒ 由 **zip 反解**回原件，并用清单里记录的 sha256 自证',
  missing: '基准：**既没有原件也没有 zip** ⇒ 无法证明任何保真性质',
};

function printVerify(report, json) {
  if (json) {
    process.stdout.write(
      `${JSON.stringify(
        {
          ok: report.ok,
          mode: report.mode,
          results: report.results.map((r) => ({
            name: r.name,
            ok: r.ok,
            checks: r.checks,
            stats: r.stats ? { ...r.stats, literals: r.stats.literals?.length } : null,
          })),
        },
        null,
        2,
      )}\n`,
    );
    return;
  }
  process.stdout.write(`${MODE_LINE[report.mode] ?? ''}\n`);
  for (const r of report.results) {
    process.stdout.write(`${r.ok ? '[ok  ]' : '[FAIL]'} ${r.name}\n`);
    for (const c of r.checks) {
      process.stdout.write(`        ${c.ok ? 'ok  ' : 'FAIL'} ${c.label}${c.detail ? `  (${c.detail})` : ''}\n`);
    }
    if (r.stats && r.stats.literals) {
      const s = r.stats;
      process.stdout.write(
        `        · 统计：${s.lines} 行（非 ASCII ${s.nonAsciiLines}）/ GBK 行 ${s.gbkLines} / GBK 箭头 ${s.gbkArrows} / 保留原码位 ${s.fallbackChars} / 宽串上下文串 ${s.wideRuns} / '::' ${s.colonColon} / 'this' ${s.thisWord} / 字面量 ${s.literals.length}\n`,
      );
    } else if (r.stats && r.stats.restoredFromZip) {
      process.stdout.write(`        · 反解得到 ${r.stats.bytes} B\n`);
    }
  }
  process.stdout.write(`\n${report.results.filter((r) => r.ok).length}/${report.results.length} 个文件通过\n`);
}

const HELP = `tools/disasm-recode.mjs — 反汇编语料的无损转写与保真断言（规则沿用旧仓 agerc-module.md §4）

  node tools/disasm-recode.mjs --verify  [--staging <dir>] [--json]   # 断言（缺省）
  node tools/disasm-recode.mjs --build   [--out-dir <dir>] [--zip <path>]  # 转写落盘 + 确定性 zip
  node tools/disasm-recode.mjs --restore [--out-dir <dir>] [--zip <path>]  # 由 zip 反解回投递原件
  node tools/disasm-recode.mjs --describe [--json]                    # 自描述：数据 / 断言 / 操作

（经派发器：pnpm tools disasm <verify|build|restore|describe> [args]）
`;

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

  if (args.action === 'restore') {
    const dir = args.outDir ?? DEFAULT_STAGING;
    const expected = expectedOriginHashes();
    let bad = 0;
    for (const r of restoreOriginals(args.zip)) {
      const act = sha256buf(r.bytes);
      const exp = expected.get(r.name);
      const ok = exp !== undefined && exp === act;
      if (!ok) bad += 1;
      const dst = path.join(dir, r.name);
      fs.mkdirSync(dir, { recursive: true });
      fs.writeFileSync(dst, r.bytes);
      process.stdout.write(`${ok ? 'ok  ' : 'FAIL'} ${path.relative(REPO_ROOT, dst)}  (${r.bytes.length} B)  ${act.slice(0, 16)}…${exp ? ` vs 清单 ${exp.slice(0, 16)}…` : ' 清单里没有该件的 sha256'}\n`);
    }
    return bad === 0 ? 0 : 1;
  }

  const report = verifyAll(args.staging, { zip: args.zip });
  if (args.action === 'verify') {
    printVerify(report, args.json);
    return report.ok ? 0 : 1;
  }
  if (args.action === 'build' && report.mode !== 'staging') {
    printVerify(report, false);
    process.stderr.write(
      '\n--build 需要 `.staging/` 里的**投递原件**（不接受"由 zip 反解再打一次 zip"）。先用 `--restore` 把原件还原回 .staging/。\n',
    );
    return 1;
  }
  if (!report.ok) {
    printVerify(report, false);
    process.stderr.write('\n保真断言未通过 ⇒ 拒绝产出任何工件（入库件必须是忠实的）。\n');
    return 1;
  }
  const outDir = args.outDir ?? DEFAULT_OUT_DIR;
  fs.mkdirSync(outDir, { recursive: true });
  const entries = [];
  for (const r of report.results) {
    const src = fs.readFileSync(path.join(args.staging, r.name));
    const { text } = transcode(src);
    const data = Buffer.from(text, 'utf8');
    fs.writeFileSync(path.join(outDir, r.name), data);
    entries.push({ name: r.name, data });
    process.stdout.write(`写出 ${path.relative(REPO_ROOT, path.join(outDir, r.name))}  (${data.length} B)\n`);
  }
  const zipBuf = buildZip(entries);
  fs.mkdirSync(path.dirname(args.zip), { recursive: true });
  fs.writeFileSync(args.zip, zipBuf);
  process.stdout.write(`打包 ${path.relative(REPO_ROOT, args.zip)}  (${zipBuf.length} B, ${entries.length} 个文件)\n`);
  process.stdout.write('★ 入库前记得：把 corpus/assets.json 的 disasm/bundle 由 deferred 翻成 lfs（--set storage/dest）。\n');
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
    process.exitCode = 2;
  }
}
