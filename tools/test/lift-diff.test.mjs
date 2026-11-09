/** @env pure @kind contract @why `lift diff` 把"读不到 / 尺寸不同"混成"有差异"、或 BMP 行序 / 位深读错 ⇒ 两次运行的像素比对给出假结论（假红或假绿） */
/**
 * tools/test/lift-diff.test.mjs — `tools/lift.mjs diff`（REQ-01M6WSANRPND80XW52QY4Y5WA8）的合同
 *
 * 被测的是**出口码语义**，不是某次运行的读数：
 *   0 = 所有像素差 ≤ tolerance；1 = 有像素超差；2 = 参数 / 读文件 / 格式错误。
 * 三条最容易静默错的边界，各由一条用例钉住：
 *   ① 尺寸不同 ⇒ 2（⛔ 不做缩放对齐，也不许掉进"有差异 = 1"）；
 *   ② `tolerance` 是 **≤**（差 3 / tolerance 3 ⇒ 0），且 `diff-pixels` 只数**超差**像素；
 *   ③ BMP 的行序（自底向上 / 自顶向下 `biHeight<0`）与 16bpp 565 —— 读错会得到
 *      "同一张画面在 y 方向镜像"这种看起来像回归、其实是我方解析器的假红。
 *
 * ★ 夹具**现造字节**（4×4，`Buffer.alloc`）：⛔ 不往仓库塞二进制；写出的格式口径与
 *   `apps/emulator-lift/src/runtime/surface.cpp` 的 `dumpBmp` 相同（BI_RGB · 行 4 字节对齐 ·
 *   缺省自底向上 · 24/32bpp；测试另加 16bpp 565 与 top-down 两种变体）。
 */
import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { bmpDiff, decodeBmp } from '../lift.mjs';

const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-lift-diff-'));
after(() => fs.rmSync(TMP, { recursive: true, force: true }));

let seq = 0;
/** 测试用文件路径（每个用例各写一份，互不共享状态） */
function file(name) {
  seq += 1;
  return path.join(TMP, `${seq}-${name}`);
}

/**
 * 现造一份 4×4 的 BMP 字节。
 * `pixel(x, y)` 返回 `[r, g, b]`（0..255；按**自顶向下**的图像坐标给，便于断言 `at=x,y`）。
 * `topDown` = 写 `biHeight < 0` 的变体；`bpp` ∈ 16 / 24 / 32。
 */
function makeBmp({ width = 4, height = 4, bpp = 24, topDown = false, pixel = () => [0, 0, 0] } = {}) {
  const bytesPerPixel = bpp / 8;
  const stride = (width * bytesPerPixel + 3) & ~3;
  const imageSize = stride * height;
  const b = Buffer.alloc(54 + imageSize);
  b.write('BM', 0, 'latin1');
  b.writeUInt32LE(b.length, 2);
  b.writeUInt32LE(14 + 40, 10); // bfOffBits
  b.writeUInt32LE(40, 14); // biSize（BITMAPINFOHEADER）
  b.writeInt32LE(width, 18);
  b.writeInt32LE(topDown ? -height : height, 22);
  b.writeUInt16LE(1, 26); // biPlanes
  b.writeUInt16LE(bpp, 28);
  b.writeUInt32LE(0, 30); // BI_RGB
  b.writeUInt32LE(imageSize, 34);
  b.writeUInt32LE(2835, 38);
  b.writeUInt32LE(2835, 42);
  for (let y = 0; y < height; y += 1) {
    // 自底向上 = 文件第一行是图像最后一行
    const srcRow = topDown ? y : height - 1 - y;
    for (let x = 0; x < width; x += 1) {
      const [r, g, bl] = pixel(x, y);
      const p = 54 + srcRow * stride + x * bytesPerPixel;
      if (bpp === 16) {
        // 只喂 0 / 255 这类**可精确表示**的值，免得 5/6 位量化自己造出 ±1 的差
        b.writeUInt16LE((((r * 31) / 255 | 0) << 11) | (((g * 63) / 255 | 0) << 5) | ((bl * 31) / 255 | 0), p);
      } else {
        b[p] = bl;
        b[p + 1] = g;
        b[p + 2] = r;
        if (bpp === 32) b[p + 3] = 0; // 与 dumpBmp 同口径：32bpp 的 A 固定写 0
      }
    }
  }
  return b;
}

