/**
 * 存档负载编码的**纯函数**：两个 CRC-32、1→2 字变换、未压缩信封。
 *
 * ★ 合同来自 `65-save` 的 `FMT-SAVE-CODEC`；本文件不碰文件系统、不认识槽。
 * ★ 只实现“可写回”的第一段：未压缩信封（解压后 == 压缩后）。压缩 / 解压是下一步。
 */

import { decodeInt, encodeInt } from './values.ts';

const u32 = (x: number): number => x >>> 0;

/** 反射 CRC-32（poly 0xEDB88320，init 0xFFFFFFFF，末步取反），按字节先后喂养。 */
export function crc32Reflected(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const b of bytes) {
    crc ^= b;
    for (let k = 0; k < 8; k += 1) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return u32(crc ^ 0xffffffff);
}

/** 非反射 CRC-32（poly 0x04C11DB7，init 0xFFFFFFFF，每字节 MSB 先，末步取反）。 */
export function crc32NonReflected(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (const b of bytes) {
    crc ^= b << 24;
    for (let k = 0; k < 8; k += 1) crc = ((crc & 0x80000000) ? ((crc << 1) ^ 0x04c11db7) : (crc << 1)) >>> 0;
  }
  return u32(crc ^ 0xffffffff);
}

/** 1→2 字变换（写侧）；`seed` / `key` 来自参数块第 4 / 第 5 槽。 */
export function transformWords(src: Uint32Array, seed: number, key: number): Uint32Array {
  const out = new Uint32Array(src.length * 2);
  let s = u32(seed);
  let k = u32(key);
  for (let i = 0; i < src.length; i += 1) {
    const x = u32(src[i]! ^ s);
    const t = k & 0xffff;
    out[2 * i] = Math.imul(x >>> 16, t) >>> 0;
    out[2 * i + 1] = Math.imul(x & 0xffff, t) >>> 0;
    s = u32(s + 0x0b0b0b0b);
    k = u32(k + 0x0b02);
  }
  return out;
}

/** 逆变换；余数不为 0 或除数为 0 ⇒ 抛（FMT-SAVE-CODEC 的唯一失败判据）。 */
export function untransformWords(src: Uint32Array, seed: number, key: number): Uint32Array {
  if (src.length % 2 !== 0) throw new Error('untransform: 密文字数必须为偶数');
  const out = new Uint32Array(src.length / 2);
  let s = u32(seed);
  let k = u32(key);
  for (let i = 0; i < out.length; i += 1) {
    const t = k & 0xffff;
    if (t === 0) throw new Error('untransform: 除数为 0（键为偶数）');
    const hi = src[2 * i]!;
    const lo = src[2 * i + 1]!;
    if (hi % t !== 0 || lo % t !== 0) throw new Error('untransform: 余数不为 0');
    const x = u32(((hi / t) << 16) | (lo / t));
    out[i] = u32(x ^ s);
    s = u32(s + 0x0b0b0b0b);
    k = u32(k + 0x0b02);
  }
  return out;
}

/**
 * 未压缩信封：`[解压后B][解压后B][压缩B][前缀...]` + body，再整体变换。
 * 返回完整文件参数块之后的那一段（20 字节参数块由调用方补）。
 */
export function buildUncompressedEnvelope(body: Uint8Array, seed: number, key: number): Uint8Array {
  if (body.length % 4 !== 0) throw new Error('save body 必须 4 字节对齐');
  const total = 12 + body.length;
  const prefix = new Uint8Array(total);
  const view = new DataView(prefix.buffer);
  view.setUint32(0, total, true);
  view.setUint32(4, total, true);
  view.setUint32(8, total, true);
  prefix.set(body, 12);
  const words = new Uint32Array(prefix.buffer);
  return new Uint8Array(transformWords(words, seed, key).buffer);
}

/** 一条**帧记录**里本层会写的四样（`FMT-SAVE-CODEC`；每条 261 个 32 位字，其余格本层不写） */
export interface SaveFrameRecord {
  /** `rec[0]` = **父帧号**（根帧 −1；⛔ 不是状态码） */
  parent: number;
  /** `rec[1]` = 脚本文件索引（脚本身份；没有 id ⇒ −1） */
  scriptIndex: number;
  /** `rec[2]` = 返回点条数 N，`rec[3 .. 3+N)` = N 个返回点位置下标（−1 = 无） */
  ret: number[];
  /** `rec[259]` = 位置 1 在**表 1**（消息显示 `0x71`）里的下标；−1 = 该项不存在 */
  tableA: number;
  /** `rec[260]` = 位置 2 在**表 2**（换脚本 `0x03`）里的下标；−1 = 该项不存在（当前帧强制 −1） */
  tableB: number;
}

