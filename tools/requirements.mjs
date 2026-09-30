#!/usr/bin/env node
/**
 * tools/requirements.mjs — **CLI**：`data/requirements/` 的查询与唯一编辑入口。
 *
 * 分层（`tools/README.md` §0）：
 *   · 模型（格式 / 不变量 / 树 / 读写 / 自描述）在 `lib/requirements.mjs`；路径在 `lib/paths.mjs`。
 *   · 本文件只做"参数 → 模型 → 输出 + 落盘计划（缺省 dry-run）"，不实现任何规则。
 *
 * 经派发器：`pnpm tools requirements <list|show|plan|validate|describe|add|set> [args…]`
 * 也可独立跑：`node tools/requirements.mjs --plan`
 */
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

import {
  BUDGET,
  DEFAULT_REQUIREMENTS_DIR,
  DOMAIN,
  ID_PREFIX,
  LIVE_STATUSES,
  OPERATIONS,
  STATUS_MARK,
  buildTree,
  deleteNode,
  describe,
  describeText,
  flatten,
  loadNodes,
  nodeId,
  rollup,
  saveNode,
  ulid,
  validateAll,
} from './lib/requirements.mjs';
import { REPO_ROOT } from './lib/paths.mjs';

export { DEFAULT_REQUIREMENTS_DIR, DOMAIN, OPERATIONS, describe, describeText };

// 状态标记表只有一份 —— 在领域模型里（网页页面的图例也用它）。
// 不再本地重定义：两处各写一份必然漂。

// ─────────────────────────────────────────────────────────── 子命令

function cmdList(args, nodes, tree) {
  const rows = nodes.map((n) => {
    const r = rollup(n, tree);
    return {
      id: n.fields.id ?? n.name,
      short: n.name.slice(-8),
      type: n.fields.type ?? '?',
      status: n.fields.status ?? '?',
      severity: n.fields.severity,
      derived: r.derivedStatus,
      parent: n.fields.parent === 'null' ? '（根）' : (n.fields.parent ?? '⚠缺'),
      kids: (tree.children.get(n.name) ?? []).length,
      live: r.live,
      title: n.title,
    };
  });
  if (args.json) return [JSON.stringify(rows, null, 2)];
  const head = ['type', 'id', 'status', '严重', '父', '子', '活', '标题'];
  const cells = rows.map((r) => [
    r.type === 'bug' ? '🐞缺陷' : r.type,
    r.id,
    `${STATUS_MARK[r.status] ?? ''}${r.status}`,
    r.severity === undefined ? '—' : r.severity,
    r.parent,
    String(r.kids),
    String(r.live),
    r.title,
  ]);
  const w = head.map((h, i) => Math.max(h.length, ...cells.map((c) => c[i].length)));
  const line = (c) => c.map((x, i) => x.padEnd(w[i])).join('  ');
  return [line(head), line(w.map((n) => '-'.repeat(n))), ...cells.map(line), '', `${rows.length} 个节点（预算 ${BUDGET.maxNodes}）`];
}

function cmdShow(args, nodes, tree) {
  const ref = args.rest[0];
  if (!ref) throw new Error('--show 需要 <id 或唯一前缀>');
  const n = tree.resolve(ref);
  if (!n) throw new Error(`找不到节点（或前缀不唯一）：${ref}`);
  const r = rollup(n, tree);
  const kids = tree.children.get(n.name) ?? [];
  const L = [`# ${n.title}`, ''];
  for (const [k, v] of Object.entries(n.fields)) L.push(`${k}: ${Array.isArray(v) ? `[${v.join(', ')}]` : v}`);
  L.push('', `文件: ${path.relative(REPO_ROOT, n.file)}  (${n.lines} 行 / 预算 ${BUDGET.maxLinesPerNode})`);
  L.push(`派生: 子树 ${r.total} 个节点，其中未收口 ${r.live} 个${r.derivedStatus !== n.fields.status ? `（聚合状态 ${r.derivedStatus}）` : ''}`);
  if (kids.length) {
    L.push('', '子节点：');
    for (const k of kids) L.push(`  ${STATUS_MARK[k.fields.status] ?? ''} ${k.fields.id}  ${k.title}`);
  }
  L.push('', '--- 正文 ---', n.body);
  return L;
}

