/**
 * packages/age-format/src/agf.mjs —— **领域模型**：AGF（ACGF 图像容器）读写
 *
 * 来源（只读参照）：`天結/scripts/agf/format.js`（与 Eushully_AGF_TooL 的 Python 版逻辑一致）。
 * 本文件**只做容器与像素排布**：不含任何"这张图是哪个界面"之类的游戏语义。
 *
 * ## 盘上布局（little-endian；真实文件实测）
 * ```text
 * [0,4)     magic  "ACGF"
 * [4,8)     version（实测恒为 2）
 * [8,12)    unknown0（实测 0）
 * [12,16)   metaUnpackedSize           ┐
 * [16,20)   metaUnpackedSize（重复）   │ 与 injectAcgfFixed 写出的三个字段一致
 * [20,24)   metaPackedSize             ┘
 * [24, 24+metaPackedSize)  meta 区：`metaUnp === metaPak` ⇒ **原样存放**，否则 LZSS 压缩
 * 紧随其后   body 头 12 字节：[0]=unknown, [4]=bodyUnpackedSize, [8]=bodyPackedSize
 * 紧随其后   body 数据（同上：相等即原样，否则 LZSS 压缩）
 * 紧随其后   **可选** ACIF 块：'ACIF' + 24 字节前缀 + [alphaUnp, alphaPak] + alpha 数据（未压缩）
 * ```
 * ★ 实测 7 个真实 AGF：**全部是 ACGF**，meta 有"1104 原样"与"183 压缩"两种形态（两条分支都真存在）；
 *   body 一律压缩，ACIF 一律紧接 body 之后（无中间填充）。
 * ★★ **"原样还是压缩"的判定规则（44 个真实件实测）**：先按本文件的 LZSS 压缩，
 *   **压得比原文小就存压缩结果，否则原样存** —— 盘上每一段都符合这条，而且压缩结果与盘上**逐字节相同**
 *   （meta / body / **alpha 也一样会压**，旧仓注释只说了前两者）。所以这条规则不是"猜"，
 *   而是可复现的：`lzssCompress(x).length < x.length ? 压 : 原样`。
 *   （反面例子：`MI040.AGF` 的 meta 压缩后 1177 > 1080 ⇒ 盘上就是原样的 1080 字节。）
 *
 * ## 为什么能"解包 → 重打包逐字节相同"
 * `readAgf` 把每一段都留成**原始 slice**（不重新拼），`writeAgf` 只按原顺序与原始长度拼回去 ⇒
 * 未改动时必得逐字节相同的 Buffer。改像素则走 `replaceBody` / `replaceAlpha`（此时长度由调用方决定）。
 *
 * ## 与 ALF 的 LZSS **不是同一套**（别互相替代）
 * 见 `lzss.mjs` 头注释：这里匹配长度是 `(hi & 0x0f) + 3`、起始窗口位 `4096-18`，
 * 而 ALF 那套是 `+2`。两者都叫 LZSS、都能解压对方的部分数据，但**字节层不互逆** ⇒ 各自实现。
 */
import { Buffer } from 'node:buffer';
import fs from 'node:fs';

export const MAGIC = 'ACGF';
export const ACIF_MAGIC = 'ACIF';
export const HEADER_SIZE = 24;
export const BODY_HDR_SIZE = 12;
export const ACIF_PREFIX_SIZE = 24;

// ─────────────────────────────────────────────────────────── LZSS（AGF 族）

const FRAME = 4096;
const FRAME_INIT = 4078; // = 4096 - 18
/** AGF 族的最短匹配长度（解码侧 `(hi & 0x0f) + 3` 的由来） */
export const MIN_MATCH = 3;

/**
 * 解压 AGF 族的 LZSS 数据。
 * @param {Buffer|Uint8Array} data
 * @param {number} outputLen
 * @returns {Buffer}
 */
