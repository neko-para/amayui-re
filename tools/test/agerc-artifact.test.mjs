/** @env assets @kind product @why AGERC 可信产物的身份（sha256 与清单基准不符） */
/**
 * tools/test/agerc-artifact.test.mjs —— **AGERC.DLL 可信产物的身份守卫**
 *
 * 为什么需要它：`corpus/assets/agerc/AGERC.DLL` 是**暂时按"可信产物"使用**的发布件
 * （本仓还没有自建链 —— 见 `release/README.md` §3/§5.2 与需求树）。
 * 它的"可信"必须**可机械复核**，否则它只是一个来路不明的二进制。
 *
 * ★ 判据**不另写一份**：基准取清单里 `binary/agerc-modified-install` 的 `origin[].sha256`
 *   —— 那条 `external-only` 条目的 sha256 正是"旧仓那一份长这样"的唯一证据。
 *   一处真源 ⇒ 不存在"第二个 sha 会不会漂"的问题。
 *
 * ★ **拆过一刀**（本轮测试分级）：原先还有一条"与旧仓逐字节比对"的用例，它依赖**旧仓在场** ⇒
 *   已移到 `agerc-artifact.external.test.mjs`（`@env external`）。本文件只剩**不需要旧仓**的那条：
 *   它读的是**已入库**的 `corpus/assets/agerc/AGERC.DLL` 与清单基准（`@env assets`）。
 *
 * 运行：`pnpm test:assets`（或 `pnpm test:all`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const ARTIFACT_REL = 'corpus/assets/agerc/AGERC.DLL';
const ARTIFACT = path.join(REPO_ROOT, ARTIFACT_REL);

const manifest = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus', 'assets.json'), 'utf8'));
const entry = manifest.entries.find((e) => e.id === 'binary/agerc-dist');
const baselineEntry = manifest.entries.find((e) => e.id === 'binary/agerc-modified-install');

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

test('可信产物已入库，且**sha256 与清单里那条 external-only 记录一致**', () => {
  assert.ok(fs.existsSync(ARTIFACT), `${ARTIFACT_REL} 不存在（可信产物是发布件，不能缺席）`);
  assert.ok(entry, '清单里应有 binary/agerc-dist 条目');
  assert.ok(baselineEntry?.origin?.[0]?.sha256, '基准缺失：binary/agerc-modified-install 应记着旧仓那一份的 sha256');

  const want = baselineEntry.origin[0].sha256;
  const got = sha256(fs.readFileSync(ARTIFACT));
  assert.equal(got, want, `${ARTIFACT_REL} 的 sha256 与清单记录的基准不符（入库件被换掉了？）`);
});
