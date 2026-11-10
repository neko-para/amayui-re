/** @env assets @kind gate @suite toolchain @why 折叠区间（语料里只有一行 COLLAPSED）的字节取不到/取错了段和偏移，或读到的二进制与 `.lst` 不是同一份 */
/**
 * tools/test/pe-corpus.assets.test.mjs —— **真实语料抽样**：折叠区间的字节 == 二进制，且与 `.lst` 对得上
 *
 * 守的是本单（`REQ-01M714HVX2P6VJQ1AHK6KJDFM7` 第 1 项）的判据：
 *   **EA → PE 段表 → 文件偏移 → 字节**这条纯 JS 路径，在**真实语料 + 真实二进制**上给出正确的那一段。
 *
 * 三条独立来源必须互相印证（任何一条单独绿都不够）：
 *   ① `.lst` 头部的 `; Input SHA256 :` == 二进制的 sha256（二进制 == 语料说的那一份）；
 *   ② 偏移：`.lst` 头部自带的 `Section N.` 段表算一遍，PE 节表算一遍，两者必须相等；
 *   ③ 长度：`.lst` 折叠行声明的 `[0000004C BYTES: …]` == `readBytesAt` 读到的字节数；
 *      外加字节内容（首 5 字节是 MSVC 序言 `8B FF 55 8B EC`）——偏移错一页这条就会红。
 *
 * ★ 两份输入都是**外部/派生**的：`.lst` 是解压产物（gitignore）、二进制是 LFS ⇒ 缺席就 `t.skip`（不假绿）。
 * ★ 这一条**不替代** `pe.test.mjs`：边界/错误码那些判据在合成 PE 上判（快、不依赖机器）。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { parseCollapsedLine } from '../lib/disasm.mjs';
import { eaToFileOffset, readBytesAt, readPeSections } from '../lib/ledger.mjs';
import { listingInputSha256, main as disasmMain, resolveReferenceBinary } from '../disasm.mjs';

const LST = path.join(REPO_ROOT, 'corpus', 'disasm', 'files', 'AGE.EXE__dumped.sectfix.EXE.lst');
/** 语料里已知的折叠站点：`[0000004C BYTES: COLLAPSED FUNCTION _CxxThrowException(x,x). …]` */
const FOLD_EA = 0x4e8a6e;

const hasListing = fs.existsSync(LST);

/** 语料 / 二进制缺席 ⇒ 如实 skip（`AGENTS.md` §5：不许 `diagnostic` + 裸 return） */
function skipReason(t) {
  if (!hasListing) return `语料未解压（${path.relative(REPO_ROOT, LST)} 不存在）⇒ 先 \`pnpm tools disasm build\``;
  try {
    const ref = resolveReferenceBinary(LST);
    if (!fs.existsSync(ref.path)) return `二进制不在场：${ref.path}（fresh clone 要 git lfs pull）`;
  } catch (err) {
    return `二进制解析不到：${err.message}`;
  }
  return null;
}

/** 只读 `.lst` 头部 8 KB（`Input SHA256` / `Imagebase` / `Section N.` 段表都在前 20 行） */
function lstHead() {
  const fd = fs.openSync(LST, 'r');
  try {
    const head = Buffer.alloc(8192);
    const n = fs.readSync(fd, head, 0, head.length, 0);
    return head.subarray(0, n).toString('latin1');
  } finally {
    fs.closeSync(fd);
  }
}

/** `.lst` 头部自带的段表（IDA 打印的 Section N. 四条数）—— 与 PE 节表**互相独立**的一个来源 */
function lstSections() {
  const lines = lstHead().split('\n');
  const out = [];
  for (let i = 0; i < lines.length; i += 1) {
    const m = /;\s*Section (\d+)\. \(virtual address ([0-9A-Fa-f]+)\)/.exec(lines[i]);
    if (!m) continue;
    const block = lines.slice(i, i + 6).join('\n');
    const pick = (re) => {
      const x = re.exec(block);
      return x ? Number.parseInt(x[1], 16) : null;
    };
    out.push({
      n: Number(m[1]),
      va: Number.parseInt(m[2], 16),
      vsize: pick(/Virtual size\s*:\s*([0-9A-Fa-f]+)/),
      rawSize: pick(/Section size in file\s*:\s*([0-9A-Fa-f]+)/),
      rawPtr: pick(/Offset to raw data for section:\s*([0-9A-Fa-f]+)/),
    });
  }
  return out;
}

const imageBase = () => {
  const m = /;\s*Imagebase\s*:\s*([0-9A-Fa-f]+)/.exec(lstHead());
  assert.ok(m, '`.lst` 头部必须有 `; Imagebase :`（EA 是 VA，没有它算不了 RVA）');
  return Number.parseInt(m[1], 16);
};

