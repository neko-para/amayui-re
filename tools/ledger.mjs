#!/usr/bin/env node
/**
 * tools/ledger.mjs —— **CLI**：知识台账（`data/ledger/`）的追加 / 查询 / 校验 / 派生
 *
 * 经派发器：`pnpm tools ledger <status|list|show|add|retract|validate|rebuild-db|compact|describe> [args…]`
 * 也可独立跑：`node tools/ledger.mjs --validate`
 *
 * 模型在 `lib/ledger.mjs`（schema / 投影 / 锚点解析 / 不变量 / DB 构建都在那里）；
 * 本文件只做"参数 → 模型 → 输出 + 落盘计划"，**不实现任何规则**。
 *
 * ★ 三条纪律：
 *   1. 缺省 **dry-run**，`--write` 才落盘；写后回读复验，不绿回滚（复用模型里的 `appendRecord`）。
 *   2. 本文件**不捕获子进程输出**（受限沙箱里 `stdio:'pipe'` 会 EPERM）—— 读跟踪文件用
 *      `execFileSync(..., {stdio:['ignore','pipe','ignore']})`，只取 stdout 一次。
 *   3. **不硬编码旧仓路径**：只读参考仓的根从 `corpus/assets.json` 的 `roots` 解析。
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

import {
  CHECK_TITLES,
  DEFAULT_CACHE_DIR,
  DEFAULT_DB_NAME,
  DEFAULT_LEDGER_DIR,
  DEFAULT_MANIFEST,
  DOMAIN,
  EFFECTIVE,
  ID_PREFIX,
  KINDS,
  OPERATIONS,
  REFERENCE_ROOT_KEY,
  ULID_RE,
  anchorText,
  appendRecord,
  describe,
  describeText,
  isoNow,
  listFiles,
  monthOf,
  newUlid,
  project,
  readRecords,
  rebuildDb,
  rebuildTwiceDigest,
  resolveAnchor,
  resolveDomain,
  serializeRecord,
  validateAll,
} from './lib/ledger.mjs';

export { DOMAIN, OPERATIONS, describe, describeText };

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '..');

const HELP = `tools/ledger.mjs —— 知识台账（append-only 文本真源 + 派生只读 SQLite）

  node tools/ledger.mjs --describe                 # ★ schema 唯一真源：字段 / 枚举 / 不变量 / 锚点与分类轴口径
  node tools/ledger.mjs --report                   # 体检：各 kind 条数 / 有效状态 / 域分布 / 锚点可解析率 / 冲突数
  node tools/ledger.mjs --domains                  # ★ 域词汇表：当前域 / 别名链 / 被拆分的值 + 逐值解析表
  node tools/ledger.mjs --list [--only-kind claim] [--only-effective stale] [--system <域>] [--subject X] [--json]
  node tools/ledger.mjs --show <id|唯一前缀>
  node tools/ledger.mjs --add --kind <k> --system <域> --subject <s> --claim <c> --anchor <json>… [--status …] [--write]
  node tools/ledger.mjs --add --kind domain --system <父域> --subject <域名> --disposition added --claim … --anchor … --write
  node tools/ledger.mjs --retract <id> --claim <理由> [--write]     # ★ 追加一条 replaces 它的记录
  node tools/ledger.mjs --validate [--json]                        # 7 条不变量；红 = 退出码 1
  node tools/ledger.mjs --rebuild-db [--write] [--db <路径>]        # 缺省 dry-run；判据 = 逻辑内容两次相同
  node tools/ledger.mjs --compact [--write]                        # 分片归位 + 同 id 去重（不删任何结论）

★ 分类轴：\`system\`（**必填字段**，不是标签）＝这条结论关于引擎的哪一块；
  **域词汇表 = 台账里 \`kind=domain\` 的记录**（\`disposition\` ∈ added/renamed/merged/split）——
  改名/归并靠 \`aliases\` 让历史值**仍可解析**（历史行一个字节都不用动）；
  **拆分**别名救不了 ⇒ 必须追加更正记录（不许靠词表悄悄改结论的含义）。
★ 概览那个动作叫 **--report**，不叫 --status —— 因为 **--status 是字段名**（--add … --status accepted）。
  动作名与字段名撞车会让参数被静默吃掉（真踩过），所以两者不许同名。
★ 日志是 **append-only**：不许手改 .jsonl、不许删行；"撤回"是**追加**一条 replaces 它的记录。
★ 锚点两种形态、都不是行号：\`bin\`（二进制 EA）与 \`guard\`（可执行守卫用例 \`路径#用例名片段\`）；
  \`repo\` 决定去哪找：\`self\`=本仓，\`reference\`=只读参考仓（旧仓，不在场时只 warn）。
`;

/**
 * 动作名与**字段名**共用 `--x` 前缀，所以**不许有歧义**：
 *   ★ `status` 是**字段**（`--add … --status accepted`），所以概览那个动作叫 **`report`** —— 不叫 `--status`。
 *     早先版本用 `--status` 当动作名，于是 `--add … --status accepted` 把 `--status` 吃成动作、
 *     `accepted` 变成多余位置参数，**静默变成"打印概览"**（真踩过：`--write` 被忽略、什么都没写）。
 *   ★ 同理 `list` 的筛选用 `--only-kind` / `--only-effective`，不叫 `--kind` / `--effective`（那是 add 的字段）。
 */
