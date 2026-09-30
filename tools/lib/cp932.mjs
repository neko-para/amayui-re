/**
 * tools/lib/cp932.mjs — **纯 CP932 / GBK 编解码工具**（不认识任何领域数据）
 *
 * 归这里的判据：只吃 Buffer / 字符串，吐字符串 / Buffer。**语料规则**（哪一行该按 GBK 解、
 * 该不该转写、断言什么）不在这里 —— 那是 `disasm-recode.mjs` 的领域决策。
 *
 * ★ 为什么不能直接用 `TextDecoder('shift_jis')`：
 *   Windows CP932 与 WHATWG `shift_jis` 有**8 个单字节**差异（实测对照 Python 的 cp932 表得到）：
 *   `0x1A/0x1C/0x7F` 会被 WHATWG 互相重映射（`0x7F` → U+001A！），
 *   `0x80/0xA0/0xFD/0xFE/0xFF` 在 WHATWG 里直接报错而 CP932 有定义。
 *   双字节部分两种实现**逐字一致**（9604 个已定义 pair 全比过，0 差异）⇒ 双字节交给 WHATWG 表。
 *
 * ★ `encodeBack` 是"逐字节忠实"的证据工具：它把转写后的文本反解回原字节。
 *   反解表由**同一套解码原语**枚举所有可解序列现场构造，**不依赖任何编码器**。
 */
const CP932 = new TextDecoder('shift_jis', { fatal: true });
const GBK = new TextDecoder('gbk', { fatal: true });

const CP932_SINGLE = new Map([
  [0x1a, 0x001a],
  [0x1c, 0x001c],
  [0x7f, 0x007f],
  [0x80, 0x0080],
  [0xa0, 0xf8f0],
  [0xfd, 0xf8f1],
  [0xfe, 0xf8f2],
  [0xff, 0xf8f3],
]);

export const isLead = (b) => (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
export const isTrail = (b) => (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);

/** 单字节 CP932 解码（含上述差异修正）；不可解 ⇒ null */
export function cp932Single(b) {
  if (CP932_SINGLE.has(b)) return String.fromCodePoint(CP932_SINGLE.get(b));
  if (b < 0x80 || (b >= 0xa1 && b <= 0xdf)) return CP932.decode(Buffer.from([b]));
  return null;
}

/** 双字节 CP932 解码；未定义 ⇒ null（如 `86 80` / `EC BD` 在 CP932 里没有映射） */
export function cp932Pair(b0, b1) {
  try {
    return CP932.decode(Buffer.from([b0, b1]));
  } catch {
    return null;
  }
}

export function gbkPair(b0, b1) {
  try {
    return GBK.decode(Buffer.from([b0, b1]));
  } catch {
    return null;
  }
}

export function gbkSingle(b) {
  try {
    return GBK.decode(Buffer.from([b]));
  } catch {
    return null;
  }
}

export function gbkText(buf) {
  try {
    return GBK.decode(buf);
  } catch {
    return null;
  }
}

/**
 * 按 **CP932 的字节结构**逐字节解一条字节串：
 *   · `0xA1..0xDF` 是**单字节**半角假名，**永远不是前导字节**（旧仓的 bug 就在这）；
 *   · `A1 F4..FE` 是 IDA 的 GBK 箭头字形；
 *   · CP932 解不出的**单字节保留原码位**（IDA 的 `; '\x80'` ⇒ U+0080）。
 */
export function decodeCp932Run(buf, stats = {}) {
  const out = [];
  let i = 0;
  while (i < buf.length) {
    const c = buf[i];
    if (c < 0x80) {
      out.push(String.fromCharCode(c));
      i += 1;
      continue;
    }
    if (c === 0xa1 && i + 1 < buf.length && buf[i + 1] >= 0xf4 && buf[i + 1] <= 0xfe) {
      const arrow = gbkPair(c, buf[i + 1]);
      if (arrow !== null) {
        out.push(arrow);
        stats.gbkArrows = (stats.gbkArrows ?? 0) + 1;
        i += 2;
        continue;
      }
    }
    if (c >= 0xa1 && c <= 0xdf) {
      const ch = cp932Single(c);
      if (ch !== null) {
        out.push(ch);
        i += 1;
        continue;
      }
    }
    if (isLead(c) && i + 1 < buf.length && isTrail(buf[i + 1])) {
      const pair = cp932Pair(c, buf[i + 1]);
      if (pair !== null) {
        out.push(pair);
        i += 2;
        continue;
      }
    }
    out.push(String.fromCharCode(c));
    stats.fallback = (stats.fallback ?? 0) + 1;
    i += 1;
  }
  return out.join('');
}

/** 整条按 GBK 解；解不出就退化成逐字节 CP932（与旧脚本同口径） */
export function decodeGbkLine(buf, stats = {}) {
  const t = gbkText(buf);
  return t !== null ? t : decodeCp932Run(buf, stats);
}

// ───────────────────────────────────────────────── 反解（文本 → 原字节）

function buildReverse(decodePair, decodeOne, leads) {
  const map = new Map();
  const dups = new Map();
  const put = (text, bytes) => {
    if (text.length === 0) return;
    if (map.has(text)) {
      if (Buffer.compare(map.get(text), bytes) !== 0) dups.set(text, (dups.get(text) ?? 1) + 1);
      return; // 首个（字典序最小）胜出
    }
    map.set(text, bytes);
  };
  for (let b = 0; b < 0x100; b += 1) {
    const ch = decodeOne(b);
    if (ch !== null) put(ch, Buffer.from([b]));
  }
  for (const b0 of leads) {
    for (let b1 = 0x40; b1 <= 0xfc; b1 += 1) {
      if (b1 === 0x7f || !isTrail(b1)) continue;
      const ch = decodePair(b0, b1);
      if (ch !== null) put(ch, Buffer.from([b0, b1]));
    }
  }
  return { map, dups };
}

const LEAD_BYTES = [
  ...Array.from({ length: 0x9f - 0x81 + 1 }, (_, i) => 0x81 + i),
  ...Array.from({ length: 0xfc - 0xe0 + 1 }, (_, i) => 0xe0 + i),
];
const GBK_LEADS = [...Array.from({ length: 0xfe - 0x81 + 1 }, (_, i) => 0x81 + i)];

let REV_CP932 = null;
let REV_GBK = null;
export const revCp932 = () => (REV_CP932 ??= buildReverse(cp932Pair, cp932Single, LEAD_BYTES));
export const revGbk = () => (REV_GBK ??= buildReverse(gbkPair, gbkSingle, GBK_LEADS));

/**
 * 把转写后的文本反解成源字节。
 * 查不到表、且码位 < 0x100 ⇒ 那就是"保留原码位"的那个字节。
 * @returns {Buffer|null} null = 有字符无法还原（解码不可逆 ⇒ 断言该红）
 */
export function encodeBack(text, { gbk = false } = {}) {
  const rev = gbk ? revGbk() : revCp932();
  const out = Buffer.allocUnsafe(text.length * 2 + 8);
  let n = 0;
  for (const ch of text) {
    const hit = rev.map.get(ch);
    if (hit !== undefined) {
      for (let k = 0; k < hit.length; k += 1) out[n++] = hit[k];
      continue;
    }
    const cp = ch.codePointAt(0);
    if (cp < 0x100) {
      out[n++] = cp;
      continue;
    }
    return null;
  }
  return out.subarray(0, n);
}
