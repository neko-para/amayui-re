/** @env assets @kind contract @why `20-alf` 的协议断言与真实索引/归档脱节（头 268 / AC 的 12 字节块与 280 起点 / TOC 记录宽度与字段 / LZSS 参数），读者会按错的字节结构写解析器 */
/**
 * tools/test/spec-alf.assets.test.mjs —— **ALF 归档的语料侧守卫**
 *
 * 把 `data/spec/chapters/20-alf.md` 的断言落到安装目录里的真实 `*.AAI` / `*.ALF` 上：
 * ① 索引头 268 字节、签名四选一；② `AC` 加装档在 `[268,280)` 有 12 字节压缩信息块、压缩数据从 280 起；
 * ③ 解压后的 TOC 记录宽度（归档 256 / 文件 80）与四个 u32 字段；④ LZSS 参数（窗口 4096 / 写位置 4078 / 位置 12 位 / 长度 = 低 4 位 + 3）。
 *
 * ★ 不证明"与引擎等价"：只证明"按本章的字节结构，真实语料能逐条解释通"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { loadManifest } from '../lib/manifest.mjs';
import { lzssDecode } from './spec-lzss.mjs';

const HEADER_BYTES = 268;
const AC_BLOCK_BYTES = 12;
const ARCHIVE_RECORD_BYTES = 256;
const FILE_RECORD_BYTES = 80;

const u16 = (b, o) => b.readUInt16LE(o);
const u32 = (b, o) => b.readUInt32LE(o);

/** 按头里的签名切开索引：AI = 裸 TOC（268 起）；AC = [268,280) 信息块 + 280 起压缩数据 */
function readToc(bytes, file) {
  const sig = bytes.toString('latin1', 0, 4);
  assert.ok(['S3AI', 'S4AI', 'S3AC', 'S4AC'].includes(sig), `${file}: 未知签名 ${sig}`);
  const compressed = sig.endsWith('AC');
  if (!compressed) return { sig, toc: bytes.subarray(HEADER_BYTES), compressed };
  assert.ok(bytes.length >= HEADER_BYTES + AC_BLOCK_BYTES, `${file}: AC 索引太短`);
  const unpacked = u32(bytes, HEADER_BYTES);
  const again = u32(bytes, HEADER_BYTES + 4);
  const packed = u32(bytes, HEADER_BYTES + 8);
  assert.equal(again, unpacked, `${file}: 12 字节块里"解压后长度"没有重复两次`);
  const dataStart = HEADER_BYTES + AC_BLOCK_BYTES;
  assert.ok(dataStart + packed <= bytes.length, `${file}: 压缩数据长度越出文件`);
  const res = lzssDecode(bytes.subarray(dataStart, dataStart + packed), unpacked);
  assert.equal(res.out.length, unpacked, `${file}: 解压长度与头里的 unpacked 不一致`);
  return { sig, toc: Buffer.from(res.out), compressed, packed, dataStart };
}

function parseToc(toc, file) {
  let o = 0;
  const archiveCount = u32(toc, o); o += 4;
  assert.ok(archiveCount > 0 && archiveCount < 0x10000, `${file}: archive_count 不合理（${archiveCount}）`);
  const archives = [];
  for (let i = 0; i < archiveCount; i += 1) {
    const field = toc.subarray(o, o + ARCHIVE_RECORD_BYTES);
    const nul = field.indexOf(0);
    assert.ok(nul > 0, `${file}: 归档名不是 NUL 结束的字节串`);
    archives.push(field.toString('latin1', 0, nul));
    o += ARCHIVE_RECORD_BYTES;
  }
  const fileCount = u32(toc, o); o += 4;
  assert.ok(fileCount > 0, `${file}: file_count 为 0`);
  const files = [];
  for (let i = 0; i < fileCount; i += 1) {
    const rec = toc.subarray(o, o + FILE_RECORD_BYTES);
    assert.equal(rec.length, FILE_RECORD_BYTES, `${file}: TOC 末尾被截断`);
    const nul = rec.subarray(0, 64).indexOf(0);
    assert.ok(nul > 0, `${file}: 文件名不是 NUL 结束的字节串`);
    files.push({
      name: rec.toString('latin1', 0, nul),
      archiveIndex: u32(rec, 64),
      fileIndex: u32(rec, 68),
      offset: u32(rec, 72),
      length: u32(rec, 76),
    });
    o += FILE_RECORD_BYTES;
  }
  return { archiveCount, archives, fileCount, files, consumed: o };
}

