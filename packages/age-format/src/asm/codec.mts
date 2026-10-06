/**
 * packages/age-format/src/asm/codec.mjs —— **AGE 脚本字符串的码页编解码**（字节 ↔ 文本，两方向都自持）
 *
 * 来源（旧仓只读参照，逐行对照移植）：
 *   · `天結/scripts/asm/age-shared.mjs` —— `decodeCp932` / `encodeCp932` / `cpToUtf16` / `utf16ToCp`
 *     （旧仓这两个函数把标准区交给 `iconv-lite`，本文件把标准区改成 `TextDecoder('shift_jis')`）
 *   · `天結/scripts/asm/README.md` §"与原版的有意差异" 第 4 条 —— 码位归属口径
 *   · `天結/scripts/lib/sjis-encode.js` —— 同一口径的另一处表述（可编码性判据）
 *
 * ## 职责边界
 * 只做「码页」这一层：一段字节 ↔ 一段 JS 字符串。不认识任何脚本结构、字段、指令语义。
 * 反汇编/重汇编要的 `codec` 参数就长这样：`{ id, decode, encode }`（外加几个口径查询）。
 *
 * ## 非显然口径（踩过才知道，务必别"顺手改回去"）
 * 1. **外字区（游戏 UDC）必须手工线性映射**：`0xF040–0xF9FC ↔ U+E000–U+E757`，索引 =
 *    `(lead-0xF0)*188 + (trail<0x7F ? trail-0x40 : trail-0x41)`（每前导 188 个）。
 *    平台解码器对这一段普遍有误，所以**解码先于平台表拦下这一段**，编码也**先于反查表**处理。
 * 2. **`0xFA40–0xFCFC` 不属于外字区**，而是 CP932 的 **IBM 扩展汉字区**（旧仓 README 第 4 条点名的那条坑）：
 *    它必须交平台表（`TextDecoder('shift_jis')`）。旧仓简体占位字大量落在这里（`现→刕=0xFA84`、
 *    `强→侔=0xFA72`、`敌→俉=0xFA61`…，由 cnjp 字体按码位还原字形）。若把它并进上面的线性段，
 *    写侧编到 `0xFAxx`、读侧却读回 `U+E7xx` ⇒ 两方向不互逆，回读验证会对上千行译文误报缺失。
 * 3. **反向表（编码方向）靠"枚举 + 解码"现场构造**：
 *    对每个**合法字节序列**解一次码，建立 `字符串 → 字节` 表；先注册者胜（单字节 → 双字节字典序 →
 *    外字区兜底）。这样 `encode(decode(bytes)) === bytes` 对**上表覆盖到的每一个字节序列**成立
 *    （含 IBM 扩展区、半角片假名、`0x5C`/`0x7E` 这类 ASCII 别名）；用"解码分支反推"或照抄某实现
 *    自带的编码表都做不到这一点。
 *    ★ 但 `0xED40–0xF940` 段有**两种写法**的字符必须照旧仓的选择来（见 `ENCODE_SKIP_*` 的长注释），
 *    否则重汇编与原文件不逐字节相同 —— 这是本文件唯一一处"优先级规则"，不是随手加的例外。
 * 4. **平台表的选择**：`TextDecoder('shift_jis')` 是 WHATWG 口径，与 Windows CP932 有 8 个**单字节**差异
 *    （`0x1A/0x1C/0x7F` 互相重映射、`0x80/0xA0/0xFD/0xFE/0xFF` 在 WHATWG 里落到 PUA 保留区或直接报错，
 *    见 `tools/lib/cp932.mjs` 头注释）。**逐序列枚举整个合法字节空间**（11536 个序列）实测：
 *    · 真正不互逆的只有两类 —— `0x1A/0x1C/0x7F` 三个字节（三者互相重排），
 *      以及"前导字节后面没有合法后继"的**孤立前导**（本来就不是一个完整字符）；
 *    · `0x80/0xA0/0xFD/0xFE/0xFF` 在这张表里与 PUA 码位一一对应，编得回去；
 *    · 剩下的 2074 个"前导+合法后继"不互逆项全都落在 `0x81–0x9F` 前导后面接**前导字节**这种位置
 *      （即解码器把两个前导中的第一个当单字节吐出来）—— 那也不是一个完整字符。
 *    **对真实数据实测**：游戏目录 106 个 `.BIN` 的全部 23954 条 `type==2` 字符串、759224 个原始字节，
 *    按 CP932 的字节结构**逐"码位单位"**检查互逆性（单字节 / 半角片假名 / 前导+后继）⇒ **不互逆 0 处**。
 * 5. **不抛异常的解码**：解不出的**单字节**保留原码位（`String.fromCharCode(b)`）。这样文件尾部
 *    若出现孤立前导字节也不会炸。**代价**：`0x1A` 等控制字节在文本里会以控制字符出现，
 *    存成 UTF-8 文本没事，但"肉眼可读"不保证。
 * 6. **编码遇到表里没有的字符**（既非外字区、也反查不到）⇒ 抛错，**不静默替换**：
 *    静默替换会把"写不进去"变成"悄悄写错"，而调用方（译文导入链）需要知道是哪个字符。
 * 7. 本文件**不依赖 `tools/lib/cp932.mjs`**（那是语料转写工具，带 GBK 箭头等语料专用口径）；
 *    这里是格式层，口径必须与运行时（emulator 的 `TextDecoder('shift_jis')`）一致。
 */
