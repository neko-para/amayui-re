/**
 * tools/lib/ui-bake/recipe.mjs —— **领域模型**：一张 UI 图的"改图配方"（recipe）
 *
 * 一个 recipe 就是**旧仓那次手工改图的完整操作清单**，落成 JSON 之后可由脚本重放：
 *   ① `clean` —— 怎么把日文原图抹成"无文字的干净底图"（列/行填充、贴底图、局部恢复、置透明）
 *   ② `text`  —— 在干净底图上叠什么中文（文案 / 位置 / CSS 层级），由 headless Chrome 渲成透明文字层
 *   ③ 合成    —— `文字层 src over 干净底图`（PIL `alpha_composite` 同口径，见 `image.mjs`）
 *
 * ## 为什么是"配方"而不是"脚本"
 * 旧仓那些 `.tmp/*.py` 的坐标写在命令行里（`clean_fill.py --x0 274 …`），**不可审计、不可回归**。
 * 这里把坐标 / 保留带 / 填充列 / CSS **全部变成数据** ⇒ 一张图的改动可以被 `git diff` 审阅、
 * 被 `verify` 逐像素对照、被守卫挡住"悄悄改参数"。
 *
 * ## 与旧仓 JSON 的血缘（字段名刻意对齐，便于机械搬运与人工核对）
 *   · `clean[].fillCol/keepL/keepR/keepT/keepB` ← `.tmp/<名>_clean.json`（清理工作台导出）
 *   · `text.layers[].box{x0,y0,x1,y1}`（含端）   ← `.tmp/<名>_selected.json`（UI 元素地图导出）
 * 唯一的差别：旧仓把 CSS 写死在 HTML 里，这里提成"效果字典（`effects.json`）+ 每块覆盖（`layer.css`）"。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { fontFaceCss, htmlShell } from './render.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const UI_BAKE_DIR = path.join(HERE, '..', '..', 'ui-bake');
export const RECIPE_DIR = path.join(UI_BAKE_DIR, 'recipes');
export const EFFECTS_FILE = path.join(UI_BAKE_DIR, 'effects.json');

/** recipe 认识的键（多余键一律报错：写错了要红，不要静默忽略） */
const TOP_KEYS = new Set(['schemaVersion', 'block', 'blockId', 'source', 'note', 'clean', 'text', '_doc']);
const CLEAN_KEYS = new Set([
  'op', 'box', 'keepL', 'keepR', 'keepT', 'keepB', 'fillCol', 'fillRow', 'fromBlock', 'from', 'to', '_note',
  // 拉普拉斯模板清理（`op:"laplace"`）
  'mode', 'w', 'h', 'iters', 'ix0', 'ix1', 'iy1', 'x0', 'y0', 'rows', 'cols',
  // 模板覆盖（`op:"template"`）
  'source', 'at',
]);
const TEXT_KEYS = new Set(['effect', 'fontSize', 'letterSpacing', 'layers', 'compose', '_note']);
const LAYER_KEYS = new Set([
  // 通用
  'kind', '_note',
  // html 层
  'text', 'pos', 'lineHeight', 'width', 'role', 'z', 'css', 'shift', 'align', 'spanBox',
  // svg 层
  'effect', 'rows', 'parts', 'fontSize', 'fontWeight', 'fontFamily', 'fill', 'fillOpacity', 'ringColor',
]);

export function loadEffects(file = EFFECTS_FILE) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

export const recipeFile = (block, dir = RECIPE_DIR) => path.join(dir, `${block}.json`);

export function loadRecipe(block, dir = RECIPE_DIR) {
  const f = recipeFile(block, dir);
  if (!fs.existsSync(f)) throw new Error(`没有这张图的配方：${f}`);
  return JSON.parse(fs.readFileSync(f, 'utf8'));
}

export function listRecipes(dir = RECIPE_DIR) {
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((f) => f.endsWith('.json'))
    .map((f) => f.slice(0, -5))
    .sort();
}

const isInt = (v) => Number.isInteger(v);
const isBox = (b) => b && isInt(b.x0) && isInt(b.y0) && isInt(b.x1) && isInt(b.y1) && b.x1 >= b.x0 && b.y1 >= b.y0;
const isPos = (p) => p && isInt(p.x) && isInt(p.y);

