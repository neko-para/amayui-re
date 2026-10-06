/**
 * tools/lib/fonts.mjs —— **领域模型**：cnjp 分发字体的构建与校验（纯 Node，不依赖 fontTools）。
 *
 * ## 这个字体是什么
 * 引擎用 **cp932 码位**读文本；译文里写不进 cp932 的简体字，由 `data/translations/subs-cn-jp.json`
 * （「简→日写法」字典）换成一个**日文写法**存进 BIN。于是字体必须把那个**日文码位**画成**简体字形** ——
 * 这就是 cnjp：在 Sarasa Gothic SC 基底上**只重写 cmap**，再加**唯一族名**与 **Shift-JIS(932) 码页声明**。
 *
 * ## 三步（全部由"成品 vs 基底"的逐字节差实测反推，落点见 docs 与需求树）
 * ① `cmap`：反转字典（日写法→简体），对**除平台 1（Mac）外**的每个子表做
 *    `cmap[日写法] = cmap[简体]`（**原地改** ⇒ 允许链式，顺序＝反转字典首次出现序）；其余 18 张表**原样搬运**；
 * ② `name`：id1/3/4/16 → 族名、id6 → PostScript 名（**全语言记录一致**）；
 * ③ `OS/2`：`ulCodePageRange1/2` 按 **fontTools `recalcCodePageRanges` 的算法**（FontForge 直译）重算。
 *
 * ★ **判据是"除 `head.modified` 外逐字节相同"**：构建会写时间戳，它连带改掉 `head` 的表校验和与
 *   文件级 `checkSumAdjustment`（实测共 9 字节）。把这三处归一化后与可信产物比 ⇒ 0 差异。
 * ★ 序列化细节刻意与 fontTools 对齐（cmap f4 的 `splitRange` 分段与阈值、编译后相同的子表**共享偏移**、
 *   name 的**字符串去重**、表按 4 字节对齐、`head.checkSumAdjustment`），否则体积与字节都会漂。
 */
import { createHash } from 'node:crypto';

// ─────────────────────────────────────────────────────────── sfnt 容器

/** 读表目录 */
export function parseSfnt(buf) {
  if (buf.length < 12) throw new Error('太短，不是 sfnt/TrueType 字体');
  const numTables = buf.readUInt16BE(4);
  const tables = [];
  for (let i = 0; i < numTables; i += 1) {
    const o = 12 + i * 16;
    tables.push({
      tag: buf.toString('latin1', o, o + 4),
      checkSum: buf.readUInt32BE(o + 4),
      offset: buf.readUInt32BE(o + 8),
      length: buf.readUInt32BE(o + 12),
    });
  }
  const byTag = new Map(tables.map((t) => [t.tag, t]));
  return {
    sfntVersion: buf.readUInt32BE(0),
    tables,
    byTag,
    table: (tag) => {
      const t = byTag.get(tag);
      if (!t) throw new Error(`字体里没有 ${tag} 表`);
      return buf.subarray(t.offset, t.offset + t.length);
    },
  };
}

/** 表级校验和（4 字节对齐、超出部分补 0） */
export function tableChecksum(buf) {
  let sum = 0;
  for (let i = 0; i < buf.length; i += 4) {
    const b0 = buf[i] ?? 0;
    const b1 = buf[i + 1] ?? 0;
    const b2 = buf[i + 2] ?? 0;
    const b3 = buf[i + 3] ?? 0;
    sum = (sum + (((b0 << 24) >>> 0) + (b1 << 16) + (b2 << 8) + b3)) >>> 0;
  }
  return sum >>> 0;
}

/**
 * fontTools `TTFTableOrder`：字体**物理落盘顺序**的偏好表。
 * ★ 目录顺序与物理顺序是两件事：fontTools 写盘时按这个表排（不在这表里的按 tag 排序垫后），
 *   而**目录记录**一律按 tag 排序 —— 两处都要照做，否则载荷全同、整文件还是逐字节不同。
 */
export const TTF_TABLE_ORDER = [
  'head', 'hhea', 'maxp', 'OS/2', 'hmtx', 'LTSH', 'VDMX', 'hdmx', 'cmap', 'fpgm',
  'prep', 'cvt ', 'loca', 'glyf', 'kern', 'name', 'post', 'gasp', 'PCLT',
];

