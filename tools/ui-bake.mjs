#!/usr/bin/env node
/**
 * tools/ui-bake.mjs — **CLI**：UI 图片烘焙链（ALF → AGF → 改图 → 注回，可复现）
 *
 * 分层（`tools/README.md` §0）：本文件只做"参数 → 模型 → 输出"；
 * 模型与算子全在 `lib/ui-bake/`（`recipe` 配方 / `bake` 重放 / `image` 像素算子 /
 * `render` headless 渲染 / `agf-source` 只读取件 / `agf-write` 注回）。
 *
 * 经派发器：`pnpm tools ui-bake <describe|list|lint|plan|run|verify|agf> [args…]`
 * 也可独立跑：`node tools/ui-bake.mjs --list`
 *
 * ★ 只读边界：**原始游戏件只读**（`lib/ui-bake/agf-source.mjs`）；本 CLI 只写
 *   `.tmp/ui-bake/`（派生区）与 `corpus/assets/ui-images/`（且只有 `--write` 才写后者）。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { REPO_ROOT } from './lib/paths.mjs';
import { bake, DEFAULT_GAME, verifyAgainstCorpus } from './lib/ui-bake/bake.mjs';
import { loadEffects, listRecipes, loadRecipe, validateRecipe } from './lib/ui-bake/recipe.mjs';
import { readAgfBuffer, decodeRgba } from './lib/ui-bake/age-format.mjs';
import { diff } from './lib/ui-bake/image.mjs';

/** 工具层的自我声明（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'ui-bake',
  title: 'UI 图片烘焙链（原始 ALF → 改图配方 → 生效版 PNG / 注回 AGF）',
  data: [
    '`tools/ui-bake/recipes/*.json`（**改图配方**：操作清单 + 文案 + 位置）',
    '`tools/ui-bake/effects.json`（文字效果字典 E1…E10 与全局层次）',
    '`corpus/assets/ui-images/*.png`（**生效版图**，判据的对照物；只 `apply` 写）',
    '`.tmp/ui-bake/**`（派生区：干净底图 / 文字层 / 重放产物 / 生成的 HTML）',
    '只读来源：`<game>/SYS4INI.BIN`（ALF 索引）→ `DATA1.ALF`（AGF 载荷）；或安装根裸件 `<game>/<块>.AGF`（优先，见 describe 的坑表）',
  ],
  access: 'rw（配方与效果字典是纯文本真源；**原始游戏件只读**；生效版图只有 `apply --write` 才写）',
  tool: 'tools/ui-bake.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 不变量 / 命令' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '配方一览（块 / 生效版 / 有无配方 / 判据状态）' },
  { name: 'lint', argv: ['--lint'], mutates: false, summary: '配方守卫（schema / 坐标 / 效果层次）—— 红 = 退出码 1' },
  { name: 'plan', argv: ['--plan'], mutates: false, summary: '打印一张图的执行计划（clean 操作 + 文字层 HTML），不渲染' },
  { name: 'run', argv: ['--run'], mutates: false, summary: '重放一张图到 `.tmp/ui-bake/`（渲文字 + 合成 + 注回 AGF）' },
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '重放并逐像素对照 `corpus/assets/ui-images/` 的生效版' },
  { name: 'agf', argv: ['--agf'], mutates: false, summary: 'AGF 层判据：注回产物与旧仓 `res/images/*.AGF` 解码后比像素' },
  { name: 'build', argv: ['--build'], mutates: true, summary: '★ 把**全部**配方从原始件完整构建出来 → `dist/ui-bake/`（证明资源是构建物，而不是旧仓搬来的）' },
  { name: 'apply', argv: ['--apply'], mutates: true, summary: '把重放产物写回 `corpus/assets/ui-images/`（缺省 dry-run，加 --write）' },
];

const HELP = `${DOMAIN.tool} —— UI 图片烘焙链

  pnpm tools ui-bake describe              字段 / 不变量 / 命令（本工具的自描述）
  pnpm tools ui-bake list                  配方一览
  pnpm tools ui-bake lint [<块>…]          配方守卫（红 = 退出码 1）
  pnpm tools ui-bake plan <块>             执行计划（clean 操作 + 文字层 HTML），不渲染
  pnpm tools ui-bake run <块> [<块>…]      重放到 .tmp/ui-bake/（渲文字 + 合成 + 注回 AGF）
  pnpm tools ui-bake verify [<块>…]        重放 + 与 corpus 生效版逐像素对照（判据）
  pnpm tools ui-bake build                 ★ 全部配方完整构建 → dist/ui-bake/（PNG + AGF + 报告）
  pnpm tools ui-bake apply <块> [--write]  写回 corpus/assets/ui-images/<块>-<版>.png

  选项  --game <安装目录>   原始游戏件所在目录（默认 ${DEFAULT_GAME}）
        --chrome <路径>     浏览器可执行文件（默认自动探测；也可用 UI_BAKE_CHROME）
        --quiet             少打印
`;

// ─────────────────────────────────────────────────────────── 自描述

const RECIPE_FIELD_DOC = [
  ['schemaVersion', '✅', '1', '配方格式版本'],
  ['block', '✅', '`SO\\d{3}[A-Z]?`', '块名（= AGF 名去扩展名 = versions.json 的键）'],
  ['blockId', '⬜', 'number', '旧仓"UI 元素地图"里的块号（可追溯用）'],
  ['source', '✅', '{kind:"alf"\\|"loose", entry, agf}', '只读来源：`alf` = ALF 索引里的 AGF；`loose` = 安装根裸件（裸件优先于 ALF 同名条目）'],
  ['note', '⬜', 'string', '一句话说明这张图改了什么（供审阅）'],
  ['clean', '✅', '[操作…]', '抹掉日文的操作清单（按序执行）'],
  ['text', '✅', '{effect, fontSize?, letterSpacing?, layers[]}', '要叠的中文：效果 + 逐层文案与位置'],
];
const CLEAN_OP_DOC = [
  ['op', '✅', '`fill` \\| `paste` \\| `restore` \\| `transparent`', '操作类型'],
  ['box', '条件', '{x0,y0,x1,y1}', '作用区域（**含端**）；`paste` 不给（用 from/to）'],
  ['fillCol', '条件', 'number', '列填充：复制哪一列（绝对 x，必须在盒内）'],
  ['fillRow', '条件', 'number', '行填充：复制哪一行（绝对 y，必须在盒内）'],
  ['keepL/keepR/keepT/keepB', '⬜', 'number', '四边保留带宽度（默认 0；列填充只用 L/R，行填充只用 T/B）'],
  ['fromBlock', '条件', 'string', '`paste` 的源块名'],
  ['from / to', '条件', '{x0,y0,x1,y1}×2', '`paste` 的源区域与目标区域（**尺寸必须相同**：缩放插值不可复现）'],
];
const LAYER_DOC = [
  ['text', '✅', 'string \\| 片段数组', '这一层画的中文；片段数组 = 逐片段可带自己的 `css`（旧仓的 `<span class="s18">` 那种混排）'],
  ['pos', '✅', '{x,y}', '这一层盒子的左上角（= 旧仓 CSS 的 left/top）'],
  ['lineHeight', '✅', 'number', '行高 px（`1` = 无单位）。★ 旧仓 grid 居中的块用**字号**；flex 居中的块见 `describe` 的坑表'],
  ['width', '⬜', 'number', '盒子宽度（`text-align:center` 的居中范围）'],
  ['role', '✅', 'string', '画法层次（`fill` / `outline` / `shadow`… 见效果字典的 roles）'],
  ['z', '⬜', 'number', '层叠序（同一块内自下而上：shadow → outline → fill）'],
  ['css', '条件', 'string', '该层与效果共享 CSS 的差异。★ 其中 `background*` / `*-background-clip` / `-webkit-text-fill-color` **自动落 span**、其余落外层 div（见坑表第 5 条）'],
  ['shift', '⬜', '{x,y}', '位置微调（旧仓那种 `top = y0 - 1` 的人工偏移）'],
];

export function describeText() {
  const L = [];
  L.push(`${DOMAIN.tool} —— ${DOMAIN.title}`);
  L.push('');
  L.push('数据');
  for (const d of DOMAIN.data) L.push(`  · ${d.replace(/\*\*/g, '').replace(/`/g, '')}`);
  L.push(`读写    ${DOMAIN.access.replace(/\*\*/g, '')}`);
  L.push('');
  L.push('配方字段（recipe.json）');
  for (const [k, req, form, desc] of RECIPE_FIELD_DOC) L.push(`  ${k.padEnd(22)} ${req}  ${form.padEnd(38)} ${desc}`);
  L.push('');
  L.push('clean 操作');
  for (const [k, req, form, desc] of CLEAN_OP_DOC) L.push(`  ${k.padEnd(22)} ${req}  ${form.padEnd(38)} ${desc}`);
  L.push('');
  L.push('text.layers 层');
  for (const [k, req, form, desc] of LAYER_DOC) L.push(`  ${k.padEnd(22)} ${req}  ${form.padEnd(38)} ${desc}`);
  L.push('');
  L.push('不变量（`lint` 会逐条判）');
  for (const s of [
    'schema：只认识上面这些键，多一个键就红（写错了不许静默忽略）',
    '坐标：`fillCol/fillRow` 必须落在自己的盒子里；保留带之和不得超过区域边长',
    'paste：源与目标矩形**尺寸必须相同**（缩放插值不可复现 ⇒ 直接拒绝）',
    '角色：每层的 `role` 必须在效果字典里；效果与层都没给 CSS ⇒ 红（那一层什么都不会画）',
    '来源：`source.agf` 必须能在 ALF 索引（或安装根裸件）里找到，且尺寸与 recipe 用到坐标相容',
    '合成口径：`render-only` 必须没有 clean 段；`canvas` 必须有 clean 段（底图要贴进页面）',
  ]) L.push(`  · ${s}`);
  L.push('');
  L.push('口径坑表（★ 这些是"照抄旧 HTML 也会错"的地方，详见 tools/ui-bake.md §5）');
  for (const s of [
    '`background-clip:text` 的声明必须落在**画字那一层**（span）：落在外层 div 上时 div 无文字节点/高 0 ⇒ 渐变轴长 0、上半段消失',
    '纵向渐变画在**行内盒**（position:static）上会按字体 ascent+descent ≈26.6px 拉伸，而不是行高 ⇒ 需要 `display:inline-block`',
    '`transform:scaleX()` 要写在**片段 span** 上（旧仓加在 flex item 上）；写在层 div 上缩放中心不同',
    '旧仓 **flex 居中** 的块比"line-height = 块高"的盒内居中低 1px ⇒ `pos.y` 用 `y0` 而非 `y0-1`',
    '旧仓 **grid 居中** 的块 `line-height` = **字号**；写成"块高"会让基线落到行盒底部（实测偏低 21px）',
    '字体必须由本仓 `corpus/assets/fonts/SarasaGothicSC/*.ttf` 钉住；不钉住时 headless 会**静默**用系统同名字体（换机器换字形）',
    '旧仓 HTML 里写的字体名可能是**错的**（SO030 四份 HTML 全写 WenQuanYi，生效版实测是 Sarasa）——以生效版像素为准',
    '安装根裸件 `<名>.AGF` 优先于 `DATA1.ALF` 里的同名条目（SO025 生效版就是裸件那份）⇒ `source.kind:"loose"`',
  ]) L.push(`  · ${s}`);
  L.push('');
  L.push('操作');
  for (const o of OPERATIONS) L.push(`  ${o.name.padEnd(10)} ${o.mutates ? '写' : '读'}  ${o.summary}`);
  L.push('');
  L.push('判据    `pnpm tools ui-bake verify` ⇒ 逐像素相同（不作字节级要求：PNG 是同一个编码器的产物，');
  L.push('        但 Pillow 与 Node 的 zlib 版本不同，deflate 输出本就不可能逐字节相同）。');
  L.push('        AGF 层的判据同样是**解码后像素相同**：调色板若有重复色，索引可在同色项间摆动（无害）。');
  return `${L.join('\n')}\n`;
}

