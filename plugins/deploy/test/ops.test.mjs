/**
 * plugins/deploy/test/ops.test.mjs —— **闭接口与审计行**的基建契约
 *
 * 测什么（`AGENTS.md` §10：只测基建契约，不测业务）：
 *   ① **接口是闭的**：op 只能取表里那几个；写类 op 没有 `write:true` 就只出计划；
 *   ② **参数校验能红**：越界路径 / 相对路径 / 盘符根 / 枚举写错 / GUID 写错 ⇒ 一个都不许放过；
 *   ③ **argv 是拼死的**：透传的参数只能以既定形状出现，**塞不进新 flag**；
 *   ④ **审计是一条一行**：换行/制表被压平（否则台账会被写坏）；缺依赖时给得出**可执行**的修法。
 *
 * 不测什么：不跑真特权动作（那要装进 DSH 后由工具调用来回答），也不测 DSH 的 API 形状（那是宿主的契约）。
 *
 * 运行：`pnpm test`（根 package.json 的 glob 已含 `plugins/*/test/**`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { DEFAULT_ALLOWED_ROOTS, OP_NAMES, OPS, auditLine, insideAnyRoot, planOp, realIo } from '../lib/ops.mjs';

/** 夹具 io：只认这几个"存在"的路径（不碰真磁盘 ⇒ 越界判据与机器无关） */
const io = {
  exists: (p) => ['E:\\repo\\dist', 'E:\\repo\\dist\\ui-bake', 'E:\\outside\\trees', 'E:\\Games\\G', 'E:\\Games\\G\\DATA1.ALF', 'E:\\dl\\LEProc.exe'].includes(p),
  isDir: (p) => ['E:\\repo\\dist', 'E:\\repo\\dist\\ui-bake', 'E:\\outside\\trees', 'E:\\Games\\G'].includes(p),
};
const env = { repoRoot: 'E:\\repo', allowedRoots: [...DEFAULT_ALLOWED_ROOTS, 'E:\\outside\\trees'], io };

test('★ op 是闭集合：不认识的 op / 写类 op 少了 write ⇒ 红且什么都不做', () => {
  assert.deepEqual(OP_NAMES, ['status', 'probe-link', 'probe-write', 'install-tree', 'bake-ui', 'relabel-medium']);
  assert.ok(planOp('run', { cmd: 'rm -rf /' }, env).problems.length > 0, '**没有**"执行任意命令"这种 op');
  assert.ok(planOp('install-tree', { out: 'E:\\outside\\trees\\x' }, env).plan, '合法调用要给出计划');
  // 探针自己不写东西 ⇒ 不接受 write:true（避免"以为它写了"的误会）
  assert.ok(planOp('probe-link', { dir: 'E:\\Games\\G', write: true }, env).problems.some((p) => /不接受 write/.test(p)));
});

test('★ 路径校验：越界 / 相对 / 盘符根 / 不存在 ⇒ 一律红', () => {
  const bad = (args) => planOp('install-tree', args, env).problems.join(' | ');
  assert.match(bad({ out: 'E:\\Windows\\System32' }), /不在允许的根内/);
  assert.match(bad({ out: 'dist\\install' }), /必须是绝对路径/);
  assert.match(bad({ out: 'E:\\' }), /盘符根/);
  assert.match(bad({ out: 'E:\\repo\\dist-other\\x' }), /不在允许的根内/, '`dist-other` 不算 `dist` 之内（按路径段比，不是字符串前缀）');
  assert.match(bad({ out: 'E:\\outside\\trees\\x', baked: 'E:\\nope' }), /baked 必须是一个存在的绝对目录/);
  assert.ok(!bad({ out: 'E:\\outside\\trees\\x' }), '允许的根之内 ⇒ 不报错');
  assert.match(planOp('relabel-medium', { out: 'E:\\outside\\trees\\x' }, env).problems.join(' | '), /out 不存在/);
  assert.equal(insideAnyRoot('E:\\repo\\dist\\a', ['E:\\repo\\dist']), true);
  assert.equal(insideAnyRoot('E:\\repo\\dist-other', ['E:\\repo\\dist']), false);
});