/** fontTools `sortedTagList`：偏好表里有的先按表序排，其余按 tag 排序垫后（DSIG 永远最后） */
export function sortedTagList(tags) {
  const sorted = [...tags].sort();
  const dsig = sorted.includes('DSIG');
  const body = sorted.filter((t) => t !== 'DSIG');
  const ordered = TTF_TABLE_ORDER.filter((t) => body.includes(t));
  const rest = body.filter((t) => !TTF_TABLE_ORDER.includes(t));
  return dsig ? [...ordered, ...rest, 'DSIG'] : [...ordered, ...rest];
}

/**
 * 组装 sfnt：**物理顺序按 `sortedTagList`、目录记录按 tag 排序**（都与 fontTools 一致）；
 * 每张表 4 字节对齐；重算表校验和与 `head.checkSumAdjustment` 最后写。
 * @param {{sfntVersion:number, tables:Array<{tag:string, buf:Buffer}>}} font
 */
export function compileSfnt({ sfntVersion, tables }) {
  const byTag = new Map(tables.map((t) => [t.tag, t]));
  const order = sortedTagList(tables.map((t) => t.tag));
  const num = order.length;
  const maxPow = num > 0 ? 2 ** Math.floor(Math.log2(num)) : 0;
  let off = 12 + num * 16;
  const placed = new Map();
  for (const tag of order) {
    const buf = byTag.get(tag).buf;
    const padded = Buffer.concat([buf, Buffer.alloc((4 - (buf.length % 4)) % 4)]);
    placed.set(tag, { buf, padded, offset: off });
    off += padded.length;
  }
  const out = Buffer.alloc(off);
  out.writeUInt32BE(sfntVersion >>> 0, 0);
  out.writeUInt16BE(num, 4);
  out.writeUInt16BE(maxPow * 16, 6);
  out.writeUInt16BE(Math.floor(Math.log2(num)) || 0, 8);
  out.writeUInt16BE(num * 16 - maxPow * 16, 10);
  const sortedTags = [...order].sort();
  sortedTags.forEach((tag, i) => {
    const r = placed.get(tag);
    const o = 12 + i * 16;
    out.write(tag, o, 'latin1');
    r.padded.copy(out, r.offset);
    out.writeUInt32BE(tableChecksum(r.padded), o + 4);
    out.writeUInt32BE(r.offset, o + 8);
    out.writeUInt32BE(r.buf.length, o + 12);
  });
  const head = placed.get('head');
  if (head) {
    const adjOff = head.offset + 8;
    out.writeUInt32BE(0, adjOff);
    out.writeUInt32BE((0xb1b0afba - tableChecksum(out)) >>> 0, adjOff);
  }
  return out;
}

// ─────────────────────────────────────────────────────────── cmap

const CmapFormat = { 4: 'format4', 12: 'format12', 14: 'format14' };

/**
 * 子表声明的字节长度（用于**原样带过**未解析的格式）。
 * ★ 不能"从 offset 抄到表尾"：基底里 f14 就排在**最前**，那样会把 f4/f12 又抄一遍（实测多 63 KB）。
 */
export function cmapSubtableLength(blob, off, format) {
  const u32At2 = [8, 10, 12, 13, 14].includes(format);
  const len = u32At2 ? blob.readUInt32BE(off + 2) : format === 2 ? blob.readUInt16BE(off + 2) : blob.readUInt16BE(off + 2);
  if (!Number.isFinite(len) || len < 4 || off + len > blob.length) return blob.length - off; // 拿不到就退化成到表尾
  return len;
}