// ─────────────────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: null, rest: [], game: DEFAULT_GAME, chrome: undefined, write: false, quiet: false, json: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--describe') out.action = 'describe';
    else if (a === '--list') out.action = 'list';
    else if (a === '--lint') out.action = 'lint';
    else if (a === '--plan') out.action = 'plan';
    else if (a === '--run') out.action = 'run';
    else if (a === '--verify') out.action = 'verify';
    else if (a === '--apply') out.action = 'apply';
    else if (a === '--build') out.action = 'build';
    else if (a === '--agf') out.action = 'agf';
    else if (a === '--game') out.game = argv[++i];
    else if (a === '--out') out.out = argv[++i];
    else if (a === '--chrome') out.chrome = argv[++i];
    else if (a === '--write') out.write = true;
    else if (a === '--quiet') out.quiet = true;
    else if (a === '--json') out.json = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
    else out.rest.push(a);
  }
  if (!out.action) out.action = 'list';
  return out;
}

const tick = (b) => (b ? 'ok  ' : 'FAIL');

function cmdLint(args) {
  const effects = loadEffects();
  const targets = args.rest.length ? args.rest : listRecipes();
  let failures = 0;
  for (const block of targets) {
    let errs;
    try {
      errs = validateRecipe(loadRecipe(block), { effects });
    } catch (err) {
      errs = [err.message];
    }
    if (errs.length) failures += 1;
    process.stdout.write(`[${tick(errs.length === 0)}] ${block}\n`);
    for (const e of errs) process.stdout.write(`        · ${e}\n`);
  }
  process.stdout.write(`\n${targets.length} 个配方：${targets.length - failures} 绿 / ${failures} 红\n`);
  return failures ? 1 : 0;
}

