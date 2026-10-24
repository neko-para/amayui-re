/**
 * 存档负载编码的**纯函数**：两个 CRC-32、1→2 字变换、未压缩信封。
 *
 * ★ 合同来自 `65-save` 的 `FMT-SAVE-CODEC`；本文件不碰文件系统、不认识槽。
 * ★ 只实现“可写回”的第一段：未压缩信封（解压后 == 压缩后）。压缩 / 解压是下一步。
 */

import { encodeInt } from './values.ts';

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

/** 20 字节参数块：密文字数 / crc1 / crc2 / seed / key(低 16 位，奇数)。 */
export interface SaveFrameRecord {
  status: number;
  scriptIndex: number;
  ret: number[];
  tableA: number;
  tableB: number;
}

export interface SaveSnapshot {
  frame: number;
  records: SaveFrameRecord[];
  ints: number[];
  floats: number[];
  strings: string[];
  version1: number;
  version2: number;
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
  const recBase = 1252 + (s.version1 === 3 ? 20000 : 0);
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
  for (let f = 0; f <= s.frame; f += 1) {
    const rec = s.records[f];
    if (!rec) continue;
    const off = recBase + 1044 * f;
    writeU32(view, off, rec.status);
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