export function lzssDecompress(data, outputLen) {
  const out = Buffer.alloc(outputLen);
  const frame = Buffer.alloc(FRAME);
  let outPtr = 0;
  let framePtr = FRAME_INIT;
  let inPtr = 0;
  const inLen = data.length;
  while (outPtr < outputLen && inPtr < inLen) {
    let flags = data[inPtr++];
    for (let k = 0; k < 8; k += 1) {
      if (outPtr >= outputLen) break;
      if (flags & 1) {
        if (inPtr >= inLen) break;
        const v = data[inPtr++];
        out[outPtr++] = v;
        frame[framePtr] = v;
        framePtr = (framePtr + 1) & 4095;
      } else {
        if (inPtr + 1 >= inLen) break;
        const lo = data[inPtr];
        const hi = data[inPtr + 1];
        inPtr += 2;
        const offset = lo | ((hi & 0xf0) << 4);
        const length = (hi & 0x0f) + MIN_MATCH;
        for (let i = 0; i < length; i += 1) {
          if (outPtr >= outputLen) break;
          const v = frame[(offset + i) & 4095];
          out[outPtr++] = v;
          frame[framePtr] = v;
          framePtr = (framePtr + 1) & 4095;
        }
      }
      flags >>= 1;
    }
  }
  return out;
}

/**
 * 压缩 AGF 族的 LZSS 数据（算法与 ALF 族同源，**两处参数不同**：最短匹配 3、mask 换块时机）。
 * ★ 与 ALF 那套一样是确定性的；能否对真实文件逐字节复现由调用方实测（见 `.tmp` 探针结论）。
 * @param {Buffer|Uint8Array} input
 * @returns {Buffer}
 */
export function lzssCompress(input) {
  const size0 = input.length;
  const lson = new Int32Array(FRAME + 1);
  const rson = new Int32Array(FRAME + 257);
  const dad = new Int32Array(FRAME + 1);
  const text = new Uint8Array(FRAME + 18 - 1);
  const out = Buffer.alloc(size0 * 2 + Math.ceil(size0 / 8) + 32);
  let outputIndex = 0;

  for (let i = FRAME + 1; i <= FRAME + 256; i += 1) rson[i] = FRAME;
  for (let i = 0; i < FRAME; i += 1) dad[i] = FRAME;

  let matchPosition = 0;
  let matchLength = 0;
  const maxLen = 18;

  const insertNode = (r) => {
    let cmp = 1;
    let p = FRAME + 1 + text[r];
    rson[r] = FRAME;
    lson[r] = FRAME;
    matchLength = 0;
    for (;;) {
      if (cmp >= 0) {
        if (rson[p] !== FRAME) p = rson[p];
        else { rson[p] = r; dad[r] = p; return; }
      } else if (lson[p] !== FRAME) p = lson[p];
      else { lson[p] = r; dad[r] = p; return; }
      let i;
      for (i = 1; i < maxLen; i += 1) {
        cmp = text[r + i] - text[p + i];
        if (cmp !== 0) break;
      }
      if (i > matchLength) {
        matchPosition = p;
        matchLength = i;
        if (i >= maxLen) break;
      }
    }
    dad[r] = dad[p];
    lson[r] = lson[p];
    rson[r] = rson[p];
    dad[lson[p]] = r;
    dad[rson[p]] = r;
    if (rson[dad[p]] === p) rson[dad[p]] = r;
    else lson[dad[p]] = r;
    dad[p] = FRAME;
  };
  const deleteNode = (p) => {
    if (dad[p] === FRAME) return;
    let q;
    if (rson[p] === FRAME) q = lson[p];
    else if (lson[p] === FRAME) q = rson[p];
    else {
      q = lson[p];
      if (rson[q] !== FRAME) {
        do { q = rson[q]; } while (rson[q] !== FRAME);
        rson[dad[q]] = lson[q];
        dad[lson[q]] = dad[q];
        lson[q] = lson[p];
        dad[lson[p]] = q;
      }
      rson[q] = rson[p];
      dad[rson[p]] = q;
    }
    dad[q] = dad[p];
    if (rson[dad[p]] === p) rson[dad[p]] = q;
    else lson[dad[p]] = q;
    dad[p] = FRAME;
  };

  const codeBuf = new Uint8Array(17);
  const flush = (n) => { for (let k = 0; k < n; k += 1) out[outputIndex++] = codeBuf[k]; };

  let codeBufPtr = 1;
  let mask = 1;
  codeBuf[0] = 0;
  let size = size0;
  let bp = 0;
  let len = 0;
  let r = FRAME - maxLen;
  let s = 0;
  let lastMatchLength = 0;
  let i = 0;
  let c = 0;

  for (len = 0; len < maxLen && size > 0; len += 1) {
    c = input[bp++];
    text[r + len] = c;
    size -= 1;
  }
  if (len === 0) return out.subarray(0, outputIndex);

  for (i = 1; i <= maxLen; i += 1) insertNode(r - i);
  insertNode(r);

  do {
    if (matchLength > len) matchLength = len;
    if (matchLength < MIN_MATCH) {
      // 匹配太短 ⇒ 发一个字面量（与解压侧 `+3` 对应：位置-长度对能表达的最短匹配就是 3）
      matchLength = 1;
      codeBuf[0] |= mask;
      codeBuf[codeBufPtr++] = text[r];
    } else {
      codeBuf[codeBufPtr++] = matchPosition & 0xff;
      codeBuf[codeBufPtr++] = (((matchPosition >> 4) & 0xf0) | (matchLength - MIN_MATCH)) & 0xff;
    }
    mask = (mask << 1) & 0xff;
    if (mask === 0) {
      flush(codeBufPtr);
      codeBuf[0] = 0;
      codeBufPtr = 1;
      mask = 1;
    }
    lastMatchLength = matchLength;
    for (i = 0; i < lastMatchLength && size > 0; i += 1) {
      c = input[bp++];
      size -= 1;
      deleteNode(s);
      text[s] = c;
      if (s < maxLen - 1) text[s + FRAME] = c;
      s = (s + 1) & (FRAME - 1);
      r = (r + 1) & (FRAME - 1);
      insertNode(r);
    }
    while (i++ < lastMatchLength) {
      deleteNode(s);
      s = (s + 1) & (FRAME - 1);
      r = (r + 1) & (FRAME - 1);
      if (--len) insertNode(r);
    }
  } while (len > 0);

  if (codeBufPtr > 1) flush(codeBufPtr);
  return out.subarray(0, outputIndex);
}

