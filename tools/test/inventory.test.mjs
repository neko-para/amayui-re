/**
 * tools/test/inventory.test.mjs — 旧仓盘点生成器的两条**基建契约**
 *
 * ① **不许静默写出错盘点**：git 事实拿不到时必须**抛错**，而不是"读不到就当空"生成一份全 0 的 md
 *    （实测踩过：受限沙箱里管道的 `spawn EPERM` 被 `catch` 吞掉，把好盘点覆盖成 0 文件/0 提交）。
 * ② **对旧仓严格只读**：`runCapture` 的临时文件写在系统临时区，不往被调用的仓库里写任何东西。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { DEFAULT_OLD_REPO, collect } from '../old-repo-inventory.mjs';

const tmpDir = () => fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-inv-'));

test('★ 非 git 目录 ⇒ collect() 必须抛错（不许静默给 0）', () => {
  const dir = tmpDir();
  assert.throws(() => collect(dir, { coupling: false }), /跑不起来|git/, '拿不到 git 事实时必须抛错');
});

test('★ 盘点生成器不会往被调用的目录里写东西（旧仓只读）', () => {
  const dir = tmpDir();
  const before = fs.readdirSync(dir).sort();
  try {
    collect(dir, { coupling: false });
  } catch {
    /* 预期抛错 */
  }
  assert.deepEqual(fs.readdirSync(dir).sort(), before, '被调用的目录里不许出现任何新文件（含 .tmp/）');
});

test('真旧仓可盘点（旧仓不在本机时如实 skip）', (t) => {
  if (!fs.existsSync(DEFAULT_OLD_REPO)) {
    t.diagnostic(`旧仓不在 ${DEFAULT_OLD_REPO} ⇒ 跳过`);
    return;
  }
  const data = collect(DEFAULT_OLD_REPO, { coupling: false });
  assert.match(data.facts.headSha, /^[0-9a-f]{40}$/, 'HEAD 必须是 40 位 sha');
  assert.ok(data.facts.trackedCount > 0, '跟踪文件数应 > 0');
  assert.ok(data.facts.commits > 0, '提交数应 > 0');
});
