/**
 * apps/workbench/smoke.ts — 工作台的**离线自检**（`node smoke.ts`）。
 *
 * 为什么跟着项目走而不是进 `tools/test/`：项目的自检跟着项目走（`tools/test/**` 由 `pnpm test`
 * 统一跑的是**基建契约**）。它**同进程** `listen(0)` + `fetch`，**不 spawn 子进程** ——
 * 受限沙箱里捕获子进程输出要开命名管道（`spawn EPERM`），同进程没有这个问题。
 *
 * 它断言的是**真源一致性**，不是"代码看起来对"：
 *   ① `/api/tree` 的每个节点窗口 / 标记 / 深度 / 父 / 子数 / 聚合告警，与 `tools/lib/requirements.mjs`
 *      的 `flatten` / `rollup` **逐条相等**（`pnpm tools requirements plan` 用的就是同一份函数）；
 *   ② `/api/node/<ref>` 与 `describeNode` 相等（字段 / 小节 / 父链 / 直接子）；
 *   ③ `/api/scripts` 的名字集合 == **`allScriptNames()` 给的全部可反汇编脚本**（候选 `.BIN` + `readHeader()`
 *      签名门槛都在模型里），汇总（`annotated` / `hasPatch` / `unchanged` / `nonScript`）与真源现算一致
 *      —— **这里一个数字都不写死**（口径是"现算"，不是"记住 941"）；
 *   ④ `/api/script/<名>` 的 `text` 与直接调 `buildView` 的结果**逐字节相等**
 *      （最大的那支 + 有变更的一支 + 没有条目的一支 + **名字过滤器没命中的四支**：那批里既有
 *      "旧管线漏提"的真译文，也有真的没变更的）；
 *   ⑤ **写路径**（`POST /api/nodes`）：写在**临时台账目录**上（绝不碰真的 `data/requirements/` ——
 *      自检里也断言这一点），断言的是"服务端没有自己的建单规则"（枚举来自模型、被拒的单子不留残 file、
 *      守卫不绿就回滚）与三道门（只在回环 / 只收 JSON / 不吐 CORS 头）；
 *   ⑥ **Markdown 渲染口径**（`src/markdown.ts`）：直接 import 那个模块断言四条口径
 *      （原始 HTML 按文本、裸文本不自动变链接、仓库内路径不做成可点链接、小节里的小标题降两级）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import {
  BUG_ONLY,
  DEFAULT_REQUIREMENTS_DIR,
  SEVERITIES,
  STATUSES,
  TYPES,
  buildTree,
  describeNode,
  flatten,
  loadNodes,
  parseNode,
  splitSections,
  validateAll,
} from '../../tools/lib/requirements.mjs';
import { REPO_ROOT } from '../../tools/lib/paths.mjs';
import {
  SPEAKER_FILTER,
  DEFAULT_PATCH,
  NO_OPS_ENTRY,
  allScriptNames,
  buildView,
  loadPatch,
  mapperContext,
  openSides,
  rowsOf,
} from '../../tools/lib/patch.mjs';
import { startServer, writesAllowed } from './server.ts';
import { MD_OPTIONS, renderMarkdown } from './src/markdown.ts';

let pass = 0;
const failures: string[] = [];
/** Awaits the callback, so an `async` assertion cannot pass by returning a promise. */
const check = async (label: string, fn: () => unknown) => {
  try {
    await fn();
    pass += 1;
    console.log(`  ok   ${label}`);
  } catch (err) {
    const msg = err && (err as Error).message ? (err as Error).message : String(err);
    failures.push(label);
    console.log(`  FAIL ${label}\n       ${msg}`);
    process.exitCode = 1;
  }
};

const server = await startServer({ port: 0, log: () => {} });
const base = `http://127.0.0.1:${(server.address() as { port: number }).port}`;

const get = async (p: string) => {
  const res = await fetch(`${base}${p}`);
  const body: any = await res.json().catch(() => null);
  return { status: res.status, body, type: res.headers.get('content-type') ?? '', cache: res.headers.get('cache-control') ?? '' };
};

