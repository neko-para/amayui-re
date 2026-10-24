/** @env pure @kind contract @why 扩展索引（*.AAI）的区号 / id 分段 / 装载栈写错了（区号当文件名、槽空折叠成越界、名字反查不分大小写丢了），扩展区取件会读到另一份文件或报错类型错，而所有基础索引测试仍然绿 */
/**
 * tools/test/emulator-new-aai.test.mjs —— 扩展索引（`CORE-FS-EXT-INDEX-KEY` / `CORE-FS-INDEX-STACK`）的 pure 守卫
 *
 * 用**合成索引字节**验证 [core/alf.ts] / [core/install.ts]：
 * 1. `S4AI` 签名：段起点 **268**、裸 TOC（不压缩）；`S4AC` 仍是 268 + 12；`S4IC` 仍是 300；
 * 2. `Install.open` 枚举 `*.AAI`（不递归、跳过目录、**顺序 = Host 顺序**、核心不排序），
 *    头标题与基础索引逐字节相同 ⇒ 按**头内区号**写槽；同区号后装载者胜；
 * 3. `readById` 的高 8 位按**有符号**解释：0 ⇒ 基础；1..127 ⇒ 槽；槽空 / 下标越界 / 128..255 是**三种**错误；
 *    扩展区取件用**该索引自己的**归档表（不加盘符前缀）；
 * 4. `readByName` 不区分大小写、基础索引优先、其次区号升序、命中扩展返回 `(区号 << 24) | 下标`；
 * 5. `offset === 0xFFFFFFFF` 的哨兵记录**不进入**归档回退。
 *
 * ★ 真实安装目录（`APPEND01..05.AAI`）由 `emulator-new-aai-region.assets.test.mjs` 覆盖。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { parseAlfIndex } from '../../apps/emulator-new/src/core/alf.ts';
import { Install } from '../../apps/emulator-new/src/core/install.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';
import { REPO_ROOT } from '../lib/paths.mjs';

const TEMP = path.join(REPO_ROOT, '.tmp', 'emulator-new-aai-test');
const TITLE = 'TESTGAME';

/* ──────────────────────────── 合成索引字节 ──────────────────────────── */

/** 只用字面量把一段字节编成 LZSS（每个 flag 管 8 个单位；与 20-alf 的压缩族同形） */
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

function fileRecord(name, archiveIndex, offset, length) {
  return Buffer.concat([field(name, 64), u32(archiveIndex), u32(0), u32(offset), u32(length)]);
}

/** TOC 段：archive_count × filename[256] → file_count × 80 字节记录 */
function tocBytes(archives, files) {
  return Buffer.concat([
    u32(archives.length),
    ...archives.map((name) => field(name, 256)),
    u32(files.length),
    ...files,
  ]);
}

/** ALF 族定长头（268）：签名 +0、标题串 +8、区号 +264 */
function alfHead(signature, title, zone) {
  const head = Buffer.alloc(268);
  head.write(signature, 0, 'latin1');
  head.write(title, 8, 'latin1');
  head.writeUInt32LE(zone >>> 0, 264);
  return head;
}

/** `S?AC`（压缩族）：268 字节头 + 12 字节压缩信息块 + LZSS TOC */
function buildAc(title, zone, archives, files) {
  const toc = tocBytes(archives, files);
  const packed = lzssLiterals(toc);
  return Buffer.concat([
    alfHead('S4AC', title, zone),
    u32(toc.length), u32(toc.length), u32(packed.length),
    packed,
  ]);
}

/** `S?AI`（非压缩族）：268 字节头 + **裸 TOC**（从 268 起，不压缩） */
function buildAi(title, zone, archives, files) {
  return Buffer.concat([alfHead('S4AI', title, zone), tocBytes(archives, files)]);
}

/** `S?IC`（初始化文件 / 基础索引）：300 字节头 + 12 字节压缩信息块 + LZSS TOC */
function buildIc(title, archives, files) {
  const head = Buffer.alloc(300);
  head.write('S4IC', 0, 'latin1');
  head.write(title, 8, 'latin1');
  const toc = tocBytes(archives, files);
  const packed = lzssLiterals(toc);
  return Buffer.concat([head, u32(toc.length), u32(toc.length), u32(packed.length), packed]);
}

