/** @env pure @kind gate @suite toolchain @why planned[].mechanism 与机制表的闭集漂了（planned 指向表里没有的机制 / 漏写归属） */
/**
 * tools/test/spec-mechanisms.test.mjs —— **机制表 ↔ audit.planned** 的机械对齐
 *
 * 判据（两条都机械）：
 * 1. `core/README.md` §2 机制表里的 id 集合，就是 `mechanism` 的合法闭集（外加 `core-out`）；
 * 2. `audit.planned[]` 的每一项都必须写 `mechanism`（`core-out` = core 外）。
 *
 * 红 = planned 指了不存在的机制 / planned 漏写归属。
 * ⛔ 这不是"覆盖完成度"判据：需求单可以还 open。
 * ★ **机制 ↔ 需求单的映射不在这条守卫的范围里**（2026-11 用户裁决：spec 机制与需求台账不应耦合）。
 *   机制表加了行**不要求**在需求树里补一张单；这里也不检查"某个机制有没有单在跟踪"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';

const CORE = path.join(REPO_ROOT, 'data', 'spec', 'core', 'README.md');
const AUDIT = path.join(REPO_ROOT, 'data', 'spec', 'audit.json');

function mechanismIds() {
  const md = fs.readFileSync(CORE, 'utf8');
  const table = md.split('## 2. 机制表')[1]?.split('### 2.1')[0] ?? '';
  const ids = [...table.matchAll(/^\|\s*`([A-Z][A-Z0-9-]+)`\s*\|/gm)].map((m) => m[1]);
  return [...new Set(ids)];
}

function planned() {
  return JSON.parse(fs.readFileSync(AUDIT, 'utf8')).planned ?? [];
}

test('★ 机制表是闭集：planned[].mechanism 只能是表里的 id 或 core-out', () => {
  const ids = new Set(mechanismIds());
  assert.ok(ids.size >= 10, `机制表解析出的 id 太少（${ids.size}）—— 表形态变了？`);
  const bad = [];
  for (const p of planned()) {
    if (p.mechanism === undefined) bad.push(`${p.id} 没写 mechanism`);
    else if (p.mechanism !== 'core-out' && !ids.has(p.mechanism)) bad.push(`${p.id} → ${p.mechanism}（不在机制表）`);
  }
  assert.deepEqual(bad, [], `planned 的机制归属与机制表不一致：\n  - ${bad.join('\n  - ')}`);
});

test('★ 判别力：机制归属写错（表里没有的 id）会被点名', () => {
  // 用同一判据跑一份"坏 planned"的内存副本：证明上面那条不是恒真。
  const ids = new Set(mechanismIds());
  const check = (rows) =>
    rows
      .filter((p) => p.mechanism === undefined || (p.mechanism !== 'core-out' && !ids.has(p.mechanism)))
      .map((p) => p.id);
  assert.deepEqual(check([{ id: 'x', mechanism: 'CORE-NOT-A-MECHANISM' }]), ['x'], '不存在的机制 id 必须被点名');
  assert.deepEqual(check([{ id: 'y' }]), ['y'], '漏写 mechanism 必须被点名');
  assert.deepEqual(check([{ id: 'z', mechanism: 'core-out' }]), [], 'core-out 是合法值');
});
