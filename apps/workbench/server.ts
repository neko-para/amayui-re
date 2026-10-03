#!/usr/bin/env node
/**
 * apps/workbench/server.ts — **项目工作台**的本地只读服务端（Node 直跑，无构建）。
 *
 * 它是什么：`data/requirements/`（需求树）与 `data/translations/patch.json`（AGE 脚本反汇编视图）
 * 的一层 HTTP。客户端在 `src/`（Vue 3 + Vite），构建产物 `dist/web/` 由本文件当根服务。
 *
 * ★ 与 DSH 无关：这是独立的本地网页；不引 DSH 的任何包、不依赖 DSH 在跑。
 * ★ **只读**：本服务没有任何写路径。改台账只有 `pnpm tools requirements set --write`，
 *   改 patch 只有 `pnpm tools patch extract|edit --write`。
 * ★ **单一真源**：解析 / 建树 / 聚合 / 引用解析 / 小节切分 / 反汇编视图**全部来自** `tools/lib/`：
 *   `requirements.mjs`（`loadNodes` / `buildTree` / `flatten` / `rollup` / `describeNode` / `describeText`）
 *   与 `patch.mjs`（`allScriptNames` / `SPEAKER_FILTER` / `openSides` / `buildView` / `rowsOf` /
 *   `mapperContext` / `NO_OPS_ENTRY`）· `bin-source.mjs`（`isAgeScript`）。
 *   这里一行都不重抄 —— 工作台是那套模型的第三个消费者（`requirements plan` / `patch view` 是前两个）。
 *   "这是不是 AGE 脚本"也只问模型：`isAgeScript()`（内层是 `readHeader()`，认不出签名即假）。
 *
 * ★ **脚本一览 = 基线根里所有能反汇编的 `.BIN`**（= `allScriptNames()` 的 `names`），
 *   **不是**按名字筛出来的子集。`SPEAKER_FILTER`（`SC*` / `SP*`，含 `$N$` 前缀）只是
 *   "旧管线给它做过**页 / 说话人标注**"的**标签**：它**不是** patch 的范围 ——
 *   patch 的范围是"全部脚本里有变更的"，靠"产物 ≠ 基线"判定 ⇒ 本服务**不依赖**旧仓 `install/`。
 *
 * 为什么放在 `apps/` 而不是 `tools/`：`tools/test/layering.test.mjs` 要求 `tools/*.mjs` 只 import `./lib/*`，
 * 所以 CLI **不能** import 本项目；反过来本项目 import 模型是允许的（测试不扫 `apps/`）。
 * 启动器 `tools/requirements.mjs --serve` 只负责 spawn 本文件。
 *
 * 为什么是 `.ts` 而 node 直接跑：Node v24 的原生 type stripping ⇒ **服务端零构建**。
 * 限制：只用**可擦除**语法（无 enum / namespace / 参数属性 / 装饰器），相对 import 带扩展名。
 *
 * 跑：`pnpm tools requirements serve [--port 7788]`、或 `node apps/workbench/server.ts`。
 */
import fs from 'node:fs';
import http from 'node:http';
import type { IncomingMessage, Server, ServerResponse } from 'node:http';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  BUDGET,
  STATUS_MARK,
  buildTree,
  describeNode,
  describeText as requirementsDescribeText,
  flatten,
  loadNodes,
  rollup,
} from '../../tools/lib/requirements.mjs';
import { DEFAULT_REQUIREMENTS_DIR } from '../../tools/lib/paths.mjs';
import {
  SPEAKER_FILTER,
  DEFAULT_PATCH,
  NO_OPS_ENTRY,
  allScriptNames,
  buildView,
  describeText as patchDescribeText,
  loadPatch,
  mapperContext,
  openSides,
  rowsOf,
} from '../../tools/lib/patch.mjs';
import { isAgeScript } from '../../tools/lib/bin-source.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
/** 客户端构建产物（`vite build` 的 outDir，见 vite.config.ts）；被 .gitignore 的 `dist/` 命中 */
const WEB_DIR = path.join(HERE, 'dist', 'web');