/** ★ 进度视图（唯一的进度真源）：按树打印 + 聚合状态 */
function cmdPlan(args, nodes, tree) {
  const L = [];
  const totals = { open: 0, doing: 0, blocked: 0, done: 0, dropped: 0, superseded: 0 };
  let bugs = 0;
  for (const n of nodes) {
    if (n.fields.status in totals) totals[n.fields.status] += 1;
    if (n.fields.type === 'bug' && LIVE_STATUSES.includes(n.fields.status)) bugs += 1;
  }
  L.push(
    `进度（${nodes.length} 个节点 / 预算 ${BUDGET.maxNodes}）　` +
      `🔜 doing ${totals.doing} · ⛔ blocked ${totals.blocked} · ⬜ open ${totals.open} · ✅ done ${totals.done} · 🚫 dropped ${totals.dropped}` +
      (bugs ? `　｜　未收口缺陷 ${bugs} 🐞` : ''),
  );
  L.push('');
  for (const { node: n, depth, rollup: r, orphan } of flatten(nodes, tree)) {
    if (n.fields.type === 'decision') continue; // 决策不属进度树
    const mark = orphan ? '❓' : STATUS_MARK[n.fields.status] ?? '?';
    const kind = n.fields.type === 'bug' ? `🐞${n.fields.severity ?? ''} ` : '';
    const prog = r.total > 0 ? `  [${r.total - r.live}/${r.total}]` : '';
    const flag = r.derivedStatus !== n.fields.status ? `  ⚠ 聚合=${r.derivedStatus}` : '';
    L.push(`${'  '.repeat(Math.max(depth, 0))}${mark} ${n.fields.id}  ${kind}${n.title}${prog}${flag}`);
  }
  const decisions = nodes.filter((n) => n.fields.type === 'decision');
  if (decisions.length) {
    L.push('', `决策（${decisions.length}，不属进度树）：`);
    for (const d of decisions) L.push(`  · ${d.fields.id}  ${d.title}${d.fields.status === 'superseded' ? '（已被取代）' : ''}`);
  }
  return L;
}

function printReport(report, { json }) {
  if (json) return [JSON.stringify(report, null, 2)];
  const L = [];
  for (const c of report.checks) {
    const tag = c.problems.length ? 'FAIL' : 'ok  ';
    L.push(`[${tag}] #${c.id} ${c.text}`);
    if (c.problems.length) for (const p of c.problems) L.push(`        · ${p}`);
  }
  L.push('', `${report.checks.length} 条不变量：${report.checks.length - report.failures} 通过 / ${report.failures} 失败`);
  return L;
}

function cmdValidate(args, nodes) {
  const report = validateAll(nodes, { repoRoot: REPO_ROOT });
  return { plan: printReport(report, args), code: report.failures > 0 ? 1 : 0 };
}

/** 把 flag 名 → 字段名的映射（`--status done`、`--tags a,b`、`--blocked-by x,y`、`--repro 路径#锚点`） */
const FIELD_FLAGS = {
  id: 'id',
  title: 'title',
  type: 'type',
  status: 'status',
  parent: 'parent',
  order: 'order',
  tags: 'tags',
  verify: 'verify',
  repro: 'repro',
  severity: 'severity',
  'done-reason': 'done_reason',
  'dropped-reason': 'dropped_reason',
  supersedes: 'supersedes',
  'blocked-by': 'blocked_by',
  body: 'body',
  body_file: 'body_file',
};

function collectFields(args) {
  const out = {};
  for (const [k, v] of Object.entries(args.flags)) {
    if (k === 'dir' || k === 'unset') continue; // 全局选项，不是节点字段
    const field = FIELD_FLAGS[k];
    if (field === undefined) throw new Error(`不认识的选项：--${k}`);
    out[field] = v;
  }
  return out;
}

function readBody(spec, fallback = null) {
  if (spec === undefined || spec === null) return fallback;
  if (spec === '-') return fs.readFileSync(0, 'utf8').replace(/\r\n/g, '\n').trim();
  const abs = path.resolve(spec);
  if (!fs.existsSync(abs)) throw new Error(`--body 指向的文件不存在：${abs}`);
  return fs.readFileSync(abs, 'utf8').replace(/\r\n/g, '\n').trim();
}

