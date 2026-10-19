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
import { git, gitAttrFilter, runCapture } from '../lib/exec.mjs';
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

test('#3 存在性：external-only / deferred / staging 的外部来源缺失 → 只 warning', () => {
  const root = makeRepo();
  for (const storage of ['external-only', 'deferred', 'staging']) {
    const entry = storage === 'staging'
      ? validEntry({ origin: [{ root: 'staging', path: 'not-here.c', sha256: HEX64 }] })
      : validEntry({ storage, origin: [{ root: 'oldRepo', path: 'sub/nope.c', sha256: HEX64 }] });
    const rep = check(root, [entry]);
    assert.deepEqual(failIds(rep), [], `${storage} 缺件不该硬红：${JSON.stringify(rep.checks.find((c) => c.id === 3).details)}`);
    assert.ok(rep.checks.some((c) => c.id === 3 && c.status === 'warn'), `${storage} 必须留下 warning（不许静默）`);
  }
});

test('#3 存在性：**入库件**（lfs / git）的 origin 缺失仍是硬红（来源必须可再取）', () => {
  const root = makeRepo();
  for (const storage of ['lfs', 'git']) {
    const e = validEntry({
      storage,
      dest: 'corpus/payload.bin',
      origin: [{ root: 'oldRepo', path: 'sub/nope.c', sha256: HEX64 }],
    });
    assert.ok(failIds(check(root, [e])).includes(3), `${storage} 的来源缺失必须硬红`);
  }
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
  // ★ 拒的理由必须是**这条 patch 自己的**（#2）—— 只断言 `ok === false` 会在"缺件的机器"上**假绿**：
  //   那里到处是无关红灯，写入被拒的理由跟这条 patch 毫无关系（实测见 `.tmp/req-verify/probe.mjs`）。
  assert.match(res.reason, /#2/, `拒的理由必须点到 #2，实际：${res.reason}`);
  assert.equal(fs.readFileSync(tmpManifest, 'utf8'), original, '被拒后清单必须与原文逐字节相同');
});

// ─────────────────────────────────────────── 写入口：闸门 = **不新增失败**
//   `REQ-01M52F9K25YNPJEXQ74QE4CT9Z`：旧闸门是"零失败" ⇒ 缺件的机器（macOS 侧没有游戏安装树、
//   旧仓 `install/`+`raw-parts/` 被上游 .gitignore 忽略）上**任何**条目都写不进去，挡它的红灯却与要写的条目无关。

/** 盘上先放一条"与本条目无关的红"（origin 缺失），返回 {root, tmp, manifest} */
function withStaleRed() {
  const root = makeRepo();
  const tmp = path.join(root, 'assets.json');
  // ★ 用**入库件缺 dest**（本机没 smudge / 没同步）当"这台机器本来就有的红灯"：
  //   external-only 的来源缺件现在只是 warning（不再是 hard），拿它当 stale red 会让本组用例失去前提。
  const stale = validEntry({
    id: 'x/stale',
    storage: 'lfs',
    dest: 'corpus/absent-stale.bin',
    origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64 }],
  });
  fs.writeFileSync(tmp, canonicalStringify(makeManifest(root, [stale])));
  return { root, tmp, manifest: loadManifest(tmp) };
}
const WRITE_OPTS = (root) => ({ repoRoot: root, runGit: false, checkHashes: false, runRecipe: false });

test('★ 写入口：这台机器本来就有的红灯**不挡**写入（闸门是"不新增失败"，不是"零失败"）', () => {
  const { root, tmp, manifest } = withStaleRed();
  const fresh = validEntry({ id: 'x/fresh' });
  const res = saveManifest({ ...manifest, entries: [...manifest.entries, fresh] }, tmp, WRITE_OPTS(root));
  assert.equal(res.ok, true, `与本条目无关的红灯不许掐死写入：${res.reason}`);
  const text = fs.readFileSync(tmp, 'utf8');
  assert.match(text, /x\/fresh/, '新条目要真的落盘');
  assert.match(text, /absent-stale\.bin/, '那条机器本地的红灯要**原样留着**（不是被吞掉、也不是被"修好"）');
});

test('★ 写入口：**新写入**引出来的失败照样挡（dest 不在盘上 ⇒ 拒 + 一个字节都不留）', () => {
  const { root, tmp, manifest } = withStaleRed();
  const before = fs.readFileSync(tmp, 'utf8');
  const bad = validEntry({ id: 'x/bad', storage: 'lfs', dest: 'corpus/absent.bin', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] });
  const res = saveManifest({ ...manifest, entries: [...manifest.entries, bad] }, tmp, WRITE_OPTS(root));
  assert.equal(res.ok, false, '新条目自己引出的失败必须挡（否则"不新增失败"就是放宽守卫）');
  assert.match(res.reason, /#3/, `理由要点到它自己那处失败，实际：${res.reason}`);
  assert.equal(fs.readFileSync(tmp, 'utf8'), before, '被拒后清单必须逐字节不变');
});

