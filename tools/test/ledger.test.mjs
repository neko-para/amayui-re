#!/usr/bin/env node
/** @env pure @kind gate @why 台账 schema/锚点/分类轴/DB 可重建被破坏 */
/**
 * tools/test/ledger.test.mjs —— **知识台账的守卫**（M3 的判据 G1–G5 + 分类轴节点全在这里）
 *
 * 这四条是从旧仓模拟器 `test/` 下**收回台账域**的静态守卫（它们只读台账、零产品代码依赖，
 * 详见 `pnpm tools requirements show 7T0D3C3S6EDTAPMG347DY3`）：
 *
 *   | 旧仓（`app/amayui-emulator/test/`） | 它真正校验的 | 本文件里的落点 |
 *   |---|---|---|
 *   | `journal.test.ts`（沿革台账结构自检） | 格式与规则**只有一份**（工具与守卫同一套判据） | #1 · #2 |
 *   | `capability-ledger.test.ts`（schema + guard 指向的**用例**真的存在 + md 与数据同步） | "声称有守卫"必须能机械核对 | #3 · #4 · #6 |
 *   | `script-ledger.test.ts`（**锚点棘轮**：锚必须真出现在声明的区间里 + links 不悬空） | 锚点可再校验 + 引线不悬空 | #3 · #5 |
 *   | `ticket-ledger.test.ts`（`done` 必须带真实存在的守卫 + 证据锚点棘轮） | 收口凭据不许空口声称 | #4 · #6 |
 *
 * ★ 与旧仓那四条最大的不同：旧仓的守卫**写死了旧仓的路径与字段**（`analysis/*.json` / `tickets/**`），
 * 搬过来只会在新仓里红得没意义。这里守的是**本台账 schema 的不变量**，
 * 而 schema 的真源只有一份：`pnpm tools ledger describe`（本文件也断言这一点）。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import {
  CHECK_TITLES,
  DEFAULT_LEDGER_DIR,
  DISPOSITIONS,
  DOMAIN_ONLY,
  EFFECTIVE,
  KINDS,
  REPOS,
  STATUSES,
  appendRecord,
  buildVocabulary,
  describe,
  describeText,
  isoNow,
  monthOf,
  newUlid,
  peOffsetOf,
  project,
  readRecords,
  rebuildTwiceDigest,
  resolveDomain,
  serializeRecord,
  testNames,
  ulidTime,
  validateAll,
} from '../lib/ledger.mjs';

const LEDGER_DIR = path.join(REPO_ROOT, DEFAULT_LEDGER_DIR);
/** 本仓里一条**真实存在**的守卫，用来当锚点（换了名字这里会红 —— 那正是要点） */
const REAL_GUARD = { path: 'tools/test/requirements.test.mjs', test: '#5 一屏预算' };
/** 守卫用的**测试词表**：默认域 `Engine`、子域 `Engine.audio`，以及它的一条历史别名 `声音` */
const SYS = 'Engine';
const ALT = 'Engine.audio';
const LEGACY = '声音';
/** 一条整名不存在的用例名（别用短串 —— 短串会在断言消息/注释里命中，那是假绿） */
const GHOST = '★ 用例名故意不存在 GA4sT';

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'ledger-guard-'));
const rm = (d) => fs.rmSync(d, { recursive: true, force: true });
/** 与 CLI 同一条口径（守卫与工具必须是同一套判据 —— 旧仓 journal 的教训就是"文档说有守卫而没有"） */
const ctxFor = (dir) => ({ repoRoot: REPO_ROOT, referenceRoot: null, ledgerDir: dir, tracked: null, parsedProblems: [] });

let seq = 0;
function rec(over = {}) {
  // ★ 每条给不同的时刻 ⇒ ULID 不同（守卫要能造出"两条记录"，而不是手搓乱序日志）
  const id = over.id ?? newUlid(Date.now() + (seq += 1000));
  return {
    id,
    at: over.at ?? isoNow(ulidTime(id)),
    kind: over.kind ?? 'claim',
    system: over.system ?? SYS,
    subject: over.subject ?? 'Engine+0x5D880',
    claim: over.claim ?? '占位断言',
    anchor: over.anchor ?? [{ type: 'guard', repo: 'self', ...REAL_GUARD }],
    ...(over.status !== undefined ? { status: over.status } : {}),
    ...(over.replaces !== undefined ? { replaces: over.replaces } : {}),
    ...(over.disposition !== undefined ? { disposition: over.disposition } : {}),
    ...(over.aliases !== undefined ? { aliases: over.aliases } : {}),
    ...(over.splitInto !== undefined ? { splitInto: over.splitInto } : {}),
    ...(over.note !== undefined ? { note: over.note } : {}),
  };
}
/** 一条 `kind=domain` 的词表记录；`system` = 它的**父域**（顶层域自己就是自己的父域） */
const dom = (subject, disposition, extra = {}) =>
  rec({ kind: 'domain', system: extra.system ?? SYS, subject, disposition, claim: extra.claim ?? `${subject} 进词表`, ...extra });

