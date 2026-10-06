/**
 * tools/test/requirements.test.mjs — 需求模型（`data/requirements/`）的守卫测试
 *
 * 分两层：
 *   ① **单元**：对内存里的合成节点逐条验证 5 条不变量**真的能红**（守卫不能是"永远绿"的摆设），
 *      外加几条"写路径不许写坏"的性质（规范形态幂等、写后复验、回滚）。
 *   ② **端到端**：对真实 `data/requirements/` 跑一遍不变量（含 `verify` 指向的守卫是否真实存在）。
 *
 * ★ 只测**基建契约**，不测"树上现在有哪几个节点"（那是 `pnpm tools requirements plan` 的活）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import {
  BUDGET,
  DEFAULT_REQUIREMENTS_DIR,
  KEY_ORDER,
  STATUSES,
  TYPES,
  buildTree,
  canonicalNode,
  deleteNode,
  flatten,
  loadNodes,
  nodeId,
  parseNode,
  planAdd,
  rollup,
  saveNode,
  ulid,
  validateAll,
} from '../lib/requirements.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const N = (over = {}) => {
  const name = over.name ?? ulid();
  const fields = { id: nodeId(name), type: 'req', status: 'open', parent: 'null', ...(over.fields ?? {}) };
  return { name, title: over.title ?? '合成节点', fields, body: over.body ?? '## 判据\n\n一条。', lines: over.lines ?? 5, file: `<mem>/${name}.md` };
};
const ids = (report) => report.checks.filter((c) => c.problems.length > 0).map((c) => c.id);

/**
 * ★ 锚点棘轮的**正例锚**：本文件里真实存在这一行 ⇒ `verify`/`repro` 指向它必须绿。
 * 负例锚必须**运行时随机生成**：写死的"不存在的锚点"这几个字本身就长在这个文件里，
 * 锚点搜索会找到它自己（第一次写这条用例时就踩了，绿得毫无意义）。
 */
const SELF_ANCHOR = 'REQ-ANCHOR-SELFTEST-DO-NOT-REMOVE';
const missingAnchorToken = () => `REQ-ANCHOR-ABSENT-${Math.random().toString(36).slice(2)}`;

// ─────────────────────────────────────────── ① 每条不变量都能红

test('#1 形态：文件名不是 ULID / id 与文件名不一致 / 枚举非法 / 未知字段 → 红', () => {
  const root = N();
  assert.deepEqual(ids(validateAll([root])), [], '合法单根必须绿');

  assert.ok(ids(validateAll([root, N({ name: 'NOT-A-ULID' })])).includes(1), '文件名非 ULID 要红');
  assert.ok(ids(validateAll([root, N({ fields: { id: 'REQ-别的' } })])).includes(1), 'id != 文件名 要红');
  assert.ok(ids(validateAll([root, N({ fields: { type: 'nope' } })])).includes(1), 'type 非法要红');
  assert.ok(ids(validateAll([root, N({ fields: { status: 'nope' } })])).includes(1), 'status 非法要红');
  assert.ok(ids(validateAll([root, N({ fields: { weirds: 'x' } })])).includes(1), '未知字段要红（不许静默忽略）');
  assert.ok(ids(validateAll([root, N({ fields: { tags: 'a,b' } })])).includes(1), 'tags 必须是列表要红');
});

