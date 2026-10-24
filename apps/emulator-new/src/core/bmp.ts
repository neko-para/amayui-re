/**
 * BMP 容器：`FMT-SAVE-STH`（`data/spec/chapters/65-save.md`）的**写侧组装 / 读侧解析**。
 *
 * ★ 这是**引擎协议**，不是平台能力：平台只交出「某张表面的像素 + 行宽 + 通道掩码」
 *   （`SurfacePixels`），两段头、行宽对齐、自下而上的翻转、通道重排全在这里做。
 *   ⛔ 平台的 `captureTexture` 不许返回整份 BMP 字节，`0x1af` 也不许把整份字节转给平台。
 * ★ 纯核心：只用 `Uint8Array` + 纯算术；不 import `node:*` / `Buffer` / DOM
 *   （`tsconfig.core.json` 的 `types: []` 是这条的真守卫）。
 * ★ 本版只建模**24 位**（`biBitCount = 24`，`BI_RGB`）：本作生效的是屏幕捕捉路径
 *   （`set:DrawMode` 缺省 0）⇒ 写出的位深恒 24；读侧 1 / 4 / 8 / 32 位的像素展开属
 *   `RES-SAVE-STH-DECODE`（未建模），这里**明确报错**，⛔ 不静默当成 24 位解。
 * ★ 32 位写变体（`set:DrawMode == 1` 的纹理捕捉）在本作**不可达**且**零样本**
 *   ⇒ 不实现（扩展点是一个独立的 encode，⛔ 不要在这里加"猜"出来的 4 字节分支）。
 * ★ 异常只带稳定 code（`BmpError`），调用方转诊断 + 状态码。
 */

import { allocBytes, readI32LE, readU16LE } from './bytes.ts';

/** 两段头长度（文件头 14 + 信息头 40）：也是像素数据起始（`FMT-SAVE-STH`：`+10 = 54`）。 */
const HEADER_SIZE = 54;
const INFO_HEADER_SIZE = 40;

export type BmpErrorCode =
  /** 连两段头都不够（< 54 字节） */
  | 'bmp-header-short'
  /** 头两字节不是 `BM` */
  | 'bmp-not-bm'
  /** 位深不在建模集合里（本版只有 24） */
  | 'bmp-bitdepth-unmodeled'
  /** 宽 / 高不是正数（自下而上的 24 位图要求正高） */
  | 'bmp-dimensions'
  /** 像素块短读（文件比"行宽 × 高"短） */
  | 'bmp-short-read'
  /** 待编码表面的宽 / 高不合法 */
  | 'bmp-surface-dimensions'
  /** 行宽 < 宽 × 每像素字节数 */
  | 'bmp-surface-stride'
  /** 每像素字节数不在 {2,3,4} */
  | 'bmp-surface-bytes-per-pixel'
  /** 通道掩码为 0 或超出该像素宽度 */
  | 'bmp-surface-masks'
  /** 像素缓冲比"行宽 × 高"短 */
  | 'bmp-surface-short';

export class BmpError extends Error {
  readonly code: BmpErrorCode;

  constructor(code: BmpErrorCode, detail = '') {
    super(detail === '' ? code : `${code}:${detail}`);
    this.name = 'BmpError';
    this.code = code;
  }
}

/** 源表面的通道掩码（来自该表面的像素格式）；24 位源没有 alpha 掩码。 */
export interface SurfaceMasks {
  r: number;
  g: number;
  b: number;
  a?: number;
}

/**
 * 平台交回的**原始表面像素**（一次捕捉的结果）。
 *
 * ★ `pixels` 是**行优先、自上而下**的源布局：平台不翻转、不换序（翻转 / 换序是引擎的事）。
 * ★ `stride` 是**平台报的行宽**（含行末填充），必须 ≥ `width × bytesPerPixel`。
 * ★ `bytesPerPixel` 不能由掩码推出来：屏幕路径取**显示位深 / 8**（2 / 3 / 4），
 *   纹理路径取源格式位宽 ⇒ 必须由平台报（引擎也是单独记这一格）。
 */
export interface BmpSurface {
  pixels: Uint8Array;
  width: number;
  height: number;
  stride: number;
  bytesPerPixel: number;
  masks: SurfaceMasks;
}

/** 解码结果：**自上而下** RGBA8（与 `decodeAgf` 的输出口径一致）。 */
export interface DecodedBmp {
  width: number;
  height: number;
  /** top-down RGBA8；长度 = `width × height × 4` */
  rgba: Uint8Array;
}

/** 行宽（字节）：`位深 × 宽` 按 **32 位（4 字节）** 对齐（`FMT-SAVE-STH` 的行宽规则）。 */
export function bmpRowStride(width: number, bitCount: number): number {
  return 4 * Math.ceil((bitCount * width) / 32);
}

function isBmp(bytes: Uint8Array): boolean {
  return bytes[0] === 0x42 && bytes[1] === 0x4d;
}

function writeU16LE(bytes: Uint8Array, offset: number, value: number): void {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
}