/** 守卫用的词表：`Engine`（顶层）+ `Engine.audio`（带历史别名 `声音`） */
const VOCAB = [dom(SYS, 'added'), dom(ALT, 'renamed', { aliases: [LEGACY], claim: '声音 → Engine.audio（含义没变，只是叫法归一）' })];

/** 手写一行坏记录（真写路径拒绝写坏数据 ⇒ 只能手搓，用来验守卫真的会红） */
function writeRaw(dir, kind, obj) {
  const f = path.join(dir, kind, `${monthOf(obj.id)}.jsonl`);
  fs.mkdirSync(path.dirname(f), { recursive: true });
  fs.appendFileSync(f, `${JSON.stringify(obj)}\n`);
  return f;
}

const seed = (dir, recs) => {
  for (const r of recs) appendRecord(dir, r);
};
/** 带词表的台账（业务用例都要 —— 否则 `system` 追不到词表，#7 会红） */
const seedWithVocab = (dir, recs = []) => seed(dir, [...VOCAB, ...recs]);

const failuresOf = (dir, extra = {}) => {
  const { records, problems } = readRecords(dir);
  return validateAll(records, { ...ctxFor(dir), parsedProblems: problems, ...extra });
};
/**
 * 只保留**业务记录**引起的问题。判据按**文件**分：词表记录住在 `<ledgerDir>/domain/**`。
 * ★ 不能按消息文本过滤（业务记录的问题消息里也常出现 `kind=domain` 这种"修法提示"）；
 * ★ 也不能只判"路径里含 /domain/"（**临时目录可能恰好在别处**，那是脆弱的字符串守卫）；
 *   所以用**相对 ledgerDir** 的路径判，且只看前缀。
 */
const isVocabProblem = (p, dir) => {
  if (!p.file) return false;
  const rel = path.relative(dir, p.file).split(path.sep).join('/');
  return rel.startsWith('domain/');
};
const dataChecks = (dir, extra) =>
  failuresOf(dir, extra).checks.map((c) => ({ id: c.id, problems: c.problems.filter((p) => !isVocabProblem(p, dir)) }));
const failedData = (dir, extra) => dataChecks(dir, extra).filter((c) => c.problems.length).map((c) => c.id);
const problemsIn = (dir, id, extra) => dataChecks(dir, extra).find((c) => c.id === id)?.problems ?? [];
/** 词表记录自己的问题（分类轴用例单独看） */
const vocabProblems = (dir) => failuresOf(dir).checks.flatMap((c) => c.problems.filter((p) => isVocabProblem(p, dir)));

// ─────────────────────────────────────────────────────────── ① 形态（schema 合法）

test('#1 形态：schema 合法；坏记录必须红（不静默跳过）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({}), rec({ subject: 'Engine+0x5D88C', claim: '另一条断言' })]);
    assert.deepEqual(failedData(dir), [], '两条合法记录必须绿');

    const bads = [
      [{ subject: '' }, /subject/],
      [{ subject: 'Engine 0x5D880' }, /subject/],
      [{ claim: '   ' }, /claim/],
      [{ kind: 'conclusion' }, /kind/],
      [{ status: 'confirmed' }, /status/],
      [{ anchor: [] }, /anchor/],
      [{ at: '2026-10-04 09:00:00' }, /at/],
      [{ system: '' }, /system/],
      [{ system: 'Renderer core' }, /system/],
    ];
    for (const [over, re] of bads) {
      const d2 = tmp();
      try {
        seedWithVocab(d2);
        // 手写坏行（坏记录**写不进**真写路径的 schema —— 那正是"写路径不写坏文件"）
        const r = { id: newUlid(), at: isoNow(), kind: 'claim', system: SYS, subject: 'S+0x1', claim: 'c', anchor: [REAL_GUARD], ...over };
        if (over.kind !== undefined) r.kind = over.kind;
        writeRaw(d2, 'claim', r);
        assert.ok(failedData(d2).includes(1), `坏记录 ${JSON.stringify(over)} 必须让 #1 红`);
        const text = problemsIn(d2, 1).map((p) => p.reason).join(' ');
        assert.match(text, re, `报错要点名是哪个字段：${text}`);
      } finally {
        rm(d2);
      }
    }
  } finally {
    rm(dir);
  }
});

test('#1 形态：id 重复 / 非法 JSON / kind 与目录不符 ⇒ 红', () => {
  const dir = tmp();
  try {
    const dup = rec({});
    seedWithVocab(dir, [dup]);
    const f = path.join(dir, 'claim', `${monthOf(dup.id)}.jsonl`);
    fs.appendFileSync(f, `${serializeRecord(dup)}\n`);
    assert.ok(failedData(dir).includes(1), 'id 重复必须红');
    fs.appendFileSync(f, '{ 这不是 JSON\n');
    assert.ok(failedData(dir).includes(1), '坏 JSON 行必须红（不静默跳过）');

    const d2 = tmp();
    try {
      seedWithVocab(d2);
      // kind=claim 的行落在 note/ 目录 ⇒ 与目录不符（且 #1 已因 kind/目录不符记它 malformed ⇒ #2 不重复报）
      writeRaw(d2, 'note', rec({ kind: 'claim' }));
      const text = problemsIn(d2, 1).map((p) => p.reason).join(' ');
      assert.match(text, /与所在目录/, text);
    } finally {
      rm(d2);
    }
  } finally {
    rm(dir);
  }
});

