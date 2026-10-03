#!/usr/bin/env node
/**
 * tools/old-repo-inventory.mjs — 旧仓盘点快照（**脚本重新实测**，绝不手抄）
 *
 * 产物：`docs/00-origin/old-repo-inventory.md` —— 给后续迁移轮按图索骥的定位基线。
 * 纪律：
 *   · **只读**：对旧仓只跑只读 git 命令与 fs 读取，绝不写、不删、不移动旧仓任何东西。
 *   · **确定性**：不写"生成时间"（那只会让每次扫描都产生 diff）；只写实测数字 + 旧仓 HEAD。
 *
 * 用法：
 *   node tools/old-repo-inventory.mjs [--old-repo <dir>] [--out <file>] [--no-coupling] [--json]
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { git as runGit } from './lib/exec.mjs';
import { DEFAULT_INVENTORY_OUT as DEFAULT_OUT, DEFAULT_OLD_REPO, REPO_ROOT } from './lib/paths.mjs';
import {
  ARCHIVE_ZIP,
  ARCHIVE_ZIP_REL,
  blobContent,
  blobSha1,
  buildArchiveZip,
  oldRepoRoot,
  plan,
  readArchiveZip,
  refAbs,
  snapshotFiles,
  SOURCE_COMMIT,
} from './lib/translate-ref.mjs';

export { DEFAULT_OLD_REPO, REPO_ROOT, DEFAULT_OUT };


/** 体积统计时按二进制扩展名跳过（它们不需要行尾统计） */
const BINARY_EXT = new Set([
  'png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'psd', 'ico', 'cur',
  'ttf', 'otf', 'woff', 'woff2', '7z', 'zip', 'rar', 'gz',
  'exe', 'dll', 'node', 'idb', 'i64', 'pdb', 'obj', 'lib', 'so', 'dylib',
  'dat', 'sth', 'agf', 'bin', 'wav', 'ogg', 'mp3', 'db', 'sqlite', 'sqlite3',
]);
const EOL_MAX_BYTES = 4 * 1024 * 1024;

/** 跨域引用实测（与立项原文 §1.5 同口径，供迁移轮确认"目录切不干净"） */
const COUPLING = [
  { from: 'tickets', tokens: ['amayui-emulator'] },
  { from: 'app/amayui-emulator', tokens: ['tickets/T-'] },
  { from: 'docs-new', tokens: ['amayui-emulator'] },
  { from: 'plugins', tokens: ['tickets/', 'src/'] },
  { from: '.agents', tokens: ['tickets/'] },
  { from: 'scripts', tokens: ['analysis/'] },
];