test('#2 树闭合：多根 / 缺 parent / 悬空 / 成环 / 孤立 → 红', () => {
  const root = N();
  const child = N({ fields: { parent: root.fields.id } });
  assert.deepEqual(ids(validateAll([root, child])), [], '单根 + 一个子必须绿');

  assert.ok(ids(validateAll([root, N()])).includes(2), '两个根要红');
  const noParent = N();
  delete noParent.fields.parent;
  assert.ok(ids(validateAll([root, noParent])).includes(2), '缺 parent 要红（根必须显式 null）');
  assert.ok(ids(validateAll([root, N({ fields: { parent: 'REQ-不存在' } })])).includes(2), 'parent 悬空要红');

  // 成环：A 的 parent 指向 B，B 的 parent 指向 A（都不是根，还额外触发"没有根"）
  const a = N();
  const b = N({ fields: { parent: a.fields.id } });
  a.fields.parent = b.fields.id;
  const cyc = validateAll([a, b]);
  assert.ok(ids(cyc).includes(2) && cyc.checks[1].problems.some((p) => /成环/.test(p)), '成环要红且要点名');

  // 孤立：parent 指向一个 decision（不参与树）⇒ 从根走不到
  const root2 = N();
  const dec = N({ fields: { type: 'decision' } });
  delete dec.fields.parent;
  const orphan = N({ fields: { parent: dec.fields.id } });
  assert.ok(ids(validateAll([root2, dec, orphan])).includes(2), '挂在决策下的节点要红（从根走不到）');
  // decision 自己写 parent 也要红
  assert.ok(ids(validateAll([root2, N({ fields: { type: 'decision', parent: root2.fields.id } })])).includes(2), 'decision 不许有 parent');
});

test('#3 状态自洽：done 无凭据 / blocked 无前置 / dropped 无理由 / verify 不存在 → 红', () => {
  const root = N();
  const k = (fields) => N({ fields: { parent: root.fields.id, ...fields } });

  assert.ok(ids(validateAll([root, k({ status: 'done' })])).includes(3), 'done 无 verify/done_reason 要红');
  assert.deepEqual(ids(validateAll([root, k({ status: 'done', done_reason: '文档类收口' })])), [], 'done_reason 是合法例外口');
  assert.deepEqual(
    ids(validateAll([root, k({ status: 'done', verify: `tools/test/requirements.test.mjs#${SELF_ANCHOR}` })], { repoRoot: REPO_ROOT })),
    [],
    'verify 指向真实存在的测试（含锚点）必须绿',
  );
  assert.ok(
    ids(validateAll([root, k({ status: 'done', verify: 'tools/test/没有这个文件.test.mjs' })], { repoRoot: REPO_ROOT })).includes(3),
    'verify 指向不存在的文件要红',
  );
  const missingAnchor = validateAll(
    [root, k({ status: 'done', verify: `tools/test/requirements.test.mjs#${missingAnchorToken()}` })],
    { repoRoot: REPO_ROOT },
  );
  assert.ok(missingAnchor.checks[2].problems.length > 0, `verify 的锚点在文件里找不到要红（锚点棘轮），实际：${JSON.stringify(missingAnchor.checks[2].problems)}`);
  assert.ok(ids(validateAll([root, k({ status: 'blocked' })])).includes(3), 'blocked 无 blocked_by 要红');
  assert.ok(ids(validateAll([root, k({ status: 'dropped' })])).includes(3), 'dropped 无理由要红');
  assert.ok(ids(validateAll([root, k({ status: 'doing', done_reason: '不该有' })])).includes(3), '非 done 写 done_reason 要红');
  assert.ok(ids(validateAll([root, k({ supersedes: [root.fields.id] })])).includes(3), '非 decision 用 supersedes 要红');
});

