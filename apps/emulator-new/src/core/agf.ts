/**
 * AGF（ACGF）解码：`data/spec/chapters/30-agf.md` 的读取侧。
 *
 * ★ 纯核心：只用 `Uint8Array` / `DataView`，不 import `node:*`、`Buffer`、DOM。
 * ★ 输出是 **top-down RGBA8**（长度 = `width * height * 4`）：盘上的 bottom-up、BGR(A) 都在这里翻转 / 换序。
 * ★ 这是**引擎协议**，不是平台能力；Electron / CLI / 测试共用这一份，⛔ 不 import 外部格式包。
 * ★ 失败一律抛 `AgfError`（带稳定的 code）；调用方把它转成显示消息型异常或诊断。
 */
import { allocBytes, readU16LE, readU32LE } from './bytes.ts';
import { decompressLzss } from './lzss.ts';

export type AgfErrorCode =
  | 'agf-too-short'
  | 'agf-version'
  | 'agf-unknown0'
  | 'agf-meta-size'
  | 'agf-body-size'
  | 'agf-acif-missing'
  | 'agf-acif-version'
  | 'agf-acif-reserved'
  | 'agf-acif-size'
  | 'agf-acif-dimensions'
  | 'agf-alpha-size'
  | 'agf-bpp'
  | 'agf-zero-size'
  | 'agf-truncated'
  | 'agf-bmp-unsupported';

export class AgfError extends Error {
  readonly code: AgfErrorCode;

  constructor(code: AgfErrorCode, detail = '') {
    super(detail === '' ? code : `${code}:${detail}`);
    this.name = 'AgfError';
    this.code = code;
  }
}

export interface DecodedAgf {
  width: number;
  height: number;
  bpp: number;
  /** top-down RGBA8；长度 = width * height * 4 */
  rgba: Uint8Array;
}

/** 按规范取一行在 body 里的字节数：**32 位（4 字节）对齐** */
export function agfStride(width: number, bpp: number): number {
  return 4 * Math.ceil((bpp * width) / 32);
}

function slice(bytes: Uint8Array, offset: number, length: number, code: AgfErrorCode): Uint8Array {
  if (offset < 0 || length < 0 || offset + length > bytes.length) {
    throw new AgfError(code, `${offset}+${length}>${bytes.length}`);
  }
  return bytes.subarray(offset, offset + length);
}

function readSection(
  bytes: Uint8Array,
  offset: number,
  unpacked: number,
  packed: number,
  code: AgfErrorCode,
): Uint8Array {
  if (!Number.isInteger(unpacked) || !Number.isInteger(packed) || unpacked < 0 || packed < 0) {
    throw new AgfError(code, `unpacked=${unpacked} packed=${packed}`);
  }
  const raw = slice(bytes, offset, packed, code);
  if (unpacked === packed) return raw;
  try {
    return decompressLzss(raw, unpacked);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new AgfError(code, `lzss:${message}`);
  }
}

function magicAt(bytes: Uint8Array, offset: number, magic: string): boolean {
  if (offset < 0 || offset + magic.length > bytes.length) return false;
  for (let i = 0; i < magic.length; i += 1) {
    if (bytes[offset + i] !== magic.charCodeAt(i)) return false;
  }
  return true;
}

/**
 * 解码一份 AGF。`BM` 开头的文件是引擎的 BMP 分支（本版没有 BMP 规范）⇒ 明确失败，
 * ⛔ 不拿平台图像库去猜 BMP。
 */
