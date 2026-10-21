/** @env pure @kind gate @why spec 层的不变量被改松（未知被洗成省略/舍弃、未登记义务仍绿、正文内联布局） */
/**
 * tools/test/spec.test.mjs —— spec 层（`lib/spec.mjs`）的**契约与判别力**
 *
 * 它守两件事：
 * ① **真源自洽**：`data/spec/{spec,bindings,audit}.json` 过得了全部不变量；试点域里未登记义务 = 0。
 * ② **守卫会红**：对每个不变量施加一处已知破坏（负例样本），要求对应的检查项报出来。
 *    —— 没有这一半，"验证器通过"只说明它没崩，不说明它认得错。
 *
 * 运行：`pnpm test`（纯文本 / 纯函数；语料侧的 EA 对账在 `spec-corpus.test.mjs` 的 assets 档）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  derive,
  describe,
  loadLedger,
  loadSpec,
  reportText,
  resolveStorage,
  validate,
} from '../lib/spec.mjs';
import { computeEffectCoverage } from '../lib/spec-effects.mjs';

const ledger = loadLedger();

/** 最小合法样本：1 章 + 2 个试点块 + 1 条规则；一个 effect 被 covers、一个被 residual 点名 ⇒ 未登记 = 0 */
function baseSpec() {
  const text = '这是规范正文：该操作数经读原语得到整数。';
  return {
    schema: 3,
    title: '样本',
    pilot: {
      name: 'p',
      question: 'q',
      sites: [
        { ea: '0x41bfdb', note: 'read-int case 3' },
        { ea: '0x41c0c0', note: 'read-int case 4' },
      ],
      internal: [{ kind: 'reg', why: '寄存器搬移是内部步骤' }],
      residuals: [{ id: 'RES-X', blocks: ['0x41c0c0'], why: '未处理', next: '写最小合同' }],
      outOfScope: [{ what: 'x', why: 'y', chapter: 'c1' }],
    },
    chapters: [
      {
        id: 'c1',
        title: 't',
        file: 'chapters/c1.md',
        status: 'bootstrap',
        residual: [],
        text: `## t\n\n### 规则\n\n<!-- rule: E1 -->\n\n${text}`,
      },
    ],
    planned: [],
    gaps: [],
    entries: [
      {
        id: 'E1',
        chapter: 'c1',
        title: '规则',
        text,
        status: 'reviewed',
        disposition: 'handled',
        falsifiers: ['若该站点读出的值不经 DEC ⇒ 本条错'],
        binding: {
          sites: [{ ea: '0x41bfdb', what: 'w' }],
          functions: ['sub_41BF50'],
          storage: [],
          evidence: ['KN:01M48F4HYZ070F651D3Z7N4N4R'],
        },
        covers: [{ blocks: ['0x41bfdb'], kinds: ['mem-read'] }],
        residual: [{ id: 'R1', kind: 'unknown', what: 'w', next: 'n' }],
      },
    ],
  };
}

const clone = () => JSON.parse(JSON.stringify(baseSpec()));
const checksOf = (spec) => new Set(validate(spec, { ledger }).problems.map((p) => p.check));
const FX = [
  { id: '0x41bfdb/mem-read/0', site: '0x41bfdb', block: '0x41bfdb', op: 'mov', kind: 'mem-read' },
  { id: '0x41c0c0/mem-read/0', site: '0x41c0c0', block: '0x41c0c0', op: 'mov', kind: 'mem-read' },
  { id: '0x41bfdb/reg/0', site: '0x41bfdb', block: '0x41bfdb', op: 'mov', kind: 'reg' },
];

const REVIEW_RE = [/\bsub_[0-9A-F]{6}\b/, /\bKN:[0-9A-HJKMNP-TV-Z]{26}\b/, /\bguard:/, /\bEngine\s*\+\s*0x/i, /\b0x5[0-9a-f]{4}\b/i, /\blayout:/, /\bshape:/, /\bdata\/(?:ledger|shape|spec)\b/, /<!--\s*rule:/];
const reviewInfoHits = (text) => REVIEW_RE.filter((re) => re.test(text)).map(String);
const bodyHasNoReviewInfo = (text) => reviewInfoHits(text).length === 0;


