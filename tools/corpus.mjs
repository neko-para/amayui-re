#!/usr/bin/env node
/**
 * tools/corpus.mjs — `corpus/assets.json` 的**守卫**与**唯一写入口**。
 *
 * 设计口径（见 docs/00-origin/decisions.md §7 与 corpus/README.md §2）：
 *   · assets.json 是 **lockfile**：只记"来源与去向"，不记规则、不重复校验和、不写可从磁盘推导的计数。
 *   · 唯一能把它变成**约束**的是本文件的 `--validate` 必须能红。没有守卫的清单等于一份 Markdown。
 *
 * 用法：
 *   node tools/corpus.mjs --validate [--no-hash] [--json]      # 守卫（缺省动作）
 *   node tools/corpus.mjs --scan [--write]                     # 补 origin[].sha256（缺省 dry-run）
 *   node tools/corpus.mjs --add '<entry-json>' [--write]       # 追加条目
 *   node tools/corpus.mjs --set <id> '<patch-json>' [--write]  # 改条目（浅合并；数组整体替换）
 *   node tools/corpus.mjs --list [--json]                      # 列条目一览
 *   node tools/corpus.mjs --manifest <path>                    # 换一份清单（测试用）
 *
 * 写纪律：任何写入都先写临时文件再原子改名；写完**回读 + 复验**，不绿则**回滚**。
 */
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const DEFAULT_MANIFEST = path.join(REPO_ROOT, 'corpus', 'assets.json');

export const KINDS = [
  'disasm-corpus',
  'binary',
  'fixture',
  'asset',
  'knowledge-source',
  'tooling',
  'agent-infra',
];
export const ROLES = ['baseline', 'reference', 'module', 'fixture', 'archived', 'rebuild', 'deferred'];
export const STORAGES = ['lfs', 'git', 'external-only', 'deferred'];

/** 真正会被搬进仓库的 storage（= §3.3 里"入库件"） */
export const CARRYING = new Set(['lfs', 'git']);
/** 只登记、不进仓库的 storage */
export const NOT_CARRYING = new Set(['external-only', 'deferred']);

const REQUIRED_FIELDS = ['id', 'kind', 'role', 'storage', 'origin', 'dest', 'readOnly', 'consume'];
const ENTRY_KEY_ORDER = [
  'id',
  'kind',
  'role',
  'storage',
  'origin',
  'derivedFrom',
  'dest',
  'readOnly',
  'recipe',
  'consume',
  'blocks',
  'note',
];
const ORIGIN_KEY_ORDER = ['root', 'path', 'sha256'];
const DERIVED_KEY_ORDER = ['ref', 'tool', 'note'];
const ROOT_KEY_ORDER = ['oldRepo', 'gameInstall', 'staging'];
const ID_RE = /^[a-z0-9][a-z0-9-]*\/[a-z0-9][a-z0-9._-]*$/;
const SHA256_RE = /^[0-9a-f]{64}$/;

// ─────────────────────────────────────────────────────────── 基础工具

export function loadManifest(manifestPath = DEFAULT_MANIFEST) {
  return JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
}

function orderKeys(obj, order) {
  const out = {};
  for (const k of order) if (k in obj) out[k] = obj[k];
  for (const k of Object.keys(obj)) if (!(k in out)) out[k] = obj[k]; // 未知键保留在尾部，绝不静默丢数据
  return out;
}

function canonOrigin(o) {
  const y = orderKeys(o, ORIGIN_KEY_ORDER);
  return y;
}

function canonDerived(d) {
  return orderKeys(d, DERIVED_KEY_ORDER);
}

function canonEntry(e) {
  const y = orderKeys(e, ENTRY_KEY_ORDER);
  if (Array.isArray(y.origin)) y.origin = y.origin.map(canonOrigin);
  if (Array.isArray(y.derivedFrom)) y.derivedFrom = y.derivedFrom.map(canonDerived);
  return y;
}

/** 规范化序列化：键序固定 ⇒ diff 干净、写入确定 */
export function canonicalStringify(manifest) {
  const roots = orderKeys(manifest.roots ?? {}, ROOT_KEY_ORDER);
  const out = {
    schemaVersion: manifest.schemaVersion,
    roots,
    entries: (manifest.entries ?? []).map(canonEntry),
  };
  for (const k of Object.keys(manifest)) {
    if (!(k in out)) out[k] = manifest[k];
  }
  return `${JSON.stringify(out, null, 2)}\n`;
}

