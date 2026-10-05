#!/usr/bin/env node
/**
 * tools/patch.mjs —— **CLI**：翻译 patch 的查询 / 提取 / 复验（唯一写入口）
 *
 * 经派发器：`pnpm tools patch <describe|status|baseline|extract|verify> [args…]`
 * 也可独立跑：`node tools/patch.mjs --status`
 *
 * 模型在 `lib/patch.mjs`（口径、锚定规则、为什么这么定都写在那边）；本文件只做"参数 → 模型 → 输出 + 落盘计划"。
 *
 * ★ **不发散**：本文件是 patch 的**唯一写入口**，且只有 `extract` 会写；`verify` 只读。
 *   子命令一律"缺事实就报错"，不静默跳过（缺基线 / 缺字典 / 越界行序都会明说）。
 * ★ 不捕获子进程输出（受限沙箱里 `stdio:'pipe'` 会 EPERM）：这里也不 spawn 任何东西。
 */
import fs from 'node:fs';
import process from 'node:process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  DEFAULT_BASE_INDEX,
  DEFAULT_PATCH,
  DEFAULT_VIEW_DIR,
  DOMAIN,
  OPERATIONS,
  NO_OPS_ENTRY,
  REPO_ROOT,
  VIEW_SCOPES,
  alignRows,
  applyAnchorHunks,
  applyEntries,
  baseIndexProblems,
  buildBaseIndex,
  buildView,
  canonicalFormProblems,
  codecContext,
  describe,
  describeText,
  disassemble,
  entryFromView,
  extractEntry,
  allScriptNames,
  loadBaseIndex,
  loadPatch,
  loadViewManifest,
  mapperContext,
  maskLabels,
  mergeViewManifest,
  openSides,
  parseEditList,
  rebuildBin,
  rowKeyLabel,
  rowsOf,
  saveBaseIndex,
  savePatch,
  saveViewManifest,
  scopeNames,
  serializePatch,
  structuralProblems,
  substituteLiterals,
  verifyEntry,
} from './lib/patch.mjs';
import { sha256buf } from './lib/fsx.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HELP = `tools/patch.mjs —— 翻译 patch（唯一入库物 = 变更叠加层；base 是持久缓存，src 按需现算）

  node tools/patch.mjs --describe                        自描述：字段 / 不变量 / 操作 / 视图范围 / 基线索引（schema 的真源）
  node tools/patch.mjs --status                          规模与分布 + 字典指纹 + **基线索引还新不新**
  node tools/patch.mjs --baseline [<脚本名>…]             基线与产物分别从哪来（散装 / 哪个 ALF 的哪一段）
  node tools/patch.mjs --index [--write]                 ★ 建**基线索引**（脚本清单 + 逐支指纹 + codec/基线指纹）
  node tools/patch.mjs --extract [--write]               从旧仓产物提取 patch（**迁移期一次性**）
  node tools/patch.mjs --verify [<脚本名>…]               判据：基线 + patch ⇒ **逐字节**相同
  node tools/patch.mjs --view [--kind data|src|both]     写视图缓存（缺省只写 data = 基线侧，永不陈旧）
  node tools/patch.mjs --find <字串>… [--regex]          ★ **检索**：日文查基线、中文查 op 载荷，按锚配对（不物化投影）
  node tools/patch.mjs --find <串> --edits e.txt --to <新>  ★ **生成编辑清单**（锚寻址；只写清单文件）
  node tools/patch.mjs --set --edits <清单> [--write]     ★ **按锚直改 op ⇒ 一次写盘**（不渲染、不重跑 diff）
  node tools/patch.mjs --edit [--write]                  **人已经把视图文件改好了**时的批量反解（整篇 / 折行走这条）

公共选项
  --base   <目录>    基线根（缺省取清单 roots.gameInstall）
  --target <目录>    **产物根**：\`--extract\` 必需；\`--verify\` **缺省不比产物**（旧仓只是迁移期的一次性来源）
  --name   <脚本名>  只处理这一个（可重复；\`--extract\` 带它时**不许** --write）
  --skip   <脚本名>  迁移期跳过某几支（可重复）—— ★ **理由写在文档里，代码里不维护名单**
  --limit  <n>       只处理前 n 个（\`--extract\` 带它时不许 --write；\`--find\` 带它时=最多打几组命中）
  --patch  <文件>    换一份 patch（诊断用；缺省 data/translations/patch.json）
  --out    <目录/文件> \`--view\` / \`--find\` / \`--edit\` 的落点；\`--index\` 的索引文件落点
  --kind   <哪种>    \`--view\`：data（缺省）| src | both；\`--find\`：data | src | both（缺省 both）
  --scope  <范围>    \`--view\`：all（缺省，= 基线根里**全部**可反汇编脚本）/ patch / annotated
  --edits  <清单>    \`--find\`：把命中写成编辑清单（**只写这个文件**）；\`--set\`：要应用的清单
  --to     <新串>    \`--find --edits\`：把清单里的 \`+\` 行按"旧串→新串"**机械**填好（仍然要人逐条看）
  --regex            \`--find\`：把匹配串当正则（\`--to\` 也随之按正则替换）
  --count / --json   \`--find\`：只要计数 / 机器可读
  --allow-stale      \`--edit\`：明知视图不是当前 patch 的，仍照旧跑（会大声警告）
  --stdout           \`--view\`：打到标准输出而不是写文件（只对单个脚本有意义）
  --bin              \`--view\`：连重建出来的 BIN 一起写（打包/调试用）

★ **三件东西的寿命不一样**（这是整套设计的地基）：
  ① **基线索引** \`dist/index/base.json\` —— 只依赖不可变的东西（基线 BIN + codec 指纹）⇒ **永不陈旧**；
  ② **base 视图** \`dist/views/data/*.txt\` —— 由基线算出，同样不陈旧；检索日文、人读都靠它；
  ③ **src 投影** —— 依赖可变的 patch ⇒ **不常驻**：查询按锚现算（merge on read），
     只有"要用编辑器整篇改"时才 \`--kind src --name <脚本> --out dist/views\` 物化一支，并记进清单供 \`--edit\` 验来源。
★ **codec 指纹**（汇编器 / 反汇编器 / 指令表）是"BIN 是文本的可逆像"这句话的**证人**：它一变，
  旧索引里的文本就不再担保能重建出同样字节（和字典的 \`subsSha\` 同一个道理）。
★ \`--find\` **不物化投影**：日文侧扫基线、中文侧扫 op 载荷（patch 里存的就是中文），命中之后才按锚配对；
  于是它既不依赖 src 缓存，也不会因为缓存陈旧而少报。
★ \`--set\` 只接受**三种**形态（改一行字面量 / 插一行 / 删一行）—— 它们各自映到 patch 自己的词汇，**不需要重新对齐**；
  行数变化的块替换只能用"渲染 + 反解"（\`--view --out\` + \`--edit\`），\`set\` 会明确拒绝并给出命令。
`;