test('★ 最小合法样本：0 条问题，且未登记 = 0', () => {
  const { problems, derived } = validate(baseSpec(), { ledger });
  assert.deepEqual(problems, []);
  assert.equal(derived.pilotSites, 2);
  assert.equal(derived.effects, null);
  const cov = computeEffectCoverage(baseSpec(), FX);
  assert.deepEqual(cov.unregistered, []);
  assert.equal(cov.covered, 1);
  assert.equal(cov.internal, 1);
  assert.equal(cov.residual, 1);
});

test('★ 判别力：去掉 covers ⇒ effect 变成"未登记"', () => {
  const s = clone();
  delete s.entries[0].covers;
  const cov = computeEffectCoverage(s, FX);
  assert.equal(cov.covered, 0);
  assert.ok(cov.unregistered.some((e) => e.id === '0x41bfdb/mem-read/0'), '未被 covers 的 effect 必须未登记');
});

test('★ 判别力：同一 effect 既被解释又被登记为残差 ⇒ 残差冗余（#9 红）', () => {
  const s = clone();
  s.pilot.residuals.push({ id: 'RES-DUP', blocks: ['0x41bfdb'], why: 'w', next: 'n' });
  const cov = computeEffectCoverage(s, FX);
  assert.ok(cov.redundantResidual.length > 0);
});

test('★ 判别力：正文内联绝对地址 / 记录内偏移 ⇒ #8 红', () => {
  for (const bad of ['值在 Engine+0x5D800 里', '值在 帧+0x34 里', '值在 0x5D8B4 里', '见 sub_41BF50', '证据 KN:01M48F4HYZ070F651D3Z7N4N4R']) {
    const s = clone();
    s.chapters[0].text = `## t\n\n### 规则\n\n<!-- rule: E1 -->\n\n${bad}`;
    assert.ok(checksOf(s).has(8), `必须拦住：${bad}`);
  }
});

test('★ 判别力：handled 没有机器绑定 ⇒ #5 红', () => {
  const s = clone();
  s.entries[0].binding.sites = [];
  s.entries[0].binding.functions = [];
  assert.ok(checksOf(s).has(5));
});

test('★ 判别力：unhandled / discarded 却 covers 候选 ⇒ #5 红（未处理/舍弃不能冒充解释）', () => {
  const un = clone();
  un.entries[0].disposition = 'unhandled';
  assert.ok(checksOf(un).has(5));
  const dis = clone();
  dis.entries[0].disposition = 'discarded';
  dis.entries[0].discard = { reason: 'r', range: 'g', impact: 'i' };
  assert.ok(checksOf(dis).has(5));
});

test('★ 判别力：replaced 缺 preserves/discards/chosen、omitted 缺依据 ⇒ #5 红', () => {
  const rep = clone();
  rep.entries[0].disposition = 'replaced';
  assert.ok(checksOf(rep).has(5));
  const om = clone();
  om.entries[0].disposition = 'omitted';
  assert.ok(checksOf(om).has(5));
});