test('★ #1 域专属字段只许出现在 kind=domain 上（同"缺陷专属字段"的纪律）', () => {
  for (const k of DOMAIN_ONLY) {
    const dir = tmp();
    try {
      seedWithVocab(dir);
      writeRaw(dir, 'claim', { ...rec({}), [k]: k === 'splitInto' ? ['a', 'b'] : ['x'] });
      const text = problemsIn(dir, 1).map((p) => p.reason).join(' ');
      assert.match(text, /域记录专属/, text);
      assert.ok(text.includes(k), `要点名 ${k}：${text}`);
    } finally {
      rm(dir);
    }
  }
});

// ─────────────────────────────────────────────────────────── ② 追加序（append-only）

test('#2 追加序：文件内 ULID 必须严格递增（只许往后加）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir);
    const older = rec({});
    const newer = rec({});
    assert.ok(older.id < newer.id, 'ULID 字典序 == 时间序');
    writeRaw(dir, 'claim', newer);
    writeRaw(dir, 'claim', older);
    const text = problemsIn(dir, 2).map((p) => p.reason).join(' ');
    assert.ok(failedData(dir).includes(2), '插入到中间 / 改历史必须红');
    assert.match(text, /追加序破坏/, text);
  } finally {
    rm(dir);
  }
});

test('#2 追加序：at 的月份、ULID 的月份、文件名三者必须一致', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir);
    const id = newUlid();
    writeRaw(dir, 'claim', { ...rec({ at: '2020-01-15T00:00:00.000Z' }), id });
    const text = problemsIn(dir, 2).map((p) => p.reason).join(' ');
    assert.ok(failedData(dir).includes(2), 'at 与 ULID 月份不一致必须红');
    assert.match(text, /月份/, text);
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ③ 锚点（形态 / 不越界 / 已跟踪）

test('#3 锚点：**形态非法**（越界 / 绝对路径 / 未知 repo / 未知 type / 缺必填）⇒ 红', () => {
  const cases = [
    [{ type: 'guard', repo: 'self', path: '../outside.mjs', test: 'x' }, /相对|越出/],
    [{ type: 'guard', repo: 'self', path: 'tools/../../x.mjs', test: 'x' }, /相对|越出/],
    [{ type: 'guard', repo: 'nope', path: 'tools/x.mjs', test: 'x' }, /repo/],
    [{ type: 'binary', repo: 'self', path: 'tools/x.mjs' }, /type/],
    [{ type: 'guard', repo: 'self', path: 'tools/x.mjs' }, /test/],
    [{ type: 'bin', repo: 'self', path: 'tools/x.mjs' }, /ea/],
  ];
  for (const [anchor, re] of cases) {
    const d = tmp();
    try {
      seedWithVocab(d);
      writeRaw(d, 'claim', { ...rec({}), anchor: [anchor] });
      const c3 = dataChecks(d).find((c) => c.id === 3);
      assert.ok(c3.problems.length > 0, `锚点 ${JSON.stringify(anchor)} 必须让 #3 红`);
      const text = c3.problems.map((p) => p.reason).join(' ');
      assert.match(text, re, `锚点 ${JSON.stringify(anchor)} 的报错不对：${text}`);
    } finally {
      rm(d);
    }
  }
});

test('★ #3 未知锚点 type 必须**响亮失败**，不许被静默改写成别的形状（写路径不写坏数据）', () => {
  const dir = tmp();
  try {
    assert.throws(() => appendRecord(dir, rec({ anchor: [{ type: 'binary', repo: 'self', path: 'tools/x.mjs' }] })), /type 非法/);
    assert.equal(readRecords(dir).records.length, 0, '抛错之后不许留残file');
    assert.throws(() => appendRecord(dir, rec({ anchor: [{ type: 'guard', repo: 'nope', path: 'tools/x.mjs', test: 'x' }] })), /repo 非法/);
  } finally {
    rm(dir);
  }
});

test('#3 锚点：`self` 必须落在**已跟踪**的文件上（未跟踪 ⇒ 红）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({})]);
    const report = validateAll(readRecords(dir).records, { ...ctxFor(dir), tracked: new Set(['some/other/file.mjs']) });
    assert.ok(
      report.checks.find((c) => c.id === 3).problems.some((p) => /已跟踪/.test(p.reason)),
      '未跟踪的 self 锚必须红',
    );
    const d2 = tmp();
    try {
      seedWithVocab(d2, [rec({ anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: REAL_GUARD.test }] })]);
      const ok = validateAll(readRecords(d2).records, { ...ctxFor(d2), tracked: new Set([REAL_GUARD.path]) });
      assert.deepEqual(ok.checks.find((c) => c.id === 3).problems, [], '已跟踪的锚必须绿');
    } finally {
      rm(d2);
    }
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ④ ★ 文件存在 ≠ 用例存在