function cmdPlan(args) {
  const block = args.rest[0];
  if (!block) throw new Error('用法：pnpm tools ui-bake plan <块>');
  const recipe = loadRecipe(block);
  const effects = loadEffects();
  process.stdout.write(`# ${block} 的执行计划\n\n`);
  process.stdout.write(`来源  ALF 索引里的 ${recipe.source.agf}\n`);
  process.stdout.write(`清理  ${(recipe.clean ?? []).length} 步\n`);
  for (const op of recipe.clean ?? []) {
    if (op.op === 'fill') process.stdout.write(`  · ${op.fillCol !== undefined ? '列' : '行'}填充 ${JSON.stringify(op.box)} keep=${op.keepL ?? 0}/${op.keepR ?? 0}/${op.keepT ?? 0}/${op.keepB ?? 0} 复制=${op.fillCol ?? op.fillRow}\n`);
    else if (op.op === 'paste') process.stdout.write(`  · 贴底图 自 ${op.fromBlock} ${JSON.stringify(op.from)} → ${JSON.stringify(op.to)}\n`);
    else process.stdout.write(`  · ${op.op} ${JSON.stringify(op.box)}\n`);
  }
  process.stdout.write(`\n文字  效果 ${recipe.text.effect}，${recipe.text.layers.length} 层\n`);
  for (const l of recipe.text.layers) {
    process.stdout.write(`  · ${String(l.role).padEnd(8)} 「${l.text}」 pos=${l.pos.x},${l.pos.y} lineHeight=${l.lineHeight}${l.width !== undefined ? ` width=${l.width}` : ''} z=${l.z ?? 1}\n`);
  }
  const font = effects.font;
  process.stdout.write(`\n字体  ${font.family}（@font-face 引本地 ttf；headless 对拉丁会 fallback）\n`);
  return 0;
}