export function decodeAgf(bytes: Uint8Array): DecodedAgf {
  if (bytes.length < 24) throw new AgfError('agf-too-short', `${bytes.length}`);
  if (bytes[0] === 0x42 && bytes[1] === 0x4d) throw new AgfError('agf-bmp-unsupported');
  // ★ 前 4 字节不参与判定：引擎不比较 `ACGF` 签名；真实样本 `SO006.AGF` 的头 4 字节是 0，
  //   版本 / unknown0 在 +4 / +8，仍被引擎接受。

  const version = readU32LE(bytes, 4);
  if (version !== 1 && version !== 2) throw new AgfError('agf-version', `${version}`);
  if (readU32LE(bytes, 8) !== 0) throw new AgfError('agf-unknown0');

  const metaUnpacked = readU32LE(bytes, 12);
  if (readU32LE(bytes, 16) !== metaUnpacked) throw new AgfError('agf-meta-size', 'repeated');
  const metaPacked = readU32LE(bytes, 20);
  const meta = readSection(bytes, 24, metaUnpacked, metaPacked, 'agf-meta-size');
  if (meta.length < 32) throw new AgfError('agf-meta-size', `decoded=${meta.length}`);

  const width = readU32LE(meta, 20);
  const height = readU32LE(meta, 24);
  if (width === 0 || height === 0) throw new AgfError('agf-zero-size', `${width}x${height}`);
  const bpp = readU16LE(meta, 30);
  if (bpp !== 8 && bpp !== 24 && bpp !== 32) throw new AgfError('agf-bpp', `${bpp}`);

  const bodyHeader = 24 + metaPacked;
  slice(bytes, bodyHeader, 12, 'agf-body-size');
  const bodyUnpacked = readU32LE(bytes, bodyHeader + 4);
  const bodyPacked = readU32LE(bytes, bodyHeader + 8);
  const body = readSection(bytes, bodyHeader + 12, bodyUnpacked, bodyPacked, 'agf-body-size');

  const stride = agfStride(width, bpp);
  const need = stride * height;
  if (body.length !== need) throw new AgfError('agf-body-size', `body=${body.length} need=${need}`);

  let alpha: Uint8Array | null = null;
  if (version === 2) {
    const acif = bodyHeader + 12 + bodyPacked;
    if (!magicAt(bytes, acif, 'ACIF')) throw new AgfError('agf-acif-missing');
    slice(bytes, acif, 36, 'agf-acif-size');
    const acifVersion = readU32LE(bytes, acif + 4);
    if (acifVersion !== 1 && acifVersion !== 2) throw new AgfError('agf-acif-version', `${acifVersion}`);
    if (readU32LE(bytes, acif + 8) !== 0) throw new AgfError('agf-acif-reserved');
    const alphaUnpacked = readU32LE(bytes, acif + 12);
    if (readU32LE(bytes, acif + 16) !== width || readU32LE(bytes, acif + 20) !== height) {
      throw new AgfError('agf-acif-dimensions');
    }
    if (readU32LE(bytes, acif + 24) !== alphaUnpacked || readU32LE(bytes, acif + 28) !== alphaUnpacked) {
      throw new AgfError('agf-acif-size', 'repeated');
    }
    const alphaPacked = readU32LE(bytes, acif + 32);
    alpha = readSection(bytes, acif + 36, alphaUnpacked, alphaPacked, 'agf-acif-size');
    if (alpha.length !== width * height) {
      throw new AgfError('agf-alpha-size', `alpha=${alpha.length} need=${width * height}`);
    }
  }

  // 调色板：meta+56 起的一个 1024 字节槽，初值全 0，meta 覆盖该槽。
  const palette = allocBytes(1024);
  if (bpp === 8) {
    if (meta.length < 56) throw new AgfError('agf-meta-size', `palette@${meta.length}`);
    const n = Math.min(1024, meta.length - 56);
    palette.set(meta.subarray(56, 56 + n), 0);
  }

  const rgba = allocBytes(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    const srcRow = (height - 1 - y) * stride;
    for (let x = 0; x < width; x += 1) {
      const d = (y * width + x) * 4;
      if (bpp === 8) {
        const index = body[srcRow + x]!;
        const p = index * 4;
        rgba[d] = palette[p + 2]!;
        rgba[d + 1] = palette[p + 1]!;
        rgba[d + 2] = palette[p]!;
        rgba[d + 3] = alpha ? alpha[y * width + x]! : 255;
      } else if (bpp === 24) {
        const s = srcRow + x * 3;
        rgba[d] = body[s + 2]!;
        rgba[d + 1] = body[s + 1]!;
        rgba[d + 2] = body[s]!;
        rgba[d + 3] = alpha ? alpha[y * width + x]! : 255;
      } else {
        const s = srcRow + x * 4;
        rgba[d] = body[s + 2]!;
        rgba[d + 1] = body[s + 1]!;
        rgba[d + 2] = body[s]!;
        // ★ **引擎已观察行为**：32bpp 的实现只取像素的**第 4 字节**，不读 ACIF alpha 段
        //   ⇒ 这里也只用 `body[s + 3]`。`30-agf.md` 的 FMT-AGF-PIXELS 已按同一结论写成合同
        //   （alpha 段仍被装载 / 解码，但 32bpp 下没有消费者）。
        rgba[d + 3] = body[s + 3]!;
      }
    }
  }

  return { width, height, bpp, rgba };
}

