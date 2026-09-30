#!/usr/bin/env node
/**
 * tools/corpus.mjs — **CLI**：`corpus/assets.json` 的守卫与唯一写入口。
 *
 * 分层（`tools/README.md` §0）：**参数 → 模型 → 输出**，仅此而已。
 *   · 模型（schema / 不变量 / 读 / 写）在 `lib/manifest.mjs`；纯工具在 `lib/{paths,fsx,exec}.mjs`。
 *   · 本文件不实现任何规则，也不被别的 CLI import（`fixtures` 要用清单模型时 import 的是 lib）。
 *
 * 经派发器：`pnpm tools corpus <validate|list|describe|scan|add|set|set-root|normalize> [args…]`
 * 也可独立跑：`node tools/corpus.mjs --validate`
 */
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  CARRYING,
  DEFAULT_MANIFEST,
  DOMAIN,
  OPERATIONS,
  describe,
  describeText,
  loadManifest,
  resolveOrigin,
  saveManifest,
  validateManifest,
} from './lib/manifest.mjs';
import { sha256File, statKind } from './lib/fsx.mjs';
import { REPO_ROOT } from './lib/paths.mjs';

export { DEFAULT_MANIFEST, DOMAIN, OPERATIONS, describe, describeText };
// ─────────────────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, noHash: false, noGit: false, noRecipe: false, manifest: DEFAULT_MANIFEST, rest: [] };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--validate' || a === '--scan' || a === '--list' || a === '--describe' || a === '--normalize')
      out.action = a.slice(2);
    else if (a === '--add') { out.action = 'add'; out.rest.push(argv[++i]); }
    else if (a === '--set') { out.action = 'set'; out.rest.push(argv[++i], argv[++i]); }
    else if (a === '--set-root') { out.action = 'set-root'; out.rest.push(argv[++i], argv[++i]); }
    else if (a === '--manifest') out.manifest = path.resolve(argv[++i]);
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--no-hash') out.noHash = true;
    else if (a === '--no-git') out.noGit = true;
    else if (a === '--no-recipe') out.noRecipe = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
    else out.rest.push(a);
  }
  if (!out.action) out.action = 'validate';
  return out;
}

function printReport(manifest, report, { json }) {
  if (json) {
    process.stdout.write(`${JSON.stringify({ schemaVersion: manifest.schemaVersion, ...report }, null, 2)}\n`);
    return;
  }
  for (const c of report.checks) {
    const tag = c.status === 'pass' ? 'ok  ' : c.status === 'warn' ? 'WARN' : 'FAIL';
    process.stdout.write(`[${tag}] #${c.id} ${c.title}\n        → ${c.message}\n`);
    for (const d of c.details) process.stdout.write(`        · ${d}\n`);
  }
  process.stdout.write(
    `\n${report.checks.length} 条断言：${report.checks.length - report.failures - report.warnings} 通过 / ` +
      `${report.failures} 失败 / ${report.warnings} 警告   （${manifest.entries.length} 个条目）\n`,
  );
}

function validateOpts(args) {
  return {
    repoRoot: REPO_ROOT,
    runGit: !args.noGit,
    checkHashes: !args.noHash,
    runRecipe: !args.noRecipe,
  };
}

function cmdScan(manifest, manifestPath, args) {
  const roots = manifest.roots ?? {};
  const plan = [];
  for (const e of manifest.entries) {
    if (CARRYING.has(e.storage)) continue; // 入库件不写 sha256
    for (const o of e.origin ?? []) {
      const abs = resolveOrigin(REPO_ROOT, roots, o);
      if (abs === null || statKind(abs) !== 'file') continue; // 目录 / 缺失：不写
      const actual = sha256File(abs);
      if (o.sha256 === actual) continue;
      plan.push({ entry: e.id, origin: o, from: o.sha256 ?? null, to: actual, path: abs });
    }
  }
  if (plan.length === 0) {
    process.stdout.write('--scan：没有需要补的 sha256（全部与盘上一致）。\n');
    return 0;
  }
  for (const p of plan) {
    process.stdout.write(`${p.from ? 'update' : 'fill  '} ${p.entry}  ${p.origin.root}:${p.origin.path}\n        ${p.to}\n`);
  }
  if (!args.write) {
    process.stdout.write(`\n（dry-run）共 ${plan.length} 处待写。加 --write 落盘。\n`);
    return 0;
  }
  for (const p of plan) p.origin.sha256 = p.to;
  const res = saveManifest(manifest, manifestPath, validateOpts(args));
  if (!res.ok) {
    process.stderr.write(`--scan 失败：${res.reason}\n`);
    return 1;
  }
  process.stdout.write(`\n已写入 ${plan.length} 处并回读复验通过。\n`);
  return 0;
}