/** 写两份夹具并返回路径对 */
function pair(aBytes, bBytes) {
  const a = file('a.bmp');
  const b = file('b.bmp');
  fs.writeFileSync(a, aBytes);
  fs.writeFileSync(b, bBytes);
  return [a, b];
}

test('diff-identical', () => {
  const img = (x, y) => [x * 60, y * 60, 30 + x * 10]; // 三通道互不相等 ⇒ 能发现 B/R 顺序读反
  const [a, b] = pair(makeBmp({ pixel: img }), makeBmp({ pixel: img }));
  const r = bmpDiff([a, b]);
  assert.equal(r.error, undefined, r.error);
  assert.equal(r.code, 0, '两张相同 BMP 必须退出 0');
  assert.equal(r.result.maxDiff, 0);
  assert.equal(r.result.diffPixels, 0);
  assert.equal(r.result.diffRatio, 0);
  assert.equal(r.result.at, null);
  assert.equal(r.result.totalPixels, 16);
  // ★ 写出口径的验收：BMP 的像素顺序是 B,G,R，解码必须还原成 R,G,B（否则"改红"会报成"改蓝"）
  const decoded = decodeBmp(fs.readFileSync(a));
  assert.deepEqual([...decoded.rgb.slice(0, 3)], [0, 0, 30], '第一个像素必须是 R,G,B（不是 BMP 的 B,G,R）');
  // ★ dumpBmp 对 32 位 surface 默认写 32bpp（A 固定 0）⇒ 同一张画面的 24bpp / 32bpp 必须判"相同"
  const [b32, b24] = pair(makeBmp({ bpp: 32, pixel: img }), makeBmp({ bpp: 24, pixel: img }));
  const mixed = bmpDiff([b32, b24]);
  assert.equal(mixed.code, 0, '32bpp（A=0）与 24bpp 的同画面必须相等：A 不进比对');
});

test('diff-changed', () => {
  const [a, b] = pair(
    makeBmp(),
    makeBmp({ pixel: (x, y) => (x === 1 && y === 2 ? [0, 0, 1] : [0, 0, 0]) }),
  );
  const r = bmpDiff([a, b]);
  assert.equal(r.code, 1, '改一个像素必须退出 1');
  assert.equal(r.result.diffPixels, 1);
  assert.equal(r.result.maxDiff, 1);
  assert.equal(r.result.diffRatio, 0.0625, '1 / 16 必须按 4 位报');
  assert.deepEqual(r.result.at, { x: 1, y: 2 }, 'at 必须是第一个超差像素的图像坐标（y 自上而下）');
});

test('diff-tolerance', () => {
  const [a, b] = pair(
    makeBmp(),
    makeBmp({ pixel: (x, y) => (x === 0 && y === 0 ? [3, 0, 0] : [0, 0, 0]) }),
  );
  // ★ 判据是 ≤ tolerance（差 3 / tolerance 3 ⇒ 绿）；且 diff-pixels 只数超差像素
  const ok = bmpDiff([a, b, '--tolerance', '3']);
  assert.equal(ok.error, undefined, ok.error);
  assert.equal(ok.code, 0, '差 3 且 --tolerance 3 必须退出 0');
  assert.equal(ok.result.maxDiff, 3, 'max-diff 仍如实报告最大值（不受 tolerance 影响）');
  assert.equal(ok.result.diffPixels, 0);
  assert.equal(ok.result.at, null);
  // 另一侧：差 3 / tolerance 2 ⇒ 必须红（否则 `<=` 被写成 `<` 之类不会被发现）
  const bad = bmpDiff([a, b, '--tolerance', '2']);
  assert.equal(bad.code, 1, '差 3 且 --tolerance 2 必须退出 1');
  assert.equal(bad.result.diffPixels, 1);
});