test('★ #4 文件存在 ≠ 用例存在：锚指向文件里**没有的用例名** ⇒ 红（旧仓 guard-spec 的教训）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({ anchor: [{ type: 'guard', repo: 'self', ...REAL_GUARD }] })]);
    assert.deepEqual(failedData(dir), [], '真用例必须绿');

    const d2 = tmp();
    try {
      seedWithVocab(d2, [rec({ anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: GHOST }] })]);
      const text = problemsIn(d2, 3).map((p) => p.reason).join(' ');
      assert.match(text, /用例名在该文件里找不到/, text);
      assert.match(text, /该文件实测 \d+ 个用例/, '报错要给出"抽到了几个用例"这个可复核的事实');
    } finally {
      rm(d2);
    }
  } finally {
    rm(dir);
  }
});

test('★ 用例名抽取：`test()` / `it()` / `describe()` 的字面量；抽不出的文件 ⇒ 红（不是"跳过"）', () => {
  assert.deepEqual(testNames("test('a', () => {}); it(\"b\", () => {}); describe(`c`, () => {});"), ['a', 'b', 'c']);
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({ anchor: [{ type: 'guard', repo: 'self', path: 'package.json', test: '随便什么' }] })]);
    const text = problemsIn(dir, 3).map((p) => p.reason).join(' ');
    assert.match(text, /抽不出任何用例名/, text);
  } finally {
    rm(dir);
  }
});

test('#4 观察可再校验：accepted 的 self 锚解析不了 ⇒ 红，且**投影自动降级 stale**（不删除）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({ status: 'accepted', anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: GHOST }] })]);
    assert.ok(failedData(dir).includes(3), '锚指向文件里没有的用例 ⇒ #3 红');
    assert.ok(failedData(dir).includes(4), 'accepted 但 self 锚解析不了 ⇒ #4 红');
    const { records } = readRecords(dir);
    const data = records.filter((r) => r.kind !== 'domain');
    const proj = project(records, ctxFor(dir));
    const e = proj.entries.find((x) => x.id === data[0].id);
    assert.equal(e.effective, 'stale', '投影必须自动降级 stale');
    assert.equal(e.status, 'accepted', '★ 声称不改写（日志是事实）——只改"有效状态"');
    assert.equal(data.length, 1, '★ 不删除');
  } finally {
    rm(dir);
  }
});

test('★ #4 只读参考仓的锚：不在场时只 warn（不许判红）—— 否则 79 条 B 类会集体假红', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [
      rec({
        status: 'accepted',
        anchor: [
          { type: 'guard', repo: 'self', ...REAL_GUARD },
          { type: 'guard', repo: 'reference', path: 'app/amayui-emulator/test/adv-msgwin.test.ts', test: '★0x72' },
        ],
      }),
    ]);
    assert.deepEqual(failedData(dir), [], 'reference 锚 + 参考仓不在场 ⇒ 不能红');
    const proj = project(readRecords(dir).records, ctxFor(dir));
    const e = proj.entries.find((x) => x.kind === 'claim');
    assert.equal(e.effective, 'accepted', '有可解析的 self 锚 ⇒ 仍然是 accepted');
    assert.equal(e.anchors.find((a) => a.anchor.repo === 'reference').kind, 'warning', '参考仓不在场是 warning 不是 error');
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ⑤ 引线 / 冲突显式化

test('#5 撤回走"追加一条 replaces"：引线必须存在、不许成环；被撤回的不再算冲突', () => {
  const dir = tmp();
  try {
    const a = rec({ claim: '断言 A' });
    seedWithVocab(dir, [a, rec({ status: 'retracted', claim: '撤回：理由', replaces: a.id })]);
    assert.deepEqual(failedData(dir), [], '撤回用追加表达 ⇒ 绿');

    const d2 = tmp();
    try {
      seedWithVocab(d2, [rec({ replaces: 'ZZZZZZZZZZZZZZZZZZZZZZZZZZ' })]);
      assert.ok(failedData(d2).includes(5), 'replaces 悬空必须红');
    } finally {
      rm(d2);
    }
    const d3 = tmp();
    try {
      seedWithVocab(d3);
      const x = rec({ claim: 'x' });
      const y = { ...rec({ claim: 'y' }), replaces: x.id };
      const xx = { ...x, replaces: y.id };
      writeRaw(d3, 'claim', y);
      writeRaw(d3, 'claim', xx);
      const text = problemsIn(d3, 5).map((p) => p.reason).join(' ');
      assert.ok(failedData(d3).includes(5));
      assert.match(text, /环/, text);
    } finally {
      rm(d3);
    }
  } finally {
    rm(dir);
  }
});