const ACTIONS = ['describe', 'report', 'list', 'domains', 'show', 'add', 'retract', 'validate', 'rebuild-db', 'compact', 'help'];

function parseArgs(argv) {
  const out = { action: null, write: false, json: false, anchors: [], rest: [] };
  const takesValue = new Set([
    'kind', 'system', 'subject', 'claim', 'status', 'note', 'anchor', 'db', 'tz', 'replaces', 'why', 'dir', 'id', 'at',
    'disposition', 'aliases', 'split-into',
    'only-kind', 'only-effective',
  ]);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--help' || a === '-h') out.action = 'help';
    else if (a.startsWith('--') && ACTIONS.includes(a.slice(2))) out.action = a.slice(2);
    else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a.startsWith('--')) {
      const k = a.slice(2);
      if (!takesValue.has(k)) throw new Error(`不认识的选项：${a}`);
      const v = argv[++i];
      if (v === undefined) throw new Error(`${a} 需要一个值`);
      if (k === 'anchor') out.anchors.push(v);
      else out[k] = v;
    } else out.rest.push(a);
  }
  if (!out.action) out.action = 'report';
  return out;
}

/** 只读参考仓的根：**从清单解析**（`roots.oldRepo`），不硬编码 */
function referenceRoot(manifestPath = path.join(REPO_ROOT, DEFAULT_MANIFEST)) {
  try {
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    const root = manifest.roots?.[REFERENCE_ROOT_KEY];
    return typeof root === 'string' ? root : null;
  } catch {
    return null;
  }
}

/** 本仓**已跟踪**的文件集合（口径：git 知道它 = 它可为锚点作证）。不在 git 仓库里返回 null（不检查） */
function trackedFiles() {
  try {
    const out = execFileSync('git', ['ls-files', '-z'], {
      cwd: REPO_ROOT,
      encoding: 'utf8',
      maxBuffer: 64 * 1024 * 1024,
      stdio: ['ignore', 'pipe', 'ignore'],
    });
    return new Set(out.split('\0').filter(Boolean).map((p) => p.split('\\').join('/')));
  } catch {
    return null;
  }
}

const ledgerDirOf = (args) => path.resolve(args.dir ?? path.join(REPO_ROOT, DEFAULT_LEDGER_DIR));
const dbPathOf = (args) => path.resolve(args.db ?? path.join(REPO_ROOT, DEFAULT_CACHE_DIR, DEFAULT_DB_NAME));