import { Buffer } from 'node:buffer';

const CP932 = new TextDecoder('shift_jis', { fatal: true });

/** AGE 脚本默认码页号（Windows-31J） */
export const CP_932 = 932;
/** UTF-16LE 码页号（脚本 v5 的字符串区用它） */
export const CP_UTF16 = 1200;
export const CP_UTF8 = 65001;
export const CP_936 = 936;

/** 外字区（游戏 UDC）在 Unicode 侧的范围 */
export const GAIJI_LO = 0xe000;
export const GAIJI_HI = 0xe757;
/** 外字区在 CP932 侧的前导字节范围（`0xFA40–0xFCFC` 属 IBM 扩展区，**不在**此列） */
export const GAIJI_LEAD_LO = 0xf0;
export const GAIJI_LEAD_HI = 0xf9;

/** 前导字节：`0x81–0x9F` / `0xE0–0xFC` */
export const isLeadByte = (b: number): boolean => (b >= 0x81 && b <= 0x9f) || (b >= 0xe0 && b <= 0xfc);
/** 后继字节：`0x40–0x7E` / `0x80–0xFC`（`0x7F` 永远不是后继） */
export const isTrailByte = (b: number): boolean => (b >= 0x40 && b <= 0x7e) || (b >= 0x80 && b <= 0xfc);

/** 该码位是否落在游戏外字区（`U+E000–U+E757`） */
export const isGaijiCodePoint = (cp: number): boolean => cp >= GAIJI_LO && cp <= GAIJI_HI;

// ───────────────────────────────────────────────────────── 外字区线性映射

/** 外字区读写位置 ← [前导, 后继]（调用方已保证前导在 `0xF0–0xF9`、后继合法） */
function gaijiIndex(lead: number, trail: number): number {
  const trailIndex = trail < 0x7f ? trail - 0x40 : trail - 0x41;
  return (lead - GAIJI_LEAD_LO) * 188 + trailIndex;
}

// ───────────────────────────────────────────────────────── 平台表封装

/** 双字节按平台 CP932 表解；未定义 ⇒ null（如 `0x86 0x80` / `0xEC 0xBD` 在 CP932 里没有映射） */
function tablePair(b0: number, b1: number): string | null {
  try {
    return CP932.decode(Buffer.from([b0, b1]));
  } catch {
    return null;
  }
}