/** 固定端口：地址可以直接记住/粘贴。撞端口不自动漂（地址变了比报错更烦）。 */
const DEFAULT_PORT = 7788;
/**
 * 默认只听回环。这个服务能读整个仓库的台账与反汇编视图 —— 绑到 0.0.0.0
 * 等于把它们放到局域网上，所以那必须是**显式**选择（`--host`）。
 */
const DEFAULT_HOST = '127.0.0.1';

/** 服务端认的 MIME（够 `dist/web` 用：Vite 只会产出这几种） */
const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.map': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
};

const nodeIdOf = (n: { fields: { id?: string }; name: string }) =>
  typeof n.fields.id === 'string' ? n.fields.id : `REQ-${n.name}`;

// ─────────────────────────────────────────────────────────── 模型入口（进程内各缓存一次）

type Base = ReturnType<typeof openSides>['base'];

let baseCache: Base | null = null;
/**
 * **基线根**（清单 `roots.gameInstall`）。
 *
 * ★ 只有这一个根：`SPEAKER_FILTER`（`SC*` / `SP*`）只是**名字标签**，与"产物根"无关 ——
 *   旧仓 `install/` 不再是本服务的依赖（`patch` 落库之后运行期只需要基线与 patch）。
 * `openRoot()` 要读 ALF 索引（几百 ms），而基线树在一次会话里不会变 ⇒ 进程内缓存一次。
 */
function baseRoot(): Base {
  if (!baseCache) baseCache = openSides({ targetDir: null }).base;
  return baseCache;
}

let mapperCache: ReturnType<typeof mapperContext> | null = null;
/** 简→日写法字典（`src` 视图要用它把中文落成 BIN 写法）；读一次就够 */
function mapper() {
  if (!mapperCache) mapperCache = mapperContext();
  return mapperCache;
}

let patchCache: { key: string; doc: ReturnType<typeof loadPatch> } | null = null;
/** patch 文档（按 mtime + size 失效） */
function patch() {
  const key = statKey(DEFAULT_PATCH);
  if (!patchCache || patchCache.key !== key) patchCache = { key, doc: loadPatch(DEFAULT_PATCH) };
  return patchCache.doc;
}

/** `mtimeMs:size` —— 缓存失效的判据必须**看见真源**，不能只看进程启动时间 */
function statKey(abs: string): string {
  try {
    const st = fs.statSync(abs);
    return `${st.mtimeMs}:${st.size}`;
  } catch {
    return 'missing';
  }
}

/** 基线 BIN 的 `mtimeMs:size`（散装直接取文件；ALF 取承载它的归档文件 + 段偏移） */
function baseFileKey(base: Base, name: string, from: string): string {
  const loose = /^loose:(.*)$/.exec(from);
  let abs: string | null = null;
  if (loose) abs = base.loose.get(String(name).toUpperCase()) ?? null;
  else {
    const arc = /→([^@]+)@/.exec(from);
    if (arc) abs = path.join(base.dir, arc[1]);
  }
  return abs ? `${from}|${statKey(abs)}` : from;
}

// ─────────────────────────────────────────────────────────── 需求（形状与旧 apps/requirements 完全一致）

/** One node as the page consumes it. Everything derived comes from the model. */
function nodeShape(item: any) {
  const n = item.node;
  const f = n.fields;
  const r = item.rollup;
  return {
    id: nodeIdOf(n),
    name: n.name,
    short: n.name.slice(-8),
    type: f.type,
    status: f.status,
    title: n.title,
    parent: f.parent === undefined ? null : f.parent === 'null' ? null : f.parent,
    depth: item.depth,
    orphan: item.orphan === true,
    mark: item.orphan ? '❓' : (STATUS_MARK as Record<string, string>)[f.status] ?? '?',
    kids: 0,
    rollup: { live: r.live, total: r.total, derivedStatus: r.derivedStatus },
    done: r.total - r.live,
    total: r.total,
    aggregated: r.derivedStatus !== f.status,
    blockedBy: f.blocked_by ?? [],
    tags: f.tags ?? [],
    severity: f.severity,
    verify: f.verify,
    repro: f.repro,
    doneReason: f.done_reason,
    droppedReason: f.dropped_reason,
    supersedes: f.supersedes ?? [],
    lines: n.lines,
    body: n.body,
  };
}