function cmdAdd(args, nodes, tree) {
  const f = collectFields(args);
  if (!f.title) throw new Error('--add 需要 --title');
  if (f.title.startsWith(ID_PREFIX)) throw new Error('--title 是标题，不是 id');
  // 身份：缺省由本工具生成；给了 --id 就以调用方为准（种子/迁移需要预先知道 id 才能写父子引用）
  let u;
  if (f.id !== undefined) {
    const bare = f.id.startsWith(ID_PREFIX) ? f.id.slice(ID_PREFIX.length) : f.id;
    if (!/^[0-9A-HJKMNP-TV-Z]{26}$/.test(bare)) {
      throw new Error(`--id 形态非法：${f.id}（应为 ${ID_PREFIX}<26 字符 Crockford base32>）`);
    }
    if (nodes.some((n) => n.name === bare)) throw new Error(`--id 已被占用：${nodeId(bare)}`);
    u = bare;
  } else {
    u = ulid();
  }
  const fields = { id: nodeId(u), type: f.type ?? 'req', status: f.status ?? 'open' };
  if (f.parent !== undefined) {
    if (f.parent !== 'null' && !tree.resolve(f.parent)) throw new Error(`--parent 指向的节点不存在：${f.parent}`);
    fields.parent = f.parent === 'null' ? 'null' : tree.resolve(f.parent).fields.id;
  }
  if (f.order !== undefined) fields.order = String(f.order);
  for (const k of ['tags', 'blocked_by', 'supersedes']) {
    if (f[k] !== undefined) fields[k] = String(f[k]).split(',').map((s) => s.trim()).filter(Boolean);
  }
  for (const k of ['verify', 'repro', 'severity', 'done_reason', 'dropped_reason']) if (f[k] !== undefined) fields[k] = f[k];
  const body =
    readBody(f.body_file ?? f.body, null) ??
    (fields.type === 'bug'
      ? '## 复现\n\n（待写：怎么观测到这个分歧，越短越好）\n\n## 期望 / 实际\n\n（待写）\n\n## 影响\n\n（待写）'
      : '## 判据\n\n（待写：怎么算做完，要可核对）\n\n## 范围 / 非目标\n\n（待写）');
  const node = { name: u, title: f.title, fields, body };
  const plan = [`add ${fields.id}  ${f.title}`, `  parent: ${fields.parent ?? '(缺！)'}  type: ${fields.type}  status: ${fields.status}`, `  → ${path.relative(REPO_ROOT, path.join(args.dir, `${u}.md`))}`];
  return {
    plan,
    apply: () => {
      const res = saveNode(node, args.dir);
      if (!res.ok) throw new Error(res.reason);
      const after = loadNodes(args.dir);
      const report = validateAll(after, { repoRoot: REPO_ROOT });
      if (report.failures > 0) {
        // ★ 写后守卫没过 ⇒ 必须把这颗新节点**整颗删掉**（否则树上留一个守卫不接受的文件）
        deleteNode(u, args.dir);
        return { rollback: true, report, removed: nodeId(u) };
      }
      return { report };
    },
  };
}

function cmdSet(args, nodes, tree) {
  const ref = args.rest[0];
  if (!ref) throw new Error('--set 需要 <id 或唯一前缀>');
  const n = tree.resolve(ref);
  if (!n) throw new Error(`找不到节点（或前缀不唯一）：${ref}`);
  if (args.rest.length > 1) throw new Error(`--set 只改一个节点，多了：${args.rest.slice(1).join(' ')}`);
  const f = collectFields(args);
  // ★ `--unset <字段>`：把字段**删掉**（"改成空值"不是同一件事 —— 空值会被守卫判非法）
  const unset = String(args.flags.unset ?? '')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
    .map((k) => {
      const field = FIELD_FLAGS[k];
      if (field === undefined || field === 'body' || field === 'body_file' || field === 'title') {
        throw new Error(`--unset 只接受字段名（不能 unset body/title）：${k}`);
      }
      return field;
    });
  const fields = { ...n.fields };
  const plan = [];
  const next = { ...n, fields };
  let bodyTouched = false;
  for (const [k, v] of Object.entries(f)) {
    if (k === 'title') {
      plan.push(`title: ${n.title} → ${v}`);
      next.title = v;
      continue;
    }
    if (k === 'body') {
      next.body = readBody(v, next.body);
      bodyTouched = true;
      plan.push(`body: 替换（${next.body.split('\n').length} 行）`);
      continue;
    }
    if (k === 'body_file') continue;
    if (k === 'parent' && v !== 'null') {
      const p = tree.resolve(v);
      if (!p) throw new Error(`--parent 指向的节点不存在：${v}`);
      if (p.name === n.name) throw new Error('parent 不能是自己');
      if (reachable(p, n, tree)) throw new Error(`会把 ${n.name} 挂到自己的子孙下（成环）`);
      fields.parent = p.fields.id;
    } else if (k === 'parent') {
      fields.parent = 'null';
    } else if (['tags', 'blocked_by', 'supersedes'].includes(k)) {
      fields[k] = String(v).split(',').map((s) => s.trim()).filter(Boolean);
    } else if (k === 'order') {
      fields.order = String(v);
    } else {
      fields[k] = v;
    }
    plan.push(`${k}: ${JSON.stringify(n.fields[k] ?? null)} → ${JSON.stringify(fields[k])}`);
  }
  for (const k of unset) {
    if (fields[k] === undefined) {
      plan.push(`${k}: （本来就没有）`);
      continue;
    }
    plan.push(`${k}: ${JSON.stringify(fields[k])} → 删除`);
    delete fields[k];
  }
  // 收口/重开时把互相矛盾的字段清掉，免得守卫报"写了 done_reason 但 status=doing"
  if (fields.status !== undefined && fields.status !== n.fields.status) {
    if (fields.status !== 'done') delete fields.done_reason;
    if (fields.status !== 'dropped') delete fields.dropped_reason;
    if (fields.status !== 'blocked') delete fields.blocked_by;
  }
  if (!bodyTouched && plan.length === 0) throw new Error('--set 没有给出任何改动');
  return {
    plan,
    apply: () => {
      const res = saveNode(next, args.dir);
      if (!res.ok) throw new Error(res.reason);
      const report = validateAll(loadNodes(args.dir), { repoRoot: REPO_ROOT });
      if (report.failures > 0) {
        // ★ 写后守卫没过 ⇒ 把**原节点原样写回**（不是"回滚报告"了事）
        const back = saveNode(n, args.dir);
        if (!back.ok) throw new Error(`回滚失败（节点 ${n.name} 现在是脏的）：${back.reason}`);
        return { rollback: true, report, restored: nodeId(n.name) };
      }
      return { report };
    },
  };
}