/** 读 cmap 的全部子表（只解析 f4/f12；f14 之类的原样保留） */
export function readCmap(buf, rec) {
  const blob = buf.subarray(rec.offset, rec.offset + rec.length);
  const n = blob.readUInt16BE(2);
  const subs = [];
  for (let i = 0; i < n; i += 1) {
    const o = 4 + i * 8;
    const platformID = blob.readUInt16BE(o);
    const platEncID = blob.readUInt16BE(o + 2);
    const off = blob.readUInt32BE(o + 4);
    const format = blob.readUInt16BE(off);
    const sub = { platformID, platEncID, format, raw: null, cmap: new Map(), language: 0 };
    if (CmapFormat[format] === 'format4') {
      sub.language = blob.readUInt16BE(off + 4);
      const segX2 = blob.readUInt16BE(off + 6);
      const segCount = segX2 / 2;
      const endO = off + 14;
      const startO = endO + segX2 + 2;
      const deltaO = startO + segX2;
      const rangeO = deltaO + segX2;
      for (let s = 0; s < segCount; s += 1) {
        const end = blob.readUInt16BE(endO + s * 2);
        const start = blob.readUInt16BE(startO + s * 2);
        const delta = blob.readInt16BE(deltaO + s * 2);
        const range = blob.readUInt16BE(rangeO + s * 2);
        for (let c = start; c <= end && c !== 0xffff; c += 1) {
          let gid;
          if (range === 0) gid = (c + delta) & 0xffff;
          else {
            gid = blob.readUInt16BE(rangeO + s * 2 + range + (c - start) * 2);
            if (gid !== 0) gid = (gid + delta) & 0xffff;
          }
          if (gid !== 0) sub.cmap.set(c, gid);
        }
      }
    } else if (CmapFormat[format] === 'format12') {
      sub.language = blob.readUInt32BE(off + 8);
      const groups = blob.readUInt32BE(off + 12);
      for (let g = 0; g < groups; g += 1) {
        const go = off + 16 + g * 12;
        const s0 = blob.readUInt32BE(go);
        const e0 = blob.readUInt32BE(go + 4);
        const g0 = blob.readUInt32BE(go + 8);
        for (let c = s0; c <= e0; c += 1) sub.cmap.set(c, g0 + (c - s0));
      }
    } else {
      // f14 等未解析的子表：**按它自己声明的长度**原样带过
      sub.raw = Buffer.from(blob.subarray(off, off + cmapSubtableLength(blob, off, format)));
    }
    subs.push(sub);
  }
  return subs;
}

const maxPowerOfTwo = (n) => 2 ** Math.floor(Math.log2(n));
/** fontTools `getSearchRange(n, itemSize)` */
export function getSearchRange(n, itemSize) {
  const exponent = n > 0 ? Math.floor(Math.log2(n)) : 0;
  const searchRange = 2 ** exponent * itemSize;
  return [searchRange, exponent, Math.max(0, n * itemSize - searchRange)];
}

/**
 * fontTools `splitRange` 的逐字移植：把一个"码位连续"的段按**字形 id 是否连续**再细分，
 * 只保留"值得拆"的子段（内部段 >8 个码位、贴边段 >4 个才拆 —— 拆一段多花 8 字节、省 2 字节/字）。
 * @returns {{start:number[], end:number[]}} 返回的是**首个之后**的子段（调用方已有第一个）
 */
export function splitRange(startCode, endCode, cmap) {
  if (startCode === endCode) return { start: [], end: [endCode] };
  let lastID = cmap.get(startCode);
  let lastCode = startCode;
  let inOrder = null;
  let orderedBegin = null;
  let subRanges = [];
  for (let code = startCode + 1; code <= endCode; code += 1) {
    const glyphID = cmap.get(code);
    if (glyphID - 1 === lastID) {
      if (inOrder === null || !inOrder) {
        inOrder = 1;
        orderedBegin = lastCode;
      }
    } else if (inOrder) {
      inOrder = 0;
      subRanges.push([orderedBegin, lastCode]);
      orderedBegin = null;
    }
    lastID = glyphID;
    lastCode = code;
  }
  if (inOrder) subRanges.push([orderedBegin, lastCode]);
  subRanges = subRanges.filter(([b, e]) => {
    if (b === startCode && e === endCode) return false;
    const threshold = b === startCode || e === endCode ? 4 : 8;
    return e - b + 1 > threshold;
  });
  if (!subRanges.length) return { start: [], end: [endCode] };
  if (subRanges[0][0] !== startCode) subRanges.unshift([startCode, subRanges[0][0] - 1]);
  if (subRanges[subRanges.length - 1][1] !== endCode) {
    subRanges.push([subRanges[subRanges.length - 1][1] + 1, endCode]);
  }
  for (let i = 1; i < subRanges.length; ) {
    if (subRanges[i - 1][1] + 1 !== subRanges[i][0]) {
      subRanges.splice(i, 0, [subRanges[i - 1][1] + 1, subRanges[i][0] - 1]);
      i += 1;
    }
    i += 1;
  }
  const start = subRanges.map(([b]) => b);
  const end = subRanges.map(([, e]) => e);
  start.shift();
  return { start, end };
}

