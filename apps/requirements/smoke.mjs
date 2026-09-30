/**
 * apps/requirements/smoke.mjs — 这个本地网页的**离线自检**。
 *
 * `node apps/requirements/smoke.mjs`
 *
 * 为什么放在项目里而不是 `tools/test/`：
 *   · 项目的自检跟着项目走（`tools/test/**` 由 `pnpm test` 统一跑的是**基建契约**）；
 *   · 它**同进程内** `listen(0)` + `fetch`，不 spawn 子进程 —— 受限沙箱里捕获
 *     子进程输出要开命名管道（`spawn EPERM`），同进程就没这个问题。
 *
 * 它断言的是**真源一致性**，不是"代码看起来对"：
 *   面板/页面显示的数字，必须与 `tools/lib/requirements.mjs` 的聚合**逐节点**一致，
 *   而后者正是 `pnpm tools requirements plan` 用的那份函数。
 */
import assert from 'node:assert/strict';

import {
  DEFAULT_REQUIREMENTS_DIR,
  buildTree,
  flatten,
  loadNodes,
} from '../../tools/lib/requirements.mjs';
import { startServer } from './server.mjs';

let pass = 0;
/** Awaits the callback, so an `async` assertion cannot pass by returning a promise. */
const check = async (label, fn) => {
  try {
    await fn();
    pass += 1;
    console.log(`  ok   ${label}`);
  } catch (err) {
    console.log(`  FAIL ${label}\n       ${err && err.message ? err.message : err}`);
    process.exitCode = 1;
  }
};

const server = await startServer({ port: 0, log: () => {} });
const base = `http://127.0.0.1:${server.address().port}`;
const get = async (p) => {
  const res = await fetch(`${base}${p}`);
  const body = await res.json().catch(() => null);
  return { status: res.status, body, type: res.headers.get('content-type') ?? '' };
};