function load(dir: string) {
  const nodes = loadNodes(dir);
  return { nodes, tree: buildTree(nodes) };
}

/**
 * The whole ledger, read fresh on every request.
 *
 * 重读而不是缓存：预算是 ≤40 节点、≤80 行/节点（不变量 5），整棵树连正文也就几十 KB，
 * 一次 `readFileSync` × 40 的开销远小于"缓存该什么时候失效"这件事的复杂度。
 */
function buildTreePayload(dir: string) {
  const { nodes, tree } = load(dir);

  const totals: Record<string, number> = { open: 0, doing: 0, blocked: 0, done: 0, dropped: 0, superseded: 0 };
  let bugs = 0;
  for (const n of nodes) {
    if (n.fields.status in totals) totals[n.fields.status] += 1;
    if (n.fields.type === 'bug' && ['open', 'doing', 'blocked'].includes(n.fields.status)) bugs += 1;
  }

  // ★ `flatten` 来自模型：深度优先 + 每节点聚合 + 孤立节点兜底，与 `plan` 同一份遍历。
  const rows: any[] = [];
  const decisions: any[] = [];
  for (const item of flatten(nodes, tree)) {
    if (item.node.fields.type === 'decision') {
      // 决策不属进度树（不变量 2：不许写 parent），与 `plan` 一样单列。
      decisions.push(nodeShape(item));
      continue;
    }
    rows.push(nodeShape(item));
  }
  // 每节点的直接子数（面板要显示"有没有子节点"）。
  const kidsById = new Map<string, number>();
  for (const n of nodes) {
    const p = n.fields.parent;
    if (p === undefined || p === 'null') continue;
    const parent = tree.resolve(p);
    if (!parent) continue;
    const key = nodeIdOf(parent);
    kidsById.set(key, (kidsById.get(key) ?? 0) + 1);
  }
  for (const row of rows) row.kids = kidsById.get(row.id) ?? 0;

  return {
    ok: true,
    generatedAt: new Date().toISOString(),
    dir,
    readOnly: true,
    budget: BUDGET,
    marks: STATUS_MARK,
    totals: { ...totals, bugs, count: nodes.length },
    nodes: rows,
    decisions,
    /** 字段/不变量/操作的自描述，页脚用同一份（页面不复述 schema）。 */
    schema: requirementsDescribeText(),
  };
}

/** One focused node + its parent chain + direct children + sections spliced from the body. */
function buildNodePayload(dir: string, ref: string) {
  const { nodes, tree } = load(dir);
  const node = tree.resolve(ref);
  if (!node) {
    return {
      ok: false,
      error: `找不到节点（或引用不唯一）：${ref}`,
      hint: '可用完整 id、唯一前缀或唯一后缀 —— 与 `pnpm tools requirements show` 同一套解析。',
    };
  }
  const d = describeNode(node, tree);
  const shape = (x: any) => ({
    id: x.id ?? nodeIdOf(x.node ?? x),
    short: (x.node ?? x).name.slice(-8),
    title: (x.node ?? x).title,
    type: (x.node ?? x).fields.type,
    status: (x.node ?? x).fields.status,
    mark: (STATUS_MARK as Record<string, string>)[(x.node ?? x).fields.status] ?? '?',
    rollup: x.rollup,
    done: x.rollup.total - x.rollup.live,
    total: x.rollup.total,
  });
  return {
    ok: true,
    readOnly: true,
    node: { ...nodeShape({ node: d.node, rollup: d.rollup, depth: 0 }), sections: d.sections },
    parentChain: d.parentChain.map((p) => shape({ node: p, rollup: rollup(p, tree) })),
    children: d.children.map(shape),
  };
}