test('★ 判别力：replaced 的 preserves/discards 越出 scope ⇒ #5 红（放电越界）', () => {
  const good = clone();
  good.entries[0].disposition = 'replaced';
  good.entries[0].replacement = {
    preserves: ['0x41bfdb'],
    discards: ['sub_41BF50'],
    chosen: '把整型读替换成等价的领域取值',
    scope: { sites: ['0x41bfdb'], functions: ['sub_41BF50'] },
  };
  assert.ok(!checksOf(good).has(5), '合法替换不该红');
  const bad = clone();
  bad.entries[0].disposition = 'replaced';
  bad.entries[0].binding.sites.push({ ea: '0x41c0c0', what: 'w2' });
  bad.entries[0].replacement = {
    preserves: ['0x41bfdb', '0x41c0c0'], // 0x41c0c0 在 binding 里、但不在 scope 里
    discards: ['sub_41BF50'],
    chosen: 'c',
    scope: { sites: ['0x41bfdb'], functions: ['sub_41BF50'] },
  };
  assert.ok(checksOf(bad).has(5), 'preserves 不在 scope 里必须红');
  const noScope = clone();
  noScope.entries[0].disposition = 'replaced';
  noScope.entries[0].replacement = { preserves: ['0x41bfdb'], discards: ['sub_41BF50'], chosen: 'c' };
  assert.ok(checksOf(noScope).has(5), '没有 scope 就谈不上越界判据');
});

test('★ 判别力：discarded 的 range 只认 EA / 函数符号；omitted 的证据要解析得到', () => {
  const badRange = clone();
  badRange.entries[0].disposition = 'discarded';
  badRange.entries[0].covers = [];
  badRange.entries[0].discard = { reason: 'r', range: '整段都不保留', impact: 'i' };
  assert.ok(checksOf(badRange).has(5));
  const okRange = clone();
  okRange.entries[0].disposition = 'discarded';
  okRange.entries[0].covers = [];
  okRange.entries[0].discard = { reason: 'r', range: ['0x41bfdb'], impact: 'i' };
  assert.ok(!checksOf(okRange).has(5), 'EA 形态的 range 不该红');
  const badOmit = clone();
  badOmit.entries[0].disposition = 'omitted';
  badOmit.entries[0].omission = { why: '内部步骤', evidence: ['KN:00000000000000000000000000'] };
  assert.ok(checksOf(badOmit).has(5), 'omission 的证据也要解析得到');
});

test('★ 判别力：reviewed 里夹一条 proposed ⇒ #7 红（reviewed 要求证据链全过 accepted）', () => {
  const s = clone();
  s.entries[0].binding.evidence.push('KN:01M4BB1GJG393S1W4J7W0S3P3T');
  assert.ok(checksOf(s).has(7));
});

test('★ 判别力：covers 选择器的块不在 binding.sites ⇒ #5 红（机器见证缺失）', () => {
  const s = clone();
  s.entries[0].covers = [{ blocks: ['0x41c0c0'], kinds: ['mem-read'] }]; // 绑定里只有 0x41bfdb
  assert.ok(checksOf(s).has(5), '缺站点必须点名');
});

test('★ 判别力：replacement.scope 不能凭空写（不在 binding 里）/ 必须交代完 ⇒ #5 红', () => {
  const fabricated = clone();
  fabricated.entries[0].disposition = 'replaced';
  fabricated.entries[0].replacement = {
    preserves: ['0x1'], discards: ['0x1'], chosen: 'c', scope: { sites: ['0x1'] },
  };
  assert.ok(checksOf(fabricated).has(5), 'scope 里的 EA 不在 binding 里必须红');

  const leftover = clone();
  leftover.entries[0].disposition = 'replaced';
  leftover.entries[0].binding.sites.push({ ea: '0x41c0c0', what: 'w2' });
  leftover.entries[0].replacement = {
    preserves: ['0x41bfdb'],
    discards: ['sub_41BF50'],
    chosen: 'c',
    scope: { sites: ['0x41bfdb', '0x41c0c0'], functions: ['sub_41BF50'] },
  };
  assert.ok(checksOf(leftover).has(5), 'scope 里没被 preserves/discards 交代的项必须红');
});