function parseArgs(argv) {
  const out = {
    action: null, write: false, names: [], skip: [], limit: null, base: null, target: undefined,
    patch: null, json: false, kind: null, out: null, stdout: false, bin: false, scope: 'all',
    regex: false, count: false, allowStale: false, patterns: [], edits: null, to: [],
  };
  const takesValue = new Set(['base', 'target', 'name', 'limit', 'patch', 'kind', 'out', 'skip', 'scope', 'edits', 'to']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--describe', '--status', '--baseline', '--index', '--extract', '--verify', '--view', '--find', '--set', '--edit', '--help', '-h'].includes(a)) {
      out.action = a.replace(/^--?/, '');
    } else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--stdout') out.stdout = true;
    else if (a === '--bin') out.bin = true;
    else if (a === '--regex') out.regex = true;
    else if (a === '--count') out.count = true;
    else if (a === '--allow-stale') out.allowStale = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} 需要值`);
      if (k === 'name') out.names.push(v);
      else if (k === 'skip') out.skip.push(v);
      else if (k === 'to') out.to.push(v);
      else if (k === 'limit') out.limit = Number(v);
      else out[k] = v;
    } else if (['baseline', 'verify', 'extract', 'status', 'describe', 'view', 'edit', 'find'].includes(out.action)) {
      // 位置参数 = 脚本名（`--baseline SC0000.BIN` / `--verify SC0000.BIN` 两种写法都认）；
      // `--find` 的位置参数是**要检索的字串**（可给多个）
      if (out.action === 'find') out.patterns.push(a);
      else out.names.push(a);
    } else throw new Error(`多余的位置参数：${a}`);
  }
  if (!out.action) out.action = 'help';
  if (out.limit !== null && (!Number.isInteger(out.limit) || out.limit <= 0)) throw new Error('--limit 必须是正整数');
  if (out.kind !== null && !['data', 'src', 'both'].includes(out.kind)) throw new Error('--kind 只能是 data / src / both');
  if (!VIEW_SCOPES.includes(out.scope)) throw new Error(`--scope 只能是 ${VIEW_SCOPES.join(' / ')}`);
  return out;
}

const pad = (s, n) => String(s).padEnd(n);
const mb = (n) => `${(n / 1048576).toFixed(2)} MB`;

/** 选名字：显式名字 > 该范围全量（可选截断） */
function pickNames(explicit, all, limit) {
  if (explicit.length) {
    const want = new Set(explicit.map((n) => n.toUpperCase()));
    const hit = all.filter((n) => want.has(n.toUpperCase()));
    const miss = explicit.filter((n) => !all.some((a) => a.toUpperCase() === n.toUpperCase()));
    for (const m of miss) process.stderr.write(`⚠ 这个范围里没有这个脚本：${m}\n`);
    return hit;
  }
  return limit === null ? all : all.slice(0, limit);
}

// ─────────────────────────────────────────────────────────── status

function cmdStatus(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) {
    process.stderr.write(`patch 还不存在：${p}\n（首次生成：pnpm tools patch extract --write）\n`);
    return 1;
  }
  const doc = loadPatch(p);
  const names = Object.keys(doc.scripts);
  const tally = { 'replace-line': 0, delete: 0, 'insert-after': 0 };
  let chars = 0;
  for (const n of names) {
    const e = doc.scripts[n];
    for (const o of e.ops) {
      tally[o.op] = (tally[o.op] ?? 0) + 1;
      if (typeof o.line === 'string') chars += o.line.length;
      if (typeof o.instr === 'string') chars += o.instr.length;
    }
  }
  const ops = Object.values(tally).reduce((a, b) => a + b, 0);
  process.stdout.write(`patch        ${p}\n`);
  process.stdout.write(`体积         ${mb(fs.statSync(p).size)}（${fs.statSync(p).size} B）\n`);
  process.stdout.write(`脚本         ${names.length}（**只含有变更的**；无变更的不进 patch）\n`);
  process.stdout.write(`操作         ${ops}\n`);
  for (const [k, v] of Object.entries(tally)) process.stdout.write(`             ${pad(k, 14)}${v}\n`);
  process.stdout.write(`新文本       ${chars} 字\n`);

  const { subsSha } = mapperContext();
  process.stdout.write(
    `字典指纹     ${doc.subsSha === subsSha ? '✔ 与记录一致' : `✖ 与当前字典**不一致**（patch ${doc.subsSha.slice(0, 12)}… / 当前 ${subsSha.slice(0, 12)}…）`}\n`,
  );

  const bad = structuralProblems(doc);
  process.stdout.write(`结构不变量   ${bad.length ? `✖ ${bad.length} 处` : '✔ 全绿'}\n`);
  for (const b of bad.slice(0, 20)) process.stdout.write(`             - ${b}\n`);

  const form = canonicalFormProblems(p);
  process.stdout.write(`规范形态     ${form.length ? `✖ ${form[0]}` : '✔ 盘上字节 == 重新序列化的结果（重跑必得同字节）'}\n`);

  // 三件东西的寿命不一样：基线索引 / base 视图（都只依赖不可变的东西 ⇒ 永不陈旧）+ src 草稿账本（可弃）
  try {
    const codec = codecContext();
    const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });
    let idx = null;
    try { idx = loadBaseIndex(); } catch { idx = null; }
    if (!idx) {
      process.stdout.write('基线索引     ✖ 没有（`dist/index/base.json`）⇒ 每次都要现算名单/现反汇编；跑 `pnpm tools patch index --write`\n');
    } else {
      const p2 = baseIndexProblems({ index: idx, root: sides.base, codecSha: codec.codecSha });
      process.stdout.write(
        `基线索引     ${p2.ok ? '✔ 与当前基线 + codec 一致' : `✖ ${p2.problems[0]}`}` +
          `（${idx.scripts.length} 支脚本 · codec ${idx.codecSha.slice(0, 8)}… · 基线 ${idx.baselineKey.slice(0, 8)}…）` +
          `${p2.ok ? '' : ' ⇒ 跑 `pnpm tools patch index --write`'}\n`,
      );
    }
    const dataDir = path.join(args.out ?? DEFAULT_VIEW_DIR, 'data');
    const dataCount = fs.existsSync(dataDir) ? fs.readdirSync(dataDir).filter((f) => /\.BIN\.txt$/i.test(f)).length : 0;
    process.stdout.write(`base 视图    ${dataCount} 个文件（${dataDir}）${dataCount ? '' : ' ⇒ 还没有：`pnpm tools patch view` 一次就够（它只依赖基线）'}\n`);
  } catch (err) {
    process.stdout.write(`基线索引     （跳过：${err.message}）\n`);
  }
  try {
    const manifest = loadViewManifest(path.join(args.out ?? DEFAULT_VIEW_DIR, 'manifest.json'));
    const covered = Object.keys(manifest.scripts ?? {});
    const { subsSha } = mapperContext();
    const codec = codecContext();
    let stale = 0;
    for (const n of covered) {
      const m = manifest.scripts[n];
      const e = doc.scripts[n];
      if (manifest.subsSha !== subsSha || manifest.codecSha !== codec.codecSha) stale += 1;
      else if (e ? m.baseSha !== e.baseSha || m.resultSha !== e.resultSha : m.baseSha !== m.resultSha) stale += 1;
    }
    process.stdout.write(
      `src 草稿账本 ${covered.length} 支物化过${covered.length ? ` · 过期 ${stale}` : ''}` +
        '（供 `--edit` 验来源；`find` 不需要它）\n',
    );
  } catch {
    process.stdout.write('src 草稿账本 无（`src` 投影不常驻：查询时按锚现算，只有要用编辑器整篇改才物化一支）\n');
  }
  for (const f of form.slice(1, 5)) process.stdout.write(`             - ${f}\n`);

  // 基线解析来源分布（要读字节 ⇒ 只在根在场时做）
  try {
    const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });
    const tallySrc = new Map();
    let missing = 0;
    let bytes = 0;
    for (const n of names) {
      const hit = sides.base.resolve(n);
      if (!hit) { missing += 1; continue; }
      bytes += hit.buf.length;
      const tag = hit.from.startsWith('loose:') ? '散装' : `ALF:${hit.from.split('→')[1]?.split('@')[0] ?? '?'}`;
      tallySrc.set(tag, (tallySrc.get(tag) ?? 0) + 1);
    }
    process.stdout.write(`基线解析     ${[...tallySrc].sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k} ${v}`).join(' · ')}` + `${missing ? ` · ⚠ 缺失 ${missing}` : ''}\n`);
    process.stdout.write(`基线字节     ${mb(bytes)}\n`);
  } catch (err) {
    process.stdout.write(`基线解析     （跳过：${err.message}）\n`);
  }
  return bad.length ? 1 : 0;
}

// ─────────────────────────────────────────────────────────── baseline

function cmdBaseline(args) {
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: args.target === 'none' ? null : (args.target ?? undefined) });
  const all = allScriptNames(sides.base).names;
  const names = pickNames(args.names, all, args.limit);
  process.stdout.write(`基线根       ${sides.base.dir}\n`);
  process.stdout.write(`产物根       ${sides.target ? sides.target.dir : '（不解析）'}\n`);
  process.stdout.write(`脚本         ${names.length}\n\n`);
  let missing = 0;
  for (const n of names) {
    const b = sides.base.resolve(n);
    const t = sides.target ? sides.target.resolve(n) : null;
    if (!b) missing += 1;
    const same = b && t && b.buf.equals(t.buf);
    process.stdout.write(
      `${pad(n, 16)} 基线 ${pad(b ? b.from : '✖ 缺失', 44)} 产物 ${pad(t ? t.from : '✖ 缺失', 44)}` +
        `${b && t ? (same ? '  = 两侧相同（无变更）' : '  ≠ 有变更') : ''}\n`,
    );
  }
  if (missing) process.stderr.write(`\n⚠ ${missing} 个脚本的基线解析不出来 —— 这是真错误，不要跳过\n`);
  return missing ? 1 : 0;
}

// ─────────────────────────────────────────────────────────── extract

