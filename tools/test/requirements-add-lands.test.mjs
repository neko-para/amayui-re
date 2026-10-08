/** @env pure @kind safety @why `add --write` 报的落盘必须是真的（说落盘就得查得到、没落盘就不许打印落点） */
/**
 * tools/test/requirements-add-lands.test.mjs — **"建单成功了"这句话必须与磁盘一致**
 *
 * 为什么要有它（实测症状）：`pnpm tools requirements add --type requirement … --write`
 * 在 stdout 上打出三行 `add REQ-… / parent: … / → data\requirements\<id>.md`，**盘上却没有文件** ——
 * 那三行是**先于写盘**打印的"计划"，而真实结果是"写进去之后被写后守卫拒回、文件已删除"
 * （理由只走 stderr、退出码 1）。读 stdout 的人会以为建好了 ⇒ 同一张单被丢过两次。
 *
 * 本守卫钉两条不变量（**全程只在系统临时区的临时根上跑**，一个字节都不写进真 `data/requirements/`）：
 *   ① `--write` 退出 0 ⇒ 该节点必须能被**另一条命令**（另起一个进程）查到；
 *   ② `--write` 被写后守卫拒回 ⇒ stdout **只许**有"没有落盘"这一行，⛔ 不许出现落点路径 / 计划。
 *
 * ★ 怎么才会红：把 `tools/requirements.mjs` 的 `main()` 里"计划只在 `apply()` 成功之后才打印"
 *   改回去（或让 `if (res?.rollback)` 不再拒绝 ⇒ 被拒也报"已落盘"）⇒ ② 当场红。
 *   `node tools/mutate-check.mjs --list` 里有对应的那一条。
 *
 * ★ **不起管道**：子进程的 stdout / stderr 重定向到**临时文件**（受限沙箱里 `stdio: 'pipe'` 会 EPERM）。
 * ★ 临时根要有自己的根节点（`--parent null`）：不变量 #2 要求"树恰好一个根、根显式写 null"。
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

import { DEFAULT_REQUIREMENTS_DIR } from '../lib/requirements.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const CLI = path.join(REPO_ROOT, 'tools', 'cli.mjs');

/**
 * 跑一条真实命令（= `pnpm tools requirements …` 走的同一条路：派发器 → CLI → 模型），
 * 两个流分别重定向到临时文件后读回。
 * @returns {{code:number, out:string, err:string}}
 */
function runCli(args) {
  const base = path.join(os.tmpdir(), `amayui-req-run-${process.pid}-${Math.random().toString(36).slice(2)}`);
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

/** 计划第一行打印的新节点 id（`add REQ-…  <标题>`）—— 打算靠它做后续引用 */
const planId = (out) => /^add (REQ-\S+)/m.exec(out)?.[1];

/** 目录快照：文件名 → 内容 sha256（逐字节比较用） */
function snapshotDir(dir) {
  const out = new Map();
  for (const f of fs.readdirSync(dir).sort()) {
    out.set(f, createHash('sha256').update(fs.readFileSync(path.join(dir, f))).digest('hex'));
  }
  return out;
}
const pairs = (m) => [...m];

/** 临时根：先建一个合法根节点（`--parent null`），返回 { dir, rootId, rootOut } */
function withRoot(prefix) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  const root = runCli(['requirements', 'add', '--title', '守卫·临时根', '--type', 'req', '--parent', 'null', '--dir', dir, '--write']);
  assert.equal(root.code, 0, `临时根必须建得出来（stderr：${root.err.trim()}）`);
  return { dir, rootId: planId(root.out), rootOut: root.out };
}