function loadContext(args) {
  const ledgerDir = ledgerDirOf(args);
  const { records, problems } = readRecords(ledgerDir);
  const opts = {
    repoRoot: REPO_ROOT,
    referenceRoot: referenceRoot(),
    ledgerDir,
    tracked: trackedFiles(),
    parsedProblems: problems,
  };
  return { ledgerDir, records, problems, opts };
}

function printReport(report, json) {
  if (json) return JSON.stringify({ checks: report.checks, failures: report.failures }, null, 2);
  const L = [];
  for (const c of report.checks) {
    L.push(`[${c.problems.length ? 'FAIL' : 'ok  '}] #${c.id} ${c.text}`);
    for (const p of c.problems) L.push(`        · ${p.file ? `${path.relative(REPO_ROOT, p.file).split(path.sep).join('/')}${p.line ? `:${p.line}` : ''} ` : ''}${p.reason}`);
  }
  L.push('', `${report.checks.length} 条不变量：${report.checks.length - report.failures} 通过 / ${report.failures} 失败`);
  return L.join('\n');
}

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

  const ctx = loadContext(args);
  const { ledgerDir, records, opts } = ctx;
  const proj = project(records, opts);

  if (args.action === 'report') return cmdReport(args, proj, ledgerDir);
  if (args.action === 'list') return cmdList(args, proj);
  if (args.action === 'domains') return cmdDomains(args, proj, ledgerDir);
  if (args.action === 'show') return cmdShow(args, proj);
  if (args.action === 'add') return cmdAdd(args, proj, ledgerDir, opts);
  if (args.action === 'retract') return cmdRetract(args, proj, ledgerDir);
  if (args.action === 'compact') return cmdCompact(args, records, ledgerDir);
  if (args.action === 'rebuild-db') return cmdRebuildDb(args, records, opts);
  if (args.action === 'validate') return cmdValidate(args, records, opts);
  process.stderr.write(`未知动作：${args.action}\n${HELP}`);
  return 2;
}

// ─────────────────────────────────────────────────────────── 动作

function cmdReport(args, proj, ledgerDir) {
  const byKind = {};
  for (const k of KINDS) byKind[k] = 0;
  for (const e of proj.entries) byKind[e.kind] = (byKind[e.kind] ?? 0) + 1;
  const anchors = proj.entries.flatMap((e) => e.anchors);
  const ok = anchors.filter((a) => a.ok).length;
  const warn = anchors.filter((a) => a.kind === 'warning').length;
  const err = anchors.filter((a) => a.kind === 'error').length;
  const files = listFiles(ledgerDir);
  if (args.json) {
    process.stdout.write(
      `${JSON.stringify({ files: files.length, records: proj.entries.length, byKind, byEffective: proj.byEffective, bySystem: proj.bySystem, domains: proj.vocab.canonical.size, anchors: { total: anchors.length, ok, warn, err }, conflicts: proj.conflicts.length }, null, 2)}\n`,
    );
    return 0;
  }
  const L = [];
  L.push(`文件        ${files.length} 个分片（kind/月）`);
  L.push(`记录        ${proj.entries.length} 条　${KINDS.map((k) => `${k}=${byKind[k]}`).join(' · ')}`);
  L.push(`有效状态    ${EFFECTIVE.map((s) => `${s}=${proj.byEffective[s] ?? 0}`).join(' · ')}`);
  L.push(`域          ${proj.vocab.canonical.size} 个当前域${proj.vocab.aliasOf.size ? ` · ${[...proj.vocab.aliasOf].filter(([a, b]) => a !== b).length} 条别名` : ''}${proj.vocab.splits.size ? ` · ${proj.vocab.splits.size} 个被拆分` : ''}　⇒ \`pnpm tools ledger domains\``);
  L.push(`域分布      ${Object.entries(proj.bySystem).map(([k, v]) => `${k}=${v}`).join(' · ') || '（无）'}`);
  L.push(`锚点        ${anchors.length} 条　可解析=${ok} · 参考仓不在场=${warn} · **红**=${err}`);
  L.push(`冲突        ${proj.conflicts.length} 组${proj.conflicts.length ? '　⇒ 见 `--validate` 的 #5' : ''}`);
  L.push('');
  L.push(proj.entries.length === 0 ? '（台账是空的 —— 这是**有意为之**：K3 通过前任何知识条目不得进来）' : '体检口径：`--validate` 是门禁，本命令只是概览。');
  process.stdout.write(`${L.join('\n')}\n`);
  return 0;
}