test('★ 判别力：残差 omitted 的自由文本 / discarded 缺 reason+range ⇒ #4 红', () => {
  const fake = clone();
  fake.entries[0].residual = [
    { id: 'R1', kind: 'omitted', what: 'w', next: 'n', evidence: '随口一句', candidate: 'ea:0x41c0c0' },
  ];
  assert.ok(checksOf(fake).has(4), '残差 omitted 的 evidence 必须是可解析的数组');

  const weak = clone();
  weak.entries[0].residual = [
    { id: 'R1', kind: 'omitted', what: 'w', next: 'n', evidence: ['KN:01M4BB1GJG393S1W4J7W0S3P3T'], candidate: 'ea:0x41c0c0' },
  ];
  assert.ok(checksOf(weak).has(4), '残差 omitted 必须有强证据，proposed 不够');

  const dis = clone();
  dis.entries[0].residual = [
    { id: 'R1', kind: 'discarded', what: 'w', next: 'n', impact: 'i', candidate: 'ea:0x41c0c0' },
  ];
  assert.ok(checksOf(dis).has(4), '残差 discarded 必须给 reason + range');
});

test('★ 判别力：storage 指针落空 ⇒ #6 红；成员指针必须落在 shape 的成员起点上', () => {
  const bad = clone();
  bad.entries[0].binding.storage = ['shape:T_Command@0x9'];
  assert.ok(checksOf(bad).has(6), 'shape 成员起点不存在 ⇒ #6 红');

  const unknownType = clone();
  unknownType.entries[0].binding.storage = ['shape:NOPE'];
  assert.ok(checksOf(unknownType).has(6), '类型不存在 ⇒ #6 红');

  const layoutRef = clone();
  layoutRef.entries[0].binding.storage = ['layout:GLOBAL_SLOTS.base.int'];
  assert.ok(checksOf(layoutRef).has(6), '`layout:` 不再被接受：机器本体只由 shape 提供');

  const ok = clone();
  ok.entries[0].binding.storage = ['shape:T1@0x60'];
  assert.ok(!checksOf(ok).has(6), '真实存在的 shape 成员必须解析得到');
});

test('storage 指针只认 shape：bindings 里 0 处 layout:', () => {
  const bindings = JSON.parse(readFileSync(new URL('../../data/spec/bindings.json', import.meta.url), 'utf8'));
  const refs = bindings.bindings.flatMap((b) => b.storage ?? []).filter((s) => s.startsWith('layout:'));
  assert.deepEqual(refs, [], '`layout:` 指针会让规范重新依赖 age-format；机器本体只由 shape 提供');
  assert.equal(resolveStorage('layout:FRAME_LAYOUT.base').ok, false, '解析器也不接受 `layout:`');
});

test('★ 判别力：证据失效 / 用例不存在 / reviewed 只有弱证据 ⇒ #7 红', () => {
  const missing = clone();
  missing.entries[0].binding.evidence = ['KN:00000000000000000000000000'];
  assert.ok(checksOf(missing).has(7));

  // ★ 必须是 draft 也算红：否则"reviewed 没有强证据"那条会**替**它变红，
  //   于是"失效证据被静默跳过"在 draft 条目上就没人抓（变异测试实测踩过）。
  const draftMissing = clone();
  draftMissing.entries[0].status = 'draft';
  draftMissing.entries[0].binding.evidence = ['KN:00000000000000000000000000'];
  assert.ok(checksOf(draftMissing).has(7), 'draft 条目的失效证据也必须红');

  const weak = clone();
  weak.entries[0].binding.evidence = ['KN:01M4BB1GJG393S1W4J7W0S3P3T']; // proposed
  assert.ok(checksOf(weak).has(7), 'reviewed 不能只靠 proposed');

  const badGuard = clone();
  badGuard.entries[0].binding.evidence = ['guard:tools/test/spec.test.mjs#这个用例不存在'];
  assert.ok(checksOf(badGuard).has(7));

  // ★ assets 档守卫默认门禁不跑 ⇒ 不能单独撑 reviewed（要么有 accepted 台账，要么有 @env pure 守卫）
  const assetsOnly = clone();
  assetsOnly.entries[0].binding.evidence = ['guard:tools/test/emulator-model.test.mjs#local 池的 slot 几何'];
  assert.ok(checksOf(assetsOnly).has(7), '只有 assets 守卫不能撑 reviewed');
});

