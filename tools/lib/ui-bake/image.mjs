/**
 * tools/lib/ui-bake/image.mjs —— **领域模型**：UI 烘焙用的图像表示与像素算子
 *
 * 判据口径：**像素级相同**（不看 PNG 字节）。所以这里只做两件必须精确的事：
 *   ① 像素算子（列/行填充、整块复制、恢复、`src over` 合成）；② PNG 编解码。
 *
 * ★ 为什么 `src over` 要自己写、不用 Jimp 自带的 `composite`/`blit`：
 *   Jimp 的 `blit` 用的是 `（a*(s-d) - d + 255) >> 8 + d` 与 `alpha = dstA + srcA`，
 *   那是**另一种混合口径**；旧仓产物是 `PIL.Image.alpha_composite` 出来的，
 *   实测两者在边缘反锯齿像素上差 1（SO002 一个块就 1109 个像素差）。
 *   下面这份是 PIL `alpha_composite` 的等价整数实现（最终通道四舍五入）。
 */
import fs from 'node:fs';
import { Jimp } from 'jimp';

export const TRANSPARENT = { r: 0, g: 0, b: 0, a: 0 };

/** 创建一个空白 RGBA 图层（用于先渲文字、再合成） */
export function blank(width, height) {
  return Jimp.fromBitmap({ data: Buffer.alloc(width * height * 4), width, height });
}

/** 从 AGF 解出的原始 RGBA 建图 */
export function fromRgba(rgba, width, height) {
  return Jimp.fromBitmap({ data: Buffer.from(rgba), width, height });
}

/** 读 PNG 文件（Jimp 编解码经实测无损：编码→回读逐通道相同） */
export async function readPng(file) {
  return Jimp.read(file);
}

/** 写 PNG 文件 */
export async function writePng(img, file) {
  fs.writeFileSync(file, await img.getBuffer('image/png'));
  return file;
}

export const clone = (img) => img.clone();
export const size = (img) => ({ w: img.bitmap.width, h: img.bitmap.height });

/** 逐通道比较：返回 `{ channels, maxDelta, bbox }`（bbox 用像素坐标，null 表示完全相同） */
export function diff(a, b) {
  const A = a.bitmap;
  const B = b.bitmap;
  if (A.width !== B.width || A.height !== B.height) {
    return { channels: -1, maxDelta: -1, bbox: null, sizeMismatch: true };
  }
  let channels = 0;
  let maxDelta = 0;
  let x0 = Infinity;
  let y0 = Infinity;
  let x1 = -Infinity;
  let y1 = -Infinity;
  for (let y = 0; y < A.height; y += 1) {
    for (let x = 0; x < A.width; x += 1) {
      const o = (y * A.width + x) * 4;
      let hit = false;
      for (let k = 0; k < 4; k += 1) {
        const d = Math.abs(A.data[o + k] - B.data[o + k]);
        if (d !== 0) {
          hit = true;
          channels += 1;
          if (d > maxDelta) maxDelta = d;
        }
      }
      if (hit) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 1 > x1) x1 = x + 1;
        if (y + 1 > y1) y1 = y + 1;
      }
    }
  }
  return { channels, maxDelta, bbox: channels ? [x0, y0, x1, y1] : null };
}

// ─────────────────────────────────────────────────────────── 像素算子

/** 一个块矩形（`x1`/`y1` 含端，与旧仓导出的 JSON 同口径） */
const clampRegion = (img, r) => {
  const { width: w, height: h } = img.bitmap;
  return {
    x0: Math.max(0, r.x0),
    y0: Math.max(0, r.y0),
    x1: Math.min(w - 1, r.x1),
    y1: Math.min(h - 1, r.y1),
  };
};

/**
 * 列填充（旧仓 `clean_fill.py` 的语义，**半开区间**，别改成闭区间）：
 *   · 保留左 `keepL` px、右 `keepR` px、上 `keepT` px、下 `keepB` px；
 *   · 被填的 x 范围是 `[x0+keepL, x1-keepR+1)` —— 注意右端是 **x1-keepR+1 取不到**，
 *     等价于"保留带从 x1-keepR+1 开始"，与旧仓 Python 的 `range(x0+keep_l, x1-keep_r+1)` 逐字对应；
 *   · 中间**每一行复制 `fillCol` 那一列**的像素（纯色底/渐变底都适用：逐行复制保留每行渐变值）。
 *
 * ★ 这里必须与旧仓逐字对齐：闭区间会多吃/少吃一列，SO002 上就是 2572 个像素的偏差。
 */
