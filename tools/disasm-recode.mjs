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
 * 用法：
 *   node tools/disasm-recode.mjs --verify [--staging <dir>] [--json]     # 只断言，不落盘（缺省）
 *   node tools/disasm-recode.mjs --build  [--out-dir <dir>] [--zip <path>] # 转写落盘 + 确定性 zip（M1）
 */
import { deflateRawSync } from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const DEFAULT_STAGING = path.join(REPO_ROOT, '.staging');
export const DEFAULT_OUT_DIR = path.join(REPO_ROOT, 'corpus', 'disasm', 'files');
export const DEFAULT_ZIP = path.join(REPO_ROOT, 'corpus', 'disasm', 'disasm-20260930.zip');

/** 入库的 4 个文件（顺序即 zip 内顺序，固定 ⇒ 打包确定） */
export const FILES = [
  { name: 'AGE.EXE__dumped.sectfix.EXE.c', derivedFrom: 'binary/age-sectfix' },
  { name: 'AGE.EXE__dumped.sectfix.EXE.lst', derivedFrom: 'binary/age-sectfix' },
  { name: 'AGERC.DLL.c', derivedFrom: 'binary/agerc-debug-unpacked' },
  { name: 'AGERC.DLL.lst', derivedFrom: 'binary/agerc-debug-unpacked' },
];

const FILE_NAME_MARKER = '; File Name   :';
const WIDE_MARKERS = ['(LPWSTR)', 'dwTypeData', 'text "UTF-16LE"', 'L"'];

// ───────────────────────────────────────────────── CP932 / GBK 解码原语

const CP932 = new TextDecoder('shift_jis', { fatal: true });
const GBK = new TextDecoder('gbk', { fatal: true });

/**
 * ★ Windows CP932 与 WHATWG `shift_jis` 的**全部**差异（实测对照 Python 的 cp932 表得到，共 8 个单字节）：
 *   · `0x1A/0x1C/0x7F`：WHATWG 会互相重映射（0x7F→U+001A！）—— 必须按 CP932 原样；
 *   · `0x80/0xA0/0xFD/0xFE/0xFF`：WHATWG 直接报错，CP932 有定义（U+0080 / U+F8F0..F8F3）。
 * 双字节部分两种实现**逐字一致**（9604 个已定义 pair 全比过，0 差异）⇒ 双字节交给 WHATWG 表。
 */
const CP932_SINGLE = new Map([
  [0x1a, 0x001a],
  [0x1c, 0x001c],
  [0x7f, 0x007f],
  [0x80, 0x0080],
  [0xa0, 0xf8f0],
  [0xfd, 0xf8f1],
  [0xfe, 0xf8f2],
  [0xff, 0xf8f3],
]);