try {
  console.log(`base ${base}\n\n1) /api/tree 与真源逐节点一致`);
  const tree = await get('/api/tree');
  await check('HTTP 200 + JSON', () => {
    assert.equal(tree.status, 200);
    assert.match(tree.type, /application\/json/);
    assert.equal(tree.body.ok, true);
  });

  // ★ 真源：模型的解析 + 建树 + 聚合（`plan` 用的同一套函数）。
  const nodes = loadNodes(DEFAULT_REQUIREMENTS_DIR);
  const modelTree = buildTree(nodes);
  const modelRows = flatten(nodes, modelTree).filter((i) => i.node.fields.type !== 'decision');
  const modelById = new Map(modelRows.map((i) => [i.node.fields.id ?? `REQ-${i.node.name}`, i]));

  await check(`节点集合一致（api ${tree.body.nodes.length} / 模型 ${modelRows.length}）`, () => {
    assert.equal(tree.body.nodes.length, modelRows.length);
    for (const row of tree.body.nodes) assert.ok(modelById.has(row.id), `模型里没有 ${row.id}`);
  });

  await check('逐节点：窗口 / 标记 / 深度 / 聚合告警 / 父 全一致', () => {
    const bad = [];
    for (const row of tree.body.nodes) {
      const m = modelById.get(row.id);
      const r = m.rollup;
      const done = r.total - r.live;
      if (row.done !== done) bad.push(`${row.short} done ${row.done}≠${done}`);
      if (row.total !== r.total) bad.push(`${row.short} total ${row.total}≠${r.total}`);
      if (row.depth !== m.depth) bad.push(`${row.short} depth ${row.depth}≠${m.depth}`);
      if (row.aggregated !== (r.derivedStatus !== m.node.fields.status)) bad.push(`${row.short} aggregated`);
      const wantParent = m.node.fields.parent === 'null' ? null : m.node.fields.parent;
      if (row.parent !== wantParent) bad.push(`${row.short} parent ${row.parent}≠${wantParent}`);
      if (row.kids !== (modelTree.children.get(m.node.name) ?? []).length) bad.push(`${row.short} kids ${row.kids}`);
    }
    assert.deepEqual(bad, []);
  });

  await check('表头总数一致', () => {
    const totals = { open: 0, doing: 0, blocked: 0, done: 0, dropped: 0, superseded: 0 };
    for (const n of nodes) if (n.fields.status in totals) totals[n.fields.status] += 1;
    for (const [k, v] of Object.entries(totals)) assert.equal(tree.body.totals[k], v, `${k} 不一致`);
    assert.equal(tree.body.totals.count, nodes.length);
  });

  await check('恰好一个根，且没有孤立节点', () => {
    assert.equal(tree.body.nodes.filter((n) => n.parent === null).length, 1);
    assert.deepEqual(tree.body.nodes.filter((n) => n.orphan), []);
  });

  await check('★ 每个节点都带非空 body（详情页正文的前提）', () => {
    const missing = tree.body.nodes.filter((n) => typeof n.body !== 'string' || n.body.trim() === '');
    assert.deepEqual(missing.map((n) => n.id), []);
  });

  await check('short === 名称末 8 位（与 list --json 同源）', () => {
    for (const row of tree.body.nodes) assert.equal(row.short, row.name.slice(-8));
  });

  await check('页脚用的自描述与 --describe 同源', () => {
    assert.ok(typeof tree.body.schema === 'string' && tree.body.schema.includes('data/requirements'));
  });

  console.log(`\n2) /api/node/<ref> —— 详情端点`);
  const sample = tree.body.nodes.find((n) => n.kids > 0) ?? tree.body.nodes[0];
  const byShort = await get(`/api/node/${encodeURIComponent(sample.short)}`);
  await check(`短名可解析：${sample.short}`, () => {
    assert.equal(byShort.status, 200);
    assert.equal(byShort.body.ok, true);
    assert.equal(byShort.body.node.id, sample.id);
  });
  await check('父链是根在前、不含自己（与模型一致）', () => {
    const chain = byShort.body.parentChain;
    assert.deepEqual(chain.map((p) => p.id), chainOfModel(sample, modelById));
    assert.ok(!chain.some((p) => p.id === sample.id));
  });
  await check('子节点与模型的直接子一致', () => {
    const m = modelById.get(sample.id);
    const want = (modelTree.children.get(m.node.name) ?? []).map((k) => k.fields.id ?? `REQ-${k.name}`);
    assert.deepEqual(byShort.body.children.map((c) => c.id), want);
  });
  await check('正文切成小节，且首段落在正文里', () => {
    const sections = byShort.body.node.sections;
    assert.ok(Array.isArray(sections) && sections.length > 0, '至少要有一段');
    const first = sections.find((s) => s.text);
    assert.ok(first, '至少要有一段有文本');
    assert.ok(byShort.body.node.body.includes(first.text.split('\n')[0]));
  });

  const byFull = await get(`/api/node/${encodeURIComponent(sample.id)}`);
  await check('完整 id 解析到同一条', () => assert.equal(byFull.body.node.id, sample.id));

  const missing = await get('/api/node/REQ-00000000000000000000000000');
  await check('未知引用 ⇒ 404 + 可读错误（不是崩）', () => {
    assert.equal(missing.status, 404);
    assert.equal(missing.body.ok, false);
    assert.match(missing.body.error, /找不到/);
  });

  console.log(`\n3) 静态资源与边界`);
  const index = await get('/');
  await check('/ 返回 index.html', () => {
    assert.equal(index.status, 200);
    assert.match(index.type, /text\/html/);
  });
  for (const f of ['app.js', 'app.css']) {
    const r = await get(`/${f}`);
    await check(`/${f} 可取`, () => assert.equal(r.status, 200));
  }

  // 界面入口：只断言"服务出去的字节里有它"，不断言 DOM 行为
  // （那需要真浏览器；见 README「已知边界」）。静态资源是文本，不能用上面的 JSON 助手。
  const getText = async (p) => {
    const res = await fetch(`${base}${p}`);
    return { status: res.status, text: await res.text() };
  };
  const html = await getText('/');
  await check('总览模板带「复制完整 id」按钮', () => assert.match(html.text, /class="copy"/));
  await check('总览模板不再有单独的「打开详情」按钮（点标题即是入口）', () =>
    assert.doesNotMatch(html.text, /打开详情/));

  const js = await getText('/app.js');
  await check('app.js 含复制实现（完整 id，不是短名）', () => {
    assert.match(js.text, /copyText/);
    assert.match(js.text, /navigator\.clipboard/);
  });
  await check('app.js 把 depth 写进行内（树状缩进的数据来源）', () => assert.match(js.text, /--depth/));

  const css = await getText('/app.css');
  await check('app.css 含树状缩进与复制按钮样式', () => {
    assert.match(css.text, /--depth/);
    assert.match(css.text, /\.copy/);
  });

  const traversal = await get('/../tools/lib/requirements.mjs');
  await check('路径穿越 ⇒ 404（只认白名单，不拼路径）', () => assert.equal(traversal.status, 404));
  const nothere = await get('/nope.js');
  await check('未知资源 ⇒ 404', () => assert.equal(nothere.status, 404));

  console.log(`\n${pass} 项通过${process.exitCode ? '（有红）' : ''}`);
} finally {
  await new Promise((resolve) => server.close(resolve));
}

function chainOfModel(node, modelById) {
  const chain = [];
  const seen = new Set([node.id]);
  let cur = modelById.get(node.id);
  for (;;) {
    const p = cur?.node.fields.parent;
    if (p === undefined || p === 'null') break;
    const parent = [...modelById.values()].find(
      (i) => i.node.fields.id === p || i.node.name === p || `REQ-${i.node.name}` === p,
    );
    if (!parent || seen.has(parent.node.fields.id ?? `REQ-${parent.node.name}`)) break;
    const id = parent.node.fields.id ?? `REQ-${parent.node.name}`;
    seen.add(id);
    chain.unshift(id);
    cur = parent;
  }
  return chain;
}
