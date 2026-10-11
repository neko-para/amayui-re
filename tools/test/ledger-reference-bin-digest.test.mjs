#!/usr/bin/env node
/** @env pure @kind gate @why `repo=reference` 的 bin 锚可以不带 len/sha256 进台账 ⇒ "这条观察没被换掉"只剩一句声称 */
/**
 * tools/test/ledger-reference-bin-digest.test.mjs —— **摘要**（"这条观察没被换掉"的唯一证据）的守卫
 *
 * ## 口径（用户已批准）
 * `repo=reference` 的 `bin` 锚**必须带 `len` + `sha256`**：`sha256` = **该 EA 处 `len` 字节的摘要**。
 * 为什么它是唯一证据：只记 EA 的锚在二进制被**换掉**之后**看起来仍然正常**（EA → 文件偏移那条映射照旧成立），
 * 只有摘要会当场对不上。
 *
 * ## ★ 只约束**新**记录 —— 而"新/旧分界"用的是本仓**既有**那套（不是新发明的第二套）
 * 既有机制 = `tools/test/ledger.test.mjs` 的"片段锚上限（新锚只许完整名）"：
 * **把存量欠账钉成一个上限常量（棘轮），只许下降** —— 新增的违规把现算值顶过上限 ⇒ 红；
 * 存量恰好停在上限上 ⇒ 不红。所以本要求落在**两处**：
 *   ① **写路径**（`ledger add`）：逐条硬拒 —— 与存量无关（存量从不经过写路径）；
 *   ② **门禁**（不变量 #8）：`缺摘要的在场 reference bin 锚数 ≤ REFERENCE_BIN_DIGEST_DEBT_MAX`。
 *
 * ## 怎么才会红
 * 1. 把 `tools/lib/ledger.mjs` 的 `referenceBinDigestGaps` 判据从 `len 或 sha256 缺一即算缺`
 *    改成 `只缺 len 才算缺`（棘轮清单，`node tools/mutate-check.mjs --list` 里有这条）⇒ 下面第 1、2 条当场红；
 * 2. 把 `cmdAdd` 里那段"逐条硬拒"去掉 ⇒ 第 2 条当场红；
 * 3. 让存量欠账涨到超过上限（= 真的多了新欠账）⇒ 第 1 条红（那正是要红的）。
 *
 * ★ 本文件**一个字节都不写进真 `data/ledger/`**：要写就写系统临时区的临时根（且只走 `--dir`）。
 * ★ **不起管道**（受限沙箱里 `stdio:'pipe'` 会 EPERM）：子进程的 stdout / stderr 重定向到临时文件后读回。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import {
  DEFAULT_LEDGER_DIR,
  REFERENCE_BIN_DIGEST_DEBT_MAX,
  describeText,
  hashAnchorAt,
  isoNow,
  newUlid,
  project,
  readRecords,
  referenceBinDigestGaps,
  resolveAnchor,
  validateAll,
} from '../lib/ledger.mjs';

const LEDGER_DIR = path.join(REPO_ROOT, DEFAULT_LEDGER_DIR);
const CLI = path.join(REPO_ROOT, 'tools', 'cli.mjs');

/**
 * 跑一条**真实命令**（= `pnpm tools ledger …` 走的同一条路：派发器 → CLI → 模型）。
 * ★ 两个流分别重定向到临时文件后读回（沙箱里不许起管道）。
 * @returns {{code:number, out:string, err:string}}
 */
function runCli(args) {
  const base = path.join(os.tmpdir(), `amayui-ledger-digest-${process.pid}-${Math.random().toString(36).slice(2)}`);
  const of = fs.openSync(`${base}.out`, 'w');
  const ef = fs.openSync(`${base}.err`, 'w');
  let r;
  try {
    r = spawnSync(process.execPath, [CLI, ...args], { cwd: REPO_ROOT, stdio: ['ignore', of, ef] });
  } finally {
    fs.closeSync(of);
    fs.closeSync(ef);
  }
  const out = fs.readFileSync(`${base}.out`, 'utf8');
  const err = fs.readFileSync(`${base}.err`, 'utf8');
  fs.rmSync(`${base}.out`, { force: true });
  fs.rmSync(`${base}.err`, { force: true });
  return { code: r.status, out, err };
}