test('★ #5 冲突必须显式化：同 kind+subject 两个不同 claim ⇒ 双方都标 conflict 并报出来', () => {
  const dir = tmp();
  try {
    const subject = 'Engine+0x5D880';
    seedWithVocab(dir, [rec({ subject, claim: 'cur_script' }), rec({ subject, claim: 'frames' })]);
    assert.ok(failedData(dir).includes(5), '两个不同 claim 必须红（冲突是产物，不许静默取一个）');
    const text = problemsIn(dir, 5).map((p) => p.reason).join(' ');
    assert.match(text, /显式冲突/, text);
    assert.match(text, /cur_script/, text);
    assert.match(text, /frames/, text);
    const proj = project(readRecords(dir).records, ctxFor(dir));
    assert.equal(proj.conflicts.length, 1);
  } finally {
    rm(dir);
  }
});

test('#5 同一 subject 同一 claim 的两条**不算**冲突（换过一次锚而已）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({ claim: '同一句话' }), rec({ claim: '同一句话' })]);
    assert.deepEqual(failedData(dir), [], 'claim 相同就不是冲突');
  } finally {
    rm(dir);
  }
});

test('★ #5 撤回（replaces）之后冲突必须消失 —— 撤回的语义就是"它不再算数"', () => {
  const dir = tmp();
  try {
    const subject = 'Engine+0x5D880';
    const a = rec({ subject, claim: '断言 A' });
    const b = rec({ subject, claim: '断言 B' });
    seedWithVocab(dir, [a, b]);
    assert.ok(failedData(dir).includes(5), '两条不同 claim ⇒ 先红着');
    seed(dir, [rec({ subject, claim: '撤回：A 作废', status: 'retracted', replaces: a.id })]);
    assert.deepEqual(failedData(dir), [], '撤回之后必须绿（★ 不是靠删行，而是靠"被 replaces 指向 ⇒ 不算数"）');
    assert.equal(project(readRecords(dir).records, ctxFor(dir)).conflicts.length, 0);
    assert.equal(readRecords(dir).records.filter((r) => r.kind !== 'domain').length, 3, '★ 历史一条都没删');
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ⑥ 派生 DB 可删可重建

test('★ #6 派生 DB：由文本真源重建**两次**，逻辑内容必须相同（可删可重建）', () => {
  const dir = tmp();
  const cache = tmp();
  try {
    seedWithVocab(dir, [
      rec({ subject: 'A+0x1', claim: 'a' }),
      rec({ subject: 'B+0x2', claim: 'b', system: ALT }),
      rec({ subject: 'C+0x3', claim: 'c', anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: GHOST }] }),
    ]);
    const { records } = readRecords(dir);
    const twice = rebuildTwiceDigest(records, cache, ctxFor(dir));
    assert.ok(twice.a.ok, twice.a.reason);
    assert.equal(twice.a.records, 5, '3 条业务 + 2 条词表');
    assert.ok(twice.a.domains >= 2, '域数要进 DB 摘要');
    assert.ok(twice.same, `两次重建的逻辑内容必须相同（${twice.a.digest} vs ${twice.b.digest}）`);
    fs.rmSync(twice.a.dbPath);
    assert.equal(rebuildTwiceDigest(records, cache, ctxFor(dir)).a.digest, twice.a.digest, '删掉 DB 后重建必须得到同一个逻辑内容');
  } finally {
    rm(dir);
    rm(cache);
  }
});

test('#6 派生 DB：把两次重建摘要改成不同 ⇒ #6 必须红（守卫红得有意义）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({})]);
    const report = failuresOf(dir, { dbProblems: [{ reason: '两次重建的逻辑内容不同（a ≠ b）' }], dbProblemsFile: 'x.sqlite' });
    assert.ok(report.checks.find((c) => c.id === 6).problems.length > 0, '#6 必须报红');
    assert.ok(report.failures > 0, 'failures 要跟着涨');
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ⑦ 分类轴（system + 域词汇表）

test('★ #7 分类轴：`system` 值追不到词表 ⇒ 红（= 待裁决，不许静默放过）', () => {
  const dir = tmp();
  try {
    seedWithVocab(dir, [rec({ system: '渲染' })]);
    const ids = failedData(dir);
    assert.ok(ids.includes(7), '未登记的域名必须红');
    const text = problemsIn(dir, 7).map((p) => p.reason).join(' ');
    assert.match(text, /不在域词汇表里/, text);
    assert.match(text, /待裁决/, text);
    assert.match(text, /kind=domain/, '报错要给出可执行的修法');
  } finally {
    rm(dir);
  }
});

test('★ #7 分类轴：改名/归并 —— 历史值靠 `aliases` **仍可解析**，历史行一个字节都不用动', () => {
  const dir = tmp();
  try {
    // 第一条记录用**改名前的旧值**写（模拟"历史行"）
    seed(dir, [dom(SYS, 'added')]);
    seed(dir, [rec({ system: LEGACY, claim: '旧值写下的历史行' })]);
    assert.ok(failedData(dir).includes(7), '旧值还没被登记成别名时 ⇒ 必须先红着');
    // 追加一条 rename 声明（append-only；**不是**回头改历史行）
    seed(dir, [dom(ALT, 'renamed', { aliases: [LEGACY] })]);
    assert.deepEqual(failedData(dir), [], '登记别名之后 ⇒ 历史行合法，且它的 system 值没被改动');
    const proj = project(readRecords(dir).records, ctxFor(dir));
    const e = proj.entries.find((x) => x.kind === 'claim');
    assert.equal(e.system.value, LEGACY, '★ 日志里的值原样保留（append-only）');
    assert.equal(e.system.via, 'alias', '解析路径是别名');
    assert.equal(e.system.canonical, ALT, '解析到当前域');
    assert.deepEqual(e.system.chain, [LEGACY, ALT], '要能给出别名链（可复核）');
  } finally {
    rm(dir);
  }
});

test('★ #7 分类轴：**拆分**不许当别名放过 —— 必须走追加更正记录的数据迁移', () => {
  const dir = tmp();
  try {
    // 先把 `Engine.audio` 登记成域（拆分前它当然是当前值），再写入"用当前值写下的行"
    seed(dir, [dom(SYS, 'added'), dom(ALT, 'added'), alt1(), alt2()]);
    const old = rec({ system: ALT, claim: '拆分前写下的行' });
    seed(dir, [old]);
    assert.deepEqual(failedData(dir), [], '词表齐全时，用当前值写的行必须绿');

    // 现在声明"Engine.audio 被拆成 mix / dsp"
    seed(dir, [dom(ALT, 'split', { splitInto: ['Engine.audio.mix', 'Engine.audio.dsp'] })]);
    assert.deepEqual(failedData(dir).filter((x) => x !== 7), [], '前面几条不变量都不该红');
    const text = problemsIn(dir, 7).map((p) => p.reason).join(' ');
    assert.ok(failedData(dir).includes(7), '被拆分的值必须红');
    assert.match(text, /已被拆分/, text);
    assert.match(text, /追加一条更正记录/, '报错要给出唯一正确的修法');
    assert.match(text, /不许靠词表悄悄改结论的含义/, '要把纪律说出来');

    // 走"追加更正记录"这条唯一正确的路：新记录 replaces 旧记录，并把值落到具体的新域
    seed(dir, [rec({ system: 'Engine.audio.mix', claim: '更正：旧行归到 mix', replaces: old.id })]);
    assert.deepEqual(failedData(dir), [], '更正之后必须绿');
    assert.equal(readRecords(dir).records.find((r) => r.id === old.id).system, ALT, '★ 旧行一个字节都没改（append-only）');
  } finally {
    rm(dir);
  }
});

test('★ #7 词表自身：同名两个来源 / 一个别名两个去处 ⇒ 红（词表不许有歧义）', () => {
  const dir = tmp();
  try {
    seed(dir, [dom(SYS, 'added'), dom(SYS, 'added')]);
    const v = buildVocabulary(readRecords(dir).records);
    assert.ok(v.problems.some((p) => /出现/.test(p.reason)), '同名两个来源必须报出来');
    assert.ok(failedData(dir).length >= 0);

    const d2 = tmp();
    try {
      seed(d2, [dom(SYS, 'added'), dom(ALT, 'renamed', { aliases: [LEGACY] }), dom('Engine.audio.mix', 'added'), dom('Engine.text', 'renamed', { aliases: [LEGACY] })]);
      const v2 = buildVocabulary(readRecords(d2).records);
      assert.ok(v2.problems.some((p) => /别名/.test(p.reason)), '一个别名两个去处必须报出来');
    } finally {
      rm(d2);
    }
  } finally {
    rm(dir);
  }
});

test('★ #7 词表：`splitInto` 的目标必须是当前词表里的域（不许指向不存在的域）', () => {
  const dir = tmp();
  try {
    seed(dir, [dom(SYS, 'added'), dom(ALT, 'split', { splitInto: ['Engine.a', 'Engine.b'] })]);
    const all = failuresOf(dir).checks.find((c) => c.id === 7).problems.map((p) => p.reason).join(' ');
    assert.match(all, /splitInto 指向/, all);
  } finally {
    rm(dir);
  }
});

test('★ #7 词表的处置字段：disposition 必填且是闭集合；renamed/merged 必填 aliases；只有 split 能写 splitInto', () => {
  const cases = [
    [dom(SYS, undefined), /disposition/],
    [dom(SYS, 'archived'), /disposition/],
    [dom(ALT, 'renamed'), /aliases/],
    [dom(ALT, 'merged'), /aliases/],
    [dom(ALT, 'added', { splitInto: ['a', 'b'] }), /splitInto/],
    [dom(ALT, 'split', { splitInto: ['only-one'] }), /至少 2 项/],
  ];
  for (const [r, re] of cases) {
    const d = tmp();
    try {
      seed(d, [dom(SYS, 'added')]);
      writeRaw(d, 'domain', r);
      const text = failuresOf(d).checks.find((c) => c.id === 1).problems.map((p) => p.reason).join(' ');
      assert.match(text, re, `域记录 ${JSON.stringify(r.disposition)} 的报错不对：${text}`);
    } finally {
      rm(d);
    }
  }
});

test('★ 分类轴：`buildVocabulary` / `resolveDomain` 是纯函数，可单独测（当前值 / 别名 / 拆分 / 未知）', () => {
  const vocab = buildVocabulary(VOCAB);
  assert.deepEqual([...vocab.canonical].sort(), [SYS, ALT].sort());
  assert.equal(resolveDomain(SYS, vocab).via, 'canonical');
  assert.equal(resolveDomain(LEGACY, vocab).via, 'alias');
  assert.equal(resolveDomain(LEGACY, vocab).canonical, ALT);
  assert.equal(resolveDomain('没登记过', vocab).via, 'unknown');
  assert.equal(resolveDomain('没登记过', vocab).canonical, null);

  const withSplit = buildVocabulary([...VOCAB, dom(ALT, 'split', { splitInto: [SYS, 'Engine.text'] })]);
  const r = resolveDomain(ALT, withSplit);
  assert.equal(r.via, 'split', '★ 被拆分的值不许解析成别名 —— 必须走数据迁移');
  assert.equal(r.canonical, null, '拆分的值没有唯一去向');
  assert.deepEqual(r.splitInto, [SYS, 'Engine.text']);
  assert.equal(resolveDomain(LEGACY, withSplit).via, 'split', '别名链上遇到拆分也要停下（传递性到此为止）');
});

test('★ 分类轴：空词表下一切照常（机制在场、数据留空）', () => {
  const dir = tmp();
  try {
    const report = failuresOf(dir);
    for (const c of report.checks) assert.deepEqual(c.problems, [], `空台账必须过 #${c.id}`);
    const proj = project([], ctxFor(dir));
    assert.equal(proj.vocab.canonical.size, 0);
    assert.deepEqual(proj.vocab.problems, []);
  } finally {
    rm(dir);
  }
});

/** 拆分用的两个新域（守卫的局部夹具） */
function alt1() {
  return dom('Engine.audio.mix', 'added', { system: SYS });
}
function alt2() {
  return dom('Engine.audio.dsp', 'added', { system: SYS });
}

// ─────────────────────────────────────────────────────────── ⑧ 写路径 / 自描述 / EA 映射

test('★ bin 锚：EA → PE 偏移；给了 sha256 就逐字节比（"观测没被换掉"的唯一证据）', () => {
  const dir = tmp();
  try {
    const pe = Buffer.alloc(0x600);
    pe.write('MZ', 0, 'latin1');
    pe.writeUInt32LE(0x80, 0x3c);
    pe.write('PE\0\0', 0x80, 'latin1');
    pe.writeUInt16LE(1, 0x80 + 6);
    pe.writeUInt16LE(0, 0x80 + 20);
    const s = 0x80 + 24;
    pe.writeUInt32LE(0x1000, s + 12);
    pe.writeUInt32LE(0x200, s + 16);
    pe.writeUInt32LE(0x400, s + 20);
    for (let i = 0; i < 8; i += 1) pe.writeUInt8(0xa0 + i, 0x400 + i);
    const rel = 'tmp-test-pe.bin';
    const abs = path.join(REPO_ROOT, rel);
    fs.writeFileSync(abs, pe);
    try {
      const digest = createHash('sha256').update(pe.subarray(0x400, 0x408)).digest('hex');
      seedWithVocab(dir, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 8, sha256: digest }] })]);
      assert.deepEqual(problemsIn(dir, 3), [], 'bin 锚必须绿');

      const d2 = tmp();
      try {
        seedWithVocab(d2, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 8, sha256: 'f'.repeat(64) }] })]);
        assert.match(problemsIn(d2, 3).map((p) => p.reason).join(' '), /字节摘要不匹配/);
      } finally {
        rm(d2);
      }
      const d3 = tmp();
      try {
        seedWithVocab(d3, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 0 }] })]);
        assert.match(problemsIn(d3, 3).map((p) => p.reason).join(' '), /len 必须是正整数/);
      } finally {
        rm(d3);
      }
      const d4 = tmp();
      try {
        seedWithVocab(d4, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x9000 }] })]);
        assert.match(problemsIn(d4, 3).map((p) => p.reason).join(' '), /不在任何 PE 节里/);
      } finally {
        rm(d4);
      }
    } finally {
      fs.rmSync(abs, { force: true });
    }
  } finally {
    rm(dir);
  }
});

