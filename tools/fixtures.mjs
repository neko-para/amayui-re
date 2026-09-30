#!/usr/bin/env node
/**
 * tools/fixtures.mjs — **CLI**：`corpus/fixtures/` 的唯一编辑入口。
 *
 * 分层（`tools/README.md` §0）：
 *   · 载荷（`samples.json`）的模型在 `lib/samples.mjs`；清单模型在 `lib/manifest.mjs`；纯工具在 `lib/*.mjs`。
 *   · 本文件只做"参数 → 模型 → 输出 + 落盘计划（缺省 dry-run）"，不实现任何规则。
 *
 * ★ 清单侧的耦合是**条件**的：fixture 是**自足条目**（固化资源，没有加工链），清单里那条
 *   `fixtures/save-samples` **不登记 origin**。只有显式给了 `--from`（= 声明"这一份是从那儿拷来的"）
 *   才会去写清单的 origin —— 不声明的路径上，本工具**不碰** `corpus/assets.json`。
 *
 * 经派发器：`pnpm tools fixtures <list|describe|restore-mtime|add|refresh|refresh-all|remove|normalize> [args…]`
 * 也可独立跑：`node tools/fixtures.mjs --list`
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { DEFAULT_MANIFEST, entryOf, loadManifest, originsByName, resolveOrigin, saveManifest } from './lib/manifest.mjs';
import {
  CARRY_ID,
  DEFAULT_SAMPLES,
  DOMAIN,
  KNOWN_TOP_KEYS,
  OPERATIONS,
  describe,
  describeText,
  fileRec,
  loadSamples,
  saveSamples,
  stripUnknownTopKeys,
} from './lib/samples.mjs';
import { FIXTURES_DIR, REPO_ROOT } from './lib/paths.mjs';
import { sha256File, statKind } from './lib/fsx.mjs';

export { DEFAULT_MANIFEST, DEFAULT_SAMPLES, DOMAIN, OPERATIONS, describe, describeText };

// 兼容旧称呼：`byName` 现在是清单模型里的 `originsByName`
const byName = originsByName;

/** 槽号 → 两个载荷文件名（与 `lib/samples.mjs` 的 `filesOf` 同一口径，这里只需要名字） */
const filesOf = (slot) => ['DAT', 'STH'].map((e) => `SAVE${slot}.${e}`);

/**
 * 算出一个槽的源目录：
 *   · 显式 `--from` 优先；
 *   · 否则用 `--root`（缺省 `gameSaves`）在清单的 roots 里解析出目录。
 * @returns {{fromAbs:string, rootName:string|null}}
 */
function resolveSource(args, manifest) {
  if (args.from) return { fromAbs: path.resolve(args.from), rootName: null };
  const rootName = args.root ?? 'gameSaves';
  const base = manifest.roots?.[rootName];
  if (typeof base !== 'string') {
    throw new Error(`清单的 roots 里没有 "${rootName}"（先用 pnpm tools corpus set-root 登记，或显式给 --from <源目录>）`);
  }
  return { fromAbs: path.isAbsolute(base) ? base : path.resolve(REPO_ROOT, base), rootName };
}

/** 把 `--from` 相对某个 roots 根的路径算出来（登记 origin 用）；不在根下就回 null */
function relFromRoot(manifest, rootName, fromAbs) {
  if (rootName === null) return null;
  const base = manifest.roots?.[rootName];
  if (typeof base !== 'string') return null;
  const baseAbs = path.isAbsolute(base) ? base : path.resolve(REPO_ROOT, base);
  const rel = path.relative(baseAbs, fromAbs);
  if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel)) return null;
  return rel.split(path.sep).join('/');
}

// ───────────────────────────────────────────────── 子命令（返回 {plan, apply}）

function cmdList(args, samples) {
  const rows = samples.samples.map((s) => {
    const files = s.files.map((f) => {
      const p = path.join(FIXTURES_DIR, f.name);
      const st = statKind(p) === 'file' ? Math.round(fs.statSync(p).mtimeMs) : null;
      const drift = st === null ? 'missing' : Math.abs(st - f.mtimeMs) <= 1000 ? 'ok' : 'drifted';
      return `${f.name} ${drift}`;
    });
    return { slot: s.slot, where: s.where, files };
  });
  const plan = args.json
    ? [JSON.stringify(rows, null, 2)]
    : [
        'slot  where                  files',
        ...rows.map((r) => `${r.slot}    ${r.where.padEnd(20)} ${r.files.join(' , ')}`),
      ];
  return { plan, apply: null };
}