/** 编译 format 4 子表（fontTools `cmap_format_4.compile` 的逐字移植） */
export function compileFormat4(sub) {
  const charCodes = [...sub.cmap.keys()].sort((a, b) => a - b);
  let startCode = [];
  let endCode = [];
  if (!charCodes.length) {
    startCode = [0xffff];
    endCode = [0xffff];
  } else {
    startCode = [charCodes[0]];
    let lastCode = charCodes[0];
    for (const charCode of charCodes.slice(1)) {
      if (charCode === lastCode + 1) { lastCode = charCode; continue; }
      const r = splitRange(startCode[startCode.length - 1], lastCode, sub.cmap);
      startCode.push(...r.start);
      endCode.push(...r.end);
      startCode.push(charCode);
      lastCode = charCode;
    }
    const r = splitRange(startCode[startCode.length - 1], lastCode, sub.cmap);
    startCode.push(...r.start);
    endCode.push(...r.end);
    startCode.push(0xffff);
    endCode.push(0xffff);
  }
  const idDelta = [];
  const idRangeOffset = [];
  const glyphIndexArray = [];
  for (let i = 0; i < endCode.length - 1; i += 1) {
    const indices = [];
    for (let code = startCode[i]; code <= endCode[i]; code += 1) indices.push(sub.cmap.get(code));
    const consecutive = indices.every((g, k) => g === indices[0] + k);
    if (consecutive) {
      idDelta.push(((indices[0] - startCode[i]) % 0x10000 + 0x10000) % 0x10000);
      idRangeOffset.push(0);
    } else {
      idDelta.push(0);
      idRangeOffset.push(2 * (endCode.length + glyphIndexArray.length - i));
      glyphIndexArray.push(...indices);
    }
  }
  idDelta.push(1); // 0xffff + 1 == 0 ⇒ 落到 .notdef
  idRangeOffset.push(0);

  const segCount = endCode.length;
  const dataLen = (endCode.length + 1 + startCode.length + idDelta.length + idRangeOffset.length + glyphIndexArray.length) * 2;
  const len = 14 + dataLen;
  const out = Buffer.alloc(len);
  const [searchRange, entrySelector, rangeShift] = getSearchRange(segCount, 2);
  out.writeUInt16BE(4, 0);
  out.writeUInt16BE(len, 2);
  out.writeUInt16BE(sub.language & 0xffff, 4);
  out.writeUInt16BE(segCount * 2, 6);
  out.writeUInt16BE(searchRange, 8);
  out.writeUInt16BE(entrySelector, 10);
  out.writeUInt16BE(rangeShift, 12);
  let o = 14;
  for (const v of endCode) { out.writeUInt16BE(v, o); o += 2; }
  out.writeUInt16BE(0, o); o += 2; // reservedPad
  for (const v of startCode) { out.writeUInt16BE(v, o); o += 2; }
  for (const v of idDelta) { out.writeUInt16BE(v & 0xffff, o); o += 2; }
  for (const v of idRangeOffset) { out.writeUInt16BE(v & 0xffff, o); o += 2; }
  for (const v of glyphIndexArray) { out.writeUInt16BE(v & 0xffff, o); o += 2; }
  return out;
}

/** 编译 format 12 子表（fontTools `cmap_format_12.compile` 的逐字移植） */
export function compileFormat12(sub) {
  const charCodes = [...sub.cmap.keys()].sort((a, b) => a - b);
  const groups = [];
  let startCharCode = charCodes[0];
  let startGlyphID = sub.cmap.get(startCharCode);
  let lastGlyphID = startGlyphID - 1;
  let lastCharCode = startCharCode - 1;
  for (const charCode of charCodes) {
    const glyphID = sub.cmap.get(charCode);
    // fontTools `cmap_format_12._IsInSameRun`：**两者都必须恰好 +1**（用差值相等会在空洞处并组，
    // 凭空造出中间码位 —— 实测就是 U+1D51 那个多出来的条目）
    if (!(glyphID === lastGlyphID + 1 && charCode === lastCharCode + 1)) {
      groups.push([startCharCode, lastCharCode, startGlyphID]);
      startCharCode = charCode;
      startGlyphID = glyphID;
    }
    lastGlyphID = glyphID;
    lastCharCode = charCode;
  }
  groups.push([startCharCode, lastCharCode, startGlyphID]);
  const len = 16 + groups.length * 12;
  const out = Buffer.alloc(len);
  out.writeUInt16BE(12, 0);
  out.writeUInt16BE(0, 2);
  out.writeUInt32BE(len, 4);
  out.writeUInt32BE(sub.language >>> 0, 8);
  out.writeUInt32BE(groups.length, 12);
  groups.forEach(([s, e, g], i) => {
    const o = 16 + i * 12;
    out.writeUInt32BE(s, o);
    out.writeUInt32BE(e, o + 4);
    out.writeUInt32BE(g, o + 8);
  });
  return out;
}