/* ──────────────────────────── 1. 签名 → 段起点 ──────────────────────────── */

test('★ parseAlfIndex 接受 S4AI：段起点 268、裸 TOC（不压缩）', () => {
  const bytes = buildAi(TITLE, 3, ['APPEND03.ALF'], [
    fileRecord('$3$C.BIN', 0, 0, 4),
    fileRecord('$3$D.BIN', 0, 4, 2),
  ]);
  const index = parseAlfIndex(bytes);
  assert.equal(index.signature, 'S4AI');
  assert.equal(index.sectionPos, 268, '★ AI 族的 TOC 从 268 起（不是 300，也没有压缩信息块）');
  assert.equal(index.originalLength, bytes.length - 268);
  assert.equal(index.packedLength, index.originalLength);
  assert.equal(index.title, TITLE);
  assert.equal(index.zone, 3, '区号来自头内 +264（不从文件名解析）');
  assert.deepEqual(index.archives, ['APPEND03.ALF']);
  assert.deepEqual(index.files[0], { name: '$3$C.BIN', archiveIndex: 0, fileIndex: 0, offset: 0, length: 4 });

  // ★ 三档：AC ⇒ 268 + 12（压缩信息块）、IC ⇒ 300 —— 不能把 AI / AC 并成一档，也不能把 IC 改成 268
  assert.equal(parseAlfIndex(buildAc(TITLE, 1, ['A.ALF'], [fileRecord('F.BIN', 0, 0, 1)])).sectionPos, 268);
  assert.equal(parseAlfIndex(buildIc(TITLE, ['A.ALF'], [fileRecord('F.BIN', 0, 0, 1)])).sectionPos, 300);
});

/* ──────────────────────────── 2. 装载栈（合成安装目录） ──────────────────────────── */

/**
 * 合成安装目录：
 * - 基础索引 `SYS4INI.BIN`：`SYSTEM4.BIN` / `DUP.BIN`（与区号 1 的同名，用来钉"基础优先"）
 * - `APPEND01.AAI`（S4AC，区号 1）：`$1$AUTORUN.BIN` / `$1$SECOND.BIN` / `$1$NOARCHIVE.BIN`
 * - `Z7.AAI` / `A7.AAI`（S4AC，**同为区号 7**）：后者（按 Host 顺序）必须覆盖前者
 * - `C3.AAI`（S4AI，区号 3）：证明 S4AI 也能装
 * - `DIR.AAI`（目录）· `MISMATCH.AAI`（标题不符）· `ZONE300.AAI`（区号 ≥ 256）
 * - `notes.txt`（不是 AAI）
 */
const ARCHIVE_FILES = {
  'DATA1.ALF': Buffer.from('AAAAAABB', 'latin1'),
  'APPEND01.ALF': Buffer.from('ABCDEFGH', 'latin1'),
  'APPEND03.ALF': Buffer.from('IJKL', 'latin1'),
  'Z7.ALF': Buffer.from('ZZZZ', 'latin1'),
  'A7.ALF': Buffer.from('AAAA', 'latin1'),
  'MISMATCH.ALF': Buffer.from('MMMM', 'latin1'),
};

/** ★ 故意不是字典序：核心若排序，slot[7] 会是被**先**给的 Z7 */
const HOST_ORDER = [
  'Z7.AAI',
  'A7.AAI',
  'APPEND01.AAI',
  'C3.AAI',
  'DIR.AAI',
  'MISMATCH.AAI',
  'ZONE300.AAI',
  'notes.txt',
  'SYS4INI.BIN',
  'DATA1.ALF',
];