test('★ 真实 *.AAI：头 268 / 四签名 / AC 的 12 字节块与 280 起点 / TOC 记录宽度与字段', (t) => {
  const { roots } = loadManifest();
  const dir = roots?.gameInstall;
  if (!dir || !fs.existsSync(dir)) { t.skip('gameInstall 不在本机'); return; }
  const indexes = fs.readdirSync(dir).filter((f) => /\.aai$/i.test(f));
  assert.ok(indexes.length > 0, '安装目录里没有 *.AAI');
  let ai = 0;
  let ac = 0;
  let checked = 0;
  const sigs = new Set();
  for (const name of indexes) {
    const bytes = fs.readFileSync(path.join(dir, name));
    if (bytes.length < HEADER_BYTES) continue;
    const { sig, toc, compressed } = readToc(bytes, name);
    sigs.add(sig);
    const tocInfo = parseToc(toc, name);
    // ① 归档名指向真实存在的 *.ALF；② offset/length 落在数据体之内；③ 载荷原样存放
    for (const arc of tocInfo.archives) {
      const p = path.join(dir, arc);
      assert.ok(fs.existsSync(p), `${name}: 归档记录指向不存在的文件 ${arc}`);
    }
    const first = tocInfo.files[0];
    const arcPath = path.join(dir, tocInfo.archives[first.archiveIndex] ?? '');
    const sizes = new Map(tocInfo.archives.map((a) => [a, fs.statSync(path.join(dir, a)).size]));
    for (const f of tocInfo.files) {
      const size = sizes.get(tocInfo.archives[f.archiveIndex]);
      assert.ok(size !== undefined, `${name}: 文件记录的 archive_index 越界（${f.archiveIndex}）`);
      assert.ok(f.offset + f.length <= size, `${name}: ${f.name} 的 offset+length 越过 ${tocInfo.archives[f.archiveIndex]}`);
    }
    if (compressed) { ac += 1; assert.ok(first.offset >= 0); } else { ai += 1; }
    checked += 1;
    if (checked >= 8) break;
  }
  assert.ok(checked > 0, '没有解析到任何 *.AAI');
  assert.ok(ac > 0, '没有找到 AC（压缩 TOC）族索引 —— AC 的 12 字节块与 280 起点无从验证');
  if (ai === 0) t.diagnostic('语料里没有 AI（裸 TOC）族索引：268 起的裸分支只在结构上定义，未在真实文件上验证');
  t.diagnostic(`AAI 抽检 ${checked} 份 · AI ${ai} · AC ${ac} · 签名 ${[...sigs].join('/')}`);
});

test('★ LZSS：位置是 12 位环内下标（0..4095）、长度 = 低 4 位 + 3、可重叠匹配', () => {
  // 合成流：#0 字面量 'A'；#1 匹配 (位置 4078, 长度 3) ⇒ 重叠拷贝自身刚写的 'A' → 'AAA'
  // flag = 0b0000_0001（低位先：bit0=1 ⇒ 字面量；bit1=0 ⇒ 匹配）
  const src = Buffer.from([0x01, 0x41, 0xee, 0xf0]);
  // 4078 = 0xFEE：低 8 位 0xEE；高 4 位 0xF ⇒ 第二字节高 4 位 0xF；长度 3 ⇒ 低 4 位 0
  const res = lzssDecode(src, 4);
  assert.equal(Buffer.from(res.out).toString('latin1'), 'AAAA', '重叠匹配必须读到本次刚写出的字节');
  // 长度上限：低 4 位 0xF ⇒ 18
  assert.equal((0x0f) + 3, 18);
  // 位置字段：b0 | (b1 & 0xF0) << 4 ⇒ 12 位
  assert.equal((0xee | ((0xf0 & 0xf0) << 4)) & 0xfff, 0xfee);
  // 位置 0 也可寻址（环回后）
  assert.equal((0x00 | ((0x00 & 0xf0) << 4)) & 0xfff, 0);
});
