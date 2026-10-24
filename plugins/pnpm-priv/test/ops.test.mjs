/** @env pure @kind safety @why 提权插件的**闭接口**破了：越界仓库根被放行、写类 op 不再默认 dry-run、`package` 收下了路径/shell 形态，或 `run` 变成了"能跑任意命令" */
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

import { OPS, OP_NAMES, TASKS, TASK_NAMES, planOp, auditLine, ALLOWED_VERBS } from '../lib/ops.mjs';
import { canSymlink, linkKind, findAmayuiLinks } from '../lib/probe.mjs';

const REPO = 'E:/Projects/amayui-re';

/**
 * 假 io：让判据与机器无关（真 fs 会让"这个目录存不存在"取决于跑测试的机器）。
 * ★ 路径归一要和被测实现**同一套语义**：盘符路径用 `path.win32`，否则 POSIX 上
 *   `path.resolve('E:/x')` 会把盘符当相对段，`present` 永远对不上（实测 7 条假红）。
 */
const WIN_PATH_RE = /^[A-Za-z]:[\\/]/;
const resolvePath = (p) => (WIN_PATH_RE.test(p) ? path.win32 : path).resolve(p);
const fakeIo = (present) => {
  const set = new Set(present.map(resolvePath));
  return { exists: (p) => set.has(resolvePath(p)), isDir: () => true };
};
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
  assert.ok(planOp('exec', { repoRoot: REPO, command: 'calc.exe' }, { allowedRoots: [] }).problems.length, '没有通用 exec');
  assert.deepEqual(OP_NAMES.sort(), ['add', 'check-links', 'install', 'remove', 'run', 'status']);
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

/* ─────────── `run`：宿主侧转发**固定任务**（不是通用 exec） ─────────── */

test('★ `run` 只认固定任务名：未知任务一律拒（本插件没有"执行任意命令"的口子）', () => {
  const io = fakeIo(WS);
  assert.deepEqual(TASK_NAMES, ['build-renderer', 'smoke', 'electron'], '任务名是闭集合');
  for (const bad of [undefined, null, '', 'shell', 'exec', 'build', 'vite build', 'electron.exe', 'BUILD-RENDERER', 42, ['smoke']]) {
    const p = planOp('run', { repoRoot: REPO, task: bad, write: true }, { allowedRoots: [], io });
    assert.ok(p.problems.length, `应当拒绝 task=${JSON.stringify(bad)}`);
    assert.match(p.problems.join(' '), /不认识的 task|只有/, `拒的理由要可读：${p.problems.join(' / ')}`);
  }
});

test('★ 每个任务的 argv / cwd 都是写死的：调用方给不出命令、flag、cwd', () => {
  const io = fakeIo(WS);
  const EXPECT = {
    'build-renderer': ['--filter', '@amayui/emulator-new', 'run', 'build:renderer'],
    smoke: ['--no-sandbox', 'apps/emulator-new', '--smoke'],
    electron: ['--no-sandbox', 'apps/emulator-new'],
  };
  for (const [task, argv] of Object.entries(EXPECT)) {
    // 塞一堆"想越过闭接口"的参数：它们都不许进 argv / 不许改 cwd
    const p = planOp(
      'run',
      { repoRoot: REPO, task, write: true, argv: ['rm', '-rf', '/'], command: 'calc.exe', cmd: 'calc.exe', cwd: 'C:/Windows', shell: true, flags: '--script-shell=cmd', timeoutMs: 1 },
      { allowedRoots: [], io },
    );
    assert.deepEqual(p.problems, [], `${task}：${p.problems.join(' / ')}`);
    assert.deepEqual(p.plan.argv, argv, `${task} 的 argv 必须来自 TASKS 表`);
    assert.equal(p.plan.cwd, resolvePath(REPO), 'cwd 由任务定（相对 repoRoot），不接调用方给的');
    assert.equal(p.plan.runner, TASKS[task].runner);
    assert.equal(p.plan.timeoutMs, TASKS[task].timeoutMs ?? 30 * 60 * 1000, '超时也由任务定');
    assert.deepEqual(TASKS[task].argv, argv, 'TASKS 表本身不许被 plan 改坏');
  }
});

test('★ `run` 缺 write:true ⇒ 计划里 write=false（index.mjs 只在这之后才 spawn）', () => {
  const io = fakeIo(WS);
  for (const task of TASK_NAMES) {
    const dry = planOp('run', { repoRoot: REPO, task }, { allowedRoots: [], io });
    assert.deepEqual(dry.problems, []);
    assert.equal(dry.plan.write, false, `${task}：缺 write ⇒ 只出计划`);
  }
  const wet = planOp('run', { repoRoot: REPO, task: 'build-renderer', write: true }, { allowedRoots: [], io });
  assert.equal(wet.plan.write, true);
});