test('★ 缺陷与需求的流程差异：repro / severity 是缺陷专属，收口凭据不同', () => {
  const root = N();
  const bug = (fields) => N({ fields: { parent: root.fields.id, type: 'bug', ...fields } });

  // ① 缺陷开着就必须有 repro（"存在证明 + 重开条件"）
  assert.ok(ids(validateAll([root, bug({ status: 'doing' })])).includes(1), '缺陷缺 repro 要红');
  // ② 需求身上不许出现缺陷专属字段
  assert.ok(ids(validateAll([root, N({ fields: { parent: root.fields.id, repro: 'x' } })])).includes(1), '需求写 repro 要红');
  assert.ok(ids(validateAll([root, N({ fields: { parent: root.fields.id, severity: 'S2' } })])).includes(1), '需求写 severity 要红');
  // ③ severity 枚举合法
  assert.ok(ids(validateAll([root, bug({ status: 'doing', repro: 'x', severity: 'P2' })])).includes(1), 'severity 写成优先级要红');
  // ④ 缺陷收口：repro 或 verify 任一为凭据即可（两者都过锚点棘轮）
  assert.deepEqual(
    ids(validateAll([root, bug({ status: 'done', repro: `tools/test/requirements.test.mjs#${SELF_ANCHOR}` })], { repoRoot: REPO_ROOT })),
    [],
    '缺陷 done 靠 repro 即可（不必有 verify）',
  );
  assert.ok(
    ids(validateAll([root, bug({ status: 'done', repro: 'tools/test/没有这个文件.test.mjs' })], { repoRoot: REPO_ROOT })).includes(3),
    '缺陷的证据文件不存在要红（与 verify 同一把锚点棘轮）',
  );
  assert.ok(
    ids(validateAll([root, bug({ status: 'doing', repro: `tools/test/requirements.test.mjs#${missingAnchorToken()}` })], { repoRoot: REPO_ROOT })).includes(3),
    '缺陷的 repro 锚点找不到也要红',
  );
  // ⑤ 缺陷不许 superseded（重复 ⇒ dropped + 理由）
  assert.ok(ids(validateAll([root, bug({ status: 'superseded', repro: 'x' })])).includes(1), '缺陷 superseded 要红');
  assert.deepEqual(
    ids(validateAll([root, bug({ status: 'dropped', dropped_reason: '不是缺陷：观测的是缓存态', repro: 'x' })])),
    [],
    '缺陷 dropped + 理由（"不是缺陷"）必须绿 —— 这条路径代表"误报"',
  );
});

test('#4 父不先于子收口：父 done 而子树里还有活节点 → 红', () => {
  const root = N({ fields: { status: 'done', done_reason: '假装做完' } });
  const kid = N({ fields: { parent: root.fields.id, status: 'doing' } });
  const r = validateAll([root, kid]);
  assert.ok(ids(r).includes(4), '父先于子收口要红');
  assert.ok(r.checks[3].problems.some((p) => /还有 1 个未收口/.test(p)), '要点名还有几个未收口');

  // 子节点全部收口（含 dropped）⇒ 父可以 done
  const root2 = N({ fields: { status: 'done', done_reason: 'x' } });
  const k1 = N({ fields: { parent: root2.fields.id, status: 'done', done_reason: 'y' } });
  const k2 = N({ fields: { parent: root2.fields.id, status: 'dropped', dropped_reason: '不做了' } });
  assert.deepEqual(ids(validateAll([root2, k1, k2])), [], '子全收口（done/dropped）后父可以 done');
});

test('#5 一屏预算：单节点超行数 / **活节点**超上限 → 红；收口的节点不占预算', () => {
  const root = N();
  assert.ok(ids(validateAll([root, N({ lines: BUDGET.maxLinesPerNode + 1 })])).includes(5), '单节点超行数要红');

  // 活节点超上限 ⇒ 红
  const many = [root, ...Array.from({ length: BUDGET.maxNodes }, () => N({ fields: { parent: root.fields.id } }))];
  const r = validateAll(many);
  assert.ok(ids(r).includes(5), '活节点超上限要红');
  assert.ok(
    r.checks[4].problems.some((p) => /活节点/.test(p)),
    '点名要说清数的是"活节点"（收口的那些不算，否则树会因为"做过的事变多"而爆预算）',
  );

  // ★ 同样多的节点，只要**已收口**就不占预算
  const closed = [
    root,
    ...Array.from({ length: BUDGET.maxNodes }, () =>
      N({ fields: { parent: root.fields.id, status: 'done', done_reason: '收口了' } }),
    ),
  ];
  assert.deepEqual(ids(validateAll(closed)), [], `${BUDGET.maxNodes} 个 done + 1 个 root 必须绿（done 不计预算）`);

  // dropped / superseded 同样不计
  const dropped = [
    root,
    ...Array.from({ length: BUDGET.maxNodes }, () => N({ fields: { parent: root.fields.id, status: 'dropped', dropped_reason: '不做了' } })),
  ];
  assert.deepEqual(ids(validateAll(dropped)), [], 'dropped 不计预算');

  // 但"活"与"收口"混在一起时，只有活的那些计数
  const mixed = [
    root,
    ...Array.from({ length: BUDGET.maxNodes }, () =>
      N({ fields: { parent: root.fields.id, status: 'done', done_reason: 'y' } }),
    ),
    N({ fields: { parent: root.fields.id, status: 'doing' } }),
  ];
  assert.deepEqual(ids(validateAll(mixed)), [], '收口节点不参与计数 ⇒ 再加一个 doing 仍绿');
});