// ─────────────────────────────────────────────────────────── 脚本（一览 + 反汇编正文）

type ScriptRow = {
  name: string;
  baseFrom: string;
  baseBytes: number;
  /** 旧管线（`annotate-speaker.js`）给它做过**页 / 说话人标注** —— 只是标签，**不是** patch 范围 */
  annotated: boolean;
  hasPatch: boolean;
  opCount: number;
};

/** 名字序（`$` 也照排）由模型给：`allScriptNames()` 已经排好，这里不再自己排一遍 */
let nameSetCache: { key: string; names: Set<string> } | null = null;

/**
 * 基线根里的**全部名字**（全大写，含 `.AGF` / `.OGG` 这类非脚本 —— 它同时是"名字在不在根里"的判据）。
 *
 * ★ 缓存在进程内（按基线根的 `mtime+size` 失效）：`names()` 每次要遍历 ALF 索引里的**两万多个**条目，
 *   而 `/api/script` 只拿它做一次查找 —— 不该为"这个名字在不在根里"把索引重建一遍。
 */
function baseNames(): Set<string> {
  const base = baseRoot();
  const key = statKey(base.dir);
  if (nameSetCache && nameSetCache.key === key) return nameSetCache.names;
  nameSetCache = { key, names: base.names() };
  return nameSetCache.names;
}

type ScriptIndex = { key: string; rows: ScriptRow[]; nonScript: number };
let indexCache: ScriptIndex | null = null;

/**
 * **一览的全量名单**：`allScriptNames(baseRoot())` —— 基线根里所有能反汇编的 `.BIN`。
 *
 * ★ 口径**不**按名字筛：`SPEAKER_FILTER` 只给行打标签。曾经拿它当范围 ⇒ 静默漏掉 247 支真译文
 *   （非 SC/SP 但产物 ≠ 基线的那批）；"有变更"只能由 patch 有没有条目判定，不能由名字判定。
 * ★ 缓存判据**看见真源**：patch 与基线根的 `mtime+size`（与正文缓存同一条纪律）；
 *   缓存后 `/api/script` 的规范名查找也不再重扫。
 *   注：这一层按**目录**判失效 —— 就地改一个**已存在**的 BIN 而不动目录时，一览里的 `baseBytes`
 *   可能停旧值（页面上的"刷新"会重取）；正文缓存按**每个 BIN 的** `mtime+size` 判，所以
 *   `/api/script` 永远看真源。
 */
function scriptIndex(): ScriptIndex {
  const base = baseRoot();
  const key = `${statKey(DEFAULT_PATCH)}|${statKey(base.dir)}`;
  if (indexCache && indexCache.key === key) return indexCache;
  const doc = patch();
  // ★ 名单与"非脚本"计数都取自模型：候选 `.BIN`、门槛签名判定都在 `allScriptNames()` 里，这里不重抄
  const { names, nonScript } = allScriptNames(base);
  const rows: ScriptRow[] = names.map((name) => {
    const hit = base.resolve(name) as { from: string; buf: Buffer };
    const entry = doc.scripts[name];
    return {
      name,
      baseFrom: hit.from,
      baseBytes: hit.buf.length,
      annotated: SPEAKER_FILTER.test(name),
      hasPatch: entry !== undefined,
      opCount: entry ? entry.ops.length : 0,
    };
  });
  indexCache = { key, rows, nonScript: nonScript.length };
  return indexCache;
}

/**
 * 一览：全部可反汇编脚本的基线与 patch 覆盖情况（**只算元信息**，不算正文）。
 *
 * 正文在 `/api/script`，且带缓存（见 `scriptView()`）。
 * 汇总（`count` / `annotated` / `hasPatch` / `unchanged` / `nonScript`）**现算**，不手抄任何数字。
 */
