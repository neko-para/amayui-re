#!/usr/bin/env node
/**
 * tools/fonts.mjs —— **CLI**：cnjp 分发字体的构建与校验（纯 Node，不依赖 fontTools）
 *
 * 规则住在 `tools/lib/fonts.mjs`（领域模型）；本文件只解析 argv、调它、打印。
 * 判据 = **除 `head.modified`（时间戳）与由它派生的两处校验和外，与可信产物逐字节相同**。
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  DEFAULT_BASE,
  DEFAULT_BOLD_BASE,
  DEFAULT_REF,
  FAMILY,
  PS_NAME,
  buildCnjp,
  fontFacts,
  fontProblems,
  normalizeHead,
  sha256,
} from './lib/fonts.mjs';

export const DOMAIN = {
  id: 'fonts',
  title: 'cnjp 分发字体（Sarasa SC 基底 + 码位重映射 + 族名/码页）',
  data: [
    '`corpus/assets/fonts/SarasaGothicSC/SarasaGothicSC-{Regular,Bold}.ttf`（**基底**，本地解压产物、gitignore；由 7z 解出）',
    '`data/translations/subs-cn-jp.json`（**简→日写法字典**，唯一入库真源；字体与 patch 共用同一个 `subsSha`）',
    '`corpus/assets/fonts/Amayui-CN_cnjp{,-Bold}.ttf`（**可信产物**，随包发；只读，当校验基准）',
    '`dist/fonts/`（构建落点，生成物、不入库）',
  ],
  access: 'r（基底 / 字典 / 基准）／w：只写 `dist/fonts/`；**不覆盖** `corpus/assets/fonts/` 里的可信产物',
  tool: 'tools/fonts.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：这个字体是什么 / 三步变换 / 判据 / 序列化口径' },
  { name: 'build', argv: ['--build'], mutates: true, summary: '从基底 + 字典构建：`[--bold] [--base <ttf>] [--dict <json>] [--out <文件>] [--write]`' },
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '★ 判据：构建结果与可信产物**除时间戳外逐字节相同**：`[--bold] [--ref <ttf>] [--json]`' },
];

const HELP = `tools/fonts.mjs —— cnjp 分发字体的构建与校验（★ 判据 = 除时间戳外逐字节相同）

  node tools/fonts.mjs --describe                 自描述（三步变换 / 判据 / 序列化口径）
  node tools/fonts.mjs --build [--write]          构建（缺省 dry-run：只报会写什么 + 统计）
  node tools/fonts.mjs --verify [--json]          ★ 校验：与 corpus 里的可信产物比（归一化时间戳后逐字节）

公共选项
  --bold              换成 Bold 那一支（基底 SarasaGothicSC-Bold.ttf / 基准 Amayui-CN_cnjp-Bold.ttf）
  --base  <ttf>       基底字体（缺省按 --bold 取 corpus/assets/fonts/SarasaGothicSC/ 下那份）
  --dict  <json>      简→日写法字典（缺省 data/translations/subs-cn-jp.json）
  --ref   <ttf>       校验基准（缺省 corpus/assets/fonts/Amayui-CN_cnjp{,-Bold}.ttf）
  --out   <文件>      构建落点（缺省 dist/fonts/Amayui-CN_cnjp{,-Bold}.ttf）
  --write             真的落盘（缺省 dry-run）
  --json              机器可读

★ 三步变换（全部由"成品 vs 基底"的逐字节差实测反推，见 tools/lib/fonts.mjs 的文件头）：
  ① \`cmap\`：反转字典（日写法→简体），对**除平台 1（Mac）外**的每个子表做 \`cmap[日写法] = cmap[简体]\`
     （原地改 ⇒ 允许链式）；其余 18 张表**原样搬运**（\`glyf\`/\`GPOS\`/\`hmtx\` 都不动）；
  ② \`name\`：id1/3/4/16 → \`${FAMILY}\`、id6 → \`${PS_NAME}\`（全语言记录一致）；
  ③ \`OS/2\`：\`ulCodePageRange1/2\` 按 fontTools \`recalcCodePageRanges\` 的算法（FontForge 直译）重算
     —— 932（JIS/Japan）那一票就在 range1 的 bit 17。
★ 序列化必须与 fontTools 对齐，否则体积与字节都会漂：cmap f4 用 \`splitRange\` 分段（含"拆分值不值"的阈值 4/8）、
  编译后**字节相同**的子表**共享偏移**、name 的**字符串去重**、表按 4 字节对齐、\`head.checkSumAdjustment\` 最后算。
★ 唯一非确定量 = \`head.modified\`（构建时间戳）：它连带改掉 head 的表校验和与文件级 \`checkSumAdjustment\`
  （实测共 9 字节）⇒ 判据把这三处归一化后再比。
★ ❌ 本工具**不**去覆盖 \`corpus/assets/fonts/Amayui-CN_cnjp*.ttf\`（那是校验基准）：字典改了要换基准，
  是一次**显式的再烘**（改完要同步 corpus 清单里的指纹），不是构建的副作用。
`;

function parseArgs(argv) {
  const out = {
    action: null, write: false, json: false, bold: false,
    base: null, dict: null, ref: null, out: null,
  };
  const takesValue = new Set(['base', 'dict', 'ref', 'out']);
  for (const a of argv) {
    if (['--describe', '--build', '--verify', '--help', '-h'].includes(a)) out.action = a.replace(/^--?/, '');
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--bold') out.bold = true;
    else if (a.startsWith('--')) {
      const [k, v] = [a.slice(2), argv[argv.indexOf(a) + 1]];
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      if (v === undefined || v.startsWith('--')) throw new Error(`${a} 需要值`);
      out[k] = v;
    } else throw new Error(`多余的位置参数：${a}`);
  }
  if (!out.action) out.action = 'help';
  return out;
}

const rel = (p) => path.relative(process.cwd(), p).replace(/\\/g, '/') || '.';
const mb = (n) => `${(n / 1048576).toFixed(2)} MB`;
const exists = (p) => p && fs.existsSync(p);
const pick = (v, dflt) => v ?? path.join(...dflt);

function pathsOf(args) {
  const base = pick(args.base, args.bold ? DEFAULT_BOLD_BASE : DEFAULT_BASE);
  const ref = pick(args.ref, args.bold ? ['corpus', 'assets', 'fonts', 'Amayui-CN_cnjp-Bold.ttf'] : DEFAULT_REF);
  const out = pick(args.out, ['dist', 'fonts', args.bold ? 'Amayui-CN_cnjp-Bold.ttf' : 'Amayui-CN_cnjp.ttf']);
  const dict = pick(args.dict, ['data', 'translations', 'subs-cn-jp.json']);
  return { base, dict, ref, out };
}

function readInputs({ base, dict, ref }, { needRef }) {
  if (!exists(base)) throw new Error(`基底字体不在：${base}（7z 解压产物，需先解出 SarasaGothicSC-*.ttf）`);
  if (!exists(dict)) throw new Error(`字典不在：${dict}`);
  if (needRef && !exists(ref)) throw new Error(`校验基准不在：${ref}`);
  return {
    baseBuf: fs.readFileSync(base),
    dict: JSON.parse(fs.readFileSync(dict, 'utf8')),
    refBuf: needRef ? fs.readFileSync(ref) : null,
  };
}

function cmdBuild(args) {
  const p = pathsOf(args);
  const { baseBuf, dict } = readInputs(p, { needRef: false });
  const t0 = Date.now();
  const { buf, stats } = buildCnjp({ baseBuf, dict });
  process.stdout.write(`基底         ${rel(p.base)}（${mb(baseBuf.length)}）\n`);
  process.stdout.write(`字典         ${rel(p.dict)}（${Object.keys(dict).length} 条）\n`);
  process.stdout.write(`重映射       ${stats.mappings} 次写入 · cmap 覆盖 ${stats.unicodeCodepoints} 个码位\n`);
  process.stdout.write(`族名         ${FAMILY} / ${PS_NAME}（改写 ${stats.renamed} 条 name 记录）\n`);
  process.stdout.write(`码页         uCodePageRange1 0x${stats.codePageRange1.toString(16)} · 2 0x${stats.codePageRange2.toString(16)}（bit17=932）\n`);
  process.stdout.write(`产物         ${mb(stats.bytes)} · ${stats.tables} 张表 · sha256 ${sha256(buf).slice(0, 16)}… · ${((Date.now() - t0) / 1000).toFixed(1)}s\n`);
  process.stdout.write(`落点         ${rel(path.resolve(p.out))}\n`);
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。校验：node tools/fonts.mjs --verify\n');
    return 0;
  }
  fs.mkdirSync(path.dirname(path.resolve(p.out)), { recursive: true });
  fs.writeFileSync(p.out, buf);
  process.stdout.write(`✔ 已写入     ${rel(path.resolve(p.out))}\n`);
  return 0;
}

function cmdVerify(args) {
  const p = pathsOf(args);
  const { baseBuf, dict, refBuf } = readInputs(p, { needRef: true });
  const t0 = Date.now();
  const { buf, stats } = buildCnjp({ baseBuf, dict });
  const problems = fontProblems({ built: buf, reference: refBuf });
  const built = fontFacts(buf);
  const ref = fontFacts(refBuf);
  const same = normalizeHead(buf).equals(normalizeHead(refBuf));

  if (args.json) {
    process.stdout.write(`${JSON.stringify({ base: rel(p.base), dict: rel(p.dict), reference: rel(p.ref), builtSha256: sha256(buf), referenceSha256: sha256(refBuf), sameAfterNormalizingTimestamp: same, stats, comparisons: { built, reference }, problems }, null, 1)}\n`);
    return problems.length ? 1 : 0;
  }
  process.stdout.write(`基底         ${rel(p.base)}\n`);
  process.stdout.write(`字典         ${rel(p.dict)}（${Object.keys(dict).length} 条）\n`);
  process.stdout.write(`基准         ${rel(p.ref)}（${mb(refBuf.length)}）\n`);
  process.stdout.write(`构建         ${mb(built.bytes)} · ${built.tables.length} 张表 · ${((Date.now() - t0) / 1000).toFixed(1)}s\n`);
  process.stdout.write(`cmap         构建 ${built.cmap.map((s) => `${s.platformID}/${s.platEncID} f${s.format} ${s.entries}`).join(' · ')}\n`);
  process.stdout.write(`             基准 ${ref.cmap.map((s) => `${s.platformID}/${s.platEncID} f${s.format} ${s.entries}`).join(' · ')}\n`);
  process.stdout.write(`码页         构建 0x${(built.codePageRange1 ?? 0).toString(16)}/0x${(built.codePageRange2 ?? 0).toString(16)} · 基准 0x${(ref.codePageRange1 ?? 0).toString(16)}/0x${(ref.codePageRange2 ?? 0).toString(16)}\n`);
  process.stdout.write(`族名         构建 ${[...new Set(built.names.map((n) => n.text))].slice(0, 3).join(' / ')}\n`);
  process.stdout.write(`sha256       构建 ${sha256(buf).slice(0, 16)}… · 基准 ${sha256(refBuf).slice(0, 16)}…\n`);
  if (problems.length) {
    process.stdout.write(`\n✖ ${problems.length} 处不符：\n`);
    for (const x of problems) process.stdout.write(`   - ${x}\n`);
    return 1;
  }
  process.stdout.write('\n✔ 归一化 head.modified 后**逐字节相同**（20 张表全等 · 只有时间戳与它派生的两处校验不同）\n');
  return 0;
}

export function describeText() {
  const L = [];
  L.push('cnjp 分发字体 —— 引擎用 cp932 码位读文本，字体把「日文写法」的码位画成「简体」字形');
  L.push('');
  L.push('## 它动哪片数据');
  for (const d of DOMAIN.data) L.push(`* ${d}`);
  L.push('');
  L.push('## 三步变换');
  L.push('① cmap：反转字典（日写法→简体）⇒ 对除平台 1（Mac）外的每个子表原地写 `cmap[日写法] = cmap[简体]`；其余表原样搬运');
  L.push(`② name：id1/3/4/16 → \`${FAMILY}\`、id6 → \`${PS_NAME}\`（全语言一致）`);
  L.push('③ OS/2：`ulCodePageRange1/2` 按 fontTools `recalcCodePageRanges`（FontForge 直译）重算；932 = range1 bit 17');
  L.push('');
  L.push('## 判据');
  L.push('* `node tools/fonts.mjs --verify`：构建结果与 `corpus/assets/fonts/Amayui-CN_cnjp*.ttf` **归一化 `head.modified` 后逐字节相同**；');
  L.push('  ★ 实测只有 9 个字节会不同，全部源自时间戳（`head.modified` 低 3 字节 + head 的表校验和 + `checkSumAdjustment`）；');
  L.push('* `tools/test/fonts.test.mjs`：同一判据的守卫（基底/基准不在场时按本仓惯例 skip），另有码页位集合的纯函数断言。');
  L.push('');
  L.push('## 序列化口径（与 fontTools 对齐，否则漂）');
  L.push('* cmap f4：`splitRange` 分段（含拆分阈值 4/8）＋ `idRangeOffset = 2*(len(endCode)+len(glyphIndexArray)-i)`；');
  L.push('* cmap 表：记录按平台/编码排序，**编译后字节相同的子表共享偏移**；f14 等未解析子表原样带过；');
  L.push('* name：记录按 (platformID, platEncID, langID, nameID) 排序，**相同字符串只存一份**；');
  L.push('* sfnt：表顺序保持、每表 4 字节对齐、表校验和与 `head.checkSumAdjustment` 最后算。');
  L.push('');
  L.push('## 不做什么');
  L.push('* ❌ 不覆盖 `corpus/assets/fonts/Amayui-CN_cnjp*.ttf`（那是校验基准）：字典变了要换基准是一次**显式的再烘**；');
  L.push('* ❌ 不改字形（`glyf`/`loca`/`GPOS`/`GSUB`/`hmtx`/… 全部逐字节搬运）⇒ 基底换版本时产物必须重烘；');
  L.push('* ❌ 不引入字体库（fonteditor-core / opentype.js 等都会**重建**未改的表，逐字节判据必崩）。');
  return L.join('\n');
}

export function main(argv = process.argv.slice(2)) {
  let args;
  try {
    args = parseArgs(argv);
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n\n${HELP}`);
    return 2;
  }
  if (args.action === 'help') { process.stdout.write(HELP); return 0; }
  if (args.action === 'describe') { process.stdout.write(`${describeText()}\n`); return 0; }
  try {
    if (args.action === 'build') return cmdBuild(args);
    if (args.action === 'verify') return cmdVerify(args);
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n`);
    return 1;
  }
  process.stderr.write(HELP);
  return 2;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.stack ?? err.message}\n`);
    process.exitCode = 2;
  }
}
