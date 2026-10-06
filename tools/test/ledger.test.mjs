#!/usr/bin/env node
/**
 * tools/test/ledger.test.mjs —— **知识台账的守卫**（M3 的判据 G1–G5 全在这里）
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
  EFFECTIVE,
  KINDS,
  REPOS,
  STATUSES,
  appendRecord,
  describe,
  describeText,
  isoNow,
  monthOf,
  newUlid,
  peOffsetOf,
  project,
  readRecords,
  rebuildTwiceDigest,
  serializeRecord,
  testNames,
  ulidTime,
  validateAll,
} from '../lib/ledger.mjs';

const LEDGER_DIR = path.join(REPO_ROOT, DEFAULT_LEDGER_DIR);
/** 本仓里一条**真实存在**的守卫，用来当锚点（换了名字这里会红 —— 那正是要点） */
const REAL_GUARD = { path: 'tools/test/requirements.test.mjs', test: '#5 一屏预算' };

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
    subject: over.subject ?? 'Engine+0x5D880',
    claim: over.claim ?? '占位断言',
    anchor: over.anchor ?? [{ type: 'guard', repo: 'self', ...REAL_GUARD }],
    ...(over.status !== undefined ? { status: over.status } : {}),
    ...(over.replaces !== undefined ? { replaces: over.replaces } : {}),
    ...(over.note !== undefined ? { note: over.note } : {}),
  };
}
/** 把记录写进一个临时台账（走**真写路径**，不是手搓文件） */
function seed(dir, recs) {
  for (const r of recs) appendRecord(dir, r);
}

const failuresOf = (dir, extra = {}) => {
  const { records, problems } = readRecords(dir);
  return validateAll(records, { ...ctxFor(dir), parsedProblems: problems, ...extra });
};
const failedChecks = (dir, extra) => failuresOf(dir, extra).checks.filter((c) => c.problems.length).map((c) => c.id);

// ─────────────────────────────────────────────────────────── ① 形态（schema 合法）