function listScriptsPayload() {
  const base = baseRoot();
  const { rows, nonScript } = scriptIndex();
  return {
    ok: true,
    readOnly: true,
    patch: DEFAULT_PATCH,
    patchBytes: fs.existsSync(DEFAULT_PATCH) ? fs.statSync(DEFAULT_PATCH).size : 0,
    /** 名单的唯一来源：基线根（`patch` 只为"有变更"的脚本建条目，所以它不是名单的来源） */
    source: 'baseline',
    count: rows.length,
    /** 其中被旧管线**标注过**的支数（标签口径，与名单范围无关） */
    annotated: rows.filter((r) => r.annotated).length,
    hasPatch: rows.filter((r) => r.hasPatch).length,
    /** 名单里**没有条目**的那些（产物 == 基线 ⇒ 不进 patch） */
    unchanged: rows.filter((r) => !r.hasPatch).length,
    /** 签名认不出（不是 AGE 脚本）的 `.BIN` 个数 —— 它们**不在**名单里 */
    nonScript,
    dirs: { base: base.dir },
    /** patch 的字段/不变量/操作自描述（页面不复述 schema）。 */
    schema: patchDescribeText(),
    scripts: rows,
  };
}

const KINDS = ['data', 'src'];

/** 视图缓存上限：最大的一支正文 ~2 MB，8 支 ≈ 20 MB，够用又不会把 900 多支全驻留 */
const VIEW_CACHE_MAX = 8;
const viewCache = new Map<string, { key: string; value: any }>();
let viewCacheHits = 0;
let viewCacheMisses = 0;

/**
 * 请求里的名字 ⇒ 基线根里的**规范名**（大小写不敏感：根里的名字全大写）。
 * 不在这里判"是不是脚本" —— 那是 `scriptView()` 的事（两条错误要分开报）。
 */
function canonicalName(raw: string): string | null {
  const key = raw.toUpperCase();
  return baseNames().has(key) ? key : null;
}

/**
 * 一支脚本的正文（`data` = 基线视图 / `src` = 基线 + patch 视图）—— 同一份 `buildView` 字节。
 *
 * ★ 接受**任何**基线根里能解析且签名是脚本的名字：没有 patch 条目就用空叠加层
 *   （`NO_OPS_ENTRY`）⇒ `src` 视图于是与 `data` 视图逐字节相同（"没有变更"不是错误）。
 */
function scriptView(name: string, kind: string) {
  const base = baseRoot();
  const doc = patch();
  const hit = base.resolve(name);
  if (!hit) return { error: `找不到这支脚本（基线根里没有这个名字）：${name}`, status: 404 };
  if (!isAgeScript(hit.buf)) {
    return { error: `找不到这支脚本的反汇编：${name} 不是 AGE 脚本（签名既不是 SYS4 也不是 SYS5）`, status: 404 };
  }
  // ★ 失效判据必须看见真源：patch 的 mtime+size + 基线 BIN 的 mtime+size。
  //   只按进程启动时间缓存的话，改完 patch 页面会一直显示旧的。
  const key = `${statKey(DEFAULT_PATCH)}|${baseFileKey(base, name, hit.from)}`;
  const cacheKey = `${name}\u0000${kind}`;
  const cached = viewCache.get(cacheKey);
  if (cached && cached.key === key) {
    viewCacheHits += 1;
    viewCache.delete(cacheKey); // LRU：重插一次 = 最近使用
    viewCache.set(cacheKey, cached);
    return { value: cached.value };
  }
  viewCacheMisses += 1;

  const entry = doc.scripts[name] ?? NO_OPS_ENTRY(hit.buf);
  let view;
  try {
    view = buildView(kind, hit.buf, entry, { lineToBin: mapper().mapper.lineToBin });
  } catch (err) {
    return { error: `${name}（${kind}）视图算不出来：${(err as Error).message}`, status: 500 };
  }
  const value = {
    name,
    kind,
    text: view.text,
    bytes: Buffer.byteLength(view.text),
    // ★ 行数口径由模型自己给（`rowsOf` = patch 锚定用的同一个行序空间），不在这里数 `\n`
    rows: rowsOf(view.text).raw.length,
    stats: view.stats,
    baseFrom: hit.from,
    baseBytes: hit.buf.length,
    hasPatch: doc.scripts[name] !== undefined,
  };
  viewCache.set(cacheKey, { key, value });
  while (viewCache.size > VIEW_CACHE_MAX) {
    const oldest = viewCache.keys().next().value as string;
    viewCache.delete(oldest);
  }
  return { value };
}

