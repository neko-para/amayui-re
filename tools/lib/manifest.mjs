/**
 * tools/lib/manifest.mjs — **`corpus/assets.json` 的领域模型**（枚举 / 不变量 / 读 / 写 / 自描述）
 *
 * 分层（见 tools/README.md §0）：
 *   · **纯工具**（`lib/paths` `lib/fsx` `lib/exec` …）不认识任何领域数据；
 *   · **领域模型**（本文件、`lib/samples.mjs`）持有 schema 与不变量，**不解析 argv、不打印**；
 *   · **CLI**（`tools/corpus.mjs`）只做"参数 → 模型 → 输出"。
 * 所以 `fixtures`（另一个域）要用清单模型时 import **本文件**，而不是 `corpus.mjs`（那是 CLI）。
 *
 * 设计口径（`docs/00-origin/decisions.md` §5）：
 *   · assets.json 是 **lockfile**：只记"来源与去向"，不记规则、不重复校验和、不写可从磁盘推导的计数。
 *   · 唯一能把它变成**约束**的是 `--validate` 必须能红。没有守卫的清单等于一份 Markdown。
 *   · 写纪律：先写临时文件再原子改名；写完**回读 + 复验**，不绿则**回滚**（且写前先在内存里预验）。
 */
import fs from 'node:fs';
import path from 'node:path';

import { gitAttrs, gitCheckIgnore, gitLsFiles, runCapture } from './exec.mjs';
import { listFiles, sha256File, statKind, toPosix } from './fsx.mjs';
import { DEFAULT_MANIFEST } from './paths.mjs';

export { DEFAULT_MANIFEST };

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

/**
 * **自足条目**：`kind=fixture` 的入库件 —— 固化资源（如实录下来的存档 / 截图），
 * **没有加工链、也没有可再取的上游**：入库的那一份**就是**原件。
 * ⇒ `origin` 允许缺席（写空数组或省略都一样，见 `originRequired`）。
 * ★ 反例：`disasm-corpus` 的入库件是从二进制**转写**出来的（有 recipe / derivedFrom），它的 `origin`
 *   记的是"转写前长什么样"的参照件；那个能力来自"来源 ≠ 入库件"，fixture 天生没有。
 */
export const SELF_CONTAINED = (e) => e?.kind === 'fixture' && CARRYING.has(e?.storage);

/** 该条目是否必须登记 `origin`（非空）——"自足条目"豁免 */
export const originRequired = (e) => !SELF_CONTAINED(e);

/** 每条目必填（与 `origin` 无关的那部分）；`origin` 是**条件必填**，见 `originRequired` */
const REQUIRED_FIELDS = ['id', 'kind', 'role', 'storage', 'dest', 'readOnly', 'consume'];
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

/** 工具**拥有**的顶层 `_doc`（写盘时自动注入 ⇒ 指向自描述，不会漂） */
export const MANIFEST_DOC =
  '本文件是**不透明数据**：字段语义 / 枚举 / 不变量 / 怎么查怎么改 见 `pnpm tools corpus describe`（说明书 corpus/assets.md）。不要手改。';