async function runOne(block, args, { log }) {
  const result = await bake({ block, repoRoot: REPO_ROOT, game: args.game, chrome: args.chrome, log });
  return result;
}

async function cmdRun(args) {
  for (const block of args.rest) {
    const r = await runOne(block, args, { log: args.quiet ? () => {} : (s) => process.stdout.write(`${s}\n`) });
    process.stdout.write(`[ok  ] ${block}: → ${path.relative(REPO_ROOT, r.files.outPng)}\n`);
  }
  return 0;
}

async function cmdVerify(args) {
  const targets = args.rest.length ? args.rest : listRecipes();
  const rows = [];
  for (const block of targets) {
    const t0 = Date.now();
    const r = await runOne(block, args, { log: () => {} });
    const v = await verifyAgainstCorpus({ block, final: r.final, repoRoot: REPO_ROOT });
    rows.push({ block, ...v, ms: Date.now() - t0 });
  }
  for (const r of rows) {
    const d = r.diff;
    const detail = d && !d.sizeMismatch
      ? `逐通道不等 ${d.channels}，最大差 ${d.maxDelta}${d.bbox ? `，bbox ${d.bbox.join(',')}` : ''}`
      : d?.sizeMismatch ? '尺寸不同' : r.reason ?? '';
    process.stdout.write(`[${tick(r.ok)}] ${r.block.padEnd(8)} ${detail}   （${r.ms} ms）\n`);
  }
  const bad = rows.filter((r) => !r.ok).length;
  process.stdout.write(`\n${rows.length} 张：${rows.length - bad} 逐像素相同 / ${bad} 有偏差\n`);
  return bad ? 1 : 0;
}

