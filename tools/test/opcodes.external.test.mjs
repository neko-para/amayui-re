/** @env external @kind gate @why 旧表来源被换过（sha256 与登记不符 ⇒ 派生链的证据变了） */
/**
 * tools/test/opcodes.external.test.mjs — 指令表**来源**的身份复核（要旧仓在场）
 *
 * ★ 从 `opcodes.test.mjs` 拆出来的原因（本轮测试分级）：它按清单条目 `knowledge/opcode-table-source`
 *   解析出旧仓里的 `scripts/asm/opcodes.json` 并现算 sha256 ⇒ 依赖**只读参考仓**在场。
 * ★ 顺带修掉一个**无痕假绿**：原写法是**裸 `return`**（连 `t.diagnostic` 都没有）——
 *   node:test 把它记成 **pass**（实测），也就是"来源不在场"时会绿灯通过。
 *
 * 它守的是"派生表的上游证据没被换过"：`pnpm tools opcodes derive` 的凭据就是这条 sha256。
 *
 * 运行：`pnpm test:all`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_MANIFEST } from '../lib/manifest.mjs';

test('④ 来源在场时，它的 sha256 必须与登记一致（来源被换过 ⇒ 红）', async (t) => {
  const manifest = JSON.parse(fs.readFileSync(DEFAULT_MANIFEST, 'utf8'));
  const entry = manifest.entries.find((e) => e.id === 'knowledge/opcode-table-source');
  const origin = (entry.origin ?? [])[0];
  const abs = path.join(manifest.roots[origin.root], origin.path);
  if (!fs.existsSync(abs)) {
    // 旧仓是每台机器不同的绝对路径：不在场就跳过（与 age-format 的样本同一口径）
    // ★ 必须 `t.skip`：裸 `return` 会被 node:test 记成 pass（实测）
    t.skip(`来源不在场：${abs}`);
    return;
  }
  const got = createHash('sha256').update(fs.readFileSync(abs)).digest('hex');
  assert.equal(got, origin.sha256, '来源文件的 sha256 与登记不符 ⇒ 派生链的证据变了');
});