// ─────────────────────────────────────────────────────────── 元数据（像素排布）

/** 每行字节数（**4 字节对齐**：8bpp 与 24bpp 都要补，32bpp 天然对齐） */
export function strideFor(w, bpp) {
  if (bpp === 8) return (w + 3) & -4;
  if (bpp === 24) return (w * 3 + 3) & -4;
  if (bpp === 32) return w * 4;
  return 0;
}

/** 一整帧的字节数（含行填充） */
export function sizeFor(w, h, bpp) {
  const s = strideFor(w, bpp);
  return s > 0 ? s * h : 0;
}

/** 从元数据里取宽高与 bpp（`injectAcgfFixed` 写出的口径：+20/+24/+30） */
export function parseWhBpp(meta) {
  let w = 0;
  let h = 0;
  let bpp = 0;
  if (meta.length >= 32) {
    w = meta.readUInt32LE(20);
    h = meta.readUInt32LE(24);
    bpp = meta.readInt16LE(30);
  }
  if ((w === 0 || h === 0) && meta.length >= 8) {
    const w2 = meta.readUInt32LE(0);
    const h2 = meta.readUInt32LE(4);
    if (w2 && h2) {
      w = w2;
      h = h2;
    }
  }
  return [w, h, bpp];
}

/**
 * 取调色板（每项 `[r,g,b]`；盘上是 BGRA 顺序，这里已换好通道）。
 *
 * 两条取值路径（**顺序照旧仓**，都是有依据的，不是瞎猜）：
 *   ① 元数据装得下「头部 + 若干项」⇒ 调色板从 `+56` 起，**能装几项就取几项**
 *      （实测：`MI040.AGF` meta=1080 ⇒ 256 项；`SO002.AGF` meta=800 ⇒ 只有 186 项，
 *      而它的像素最大索引正好是 185 ⇒ **够用**，不是坏件 —— 这是"截断调色板"变体，
 *      旧仓那个照抄来的 `≥ 1080` 阈值会把它误判成"没有调色板"）；
 *   ② 元数据恰好 1024 字节左右（没有头部）⇒ 调色板就是**最后 1024 字节**。
 * ★ 一项都取不到时返回 `null` —— **不编造灰度**（旧仓会造灰度表；静默给出错颜色比报错更难查）。
 */
