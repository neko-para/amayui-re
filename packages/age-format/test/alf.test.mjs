/**
 * packages/age-format/test/alf.test.mjs —— ALF 的**往返判据**：解包 → 重打包必须逐字节相同
 *
 * ★ 样本不入库（原始游戏文件，见 `corpus/assets/samples.md`）：`loadSample` 拿不到就**跳过**，
 *   并把"为什么跳过"写在用例名里 —— fresh clone 上不该红，但也**不许静默通过**。
 * ★ 全程纯 JS，不 spawn 任何进程（受限沙箱里捕获子进程输出会 EPERM）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

import {
  readAlf,
  readAlfBuffer,
  writeIndex,
  writeArchive,
  loadPayloads,
  declaredPayloadBytes,
  buildToc,
} from '../src/alf.mjs';
import * as lzss from '../src/lzss.mjs';
import { loadSample, fileOf, sha256 } from './samples.mjs';

const sample = loadSample('assets/samples-alf');
if (!sample) console.log('[ALF] 跳过：样本不在场（原始游戏文件不入库，见 corpus/assets/samples.md）');
const skip = sample ? false : '样本不在场（游戏文件不入库）';

/** 读索引，并把"归档解析"指到样本那份数据体（同目录语义，但不要求数据体在索引旁边） */
function openSample() {
  const idx = fileOf(sample, 'APPEND02.AAI');
  const arc = fileOf(sample, 'APPEND02.ALF');
  const alf = readAlf(idx.abs);
  const real = alf.archivePath;
  alf.archivePath = (name) => (name === 'APPEND02.ALF' ? arc.abs : real(name));
  return { alf, idx, arc };
}

test('ALF：索引文件读进来再写出去，逐字节相同', { skip }, () => {
  const { alf, idx } = openSample();
  const out = writeIndex(alf);
  assert.equal(out.length, idx.buf.length, `长度必须一致（原始 ${idx.buf.length}）`);
  assert.ok(out.equals(idx.buf), '索引必须逐字节相同');
  assert.equal(sha256(out), idx.sha256, '并且与清单登记的 sha256 一致');
});

test('ALF：目录区解压 → 重压，逐字节相同（LZSS 两个方向互逆）', { skip }, () => {
  const { alf } = openSample();
  const toc = alf.section.data.subarray(0, alf.section.size);
  const repacked = lzss.pack(toc);
  assert.equal(repacked.length, alf.section.packed.length, '压缩后长度必须相同');
  assert.ok(repacked.equals(alf.section.packed), '压缩结果必须逐字节相同');
  const back = lzss.unpack(repacked, repacked.length, alf.section.size);
  assert.equal(back.size, alf.section.size);
  assert.ok(back.data.subarray(0, back.size).equals(toc), '再解压回去也必须相同');
});

test('ALF：目录区结构自洽（归档数 / 条目数 / 声明载荷量 / 索引越界）', { skip }, () => {
  const { alf } = openSample();
  assert.equal(alf.archiveCount, alf.archives.length);
  assert.equal(alf.fileCount, alf.entries.length);
  assert.equal(alf.section.size, alf.section.originalLength, '目录区解压长度必须等于段头声明');
  assert.equal(alf.archives[0].filename, 'APPEND02.ALF');
  for (const e of alf.entries) {
    assert.ok(alf.archives[e.archiveIndex], `条目 ${e.filename} 的 archiveIndex=${e.archiveIndex} 越界`);
    assert.ok(e.offset + e.length <= fileOf(sample, 'APPEND02.ALF').buf.length, `条目 ${e.filename} 越出数据体`);
  }
  assert.ok(declaredPayloadBytes(alf) > 0);
});

test('ALF：目录区拼装对未初始化字段保真（不能"重拼名字再补零"）', { skip }, () => {
  const { alf } = openSample();
  const toc = buildToc(alf);
  const orig = alf.section.data.subarray(0, alf.section.size);
  assert.ok(toc.subarray(0, orig.length).equals(orig), '未改动时目录区必须逐字节还原（含文件名后的垃圾字节）');
});

test('ALF：★ 读载荷 → 重打包数据体（.ALF），逐字节相同', { skip }, () => {
  const { alf, arc } = openSample();
  const r = loadPayloads(alf);
  assert.equal(r.missingArchives.length, 0, `缺归档：${r.missingArchives.join(', ')}`);
  assert.equal(r.missingEntries.length, 0, `缺条目：${r.missingEntries.join(', ')}`);
  assert.equal(r.loaded, alf.fileCount, '每个条目都必须读到载荷');

  const { data, gaps } = writeArchive(alf, 0);
  assert.equal(gaps.length, 0, '归档里不该有未被引用的空洞（否则不可能逐字节还原）');
  assert.equal(data.length, arc.buf.length, `数据体长度必须一致（原始 ${arc.buf.length}）`);
  assert.ok(data.equals(arc.buf), '数据体必须逐字节相同');
  assert.equal(sha256(data), arc.sha256, '并且与清单登记的 sha256 一致');
});

test('ALF：★ 改一个条目再打包，索引与数据体都自洽（不是"只会原样抄回去"）', { skip }, () => {
  const { alf, idx, arc } = openSample();
  loadPayloads(alf);

  const victim = alf.entries.find((e) => e.length > 64);
  const grown = Buffer.concat([Buffer.from(victim.payload), Buffer.from('AGE-FORMAT-TEST')]);
  victim.payload = grown;
  victim.length = grown.length;

  // 长度变了 ⇒ 该归档内所有条目的 offset 都要重算（这是"改长度"唯一的正确姿势）
  let cursor = 0;
  for (const e of alf.entries) {
    e.offset = cursor;
    cursor += e.length;
  }

  const newIndex = writeIndex(alf);
  const newArc = writeArchive(alf, 0).data;
  assert.notEqual(sha256(newIndex), idx.sha256, '改过之后索引必然不同');
  assert.equal(newArc.length, arc.buf.length + 15, '数据体正好长出 15 字节');
  assert.ok(
    newArc.subarray(victim.offset, victim.offset + grown.length).equals(grown),
    '新载荷必须落在它声明的 offset 上',
  );

  // 回读新索引：必须仍可解析，且该条目的 offset/length 就是新值
  const reparsed = readAlfBuffer(newIndex, idx.abs);
  const back = reparsed.entries.find((e) => e.filename === victim.filename);
  assert.equal(back.length, grown.length);
  assert.equal(back.offset, victim.offset);
  // 而且回读后"原样重打包"就回到新索引本身（自洽闭环）
  assert.ok(writeIndex(reparsed).equals(newIndex), '新索引必须自洽：再写一遍还是它自己');
});

test('ALF：写数据体时"有空洞"必须报错（而不是静默补零）', { skip }, () => {
  const { alf } = openSample();
  loadPayloads(alf);
  for (const e of alf.entries) e.offset += 16; // 人为制造 16 字节空洞
  assert.throws(() => writeArchive(alf, 0), /未被任何条目引用的字节/);
  const { gaps } = writeArchive(alf, 0, { allowGaps: true });
  assert.equal(gaps.length, 1);
  assert.equal(gaps[0].offset, 0);
  assert.equal(gaps[0].length, 16);
});
