/** @env pure @kind gate @why 体验锁判活/guard 的判据坏了 ⇒ agent 会再次打断用户 */
/**
 * tools/test/experience.test.mjs — `tools/experience.mjs` 的**纯函数**守卫
 *
 * ★ 钉住的是"让路"这件事的四条判据（⛔ 不起真进程、⛔ 不 kill 任何东西）：
 *   ① 缺锁 ⇒ INACTIVE（带 reason）；
 *   ② 坏 JSON / 坏形状 ⇒ INACTIVE（带 reason，⛔ 不许把"读不懂"读成 ACTIVE 以外的意思）；
 *   ③ 锁在但 pid **已死** ⇒ INACTIVE（陈旧锁；用户上次 `_Exit(3)` 就会留下它）；
 *   ④ pid **活着** ⇒ ACTIVE；guard 对 ACTIVE 返回 **2**（构建 / 测试据此让路）；
 *      并验证 `DSH_ALLOW_DURING_USER_RUN=1` 是唯一放行口。
 * ★ 判活用**注入的** `alive`（⛔ 不真杀进程、⛔ 不依赖机器上有没有别的进程）；
 *   锁文件由测试自己在 `mkdtempSync` 的临时目录里建。
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import * as E from '../experience.mjs';

/** 每个测试一个临时目录（锁文件由测试自己建；`t.after` 清理） */
function tmpDir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-experience-test-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function writeLock(dir, value) {
  const file = path.join(dir, E.LOCK_FILE_NAME);
  fs.writeFileSync(file, typeof value === 'string' ? value : JSON.stringify(value, null, 2));
  return file;
}

const neverAlive = () => {
  throw new Error('这条不该判活：缺锁 / 坏锁时 alive() 不应被调用');
};
const quiet = { error: () => {} };

// =====================================================================================
// ① 缺锁 ⇒ inactive（带 reason）
// =====================================================================================

test('缺锁 ⇒ readLock 报 missing、classify ⇒ inactive（带 reason）', (t) => {
  const dir = tmpDir(t);
  const file = path.join(dir, E.LOCK_FILE_NAME);
  const read = E.readLock({ path: file });
  assert.equal(read.ok, false);
  assert.equal(read.state, 'missing');
  assert.match(read.reason, /不存在/);

  const c = E.classifyLock(read, { alive: neverAlive });
  assert.equal(c.active, false);
  assert.equal(c.state, 'missing');
  assert.match(c.reason, /不存在/);
});

test('classifyLock(null) 也按"缺锁"处理（调用方传 null 不许炸 / 不许当 active）', () => {
  const c = E.classifyLock(null, { alive: neverAlive });
  assert.equal(c.active, false);
  assert.equal(c.state, 'missing');
});

// =====================================================================================
// ② 坏 JSON / 坏形状 ⇒ inactive（带 reason）
// =====================================================================================

test('坏 JSON ⇒ inactive，且 reason 点名"不是合法 JSON"', (t) => {
  const dir = tmpDir(t);
  const file = writeLock(dir, '{"pid": 123, "argv": [}');
  const read = E.readLock({ path: file });
  assert.equal(read.ok, false);
  assert.equal(read.state, 'invalid');
  assert.match(read.reason, /JSON/);

  const c = E.classifyLock(read, { alive: neverAlive });
  assert.equal(c.active, false);
  assert.equal(c.state, 'invalid');
  assert.match(c.reason, /JSON/);
});

test('JSON 合法但缺 pid / pid 不是正整数 ⇒ inactive（带 reason）', (t) => {
  const dir = tmpDir(t);
  for (const bad of [{ started_at: '2026-11-27T05:00:00Z' }, { pid: 0 }, { pid: -3 }, { pid: '123' }]) {
    const file = writeLock(dir, bad);
    const c = E.classifyLock(E.readLock({ path: file }), { alive: neverAlive });
    assert.equal(c.active, false, `坏形状不许 active：${JSON.stringify(bad)}`);
    assert.equal(c.state, 'invalid');
    assert.match(c.reason, /pid/);
  }
});

// =====================================================================================
// ③ pid 已死 ⇒ inactive（陈旧锁）
// =====================================================================================

test('pid 已死 ⇒ inactive（stale），且 alive() 收到锁里的 pid', (t) => {
  const dir = tmpDir(t);
  const file = writeLock(dir, {
    pid: 424242,
    started_at: '2026-11-27T05:00:00Z',
    exe: 'out/emulator-lift-aot',
    argv: ['out/emulator-lift-aot', '--user-run'],
  });
  const seen = [];
  const c = E.classifyLock(E.readLock({ path: file }), {
    alive: (pid) => {
      seen.push(pid);
      return false;
    },
  });
  assert.deepEqual(seen, [424242]);
  assert.equal(c.active, false);
  assert.equal(c.state, 'stale');
  assert.match(c.reason, /424242/);
});