/**
 * 校验一个 recipe，返回问题清单（空数组 = 绿）。
 * @param {object} r
 * @param {{effects?: object}} [opts] 给了效果字典就顺带校验 `effect` / `layers[].role`
 */
export function validateRecipe(r, { effects = null } = {}) {
  const errs = [];
  const bad = (m) => errs.push(m);
  if (!r || typeof r !== 'object' || Array.isArray(r)) return ['不是一个对象'];
  for (const k of Object.keys(r)) if (!TOP_KEYS.has(k)) bad(`未知顶层键 \`${k}\``);
  if (r.schemaVersion !== 1) bad(`schemaVersion 必须是 1（实际 ${JSON.stringify(r.schemaVersion)}）`);
  if (typeof r.block !== 'string' || !/^SO\d{3}[A-Z]?$/.test(r.block)) bad(`block 形态不是 SO\\d{3}[A-Z]?：${JSON.stringify(r.block)}`);
  if (r.blockId !== undefined && (!isInt(r.blockId) || r.blockId < 0)) bad('blockId 必须是自然数');
  if (!r.source || !['alf', 'loose'].includes(r.source.kind) || !isInt(r.source.entry) || typeof r.source.agf !== 'string')
    bad('source 必须是 { kind:"alf"|"loose", entry:<ALF 目录序号>, agf:"<名>.AGF" }（`loose` = 取安装根裸件，裸件优先于 ALF 条目）');
  if (r.note !== undefined && typeof r.note !== 'string') bad('note 必须是字符串');

  const clean = r.clean ?? [];
  if (!Array.isArray(clean)) bad('clean 必须是数组');
  else
    clean.forEach((op, i) => {
      const at = `clean[${i}]`;
      if (!op || typeof op !== 'object') return bad(`${at} 不是对象`);
      for (const k of Object.keys(op)) if (!CLEAN_KEYS.has(k)) bad(`${at} 未知键 \`${k}\``);
      if (!['fill', 'paste', 'restore', 'transparent', 'laplace', 'template'].includes(op.op))
        return bad(`${at}.op 非法：${JSON.stringify(op.op)}`);
      if (!['paste', 'laplace', 'template'].includes(op.op) && !isBox(op.box))
        bad(`${at}.box 必须是 {x0,y0,x1,y1} 且 x1>=x0 / y1>=y0`);
      if (op.op === 'laplace') {
        if (Array.isArray(op.cols)) {
          // 「全部 15 个中簇块」形式（旧仓 `so001_clean15.py` 的循环）：模式写在每个 col 里
          for (const k of ['w', 'h', 'ix0', 'ix1', 'iy1']) if (!isInt(op[k])) bad(`${at}.${k} 必须是整数（15 块形式）`);
          if (!Array.isArray(op.rows) || !op.rows.length) bad(`${at}.rows 必须是非空数组（15 块形式）`);
          op.cols.forEach((c, j) => {
            if (!Array.isArray(c) || !isInt(c[0]) || !['warm', 'gray', 'cyan'].includes(c[1])) bad(`${at}.cols[${j}] 必须是 [x, warm|gray|cyan]`);
          });
        } else {
          if (!['warm', 'gray', 'cyan'].includes(op.mode)) bad(`${at}.mode 只认 warm / gray / cyan：${JSON.stringify(op.mode)}`);
          if (!isInt(op.x0) || !isInt(op.y0)) bad(`${at} 单块形式必须给整数 x0 / y0`);
          if (!isInt(op.w) || op.w <= 0 || !isInt(op.h) || op.h <= 0) bad(`${at} 单块形式必须给正整数 w / h`);
        }
        if (op.iters !== undefined && (!isInt(op.iters) || op.iters < 0)) bad(`${at}.iters 必须是自然数`);
        return;
      }
      if (op.op === 'template') {
        if (typeof op.source !== 'string' || !op.source) bad(`${at}.source 必须是"从哪张图取模板"（块名）`);
        if (!isBox(op.from)) bad(`${at}.from 必须是模板矩形 {x0,y0,x1,y1}`);
        if (!Array.isArray(op.at) || !op.at.length) bad(`${at}.at 必须是非空数组（模板要盖到哪些左上角）`);
        else
          op.at.forEach((p, j) => {
            if (!p || !isInt(p.x) || !isInt(p.y)) bad(`${at}.at[${j}] 必须是 {x:<int>, y:<int>}`);
          });
        return;
      }
      for (const k of ['keepL', 'keepR', 'keepT', 'keepB']) if (op[k] !== undefined && !isInt(op[k])) bad(`${at}.${k} 必须是整数`);
      if (op.op === 'fill') {
        const hasCol = isInt(op.fillCol);
        const hasRow = isInt(op.fillRow);
        if (hasCol === hasRow) bad(`${at} 是 fill，必须**恰好**给 fillCol 或 fillRow 之一`);
        if (hasCol || hasRow) {
          const axis = hasCol ? 'x' : 'y';
          const src = hasCol ? op.fillCol : op.fillRow;
          const lo = axis === 'x' ? op.box.x0 : op.box.y0;
          const hi = axis === 'x' ? op.box.x1 : op.box.y1;
          if (src < lo || src > hi) bad(`${at}.fill${hasCol ? 'Col' : 'Row'}=${src} 落在区域外（${lo}…${hi}）`);
          const keepA = hasCol ? (op.keepL ?? 0) : (op.keepT ?? 0);
          const keepB = hasCol ? (op.keepR ?? 0) : (op.keepB ?? 0);
          const span = hi - lo + 1;
          if (keepA + keepB > span) bad(`${at} 保留带 ${keepA}+${keepB} 超过区域边长 ${span}`);
        }
      }
      if (op.op === 'paste') {
        if (!isBox(op.from) || !isBox(op.to)) bad(`${at} 是 paste，必须给 from 与 to 两个矩形`);
        else {
          const sw = op.from.x1 - op.from.x0 + 1;
          const sh = op.from.y1 - op.from.y0 + 1;
          const tw = op.to.x1 - op.to.x0 + 1;
          const th = op.to.y1 - op.to.y0 + 1;
          if (sw !== tw || sh !== th) bad(`${at} paste 的源 ${sw}×${sh} 与目标 ${tw}×${th} 尺寸不同（缩放插值不可复现）`);
        }
        if (typeof op.fromBlock !== 'string' || !op.fromBlock) bad(`${at} 是 paste，必须给 fromBlock（源块名）`);
      }
    });

  if (!r.text) bad('缺 text 段（要渲什么中文）');
  else {
    for (const k of Object.keys(r.text)) if (!TEXT_KEYS.has(k)) bad(`text 未知键 \`${k}\``);
    const eff = effects && r.text.effect ? effects.effects?.[r.text.effect] : null;
    if (effects && r.text.effect && !eff) bad(`text.effect 不在效果字典里：${r.text.effect}`);
    if (r.text.compose !== undefined && !['over-base', 'render-only', 'canvas'].includes(r.text.compose))
      bad(`text.compose 只能是 "over-base"（缺省）/ "render-only" / "canvas"：${JSON.stringify(r.text.compose)}`);
    if (r.text.compose === 'render-only' && (r.clean ?? []).length)
      bad('text.compose="render-only" 时 clean 段无效（成品就是渲染结果、不含底图）—— 应该删掉 clean');
    if (r.text.compose === 'canvas' && !(r.clean ?? []).length)
      bad('text.compose="canvas" 时必须有 clean 段（底图要贴进页面）');
    if (!Array.isArray(r.text.layers) || !r.text.layers.length) bad('text.layers 必须是非空数组');
    else
      r.text.layers.forEach((l, i) => {
        const at = `text.layers[${i}]`;
        if (!l || typeof l !== 'object') return bad(`${at} 不是对象`);
        for (const k of Object.keys(l)) if (!LAYER_KEYS.has(k)) bad(`${at} 未知键 \`${k}\``);
        if (l.kind === 'svg') {
          // SVG 层：行 / 画法 / 字体必给；也可以整包引用效果字典的 `svg.<name>`（recipe 只写差异）
          const s = { ...(effects?.svg?.[l.effect] ?? {}), ...l };
          if (!Array.isArray(s.rows) || !s.rows.length) bad(`${at}.rows 必须是非空数组（每行一个 <text>）`);
          else
            s.rows.forEach((row, j) => {
              if (!isInt(row.x) || !isInt(row.y)) bad(`${at}.rows[${j}] 必须给整数 x / y`);
              if (row.text === undefined) bad(`${at}.rows[${j}].text 必须给`);
            });
          if (!isInt(s.fontSize)) bad(`${at}.fontSize 必须给（SVG 层不吃效果字典的字号）`);
          if (!Array.isArray(s.parts) || !s.parts.length) bad(`${at}.parts 必须给（要画哪几层，如 ["fill","inset","ring"]）`);
          if (!s.fill) bad(`${at}.fill 必须给（字形颜色；可来自效果字典的 svg.<名>）`);
          if (typeof l.text !== 'string' && l.text !== undefined) bad(`${at}.text 只允许出现在 html 层`);
          return;
        }
        for (const k of ['text', 'pos', 'lineHeight', 'role']) if (l[k] === undefined) bad(`${at}.${k} 必给（html 层）`);
        if (l.text !== undefined && typeof l.text !== 'string' && !Array.isArray(l.text)) bad(`${at}.text 必须是字符串或片段数组`);
        if (!l.pos || !isInt(l.pos.x) || !isInt(l.pos.y)) bad(`${at}.pos 必须是 {x:<int>, y:<int>}`);
        if (!isInt(l.lineHeight) || l.lineHeight <= 0) bad(`${at}.lineHeight 必须是正整数`);
        if (l.width !== undefined && (!isInt(l.width) || l.width <= 0)) bad(`${at}.width 必须是正整数`);
        if (typeof l.role !== 'string' || !l.role) bad(`${at}.role 必须给（用于生成类名与人工审阅）`);
        if (!eff) return;
        if (eff.layers && !eff.layers.includes(l.role) && !(effects.roles && effects.roles[l.role])) {
          bad(`${at}.role=${l.role} 既不在效果 ${r.text.effect} 的层次里（${(eff.layers ?? []).join('/')}），也不是效果字典里的全局层次`);
        }
        if (effects.roles && !effects.roles[l.role]) bad(`${at}.role=${l.role} 不在效果字典的 roles 里`);
        if (!eff.css?.[l.role] && !l.css) {
          bad(`${at}.role=${l.role} 既不在效果 ${r.text.effect} 的共享 CSS 里、自己也没给 css ⇒ 这一层什么都不会画`);
        }
      });
  }
  return errs;
}