test('★ schema 覆盖计划要求的那几个字段，且 `blocked_by` 的正例成立（依赖指向真实节点）', () => {
  for (const k of ['type', 'status', 'parent', 'blocked_by', 'verify']) {
    assert.ok(KEY_ORDER.includes(k), `KEY_ORDER 必须有 ${k}`);
  }
  for (const t of ['req', 'bug', 'spike', 'decision']) assert.ok(TYPES.includes(t), `type 枚举缺 ${t}`);

  const root = N();
  const blocker = N({ fields: { parent: root.fields.id } });
  const blocked = N({ fields: { parent: root.fields.id, status: 'blocked', blocked_by: [blocker.fields.id] } });
  assert.deepEqual(ids(validateAll([root, blocker, blocked])), [], 'blocked + 指向真实前置 ⇒ 绿');

  const dangling = N({ fields: { parent: root.fields.id, status: 'blocked', blocked_by: ['REQ-不存在'] } });
  assert.ok(ids(validateAll([root, blocker, dangling])).includes(2), 'blocked_by 悬空 ⇒ 红');
  assert.ok(STATUSES.includes('blocked'), 'status 枚举含 blocked');
});

// ─────────────────────────────────────────── 派生（聚合 / 顺序 / 孤立可见）

test('派生：rollup 只数活节点、flatten 把未连到根的节点也列出来（不许"看不见"）', () => {
  const root = N();
  const kid = N({ fields: { parent: root.fields.id, status: 'done', done_reason: 'x' } });
  const grand = N({ fields: { parent: kid.fields.id, status: 'blocked', blocked_by: [root.fields.id] } });
  const tree = buildTree([root, kid, grand]);
  assert.deepEqual(rollup(root, tree), { live: 1, total: 2, derivedStatus: root.fields.status }, '父的聚合：2 个后代、其中 1 个活');

  const orphan = N({ fields: { parent: 'REQ-悬空' } });
  const rows = flatten([root, kid, grand, orphan], buildTree([root, kid, grand, orphan]));
  const o = rows.find((r) => r.node.name === orphan.name);
  assert.ok(o && o.orphan === true, '孤立/悬空节点必须出现在列表里并标 orphan');

  // order 是展示顺序（不是身份）：重排不改 id
  const a = N({ fields: { parent: root.fields.id, order: '20' } });
  const b = N({ fields: { parent: root.fields.id, order: '10' } });
  const t2 = buildTree([root, a, b]);
  assert.deepEqual(t2.children.get(root.name).map((x) => x.name), [b.name, a.name], '同级按 order 排');
});

// ─────────────────────────────────────────── 解析 / 写盘性质