function cmdRestoreMtime(args, samples) {
  const plan = [];
  const jobs = [];
  for (const s of samples.samples) {
    for (const f of s.files) {
      const p = path.join(FIXTURES_DIR, f.name);
      if (statKind(p) !== 'file') {
        plan.push(`FAIL 缺文件 ${f.name}`);
        continue;
      }
      const cur = Math.round(fs.statSync(p).mtimeMs);
      if (Math.abs(cur - f.mtimeMs) <= 1000) {
        plan.push(`ok   ${f.name} 已经是记录的 instant`);
        continue;
      }
      plan.push(`set  ${f.name}  ${new Date(cur).toISOString()} → ${new Date(f.mtimeMs).toISOString()}（墙上 ${f.mtimeLocal} @UTC${f.tzOffsetMinutes >= 0 ? '+' : ''}${f.tzOffsetMinutes / 60}）`);
      jobs.push([p, f.mtimeMs]);
    }
  }
  return {
    plan,
    apply: () => {
      for (const [p, ms] of jobs) {
        const t = ms / 1000;
        fs.utimesSync(p, t, t);
      }
      return `${jobs.length} 个文件的 mtime 已按 samples.json 设回`;
    },
  };
}

function cmdAdd(args, samples, manifest) {
  const slot = args.rest[0];
  if (!slot) throw new Error('--add 需要 <槽号>');
  if (!/^\d{2,3}$/.test(slot)) throw new Error(`槽号形态非法：${slot}`);
  if (samples.samples.some((s) => s.slot === slot)) throw new Error(`槽 ${slot} 已在 samples.json 里`);
  if (!args.where) throw new Error('--add 需要 --where <游戏内定位>（这是给人看的信息，必须由来源方给出）');

  const { fromAbs, rootName } = resolveSource(args, manifest);
  const relDir = relFromRoot(manifest, rootName, fromAbs);
  const recs = [];
  const plan = [`add 槽 ${slot}（${args.where}） 取自 ${fromAbs}`];
  for (const n of filesOf(slot)) {
    const src = path.join(fromAbs, n);
    if (statKind(src) !== 'file') throw new Error(`源文件不存在：${src}`);
    const rec = fileRec(src);
    recs.push({ rel: relDir === null ? null : `${relDir}/${n}`, name: n, src, rec });
    plan.push(`  copy ${src}\n    →  corpus/fixtures/${n}   (mtime ← 源；sha256 ${sha256File(src).slice(0, 12)}…)`);
  }
  plan.push(
    relDir === null
      ? '★ 不写清单 origin：没有可登记的 root（--from 是显式目录，且未落在任何 roots 根下）—— fixture 本来就是自足条目'
      : `清单：fixtures/save-samples 的 origin += ${recs.map((r) => `${rootName}:${r.rel}`).join(' , ')}`,
  );

  const touchManifest = recs.every((r) => r.rel !== null);
  return {
    plan,
    apply: () => {
      // ① 拷文件 + 设 mtime
      for (const r of recs) {
        const dst = path.join(FIXTURES_DIR, r.name);
        fs.copyFileSync(r.src, dst);
        const t = r.rec.mtimeMs / 1000;
        fs.utimesSync(dst, t, t);
      }
      // ② 清单（只在"确实声明了来源"时写；走同一个 saveManifest ⇒ 清单仍只有一个写入口）
      if (touchManifest) {
        const entry = entryOf(manifest, CARRY_ID);
        entry.origin = [...(entry.origin ?? []), ...recs.map((r) => ({ root: rootName, path: r.rel }))];
        const mres = saveManifest(manifest, args.manifest, { repoRoot: REPO_ROOT });
        if (!mres.ok) {
          for (const r of recs) {
            try {
              fs.unlinkSync(path.join(FIXTURES_DIR, r.name));
            } catch {
              /* ignore */
            }
          }
          throw new Error(`清单写入失败（已回滚拷贝）：${mres.reason}`);
        }
      }
      // ③ samples.json
      samples.samples.push({ slot, where: args.where, files: recs.map((r) => r.rec) });
      const sres = saveSamples(samples, args.samples);
      if (!sres.ok) throw new Error(sres.reason);
      return `已加入槽 ${slot}（${touchManifest ? '清单 origin + samples.json' : 'samples.json'} 都已复验）`;
    },
  };
}