// ─────────────────────────────────────────────────────────── CSS / HTML 生成

const px = (n) => `${n}px`;

const escapeHtml = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

/**
 * 把 `text` 归一成"片段数组"：字符串 → 单片段；数组 → 每段可带自己的 CSS。
 * 为什么要片段：CSS 的 `letter-spacing` **会在最后一个字符后面也加**一段间距，
 * 所以要收窄「ADV」这类拉丁串时，旧仓的写法是
 * `ADV<span style="letter-spacing:-0.4px">…`（只包住需要收窄的那几个字），
 * 而不是给整行加 letter-spacing —— 两者像素不同。
 */
function fragments(text) {
  if (typeof text === 'string') return [{ text }];
  return text.map((f) => (typeof f === 'string' ? { text: f } : f));
}

const fragHtml = (body, role) =>
  fragments(body)
    .map((f) => {
      const inner = escapeHtml(f.text);
      return f.css ? `<span class="${role}-f" style="${f.css}">${inner}</span>` : inner;
    })
    .join('');

// ── SVG（阴刻 / outline 类效果）：把旧仓那套 `feOffset + feComposite + feMorphology` 结构化

/** 内阴影：把字形向外偏 1px 相减 → 得到"棱"，再染黑半透明 */
const INSET_FILTER = (id) => `<filter id="${id}" x="-30%" y="-30%" width="160%" height="160%">
      <feOffset dx="1" dy="1" result="off"/>
      <feComposite in="SourceAlpha" in2="off" operator="out" result="diff"/>
      <feGaussianBlur in="diff" stdDeviation="0.2" result="blur"/>
      <feComposite in="blur" in2="SourceAlpha" operator="in" result="clip"/>
      <feFlood flood-color="#000000" flood-opacity="0.5" result="color"/>
      <feComposite in="color" in2="clip" operator="in"/>
    </filter>`;