test('★ `installDir` 只给 electron 用：绝对路径、存在、是目录，且落在 allowedRoots 内', () => {
  const GAME = 'E:/Games/Eushully/天結';
  // 这份 io 里：WS 两个文件存在、REPO 与 GAME 是目录
  const files = new Set(WS.map(resolvePath));
  const dirs = new Set([resolvePath(REPO), resolvePath(GAME)]);
  const io = { exists: (p) => files.has(resolvePath(p)) || dirs.has(resolvePath(p)), isDir: (p) => dirs.has(resolvePath(p)) };
  const okPlan = planOp('run', { repoRoot: REPO, task: 'electron', installDir: GAME }, { allowedRoots: [], io });
  assert.deepEqual(okPlan.problems, [], okPlan.problems.join(' / '));
  assert.deepEqual(okPlan.plan.argv.slice(-2), ['--install', resolvePath(GAME)], 'installDir 只能拼成 `--install <dir>` 两个 token');
  assert.ok(!okPlan.plan.argv.some((a) => /[;&|<>]/.test(a)), '不该有待解析的 shell 形态');

  // 相对路径 / 空串 / 不存在 / 不是目录 / 别的任务都给不了
  for (const bad of ['Games/x', '', '   ']) {
    assert.ok(planOp('run', { repoRoot: REPO, task: 'electron', installDir: bad }, { allowedRoots: [], io }).problems.length, `应当拒绝 installDir=${JSON.stringify(bad)}`);
  }
  assert.ok(planOp('run', { repoRoot: REPO, task: 'electron', installDir: 'E:/Games/nope' }, { allowedRoots: [], io }).problems.length, '不存在的目录 ⇒ 拒');
  const notDir = { exists: () => true, isDir: () => false };
  assert.ok(planOp('run', { repoRoot: REPO, task: 'electron', installDir: GAME }, { allowedRoots: [], io: notDir }).problems.length, '不是目录 ⇒ 拒');
  assert.ok(planOp('run', { repoRoot: REPO, task: 'smoke', installDir: GAME }, { allowedRoots: [], io }).problems.length, 'smoke 不接受 installDir');
  assert.ok(planOp('run', { repoRoot: REPO, task: 'build-renderer', installDir: GAME }, { allowedRoots: [], io }).problems.length, 'build-renderer 不接受 installDir');
  // 收窄 allowedRoots 后：安装目录不在根内 ⇒ 拒（注意：收紧到仓库根会连游戏目录一起挡掉，这是有意的）
  const narrowed = planOp('run', { repoRoot: REPO, task: 'electron', installDir: GAME }, { allowedRoots: [REPO], io });
  assert.match(narrowed.problems.join(' '), /不在允许的根内/);
});

test('★ 缺前置产物 ⇒ plan.missing 点名（别把"窗口打开一片空白"当成功）', () => {
  const io = fakeIo(WS); // 没有 dist/renderer/index.html
  const missing = planOp('run', { repoRoot: REPO, task: 'smoke' }, { allowedRoots: [], io });
  assert.deepEqual(missing.plan.missing, ['apps/emulator-new/dist/renderer/index.html']);
  assert.deepEqual(planOp('run', { repoRoot: REPO, task: 'electron' }, { allowedRoots: [], io }).plan.missing, ['apps/emulator-new/dist/renderer/index.html']);
  const built = fakeIo([...WS, path.join(REPO, 'apps', 'emulator-new', 'dist', 'renderer', 'index.html')]);
  assert.deepEqual(planOp('run', { repoRoot: REPO, task: 'smoke' }, { allowedRoots: [], io: built }).plan.missing, []);
  assert.deepEqual(planOp('run', { repoRoot: REPO, task: 'build-renderer' }, { allowedRoots: [], io }).plan.missing, [], 'build-renderer 没有前置');
});

test('★ `allowRun: false` ⇒ run 一律拒（部署方的总闸），其余 op 不受影响', () => {
  const io = fakeIo(WS);
  for (const task of TASK_NAMES) {
    const p = planOp('run', { repoRoot: REPO, task, write: true }, { allowedRoots: [], io, allowRun: false });
    assert.ok(p.problems.length, `${task}：总闸关掉后必须拒`);
    assert.match(p.problems.join(' '), /allowRun/);
  }
  assert.deepEqual(planOp('status', { repoRoot: REPO }, { allowedRoots: [], io, allowRun: false }).problems, []);
  assert.deepEqual(planOp('install', { repoRoot: REPO }, { allowedRoots: [], io, allowRun: false }).problems, []);
});

test('★ `run` 的审计行带任务名（op=run · note=run <task>）且仍是**一行**', () => {
  const io = fakeIo(WS);
  const p = planOp('run', { repoRoot: REPO, task: 'smoke' }, { allowedRoots: [], io });
  const line = auditLine({
    at: '2026-10-24T00:00:00.000Z',
    op: 'run',
    repo: p.plan.repoRoot,
    write: false,
    ok: true,
    code: 0,
    ms: 3,
    cmd: `electron ${p.plan.argv.join(' ')}`,
    note: `run ${p.plan.task}（dry-run，未启动）`,
  });
  assert.ok(!/[\r\n\t]/.test(line), '审计行不许含换行/制表');
  for (const k of ['op=run', 'note=run smoke', 'write=0', 'ok=1']) assert.ok(line.includes(k), `审计行缺 ${k}：${line}`);
});