function writeU32LE(bytes: Uint8Array, offset: number, value: number): void {
  bytes[offset] = value & 0xff;
  bytes[offset + 1] = (value >>> 8) & 0xff;
  bytes[offset + 2] = (value >>> 16) & 0xff;
  bytes[offset + 3] = (value >>> 24) & 0xff;
}

/** 掩码尾零数 = 引擎的"右移位"（`0x43B390 shr eax,1` 那一段数出来的） */
function trailingZeros(mask: number): number {
  let shift = 0;
  while (shift < 32 && ((mask >>> shift) & 1) === 0) shift += 1;
  return shift;
}

/** 掩码里的位数（连续掩码时就是通道位宽） */
function bitsOf(mask: number): number {
  let bits = 0;
  for (let value = mask >>> 0; value !== 0; value >>>= 1) bits += value & 1;
  return bits;
}

/**
 * 一个源通道 → 8 位输出值：先按掩码取位、右移尾零数，再左移补满 8 位。
 *
 * ★ 24 / 32 位源的通道就是 8 位 ⇒ 左移 0、原样搬运（`FMT-SAVE-STH`：「不缩放」）。
 * ★ 16 位源（`bytesPerPixel == 2`）的 5 / 6 位通道各补 3 / 2 位 —— 这是引擎按掩码位宽算出的
 *   左移量（`0x43B3A4`–`0x43B3B2`：5 位 ⇒ 3、6 位 ⇒ 2；这两档之外本作不可达）。
 *   ⛔ 这里**不**声称与真机逐像素相同：可达输入是 24 位源（样本全是 24 位源），16 位那档没有样本。
 */
function channelValue(pixel: number, mask: number): number {
  const shift = trailingZeros(mask);
  const bits = bitsOf(mask);
  const scale = bits >= 8 ? 0 : 8 - bits;
  return (((pixel & mask) >>> shift) << scale) & 0xff;
}

function validMask(mask: number, limit: number): boolean {
  return Number.isInteger(mask) && mask > 0 && mask < limit;
}

function assertSurface(surface: BmpSurface): void {
  const { width, height, stride, bytesPerPixel, pixels, masks } = surface;
  if (!Number.isInteger(width) || width <= 0 || !Number.isInteger(height) || height <= 0) {
    throw new BmpError('bmp-surface-dimensions', `${width}x${height}`);
  }
  if (!Number.isInteger(bytesPerPixel) || bytesPerPixel < 2 || bytesPerPixel > 4) {
    throw new BmpError('bmp-surface-bytes-per-pixel', `${bytesPerPixel}`);
  }
  const rowBytes = width * bytesPerPixel;
  if (!Number.isInteger(stride) || stride < rowBytes) {
    throw new BmpError('bmp-surface-stride', `${stride}<${rowBytes}`);
  }
  if (pixels.length < stride * height) {
    throw new BmpError('bmp-surface-short', `${pixels.length}<${stride * height}`);
  }
  const limit = 2 ** (8 * bytesPerPixel);
  if (!validMask(masks.r, limit) || !validMask(masks.g, limit) || !validMask(masks.b, limit)) {
    throw new BmpError('bmp-surface-masks', `r=${masks.r} g=${masks.g} b=${masks.b} bpp=${bytesPerPixel}`);
  }
}

/**
 * 把一份表面像素组装成 24 位 BMP（`FMT-SAVE-STH` 的**写侧**）。
 *
 * 逐字段照 `FMT-SAVE-STH` 写（⛔ 不是照 BMP 教科书）：
 * - `+0` `BM`；`+10` 像素起点 = 54；`+14` `biSize` = 40；
 * - `+18` / `+22` 宽 / 高（正数，取自表面自己报的尺寸）；
 * - `+26` 平面数 = 1；`+28` 位深 = 24；`+30` 压缩 = 0；`+34` `biSizeImage` = 0；
 * - **`+2` 声明的文件长度 = 行宽 × 高 + 40**：★ 比真实长度（54 + 像素字节数）**少 14 字节**，
 *   这是引擎写侧的行为（真机四份样本：声明 172840 / 实际 172854）。⛔ **不许"修正"成 +54**：
 *   读侧从不比较该字段，但写侧必须照写，否则产物与真机不是逐字节相同；
 * - 像素块 = 对齐行宽 × 高，**整块零初始化**后只覆写有效像素 ⇒ 行末填充字节恒 0；
 * - 行序**自下而上**：源第 `r` 行 → 文件第 `height-1-r` 行；
 * - 每像素依次写 **蓝、绿、红**（与源表面的掩码顺序无关）。
 *
 * 失败一律抛 `BmpError`（调用方转诊断 + 状态 2）；⛔ 不静默裁剪 / 不补 0 冒充有效像素。
 */