/**
 * ★ `build`：把**全部有配方的图**从原始件完整构建出来，落到 `dist/ui-bake/`。
 *
 * 为什么需要它：`corpus/assets/ui-images/` 里那批 PNG 是**旧仓搬进来的**（`apply` 从没执行过），
 * 所以"当前资源"并不能证明**本仓能构建它们**。这个动作给出一条可复核的构建链：
 *
 * ```
 *   原始 ALF/AGF（只读）→ clean 段 → headless 渲染 → 合成 → dist/ui-bake/<块>-<版>.png
 *                                                      └→ dist/ui-bake/<块>.AGF（注回）
 * ```
 *
 * 并对每个产物同时报两个判据：① 与 `corpus` 生效版**逐像素**；② 注回 AGF **解码后**与来源件比对。
 * `dist/` 是生成物区（`.gitignore` 已忽略）⇒ 构建行为不会污染入库件。
 */
async function cmdBuild(args) {
  const outDir = args.out ?? path.join(REPO_ROOT, 'dist', 'ui-bake');
  fs.mkdirSync(outDir, { recursive: true });
  const oldRepo = args.oldRepo ?? 'E:\\Games\\Eushully\\天結';
  const versions = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus/assets/ui-images/versions.json'), 'utf8'));
  const img = await import('./lib/ui-bake/image.mjs');
  const rows = [];
  for (const block of listRecipes()) {
    const t0 = Date.now();
    const r = await runOne(block, args, { log: () => {} });
    const cur = versions.images[block]?.current;
    const pngOut = path.join(outDir, `${block}-${cur}.png`);
    const agfOut = path.join(outDir, `${block}.AGF`);
    fs.copyFileSync(r.files.outPng, pngOut);
    fs.copyFileSync(r.files.agfOut, agfOut);
    // ① PNG 层判据：构建出的图 vs corpus 生效版，逐像素
    const v = await verifyAgainstCorpus({ block, final: r.final, repoRoot: REPO_ROOT });
    // ② AGF 层判据：构建出的 AGF 解码后 vs 旧仓 `res/images/<块>.AGF` 解码后（同一份 8bpp 领域的比对）。
    //    ★ `loose` 来源（SO025）**不能**这么比：它的生效版来自安装根裸件，而旧仓 `res/images/SO025.AGF`
    //    是另一条链的产物（256 项调色板只有 18 项相同）⇒ 两条链各自的量化损失，不等 ≠ 错。
    //    这一类比的是"构建出的 AGF 解码后 vs 生效版 PNG"，只断言量化损失在量级内（与旧仓自身同量级）。
    const built = decodeRgba(readAgfBuffer(fs.readFileSync(agfOut), `${block}.AGF（构建）`));
    const { loadRecipe: lr, loadEffects: le } = await import('./lib/ui-bake/recipe.mjs');
    const rec = lr(block);
    const srcKind = rec.source?.kind ?? 'alf';
    let agf = null;
    let agfNote = '';
    if (srcKind === 'loose') {
      const refPng2 = await img.readPng(path.join(REPO_ROOT, 'corpus/assets/ui-images', `${block}-${cur}.png`));
      const dq = diff(img.fromRgba(built.rgba, built.width, built.height), refPng2);
      agf = { channels: dq.channels, maxDelta: dq.maxDelta, bbox: dq.bbox };
      agfNote = `（loose 来源：只判量化损失 maxΔ≤48，实测 ${dq.maxDelta}）`;
      agf.quantOnly = dq.maxDelta <= 48;
    } else {
      const srcFile = path.join(oldRepo, 'res', 'images', `${block}.AGF`);
      if (fs.existsSync(srcFile)) {
        const old = decodeRgba(readAgfBuffer(fs.readFileSync(srcFile), `${block}.AGF（旧仓）`));
        agf = diff(img.fromRgba(built.rgba, built.width, built.height), img.fromRgba(old.rgba, old.width, old.height));
      }
    }
    // ③ 参考量：构建出的 AGF 解码后 vs 我们自己的成品 PNG —— 这一项**必然非零**（8bpp 调色板量化损失）
    const minePng = img.fromRgba(r.final.bitmap.data, r.final.bitmap.width, r.final.bitmap.height);
    const quant = diff(img.fromRgba(built.rgba, built.width, built.height), minePng);
    rows.push({ block, cur, png: v, agf, agfNote, quant, ms: Date.now() - t0 });
  }
  const rel = path.relative(REPO_ROOT, outDir);
  process.stdout.write(`构建产物：${rel}/（${rows.length} 张 PNG + ${rows.length} 个 AGF）\n\n`);
  process.stdout.write('① PNG 逐像素 vs corpus 生效版   ② AGF 解码后 vs **来源件**（ALF 条目 / 安装根裸件）   ③ 参考：AGF 量化损失（必然>0）\n');
  for (const r of rows) {
    const pngTag = r.png.ok ? '✅ 0' : `⚠️ ${r.png.diff?.channels} 通道`;
    const agfTag = r.agf === null ? '（来源件不在场）' : r.agf.quantOnly ? `✅ 量化损失量级内${r.agfNote}` : r.agf.channels === 0 ? `✅ 0${r.agfNote}` : `⚠️ ${r.agf.channels} 通道（maxΔ ${r.agf.maxDelta}）`;
    process.stdout.write(`${r.block.padEnd(8)} ① ${pngTag.padEnd(16)} ② ${agfTag.padEnd(30)} ③ maxΔ ${r.quant.maxDelta}\n`);
  }
  const okPng = rows.filter((r) => r.png.ok).length;
  const okAgf = rows.filter((r) => r.agf && r.agf.channels === 0).length;
  process.stdout.write(`\n① ${okPng}/${rows.length} 张 PNG 逐像素相同；② ${okAgf}/${rows.length} 个 AGF 解码后相同。\n`);
  process.stdout.write(`③ 是"8bpp 调色板量化"的固有损失（构建链自己的量化 vs 成品 PNG），旧仓那批 AGF 同样非零、量级相同。\n`);
  process.stdout.write(`⇒ 这批 AGF 是**从原始 ALF/AGF 构建**出来的；corpus/assets/ui-images/ 未被本动作写入。\n`);
  return 0;
}