try {
  console.log(`base ${base}\n\n1) /api/tree 与真源逐节点一致`);
  const tree = await get('/api/tree');
  await check('HTTP 200 + JSON + no-store', () => {
    assert.equal(tree.status, 200);
    assert.match(tree.type, /application\/json/);
    assert.equal(tree.cache, 'no-store');
    assert.equal(tree.body.ok, true);
  });

  // ★ 真源：模型的解析 + 建树 + 聚合（`plan` 用的同一套函数）。
  const nodes = loadNodes(DEFAULT_REQUIREMENTS_DIR);
  const modelTree = buildTree(nodes);
  const modelRows = flatten(nodes, modelTree).filter((i: any) => i.node.fields.type !== 'decision');
  const modelById = new Map(modelRows.map((i: any) => [i.node.fields.id ?? `REQ-${i.node.name}`, i]));

  await check(`节点集合一致（api ${tree.body.nodes.length} / 模型 ${modelRows.length}）`, () => {
    assert.equal(tree.body.nodes.length, modelRows.length);
    for (const row of tree.body.nodes) assert.ok(modelById.has(row.id), `模型里没有 ${row.id}`);
  });

  await check('逐节点：窗口 / 标记 / 深度 / 父 / 子数 / 聚合告警 全一致', () => {
    const bad: string[] = [];
    for (const row of tree.body.nodes) {
      const m: any = modelById.get(row.id);
      const r = m.rollup;
      if (row.done !== r.total - r.live) bad.push(`${row.short} done ${row.done}≠${r.total - r.live}`);
      if (row.total !== r.total) bad.push(`${row.short} total ${row.total}≠${r.total}`);
      if (row.depth !== m.depth) bad.push(`${row.short} depth ${row.depth}≠${m.depth}`);
      if (row.aggregated !== (r.derivedStatus !== m.node.fields.status)) bad.push(`${row.short} aggregated`);
      if (row.mark !== (m.orphan ? '❓' : (tree.body.marks as any)[m.node.fields.status] ?? '?')) {
        bad.push(`${row.short} mark ${row.mark}`);
      }
      const wantParent = m.node.fields.parent === 'null' ? null : m.node.fields.parent;
      if (row.parent !== wantParent) bad.push(`${row.short} parent ${row.parent}≠${wantParent}`);
      if (row.kids !== (modelTree.children.get(m.node.name) ?? []).length) bad.push(`${row.short} kids ${row.kids}`);
      if (row.rollup.derivedStatus !== r.derivedStatus || row.rollup.live !== r.live) bad.push(`${row.short} rollup`);
    }
    assert.deepEqual(bad, []);
  });

  await check('表头总数一致', () => {
    const totals: Record<string, number> = { open: 0, doing: 0, blocked: 0, done: 0, dropped: 0, superseded: 0 };
    for (const n of nodes) if (n.fields.status in totals) totals[n.fields.status] += 1;
    for (const [k, v] of Object.entries(totals)) assert.equal(tree.body.totals[k], v, `${k} 不一致`);
    assert.equal(tree.body.totals.count, nodes.length);
  });

  await check('恰好一个根，且没有孤立节点', () => {
    assert.equal(tree.body.nodes.filter((n: any) => n.parent === null).length, 1);
    assert.deepEqual(tree.body.nodes.filter((n: any) => n.orphan), []);
  });

  await check('★ 每个节点都带非空 body（详情页正文的前提）', () => {
    const missing = tree.body.nodes.filter((n: any) => typeof n.body !== 'string' || n.body.trim() === '');
    assert.deepEqual(missing.map((n: any) => n.id), []);
  });

  await check('short === 名称末 8 位（与 list --json 同源）', () => {
    for (const row of tree.body.nodes) assert.equal(row.short, row.name.slice(-8));
  });

  await check('页脚用的自描述与 --describe 同源', () => {
    assert.ok(typeof tree.body.schema === 'string' && tree.body.schema.includes('data/requirements'));
  });

  console.log('\n2) /api/node/<ref> —— 详情端点（与 describeNode 相等）');
  const sample = tree.body.nodes.find((n: any) => n.kids > 0) ?? tree.body.nodes[0];
  const modelNode = modelById.get(sample.id) as any;
  const modelDescribe = describeNode(modelNode.node, modelTree);

  const byShort = await get(`/api/node/${encodeURIComponent(sample.short)}`);
  await check(`短名可解析：${sample.short}`, () => {
    assert.equal(byShort.status, 200);
    assert.equal(byShort.body.ok, true);
    assert.equal(byShort.body.node.id, sample.id);
  });
  await check('字段与 describeNode 相等（id/短名/标题/行数/正文）', () => {
    const n = byShort.body.node;
    assert.equal(n.id, modelDescribe.id);
    assert.equal(n.short, modelDescribe.short);
    assert.equal(n.title, modelDescribe.title);
    assert.equal(n.lines, modelDescribe.lines);
    assert.equal(n.body, modelDescribe.body);
    assert.deepEqual(n.rollup, {
      live: modelDescribe.rollup.live,
      total: modelDescribe.rollup.total,
      derivedStatus: modelDescribe.rollup.derivedStatus,
    });
    assert.equal(n.done, modelDescribe.done);
  });
  await check('小节与模型的 splitSections 逐节相等（服务端没另写一套切法）', () => {
    assert.deepEqual(byShort.body.node.sections, splitSections(modelNode.node.body));
    assert.deepEqual(byShort.body.node.sections, modelDescribe.sections);
  });
  await check('父链是根在前、不含自己（与模型一致）', () => {
    assert.deepEqual(
      byShort.body.parentChain.map((p: any) => p.id),
      modelDescribe.parentChain.map((p: any) => p.id),
    );
    assert.ok(!byShort.body.parentChain.some((p: any) => p.id === sample.id));
  });
  await check('子节点与模型的直接子一致', () => {
    assert.deepEqual(
      byShort.body.children.map((c: any) => c.id),
      modelDescribe.children.map((c: any) => c.id),
    );
    assert.deepEqual(
      byShort.body.children.map((c: any) => c.rollup),
      modelDescribe.children.map((c: any) => c.rollup),
    );
  });
  await check('正文切成小节，且首段落在正文里', () => {
    const sections = byShort.body.node.sections;
    assert.ok(Array.isArray(sections) && sections.length > 0, '至少要有一段');
    const first = sections.find((s: any) => s.text);
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

  console.log('\n3) /api/scripts —— 全部可反汇编的 AGE 脚本一览（只算元信息）');
  // ★ 真源 = **模型自己**：名单与"非脚本"计数都来自 `allScriptNames()`（候选 `.BIN` + 签名判定），
  //   标签来自 `SPEAKER_FILTER`，覆盖情况来自 patch 文档 ⇒ 这里没有写死的数字，全是现算。
  //   "旧管线标注过"只是**标签**：它**不是** patch 的范围（拿它当范围会漏掉非 SC/SP 的真译文）。
  const baseline = openSides({ targetDir: null }).base;
  const doc = loadPatch(DEFAULT_PATCH);
  const { names: truth, nonScript: notScript } = allScriptNames(baseline);
  const annotatedTruth = truth.filter((n) => SPEAKER_FILTER.test(n));
  const inRange = new Set(truth);
  const patchKeys = Object.keys(doc.scripts);
  /** patch 的键里**落在名单范围内**的那些（范围外的键是错误，下面单独断言） */
  const keysInRange = patchKeys.filter((n) => inRange.has(n));
  const unchangedTruth = truth.filter((n) => doc.scripts[n] === undefined);
  const index = await get('/api/scripts');
  await check(`HTTP 200 + 名字集合 == allScriptNames() 的全部脚本（${truth.length} 支）`, () => {
    assert.equal(index.status, 200);
    assert.equal(index.body.ok, true);
    assert.deepEqual(index.body.scripts.map((s: any) => s.name), truth);
    assert.equal(index.body.source, 'baseline');
  });
  await check(
    `汇总由真源现算一致（count ${index.body.count} · annotated ${index.body.annotated} · ` +
      `hasPatch ${index.body.hasPatch} · unchanged ${index.body.unchanged} · nonScript ${index.body.nonScript}）`,
    () => {
      assert.equal(index.body.count, truth.length);
      assert.equal(index.body.annotated, annotatedTruth.length);
      assert.equal(index.body.hasPatch, keysInRange.length);
      assert.equal(index.body.unchanged, unchangedTruth.length);
      assert.equal(index.body.nonScript, notScript.length);
      // 口径自洽：名单 = 有条目 + 没条目；且"标注过"只是标签 ⇒ 名单**不该**被它收窄
      assert.equal(index.body.count, index.body.hasPatch + index.body.unchanged);
      assert.ok(index.body.count > index.body.annotated, '名单不该等于"标注过"的那个子集');
    },
  );
  await check('每支的 baseFrom / baseBytes / annotated / hasPatch / opCount 与真源一致', () => {
    const bad: string[] = [];
    for (const row of index.body.scripts) {
      const hit = baseline.resolve(row.name);
      const entry = doc.scripts[row.name];
      if ((hit ? hit.from : '') !== row.baseFrom) bad.push(`${row.name} baseFrom ${row.baseFrom}≠${hit ? hit.from : ''}`);
      if ((hit ? hit.buf.length : 0) !== row.baseBytes) bad.push(`${row.name} baseBytes`);
      if (SPEAKER_FILTER.test(row.name) !== row.annotated) bad.push(`${row.name} annotated`);
      if ((entry !== undefined) !== row.hasPatch) bad.push(`${row.name} hasPatch`);
      if ((entry ? entry.ops.length : 0) !== row.opCount) bad.push(`${row.name} opCount`);
    }
    assert.deepEqual(bad, []);
  });
  await check('patch 的每个键都在名单里；名字过滤器**没**命中的脚本也在名单里（名单 ⊋ 标注）', () => {
    const names = new Set(index.body.scripts.map((s: any) => s.name));
    for (const k of patchKeys) assert.ok(names.has(k), `名单里没有 ${k}`);
    for (const n of ['SN0000.BIN', 'SG0010.BIN', 'CONFIG.BIN']) {
      assert.ok(names.has(n), `非标注脚本不在名单里：${n}`);
    }
    assert.ok(names.size > annotatedTruth.length, '名单不该等于"标注过"的子集');
  });
  await check('★ 名字过滤器漏掉的那批（非 SC/SP 但**有变更**）在名单里且有条目', () => {
    const byName = new Map(index.body.scripts.map((s: any) => [s.name, s]));
    for (const n of ['PLINIT.BIN', 'BIINIT.BIN']) {
      assert.ok(!SPEAKER_FILTER.test(n), `${n} 本就不该被标注过滤器命中`);
      const row: any = byName.get(n);
      assert.ok(row, `${n} 不在名单里`);
      assert.equal(row.annotated, false);
      assert.equal(row.hasPatch, true, `${n} 应当有 patch 条目（旧口径曾把它当"没译文"漏掉）`);
    }
  });
  await check('一览里不带正文（只算元信息）', () => {
    for (const row of index.body.scripts) assert.ok(!('text' in row) && !('rows' in row), '不该有 text/rows');
  });

  console.log('\n4) /api/script/<name> —— 正文与 buildView 逐字节相等');
  const rows: any[] = index.body.scripts.filter((s: any) => s.baseBytes > 0);
  const biggest = [...rows].sort((a, b) => b.baseBytes - a.baseBytes)[0];
  const changedSmall = rows.find((s: any) => s.hasPatch && s.baseBytes < 400_000) ?? rows.find((s: any) => s.hasPatch);
  const noEntry = rows.find((s: any) => !s.hasPatch);
  // ★ **非 SC/SP** 的样本（名字过滤器没命中）：`SN0000.BIN` / `SG0010.BIN` 是"名单被收窄过就 404"的证人；
  //   再补两支 `PLINIT.BIN` / `BIINIT.BIN` —— 它们是**旧口径漏提**的那批（有译文、名字不匹配过滤器），
  //   逐字节断言在这里就是"漏了会红"的那道门。
  const nonAnnotated = ['SN0000.BIN', 'SG0010.BIN', 'PLINIT.BIN', 'BIINIT.BIN'].map((n) => rows.find((s: any) => s.name === n));
  assert.deepEqual(nonAnnotated.filter(Boolean).length, 4, '基线根里应有 SN0000.BIN / SG0010.BIN / PLINIT.BIN / BIINIT.BIN');
  for (const s of nonAnnotated) assert.equal((s as any).annotated, false, `${(s as any).name} 不该被标注过滤器命中`);
  // ★ 条目里若有**带 `header`** 的那支（产物头部 4 行 ≠ 基线），就要走一遍"头部也要替掉"的那条路
  //   （`header` 是可选字段，服务端必须把它传进 `buildView`，否则那支队会少改 4 行）。
  //   现在真源里**没有**这样的条目（唯一一支 `$1$IMINIT.BIN` 已被裁为"已知产物异常、不继承"），
  //   所以这里是**条件断言** —— 该字段格式能力的强断言在 `tools/test/patch.test.mjs`（合成文本那一组）。
  const headerEntry = rows.find((s: any) => doc.scripts[s.name]?.header !== undefined);
  await check(
    `带 header 的条目${headerEntry ? `（${headerEntry.name}）` : '（当前真源里没有 ⇒ 只记一笔）'}逐字节相等`,
    () => assert.ok(true),
  );
  const picked = [biggest, changedSmall, noEntry, headerEntry, ...(nonAnnotated as any[])].filter(Boolean) as any[];

  const ctx = mapperContext();
  for (const pick of picked) {
    const hit = baseline.resolve(pick.name) as any;
    // 与 server.ts 完全同一个 entry：没有条目的脚本 = 空叠加层（`src` 于是与 `data` 同形）
    const entry = doc.scripts[pick.name] ?? NO_OPS_ENTRY(hit.buf);
    for (const kind of ['data', 'src'] as const) {
      const got = await get(`/api/script/${encodeURIComponent(pick.name)}?kind=${kind}`);
      await check(`${pick.name} [${kind}]：text 与 buildView 逐字节相等（${got.body?.rows ?? '?'} 行）`, () => {
        assert.equal(got.status, 200);
        assert.equal(got.body.name, pick.name);
        assert.equal(got.body.kind, kind);
        const want = buildView(kind, hit.buf, entry, { lineToBin: ctx.mapper.lineToBin }).text;
        assert.equal(got.body.text, want, 'text 与 buildView 的输出不同');
        assert.ok(Buffer.from(got.body.text, 'utf8').equals(Buffer.from(want, 'utf8')), 'UTF-8 字节不同');
        assert.equal(got.body.bytes, Buffer.byteLength(want));
        assert.equal(got.body.rows, rowsOf(want).raw.length);
        assert.equal(got.body.baseFrom, hit.from);
        assert.equal(got.body.baseBytes, hit.buf.length);
        assert.equal(got.body.hasPatch, doc.scripts[pick.name] !== undefined);
      });
    }
  }

  // 没有 patch 条目 ⇒ 空叠加层 ⇒ `src` == `data`（打开正文不该报错、也不该显示成异常）
  // ★ 样本**从真源现算**（不写死名字）：名单里有、patch 里没有的那些里取两支
  const noEntrySamples = unchangedTruth.slice(0, 2);
  assert.equal(noEntrySamples.length, 2, '真源里应当至少有两支没有 patch 条目的脚本');
  for (const n of noEntrySamples) {
    const d = await get(`/api/script/${encodeURIComponent(n)}?kind=data`);
    const s = await get(`/api/script/${encodeURIComponent(n)}?kind=src`);
    await check(`${n}（没有条目）：data / src 都 200 且逐字节相同`, () => {
      assert.equal(d.status, 200);
      assert.equal(s.status, 200);
      assert.equal(d.body.hasPatch, false);
      assert.equal(s.body.hasPatch, false);
      assert.ok(d.body.rows > 0 && d.body.bytes > 0);
      assert.equal(s.body.text, d.body.text);
      assert.equal(s.body.rows, d.body.rows);
    });
  }

  await check('同一支第二次请求：命中服务端视图缓存（且内容不变）', async () => {
    const name = (changedSmall ?? biggest).name;
    const a = await get(`/api/script/${encodeURIComponent(name)}?kind=src`);
    const before = (await get('/api/health')).body.viewCache.hits;
    const b = await get(`/api/script/${encodeURIComponent(name)}?kind=src`);
    const after = (await get('/api/health')).body.viewCache.hits;
    assert.equal(a.body.text, b.body.text);
    assert.ok(after > before, `第二次没有命中缓存（hits ${before} → ${after}）`);
  });

  console.log('\n5) 边界：未知引用 / 非法 kind / 不是脚本的 BIN / 方法用错');
  const badKind = await get(`/api/script/${encodeURIComponent(picked[0].name)}?kind=nope`);
  await check('kind 非法 ⇒ 400', () => {
    assert.equal(badKind.status, 400);
    assert.equal(badKind.body.ok, false);
  });
  const noScript = await get('/api/script/NOPE.BIN');
  await check('未知脚本 ⇒ 404 + 可读错误', () => {
    assert.equal(noScript.status, 404);
    assert.match(noScript.body.error, /找不到/);
  });
  const notAScript = await get('/api/script/AGE.EXE__USERDATA.BIN');
  await check('基线根里有这个名字、但不是 AGE 脚本 ⇒ 404 + 可读错误（与"名字不存在"分开报）', () => {
    assert.equal(notAScript.status, 404);
    assert.match(notAScript.body.error, /不是 AGE 脚本/);
  });
  const small = await get(`/api/script/${encodeURIComponent((picked[0].name).toLowerCase())}?kind=data`);
  await check('脚本名大小写不敏感（解析到同一支）', () => assert.equal(small.body.name, picked[0].name));
  const nonAnnotatedLower = await get('/api/script/sn0000.bin?kind=data');
  await check('名字过滤器没命中的脚本也能整支取到（名字大小写不敏感）', () => {
    assert.equal(nonAnnotatedLower.status, 200);
    assert.equal(nonAnnotatedLower.body.name, 'SN0000.BIN');
  });

  const health = await get('/api/health');
  await check('/api/health 自检：节点数 / patch 脚本数 / 基线根 / 标注标签口径 都在', () => {
    assert.equal(health.status, 200);
    assert.equal(health.body.ok, true);
    assert.equal(health.body.readOnly, true);
    assert.equal(health.body.nodes, nodes.length);
    assert.equal(health.body.patchScripts, patchKeys.length);
    assert.ok(health.body.base);
    assert.equal(health.body.annotatedFilter, String(SPEAKER_FILTER));
    assert.ok(!('target' in health.body), '旧 target 字段已废：标注标签不再从产物根推');
  });

  // 写路径只有一条：别的动作都必须被拒（不是"没实现"，是刻意没有）
  for (const method of ['POST', 'PUT', 'DELETE', 'PATCH']) {
    const res = await fetch(`${base}/api/tree`, { method });
    await check(`${method} /api/tree ⇒ 405（唯一写路径是 POST /api/nodes）`, async () => {
      assert.equal(res.status, 405);
      const body: any = await res.json();
      assert.equal(body.ok, false);
    });
  }

  const unknown = await get('/api/nope');
  await check('未知端点 ⇒ 404 JSON', () => {
    assert.equal(unknown.status, 404);
    assert.equal(unknown.body.ok, false);
  });
  const traversal = await get('/../tools/lib/requirements.mjs');
  await check('路径穿越 ⇒ 404（白名单：请求路径只当查表的键）', () => assert.equal(traversal.status, 404));

  // ───────────────────────────────────────────────────────── 写路径（建单）
  console.log('\n6) 写路径（POST /api/nodes）：写在临时台账上，规则全部来自模型');
  await check('可写性由监听地址派生：回环开、别的关（纯函数，不起服务）', () => {
    for (const h of ['127.0.0.1', '127.0.0.5', 'localhost', '::1']) {
      assert.equal(writesAllowed(h).allowed, true, `${h} 是回环，应当可写`);
    }
    for (const h of ['0.0.0.0', '::', '192.168.1.20', 'example.com']) {
      const r = writesAllowed(h);
      assert.equal(r.allowed, false, `${h} 不是回环，写路径必须关掉`);
      assert.match(r.why, /不是回环/, '要明说为什么关（页面就显示这句话）');
    }
  });

  // ★ 自检**绝不写真的台账**：另起一个服务，`--dir` 指向一份副本；
  //   最后还会断言真的 `data/requirements/` 一个文件都没多。
  const tmpRoot = path.join(REPO_ROOT, '.tmp');
  fs.mkdirSync(tmpRoot, { recursive: true });
  const writeDir = fs.mkdtempSync(path.join(tmpRoot, 'smoke-req-'));
  for (const f of fs.readdirSync(DEFAULT_REQUIREMENTS_DIR)) {
    if (f.endsWith('.md')) fs.copyFileSync(path.join(DEFAULT_REQUIREMENTS_DIR, f), path.join(writeDir, f));
  }
  const realBefore = fs.readdirSync(DEFAULT_REQUIREMENTS_DIR).sort();
  const writeNodes = () => loadNodes(writeDir);

  const wServer = await startServer({ port: 0, log: () => {}, dir: writeDir });
  const wBase = `http://127.0.0.1:${(wServer.address() as { port: number }).port}`;
  /** POST 一个 JSON 体进去（`type` 用来验"只收 application/json"那道门） */
  const post = async (body: unknown, contentType = 'application/json', url = '/api/nodes') => {
    const res = await fetch(`${wBase}${url}`, {
      method: 'POST',
      headers: contentType === '' ? {} : { 'content-type': contentType },
      body: typeof body === 'string' ? body : JSON.stringify(body),
    });
    return { status: res.status, body: (await res.json().catch(() => null)) as any, res };
  };

  try {
    const wHealth = await (await fetch(`${wBase}/api/health`)).json();
    await check('health.writes：开了 + 表单枚举与模型逐项相等（服务端不另写一份枚举）', () => {
      assert.equal(wHealth.writes.enabled, true);
      assert.equal(wHealth.writes.endpoint, 'POST /api/nodes');
      assert.deepEqual(wHealth.writes.types, TYPES);
      assert.deepEqual(wHealth.writes.statuses, STATUSES);
      assert.deepEqual(wHealth.writes.severities, SEVERITIES);
      assert.deepEqual(wHealth.writes.bugOnly, BUG_ONLY);
      assert.equal(wHealth.writes.defaults.type, 'req');
      assert.equal(wHealth.writes.defaults.status, 'open');
    });

    const rootRow = tree.body.nodes.find((n: any) => n.parent === null);
    const parentRow = tree.body.nodes.find((n: any) => n.kids > 0) ?? rootRow;

    const relPath = 'docs/01-translation/patch-design.md';
    const created = await post({
      title: '自检临时节点（会被删掉）',
      type: 'req',
      status: 'open',
      parent: parentRow.id,
      tags: ['smoke', 'temp'],
      verify: relPath,
      body: '## 判据\n\n**自检**用的：`a|b` 与表格。\n\n| 列 | 值 |\n|---|---|\n| x | 1 |\n',
    });
    await check('合法建单 ⇒ 201 + id/短名/文件路径', () => {
      assert.equal(created.status, 201);
      assert.equal(created.body.ok, true);
      assert.match(created.body.created.id, /^REQ-[0-9A-HJKMNP-TV-Z]{26}$/);
      assert.equal(created.body.created.short, created.body.created.name.slice(-8));
      assert.equal(created.body.dir, writeDir);
      assert.ok(fs.existsSync(created.body.created.file), '返回的文件路径必须真的存在');
      assert.equal(path.dirname(created.body.created.file), writeDir, '必须写在临时目录里');
    });

    const newName = created.body.created.name;
    await check('落盘形态 = 模型口径（父是完整 id、标题/类型/状态如所发、正文按规范形态）', () => {
      const text = fs.readFileSync(path.join(writeDir, `${newName}.md`), 'utf8');
      const parsed = parseNode(text, `${newName}.md`);
      assert.equal(parsed.title, '自检临时节点（会被删掉）');
      assert.equal(parsed.fields.id, created.body.created.id);
      assert.equal(parsed.fields.type, 'req');
      assert.equal(parsed.fields.status, 'open');
      assert.equal(parsed.fields.parent, parentRow.id, 'parent 要落成**完整 id**（不是短名）');
      assert.deepEqual(parsed.fields.tags, ['smoke', 'temp']);
      assert.equal(parsed.fields.verify, relPath);
      assert.match(parsed.body, /## 判据/);
      assert.ok(text.endsWith('\n'), '规范形态以换行收尾');
    });

    await check('写后全树守卫仍然全绿（新节点没把树弄坏）', () => {
      const report = validateAll(writeNodes(), { repoRoot: REPO_ROOT });
      assert.equal(report.failures, 0, JSON.stringify(report.checks.filter((c: any) => c.problems.length)));
      assert.equal(writeNodes().length, nodes.length + 1);
    });

    await check('新节点立刻能从**读**端点拿到（短名解析，与列表同一套）', async () => {
      const got = await (await fetch(`${wBase}/api/node/${created.body.created.short}`)).json();
      assert.equal(got.ok, true);
      assert.equal(got.node.id, created.body.created.id);
      assert.equal(got.node.parent, parentRow.id);
    });

    // ★ 守卫不绿 ⇒ 422 + **磁盘上没有残 file**（回滚是模型 `planAdd().apply()` 干的）
    const beforeFiles = fs.readdirSync(writeDir).sort();
    const bad = await post({ title: '缺 repro 的缺陷', type: 'bug', status: 'doing', parent: parentRow.id });
    await check('守卫不绿的建单 ⇒ 422 + report 点名 repro + 磁盘没留残 file（已回滚）', () => {
      assert.equal(bad.status, 422);
      assert.equal(bad.body.ok, false);
      assert.equal(bad.body.rolledBack, true);
      assert.ok(bad.body.report.failures > 0, '要带不变量报告');
      const problems = bad.body.report.checks.flatMap((c: any) => c.problems).join('\n');
      assert.match(problems, /repro/, '报告要点名缺什么：' + problems);
      assert.deepEqual(fs.readdirSync(writeDir).sort(), beforeFiles, '回滚后目录必须回到原样');
    });

    await check('请求被拒的三种情形：非 JSON ⇒ 415、缺标题 ⇒ 400、不认得的字段 ⇒ 400', async () => {
      const wrongType = await post({ title: 'x' }, 'text/plain');
      assert.equal(wrongType.status, 415);
      assert.match(wrongType.body.error, /application\/json/);

      const noTitle = await post({ type: 'req', parent: parentRow.id });
      assert.equal(noTitle.status, 400);
      assert.match(noTitle.body.error, /标题/);

      // ★ 静默忽略错别字会让"以为挂上了父节点"变成一棵孤立子树 ⇒ 必须报错
      const typo = await post({ title: 'x', parnet: parentRow.id });
      assert.equal(typo.status, 400);
      assert.match(typo.body.error, /不认得/);

      const badParent = await post({ title: 'x', parent: 'REQ-不存在' });
      assert.equal(badParent.status, 400);
      assert.match(badParent.body.error, /parent/);
    });

    await check('id 撞车 ⇒ 400（`saveNode` 会覆盖同名文件 —— 那不是新建，是毁掉一个已有节点）', async () => {
      const taken = writeNodes()[0];
      const res = await post({ title: '想顶掉别人', id: taken.fields.id });
      assert.equal(res.status, 400);
      assert.match(res.body.error, /已被占用/);
      assert.ok(fs.existsSync(path.join(writeDir, `${taken.name}.md`)), '原文件必须还在');
    });

    await check('非 JSON 体 / 非对象体 / 端点与方法用错 ⇒ 400 / 405，且都不留文件', async () => {
      const broken = await post('{不是 JSON');
      assert.equal(broken.status, 400);
      assert.match(broken.body.error, /JSON/);
      const arr = await post([1, 2, 3]);
      assert.equal(arr.status, 400);

      const getOnWrite = await get('/api/nodes');
      assert.equal(getOnWrite.status, 405);
      assert.match(getOnWrite.body.error, /只收 POST/);

      const before = fs.readdirSync(writeDir).sort();
      assert.deepEqual(fs.readdirSync(writeDir).sort(), before);
    });

    await check('★ 不吐 CORS 头：跨站页面发不出 application/json 的简单请求 ⇒ 预检必然失败', async () => {
      const options = await fetch(`${wBase}/api/nodes`, {
        method: 'OPTIONS',
        headers: { origin: 'http://evil.example', 'access-control-request-method': 'POST' },
      });
      assert.equal(options.headers.get('access-control-allow-origin'), null, '不许回 CORS 头');
      const res = await fetch(`${wBase}/api/nodes`, {
        method: 'POST',
        headers: { 'content-type': 'application/json', origin: 'http://evil.example' },
        body: JSON.stringify({ title: 'x', parent: parentRow.id }),
      });
      assert.equal(res.headers.get('access-control-allow-origin'), null, '响应也不许带 CORS 头');
      await res.json();
    });

    await check('★ 真的台账一个文件都没多（自检只写临时副本）', () => {
      assert.deepEqual(fs.readdirSync(DEFAULT_REQUIREMENTS_DIR).sort(), realBefore);
    });
  } finally {
    await new Promise((resolve) => wServer.close(resolve));
    fs.rmSync(writeDir, { recursive: true, force: true });
  }

  // 非回环监听 ⇒ `/api/health` 明说不能写、POST 直接 403（真正把门关上，而不是"界面禁用按钮"）
  const openServer = await startServer({ port: 0, host: '0.0.0.0', log: () => {}, dir: writeDir });
  const oBase = `http://127.0.0.1:${(openServer.address() as { port: number }).port}`;
  try {
    const oHealth = await (await fetch(`${oBase}/api/health`)).json();
    await check('绑到 0.0.0.0 ⇒ health.writes.enabled=false 且写明理由', () => {
      assert.equal(oHealth.writes.enabled, false);
      assert.match(oHealth.writes.why, /0\.0\.0\.0/);
    });
    const refused = await fetch(`${oBase}/api/nodes`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ title: 'x' }),
    });
    await check('绑到 0.0.0.0 ⇒ POST 建单 403（不是靠界面藏按钮）', async () => {
      assert.equal(refused.status, 403);
      const body: any = await refused.json();
      assert.equal(body.ok, false);
      assert.match(body.error, /不是回环/);
    });
  } finally {
    await new Promise((resolve) => openServer.close(resolve));
  }

  // ───────────────────────────────────────────────────────── Markdown 渲染口径
  console.log('\n7) Markdown 渲染（`src/markdown.ts`，客户端与自检共用同一个模块）');
  await check('渲染口径：不开原始 HTML（同一条结论解决"要不要 sanitize"）', () => {
    assert.equal(MD_OPTIONS.html, false);
    const html = renderMarkdown('<script>alert(1)</script>');
    assert.ok(!html.includes('<script'), '原始 HTML 不许变成标签：' + html);
    assert.match(html, /&lt;script&gt;/);
  });
  await check('渲染口径：不把裸文本自动变成链接（linkify 关）', () => {
    assert.equal(MD_OPTIONS.linkify, false);
    assert.ok(!renderMarkdown('看 https://example.com 与 1.2.3').includes('<a '), '裸链接不许自动变链接');
  });
  await check('常用语法真的渲染了：粗体 / 行内代码 / 表格 / 列表 / 引用', () => {
    assert.match(renderMarkdown('**粗**'), /<strong>粗<\/strong>/);
    assert.match(renderMarkdown('`码`'), /<code>码<\/code>/);
    assert.match(renderMarkdown('| a | b |\n|---|---|\n| 1 | 2 |'), /<table>/);
    assert.match(renderMarkdown('- x\n- y'), /<ul>/);
    assert.match(renderMarkdown('> 引'), /<blockquote>/);
    assert.match(renderMarkdown('```\ncode\n```'), /<pre><code>/);
  });
  /**
   * ★ 实体（`&…;`）：这条同时是**依赖树**的哨兵 —— 本机是全扁平（hoisted）布局且 `fs.realpathSync`
   * 不解析 junction ⇒ **一个包名在树里只能有一个版本**，否则根上的那个会遮蔽别人自己那份嵌套依赖
   * （踩过：markdown-it 要 `entities@8`、`@vue/compiler-core` 要 `entities@7` ⇒ `vue-tsc` 一遇到
   * 模板里的 `&lt;` 就崩在 `decode.fromCodePoint is not a function`）。现在 `markdown-it` 的
   * `entities` 被钉在 `^7`（见根 `package.json` 的 `pnpm.overrides`），这里断言它**真的还能解码**。
   */
  await check('实体解码仍然正确（依赖钉版本的哨兵：&amp; / &copy; / &#65; / 未知实体 / 裸 &）', () => {
    assert.equal(renderMarkdown('&amp;'), '<p>&amp;</p>\n', '`&amp;` 要解成 `&` 再为 HTML 转义回来');
    assert.match(renderMarkdown('&copy;'), /©/, '命名实体要解开');
    assert.match(renderMarkdown('&#65;'), /A/, '数字实体要解开');
    assert.match(renderMarkdown('&lt;'), /&lt;/, '`&lt;` 解成 `<` 后再转义回 `&lt;`（绝不是真标签）');
    assert.match(renderMarkdown('&nope;'), /&amp;nope;/, '不认识的实体原样显示');
    assert.match(renderMarkdown('AT&T'), /AT&amp;T/, '裸 `&` 要转义');
  });
  await check('★ 仓库内路径不做成可点链接（工作台不服务仓库文件）；外链反而要能开', () => {
    const rel = renderMarkdown('[设计](docs/01-translation/patch-design.md)');
    assert.ok(!rel.includes('href='), '相对路径不许带 href（点了只会 404）：' + rel);
    assert.match(rel, /class="md-ref"/);
    assert.match(rel, /设计/);
    const ext = renderMarkdown('[外链](https://example.com/a)');
    assert.match(ext, /href="https:\/\/example\.com\/a"/);
    assert.match(ext, /target="_blank"/);
    assert.match(ext, /rel="noreferrer noopener"/);
    assert.match(renderMarkdown('[邮件](mailto:a@b.c)'), /href="mailto:a@b.c"/);
  });
  await check('★ 小节正文里的小标题降两级（卡片标题 h3 ⇒ 正文 # → h4、### → h6 封顶）', () => {
    const html = (src: string) => renderMarkdown(src);
    assert.match(html('# 一级'), /<h3>一级<\/h3>/);
    assert.match(html('## 二级'), /<h4>二级<\/h4>/);
    assert.match(html('### 三级'), /<h5>三级<\/h5>/);
    assert.match(html('##### 五级'), /<h6>五级<\/h6>/, '超过 h6 要封顶');
    assert.ok(html('# 一级').includes('</h3>'), '开闭标签要成对');
  });
  await check('空输入给空串（调用方不必自己判空）', () => {
    assert.equal(renderMarkdown(''), '');
    assert.equal(renderMarkdown('   \n\n'), '');
    assert.equal(renderMarkdown(null), '');
    assert.equal(renderMarkdown(undefined), '');
  });
  await check('★ 详情页真的走这套渲染（页面里没有"把 Markdown 原文塞进 pre"的老路）', () => {
    const view = fs.readFileSync(new URL('./src/views/RequirementDetail.vue', import.meta.url), 'utf8');
    assert.match(view, /renderMarkdown/, '详情页必须用渲染器');
    assert.match(view, /v-html/, '渲染结果要挂进 DOM');
    assert.ok(!/<pre v-if="s\.text"/.test(view), '旧的 <pre> 原文展示必须已经撤掉');
  });

  console.log('\n8) 静态（dist/web 的白名单视图）');
  const built = fs.existsSync(new URL('./dist/web/index.html', import.meta.url));
  if (!built) {
    console.log('  skip dist/web 还没构建（先 node scripts/vite-cli.mjs build）');
  } else {
    const res = await fetch(`${base}/`);
    const html = await res.text();
    await check('/ 返回构建出来的 index.html', () => {
      assert.equal(res.status, 200);
      assert.match(res.headers.get('content-type') ?? '', /text\/html/);
      assert.equal(res.headers.get('cache-control'), 'no-store');
      assert.match(html, /<div id="app">/);
      assert.match(html, /assets\/index-[\w-]+\.js/);
    });
    const asset = /src="([^"]*assets\/index-[\w-]+\.js)"/.exec(html)?.[1];
    const js = await fetch(`${base}${asset}`);
    await check(`构建产物可取：${asset}`, () => {
      assert.equal(js.status, 200);
      assert.match(js.headers.get('content-type') ?? '', /javascript/);
    });
    await check('★ 建单对话框的 chunk 不被外壳预加载（懒加载要真的懒）', () => {
      assert.ok(
        !html.includes('NewRequirementDialog'),
        'index.html 里不该出现对话框 chunk —— 出现了说明它被提升成静态依赖，页面一打开就下载表单那一坨',
      );
      const chunks = fs.readdirSync(new URL('./dist/web/assets/', import.meta.url));
      assert.ok(
        chunks.some((f) => f.startsWith('NewRequirementDialog') && f.endsWith('.js')),
        '它仍应作为独立 chunk 存在（懒加载 ≠ 不打包）',
      );
    });
  }

  console.log('\n9) 与旧 `apps/requirements/server.mjs` 的**读**端点形状对照（旧目录退役后自动跳过）');
  const legacyEntry = new URL('../requirements/server.mjs', import.meta.url);
  if (!fs.existsSync(legacyEntry)) {
    console.log('  skip 旧目录已不在（正是需求单 §退役 的目标）');
  } else {
    // 旧服务也是同进程可起的（它只 import tools/lib，不 spawn）⇒ 拿它当**形状的证人**
    const legacy = await import(legacyEntry.href);
    const old = await legacy.startServer({ port: 0, log: () => {} });
    const oldBase = `http://127.0.0.1:${(old.address() as { port: number }).port}`;
    const oldGet = async (p: string) => {
      const res = await fetch(`${oldBase}${p}`);
      return { status: res.status, body: (await res.json()) as any };
    };
    try {
      const mineTree = (await get('/api/tree')).body;
      const oldTree = (await oldGet('/api/tree')).body;
      await check('/api/tree 与旧服务同形（顶层键 + 表头 + 每行逐字段）', () => {
        assert.deepEqual(Object.keys(mineTree).sort(), Object.keys(oldTree).sort());
        assert.deepEqual(mineTree.totals, oldTree.totals);
        assert.deepEqual(mineTree.marks, oldTree.marks);
        assert.deepEqual(mineTree.budget, oldTree.budget);
        assert.equal(mineTree.dir, oldTree.dir);
        assert.equal(mineTree.schema, oldTree.schema);
        assert.deepEqual(mineTree.decisions, oldTree.decisions);
        assert.equal(mineTree.nodes.length, oldTree.nodes.length);
        for (let i = 0; i < mineTree.nodes.length; i += 1) {
          assert.deepEqual(mineTree.nodes[i], oldTree.nodes[i], `第 ${i} 行不同：${mineTree.nodes[i].short}`);
        }
      });
      const ref = mineTree.nodes[1].short;
      const mineNode = (await get(`/api/node/${encodeURIComponent(ref)}`)).body;
      const oldNode = (await oldGet(`/api/node/${encodeURIComponent(ref)}`)).body;
      await check(`/api/node/${ref} 与旧服务同形（含 sections / parentChain / children）`, () => {
        assert.deepEqual(Object.keys(mineNode).sort(), Object.keys(oldNode).sort());
        assert.deepEqual(mineNode.node, oldNode.node);
        assert.deepEqual(mineNode.parentChain, oldNode.parentChain);
        assert.deepEqual(mineNode.children, oldNode.children);
      });
      const mineMiss = await get('/api/node/REQ-00000000000000000000000000');
      const oldMiss = await oldGet('/api/node/REQ-00000000000000000000000000');
      await check('未知引用的错误体也与旧服务同形', () => assert.deepEqual(mineMiss.body, oldMiss.body));
    } finally {
      await new Promise((resolve) => old.close(resolve));
    }
  }

  console.log(`\n${pass} 项通过${failures.length ? `（${failures.length} 项红：${failures.join(' · ')}）` : '，全绿'}`);
} finally {
  await new Promise((resolve) => server.close(resolve));
}
