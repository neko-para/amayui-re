/** @env pure @kind gate @suite toolchain @why EA→文件偏移 的边界判错了（命中/未映射/越界/虚拟尾部），或 `--bytes` 开关在 describe 里消失 —— 折叠区间的字节会静默取错/取不到 */
/**
 * tools/test/pe.test.mjs —— **EA → PE 段表 → 文件偏移 → 字节** 的纯函数守卫（合成 PE，不依赖语料/游戏）
 *
 * 为什么必须有这一份：PE 解析在 `tools/lib/ledger.mjs` 里**只有一处**（`tools/test/ledger.test.mjs` 钉着
 * "工具侧不许出现第二处 PE 节表解析"），而 `readPeSections` / `eaToFileOffset` / `readBytesAt` 是
 * "折叠区间取字节"的唯一入口，它最容易错的三件事**在合成缓冲上就能判**、且不依赖机器上有没有 LFS/语料：
 *
 *   ① **EA 是 VA 不是 RVA**：`rva = ea − ImageBase`。把它当 RVA 比，对任何真实镜像恒失败
 *      （`ledger` 踩过，见 `tools/test/ledger.pe.external.test.mjs`）；合成 PE 的 ImageBase ≠ 0 才测得到这一半。
 *   ② **段界 = `max(VirtualSize, SizeOfRawData)`**：`VirtualSize > SizeOfRawData` 的那一截是**虚拟尾部**
 *      （文件里没有字节）。用错判据会把它报成"不在任何段里"——对调用方是**不同**的结论。
 *   ③ **失败必须响亮**：非映射 / 越界 / 虚拟尾部 / 跨节 / 文件截断各有自己的 `code`；
 *      ⛔ 返回 `null` 或空 Buffer 会让"折叠区间取到字节了"变成假话。
 *
 * ★ 合成缓冲是**手写的**（不是抄库）：字段偏移见下面的常量注释，来源是 PE/COFF 规范；
 *   布局 = DOS 头 0x40 + `PE\0\0` + 20 B COFF + 0xE0 可选头 + 2 × 40 B 节表 + 两段原始字节。
 *
 * 运行：`pnpm verify`（`@env pure`，零外部依赖）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { describe, describeText, parseCollapsedLine } from '../lib/disasm.mjs';
import { PeError, eaToFileOffset, readBytesAt, readPeSections } from '../lib/ledger.mjs';

/* ─────────────────────── 合成 PE32（手写字段，偏移见上） ─────────────────────── */

const PE_AT = 0x40; // e_lfanew：DOS 头 0x00–0x3F
const OPT_SIZE = 0xe0; // PE32 可选头典型长度（224）
const SECTION_TABLE = PE_AT + 4 + 20 + OPT_SIZE; // 0x138
const FILE_LEN = 0x700;

/** 2 个节：`.text`（正好可执行、raw 0x200）与 `.data`（VirtualSize 0x300 > raw 0x100 ⇒ 有虚拟尾部） */
const DEFAULT_SECTIONS = [
  { name: '.text', virtualAddress: 0x1000, virtualSize: 0x180, sizeOfRawData: 0x200, pointerToRawData: 0x200, fill: 0x10, characteristics: 0x60000020 },
  { name: '.data', virtualAddress: 0x2000, virtualSize: 0x300, sizeOfRawData: 0x100, pointerToRawData: 0x600, fill: 0xa0, characteristics: 0xc0000040 },
];

/**
 * 造一份最小 PE32。段原始字节按 `(fill + k) & 0xFF` 填 ⇒ 期望值可以逐字节算出来。
 * ⛔ 不读任何文件、不依赖任何库。
 */
function synthPe({ imageBase = 0x400000, sizeOfImage = 0x3000, sections = DEFAULT_SECTIONS } = {}) {
  const buf = Buffer.alloc(FILE_LEN);
  buf.write('MZ', 0, 'latin1');
  buf.writeUInt32LE(PE_AT, 0x3c);
  buf.write('PE\0\0', PE_AT, 'latin1');
  const coff = PE_AT + 4;
  buf.writeUInt16LE(0x014c, coff); // Machine = i386
  buf.writeUInt16LE(sections.length, coff + 2);
  buf.writeUInt16LE(OPT_SIZE, coff + 16);
  buf.writeUInt16LE(0x0102, coff + 18);
  const opt = coff + 20;
  buf.writeUInt16LE(0x10b, opt); // PE32
  buf.writeUInt32LE(imageBase, opt + 28);
  buf.writeUInt32LE(0x1000, opt + 32); // SectionAlignment
  buf.writeUInt32LE(0x200, opt + 36); // FileAlignment
  buf.writeUInt32LE(sizeOfImage, opt + 56);
  sections.forEach((s, i) => {
    const h = SECTION_TABLE + i * 40;
    buf.write(s.name.padEnd(8, '\0'), h, 'latin1');
    buf.writeUInt32LE(s.virtualSize, h + 8);
    buf.writeUInt32LE(s.virtualAddress, h + 12);
    buf.writeUInt32LE(s.sizeOfRawData, h + 16);
    buf.writeUInt32LE(s.pointerToRawData, h + 20);
    buf.writeUInt32LE(s.characteristics ?? 0x60000020, h + 36);
    for (let k = 0; k < s.sizeOfRawData && s.pointerToRawData + k < buf.length; k += 1) {
      buf[s.pointerToRawData + k] = (s.fill + k) & 0xff;
    }
  });
  return buf;
}