/** p 是不是 n 的祖先（用来拒绝把 n 挂到自己的子孙下） */
function reachable(p, n, tree) {
  let cur = p;
  const seen = new Set();
  while (cur) {
    if (cur.name === n.name) return true;
    if (seen.has(cur.name)) return false;
    seen.add(cur.name);
    const parentRef = cur.fields.parent;
    if (parentRef === undefined || parentRef === 'null') return false;
    cur = tree.resolve(parentRef);
  }
  return false;
}

// ─────────────────────────────────────────────────────────── CLI
function parseArgs(argv) {
  const out = { action: null, write: false, json: false, rest: [], flags: {}, dir: DEFAULT_REQUIREMENTS_DIR };
  const takesValue = new Set(['id', 'title', 'type', 'status', 'parent', 'order', 'tags', 'verify', 'repro', 'severity', 'done-reason', 'dropped-reason', 'supersedes', 'blocked-by', 'unset', 'body', 'body_file', 'dir', 'port']);
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (['--list', '--plan', '--validate', '--describe', '--serve'].includes(a)) out.action = a.slice(2);
    else if (a === '--show' || a === '--add' || a === '--set') {
      out.action = a.slice(2);
      if (a !== '--add') out.rest.push(argv[++i]);
    } else if (a === '--write') out.write = true;
    else if (a === '--json') out.json = true;
    else if (a === '--help' || a === '-h') out.action = 'help';
    else if (a.startsWith('--')) {
      const key = a.slice(2);
      if (!takesValue.has(key)) throw new Error(`不认识的选项：${a}`);
      out.flags[key] = argv[++i];
    } else out.rest.push(a);
  }
  if (out.flags.dir) out.dir = path.resolve(out.flags.dir);
  if (!out.action) out.action = 'plan';
  return out;
}

/**
 * ★ `--serve` 的**薄启动器**。
 *
 * 网页是**独立项目** `apps/requirements/`（服务 + 客户端），不是 `tools/` 的一份子：
 * `test/layering.test.mjs` 要求每个 `tools/*.mjs` 只 import `./lib/*`，所以这里
 * **不能** import 那个项目 —— 只能 spawn 它，并把端口原样转发。
 *
 * `stdio: 'inherit'` 而不是捕获：受限沙箱里捕获子进程输出要开命名管道 ⇒ `spawn EPERM`。
 * 于是"启动时打印端口"这件事由服务自己写进这个终端，本文件不代抄一遍。
 */