/**
 * **纹理记录表**的一项（`FMT-SAVE-CODEC` 的 `+1252` 副块；每项 20 B = 5 个 u32）。
 *
 * ★ 逐格身份（顺序即落盘顺序）：`file` = 来源文件号、`color` = 颜色键、
 *   `flag0` = **恢复重建门**（读侧严格 `== 1` 才重建；⛔ 不是任意非 0）、
 *   `flag1` = 零读者标志（写侧照抄、本作没有读者）、`blank` = 空白创建标记。
 * ★ 写侧是**整段复制场景的备份表**（`0x258` / `0x259` 写的两张 1000 项表 + 纹理记录），
 *   ⛔ 不是"按需挑几条"。
 */
export interface SaveTextureRecord {
  file: number;
  color: number;
  flag0: number;
  flag1: number;
  blank: number;
}

export interface SaveSnapshot {
  frame: number;
  records: SaveFrameRecord[];
  ints: number[];
  floats: number[];
  strings: string[];
  version1: number;
  version2: number;
  /**
   * v1 = 3 的 **20000 B 纹理记录表**（1000 项，`+1252` 起；`FMT-SAVE-CODEC`）。
   * ★ 缺省 / 少于 1000 项 ⇒ 缺的位置**留 0**：那等于"门 = 0 ⇒ 一条纹理都不重建"，
   *   是**合法**文件（不是"没写这块"）—— 20000 B 始终存在，位置也不因此移动。
   * ★ `version1 !== 3` 时**整块不写**（也没有这 20000 B）。
   */
  textureRecords?: SaveTextureRecord[];
}

const writeU32 = (view: DataView, off: number, v: number): void => view.setUint32(off, v >>> 0, true);

/**
 * 按 `FMT-SAVE-CODEC` 构造主体（未压缩；尾段 N = 0）。
 *
 * ★ 六个块 = 六个**全局池**（局部池不在里面）：
 *   0 整型值池 / 1 浮点值池 / 2 字符串池 / 3 整型引用池 / 4 浮点引用池 / 5 字符串引用表。
 *   本实现对值族三池有模型 ⇒ 写 c0/c1/c2 + 块 0/1/2；引用三池尚未建模（读侧只按计数跳过）
 *   ⇒ `c3 = c4 = c5 = 0`、块 3/4/5 长度为 0。
 * ★ 浮点族两块（c1 / c4）的**内容**只在版本 2 ≥ 20 时写；`版本 1 = 2` 的容器**无论版本 2**
 *   都不写这两块内容（`FMT-SAVE-CODEC` 的两条门控）。计数与洞始终存在 ⇒
 *   "非 0 计数 + 未写空洞"是**合法**文件。
 * ★ `版本 1 = 3` 时 `+1252` 起还有 **20000 B 纹理记录表**（1000 × 20 B，五项见 `SaveTextureRecord`）；
 *   其它版本没有这 20000 B。**别的块偏移只由这一条决定**，记录内容不改变布局。
 */