// ─────────────────────────────────────────────────────────── 静态资源（白名单，请求路径不参与拼路径）

type StaticIndex = { key: string; root: string; map: Map<string, string> };
let staticCache: StaticIndex | null = null;

/** 递归列出 `dist/web` 下的文件，键是 posix 相对路径。请求路径只当**查表的键**。 */
function staticIndex(): StaticIndex | null {
  const key = statKey(WEB_DIR);
  if (key === 'missing') return null;
  if (staticCache && staticCache.key === key) return staticCache;
  const map = new Map<string, string>();
  const walk = (absDir: string, prefix: string) => {
    for (const e of fs.readdirSync(absDir, { withFileTypes: true })) {
      const rel = prefix ? `${prefix}/${e.name}` : e.name;
      if (e.isDirectory()) walk(path.join(absDir, e.name), rel);
      else if (e.isFile()) map.set(rel, path.join(absDir, e.name));
    }
  };
  try {
    walk(WEB_DIR, '');
  } catch {
    return null;
  }
  staticCache = { key, root: WEB_DIR, map };
  return staticCache;
}

// ─────────────────────────────────────────────────────────── HTTP

function sendJson(res: ServerResponse, status: number, body: unknown) {
  const text = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(text),
    'Cache-Control': 'no-store',
  });
  res.end(text);
}

function sendFile(res: ServerResponse, abs: string) {
  let data: Buffer;
  try {
    data = fs.readFileSync(abs);
  } catch (err) {
    sendJson(res, 500, { ok: false, error: `读不到 ${path.basename(abs)}：${(err as Error).message}` });
    return;
  }
  res.writeHead(200, {
    'Content-Type': MIME[path.extname(abs).toLowerCase()] ?? 'application/octet-stream',
    'Content-Length': data.length,
    // 构建产物带内容 hash，但仍然 no-store：本地只读视图没有必要缓存策略
    'Cache-Control': 'no-store',
  });
  res.end(data);
}

export type ServerOptions = {
  port?: number;
  host?: string;
  dir?: string;
  /** `dev` 模式只服务 `/api/*`（前端由 Vite 的 dev server 出，`/api` 走它的 proxy） */
  dev?: boolean;
  log?: (line: string) => void;
  /** 不再监听 stdin/SIGINT（自检用不到） */
  installSignalHandlers?: boolean;
};