export function fillColumns(img, r, keepL, keepR, keepT = 0, keepB = 0) {
  const { width: w, data } = img.bitmap;
  const { x0, y0, x1, y1 } = clampRegion(img, r);
  for (let y = y0 + keepT; y < y1 - keepB + 1; y += 1) {
    const so = (y * w + r.fillCol) * 4;
    const p = [data[so], data[so + 1], data[so + 2], data[so + 3]];
    for (let x = x0 + keepL; x < x1 - keepR + 1; x += 1) {
      const o = (y * w + x) * 4;
      data[o] = p[0];
      data[o + 1] = p[1];
      data[o + 2] = p[2];
      data[o + 3] = p[3];
    }
  }
  return img;
}

/**
 * 行填充（列填充的并列模式，同样**半开区间**）：区域中间**每列复制 `fillRow` 那一行**的像素；
 * 保留带口径同 {@link fillColumns}（适合背景横向一致的场合）。
 */
export function fillRows(img, r, keepL, keepR, keepT = 0, keepB = 0) {
  const { width: w, data } = img.bitmap;
  const { x0, y0, x1, y1 } = clampRegion(img, r);
  for (let x = x0 + keepL; x < x1 - keepR + 1; x += 1) {
    const so = (r.fillRow * w + x) * 4;
    const p = [data[so], data[so + 1], data[so + 2], data[so + 3]];
    for (let y = y0 + keepT; y < y1 - keepB + 1; y += 1) {
      const o = (y * w + x) * 4;
      data[o] = p[0];
      data[o + 1] = p[1];
      data[o + 2] = p[2];
      data[o + 3] = p[3];
    }
  }
  return img;
}

/** 置透明（旧仓 `clean_fill.py --transparent`） */
export function makeTransparent(img, r) {
  const { width: w, data } = img.bitmap;
  const { x0, y0, x1, y1 } = clampRegion(img, r);
  for (let y = y0; y <= y1; y += 1) {
    for (let x = x0; x <= x1; x += 1) data[(y * w + x) * 4 + 3] = 0;
  }
  return img;
}

/**
 * 整块复制：把 `src` 的 `from` 区域 1:1 打到 `img` 的 `to` 左上角。
 * ★ 旧仓那条路径是 `crop(from).resize(to 尺寸, LANCZOS).paste()`；实测用到的场合
 *   **源与目标同尺寸**（SO020 关闭菜单从 SO021 取 154×48 打到 154×48），此时 resize 是恒等，
 *   所以这里只做 1:1 拷贝、并在尺寸不等时**报错**而不是悄悄缩放（缩放插值不可复现）。
 */
export function paste(img, src, from, to) {
  const f = { x0: from.x0, y0: from.y0, x1: from.x1, y1: from.y1 };
  const tw = to.x1 - to.x0 + 1;
  const th = to.y1 - to.y0 + 1;
  const sw = f.x1 - f.x0 + 1;
  const sh = f.y1 - f.y0 + 1;
  if (sw !== tw || sh !== th) {
    throw new Error(`整块复制的源与目标尺寸不同（${sw}×${sh} → ${tw}×${th}）——缩放插值不可复现，请改用 1:1 的源区域`);
  }
  const { width: w, data } = img.bitmap;
  const { width: sw2, data: sdata } = src.bitmap;
  for (let y = 0; y < sh; y += 1) {
    for (let x = 0; x < sw; x += 1) {
      const so = ((f.y0 + y) * sw2 + (f.x0 + x)) * 4;
      const o = ((to.y0 + y) * w + (to.x0 + x)) * 4;
      data[o] = sdata[so];
      data[o + 1] = sdata[so + 1];
      data[o + 2] = sdata[so + 2];
      data[o + 3] = sdata[so + 3];
    }
  }
  return img;
}

/** 从 `orig` 恢复一块（覆盖前面填充/复制的结果） */
export function restore(img, orig, r) {
  const { width: w, data } = img.bitmap;
  const { x0, y0, x1, y1 } = clampRegion(img, r);
  const sdata = orig.bitmap.data;
  for (let y = y0; y <= y1; y += 1) {
    for (let x = x0; x <= x1; x += 1) {
      const o = (y * w + x) * 4;
      data[o] = sdata[o];
      data[o + 1] = sdata[o + 1];
      data[o + 2] = sdata[o + 2];
      data[o + 3] = sdata[o + 3];
    }
  }
  return img;
}

/**
 * `src over dst`（PIL `Image.alpha_composite` 同口径）。
 * 逐像素：`outA = srcA + trunc(dstA*(255-srcA)/255)`；
 * `outC = round((srcC*srcA + trunc(dstC*dstA*(255-srcA)/255)) / outA)`。
 */