function cmdRefresh(args, samples, manifest) {
  const only = args.rest[0];
  const targets = only ? samples.samples.filter((s) => s.slot === only) : samples.samples;
  if (targets.length === 0) throw new Error(only ? `samples.json 里没有槽 ${only}` : 'samples.json 里没有样本');
  const srcMap = byName(manifest, CARRY_ID);

  // 源目录：`--from` / `--root` 显式指定优先；否则按清单登记的 origin 逐文件解析
  // （★ origin 可以缺席 —— fixture 是自足条目 —— 这时如实报出并指向 `--from`，不静默跳过）
  const explicit = args.from || args.root ? resolveSource(args, manifest) : null;

  const plan = [];
  const jobs = [];
  for (const s of targets) {
    for (const f of s.files) {
      let abs = null;
      let how = '';
      if (explicit) {
        abs = path.join(explicit.fromAbs, f.name);
        how = explicit.fromAbs;
      } else {
        const o = srcMap.get(f.name);
        if (!o) {
          plan.push(`FAIL ${f.name} 清单里没有登记 origin（给 --from <源目录> 或先 add 时登记）`);
          continue;
        }
        abs = resolveOrigin(REPO_ROOT, manifest.roots, o);
        how = `${o.root}:${o.path}`;
      }
      if (abs === null || statKind(abs) !== 'file') {
        plan.push(`FAIL ${f.name} 的来源不在盘上：${how}`);
        continue;
      }
      const rec = fileRec(abs);
      const drift = Math.abs(rec.mtimeMs - f.mtimeMs) > 1000;
      plan.push(
        `${drift ? 'sync' : 'ok  '} ${f.name}  ← ${how}` +
          (drift ? `  (mtime ${new Date(f.mtimeMs).toISOString()} → ${new Date(rec.mtimeMs).toISOString()})` : ''),
      );
      jobs.push({ f, abs, rec, drift });
    }
  }
  return {
    plan,
    apply: () => {
      let n = 0;
      for (const j of jobs) {
        const dst = path.join(FIXTURES_DIR, j.f.name);
        fs.copyFileSync(j.abs, dst);
        const t = j.rec.mtimeMs / 1000;
        fs.utimesSync(dst, t, t);
        Object.assign(j.f, j.rec);
        if (j.drift) n += 1;
      }
      // ★ 清单侧**一个字都不改**：origin 是"从哪来"，不是"内容是什么"；
      //   内容有没有变由 git/LFS 与 samples.json 的 mtime 事实回答。
      const sres = saveSamples(samples, args.samples);
      if (!sres.ok) throw new Error(sres.reason);
      return `已重取 ${jobs.length} 个文件（其中 ${n} 个确有变化）`;
    },
  };
}

function cmdRemove(args, samples, manifest) {
  const slot = args.rest[0];
  if (!slot) throw new Error('--remove 需要 <槽号>');
  const idx = samples.samples.findIndex((s) => s.slot === slot);
  if (idx < 0) throw new Error(`samples.json 里没有槽 ${slot}`);
  const names = filesOf(slot);
  const entry = entryOf(manifest, CARRY_ID);
  const affected = (entry.origin ?? []).filter((o) => names.includes(path.basename(o.path)));
  const plan = [
    `remove 槽 ${slot}：删 ${names.join(' + ')}` +
      (affected.length ? `，并从清单 origin 里摘掉 ${affected.length} 条` : '（清单没有 origin，无需动它）'),
  ];
  return {
    plan,
    apply: () => {
      if (affected.length) {
        entry.origin = (entry.origin ?? []).filter((o) => !names.includes(path.basename(o.path)));
        const mres = saveManifest(manifest, args.manifest, { repoRoot: REPO_ROOT });
        if (!mres.ok) throw new Error(`清单写入失败（槽未摘除）：${mres.reason}`);
      }
      for (const n of names) {
        try {
          fs.unlinkSync(path.join(FIXTURES_DIR, n));
        } catch {
          /* ignore */
        }
      }
      samples.samples.splice(idx, 1);
      const sres = saveSamples(samples, args.samples);
      if (!sres.ok) throw new Error(sres.reason);
      return `已移除槽 ${slot}`;
    },
  };
}