/** 工具层的自我声明：**我动哪片数据、有哪些操作**（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'corpus',
  title: '素材清单（lockfile：来源与去向）',
  data: ['`corpus/assets.json`（**唯一写入口就是本工具**）'],
  access: 'rw（唯一写入口；缺省 dry-run，写前内存预验 + 写后回读复验，不绿回滚）',
  tool: 'tools/corpus.mjs',
};

export const OPERATIONS = [
  { name: 'validate', argv: ['--validate'], mutates: false, summary: '跑 9 条不变量（红/绿 + 逐条详情）—— 全仓门禁' },
  { name: 'list', argv: ['--list'], mutates: false, summary: '条目一览（id/kind/role/storage/dest/origin 数）' },
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 枚举 / 不变量 / 操作' },
  { name: 'scan', argv: ['--scan'], mutates: true, summary: '补不入库件的 origin[].sha256（缺省 dry-run，加 --write 落盘）' },
  { name: 'add', argv: ['--add'], mutates: true, summary: '加条目：`<entry-json>` [--write]' },
  { name: 'set', argv: ['--set'], mutates: true, summary: "改条目（如 deferred → lfs 翻牌）：`<id> '<patch-json>'` [--write]" },
  { name: 'set-root', argv: ['--set-root'], mutates: true, summary: '加/改来源根：`<name> <path>` [--write]' },
  { name: 'normalize', argv: ['--normalize'], mutates: true, summary: '拉回规范形态（剔多余顶层键、重排、重写 _doc）[--write]' },
];

/**
 * 9 条不变量的**唯一标题真源**（`--describe` 与 `--validate` 都从这里取，避免两处措辞漂移）。
 */
export const CHECK_TITLES = new Map([
  [1, 'schema：必填齐全 / 枚举合法 / id 全局唯一'],
  [2, '自洽：external-only/deferred ⇔ dest=null；lfs/git ⇔ dest 非空'],
  [3, '存在性：dest 与 origin 都能在盘上找到（staging 只 warning；目录型 dest 必须非空）'],
  [4, '校验和纪律：入库件不写 sha256；不入库的文件件必须写且与盘上一致；目录型 dest 的副本必须与来源逐字节相同'],
  [5, '忽略一致性：external-only 且 dest 在仓内 ⇒ 必须被 .gitignore 命中'],
  [6, 'LFS 一致性：storage=lfs ⇒ dest 下**每个已跟踪文件**（含目录型 dest 里的载荷，不靠 origin 枚举）filter = lfs；未跟踪的不算'],
  [7, '语料保真：入库的 disasm-corpus 必须有 recipe 且断言通过（豁免参照件/清单）'],
  [8, '知识准入门：knowledge-source 只能是 external-only/deferred'],
  [9, '真前身可解析：ref 必须存在；入库语料必须指到 kind=binary（豁免参照件/清单）'],
]);

/** 字段说明（`--describe` 用；这是**自描述**，不是第二份 schema） */
const ENTRY_FIELD_DOC = [
  ['id', '✅', '`域/名`', '稳定键，其它文档引用它'],
  ['kind', '✅', `\`${KINDS.join('` \\| `')}\``, '类别'],
  ['role', '✅', `\`${ROLES.join('` \\| `')}\``, '人类语义角色'],
  ['storage', '✅', `\`${STORAGES.join('` \\| `')}\``, '存储去向（也是进度：deferred → lfs）'],
  ['origin', '条件', '数组 {root,path,sha256?}', '来源；root ∈ roots 的键或 abs。★ **自足条目**（入库的 `kind=fixture`）允许缺席：它的"来源"就是入库的那一份本身，登记它等于把 dest 抄第二遍；其余条目必须非空'],
  ['derivedFrom', '条件', '[{ref,tool?,note?}]', '真前身（**从**某个件、**用工具**产出的那份）；入库的 disasm-corpus 必填且要指到 kind=binary'],
  ['dest', '✅', 'string \\| null', '入库路径；external-only/deferred ⇒ 必须 null'],
  ['readOnly', '✅', 'boolean', '只读件禁止就地修改'],
  ['recipe', '条件', 'string', '加工 / 转码脚本；入库的 disasm-corpus 必填'],
  ['consume', '✅', 'string', '一句话消费规则'],
  ['blocks', '⬜', '["M1","K3"]', '哪些迁移批次依赖它'],
  ['note', '⬜', 'string', '补充'],
];

// ─────────────────────────────────────────────────────────── 读 / 规范化

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