export function composeOver(dst, src) {
  const d = dst.bitmap.data;
  const s = src.bitmap.data;
  const n = Math.min(d.length, s.length);
  for (let o = 0; o < n; o += 4) {
    const sa = s[o + 3];
    if (sa === 0) continue;
    const da = d[o + 3];
    const outA = sa + Math.trunc((da * (255 - sa)) / 255);
    if (sa === 255) {
      d[o] = s[o];
      d[o + 1] = s[o + 1];
      d[o + 2] = s[o + 2];
    } else {
      for (let k = 0; k < 3; k += 1) {
        const num = s[o + k] * sa + Math.trunc((d[o + k] * da * (255 - sa)) / 255);
        d[o + k] = Math.round(num / outA);
      }
    }
    d[o + 3] = outA;
  }
  return dst;
}

// ─────────────────────────────────────────────── 拉普拉斯模板清理（SO001 中簇/列用的那条算法）

/** 取块内某像素的 RGB（**丢掉 alpha**：旧仓 numpy 是 `convert('RGB')`，掩码只看颜色） */
const rgbAt = (data, w, x, y) => {
  const o = (y * w + x) * 4;
  return [data[o], data[o + 1], data[o + 2]];
};

/**
 * 按模式找"文字行"的包围盒（旧仓 `so001_clean15.py::find_box` 的逐字等价）。
 *
 * 返回块内坐标 `[rowLo, rowHi, colLo, colHi]`（含端，已按 PAD=2 外扩并夹在 `ix0+1..ix1-1 / iy1-1`）。
 * 找不到返回 `null`（那一块保持原样）—— 旧仓的 `continue` 就是这个语义。
 */
function findTextBox(seg, { ix0, ix1, mode }) {
  const w = seg.width;
  const rows = new Array(seg.height).fill(0);
  for (let y = 0; y < seg.height; y += 1) {
    let s = 0;
    for (let x = ix0; x <= ix1; x += 1) if (seg.mask[y * w + x]) s += 1;
    rows[y] = s;
  }
  const trl = [];
  for (let y = 13; y < 70; y += 1) if (rows[y] > 0) trl.push(y);
  if (!trl.length) return null;
  const runs = [];
  let cur = [trl[0]];
  for (let i = 1; i < trl.length; i += 1) {
    if (trl[i] - cur[cur.length - 1] <= 3) cur.push(trl[i]);
    else { runs.push(cur); cur = [trl[i]]; }
  }
  runs.push(cur);
  let band = runs[0];
  for (const r of runs) if (r.length > band.length) band = r;
  const rowLo = band[0];
  const rowHi = band[band.length - 1];
  const tcol = new Array(seg.width).fill(0);
  for (let x = 0; x < seg.width; x += 1) {
    let s = 0;
    for (let y = rowLo; y <= rowHi; y += 1) if (seg.mask[y * w + x]) s += 1;
    tcol[x] = s;
  }
  const colBody = [];
  for (let x = ix0; x <= ix1; x += 1) if (tcol[x] > 1) colBody.push(x);
  if (!colBody.length) return null;
  const PAD = 2;
  return [
    Math.max(ix0 + 1, rowLo - PAD),
    Math.min(seg.iy1 - 1, rowHi + PAD),
    Math.max(ix0 + 1, Math.min(...colBody) - PAD),
    Math.min(ix1 - 1, Math.max(...colBody) + PAD),
  ];
}

/** 单通道拉普拉斯插值：四边做边界，先给初值再 Jacobi 迭代（旧仓 `laplace_channel` 逐字等价） */
function laplaceChannel(seg, ch, box, iters) {
  const [rowLo, rowHi, colLo, colHi] = box;
  const vt = rowLo - 1;
  const vb = rowHi + 1;
  const cl = colLo - 1;
  const cr = colHi + 1;
  const H = rowHi - rowLo + 1;
  const W = colHi - colLo + 1;
  const px = (x, y) => seg.data[(y * seg.width + x) * 4 + ch];
  const capTop = new Float64Array(W);
  const capBot = new Float64Array(W);
  const capLeft = new Float64Array(H);
  const capRight = new Float64Array(H);
  for (let i = 0; i < W; i += 1) { capTop[i] = px(colLo + i, vt); capBot[i] = px(colLo + i, vb); }
  for (let i = 0; i < H; i += 1) { capLeft[i] = px(cl, rowLo + i); capRight[i] = px(cr, rowLo + i); }
  const c00 = capTop[0];
  const c10 = capTop[W - 1];
  const c01 = capBot[0];
  const c11 = capBot[W - 1];
  const U = new Float64Array(H * W);
  for (let iy = 0; iy < H; iy += 1) {
    const v = (iy + 1) / (H + 1);
    for (let ix = 0; ix < W; ix += 1) {
      const u = (ix + 1) / (W + 1);
      const T = capTop[ix] * (1 - v) + capBot[ix] * v;
      const Lc = capLeft[iy] * (1 - u) + capRight[iy] * u;
      const C = c00 * (1 - u) * (1 - v) + c10 * u * (1 - v) + c01 * (1 - u) * v + c11 * u * v;
      U[iy * W + ix] = T + Lc - C;
    }
  }
  for (let it = 0; it < iters; it += 1) {
    for (let iy = 0; iy < H; iy += 1) {
      for (let ix = 0; ix < W; ix += 1) {
        const up = iy > 0 ? U[(iy - 1) * W + ix] : capTop[ix];
        const dn = iy < H - 1 ? U[(iy + 1) * W + ix] : capBot[ix];
        const lf = ix > 0 ? U[iy * W + ix - 1] : capLeft[iy];
        const rt = ix < W - 1 ? U[iy * W + ix + 1] : capRight[iy];
        U[iy * W + ix] = 0.25 * (up + dn + lf + rt);
      }
    }
  }
  return { U, rowLo, rowHi, colLo, colHi };
}

