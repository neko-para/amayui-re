/**
 * packages/age-format/src/alf.mjs —— **领域模型**：ALF 归档（`*.AAI` / `SYS?INI.BIN` 索引 + `*.ALF` 数据体）
 *
 * 来源（只读参照）：`天結/scripts/alf/unpack_alf.mjs`（asmodean `exs4alf` 的 Node 移植）与
 * `天結/tools/alf/{unpack_alf,packdata,lzss}/*.cpp`。本文件**只做容器**：不写任何游戏语义结论。
 *
 * ## 盘上布局（均 little-endian；字段宽度按 C `struct` 的磁盘表示，不随平台变）
 * ```text
 * [0, 300)        S4HDR : char signature_title[240] + unknown[60]         ← S4 系（单字节文件名）
 * [0, 540)        S5HDR : WCHAR signature_title[240] + unknown[60]        ← S5 系（UTF-16LE 文件名）
 * 偏移 268 / 532  S4AC / S5AC：**加装档**把段起点提前到这里（原版注释叫 "Hack for addon archives"）
 * 段起点          S4SECTHDR : uint32 original_length + uint32 original_length2 + uint32 length
 * 紧随其后         length 字节 LZSS 压缩数据 ⇒ 解压出 original_length 字节的**目录区（TOC）**
 *
 * 目录区（解压后）：
 *   uint32 archive_count
 *   archive_count × { char filename[256]   | WCHAR filename[256] }       ← 指向同目录下的 *.ALF
 *   uint32 file_count
 *   file_count × { char filename[64]       | WCHAR filename[64]
 *                  uint32 archive_index; uint32 file_index; uint32 offset; uint32 length }
 * ```
 * ★ `file_index` 的用途至今不明（原版注释就写着 "within archive?"）—— 本文件**不做解释，只保真搬运**。
 *
 * ## 为什么能"解包 → 重打包逐字节相同"
 * 1. 文件名字段后面那截是**未初始化内存**（原版注释），所以每项都保留**原始定长字节**，
 *    只在真的要改名时才覆写 —— 不"重新拼一个名字再补零"（那必然与原件不同）。
 * 2. 目录区用 `lzss.pack` 重压。该算法对真实游戏索引文件实测**逐字节可复现**（见 `lzss.mjs` 头注释）。
 * 3. 载荷（各 `*.ALF` 里的文件内容）**不做任何再压缩**：ALF 里的文件是原样存放的，
 *    所以 repack 就是"按 TOC 声明的 offset/length 从归档里搬字节"。
 * ⇒ 不做任何修改的 `readAlf` → `writeAlf` 必得**逐字节相同**的 Buffer（守卫就是这么断言的）。
 */
import { Buffer } from 'node:buffer';
import fs from 'node:fs';
import path from 'node:path';

import * as lzss from './lzss.mjs';

/** 索引文件的签名（前 4 或前 8 字节）→ 盘上布局参数 */
export const LAYOUTS = {
  S4: { unicode: false, hdrSize: 300, addonPos: 268, arcEntry: 256, filName: 64, filEntry: 80, sigs: ['S3IC', 'S4IC', 'S3AC', 'S4AC'] },
  S5: { unicode: true, hdrSize: 540, addonPos: 532, arcEntry: 512, filName: 128, filEntry: 144, sigs: ['S5IC', 'S5AC'] },
};

export const SECT_HDR_SIZE = 12;

/** 读定长单字节字符串（遇 NUL 截断；按 latin1 逐字节解码 —— 与旧仓口径一致） */
function readAnsiField(buf, off, len) {
  let end = off;
  const limit = off + len;
  while (end < limit && buf[end] !== 0) end += 1;
  return buf.toString('latin1', off, end);
}

/** 读定长 UTF-16LE 字符串（遇 2 字节 NUL 截断） */
function readWideField(buf, off, len) {
  const limit = off + len;
  let end = off;
  for (let p = off; p + 1 < limit; p += 2) {
    if (buf[p] === 0 && buf[p + 1] === 0) {
      end = p;
      break;
    }
    end = p + 2;
  }
  if (end > limit) end = limit;
  return buf.toString('utf16le', off, end);
}