function cmdExtract(args) {
  const partial = args.names.length > 0 || args.limit !== null;
  if (partial && args.write) {
    process.stderr.write('✖ 部分提取（--name / --limit）只能是 dry-run：\n  整份 patch 一次写完，否则会把没提取到的条目删掉。\n');
    return 2;
  }
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: args.target === 'none' ? null : (args.target ?? undefined) });
  if (!sides.target) { process.stderr.write('✖ 提取必须要有产物根（--target），否则无从知道"变成了什么"\n'); return 2; }

  const { mapper, subsSha } = mapperContext();
  // ★ 范围 = **基线根里全部能反汇编的 AGE 脚本**，不按名字过滤：
  //   "有没有译文"只能由"产物 ≠ 基线"判定，任何名字过滤都会静默漏掉真译文（实测漏过 247 支）。
  const { names: all, nonScript } = allScriptNames(sides.base);
  const skipSet = new Set(args.skip.map((n) => n.toUpperCase()));
  const names = pickNames(args.names, all, args.limit);
  process.stdout.write(`基线根       ${sides.base.dir}\n`);
  process.stdout.write(`产物根       ${sides.target.dir}\n`);
  process.stdout.write(`范围         基线根里全部可反汇编的 AGE 脚本 ${all.length} 支${nonScript.length ? `（另有 ${nonScript.length} 个 .BIN 不是脚本：${nonScript.join(', ')}）` : ''}\n`);
  process.stdout.write(`带提取       ${names.length} 支${partial ? '（dry-run；部分提取不许写盘）' : ''}\n\n`);

  const doc = { schemaVersion: 1, subsSha, scripts: {} };
  const failures = [];
  const missing = [];
  const skipped = [];
  let ops = 0;
  let empty = 0;
  let checked = 0;
  let forced = 0;
  let locals = 0;
  const t0 = Date.now();
  for (const [idx, name] of names.entries()) {
    const b = sides.base.resolve(name);
    const t = sides.target.resolve(name);
    if (!b) { missing.push(`${name}: 基线解析不出来（散装没有、ALF 里也没有）`); continue; }
    if (!t) { missing.push(`${name}: 产物解析不出来（散装没有、ALF 里也没有）`); continue; }
    // 迁移期可以用 `--skip <名字>` 跳过某些支（**理由写在文档里**，不在代码里维护名单）
    if (skipSet.has(name.toUpperCase())) { skipped.push(name); continue; }
    let entry;
    try {
      entry = extractEntry(b.buf, t.buf, mapper);
    } catch (err) {
      failures.push(`${name}: 提取失败 —— ${err.message}`);
      continue;
    }
    // ★ 提取即自证：基线 + 刚提取的 patch ⇒ 必须逐字节等于产物
    try {
      const rebuilt = rebuildBin(b.buf, entry, { lineToBin: mapper.lineToBin });
      if (rebuilt.length !== t.buf.length || !rebuilt.equals(t.buf)) {
        failures.push(`${name}: 提取后重建不逐字节相同（重建 ${rebuilt.length} B / 产物 ${t.buf.length} B）`);
        continue;
      }
    } catch (err) {
      failures.push(`${name}: 提取后重建失败 —— ${err.message}`);
      continue;
    }
    // ★ 逐字段构造**必须带上可选的 `header`**：`extractEntry` 的返回值里它是可选的，
    //   漏掉它会让"提取时自证通过、落盘后 verify 失败"（实测 `$1$IMINIT.BIN` 就踩了这一脚）。
    doc.scripts[name] = {
      baseSha: entry.baseSha,
      resultSha: entry.resultSha,
      ...(entry.header ? { header: entry.header } : {}),
      ops: entry.ops,
    };
    if (entry.ops.length === 0) {
      // ★ **没有变更的脚本不进 patch**（"没改"不需要记录；空条目只会让 diff 变大）
      delete doc.scripts[name];
      empty += 1;
    }
    ops += entry.ops.length;
    forced += entry.stats.forcedReplace ?? 0;
    locals += entry.stats.localLabels ?? 0;
    checked += 1;
    if ((idx + 1) % 25 === 0) process.stdout.write(`… ${idx + 1}/${names.length}（操作 ${ops}）\n`);
  }

  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  process.stdout.write(`\n提取完成     通过 ${checked} · 操作 ${ops} · 用时 ${secs}s\n`);
  process.stdout.write(`             其中**无变更、不进 patch** ${empty} 个（"没改"不记录）\n`);
  if (skipped.length) {
    process.stdout.write(`             **--skip 跳过** ${skipped.length} 个：${skipped.join(', ')}\n`);
  }
  process.stdout.write(`             "值相同但 label 目标变了 ⇒ 降级成 replace-line" ${forced} 处 · patch 局部 label ${locals} 个\n`);
  if (missing.length) {
    process.stdout.write(`✖ 解析不出来 ${missing.length} 条：\n`);
    for (const m of missing.slice(0, 20)) process.stdout.write(`   - ${m}\n`);
  }
  if (failures.length) {
    process.stdout.write(`✖ 提取/自证失败 ${failures.length} 条：\n`);
    for (const f of failures.slice(0, 20)) process.stdout.write(`   - ${f}\n`);
  }
  if (missing.length || failures.length) {
    process.stderr.write('\n✖ 有条目过不去 ⇒ **一个字都不写**（宁可没有 patch，也不要一份错的 patch）\n');
    return 1;
  }

  // 预演体积（与落盘同一份序列化）
  const text = serializePatch(doc);
  process.stdout.write(`将写入       ${args.patch ?? DEFAULT_PATCH}（${mb(Buffer.byteLength(text))}）\n`);
  const unmappable = mapper.unmappable();
  if (unmappable.length) {
    process.stdout.write(`字典缺口     ${unmappable.length} 个字编不出去且字典里没有（用了全角空格兜底）：`);
    process.stdout.write(`${unmappable.slice(0, 12).map(([c, n]) => `${c}×${n}`).join(' ')}${unmappable.length > 12 ? ' …' : ''}\n`);
  }
  if (mapper.stats.revertedInverse) {
    process.stdout.write(`反查放弃     ${mapper.stats.revertedInverse} 处（反查回来再重建会变 ⇒ 保留 BIN 原形式，宁可少认一个中文）\n`);
  }
  const bad = structuralProblems(doc);
  if (bad.length) {
    process.stdout.write(`✖ 结构不变量不过：\n  - ${bad.join('\n  - ')}\n`);
    return 1;
  }
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = savePatch(doc, args.patch ?? DEFAULT_PATCH);
  if (!res.ok) { process.stderr.write(`✖ ${res.reason}\n`); return 1; }
  process.stdout.write(`✔ 已写入     ${args.patch ?? DEFAULT_PATCH}（${res.bytes} B，回读复验通过）\n`);
  return 0;
}

// ─────────────────────────────────────────────────────────── verify