/** `.lst` 里 8 位大写 EA 的那一行（`0x4E8A6E` → `.text:004E8A6E …`） */
function lstLineAt(ea) {
  const text = fs.readFileSync(LST, 'utf8');
  const pat = new RegExp(`^\\S+?:${ea.toString(16).toUpperCase().padStart(8, '0')}\\s`);
  return text.split('\n').find((l) => pat.test(l)) ?? null;
}

/** 同步抓 stdout / stderr（**不 spawn 子进程**：受限沙箱里管道会 EPERM） */
function capture(argv) {
  const out = [];
  const err = [];
  const writeOut = process.stdout.write;
  const writeErr = process.stderr.write;
  process.stdout.write = (s) => {
    out.push(String(s));
    return true;
  };
  process.stderr.write = (s) => {
    err.push(String(s));
    return true;
  };
  try {
    const code = disasmMain(argv);
    return { code, out: out.join(''), err: err.join('') };
  } finally {
    process.stdout.write = writeOut;
    process.stderr.write = writeErr;
  }
}

/* ───────────────────────── ① 字节 == 二进制，长度 == 折叠行 ───────────────────────── */

test('★ 真实语料：折叠区间 0x4E8A6E 的字节（.lst 折叠长度 == PE 取字节长度，偏移两源一致）', (t) => {
  const why = skipReason(t);
  if (why) {
    t.skip(why);
    return;
  }
  const ref = resolveReferenceBinary(LST);
  const want = listingInputSha256(LST);
  assert.ok(want, '`.lst` 头部必须有 `; Input SHA256 :`（否则"同一份二进制"这条退化成恒真）');

  // ① 二进制 == 语料说的那一份
  const buf = fs.readFileSync(ref.path);
  const got = createHash('sha256').update(buf).digest('hex').toUpperCase();
  assert.equal(
    got,
    want,
    `${path.relative(REPO_ROOT, ref.path)} 的 sha256 与 .lst 头部 Input SHA256 不符 ⇒ 语料与二进制不是同一份（这是清单/语料问题，停下报告）`,
  );

  // ② 折叠行声明的长度
  const line = lstLineAt(FOLD_EA);
  assert.ok(line, `语料里必须有 0x${FOLD_EA.toString(16)} 那一行`);
  const fold = parseCollapsedLine(line);
  assert.ok(fold, `0x${FOLD_EA.toString(16)} 必须是 COLLAPSED 折叠行（实际：${line.slice(0, 80)}）`);
  assert.ok(fold.declaredBytes > 0, `折叠行必须声明字节数（实际：${line.slice(0, 80)}）`);

  // ③ 偏移：PE 节表 vs `.lst` 头部段表（两条独立来源）
  const pe = readPeSections(buf);
  const base = imageBase();
  const at = eaToFileOffset(pe, FOLD_EA);
  const sec = lstSections().find((s) => FOLD_EA >= base + s.va && FOLD_EA < base + s.va + Math.max(s.vsize, s.rawSize));
  assert.ok(sec, `.lst 头部段表里必须找得到包含 0x${FOLD_EA.toString(16)} 的段`);
  assert.equal(
    sec.rawPtr + (FOLD_EA - base - sec.va),
    at.fileOffset,
    `偏移两源不一致：.lst 头部 Section ${sec.n} 算出 ${sec.rawPtr + (FOLD_EA - base - sec.va)}，PE 节表算出 ${at.fileOffset}`,
  );
  // PE 的段四条数必须与 `.lst` 头部 Section 1 完全一致（这份语料就是从这份 PE 导出的）
  const text = pe.sections.find((s) => s.name === '.text');
  assert.ok(text, 'PE 里必须有 .text');
  const lstText = lstSections().find((s) => s.va === text.virtualAddress);
  assert.ok(lstText, '`.lst` 头部必须有与 PE `.text` 同 VA 的 Section 块');
  assert.deepEqual(
    { va: text.virtualAddress, vsize: text.virtualSize, rawSize: text.sizeOfRawData, rawPtr: text.pointerToRawData },
    { va: lstText.va, vsize: lstText.vsize, rawSize: lstText.rawSize, rawPtr: lstText.rawPtr },
    'PE 节表与 .lst 头部段表的四条数必须一致（不一致 = 语料不是从这份二进制导出的）',
  );

  // ④ 长度 == 折叠行声明；内容 == MSVC 序言（偏移错一页就匹配不上）
  const r = readBytesAt(buf, pe, FOLD_EA, fold.declaredBytes);
  assert.equal(r.length, fold.declaredBytes, `readBytesAt 必须正好读到折叠行声明的 ${fold.declaredBytes} 字节`);
  assert.equal(r.sectionName, '.text');
  assert.deepEqual([...r.bytes.subarray(0, 5)], [0x8b, 0xff, 0x55, 0x8b, 0xec], '首 5 字节必须是 MSVC 序言 mov edi,edi / push ebp / mov ebp,esp');
});