test('★ `add --write` 退出 0 ⇒ 该节点必须能被**另一条命令**查到（临时根）', () => {
  const real = snapshotDir(DEFAULT_REQUIREMENTS_DIR);
  const { dir, rootId } = withRoot('amayui-req-lands-');
  try {
    assert.ok(rootId, '临时根的 id 必须能从 stdout 的计划里读到');

    const child = runCli(['requirements', 'add', '--title', '守卫·临时子', '--type', 'req', '--parent', rootId, '--dir', dir, '--write']);
    assert.equal(child.code, 0, `子节点必须建得出来（stderr：${child.err.trim()}）`);
    const childId = planId(child.out);
    assert.ok(childId, '子节点的 id 必须能从 stdout 的计划里读到');
    assert.ok(fs.existsSync(path.join(dir, `${childId.slice('REQ-'.length)}.md`)), '报"已落盘"就必须真的有那个文件');

    // ★ 判据：**另起一个进程** = "下一条命令"，按完整 id 查
    const show = runCli(['requirements', 'show', childId, '--dir', dir]);
    assert.equal(show.code, 0, `下一条命令必须查得到刚建的节点（stdout：${show.out.trim()}）`);
    assert.match(show.out, /守卫·临时子/, 'show 必须把它印出来');
    const validate = runCli(['requirements', 'validate', '--dir', dir]);
    assert.equal(validate.code, 0, `临时根的树必须全绿：${validate.out.trim()}`);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
  assert.deepEqual(pairs(real), pairs(snapshotDir(DEFAULT_REQUIREMENTS_DIR)), '本文件不许写进真 data/requirements/');
});

test('★ `add --write` 被写后守卫拒回 ⇒ stdout 只许说"没有落盘"，不许出现落点（临时根）', () => {
  const real = snapshotDir(DEFAULT_REQUIREMENTS_DIR);
  const { dir, rootId } = withRoot('amayui-req-refused-');
  try {
    assert.ok(rootId);
    const before = snapshotDir(dir);

    // ① 症状原样：`--type requirement`（枚举是 req | bug | spike | decision）⇒ 不变量 #1 拒回
    const badType = runCli(['requirements', 'add', '--title', '守卫·非法类型', '--type', 'requirement', '--parent', rootId, '--dir', dir, '--write']);
    assert.notEqual(badType.code, 0, '被拒必须是失败退出（报成功却没落盘是最坏的一种）');
    assert.deepEqual(pairs(snapshotDir(dir)), pairs(before), '被拒后临时根必须逐字节回到原样（不许留残 file）');
    assert.match(badType.out, /未落盘/, `stdout 必须明说没有落盘，实际：${JSON.stringify(badType.out)}`);
    assert.ok(!badType.out.includes('.md'), `stdout 不许出现落点/计划（那正是"看起来成功了"的来源）：${JSON.stringify(badType.out)}`);
    assert.match(badType.err, /requirement/, 'stderr 要点名被拒的取值');
    assert.match(badType.err, /应为 req \| bug \| spike \| decision/, '错误信息要指路（枚举写全，用户才知道该填什么）');
    assert.match(badType.err, /→ .*\.md/, '计划不许被吞掉 —— 被拒时它挂在 stderr（"我递了什么参数"仍然看得到）');

    // ② 症状第 1 条：类型合法、但根节点没写 parent ⇒ 不变量 #2 拒回，同样不许在 stdout 上"报成功"
    const noParent = runCli(['requirements', 'add', '--title', '守卫·缺 parent', '--type', 'req', '--dir', dir, '--write']);
    assert.notEqual(noParent.code, 0, '缺 parent 也必须失败退出');
    assert.match(noParent.out, /未落盘/);
    assert.ok(!noParent.out.includes('.md'), `缺 parent 时 stdout 也不许出现落点：${JSON.stringify(noParent.out)}`);
    assert.deepEqual(pairs(snapshotDir(dir)), pairs(before), '被拒后临时根必须逐字节回到原样');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
  assert.deepEqual(pairs(real), pairs(snapshotDir(DEFAULT_REQUIREMENTS_DIR)), '本文件不许写进真 data/requirements/');
});
