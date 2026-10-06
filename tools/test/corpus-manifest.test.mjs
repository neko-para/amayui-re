/** @env pure @kind gate @why 清单不变量被破坏（合成清单路径：#1–#9 的守卫不再会红） */
/**
 * tools/test/corpus-manifest.test.mjs — 素材清单守卫的**单元**层
 *
 * 对**合成清单**逐条验证 §3.3 的 9 个断言**真的能红**（守卫不能是"永远绿"的摆设）。
 *
 * ★ **拆过一刀**（本轮测试分级）：原先还有两条"端到端"（真实 `corpus/assets.json` + CLI 退出码）——
 *   它们要求 47 个条目的 `dest`/`origin` 都在盘上（= 装了游戏 / LFS 已 smudge），
 *   实测（`gameInstall` 指空）会**红 2 条**，而本文件这 27 条合成用例照样全绿
 *   ⇒ 已移到 `corpus-manifest.assets.test.mjs`（`@env assets`），免得"纯路径的守卫"跟着机器环境一起红。
 *
 * 运行：`pnpm test`（默认档 = `@env pure`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import {
  DEFAULT_MANIFEST,
  canonicalStringify,
  loadManifest,
  saveManifest,
  validateManifest,
} from '../lib/manifest.mjs';
import { git, gitAttrFilter } from '../lib/exec.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const CORPUS_MJS = path.join(REPO_ROOT, 'tools', 'corpus.mjs');
const HEX64 = 'a'.repeat(64);

/** 造一个"假仓库根"（含 old/ game/ staging/ corpus/ 与一个真实文件） */
function makeRepo() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-corpus-'));
  for (const d of ['old/sub', 'game', 'staging', 'corpus']) fs.mkdirSync(path.join(root, d), { recursive: true });
  fs.writeFileSync(path.join(root, 'old', 'sub', 'x.c'), 'hello\n');
  fs.writeFileSync(path.join(root, 'corpus', 'payload.bin'), 'PAYLOAD');
  return root;
}

function makeManifest(root, entries) {
  return {
    schemaVersion: 1,
    roots: {
      oldRepo: path.join(root, 'old'),
      gameInstall: path.join(root, 'game'),
      staging: path.join(root, 'staging'),
    },
    entries,
  };
}

/** 合成清单一律关掉 fs/git/脚本三件"环境相关"的检查 ⇒ 只测断言逻辑本身 */
function check(root, entries) {
  return validateManifest(makeManifest(root, entries), {
    repoRoot: root,
    runGit: false,
    checkHashes: false,
    runRecipe: false,
  });
}

const failIds = (report) => report.checks.filter((c) => c.status === 'fail').map((c) => c.id);

function validEntry(over = {}) {
  return {
    id: 'x/one',
    kind: 'asset',
    role: 'reference',
    storage: 'external-only',
    origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64 }],
    dest: null,
    readOnly: true,
    consume: '只登记，不搬',
    ...over,
  };
}

// ─────────────────────────────────────────── 单元：9 条断言都能红

test('#1 schema：合法清单通过', () => {
  const root = makeRepo();
  const report = check(root, [validEntry()]);
  assert.deepEqual(failIds(report), [], JSON.stringify(report.checks, null, 2));
});

test('#1 schema：id 重复 → 红', () => {
  const root = makeRepo();
  const report = check(root, [validEntry(), validEntry()]);
  assert.ok(failIds(report).includes(1));
});

test('#1 schema：kind 非法 → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ kind: 'nope' })])).includes(1));
});

test('#1 schema：非自足条目的 origin 为空数组 → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ origin: [] })])).includes(1));
  // 也允许整个字段缺席（同一个判据）
  const noOrigin = validEntry();
  delete noOrigin.origin;
  assert.ok(failIds(check(root, [noOrigin])).includes(1));
});

test('#1 schema：★ 自足条目（入库的 kind=fixture）origin 允许为空 / 缺席', () => {
  const root = makeRepo();
  const base = { kind: 'fixture', role: 'fixture', storage: 'lfs', dest: 'corpus/payload.bin' };
  assert.deepEqual(failIds(check(root, [validEntry({ ...base, origin: [] })])), []);
  const noOrigin = validEntry(base);
  delete noOrigin.origin;
  assert.deepEqual(failIds(check(root, [noOrigin])), []);
  // 但"自足"只认 kind=fixture：同样的 shape 换成别的 kind 仍然要 origin
  assert.ok(failIds(check(root, [validEntry({ ...base, kind: 'asset', origin: [] })])).includes(1));
});