const readField = (unicode, buf, off, len) => (unicode ? readWideField(buf, off, len) : readAnsiField(buf, off, len));

/**
 * 把字符串写进定长字段：**只覆写字符串本身与其 1 个 NUL 终止符**，其余字节原样保留。
 * @returns {number} 下一个字段的偏移（恒等于 off + fieldLen；返回它只是让调用点更清楚）
 */
function writeField(buf, off, fieldLen, str, unicode) {
  const raw = unicode ? Buffer.from(str, 'utf16le') : Buffer.from(str, 'latin1');
  const max = fieldLen - (unicode ? 2 : 1); // 至少留一个终止符
  if (raw.length > max) throw new Error(`文件名太长（${raw.length} > ${max} 字节）：${str}`);
  raw.copy(buf, off);
  // 终止符：定长字段里本来就是 0，但改名后可能变短 ⇒ 显式补一个
  buf.fill(0, off + raw.length, off + raw.length + (unicode ? 2 : 1));
  return off + fieldLen;
}

/**
 * 解析索引/归档文件里的**段**（`S4SECTHDR` + LZSS 数据）。
 * @param {Buffer} buf
 * @param {number} pos 段起点
 */
export function readSection(buf, pos) {
  if (pos + SECT_HDR_SIZE > buf.length) throw new Error(`段头越界：pos=${pos}, 文件 ${buf.length} 字节`);
  const originalLength = buf.readUInt32LE(pos);
  const originalLength2 = buf.readUInt32LE(pos + 4);
  const length = buf.readUInt32LE(pos + 8);
  if (pos + SECT_HDR_SIZE + length > buf.length) {
    throw new Error(`压缩数据越界：声明 ${length} 字节，文件只剩 ${buf.length - pos - SECT_HDR_SIZE}`);
  }
  const packed = buf.subarray(pos + SECT_HDR_SIZE, pos + SECT_HDR_SIZE + length);
  const { data, size } = lzss.unpack(packed, length, originalLength);
  return { originalLength, originalLength2, length, packed, data, size };
}

/** 按签名判定盘上布局；认不出来就抛（"拿不到事实就抛"，不猜） */
export function detectLayout(buf) {
  for (const [name, l] of Object.entries(LAYOUTS)) {
    for (const sig of l.sigs) {
      const want = l.unicode
        ? Buffer.from(sig, 'utf16le')
        : Buffer.from(sig, 'latin1');
      if (buf.length >= want.length && buf.subarray(0, want.length).equals(want)) {
        const addon = sig.endsWith('AC');
        return { name, ...l, signature: sig, addon, sectionPos: addon ? l.addonPos : l.hdrSize };
      }
    }
  }
  throw new Error(`不是可识别的 ALF 索引文件（前 8 字节：${buf.subarray(0, 8).toString('hex')}）`);
}

/**
 * 读一个索引文件（`*.AAI` / `SYS?INI.BIN`）。
 *
 * **只读索引本身**：`*.ALF` 数据体是**另外的文件**（TOC 里按文件名指向），不去读它们 ——
 * 这样"看一眼目录"不需要 7.7 GB 的归档在场。
 * @param {string} file 索引文件路径
 */
export function readAlf(file) {
  return readAlfBuffer(fs.readFileSync(file), file);
}

/**
 * 同上，但吃内存里的字节。
 * @param {Buffer} buf
 * @param {string} [file] 只用于报错与 `baseDir` 推导
 */
