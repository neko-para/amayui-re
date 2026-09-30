/**
 * tools/test/plan.test.mjs — `PLAN.md`（多轮迁移的进度表）的三条契约
 *
 * `PLAN.md` 是本仓**唯一允许手写状态**的文件（`docs/00-origin/decisions.md` §6 的例外），
 * 因此它必须一直是"**批次级的进度表**"，而不是又一份会漂的文档：
 *   ① 覆盖全部批次（新增批次不能漏进计划）；
 *   ② 每个批次行必须明写四种状态之一（读得出来"到哪了"）；
 *   ③ **不落细节**：不许出现哈希、不许长段落、不许膨胀。
 *
 * 运行：`pnpm test`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const PLAN = path.join(REPO_ROOT, 'PLAN.md');
const BATCHES = ['M0', 'M1', 'M1b', 'M2', 'M3', 'M4', 'M5', 'M6', 'M7', 'M8', 'K1', 'K2', 'K3'];
const STATUSES = ['未开始', '进行中', '阻塞', '已完成'];

const lines = () => fs.readFileSync(PLAN, 'utf8').split('\n');

test('① PLAN.md 存在，且覆盖全部批次（每个批次一行）', () => {
  assert.ok(fs.existsSync(PLAN), 'PLAN.md 必须在仓库根');
  const text = fs.readFileSync(PLAN, 'utf8');
  const missing = BATCHES.filter((b) => !new RegExp(`\\|\\s*${b}\\s*\\|`).test(text));
  assert.deepEqual(missing, [], `PLAN.md 里缺这些批次：${missing.join(', ')}`);
});

test('② 每个批次行都明写四种状态之一', () => {
  const bad = [];
  for (const b of BATCHES) {
    const line = lines().find((l) => new RegExp(`\\|\\s*${b}\\s*\\|`).test(l));
    if (!line) continue; // ① 已报
    if (!STATUSES.some((s) => line.includes(s))) bad.push(`${b}: ${line.trim()}`);
  }
  assert.deepEqual(bad, [], `批次行没有可读状态：\n  - ${bad.join('\n  - ')}`);
});

test('③ 不落细节：无哈希、无长段落、整体保持简短', () => {
  const ls = lines();
  const hashes = ls.filter((l) => /\b[0-9a-f]{40,}\b/.test(l));
  assert.deepEqual(hashes, [], `PLAN.md 里不该出现哈希：\n  - ${hashes.join('\n  - ')}`);

  const long = ls.map((l, i) => [i + 1, l]).filter(([, l]) => l.length > 220);
  assert.deepEqual(long.map(([n]) => n), [], `这些行太长（细节应移到域文档）：${long.map(([n]) => n).join(', ')}`);

  assert.ok(ls.length <= 90, `PLAN.md 应保持简短（当前 ${ls.length} 行；细节请写进域文档）`);
});