/** 编译整张 cmap 表：**编译后字节相同的子表共享同一偏移**（fontTools 的行为） */
export function compileCmap(subs) {
  const ordered = [...subs].sort((a, b) => a.platformID - b.platformID || a.platEncID - b.platEncID);
  const done = new Map(); // chunk(hex) → offset
  const chunks = [];
  let total = 4 + ordered.length * 8;
  const records = [];
  for (const sub of ordered) {
    const chunk = sub.raw ? sub.raw : sub.format === 4 ? compileFormat4(sub) : compileFormat12(sub);
    const key = createHash('sha1').update(chunk).digest('hex');
    let offset = done.get(key);
    if (offset === undefined) {
      offset = total;
      done.set(key, offset);
      chunks.push(chunk);
      total += chunk.length;
    }
    records.push({ platformID: sub.platformID, platEncID: sub.platEncID, offset });
  }
  const out = Buffer.alloc(total);
  out.writeUInt16BE(0, 0);
  out.writeUInt16BE(ordered.length, 2);
  records.forEach((r, i) => {
    const o = 4 + i * 8;
    out.writeUInt16BE(r.platformID, o);
    out.writeUInt16BE(r.platEncID, o + 2);
    out.writeUInt32BE(r.offset, o + 4);
  });
  let at = 4 + ordered.length * 8;
  for (const c of chunks) { c.copy(out, at); at += c.length; }
  return out;
}

/**
 * 按字典把 cmap 重映射（旧 `font_CN_JP.py` 的语义）：反转字典后**原地**写
 * `cmap[日写法] = cmap[简体]`，跳过平台 1（Mac）。原地改 ⇒ 允许链式。
 * @returns {number} 实际写入次数
 */
export function remapCmap(subs, dict) {
  const inverted = new Map();
  for (const [cn, jp] of Object.entries(dict)) if (!inverted.has(jp)) inverted.set(jp, cn);
  let applied = 0;
  for (const sub of subs) {
    if (sub.platformID === 1 || !sub.cmap.size) continue;
    for (const [jp, cn] of inverted) {
      if (jp === cn) continue;
      const gid = sub.cmap.get(cn.codePointAt(0));
      if (gid === undefined) continue;
      sub.cmap.set(jp.codePointAt(0), gid);
      applied += 1;
    }
  }
  return applied;
}

// ─────────────────────────────────────────────────────────── name

/** 读 name 记录（原始字节 + 标识） */
export function readNames(buf, rec) {
  const t = buf.subarray(rec.offset, rec.offset + rec.length);
  const count = t.readUInt16BE(2);
  const strOff = t.readUInt16BE(4);
  const out = [];
  for (let i = 0; i < count; i += 1) {
    const o = 6 + i * 12;
    const platformID = t.readUInt16BE(o);
    const platEncID = t.readUInt16BE(o + 2);
    const langID = t.readUInt16BE(o + 4);
    const nameID = t.readUInt16BE(o + 6);
    const len = t.readUInt16BE(o + 8);
    const off = t.readUInt16BE(o + 10);
    out.push({ platformID, platEncID, langID, nameID, raw: Buffer.from(t.subarray(strOff + off, strOff + off + len)) });
  }
  return out;
}

export const nameText = (r) => (r.platformID === 3 || r.platformID === 0 ? Buffer.from(r.raw).swap16().toString('utf16le') : r.raw.toString('latin1'));
const encodeName = (s, platformID) => (platformID === 3 || platformID === 0 ? Buffer.from(s, 'utf16le').swap16() : Buffer.from(s, 'latin1'));

/**
 * 改族名 —— ★ 规则由"成品 vs 基底"逐条实测（Regular 与 Bold 都验过）：
 *   id1/id16（family / typographic family）→ 族名
 *   id3/id4（unique ID / full name）      → 族名 + 子族名，**但子族名是 `Regular` 时省略**
 *   id6（PostScript name）                → PS 名 + `-子族名`，**同样在 `Regular` 时省略**
 *   id2/id17（subfamily）                 → 不动
 * ⇒ Regular 得到 `Amayui CN` / `Amayui-CN`，Bold 得到 `Amayui CN Bold` / `Amayui-CN-Bold`。
 */