function writeInstall() {
  fs.rmSync(TEMP, { recursive: true, force: true });
  fs.mkdirSync(TEMP, { recursive: true });
  fs.writeFileSync(path.join(TEMP, 'SYS4INI.BIN'), buildIc(TITLE, ['DATA1.ALF'], [
    fileRecord('SYSTEM4.BIN', 0, 0, 4),
    fileRecord('DUP.BIN', 0, 4, 2),
  ]));
  fs.writeFileSync(path.join(TEMP, 'APPEND01.AAI'), buildAc(TITLE, 1, ['APPEND01.ALF'], [
    fileRecord('$1$AUTORUN.BIN', 0, 0, 4),
    fileRecord('$1$SECOND.BIN', 0, 4, 2),
    fileRecord('$1$NOARCHIVE.BIN', 0, 0xffffffff, 4),
  ]));
  fs.writeFileSync(path.join(TEMP, 'Z7.AAI'), buildAc(TITLE, 7, ['Z7.ALF'], [
    fileRecord('Z7FIRST.BIN', 0, 0, 4),
  ]));
  fs.writeFileSync(path.join(TEMP, 'A7.AAI'), buildAc(TITLE, 7, ['A7.ALF'], [
    fileRecord('A7LAST.BIN', 0, 0, 4),
  ]));
  fs.writeFileSync(path.join(TEMP, 'C3.AAI'), buildAi(TITLE, 3, ['APPEND03.ALF'], [
    fileRecord('$3$C.BIN', 0, 0, 4),
  ]));
  fs.mkdirSync(path.join(TEMP, 'DIR.AAI'));
  fs.writeFileSync(path.join(TEMP, 'MISMATCH.AAI'), buildAc('OTHERGAME', 9, ['MISMATCH.ALF'], [
    fileRecord('M.BIN', 0, 0, 4),
  ]));
  fs.writeFileSync(path.join(TEMP, 'ZONE300.AAI'), buildAc(TITLE, 300, ['MISMATCH.ALF'], [
    fileRecord('Z.BIN', 0, 0, 4),
  ]));
  fs.writeFileSync(path.join(TEMP, 'notes.txt'), 'not an index');
  for (const [name, payload] of Object.entries(ARCHIVE_FILES)) fs.writeFileSync(path.join(TEMP, name), payload);
}

/** 固定 Host 顺序（宿主顺序是合同的输入；真实 `nodeHost.listDir` 的顺序由 OS 决定，不适合钉"后者胜"） */
const orderedHost = { ...nodeHost, listDir: async () => HOST_ORDER };

test('★ 扩展索引：同区号后者覆盖（顺序 = Host 顺序，核心不排序）', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  assert.equal(install.slots.length, 256, '槽表容量固定 256');
  assert.equal(install.slot(7)?.files[0]?.name, 'A7LAST.BIN',
    '★ 按 Host 顺序 A7.AAI 在 Z7.AAI 之后 ⇒ 后装载者胜（若核心按字典序排，胜者会是 Z7）');
  assert.equal(install.slot(7)?.archives[0], 'A7.ALF', '槽里是**整份**索引（归档表也是它自己的）');
  assert.equal(install.slot(3)?.signature, 'S4AI', 'S4AI 也能装载并按头内区号写槽');
  assert.deepEqual(install.aaiDiagnostics.map((line) => line.split(':')[0]),
    ['MISMATCH.AAI', 'ZONE300.AAI'], '目录 / 非 AAI / 基础索引都不该产生诊断');
});

test('★ readById(0x01000000) 经扩展索引取件（归档名用该索引自己的表）', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  const first = await install.readById(0x01000000);
  assert.equal(first.id, 0x01000000);
  assert.equal(first.name, '$1$AUTORUN.BIN', '★ 低 24 位是**该扩展索引自己的**下标');
  assert.equal(first.side, 'archive');
  assert.equal(Buffer.from(first.bytes).toString('latin1'), 'ABCD');
  assert.equal(first.entryLength, 4);
  assert.equal(first.archive, 'APPEND01.ALF', '★ 归档名来自这份索引的归档表，不加盘符前缀');

  const second = await install.readById(0x01000001);
  assert.equal(second.name, '$1$SECOND.BIN');
  assert.equal(Buffer.from(second.bytes).toString('latin1'), 'EF');

  const viaAi = await install.readById(0x03000000);
  assert.equal(viaAi.name, '$3$C.BIN');
  assert.equal(Buffer.from(viaAi.bytes).toString('latin1'), 'IJKL');
});