test('★ 写入口：比较粒度必须是**明细集合** —— 失败"条数"没变但换了项，也要挡（只比计数会假绿）', () => {
  const { root, tmp, manifest } = withStaleRed();
  // 候选：原来那处缺失变成"在盘上"（该失败消失），同时新条目引入**另一处**缺失 ⇒ #3 的计数**还是 1**
  const fixed = validEntry({ id: 'x/now-present', storage: 'lfs', dest: 'corpus/payload.bin', origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64 }] });
  const bad = validEntry({ id: 'x/other-missing', storage: 'lfs', dest: 'corpus/other-absent.bin', origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64 }] });
  const candidate = { ...manifest, entries: [fixed, bad] };
  const beforeRep = validateManifest(manifest, WRITE_OPTS(root));
  const afterRep = validateManifest(candidate, WRITE_OPTS(root));
  assert.equal(afterRep.failures, beforeRep.failures, '前置：两次的**失败条数**必须相同，否则这条守卫测不到粒度');
  const res = saveManifest(candidate, tmp, WRITE_OPTS(root));
  assert.equal(res.ok, false, '换了一个失败项也算新增失败；只比计数会在这里假绿');
  assert.match(res.reason, /other-absent\.bin/, `理由要点到那个**新**失败项，实际：${res.reason}`);
});

// ─────────────────────────────────────────── origin 的 machine-local 档

test('#3：disposition=machine-local 的 origin 缺件 ⇒ 只 warn，且**逐条报数**（不许静默丢）', () => {
  const root = makeRepo();
  const e = validEntry({ origin: [{ root: 'oldRepo', path: 'sub/gone.bin', disposition: 'machine-local' }] });
  const report = check(root, [e]);
  assert.deepEqual(failIds(report), []);
  const c3 = report.checks.find((c) => c.id === 3);
  assert.equal(c3.status, 'warn');
  assert.match(c3.message, /1 处允许的暂缺/);
  assert.match(c3.details.join('\n'), /machine-local/);
});

test('#4：machine-local **既不要求 pin、也不比对**（文件在盘上、没有 sha256 同样合规）', () => {
  const root = makeRepo();
  const e = validEntry({ origin: [{ root: 'oldRepo', path: 'sub/x.c', disposition: 'machine-local' }] });
  assert.deepEqual(failIds(check(root, [e])), []);
  // ★ 开哈希也要一样（上层那个 check() 关掉了哈希比对）
  const rep = validateManifest(makeManifest(root, [e]), { repoRoot: root, runGit: false, checkHashes: true, runRecipe: false });
  assert.deepEqual(failIds(rep), [], 'machine-local 不进 #4：既不要求 pin，也不比对');
});

test('#1：machine-local 与 sha256 不许共存 · 入库件不许标 machine-local · 闭集合外的值要红', () => {
  const root = makeRepo();
  const withPin = validEntry({ origin: [{ root: 'oldRepo', path: 'sub/x.c', sha256: HEX64, disposition: 'machine-local' }] });
  assert.ok(failIds(check(root, [withPin])).includes(1), 'machine-local 与 sha256 并存 ⇒ #1 红');
  const carried = validEntry({
    storage: 'lfs',
    dest: 'corpus/payload.bin',
    origin: [{ root: 'oldRepo', path: 'sub/x.c', disposition: 'machine-local' }],
  });
  assert.ok(failIds(check(root, [carried])).includes(1), '入库件的来源必须可再取 ⇒ 不许 machine-local');
  const bogus = validEntry({ origin: [{ root: 'oldRepo', path: 'sub/x.c', disposition: 'nope' }] });
  assert.ok(failIds(check(root, [bogus])).includes(1), 'disposition 是闭集合');
});

test('★ --scan 只补**该补的** pin：machine-local 跳过（本机产物写 pin 只会制造机器相关的红）', () => {
  const root = makeRepo();
  const tmp = path.join(root, 'assets.json');
  const normal = validEntry({ id: 'x/normal', origin: [{ root: 'oldRepo', path: 'sub/x.c' }] }); // 缺 pin ⇒ 会被补
  const local = validEntry({ id: 'x/machine-local', origin: [{ root: 'oldRepo', path: 'sub/x.c', disposition: 'machine-local' }] });
  fs.writeFileSync(tmp, canonicalStringify(makeManifest(root, [normal, local])));
  const args = [CORPUS_MJS, '--scan', '--manifest', tmp, '--write', '--no-git', '--no-recipe'];
  const r = runCapture(REPO_ROOT, process.execPath, args);
  assert.equal(r.code, 0, `--scan 必须成功：${r.out}${r.error}`);
  assert.match(r.out, /另有 1 处 machine-local 跳过/, `--scan 要说清它跳过了什么：${r.out}`);
  const after = JSON.parse(fs.readFileSync(tmp, 'utf8'));
  const get = (id) => after.entries.find((e) => e.id === id).origin[0];
  assert.equal(typeof get('x/normal').sha256, 'string', '普通 origin 的 pin 必须补上');
  assert.equal(get('x/machine-local').sha256, undefined, 'machine-local 的 pin 不许补');
});