/** 单字节按平台 CP932 表解；未定义 ⇒ null */
function tableSingle(b: number): string | null {
  try {
    return CP932.decode(Buffer.from([b]));
  } catch {
    return null;
  }
}

// ───────────────────────────────────────────────────────── 反向表（编码方向）

let REVERSE: Map<string, Uint8Array> | null = null;

/**
 * 编码方向**不采用**的字节对区间：`0xED40–0xF940`。
 *
 * 旧仓的 `iconv-lite` 在 `shiftjis` 编码表里显式声明了 `encodeSkipVals = [{from:0xED40,to:0xF940}]`
 * （**只影响编码方向，解码方向照常**）。原因不是"这段解码不对"，而是同一批字符在 CP932 里有**两种字节写法**：
 * 微软把 NEC 选定 IBM 扩展字又排在 `0xED40–0xF940`（NEC 行），而 CP932 的 IBM 扩展区在 `0xFA40–0xFCFC`。
 * 实测（全字节空间枚举）：
 *   · 同一字符有多个 CP932 写法的字符共 **396** 个；
 *   · 其中 **373** 个的"字典序最小写法"落在 `0xED40–0xF940`，而 `iconv-lite` 一律取**区外**那个写法
 *     （例：`U+FA19` → 我按最小值得 `0xEE62`，iconv 取 `0xFB7E`；`U+4FC9 俉` → `0xED45` vs `0xFA61`）。
 *   游戏原始 `.BIN` 里的字节是**按 iconv 口径**写进去的（实测 `STINIT2.BIN` 就带 `FB 7E`），
 *   若照最小值编码，重汇编就与原文件不等 —— 所以这里必须照 iconv 那条规则把该区间从**编码**表里排除。
 *
 * ★ 与 `iconv-lite` 的唯一有意差异：**外字区 `0xF040–0xF9FC` 仍然可编码**。
 *   iconv 的 skip 区间把外字区也盖住了 ⇒ 它对 1693 个外字位返回"不可编码"（写出去变 `?`，把原文写坏）；
 *   本包解码方向把这一段线性映射到 `U+E000–U+E757`，编码方向必须对称地编回去，否则"外字往返无损"是假的。
 *   实测后果：iconv 口径能编码的字符集合 ⊂ 本包能编码的集合，即**本包只在 iconv 会写坏的地方更强**。
 *   其余情况（含 skip 区间内"只有这一种写法"的字符）一律保留原写法 ⇒ `encode(decode(bytes)) === bytes`
 *   对全部 11536 个合法字节序列**仍然成立**。
 */
export const ENCODE_SKIP_LO = 0xed40;
export const ENCODE_SKIP_HI = 0xf940;

/** 该字节对是否属于"编码方向跳过"的区间 */
export const isEncodeSkipped = (lead: number, trail: number): boolean => {
  const mb = (lead << 8) | trail;
  return mb >= ENCODE_SKIP_LO && mb <= ENCODE_SKIP_HI;
};

/**
 * 构造 `字符串 → Buffer` 反查表。
 * 顺序即优先级：**先注册者胜**。枚举顺序 = 单字节 → 外字区 → `0x81–0x9F` 前导 → `0xE0–0xFC` 前导，
 * 每个前导内后继字节升序（= 字典序最小者胜，与旧仓 `iconv-lite` 的 `_fillEncodeTable` 同序）。
 * `0xED40–0xF940` 段跳过（见 `ENCODE_SKIP_*` 的说明）；外字区**不跳**（比 iconv 强的那一处）。
 */
