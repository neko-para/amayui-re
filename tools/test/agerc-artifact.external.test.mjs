/** @env external @kind product @why AGERC 入库件与旧仓那份不再逐字节相同 */
/**
 * tools/test/agerc-artifact.external.test.mjs —— AGERC 可信产物的**逐字节**比对（要旧仓在场）
 *
 * ★ 从 `agerc-artifact.test.mjs` 里**拆出来**的原因（本轮测试分级）：
 *   那条用例依赖**只读参考仓**（旧仓）在场 ⇒ 它是 `@env external`，不该混在默认档（`pure`）里 ——
 *   否则"这台机器上没跑"与"跑了且绿"在门禁输出里长得一模一样。
 *
 * 它防的是：盘上那份 `corpus/assets/agerc/AGERC.DLL` 被换掉、而清单里的 sha256 常量还恰好对得上。
 * 真实事故先例：有人为测"加壳 vs 不加壳"把旧仓 `install/AGERC.DLL` 换了，当时由 `pnpm tools corpus validate` 报的红。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const ARTIFACT_REL = 'corpus/assets/agerc/AGERC.DLL';
const ARTIFACT = path.join(REPO_ROOT, ARTIFACT_REL);

const manifest = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'corpus', 'assets.json'), 'utf8'));

test('★ 与旧仓那份**逐字节**相同（旧仓不在本机时如实 skip）', (t) => {
  const repo = manifest.roots?.oldRepo;
  if (!repo || !fs.existsSync(repo)) {
    // ★ 必须 `t.skip`：`t.diagnostic` + 裸 `return` 会被 node:test 记成 **pass**（实测）
    t.skip(`旧仓不在 ${repo ?? '(未登记)'}`);
    return;
  }
  const src = path.join(repo, 'patch', 'AGERC.DLL');
  if (!fs.existsSync(src)) {
    t.skip(`旧仓 ${src} 不在`);
    return;
  }
  assert.ok(
    fs.readFileSync(ARTIFACT).equals(fs.readFileSync(src)),
    `${ARTIFACT_REL} 与旧仓 patch/AGERC.DLL 不逐字节相同`,
  );
});
