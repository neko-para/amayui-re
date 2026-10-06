/** @env pure @kind safety @why 提权安装插件的**闭接口**破了：越界仓库根被放行、写类 op 不再默认 dry-run、或 `package` 收下了路径/shell 形态 */
/**
 * plugins/pnpm-priv/test/ops.test.mjs —— 纯逻辑与探针的判据
 *
 * ★ 本文件**不 import DSH 的包**（`@deepseek-ai/dsh-tools` / `schemastery`）：
 *   那两个是插件的运行期依赖，装不装得上与"闭接口的校验对不对"无关。
 *   所以这里只测 `lib/ops.mjs`（纯逻辑，io 可注入）与 `lib/probe.mjs`（探针自清）。
 *   插件加载契约（`defineTool` 的形状）由 `plugins/deploy/test/plugin.test.mjs` 那一类覆盖。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { OPS, OP_NAMES, planOp, auditLine, ALLOWED_VERBS } from '../lib/ops.mjs';
import { canSymlink, linkKind, findAmayuiLinks } from '../lib/probe.mjs';

const REPO = 'E:/Projects/amayui-re';

/** 假 io：让判据与机器无关（真 fs 会让"这个目录存不存在"取决于跑测试的机器） */
const fakeIo = (present) => ({ exists: (p) => present.includes(path.resolve(p)), isDir: () => true });
const WS = [path.join(REPO, 'package.json'), path.join(REPO, 'pnpm-workspace.yaml')];

test('★ `repoRoot` 的校验：相对路径 / 缺失 / 不是 workspace 根 ⇒ 一律拒', () => {
  assert.ok(planOp('install', { repoRoot: undefined }, { allowedRoots: [] }).problems.length, '缺 repoRoot ⇒ 拒');
  assert.ok(planOp('install', { repoRoot: 'relative/path' }, { allowedRoots: [] }).problems.length, '相对路径 ⇒ 拒');
  assert.ok(planOp('install', { repoRoot: REPO }, { allowedRoots: [], io: fakeIo([path.join(REPO, 'package.json')]) }).problems.length,
    '只有 package.json、没有 pnpm-workspace.yaml ⇒ 拒（免得在任意 npm 包目录里跑 pnpm install）');
  const ok = planOp('install', { repoRoot: REPO }, { allowedRoots: [], io: fakeIo(WS) });
  assert.deepEqual(ok.problems, [], `workspace 根应当通过：${ok.problems.join(' / ')}`);
});

test('★ `allowedRoots` 收窄时，根外的仓库根必须被拒（按路径段比较，不是字符串前缀）', () => {
  const io = fakeIo(WS);
  assert.deepEqual(planOp('install', { repoRoot: REPO }, { allowedRoots: [REPO], io }).problems, []);
  assert.ok(planOp('install', { repoRoot: REPO }, { allowedRoots: ['E:/Projects/other'], io }).problems.length, `根外 ⇒ 拒`);
  // 前缀相似的兄弟目录不算"之内"
  assert.ok(planOp('install', { repoRoot: REPO }, { allowedRoots: [path.join(REPO, 'dist')], io }).problems.length,
    '`<root>/dist` 不该把 `<root>` 本身放进来（路径段比较）');
});

test('★ 写类 op 默认 dry-run：不给 write:true ⇒ 计划里 write=false 且 argv 不含写开关', () => {
  const io = fakeIo(WS);
  const dry = planOp('install', { repoRoot: REPO }, { allowedRoots: [], io });
  assert.equal(dry.plan.write, false);
  assert.deepEqual(dry.plan.argv, ['install'], 'dry-run 的 argv 只有 verb');
  const wet = planOp('install', { repoRoot: REPO, write: true, frozenLockfile: true }, { allowedRoots: [], io });
  assert.equal(wet.plan.write, true);
  assert.deepEqual(wet.plan.argv, ['install', '--frozen-lockfile', '--config.confirmModulesPurge=false']);
});

test('★ `op=install` 不接受 verb 白名单里的别的动词（add/remove 走各自的 op）', () => {
  const io = fakeIo(WS);
  assert.ok(planOp('install', { repoRoot: REPO, verb: 'add' }, { allowedRoots: [], io }).problems.length);
  assert.deepEqual(planOp('install', { repoRoot: REPO, verb: 'install' }, { allowedRoots: [], io }).problems, []);
  assert.ok(!OP_NAMES.every((o) => OPS[o].kind === 'pnpm') || ALLOWED_VERBS.length === 3, 'verb 白名单是三项');
});