export function extractPaletteRgb(meta) {
  let palOff;
  if (meta.length >= 56 + 4) palOff = 56;
  else if (meta.length >= 1024) palOff = meta.length - 1024;
  else return null;
  const count = Math.min(256, Math.floor((meta.length - palOff) / 4));
  if (count === 0) return null;
  const pal = [];
  for (let i = 0; i < count; i += 1) {
    pal.push([meta[palOff + i * 4 + 2], meta[palOff + i * 4 + 1], meta[palOff + i * 4]]);
  }
  return pal;
}

// ─────────────────────────────────────────────────────────── 容器读写

/** 读一个"可压缩段"：`unpacked === packed` 时原样，否则解压 */
function readPackedSection(buf, off, unpacked, packed) {
  const raw = buf.subarray(off, off + packed);
  if (raw.length !== packed) throw new Error(`段越界：期望 ${packed} 字节，实际 ${raw.length}`);
  return unpacked === packed ? Buffer.from(raw) : lzssDecompress(raw, unpacked);
}

/**
 * 读一个 AGF（**只支持 ACGF**；无头变体不猜）。
 * @param {string} file
 */
export function readAgf(file) {
  const buf = fs.readFileSync(file);
  return readAgfBuffer(buf, file);
}

/** 同上，但吃内存里的字节（给"从 ALF 里解出来的 AGF"用） */
export function readAgfBuffer(buf, file = '<memory>') {
  if (buf.length < HEADER_SIZE + BODY_HDR_SIZE) throw new Error(`太短，不是 ACGF：${buf.length} 字节`);
  const magic = buf.subarray(0, 4).toString('latin1');
  if (magic !== MAGIC) throw new Error(`不是 ACGF（前 4 字节 ${JSON.stringify(magic)}）：${file}`);

  const header = Buffer.from(buf.subarray(0, HEADER_SIZE));
  const metaUnpackedSize = buf.readInt32LE(12);
  const metaPackedSize = buf.readInt32LE(20);
  const metaOffset = HEADER_SIZE;
  const metaRaw = Buffer.from(buf.subarray(metaOffset, metaOffset + metaPackedSize));
  const meta = metaUnpackedSize === metaPackedSize ? Buffer.from(metaRaw) : lzssDecompress(metaRaw, metaUnpackedSize);

  const bodyHeaderOffset = metaOffset + metaPackedSize;
  const bodyHeader = Buffer.from(buf.subarray(bodyHeaderOffset, bodyHeaderOffset + BODY_HDR_SIZE));
  const bodyUnknown = bodyHeader.readInt32LE(0);
  const bodyUnpackedSize = bodyHeader.readInt32LE(4);
  const bodyPackedSize = bodyHeader.readInt32LE(8);
  const bodyOffset = bodyHeaderOffset + BODY_HDR_SIZE;
  const bodyRaw = Buffer.from(buf.subarray(bodyOffset, bodyOffset + bodyPackedSize));
  const body = bodyUnpackedSize === bodyPackedSize ? Buffer.from(bodyRaw) : lzssDecompress(bodyRaw, bodyUnpackedSize);
  const bodyEnd = bodyOffset + bodyPackedSize;

  // ACIF：原版是在 bodyEnd 之后 2048 字节窗口里 indexOf('ACIF')
  let acif = null;
  const window = buf.subarray(bodyEnd, Math.min(bodyEnd + 2048, buf.length));
  const idx = window.indexOf(Buffer.from(ACIF_MAGIC, 'latin1'));
  if (idx >= 0) {
    const acifOffset = bodyEnd + idx;
    const prefix = Buffer.from(buf.subarray(acifOffset, acifOffset + 4 + ACIF_PREFIX_SIZE));
    const alphaUnpackedSize = buf.readInt32LE(acifOffset + 4 + ACIF_PREFIX_SIZE);
    const alphaPackedSize = buf.readInt32LE(acifOffset + 4 + ACIF_PREFIX_SIZE + 4);
    const alphaOffset = acifOffset + 4 + ACIF_PREFIX_SIZE + 8;
    const alphaRaw = Buffer.from(buf.subarray(alphaOffset, alphaOffset + alphaPackedSize));
    const alpha = alphaUnpackedSize === alphaPackedSize ? Buffer.from(alphaRaw) : lzssDecompress(alphaRaw, alphaUnpackedSize);
    acif = {
      offset: acifOffset,
      gap: Buffer.from(buf.subarray(bodyEnd, acifOffset)),
      prefix,
      alphaUnpackedSize,
      alphaPackedSize,
      alphaRaw,
      alpha,
      end: alphaOffset + alphaPackedSize,
    };
  } else {
    acif = null;
  }
  const trailer = Buffer.from(buf.subarray(acif ? acif.end : bodyEnd));

  const [width, height, bpp] = parseWhBpp(meta);
  return {
    file,
    buf,
    header,
    version: buf.readInt32LE(4),
    metaUnpackedSize,
    metaPackedSize,
    metaRaw,
    meta,
    bodyHeaderOffset,
    bodyHeader,
    bodyUnknown,
    bodyUnpackedSize,
    bodyPackedSize,
    bodyRaw,
    body,
    acif,
    trailer,
    width,
    height,
    bpp,
    version2: buf.readInt32LE(8),
  };
}