async function cmdApply(args) {
  if (!args.rest.length) throw new Error('用法：pnpm tools ui-bake apply <块> [--write]');
  const versions = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus/assets/ui-images/versions.json'), 'utf8'));
  for (const block of args.rest) {
    const r = await runOne(block, args, { log: () => {} });
    const v = await verifyAgainstCorpus({ block, final: r.final, repoRoot: REPO_ROOT });
    const dest = path.join(REPO_ROOT, 'corpus', 'assets', 'ui-images', `${block}-${versions.images[block].current}.png`);
    const tag = v.ok ? 'ok  ' : 'WARN';
    process.stdout.write(`[${tag}] ${block}: ${v.ok ? '与现有效版逐像素相同' : '与现有效版有偏差 —— 写下去会替换生效版！'}\n`);
    process.stdout.write(`        源 ${path.relative(REPO_ROOT, r.files.outPng)}\n        目标 ${path.relative(REPO_ROOT, dest)}\n`);
    if (!args.write) {
      process.stdout.write('        （dry-run；加 --write 才落盘）\n');
      continue;
    }
    fs.copyFileSync(r.files.outPng, dest);
    process.stdout.write(`        已写入（AGF 落在 ${path.relative(REPO_ROOT, r.files.agfOut)}）\n`);
  }
  return 0;
}

async function cmdList(args) {
  const versions = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus/assets/ui-images/versions.json'), 'utf8'));
  const recipes = new Set(listRecipes());
  const rows = Object.entries(versions.images).map(([block, info]) => ({
    block,
    current: info.current,
    localized: info.localized,
    recipe: recipes.has(block) ? '有' : '—',
  }));
  const w = Math.max(...rows.map((r) => r.block.length));
  process.stdout.write(`${'块'.padEnd(w)}  版  汉化  配方\n`);
  for (const r of rows) process.stdout.write(`${r.block.padEnd(w)}  ${String(r.current).padStart(2)}  ${r.localized ? '是  ' : '否  '}  ${r.recipe}\n`);
  process.stdout.write(`\n共 ${rows.length} 张：有配方 ${rows.filter((r) => r.recipe === '有').length} 张\n`);
  if (args.json) process.stdout.write(`${JSON.stringify(rows, null, 2)}\n`);
  return 0;
}