test('★ 写路径只有一条：appendRecord 追加一行、写后回读复验、同输入同字节', () => {
  const dir = tmp();
  try {
    const r = rec({ subject: 'S+0x1', claim: 'x' });
    const res = appendRecord(dir, r);
    const onDisk = fs.readFileSync(res.file, 'utf8');
    assert.equal(onDisk, `${serializeRecord(r)}\n`, '一行一条、字段序固定');
    const r2 = rec({ subject: 'S+0x2', claim: 'y' });
    appendRecord(dir, r2);
    assert.ok(fs.readFileSync(res.file, 'utf8').startsWith(onDisk), '追加不得改动已有行');
  } finally {
    rm(dir);
  }
});

test('★ 台账是空的（有意为之）：K3 通过前任何知识条目不得进来；词表也留空', () => {
  const { records, problems } = readRecords(LEDGER_DIR);
  assert.deepEqual(problems, [], '真台账里不该有坏行');
  assert.equal(records.length, 0, 'data/ledger/ 里**没有条目**是 M3 的口径（K3 才是唯一写入者）');
  const report = validateAll(records, { repoRoot: REPO_ROOT, referenceRoot: null, ledgerDir: LEDGER_DIR, tracked: null, parsedProblems: [] });
  for (const c of report.checks) assert.deepEqual(c.problems, [], `真台账必须过 #${c.id}`);
  assert.equal(project(records, {}).vocab.canonical.size, 0, '★ 域词汇表也留空 —— 归一到哪些域是 K1 的活');
});