function cmdNormalize(args, samples) {
  const dropped = Object.keys(samples).filter((k) => !KNOWN_TOP_KEYS.includes(k));
  const plan = [
    dropped.length ? `剔除多余顶层键：${dropped.join(', ')}` : '没有多余顶层键',
    `规范形态：顶层 ${KNOWN_TOP_KEYS.join(' , ')}；槽按号排序、每槽固定 DAT/STH 顺序`,
    `当前槽：${samples.samples.map((s) => s.slot).join(', ')}`,
  ];
  return {
    plan,
    apply: () => {
      stripUnknownTopKeys(samples);
      const res = saveSamples(samples, args.samples);
      if (!res.ok) throw new Error(res.reason);
      return dropped.length ? `已剔除 ${dropped.length} 个多余顶层键并复验通过` : '已复验（无需改动）';
    },
  };
}

// ───────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, rest: [], samples: DEFAULT_SAMPLES, manifest: DEFAULT_MANIFEST };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--list' || a === '--restore-mtime' || a === '--refresh-all' || a === '--normalize' || a === '--describe')
      out.action = a.slice(2);
    else if (a === '--add' || a === '--refresh' || a === '--remove') {
      out.action = a.slice(2);
      out.rest.push(argv[++i]);
    } else if (a === '--from') out.from = argv[++i];
    else if (a === '--root') out.root = argv[++i];
    else if (a === '--where') out.where = argv[++i];
    else if (a === '--samples') out.samples = path.resolve(argv[++i]);
    else if (a === '--manifest') out.manifest = path.resolve(argv[++i]);
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
  }
  return out;
}

const HELP = `tools/fixtures.mjs — corpus/fixtures/ 的唯一编辑入口（缺省 dry-run，加 --write 落盘）

  node tools/fixtures.mjs --list [--json]
  node tools/fixtures.mjs --restore-mtime [--write]
  node tools/fixtures.mjs --add <槽> --where <定位> [--from <源目录> | --root <roots 名>] [--write]
  node tools/fixtures.mjs --refresh <槽> | --refresh-all [--from <源目录>] [--root <roots 名>] [--write]
  node tools/fixtures.mjs --remove <槽> [--write]
  node tools/fixtures.mjs --normalize [--write]              # schema 变过后把文件拉回规范形态
  pnpm tools fixtures describe [--json]                # 自描述：字段 / 不变量（含谁在守）/ 怎么查怎么改

★ fixture 是**自足条目**（固化资源，没有加工链）⇒ 清单里不登记 origin，所以 --refresh 缺省会如实报
  "没有登记 origin"。要重取就给 --from <源目录> 或 --root <roots 名>；只有显式给了来源，本工具才会
  把 origin 写进清单（不声明的路径上它**不碰** corpus/assets.json）。
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help' || !args.action) {
    process.stdout.write(HELP);
    return args.action === 'help' ? 0 : 2;
  }
  if (args.action === 'describe') {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }
  const samples = loadSamples(args.samples);
  const manifest = loadManifest(args.manifest);
  const cmds = {
    list: () => cmdList(args, samples),
    'restore-mtime': () => cmdRestoreMtime(args, samples),
    add: () => cmdAdd(args, samples, manifest),
    refresh: () => cmdRefresh(args, samples, manifest),
    'refresh-all': () => cmdRefresh({ ...args, rest: [] }, samples, manifest),
    normalize: () => cmdNormalize(args, samples),
    remove: () => cmdRemove(args, samples, manifest),
  };
  const cmd = cmds[args.action];
  if (!cmd) {
    process.stderr.write(`未知动作：${args.action}\n${HELP}`);
    return 2;
  }
  const { plan, apply } = cmd();
  for (const line of plan) process.stdout.write(`${line}\n`);
  if (!apply) return 0;
  const mutating = args.action !== 'list';
  if (!args.write && mutating) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const msg = apply();
  process.stdout.write(`\n${msg}\n`);
  return 0;
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
