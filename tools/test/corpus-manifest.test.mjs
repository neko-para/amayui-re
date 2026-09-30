/**
 * tools/test/corpus-manifest.test.mjs — 素材清单守卫的测试
 *
 * 分两层：
 *   ① **单元**：对合成清单逐条验证 §3.3 的 9 个断言**真的能红**（守卫不能是"永远绿"的摆设）；
 *   ② **端到端**：对真实 `corpus/assets.json` 跑一次 CLI `--validate`，并要求退出码 0。
 *
 * 运行：`pnpm test`（根 package.json 的 test 脚本 = node --test 加 glob；glob 形态见该脚本，别写进本注释）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import {
  DEFAULT_MANIFEST,
  REPO_ROOT,
  canonicalStringify,
  gitAttrFilter,
  validateManifest,
} from '../corpus.mjs';

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

test('#1 schema：origin 为空数组 → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ origin: [] })])).includes(1));
});

test('#2 自洽：external-only 却给了 dest → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ storage: 'external-only', dest: 'corpus/x' })])).includes(2));
});

test('#2 自洽：deferred 却给了 dest → 红', () => {
  const root = makeRepo();
  assert.ok(failIds(check(root, [validEntry({ storage: 'deferred', dest: 'corpus/x' })])).includes(2));
});

test('#2 自洽：lfs 却没有 dest → 红', () => {
  const root = makeRepo();
  const e = validEntry({ storage: 'lfs', dest: null, origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  assert.ok(failIds(check(root, [e])).includes(2));
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

// ─────────────────────────────────────────── 端到端

test('端到端：真实 corpus/assets.json 过 --validate（退出码 0）', () => {
  assert.ok(fs.existsSync(DEFAULT_MANIFEST), 'corpus/assets.json 必须存在');
  const out = execFileSync(process.execPath, [CORPUS_MJS, '--validate', '--json'], {
    cwd: REPO_ROOT,
    encoding: 'utf8',
  });
  const report = JSON.parse(out);
  const bad = report.checks.filter((c) => c.status === 'fail').map((c) => `#${c.id} ${c.title}：${c.message}`);
  assert.deepEqual(bad, [], `守卫必须全绿，实际：\n${bad.join('\n')}`);
  assert.equal(report.failures, 0);
});

test('端到端：--list 能列出全部条目', () => {
  const out = execFileSync(process.execPath, [CORPUS_MJS, '--list', '--json'], { cwd: REPO_ROOT, encoding: 'utf8' });
  const rows = JSON.parse(out);
  assert.ok(Array.isArray(rows) && rows.length > 10);
  assert.ok(rows.every((r) => typeof r.id === 'string' && r.id.includes('/')));
});

test('端到端：写入非法状态会被回滚（清单不是"写什么是什么"）', () => {
  const root = makeRepo();
  const tmpManifest = path.join(root, 'assets.json');
  const original = fs.readFileSync(DEFAULT_MANIFEST, 'utf8');
  fs.writeFileSync(tmpManifest, original);

  let failed = false;
  try {
    execFileSync(
      process.execPath,
      [CORPUS_MJS, '--manifest', tmpManifest, '--set', 'disasm/bundle', '{"storage":"lfs"}', '--write'],
      { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] },
    );
  } catch {
    failed = true;
  }
  assert.ok(failed, '把 deferred 的 bundle 直接改成 lfs（dest 仍是 null）必须被守卫拒绝');
  assert.equal(fs.readFileSync(tmpManifest, 'utf8'), original, '回滚后清单必须与原文逐字节相同');
});