/** 真 `data/ledger/` 的快照（文件 → 内容 sha256）：用来证明本文件没往里写 */
function snapshotDir(dir) {
  const out = [];
  for (const kind of fs.readdirSync(dir).sort()) {
    const d = path.join(dir, kind);
    if (!fs.statSync(d).isDirectory()) continue;
    for (const f of fs.readdirSync(d).sort()) {
      out.push(
        `${kind}/${f} ${createHash('sha256').update(fs.readFileSync(path.join(d, f))).digest('hex')}`,
      );
    }
  }
  return out;
}

/** 一条**形态合法**的临时记录（只喂给 `validateAll`，不落盘） */
const fixture = (anchors) => ({
  id: newUlid(),
  at: isoNow(),
  kind: 'observation',
  subject: 'guard/digest-fixture',
  claim: '守卫夹具（不落盘）',
  status: 'proposed',
  anchor: anchors,
  _file: path.join(LEDGER_DIR, 'observation', 'fixture.jsonl'),
  _line: 1,
  _fileKind: 'observation',
});

/** `#8` 那一张检查（其它不变量与本节无关：夹具不是真记录） */
function check8(records, debtMax) {
  const report = validateAll(records, {
    repoRoot: REPO_ROOT,
    referenceRoot: null, // ★ null ⇒ 参考仓锚只算 warning（不读任何二进制）⇒ 这条守卫便宜
    ledgerDir: LEDGER_DIR,
    tracked: null,
    parsedProblems: [],
    referenceBinDigestDebtMax: debtMax,
  });
  const c = report.checks.find((x) => x.id === 8);
  assert.ok(c, '不变量 #8 必须存在（摘要是门禁，不是建议）');
  return c;
}

/**
 * 合成 PE32（ImageBase 0x400000、一个 `.text`：RVA 0x1000 / raw 0x400）——
 * 用来把"EA → 偏移 → 字节 → sha256"这条链**独立复算**一遍（偏移由本文件手写，不看实现）。
 */
function syntheticPe() {
  const buf = Buffer.alloc(0x600);
  buf.write('MZ', 0, 'latin1');
  buf.writeUInt32LE(0x80, 0x3c);
  buf.write('PE\0\0', 0x80, 'latin1');
  buf.writeUInt16LE(1, 0x80 + 6); // NumberOfSections
  buf.writeUInt16LE(0xe0, 0x80 + 20); // SizeOfOptionalHeader（PE32）
  buf.writeUInt16LE(0x10b, 0x80 + 24); // Magic
  buf.writeUInt32LE(0x400000, 0x80 + 24 + 28); // ImageBase
  const s = 0x80 + 24 + 0xe0;
  buf.write('.text', s, 'latin1');
  buf.writeUInt32LE(0x1000, s + 12); // VirtualAddress（RVA）
  buf.writeUInt32LE(0x200, s + 16); // SizeOfRawData
  buf.writeUInt32LE(0x400, s + 20); // PointerToRawData
  const ea = 0x401020; // RVA 0x1020 ⇒ 偏移 0x400 + 0x20 = 0x420
  const offset = 0x420;
  const bytes = Buffer.from('0123456789abcdef', 'latin1');
  bytes.copy(buf, offset);
  return { buf, ea, offset, bytes };
}

// ─────────────────────────────────────────────────────────── ① 规则本身（双向）