export function renameFont(recs, { family, psName, ids = [1, 3, 4, 16], psId = 6 }) {
  const subOf = new Map();
  for (const r of recs) {
    if (r.nameID === 2 || r.nameID === 17) subOf.set(`${r.platformID}/${r.platEncID}/${r.langID}`, nameText(r));
  }
  const isRegular = (r) => (subOf.get(`${r.platformID}/${r.platEncID}/${r.langID}`) ?? 'Regular') === 'Regular';
  let changed = 0;
  for (const r of recs) {
    let next = null;
    if (r.nameID === 1 || r.nameID === 16) next = family;
    else if (r.nameID === 3 || r.nameID === 4) next = isRegular(r) ? family : `${family} ${subOf.get(`${r.platformID}/${r.platEncID}/${r.langID}`)}`;
    else if (r.nameID === psId) next = isRegular(r) ? psName : `${psName}-${subOf.get(`${r.platformID}/${r.platEncID}/${r.langID}`)}`;
    if (next === null) continue;
    if (nameText(r) === next) continue;
    r.raw = encodeName(next, r.platformID);
    changed += 1;
  }
  return changed;
}

/** 编译 name 表（fontTools 口径：按 (platformID, platEncID, langID, nameID) 排序 + **相同字符串只存一份**） */
export function compileName(recs) {
  const ordered = [...recs].sort(
    (a, b) => a.platformID - b.platformID || a.platEncID - b.platEncID || a.langID - b.langID || a.nameID - b.nameID,
  );
  const n = ordered.length;
  const stringOffset = 6 + n * 12;
  const strings = [];
  let stringLen = 0;
  const seen = new Map();
  const records = ordered.map((r) => {
    const key = r.raw.toString('hex');
    let at = seen.get(key);
    if (at === undefined) {
      at = stringLen;
      seen.set(key, at);
      strings.push(r.raw);
      stringLen += r.raw.length;
    }
    return { r, at };
  });
  const out = Buffer.alloc(stringOffset + stringLen);
  out.writeUInt16BE(0, 0);
  out.writeUInt16BE(n, 2);
  out.writeUInt16BE(stringOffset, 4);
  records.forEach(({ r, at }, i) => {
    const o = 6 + i * 12;
    out.writeUInt16BE(r.platformID, o);
    out.writeUInt16BE(r.platEncID, o + 2);
    out.writeUInt16BE(r.langID, o + 4);
    out.writeUInt16BE(r.nameID, o + 6);
    out.writeUInt16BE(r.raw.length, o + 8);
    out.writeUInt16BE(at, o + 10);
  });
  let at = stringOffset;
  for (const s of strings) { s.copy(out, at); at += s.length; }
  return out;
}

// ─────────────────────────────────────────────────────────── OS/2 码页

/**
 * `ulCodePageRange` 位集合的重算：**fontTools `calcCodePageRanges` 的逐字移植**
 * （它自己又是 FontForge `tottf.c` 的直译）。判据是"探针字符在不在 cmap 里"+ `hasAscii` / `hasLineart`。
 * @param {Iterable<number>} unicodes
 * @returns {Set<number>} 位号（0–63；0–31 = range1，32–63 = range2）
 */