function cmdDomains(args, proj, ledgerDir) {
  const v = proj.vocab;
  const all = [...proj.entries];
  if (args.json) {
    process.stdout.write(
      `${JSON.stringify({
        canonical: [...v.canonical].sort(),
        aliasOf: Object.fromEntries([...v.aliasOf].sort()),
        splits: Object.fromEntries([...v.splits].sort()),
        records: v.records.length,
        resolution: all
          .map((e) => ({ value: e.system.value, canonical: e.system.canonical, via: e.system.via, chain: e.system.chain }))
          .filter((r, i, arr) => arr.findIndex((x) => x.value === r.value) === i)
          .sort((a, b) => a.value.localeCompare(b.value)),
        problems: v.problems,
      }, null, 2)}\n`,
    );
    return 0;
  }
  const L = [];
  L.push(`域记录      ${v.records.length} 条（kind=domain）`);
  L.push(`当前域      ${v.canonical.size} 个${v.canonical.size ? `　${[...v.canonical].sort().join(' · ')}` : '　（空词表 —— 见下）'}`);
  if (v.aliasOf.size) L.push(`别名        ${v.aliasOf.size} 条（历史值 → 当前域，单向）`);
  for (const [a, c] of [...v.aliasOf].sort()) if (a !== c) L.push(`              ${a} → ${c}`);
  if (v.splits.size) {
    L.push(`被拆分      ${v.splits.size} 个（★ 别名不覆盖它们：必须走追加更正记录）`);
    for (const [k, into] of [...v.splits].sort()) L.push(`              ${k} → ${into.join(' / ')}`);
  }
  for (const x of v.problems) L.push(`词表歧义    ${x.reason}`);

  // 逐值解析表：把"记录里出现过的值"与"词表登记过的历史值"都列出来
  const values = new Set([...all.map((e) => e.system.value), ...v.aliasOf.keys(), ...v.splits.keys()]);
  L.push('');
  if (v.canonical.size === 0 && v.records.length === 0) {
    L.push('（词表是空的 —— 这是**有意为之**：归一到哪些域是 K1 的活，本节点只落机制）');
    L.push('  怎么填：追加一条 kind=domain 的记录 ——');
    L.push('    pnpm tools ledger add --kind domain --system <父域> --subject <域名> \\');
    L.push('      --disposition added --claim <一句话> --anchor <json> --write');
    L.push('  ★ 而且它**必须带锚**（"域怎么分"是知识，不是配置）。');
  } else if (values.size === 0) {
    L.push('（还没有任何记录引用过域名 ⇒ 没有可解析的值）');
  } else {
    L.push(`${'值'.padEnd(24)} ${'有效域'.padEnd(20)} 依据`);
    for (const value of [...values].sort()) {
      const r = resolveDomain(value, v);
      const canon = r.via === 'split' ? `(已拆分：${(r.splitInto ?? []).join('/')})` : r.canonical ?? '(未在词表中：待裁决)';
      L.push(`${value.padEnd(24)} ${canon.padEnd(20)} ${r.via}${r.chain.length > 1 ? `　链：${r.chain.join(' → ')}` : ''}`);
    }
  }
  process.stdout.write(`${L.join('\n')}\n`);
  return 0;
}

