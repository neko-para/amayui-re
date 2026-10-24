/** @env pure @kind contract @why `.STH` 的 BMP 容器在 core（`FMT-SAVE-STH`）：声明长度差 14 / 行宽对齐 / 自下而上 / 行末填充归零 / 位深不在建模集合要明确报错；任何一条写错都只产出"看起来像 BMP"的坏文件，读回来才发现 */
/**
 * tools/test/emulator-new-bmp.test.mjs —— `core/bmp.ts` 的纯守卫（`FMT-SAVE-STH` 的写 / 读两侧）
 *
 * ★ 为什么这些判据（每条都能静默错）：
 *   - **声明的文件长度 = 行宽 × 高 + 40**（比真实长度少 14）：把它"修正"成 +54，产物与真机不是逐字节相同；
 *   - **行宽按 32 位对齐**、**行末填充 = 0**：宽不是 4 的倍数时才会出现填充；写错就多出脏字节；
 *   - **自下而上 + 每像素 蓝、绿、红**：写反了图还是"能解"的，只是上下颠倒 / 红蓝互换（⛔ 不声称像素等价，
 *     只钉字节序）；
 *   - **读侧零读者**：`biSizeImage` / 声明长度 / 压缩 / 平面数都不参与解码（引擎的读取器从不比较它们）；
 *   - **位深**：本版只建模 24；1 / 4 / 8 / 32 必须**明确报** `bmp-bitdepth-unmodeled`，⛔ 不许当 24 位解。
 * ★ 合成侧的行为守卫（`0x1ae` / `0x1af`）在 `emulator-new-ops-100-200.test.mjs`；真机字节在
 *   `emulator-new-save-sth.assets.test.mjs`。本条只负责纯函数。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { BmpError, bmpBitDepth, bmpRowStride, decodeBmp, encodeBmp24 } from '../../apps/emulator-new/src/core/bmp.ts';

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

/**
 * 合成表面：`w × h`、每像素 3 字节、掩码取 `R = 0xFF` 口径（源字节序 R,G,B）。
 * `pixel(x, y)` 决定源值（自上而下）。
 */
function surface(w, h, pixel) {
  const pixels = new Uint8Array(w * h * 3);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const [r, g, b] = pixel(x, y);
      const at = (y * w + x) * 3;
      pixels[at] = r;
      pixels[at + 1] = g;
      pixels[at + 2] = b;
    }
  }
  return { pixels, width: w, height: h, stride: w * 3, bytesPerPixel: 3, masks: { r: 0xff, g: 0xff00, b: 0xff0000 } };
}

/** 抛出的 `BmpError.code`（没抛 ⇒ null）。 */
function codeOf(fn) {
  try {
    fn();
  } catch (error) {
    return error instanceof BmpError ? error.code : `not-bmp-error:${error.message}`;
  }
  return null;
}

test('★ encodeBmp24：24 位头字段（声明长度 = 行宽 × 高 + 40，不是 +54）', () => {
  const bmp = encodeBmp24(surface(4, 2, () => [1, 2, 3]));
  assert.equal(bmp.length, 54 + 4 * 3 * 2, '真实长度 = 54 + 像素字节数');
  assert.equal(String.fromCharCode(bmp[0], bmp[1]), 'BM');
  assert.equal(u32(bmp, 2), 4 * 3 * 2 + 40, '★ 声明长度 = 行宽 × 高 + 40（差 14 是引擎写侧行为）');
  assert.equal(bmp.length - u32(bmp, 2), 14, '声明比实际少 14 字节');
  assert.equal(u16(bmp, 6), 0, '+6 保留');
  assert.equal(u16(bmp, 8), 0, '+8 保留');
  assert.equal(u32(bmp, 10), 54, '像素数据起始 = 两段头长');
  assert.equal(u32(bmp, 14), 40, 'biSize');
  assert.equal(u32(bmp, 18), 4, '宽');
  assert.equal(u32(bmp, 22), 2, '高（正数 ⇒ 自下而上）');
  assert.equal(u16(bmp, 26), 1, '平面数');
  assert.equal(u16(bmp, 28), 24, '位深 = 24');
  assert.equal(u32(bmp, 30), 0, '压缩 = BI_RGB');
  assert.equal(u32(bmp, 34), 0, 'biSizeImage = 0');
  assert.equal(u32(bmp, 38), 0, '每米像素（x）');
  assert.equal(u32(bmp, 42), 0, '每米像素（y）');
  assert.equal(u32(bmp, 46), 0, '调色板计数');
  assert.equal(u32(bmp, 50), 0, '重要调色板计数');
  assert.equal(bmpRowStride(4, 24), 12, '4 的倍数宽 ⇒ 行宽 = 3 × 宽');
});

test('★ encodeBmp24：行宽按 32 位对齐（宽 321 ⇒ 每行 1 个填充字节，值为 0）', () => {
  const bmp = encodeBmp24(surface(321, 1, () => [1, 2, 3]));
  const stride = 4 * Math.ceil((321 * 24) / 32); // 964
  assert.equal(stride, 964);
  assert.equal(stride % 4, 0, '行字节数 4 字节对齐');
  assert.equal(bmp.length, 54 + stride, '像素块按对齐行宽分配');
  assert.equal(u32(bmp, 2), stride + 40, '声明长度按对齐行宽算');
  // 最后一个有效像素的 B,G,R 之后只剩一个填充字节
  assert.deepEqual([...bmp.subarray(54 + stride - 4, 54 + stride)], [3, 2, 1, 0], '末像素 BGR 之后是 0 填充');
});