test('★ 摘要棘轮：新记录缺 len/sha256 ⇒ 红；存量同样缺 ⇒ 不红（双向）', () => {
  const { records } = readRecords(LEDGER_DIR);
  const real = project(records, { repoRoot: REPO_ROOT, referenceRoot: null, ledgerDir: LEDGER_DIR });
  const realDebt = referenceBinDigestGaps(real.entries);
  // ★ 前提断言：存量里**确实**有这样的锚 —— 否则下面这条双向断言会退化成恒真
  assert.ok(realDebt.length > 0, '本机实测存量里必须有"缺摘要的 reference bin 锚"（没有 ⇒ 这条守卫没在守任何东西）');
  assert.ok(
    realDebt.length <= REFERENCE_BIN_DIGEST_DEBT_MAX,
    `存量欠账 ${realDebt.length} 已超棘轮上限 ${REFERENCE_BIN_DIGEST_DEBT_MAX} ⇒ 门禁会红。` +
      '若这确实是**新**欠账，那就是要红；若只是补过摘要、上限有余量，就把上限调低到实测值（只许下降）。',
  );

  // 方向 ①（**存量**）：上限 = 现算值 ⇒ 真台账一条 #8 都不许有（"只约束新记录"就是这一句）
  assert.deepEqual(
    check8(records, realDebt.length).problems,
    [],
    '存量欠账（上限 = 现算值）不许红 —— 这条新要求只约束**新**记录',
  );

  // 方向 ②（**新记录**缺摘要）：多了它就顶过上限 ⇒ 必须红，且点名那条新记录
  const missing = fixture([{ type: 'bin', repo: 'reference', path: 'raw-parts/AGE.EXE__dumped.sectfix.EXE', ea: 0x497620, len: 16 }]);
  const red = check8([...records, missing], realDebt.length);
  assert.ok(red.problems.length > 0, '★ 新记录缺 sha256（只给了 len）必须红 —— 否则"唯一证据"可以被静默跳过');
  assert.ok(
    red.problems.some((p) => p.reason.includes(missing.id)),
    `红的时候要点名**是哪一条**新记录（ULID 字典序 == 时间序 ⇒ 追加进来的就在最前面），实际：${JSON.stringify(red.problems.map((p) => p.reason))}`,
  );
  assert.ok(
    red.problems.some((p) => p.reason.includes('--hash-anchor')),
    '错误信息必须给出**可执行**的补摘要命令（照抄即可），而不是"请填摘要"',
  );

  // 方向 ③（**新记录带齐**）：同样多一条记录，但摘要齐 ⇒ 一条 #8 都不许有
  const complete = fixture([
    { type: 'bin', repo: 'reference', path: 'raw-parts/AGE.EXE__dumped.sectfix.EXE', ea: 0x497620, len: 70, sha256: 'a'.repeat(64) },
  ]);
  assert.deepEqual(
    check8([...records, complete], realDebt.length).problems,
    [],
    '带齐 `len` + `sha256` 的新记录不许红（★ 摘要**对错**是 resolveAnchor 的事，不是这里）',
  );
});

// ─────────────────────────────────────────────────────────── ② 写路径：逐条硬拒