function cmdList(args, proj) {
  let rows = proj.entries;
  // ★ 筛选用 `--only-*`：`--kind` / `--effective` 是 add/list 的**字段名**（见 parseArgs 的注释）
  if (args['only-kind']) rows = rows.filter((e) => e.kind === args['only-kind']);
  if (args['only-effective']) rows = rows.filter((e) => e.effective === args['only-effective']);
  if (args.subject) rows = rows.filter((e) => e.subject === args.subject);
  if (args.status) rows = rows.filter((e) => (e.status ?? 'proposed') === args.status);
  // ★ `--system` 走**别名解析**：给历史值也能查到（例：给 `声音` 能查到已归一到 `音频` 的记录）
  if (args.system) {
    const want = proj.vocab.canonical.has(args.system) || proj.vocab.aliasOf.has(args.system)
      ? resolveDomain(args.system, proj.vocab).canonical
      : args.system;
    rows = rows.filter((e) => (e.system.via === 'split' || e.system.via === 'unknown' ? e.system.value === args.system : e.system.canonical === want));
  }
  rows = [...rows].sort((a, b) => String(a.id).localeCompare(String(b.id)));
  if (args.json) {
    process.stdout.write(`${JSON.stringify(rows.map((e) => ({ id: e.id, at: e.at, kind: e.kind, system: e.system.value, systemCurrent: e.system.canonical, systemVia: e.system.via, subject: e.subject, status: e.status ?? 'proposed', effective: e.effective, claim: e.claim, anchors: e.anchors.map((a) => ({ ...a.anchor, resolved: a.ok ? 'ok' : a.kind })) })), null, 2)}\n`);
    return 0;
  }
  if (rows.length === 0) {
    process.stdout.write('（没有匹配的记录）\n');
    return 0;
  }
  const L = [`${'id'.padEnd(30)} ${'kind'.padEnd(12)} ${'system'.padEnd(14)} ${'eff'.padEnd(10)} subject`];
  for (const e of rows) L.push(`${e.id.padEnd(30)} ${e.kind.padEnd(12)} ${String(e.system.canonical ?? e.system.value).padEnd(14)} ${e.effective.padEnd(10)} ${e.subject}`);
  L.push('', `${rows.length} 条`);
  process.stdout.write(`${L.join('\n')}\n`);
  return 0;
}

function cmdShow(args, proj) {
  const ref = args.rest[0];
  if (!ref) throw new Error('--show 需要 <id 或唯一前缀>');
  const hits = proj.entries.filter((e) => e.id === ref || e.id === `${ID_PREFIX}${ref}` || e.id.endsWith(ref) || e.id.includes(ref));
  if (hits.length === 0) throw new Error(`找不到记录：${ref}`);
  if (hits.length > 1) throw new Error(`前缀不唯一（${hits.length} 条）：${hits.map((e) => e.id).join(', ')}`);
  const e = hits[0];
  const L = [serializeRecord(e)];
  L.push('');
  L.push(`文件        ${path.relative(REPO_ROOT, e._file).split(path.sep).join('/')}:${e._line}`);
  L.push(`有效状态    ${e.effective}（声称 ${e.status ?? 'proposed'}）`);
  L.push('锚点：');
  for (const a of e.anchors) L.push(`  ${a.ok ? '✓' : a.kind === 'warning' ? '·' : '✗'} ${anchorText(a.anchor)}　${a.why}`);
  if (e.conflictWith.length) L.push(`冲突对家    ${e.conflictWith.join(', ')}`);
  process.stdout.write(`${L.join('\n')}\n`);
  return 0;
}

function parseAnchorSpec(spec) {
  let a;
  try {
    a = JSON.parse(spec);
  } catch (err) {
    throw new Error(`--anchor 必须是 JSON（例：'{"type":"guard","repo":"self","path":"tools/test/ledger.test.mjs","test":"#1 形态"}'）：${err.message}`);
  }
  return a;
}