test('★ schema 只有一份真源：describe() 的枚举与校验器用的是同一批常量', () => {
  const d = describe();
  assert.equal(d.file, 'data/ledger/<kind>/<YYYY-MM>.jsonl');
  for (const k of KINDS) assert.ok(d.fields.find((f) => f.name === 'kind').type.includes(k), `kind 枚举少了 ${k}`);
  for (const s of STATUSES) assert.ok(d.fields.find((f) => f.name === 'status').type.includes(s), `status 枚举少了 ${s}`);
  for (const s of EFFECTIVE) assert.ok(d.effective.values.includes(s), `effective 取值少了 ${s}`);
  for (const r of REPOS) assert.ok(d.anchor.fields.find((f) => f.name === 'repo').type.includes(r), `repo 枚举少了 ${r}`);
  for (const x of DISPOSITIONS) assert.ok(d.classification.dispositions.includes(x), `disposition 枚举少了 ${x}`);
  assert.ok(d.fields.some((f) => f.name === 'system' && f.req === '✅'), 'system 必须是必填字段');
  assert.equal(d.invariants.length, CHECK_TITLES.size, '不变量条数必须与 CHECK_TITLES 一致');
  for (const inv of d.invariants) assert.ok(inv.enforcedBy?.includes('validate'), `不变量 ${inv.id} 没说谁在守它`);
  assert.ok(d.writePath.includes('写路径只有一条'), '必须给出唯一写入口');
  assert.ok(d.classification.query.length > 0, '分类轴要说清"怎么查"');
  assert.ok(describeText().includes('## 分类轴'), '文本形态要能用');
});