export function readAlfBuffer(buf, file = '<memory>') {
  const layout = detectLayout(buf);
  const header = buf.subarray(0, layout.sectionPos);
  const sect = readSection(buf, layout.sectionPos);
  const toc = sect.data;

  const archiveCount = toc.readUInt32LE(0);
  const archives = [];
  let off = 4;
  for (let i = 0; i < archiveCount; i += 1) {
    archives.push({
      filename: readField(layout.unicode, toc, off, layout.arcEntry),
      raw: Buffer.from(toc.subarray(off, off + layout.arcEntry)),
      tocOffset: off,
    });
    off += layout.arcEntry;
  }
  const fileCount = toc.readUInt32LE(off);
  const fileHdrOffset = off;
  off += 4;

  const entries = [];
  for (let i = 0; i < fileCount; i += 1) {
    const name = readField(layout.unicode, toc, off, layout.filName);
    entries.push({
      filename: name,
      archiveIndex: toc.readUInt32LE(off + layout.filName),
      fileIndex: toc.readUInt32LE(off + layout.filName + 4),
      offset: toc.readUInt32LE(off + layout.filName + 8),
      length: toc.readUInt32LE(off + layout.filName + 12),
      raw: Buffer.from(toc.subarray(off, off + layout.filEntry)),
      tocOffset: off,
    });
    off += layout.filEntry;
  }
  const tocTrailing = Buffer.from(toc.subarray(off, sect.size));

  const baseDir = file === '<memory>' ? process.cwd() : path.dirname(path.resolve(file));
  return {
    file,
    buf,
    layout,
    header: Buffer.from(header),
    section: sect,
    toc,
    archiveCount,
    archives,
    fileCount,
    fileHdrOffset,
    entries,
    tocTrailing,
    /** 归档文件名的绝对路径（按索引所在目录解析 —— 与旧仓一致） */
    archivePath: (name) => path.join(baseDir, name),
    baseDir,
  };
}

/** 目录项按名字建索引（重名保留先出现的那个，并在 `duplicates` 里报出来） */
export function indexEntries(alf) {
  const map = new Map();
  const duplicates = [];
  for (const e of alf.entries) {
    if (map.has(e.filename)) duplicates.push(e.filename);
    else map.set(e.filename, e);
  }
  return { map, duplicates };
}

/**
 * 把目录区**重新拼回字节**。默认逐字节还原原始 TOC（含未初始化字段的原始内容）。
 * @param {object} alf `readAlf` 的返回值
 * @param {{entries?: object[], archives?: object[], fileCount?: number}} [patch] 可选的目录变更
 */
export function buildToc(alf, patch = {}) {
  const { layout } = alf;
  const archives = patch.archives ?? alf.archives;
  const entries = patch.entries ?? alf.entries;
  // ★ 长度**按新目录算**：加条目会让 TOC 变长，用旧的 section.size 分配会静默截断。
  const need = 4 + archives.length * layout.arcEntry + 4 + entries.length * layout.filEntry;
  const out = Buffer.alloc(Math.max(need, alf.toc.length));
  // ① 原始 TOC 全量拷进来（未初始化字段因此逐字节保留）
  alf.toc.copy(out, 0, 0, Math.min(alf.toc.length, out.length));

  out.writeUInt32LE(archives.length, 0);
  let off = 4;
  for (const a of archives) {
    a.raw.copy(out, off);
    if (typeof a.filename === 'string') writeField(out, off, layout.arcEntry, a.filename, layout.unicode);
    off += layout.arcEntry;
  }
  out.writeUInt32LE(entries.length, off);
  off += 4;
  for (const e of entries) {
    e.raw.copy(out, off);
    if (typeof e.filename === 'string') writeField(out, off, layout.filName, e.filename, layout.unicode);
    out.writeUInt32LE(e.archiveIndex >>> 0, off + layout.filName);
    out.writeUInt32LE(e.fileIndex >>> 0, off + layout.filName + 4);
    out.writeUInt32LE(e.offset >>> 0, off + layout.filName + 8);
    out.writeUInt32LE(e.length >>> 0, off + layout.filName + 12);
    off += layout.filEntry;
  }
  return out;
}

/**
 * ★ **索引与数据体是两个文件**（这一点最容易搞错）：
 *   · 索引（`*.AAI` / `SYS4INI.BIN`）= 头 + 目录区（LZSS 压缩），**没有载荷**；
 *   · 数据体（`*.ALF`）= 载荷字节的裸拼接，**没有头、没有目录区**。
 * 所以"重打包"要分两个出口写；`writeIndex` 对未改动的索引必得逐字节相同的文件。
 * @param {object} alf
 * @param {{patch?: object}} [opts]
 * @returns {Buffer} 索引文件字节
 */
export function writeIndex(alf, opts = {}) {
  const toc = buildToc(alf, opts.patch ?? {});
  const packed = lzss.pack(toc);
  const out = Buffer.alloc(alf.layout.sectionPos + SECT_HDR_SIZE + packed.length);
  alf.header.copy(out, 0);
  const sp = alf.layout.sectionPos;
  out.writeUInt32LE(toc.length, sp);
  out.writeUInt32LE(alf.section.originalLength2, sp + 4);
  out.writeUInt32LE(packed.length, sp + 8);
  packed.copy(out, sp + SECT_HDR_SIZE);
  return out;
}

