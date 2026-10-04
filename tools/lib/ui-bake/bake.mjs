/**
 * tools/lib/ui-bake/bake.mjs —— **领域模型**：按 recipe 重放一张 UI 图的改图链
 *
 * 一次 `bake(block)` 就是旧仓那次手工改图的**机械化重放**：
 *
 * ```text
 *   原始游戏件 DATA1.ALF  ──ALF 目录──▶ SO0xx.AGF ──解码──▶ RGBA（= 旧仓 SO0xx-0.png，逐像素相同）
 *        │
 *        ├─ clean 段（列/行填充 / 贴底图 / 恢复 / 置透明）──▶ 干净底图
 *        ├─ text  段（recipe → HTML → headless Chrome）  ──▶ 透明文字层
 *        └─ 合成：文字层 `src over` 干净底图             ──▶ 生效版图（= corpus/assets/ui-images/SO0xx-N.png）
 *                                                        └─▶ 注回 AGF（解码后像素与旧仓产物一致）
 * ```
 *
 * ★ 三个"必须"：
 *   ① 原始游戏件**只读**（`agf-source.mjs` 只 readSync）；
 *   ② 渲文字**必须**走 headless Chrome（字形光栅化是产物的一部分，见 `render.mjs`）；
 *   ③ 合成**必须**用 PIL 等价口径（Jimp 自带的 blit 口径不同，边缘差 1，见 `image.mjs`）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { openGame, loadOriginal } from './agf-source.mjs';
import { injectImage } from './agf-write.mjs';
import {
  clone,
  composeOver,
  diff,
  fillColumns,
  fillRows,
  fromRgba,
  laplaceClean,
  makeTransparent,
  paste,
  readPng,
  restore,
  writePng,
} from './image.mjs';
import { buildHtml, loadEffects, loadRecipe } from './recipe.mjs';
import { renderHtml } from './render.mjs';

/** 旧仓 AGF 的"来源"清册：真源是 `corpus/assets/ui-images/versions.json` 与 recipe 的 `source` */
export const DEFAULT_GAME = 'E:\\Games\\Eushully\\天結いキャッスルマイスター';
export const DEFAULT_OLD_REPO = 'E:\\Games\\Eushully\\天結';

/**
 * 读 recipe 算"生效版是第几版"：真源是 `corpus/assets/ui-images/versions.json`（不要手写进 recipe）。
 * @param {string} repoRoot
 */
export function readVersions(repoRoot) {
  const f = path.join(repoRoot, 'corpus', 'assets', 'ui-images', 'versions.json');
  return JSON.parse(fs.readFileSync(f, 'utf8'));
}

/**
 * 渲染字体的绝对路径（真源：`corpus/assets/fonts/SarasaGothicSC-*.7z` 的解压目录）。
 *
 * ★ 这里**必须硬失败**，不能"找不到就退回系统字体"：
 *   `@font-face` 的 `url()` 指向不存在的文件时，headless Chrome 会**静默**去用系统装的同名字体
 *   （或 fallback 到别的西文字体），字形变了而命令照样成功 —— 这条正是旧仓踩过的坑
 *   （SO009B/SO017 的拉丁 fallback）。
 *   ⇒ 缺失就抛错，并把"怎么解出来"写在错误里。
 */
export function fontPaths(repoRoot) {
  const dir = path.join(repoRoot, 'corpus', 'assets', 'fonts', 'SarasaGothicSC');
  const regular = path.join(dir, 'SarasaGothicSC-Regular.ttf');
  const bold = path.join(dir, 'SarasaGothicSC-Bold.ttf');
  const missing = [regular, bold].filter((p) => !fs.existsSync(p));
  if (missing.length) {
    throw new Error(
      `渲染字体不在场（缺 ${missing.map((p) => path.basename(p)).join(' / ')}）。\n` +
        `  这些 ttf 由 corpus/assets/fonts/SarasaGothicSC-TTF-1.0.40.7z 解出（与旧仓 res/fonts/ 逐字节相同），\n` +
        `  且按 .gitignore 的既定口径**不入库** ⇒ 每台机器自己解一次：\n` +
        `    tar -xf corpus/assets/fonts/SarasaGothicSC-TTF-1.0.40.7z -C corpus/assets/fonts/SarasaGothicSC\n` +
        `  （缺失时**不要**退回系统字体：headless 会静默换成系统同名字体，字形就变了。）`,
    );
  }
  return { regular, bold };
}