/** 工具层的自我声明：**我动哪片数据、有哪些操作**（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'old-repo',
  title: '旧仓（**只读外部仓库**）：盘点 + 翻译参考快照重建',
  data: [
    '`<旧仓>`（仓库外，默认 `E:\\Games\\Eushully\\天結`；只跑只读 git 命令与 fs 读取）',
    '`docs/00-origin/old-repo-inventory.md`（生成物，由本工具拥有）',
    `\`${ARCHIVE_ZIP_REL}\` + \`docs/01-translation/ref/assets/**\`（翻译参考快照；由本工具按规则重建）`,
  ],
  access: 'r only（对旧仓**严格只读**）／w：只写那份生成物与翻译参考快照',
  tool: 'tools/old-repo-inventory.mjs',
};

export const OPERATIONS = [
  { name: 'inventory', argv: [], mutates: true, summary: '重新实测旧仓 → `docs/00-origin/old-repo-inventory.md`（生成物）' },
  {
    name: 'translate-ref',
    argv: ['--translate-ref'],
    mutates: true,
    summary: `从旧仓重建翻译参考快照（\`ref/assets/**\` 散文件 + \`${ARCHIVE_ZIP_REL}\`）；缺省 dry-run`,
  },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：数据 / 操作' },
];

export function describe() {
  return {
    domain: DOMAIN,
    invariants: [
      {
        id: 1,
        text: '对旧仓严格只读：不改、不删、不移动它任何东西',
        enforcedBy: '只用只读 git 命令与 `fs` 读取；证据 = 旧仓 `git status --porcelain` 必须为空',
      },
      {
        id: 2,
        text: '生成物是状态，因此**由脚本生成**：文件头写明"别手改，改脚本"，且不写生成时间（避免每次扫描都产生 diff）',
        enforcedBy: '生成物头部自带说明；`pnpm tools old-repo inventory` 可随时重跑',
      },
      {
        id: 3,
        text: '翻译参考快照的落点是**规则**不是清单（`keywords-*`/`glossary-draft`/`SG与SC分节对应` ⇒ `assets/` 散文件；其余 ⇒ `archive.zip`），且与旧仓来源**逐字节**相同',
        enforcedBy: '`tools/test/translation-ref.test.mjs`（逐字节比对 blob sha + zip 条目集合 + 拒绝来源清单之外的文件）',
      },
    ],
    operations: OPERATIONS,
    writePath:
      '只写生成物 `docs/00-origin/old-repo-inventory.md` 与翻译参考快照（`ref/assets/**` + `ref/archive.zip`）；对旧仓没有任何写操作。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.domain.id} —— ${d.domain.title}（自描述）`);
  L.push('');
  L.push('## 我动哪片数据');
  for (const x of d.domain.data) L.push(`* ${x}`);
  L.push(`* 读写：${d.domain.access}`);
  L.push(`* 工具：\`${d.domain.tool}\``);
  L.push('');
  L.push('## 不变量（含"谁在守它"）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.text}　—　${c.enforcedBy}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools ${d.domain.id} ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}

/** 只读 git 命令：直接用纯工具 `lib/exec.mjs` 的 `git()`（fd 重定向；非零退出即抛，绝不"读不到就当空"） */
const git = runGit;

function human(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KiB', 'MiB', 'GiB', 'TiB'];
  let v = bytes / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i += 1;
  }
  return `${v.toFixed(1)} ${units[i]}`;
}

/** 递归统计（跟随符号链接，但用 realpath 防环） */
function walk(dir, seen = new Set()) {
  let files = 0;
  let bytes = 0;
  let dirs = 0;
  let real;
  try {
    real = fs.realpathSync(dir);
  } catch {
    return { files, bytes, dirs, missing: true };
  }
  if (seen.has(real)) return { files, bytes, dirs, loop: true };
  seen.add(real);
  let items;
  try {
    items = fs.readdirSync(dir, { withFileTypes: true });
  } catch {
    return { files, bytes, dirs, denied: true };
  }
  for (const it of items) {
    const p = path.join(dir, it.name);
    let st;
    try {
      st = fs.statSync(p); // 跟随符号链接
    } catch {
      continue;
    }
    if (st.isDirectory()) {
      dirs += 1;
      const sub = walk(p, seen);
      files += sub.files;
      bytes += sub.bytes;
      dirs += sub.dirs;
    } else if (st.isFile()) {
      files += 1;
      bytes += st.size;
    }
  }
  return { files, bytes, dirs };
}