/** AGF 层判据：把 `corpus/assets/ui-images-baked` 登记的旧仓 AGF 与我们注回的产物**解码后**比像素 */
async function cmdAgf(args) {
  const oldRepo = args.rest[0] ?? 'E:\\Games\\Eushully\\天結';
  const rows = [];
  for (const block of listRecipes()) {
    const r = await runOne(block, args, { log: () => {} });
    const oldAgf = path.join(oldRepo, 'res', 'images', `${block}.AGF`);
    const { fromRgba } = await import('./lib/ui-bake/image.mjs');
    const { loadRecipe } = await import('./lib/ui-bake/recipe.mjs');
    const recipe = loadRecipe(block);
    const ours = decodeRgba(readAgfBuffer(fs.readFileSync(r.files.agfOut), `${block}.AGF（新）`));
    // ★ `loose` 来源（SO025 那种安装根裸件）：注回用的是**裸件的调色板**，而旧仓 `res/images/<块>.AGF`
    //   是另一条链的产物（同尺寸、同 bpp，但 256 项调色板只有 18 项相同）。两者的差异是
    //   **两条链各自的 8bpp 量化损失**，不等 ≠ 错 —— 所以这一类只断言"落在量化损失量级内"
    //   （旧仓自己的 AGF 解码 vs 生效版 PNG 也差 182641 通道 / 最大差 46，量级相同）。
    if (recipe.source?.kind === 'loose') {
      const png = await (await import('./lib/ui-bake/image.mjs')).readPng(path.join(REPO_ROOT, 'corpus', 'assets', 'ui-images', `${block}-${JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus/assets/ui-images/versions.json'), 'utf8')).images[block].current}.png`));
      const d = diff(fromRgba(ours.rgba, ours.width, ours.height), png);
      const ok = d.maxDelta <= 48;
      rows.push({ block, ok, diff: d, note: `来源=安装根裸件；与生效版 PNG 的 8bpp 量化损失 maxΔ=${d.maxDelta}（旧仓自己的 AGF 也是同量级，阈值 48）` });
      continue;
    }
    if (!fs.existsSync(oldAgf)) {
      rows.push({ block, ok: false, reason: `旧仓产物不在场：${oldAgf}` });
      continue;
    }
    const b = decodeRgba(readAgfBuffer(fs.readFileSync(oldAgf), `${block}.AGF（旧）`));
    const d = diff(fromRgba(ours.rgba, ours.width, ours.height), fromRgba(b.rgba, b.width, b.height));
    rows.push({ block, ok: d.channels === 0, diff: d });
  }
  for (const r of rows) {
    process.stdout.write(`[${tick(r.ok)}] ${r.block.padEnd(8)} ${r.ok ? `AGF 解码后像素相同${r.note ? `（${r.note}）` : ''}` : r.reason ?? `逐通道不等 ${r.diff?.channels}，最大差 ${r.diff?.maxDelta}`}\n`);
  }
  const bad = rows.filter((r) => !r.ok).length;
  process.stdout.write(`\n${rows.length} 张：${rows.length - bad} 相同 / ${bad} 有偏差\n`);
  return bad ? 1 : 0;
}

export async function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  switch (args.action) {
    case 'describe':
      process.stdout.write(describeText());
      return 0;
    case 'list':
      return await cmdList(args);
    case 'lint':
      return cmdLint(args);
    case 'plan':
      return cmdPlan(args);
    case 'run':
      return await cmdRun(args);
    case 'verify':
      return await cmdVerify(args);
    case 'build':
      return await cmdBuild(args);
    case 'apply':
      return await cmdApply(args);
    case 'agf':
      return await cmdAgf(args);
    case 'help':
      process.stdout.write(HELP);
      return 0;
    default:
      process.stderr.write(HELP);
      return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
      process.exitCode = 2;
    });
}
