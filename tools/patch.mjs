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
  DEFAULT_PATCH,
  DOMAIN,
  OPERATIONS,
  NO_OPS_ENTRY,
  SPEAKER_FILTER,
  REPO_ROOT,
  buildView,
  canonicalFormProblems,
  describe,
  describeText,
  entryFromView,
  extractEntry,
  loadPatch,
  mapperContext,
  allScriptNames,
  openSides,
  rebuildBin,
  savePatch,
  serializePatch,
  structuralProblems,
  verifyEntry,
} from './lib/patch.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HELP = `tools/patch.mjs —— 翻译 patch（唯一入库物 = 变更叠加层；data / src 都是视图）

  node tools/patch.mjs --describe                        自描述：字段 / 不变量 / 操作（schema 的真源）
  node tools/patch.mjs --status                          规模与分布 + 字典指纹是否与记录一致
  node tools/patch.mjs --baseline [<脚本名>…]             基线与产物分别从哪来（散装 / 哪个 ALF 的哪一段）
  node tools/patch.mjs --extract [--write]               从旧仓产物提取 patch（**迁移期一次性**）
  node tools/patch.mjs --verify [<脚本名>…]               判据：基线 + patch ⇒ **逐字节**相同
  node tools/patch.mjs --view [--kind data|src|both]     生成 \`data\` / \`src\` **视图**（生成物，不入库）
  node tools/patch.mjs --edit [--write]                  ★ **改过的 \`src\` 视图 ⇒ 反解回 patch**（只处理与当前重建结果不同的那些）

公共选项
  --base   <目录>    基线根（缺省取清单 roots.gameInstall）
  --target <目录>    **产物根**：\`--extract\` 必需；\`--verify\` **缺省不比产物**（旧仓只是迁移期的一次性来源），
                     要跟产物复核就显式给
  --name   <脚本名>  只处理这一个（可重复；\`--extract\` 带它时**不许** --write）
  --skip   <脚本名>  迁移期跳过某几支（可重复）—— ★ **理由写在文档里，代码里不维护名单**
  --limit  <n>       只处理前 n 个（同上：带它时不许 --write）
  --patch  <文件>    换一份 patch（诊断用；缺省 data/translations/patch.json）
  --out    <目录>    \`--view\` 的落点（缺省 dist/views/，已被 .gitignore 命中）
  --kind   <哪种>    \`--view\`：data / src / both（缺省 both）
  --stdout            \`--view\`：打到标准输出而不是写文件（只对单个脚本有意义）
  --bin               \`--view\`：连重建出来的 BIN 一起写（打包/调试用）

★ \`--extract\` 缺省 dry-run：只报告会写什么。加 \`--write\` 才落盘，且写前逐条复验（基线+patch ⇒ 逐字节等于产物），
  有条目过不去就**一个字都不写**。部分提取（--name/--limit）只能是 dry-run —— 否则会把其余条目删掉。
★ \`--verify\` 缺省的判据是**自证**（重建的 sha256 == 条目里的 \`resultSha\`）；\`--target\` 是**迁移期**的额外一道。
`;

function parseArgs(argv) {
  const out = { action: null, write: false, names: [], skip: [], limit: null, base: null, target: undefined, patch: null, json: false, kind: 'both', out: null, stdout: false, bin: false };
  const takesValue = new Set(['base', 'target', 'name', 'limit', 'patch', 'kind', 'out', 'skip']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--describe', '--status', '--baseline', '--extract', '--verify', '--view', '--edit', '--help', '-h'].includes(a)) {
      out.action = a.replace(/^--?/, '');
    } else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--stdout') out.stdout = true;
    else if (a === '--bin') out.bin = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} 需要值`);
      if (k === 'name') out.names.push(v);
      else if (k === 'skip') out.skip.push(v);
      else if (k === 'limit') out.limit = Number(v);
      else out[k] = v;
    } else if (['baseline', 'verify', 'extract', 'status', 'describe', 'view', 'edit'].includes(out.action)) {
      // 位置参数 = 脚本名（`--baseline SC0000.BIN` / `--verify SC0000.BIN` 两种写法都认）
      out.names.push(a);
    } else throw new Error(`多余的位置参数：${a}`);
  }
  if (!out.action) out.action = 'help';
  if (out.limit !== null && (!Number.isInteger(out.limit) || out.limit <= 0)) throw new Error('--limit 必须是正整数');
  if (!['data', 'src', 'both'].includes(out.kind)) throw new Error('--kind 只能是 data / src / both');
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
  const sides = openSides({ baseDir: args.base ?? undefined, targetDir: null });
  const all = Object.keys(doc.scripts).sort();
  const names = args.names.length ? args.names : pickNames([], all, args.limit);
  const kinds = args.kind === 'both' ? ['data', 'src'] : [args.kind];
  const outRoot = args.out ?? path.join(REPO_ROOT, 'dist', 'views');

  if (args.stdout && names.length !== 1) {
    process.stderr.write('✖ --stdout 只对**单个**脚本有意义（用 --name 指定）\n');
    return 2;
  }

  let written = 0;
  let bytes = 0;
  const t0 = Date.now();
  for (const kind of kinds) {
    const dir = path.join(outRoot, kind);
    if (!args.stdout) fs.mkdirSync(dir, { recursive: true });
    for (const [idx, name] of names.entries()) {
      const hit = sides.base.resolve(name);
      if (!hit) { process.stderr.write(`✖ ${name}：基线无法解析\n`); return 1; }
      let view;
      try {
        // 没有条目的脚本 = 没有变更 ⇒ 用空叠加层（`src` 视图于是等于 `data` 视图）
        const entry = doc.scripts[name] ?? NO_OPS_ENTRY(hit.buf);
        view = buildView(kind, hit.buf, entry, { lineToBin: ctx.mapper.lineToBin });
      } catch (err) {
        process.stderr.write(`✖ ${name}（${kind}）：${err.message}\n`);
        return 1;
      }
      if (args.stdout) {
        process.stdout.write(view.text);
        continue;
      }
      const file = path.join(dir, `${name}.txt`);
      fs.writeFileSync(file, view.text, 'utf8');
      bytes += Buffer.byteLength(view.text);
      written += 1;
      if (args.bin) fs.writeFileSync(path.join(outRoot, `${kind}-bin`, name), view.bin);
      if ((idx + 1) % 50 === 0) process.stdout.write(`… ${kind} ${idx + 1}/${names.length}\n`);
    }
  }
  if (args.stdout) return 0;
  process.stdout.write(`视图落点     ${outRoot}\n`);
  process.stdout.write(`写出         ${written} 个文件（${kinds.join(' + ')} · ${names.length} 个脚本 · ${mb(bytes)} · ${((Date.now() - t0) / 1000).toFixed(1)}s）\n`);
  process.stdout.write('（生成物：可无限重算，不入库；真源只有基线 + patch）\n');
  return 0;
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
  if (args.action === 'extract') return cmdExtract(args);
  if (args.action === 'verify') return cmdVerify(args);
  if (args.action === 'view') return cmdView(args);
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