/** `assert.throws` 的校验函数：按 `code` 断言，顺带要求消息里有"人话" */
const code = (want) => (err) => {
  assert.ok(err instanceof PeError, `应当是 PeError，实际 ${err?.name}: ${err?.message}`);
  assert.equal(err.code, want, `错码应为 ${want}，实际 ${err.code}（${err.message}）`);
  assert.ok(err.message.length > 20, `错误消息必须说明"哪个 EA / 哪个段 / 差多少"，实际：${err.message}`);
  return true;
};

/* ─────────────────────────── ① 命中 ─────────────────────────── */

test('① 命中：EA(VA) → 文件偏移 = PointerToRawData + (RVA − VirtualAddress)，字节逐字节可算', () => {
  const buf = synthPe();
  const pe = readPeSections(buf);
  assert.equal(pe.imageBase, 0x400000);
  assert.equal(pe.numberOfSections, 2);
  assert.deepEqual(pe.sections.map((s) => s.name), ['.text', '.data']);
  assert.equal(pe.sections[0].mappedSize, 0x200, '.text 的段界取 max(0x180, 0x200)');
  assert.equal(pe.sections[1].mappedSize, 0x300, '.data 的段界取 max(0x300, 0x100)——虚拟尾部必须算在段里');

  const ea = 0x400000 + 0x1000 + 0x10;
  const at = eaToFileOffset(pe, ea);
  assert.equal(at.rva, 0x1010);
  assert.equal(at.offsetInSection, 0x10);
  assert.equal(at.fileOffset, 0x210, '0x200（PointerToRawData）+ 0x10');
  assert.equal(at.sectionName, '.text');
  assert.equal(at.sectionIndex, 0);

  const r = readBytesAt(buf, pe, ea, 8);
  assert.equal(r.length, 8);
  assert.deepEqual([...r.bytes], [0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27]);
  assert.equal(r.bytes.toString('hex').toUpperCase(), '2021222324252627');

  // 取到的字节是**拷贝**：改它不许动到源缓冲（语料只读）
  r.bytes[0] = 0xff;
  assert.equal(buf[r.fileOffset], 0x20, 'readBytesAt 必须返回拷贝，不是源缓冲的视图');
});

/* ────────────────────── ② 未映射节 ────────────────────── */

test('② 未映射节：EA 落在两个段之间的空隙 ⇒ 抛 unmapped（不是 null / 空 Buffer）', () => {
  const pe = readPeSections(synthPe());
  // .text 映射 RVA 0x1000..0x11FF、.data 从 0x2000 起 ⇒ 0x1900 在空隙里
  assert.throws(() => eaToFileOffset(pe, 0x400000 + 0x1900), code('unmapped'));
  assert.throws(() => readBytesAt(synthPe(), pe, 0x400000 + 0x1900, 4), code('unmapped'));
  // 消息必须把"已映射的段"列出来（否则读者只能猜）
  try {
    eaToFileOffset(pe, 0x400000 + 0x1900);
  } catch (err) {
    assert.match(err.message, /\.text=RVA 0x1000/, `应列出已映射段，实际：${err.message}`);
    assert.match(err.message, /\.data=RVA 0x2000/);
  }
});

/* ──────────────────────── ③ 越界 ──────────────────────── */

test('③ 越界：低于 ImageBase（把 RVA 当 EA）/ 超出 SizeOfImage ⇒ 各自响亮报错', () => {
  const pe = readPeSections(synthPe());
  // 0x1010 是 .text 的 RVA —— 把它当 EA 传进来是**最常见**的错，必须点出来
  assert.throws(() => eaToFileOffset(pe, 0x1010), code('below-image-base'));
  try {
    eaToFileOffset(pe, 0x1010);
  } catch (err) {
    assert.match(err.message, /RVA 当成 EA/, `要把"你可能传了 RVA"说出来，实际：${err.message}`);
  }
  // SizeOfImage = 0x3000 ⇒ 0x403000 已在镜像外
  assert.throws(() => eaToFileOffset(pe, 0x400000 + 0x3000), code('beyond-image'));
});

/* ───────────── ④ 节内偏移超 SizeOfRawData + 跨节 + 文件截断 ───────────── */