export function calcCodePageRanges(unicodes) {
  const set = unicodes instanceof Set ? unicodes : new Set(unicodes);
  const has = (ch) => set.has(ch.codePointAt(0));
  const bits = new Set();
  const hasAscii = [...Array(0x7e - 0x20 + 1).keys()].every((k) => set.has(0x20 + k));
  const hasLineart = has('┤');
  for (const uni of set) {
    if (uni === 0x00de && hasAscii) bits.add(0); // Latin 1 (Þ)
    else if (uni === 0x013d && hasAscii) { bits.add(1); if (hasLineart) bits.add(58); } // Ľ
    else if (uni === 0x0411) { // Б
      bits.add(2);
      if (has('Ѕ') && hasLineart) bits.add(57);
      if (has('╜') && hasLineart) bits.add(49);
    } else if (uni === 0x0386) { // Ά
      bits.add(3);
      if (hasLineart && has('½')) bits.add(48);
      if (hasLineart && has('√')) bits.add(60);
    } else if (uni === 0x0130 && hasAscii) { bits.add(4); if (hasLineart) bits.add(56); } // İ
    else if (uni === 0x05d0) { bits.add(5); if (hasLineart && has('√')) bits.add(53); } // א
    else if (uni === 0x0631) { // ر
      bits.add(6);
      if (has('√')) bits.add(51);
      if (hasLineart) bits.add(61);
    } else if (uni === 0x0157 && hasAscii) { bits.add(7); if (hasLineart) bits.add(59); } // ŗ
    else if (uni === 0x20ab && hasAscii) bits.add(8); // ₫
    else if (uni === 0x0e45) bits.add(16); // ๅ
    else if (uni === 0x30a8) bits.add(17); // エ ⇒ JIS/Japan (932)
    else if (uni === 0x3105) bits.add(18); // ㄅ
    else if (uni === 0x3131) bits.add(19); // ㄱ
    else if (uni === 0x592e) bits.add(20); // 央
    else if (uni === 0xacf4) bits.add(21); // 곴
    else if (uni === 0x2665 && hasAscii) bits.add(30); // ♥
    else if (uni === 0x00fe && hasAscii && hasLineart) bits.add(54); // þ
    else if (uni === 0x255a && hasAscii) { bits.add(62); bits.add(63); } // ╚
    else if (hasAscii && hasLineart && has('√')) {
      if (uni === 0x00c5) bits.add(50); // Å
      else if (uni === 0x00e9) bits.add(52); // é
      else if (uni === 0x00f5) bits.add(55); // õ
    }
  }
  if (hasAscii && has('‰') && has('∑')) bits.add(29);
  return bits;
}

/** 把位集合折成 OS/2 的两个 32 位范围值 */
export function codePageRangeValues(bits) {
  let r1 = 0;
  let r2 = 0;
  for (const b of bits) {
    if (b < 32) r1 = (r1 | (1 << b)) >>> 0;
    else r2 = (r2 | (1 << (b - 32))) >>> 0;
  }
  return { range1: r1, range2: r2 };
}

/** 从 cmap 的 **Unicode 子表**（平台 0，或平台 3/编码 1、10）收集码位，重算并写入 OS/2 */
export function recalcCodePageRanges(os2, subs) {
  const unicodes = new Set();
  for (const s of subs) {
    const isUnicode = s.platformID === 0 || (s.platformID === 3 && (s.platEncID === 1 || s.platEncID === 10));
    if (!isUnicode) continue;
    for (const c of s.cmap.keys()) unicodes.add(c);
  }
  const { range1, range2 } = codePageRangeValues(calcCodePageRanges(unicodes));
  const out = Buffer.from(os2);
  const version = out.readUInt16BE(0);
  if (version >= 1 && out.length >= 86) {
    out.writeUInt32BE(range1, 78);
    out.writeUInt32BE(range2, 82);
  }
  return { os2: out, range1, range2, unicodes: unicodes.size };
}

// ─────────────────────────────────────────────────────────── 构建 / 校验

export const DEFAULT_BASE = ['corpus', 'assets', 'fonts', 'SarasaGothicSC', 'SarasaGothicSC-Regular.ttf'];
export const DEFAULT_BOLD_BASE = ['corpus', 'assets', 'fonts', 'SarasaGothicSC', 'SarasaGothicSC-Bold.ttf'];
export const DEFAULT_REF = ['corpus', 'assets', 'fonts', 'Amayui-CN_cnjp.ttf'];
export const FAMILY = 'Amayui CN';
export const PS_NAME = 'Amayui-CN';

/**
 * 从基底 + 字典构建 cnjp 字体（纯内存）。
 * @returns {{buf:Buffer, stats:object}}
 */