test('#1 形态：schema 合法；坏记录必须红（不静默跳过）', () => {
  const dir = tmp();
  try {
    seed(dir, [rec({}), rec({ subject: 'Engine+0x5D88C', claim: '另一条断言' })]);
    assert.deepEqual(failedChecks(dir), [], '两条合法记录必须绿');

    const bads = [
      [{ subject: '' }, /subject/],
      [{ subject: 'Engine 0x5D880' }, /subject/],
      [{ claim: '   ' }, /claim/],
      [{ kind: 'conclusion' }, /kind/],
      [{ status: 'confirmed' }, /status/],
      [{ anchor: [] }, /anchor/],
      [{ at: '2026-10-04 09:00:00' }, /at/],
    ];
    for (const [over, re] of bads) {
      const d2 = tmp();
      try {
        // 手写坏行（坏记录**写不进**真写路径的 schema —— 那正是"写路径不写坏文件"）
        fs.mkdirSync(path.join(d2, 'claim'), { recursive: true });
        const r = rec({ ...over, id: over.id ?? newUlid() });
        fs.writeFileSync(path.join(d2, 'claim', `${monthOf(r.id)}.jsonl`), `${JSON.stringify(r)}\n`);
        const ids = failedChecks(d2);
        assert.ok(ids.includes(1), `坏记录 ${JSON.stringify(over)} 必须让 #1 红`);
        const text = failuresOf(d2).checks[0].problems.map((p) => p.reason).join(' ');
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
    seed(dir, [dup]);
    // 同 id 写两次（手写坏行：真写路径不允许重复 id）
    const f = path.join(dir, 'claim', `${monthOf(dup.id)}.jsonl`);
    fs.appendFileSync(f, `${serializeRecord(dup)}\n`);
    assert.ok(failedChecks(dir).includes(1), 'id 重复必须红');

    // 非法 JSON 行
    fs.appendFileSync(f, '{ 这不是 JSON\n');
    assert.ok(failedChecks(dir).includes(1), '坏 JSON 行必须红（不静默跳过）');

    // kind 与目录不符
    const d2 = tmp();
    try {
      fs.mkdirSync(path.join(d2, 'note'), { recursive: true });
      const r2 = rec({ kind: 'claim' });
      fs.writeFileSync(path.join(d2, 'note', `${monthOf(r2.id)}.jsonl`), `${serializeRecord(r2)}\n`);
      assert.ok(failedChecks(d2).includes(1), 'kind 与所在目录不一致必须红');
    } finally {
      rm(d2);
    }
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ② 追加序（append-only）

test('#2 追加序：文件内 ULID 必须严格递增（只许往后加）', () => {
  const dir = tmp();
  try {
    const older = rec({});
    const newer = rec({});
    assert.ok(older.id < newer.id, 'ULID 字典序 == 时间序');
    // 先写"新"、再写"旧" ⇒ 递增被破坏。★ 手写坏行才做得到（真写路径按当前时刻生成 ULID）
    const month = monthOf(newer.id);
    fs.mkdirSync(path.join(dir, 'claim'), { recursive: true });
    fs.writeFileSync(
      path.join(dir, 'claim', `${month}.jsonl`),
      `${serializeRecord(newer)}\n${serializeRecord(older)}\n`,
    );
    const ids = failedChecks(dir);
    assert.ok(ids.includes(2), '插入到中间 / 改历史必须红');
    const text = failuresOf(dir).checks[1].problems.map((p) => p.reason).join(' ');
    assert.match(text, /追加序破坏/, text);
  } finally {
    rm(dir);
  }
});

test('#2 追加序：at 的月份、ULID 的月份、文件名三者必须一致', () => {
  const dir = tmp();
  try {
    const id = newUlid();
    // at 写成另一个月（记录会落错分片）
    const r = rec({ id, at: '2020-01-15T00:00:00.000Z' });
    fs.mkdirSync(path.join(dir, 'claim'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'claim', `${monthOf(id)}.jsonl`), `${serializeRecord(r)}\n`);
    const text = failuresOf(dir).checks[1].problems.map((p) => p.reason).join(' ');
    assert.ok(failedChecks(dir).includes(2), 'at 与 ULID 月份不一致必须红');
    assert.match(text, /月份/, text);
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ③ 锚点（形态 / 不越界 / 已跟踪）

test('#3 锚点：**形态非法**（越界 / 绝对路径 / 未知 repo / 未知 type / 缺必填）⇒ 红', () => {
  const dir = tmp();
  try {
    // ★ 形态非法的锚**写不进真写路径**（serializeRecord 对未知 type/repo 直接抛）
    //   ⇒ 这里只能手写 JSON 行来造"坏数据"，正是守卫要抓的东西。
    const line = (anchor, id = newUlid()) =>
      `${JSON.stringify({ id, at: isoNow(ulidTime(id)), kind: 'claim', subject: 'S+0x1', claim: 'c', anchor: [anchor] })}\n`;
    const cases = [
      [{ type: 'guard', repo: 'self', path: '../outside.mjs', test: 'x' }, /相对|越出/],
      [{ type: 'guard', repo: 'self', path: 'tools/../../x.mjs', test: 'x' }, /相对|越出/],
      [{ type: 'guard', repo: 'nope', path: 'tools/x.mjs', test: 'x' }, /repo/],
      [{ type: 'binary', repo: 'self', path: 'tools/x.mjs' }, /type/],
      [{ type: 'guard', repo: 'self', path: 'tools/x.mjs' }, /test/],
      [{ type: 'bin', repo: 'self', path: 'tools/x.mjs' }, /ea/],
    ];
    for (const [anchor, re] of cases) {
      const d2 = tmp();
      try {
        const id = newUlid();
        fs.mkdirSync(path.join(d2, 'claim'), { recursive: true });
        fs.writeFileSync(path.join(d2, 'claim', `${monthOf(id)}.jsonl`), line(anchor, id));
        const report = failuresOf(d2);
        const c3 = report.checks.find((c) => c.id === 3);
        assert.ok(c3.problems.length > 0, `锚点 ${JSON.stringify(anchor)} 必须让 #3 红`);
        const text = c3.problems.map((p) => p.reason).join(' ');
        assert.match(text, re, `锚点 ${JSON.stringify(anchor)} 的报错不对：${text}`);
      } finally {
        rm(d2);
      }
    }
  } finally {
    rm(dir);
  }
});

test('#3 锚点：未知 type 必须**响亮失败**，不许被静默改写成别的形状（写路径不写坏数据）', () => {
  const dir = tmp();
  try {
    const bad = { type: 'binary', repo: 'self', path: 'tools/x.mjs' };
    const r = rec({ anchor: [bad] });
    assert.throws(() => appendRecord(dir, r), /type 非法/, 'serializeRecord 必须对未知 type 抛错');
    assert.equal(readRecords(dir).records.length, 0, '抛错之后不许留残file');
    const badRepo = rec({ anchor: [{ type: 'guard', repo: 'nope', path: 'tools/x.mjs', test: 'x' }] });
    assert.throws(() => appendRecord(dir, badRepo), /repo 非法/, '未知 repo 同样抛错');
  } finally {
    rm(dir);
  }
});

test('#3 锚点：`self` 必须落在**已跟踪**的文件上（未跟踪 ⇒ 红）', () => {
  const dir = tmp();
  try {
    const r = rec({});
    seed(dir, [r]);
    // 造一个"已跟踪集合"：只承认别的文件 ⇒ 本条锚点算未跟踪
    const report = validateAll(readRecords(dir).records, {
      ...ctxFor(dir),
      tracked: new Set(['some/other/file.mjs']),
    });
    assert.ok(
      report.checks.find((c) => c.id === 3).problems.some((p) => /已跟踪/.test(p.reason)),
      '未跟踪的 self 锚必须红',
    );
    // 换成"已跟踪"的那份 ⇒ 绿
    const d2 = tmp();
    try {
      seed(d2, [rec({ anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: REAL_GUARD.test }] })]);
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
    seed(dir, [rec({ anchor: [{ type: 'guard', repo: 'self', ...REAL_GUARD }] })]);
    assert.deepEqual(failedChecks(dir), [], '真用例必须绿');

    // ★ 这条用例名**整条**在文件里不存在（别用短串 —— 短串会在断言消息/注释里命中，那是假绿）
    const ghostName = '★ 用例名故意不存在 GA4sT';
    const d2 = tmp();
    try {
      seed(d2, [rec({ anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: ghostName }] })]);
      const report = failuresOf(d2);
      assert.ok(report.checks.find((c) => c.id === 3).problems.length > 0, '指向不存在用例的锚必须在 #3 红');
      const text = report.checks.find((c) => c.id === 3).problems.map((p) => p.reason).join(' ');
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
  const names = testNames("test('a', () => {}); it(\"b\", () => {}); describe(`c`, () => {});");
  assert.deepEqual(names, ['a', 'b', 'c']);
  const dir = tmp();
  try {
    // 指向一个**不是测试文件**的文件（`useCases` 抽不出用例名）⇒ 必须红
    seed(dir, [rec({ anchor: [{ type: 'guard', repo: 'self', path: 'package.json', test: '随便什么' }] })]);
    const text = failuresOf(dir).checks.find((c) => c.id === 3).problems.map((p) => p.reason).join(' ');
    assert.match(text, /抽不出任何用例名/, text);
  } finally {
    rm(dir);
  }
});

test('#4 观察可再校验：accepted 的 self 锚解析不了 ⇒ 红，且**投影自动降级 stale**（不删除）', () => {
  const dir = tmp();
  try {
    const r = rec({
      status: 'accepted',
      anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: '★ 用例名故意不存在 GA4sT' }],
    });
    seed(dir, [r]);
    assert.ok(failedChecks(dir).includes(3), '锚指向文件里没有的用例 ⇒ #3 红');
    assert.ok(failedChecks(dir).includes(4), 'accepted 但 self 锚解析不了 ⇒ #4 红');
    const { records } = readRecords(dir);
    const proj = project(records, ctxFor(dir));
    assert.equal(proj.entries[0].effective, 'stale', '投影必须自动降级 stale');
    assert.equal(proj.entries[0].status, 'accepted', '★ 声称不改写（日志是事实）——只改"有效状态"');
    assert.equal(records.length, 1, '★ 不删除');
  } finally {
    rm(dir);
  }
});

test('★ #4 只读参考仓的锚：不在场时只 warn（不许判红）—— 否则 79 条 B 类会集体假红', () => {
  const dir = tmp();
  try {
    const ref = rec({
      status: 'accepted',
      anchor: [
        { type: 'guard', repo: 'self', ...REAL_GUARD },
        { type: 'guard', repo: 'reference', path: 'app/amayui-emulator/test/adv-msgwin.test.ts', test: '★0x72' },
      ],
    });
    seed(dir, [ref]);
    assert.deepEqual(failedChecks(dir), [], 'reference 锚 + 参考仓不在场 ⇒ 不能红');
    const { records } = readRecords(dir);
    const proj = project(records, ctxFor(dir));
    assert.equal(proj.entries[0].effective, 'accepted', '有可解析的 self 锚 ⇒ 仍然是 accepted');
    const warn = proj.entries[0].anchors.find((a) => a.anchor.repo === 'reference');
    assert.equal(warn.kind, 'warning', '参考仓不在场是 warning 不是 error');
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ⑤ 引线 / 冲突显式化

test('#5 撤回走"追加一条 replaces"：引线必须存在、不许成环；被撤回的不再算冲突', () => {
  const dir = tmp();
  try {
    const a = rec({ claim: '断言 A' });
    seed(dir, [a]);
    const b = rec({ status: 'retracted', claim: '撤回：理由', replaces: a.id });
    seed(dir, [b]);
    assert.deepEqual(failedChecks(dir), [], '撤回用追加表达 ⇒ 绿');

    const d2 = tmp();
    try {
      seed(d2, [rec({ replaces: 'ZZZZZZZZZZZZZZZZZZZZZZZZZZ' })]);
      assert.ok(failedChecks(d2).includes(5), 'replaces 悬空必须红');
    } finally {
      rm(d2);
    }
    const d3 = tmp();
    try {
      const x = rec({});
      const y = { ...rec({ claim: 'y' }), replaces: x.id };
      // 手写形成环：x.replaces = y.id, y.replaces = x.id
      const xx = { ...x, replaces: y.id };
      fs.mkdirSync(path.join(d3, 'claim'), { recursive: true });
      fs.writeFileSync(path.join(d3, 'claim', `${monthOf(x.id)}.jsonl`), `${serializeRecord(yy(y, xx))}\n${serializeRecord(xx)}\n`);
      const text = failuresOf(d3).checks[4].problems.map((p) => p.reason).join(' ');
      assert.ok(failedChecks(d3).includes(5));
      assert.match(text, /环/, text);
    } finally {
      rm(d3);
    }
  } finally {
    rm(dir);
  }
});
const yy = (y, xx) => ({ ...y, replaces: xx.id });

test('★ #5 冲突必须显式化：同 kind+subject 两个不同 claim ⇒ 双方都标 conflict 并报出来', () => {
  const dir = tmp();
  try {
    const subject = 'Engine+0x5D880';
    seed(dir, [
      rec({ subject, claim: 'cur_script' }),
      rec({ subject, claim: 'frames' }),
    ]);
    const ids = failedChecks(dir);
    assert.ok(ids.includes(5), '两个不同 claim 必须红（冲突是产物，不许静默取一个）');
    const text = failuresOf(dir).checks[4].problems.map((p) => p.reason).join(' ');
    assert.match(text, /显式冲突/, text);
    assert.match(text, /cur_script/, text);
    assert.match(text, /frames/, text);
    const proj = project(readRecords(dir).records, ctxFor(dir));
    assert.equal(proj.byEffective.conflict, 2, '冲突双方都计 conflict');
    assert.equal(proj.conflicts.length, 1);
  } finally {
    rm(dir);
  }
});

test('#5 同一 subject 同一 claim 的两条**不算**冲突（换过一次锚而已）', () => {
  const dir = tmp();
  try {
    seed(dir, [rec({ claim: '同一句话' }), rec({ claim: '同一句话' })]);
    assert.deepEqual(failedChecks(dir), [], 'claim 相同就不是冲突');
  } finally {
    rm(dir);
  }
});

test('★ #5 撤回（replaces）之后冲突必须消失 —— 撤回的语义就是"它不再算数"', () => {
  const dir = tmp();
  try {
    const subject = 'Engine+0x5D880';
    const a = rec({ subject, claim: '断言 A' });
    seed(dir, [a]);
    const b = rec({ subject, claim: '断言 B' });
    seed(dir, [b]);
    assert.ok(failedChecks(dir).includes(5), '两条不同 claim ⇒ 先红着');
    // 撤回 A：**追加**一条 retracted 且 replaces 指向 a
    seed(dir, [rec({ subject, claim: '撤回：A 作废', status: 'retracted', replaces: a.id })]);
    assert.deepEqual(failedChecks(dir), [], '撤回之后必须绿（★ 不是靠删行，而是靠"被 replaces 指向 ⇒ 不算数"）');
    const proj = project(readRecords(dir).records, ctxFor(dir));
    assert.equal(proj.conflicts.length, 0);
    assert.equal(readRecords(dir).records.length, 3, '★ 历史一条都没删');
  } finally {
    rm(dir);
  }
});

test('★ bin 锚：EA → PE 偏移；给了 sha256 就逐字节比（"观测没被换掉"的唯一证据）', () => {
  const dir = tmp();
  try {
    // 合成最小 PE：MZ + PE 头 + 1 节（VA 0x1000 → raw 0x400）
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
      const good = rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 8, sha256: digest }] });
      seed(dir, [good]);
      const report = failuresOf(dir);
      assert.deepEqual(report.checks.find((c) => c.id === 3).problems, [], `bin 锚必须绿：${JSON.stringify(report.checks[2].problems)}`);

      // 摘要写错 ⇒ 红（观测被换掉）
      const d2 = tmp();
      try {
        seed(d2, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 8, sha256: 'f'.repeat(64) }] })]);
        const text = failuresOf(d2).checks.find((c) => c.id === 3).problems.map((p) => p.reason).join(' ');
        assert.match(text, /字节摘要不匹配/, text);
      } finally {
        rm(d2);
      }
      // len 非法 ⇒ 红
      const d3 = tmp();
      try {
        seed(d3, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x1000, len: 0 }] })]);
        const text = failuresOf(d3).checks.find((c) => c.id === 3).problems.map((p) => p.reason).join(' ');
        assert.match(text, /len 必须是正整数/, text);
      } finally {
        rm(d3);
      }
      // EA 不在任何节里 ⇒ 红（不许瞎猜偏移）
      const d4 = tmp();
      try {
        seed(d4, [rec({ anchor: [{ type: 'bin', repo: 'self', path: rel, ea: 0x9000 }] })]);
        const text = failuresOf(d4).checks.find((c) => c.id === 3).problems.map((p) => p.reason).join(' ');
        assert.match(text, /不在任何 PE 节里/, text);
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

// ─────────────────────────────────────────────────────────── ⑥ 派生 DB 可删可重建

test('★ #6 派生 DB：由文本真源重建**两次**，逻辑内容必须相同（可删可重建）', () => {
  const dir = tmp();
  const cache = tmp();
  try {
    seed(dir, [
      rec({ subject: 'A+0x1', claim: 'a' }),
      rec({ subject: 'B+0x2', claim: 'b', status: 'proposed' }),
      rec({ subject: 'C+0x3', claim: 'c', anchor: [{ type: 'guard', repo: 'self', path: REAL_GUARD.path, test: '不存在' }] }),
    ]);
    const { records } = readRecords(dir);
    const twice = rebuildTwiceDigest(records, cache, ctxFor(dir));
    assert.ok(twice.a.ok, twice.a.reason);
    assert.equal(twice.a.records, 3);
    assert.equal(twice.a.anchors, 3);
    assert.ok(twice.same, `两次重建的逻辑内容必须相同（${twice.a.digest} vs ${twice.b.digest}）`);

    // 删掉 DB 再建 ⇒ 还是同一个逻辑内容（"可删可重建"的字面判据）
    fs.rmSync(twice.a.dbPath);
    const again = rebuildTwiceDigest(records, cache, ctxFor(dir));
    assert.equal(again.a.digest, twice.a.digest, '删掉 DB 后重建必须得到同一个逻辑内容');

    // validate 的 #6 也真的会跑这件事（不是"文档说有守卫"）
    const report = failuresOf(dir, { dbProblems: [], dbProblemsFile: '' });
    assert.equal(report.checks.find((c) => c.id === 6).problems.length, 0);
  } finally {
    rm(dir);
    rm(cache);
  }
});

test('#6 派生 DB：把两次重建摘要改成不同 ⇒ #6 必须红（守卫红得有意义）', () => {
  const dir = tmp();
  try {
    seed(dir, [rec({})]);
    const report = failuresOf(dir, { dbProblems: [{ reason: '两次重建的逻辑内容不同（a ≠ b）' }], dbProblemsFile: 'x.sqlite' });
    assert.ok(report.checks.find((c) => c.id === 6).problems.length > 0, '#6 必须报红');
    assert.ok(report.failures > 0, 'failures 要跟着涨');
  } finally {
    rm(dir);
  }
});

// ─────────────────────────────────────────────────────────── ⑦ 写路径 / 自描述 / EA 映射

test('★ 写路径只有一条：appendRecord 追加一行、写后回读复验、同输入同字节', () => {
  const dir = tmp();
  try {
    const r = rec({ subject: 'S+0x1', claim: 'x' });
    const res = appendRecord(dir, r);
    assert.ok(fs.existsSync(res.file));
    const onDisk = fs.readFileSync(res.file, 'utf8');
    assert.equal(onDisk, `${serializeRecord(r)}\n`, '一行一条、字段序固定');
    // 再追加一条：**只许往后加**（前一行逐字节不动）
    const r2 = rec({ subject: 'S+0x2', claim: 'y' });
    appendRecord(dir, r2);
    const after = fs.readFileSync(res.file, 'utf8');
    assert.ok(after.startsWith(onDisk), '追加不得改动已有行');
    const { records } = readRecords(dir);
    assert.equal(records.length, 2);
  } finally {
    rm(dir);
  }
});

test('★ 台账是空的（有意为之）：K3 通过前任何知识条目不得进来', () => {
  const { records, problems } = readRecords(LEDGER_DIR);
  assert.deepEqual(problems, [], '真台账里不该有坏行');
  assert.equal(records.length, 0, 'data/ledger/ 里**没有条目**是 M3 的口径（K3 才是唯一写入者）');
  const report = validateAll(records, { repoRoot: REPO_ROOT, referenceRoot: null, ledgerDir: LEDGER_DIR, tracked: null, parsedProblems: [] });
  for (const c of report.checks) assert.deepEqual(c.problems, [], `真台账必须过 #${c.id}`);
});

test('★ schema 只有一份真源：describe() 的枚举与校验器用的是同一批常量', () => {
  const d = describe();
  assert.equal(d.file, 'data/ledger/<kind>/<YYYY-MM>.jsonl');
  for (const k of KINDS) assert.ok(d.fields.find((f) => f.name === 'kind').type.includes(k), `kind 枚举少了 ${k}`);
  for (const s of STATUSES) assert.ok(d.fields.find((f) => f.name === 'status').type.includes(s), `status 枚举少了 ${s}`);
  for (const s of EFFECTIVE) assert.ok(d.effective.values.includes(s), `effective 取值少了 ${s}`);
  for (const r of REPOS) assert.ok(d.anchor.fields.find((f) => f.name === 'repo').type.includes(r), `repo 枚举少了 ${r}`);
  assert.equal(d.invariants.length, CHECK_TITLES.size, '不变量条数必须与 CHECK_TITLES 一致');
  for (const inv of d.invariants) assert.ok(inv.enforcedBy?.includes('validate'), `不变量 ${inv.id} 没说谁在守它`);
  assert.ok(d.writePath.includes('写路径只有一条'), '必须给出唯一写入口（"写路径只有一条"）');
  assert.ok(describeText().includes('## 形态'), '文本形态要能用');
});

test('★ 锚点用 EA 不用行号：EA → 文件偏移由 PE 节表现算（同 EA 在任何一次反汇编里都成立）', () => {
  // 合成一个最小 PE：MZ + PE 头 + 1 个节（VA 0x1000 → raw 0x400）
  const buf = Buffer.alloc(0x600);
  buf.write('MZ', 0, 'latin1');
  buf.writeUInt32LE(0x80, 0x3c);
  buf.write('PE\0\0', 0x80, 'latin1');
  buf.writeUInt16LE(1, 0x80 + 6); // NumberOfSections
  buf.writeUInt16LE(0, 0x80 + 20); // SizeOfOptionalHeader = 0（省得算）
  const s = 0x80 + 24;
  buf.writeUInt32LE(0x1000, s + 12); // VirtualAddress
  buf.writeUInt32LE(0x200, s + 16); // SizeOfRawData
  buf.writeUInt32LE(0x400, s + 20); // PointerToRawData
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
