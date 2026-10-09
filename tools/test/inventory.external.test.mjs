/** @env external @kind gate @why 真旧仓的盘点事实（HEAD/跟踪数/提交数）取不到或变了 */
/**
 * tools/test/inventory.external.test.mjs — 对**真旧仓**盘点的冒烟（要旧仓在场）
 *
 * ★ 从 `inventory.test.mjs` 拆出来的原因（本轮测试分级）：它读的是**仓库外的只读来源**（旧仓），
 *   属于 `@env external`；混在默认档里会让"这台机器上没跑"与"跑了且绿"长得一样。
 * ★ 顺带修掉一个真·假绿：原写法是 `t.diagnostic(...)` + 裸 `return` —— node:test 把那种写法
 *   记成 **pass**（实测），也就是**旧仓不在场时会绿灯通过**。现在改成 `t.skip()`。
 *
 * 它守的是"盘点这条链真的能跑通"（`pnpm tools old-repo inventory` 的上游）。
 *
 * 运行：`pnpm test:all`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import { collect, defaultOldRepo } from '../old-repo-inventory.mjs';

test('真旧仓可盘点（旧仓不在本机时如实 skip）', (t) => {
  const oldRepo = defaultOldRepo();
  if (!fs.existsSync(oldRepo)) {
    t.skip(`旧仓不在 ${oldRepo}`);
    return;
  }
  const data = collect(oldRepo, { coupling: false });
  assert.match(data.facts.headSha, /^[0-9a-f]{40}$/, 'HEAD 必须是 40 位 sha');
  assert.ok(data.facts.trackedCount > 0, '跟踪文件数应 > 0');
  assert.ok(data.facts.commits > 0, '提交数应 > 0');
});