/** 解析 origin 的绝对路径；root='abs' 表示 origin.path 本身就是绝对路径 */
export function resolveOrigin(repoRoot, roots, origin) {
  if (origin.root === 'abs') return path.resolve(origin.path);
  const base = roots?.[origin.root];
  if (base === undefined) return null;
  const baseAbs = path.isAbsolute(base) ? base : path.resolve(repoRoot, base);
  return path.resolve(baseAbs, origin.path);
}

/** 'file' | 'dir' | 'other' | 'missing'（跟随符号链接，因为外部素材本来就是链接） */
export function statKind(p) {
  try {
    const st = fs.statSync(p);
    if (st.isFile()) return 'file';
    if (st.isDirectory()) return 'dir';
    return 'other';
  } catch {
    return 'missing';
  }
}

export function sha256File(p) {
  const h = createHash('sha256');
  const buf = fs.readFileSync(p);
  h.update(buf);
  return h.digest('hex');
}

function runGit(repoRoot, args) {
  try {
    const out = execFileSync('git', args, { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    return { code: 0, out };
  } catch (err) {
    return { code: err.status ?? 1, out: `${err.stdout ?? ''}${err.stderr ?? ''}` };
  }
}

export function gitCheckIgnore(repoRoot, relPath) {
  const r = runGit(repoRoot, ['check-ignore', '-v', '--', relPath]);
  return { ignored: r.code === 0, detail: r.out.trim() };
}

export function gitAttrFilter(repoRoot, relPath) {
  const r = runGit(repoRoot, ['check-attr', 'filter', '--', relPath]);
  const m = /filter:\s*(\S+)/.exec(r.out);
  return { value: m ? m[1] : 'unspecified', detail: r.out.trim() };
}

// ─────────────────────────────────────────────────────────── 守卫（§3.3 的 9 条）

/**
 * @returns {{checks: Array<{id:number,title:string,status:'pass'|'fail'|'warn',message:string,details:string[]}>,
 *            failures:number, warnings:number}}
 */
export function validateManifest(manifest, opts = {}) {
  const {
    repoRoot = REPO_ROOT,
    runGit: useGit = true,
    checkHashes = true,
    runRecipe: useRecipe = true,
  } = opts;

  const checks = [];
  const add = (id, title, status, message, details = []) =>
    checks.push({ id, title, status, message, details });

  const entries = Array.isArray(manifest?.entries) ? manifest.entries : [];
  const byId = new Map();
  const roots = manifest?.roots ?? {};

  // (1) schema ──────────────────────────────────────────────
  {
    const details = [];
    if (manifest?.schemaVersion !== 1) details.push(`schemaVersion 必须是 1，实际 ${JSON.stringify(manifest?.schemaVersion)}`);
    if (!roots || typeof roots !== 'object' || Array.isArray(roots)) details.push('roots 必须是对象');
    for (const k of ROOT_KEY_ORDER) {
      if (typeof roots?.[k] !== 'string' || roots[k].length === 0) details.push(`roots.${k} 必须是绝对/相对路径字符串`);
    }
    if (!Array.isArray(manifest?.entries)) details.push('entries 必须是数组');
    if (entries.length === 0) details.push('entries 不得为空');

    for (const [i, e] of entries.entries()) {
      const at = `entries[${i}]${e?.id ? ` (${e.id})` : ''}`;
      for (const f of REQUIRED_FIELDS) {
        if (!(f in (e ?? {}))) details.push(`${at}: 缺必填字段 ${f}`);
      }
      if (typeof e?.id !== 'string' || !ID_RE.test(e.id)) details.push(`${at}: id 形态非法（应为 "域/名"）`);
      if (e?.id) {
        if (byId.has(e.id)) details.push(`${at}: id 重复（已被 entries[${byId.get(e.id)}] 占用）`);
        else byId.set(e.id, i);
      }
      if (!KINDS.includes(e?.kind)) details.push(`${at}: kind 非法 ${JSON.stringify(e?.kind)}`);
      if (!ROLES.includes(e?.role)) details.push(`${at}: role 非法 ${JSON.stringify(e?.role)}`);
      if (!STORAGES.includes(e?.storage)) details.push(`${at}: storage 非法 ${JSON.stringify(e?.storage)}`);
      if (typeof e?.readOnly !== 'boolean') details.push(`${at}: readOnly 必须是 boolean`);
      if (typeof e?.consume !== 'string' || e.consume.trim() === '') details.push(`${at}: consume 必须是非空字符串`);
      if (!(e?.dest === null || typeof e?.dest === 'string')) details.push(`${at}: dest 必须是 string | null`);
      if (Array.isArray(e?.dest)) details.push(`${at}: dest 必须是 string | null`);
      if (!Array.isArray(e?.origin) || e.origin.length === 0) {
        details.push(`${at}: origin 必须是**非空**数组`);
      } else {
        for (const [j, o] of e.origin.entries()) {
          const oat = `${at}.origin[${j}]`;
          if (typeof o?.root !== 'string') details.push(`${oat}: root 必须是字符串`);
          else if (!(o.root in roots)) details.push(`${oat}: root "${o.root}" 不在 roots 里`);
          if (typeof o?.path !== 'string' || o.path === '') details.push(`${oat}: path 必须是非空字符串`);
          if ('sha256' in (o ?? {}) && !SHA256_RE.test(o.sha256)) details.push(`${oat}: sha256 形态非法`);
        }
      }
      if ('derivedFrom' in (e ?? {})) {
        if (!Array.isArray(e.derivedFrom)) details.push(`${at}: derivedFrom 必须是数组`);
        else {
          for (const [j, d] of e.derivedFrom.entries()) {
            if (typeof d?.ref !== 'string' || d.ref === '') details.push(`${at}.derivedFrom[${j}]: ref 必须是非空字符串`);
          }
        }
      }
      if ('recipe' in (e ?? {}) && (typeof e.recipe !== 'string' || e.recipe === '')) {
        details.push(`${at}: recipe 必须是非空字符串`);
      }
      if ('blocks' in (e ?? {}) && !(Array.isArray(e.blocks) && e.blocks.every((b) => typeof b === 'string'))) {
        details.push(`${at}: blocks 必须是字符串数组`);
      }
      if ('note' in (e ?? {}) && typeof e.note !== 'string') details.push(`${at}: note 必须是字符串`);
    }
    add(1, 'schema：必填齐全 / 枚举合法 / id 全局唯一', details.length ? 'fail' : 'pass',
      details.length ? `${details.length} 处不合规` : `${entries.length} 个条目全部合规`, details);
  }

  // (2) 自洽：storage ⇔ dest ─────────────────────────────────
  {
    const details = [];
    for (const e of entries) {
      if (CARRYING.has(e?.storage)) {
        if (typeof e.dest !== 'string' || e.dest === '') {
          details.push(`${e.id}: storage=${e.storage} ⇒ dest 必须是非空路径`);
        } else if (path.isAbsolute(e.dest)) {
          details.push(`${e.id}: dest 必须是仓库内相对路径（含绝对路径）`);
        } else if (e.dest.split(/[\\/]/).includes('..')) {
          details.push(`${e.id}: dest 不得越出仓库（含 ..）`);
        }
      } else if (NOT_CARRYING.has(e?.storage)) {
        if (e.dest !== null) details.push(`${e.id}: storage=${e.storage} ⇒ dest 必须是 null，实际 ${JSON.stringify(e.dest)}`);
      }
    }
    add(2, '自洽：external-only/deferred ⇔ dest=null；lfs/git ⇔ dest 非空', details.length ? 'fail' : 'pass',
      details.length ? `${details.length} 处不自洽` : '全部自洽', details);
  }

  // (3) 存在性 ───────────────────────────────────────────────
  {
    const details = [];
    let warns = 0;
    for (const e of entries) {
      if (CARRYING.has(e?.storage) && typeof e.dest === 'string' && e.dest !== '') {
        const abs = path.resolve(repoRoot, e.dest);
        if (statKind(abs) === 'missing') details.push(`${e.id}: dest 在盘上不存在 → ${e.dest}`);
      }
      for (const o of e.origin ?? []) {
        const abs = resolveOrigin(repoRoot, roots, o);
        if (abs === null) continue; // (1) 已报
        if (statKind(abs) === 'missing') {
          // ★ staging 例外：它是用完即弃的中转，不是长期位置 ⇒ 只报 warning
          if (o.root === 'staging') {
            warns += 1;
            details.push(`warn: ${e.id}: origin 暂缺（root=staging，允许） → ${o.path}`);
          } else {
            details.push(`${e.id}: origin 在盘上不存在 → ${o.root}:${o.path}`);
          }
        }
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(3, '存在性：dest 与 origin 都能在盘上找到（staging 只 warning）',
      hard.length ? 'fail' : warns ? 'warn' : 'pass',
      hard.length ? `${hard.length} 处缺失` : warns ? `${warns} 处 staging 暂缺（允许）` : '全部存在', details);
  }

  // (4) 校验和纪律 ───────────────────────────────────────────
  {
    const details = [];
    for (const e of entries) {
      for (const o of e.origin ?? []) {
        const abs = resolveOrigin(repoRoot, roots, o);
        const kind = abs === null ? 'missing' : statKind(abs);
        const has = typeof o.sha256 === 'string';
        if (CARRYING.has(e.storage)) {
          if (has) details.push(`${e.id}: 入库件不得写 origin[].sha256（校验和交给 git/LFS） → ${o.path}`);
          continue;
        }
        if (kind === 'dir') {
          if (has) details.push(`${e.id}: 目录型 origin 不得写 sha256（不可稳定复现） → ${o.path}`);
          continue;
        }
        if (kind === 'file') {
          if (!has) {
            details.push(`${e.id}: 不入库的文件型 origin 必须写 sha256 → ${o.root}:${o.path}（用 --scan --write 补）`);
          } else if (checkHashes) {
            const actual = sha256File(abs);
            if (actual !== o.sha256) details.push(`${e.id}: sha256 与盘上不符 → ${o.path}（清单 ${o.sha256.slice(0, 12)}… / 实际 ${actual.slice(0, 12)}…）`);
          }
        }
      }
    }
    add(4, '校验和纪律：入库件不写 sha256；不入库的文件件必须写且与盘上一致',
      details.length ? 'fail' : 'pass', details.length ? `${details.length} 处违规` : '全部合规', details);
  }

  // (5) 忽略一致性 ───────────────────────────────────────────
  {
    const details = [];
    if (!useGit) details.push('warn: --no-git：跳过');
    else {
      for (const e of entries) {
        if (e.storage !== 'external-only' || typeof e.dest !== 'string' || e.dest === '') continue;
        const r = gitCheckIgnore(repoRoot, e.dest);
        if (!r.ignored) details.push(`${e.id}: external-only 且 dest 在仓内 ⇒ 必须被 .gitignore 命中 → ${e.dest}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(5, '忽略一致性：external-only 且 dest 在仓内 ⇒ 必须被 .gitignore 命中',
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处未命中` : '无此形态条目 / 全部命中', details);
  }

  // (6) LFS 一致性 ───────────────────────────────────────────
  {
    const details = [];
    let checked = 0;
    if (!useGit) details.push('warn: --no-git：跳过');
    else {
      for (const e of entries) {
        if (e.storage !== 'lfs') continue;
        checked += 1;
        const r = gitAttrFilter(repoRoot, e.dest);
        if (r.value !== 'lfs') details.push(`${e.id}: storage=lfs ⇒ git check-attr filter 必须是 lfs，实际 "${r.value}" → ${e.dest}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(6, 'LFS 一致性：storage=lfs ⇒ git check-attr filter = lfs',
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处不是 lfs` : `${checked} 个 lfs 条目已核对`, details);
  }

  // (7) 语料保真（★ 只对"真正入库的那一份"强制）──────────────
  {
    const details = [];
    let ran = 0;
    for (const e of entries) {
      if (e.kind !== 'disasm-corpus' || !CARRYING.has(e.storage)) continue;
      if (typeof e.recipe !== 'string' || e.recipe === '') {
        details.push(`${e.id}: 入库的 disasm-corpus 必须写 recipe（转码/加工脚本）`);
        continue;
      }
      const recipeAbs = path.resolve(repoRoot, e.recipe);
      if (statKind(recipeAbs) !== 'file') {
        details.push(`${e.id}: recipe 不存在 → ${e.recipe}`);
        continue;
      }
      if (!useRecipe) {
        details.push(`warn: ${e.id}: --no-recipe：未跑断言`);
        continue;
      }
      try {
        execFileSync(process.execPath, [recipeAbs, '--verify'], { cwd: repoRoot, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
        ran += 1;
      } catch (err) {
        const tail = `${err.stdout ?? ''}${err.stderr ?? ''}`.trim().split('\n').slice(-6).join('\n      ');
        details.push(`${e.id}: recipe 断言未通过（${e.recipe} --verify 退出 ${err.status ?? '?'}）\n      ${tail}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(7, '语料保真：入库的 disasm-corpus 必须有 recipe 且断言通过（豁免参照件/清单）',
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处未通过` : ran ? `${ran} 个条目断言通过` : '本轮无入库语料（bundle 为 deferred）', details);
  }

  // (8) 知识准入门 ───────────────────────────────────────────
  {
    const details = [];
    for (const e of entries) {
      if (e.kind === 'knowledge-source' && !NOT_CARRYING.has(e.storage)) {
        details.push(`${e.id}: kind=knowledge-source ⇒ storage 只能是 external-only/deferred（K3 通过前不得入库），实际 ${e.storage}`);
      }
    }
    add(8, '知识准入门：knowledge-source 只能是 external-only/deferred',
      details.length ? 'fail' : 'pass', details.length ? `${details.length} 处违规` : '全部合规', details);
  }

  // (9) 真前身可解析（★ 同样只对"真正入库的那一份"强制）──────
  {
    const details = [];
    for (const e of entries) {
      const refs = Array.isArray(e.derivedFrom) ? e.derivedFrom : [];
      for (const d of refs) {
        if (!byId.has(d.ref)) details.push(`${e.id}: derivedFrom.ref "${d.ref}" 不是已存在的 id`);
      }
      if (e.kind === 'disasm-corpus' && CARRYING.has(e.storage)) {
        if (refs.length === 0) {
          details.push(`${e.id}: 入库的 disasm-corpus 必须有 derivedFrom（真前身）`);
        } else {
          const ok = refs.some((d) => entries[byId.get(d.ref)]?.kind === 'binary');
          if (!ok) details.push(`${e.id}: 入库的 disasm-corpus 的 derivedFrom 里必须有一条指向 kind=binary`);
        }
      }
    }
    add(9, '真前身可解析：ref 必须存在；入库语料必须指到 kind=binary（豁免参照件/清单）',
      details.length ? 'fail' : 'pass', details.length ? `${details.length} 处不可解析` : '全部可解析', details);
  }

  const failures = checks.filter((c) => c.status === 'fail').length;
  const warnings = checks.filter((c) => c.status === 'warn').length;
  return { checks, failures, warnings };
}

// ─────────────────────────────────────────────────────────── 写入

function writeAtomically(target, text) {
  const tmp = `${target}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, text, 'utf8');
  fs.renameSync(tmp, target);
}

/**
 * 落盘 + 回读 + 复验；不绿则回滚。
 * @returns {{ok:boolean, restored?:boolean, failures?:number}}
 */
export function saveManifest(manifest, manifestPath, opts = {}) {
  const backup = fs.readFileSync(manifestPath, 'utf8');
  writeAtomically(manifestPath, canonicalStringify(manifest));
  let reread;
  try {
    reread = loadManifest(manifestPath);
  } catch (err) {
    fs.writeFileSync(manifestPath, backup, 'utf8');
    return { ok: false, restored: true, reason: `回读失败：${err.message}` };
  }
  const { failures, checks } = validateManifest(reread, opts);
  if (failures > 0) {
    fs.writeFileSync(manifestPath, backup, 'utf8');
    const bad = checks.filter((c) => c.status === 'fail').map((c) => `#${c.id} ${c.title}：${c.message}`);
    return { ok: false, restored: true, failures, reason: `写后复验未通过（已回滚）：\n  - ${bad.join('\n  - ')}` };
  }
  return { ok: true, failures: 0 };
}

// ─────────────────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, noHash: false, noGit: false, noRecipe: false, manifest: DEFAULT_MANIFEST, rest: [] };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--validate' || a === '--scan' || a === '--list') out.action = a.slice(2);
    else if (a === '--add') { out.action = 'add'; out.rest.push(argv[++i]); }
    else if (a === '--set') { out.action = 'set'; out.rest.push(argv[++i], argv[++i]); }
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
  process.stdout.write(`已更新 ${id} 并回读复验通过。\n`);
  return 0;
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
  node tools/corpus.mjs --list [--json]
  node tools/corpus.mjs --manifest <path>          # 换一份清单
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.action === 'help') {
    process.stdout.write(HELP);
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
    case 'list':
      return cmdList(manifest, args);
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