function cmdVerify(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) { process.stderr.write(`patch 不存在：${p}\n`); return 1; }
  const doc = loadPatch(p);
  const structural = structuralProblems(doc);
  if (structural.length) {
    process.stderr.write(`✖ 结构不变量不过（先修它）：\n  - ${structural.join('\n  - ')}\n`);
    return 1;
  }
  const { mapper, subsSha } = mapperContext();
  // ★ 默认**不比产物**：产物根（旧仓 `install/`）只是**迁移期的一次性来源**，稳态校验靠 `resultSha` 自证。
  //   要对产物复核就显式 `--target <目录>`。
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: args.target ?? null });
  const skipSet = new Set(args.skip.map((n) => n.toUpperCase()));
  const all = Object.keys(doc.scripts).sort();
  const names = pickNames(args.names, all, args.limit);

  process.stdout.write(`patch        ${p}\n`);
  process.stdout.write(`基线根       ${sides.base.dir}\n`);
  process.stdout.write(`产物根       ${sides.target ? sides.target.dir : '（不解析 ⇒ 只对 resultSha 自证）'}\n`);
  process.stdout.write(
    `字典指纹     ${doc.subsSha === subsSha ? '✔ 一致' : '✖ **与记录不一致**（换过字典 ⇒ 重建结果可能与 resultSha 不符）'}\n`,
  );
  process.stdout.write(`带核对       ${names.length} 个脚本\n\n`);

  // 覆盖：patch 的键必须是"基线根全部可反汇编脚本"的**子集**；缺席的就是"没有变更"（不进 patch）
  let coverage = null;
  if (sides.target) {
    const { names: range, nonScript } = allScriptNames(sides.base);
    const rangeSet = new Set(range.map((n) => n.toUpperCase()));
    const keys = new Set(Object.keys(doc.scripts).map((n) => n.toUpperCase()));
    const notInRange = [...keys].filter((n) => !rangeSet.has(n));
    const unchanged = range.filter((n) => !keys.has(n));
    coverage = { range: range.length, patch: keys.size, notInRange, unchanged, nonScript: nonScript.length };
    process.stdout.write(
      `覆盖         基线根可反汇编 ${coverage.range} 支 · patch ${coverage.patch} 支 · 其余 ${unchanged.length} 支视为**无变更**` +
        `（另有 ${coverage.nonScript} 个 .BIN 不是脚本）` +
        `${notInRange.length ? ` · ✖ patch 里有 ${notInRange.length} 支不在这个范围里` : ''}\n`,
    );
    for (const n of notInRange.slice(0, 10)) process.stdout.write(`             - 不在范围：${n}\n`);
  }

  const failed = [];
  const t0 = Date.now();
  for (const [idx, name] of names.entries()) {
    const r = verifyEntry(name, doc.scripts[name], { base: sides.base, target: sides.target, lineToBin: mapper.lineToBin });
    if (!r.ok) failed.push(r);
    else if (args.names.length || names.length <= 8) {
      process.stdout.write(
        `✔ ${pad(name, 16)} ${String(doc.scripts[name].ops.length).padStart(5)} 操作 · 基线 ${r.baseFrom} · ${r.stats.resultBytes} B\n`,
      );
    }
    if ((idx + 1) % 25 === 0) process.stdout.write(`… ${idx + 1}/${names.length}（失败 ${failed.length}）\n`);
  }

  // ★ "没有条目"是一种**主张**："这个脚本没有变更"。有产物根时就要**验**它，
  //   否则"少建条目"会变成一条静默丢改动的路。
  //   调用方用 `--skip` 显式声明"这一支的差异是旧仓的坏文件、我知道"时，单独列出来、不当失败。
  const untouchedFailed = [];
  const skippedSeen = [];
  if (coverage && sides.target) {
    for (const name of coverage.unchanged) {
      const b = sides.base.resolve(name);
      const t = sides.target.resolve(name);
      if (!b || !t) { untouchedFailed.push({ name, problems: ['基线或产物解析不出来'] }); continue; }
      const eq = b.buf.equals(t.buf);
      if (skipSet.has(name.toUpperCase())) { skippedSeen.push({ name, differs: !eq }); continue; }
      if (!eq) {
        untouchedFailed.push({
          name,
          problems: [`patch 里没有它，但产物与基线**不同**（基线 ${b.buf.length} B / 产物 ${t.buf.length} B）⇒ 漏改了`],
        });
      }
    }
    process.stdout.write(
      `\n无条目复核   ${coverage.unchanged.length - untouchedFailed.length - skippedSeen.length}/${coverage.unchanged.length - skippedSeen.length} 个"无变更"脚本确实与基线逐字节相同\n`,
    );
    for (const a of skippedSeen) {
      process.stdout.write(`--skip 复核   ${a.name}：产物与基线不同（${a.differs ? '是' : '⚠ 否'}）—— 按调用方的 --skip 跳过\n`);
    }
  }
  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  process.stdout.write(`\n核对完成     ${names.length - failed.length}/${names.length} 通过 · 失败 ${failed.length} · 用时 ${secs}s\n`);
  for (const f of failed.slice(0, 30)) {
    process.stdout.write(`✖ ${f.name}\n`);
    for (const q of f.problems) process.stdout.write(`   - ${q}\n`);
  }
  if (failed.length > 30) process.stdout.write(`… 还有 ${failed.length - 30} 条失败未列出\n`);
  for (const f of untouchedFailed) {
    process.stdout.write(`✖ ${f.name}（无条目）\n`);
    for (const q of f.problems) process.stdout.write(`   - ${q}\n`);
  }
  const covBad = coverage ? coverage.notInRange.length : 0;
  return failed.length || untouchedFailed.length || covBad ? 1 : 0;
}

// ─────────────────────────────────────────────────────────── view

/**
 * 写视图 + **逐脚本**更新清单（`--view` 与直改之后的"顺手刷新"共用这一条）。
 * @param {boolean} full 本次是否覆盖了**全范围**（覆盖全范围 ⇒ 清单里其余旧记账可以丢；否则增量合并）
 * @returns {{written:number, bytes:number, entries:object}}
 */
function writeViews({ outRoot, names, kinds, doc, ctx, sides, full = false, bin = false, quiet = false, scope = 'partial' }) {
  const entries = {};
  let written = 0;
  let bytes = 0;
  for (const kind of kinds) {
    const dir = path.join(outRoot, kind);
    fs.mkdirSync(dir, { recursive: true });
    for (const [idx, name] of names.entries()) {
      const hit = sides.base.resolve(name);
      if (!hit) throw new Error(`${name}：基线无法解析`);
      // 基线指纹在这一层顺手算出来（清单要用；`NO_OPS_ENTRY` 也是同一条哈希）
      const bsha = sha256buf(hit.buf);
      const entry = doc.scripts[name] ?? { baseSha: bsha, resultSha: bsha, ops: [] };
      if (!entries[name]) entries[name] = { baseSha: entry.baseSha, resultSha: entry.resultSha };
      const view = buildView(kind, hit.buf, entry, { lineToBin: ctx.mapper.lineToBin });
      fs.writeFileSync(path.join(dir, `${name}.txt`), view.text, 'utf8');
      bytes += Buffer.byteLength(view.text);
      written += 1;
      if (bin) fs.writeFileSync(path.join(outRoot, `${kind}-bin`, name), view.bin);
      if (!quiet && (idx + 1) % 100 === 0) process.stdout.write(`… ${kind} ${idx + 1}/${names.length}\n`);
    }
  }
  // ★ 逐脚本记账（可增量）：只重建一支就只让它那一格变新
  const manifestPath = path.join(outRoot, 'manifest.json');
  let prev = null;
  try { prev = loadViewManifest(manifestPath); } catch { prev = null; }
  const m = mergeViewManifest(prev, { scope, kinds, entries, subsSha: ctx.subsSha, full });
  const res = saveViewManifest(m, manifestPath);
  return { written, bytes, entries, manifestPath, manifestBytes: res.bytes };
}

// ─────────────────────────────────────────────────────────── index（基线索引：持久侧只依赖不可变的东西）

function cmdIndex(args) {
  const ctx = mapperContext();
  const codec = codecContext();
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });
  const t0 = Date.now();
  let index;
  try {
    index = buildBaseIndex({ root: sides.base, subsSha: ctx.subsSha, codecSha: codec.codecSha });
  } catch (err) {
    process.stderr.write(`✖ 建索引失败：${err.message}\n`);
    return 1;
  }
  const p = args.out ?? DEFAULT_BASE_INDEX;
  const text = `${JSON.stringify(index)}\n`;
  process.stdout.write(`基线根       ${sides.base.dir}\n`);
  process.stdout.write(`脚本         ${index.scripts.length} 支可反汇编${index.nonScript.length ? ` · ${index.nonScript.length} 个 .BIN 不是脚本` : ''}\n`);
  process.stdout.write(`codec 指纹   ${index.codecSha.slice(0, 12)}…（汇编器 / 反汇编器 / 指令表：${codec.files.length} 个文件）\n`);
  process.stdout.write(`基线指纹     ${index.baselineKey.slice(0, 12)}…（散装件与归档的 size+mtime）\n`);
  process.stdout.write(`字典指纹     ${index.subsSha.slice(0, 12)}…\n`);
  process.stdout.write(`用时         ${((Date.now() - t0) / 1000).toFixed(1)}s\n`);
  let prev = null;
  try { prev = loadBaseIndex(p); } catch { prev = null; }
  if (prev) {
    const same = prev.codecSha === index.codecSha && prev.baselineKey === index.baselineKey;
    process.stdout.write(`与旧索引     ${same ? '一致（不用重写）' : `**变了**（codec ${prev.codecSha.slice(0, 8)}→${index.codecSha.slice(0, 8)} · 基线 ${prev.baselineKey.slice(0, 8)}→${index.baselineKey.slice(0, 8)}）`}\n`);
  }
  process.stdout.write(`将写入       ${p}（${mb(Buffer.byteLength(text))}）\n`);
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = saveBaseIndex(index, p);
  process.stdout.write(`✔ 已写入     ${p}（${res.bytes} B）\n`);
  return 0;
}

// ─────────────────────────────────────────────────────────── view

/**
 * 写视图缓存文件（不改账本）。
 * @returns {{written:number, bytes:number, entries:object}} `entries` = 逐支 `{baseSha, resultSha}`
 */
function writeViewFiles({ outRoot, names, kinds, doc, ctx, sides, bin = false, quiet = false }) {
  const entries = {};
  let written = 0;
  let bytes = 0;
  for (const kind of kinds) {
    const dir = path.join(outRoot, kind);
    fs.mkdirSync(dir, { recursive: true });
    for (const [idx, name] of names.entries()) {
      const hit = sides.base.resolve(name);
      if (!hit) throw new Error(`${name}：基线无法解析`);
      // 基线指纹在这一层顺手算出来（`NO_OPS_ENTRY` 也是同一条哈希）
      const bsha = sha256buf(hit.buf);
      const entry = doc.scripts[name] ?? { baseSha: bsha, resultSha: bsha, ops: [] };
      if (!entries[name]) entries[name] = { baseSha: entry.baseSha, resultSha: entry.resultSha };
      const view = buildView(kind, hit.buf, entry, { lineToBin: ctx.mapper.lineToBin });
      fs.writeFileSync(path.join(dir, `${name}.txt`), view.text, 'utf8');
      bytes += Buffer.byteLength(view.text);
      written += 1;
      if (bin) fs.writeFileSync(path.join(outRoot, `${kind}-bin`, name), view.bin);
      if (!quiet && (idx + 1) % 100 === 0) process.stdout.write(`… ${kind} ${idx + 1}/${names.length}\n`);
    }
  }
  return { written, bytes, entries };
}

