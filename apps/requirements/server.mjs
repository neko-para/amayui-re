#!/usr/bin/env node
/**
 * apps/requirements/server.mjs — 需求台账的**本地只读网页**。
 *
 * 它是什么：`data/requirements/` 的一层 HTTP + 浏览器视图。总览看整棵树与进度，
 * 每条需求有自己的地址，用**浏览器自己的 tab**并排看。
 *
 * ★ 与 DSH **无关**：这是一个独立的本地网页。之所以做成网页，只是因为"切 tab 比切应用快"。
 *   所以它不引 DSH 的主题 token、不引 DSH 的任何包、不依赖 DSH 在跑。
 *
 * ★ 只读：本服务**没有任何写路径**。改台账只有 `pnpm tools requirements set --write`。
 *
 * ★ 单一真源：解析 / 建树 / 聚合 / 引用解析 / 小节切分**全部来自** `tools/lib/requirements.mjs`。
 *   这里一行都不重抄 —— 网页只是那套模型的另一个消费者（`plan` 是前一个）。
 *
 * 为什么放在 `apps/` 而不是 `tools/`：`tools/*.mjs` 被 `test/layering.test.mjs`
 * 要求只能 import `./lib/*`，所以它不能 import 本项目；反过来本项目 import 模型是允许的
 * （测试不扫 `apps/`）。启动器 `tools/requirements.mjs --serve` 只负责 spawn 本文件。
 *
 * 跑：`pnpm tools requirements serve [--port 7788]` 或直接 `node apps/requirements/server.mjs`
 */
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import {
  BUDGET,
  STATUS_MARK,
  buildTree,
  describeNode,
  describeText,
  flatten,
  loadNodes,
  rollup,
} from '../../tools/lib/requirements.mjs';
import { DEFAULT_REQUIREMENTS_DIR } from '../../tools/lib/paths.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.join(HERE, 'public');

/**
 * 固定端口：你要求"启动时打印即可"，固定端口让 `http://127.0.0.1:7788` 可以直接记住/粘贴。
 * 撞了端口不会自动漂（那样地址就变了）—— 而是明确报错并提示换个 `--port`。
 */
const DEFAULT_PORT = 7788;
/**
 * 默认只听回环。这个服务能读 `data/requirements/` —— 绑到 0.0.0.0 等于把仓库内容
 * 放到局域网上，所以那必须是**显式**选择（`--host`）。
 */
const DEFAULT_HOST = '127.0.0.1';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
};

const nodeIdOf = (n) => (typeof n.fields.id === 'string' ? n.fields.id : `REQ-${n.name}`);

/** One node as the page consumes it. Everything derived comes from the model. */
function nodeShape(item) {
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
    mark: item.orphan ? '❓' : STATUS_MARK[f.status] ?? '?',
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

/**
 * The whole ledger, read fresh on every request.
 *
 * 重读而不是缓存：预算是 ≤40 节点、≤80 行/节点（不变量 5），整棵树连正文也就几十 KB，
 * 一次 `readFileSync` × 40 的开销远小于"缓存该什么时候失效"这件事的复杂度。
 */
function buildTreePayload(dir) {
  const { nodes, tree } = load(dir);

  const totals = { open: 0, doing: 0, blocked: 0, done: 0, dropped: 0, superseded: 0 };
  let bugs = 0;
  for (const n of nodes) {
    if (n.fields.status in totals) totals[n.fields.status] += 1;
    if (n.fields.type === 'bug' && ['open', 'doing', 'blocked'].includes(n.fields.status)) bugs += 1;
  }

  // ★ `flatten` 来自模型：深度优先 + 每节点聚合 + 孤立节点兜底，与 `plan` 同一份遍历。
  const rows = [];
  const decisions = [];
  for (const item of flatten(nodes, tree)) {
    if (item.node.fields.type === 'decision') {
      // 决策不属进度树（不变量 2：不许写 parent），与 `plan` 一样单列。
      decisions.push(nodeShape(item));
      continue;
    }
    rows.push(nodeShape(item));
  }
  // 每节点的直接子数（面板要显示"有没有子节点"）。
  const kidsById = new Map();
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
    /** 字段/不变量/操作的自描述，页脚与 `--help` 用同一份（不再手抄）。 */
    schema: describeText(),
  };
}