/** 外环：字形膨胀 1px 后减去自身，染成棕褐 */
const RING_FILTER = (id, color) => `<filter id="${id}" x="-30%" y="-30%" width="160%" height="160%">
      <feMorphology operator="dilate" radius="1" in="SourceAlpha" result="d"/>
      <feComposite in="d" in2="SourceAlpha" operator="out" result="ring"/>
      <feFlood flood-color="${color}" result="c"/>
      <feComposite in="c" in2="ring" operator="in"/>
    </filter>`;

/**
 * 一个 SVG 文字层 = N 个 `<text>`（每行一个），每行按 `parts` 画多层（fill → inset → ring）。
 * 行数不多时 HTML 更省事；**这里专门服务"HTML 表达不了的效果"**（内阴影 / 膨胀外环），
 * 这也是旧仓第一列（SO009B 阴刻）唯一的路子。
 */
function svgLayerHtml(layer, index) {
  const parts = layer.parts ?? ['fill'];
  const insetId = `inset${index}`;
  const ringId = `ring${index}`;
  const defs = [];
  if (parts.includes('inset')) defs.push(INSET_FILTER(insetId));
  if (parts.includes('ring')) defs.push(RING_FILTER(ringId, layer.ringColor ?? '#b4887c'));
  const rows = layer.rows
    .map((row) => {
      const tspans = (row.parts ?? fragments(row.text))
        .map((f) => `<tspan${f.css ? ` style="${f.css.replace(/;/g, ';')}"` : ''}>${escapeHtml(f.text)}</tspan>`)
        .join('');
      return parts
        .map((p) => `  <text class="txt ${p}" x="${row.x}" y="${row.y}" dominant-baseline="text-before-edge">${tspans}</text>`)
        .join('\n');
    })
    .join('\n');
  return { defs: defs.join('\n    '), body: rows, css: `text.txt { font-family:"${layer.fontFamily}"; font-size:${layer.fontSize}px; font-weight:${layer.fontWeight ?? 'normal'}; }\n  .fill { fill:${layer.fill}; fill-opacity:${layer.fillOpacity ?? 1}; }\n  .inset { filter:url(#${insetId}); }\n  .ring { filter:url(#${ringId}); }` };
}