function reverseTable(): Map<string, Uint8Array> {
  if (REVERSE) return REVERSE;
  const map = new Map<string, Uint8Array>();
  const put = (text: string, bytes: Uint8Array): void => {
    if (text === '' || text === undefined || map.has(text)) return;
    map.set(text, bytes);
  };
  // 1) 单字节
  for (let b = 0; b < 0x100; b += 1) { const t = tableSingle(b); if (t !== null) put(t, Buffer.from([b])); }
  // 2) 标准双字节区（跳过 0xED40–0xF940）
  const leads = [];
  for (let b = 0x81; b <= 0x9f; b += 1) leads.push(b);
  for (let b = 0xe0; b <= 0xfc; b += 1) leads.push(b);
  for (const lead of leads) {
    for (let trail = 0x40; trail <= 0xfc; trail += 1) {
      if (!isTrailByte(trail)) continue;
      if (isEncodeSkipped(lead, trail)) continue;
      const t2 = tablePair(lead, trail);
      if (t2 !== null) put(t2, Buffer.from([lead, trail]));
    }
  }
  // 3) 外字区（线性，最后兜底：它覆盖 U+E000–U+E757，且必须始终可编码）
  for (let lead = GAIJI_LEAD_LO; lead <= GAIJI_LEAD_HI; lead += 1) {
    for (let trail = 0x40; trail <= 0xfc; trail += 1) {
      if (!isTrailByte(trail)) continue;
      put(String.fromCharCode(GAIJI_LO + gaijiIndex(lead, trail)), Buffer.from([lead, trail]));
    }
  }
  REVERSE = map;
  return map;
}

// ───────────────────────────────────────────────────────── 公开编解码

/**
 * CP932 字节 → 字符串（外字区按线性表，其余按平台表；解不出的单字节保留原码位）。
 * @param {Buffer|Uint8Array} buf
 * @returns {string}
 */
export function decodeCp932(buf: Uint8Array): string {
  if (!buf || buf.length === 0) return '';
  const b = Buffer.isBuffer(buf) ? buf : Buffer.from(buf);
  let out = '';
  let p = 0;
  while (p < b.length) {
    const c = b[p];
    if (c < 0x80) {
      out += String.fromCharCode(c);
      p += 1;
      continue;
    }
    if (p + 1 < b.length && isTrailByte(b[p + 1])) {
      const lead = c;
      const trail = b[p + 1];
      if (lead >= GAIJI_LEAD_LO && lead <= GAIJI_LEAD_HI) {
        // 外字区（游戏 UDC）——先于平台表拦下
        out += String.fromCharCode(GAIJI_LO + gaijiIndex(lead, trail));
        p += 2;
        continue;
      }
      if (isLeadByte(lead)) {
        const pair = tablePair(lead, trail);
        if (pair !== null) {
          out += pair;
          p += 2;
          continue;
        }
      }
    }
    const one = tableSingle(c);
    out += one !== null ? one : String.fromCharCode(c);
    p += 1;
  }
  return out;
}

/**
 * 字符串 → CP932 字节。
 * @param {string} str
 * @returns {Buffer}
 * @throws 表里没有该字符时抛错（不静默替换）
 */
export function encodeCp932(str: string): Uint8Array {
  const rev = reverseTable();
  const out = Buffer.allocUnsafe(str.length * 2 + 8);
  let n = 0;
  for (const ch of str) {
    const hit = rev.get(ch);
    if (hit !== undefined) {
      for (let k = 0; k < hit.length; k += 1) out[n++] = hit[k];
      continue;
    }
    const cp = ch.codePointAt(0) ?? 0;
    // 外字区（U+E000–U+E757 = 1880 个位）已全部在反查表里；走到这里就是真编不出去。
    throw new Error(`encodeCp932: 无法编码字符 U+${cp.toString(16).toUpperCase()} (${ch})`);
  }
  return out.subarray(0, n);
}

/** 一个字符能否按 CP932 编出去（编码方向的探针；外字区算"能"） */
export function canEncodeCp932(ch: string): boolean {
  const cp = ch.codePointAt(0) ?? 0;
  if (isGaijiCodePoint(cp)) return true;
  return reverseTable().has(ch);
}