test('★ 写路径硬拒：不带摘要的 reference bin 锚写不进去（错误信息给出 --hash-anchor 命令）', () => {
  const before = snapshotDir(LEDGER_DIR);
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-ledger-digest-refuse-'));
  try {
    // 只给 len、不给 sha256 —— 仍然算"缺"（要求是 len + sha256 **两个都在**）
    const bad = runCli([
      'ledger', 'add', '--kind', 'observation', '--subject', 'guard/digest-refused', '--claim', '守卫：缺摘要必须被拒',
      '--anchor', JSON.stringify({ type: 'bin', repo: 'reference', path: 'raw-parts/AGE.EXE__dumped.sectfix.EXE', ea: 0x497620, len: 16 }),
      '--dir', tmp, '--write',
    ]);
    assert.notEqual(bad.code, 0, `缺摘要必须被拒（退出码 0 就意味着"唯一证据"没人管），stdout：${bad.out.trim()}`);
    assert.match(bad.err, /--hash-anchor/, '错误信息必须给出可执行的补摘要命令');
    assert.match(bad.err, /sha256/, '错误信息要点名缺的是什么');
    assert.match(bad.err, /len/, '错误信息要点名缺的是什么');
    const shard = path.join(tmp, 'observation');
    assert.ok(!fs.existsSync(shard) || fs.readdirSync(shard).length === 0, '被拒之后不许留下任何分片');

    // 正对照：带齐 `len` + `sha256` ⇒ 写得进去（落点是**临时根**，不是真台账）
    const good = runCli([
      'ledger', 'add', '--kind', 'observation', '--subject', 'guard/digest-accepted', '--claim', '守卫：带摘要应当被收',
      '--anchor', JSON.stringify({ type: 'bin', repo: 'reference', path: 'raw-parts/__guard-fixture__.BIN', ea: 0x1000, len: 4, sha256: 'b'.repeat(64) }),
      '--dir', tmp, '--write',
    ]);
    assert.equal(good.code, 0, `带齐摘要的锚必须写得进去（正对照）：${good.err.trim()}`);
    assert.equal(fs.readdirSync(shard).length, 1, '写进去了就该有分片');
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
  assert.deepEqual(snapshotDir(LEDGER_DIR), before, '本文件不许写进真 data/ledger/');
});

// ─────────────────────────────────────────────────────────── ③ 只读帮助动作

test('★ --hash-anchor：EA → 偏移 / 字节 / sha256（独立复算一致，且打印可直接照抄的锚 JSON）', () => {
  const { buf, ea, offset, bytes } = syntheticPe();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-ledger-hash-'));
  const file = path.join(tmp, 'fixture.exe');
  fs.writeFileSync(file, buf);
  try {
    // 纯核心：偏移与摘要是**本文件手写 + crypto 复算**的，不看实现
    const pure = hashAnchorAt(buf, ea, bytes.length);
    assert.equal(pure.ok, true);
    assert.equal(pure.offset, offset, '★ 偏移 = raw + (EA - ImageBase)，不是 EA 本身');
    const want = createHash('sha256').update(buf.subarray(offset, offset + bytes.length)).digest('hex');
    assert.equal(pure.sha256, want, '摘要 = 该 EA 处 `len` 字节的 sha256');
    assert.equal(pure.hex, bytes.toString('hex'), '十六进制字节要能核对');

    // 越界 / 非法 len 要**明说**，不许给个空摘要
    assert.equal(hashAnchorAt(buf, ea, 0).ok, false, 'len 必须正整数');
    assert.equal(hashAnchorAt(buf, 0x999999, 4).ok, false, 'EA 不在任何节里 ⇒ 不猜');

    // CLI 端到端：写手在错误信息里读到的那条命令必须真的能跑、且打印可照抄的锚 JSON
    const r = runCli(['ledger', 'hash-anchor', file, `0x${ea.toString(16)}`, String(bytes.length)]);
    assert.equal(r.code, 0, `--hash-anchor 必须跑得起来（stderr：${r.err.trim()}）`);
    assert.ok(r.out.includes(want), `打印的 sha256 必须等于独立复算值 ${want}，实际：${r.out.trim()}`);
    assert.ok(r.out.includes(`0x${offset.toString(16)}`), '偏移也要打印出来（否则没法跟 .lst 对账）');
    const json = /^ {2}(\{"type":"bin".*\})$/m.exec(r.out)?.[1];
    assert.ok(json, `必须打印一份**可照抄**的锚 JSON，实际：${r.out.trim()}`);
    const a = JSON.parse(json);
    assert.equal(a.ea, ea);
    assert.equal(a.len, bytes.length);
    assert.equal(a.sha256, want);
    assert.equal(a.repo, 'reference');
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ─────────────────────────────────────────────────────────── ④ 别绕过去

test('★ 已带 sha256 的锚仍要走既有"字节摘要不匹配"那关（没被摘要要求绕过去）', () => {
  const { buf, ea, offset, bytes } = syntheticPe();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-ledger-mismatch-'));
  fs.writeFileSync(path.join(tmp, 'fixture.exe'), buf);
  try {
    const anchor = (sha256) => ({ type: 'bin', repo: 'self', path: 'fixture.exe', ea, len: bytes.length, sha256 });
    const want = createHash('sha256').update(buf.subarray(offset, offset + bytes.length)).digest('hex');
    const ok = resolveAnchor(anchor(want), { repoRoot: tmp, referenceRoot: null, tracked: null });
    assert.equal(ok.ok, true, `摘要对上就该过：${ok.why}`);
    const bad = resolveAnchor(anchor('0'.repeat(64)), { repoRoot: tmp, referenceRoot: null, tracked: null });
    assert.equal(bad.ok, false, '摘要对不上必须红（否则"带了摘要"反而变成免检）');
    assert.equal(bad.kind, 'error');
    assert.match(bad.why, /字节摘要不匹配/, '错误信息要说清是摘要不匹配，不是"文件不存在"');
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});

// ─────────────────────────────────────────────────────────── ⑤ 自描述同步

test('★ describe 的不变量 #8 与棘轮常量同源（schema 只有一份真源）', () => {
  const t = describeText();
  const lines = t.split('\n').filter((l) => l.startsWith('8. '));
  assert.equal(lines.length, 1, '不变量 #8 必须（且只）出现一次');
  assert.match(lines[0], /sha256/, '#8 的正文要点名 sha256');
  assert.match(lines[0], /REFERENCE_BIN_DIGEST_DEBT_MAX/, '#8 要写出棘轮上限的名字（否则"存量不许红"无从复核）');
  const row = (n) => t.split('\n').filter((l) => l.startsWith(`| \`${n}\` |`));
  assert.equal(row('len').length, 1, '锚点字段表里应有且只有一行 `len`');
  assert.match(row('len')[0], /必填/, '`len` 在新记录上的必填口径要写进字段表');
  assert.match(row('sha256')[0], /必填/, '`sha256` 在新记录上的必填口径要写进字段表');
  assert.match(t, /--hash-anchor/, '自描述要指向那条只读的补摘要命令');
});
