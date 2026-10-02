/**
 * packages/age-format/src/lzss.mjs —— **纯工具**：AGE 用的 LZSS（Okumura/Allegro 变体）
 *
 * 来源（只读参照，逐行对照移植）：
 *   · `天結/tools/alf/lzss/lzss.cpp`（解压 `lzss_read` + 压缩 `lzss_write`/`lzss_insertnode`/`lzss_deletenode`）
 *   · `天結/scripts/alf/lzss.mjs`（旧仓只移植过解压方向；压缩方向由本文件补齐）
 *
 * ## 非显然口径（踩过才知道）
 * 1. **两个方向不是同一个"格式"**：ALF 的目录区（`S4SECTHDR`）用的是本文件这一族；
 *    AGF 图像数据里那套（`scripts/agf/format.js` 的 `lzssDecompress`）**参数不同**
 *    （匹配长度 `(hi & 0x0f) + 3` 而不是 `+2`，且一次只处理 8 个 flag 为一个循环体）。
 *    两者都叫 LZSS，但**不能互相替代** —— 所以这里叫 `lzss`（ALF 族），AGF 那套实现在 `agf.mjs` 内自带。
 * 2. **`mask` 必须是 8 位**：C 里 `unsigned char mask` 左移到 0 就回绕，靠这个判定"8 个单位了"。
 *    JS 的数字是 64 位 ⇒ 必须显式 `& 0xFF`，否则永远不会回绕（会写出一个巨大的 flag 块）。
 * 3. **压缩是可复现的**：对真实游戏索引文件实测（`.AAI` / `SYS4INI.BIN`，含 44.6 万字节的目录区）
 *    「解压 → 重压」**逐字节相同**。这是格式层"解包→重打包逐字节相同"能当判据的前提。
 * 4. 单次调用语义：C 版有 `state`/`goto pos1/pos2` 的**流式中断恢复**分支，只有分块喂数据时才会走到。
 *    这里的 `pack`/`unpack` 都是"一次给全"，故不移植那两个分支（语义等价，见旧仓 lzss.mjs 的同款说明）。
 */
import { Buffer } from 'node:buffer';

/** 环形缓冲长度（4K）—— 12 位位置编码的来源 */
export const N = 4096;
/** 最长匹配长度 */
export const F = 18;
/** 匹配长度 ≤ 该值就发字面量 */
export const THRESHOLD = 2;

/**
 * 解压 LZSS 数据。
 * @param {Buffer|Uint8Array} input 压缩数据
 * @param {number} inputLen 压缩数据长度
 * @param {number} outputLen 期望解压出的字节数（调用方从段头拿）
 * @returns {{data: Buffer, size: number}} `size` = 实际写出字节数（输入提前耗尽时 < outputLen）
 */
export function unpack(input, inputLen, outputLen) {
  const out = Buffer.alloc(outputLen);
  const text = new Uint8Array(N + F - 1);
  let r = N - F;
  let flags = 0;
  let size = 0;
  let ip = 0;

  for (;;) {
    // flag 字节用高字节计数 8 项：右移到 0x100 以下就说明该取下一个了
    if (((flags >>= 1) & 256) === 0) {
      if (ip >= inputLen) break;
      flags = input[ip++] | 0xff00;
    }
    if (flags & 1) {
      if (ip >= inputLen) break;
      const c = input[ip++];
      text[r++] = c;
      r &= N - 1;
      out[size++] = c;
      if (size >= outputLen) break;
    } else {
      if (ip + 2 > inputLen) break;
      let i = input[ip++];
      const j = input[ip++];
      i |= (j & 0xf0) << 4; // 12 位位置：低 8 位 + 高 4 位
      const len = (j & 0x0f) + THRESHOLD;
      for (let k = 0; k <= len; k++) {
        const c = text[(i + k) & (N - 1)];
        text[r++] = c;
        r &= N - 1;
        out[size++] = c;
        if (size >= outputLen) return { data: out, size };
      }
    }
  }
  return { data: out, size };
}

/**
 * 压缩（Okumura LZSS + 二叉树加速）。
 *
 * ★ **确定性**：同一输入必得同一输出 —— 所以"解压 → 重压"可以当作往返判据。
 * @param {Buffer|Uint8Array} input
 * @returns {Buffer} 压缩数据
 */