function createHandler(dir: string, dev: boolean) {
  return (req: IncomingMessage, res: ServerResponse) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      const pathname = url.pathname;

      // 只读：任何非 GET/HEAD 都是 405（不是"没实现"，是**刻意没有**写路径）
      if (req.method !== 'GET' && req.method !== 'HEAD') {
        sendJson(res, 405, { ok: false, error: `只读服务：不支持 ${req.method}。改数据走 CLI（requirements set --write / patch edit --write）。` });
        return;
      }

      if (pathname === '/api/health') {
        const base = baseRoot();
        const doc = patch();
        let nodes = 0;
        try {
          nodes = loadNodes(dir).length;
        } catch {
          nodes = -1;
        }
        sendJson(res, 200, {
          ok: true,
          readOnly: true,
          requirementsDir: dir,
          nodes,
          patch: DEFAULT_PATCH,
          patchBytes: fs.existsSync(DEFAULT_PATCH) ? fs.statSync(DEFAULT_PATCH).size : 0,
          patchScripts: Object.keys(doc.scripts).length,
          base: base.dir,
          /** "旧管线标注过"的口径就是这条正则 —— **标签**，与任何产物根、与 patch 范围都无关 */
          annotatedFilter: String(SPEAKER_FILTER),
          webBuilt: staticIndex() !== null,
          viewCache: { max: VIEW_CACHE_MAX, size: viewCache.size, hits: viewCacheHits, misses: viewCacheMisses },
        });
        return;
      }
      if (pathname === '/api/tree') {
        sendJson(res, 200, buildTreePayload(dir));
        return;
      }
      if (pathname === '/api/node' || pathname.startsWith('/api/node/')) {
        // `/api/node/<ref>`：ref 走 URL 编码，这里显式解回来（含中文标题片段也不怕）。
        const raw = pathname === '/api/node' ? url.searchParams.get('ref') ?? '' : pathname.slice('/api/node/'.length);
        let ref = raw;
        try {
          ref = decodeURIComponent(raw);
        } catch {
          /* 保持原样：解析失败交给模型报"找不到" */
        }
        const payload = buildNodePayload(dir, ref);
        sendJson(res, payload.ok ? 200 : 404, payload);
        return;
      }
      if (pathname === '/api/scripts') {
        sendJson(res, 200, listScriptsPayload());
        return;
      }
      if (pathname === '/api/script' || pathname.startsWith('/api/script/')) {
        const raw = pathname === '/api/script' ? url.searchParams.get('name') ?? '' : pathname.slice('/api/script/'.length);
        let name = raw;
        try {
          name = decodeURIComponent(raw);
        } catch {
          /* 交给下面的名单查不到 */
        }
        const kind = url.searchParams.get('kind') ?? 'src';
        if (!KINDS.includes(kind)) {
          sendJson(res, 400, { ok: false, error: `kind 只能是 ${KINDS.join(' / ')}，实际：${kind}` });
          return;
        }
        // 名字先落成基线根里的规范名（大小写不敏感）；**不要求**它被标注过、或有 patch 条目
        const key = canonicalName(name);
        if (!key) {
          sendJson(res, 404, {
            ok: false,
            error: `找不到这支脚本（基线根里没有这个名字）：${name}`,
            hint: '名单见 GET /api/scripts —— 口径是「基线根里所有能反汇编的 .BIN」；名字按文件名比较（大小写不敏感）。',
          });
          return;
        }
        const got = scriptView(key, kind);
        if (got.error) {
          sendJson(res, got.status ?? 500, { ok: false, error: got.error });
          return;
        }
        sendJson(res, 200, { ok: true, readOnly: true, ...got.value });
        return;
      }
      if (pathname.startsWith('/api/')) {
        sendJson(res, 404, { ok: false, error: `未知端点：${pathname}` });
        return;
      }

      if (dev) {
        // dev：静态由 Vite 的 dev server 出（它的 proxy 把 /api 转到这里）
        sendJson(res, 404, { ok: false, error: `--dev 模式只服务 /api/*；页面走 Vite dev server（见 vite.config.ts 的 proxy）` });
        return;
      }

      // ★ 白名单：请求路径只当**查表的键**，从不参与拼路径 ⇒ 没有 `..` 这一说
      const want = pathname === '/' ? 'index.html' : pathname.slice(1);
      let key = want;
      try {
        key = decodeURIComponent(want);
      } catch {
        sendJson(res, 400, { ok: false, error: `请求路径不是合法编码：${want}` });
        return;
      }
      const index = staticIndex();
      const file = index?.map.get(key);
      if (file) {
        sendFile(res, file);
        return;
      }
      if (!index) {
        sendJson(res, 503, {
          ok: false,
          error: 'dist/web 还没构建',
          hint: 'cd apps/workbench && node scripts/vite-cli.mjs build（或在开发时用 --dev + Vite dev server）',
        });
        return;
      }
      sendJson(res, 404, { ok: false, error: `未知资源：${pathname}` });
    } catch (err) {
      sendJson(res, 500, { ok: false, error: err && (err as Error).message ? (err as Error).message : String(err) });
    }
  };
}

