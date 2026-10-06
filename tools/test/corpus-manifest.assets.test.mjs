/** @env assets @kind gate @why 真实 corpus/assets.json 过不了全部断言（含 git 检查/哈希复核/recipe） */
/**
 * tools/test/corpus-manifest.assets.test.mjs — **端到端**：真实清单必须全绿
 *
 * ★ 从 `corpus-manifest.test.mjs` 拆出来的原因（本轮测试分级）：
 *   这两条读的是**真实 `corpus/assets.json`**，而它要求 47 个条目的 `dest`/`origin` **都在盘上**
 *   —— 也就是**装了游戏 / LFS 已 smudge**。实测（把 `gameInstall` 临时指空）：本文件整体会**红 2 条**，
 *   而 `corpus-manifest.test.mjs` 里那 27 条**合成清单**的用例照样全绿
 *   ⇒ 两类混在一个文件里，等于让"纯路径的守卫"跟着"这台机器装没装游戏"一起红。
 *
 * 运行：`pnpm test:assets`（或 `pnpm test:all`）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

import { DEFAULT_MANIFEST, loadManifest, validateManifest } from '../lib/manifest.mjs';
import { REPO_ROOT } from '../lib/paths.mjs';

const CORPUS_MJS = path.join(REPO_ROOT, 'tools', 'corpus.mjs');

// ★ 不捕获子进程输出（`stdio: 'pipe'` 在受限沙箱里会 `spawn EPERM`）⇒ 用 `ignore` + 退出码。
test('端到端：真实 corpus/assets.json 过全部断言（含 git 检查、哈希复核、真跑 recipe）', () => {
  assert.ok(fs.existsSync(DEFAULT_MANIFEST), 'corpus/assets.json 必须存在');
  const report = validateManifest(loadManifest(DEFAULT_MANIFEST), { repoRoot: REPO_ROOT });
  const bad = report.checks.filter((c) => c.status === 'fail').map((c) => `#${c.id} ${c.title}：${c.message}`);
  assert.deepEqual(bad, [], `守卫必须全绿，实际：\n${bad.join('\n')}`);
  assert.equal(report.failures, 0);
});

test('端到端：CLI `--validate` 退出码 0（外壳与退出码，不捕获输出）', () => {
  assert.doesNotThrow(() => {
    execFileSync(process.execPath, [CORPUS_MJS, '--validate'], { cwd: REPO_ROOT, stdio: 'ignore' });
  }, 'pnpm tools corpus validate 必须退出 0');
});