function cmdView(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) { process.stderr.write(`patch 不存在：${p}\n`); return 1; }
  const doc = loadPatch(p);
  const structural = structuralProblems(doc);
  if (structural.length) {
    process.stderr.write(`✖ 结构不变量不过（先修它）：\n  - ${structural.join('\n  - ')}\n`);
    return 1;
  }
  const ctx = mapperContext();
  const codec = codecContext();
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });

  // ★ 名单优先取**基线索引**（它只依赖不可变的东西 ⇒ 永不陈旧），没有才现算。
  let allNames = null;
  let indexNote = '';
  try {
    const idx = loadBaseIndex();
    const fresh = baseIndexProblems({ index: idx, root: sides.base, codecSha: codec.codecSha });
    if (fresh.ok) { allNames = idx.scripts; indexNote = '（名单取自基线索引）'; }
    else indexNote = `（基线索引不可用：${fresh.problems[0]} ⇒ 现算名单）`;
  } catch {
    indexNote = '（没有基线索引 ⇒ 现算名单；跑 pnpm tools patch index --write 可省这一步）';
  }
  if (!allNames) allNames = allScriptNames(sides.base).names;

  let inScope;
  try {
    inScope = scopeNames(args.scope, { all: allNames, doc });
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n`);
    return 2;
  }
  // ★ 显式 `--name` 时**不受 scope 限制**（scope 是"批量生成时的取哪些"，不是"允许哪些"）。
  const names = args.names.length
    ? pickNames(args.names, allNames, null)
    : args.limit === null
      ? inScope
      : inScope.slice(0, args.limit);
  // ★ **缺省只写 `data`**（基线侧，永不陈旧）；`src` 只在"要用编辑器整篇改"时按支物化。
  const kinds = args.kind ? (args.kind === 'both' ? ['data', 'src'] : [args.kind]) : ['data'];
  const outRoot = args.out ?? DEFAULT_VIEW_DIR;
  if (names.length === 0) { process.stderr.write('✖ 没有可生成的脚本\n'); return 2; }

  if (args.stdout && names.length !== 1) {
    process.stderr.write('✖ --stdout 只对**单个**脚本有意义（用 --name 指定）\n');
    return 2;
  }

  // ★ `--stdout` 时**只有视图文本**进 stdout（其余报告走 stderr）：给人 `| less` / 重定向用。
  const say = args.stdout ? (s) => process.stderr.write(s) : (s) => process.stdout.write(s);
  say(`范围         ${args.scope}（基线根里 ${allNames.length} 支可反汇编脚本${args.scope === 'all' ? '' : ` → 本范围 ${inScope.length} 支`}）${indexNote}\n`);
  say(`带生成       ${names.length} 支 × ${kinds.join(' + ')}\n`);

  const t0 = Date.now();
  if (args.stdout) {
    const hit = sides.base.resolve(names[0]);
    if (!hit) { process.stderr.write(`✖ ${names[0]}：基线无法解析\n`); return 1; }
    const bsha = sha256buf(hit.buf);
    const entry = doc.scripts[names[0]] ?? { baseSha: bsha, resultSha: bsha, ops: [] };
    for (const kind of kinds) process.stdout.write(buildView(kind, hit.buf, entry, { lineToBin: ctx.mapper.lineToBin }).text);
    return 0;
  }

  let res;
  const full = names.length === inScope.length && !args.names.length && args.limit === null;
  try {
    res = writeViewFiles({ outRoot, names, kinds, doc, ctx, sides, bin: args.bin });
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n`);
    return 1;
  }

  // ★ 物化了 `src` ⇒ 记进**草稿账本**（`edit` 靠它验来源）；`data` 不进账本（它的保鲜靠基线索引）。
  let draftNote = '';
  if (kinds.includes('src')) {
    const manifestPath = path.join(outRoot, 'manifest.json');
    let prev = null;
    try { prev = loadViewManifest(manifestPath); } catch { prev = null; }
    const m = mergeViewManifest(prev, { scope: full ? args.scope : 'partial', kinds: ['src'], entries: res.entries, subsSha: ctx.subsSha, codecSha: codec.codecSha, full: false });
    const saved = saveViewManifest(m, manifestPath);
    draftNote = `草稿账本     ${manifestPath}（${saved.bytes} B：${Object.keys(m.scripts).length} 支被物化过 src ⇒ 供 --edit 验来源）\n`;
    draftNote += '草稿区       `src` 只是**草稿**（服务 `patch edit`）：随时可删 —— 删了 `edit` 会要求重新物化，别的什么都不受影响\n';
  }
  // ★ 全范围写了 `data` ⇒ 顺手把**基线索引**也落盘（同一趟扫描的结果，不让用户再跑一次）。
  let indexLine = '';
  if (kinds.includes('data') && full && args.scope === 'all') {
    try {
      const index = buildBaseIndex({ root: sides.base, subsSha: ctx.subsSha, codecSha: codec.codecSha, entries: res.entries });
      const saved = saveBaseIndex(index);
      indexLine = `基线索引     ${DEFAULT_BASE_INDEX}（${saved.bytes} B：脚本清单 + codec ${codec.codecSha.slice(0, 8)}… + 基线 ${index.baselineKey.slice(0, 8)}…）\n`;
    } catch (err) {
      indexLine = `基线索引     写失败（${err.message}）—— 之后跑 pnpm tools patch index --write\n`;
    }
  }

  // ★ 视图目录里**不在本次范围**的文件：只在**全范围**运行时报（那才是"旧范围的遗留"的判据；
  //   部分运行（`--name`）看到的是"其余脚本的缓存"，不是遗留）。
  const wanted = new Set(names.map((n) => `${n}.txt`.toUpperCase()));
  const extra = [];
  if (full) {
    for (const kind of kinds) {
      const dir = path.join(outRoot, kind);
      if (!fs.existsSync(dir)) continue;
      for (const f of fs.readdirSync(dir)) {
        if (/\.BIN\.txt$/i.test(f) && !wanted.has(f.toUpperCase())) extra.push(`${kind}/${f}`);
      }
    }
  }

  process.stdout.write(`视图落点     ${outRoot}\n`);
  process.stdout.write(`写出         ${res.written} 个文件（${kinds.join(' + ')} · ${names.length} 个脚本 · ${mb(res.bytes)} · ${((Date.now() - t0) / 1000).toFixed(1)}s）\n`);
  if (extra.length) {
    process.stdout.write(`范围外遗留   ${extra.length} 个视图文件不在本次范围里（别的范围的旧产物）：${extra.slice(0, 5).join(' · ')}${extra.length > 5 ? ' …' : ''}\n`);
  }
  process.stdout.write(indexLine);
  process.stdout.write(draftNote);
  process.stdout.write('（缓存：可无限重算，不入库。`data` = 基线侧：永不陈旧；`src` = 依赖 patch：按需物化）\n');
  return 0;
}

// ─────────────────────────────────────────────────────────── set（按锚直改 op ⇒ 一次写盘进 patch）

/**
 * 编辑后的**行空间比对**：把清单的改动**独立**地算成一份"期望的（遮蔽后）行列表"，
 * 再和"新条目重建出来的 src 视图"逐行比 —— 相等才允许写盘。
 *
 * ★ 为什么能这么比：literal 改了不影响行数、`insert`/`delete` 只差我们自己加/删的那一行；
 *   label 引用两边都遮蔽成 `label_?`（地址会漂，但不是内容）。任一处不符 ⇒ 说明工具或清单有问题 ⇒ **不写**。
 */
function expectedMaskedRows({ baseMasked, oldOps, hunks, where }) {
  const { rows } = alignRows(baseMasked, oldOps);
  const seen = new Map();
  let list = rows.map((r) => {
    const k = r.op === 'insert' ? (seen.get(r.base) ?? 0) + 1 : 0;
    if (r.op === 'insert') seen.set(r.base, k);
    return { anchor: r.base, k, text: maskLabels(r.payload ?? baseMasked[r.base]) };
  });
  const indexOf = (anchor, k) => list.findIndex((x) => x.anchor === anchor && x.k === k);
  for (const h of hunks) {
    const i = indexOf(h.at, h.k);
    if (i < 0) throw new Error(`${where}:${h.line}: 定位不到 ${rowKeyLabel(h.at, h.k)} 那一行`);
    if (h.minus.length === 1 && h.plus.length === 1) {
      list[i] = { anchor: h.at, k: h.k, text: maskLabels(h.plus[0]) };
    } else if (h.minus.length === 1) {
      list.splice(i, 1);
    } else {
      list.splice(i + 1, 0, { anchor: h.at, k: h.k + 1, text: maskLabels(h.plus[0]) });
      // 之后同锚的插入行序号整体 +1（与 op 层的顺序语义一致）
      for (let j = i + 2; j < list.length && list[j].anchor === h.at; j += 1) list[j] = { ...list[j], k: list[j].k + 1 };
    }
  }
  return list.map((x) => x.text);
}