test('#2 自洽：external-only 却给了 dest → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ storage: 'external-only', dest: 'corpus/x' })])).includes(2));
});

test('#2 自洽：deferred 却给了 dest → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ storage: 'deferred', dest: 'corpus/x' })])).includes(2));
});

test('#2 自洽：lfs 却没有 dest → 红（★ 守卫不得崩：dest=null 时不许走到 path.resolve）', () => {
  const root = makeRepo();
  const e = validEntry({ storage: 'lfs', dest: null, origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  let report;
  assert.doesNotThrow(() => {
    report = check(root, [e]);
  }, 'dest 为 null 时必须报 #2 违规，而不是抛 "paths[1] must be of type string"');
  assert.ok(failIds(report).includes(2));
});

test('#3 存在性：dest 不在盘上 → 红', () => {
  const root = makeRepo();
  const e = validEntry({ storage: 'lfs', dest: 'corpus/does-not-exist.bin', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  assert.ok(failIds(check(root, [e])).includes(3));
});

test('#3 存在性：origin 缺失 → 红；root=staging 缺失 → 只 warning', () => {
  const root = makeRepo();
  const bad = check(root, [validEntry({ origin: [{ root: 'oldRepo', path: 'sub/nope.c', sha256: HEX64 }] })]);
  assert.ok(failIds(bad).includes(3));

  const okStaging = check(root, [validEntry({ origin: [{ root: 'staging', path: 'not-here.c', sha256: HEX64 }] })]);
  assert.deepEqual(failIds(okStaging), []);
  assert.ok(okStaging.checks.some((c) => c.status === 'warn'));
});

test('#4 校验和：不入库的文件 origin 缺 sha256 → 红', () => {
  const root = makeRepo();
  const e = validEntry({ origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  assert.ok(failIds(check(root, [e])).includes(4));
});

test('#4 校验和：目录型 origin 写了 sha256 → 红', () => {
  const root = makeRepo();
  const e = validEntry({ origin: [{ root: 'oldRepo', path: 'sub', sha256: HEX64 }] });
  assert.ok(failIds(check(root, [e])).includes(4));
});

test('#4 校验和：入库件写了 sha256 → 红', () => {
  const root = makeRepo();
  const e = validEntry({
    storage: 'lfs',
    dest: 'corpus/payload.bin',
    origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64 }],
  });
  assert.ok(failIds(check(root, [e])).includes(4));
});

test('#3 存在性：目录型 dest 为空 → 红', () => {
  const root = makeRepo();
  fs.mkdirSync(path.join(root, 'corpus', 'fx'), { recursive: true });
  const e = validEntry({ storage: 'lfs', dest: 'corpus/fx', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  assert.ok(failIds(check(root, [e])).includes(3));
});

test('#4 校验和：目录型 dest 里缺来源文件 → 红', () => {
  const root = makeRepo();
  const d = path.join(root, 'corpus', 'fx');
  fs.mkdirSync(d, { recursive: true });
  fs.writeFileSync(path.join(d, 'other.bin'), 'x');
  const e = validEntry({ storage: 'lfs', dest: 'corpus/fx', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  assert.ok(failIds(check(root, [e])).includes(4));
});

test('#4 校验和：目录型 dest 的副本必须与来源逐字节相同（篡改 ⇒ 红，拷回 ⇒ 绿）', () => {
  const root = makeRepo();
  const d = path.join(root, 'corpus', 'fx');
  fs.mkdirSync(d, { recursive: true });
  const e = validEntry({ storage: 'lfs', dest: 'corpus/fx', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  const opts = { repoRoot: root, runGit: false, checkHashes: true, runRecipe: false };

  fs.writeFileSync(path.join(d, 'x.c'), 'TAMPERED\n');
  assert.ok(failIds(validateManifest(makeManifest(root, [e]), opts)).includes(4));

  fs.copyFileSync(path.join(root, 'old', 'sub', 'x.c'), path.join(d, 'x.c'));
  assert.deepEqual(failIds(validateManifest(makeManifest(root, [e]), opts)), []);
});

test('#4 校验和：sha256 与盘上不符 → 红（这一条才是"没被改过"的证据）', () => {
  const root = makeRepo();
  const report = validateManifest(makeManifest(root, [validEntry()]), {
    repoRoot: root,
    runGit: false,
    checkHashes: true,
    runRecipe: false,
  });
  assert.ok(failIds(report).includes(4));
});

test('#6 LFS 一致性：真仓的 .gitattributes 必须把 disasm zip 交给 LFS', () => {
  const r = gitAttrFilter(REPO_ROOT, 'corpus/disasm/disasm-20260930.zip');
  assert.equal(r.value, 'lfs', r.detail);
});

test('★ #6：目录型 dest 的载荷**不靠 origin 枚举**（自足条目 origin 为空也要看得见）', () => {
  // 真仓、真 .gitattributes，两种"LFS 声明坏掉"的形态都必须红 —— 而 fixture 的 origin 已按自足条目清空，
  // 靠"origin 里的来源文件"枚举的旧写法在这两种情形下都会**静默全绿**。
  const attrPath = path.join(REPO_ROOT, '.gitattributes');
  const original = fs.readFileSync(attrPath, 'utf8');
  const entry = () =>
    validEntry({ id: 'fixtures/save-samples', kind: 'fixture', role: 'fixture', storage: 'lfs', dest: 'corpus/fixtures', origin: [] });
  const c6 = () =>
    validateManifest(makeManifest(REPO_ROOT, [entry()]), { repoRoot: REPO_ROOT, runGit: true, checkHashes: false, runRecipe: false })
      .checks.find((c) => c.id === 6);
  const before = git(REPO_ROOT, ['status', '--porcelain']); // 只在 git 可用时才断言"没动 git 状态"

  try {
    // ① 规则还在、只是丢了 filter=lfs（保留 -text）⇒ 逐文件点名
    fs.writeFileSync(attrPath, original.replace(/ filter=lfs diff=lfs merge=lfs/g, ''), 'utf8');
    assert.equal(c6().status, 'fail', '① 载荷丢了 filter=lfs 必须红');
    assert.ok(c6().details.some((d) => /SAVE79\.DAT/.test(d)), `① 必须点名盘上的载荷，实际：${JSON.stringify(c6().details)}`);

    // ② LFS 规则整块被删（载荷会退化成 text: auto）⇒ 目录级兜底断言
    fs.writeFileSync(attrPath, `${original.split('\n')[0]}\n`, 'utf8');
    assert.equal(c6().status, 'fail', '② LFS 规则整块消失必须红');
    assert.ok(c6().details.some((d) => /一个 filter=lfs 的文件都没有/.test(d)), `② 必须报"规则没覆盖"，实际：${JSON.stringify(c6().details)}`);
  } finally {
    fs.writeFileSync(attrPath, original, 'utf8'); // 逐字节还回去
  }
  assert.equal(fs.readFileSync(attrPath, 'utf8'), original, '测试必须把 .gitattributes 原样还回去');
  assert.equal(c6().status, 'pass', '还原后必须恢复全绿');
  if (before !== '') assert.equal(git(REPO_ROOT, ['status', '--porcelain']), before, '这条用例不得改动仓库的 git 状态');
});

test('#8 知识准入门：knowledge-source + lfs → 红', () => {
  const root = makeRepo();
  const e = validEntry({
    kind: 'knowledge-source',
    storage: 'lfs',
    dest: 'corpus/payload.bin',
    origin: [{ root: 'oldRepo', path: 'sub/x.c' }],
  });
  assert.ok(failIds(check(root, [e])).includes(8));
});

test('#8 知识准入门：knowledge-source + deferred 且没有前身 → 通过', () => {
  const root = makeRepo();
  const e = validEntry({ kind: 'knowledge-source', role: 'archived', storage: 'deferred' });
  assert.deepEqual(failIds(check(root, [e])), []);
});

test('#9 真前身：ref 指向不存在的 id → 红', () => {
  const root = makeRepo();
  const e = validEntry({ derivedFrom: [{ ref: 'binary/nope' }] });
  assert.ok(failIds(check(root, [e])).includes(9));
});

test('#9 真前身：入库语料的 derivedFrom 里没有 kind=binary → 红', () => {
  const root = makeRepo();
  const tool = validEntry({ id: 'tooling/x', kind: 'tooling', role: 'rebuild', storage: 'deferred' });
  const corpusEntry = validEntry({
    id: 'disasm/bundle',
    kind: 'disasm-corpus',
    role: 'baseline',
    storage: 'lfs',
    dest: 'corpus/payload.bin',
    origin: [{ root: 'staging', path: 'x.c' }],
    derivedFrom: [{ ref: 'tooling/x' }],
    recipe: 'tools/corpus.mjs',
  });
  assert.ok(failIds(check(root, [tool, corpusEntry])).includes(9));
});

test('#7 语料保真：入库的 disasm-corpus 没有 recipe → 红', () => {
  const root = makeRepo();
  const bin = validEntry({ id: 'binary/age', kind: 'binary', role: 'deferred', storage: 'deferred', origin: [{ root: 'gameInstall', path: 'x.c', sha256: HEX64 }] });
  const e = validEntry({
    id: 'disasm/bundle',
    kind: 'disasm-corpus',
    role: 'baseline',
    storage: 'lfs',
    dest: 'corpus/payload.bin',
    origin: [{ root: 'staging', path: 'x.c' }],
    derivedFrom: [{ ref: 'binary/age' }],
  });
  assert.ok(failIds(check(root, [bin, e])).includes(7));
});

test('★ 豁免口径（用户拍板）：deferred 的 disasm-corpus 不需要 recipe，也不需要前身', () => {
  const root = makeRepo();
  const e = validEntry({ id: 'disasm/not-carried', kind: 'disasm-corpus', role: 'deferred', storage: 'deferred' });
  const report = check(root, [e]);
  assert.deepEqual(failIds(report), []);
  // 而且它确实在 #7/#9 的详情里被说明为"本轮无入库语料"
  assert.match(report.checks.find((c) => c.id === 7).message, /deferred/);
});

// ─────────────────────────────────────────── 序列化确定性

test('canonicalStringify：同输入 ⇒ 同字节，且键序固定', () => {
  const root = makeRepo();
  const a = makeManifest(root, [validEntry({ note: 'x', blocks: ['M1'] })]);
  const b = { ...a, entries: [{ ...a.entries[0], consume: a.entries[0].consume }] };
  assert.equal(canonicalStringify(a), canonicalStringify(b));
  const text = canonicalStringify(a);
  assert.match(text, /"schemaVersion": 1/);
  // 键序：id 在 kind 之前，consume 在 blocks 之前
  assert.ok(text.indexOf('"id"') < text.indexOf('"kind"'));
  assert.ok(text.indexOf('"consume"') < text.indexOf('"blocks"'));
  assert.ok(text.endsWith('\n'));
});

// ─────────────────────────────────────────── 端到端（**已移走**）
//
// ★ 原先这里的两条"端到端"（真实清单全绿 / CLI 退出码 0）依赖**盘上资产是否齐全** ⇒
//   已移到 `corpus-manifest.assets.test.mjs`（`@env assets`）。本文件只管**合成清单**的单元路径。

test('端到端：写入非法状态会被拒绝（清单不是"写什么是什么"）', () => {
  const root = makeRepo();
  const tmpManifest = path.join(root, 'assets.json');
  const original = fs.readFileSync(DEFAULT_MANIFEST, 'utf8');
  fs.writeFileSync(tmpManifest, original);

  // ★ 这个 patch 在任何现有状态下都非法（lfs 必须有 dest）⇒ 与清单当前状态无关
  const manifest = loadManifest(DEFAULT_MANIFEST);
  const patched = {
    ...manifest,
    entries: manifest.entries.map((e) => (e.id === 'disasm/bundle' ? { ...e, storage: 'lfs', dest: null } : e)),
  };
  const res = saveManifest(patched, tmpManifest, { repoRoot: REPO_ROOT, runGit: false, checkHashes: false, runRecipe: false });
  assert.equal(res.ok, false, 'lfs 却没有 dest 必须被拒绝');
  assert.equal(fs.readFileSync(tmpManifest, 'utf8'), original, '被拒后清单必须与原文逐字节相同');
});