test('★ readByName("$1$autorun.bin") 大小写不敏感；基础索引优先、其次区号升序', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  const hit = await install.readByName('$1$autorun.bin');
  assert.ok(hit, '★ 小写必须命中（不区分大小写）');
  assert.equal(hit.id, 0x01000000, '★ 命中扩展索引 ⇒ (区号 << 24) | 下标');
  assert.equal(hit.name, '$1$AUTORUN.BIN');

  const base = await install.readByName('system4.bin');
  assert.equal(base?.id, 0, '基础索引优先（且不区分大小写）');

  const dup = await install.readByName('dup.bin');
  assert.equal(dup?.id, 1, '基础索引里同名 ⇒ 返回基础索引的下标（高 8 位为 0），不是区号 1 的那份');

  assert.equal(await install.readByName('no-such-file.bin'), null, '全不中 ⇒ null（不是 0 号文件）');
});

test('★ 槽空 vs 下标越界是**不同**错误（128..255 也未定义）', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  await assert.rejects(install.readById(0x7f000000), /aai-not-loaded:127/,
    '★ 区号 127 没装载 ⇒ "未装载"，不是越界');
  await assert.rejects(install.readById(0x01000005), /aai-entry-out-of-range:1:5:3/,
    '★ 区号 1 只有 3 条 ⇒ "扩展区条目越界"，带上区号 / 下标 / 条数');
  await assert.rejects(install.readById(0x81000000), /aai-zone-undefined:-127/,
    '★ 高 8 位按有符号解释：0x81 ⇒ -127（128..255 无定义，⛔ 不模拟读槽表之外）');
  await assert.rejects(install.readById(9), /script-id-out-of-range:9\/2/,
    '基础区仍是原来的错误（三者彼此可分）');

  assert.notEqual(
    await install.readById(0x7f000000).catch((e) => e.message),
    await install.readById(0x01000005).catch((e) => e.message),
    '槽空与下标越界必须是两条不同的消息',
  );
});

test('★ 扩展索引装载失败：标题不符 / 区号越槽 ⇒ 诊断 + 跳过（不中止、不写槽）', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  const diagnostics = install.aaiDiagnostics.join('\n');
  assert.match(diagnostics, /MISMATCH\.AAI: aai-title-mismatch:OTHERGAME/,
    '头标题必须与基础索引逐字节相同');
  assert.match(diagnostics, /ZONE300\.AAI: aai-zone-out-of-slot:300/,
    '区号 ≥ 槽表容量 ⇒ 跳过（规范不规定越界行为）');
  assert.equal(diagnostics.match(/DIR\.AAI/g), null, '目录项静默跳过（不是装载失败）');

  assert.equal(install.slot(9), null, '失败的那份不写槽');
  await assert.rejects(install.readById(0x09000000), /aai-not-loaded:9/);
  // ★ 失败的那份也不能顶掉同区号先前成功装载的那份（这里是"没有先前那份"⇒ 槽仍空）
  assert.equal(install.slot(9), null);
});

test('★ offset === 0xFFFFFFFF 不进入归档回退（哨兵在偏移字段）', async () => {
  writeInstall();
  const install = await Install.open(orderedHost, TEMP);

  await assert.rejects(install.readById(0x01000002), /alf-no-archive:\$1\$NOARCHIVE\.BIN/,
    '★ 哨兵记录不许拿去 readSlice（读写位置 4 GiB 的失败不是"取件失败"的正确形状）');
  await assert.rejects(install.readArchiveById(0x01000002), /alf-no-archive/);
});

test('★ 目录枚举失败 ⇒ 视为没有扩展索引（open 不失败）', async () => {
  writeInstall();
  const noList = { ...nodeHost, listDir: async () => { throw new Error('EACCES'); } };
  const install = await Install.open(noList, TEMP);
  assert.equal(install.slot(1), null);
  assert.deepEqual(install.aaiDiagnostics, []);
  assert.equal((await install.readById(0)).name, 'SYSTEM4.BIN', '基础索引照常可用');
});

test('★ 清理临时安装树', () => {
  fs.rmSync(TEMP, { recursive: true, force: true });
});
