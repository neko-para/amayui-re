/** @env pure @kind gate @suite toolchain @why `--list` 的筛选（`--live` / `--status`）静默失效或筛错时要红 */
/**
 * tools/test/requirements-cli.test.mjs —— `--list` 查询面的 CLI 契约
 *
 * 只测两件事：
 *   ① `--live` / `--status a,b` 真的**筛掉了**不该出现的状态（与全量基线对账，不写死节点数）；
 *   ② 用错动作 / 写错状态时**响亮报错**（⛔ 不许像旧版那样"静默全量"）。
 *
 * 不做：spawn 子进程（受限沙箱里捕获输出会 EPERM）；直接调导出的 `main`，临时接管 stdout。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { main } from '../requirements.mjs';
import { LIVE_STATUSES } from '../lib/requirements.mjs';

/** 跑一次 `main`，把 stdout 收成字符串。 */
function run(argv) {
  const chunks = [];
  const original = process.stdout.write;
  process.stdout.write = (chunk) => {
    chunks.push(String(chunk));
    return true;
  };
  try {
    const code = main(argv);
    return { code, text: chunks.join('') };
  } finally {
    process.stdout.write = original;
  }
}

const listJson = (argv) => JSON.parse(run([...argv, '--json']).text);

test('--list --live 只返回活节点，且真的少于全量', () => {
  const all = listJson(['--list']);
  const live = listJson(['--list', '--live']);
  const expected = all.filter((r) => LIVE_STATUSES.includes(r.status));
  assert.equal(live.length, expected.length);
  assert.ok(live.length > 0, '全库应当至少有一个活节点');
  assert.ok(live.length < all.length, '--live 必须筛掉 done / dropped，不能等于全量');
  assert.ok(live.every((r) => LIVE_STATUSES.includes(r.status)));
});

test('--list --status done 只返回 done（数量与全量里的 done 对账）', () => {
  const all = listJson(['--list']);
  const done = listJson(['--list', '--status', 'done']);
  assert.equal(done.length, all.filter((r) => r.status === 'done').length);
  assert.ok(done.every((r) => r.status === 'done'));
});

test('--list --status open,doing 按显式列表筛选', () => {
  const all = listJson(['--list']);
  const mix = listJson(['--list', '--status', 'open,doing']);
  assert.equal(mix.length, all.filter((r) => ['open', 'doing'].includes(r.status)).length);
  assert.ok(mix.every((r) => ['open', 'doing'].includes(r.status)));
});

test('文本摘要写明筛选口径与全库规模', () => {
  const { code, text } = run(['--list', '--live']);
  assert.equal(code, 0);
  assert.match(text, /筛选：--live/);
  assert.match(text, /全库 \d+/);
});

test('错误用法响亮报错（不许静默全量）', () => {
  assert.throws(() => main(['--list', '--status', 'nope']), /不认识的状态/);
  assert.throws(() => main(['--list', '--status']), /需要逗号分隔的状态/);
  assert.throws(() => main(['--list', '--live', '--status', 'open']), /只能二选一/);
  assert.throws(() => main(['--plan', '--status', 'open']), /只用于 --list/);
  assert.throws(() => main(['--plan', '--live']), /只用于 --list/);
});