function serveWeb(args) {
  const server = path.join(REPO_ROOT, 'apps', 'requirements', 'server.mjs');
  if (!fs.existsSync(server)) {
    process.stderr.write(`找不到网页服务：${server}\n（apps/requirements/ 就是这个项目）\n`);
    return 2;
  }
  const argv = [server];
  const port = args.flags.port;
  if (port !== undefined) {
    if (!/^\d+$/.test(String(port))) {
      process.stderr.write(`--port 必须是端口号（0-65535），实际：${port}\n`);
      return 2;
    }
    argv.push('--port', String(port));
  }
  const child = spawn(process.execPath, argv, { stdio: 'inherit', cwd: REPO_ROOT });
  return new Promise((resolve) => {
    child.on('error', (err) => {
      process.stderr.write(`启动网页服务失败：${err.message}\n`);
      resolve(2);
    });
    child.on('exit', (code, signal) => {
      if (signal) {
        process.stderr.write(`\n网页服务被 ${signal} 结束。\n`);
        resolve(0);
        return;
      }
      resolve(code ?? 0);
    });
  });
}

const HELP = `tools/requirements.mjs — data/requirements/ 的查询与唯一编辑入口（缺省动作 plan；缺省 dry-run）

  node tools/requirements.mjs --plan                  # ★ 进度视图（唯一的进度真源）
  node tools/requirements.mjs --list [--json]         # 一览
  node tools/requirements.mjs --show <id|前缀>         # 一个节点：字段 + 子树 + 正文
  node tools/requirements.mjs --validate [--json]     # 全部不变量（红 = 退出码 1）
  node tools/requirements.mjs --serve [--port 7788]   # 本地只读网页（总览 + 详情），项目在 apps/requirements/
  node tools/requirements.mjs --add --title <标题> [--parent <id>] [--type req] [--status open]
                                       [--order n] [--tags a,b] [--verify 路径#测试名] [--body <md 文件>] [--write]
  node tools/requirements.mjs --set <id|前缀> [--status doing] [--verify …] [--parent …] [--title …]
                                       [--tags a,b] [--unset tags,blocked_by] [--body <md 文件>] [--write]
  pnpm tools requirements describe                    # 自描述：字段 / 不变量 / 预算 / 操作

★ 字段与不变量只有一份真源：本文件的 --describe（文档 data/requirements/README.md 只写口径与指向）。
★ 层次：parent 只写在子节点上；children 一律派生。预算：单节点 ≤ ${BUDGET.maxLinesPerNode} 行、总数 ≤ ${BUDGET.maxNodes}。
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
  if (args.action === 'add' && !args.write) {
    // dry-run：也要先算一遍，才能报告"要是写会怎样"
  }

  // ★ 网页服务：这是个**长驻进程**，而且是独立项目（`apps/requirements/`）。
  //   本文件只做"薄启动器"——spawn 它、原样转发，不实现任何 HTTP / 渲染规则
  //   （否则 CLI 就要 import 模型之外的东西，`test/layering.test.mjs` 会红）。
  if (args.action === 'serve') return serveWeb(args);

  const nodes = loadNodes(args.dir);
  const tree = buildTree(nodes);

  if (args.action === 'validate') {
    const { plan, code } = cmdValidate(args, nodes);
    for (const l of plan) process.stdout.write(`${l}\n`);
    return code;
  }
  if (args.action === 'list') {
    for (const l of cmdList(args, nodes, tree)) process.stdout.write(`${l}\n`);
    return 0;
  }
  if (args.action === 'show') {
    for (const l of cmdShow(args, nodes, tree)) process.stdout.write(`${l}\n`);
    return 0;
  }
  if (args.action === 'plan') {
    for (const l of cmdPlan(args, nodes, tree)) process.stdout.write(`${l}\n`);
    return 0;
  }
  const cmds = { add: () => cmdAdd(args, nodes, tree), set: () => cmdSet(args, nodes, tree) };
  const cmd = cmds[args.action];
  if (!cmd) {
    process.stderr.write(`未知动作：${args.action}\n${HELP}`);
    return 2;
  }
  const { plan, apply } = cmd();
  for (const l of plan) process.stdout.write(`${l}\n`);
  if (!args.write) {
    process.stdout.write('\n（dry-run）加 --write 落盘。\n');
    return 0;
  }
  const res = apply();
  if (res?.rollback) {
    process.stderr.write(`写后守卫未过 —— 已回滚（节点文件恢复原状）：\n`);
    for (const l of printReport(res.report, {})) process.stderr.write(`${l}\n`);
    return 1;
  }
  process.stdout.write('\n已落盘。\n');
  for (const l of printReport(res.report, {})) process.stdout.write(`${l}\n`);
  return res.report.failures > 0 ? 1 : 0;
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