test('★ 判别力：reviewed 没有 falsifiers ⇒ #7 红（不可证伪的合同不许贴 reviewed）', () => {
  const s = clone();
  delete s.entries[0].falsifiers;
  assert.ok(checksOf(s).has(7));
});

test('★ 判别力：未知章节 / 非法 dispose / 非法 residual kind ⇒ #4 红', () => {
  const s = clone();
  s.entries[0].chapter = 'nope';
  assert.ok(checksOf(s).has(4));
  const d = clone();
  d.entries[0].disposition = 'done-ish';
  assert.ok(checksOf(d).has(4));
  const r = clone();
  r.entries[0].residual[0].kind = 'whatever';
  assert.ok(checksOf(r).has(4));
});

test('★ 判别力：试点域重复类型 / 空 outOfScope ⇒ #3 / #10 红', () => {
  const s = clone();
  s.pilot.sites.push({ ...s.pilot.sites[0] });
  assert.ok(checksOf(s).has(3));
  const o = clone();
  o.pilot.outOfScope = [];
  assert.ok(checksOf(o).has(10));
});

test('★ 判别力：章节状态与条目打架（bootstrap 空壳 / none 藏条目）⇒ #11 红', () => {
  const empty = clone();
  empty.chapters[0].status = 'bootstrap';
  empty.entries[0].chapter = 'c2';
  empty.chapters.push({ id: 'c2', title: 't', status: 'partial', question: 'q', residual: [] });
  assert.ok(checksOf(empty).has(11), 'bootstrap 章没有条目必须红');
  const hidden = clone();
  hidden.chapters[0].status = 'none';
  assert.ok(checksOf(hidden).has(11), 'none 章挂条目必须红');
});

test('★ 试点块是**机器站点**：每个块入口都带 ea（本试点是 x86 块账）', () => {
  const model = loadSpec().model;
  assert.ok(model.pilot.sites.length > 0);
  for (const c of model.pilot.sites) {
    assert.match(c.ea, /^0x[0-9a-f]+$/, `试点块 ${c.ea} 缺 ea —— 试点必须是机器块`);
  }
});

test('★ 派生报告不输出百分数（范围闭合 ≠ 解释正确）', () => {
  const model = loadSpec().model;
  const { problems, derived, weak } = validate(loadSpec(), { ledger });
  assert.deepEqual(problems, []);
  const text = reportText(model, { file: 'x', derived, weak });
  assert.ok(!text.includes('%'), '报告里不许出现百分数');
  assert.ok(text.includes('未登记'));
  assert.ok(text.includes('诚实项'));
});

test('★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0', () => {
  const bundle = loadSpec();
  const { problems, derived } = validate(bundle, { ledger });
  assert.deepEqual(problems, [], `${bundle.files.spec} / ${bundle.files.bindings} / ${bundle.files.audit} 有不变量失败`);
  assert.deepEqual(derived.unregistered, [], '试点域里存在未登记义务');
  assert.equal(derived.gaps, 0, '立即数形态现在登记在 pilot.residuals，不是 gaps');
  assert.ok(derived.effects > 0, '语料在场时应当现算 effect');
  assert.equal(derived.unregistered.length, 0, 'effect 必须全部归属');
  assert.ok(derived.covered > 0 && derived.residualEffects > 0);
});