function isIgnored(oldRepo, rel) {
  try {
    execFileSync('git', ['check-ignore', '-q', '--', rel], { cwd: oldRepo, stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}

// ─────────────────────────────────────────────────────────── 各项实测

function gitFacts(oldRepo) {
  const head = git(oldRepo, ['log', '-1', '--format=%H%x09%cI%x09%s']);
  const [sha, date, subject] = head.split('\t');
  const tracked = git(oldRepo, ['ls-files']).split('\n').filter(Boolean);
  const lfsPatterns = fs.existsSync(path.join(oldRepo, '.gitattributes'))
    ? fs
        .readFileSync(path.join(oldRepo, '.gitattributes'), 'utf8')
        .split('\n')
        .map((l) => l.trim())
        .filter((l) => l && !l.startsWith('#') && l.includes('filter=lfs'))
    : [];
  const allAttrLines = fs.existsSync(path.join(oldRepo, '.gitattributes'))
    ? fs.readFileSync(path.join(oldRepo, '.gitattributes'), 'utf8').split('\n').filter((l) => l.trim() && !l.trim().startsWith('#'))
    : [];
  let packBytes = 0;
  for (const d of ['objects/pack']) {
    const p = path.join(oldRepo, '.git', d);
    if (!fs.existsSync(p)) continue;
    for (const f of fs.readdirSync(p)) packBytes += fs.statSync(path.join(p, f)).size;
  }
  return {
    headSha: sha ?? '',
    headDate: date ?? '',
    headSubject: subject ?? '',
    commits: Number(git(oldRepo, ['rev-list', '--count', 'HEAD']) || 0),
    trackedCount: tracked.length,
    tracked,
    lfsPatterns,
    textEolRules: allAttrLines.length - lfsPatterns.length,
    hasRootPackageJson: fs.existsSync(path.join(oldRepo, 'package.json')),
    packBytes,
    lfsFiles: git(oldRepo, ['lfs', 'ls-files'], { tolerant: true }).split('\n').filter(Boolean).length,
  };
}

function inventory(oldRepo) {
  const rows = [];
  for (const it of fs.readdirSync(oldRepo, { withFileTypes: true })) {
    if (it.name === '.git') continue;
    const rel = it.name;
    const abs = path.join(oldRepo, rel);
    const lst = fs.lstatSync(abs);
    const isLink = lst.isSymbolicLink();
    let target = '';
    if (isLink) {
      try {
        target = fs.readlinkSync(abs);
      } catch {
        target = '?';
      }
    }
    const st = fs.statSync(abs); // 跟随
    const tracked = git(oldRepo, ['ls-files', '--', rel]).split('\n').filter(Boolean).length;
    const w = st.isDirectory() ? walk(abs) : { files: 1, bytes: st.size, dirs: 0 };
    rows.push({
      name: rel,
      type: isLink ? 'LINK' : st.isDirectory() ? 'DIR' : 'FILE',
      target,
      tracked,
      files: w.files,
      bytes: w.bytes,
      ignored: isIgnored(oldRepo, rel),
    });
  }
  rows.sort((a, b) => (a.type === b.type ? b.bytes - a.bytes : a.type.localeCompare(b.type)));
  return rows;
}

function eolCensus(oldRepo, tracked) {
  const out = { scanned: 0, lf: 0, crlf: 0, mixed: 0, noEol: 0, skippedBinary: 0, skippedLarge: 0, mixedSamples: [] };
  for (const rel of tracked) {
    const ext = path.extname(rel).slice(1).toLowerCase();
    if (BINARY_EXT.has(ext)) {
      out.skippedBinary += 1;
      continue;
    }
    const abs = path.join(oldRepo, rel);
    let st;
    try {
      st = fs.statSync(abs);
    } catch {
      continue;
    }
    if (!st.isFile()) continue;
    if (st.size > EOL_MAX_BYTES) {
      out.skippedLarge += 1;
      continue;
    }
    let buf;
    try {
      buf = fs.readFileSync(abs);
    } catch {
      continue;
    }
    if (buf.includes(0)) {
      out.skippedBinary += 1;
      continue;
    }
    const crlf = (buf.toString('latin1').match(/\r\n/g) ?? []).length;
    const lf = (buf.toString('latin1').match(/\n/g) ?? []).length;
    out.scanned += 1;
    if (lf === 0) out.noEol += 1;
    else if (crlf === 0) out.lf += 1;
    else if (crlf === lf) out.crlf += 1;
    else {
      out.mixed += 1;
      if (out.mixedSamples.length < 20) out.mixedSamples.push(rel);
    }
  }
  return out;
}

function caseConflicts(tracked) {
  const seen = new Map();
  const dup = [];
  for (const rel of tracked) {
    const key = rel.toLowerCase();
    if (seen.has(key) && seen.get(key) !== rel) dup.push([seen.get(key), rel]);
    else seen.set(key, rel);
  }
  return dup;
}

function coupling(oldRepo, pairs) {
  const rows = [];
  for (const p of pairs) {
    const files = git(oldRepo, ['ls-files', '--', p.from]).split('\n').filter(Boolean);
    let hitFiles = 0;
    let hitLines = 0;
    for (const rel of files) {
      let text;
      try {
        text = fs.readFileSync(path.join(oldRepo, rel), 'utf8');
      } catch {
        continue;
      }
      let hitHere = false;
      for (const line of text.split('\n')) {
        if (p.tokens.some((t) => line.includes(t))) {
          hitLines += 1;
          hitHere = true;
        }
      }
      if (hitHere) hitFiles += 1;
    }
    rows.push({ from: p.from, tokens: p.tokens, files: hitFiles, lines: hitLines, scannedFiles: files.length });
  }
  return rows;
}

// ─────────────────────────────────────────────────────────── 渲染

export function renderMarkdown(data) {
  const { oldRepo, facts, rows, eol, conflicts, couplingRows, skills } = data;
  const L = [];
  L.push('# 旧仓盘点（实测快照）');
  L.push('');
  L.push('> 本文件由 `pnpm tools old-repo inventory`（`tools/old-repo-inventory.mjs`）**重新实测生成** —— **不要手改，改脚本**。');
  L.push('> 快照里**不写生成时间**（那只会让每次扫描都产生 diff）；只写实测数字与旧仓 HEAD。');
  L.push('> 旧仓全程**只读**：本脚本只跑只读 git 命令与 fs 读取。');
  L.push('');
  L.push(`* 旧仓：\`${oldRepo}\``);
  L.push(`* HEAD：\`${facts.headSha}\`（${facts.headDate}）`);
  L.push(`* HEAD 提交：${facts.headSubject}`);
  L.push(`* 提交数：${facts.commits}　|　跟踪文件：${facts.trackedCount}　|　pack 体积：${human(facts.packBytes)}`);
  L.push(`* 根 \`package.json\`：${facts.hasRootPackageJson ? '**存在**' : '**不存在**（⇒ 旧仓没有 workspace）'}`);
  L.push(`* \`.gitattributes\`：LFS 规则 ${facts.lfsPatterns.length} 条，\`text\`/\`eol\` 规则 ${facts.textEolRules} 条`);
  L.push(`* LFS 跟踪文件：${facts.lfsFiles}`);
  L.push('');
  L.push('## 1. 顶层目录实测');
  L.push('');
  L.push('| 目录 / 文件 | 类型 | 跟踪 | 全部文件 | 体积 | gitignore | 备注 |');
  L.push('|---|---|---:|---:|---:|---|---|');
  for (const r of rows) {
    const note = r.type === 'LINK' ? `符号链接 → \`${r.target}\`` : r.files === 0 && r.tracked === 0 ? '空' : '';
    L.push(`| \`${r.name}\` | ${r.type} | ${r.tracked} | ${r.files} | ${human(r.bytes)} | ${r.ignored ? '是' : '否'} | ${note} |`);
  }
  L.push('');
  L.push('## 2. 行尾实测（跟踪的文本文件）');
  L.push('');
  L.push('| 项 | 数量 |');
  L.push('|---|---:|');
  L.push(`| 已扫描 | ${eol.scanned} |`);
  L.push(`| 纯 LF | ${eol.lf} |`);
  L.push(`| 纯 CRLF | ${eol.crlf} |`);
  L.push(`| **混合行尾** | **${eol.mixed}** |`);
  L.push(`| 无行尾符 | ${eol.noEol} |`);
  L.push(`| 跳过（二进制 / 含 NUL） | ${eol.skippedBinary} |`);
  L.push(`| 跳过（> ${human(EOL_MAX_BYTES)}） | ${eol.skippedLarge} |`);
  L.push('');
  if (eol.mixedSamples.length) {
    L.push('混合行尾样本（最多 20 个）：');
    L.push('');
    for (const s of eol.mixedSamples) L.push(`* \`${s}\``);
    L.push('');
  }
  L.push('## 3. 大小写冲突实测（win/mac 不敏感、Linux CI 敏感）');
  L.push('');
  L.push(conflicts.length === 0 ? '**0 组冲突**（跟踪路径里没有仅大小写不同的重名）。' : `**${conflicts.length} 组冲突**：`);
  for (const [a, b] of conflicts) L.push(`* \`${a}\` vs \`${b}\``);
  L.push('');
  if (couplingRows) {
    L.push('## 4. 跨域引用实测（说明"为什么不能简单按目录切"）');
    L.push('');
    L.push('| 从 | 命中 token | 命中文件 | 命中行 | 扫描文件 |');
    L.push('|---|---|---:|---:|---:|');
    for (const c of couplingRows) {
      L.push(`| \`${c.from}\` | ${c.tokens.map((t) => `\`${t}\``).join(' / ')} | ${c.files} | ${c.lines} | ${c.scannedFiles} |`);
    }
    L.push('');
  }
  L.push('## 5. 供迁移轮注意的实测要点');
  L.push('');
  L.push(`* \`.agents/skills/\` 下**实测 ${skills.count} 个技能**（每个一个 \`SKILL.md\`）：${skills.names.map((n) => `\`${n}\``).join('、')}`);
  L.push('* 技能发现路径**固定** `.agents/skills/<名字>/SKILL.md`（DSH 硬要求）⇒ 新仓该目录必须在，内容从零重写。');
  L.push('* 大件（`install/` `raw/` `raw-parts/` `.tmp/` `tools/`）**不入库**：用符号链接 / 路径登记引用。');
  L.push('* `raw/` 是**符号链接**（指向外部游戏安装目录）：可用，只要不 track。');
  L.push('');
  return `${L.join('\n')}\n`;
}

// ─────────────────────────────────────────────────────────── 翻译参考快照（重建）

/**
 * 快照与旧仓来源的**逐件对账**（只读，不写任何东西）。
 *
 * ★ 比的是 **blob sha1**：① 资产侧直接算盘上文件的 blob sha；
 *   ② zip 侧把条目取出来算（写入是确定性的 ⇒ "条目内容相同" 等价于 "zip 字节相同"）。
 *   ⇒ 不用为了"看一眼差多少"就把 253 个文件从旧仓全读一遍。
 */
export function translateRefStatus(repo = oldRepoRoot()) {
  const items = plan(repo);
  const assets = items.filter((i) => !i.packed);
  const packed = items.filter((i) => i.packed);

  const missing = [];
  const differing = [];
  for (const it of assets) {
    const abs = refAbs(it.refRel);
    if (!fs.existsSync(abs)) {
      missing.push(it.refRel);
      continue;
    }
    if (blobSha1(fs.readFileSync(abs)) !== it.sha) differing.push(it.refRel);
  }

  let zip = null;
  let zipError = null;
  try {
    zip = readArchiveZip();
  } catch (err) {
    zipError = err.message;
  }
  const extraInZip = [];
  const zipDiff = [];
  const zipMissing = [];
  if (zip) {
    const want = new Map(packed.map((p) => [p.refRel, p.sha]));
    for (const [name, data] of zip) {
      if (!want.has(name)) {
        extraInZip.push(name);
        continue;
      }
      if (blobSha1(data) !== want.get(name)) zipDiff.push(name);
    }
    for (const p of packed) if (!zip.has(p.refRel)) zipMissing.push(p.refRel);
  }

  // 快照区里"来源清单之外"的东西（assets 侧散件；zip 本身与说明允许）
  const known = new Set(['README.md', 'archive.zip', ...assets.map((a) => a.refRel)]);
  const extra = snapshotFiles().filter((f) => !known.has(f));

  return { repo, items, assets, packed, missing, differing, zip, zipError, extraInZip, zipDiff, zipMissing, extra };
}

/** `translate-ref`：缺省 dry-run，`--write` 才落盘（写后逐件回读复验，不绿回滚）。 */
export function cmdTranslateRef(repo, { write = false } = {}) {
  if (!repo || !fs.existsSync(repo)) {
    process.stderr.write(`旧仓不存在：${repo ?? '(未登记)'}\n`);
    return 2;
  }
  const s = translateRefStatus(repo);
  const out = (t) => process.stdout.write(`${t}\n`);

  out(`来源       旧仓 ${repo} @ ${SOURCE_COMMIT}`);
  out(`映射       资产（散文件） ${s.assets.length} 件 · 存档（进 zip） ${s.packed.length} 件`);
  out(
    `资产侧     ${s.missing.length ? `缺 ${s.missing.length}` : '齐'} / ${s.differing.length ? `与来源不同 ${s.differing.length}` : '与来源一致'}`,
  );
  out(
    `zip 侧     ${
      s.zipError
        ? `✖ ${s.zipError}`
        : `条目 ${s.zip.size} · 缺 ${s.zipMissing.length} · 多 ${s.extraInZip.length} · 内容不符 ${s.zipDiff.length}`
    }`,
  );
  if (s.extra.length) out(`快照区杂项 ${s.extra.length} 件（来源清单之外）`);

  // ★ 两类分开：**blockers** = 快照区里有不该有的东西（重建也不会消失 ⇒ 拒绝写）；
  //   **pending** = 与来源不一致 / 缺件（重建就是去修它）。
  const blockers = [...s.extra, ...s.extraInZip];
  const pending = [
    ...s.missing.map((x) => `缺资产 ${x}`),
    ...s.differing.map((x) => `资产与来源不同 ${x}`),
    ...(s.zipError ? [s.zipError] : []),
    ...s.zipMissing.map((x) => `zip 缺条目 ${x}`),
    ...s.zipDiff.map((x) => `zip 条目内容不符 ${x}`),
  ];
  if (s.items.length === 0) blockers.push('映射命中 0 件（旧仓读取不可信？）');
  if (s.assets.length === 0 || s.packed.length === 0) blockers.push('资产侧或存档侧为空（映射规则被改坏了？）');

  if (blockers.length) {
    out('');
    for (const p of blockers.slice(0, 20)) out(`  ✖ ${p}`);
    if (blockers.length > 20) out(`  … 另有 ${blockers.length - 20} 项`);
  }

  if (!write) {
    out('');
    out(
      blockers.length
        ? `快照区有 ${blockers.length} 处不该有的东西（重建消不掉 ⇒ 得先人处理）。`
        : pending.length
          ? `有 ${pending.length} 处与旧仓来源不一致（重建即可修）。`
          : '快照与旧仓来源一致。',
    );
    out('（dry-run）加 --write 重建。');
    return 0;
  }

  if (blockers.length) {
    process.stderr.write('拒绝重建：快照区里有来源清单之外的东西（重建不会让它们消失，先人工处理）。\n');
    return 2;
  }

  // 写：资产侧逐件从旧仓取**原始字节**；存档侧整包一次重建（确定性 zip）。
  for (const it of s.assets) {
    const abs = refAbs(it.refRel);
    fs.mkdirSync(path.dirname(abs), { recursive: true });
    fs.writeFileSync(abs, blobContent(repo, it.oldRel));
  }
  const zipBuf = buildArchiveZip(repo, s.packed);
  fs.writeFileSync(ARCHIVE_ZIP, zipBuf);

  // 写后回读复验（不绿回滚这一条在这里只是"报出来"，因为写的是**从旧仓取来的原件**）
  const after = translateRefStatus(repo);
  const bad =
    after.missing.length + after.differing.length + after.extra.length +
    after.zipMissing.length + after.extraInZip.length + after.zipDiff.length;
  out('');
  out(`已落盘：资产 ${s.assets.length} 件 · ${ARCHIVE_ZIP_REL} ${zipBuf.length} B`);
  out(bad === 0 ? '写后回读：✔ 与旧仓来源逐件一致' : `写后回读：✖ ${bad} 处不一致（工具的问题，别提交）`);
  return bad === 0 ? 0 : 2;
}

// ─────────────────────────────────────────────────────────── CLI

function parseArgs(argv) {
  const out = { oldRepo: DEFAULT_OLD_REPO, out: DEFAULT_OUT, coupling: true, json: false, write: false, action: null };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--old-repo') out.oldRepo = argv[++i];
    else if (a === '--out') out.out = path.resolve(argv[++i]);
    else if (a === '--no-coupling') out.coupling = false;
    else if (a === '--json') out.json = true;
    else if (a === '--write') out.write = true;
    else if (a === '--describe') out.describe = true;
    else if (a === '--translate-ref') out.action = 'translate-ref';
    else if (a === '--help' || a === '-h') out.help = true;
  }
  return out;
}

export function collect(oldRepo, { coupling: withCoupling = true } = {}) {
  const facts = gitFacts(oldRepo);
  const rows = inventory(oldRepo);
  const eol = eolCensus(oldRepo, facts.tracked);
  const conflicts = caseConflicts(facts.tracked);
  const couplingRows = withCoupling ? coupling(oldRepo, COUPLING) : null;
  const skillsDir = path.join(oldRepo, '.agents', 'skills');
  const skills = {
    count: 0,
    names: [],
  };
  if (fs.existsSync(skillsDir)) {
    for (const d of fs.readdirSync(skillsDir, { withFileTypes: true })) {
      if (d.isDirectory() && fs.existsSync(path.join(skillsDir, d.name, 'SKILL.md'))) {
        skills.count += 1;
        skills.names.push(d.name);
      }
    }
    skills.names.sort();
  }
  return { oldRepo, facts, rows, eol, conflicts, couplingRows, skills };
}

const HELP = `tools/old-repo-inventory.mjs — 只读旧仓：盘点 + 翻译参考快照重建

  node tools/old-repo-inventory.mjs [inventory] [--old-repo <dir>] [--out <file>] [--no-coupling] [--json]
  node tools/old-repo-inventory.mjs --translate-ref [--old-repo <dir>] [--write]
  node tools/old-repo-inventory.mjs --describe [--json]     # 自描述：数据 / 操作

（经派发器：pnpm tools old-repo <inventory|translate-ref|describe> [args]）
`;

export function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  if (args.help) {
    process.stdout.write(HELP);
    return 0;
  }
  if (args.describe) {
    process.stdout.write(args.json ? `${JSON.stringify(describe(), null, 2)}\n` : describeText());
    return 0;
  }
  if (args.action === 'translate-ref') return cmdTranslateRef(args.oldRepo, { write: args.write });
  if (!fs.existsSync(args.oldRepo)) {
    process.stderr.write(`旧仓不存在：${args.oldRepo}\n`);
    return 2;
  }
  const data = collect(args.oldRepo, { coupling: args.coupling });

  // ★ 拒绝写出"错盘点"：git 事实拿不到（不是 git 仓库 / git 跑不起来）时，宁可失败也不写全 0 的文件。
  //   （实测踩过：受限沙箱里管道 spawn EPERM 被 catch 吞掉 ⇒ 静默覆盖成一份全 0 的盘点。）
  const suspicious = [];
  if (!/^[0-9a-f]{40}$/.test(data.facts.headSha)) suspicious.push('HEAD 不是 40 位 sha（不是 git 仓库？）');
  if (data.facts.trackedCount === 0) suspicious.push('跟踪文件数为 0');
  if (data.facts.commits === 0) suspicious.push('提交数为 0');
  if (suspicious.length) {
    process.stderr.write(`拒绝写出盘点（实测结果不可信）：${suspicious.join('；')}\n`);
    process.stderr.write(`（没有覆盖 ${path.relative(REPO_ROOT, args.out)}；请先确认 ${args.oldRepo} 是完好的 git 仓库、且 git 能跑起来）\n`);
    return 2;
  }

  if (args.json) {
    process.stdout.write(`${JSON.stringify(data, null, 2)}\n`);
    return 0;
  }
  const md = renderMarkdown(data);
  fs.mkdirSync(path.dirname(args.out), { recursive: true });
  const existed = fs.existsSync(args.out);
  const prev = existed ? fs.readFileSync(args.out, 'utf8') : null;
  fs.writeFileSync(args.out, md, 'utf8');
  process.stdout.write(`写出 ${path.relative(REPO_ROOT, args.out)}（${md.length} 字符${prev === md ? '，与上次内容一致' : ''}）\n`);
  process.stdout.write(`旧仓 HEAD ${data.facts.headSha.slice(0, 12)}：跟踪 ${data.facts.trackedCount} 文件 / ${data.facts.commits} 次提交；混合行尾 ${data.eol.mixed} 个；大小写冲突 ${data.conflicts.length} 组\n`);
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