const isLead = (b) => (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
const isTrail = (b) => (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);

/** 单字节 CP932 解码（含上述差异修正）；不可解 ⇒ null */
function cp932Single(b) {
  if (CP932_SINGLE.has(b)) return String.fromCodePoint(CP932_SINGLE.get(b));
  if (b < 0x80 || (b >= 0xa1 && b <= 0xdf)) return CP932.decode(Buffer.from([b]));
  return null;
}

/** 双字节 CP932 解码；未定义 ⇒ null（如 `86 80` / `EC BD` 这两个在 CP932 里没有映射） */
function cp932Pair(b0, b1) {
  try {
    return CP932.decode(Buffer.from([b0, b1]));
  } catch {
    return null;
  }
}

/** 单条 CP932 字节串（规则 3）：按字节结构走，解不出的单字节保留原码位 */
export function decodeCp932Run(buf, stats = {}) {
  const out = [];
  let i = 0;
  while (i < buf.length) {
    const c = buf[i];
    if (c < 0x80) {
      out.push(String.fromCharCode(c));
      i += 1;
      continue;
    }
    // IDA 的 GBK 箭头字形 A1 F4..FE（CP932 下这两字节不成字）
    if (c === 0xa1 && i + 1 < buf.length && buf[i + 1] >= 0xf4 && buf[i + 1] <= 0xfe) {
      try {
        out.push(GBK.decode(buf.subarray(i, i + 2)));
        stats.gbkArrows = (stats.gbkArrows ?? 0) + 1;
        i += 2;
        continue;
      } catch {
        /* 落到下面 */
      }
    }
    // ★ 半角假名 A1..DF 是单字节，永远不是前导字节（旧仓的 bug 就在这里）
    if (c >= 0xa1 && c <= 0xdf) {
      const ch = cp932Single(c);
      if (ch !== null) {
        out.push(ch);
        i += 1;
        continue;
      }
    }
    if (isLead(c) && i + 1 < buf.length && isTrail(buf[i + 1])) {
      const pair = cp932Pair(c, buf[i + 1]);
      if (pair !== null) {
        out.push(pair);
        i += 2;
        continue;
      }
    }
    // 单字节保留原码位（IDA 的 `; '\x80'` 字节字面量 ⇒ U+0080）
    out.push(String.fromCharCode(c));
    stats.fallback = (stats.fallback ?? 0) + 1;
    i += 1;
  }
  return out.join('');
}

/** GBK 整行（规则 2）；整行解不出就逐字节退 */
export function decodeGbkLine(buf, stats = {}) {
  try {
    return GBK.decode(buf);
  } catch {
    return decodeCp932Run(buf, stats); // 退化成逐字节（与旧脚本同口径）
  }
}

// ───────────────────────────────────────────────── 反解（保真的证据）

/** 反向表：文本 → 原字节。用"同一套解码原语"枚举所有可解序列构造，避免依赖任何编码器。 */
function buildReverse(decodePair, decodeSingle, leads) {
  const map = new Map();
  const dups = new Map();
  const put = (text, bytes) => {
    if (text.length === 0) return;
    if (map.has(text)) {
      if (Buffer.compare(map.get(text), bytes) !== 0) {
        dups.set(text, (dups.get(text) ?? 1) + 1);
      }
      return; // 首个（字典序最小）胜出
    }
    map.set(text, bytes);
  };
  for (let b = 0; b < 0x100; b += 1) {
    const ch = decodeSingle(b);
    if (ch !== null) put(ch, Buffer.from([b]));
  }
  for (const b0 of leads) {
    for (let b1 = 0x40; b1 <= 0xfc; b1 += 1) {
      if (b1 === 0x7f) continue;
      if (!isTrail(b1)) continue;
      const ch = decodePair(b0, b1);
      if (ch !== null) put(ch, Buffer.from([b0, b1]));
    }
  }
  return { map, dups };
}

const LEAD_BYTES = [
  ...Array.from({ length: 0x9f - 0x81 + 1 }, (_, i) => 0x81 + i),
  ...Array.from({ length: 0xfc - 0xe0 + 1 }, (_, i) => 0xe0 + i),
];

let REV_CP932 = null;
let REV_GBK = null;

function revCp932() {
  if (!REV_CP932) {
    REV_CP932 = buildReverse(cp932Pair, (b) => cp932Single(b), LEAD_BYTES);
  }
  return REV_CP932;
}

function revGbk() {
  if (!REV_GBK) {
    REV_GBK = buildReverse(
      (b0, b1) => {
        try {
          return GBK.decode(Buffer.from([b0, b1]));
        } catch {
          return null;
        }
      },
      (b) => {
        try {
          return GBK.decode(Buffer.from([b]));
        } catch {
          return null;
        }
      },
      [...Array.from({ length: 0xfe - 0x81 + 1 }, (_, i) => 0x81 + i)],
    );
  }
  return REV_GBK;
}

/**
 * 反解：把转写后的文本还原成源字节。
 * 落点规则与解码一一对应：能查到表就用表；查不到且码位 < 0x100 ⇒ 就是那个"保留原码位"的字节。
 * @returns {Buffer|null} null = 有字符无法还原（说明解码不可逆 ⇒ 断言该红）
 */
export function encodeBack(text, { gbk = false } = {}) {
  const rev = gbk ? revGbk() : revCp932();
  // 预分配（CP932/GBK 每字符最多 2 字节；码位 < 0x100 的"保留原码位"字符是 1 字节）
  const out = Buffer.allocUnsafe(text.length * 2 + 8);
  let n = 0;
  for (const ch of text) {
    const hit = rev.map.get(ch);
    if (hit !== undefined) {
      for (let k = 0; k < hit.length; k += 1) out[n++] = hit[k];
      continue;
    }
    const cp = ch.codePointAt(0);
    if (cp < 0x100) {
      out[n++] = cp;
      continue;
    }
    return null;
  }
  return out.subarray(0, n);
}

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

export function verifyAll(stagingDir = DEFAULT_STAGING) {
  const results = FILES.map((f) => {
    const abs = path.join(stagingDir, f.name);
    if (!fs.existsSync(abs)) {
      return { name: f.name, ok: false, checks: [{ label: '源文件存在', ok: false, detail: `找不到 → ${abs}` }], stats: null };
    }
    return checkFile(abs);
  });
  return { results, ok: results.every((r) => r.ok) };
}

// ───────────────────────────────────────────────── zip（无依赖、确定性）

const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

export function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i += 1) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const ZIP_DOS_DATE = ((2026 - 1980) << 9) | (9 << 5) | 30; // 2026-09-30
const ZIP_DOS_TIME = 0;