export function buildSaveBody(s: SaveSnapshot, intKey: number): Uint8Array {
  const c0 = s.ints.length;
  const c1 = s.floats.length;
  const c2 = s.strings.length;
  const c3 = 0;
  const c4 = 0;
  const c5 = 0;
  const writesFloatContent = s.version1 !== 2 && s.version2 >= 20;
  const arena = new Uint8Array(s.strings.reduce((n, x) => n + x.length + 1, 0));
  let ap = 0;
  for (const str of s.strings) {
    for (let i = 0; i < str.length; i += 1) arena[ap++] = str.charCodeAt(i) & 0xff;
    arena[ap++] = 0;
  }
  const capWords = Math.ceil(arena.length / 4);
  // ★ v1=3 的 20000 B = **纹理记录表**（1000 × 20 B）：五项 = 来源文件号 / 颜色键 /
  //   恢复重建门 / 零读者标志 / 空白标记（`FMT-SAVE-CODEC`）。v1 ≠ 3 时整块不存在。
  const textureBase = 1252;
  const textureBytes = s.version1 === 3 ? 20000 : 0;
  const recBase = textureBase + textureBytes;
  const recCount = s.frame + 1;
  const countBase = recBase + 1044 * recCount;
  const blocksBase = countBase + 24;
  const block0 = 4 * c0;
  const block1 = 4 * c1;
  const block2 = 4 + 4 * capWords;
  // 引用三池的块长恒为 0（写侧不写内容、读侧只跳过）⇒ 现在是 block0 + block1 + block2。
  // ★ 将来启用引用池时**必须**在这里加上 `4 * (c3 + c4 + c5)` —— 那一项在 spec 的 tail 公式里，
  //   漏掉会让尾段整体前移（这正是本轮修掉的那处缺口）。
  const tailBase = blocksBase + block0 + block1 + block2;
  const tailLen = 4 + 4 + 2 * 4 + 740;
  // slack = `c2 + 16463 + 3N` 个 32 位字（N = 场景记录条数，本实现恒 0）
  const slackWords = c2 + 16463 + 3 * 0;
  const total = tailBase + tailLen + slackWords * 4;
  const out = new Uint8Array(total);
  const view = new DataView(out.buffer);
  writeU32(view, 0, s.frame);
  // 纹理记录表：`version1 == 3` 才存在；缺的位置留 0（= 门 0，一条都不重建），长度不变。
  if (s.version1 === 3) {
    const textureRecords = s.textureRecords ?? [];
    const n = Math.min(textureRecords.length, 1000);
    for (let i = 0; i < n; i += 1) {
      const rec = textureRecords[i]!;
      const off = textureBase + 20 * i;
      writeU32(view, off, rec.file);
      writeU32(view, off + 4, rec.color);
      writeU32(view, off + 8, rec.flag0);
      writeU32(view, off + 12, rec.flag1);
      writeU32(view, off + 16, rec.blank);
    }
  }
  for (let f = 0; f <= s.frame; f += 1) {
    const rec = s.records[f];
    if (!rec) continue;
    const off = recBase + 1044 * f;
    writeU32(view, off, rec.parent);
    writeU32(view, off + 4, rec.scriptIndex);
    writeU32(view, off + 8, rec.ret.length);
    for (let i = 0; i < rec.ret.length && i < 256; i += 1) writeU32(view, off + 12 + 4 * i, rec.ret[i]!);
    writeU32(view, off + 1036, rec.tableA);
    writeU32(view, off + 1040, rec.tableB);
  }
  writeU32(view, countBase, c0);
  writeU32(view, countBase + 4, c1);
  writeU32(view, countBase + 8, c2);
  writeU32(view, countBase + 12, c3);
  writeU32(view, countBase + 16, c4);
  writeU32(view, countBase + 20, c5);
  let p = blocksBase;
  // 块 0：全局整型池前缀 —— 写侧对每个值做一次编码
  for (const v of s.ints) { writeU32(view, p, encodeInt(v | 0, intKey)); p += 4; }
  // 块 1：全局浮点池前缀（raw 32 位）。内容被门控时**只留等长空洞**（全 0），块长仍是 4×c1
  if (writesFloatContent) for (const v of s.floats) { writeU32(view, p, v >>> 0); p += 4; }
  else p += block1;
  // 块 2：字符串段 —— `u32 capWords` + `capWords` 个字的 NUL 结尾字节池
  writeU32(view, p, capWords); p += 4;
  out.set(arena, p); p += 4 * capWords;
  // 块 3 / 4 / 5：c3 = c4 = c5 = 0 ⇒ 零长，紧接尾段
  writeU32(view, tailBase, 740);
  writeU32(view, tailBase + 4, 0);
  return out;
}

/** 完整文件：292 字节头 + 20 字节参数块 + 未压缩信封。 */
export function buildSaveFile(s: SaveSnapshot, intKey: number, seed: number, saveKey: number): Uint8Array {
  const body = buildSaveBody(s, intKey);
  const cipher = buildUncompressedEnvelope(body, seed, saveKey);
  const header = new Uint8Array(292);
  const hv = new DataView(header.buffer);
  header.set(new TextEncoder().encode('S4SD'), 0);
  header.set(new TextEncoder().encode('460B'), 4);
  header.set(new TextEncoder().encode('AMAYUI'), 8);
  hv.setUint32(240, body.length + 12, true);
  hv.setUint32(284, s.version1, true);
  hv.setUint32(288, s.version2, true);
  const params = buildParams(cipher, seed, saveKey);
  const out = new Uint8Array(292 + 20 + cipher.length);
  out.set(header, 0);
  out.set(params, 292);
  out.set(cipher, 312);
  return out;
}