function cmdSet(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) { process.stderr.write(`patch 不存在：${p}\n`); return 1; }
  if (!args.edits) {
    process.stderr.write(
      '✖ --set 要给编辑清单：--edits <文件>\n' +
        '  形态（一条记录 = 一个 hunk，头行用**锚**）：\n' +
        '    <脚本名> <锚>[+<k>]        # 锚 = 基线行序；+k = 挂在它后面的第 k 条插入行\n' +
        '    - <当前内容（必须与当前 src 视图那一行逐字相同）>\n' +
        '    + <新内容>\n' +
        '  生成模板：pnpm tools patch find <字串> --edits <文件> [--to <新串>]\n' +
        '  ★ 只支持：改一行字面量 / 插一行 / 删一行；行数变化的块替换请用 `patch view --out` + `patch edit`\n',
    );
    return 2;
  }
  let hunks;
  try {
    hunks = parseEditList(fs.readFileSync(args.edits, 'utf8'), args.edits);
  } catch (err) {
    process.stderr.write(`✖ ${err.message}\n`);
    return 2;
  }
  const doc = loadPatch(p);
  const structural = structuralProblems(doc);
  if (structural.length) {
    process.stderr.write(`✖ 结构不变量不过（先修它）：\n  - ${structural.join('\n  - ')}\n`);
    return 1;
  }
  const ctx = mapperContext();
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });

  const byScript = new Map();
  const only = args.names.length ? new Set(args.names.map((n) => n.toUpperCase())) : null;
  for (const h of hunks) {
    if (only && !only.has(h.name.toUpperCase())) continue; // `--name` 收窄（清单里可以同时写好几支）
    if (!byScript.has(h.name)) byScript.set(h.name, []);
    byScript.get(h.name).push(h);
  }
  if (!byScript.size) {
    process.stderr.write('✖ --name 收窄之后一条编辑都不剩（名字对不对？）\n');
    return 2;
  }

  const updates = {};
  const plans = [];
  process.stdout.write(`编辑清单     ${args.edits}（${hunks.length} 条记录 / ${byScript.size} 支脚本，锚寻址）\n`);
  for (const [name, list] of byScript) {
    const hit = sides.base.resolve(name);
    if (!hit) { process.stderr.write(`✖ ${name}：基线里没有这支脚本\n`); return 1; }
    const bsha = sha256buf(hit.buf);
    const baseRows = rowsOf(disassemble(hit.buf));
    const entry = doc.scripts[name] ?? { baseSha: bsha, resultSha: bsha, ops: [] };
    let newEntry;
    let report;
    try {
      const applied = applyAnchorHunks({ baseMasked: baseRows.masked, entry, hunks: list, where: args.edits });
      newEntry = applied.entry;
      report = applied.report;
    } catch (err) {
      process.stderr.write(`✖ ${err.message}\n`);
      return 1;
    }
    // ★ 判据：新条目重建出来的 src 视图，其（遮蔽后）行空间必须**逐行等于**我们独立算出的期望
    let newView;
    let expected;
    try {
      newView = buildView('src', hit.buf, newEntry, { lineToBin: ctx.mapper.lineToBin });
      expected = expectedMaskedRows({ baseMasked: baseRows.masked, oldOps: entry.ops ?? [], hunks: list, where: args.edits });
    } catch (err) {
      process.stderr.write(`✖ ${name}：${err.message}\n`);
      return 1;
    }
    const got = rowsOf(newView.text).masked;
    if (got.length !== expected.length) {
      process.stderr.write(
        `✖ ${name}：重建后的行数与期望不符（重建 ${got.length} / 期望 ${expected.length}）⇒ **一个字都不写**\n` +
          '  这通常说明清单里的改动不只是"文案"（例如动了决定行数的指令）\n',
      );
      return 1;
    }
    const bad = got.findIndex((l, i) => l !== expected[i]);
    if (bad >= 0) {
      process.stderr.write(
        `✖ ${name}：重建结果与期望不一致（第 ${bad} 行序）⇒ **一个字都不写**\n  重建：${got[bad]}\n  期望：${expected[bad]}\n`,
      );
      return 1;
    }
    process.stdout.write(`\n${name}${doc.scripts[name] ? '' : '（**新建条目**）'}　操作 ${(entry.ops ?? []).length} → ${newEntry.ops.length}\n`);
    for (const r of report) {
      const verb = r.verb === 'replace' ? '改字面量' : r.verb === 'insert' ? '插一行' : '删一行';
      process.stdout.write(`  ${String(r.key).padEnd(10)} ${verb}${r.created && r.verb === 'replace' ? '（新建 replace-line）' : ''}\n`);
      if (r.before) process.stdout.write(`        - ${r.before}\n`);
      if (r.after) process.stdout.write(`        + ${r.after}\n`);
    }
    updates[name] = {
      baseSha: newEntry.baseSha,
      // ★ `resultSha` = **重建出来的产物 BIN** 的 sha（编辑就是"换了产物"）——
      //   它必须在这里现算：`applyAnchorHunks` 只动 ops，不会替我们更新指纹。
      resultSha: sha256buf(newView.bin),
      ...(newEntry.header ? { header: newEntry.header } : {}),
      ops: newEntry.ops,
    };
    plans.push({ name, ops: `${(entry.ops ?? []).length} → ${newEntry.ops.length}` });
  }
  if (!plans.length) {
    process.stdout.write('\n没有实际改动 ⇒ 什么都不写。\n');
    return 0;
  }
  const unmappable = ctx.mapper.unmappable();
  if (unmappable.length) {
    process.stdout.write(
      `\n⚠ 有 ${unmappable.length} 个字编不进 cp932 且字典里没有（会落成全角空格）：${unmappable.slice(0, 12).map(([c, n]) => `${c}×${n}`).join(' ')}\n`,
    );
  }
  process.stdout.write(`\n将写入       ${p}（${plans.length} 支脚本）\n`);
  for (const plan of plans) process.stdout.write(`  ${plan.name}　操作 ${plan.ops}\n`);
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。（清单里的锚 = 基线行序，= patch 自己的键空间）\n');
    return 0;
  }
  const res = applyEntries(doc, updates, p);
  if (!res.ok) { process.stderr.write(`✖ ${res.reason}\n`); return 1; }
  process.stdout.write(`✔ 已写入     ${p}（${res.bytes} B，回读复验通过）\n`);
  // ★ 写盘**不需要刷新任何缓存**：`data` 视图只依赖基线（没变），`src` 投影不落盘（merge on read）。
  process.stdout.write('缓存         data 视图与基线索引都不受影响（它们只依赖不可变的基线）\n');
  return 0;
}

// ─────────────────────────────────────────────────────────── find（merge on read：不物化投影）

/** 一份文本里逐行的**基线行序**（`null` = 那一行不进行序空间：空行 / label 定义行）—— 顺手也给出文件行号 */
function scanLines(text) {
  const out = [];
  let row = 0;
  for (const [i, line] of text.split('\n').entries()) {
    if (i < 4) { out.push({ line: i + 1, row: null, text: line }); continue; }
    const t = line.replace(/\s+\/\/.*$/, '').trim();
    if (t === '' || /^label_[0-9a-fA-F]+$/.test(t)) { out.push({ line: i + 1, row: null, text: line }); continue; }
    out.push({ line: i + 1, row, text: line });
    row += 1;
  }
  return out;
}

/**
 * 一支脚本的**命中**（merge on read）：
 * * `data` 侧 = 基线文本里命中且**行序存在**的行；
 * * `src` 侧 = **op 载荷**命中，或"基线行未被 replace/delete"的命中；
 * 两侧用**行键**（锚 + `+k`）合并成一组 —— 不需要物化投影，也不需要行对齐。
 *
 * @returns {{groups:Array<object>, counts:{data:number,src:number}}}
 */