/** 最小 ZIP 写入器：deflate + 固定时间戳 + 无 extra ⇒ 同输入同字节 */
export function buildZip(entries) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const e of entries) {
    const nameBuf = Buffer.from(e.name, 'utf8');
    const crc = crc32(e.data);
    const comp = deflateRawSync(e.data, { level: 9 });
    const useDeflate = comp.length < e.data.length;
    const payload = useDeflate ? comp : e.data;
    const method = useDeflate ? 8 : 0;

    const local = Buffer.alloc(30 + nameBuf.length);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(method, 8);
    local.writeUInt16LE(ZIP_DOS_TIME, 10);
    local.writeUInt16LE(ZIP_DOS_DATE, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(payload.length, 18);
    local.writeUInt32LE(e.data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28);
    nameBuf.copy(local, 30);
    locals.push(local, payload);

    const central = Buffer.alloc(46 + nameBuf.length);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);
    central.writeUInt16LE(20, 6);
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(method, 10);
    central.writeUInt16LE(ZIP_DOS_TIME, 12);
    central.writeUInt16LE(ZIP_DOS_DATE, 14);
    central.writeUInt32LE(crc, 16);
    central.writeUInt32LE(payload.length, 20);
    central.writeUInt32LE(e.data.length, 24);
    central.writeUInt16LE(nameBuf.length, 28);
    central.writeUInt16LE(0, 30);
    central.writeUInt16LE(0, 32);
    central.writeUInt16LE(0, 34);
    central.writeUInt16LE(0, 36);
    central.writeUInt32LE(0o644 << 16, 38);
    central.writeUInt32LE(offset, 42);
    nameBuf.copy(central, 46);
    centrals.push(central);

    offset += local.length + payload.length;
  }
  const centralBuf = Buffer.concat(centrals);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(entries.length, 8);
  eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);
  eocd.writeUInt16LE(0, 20);
  return Buffer.concat([...locals, centralBuf, eocd]);
}

// ───────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: 'verify', staging: DEFAULT_STAGING, outDir: DEFAULT_OUT_DIR, zip: DEFAULT_ZIP, json: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--verify') out.action = 'verify';
    else if (a === '--build') out.action = 'build';
    else if (a === '--staging') out.staging = path.resolve(argv[++i]);
    else if (a === '--out-dir') out.outDir = path.resolve(argv[++i]);
    else if (a === '--zip') out.zip = path.resolve(argv[++i]);
    else if (a === '--json') out.json = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
  }
  return out;
}

function printVerify(report, json) {
  if (json) {
    process.stdout.write(
      `${JSON.stringify(
        {
          ok: report.ok,
          results: report.results.map((r) => ({
            name: r.name,
            ok: r.ok,
            checks: r.checks,
            stats: r.stats ? { ...r.stats, literals: r.stats.literals.length } : null,
          })),
        },
        null,
        2,
      )}\n`,
    );
    return;
  }
  for (const r of report.results) {
    process.stdout.write(`${r.ok ? '[ok  ]' : '[FAIL]'} ${r.name}\n`);
    for (const c of r.checks) {
      process.stdout.write(`        ${c.ok ? 'ok  ' : 'FAIL'} ${c.label}${c.detail ? `  (${c.detail})` : ''}\n`);
    }
    if (r.stats) {
      const s = r.stats;
      process.stdout.write(
        `        · 统计：${s.lines} 行（非 ASCII ${s.nonAsciiLines}）/ GBK 行 ${s.gbkLines} / GBK 箭头 ${s.gbkArrows} / 保留原码位 ${s.fallbackChars} / 宽串上下文串 ${s.wideRuns} / '::' ${s.colonColon} / 'this' ${s.thisWord} / 字面量 ${s.literals.length}\n`,
      );
    }
  }
  process.stdout.write(`\n${report.results.filter((r) => r.ok).length}/${report.results.length} 个文件通过全部断言\n`);
}

const HELP = `tools/disasm-recode.mjs — 反汇编语料的无损转写与保真断言（规则沿用旧仓 agerc-module.md §4）

  node tools/disasm-recode.mjs --verify [--staging <dir>] [--json]
  node tools/disasm-recode.mjs --build  [--out-dir <dir>] [--zip <path>]
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  const report = verifyAll(args.staging);
  if (args.action === 'verify') {
    printVerify(report, args.json);
    return report.ok ? 0 : 1;
  }
  if (!report.ok) {
    printVerify(report, false);
    process.stderr.write('\n保真断言未通过 ⇒ 拒绝产出任何工件（入库件必须是忠实的）。\n');
    return 1;
  }
  fs.mkdirSync(args.outDir, { recursive: true });
  const entries = [];
  for (const r of report.results) {
    const src = fs.readFileSync(path.join(args.staging, r.name));
    const { text } = transcode(src);
    const data = Buffer.from(text, 'utf8');
    fs.writeFileSync(path.join(args.outDir, r.name), data);
    entries.push({ name: r.name, data });
    process.stdout.write(`写出 ${path.relative(REPO_ROOT, path.join(args.outDir, r.name))}  (${data.length} B)\n`);
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