/**
 * 把某个归档（`.ALF`）按目录重打包：条目 i 的载荷放在 `offset` 处。
 *
 * 两种情况都要对：
 *   · **未改动**：`offset` 恰好是顺序累加、条目也按 offset 升序排列 ⇒ 逐字节还原原归档；
 *   · **改动过**：调用方已经改好 `offset`/`length`（例如把某条指向新追加的数据），
 *     数据体必须真的按新 offset 摆放，否则索引与数据体会对不上。
 * ★ **不留空洞**是"逐字节还原"的前提：旧仓 `packdata.cpp` 那种"把改过的文件另写 DATA7.ALF、
 *   只挪 TOC 指针"的做法会让归档里留下不再被引用的字节 —— 那不是逐字节还原。
 * @param {object} alf
 * @param {number} archiveIndex 第几个归档（见 `alf.archives`）
 * @param {{entries?: object[], allowGaps?: boolean, padByte?: number}} [opts]
 * @returns {{data: Buffer, gaps: Array<{offset:number,length:number}>}}
 */
export function writeArchive(alf, archiveIndex, opts = {}) {
  const entries = (opts.entries ?? alf.entries).filter((e) => e.archiveIndex === archiveIndex && e.length > 0);
  const allowGaps = opts.allowGaps === true;
  const padByte = opts.padByte ?? 0;
  const sorted = [...entries].sort((a, b) => a.offset - b.offset);
  const end = sorted.reduce((n, e) => Math.max(n, e.offset + e.length), 0);
  const out = Buffer.alloc(end, padByte);
  const gaps = [];
  let cursor = 0;
  for (const e of sorted) {
    if (e.offset > cursor) gaps.push({ offset: cursor, length: e.offset - cursor });
    if (!e.payload) throw new Error(`条目 ${e.filename} 声明 ${e.length} 字节但没有载荷（先 loadPayloads）`);
    if (e.payload.length !== e.length) {
      throw new Error(`条目 ${e.filename} 载荷长度不符：声明 ${e.length}，实际 ${e.payload.length}`);
    }
    e.payload.copy(out, e.offset);
    cursor = Math.max(cursor, e.offset + e.length);
  }
  if (gaps.length && !allowGaps) {
    const detail = gaps.map((g) => `+${g.offset}(len ${g.length})`).join(', ');
    throw new Error(
      `归档里存在未被任何条目引用的字节（${gaps.length} 处：${detail}）⇒ 无法逐字节还原。` +
        '要么按 offset 升序重排条目并重算 offset（无空洞），要么显式传 allowGaps: true 接受补零。',
    );
  }
  return { data: out, gaps };
}

/**
 * 便捷：把某一项换成新载荷，并**重算该归档内所有条目的 offset**（顺序 = 原 offset 升序）。
 *
 * 这是"改一个文件再打包"唯一的正确姿势：offset 是归档内的绝对位置，改长度必影响后续条目。
 * @param {object} alf
 * @param {Map<string, Buffer>|Record<string, Buffer>} replacements 条目名 → 新载荷
 * @returns {{patched: string[], changedArchives: number[]}}
 */
export function applyReplacements(alf, replacements) {
  const map = replacements instanceof Map ? replacements : new Map(Object.entries(replacements));
  const patched = [];
  for (const [name, buf] of map) {
    const e = alf.entries.find((x) => x.filename === name);
    if (!e) throw new Error(`目录里没有这个条目：${name}`);
    e.payload = Buffer.from(buf);
    e.length = e.payload.length;
    patched.push(name);
  }
  const changedArchives = [...new Set(alf.entries.filter((e) => map.has(e.filename)).map((e) => e.archiveIndex))];
  for (const ai of changedArchives) {
    let cursor = 0;
    for (const e of alf.entries.filter((x) => x.archiveIndex === ai).sort((a, b) => a.offset - b.offset)) {
      e.offset = cursor;
      cursor += e.length;
    }
  }
  return { patched, changedArchives };
}