export function buildParams(cipher: Uint8Array, seed: number, key: number): Uint8Array {
  if (cipher.length % 4 !== 0) throw new Error('密文必须 4 字节对齐');
  const out = new Uint8Array(20);
  const view = new DataView(out.buffer);
  view.setUint32(0, cipher.length / 4, true);
  view.setUint32(4, crc32NonReflected(cipher), true);
  view.setUint32(8, crc32Reflected(cipher), true);
  view.setUint32(12, seed >>> 0, true);
  view.setUint16(16, (key & 0xffff) >>> 0, true);
  return out;
}


/* ─────────────────────────────── 读侧：头 + 信封 ─────────────────────────────── */

export type SaveFormatErrorCode =
  | 'save-short'
  | 'save-magic'
  | 'save-title'
  | 'save-params'
  | 'save-cipher-length'
  | 'save-crc1'
  | 'save-crc2'
  | 'save-transform'
  | 'save-envelope-prefix';

export class SaveFormatError extends Error {
  readonly code: SaveFormatErrorCode;
  constructor(code: SaveFormatErrorCode, detail = '') {
    super(detail === '' ? code : code + ':' + detail);
    this.name = 'SaveFormatError';
    this.code = code;
  }
}

export interface ParsedSaveEnvelope {
  version1: number;
  version2: number;
  /** 头 +240 声明的**未变换**负载字节数（不参与边界判断，只回给调用方） */
  declaredBodyBytes: number;
  /** 未变换的负载本体（不含 12 字节信封前缀） */
  body: Uint8Array;
}

const u16 = (x: number): number => x & 0xffff;
const readU32At = (bytes: Uint8Array, off: number): number =>
  (bytes[off]! | (bytes[off + 1]! << 8) | (bytes[off + 2]! << 16) | (bytes[off + 3]! << 24)) >>> 0;

/**
 * 读侧第一段：292 字节头 + 20 字节参数块 + 变换信封。
 * ★ 顺序按 FMT-SAVE-ENVELOPE：头校验 → 参数块长度 → 两个校验和 → 逆变换 → 三个长度前缀。
 * ★ 只返回 body，不解释六个池 / 帧记录（那是 RESTORE 的下一步）。
 */
export function parseSaveEnvelope(bytes: Uint8Array): ParsedSaveEnvelope {
  if (bytes.length < 312) throw new SaveFormatError('save-short');
  const magic = String.fromCharCode(bytes[0]!, bytes[1]!, bytes[2]!, bytes[3]!);
  if (magic !== 'S4SD' && magic !== 'S3SD') throw new SaveFormatError('save-magic');
  const title = String.fromCharCode(bytes[8]!, bytes[9]!, bytes[10]!, bytes[11]!, bytes[12]!, bytes[13]!);
  if (title !== 'AMAYUI') throw new SaveFormatError('save-title');
  const version1 = readU32At(bytes, 284);
  const version2 = readU32At(bytes, 288);
  const declaredBodyBytes = readU32At(bytes, 240);
  const words = readU32At(bytes, 292);
  const crc1 = readU32At(bytes, 296);
  const crc2 = readU32At(bytes, 300);
  const seed = readU32At(bytes, 304);
  const key = u16(bytes[308]! | (bytes[309]! << 8));
  if (words === 0 || words % 2 !== 0) throw new SaveFormatError('save-params');
  const cipherBytes = words * 4;
  if (bytes.length < 312 + cipherBytes) throw new SaveFormatError('save-cipher-length');
  const cipher = bytes.subarray(312, 312 + cipherBytes);
  if (crc32NonReflected(cipher) !== crc1) throw new SaveFormatError('save-crc1');
  if (crc32Reflected(cipher) !== crc2) throw new SaveFormatError('save-crc2');
  let plain: Uint32Array;
  try {
    plain = untransformWords(new Uint32Array(cipher.buffer, cipher.byteOffset, cipher.byteLength / 4), seed, key);
  } catch (error) {
    throw new SaveFormatError('save-transform', error instanceof Error ? error.message : String(error));
  }
  if (plain.length < 3) throw new SaveFormatError('save-envelope-prefix');
  const total = plain[0]!;
  if (plain[1] !== total || plain[2] !== total) throw new SaveFormatError('save-envelope-prefix');
  const plainBytes = new Uint8Array(plain.buffer);
  if (total !== plainBytes.length || total < 12) throw new SaveFormatError('save-envelope-prefix');
  return { version1, version2, declaredBodyBytes, body: plainBytes.subarray(12) };
}