test('引用解析：完整 id / 唯一前缀 / **唯一后缀**（ULID 前缀相同，短别名只能靠后缀）都能找到；歧义则拒绝', () => {
  // 造两个"前 10 位相同、尾部不同"的 ULID（模拟同批种子）
  const a = { ...N({ name: '01M3TCGGJ0AVHPJTF1KG9R0BA7' }) };
  const b = { ...N({ name: '01M3TCGGJ0MYPJXZFJYXTKS1JX' }) };
  const tree = buildTree([a, b]);
  assert.equal(tree.resolve(a.fields.id)?.name, a.name, '完整 id');
  assert.equal(tree.resolve(a.name.slice(0, 12))?.name, a.name, '唯一前缀（要从头开始的才叫前缀）');
  assert.equal(tree.resolve('9R0BA7')?.name, a.name, '★ 唯一后缀（短别名）');
  assert.equal(tree.resolve('1JX')?.name, b.name, '★ 唯一后缀（更短的）');
  assert.equal(tree.resolve('01M3TCGGJ0'), null, '⚠ 歧义前缀必须拒绝（前缀是全同的）');
  assert.equal(tree.resolve('不存在'), null, '找不到就是找不到');
});

test('解析：合法形态往返一致；缺标题 / 参数行写坏 / 缺正文 / 重复键 → 抛错', () => {
  const text = '# 标题\n\n- id: REQ-01ABC\n- tags: [a, b]\n\n## 判据\n\n一条。\n';
  const p = parseNode(text, 'x.md');
  assert.equal(p.title, '标题');
  assert.deepEqual(p.fields, { id: 'REQ-01ABC', tags: ['a', 'b'] });
  assert.equal(p.body, '## 判据\n\n一条。');
  assert.equal(parseNode(canonicalNode(p), 'x.md').body, p.body, '规范形态往返一致');

  assert.throws(() => parseNode('没有标题\n', 'x.md'), /第一行必须是/);
  assert.throws(() => parseNode('# T\n\nid: REQ-01ABC\n\n## 判据\nx\n', 'x.md'), /参数行必须形如/);
  assert.throws(() => parseNode('# T\n\n- id: REQ-01ABC\n', 'x.md'), /必须有正文/);
  assert.throws(() => parseNode('# T\n\n- id: a\n- id: b\n\n## 判据\nx\n', 'x.md'), /重复/);
});

