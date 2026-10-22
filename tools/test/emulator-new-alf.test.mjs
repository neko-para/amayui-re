/** @env pure @kind contract @why ALF 索引解析 / LZSS / 取件顺序写错了（名字错、偏移错、松散优先丢了），启动会读到另一份文件而"看起来正常" */
/**
 * tools/test/emulator-new-alf.test.mjs —— ALF 驱动（FMT-ALF）的 pure 守卫
 *
 * 用**合成字节**验证 [core/lzss.ts] / [core/alf.ts] / [core/install.ts]：
 * 1. LZSS：字面量与"匹配"两种单位；匹配的位置是环形窗口的**绝对写位置**；
 * 2. ALF 索引：S4 头 → 段（12 字节头 + 压缩数据）→ TOC（归档表 + 文件表）；
 * 3. 取件：统一文件 id == TOC 条目下标；**松散文件优先**，否则按 (archive, offset, length) 切片。
 *
 * ★ 真实语料由 `tools/test/emulator-new-boot.assets.test.mjs` 覆盖（21109 条目的索引 + GB 级归档）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { decompressLzss } from '../../apps/emulator-new/src/core/lzss.ts';
import { parseAlfIndex } from '../../apps/emulator-new/src/core/alf.ts';
import { Install } from '../../apps/emulator-new/src/core/install.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';
import { REPO_ROOT } from '../lib/paths.mjs';

/** 核心层输出的是纯 `Uint8Array`（不是 `Buffer`）⇒ 断言前显式转成字节串 */
const latin1 = (bytes) => Buffer.from(bytes).toString('latin1');

const TEMP = path.join(REPO_ROOT, '.tmp', 'emulator-new-alf-test');

/** 只用字面量把一段字节编成 LZSS（每个 flag 管 8 个单位） */
function lzssLiterals(bytes) {
  const parts = [];
  for (let i = 0; i < bytes.length; i += 8) {
    const flags = Math.min(8, bytes.length - i) === 8 ? 0xff : (1 << (bytes.length - i)) - 1;
    parts.push(Buffer.from([flags]), bytes.subarray(i, i + 8));
  }
  return Buffer.concat(parts);
}

function u32(v) {
  const b = Buffer.alloc(4);
  b.writeUInt32LE(v >>> 0, 0);
  return b;
}

function field(text, size) {
  const b = Buffer.alloc(size);
  b.write(text, 0, 'latin1');
  return b;
}

/** 构造一个最小的 S4IC 索引：1 个归档 + 2 个文件 */
function buildIndex() {
  const archives = [field('DATA1.ALF', 256)];
  const files = [
    Buffer.concat([field('SYSTEM4.BIN', 64), u32(0), u32(0), u32(0), u32(6)]),
    Buffer.concat([field('OTHER.BIN', 64), u32(0), u32(0), u32(6), u32(2)]),
  ];
  const toc = Buffer.concat([u32(archives.length), ...archives, u32(files.length), ...files]);
  const packed = lzssLiterals(toc);
  const head = Buffer.alloc(300);
  head.write('S4IC', 0, 'latin1');
  const section = Buffer.concat([u32(toc.length), u32(toc.length), u32(packed.length), packed]);
  return Buffer.concat([head, section]);
}

/** 构造 DATA1.ALF 的载荷：前 6 字节给 SYSTEM4.BIN，后 2 字节给 OTHER.BIN */
const ARCHIVE_PAYLOAD = Buffer.from('AAAAAABB', 'latin1');

test('★ LZSS：字面量 + 匹配（位置是环形窗口的绝对写位置）', async () => {
  // 3 个字面量 A B C，然后一个匹配（pos = 0x0FEE, len = 3）⇒ 再吐 A B C
  const packed = Buffer.from([0x07, 0x41, 0x42, 0x43, 0xee, 0xf0]);
  const out = decompressLzss(packed, 6);
  assert.equal(latin1(out), 'ABCABC');
});

test('★ LZSS：截断的压缩流要响亮失败（不许静默返回短缓冲）', async () => {
  assert.throws(() => decompressLzss(Buffer.from([0x01]), 8), /lzss-truncated/);
});

test('★ ALF 索引：S4IC → 段 → TOC（归档名 + 文件表）', async () => {
  const index = parseAlfIndex(buildIndex());
  assert.equal(index.signature, 'S4IC');
  assert.equal(index.sectionPos, 300);
  assert.deepEqual(index.archives, ['DATA1.ALF']);
  assert.equal(index.files.length, 2);
  assert.deepEqual(index.files[0], { name: 'SYSTEM4.BIN', archiveIndex: 0, fileIndex: 0, offset: 0, length: 6 });
  assert.deepEqual(index.files[1], { name: 'OTHER.BIN', archiveIndex: 0, fileIndex: 0, offset: 6, length: 2 });
});

test('★ ALF 索引：非 S4 签名 / 段长不一致 / 越界都要响亮失败', async () => {
  const bad = buildIndex();
  bad.write('S5IC', 0, 'latin1');
  assert.throws(() => parseAlfIndex(bad), /alf-unsupported-signature/);

  const mismatch = buildIndex();
  mismatch.writeUInt32LE(1, 304);
  assert.throws(() => parseAlfIndex(mismatch), /alf-section-length-mismatch/);

  const short = buildIndex().subarray(0, 305);
  assert.throws(() => parseAlfIndex(short), /alf-(section|packed)-past-eof/);
});

test('★ 取件：id == 条目下标；**松散文件优先**；没有松散文件才从归档切片', async () => {
  fs.rmSync(TEMP, { recursive: true, force: true });
  fs.mkdirSync(TEMP, { recursive: true });
  fs.writeFileSync(path.join(TEMP, 'SYS4INI.BIN'), buildIndex());
  fs.writeFileSync(path.join(TEMP, 'DATA1.ALF'), ARCHIVE_PAYLOAD);
  fs.writeFileSync(path.join(TEMP, 'SYSTEM4.BIN'), 'LOOSE4');

  const install = await Install.open(nodeHost, TEMP);
  assert.equal(install.entryCount, 2);
  assert.equal(install.entry(0).name, 'SYSTEM4.BIN');

  const loose = await install.readById(0);
  assert.equal(loose.side, 'loose');
  assert.equal(loose.name, 'SYSTEM4.BIN');
  assert.equal(latin1(loose.bytes), 'LOOSE4');
  assert.equal(loose.entryLength, 6, 'TOC 登记的长度仍然报出来（与实际字节数不同）');

  const archived = await install.readById(1);
  assert.equal(archived.side, 'archive');
  assert.equal(latin1(archived.bytes), 'BB');
  assert.equal(archived.archive, 'DATA1.ALF');

  const onlyArchive = await install.readArchiveById(0);
  assert.equal(latin1(onlyArchive.bytes), 'AAAAAA');

  assert.equal(await install.locate('SYSTEM4.BIN'), 'loose');
  assert.equal(await install.locate('OTHER.BIN'), 'archive');
  assert.equal(await install.locate('NOPE.BIN'), 'missing');

  await assert.rejects(install.readById(9), /script-id-out-of-range/);
  fs.rmSync(TEMP, { recursive: true, force: true });
});