/* ───────────────────────── ② CLI：`--bytes` 的三种形态 ───────────────────────── */

test('★ `disasm-at at --bytes`：32 字节 hex + 折叠长度标注；不带开关时输出里没有字节块', (t) => {
  const why = skipReason(t);
  if (why) {
    t.skip(why);
    return;
  }
  const common = ['--at', '--ea', '0x4E8A6E', '--file', LST, '--lines', '5'];
  const withBytes = capture([...common, '--bytes', '32']);
  assert.equal(withBytes.code, 0, `--bytes 32 必须成功（stderr：${withBytes.err}）`);
  assert.match(withBytes.out, /来自 PE 原始字节（折叠区间也能取）/, '必须标注"来自 PE 原始字节（折叠区间也能取）"');
  assert.match(withBytes.out, /声明 76 字节/, '必须把折叠行声明的长度打出来（76 = 0x4C）');
  assert.match(withBytes.out, /在区间内/, '取 32 字节时要说清它只是折叠区间的前缀');

  // 32 个 hex 字节必须与直读结果一致
  const rows = [...withBytes.out.matchAll(/^\s+[0-9a-f]{8}  ((?:[0-9A-F]{2} )*[0-9A-F]{2})\s*$/gm)].map((m) => m[1]);
  const hex = rows.join(' ').split(' ').filter(Boolean);
  assert.equal(hex.length, 32, `应打出 32 个 hex 字节，实际 ${hex.length}：${rows.join(' | ')}`);
  const ref = resolveReferenceBinary(LST);
  const buf = fs.readFileSync(ref.path);
  const direct = readBytesAt(buf, readPeSections(buf), FOLD_EA, 32).bytes;
  assert.deepEqual(hex, [...direct].map((x) => x.toString(16).toUpperCase().padStart(2, '0')), 'CLI 打出的字节必须等于直读的字节');

  // 不带 --bytes ⇒ 输出里**不许**出现字节块（默认行为不变）
  const plain = capture(common);
  assert.equal(plain.code, 0);
  assert.doesNotMatch(plain.out, /PE 原始字节/, '不加 --bytes 时输出必须与旧版一致（不许悄悄多打字节）');

  // --json 形态：pe.hex / pe.fold.declaredBytes 可供机器消费
  const j = capture([...common, '--bytes', '32', '--json']);
  assert.equal(j.code, 0);
  const parsed = JSON.parse(j.out);
  assert.equal(parsed.pe.section, '.text');
  assert.equal(parsed.pe.fold.declaredBytes, 0x4c);
  assert.equal(parsed.pe.hex.length, 64);
  assert.equal(parsed.pe.hex, direct.toString('hex').toUpperCase());
});

test('★ 响亮失败：EA 落在 `.text` 的虚拟尾部（文件里没有字节）⇒ 退出码 1 + 点名原因，不给空字节', (t) => {
  const why = skipReason(t);
  if (why) {
    t.skip(why);
    return;
  }
  const ref = resolveReferenceBinary(LST);
  const buf = fs.readFileSync(ref.path);
  const pe = readPeSections(buf);
  const text = pe.sections.find((s) => s.name === '.text');
  assert.ok(text);
  // `.text` 的 VirtualSize > SizeOfRawData ⇒ raw 末尾之后还有 0xA00 字节"在段里但没有文件字节"
  assert.ok(text.virtualSize > text.sizeOfRawData, '这条用例要求 .text 带虚拟尾部（真实镜像就是这样）');
  const eaInVirtualTail = pe.imageBase + text.virtualAddress + text.sizeOfRawData + 0x10;

  const r = capture(['--at', '--ea', `0x${eaInVirtualTail.toString(16)}`, '--file', LST, '--lines', '2', '--bytes', '8']);
  assert.equal(r.code, 1, '取不到字节必须以非 0 退出（不是"成功但没输出"）');
  assert.match(r.err, /PE 取字节失败/, `stderr 必须响亮报错，实际：${r.err}`);
  assert.match(r.err, /虚拟尾部|文件里没有字节/, `错误必须说清"为什么读不到"，实际：${r.err}`);
  assert.doesNotMatch(r.out, /PE 原始字节/, '失败时不许打出任何字节块');
});