/* ─────────────────────────────── 读侧：负载主体 ─────────────────────────────── */

export interface ParsedSaveBody {
  frame: number;
  records: SaveFrameRecord[];
  ints: number[];
  floats: number[];
  strings: string[];
  textureRecords: SaveTextureRecord[];
}

const readU32 = (bytes: Uint8Array, off: number): number =>
  (bytes[off]! | (bytes[off + 1]! << 8) | (bytes[off + 2]! << 16) | (bytes[off + 3]! << 24)) >>> 0;

/**
 * 读侧第二段：负载主体的**结构**（六池计数 + 帧记录 + 纹理记录表）。
 * ★ 只解析，不应用（帧 / 池的还原仍属 RESTORE）；引用三池（c3/c4/c5）只按计数跳过。
 * ★ 版本 1 = 3 才有 20000 B 纹理记录表；其它版本整块不存在。
 */
export function parseSaveBody(body: Uint8Array, version1: number, intKey: number): ParsedSaveBody {
  if (body.length < 4) throw new SaveFormatError('save-short', 'body');
  const frame = readU32(body, 0) | 0;
  if (frame < 0 || frame > 39) throw new SaveFormatError('save-params', 'frame=' + frame);
  const textureBytes = version1 === 3 ? 20000 : 0;
  const textureBase = 1252;
  const recBase = textureBase + textureBytes;
  const recCount = frame + 1;
  const countBase = recBase + 1044 * recCount;
  const blocksBase = countBase + 24;
  if (body.length < blocksBase) throw new SaveFormatError('save-short', 'records');

  const textureRecords: SaveTextureRecord[] = [];
  if (version1 === 3) {
    for (let i = 0; i < 1000; i += 1) {
      const off = textureBase + 20 * i;
      textureRecords.push({
        file: readU32(body, off) | 0,
        color: readU32(body, off + 4) | 0,
        flag0: readU32(body, off + 8) | 0,
        flag1: readU32(body, off + 12) | 0,
        blank: readU32(body, off + 16) | 0,
      });
    }
  }

  const records: SaveFrameRecord[] = [];
  for (let f = 0; f < recCount; f += 1) {
    const off = recBase + 1044 * f;
    const n = Math.min(readU32(body, off + 8) | 0, 256);
    if (n < 0) throw new SaveFormatError('save-params', 'ret=' + n);
    const ret: number[] = [];
    for (let i = 0; i < n; i += 1) ret.push(readU32(body, off + 12 + 4 * i) | 0);
    records.push({
      parent: readU32(body, off) | 0,
      scriptIndex: readU32(body, off + 4) | 0,
      ret,
      tableA: readU32(body, off + 1036) | 0,
      tableB: readU32(body, off + 1040) | 0,
    });
  }

  const c0 = readU32(body, countBase) | 0;
  const c1 = readU32(body, countBase + 4) | 0;
  const c2 = readU32(body, countBase + 8) | 0;
  const c3 = readU32(body, countBase + 12) | 0;
  const c4 = readU32(body, countBase + 16) | 0;
  const c5 = readU32(body, countBase + 20) | 0;
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0) throw new SaveFormatError('save-params', 'counts');
  let p = blocksBase;
  if (body.length < p + 4 * c0 + 4 * c1 + 4 + 4 * c3 + 4 * c4 + 4 * c5) throw new SaveFormatError('save-short', 'blocks');
  const ints: number[] = [];
  for (let i = 0; i < c0; i += 1) { ints.push(decodeInt(readU32(body, p), intKey)); p += 4; }
  const floats: number[] = [];
  for (let i = 0; i < c1; i += 1) { floats.push(readU32(body, p)); p += 4; }
  const capWords = readU32(body, p) | 0; p += 4;
  if (capWords < 0 || body.length < p + 4 * capWords) throw new SaveFormatError('save-short', 'strings');
  const arena = body.subarray(p, p + 4 * capWords);
  p += 4 * capWords;
  const strings: string[] = [];
  let start = 0;
  for (let i = 0; i < arena.length && strings.length < c2; i += 1) {
    if (arena[i] !== 0) continue;
    let s = '';
    for (let j = start; j < i; j += 1) s += String.fromCharCode(arena[j]!);
    strings.push(s);
    start = i + 1;
  }
  // 引用三池只按计数跳过（本实现不解释内容）
  p += 4 * (c3 + c4 + c5);
  if (p > body.length) throw new SaveFormatError('save-short', 'tail');
  return { frame, records, ints, floats, strings, textureRecords };
}