function hitsOfScript({ name, baseText, entry, kinds, matchers }) {
  const ops = entry?.ops ?? [];
  const replaced = new Set();
  const deleted = new Set();
  for (const o of ops) {
    if (o.op === 'replace-line') replaced.add(o.i);
    else if (o.op === 'delete') deleted.add(o.i);
  }
  const hit = (t) => matchers.some((m) => (m instanceof RegExp ? m.test(t) : t.includes(m)));
  const groups = new Map();
  const key = (anchor, k) => `${anchor}\u0000${k}`;
  const mk = (anchor, k) => {
    const kk = key(anchor, k);
    if (!groups.has(kk)) groups.set(kk, { anchor, k, key: rowKeyLabel(anchor, k), data: null, src: null });
    return groups.get(kk);
  };
  let dataHits = 0;
  let srcHits = 0;

  // ── 基线侧：一次扫描同时喂两个侧（`data` 全收；`src` 只收"没被 replace/delete"的行）
  if (baseText !== null) {
    for (const l of scanLines(baseText)) {
      if (l.row === null || !hit(l.text)) continue;
      const isData = kinds.includes('data');
      const isSrc = kinds.includes('src') && !replaced.has(l.row) && !deleted.has(l.row);
      if (!isData && !isSrc) continue;
      const g = mk(l.row, 0);
      if (isData) { g.data = { line: l.line, row: l.row, text: l.text }; dataHits += 1; }
      if (isSrc && !g.src) { g.src = { line: null, row: l.row, text: l.text }; srcHits += 1; }
    }
  }

  // ── op 载荷（= 中文）：查中文根本不需要投影
  if (kinds.includes('src')) {
    const insertSeen = new Map(); // anchor → 已数过的插入条数
    for (const o of ops) {
      const payload = o.op === 'replace-line' ? o.line : o.op === 'insert-after' ? o.instr : null;
      if (!payload || !hit(payload)) continue;
      const k = o.op === 'insert-after' ? (insertSeen.get(o.i) ?? 0) + 1 : 0;
      if (o.op === 'insert-after') insertSeen.set(o.i, k);
      const g = mk(o.i, k);
      if (!g.src) { g.src = { line: null, row: o.i, text: payload }; srcHits += 1; }
    }
  }

  const arr = [...groups.values()].sort((a, b) => a.anchor - b.anchor || a.k - b.k);
  // 单侧命中也要把**对侧**填出来（人要看"日文 ↔ 当前中文"）
  for (const g of arr) {
    if (g.data || baseText === null || g.anchor < 0) continue;
    const l = scanLines(baseText).find((x) => x.row === g.anchor);
    if (l) g.data = { line: l.line, row: g.anchor, text: l.text };
  }
  return { groups: arr, counts: { data: dataHits, src: srcHits } };
}

/** 把命中写成**编辑清单**（锚寻址；只写这份文件，不碰 patch）—— 每条都带"期望的当前内容" */
function writeEditList({ file, perScript, args }) {
  const out = [];
  out.push('# 编辑清单（由 `pnpm tools patch find --edits` 生成）——逐条审一遍再应用：');
  out.push(`#   pnpm tools patch set --edits ${file}          # dry-run：逐条打「- 现在 / + 改后」`);
  out.push(`#   pnpm tools patch set --edits ${file} --write  # 一次写盘进 patch`);
  out.push('# 一条记录 = 一个 hunk：头行 `<脚本> <锚>[+<k>]`（锚 = 基线行序，不是文件行号）');
  out.push('#   `- 当前内容` 必须与当前 src 视图那一行逐字相同（对不上会报错）。');
  out.push('#   ★ 只改字面量 / 插一行 / 删一行 —— 行数变化的块替换要用 `patch view --out` + `patch edit`。');
  out.push('#   ★ 下面 `+` 行默认与 `-` 行相同 = **什么都没改的模板**：把要改的地方写出来（或生成时加 --to）。');
  let hunks = 0;
  for (const s of perScript) {
    for (const g of s.groups) {
      if (!g.src) continue;
      out.push('');
      if (g.data) out.push(`# 日文原文：${g.data.text}`);
      out.push(`${s.name} ${g.anchor}${g.k ? `+${g.k}` : ''}`);
      out.push(`- ${g.src.text}`);
      out.push(`+ ${replaceInLine(g.src.text, args)}`);
      hunks += 1;
    }
  }
  fs.writeFileSync(file, `${out.join('\n')}\n`, 'utf8');
  return { hunks, bytes: Buffer.byteLength(out.join('\n')) + 1 };
}

/** `--to` 生成用：只把**引号里**的匹配换掉（与检索同一套口径：`comment` 跳过、行尾注释不碰） */
function replaceInLine(line, args) {
  if (!args.to.length) return line;
  const t = line.replace(/\s+\/\/.*$/, '').trim();
  if (t === '' || /^comment\b/.test(t)) return line;
  return substituteLiterals(`${line}\n`, args.patterns.map((from, i) => ({ from, to: args.to[i] ?? args.to[0] })), { regex: args.regex })
    .text.replace(/\n$/, '');
}

function cmdFind(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) { process.stderr.write(`patch 不存在：${p}\n`); return 1; }
  if (!args.patterns.length) {
    process.stderr.write('✖ 要检索什么？给一个或多个字串（要正则就加 --regex）\n');
    return 2;
  }
  if (args.edits && args.to.length && args.patterns.length !== args.to.length) {
    process.stderr.write('✖ `--edits --to` 时，`--to` 的条数必须与检索串一一对应（给一个串就配一个 --to）\n');
    return 2;
  }
  const doc = loadPatch(p);
  const structural = structuralProblems(doc);
  if (structural.length) {
    process.stderr.write(`✖ 结构不变量不过（先修它）：\n  - ${structural.join('\n  - ')}\n`);
    return 1;
  }
  const ctx = mapperContext();
  const codec = codecContext();
  const dir = args.out ?? DEFAULT_VIEW_DIR;
  const kinds = args.kind ? (args.kind === 'both' ? ['data', 'src'] : [args.kind]) : ['data', 'src'];
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });

  // ★ 名单与"基线文本能不能直接用"都问**基线索引**（它只依赖不可变的东西 ⇒ 永不陈旧）。
  let allNames = null;
  let indexOk = false;
  try {
    const idx = loadBaseIndex();
    indexOk = baseIndexProblems({ index: idx, root: sides.base, codecSha: codec.codecSha }).ok;
    if (indexOk) allNames = idx.scripts;
  } catch { indexOk = false; }
  if (!allNames) allNames = allScriptNames(sides.base).names;
  const names = args.names.length ? pickNames(args.names, allNames, null) : allNames;
  if (!names.length) return 2;

  // ★ `data` 文本从缓存读（索引担保它对应当前基线 + codec），否则**现算那一支**（只反汇编，不重放 patch）。
  const dataDir = path.join(dir, 'data');
  const useText = indexOk;
  process.stdout.write(
    `来源         ${useText ? `基线文本缓存（基线索引担保）${dataDir}` : '**现算基线文本**（没有基线索引 / 索引陈旧 ⇒ 慢一点；跑 pnpm tools patch index --write + patch view 可省）'}\n`,
  );
  process.stdout.write(`检索         ${args.regex ? '正则' : '字串'} ${args.patterns.map((x) => JSON.stringify(x)).join(' · ')}（${kinds.join(' + ')}）\n`);
  process.stdout.write('口径         日文查基线、中文查 op 载荷，按**锚**配对 —— **不物化 src 投影**\n');

  const matchers = args.patterns.map((s) => (args.regex ? new RegExp(s, 'u') : s));
  const perScript = [];
  let tally = { data: 0, src: 0, groups: 0 };
  const failures = [];
  const t0 = Date.now();
  let fellBack = 0;
  for (const [idx, name] of names.entries()) {
    let baseText = null;
    try {
      const f = path.join(dataDir, `${name}.txt`);
      if (useText && fs.existsSync(f)) baseText = fs.readFileSync(f, 'utf8');
      else {
        const hit = sides.base.resolve(name);
        if (!hit) throw new Error('基线无法解析');
        fellBack += 1;
        baseText = buildView('data', hit.buf, { baseSha: '', resultSha: '', ops: [] }).text;
      }
      const { groups, counts } = hitsOfScript({ name, baseText, entry: doc.scripts[name], kinds, matchers });
      if (!groups.length) continue;
      tally.data += counts.data;
      tally.src += counts.src;
      tally.groups += groups.length;
      perScript.push({ name, groups, counts });
    } catch (err) {
      failures.push(`${name}: ${err.message}`);
    }
    if ((idx + 1) % 300 === 0) process.stdout.write(`… 扫过 ${idx + 1}/${names.length}（命中 ${tally.groups} 组）\n`);
  }
  if (fellBack) process.stdout.write(`现算         ${fellBack} 支没有可用的基线文本缓存 ⇒ 当场反汇编\n`);

  // ── 生成编辑清单（只写这份文件，不碰 patch）
  if (args.edits) {
    if (!perScript.length) {
      process.stderr.write('\n✖ 一处都没命中 ⇒ 不生成清单（是不是检索串写错了？）\n');
      return 1;
    }
    const res = writeEditList({ file: args.edits, perScript, args });
    process.stdout.write(`\n生成清单     ${args.edits}（${res.hunks} 条记录 / ${perScript.length} 支脚本 · ${res.bytes} B，锚寻址）\n`);
    if (!args.to.length) process.stdout.write('★ `+` 行与 `-` 行相同 = **模板**：逐条把要改的地方写出来，再应用。\n');
    else process.stdout.write(`★ \`+\` 行已按 ${args.to.map((x) => JSON.stringify(x)).join(' · ')} 机械填好 —— **仍然要逐条看一遍**（自动替换会误伤同形词）。\n`);
    process.stdout.write(`应用         pnpm tools patch set --edits ${args.edits}          # dry-run\n`);
    process.stdout.write(`             pnpm tools patch set --edits ${args.edits} --write\n`);
    return failures.length ? 1 : 0;
  }

  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  if (args.json) {
    process.stdout.write(`${JSON.stringify({ patterns: args.patterns, regex: args.regex, kinds, fromIndex: useText, scripts: perScript }, null, 1)}\n`);
    return failures.length ? 1 : 0;
  }

  const limit = args.limit ?? 200;
  let printed = 0;
  if (!args.count) {
    for (const s of perScript) {
      if (printed >= limit) { process.stdout.write(`… 还有 ${perScript.length} 支脚本的命中未列出（--limit ${limit}）\n`); break; }
      process.stdout.write(`\n${s.name}　（data ${s.counts.data} · src ${s.counts.src}）\n`);
      for (const g of s.groups) {
        if (printed >= limit) break;
        printed += 1;
        const d = g.data ?? null;
        const t = g.src ?? null;
        const keyTxt = String(g.key).padEnd(10);
        if (d) process.stdout.write(`  ${keyTxt} data L${String(d.line).padStart(4)} │ ${d.text}\n`);
        if (t && (!d || t.text !== d.text)) process.stdout.write(`  ${keyTxt} src        │ ${t.text}\n`);
        else if (d && t && t.text === d.text) process.stdout.write(`  ${' '.repeat(10)} src        │ （与原文相同 ⇒ 这一行还没译）\n`);
      }
    }
  }
  process.stdout.write('\n');
  process.stdout.write(`命中         ${tally.data + tally.src} 行（data ${tally.data} · src ${tally.src}）· 配对 ${tally.groups} 组 · 涉及 ${perScript.length} 支脚本 · ${secs}s\n`);
  process.stdout.write('行键         `i=<锚>` = 基线行序（= patch 的键空间）；`i=<锚>+k` = 挂在它后面的第 k 条插入行\n');
  if (args.count) {
    for (const s of perScript) process.stdout.write(`  ${s.name}  data ${s.counts.data} · src ${s.counts.src}\n`);
  }
  for (const f of failures) process.stderr.write(`✖ ${f}\n`);
  return failures.length ? 1 : 0;
}