/** 三种模式的文字掩码（旧仓 `mask_for` 逐字等价；阈值一律照抄） */
const MASKS = {
  warm: (r, g, b) => (r > 140 && g > 80 && b < 130 && r - b > 60) || r + g + b < 250,
  gray: (r, g, b) => (r + g + b) / 3 < 60,
  cyan: (r, g, b) => {
    const cyan = Math.abs(r - 1) < 50 && Math.abs(g - 254) < 22 && Math.abs(b - 255) < 22;
    const white = r > 200 && g > 200 && b > 200;
    return !cyan || white;
  },
};

/**
 * **拉普拉斯模板清理**：在块内自动找出文字包围盒，再用四周颜色做拉普拉斯插值把文字抹掉。
 * `cyan` 模式不插值，直接填纯青 `(1,254,255)`（旧仓原样）。
 *
 * @param {object} img
 * @param {object} r `{x0,y0,w,h, mode:'warm'|'gray'|'cyan', iters?, ix0?, ix1?, iy1?}`
 */
export function laplaceClean(img, r) {
  const { x0, y0, w, h, mode } = r;
  const iters = r.iters ?? (mode === 'gray' || mode === 'warm' ? 700 : 700);
  const ix0 = r.ix0 ?? 7;
  const ix1 = r.ix1 ?? 70;
  const iy1 = r.iy1 ?? 71;
  const main = img.bitmap.data;
  const mainW = img.bitmap.width;
  // 摘出块（含 alpha，插值只动 RGB、alpha 原样保留）
  const segData = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const so = ((y0 + y) * mainW + (x0 + x)) * 4;
      segData[(y * w + x) * 4] = main[so];
      segData[(y * w + x) * 4 + 1] = main[so + 1];
      segData[(y * w + x) * 4 + 2] = main[so + 2];
      segData[(y * w + x) * 4 + 3] = main[so + 3];
    }
  }
  const seg = { data: segData, width: w, height: h, iy1, mask: new Uint8Array(w * h) };
  const fn = MASKS[mode];
  if (!fn) throw new Error(`不认识的拉普拉斯模式：${mode}（只认 warm / gray / cyan）`);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const [R, G, B] = rgbAt(segData, w, x, y);
      seg.mask[y * w + x] = fn(R, G, B) ? 1 : 0;
    }
  }
  const box = findTextBox(seg, { ix0, ix1, mode });
  if (!box) return { changed: false, box: null };
  const [rowLo, rowHi, colLo, colHi] = box;
  if (mode === 'cyan') {
    for (let y = rowLo; y <= rowHi; y += 1) {
      for (let x = colLo; x <= colHi; x += 1) {
        const o = (y * w + x) * 4;
        segData[o] = 1;
        segData[o + 1] = 254;
        segData[o + 2] = 255;
      }
    }
  } else {
    for (let ch = 0; ch < 3; ch += 1) {
      const { U } = laplaceChannel(seg, ch, box, iters);
      const W = colHi - colLo + 1;
      for (let iy = 0; iy < rowHi - rowLo + 1; iy += 1) {
        for (let ix = 0; ix < W; ix += 1) {
          // numpy 写回 uint8 数组是**截断**（不是四舍五入）
          segData[((rowLo + iy) * w + (colLo + ix)) * 4 + ch] = Math.trunc(U[iy * W + ix]) & 0xff;
        }
      }
    }
  }
  // 写回大图（只写块内，块外逐像素不动）
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const so = (y * w + x) * 4;
      const o = ((y0 + y) * mainW + (x0 + x)) * 4;
      main[o] = segData[so];
      main[o + 1] = segData[so + 1];
      main[o + 2] = segData[so + 2];
      main[o + 3] = segData[so + 3];
    }
  }
  return { changed: true, box };
}