// =====================================================================================
// ④ pid 活着 ⇒ active；guard 的退出码
// =====================================================================================

test('pid 活着 ⇒ active（带锁内容）', (t) => {
  const dir = tmpDir(t);
  const lock = {
    pid: process.pid,
    started_at: '2026-11-27T05:00:00Z',
    exe: '/tmp/out/emulator-lift-aot',
    cwd: '/tmp/repo',
    game_dir: '/tmp/game',
    appdata: '/tmp/appdata',
    argv: ['/tmp/out/emulator-lift-aot', '--user-run', '--present', 'headless'],
  };
  const file = writeLock(dir, lock);
  const c = E.classifyLock(E.readLock({ path: file }), { alive: () => true });
  assert.equal(c.active, true);
  assert.equal(c.state, 'active');
  assert.equal(c.pid, process.pid);
  assert.equal(c.lock.exe, lock.exe);
  assert.deepEqual(c.lock.argv, lock.argv);
});

test('真实判活：当前 pid 活；一个不存在的 pid 死（ESRCH）', () => {
  assert.equal(E.pidAlive(process.pid), true);
  assert.equal(E.pidAlive(2147483647), false);
});

test('guard：ACTIVE ⇒ 2（并打印"等用户退出"）；INACTIVE ⇒ 0；DSH_ALLOW_DURING_USER_RUN=1 ⇒ 0', () => {
  const lines = [];
  const log = { error: (s) => lines.push(String(s)) };

  assert.equal(E.guard({ isActive: () => ({ active: false, state: 'missing' }), env: {}, log }), 0);

  const code = E.guard({ isActive: () => ({ active: true, pid: 4242, lock: { started_at: 'x', exe: 'y' } }), env: {}, log });
  assert.equal(code, 2, 'ACTIVE 必须让路（退出码 2）');
  const text = lines.join('\n');
  assert.match(text, /4242/);
  assert.match(text, /等用户退出/);
  assert.match(text, /DSH_ALLOW_DURING_USER_RUN=1/);

  lines.length = 0;
  const allowed = E.guard({
    isActive: () => ({ active: true, pid: 4242 }),
    env: { DSH_ALLOW_DURING_USER_RUN: '1' },
    log,
  });
  assert.equal(allowed, 0, '显式放行必须 0');
  assert.match(lines.join('\n'), /DSH_ALLOW_DURING_USER_RUN=1/);
});

// =====================================================================================
// ⑤ isExperienceActive：锁优先，锁不可用才扫命令行（兜底）
// =====================================================================================

test('isExperienceActive：锁 ACTIVE ⇒ 来源=lock，且**不**再扫命令行', (t) => {
  const dir = tmpDir(t);
  const file = writeLock(dir, { pid: 777, started_at: 'x', argv: ['a', '--user-run'] });
  let scanned = 0;
  const r = E.isExperienceActive({
    lockOptions: { path: file },
    alive: () => true,
    scan: () => {
      scanned += 1;
      return { ok: true, pids: [{ pid: 1, command: 'unexpected' }], error: null };
    },
  });
  assert.equal(r.active, true);
  assert.equal(r.source, 'lock');
  assert.equal(scanned, 0);
});

test('isExperienceActive：锁缺失但命令行里有 --user-run ⇒ ACTIVE（来源=process-scan）', (t) => {
  const dir = tmpDir(t);
  const r = E.isExperienceActive({
    lockOptions: { path: path.join(dir, E.LOCK_FILE_NAME) },
    alive: () => true,
    scan: () => ({ ok: true, pids: [{ pid: 90210, command: 'emulator-lift-aot --user-run' }], error: null }),
  });
  assert.equal(r.active, true);
  assert.equal(r.source, 'process-scan');
  assert.equal(r.pid, 90210);
});

test('isExperienceActive：锁死 + 命令行也没命中 ⇒ INACTIVE（陈旧锁），带扫描状态', (t) => {
  const dir = tmpDir(t);
  const file = writeLock(dir, { pid: 424242 });
  const r = E.isExperienceActive({
    lockOptions: { path: file },
    alive: () => false,
    scan: () => ({ ok: true, pids: [], error: null }),
  });
  assert.equal(r.active, false);
  assert.equal(r.state, 'stale');
  assert.deepEqual(r.scan, { ok: true, error: null });
});