/**
 * 把一层的 CSS 声明分成"给盒子"和"给 span"两堆。
 *
 * ★ 为什么必须分：`background-clip:text` 只在**声明它的那个元素**上把背景裁成**它自己的文字**。
 *   旧仓把渐变写在最内层的 `<span class="ly">` 上，所以渐变正好落在字形里；
 *   如果把它写到外层 div 上，div 的背景会铺满整个 div 盒（渐变按盒高计算），
 *   而 div 自己**没有文字节点**（文字在 span 里）⇒ 裁出来的是"空文字"，渐变根本不上字
 *   —— 实测症状：字心变成描边色（深棕），且**看不出任何渐变**（SO001 中簇 22px 组）。
 */
const SPAN_SCOPED = /^(background|background-image|-webkit-background-clip|background-clip|-webkit-text-fill-color)$/;
function splitCss(css) {
  if (!css) return { box: '', span: '' };
  const box = [];
  const span = [];
  for (const decl of String(css).split(';')) {
    const d = decl.trim();
    if (!d) continue;
    const prop = d.slice(0, d.indexOf(':')).trim().toLowerCase();
    (SPAN_SCOPED.test(prop) ? span : box).push(d);
  }
  return { box: box.length ? `${box.join('; ')};` : '', span: span.length ? `${span.join('; ')};` : '' };
}

/**
 * 由 recipe + 效果字典生成文字层的 CSS 与正文。
 *
 * 定位口径（与旧仓 HTML **逐条对齐**，别改）：
 *   · 一层 = 一个绝对定位盒子（`left/top` = 该层左上角）＋ 若干 `<span>` 画法层次；
 *     `line-height` 等于行高时即**行内垂直居中**（旧仓 `.tb` 就是这么做的）；
 *   · 同块多层 = 多个 `<span>`，**后写的压在上面**（`z-index` 显式给）；
 *   · `pos`：给 `left/top`，`lineHeight` 给行高（`1` = 无单位）；
 *   · `shift` 是位置微调（旧仓的 `top = y0 - 1` 这类人工偏移）。
 *
 * 两种层：`kind` 缺省 = `html`（div + span）；`kind:"svg"` = SVG `<text>`（内阴影/膨胀外环这类
 * HTML 表达不了的效果，见 {@link svgLayerHtml}）。
 */