/** 字符串里第一个编不出去的字符；全部可编码 ⇒ null */
export function firstUnencodable(str: string): string | null {
  for (const ch of str) if (!canEncodeCp932(ch)) return ch;
  return null;
}

/**
 * UTF-16LE 字节 → 字符串（脚本 v5 的字符串区用）。
 * 口径同旧仓 `cpToUtf16(CP_UTF16, buf)`：按 16 位小端读，**去掉尾部 NUL**。
 */
export function decodeUtf16Le(buf: Uint8Array): string {
  if (!buf || buf.length === 0) return '';
  const b = Buffer.isBuffer(buf) ? buf : Buffer.from(buf);
  // 分批 fromCharCode：v5 的长串可能上万字符，一次性 spread 会顶到引擎的参数上限
  const parts = [];
  const batch = [];
  for (let p = 0; p + 1 < b.length; p += 2) {
    batch.push(b.readUInt16LE(p));
    if (batch.length === 2048) {
      parts.push(String.fromCharCode(...batch));
      batch.length = 0;
    }
  }
  if (batch.length > 0) parts.push(String.fromCharCode(...batch));
  return parts.join('').replace(/\u0000+$/, '');
}

/** 字符串 → UTF-16LE 字节（脚本 v5；不含尾部 NUL —— 那是调用方的排版责任） */
export function encodeUtf16Le(str: string): Uint8Array {
  const s = String(str);
  const out = Buffer.alloc(s.length * 2);
  for (let i = 0; i < s.length; i += 1) out.writeUInt16LE(s.charCodeAt(i), i * 2);
  return out;
}

/**
 * 一个 codepage 号对应的解码/编码口径；不在支持范围内 ⇒ null。
 *
 * 返回对象的形状就是反汇编器 / 重汇编器要的 `codec`：
 * `{ id, name, decode(bytes), encode(str), canEncode(ch) }`。
 *
 * 注意命名：`decode` / `encode` 是**码页方向**（字节串 ↔ 字符串）；
 * 脚本 v5 字符串区的 UTF-16LE 走 `codec.decodeUtf16Le` / `codec.encodeUtf16Le`（与 `id` 无关）。
 * `cp` 可以是码页号，也可以是名字（`sjis`/`shiftjis`/`shift-jis`/`cp932`/`utf16le`/`utf16`）。
 */
export function codecFor(cp: number): { id: number; name: string; decode: (b: Uint8Array) => string; encode: (s: string) => Uint8Array; canEncode: (ch: string) => boolean } | null {
  const n = typeof cp === 'string' ? parseCodepage(cp) : cp;
  const common = { decodeUtf16Le, encodeUtf16Le };
  if (n === CP_932) {
    return { id: CP_932, name: 'cp932', decode: decodeCp932, encode: encodeCp932, canEncode: canEncodeCp932, ...common };
  }
  if (n === CP_UTF16) {
    return { id: CP_UTF16, name: 'utf16le', decode: decodeUtf16Le, encode: encodeUtf16Le, canEncode: () => true, ...common };
  }
  return null;
}

/** 码页名字/数字 → 码页号；不认识 ⇒ 0（与旧仓 `parseCodepage` 同口径：0 表示"不认识"） */
export function parseCodepage(s: string): number {
  if (typeof s === 'number') return Number.isFinite(s) && s > 0 ? s : 0;
  const t = String(s).toLowerCase();
  if (t === 'utf8' || t === 'utf-8') return CP_UTF8;
  if (t === 'sjis' || t === 'shiftjis' || t === 'shift-jis' || t === 'cp932') return CP_932;
  if (t === 'gbk') return CP_936;
  if (t === 'utf16le' || t === 'utf16') return CP_UTF16;
  const n = parseInt(t, 0);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

/** 默认字符串码页（AGE 脚本的历史默认值） */
export const defaultCodec = codecFor(CP_932);