function cmdAdd(args, proj, ledgerDir, opts) {
  const now = Date.now();
  const id = args.id ?? newUlid(now);
  const rec = {
    id: id.startsWith(ID_PREFIX) ? id.slice(ID_PREFIX.length) : id,
    at: args.at ?? isoNow(now),
    kind: args.kind,
    // ★ 分类轴：`--system` 必填（词表为空时也不能省 —— 见 describe 的"分类轴"一节）
    system: args.system,
    subject: args.subject,
    claim: args.claim,
    anchor: args.anchors.map(parseAnchorSpec),
  };
  if (args.status !== undefined) rec.status = args.status;
  if (args.note !== undefined) rec.note = args.note;
  if (args.replaces !== undefined) rec.replaces = args.replaces.startsWith(ID_PREFIX) ? args.replaces.slice(ID_PREFIX.length) : args.replaces;
  // 域记录专属（`--aliases a,b` / `--split-into x,y`）
  if (args.disposition !== undefined) rec.disposition = args.disposition;
  if (args.aliases !== undefined) rec.aliases = String(args.aliases).split(',').map((s) => s.trim()).filter(Boolean);
  if (args['split-into'] !== undefined) rec.splitInto = String(args['split-into']).split(',').map((s) => s.trim()).filter(Boolean);

  if (!ULID_RE.test(rec.id)) throw new Error(`id 非法（应是 26 字符 ULID；缺省自动生成）：${rec.id}`);
  if (proj.entries.some((e) => e.id === rec.id)) throw new Error(`id 已存在：${ID_PREFIX}${rec.id}（台账是 append-only，要改就追加一条 replaces 它的记录）`);

  // ★ 落盘前先自校验：拿"现有 + 这条"跑不变量，挑出**由这条记录引起**的问题。
  //   顺序要紧：**先校验、后序列化** —— serializeRecord 对非法锚点 type 是抛错的（它不该悄悄改写数据）。
  const trial = { ...rec, _file: path.join(ledgerDir, rec.kind ?? '?', `${monthOf(rec.id)}.jsonl`), _line: 0, _fileKind: rec.kind };
  const report = validateAll([...proj.entries, trial], { ...opts, ledgerDir, parsedProblems: [] });
  const own = [
    ...(report.checks.find((c) => c.id === 1)?.problems ?? []),
    ...(report.checks.find((c) => c.id === 3)?.problems ?? []),
    ...(report.checks.find((c) => c.id === 7)?.problems ?? []),
  ].filter((p) => p.line === 0 && !/id 与 .* 重复/.test(p.reason))
    // ★ **引导（bootstrap）**：往空词表里加**第一条**域记录时，它自己的 `system`（父域）当然还不在词表里 ——
    //   那是正常的，不是错。除此之外任何"值追不到词表"都必须拦住。
    .filter((p) => !(rec.kind === 'domain' && /不在域词汇表里/.test(p.reason)));
  if (own.length) {
    process.stderr.write(`✗ 自校验不通过，不写（${ID_PREFIX}${rec.id}）：\n  - ${own.map((p) => p.reason).join('\n  - ')}\n`);
    return 1;
  }
  const L = [`待追加（dry-run 形态）：`, serializeRecord(rec), ''];
  const target = path.join(ledgerDir, rec.kind ?? '?', `${monthOf(rec.id)}.jsonl`);
  L.push(`落点        ${path.relative(REPO_ROOT, target).split(path.sep).join('/')}`);
  const anchors = rec.anchor.map((a) => ({ anchor: a, ...resolveAnchor(a, opts) }));
  for (const a of anchors) L.push(`锚点        ${a.ok ? '✓' : a.kind === 'warning' ? '·' : '✗'} ${anchorText(a.anchor)}　${a.why}`);

  if (!args.write) {
    process.stdout.write(`${L.join('\n')}\n\n（dry-run）加 --write 落盘。\n`);
    return anchors.some((a) => a.kind === 'error') ? 1 : 0;
  }
  if (anchors.some((a) => a.kind === 'error')) {
    process.stderr.write(`${L.join('\n')}\n\n✗ 有锚点解析不了（error）⇒ 不写。\n（确实是"将来才有的观察"就先写 status=proposed，或用 repo=reference 指向旧仓。）\n`);
    return 1;
  }
  appendRecord(ledgerDir, rec);
  process.stdout.write(`${L.join('\n')}\n\n已追加      ${ID_PREFIX}${rec.id}\n`);
  return 0;
}

