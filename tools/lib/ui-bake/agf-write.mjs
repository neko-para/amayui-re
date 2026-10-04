/**
 * tools/lib/ui-bake/agf-write.mjs —— 纯工具：把改好的 RGBA **注回** AGF（保留原件的一切非像素字段）
 *
 * 口径（照旧仓 `scripts/agf/cli.js inject`，即"有头注入"）：
 *   · 头 / meta（含调色板）/ ACIF 前缀 / trailer **全部沿用原件**，只换 body 与 alpha 数据；
 *   · body 与 alpha 都走 `packSection`（**压得小就压，否则原样**）—— 这是 AGF 族盘上的实测规则；
 *   · 8bpp 的像素用**原调色板的最近色**量化（`encodeBody`，严格小于才换 ⇒ 同色并列取小索引）。
 *
 * ★ 已知且无害的偏差：调色板里若有**同一颜色的重复项**，最近色量化可能取到 114 而原盘是 115
 *   ——两者 RGB 完全相同 ⇒ **解出来的像素一模一样**，只在"原始索引字节"层面差几位。
 *   实测 10 张里 9 张 body 逐字节相同，唯一例外是 `SO009B.AGF`（调色板有 1 组重复色，
 *   73/182272 字节的索引在 114/115 之间摆动）。所以 AGF 层的判据是**解码后像素相同**，
 *   不是"AGF 字节相同"。
 */
import { writeAgf, encodeBody, alphaFromRgba, packSection, parseWhBpp, extractPaletteRgb } from './age-format.mjs';

/**
 * 用一张改好的图替换 AGF 的像素数据。
 * @param {object} agf `readAgfBuffer` 的返回值
 * @param {{width:number,height:number,rgba:Buffer}} img
 * @returns {Buffer} 新的 AGF 字节
 */
export function injectImage(agf, img) {
  const [w, h, bpp] = parseWhBpp(agf.meta);
  if (w !== img.width || h !== img.height) throw new Error(`尺寸不匹配：AGF ${w}×${h} vs 图 ${img.width}×${img.height}`);
  const palette = bpp === 8 ? extractPaletteRgb(agf.meta) : null;
  if (bpp === 8 && !palette) throw new Error('8bpp 但原件 meta 里没有调色板 —— 不编造灰度表（`agf.mjs` 同口径）');
  const body = encodeBody(img, bpp, palette);
  const override = { bodyRaw: packSection(body).raw, bodyUnpackedSize: body.length };
  if (agf.acif) {
    const alpha = alphaFromRgba(img);
    override.alphaRaw = packSection(alpha).raw;
    override.alphaUnpackedSize = alpha.length;
  }
  return writeAgf(agf, override);
}