test('★ 锚点用 EA 不用行号：EA → 文件偏移由 PE 节表现算（同 EA 在任何一次反汇编里都成立）', () => {
  const buf = Buffer.alloc(0x600);
  buf.write('MZ', 0, 'latin1');
  buf.writeUInt32LE(0x80, 0x3c);
  buf.write('PE\0\0', 0x80, 'latin1');
  buf.writeUInt16LE(1, 0x80 + 6);
  buf.writeUInt16LE(0, 0x80 + 20);
  const s = 0x80 + 24;
  buf.writeUInt32LE(0x1000, s + 12);
  buf.writeUInt32LE(0x200, s + 16);
  buf.writeUInt32LE(0x400, s + 20);
  assert.equal(peOffsetOf(buf, 0x1000), 0x400);
  assert.equal(peOffsetOf(buf, 0x1010), 0x410, '★ 偏移 = raw + (EA - VA)，不是 EA 本身');
  assert.equal(peOffsetOf(buf, 0x2000), null, '不在任何节里 ⇒ null（不许瞎猜）');
  assert.equal(peOffsetOf(Buffer.from('not a pe'), 0x1000), null);
});

test('ULID：字典序 == 时间序；月份与 at 自洽（分片规则依赖它）', () => {
  const a = newUlid(Date.parse('2026-01-31T23:59:59.999Z'));
  const b = newUlid(Date.parse('2026-02-01T00:00:00.000Z'));
  assert.ok(a < b, '先发生的 ULID 必须更小');
  assert.equal(monthOf(a), '2026-01');
  assert.equal(monthOf(b), '2026-02');
  assert.equal(ulidTime(a), Date.parse('2026-01-31T23:59:59.999Z'));
  assert.ok(/^[0-9A-HJKMNP-TV-Z]{26}$/.test(a), `ULID 字符集（Crockford base32）：${a}`);
});