/** 只改归档文件名（TOC 里的那一条），其余原样 —— `rename` 之外没有任何推断 */
export function renameArchive(alf, index, newName) {
  const a = alf.archives[index];
  if (!a) throw new Error(`没有第 ${index} 个归档`);
  const raw = Buffer.from(a.raw);
  writeField(raw, 0, alf.layout.arcEntry, newName, alf.layout.unicode);
  return { ...a, filename: newName, raw };
}

/**
 * 从各 `*.ALF` 里把载荷读进内存（每组一个归档只开一次）。
 *
 * ★ **不出仓也能用**：归档缺席时**不抛**，只在 `missingArchives` 里报出来 —— 调用方自己决定
 *   "缺席即跳过"还是"缺席即失败"。默认**按需读**：只读 `wanted` 里的名字。
 * @param {object} alf
 * @param {{wanted?: string[]|Set<string>, onProgress?: (done: number, total: number) => void}} [opts]
 * @returns {{loaded: number, missingArchives: string[], missingEntries: string[]}}
 */
export function loadPayloads(alf, opts = {}) {
  const wanted = opts.wanted ? new Set(opts.wanted) : null;
  const targets = alf.entries.filter((e) => e.length > 0 && (!wanted || wanted.has(e.filename)));
  const byArchive = new Map();
  for (const e of targets) {
    if (!byArchive.has(e.archiveIndex)) byArchive.set(e.archiveIndex, []);
    byArchive.get(e.archiveIndex).push(e);
  }
  const missingArchives = [];
  const missingEntries = [];
  let loaded = 0;
  for (const [arcIndex, list] of byArchive) {
    const info = alf.archives[arcIndex];
    const abs = info ? alf.archivePath(info.filename) : null;
    if (!abs || !fs.existsSync(abs)) {
      missingArchives.push(info ? info.filename : `#${arcIndex}`);
      for (const e of list) missingEntries.push(e.filename);
      continue;
    }
    const size = fs.statSync(abs).size;
    const fd = fs.openSync(abs, 'r');
    try {
      for (const e of list) {
        if (e.offset + e.length > size) {
          missingEntries.push(e.filename);
          continue;
        }
        const buf = Buffer.alloc(e.length);
        let read = 0;
        while (read < e.length) {
          const n = fs.readSync(fd, buf, read, e.length - read, e.offset + read);
          if (n <= 0) break;
          read += n;
        }
        if (read !== e.length) {
          missingEntries.push(e.filename);
          continue;
        }
        e.payload = buf;
        loaded += 1;
        if (opts.onProgress) opts.onProgress(loaded, targets.length);
      }
    } finally {
      fs.closeSync(fd);
    }
  }
  return { loaded, missingArchives, missingEntries };
}

/** 只算载荷的"声明总量"（不解包任何归档）—— 给"这个归档有多大"这类问句用 */
export function declaredPayloadBytes(alf) {
  return alf.entries.reduce((n, e) => n + (e.length >>> 0), 0);
}

/**
 * 解包到目录：`<outDir>/<归档名前缀>/<条目名>`（与旧仓 `unpack_alf.mjs` 的落点一致）。
 * @returns {{written: number, skipped: string[]}}
 */
export function unpackTo(alf, outDir) {
  fs.mkdirSync(outDir, { recursive: true });
  const written = [];
  const skipped = [];
  const { map: arcByName } = indexEntries(alf);
  void arcByName;
  for (let i = 0; i < alf.archives.length; i += 1) {
    const list = alf.entries.filter((e) => e.archiveIndex === i && e.payload && e.length > 0);
    if (list.length === 0) continue;
    const prefix = alf.archives[i].filename.replace(/\.[^.]*$/, '');
    const dir = path.join(outDir, prefix);
    fs.mkdirSync(dir, { recursive: true });
    for (const e of list) {
      // ★ 条目名是**外部数据**：不给它越出 outDir 的机会
      const safe = path.basename(e.filename);
      if (safe !== e.filename) {
        skipped.push(e.filename);
        continue;
      }
      fs.writeFileSync(path.join(dir, safe), e.payload);
      written.push(`${prefix}/${safe}`);
    }
  }
  return { written, skipped };
}