function cmdRetract(args, proj, ledgerDir) {
  const ref = args.rest[0];
  if (!ref) throw new Error('--retract 需要 <id 或唯一前缀> 与 --claim（撤回理由）');
  const hits = proj.entries.filter((e) => e.id === ref || e.id === `${ID_PREFIX}${ref}` || e.id.endsWith(ref));
  if (hits.length !== 1) throw new Error(hits.length === 0 ? `找不到记录：${ref}` : `前缀不唯一：${hits.map((e) => e.id).join(', ')}`);
  const target = hits[0];
  const why = args.claim ?? args.why;
  if (!why) throw new Error('撤回必须写理由（--claim 或 --why）—— 禁静默关单');
  const now = Date.now();
  const rec = {
    id: (args.id ?? newUlid(now)).replace(ID_PREFIX, ''),
    at: isoNow(now),
    kind: target.kind,
    subject: target.subject,
    claim: `撤回：${why}`,
    anchor: target.anchor.map((a) => ({ ...a })),
    status: 'retracted',
    replaces: target.id,
  };
  const L = [`将**追加**（不改历史）：`, serializeRecord(rec), '', `它 replaces ${ID_PREFIX}${target.id}`];
  if (!args.write) {
    process.stdout.write(`${L.join('\n')}\n\n（dry-run）加 --write 落盘。\n`);
    return 0;
  }
  appendRecord(ledgerDir, rec);
  process.stdout.write(`${L.join('\n')}\n\n已追加      ${ID_PREFIX}${rec.id}\n`);
  return 0;
}