/** 执行 clean 段：返回干净底图（**不改**传入的图，返回新图） */
export function applyClean(recipe, base, archive, log = () => {}) {
  const work = clone(base);
  for (const op of recipe.clean ?? []) {
    if (op.op === 'fill') {
      const keepL = op.keepL ?? 0;
      const keepR = op.keepR ?? op.keepL ?? 0;
      const keepT = op.keepT ?? 0;
      const keepB = op.keepB ?? op.keepT ?? 0;
      // ★ 像素算子吃的是 `{box, fillCol|fillRow}`，不是整条 op（`_note` 等键不该流进去）
      const region = { ...op.box, fillCol: op.fillCol, fillRow: op.fillRow };
      if (op.fillCol !== undefined) fillColumns(work, region, keepL, keepR, keepT, keepB);
      else fillRows(work, region, keepL, keepR, keepT, keepB);
      log(`  clean: ${op.fillCol !== undefined ? '列填充' : '行填充'} ${JSON.stringify(op.box)} keep=${keepL}/${keepR}/${keepT}/${keepB} 复制${op.fillCol !== undefined ? `列 ${op.fillCol}` : `行 ${op.fillRow}`}`);
    } else if (op.op === 'paste') {
      const src = loadOriginal(archive, `${op.fromBlock}.AGF`);
      paste(work, fromRgba(src.rgba, src.width, src.height), op.from, op.to);
      log(`  clean: 贴底图 自 ${op.fromBlock} ${JSON.stringify(op.from)} → ${JSON.stringify(op.to)}`);
    } else if (op.op === 'laplace') {
      // 两种写法：单块（x0/y0/w/h）或「全部 15 个中簇块」（rows × cols，旧仓 so001_clean15.py 的循环）
      if (Array.isArray(op.rows)) {
        for (const y0 of op.rows) {
          for (const [x0, mode] of op.cols) {
            const { changed, box } = laplaceClean(work, { x0, y0, w: op.w, h: op.h, mode, iters: op.iters, ix0: op.ix0, ix1: op.ix1, iy1: op.iy1 });
            log(`  clean: 拉普拉斯 ${mode.padEnd(5)} 块(${x0},${y0}) → ${changed ? JSON.stringify(box) : '跳过'}`);
          }
        }
      } else {
        const { changed, box } = laplaceClean(work, op);
        log(`  clean: 拉普拉斯 ${op.mode.padEnd(5)} 块(${op.x0},${op.y0}) ${op.w}×${op.h} → ${changed ? JSON.stringify(box) : '跳过'}`);
      }
    } else if (op.op === 'template') {
      // `source: "<self>"` = 从**本图的原始件**取模板（SO001 的右带/面板干净钮就是这种）；
      // 给块名则从那个块的原始 AGF 取（跨图贴底图，SO020 关菜单那种）。
      const src = op.source === '<self>' ? base : (() => { const o = loadOriginal(archive, `${op.source}.AGF`); return fromRgba(o.rgba, o.width, o.height); })();
      const tw = op.from.x1 - op.from.x0 + 1;
      const th = op.from.y1 - op.from.y0 + 1;
      for (const p of op.at) paste(work, src, op.from, { x0: p.x, y0: p.y, x1: p.x + tw - 1, y1: p.y + th - 1 });
      log(`  clean: 模板覆盖 ${op.source} ${JSON.stringify(op.from)}（${tw}×${th}）→ ${op.at.length} 处`);
    } else if (op.op === 'restore') {
      restore(work, base, op.box);
      log(`  clean: 恢复 ${JSON.stringify(op.box)}`);
    } else if (op.op === 'transparent') {
      makeTransparent(work, op.box);
      log(`  clean: 置透明 ${JSON.stringify(op.box)}`);
    } else {
      throw new Error(`不认识的清理操作：${op.op}`);
    }
  }
  return work;
}

/**
 * 重放一张图。返回各阶段产物与判定结果。
 * @param {object} o
 * @param {string} o.block
 * @param {string} [o.repoRoot]
 * @param {string} [o.game]
 * @param {string} [o.workDir]   中间产物落点（默认 `<repoRoot>/.tmp/ui-bake`）
 * @param {string} [o.chrome]
 * @param {boolean} [o.keepWork] 是否保留中间 PNG 之外的临时 HTML
 * @param {(s:string)=>void} [o.log]
 */