export function startServer(opts: ServerOptions = {}): Promise<Server> {
  const { port = DEFAULT_PORT, host = DEFAULT_HOST, dir = DEFAULT_REQUIREMENTS_DIR, dev = false, log = console.log } = opts;
  if (port !== 0 && (!Number.isInteger(port) || port < 0 || port > 65535)) {
    return Promise.reject(new Error(`--port 必须是端口号（0-65535），实际：${port}`));
  }
  const server = http.createServer(createHandler(dir, dev));
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.removeListener('error', reject);
      const actual = (server.address() as { port: number }).port;
      log('项目工作台（只读）');
      log(`  页面  http://${host}:${actual}/` + (dev ? '   ← --dev：本进程只管 /api/*，页面走 Vite dev server' : ''));
      log(`  需求  ${dir}`);
      log(`  patch ${DEFAULT_PATCH}`);
      log(`  API   http://${host}:${actual}/api/tree · /api/scripts · /api/health`);
      if (!dev && !staticIndex()) log('  ⚠ dist/web 还没构建：cd apps/workbench && node scripts/vite-cli.mjs build');
      log('  停    Ctrl+C');
      resolve(server);
    });
  });
}

// ─────────────────────────────────────────────────────────── CLI

const HELP = `apps/workbench/server.ts — 项目工作台的本地只读服务端（Node 直跑，无构建）

  node apps/workbench/server.ts [--port 7788] [--host 127.0.0.1] [--dir <需求目录>] [--dev]

  --port  监听端口（缺省 7788；0 = 让系统分配）
  --host  监听地址（缺省 127.0.0.1。★ 绑到非回环**必须**显式给这一项）
  --dir   需求台账目录（缺省 data/requirements/）
  --dev   只服务 /api/*（页面由 Vite dev server 出，它的 proxy 转到这里）

端点（全部 GET，全部只读）：
  /api/tree                     需求树 + 表头 + 预算 + 标记表 + 自描述
  /api/node/<ref>               一条需求：字段 + 正文小节 + 父链 + 直接子
  /api/scripts                  全部可反汇编的 AGE 脚本一览（「标注过」只是标签）+ 汇总
  /api/script/<name>?kind=…     一支脚本的反汇编正文（kind = data | src；text 就是 buildView 的输出）
  /api/health                   轻量自检
  /                             客户端（dist/web/ 的白名单视图）

★ 只读：没有任何写路径。改台账 \`pnpm tools requirements set --write\`；改 patch \`pnpm tools patch edit --write\`。
`;

function parseCli(argv: string[]) {
  const out: { port: number; host: string; dir: string | undefined; dev: boolean; help?: boolean } = {
    port: DEFAULT_PORT,
    host: DEFAULT_HOST,
    dir: undefined,
    dev: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--port') out.port = Number(argv[++i]);
    else if (a === '--host') out.host = argv[++i];
    else if (a === '--dir') out.dir = path.resolve(argv[++i]);
    else if (a === '--dev') out.dev = true;
    else if (a === '--help' || a === '-h') out.help = true;
    else throw new Error(`不认识的选项：${a}`);
  }
  return out;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  let opts: ReturnType<typeof parseCli> | undefined;
  try {
    opts = parseCli(process.argv.slice(2));
  } catch (err) {
    process.stderr.write(`${(err as Error).message}\n${HELP}`);
    process.exitCode = 2;
  }
  if (opts?.help) {
    process.stdout.write(HELP);
  } else if (opts) {
    try {
      const server = await startServer(opts);
      const bye = () => {
        server.close(() => process.exit(0));
        // close() 只等已有连接；这里不强制 destroy，让浏览器自己断开。
        setTimeout(() => process.exit(0), 200).unref();
      };
      process.on('SIGINT', bye);
      process.on('SIGTERM', bye);
    } catch (err) {
      const e = err as NodeJS.ErrnoException;
      if (e && e.code === 'EADDRINUSE') {
        process.stderr.write(
          `端口 ${opts.port} 已被占用。\n` +
            `这是固定端口的代价：换个端口即可，例如  pnpm tools requirements serve --port ${opts.port + 1}\n`,
        );
      } else {
        process.stderr.write(`启动失败：${e && e.stack ? e.stack : e}\n`);
      }
      process.exitCode = 1;
    }
  }
}