test('diff-bad-input', () => {
  // ① 尺寸不同 ⇒ 2（⛔ 不做缩放对齐；更不许混成 1）
  const [a, b] = pair(makeBmp(), makeBmp({ width: 5 }));
  const size = bmpDiff([a, b]);
  assert.equal(size.code, 2, '尺寸不同必须退出 2');
  assert.match(size.error, /尺寸不同/);
  // ② 缺文件 ⇒ 2（读不到不是"有差异"）
  const missing = bmpDiff([a, path.join(TMP, 'does-not-exist.bmp')]);
  assert.equal(missing.code, 2);
  assert.match(missing.error, /读不到/);
  // ③ 不是 BMP ⇒ 2（格式错与像素差异分开）
  const garbage = file('garbage.bmp');
  fs.writeFileSync(garbage, Buffer.from('not a bmp at all', 'latin1'));
  const bad = bmpDiff([a, garbage]);
  assert.equal(bad.code, 2);
  // ④ 参数错 ⇒ 2（不是 0 / 1）
  assert.equal(bmpDiff([a, b, '--tolerance', '256']).code, 2);
  assert.equal(bmpDiff([a, b, '--tolerance', 'x']).code, 2);
  assert.equal(bmpDiff([a]).code, 2);
});

test('diff-top-down-bmp', () => {
  const changed = (x, y) => (x === 2 && y === 1 ? [0, 0, 7] : [0, 0, 0]);
  // ★ 同一张画面的两种行序必须比对成"相同"：读错行序会得到 y 方向镜像的假红
  const [ta, tb] = pair(makeBmp({ topDown: true, pixel: changed }), makeBmp({ topDown: false, pixel: changed }));
  const same = bmpDiff([ta, tb]);
  assert.equal(same.code, 0, '自顶向下（biHeight<0）与自底向上必须读到同一张画面');
  assert.equal(same.result.maxDiff, 0);
  // ★ 差异位置也必须按图像坐标报（不是"文件里的行号"）
  const [ta2, tb2] = pair(makeBmp({ topDown: true, pixel: changed }), makeBmp({ topDown: false }));
  const diff = bmpDiff([ta2, tb2]);
  assert.equal(diff.code, 1);
  assert.equal(diff.result.diffPixels, 1);
  assert.deepEqual(diff.result.at, { x: 2, y: 1 }, 'top-down 的 y 也要从图像顶部数');
  assert.equal(diff.result.maxDiff, 7);
});

test('diff-16bpp-565', () => {
  const changed = (x, y) => (x === 3 && y === 0 ? [255, 0, 255] : [0, 0, 0]);
  // 16bpp 565 与 24bpp 的同画面必须逐像素相同（解码公式与 dumpBmp 的写出换算一致）
  const [a16, a24] = pair(makeBmp({ bpp: 16, pixel: changed }), makeBmp({ bpp: 24, pixel: changed }));
  const same = bmpDiff([a16, a24]);
  assert.equal(same.error, undefined, same.error);
  assert.equal(same.code, 0);
  assert.equal(same.result.maxDiff, 0);
  // 差异也要能测出来（否则"16bpp 一律读成 0"也会绿）
  const [a16b, b24] = pair(makeBmp({ bpp: 16, pixel: changed }), makeBmp({ bpp: 24 }));
  const diff = bmpDiff([a16b, b24]);
  assert.equal(diff.code, 1);
  assert.equal(diff.result.diffPixels, 1);
  assert.deepEqual(diff.result.at, { x: 3, y: 0 });
  assert.equal(diff.result.maxDiff, 255);
});