export function encodeBmp24(surface: BmpSurface): Uint8Array {
  assertSurface(surface);
  const { pixels, width, height, stride, bytesPerPixel, masks } = surface;

  const rowStride = bmpRowStride(width, 24);
  const pixelBytes = rowStride * height;
  const out = allocBytes(HEADER_SIZE + pixelBytes);

  // ── 14 字节文件头 ──
  out[0] = 0x42; // 'B'
  out[1] = 0x4d; // 'M'
  writeU32LE(out, 2, pixelBytes + 40); // ★ 声明长度（刻意比实际短 14；见函数注释）
  // +6 / +8 保留字段 = 0（零初始化）
  writeU32LE(out, 10, HEADER_SIZE); // 像素数据起始 = 两段头长

  // ── 40 字节信息头 ──
  writeU32LE(out, 14, INFO_HEADER_SIZE); // biSize
  writeU32LE(out, 18, width);
  writeU32LE(out, 22, height);
  writeU16LE(out, 26, 1); // 平面数
  writeU16LE(out, 28, 24); // 位深（本作恒 24）
  // +30 压缩 / +34 biSizeImage / +38 / +42 每米像素 / +46 / +50 调色板：全 0（零初始化）

  // ── 像素块：自下而上、每像素 蓝 绿 红；行末填充保持 0 ──
  for (let row = 0; row < height; row += 1) {
    const srcRow = row * stride;
    let dst = HEADER_SIZE + (height - 1 - row) * rowStride;
    for (let x = 0; x < width; x += 1) {
      const at = srcRow + x * bytesPerPixel;
      let pixel = 0;
      for (let byte = 0; byte < bytesPerPixel; byte += 1) pixel |= pixels[at + byte]! << (byte * 8);
      out[dst] = channelValue(pixel, masks.b);
      out[dst + 1] = channelValue(pixel, masks.g);
      out[dst + 2] = channelValue(pixel, masks.r);
      dst += 3;
    }
  }

  return out;
}

/**
 * 位深探测：头够长且是 `BM` 才返回值；否则 `null`。
 *
 * ★ 只读 `+28`，不判位深集合（那是调用方的事）：这样"头不齐 / 非 BMP"与
 *   "位深不在建模集合里"是**两件可区分的事**（前者 `null`，后者 1 / 4 / 8 / 32）。
 */
export function bmpBitDepth(bytes: Uint8Array): number | null {
  // 最少要读到 +28 那两个字节（+28/+29）：30 字节
  if (bytes.length < 30 || !isBmp(bytes)) return null;
  return readU16LE(bytes, 28);
}

/**
 * 解析一份 24 位 BMP（`FMT-SAVE-STH` 的**读侧**）。
 *
 * 只按合同读：两段头齐 + `BM` + 位深 = 24；像素字节数按「宽 × 位深按 32 位对齐 × 高」**算出**，
 * **不读** `biSizeImage`、**不比较**声明的文件长度、**不看**压缩与平面数（引擎的读取器零读者）。
 *
 * - 位深 ≠ 24 ⇒ `bmp-bitdepth-unmodeled`（与调用方的状态码分开：1/4/8/32 是**本版未建模**，不是文件坏了）；
 * - 像素块短读 ⇒ `bmp-short-read`：本版**显式失败**。引擎只看每次读取调用的布尔结果、不比较字节数
 *   （`FMT-SAVE-STH`：「短读按成功继续」），实现不复刻宿主残留内存 ⇒ 记 `RES-SAVE-STH-SHORTREAD` 的
 *   实现口径（显式报错），⛔ 不把"短读判失败"说成引擎合同。
 */
export function decodeBmp(bytes: Uint8Array): DecodedBmp {
  if (bytes.length < HEADER_SIZE) {
    throw new BmpError('bmp-header-short', `${bytes.length}<${HEADER_SIZE}`);
  }
  if (!isBmp(bytes)) throw new BmpError('bmp-not-bm');

  const bitCount = readU16LE(bytes, 28);
  if (bitCount !== 24) throw new BmpError('bmp-bitdepth-unmodeled', `${bitCount}`);

  // 高是正数 ⇒ 自下而上。负高（自上而下）不在本版：引擎写侧恒写正数、样本也全是正数。
  const width = readI32LE(bytes, 18);
  const height = readI32LE(bytes, 22);
  if (width <= 0 || height <= 0) throw new BmpError('bmp-dimensions', `${width}x${height}`);

  const rowStride = bmpRowStride(width, 24);
  const need = HEADER_SIZE + rowStride * height;
  if (bytes.length < need) throw new BmpError('bmp-short-read', `${bytes.length}<${need}`);

  const rgba = allocBytes(width * height * 4);
  for (let row = 0; row < height; row += 1) {
    const srcBase = HEADER_SIZE + (height - 1 - row) * rowStride;
    for (let x = 0; x < width; x += 1) {
      const s = srcBase + x * 3;
      const d = (row * width + x) * 4;
      rgba[d] = bytes[s + 2]!; // R
      rgba[d + 1] = bytes[s + 1]!; // G
      rgba[d + 2] = bytes[s]!; // B
      rgba[d + 3] = 0xff; // 24 位源没有 alpha ⇒ 不透明
    }
  }

  return { width, height, rgba };
}