// ─────────────────────────────────────────────────────────── edit（改过的视图 ⇒ patch）

function cmdEdit(args) {
  const p = args.patch ?? DEFAULT_PATCH;
  if (!fs.existsSync(p)) { process.stderr.write(`patch 不存在：${p}\n`); return 1; }
  const doc = loadPatch(p);
  const structural = structuralProblems(doc);
  if (structural.length) {
    process.stderr.write(`✖ 结构不变量不过（先修它）：\n  - ${structural.join('\n  - ')}\n`);
    return 1;
  }
  const ctx = mapperContext();
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });
  const dir = args.out ?? path.join(REPO_ROOT, 'dist', 'views', 'src');
  // ★ 默认扫**视图目录**：人能改的只有那里的文件（没有条目的脚本也在其中 ⇒ 给它建第一条 patch）
  const names = args.names.length
    ? args.names
    : fs.existsSync(dir)
      ? fs.readdirSync(dir).filter((f) => /\.BIN\.txt$/i.test(f)).map((f) => f.replace(/\.txt$/i, '')).sort()
      : Object.keys(doc.scripts).sort();

  const changed = [];
  const failures = [];
  // ★ **新鲜度护栏**：视图文件是生成物。若清单能证明"这一支的视图不是当前 patch 的"，
  //   反解就会把**旧状态**当成"人改的"写回 patch（症状：悄悄回退别人的改动）。
  //   ⇒ 证明得了就**拒绝**；证明不了（清单里没这支）只**警告**，因为局部生成 / 手写视图是合法路径。
  const manifestPath = path.join(dir, '..', 'manifest.json');
  let manifest = null;
  try { manifest = loadViewManifest(manifestPath); } catch { manifest = null; }
  const { subsSha } = mapperContext();
  const stale = new Set();
  const unproven = [];
  for (const name of names) {
    const m = manifest?.scripts?.[name];
    if (!m) { unproven.push(name); continue; }
    if (manifest.subsSha !== subsSha) { stale.add(name); continue; }
    const e = doc.scripts[name];
    if (e) {
      if (m.baseSha !== e.baseSha || m.resultSha !== e.resultSha) stale.add(name);
    } else if (m.baseSha !== m.resultSha) stale.add(name);
  }
  if (stale.size && !args.allowStale) {
    process.stderr.write(
      `✖ 有 ${stale.size} 支脚本的视图比 patch 旧（清单说得出来）：${[...stale].slice(0, 8).join(' / ')}${stale.size > 8 ? ' …' : ''}\n` +
        '  ⇒ 反解会把"旧状态"当成你改的写回 patch。先重建它们：\n' +
        `     pnpm tools patch view --kind src ${[...stale].slice(0, 5).map((n) => `--name '${n}'`).join(' ')}\n` +
        '  （明知故犯：加 --allow-stale）\n',
    );
    return 1;
  }
  if (unproven.length) {
    process.stderr.write(
      `⚠ 有 ${unproven.length} 支脚本的视图**证明不了**新鲜度（清单里没有它们）—— 若你刚生成过就没问题；\n` +
        '  想让它可证明：用 `pnpm tools patch view --kind src --name <脚本>` 重新生成（会记进清单）。\n',
    );
  }
  for (const name of names) {
    const file = path.join(dir, `${name}.txt`);
    if (!fs.existsSync(file)) continue;
    const edited = fs.readFileSync(file, 'utf8');
    const hit = sides.base.resolve(name);
    if (!hit) { failures.push(`${name}: 基线无法解析`); continue; }
    const before = doc.scripts[name]?.ops.length ?? 0;
    let current;
    try {
      current = buildView('src', hit.buf, doc.scripts[name] ?? NO_OPS_ENTRY(hit.buf), { lineToBin: ctx.mapper.lineToBin }).text;
    } catch (err) {
      failures.push(`${name}: 当前视图算不出来 —— ${err.message}`);
      continue;
    }
    if (edited === current) continue; // 没改过
    try {
      const { entry } = entryFromView(hit.buf, edited, ctx);
      const after = entry.ops.length;
      // ★ 逐字段构造**必须带上可选的 `header`**：`extractEntry` 的返回值里它是可选的，
    //   漏掉它会让"提取时自证通过、落盘后 verify 失败"（实测 `$1$IMINIT.BIN` 就踩了这一脚）。
    doc.scripts[name] = {
      baseSha: entry.baseSha,
      resultSha: entry.resultSha,
      ...(entry.header ? { header: entry.header } : {}),
      ops: entry.ops,
    };
      changed.push(`${name}：操作 ${before} → ${after}（降级 ${entry.stats.forcedReplace} · 局部 label ${entry.stats.localLabels}）`);
    } catch (err) {
      failures.push(`${name}: ${err.message}`);
    }
  }

  process.stdout.write(`视图来源     ${dir}\n`);
  process.stdout.write(`扫描         ${names.length} 个视图文件（只处理**与当前重建结果不同**的那些）\n`);
  if (!changed.length && !failures.length) {
    process.stdout.write('\n没有发现改动 ⇒ 什么都不写。\n');
    return 0;
  }
  for (const c of changed) process.stdout.write(`✔ ${c}\n`);
  for (const f of failures) process.stdout.write(`✖ ${f}\n`);
  if (failures.length) {
    process.stderr.write('\n✖ 有脚本反解不过 ⇒ **一个字都不写**（宁可没改，也不要一份半对的 patch）\n');
    return 1;
  }
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = savePatch(doc, p);
  if (!res.ok) { process.stderr.write(`✖ ${res.reason}\n`); return 1; }
  process.stdout.write(`\n✔ 已写入     ${p}（${res.bytes} B，回读复验通过）\n`);
  return 0;
}

// ─────────────────────────────────────────────────────────── 入口

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') { process.stdout.write(HELP); return 0; }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }
  if (args.action === 'status') return cmdStatus(args);
  if (args.action === 'baseline') return cmdBaseline(args);
  if (args.action === 'index') return cmdIndex(args);
  if (args.action === 'extract') return cmdExtract(args);
  if (args.action === 'verify') return cmdVerify(args);
  if (args.action === 'view') return cmdView(args);
  if (args.action === 'find') return cmdFind(args);
  if (args.action === 'set') return cmdSet(args);
  if (args.action === 'edit') return cmdEdit(args);
  process.stderr.write(`未知动作：${args.action}\n${HELP}`);
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