function cmdAdd(manifest, manifestPath, args) {
  const raw = args.rest[0];
  if (!raw) throw new Error('--add 需要一段 entry JSON');
  const entry = JSON.parse(raw);
  if ((manifest.entries ?? []).some((e) => e.id === entry.id)) throw new Error(`id 已存在：${entry.id}`);
  const preview = { ...manifest, entries: [...manifest.entries, entry] };
  const { failures, checks } = validateManifest(preview, validateOpts(args));
  const bad = checks.filter((c) => c.status === 'fail');
  for (const c of bad) process.stdout.write(`[FAIL] #${c.id} ${c.title}\n        → ${c.message}\n`);
  if (failures > 0 && !args.write) {
    process.stdout.write(`\n（dry-run）新增会让守卫红 ${failures} 条；--write 也会被回滚。\n`);
    return 0;
  }
  if (!args.write) {
    process.stdout.write('（dry-run）新增合法。加 --write 落盘。\n');
    return 0;
  }
  const res = saveManifest(preview, manifestPath, validateOpts(args));
  if (!res.ok) {
    process.stderr.write(`--add 失败：${res.reason}\n`);
    return 1;
  }
  process.stdout.write(`已新增 ${entry.id} 并回读复验通过。\n`);
  return 0;
}

function cmdSet(manifest, manifestPath, args) {
  const [id, raw] = args.rest;
  if (!id || !raw) throw new Error('--set 需要 <id> 与一段 patch JSON');
  const idx = (manifest.entries ?? []).findIndex((e) => e.id === id);
  if (idx < 0) throw new Error(`找不到 id：${id}`);
  const patch = JSON.parse(raw);
  const before = manifest.entries[idx];
  const after = { ...before, ...patch };
  const preview = { ...manifest, entries: manifest.entries.map((e, i) => (i === idx ? after : e)) };
  process.stdout.write(`${id} 的改动：\n`);
  for (const k of new Set([...Object.keys(before), ...Object.keys(patch)])) {
    if (JSON.stringify(before[k]) === JSON.stringify(after[k])) continue;
    process.stdout.write(`  ${k}: ${JSON.stringify(before[k])} → ${JSON.stringify(after[k])}\n`);
  }
  if (!args.write) {
    process.stdout.write('（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = saveManifest(preview, manifestPath, validateOpts(args));
  if (!res.ok) {
    process.stderr.write(`--set 失败：${res.reason}\n`);
    return 1;
  }
  if (res.dropped?.length) {
    process.stdout.write(`（已自动剔除 ${res.dropped.length} 处入库件 origin 上的 sha256：#4 规则）\n  ${res.dropped.join('\n  ')}\n`);
  }
  process.stdout.write(`已更新 ${id} 并回读复验通过。\n`);
  return 0;
}

function cmdSetRoot(manifest, manifestPath, args) {
  const [name, value] = args.rest;
  if (!name || !value) throw new Error('--set-root 需要 <root 名> <路径>');
  const before = manifest.roots?.[name];
  const preview = { ...manifest, roots: { ...manifest.roots, [name]: value } };
  process.stdout.write(`roots.${name}: ${before === undefined ? '（新增）' : JSON.stringify(before)} → ${JSON.stringify(value)}\n`);
  if (!args.write) {
    process.stdout.write('（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = saveManifest(preview, manifestPath, validateOpts(args));
  if (!res.ok) {
    process.stderr.write(`--set-root 失败：${res.reason}\n`);
    return 1;
  }
  process.stdout.write(`已更新 roots.${name} 并回读复验通过。\n`);
  return 0;
}

const MANIFEST_TOP_KEYS = ['schemaVersion', '_doc', 'roots', 'entries'];

/** 把清单拉回规范形态：剔除多余顶层键 + 重新规范化（`_doc` 会按本工具的版本重写） */
function cmdNormalize(manifest, manifestPath, args) {
  const dropped = Object.keys(manifest).filter((k) => !MANIFEST_TOP_KEYS.includes(k));
  const plan = [
    dropped.length ? `剔除多余顶层键：${dropped.join(', ')}` : '没有多余顶层键',
    `规范形态：顶层 ${MANIFEST_TOP_KEYS.join(' , ')}；条目按 id 排序、字段按固定键序`,
    `当前条目：${manifest.entries.length} 个`,
  ];
  if (!args.write) return { plan, apply: null };
  return {
    plan,
    apply: () => {
      for (const k of dropped) delete manifest[k];
      manifest.entries.sort((a, b) => a.id.localeCompare(b.id));
      const res = saveManifest(manifest, manifestPath, validateOpts(args));
      if (!res.ok) throw new Error(res.reason);
      return dropped.length ? `已剔除 ${dropped.length} 个多余顶层键并复验通过` : '已复验（无需改动）';
    },
  };
}

function cmdList(manifest, args) {
  if (args.json) {
    process.stdout.write(`${JSON.stringify(manifest.entries.map((e) => ({
      id: e.id, kind: e.kind, role: e.role, storage: e.storage, dest: e.dest, origins: e.origin?.length ?? 0,
    })), null, 2)}\n`);
    return 0;
  }
  const rows = manifest.entries.map((e) => [e.id, e.kind, e.role, e.storage, e.dest ?? '—', String(e.origin?.length ?? 0)]);
  const head = ['id', 'kind', 'role', 'storage', 'dest', 'origin'];
  const w = head.map((h, i) => Math.max(h.length, ...rows.map((r) => r[i].length)));
  const line = (cells) => cells.map((c, i) => c.padEnd(w[i])).join('  ');
  process.stdout.write(`${line(head)}\n${line(w.map((n) => '-'.repeat(n)))}\n`);
  for (const r of rows) process.stdout.write(`${line(r)}\n`);
  process.stdout.write(`\n${rows.length} 个条目\n`);
  return 0;
}

const HELP = `tools/corpus.mjs — corpus/assets.json 的守卫与唯一写入口

  node tools/corpus.mjs --validate [--no-hash] [--no-git] [--no-recipe] [--json]
  node tools/corpus.mjs --scan [--write]
  node tools/corpus.mjs --add '<entry-json>' [--write]
  node tools/corpus.mjs --set <id> '<patch-json>' [--write]
  node tools/corpus.mjs --set-root <name> <path> [--write]
  node tools/corpus.mjs --list [--json]
  pnpm tools corpus describe [--json]        # 自描述：字段 / 枚举 / 不变量 / 怎么查怎么改
  node tools/corpus.mjs --normalize [--write]      # 拉回规范形态（剔多余顶层键、重排、重写 _doc）
  node tools/corpus.mjs --manifest <path>          # 换一份清单
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }
  const manifest = loadManifest(args.manifest);
  switch (args.action) {
    case 'validate': {
      const report = validateManifest(manifest, validateOpts(args));
      printReport(manifest, report, args);
      return report.failures > 0 ? 1 : 0;
    }
    case 'scan':
      return cmdScan(manifest, args.manifest, args);
    case 'add':
      return cmdAdd(manifest, args.manifest, args);
    case 'set':
      return cmdSet(manifest, args.manifest, args);
    case 'set-root':
      return cmdSetRoot(manifest, args.manifest, args);
    case 'list':
      return cmdList(manifest, args);
    case 'normalize': {
      const { plan, apply } = cmdNormalize(manifest, args.manifest, args);
      for (const line of plan) process.stdout.write(`${line}\n`);
      if (!apply) {
        process.stdout.write('\n（dry-run）加 --write 落盘。\n');
        return 0;
      }
      process.stdout.write(`\n${apply()}\n`);
      return 0;
    }
    default:
      process.stderr.write(`未知动作：${args.action}\n${HELP}`);
      return 2;
  }
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n`);
    process.exitCode = 2;
  }
}