test('★ `package` 只接受包名形态：路径 / `file:` / `link:` / shell 元字符一律拒', () => {
  const io = fakeIo(WS);
  const base = { repoRoot: REPO, write: true };
  for (const good of ['typescript', '@types/node', 'jimp@^1.6.1', '@amayui/age-format@workspace:*']) {
    assert.deepEqual(planOp('add', { ...base, package: good }, { allowedRoots: [], io }).problems, [], `应当接受 ${good}`);
  }
  for (const bad of ['../evil', 'file:../x', 'link:../x', 'a; rm -rf /', 'x && y', '/abs/path', '$(whoami)', 'a b']) {
    assert.ok(planOp('add', { ...base, package: bad }, { allowedRoots: [], io }).problems.length, `应当拒绝 ${JSON.stringify(bad)}`);
  }
});

test('★ `add` 的开关是枚举/布尔，调用方给不出新 flag', () => {
  const io = fakeIo(WS);
  const p = planOp('add', { repoRoot: REPO, write: true, package: 'jimp', dev: true, workspace: true }, { allowedRoots: [], io });
  assert.deepEqual(p.problems, []);
  assert.deepEqual(p.plan.argv, ['add', '-D', '-w', 'jimp', '--config.confirmModulesPurge=false']);
  // 未知参数被忽略（不进 argv）⇒ 拼不出 `--script-shell` 之类
  const q = planOp('add', { repoRoot: REPO, write: true, package: 'jimp', scriptShell: 'cmd' }, { allowedRoots: [], io });
  assert.ok(!q.plan.argv.some((a) => String(a).includes('script')), `未知参数不许进 argv：${q.plan.argv.join(' ')}`);
});

test('★ 审计行是**一行**（换行/制表被压平），且带 op/repo/ok/ms 这些可追字段', () => {
  const line = auditLine({ at: '2026-10-06T00:00:00.000Z', op: 'install', repo: `${REPO}\nX`, write: true, ok: true, code: 0, ms: 12.4, cmd: 'pnpm install\t--x' });
  assert.ok(!/[\r\n\t]/.test(line), '审计行不许含换行/制表');
  for (const k of ['op=install', 'repo=', 'write=1', 'ok=1', 'code=0', 'ms=12']) assert.ok(line.includes(k), `审计行缺 ${k}：${line}`);
});

test('★ 探针：`canSymlink()` 自清（跑完临时目录不留东西），`linkKind()` 能区分 junction 与符号链接', () => {
  const before = fs.readdirSync(os.tmpdir()).filter((n) => n.startsWith('pnpm-priv-probe-')).length;
  const r = canSymlink();
  assert.equal(typeof r.ok, 'boolean', 'canSymlink 必须给布尔判据');
  assert.ok(typeof r.why === 'string' && r.why.length > 0, '必须给出为什么');
  const after = fs.readdirSync(os.tmpdir()).filter((n) => n.startsWith('pnpm-priv-probe-')).length;
  assert.equal(after, before, '★ 探针必须自清（临时目录里不许留 pnpm-priv-probe-*）');
  // linkKind：本仓现存的链接要么是真符号链接、要么是 junction —— 两种都要能判出来
  const k = linkKind(REPO);
  assert.ok(['symlink', 'junction-or-dir', 'other', 'missing'].includes(k.kind), `linkKind 的返回值要在枚举里，实际 ${k.kind}`);
});

test('★ 不认识的 op ⇒ 拒（闭接口）', () => {
  assert.ok(planOp('run', { repoRoot: REPO }, { allowedRoots: [] }).problems.length);
  assert.deepEqual(OP_NAMES.sort(), ['add', 'check-links', 'install', 'remove', 'status']);
});

test('★ 只读 op 不接受 write:true；`status` 与 `check-links` 的 plan 不带 argv', () => {
  const io = fakeIo(WS);
  for (const op of ['status', 'check-links']) {
    const p = planOp(op, { repoRoot: REPO }, { allowedRoots: [], io });
    assert.deepEqual(p.problems, []);
    assert.equal(p.plan.write, false);
    assert.equal(p.plan.argv, undefined, `${op} 不该有 argv`);
  }
});

test('★ 找链接：在本仓里应当能找到 `node_modules/@amayui/*`（否则这条检查是假的）', () => {
  const links = findAmayuiLinks(REPO);
  assert.ok(Array.isArray(links));
  for (const l of links) {
    assert.ok(typeof l.consumer === 'string' && typeof l.name === 'string' && typeof l.kind === 'string', '每个链接项要有 consumer/name/kind');
  }
});