test('CLI 写路径：`--set … --unset <字段>` 能删字段，且不合法时**逐字节回滚**（不留残file）', async () => {
  const { main: cliMain } = await import('../requirements.mjs');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-cli-'));
  const quiet = (fn) => {
    const w = process.stdout.write.bind(process.stdout);
    const e = process.stderr.write.bind(process.stderr);
    process.stdout.write = () => true;
    process.stderr.write = () => true;
    try {
      return fn();
    } finally {
      process.stdout.write = w;
      process.stderr.write = e;
    }
  };
  try {
    const rootN = N({ title: '根' });
    assert.equal(saveNode(rootN, dir).ok, true);
    const kid = N({ title: '子', fields: { parent: rootN.fields.id, tags: ['a'] } });
    assert.equal(saveNode(kid, dir).ok, true);

    // ① --unset tags ⇒ 字段真的消失
    assert.equal(quiet(() => cliMain(['--set', kid.fields.id, '--unset', 'tags', '--dir', dir, '--write'])), 0);
    assert.equal(loadNodes(dir).find((n) => n.name === kid.name).fields.tags, undefined, '`--unset` 必须把字段删掉');

    // ② 让 `--set` 写进非法态（把子节点设成 blocked 但没有 blocked_by）⇒ 守卫拒绝 + 逐字节还原
    const before = fs.readFileSync(path.join(dir, `${kid.name}.md`), 'utf8');
    assert.equal(quiet(() => cliMain(['--set', kid.fields.id, '--status', 'blocked', '--dir', dir, '--write'])), 1, '非法改动必须被拒');
    assert.equal(fs.readFileSync(path.join(dir, `${kid.name}.md`), 'utf8'), before, '被拒后必须逐字节还原');
    assert.equal(fs.readdirSync(dir).filter((f) => f.includes('.tmp-')).length, 0, '不许留下临时文件');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('★ 建单只有一份规则（`planAdd`）：CLI `--add` 与工作台的 `POST /api/nodes` 都调它', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-add-'));
  /** 每次重新读盘：`planAdd` 的 `nodes`/`tree` 是调用方给的**当时**快照 */
  const ctx = () => {
    const nodes = loadNodes(dir);
    return { dir, nodes, tree: buildTree(nodes), repoRoot: REPO_ROOT };
  };
  try {
    const rootN = N({ title: '根' });
    assert.equal(saveNode(rootN, dir).ok, true);

    // ① 最少输入（只有标题 + 父）⇒ 缺省 type/status + 缺省正文骨架，落盘即规范形态
    const a = planAdd({ title: '子节点', parent: rootN.fields.id }, ctx());
    assert.match(a.id, /^REQ-[0-9A-HJKMNP-TV-Z]{26}$/);
    assert.equal(a.fields.type, 'req');
    assert.equal(a.fields.status, 'open');
    assert.equal(a.fields.parent, rootN.fields.id, 'parent 落成**完整 id**');
    assert.match(a.body, /## 判据/);
    assert.equal(a.apply().report.failures, 0, '缺省建出来的节点必须直接是绿的');
    assert.equal(loadNodes(dir).length, 2);
    assert.equal(
      fs.readFileSync(path.join(dir, `${a.name}.md`), 'utf8'),
      canonicalNode({ name: a.name, title: a.title, fields: a.fields, body: a.body }),
      '落盘的就是规范形态（固定键序、同输入同字节）',
    );

    // ② 缺陷的缺省正文是**另一套**（"复现"），而且缺 repro 时守卫必须拒 + **文件要消失**
    const bad = planAdd({ title: '缺 repro 的缺陷', type: 'bug', status: 'doing', parent: rootN.fields.id }, ctx());
    assert.match(bad.body, /## 复现/, '缺陷的缺省正文该问"怎么复现"');
    const rolled = bad.apply();
    assert.equal(rolled.rollback, true, '缺 repro 必须被守卫拒回');
    assert.ok(rolled.report.failures > 0);
    assert.ok(!fs.existsSync(path.join(dir, `${bad.name}.md`)), '被拒的新增不许留残 file');
    assert.equal(loadNodes(dir).length, 2, '回滚后节点数不变');

    // ③ 写下去会出事的三种，必须在**写之前**就抛（不是写完再回滚）
    assert.throws(() => planAdd({ title: 'x', id: rootN.fields.id }, ctx()), /已被占用/, 'id 撞车会覆盖同名文件 ⇒ 必须挡');
    assert.throws(() => planAdd({ title: 'x', parnet: 'REQ-x' }, ctx()), /不认得/, '不认得的键不许静默忽略');
    assert.throws(() => planAdd({ title: 'x', parent: 'REQ-不存在' }, ctx()), /parent/);
    assert.throws(() => planAdd({}, ctx()), /标题/);
    assert.throws(() => planAdd({ title: 'REQ-01ABC' }, ctx()), /不是 id/, '`--title` 是标题不是 id');
    assert.throws(() => planAdd({ title: 'x', id: 'abc' }, ctx()), /形态非法/);
    assert.throws(() => planAdd({ title: 'x', body: '没有小节' }, ctx()), /## 小节/);
    // ★ 枚举非法**不在这里挡**：那是 `validateAll`（不变量 #1）的活 —— 这里只挡"写下去会出事"的。
    //   换句话说：`planAdd` 不抄第二份裁决，非法的单子走"写后守卫拒回 + 删文件"那条路。
    const badType = planAdd({ title: '类型非法', type: 'nope' }, ctx());
    assert.equal(badType.apply().rollback, true, '枚举非法由写后守卫拒回');
    assert.ok(!fs.existsSync(path.join(dir, `${badType.name}.md`)));

    // ④ 空列表 / 空串不写（表单留空、`--tags ''` 都不该多出一行噪声）
    const empty = planAdd({ title: '空列表', parent: rootN.fields.id, tags: [], blocked_by: '  ', order: '' }, ctx());
    assert.equal(empty.fields.tags, undefined);
    assert.equal(empty.fields.blocked_by, undefined);
    assert.equal(empty.fields.order, undefined);

    // ⑤ 逗号串也认（CLI 就是把 `--tags a,b` 原样递进来的）
    const full = planAdd({ title: '给全', parent: rootN.fields.id, tags: 'a, b', order: '30' }, ctx());
    assert.deepEqual(full.fields.tags, ['a', 'b']);
    assert.equal(full.fields.order, '30');

    // ⑥ plan 就是 CLI 打印的那三行（"写不写、写到哪、挂在哪"一眼可见）
    assert.equal(full.plan.length, 3);
    assert.match(full.plan[0], /^add REQ-/);
    assert.match(full.plan[1], /parent: REQ-/);
    assert.match(full.plan[2], /\.md$/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('CLI `--add`：dry-run 不落盘；`--write` 落盘并跑写后守卫（走的就是 planAdd）', async () => {
  const { main: cliMain } = await import('../requirements.mjs');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-cli-add-'));
  const quiet = (fn) => {
    const w = process.stdout.write.bind(process.stdout);
    const e = process.stderr.write.bind(process.stderr);
    process.stdout.write = () => true;
    process.stderr.write = () => true;
    try {
      return fn();
    } finally {
      process.stdout.write = w;
      process.stderr.write = e;
    }
  };
  try {
    const rootN = N({ title: '根' });
    assert.equal(saveNode(rootN, dir).ok, true);

    // ① dry-run：什么都不写
    assert.equal(quiet(() => cliMain(['--add', '--title', '干跑', '--parent', rootN.fields.id, '--dir', dir])), 0);
    assert.equal(loadNodes(dir).length, 1, 'dry-run 不许落盘');

    // ② --write：落盘 + 全树守卫绿
    assert.equal(quiet(() => cliMain(['--add', '--title', '真建', '--parent', rootN.fields.id, '--tags', 'a,b', '--dir', dir, '--write'])), 0);
    const after = loadNodes(dir);
    assert.equal(after.length, 2);
    const made = after.find((n) => n.title === '真建');
    assert.ok(made, '节点必须真的落盘');
    assert.deepEqual(made.fields.tags, ['a', 'b']);
    assert.equal(validateAll(after, { repoRoot: REPO_ROOT }).failures, 0);

    // ③ 守卫不绿的建单：CLI 退出码 1，且**不留残 file**
    const before = fs.readdirSync(dir).sort();
    assert.equal(
      quiet(() => cliMain(['--add', '--title', '缺 repro', '--type', 'bug', '--status', 'doing', '--parent', rootN.fields.id, '--dir', dir, '--write'])),
      1,
    );
    assert.deepEqual(fs.readdirSync(dir).sort(), before, '被拒后目录必须回到原样');
    assert.equal(loadNodes(dir).length, 2);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('写盘：同输入同字节、写后回读复验、新建失败不留残file', () => {  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-'));
  try {
    const n = N({ title: '写盘测试' });
    assert.equal(saveNode(n, dir).ok, true);
    const text = fs.readFileSync(path.join(dir, `${n.name}.md`), 'utf8');
    assert.equal(text, canonicalNode(n), '落盘的就是规范形态');
    assert.equal(saveNode(n, dir).ok, true);
    assert.equal(fs.readFileSync(path.join(dir, `${n.name}.md`), 'utf8'), text, '再写一次同字节（幂等）');
    assert.equal(loadNodes(dir).length, 1);
    assert.equal(deleteNode(n.name, dir).ok, true);
    assert.equal(loadNodes(dir).length, 0);
    assert.equal(deleteNode(n.name, dir).ok, false, '删不存在的节点要如实报错');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('★ 写后回滚：守卫不接受的新增/修改**必须把磁盘还原**（不许只报"已回滚"却留残file）', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-rb-'));
  try {
    const rootNode = N({ title: '根' });
    assert.equal(saveNode(rootNode, dir).ok, true);
    const before = fs.readdirSync(dir).sort();

    // 记录"拒绝一个坏节点"时该发生什么：CLI 的 apply 负责删/还原 —— 这里直接验它的两条路径
    //   ① 新增被拒 ⇒ 文件必须消失
    const badNew = N({ title: '坏的新增', fields: { parent: rootNode.fields.id, type: 'bug', status: 'doing' } }); // 缺 repro
    assert.equal(saveNode(badNew, dir).ok, true);
    assert.ok(validateAll(loadNodes(dir), { repoRoot: REPO_ROOT }).failures > 0, '前置：这个节点确实过不了守卫');
    assert.equal(deleteNode(badNew.name, dir).ok, true);
    assert.deepEqual(fs.readdirSync(dir).sort(), before, '新增被拒后目录必须回到原样');

    //   ② 修改被拒 ⇒ 原内容必须原样写回（逐字节）
    const original = fs.readFileSync(path.join(dir, `${rootNode.name}.md`), 'utf8');
    const broken = { ...rootNode, fields: { ...rootNode.fields, verify: 'tools/test/不存在.mjs' }, };
    assert.equal(saveNode(broken, dir).ok, true);
    assert.ok(validateAll(loadNodes(dir), { repoRoot: REPO_ROOT }).failures > 0, '前置：这个改动确实过不了守卫');
    assert.equal(saveNode(rootNode, dir).ok, true);
    assert.equal(fs.readFileSync(path.join(dir, `${rootNode.name}.md`), 'utf8'), original, '修改被拒后必须逐字节还原');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('写盘：字段**插入顺序**不同不算不一致 —— 落盘一律拉成 KEY_ORDER（回归：这里曾误报"回读不一致"）', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-req-order-'));
  try {
    const name = ulid();
    const fields = { id: nodeId(name), type: 'req', status: 'done', parent: 'null', tags: ['b', 'a'], verify: 'x#y' };
    const n = { name, title: '键序测试', fields, body: '## 判据\n\n一条。', lines: 3, file: 'x' };
    const res = saveNode(n, dir);
    assert.equal(res.ok, true, `键序不该导致回滚：${res.reason ?? ''}`);
    const text = fs.readFileSync(path.join(dir, `${name}.md`), 'utf8');
    const order = [...text.matchAll(/^- ([a-z_]+):/gm)].map((m) => m[1]);
    assert.deepEqual(order, ['id', 'type', 'status', 'parent', 'verify', 'tags'], '落盘必须按 KEY_ORDER 排（tags 在最后）');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

// ─────────────────────────────────────────── ② 端到端：真树必须绿

test('端到端：真实 data/requirements/ 过全部不变量（含 verify 指向的守卫真实存在）', () => {
  assert.ok(fs.existsSync(DEFAULT_REQUIREMENTS_DIR), 'data/requirements/ 必须存在');
  assert.ok(
    fs.existsSync(path.join(DEFAULT_REQUIREMENTS_DIR, 'README.md')),
    'data/requirements/README.md 必须在（它是这个目录的散文口径；节点文件都在，它没了就等于口径没了）',
  );
  const nodes = loadNodes();
  assert.ok(nodes.length > 0, '需求树不能是空的（根节点必须真实存在）');
  assert.ok(
    nodes.some((n) => n.fields.parent === 'null'),
    '树必须有唯一的根（parent: null）',
  );
  const report = validateAll(nodes, { repoRoot: REPO_ROOT });
  const bad = report.checks.filter((c) => c.problems.length).map((c) => `#${c.id} ${c.text}：\n      ${c.problems.join('\n      ')}`);
  assert.deepEqual(bad, [], `需求树必须全绿，实际：\n${bad.join('\n')}`);
});
