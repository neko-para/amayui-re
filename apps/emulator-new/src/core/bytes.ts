/**
 * 字节原语（**核心层**）：把原先散在 `Buffer` 上的操作集中成一小份平台无关的实现。
 *
 * ★ 为什么不用 `TextDecoder` 做 latin1：WHATWG 的 `latin1` 标签是 **windows-1252** 的别名 ——
 *   0x80–0x9F 会被映射成 `€‚ƒ…` 之类，**不是**"一字节一字符"。而本层的口径（`55-config.md` /
 *   ALF 名字 / 内联字符串）是**字节串**：`Buffer.toString('latin1')` 的语义是 `U+0000–U+00FF`
 *   与字节一一对应。⇒ 这里手写这一条（纯查表，不是 polyfill），`TextDecoder` 留给真正的多字节文本。
 * ★ `readU32LE` 等按 `Buffer.readUInt32LE` 的语义写：越界**响亮失败**（不静默返回 0）。
 */

/** 分配一段零初始化字节（等价 `Buffer.alloc(n)`） */
export function allocBytes(length: number): Uint8Array {
  return new Uint8Array(length);
}

/** latin1 / 字节串解码：`U+0000–U+00FF` 与字节一一对应（⛔ 不是 windows-1252） */
export function decodeLatin1(bytes: Uint8Array, start = 0, end = bytes.length): string {
  const CHUNK = 0x2000;
  let out = '';
  for (let at = start; at < end; at += CHUNK) {
    const stop = at + CHUNK < end ? at + CHUNK : end;
    const codes: number[] = [];
    for (let i = at; i < stop; i += 1) codes.push(bytes[i]!);
    out += String.fromCharCode(...codes);
  }
  return out;
}

/** latin1 / 字节串编码：取每个码元的低 8 位（等价 `Buffer.from(s, 'latin1')`，含 >0xFF 的截断） */
export function encodeLatin1(text: string): Uint8Array {
  const out = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i += 1) out[i] = text.charCodeAt(i) & 0xff;
  return out;
}

/** 小端 u32 读（越界 ⇒ 抛，不补 0） */
export function readU32LE(bytes: Uint8Array, offset: number): number {
  if (!Number.isInteger(offset) || offset < 0 || offset + 4 > bytes.length) {
    throw new Error(`read-u32-out-of-range:${offset}/${bytes.length}`);
  }
  return (bytes[offset]! | (bytes[offset + 1]! << 8) | (bytes[offset + 2]! << 16) | (bytes[offset + 3]! << 24)) >>> 0;
}

/** 小端 i32 读（越界 ⇒ 抛，不补 0） */
export function readI32LE(bytes: Uint8Array, offset: number): number {
  return readU32LE(bytes, offset) | 0;
}

/** 找某个字节（等价 `Buffer.indexOf(value, from)`；找不到 ⇒ −1） */
export function indexOfByte(bytes: Uint8Array, value: number, from = 0): number {
  for (let i = from < 0 ? 0 : from; i < bytes.length; i += 1) {
    if (bytes[i] === value) return i;
  }
  return -1;
}