test('★ 跨文件：章节规则 / bindings / audit 必须按 id 一一对应（少一份 / 多一份 / 拼错 id ⇒ #12 红）', () => {
  const model = baseSpec();
  const bundle = {
    spec: {
      schema: 3,
      title: model.title,
      chapters: [{ id: 'c1', file: 'chapters/c1.md', title: 't' }],
    },
    chapters: [{ id: 'c1', title: 't', text: model.chapters[0].text }],
    bindings: {
      schema: 3,
      bindings: model.entries.map((e) => ({ rule: e.id, ...e.binding })),
    },
    audit: {
      schema: 3,
      pilot: model.pilot,
      chapters: [{ id: 'c1', status: 'bootstrap', residual: [] }],
      planned: [],
      rules: model.entries.map((e) => ({
        id: e.id, status: e.status, disposition: e.disposition, covers: e.covers, residual: e.residual, falsifiers: e.falsifiers,
      })),
      gaps: [],
    },
  };
  assert.deepEqual(validate(bundle, { ledger, effects: FX }).problems, []);
  const missing = JSON.parse(JSON.stringify(bundle));
  missing.audit.rules = [];
  assert.ok(validate(missing).problems.some((p) => p.check === 12), '缺 audit 规则必须红');
  const orphan = JSON.parse(JSON.stringify(bundle));
  orphan.bindings.bindings.push({ rule: 'NOPE', sites: [], functions: [], storage: [], evidence: [] });
  assert.ok(validate(orphan).problems.some((p) => p.check === 12), '孤儿绑定必须红');
  const noHeading = JSON.parse(JSON.stringify(bundle));
  noHeading.chapters[0].text = '### 规则\n\n<!-- rule: E1 -->\n\n正文';
  noHeading.chapters[0].text = noHeading.chapters[0].text.replace('### 规则', '规则（没有 ###）');
  assert.ok(validate(noHeading).problems.some((p) => p.check === 12), '标记不在 ### 小节里必须红');
});

test('★ 分发正文：每章 Markdown 自包含（无汇编引用 / 台账 id / 守卫 / 本仓路径）', () => {
  const { model } = loadSpec();
  assert.ok(model.chapters.length > 0);
  for (const c of model.chapters) {
    assert.ok(String(c.text ?? '').trim(), `章节 ${c.id} 不能为空`);
    const text = String(c.text).replace(/<!--\s*rule:[^>]*-->/g, '');
    assert.ok(bodyHasNoReviewInfo(text), `章节 ${c.id} 含复核信息：${reviewInfoHits(text).join(' / ')}`);
  }
});

test('★ 审计缺口：只进 audit；缺 note / residual / 章节 ⇒ #13 红', () => {
  const good = clone();
  good.gaps = [{ id: 'GAP-T', chapter: 'c1', title: 't', note: 'n', residual: [{ id: 'RG', kind: 'unknown', what: 'w', next: 'n' }] }];
  assert.ok(!checksOf(good).has(13), '合法缺口不该红');
  const noResidual = clone();
  noResidual.gaps = [{ id: 'GAP-T', chapter: 'c1', title: 't', note: 'n', residual: [] }];
  assert.ok(checksOf(noResidual).has(13), '缺口必须点名具体义务');
  const badChapter = clone();
  badChapter.gaps = [{ id: 'GAP-T', chapter: 'nope', title: 't', note: 'n', residual: [{ id: 'RG', kind: 'unknown', what: 'w', next: 'n' }] }];
  assert.ok(checksOf(badChapter).has(13), '缺口不能指向不存在的章节');
});

test('★ 自描述：不变量 / 处理状态 / 残差类别 / 操作都在，且不说覆盖率', () => {
  const d = describe();
  assert.ok(d.invariants.length >= 13, '不变量至少 13 条');
  assert.ok(d.invariants.some(([id, text]) => id === 11 && text.includes('章节状态')));
  assert.ok(d.invariants.some(([id, text]) => id === 12 && text.includes('跨文件')));
  assert.ok(d.invariants.some(([id, text]) => id === 13 && text.includes('审计缺口')));
  assert.ok(d.dispositions.handled && d.dispositions.replaced && d.dispositions.omitted && d.dispositions.discarded && d.dispositions.unhandled);
  assert.ok(d.residualKinds.unknown && d.residualKinds.unhandled && d.residualKinds.omitted && d.residualKinds.discarded);
  assert.ok(d.operations.some(([name]) => name === 'validate'));
  assert.ok(!d.operations.some(([name]) => name === 'render'), '不分发单文件：不再有 render 操作');
  assert.ok(d.writePath.includes('没有写操作'));
});