function load(dir) {
  const nodes = loadNodes(dir);
  return { nodes, tree: buildTree(nodes) };
}

/** One focused node + its parent chain + direct children + sections spliced from the body. */
function buildNodePayload(dir, ref) {
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
  const shape = (x) => ({
    id: x.id ?? nodeIdOf(x.node ?? x),
    short: (x.node ?? x).name.slice(-8),
    title: (x.node ?? x).title,
    type: (x.node ?? x).fields.type,
    status: (x.node ?? x).fields.status,
    mark: STATUS_MARK[(x.node ?? x).fields.status] ?? '?',
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

function sendJson(res, status, body) {
  const text = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(text),
    'Cache-Control': 'no-store',
  });
  res.end(text);
}

/**
 * Only the project's own `public/` directory, looked up through a **whitelist** of
 * names — never by joining the request path. `server.mjs` sits one level above
 * `public/`, so a generic static server here would be one `..` away from serving
 * the source and everything above it.
 */
function serveStatic(res, name) {
  if (!Object.prototype.hasOwnProperty.call(STATIC, name)) {
    sendJson(res, 404, { ok: false, error: `未知资源：${name}` });
    return;
  }
  const file = path.join(PUBLIC_DIR, STATIC[name]);
  let data;
  try {
    data = fs.readFileSync(file);
  } catch (err) {
    sendJson(res, 500, { ok: false, error: `读不到 ${STATIC[name]}：${err.message}` });
    return;
  }
  res.writeHead(200, {
    'Content-Type': MIME[path.extname(file)] ?? 'application/octet-stream',
    'Content-Length': data.length,
    'Cache-Control': 'no-store',
  });
  res.end(data);
}

/** name → file. Whitelist, so the URL never becomes a path. */
const STATIC = {
  '': 'index.html',
  'index.html': 'index.html',
  'app.js': 'app.js',
  'app.css': 'app.css',
};

function createHandler(dir) {
  return (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      const pathname = url.pathname;

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
      if (pathname === '/' || pathname === '/index.html' || STATIC[pathname.slice(1)] !== undefined) {
        serveStatic(res, pathname === '/' ? '' : pathname.slice(1));
        return;
      }
      sendJson(res, 404, { ok: false, error: `未知端点：${pathname}` });
    } catch (err) {
      sendJson(res, 500, { ok: false, error: err && err.message ? err.message : String(err) });
    }
  };
}

export function startServer({ port = DEFAULT_PORT, host = DEFAULT_HOST, dir = DEFAULT_REQUIREMENTS_DIR, log = console.log } = {}) {
  const server = http.createServer(createHandler(dir));
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, () => {
      server.removeListener('error', reject);
      const actual = server.address().port;
      log(`需求台账（只读）：http://${host}:${actual}/`);
      log(`  数据  ${dir}`);
      log(`  API   http://${host}:${actual}/api/tree`);
      log('  停    Ctrl+C');
      resolve(server);
    });
  });
}

function parseCli(argv) {
  const out = { port: DEFAULT_PORT, host: DEFAULT_HOST, dir: undefined };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--port') out.port = Number(argv[++i]);
    else if (a === '--host') out.host = argv[++i];
    else if (a === '--dir') out.dir = path.resolve(argv[++i]);
    else if (a === '--help' || a === '-h') out.help = true;
    else throw new Error(`不认识的选项：${a}`);
  }
  return out;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  let opts;
  try {
    opts = parseCli(process.argv.slice(2));
  } catch (err) {
    process.stderr.write(`${err.message}\n用法：node apps/requirements/server.mjs [--port 7788] [--host 127.0.0.1] [--dir <需求目录>]\n`);
    process.exitCode = 2;
  }
  if (opts?.help) {
    process.stdout.write('用法：node apps/requirements/server.mjs [--port 7788] [--host 127.0.0.1] [--dir <需求目录>]\n\n只读；没有任何写路径。改台账走 `pnpm tools requirements set --write`。\n');
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
      if (err && err.code === 'EADDRINUSE') {
        process.stderr.write(
          `端口 ${opts.port} 已被占用。\n` +
            `这是固定端口的代价：换个端口即可，例如  pnpm tools requirements serve --port ${opts.port + 1}\n`,
        );
      } else {
        process.stderr.write(`启动失败：${err && err.stack ? err.stack : err}\n`);
      }
      process.exitCode = 1;
    }
  }
}