test('★ argv 是拼死的：透传参数只能以既定形状出现（塞不进新 flag）', () => {
  const r = planOp(
    'install-tree',
    { out: 'E:\\outside\\trees\\x', write: true, baked: 'E:\\repo\\dist\\ui-bake', alf: 'copy', leCmd: 'E:\\dl\\LEProc.exe', leProfile: '1bad53a5-5774-46ee-bdcd-0afe948cf006', relabelMedium: true },
    env,
  );
  assert.deepEqual(r.problems, []);
  assert.deepEqual(r.plan.argv, [
    'release', 'install',
    '--out', 'E:\\outside\\trees\\x',
    '--baked', 'E:\\repo\\dist\\ui-bake',
    '--alf', 'copy',
    '--le-cmd', 'E:\\dl\\LEProc.exe',
    '--le-profile', '1bad53a5-5774-46ee-bdcd-0afe948cf006',
    '--relabel-medium',
    '--write',
  ]);
  // 少 write ⇒ 不带 --write（工具自己会 dry-run）
  assert.ok(!planOp('install-tree', { out: 'E:\\outside\\trees\\x' }, env).plan.argv.includes('--write'));
  // 枚举/形态写错 ⇒ 红
  assert.match(planOp('install-tree', { out: 'E:\\outside\\trees\\x', alf: 'symlink' }, env).problems.join(' | '), /alf 只能是/);
  assert.match(planOp('install-tree', { out: 'E:\\outside\\trees\\x', leProfile: '不是guid' }, env).problems.join(' | '), /GUID 形态/);
  // 路径边界要在**给了 write:true** 之后才轮到判（否则先被"必须显式 write"拦下）
  assert.match(planOp('bake-ui', { out: 'E:\\Windows', write: true }, env).problems.join(' | '), /不在允许的根内/);
  // ★ `ui-bake build` 没有 dry-run ⇒ 没给 write:true 必须拒（否则"缺省 dry-run"是假话）
  assert.match(planOp('bake-ui', {}, env).problems.join(' | '), /必须显式给 write:true/);
  assert.deepEqual(planOp('bake-ui', { write: true }, env).problems, []);
  assert.ok(planOp('bake-ui', { write: true }, env).plan.argv.includes('--write'));
});

test('★ 审计是一行：换行/制表被压平，字段齐全', () => {
  const line = auditLine({ at: '2026-10-05T00:00:00.000Z', op: 'install-tree', write: true, out: 'E:\\a\nb', ok: false, code: 1, ms: 12.7, note: 'x\ty\nz' });
  assert.equal(line.split('\n').length, 1, '一条记录必须占一行');
  assert.match(line, /^2026-10-05T00:00:00\.000Z op=install-tree write=1 out=E:\\a b ok=0 code=1 ms=13 note=x y z$/);
  assert.match(auditLine({ at: 't', op: 'status', ok: true, ms: 0 }), /^t op=status write=0 ok=1 ms=0$/);
});

test('★ 不给 io 时用**真 fs**（回归：默认成"什么都说不存在"的桩会把真路径全判死）', () => {
  assert.equal(realIo.exists(import.meta.filename), true);
  assert.equal(realIo.isDir(path.dirname(import.meta.filename)), true);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'deploy-ops-'));
  try {
    const r = planOp('relabel-medium', { out: dir }, { repoRoot: path.parse(dir).root, allowedRoots: [dir] });
    assert.deepEqual(r.problems, [], '真目录 + 允许的根 ⇒ 不许报"不存在 / 不是目录"');
    assert.equal(r.plan.kind, 'icacls');
    assert.deepEqual(planOp('probe-write', { dir }, { repoRoot: dir, allowedRoots: [dir] }).problems, []);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('cli 前缀是"域 + 动作"两段（透传的 flag 只能跟在后面）', () => {
  assert.ok(OPS['install-tree'].cli.length === 2 && OPS['bake-ui'].cli.length === 2);
});