function cmdValidate(args, records, opts) {
  // 先把派生 DB 也真建一遍（#6 的证据），建到临时区，不碰 .cache/
  const tmp = path.join(REPO_ROOT, '.tmp', `ledger-validate-${process.pid}`);
  let dbProblems = [];
  let dbProblemsFile = '';
  try {
    const twice = rebuildTwiceDigest(records, tmp, opts);
    if (!twice.a.ok) dbProblems = [{ reason: twice.a.reason }];
    else if (!twice.same) {
      dbProblemsFile = twice.a.dbPath;
      dbProblems = [{ reason: `两次重建的**逻辑内容**不同（${twice.a.digest} ≠ ${twice.b.digest}）⇒ 派生不是确定性的` }];
    }
  } catch (err) {
    dbProblems = [{ reason: `重建派生 DB 时抛错：${err.message}` }];
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  const report = validateAll(records, { ...opts, dbProblems, dbProblemsFile });
  process.stdout.write(`${printReport(report, args.json)}\n`);
  return report.failures > 0 ? 1 : 0;
}

function cmdRebuildDb(args, records, opts) {
  const dbPath = dbPathOf(args);
  const tmp = path.join(REPO_ROOT, '.tmp', `ledger-rebuild-${process.pid}`);
  const twice = rebuildTwiceDigest(records, tmp, opts);
  fs.rmSync(tmp, { recursive: true, force: true });
  if (!twice.a.ok) {
    process.stderr.write(`重建失败：${twice.a.reason}\n（node:sqlite 是 Node 内置模块；请升 Node，或不要指望"没有查询层"能静默过去。）\n`);
    return 2;
  }
  const L = [
    `记录        ${twice.a.records} 条 · 锚点 ${twice.a.anchors} 条 · 冲突 ${twice.a.conflicts} 组`,
    `逻辑摘要    ${twice.a.digest}`,
    `确定性      两次重建的逻辑内容 ${twice.same ? '相同 ✓' : `**不同 ✗**（${twice.b.digest}）`}`,
    `落点        ${path.relative(REPO_ROOT, dbPath).split(path.sep).join('/')}`,
  ];
  if (!twice.same) {
    process.stderr.write(`${L.join('\n')}\n\n✗ 不是确定性的 ⇒ 不落盘。\n`);
    return 1;
  }
  if (!args.write) {
    process.stdout.write(`${L.join('\n')}\n\n（dry-run）加 --write 落盘。删掉本地 DB 后跑同样这条命令即可重建。\n`);
    return 0;
  }
  const res = rebuildDb(records, dbPath, opts);
  if (!res.ok) {
    process.stderr.write(`落盘失败：${res.reason}\n`);
    return 1;
  }
  process.stdout.write(`${L.join('\n')}\n\n已重建      ${dbPath}\n`);
  return 0;
}

function cmdCompact(args, records, ledgerDir) {
  // 目标形态：每条记录落在 `<kind>/<ULID 推出的月>.jsonl`，文件内按 id 严格递增，且**同 id 去重**（保留最新 at）
  const wanted = new Map();
  for (const r of records) {
    if (typeof r.id !== 'string' || !ULID_RE.test(r.id)) continue;
    const prev = wanted.get(r.id);
    if (!prev || String(r.at ?? '') >= String(prev.at ?? '')) wanted.set(r.id, r);
  }
  const plan = new Map(); // 理想文件 → 行
  for (const r of wanted.values()) {
    const f = path.join(ledgerDir, r.kind, `${monthOf(r.id)}.jsonl`);
    if (!plan.has(f)) plan.set(f, []);
    plan.get(f).push(r);
  }
  for (const rows of plan.values()) rows.sort((a, b) => String(a.id).localeCompare(String(b.id)));

  const L = ['分片归位（分片规则：kind + ULID 推出的月；文件内按 ULID 递增；同 id 去重保留最新 at）'];
  let moved = 0;
  let dropped = records.length - wanted.size;
  for (const [f, rows] of [...plan].sort((a, b) => a[0].localeCompare(b[0]))) {
    const want = `${rows.map(serializeRecord).join('\n')}\n`;
    const have = fs.existsSync(f) ? fs.readFileSync(f, 'utf8') : '';
    const same = have === want;
    if (!same) moved += 1;
    L.push(`  ${same ? '=' : '~'} ${path.relative(REPO_ROOT, f).split(path.sep).join('/')}　${rows.length} 条${same ? '' : '（需重写）'}`);
  }
  // 空掉的旧文件
  const existing = listFiles(ledgerDir).map((x) => x.file);
  for (const f of existing) if (!plan.has(f)) L.push(`  - ${path.relative(REPO_ROOT, f).split(path.sep).join('/')}　将删除（内容已归位）`);
  L.push('', `需要重写的分片 ${moved} 个 · 重复 id 去掉 ${dropped} 条（保留最新 at）`);
  if (!args.write) {
    process.stdout.write(`${L.join('\n')}\n\n（dry-run）加 --write 落盘。★ 只为"合并两个设备各自追加的日志"用，不删任何结论。\n`);
    return 0;
  }
  for (const f of existing) if (!plan.has(f)) fs.rmSync(f);
  for (const [f, rows] of plan) {
    fs.mkdirSync(path.dirname(f), { recursive: true });
    fs.writeFileSync(f, `${rows.map(serializeRecord).join('\n')}\n`);
  }
  process.stdout.write(`${L.join('\n')}\n\n已归位。\n`);
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  try {
    process.exitCode = main();
  } catch (err) {
    process.stderr.write(`ERROR: ${err.message}\n${err.stack}\n`);
    process.exitCode = 2;
  }
}