/**
 * 按原始片段拼回 AGF 字节 ⇒ 未改动时**逐字节相同**。
 * @param {object} agf
 * @param {{metaRaw?: Buffer, bodyRaw?: Buffer, bodyUnpackedSize?: number, alphaRaw?: Buffer, alphaUnpackedSize?: number}} [override]
 */
export function writeAgf(agf, override = {}) {
  const metaRaw = override.metaRaw ?? agf.metaRaw;
  const bodyRaw = override.bodyRaw ?? agf.bodyRaw;
  const alphaRaw = override.alphaRaw ?? (agf.acif ? agf.acif.alphaRaw : null);

  const header = Buffer.from(agf.header);
  // ★ 实测口径：+12 与 +16 都记**未压缩**大小，+20 才是压缩后大小（`injectAcgfFixed` 也是这么写的）。
  //   把 +12/+16 写成压缩后大小会得到"长度对、内容错"的文件（真实件 MI042/MI047 就是这么抓出来的）。
  header.writeInt32LE(agf.metaUnpackedSize, 12);
  header.writeInt32LE(agf.metaUnpackedSize, 16);
  header.writeInt32LE(metaRaw.length, 20);

  const bodyHeader = Buffer.from(agf.bodyHeader);
  const bodyUnpackedSize = override.bodyUnpackedSize ?? agf.bodyUnpackedSize;
  bodyHeader.writeInt32LE(bodyUnpackedSize, 4);
  bodyHeader.writeInt32LE(bodyRaw.length, 8);

  const parts = [header, metaRaw, bodyHeader, bodyRaw];
  if (agf.acif) {
    parts.push(agf.acif.gap);
    const prefix = Buffer.from(agf.acif.prefix);
    const alphaUnpackedSize = override.alphaUnpackedSize ?? agf.acif.alphaUnpackedSize;
    const alphaSize = Buffer.alloc(8);
    alphaSize.writeInt32LE(alphaUnpackedSize, 0);
    alphaSize.writeInt32LE(alphaRaw.length, 4);
    parts.push(prefix, alphaSize, alphaRaw);
  }
  parts.push(agf.trailer);
  return Buffer.concat(parts);
}

/**
 * 按实测规则把一段数据装成"盘上形态"：**压得小就压，否则原样**（见文件头 ★★）。
 * @param {Buffer|Uint8Array} data
 * @returns {{raw: Buffer, unpackedSize: number, compressed: boolean}}
 */
export function packSection(data) {
  const src = Buffer.isBuffer(data) ? data : Buffer.from(data);
  const packed = lzssCompress(src);
  if (packed.length < src.length) return { raw: packed, unpackedSize: src.length, compressed: true };
  return { raw: src, unpackedSize: src.length, compressed: false };
}

/** 便捷：只判"这个 AGF 读进来再写出去是否逐字节相同" */
export function roundTripEqual(agf) {
  const out = writeAgf(agf);
  return out.length === agf.buf.length && out.equals(agf.buf);
}

// ─────────────────────────────────────────────────────────── 像素

/**
 * 解成 **top-down RGBA**（`rgba.length === width*height*4`）。
 * 与旧仓 `decodeAgfRgba` 一致：盘上是 bottom-up、通道序 BGR(A)。
 * @param {object} agf `readAgf` 的返回值
 * @returns {{width:number, height:number, rgba:Buffer}}
 */