export function buildTextHtml(recipe, effects) {
  const eff = recipe.text.effect ? effects.effects[recipe.text.effect] : null;
  const fam = effects.font.family;
  const base = effects.font.base ?? {};
  const fontSize = recipe.text.fontSize ?? eff?.fontSize ?? base.fontSize;
  const letterSpacing = recipe.text.letterSpacing ?? eff?.letterSpacing ?? base.letterSpacing;
  const css = [`.b { position:absolute; font-family:"${fam}"; text-align:center; white-space:nowrap;`];
  if (fontSize !== undefined) css.push(` font-size:${px(fontSize)};`);
  if (letterSpacing !== undefined) css.push(` letter-spacing:${px(letterSpacing)};`);
  css.push(` }`);
  css.push(`.b > span { position:absolute; left:0; right:0; top:0; }`);
  const body = [];
  recipe.text.layers.forEach((l, i) => {
    if (l.kind === 'svg') {
      const s = svgLayerHtml({ ...effects.svg?.[l.effect], ...l }, i + 1);
      css.push(s.css);
      body.push(`<svg class="svg-layer" width="100%" height="100%" xmlns="http://www.w3.org/2000/svg">\n  <defs>\n    ${s.defs}\n  </defs>\n${s.body}\n</svg>`);
      return;
    }
    const cls = `l${i + 1}`;
    const { box, span } = splitCss(l.css);
    // `spanBox:"inline"` = 把 span 做成**行内盒**（`position:static; display:inline-block`），
    // 并用 flex 把这一层在盒子里居中 —— 这是旧仓 `.tb{display:grid;place-items:center}` + `.ly{grid-area:1/1}`
    // 的等价物。★ 为什么不能省：渐变（`background-clip:text`）画在**行内盒**上时，
    // 盒 = 文字宽 × 行高，渐变正好落在字形里；而工具缺省的绝对定位 span（`left:0;right:0`）
    // 宽度被拉满整盒、高度不定 ⇒ 渐变轴长/位置都不对（SO001 中簇、SO017 都踩过这一条）。
    const inlineSpan = l.spanBox === 'inline';
    const decl = [`left:${px(l.pos.x - (l.shift?.x ?? 0))}; top:${px(l.pos.y - (l.shift?.y ?? 0))};`];
    if (l.width !== undefined) decl.push(`width:${px(l.width)};`);
    if (l.align === 'center' || inlineSpan) decl.push('display:grid; place-items:center;');
    decl.push(`line-height:${l.lineHeight === 1 ? '1' : px(l.lineHeight)};`);
    if (l.z !== undefined) decl.push(`z-index:${l.z};`);
    if (box) decl.push(box);
    css.push(`#${cls} { ${decl.join(' ')} }`);
    const defs = [];
    if (eff?.css?.[l.role]) defs.push(eff.css[l.role]);
    if (span) defs.push(span);
    if (inlineSpan) defs.push('position:static; display:inline-block;');
    css.push(`#${cls} .${l.role} { ${defs.join(' ')} }`);
    body.push(`<div class="b" id="${cls}"><span class="${l.role}">${fragHtml(l.text, l.role)}</span></div>`);
  });
  return { css: css.join('\n'), body: body.join('\n') };
}

/** 生成完整 HTML（透明底，画布 = 图尺寸）。
 * @param {{backgroundImage?: string}} [opts] 给了就把底图作为 `<img id="bg">` 贴进页面（`compose:"canvas"`） */
export function buildHtml(recipe, effects, { width, height, fontRegular, fontBold, backgroundImage }) {
  const { css, body } = buildTextHtml(recipe, effects);
  const head = [fontFaceCss(fontRegular, fontBold, effects.font.family), css].filter(Boolean).join('\n');
  const bg = backgroundImage
    ? `#bg { position:absolute; left:0; top:0; width:${width}px; height:${height}px; }\n`
    : '';
  const bgTag = backgroundImage ? `<img id="bg" src="${backgroundImage}">\n` : '';
  return htmlShell({ width, height, headCss: `${head}\n${bg}`, body: `${bgTag}${body}` });
}