test('④ 节内偏移超 SizeOfRawData（虚拟尾部）⇒ no-raw-bytes；跨节读 ⇒ range-exceeds-raw；文件短 ⇒ file-truncated', () => {
  const buf = synthPe();
  const pe = readPeSections(buf);
  // .data：VirtualSize 0x300 > SizeOfRawData 0x100 ⇒ 段内 +0x250 在段里，但**文件里没有字节**
  assert.throws(() => eaToFileOffset(pe, 0x400000 + 0x2000 + 0x250), code('no-raw-bytes'));
  // 从 .data 起点读 0x101 字节 ⇒ 越过 SizeOfRawData；最后 1 字节会落进文件里的别的区域
  assert.throws(() => readBytesAt(buf, pe, 0x400000 + 0x2000, 0x101), code('range-exceeds-raw'));
  // 正好整段仍可读 —— 边界不许"宁左勿右"地多报
  assert.equal(readBytesAt(buf, pe, 0x400000 + 0x2000, 0x100).length, 0x100);

  // 文件被截到 .data 原始字节中间：节表还在（0x138 之前），但字节不够
  const cut = synthPe().subarray(0, 0x680);
  assert.throws(() => readBytesAt(cut, pe, 0x400000 + 0x2000, 0x100), code('file-truncated'));

  // n 必须是正整数：0 是"静默的空结果"，直接拒
  assert.throws(() => readBytesAt(buf, pe, 0x400000 + 0x1000, 0), code('bad-length'));
  assert.throws(() => readBytesAt(buf, pe, 0x400000 + 0x1000, -1), code('bad-length'));
});

/* ───────────────────── 头部结构本身的响亮拒绝 ───────────────────── */

test('头部坏了 ⇒ 各自响亮报错（不是"读不到就当空"）', () => {
  assert.throws(() => readPeSections(Buffer.alloc(0x10)), code('too-small'));
  assert.throws(() => readPeSections(Buffer.alloc(0x100)), code('not-mz'));

  const badFanew = Buffer.alloc(0x100);
  badFanew.write('MZ', 0, 'latin1');
  badFanew.writeUInt32LE(0x80, 0x3c); // 0x80 处不是 PE 签名
  assert.throws(() => readPeSections(badFanew), code('not-pe'));

  const pe32plus = synthPe();
  pe32plus.writeUInt16LE(0x20b, PE_AT + 4 + 20); // 可选头 magic 改成 PE32+
  assert.throws(() => readPeSections(pe32plus), code('unsupported-optional-header'));

  const noSections = synthPe();
  noSections.writeUInt16LE(0, PE_AT + 4 + 2);
  assert.throws(() => readPeSections(noSections), code('bad-section-count'));

  // ★ 只给一份节表数组算不出偏移（EA 是 VA）—— 这正是要防的那类错，必须点名
  assert.throws(() => eaToFileOffset(readPeSections(synthPe()).sections, 0x401000), code('bad-pe-model'));
});

/* ─────────────────── 折叠行解析（--bytes 的折叠标注） ─────────────────── */

test('★ 折叠行：只认 COLLAPSED / CHUNK 形态；声明长度从 `[NNNNNNNN BYTES:` 读，没写 ⇒ null（不是 0）', () => {
  const fold = '.text:004E8A6E ; [0000004C BYTES: COLLAPSED FUNCTION _CxxThrowException(x,x). PRESS CTRL-NUMPAD+ TO EXPAND]';
  assert.deepEqual(parseCollapsedLine(fold)?.declaredBytes, 0x4c);
  const chunk = '.text:004E8ABA ; [000000C0 BYTES: COLLAPSED CHUNK OF FUNCTION _setlocale. PRESS CTRL-NUMPAD+ TO EXPAND]';
  assert.deepEqual(parseCollapsedLine(chunk)?.declaredBytes, 0xc0);
  // 孤立 CHUNK 没有 `[NNNN BYTES:` 形态 ⇒ null（"没写" ≠ "0 字节"）
  const orphan = '.text:004F0F4F ; START OF FUNCTION CHUNK FOR sub_4F0F00';
  assert.deepEqual(parseCollapsedLine(orphan)?.declaredBytes, null);
  // 普通指令行 / 非字符串 ⇒ null（不是折叠行）
  assert.equal(parseCollapsedLine('.text:004E8C12 mov edi, edi'), null);
  assert.equal(parseCollapsedLine(''), null);
  assert.equal(parseCollapsedLine(undefined), null);
});

/* ────────────────── describe 不许漏掉新开关（漂移守卫） ────────────────── */

test('★ describe 必须反映 `--bytes` / `--bin`（新开关不许只活在 parseArgs 里）', () => {
  const d = describe();
  const names = d.fields.map(([n]) => n);
  assert.ok(names.includes('bytes'), `describe.fields 缺 bytes：${names.join(' / ')}`);
  assert.ok(names.includes('bin'), `describe.fields 缺 bin：${names.join(' / ')}`);
  const text = describeText(d);
  assert.match(text, /--bytes/, 'describeText 必须写出 --bytes');
  assert.match(text, /--bin/, 'describeText 必须写出 --bin');
  const at = d.operations.find((o) => o.name === 'at');
  assert.ok(at, 'describe.operations 必须有 at');
  assert.match(at.summary, /--bytes/, '动作表（OPERATIONS）的 at 摘要必须提 --bytes');
  // 命令提示用域自己的 id（`disasm` 是语料保真域，`pnpm tools disasm at` 不存在）
  assert.match(text, /pnpm tools disasm-at at/, `describeText 的命令提示必须用域 id，实际：${text}`);
});