test('★ encodeBmp24：自下而上 + 每像素 蓝、绿、红', () => {
  const bmp = encodeBmp24(
    surface(2, 2, (x, y) => (y === 0 ? [[255, 0, 0], [0, 255, 0]][x] : [[0, 0, 255], [255, 255, 255]][x])),
  );
  // 文件第一行 = 源**最后**一行（蓝 / 白）；每像素依次 蓝、绿、红
  assert.deepEqual([...bmp.subarray(54, 60)], [255, 0, 0, 255, 255, 255], '文件首行 = 源末行，BGR');
  // 行宽 = ceil(24 × 2 / 32) × 4 = 8 ⇒ 第二行从 54 + 8 = 62 起
  assert.deepEqual([...bmp.subarray(62, 68)], [0, 0, 255, 0, 255, 0], '文件第二行 = 源首行（红 / 绿）');
  assert.deepEqual([...bmp.subarray(60, 62)], [0, 0], '首行行末填充为 0');
});

test('★ decodeBmp：24 位往返；声明长度 / biSizeImage / 压缩 / 平面数 / 像素起始都不影响解码', () => {
  const src = surface(3, 2, (x, y) => (x === 0 ? [10, 20, 30] : x === 1 ? [40, 50, 60] : [70, 80 + y, 90]));
  const bmp = encodeBmp24(src);
  const decoded = decodeBmp(bmp);
  assert.equal(decoded.width, 3);
  assert.equal(decoded.height, 2);
  assert.equal(decoded.rgba.length, 3 * 2 * 4);
  // 解码回来是**自上而下** RGBA8，与源像素一致（A = 255）
  for (let y = 0; y < 2; y += 1) {
    for (let x = 0; x < 3; x += 1) {
      const s = (y * src.width + x) * 3;
      const d = (y * 3 + x) * 4;
      assert.deepEqual([...decoded.rgba.subarray(d, d + 4)], [src.pixels[s], src.pixels[s + 1], src.pixels[s + 2], 255]);
    }
  }

  // ★ 零读者：下面这些字段被改坏，解码结果必须**一字不变**
  const mutated = Uint8Array.from(bmp);
  mutated[2] = 0xfe; // 声明长度
  mutated[10] = 0xfe; // 像素数据起始（引擎不看它，恒从 54 读）
  mutated[30] = 1; // 压缩 = BI_RLE8
  mutated[34] = 0x7f; // biSizeImage
  mutated[26] = 2; // 平面数
  const again = decodeBmp(mutated);
  assert.deepEqual([...again.rgba], [...decoded.rgba], '零读者：这些字段不参与解码');
});

test('★ decodeBmp：位深 1 / 4 / 8 / 32 明确报 bmp-bitdepth-unmodeled（探测与解码分开）', () => {
  const bmp = encodeBmp24(surface(2, 2, () => [1, 2, 3]));
  assert.equal(bmpBitDepth(bmp), 24);
  for (const bitCount of [1, 4, 8, 32]) {
    const other = Uint8Array.from(bmp);
    other[28] = bitCount & 0xff;
    other[29] = (bitCount >>> 8) & 0xff;
    assert.equal(bmpBitDepth(other), bitCount, `探测出 ${bitCount} 位`);
    assert.equal(codeOf(() => decodeBmp(other)), 'bmp-bitdepth-unmodeled', `位深 ${bitCount} ⇒ 明确拒绝`);
  }
  // 头不齐 / 非 BM 是**另外两种**（探测给 null，解码给各自的 code）
  assert.equal(bmpBitDepth(bmp.subarray(0, 29)), null);
  assert.equal(codeOf(() => decodeBmp(bmp.subarray(0, 40))), 'bmp-header-short');
  const notBm = Uint8Array.from(bmp);
  notBm[1] = 0x00;
  assert.equal(bmpBitDepth(notBm), null);
  assert.equal(codeOf(() => decodeBmp(notBm)), 'bmp-not-bm');
  // 像素块短读：本版**显式失败**（RES-SAVE-STH-SHORTREAD 的实现口径，⛔ 不是引擎合同）
  assert.equal(codeOf(() => decodeBmp(bmp.subarray(0, bmp.length - 1))), 'bmp-short-read');
});

test('★ encodeBmp24：表面不合法 ⇒ 抛带 code 的 BmpError（⛔ 不静默裁剪 / 不补 0）', () => {
  const good = surface(2, 2, () => [1, 2, 3]);
  assert.equal(encodeBmp24(good).length, 54 + 8 * 2, '基准：2 × 2 的 24 位图');

  assert.equal(codeOf(() => encodeBmp24({ ...good, width: 0 })), 'bmp-surface-dimensions');
  assert.equal(codeOf(() => encodeBmp24({ ...good, height: -1 })), 'bmp-surface-dimensions');
  assert.equal(codeOf(() => encodeBmp24({ ...good, bytesPerPixel: 5 })), 'bmp-surface-bytes-per-pixel');
  assert.equal(codeOf(() => encodeBmp24({ ...good, stride: 5 })), 'bmp-surface-stride');
  assert.equal(codeOf(() => encodeBmp24({ ...good, pixels: new Uint8Array(4) })), 'bmp-surface-short');
  assert.equal(codeOf(() => encodeBmp24({ ...good, masks: { r: 0, g: 0xff00, b: 0xff0000 } })), 'bmp-surface-masks');
});