export function decodeRgba(agf) {
  const { width: w, height: h, bpp, body, meta, acif } = agf;
  const s = strideFor(w, bpp);
  const need = s * h;
  if (need === 0) throw new Error(`不支持的 bpp=${bpp}`);
  const pixels = body.length >= need ? body : Buffer.concat([body, Buffer.alloc(need - body.length)]);
  const alpha = acif && acif.alpha.length === w * h ? acif.alpha : null;
  const rgba = Buffer.alloc(w * h * 4);
  let palette = null;
  if (bpp === 8) {
    palette = extractPaletteRgb(meta);
    if (!palette) throw new Error('8bpp 但元数据里没有调色板（不编造灰度）');
  }
  for (let y = 0; y < h; y += 1) {
    const srcRow = (h - 1 - y) * s;
    for (let x = 0; x < w; x += 1) {
      const d = (y * w + x) * 4;
      if (bpp === 8) {
        const idx = pixels[srcRow + x];
        const c = palette[idx];
        if (!c) {
          throw new Error(
            `8bpp 像素索引 ${idx} 超出调色板范围（只有 ${palette.length} 项）—— 这是坏件或调色板被截断得更狠`,
          );
        }
        rgba[d] = c[0];
        rgba[d + 1] = c[1];
        rgba[d + 2] = c[2];
        rgba[d + 3] = alpha ? alpha[y * w + x] : 255;
      } else if (bpp === 24) {
        const s2 = srcRow + x * 3;
        rgba[d] = pixels[s2 + 2];
        rgba[d + 1] = pixels[s2 + 1];
        rgba[d + 2] = pixels[s2];
        rgba[d + 3] = alpha ? alpha[y * w + x] : 255;
      } else {
        const s2 = srcRow + x * 4;
        rgba[d] = pixels[s2 + 2];
        rgba[d + 1] = pixels[s2 + 1];
        rgba[d + 2] = pixels[s2];
        rgba[d + 3] = pixels[s2 + 3];
        if (alpha) rgba[d + 3] = alpha[y * w + x];
      }
    }
  }
  return { width: w, height: h, rgba };
}

/**
 * 把 top-down RGBA 编回 body 字节（bottom-up + 行对齐 + 通道交换）。
 * @param {{width:number,height:number,rgba:Buffer}} img
 * @param {number} bpp
 * @param {Array<[number,number,number]>|null} [paletteRgb] 8bpp 时必给（不量化、只做最近色）
 */
export function encodeBody(img, bpp, paletteRgb = null) {
  const { width: w, height: h, rgba } = img;
  const s = strideFor(w, bpp);
  const out = Buffer.alloc(s * h);
  for (let y = 0; y < h; y += 1) {
    const dstRow = (h - 1 - y) * s;
    for (let x = 0; x < w; x += 1) {
      const i = (y * w + x) * 4;
      const r = rgba[i];
      const g = rgba[i + 1];
      const b = rgba[i + 2];
      if (bpp === 8) {
        if (!paletteRgb) throw new Error('8bpp 需要调色板');
        let best = 0;
        let bestD = Infinity;
        for (let p = 0; p < 256; p += 1) {
          const dr = r - paletteRgb[p][0];
          const dg = g - paletteRgb[p][1];
          const db = b - paletteRgb[p][2];
          const d = dr * dr + dg * dg + db * db;
          if (d < bestD) { bestD = d; best = p; }
        }
        out[dstRow + x] = best;
      } else if (bpp === 24) {
        out[dstRow + x * 3] = b;
        out[dstRow + x * 3 + 1] = g;
        out[dstRow + x * 3 + 2] = r;
      } else {
        out[dstRow + x * 4] = b;
        out[dstRow + x * 4 + 1] = g;
        out[dstRow + x * 4 + 2] = r;
        out[dstRow + x * 4 + 3] = rgba[i + 3];
      }
    }
  }
  return out;
}

/** 从 RGBA 抽 alpha 通道（ACIF 存的就是这个） */
export function alphaFromRgba(img) {
  const n = img.width * img.height;
  const out = Buffer.alloc(n);
  for (let i = 0; i < n; i += 1) out[i] = img.rgba[i * 4 + 3];
  return out;
}
