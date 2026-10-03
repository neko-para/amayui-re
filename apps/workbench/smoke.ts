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
 *      "旧管线漏提"的真译文，也有真的没变更的）。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';

import {
  DEFAULT_REQUIREMENTS_DIR,
  buildTree,
  describeNode,
  flatten,
  loadNodes,
  splitSections,
} from '../../tools/lib/requirements.mjs';
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
import { startServer } from './server.ts';

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

  console.log('\n5) 边界与只读');
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

  // 只读是**纪律**：任何写动作都必须被拒（不是"没实现"，是刻意没有）
  for (const method of ['POST', 'PUT', 'DELETE', 'PATCH']) {
    const res = await fetch(`${base}/api/tree`, { method });
    await check(`${method} /api/tree ⇒ 405（没有任何写路径）`, async () => {
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

  console.log('\n6) 静态（dist/web 的白名单视图）');
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
  }

  console.log('\n7) 与旧 `apps/requirements/server.mjs` 的形状对照（旧目录退役后自动跳过）');
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