export async function bake({ block, repoRoot, game = DEFAULT_GAME, workDir, chrome, log = () => {} }) {
  const root = repoRoot ?? process.cwd();
  const work = workDir ?? path.join(root, '.tmp', 'ui-bake');
  fs.mkdirSync(work, { recursive: true });

  const recipe = loadRecipe(block);
  const effects = loadEffects();
  const versions = readVersions(root);
  const info = versions.images?.[block];
  if (!info) throw new Error(`versions.json 里没有 ${block}`);
  const current = info.current;

  const archive = openGame(game);
  const orig = loadOriginal(archive, recipe.source.agf, { kind: recipe.source.kind });
  log(`原始件：${recipe.source.agf}（${recipe.source.kind === 'loose' ? '安装根裸件' : `${archive.indexFile} → ALF`}：${orig.file}） ${orig.width}×${orig.height} bpp=${orig.bpp}`);
  const base = fromRgba(orig.rgba, orig.width, orig.height);

  // ── clean
  const cleaned = applyClean(recipe, base, archive, log);
  const cleanPng = path.join(work, `${block}-clean.png`);
  await writePng(cleaned, cleanPng);

  // ── text（headless Chrome）
  const fonts = fontPaths(root);
  // ★ 三种口径（recipe 的 `text.compose` 显式声明，**不许猜**）：
  //   · `over-base`（缺省）：只渲透明文字层，再 `src over` 干净底图（PIL `alpha_composite` 同口径）；
  //   · `render-only`：成品就是渲染结果（旧仓 SO009B 那样：产物与本页渲染 PNG 逐字节相同）；
  //   · `canvas`：把干净底图作 `<img id="bg">` 贴进页面、**整画布截图**，截图即成品
  //     —— 旧仓 SO009A / SO020 就是这么做的。这条口径会把 Chrome 的 premultiply 往返
  //     （`a=0` 的 RGB 归零、`0<a<255` 取整）烙进底图，而那些像素**就在生效版里**
  //     ⇒ 要复现生效版就必须走这条；反过来，生效版是 PIL 合成的图（SO002）走这条会差 1500+ px。
  const compose = recipe.text.compose ?? 'over-base';
  const useCanvas = compose === 'canvas';
  const html = buildHtml(recipe, effects, {
    width: orig.width,
    height: orig.height,
    fontRegular: fonts.regular,
    fontBold: fonts.bold,
    backgroundImage: useCanvas ? path.basename(cleanPng) : undefined,
  });
  const htmlPath = path.join(work, `${block}-text.html`);
  fs.writeFileSync(htmlPath, html);
  const layerPng = path.join(work, `${block}-textlayer.png`);
  renderHtml({ htmlPath, width: orig.width, height: orig.height, outPath: layerPng, chrome });
  log(`文字层：${htmlPath} → ${layerPng}（headless Chrome，画布 ${orig.width}×${orig.height} 透明底）`);
  const layer = await readPng(layerPng);

  // ── 合成
  const renderOnly = compose === 'render-only';
  const final = renderOnly || useCanvas ? clone(layer) : composeOver(clone(cleaned), layer);
  const outPng = path.join(work, `${block}-${current}.png`);
  await writePng(final, outPng);

  // ── AGF 注回（保留原件头 / meta / 调色板 / trailer）
  const agfOut = path.join(work, `${block}.AGF`);
  fs.writeFileSync(agfOut, injectImage(orig.agf, { width: orig.width, height: orig.height, rgba: final.bitmap.data }));
  log(`注回：${agfOut}（body ${recipe.source.agf} 的调色板最近色量化）`);

  return { block, current, recipe, width: orig.width, height: orig.height, base, cleaned, layer, final, files: { cleanPng, layerPng, outPng, agfOut, htmlPath } };
}

/**
 * 与 `corpus/assets/ui-images/<block>-<current>.png` 逐像素对照。
 * @returns {{ok:boolean, diff:object, refFile:string}}
 */
export async function verifyAgainstCorpus({ block, final, repoRoot }) {
  const versions = readVersions(repoRoot);
  const current = versions.images?.[block]?.current;
  const refFile = path.join(repoRoot, 'corpus', 'assets', 'ui-images', `${block}-${current}.png`);
  if (!fs.existsSync(refFile)) return { ok: false, diff: null, refFile, reason: `参考图不在场：${refFile}` };
  const ref = await readPng(refFile);
  const d = diff(final, ref);
  return { ok: d.channels === 0, diff: d, refFile };
}