/**
 * 颜色键：把 RGBA 里**整条 ARGB 等于键**的像素写成 0（透明黑）。
 *
 * ★ 为什么必须比 alpha（本轮缺陷的根因）：引擎在上传期按 **32 位像素值**比较，A 字节参与。
 *   逐字证据（`sub_48B090`，32bpp 分支 `0x48BEC0` 起）：先由源字节装配
 *   `(A<<24) | (R<<16) | (G<<8) | B`，再 `cmp ecx, [ebp+var_43C]`（`0x48BF2F`）；
 *   `var_43C` 是 `0x1f9` 归一化后的键。24bpp 走 `0x48C040`，有 alpha 段时同样读 A；
 *   8bpp 的调色板在装配时把 A 固定成 0xFF，再与键比较（`0x48B3BB`）——
 *   所以"键 0"在三条路径上都**不会**命中不透明像素。
 * ★ 键 0 = **无键**：`0x1f9` 的操作数 < 0 ⇒ 归一化成 0（TITLE 的 `set-texture 5272 4 (local-int 40d)`
 *   前一条 `sub (local-int 40d) 0 3` 就是负值）。只有非负操作数才归一化成
 *   `0xFF000000 | (值 & 0xFFFFFF)`；后者才清"不透明黑"。
 * ★ 装配顺序是 **0xAARRGGBB**（A/R/G/B），⛔ 不要漏掉 A 或把通道顺序换掉：
 *   漏掉 A 会把 alpha=255 的黑色描边当成键 0 清成洞（正是本轮"破裂"的成因）。
 */
/**
 * 这个"颜色键操作数"到底是不是一个键。
 *
 * ★ 三个哨兵都**不是键**：`0`（无键）、`-1`（调用方用有符号数表示"无键"，例如影片帧上传）。
 *   只有**正数**才当键用；否则 `-1` 会被 `>>> 0` 变成 `0xFFFFFFFF`，
 *   把影片里所有纯白像素（包括 logo 周围的白底）都清成透明 —— 表现为白闪 / logo 边缘黑噪点。
 */
export function hasColorKey(color: number): boolean {
  return color > 0;
}

export function applyColorKey(rgba: Uint8Array, colorKey: number): void {
  if ((rgba.length & 3) !== 0) throw new Error(`rgba-length-not-multiple-of-4:${rgba.length}`);
  const key = colorKey >>> 0;
  for (let i = 0; i < rgba.length; i += 4) {
    const argb = ((rgba[i + 3]! << 24) | (rgba[i]! << 16) | (rgba[i + 1]! << 8) | rgba[i + 2]!) >>> 0;
    if (argb === key) {
      rgba[i] = 0;
      rgba[i + 1] = 0;
      rgba[i + 2] = 0;
      rgba[i + 3] = 0;
    }
  }
}