export function pack(input) {
  const size0 = input.length;
  const lson = new Int32Array(N + 1);
  const rson = new Int32Array(N + 257);
  const dad = new Int32Array(N + 1);
  const text = new Uint8Array(N + F - 1);
  // 最坏情形：每个单位都发位置-长度对（2 字节）+ 每 8 个单位 1 个 flag 字节
  const out = Buffer.alloc(size0 * 2 + Math.ceil(size0 / 8) + 32);
  let outputIndex = 0;

  for (let i = N + 1; i <= N + 256; i += 1) rson[i] = N; // 256 棵树的根
  for (let i = 0; i < N; i += 1) dad[i] = N; // N = "未使用"

  let matchPosition = 0;
  let matchLength = 0;

  /** 把 text_buf[r..r+F-1] 插入第 text_buf[r] 棵树，回填 matchPosition/matchLength */
  const insertNode = (r) => {
    let cmp = 1;
    let p = N + 1 + text[r];
    rson[r] = N;
    lson[r] = N;
    matchLength = 0;
    for (;;) {
      if (cmp >= 0) {
        if (rson[p] !== N) p = rson[p];
        else {
          rson[p] = r;
          dad[r] = p;
          return;
        }
      } else if (lson[p] !== N) p = lson[p];
      else {
        lson[p] = r;
        dad[r] = p;
        return;
      }
      let i;
      for (i = 1; i < F; i += 1) {
        cmp = text[r + i] - text[p + i];
        if (cmp !== 0) break;
      }
      if (i > matchLength) {
        matchPosition = p;
        matchLength = i;
        if (i >= F) break;
      }
    }
    // 找到满长匹配 ⇒ 用新节点顶掉旧节点（旧的会更早被淘汰）
    dad[r] = dad[p];
    lson[r] = lson[p];
    rson[r] = rson[p];
    dad[lson[p]] = r;
    dad[rson[p]] = r;
    if (rson[dad[p]] === p) rson[dad[p]] = r;
    else lson[dad[p]] = r;
    dad[p] = N;
  };

  const deleteNode = (p) => {
    if (dad[p] === N) return;
    let q;
    if (rson[p] === N) q = lson[p];
    else if (lson[p] === N) q = rson[p];
    else {
      q = lson[p];
      if (rson[q] !== N) {
        do {
          q = rson[q];
        } while (rson[q] !== N);
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
    dad[p] = N;
  };

  const codeBuf = new Uint8Array(17); // [0] = 8 个 flag；[1..16] = 最多 8 个单位
  const flush = (n) => {
    for (let k = 0; k < n; k += 1) out[outputIndex++] = codeBuf[k];
  };

  let codeBufPtr = 1;
  let mask = 1;
  codeBuf[0] = 0;
  let size = size0;
  let bp = 0;
  let len = 0;
  let r = N - F;
  let s = 0;
  let lastMatchLength = 0;
  let i = 0;
  let c = 0;

  for (len = 0; len < F && size > 0; len += 1) {
    c = input[bp++];
    text[r + len] = c;
    size -= 1;
  }
  if (len === 0) return out.subarray(0, outputIndex);

  for (i = 1; i <= F; i += 1) insertNode(r - i);
  insertNode(r);

  do {
    if (matchLength > len) matchLength = len; // 末尾附近匹配可能超长
    if (matchLength <= THRESHOLD) {
      matchLength = 1;
      codeBuf[0] |= mask;
      codeBuf[codeBufPtr++] = text[r];
    } else {
      codeBuf[codeBufPtr++] = matchPosition & 0xff;
      codeBuf[codeBufPtr++] = (((matchPosition >> 4) & 0xf0) | (matchLength - (THRESHOLD + 1))) & 0xff;
    }
    mask = (mask << 1) & 0xff; // ★ 8 位回绕（见文件头「非显然口径」第 2 条）
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
      if (s < F - 1) text[s + N] = c; // 环形缓冲尾部的 F-1 个冗余字节，便于整段比较
      s = (s + 1) & (N - 1);
      r = (r + 1) & (N - 1);
      insertNode(r);
    }
    while (i++ < lastMatchLength) {
      deleteNode(s);
      s = (s + 1) & (N - 1);
      r = (r + 1) & (N - 1);
      if (--len) insertNode(r);
    }
  } while (len > 0);

  if (codeBufPtr > 1) flush(codeBufPtr);
  return out.subarray(0, outputIndex);
}

/** 便捷：把一段数据压成"解压回原样"的 LZSS 字节（`unpack(pack(x), x.length) === x`） */
export function roundTripBytes(data) {
  return pack(data);
}