export function buildCnjp({ baseBuf, dict, family = FAMILY, psName = PS_NAME }) {
  const font = parseSfnt(baseBuf);
  const cmapRec = font.byTag.get('cmap');
  const nameRec = font.byTag.get('name');
  const os2Rec = font.byTag.get('OS/2');
  if (!cmapRec || !nameRec || !os2Rec) throw new Error('基底缺 cmap / name / OS/2 表');

  const subs = readCmap(baseBuf, cmapRec);
  const applied = remapCmap(subs, dict);
  const cmapBuf = compileCmap(subs);

  const names = readNames(baseBuf, nameRec);
  const renamed = renameFont(names, { family, psName });
  const nameBuf = compileName(names);

  const os2 = recalcCodePageRanges(baseBuf.subarray(os2Rec.offset, os2Rec.offset + os2Rec.length), subs);

  const tables = font.tables.map((t) => {
    if (t.tag === 'cmap') return { tag: 'cmap', buf: cmapBuf };
    if (t.tag === 'name') return { tag: 'name', buf: nameBuf };
    if (t.tag === 'OS/2') return { tag: 'OS/2', buf: os2.os2 };
    return { tag: t.tag, buf: Buffer.from(baseBuf.subarray(t.offset, t.offset + t.length)) };
  });
  const buf = compileSfnt({ sfntVersion: font.sfntVersion, tables });
  return {
    buf,
    stats: {
      mappings: applied,
      renamed,
      codePageRange1: os2.range1,
      codePageRange2: os2.range2,
      unicodeCodepoints: os2.unicodes,
      tables: tables.length,
      bytes: buf.length,
    },
  };
}

/** 归一化：`head.modified`（时间戳）与由它派生的两处校验和 —— 只有这三处允许不同 */
export function normalizeHead(buf) {
  const out = Buffer.from(buf);
  const font = parseSfnt(out);
  const head = font.byTag.get('head');
  if (!head) return out;
  const adjOff = head.offset + 8;
  out.writeUInt32BE(0, adjOff); // checkSumAdjustment
  out.writeUInt32BE(0, adjOff + 25); // modified 的低 3 字节（28+5 → 33）
  // 归一化后重算 head 的表校验和写回目录（否则目录里那一格会残留旧值）
  const headBuf = out.subarray(head.offset, head.offset + head.length);
  const numTables = out.readUInt16BE(4);
  for (let i = 0; i < numTables; i += 1) {
    const o = 12 + i * 16;
    if (out.toString('latin1', o, o + 4) === 'head') out.writeUInt32BE(tableChecksum(headBuf), o + 4);
  }
  return out;
}

/** 判据：归一化后逐字节相同（缺件 / 长度不符 / 具体差异位置都报出来） */
export function fontProblems({ built, reference }) {
  const problems = [];
  const a = normalizeHead(built);
  const b = normalizeHead(reference);
  if (a.length !== b.length) {
    problems.push(`长度不同：构建 ${a.length} B / 参考 ${b.length} B`);
    return problems;
  }
  const diffs = [];
  for (let i = 0; i < a.length; i += 1) if (a[i] !== b[i]) diffs.push(i);
  if (diffs.length) {
    const ranges = [];
    let s = diffs[0];
    let p = diffs[0];
    for (const d of diffs.slice(1)) { if (d !== p + 1) { ranges.push([s, p]); s = d; } p = d; }
    ranges.push([s, p]);
    problems.push(
      `归一化后仍有 ${diffs.length} 个字节不同（${ranges.length} 段）：` +
        ranges.slice(0, 8).map(([x, y]) => `0x${x.toString(16)}–0x${y.toString(16)}`).join(' '),
    );
  }
  return problems;
}

/** 供 `--describe` / `--verify` 打印的字体事实（表清单 + cmap 子表 + name + 码页） */
export function fontFacts(buf) {
  const font = parseSfnt(buf);
  const subs = font.byTag.has('cmap') ? readCmap(buf, font.byTag.get('cmap')) : [];
  const names = font.byTag.has('name') ? readNames(buf, font.byTag.get('name')) : [];
  const os2 = font.byTag.get('OS/2') ? font.table('OS/2') : null;
  const bitsOf = (v) => [...Array(32).keys()].filter((i) => (v >>> i) & 1);
  return {
    bytes: buf.length,
    sfntVersion: font.sfntVersion,
    tables: font.tables.map((t) => `${t.tag}(${t.length})`),
    cmap: subs.map((s) => ({ platformID: s.platformID, platEncID: s.platEncID, format: s.format, entries: s.cmap.size })),
    names: names.map((r) => ({ nameID: r.nameID, platformID: r.platformID, langID: r.langID, text: nameText(r) })),
    codePageRange1: os2 && os2.length >= 82 ? os2.readUInt32BE(78) : null,
    codePageRange2: os2 && os2.length >= 86 ? os2.readUInt32BE(82) : null,
    bits1: os2 && os2.length >= 82 ? bitsOf(os2.readUInt32BE(78)) : [],
    bits2: os2 && os2.length >= 86 ? bitsOf(os2.readUInt32BE(82)) : [],
  };
}

export const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');