/** 规范化序列化：键序固定 + 自动注入工具拥有的 `_doc` ⇒ diff 干净、写入确定、自描述不漂 */
export function canonicalStringify(manifest) {
  const roots = orderKeys(manifest.roots ?? {}, ROOT_KEY_ORDER);
  const out = {
    schemaVersion: manifest.schemaVersion,
    _doc: MANIFEST_DOC,
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

/** 按 id 取条目（找不到就抛：调用方不该在"条目不存在"时静默继续） */
export function entryOf(manifest, id) {
  const e = manifest.entries.find((x) => x.id === id);
  if (!e) throw new Error(`清单里找不到条目 ${id}`);
  return e;
}

/** 某条目的 origin，按 basename 索引（跨条目的同名文件不会互相串） */
export function originsByName(manifest, id) {
  const map = new Map();
  for (const o of entryOf(manifest, id).origin ?? []) map.set(path.basename(o.path), o);
  return map;
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
  const add = (id, status, message, details = []) =>
    checks.push({ id, title: CHECK_TITLES.get(id) ?? `#${id}`, status, message, details });

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
      // ★ 自足条目（入库的 fixture：固化资源，没有加工链 / 没有可再取的上游）：
      //   origin 可以**缺席或为空** —— 它的"来源"就是入库的那一份本身。
      if (e?.origin !== undefined && !Array.isArray(e.origin)) {
        details.push(`${at}: origin 必须是数组（或对自足条目留空）`);
      } else if ((e?.origin ?? []).length === 0 && originRequired(e)) {
        details.push(`${at}: origin 必须是**非空**数组（只有入库的 kind=fixture 可以空）`);
      } else {
        for (const [j, o] of (e?.origin ?? []).entries()) {
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
    add(1, details.length ? 'fail' : 'pass',
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
    add(2, details.length ? 'fail' : 'pass',
      details.length ? `${details.length} 处不自洽` : '全部自洽', details);
  }

  // (3) 存在性 ───────────────────────────────────────────────
  {
    const details = [];
    let warns = 0;
    for (const e of entries) {
      if (CARRYING.has(e?.storage) && typeof e.dest === 'string' && e.dest !== '') {
        const abs = path.resolve(repoRoot, e.dest);
        const k = statKind(abs);
        if (k === 'missing') details.push(`${e.id}: dest 在盘上不存在 → ${e.dest}`);
        else if (k === 'dir' && listFiles(abs).length === 0) {
          details.push(`${e.id}: dest 是目录但里面没有任何载荷文件 → ${e.dest}`);
        }
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
    add(3,
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
    // ★ 目录型 dest：副本必须与来源**逐字节相同**（哈希不写进清单，但每次 --validate 现算现比）
    for (const e of entries) {
      // dest 不是字符串时**不要**碰 path.resolve（那会抛 "paths[1] ... Received null"，
      // 把"守卫该红"变成"进程崩"[#2 已经会报这条违规]）—— 实测踩过。
      if (!CARRYING.has(e.storage) || typeof e.dest !== 'string' || e.dest === '') continue;
      const destAbs = path.resolve(repoRoot, e.dest);
      if (statKind(destAbs) !== 'dir') continue;
      for (const o of e.origin ?? []) {
        const abs = resolveOrigin(repoRoot, roots, o);
        if (abs === null || statKind(abs) !== 'file') continue;
        const target = path.join(destAbs, path.basename(abs));
        if (statKind(target) !== 'file') {
          details.push(`${e.id}: 目录型 dest 里缺来源文件 ${path.basename(abs)} → ${e.dest}`);
          continue;
        }
        if (checkHashes) {
          const srcHash = sha256File(abs);
          const copyHash = sha256File(target);
          if (srcHash !== copyHash) {
            details.push(
              `${e.id}: 副本与来源不一致 → ${path.basename(abs)}（来源 ${srcHash.slice(0, 12)}… / 副本 ${copyHash.slice(0, 12)}…）`,
            );
          }
        }
      }
    }
    add(4,
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
        if (r.error) details.push(`warn: ${e.id}: git 不可用 ⇒ 未执行忽略检查（${r.error}）`);
        else if (!r.ignored) details.push(`${e.id}: external-only 且 dest 在仓内 ⇒ 必须被 .gitignore 命中 → ${e.dest}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(5,
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处未命中` : '无此形态条目 / 全部命中', details);
  }

  // (6) LFS 一致性 ───────────────────────────────────────────
  {
    const details = [];
    let checked = 0;
    const lfsDirs = new Set(); // 目录型 dest 里**真的**核对到 filter=lfs 的那些（用于下面的"规则整块消失"断言）
    if (!useGit) details.push('warn: --no-git：跳过');
    else {
      for (const e of entries) {
        if (e.storage !== 'lfs') continue;
        // 同上：dest 不是字符串时交给 #2 报，别在这里崩
        if (typeof e.dest !== 'string' || e.dest === '') continue;
        const destAbs = path.resolve(repoRoot, e.dest);
        const destKind = statKind(destAbs);
        // ★ 目录型 dest：枚举**盘上真实存在**的已跟踪文件（不看 origin）。
        //   为什么不用 origin 枚举：origin 是"来源"，它缺席时（自足条目）会**静默漏检**整片载荷；
        //   而 `-text` 恰好是"字节不该被 eol 转换"的标记（LFS 规则都带它）⇒ 用 git 自己的属性判，
        //   不在这里复制一份"哪些扩展名算载荷"的清单（那会与 .gitattributes 漂移）。
        const tracked =
          destKind === 'dir'
            ? (() => {
                const r = gitLsFiles(repoRoot, toPosix(e.dest));
                if (r.error) {
                  details.push(`warn: ${e.id}: git ls-files 跑不起来 ⇒ 未枚举目录内文件（${r.error}） → ${e.dest}`);
                  return [];
                }
                return r.files.filter((f) => statKind(path.resolve(repoRoot, f)) === 'file');
              })()
            : [toPosix(e.dest)];
        if (tracked.length === 0) continue;

        const { attrs, error } = gitAttrs(repoRoot, tracked);
        if (error) {
          details.push(`warn: ${e.id}: git check-attr 跑不起来 ⇒ 未执行 LFS 属性检查（${error}）`);
          continue;
        }
        for (const t of tracked) {
          checked += 1;
          const a = attrs[t] ?? {};
          if (destKind === 'file') {
            if ((a.filter ?? 'unspecified') !== 'lfs') {
              details.push(`${e.id}: storage=lfs ⇒ git check-attr filter 必须是 lfs，实际 "${a.filter ?? 'unspecified'}" → ${t}`);
            }
            continue;
          }
          // 目录型：**纯文本侧车**（.gitattributes 的 `* text=auto` 给的 `text: auto` / `set`）
          // 本来就不该走 LFS，放行；其余（`text: unset` = 字节不该被 eol 转换，或已经 filter=lfs）必须走 LFS。
          // ★ 判据故意**不**抄一份"哪些扩展名算载荷"的清单 —— 那是 .gitattributes 的事，抄过来必然漂。
          if (a['text'] === 'auto' || a['text'] === 'set') continue;
          if ((a.filter ?? 'unspecified') === 'lfs') {
            lfsDirs.add(e.dest);
            continue;
          }
          details.push(`${e.id}: 入库目录里的 ${t} 不是纯文本侧车却没有 filter=lfs ⇒ 它会以普通对象进 git，请补 .gitattributes 规则`);
        }
      }
      // ★ 自足条目（fixture，origin 已按口径清空）没有"来源"可比对 ⇒ 它**只能**靠上面的属性判；
      //   万一 .gitattributes 的 LFS 规则被整块删掉，所有载荷会一起退化成 `text: auto` 混过去。
      //   这一条把那个缺口堵上：入库的目录型 dest 至少得有一个真的 filter=lfs 的文件。
      for (const e of entries) {
        if (e.storage !== 'lfs' || typeof e.dest !== 'string' || e.dest === '') continue;
        if (statKind(path.resolve(repoRoot, e.dest)) !== 'dir') continue;
        if (lfsDirs.has(e.dest)) continue;
        details.push(`${e.id}: 目录型 dest 里一个 filter=lfs 的文件都没有 ⇒ .gitattributes 的 LFS 规则要么没覆盖、要么已被删掉 → ${e.dest}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(6,
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处不是 lfs` : `${checked} 个入库路径已核对`, details);
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
      const r = runCapture(repoRoot, process.execPath, [recipeAbs, '--verify']);
      if (r.code === 0) {
        ran += 1;
      } else if (r.error) {
        details.push(`warn: ${e.id}: 跑 recipe 失败（${r.error}）⇒ 断言未执行`);
      } else {
        const tail = r.out.trim().split('\n').slice(-6).join('\n      ');
        details.push(`${e.id}: recipe 断言未通过（${e.recipe} --verify 退出 ${r.code}）\n      ${tail}`);
      }
    }
    const hard = details.filter((d) => !d.startsWith('warn:'));
    add(7,
      hard.length ? 'fail' : 'pass', hard.length ? `${hard.length} 处未通过` : ran ? `${ran} 个条目断言通过` : '没有入库的 disasm-corpus 条目（都是 deferred / external-only）', details);
  }

  // (8) 知识准入门 ───────────────────────────────────────────
  {
    const details = [];
    for (const e of entries) {
      if (e.kind === 'knowledge-source' && !NOT_CARRYING.has(e.storage)) {
        details.push(`${e.id}: kind=knowledge-source ⇒ storage 只能是 external-only/deferred（K3 通过前不得入库），实际 ${e.storage}`);
      }
    }
    add(8,
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
    add(9,
      details.length ? 'fail' : 'pass', details.length ? `${details.length} 处不可解析` : '全部可解析', details);
  }

  const failures = checks.filter((c) => c.status === 'fail').length;
  const warnings = checks.filter((c) => c.status === 'warn').length;
  return { checks, failures, warnings };
}

/** 自描述：**JSON 是不透明数据**，字段语义 / 枚举 / 不变量 / 操作一律由控制脚本给出（文档只指向它） */
export function describe() {
  return {
    file: 'corpus/assets.json',
    purpose: 'lockfile 性质的素材清单：记「来源与去向」；不是约束，也不重复校验和',
    topLevel: {
      schemaVersion: 1,
      _doc: '由本工具拥有（写盘时自动注入指向本自描述的指针）',
      roots: {
        required: [...ROOT_KEY_ORDER],
        note: '绝对路径只写在这里；条目里一律相对路径；额外来源根用 --set-root 登记',
      },
      entries: '见下方 entry 字段表',
    },
    entry: {
      required: REQUIRED_FIELDS,
      conditionalRequired: ['origin（除**自足条目**外必填）', 'derivedFrom / recipe（入库的 disasm-corpus 必填）'],
      fields: ENTRY_FIELD_DOC.map(([name, req, type, desc]) => ({ name, req, type, desc })),
      notWritten: ['bytes', '入库件 sha256', 'LFS oid', 'status', 'generatedAt', '任何可从磁盘/git 推导的计数'],
      selfContained:
        '入库的 `kind=fixture`（固化资源：没有加工链）**允许 origin 缺席** —— 它的"来源"就是入库的那一份本身；' +
        '因此"副本 == 来源"的逐字节断言只对**有 origin 的**目录型 dest 生效（fixture 的完整性交给 git/LFS 校验和）。',
    },
    invariants: [...CHECK_TITLES].map(([id, title]) => ({ id, title })),
    operations: OPERATIONS,
    writePath:
      '只有本工具（唯一写入口）；缺省 dry-run，--write 才落盘；写前内存预验 + 写后回读复验，不绿回滚。**不要手改 JSON**。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push(`★ 读写：${DOMAIN.access}`);
  L.push('');
  L.push('## 顶层');
  L.push(`* schemaVersion: ${d.topLevel.schemaVersion}`);
  L.push(`* _doc: ${d.topLevel._doc}`);
  L.push(`* roots: 必填 ${d.topLevel.roots.required.join(' / ')} —— ${d.topLevel.roots.note}`);
  L.push(`* entries: ${d.topLevel.entries}`);
  if (d.entry.conditionalRequired) L.push(`* 条件必填：${d.entry.conditionalRequired.join(' / ')}`);
  L.push('');
  L.push('## entry 字段');
  L.push('| 字段 | 必填 | 类型 / 枚举 | 说明 |');
  L.push('|---|---|---|---|');
  for (const f of d.entry.fields) L.push(`| \`${f.name}\` | ${f.req} | ${f.type} | ${f.desc} |`);
  L.push('');
  L.push(`**刻意不写**：${d.entry.notWritten.join('、')}`);
  L.push('');
  L.push(`**自足条目**：${d.entry.selfContained}`);
  L.push('');
  L.push('## 不变量（`--validate` 的断言；标题即真源）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.title}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools ${DOMAIN.id} ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}

// ─────────────────────────────────────────────────────────── 写入

function writeAtomically(target, text) {
  const tmp = `${target}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, text, 'utf8');
  fs.renameSync(tmp, target);
}

/**
 * 规范化：**入库件（lfs/git）的 origin 不允许带 sha256**（规则 #4）。
 * 翻牌（deferred → lfs）时最容易踩：`--scan` 在它还是 deferred 时已经补过 sha256。
 * ⇒ 写入前自动剔除并报告，而不是让用户去手改（手改必然漂）。
 * @returns {string[]} 被剔除的项（人类可读）
 */
export function normalizeManifest(manifest) {
  const dropped = [];
  for (const e of manifest.entries ?? []) {
    if (!CARRYING.has(e.storage)) continue;
    for (const o of e.origin ?? []) {
      if ('sha256' in o) {
        dropped.push(`${e.id}  ${o.root}:${o.path}`);
        delete o.sha256;
      }
    }
  }
  return dropped;
}

/**
 * 落盘 + 回读 + 复验；**任何**异常或红灯都回滚。
 * 两道闸：① 先在**内存里**复验（不绿就一个字都不写）；② 写后回读再验（二道闸，异常也回滚）。
 * @returns {{ok:boolean, restored?:boolean, failures?:number, dropped?:string[], reason?:string}}
 */
export function saveManifest(manifest, manifestPath, opts = {}) {
  const backup = fs.readFileSync(manifestPath, 'utf8');
  const dropped = normalizeManifest(manifest);

  const failText = (checks) =>
    `写后复验未通过（已回滚）：\n  - ${checks
      .filter((c) => c.status === 'fail')
      .map((c) => `#${c.id} ${c.title}：${c.message}`)
      .join('\n  - ')}`;

  // ① 内存预验：不绿就根本不落盘
  try {
    const pre = validateManifest(manifest, opts);
    if (pre.failures > 0) return { ok: false, restored: false, failures: pre.failures, reason: failText(pre.checks) };
  } catch (err) {
    return { ok: false, restored: false, reason: `预验抛错（未写盘）：${err.message}` };
  }

  // ② 写 → 回读 → 复验
  try {
    writeAtomically(manifestPath, canonicalStringify(manifest));
    const reread = loadManifest(manifestPath);
    const post = validateManifest(reread, opts);
    if (post.failures > 0) {
      fs.writeFileSync(manifestPath, backup, 'utf8');
      return { ok: false, restored: true, failures: post.failures, reason: failText(post.checks) };
    }
  } catch (err) {
    fs.writeFileSync(manifestPath, backup, 'utf8');
    return { ok: false, restored: true, reason: `写/复验抛错（已回滚）：${err.message}` };
  }
  return { ok: true, failures: 0, dropped };
}

