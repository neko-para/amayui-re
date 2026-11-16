/** @env pure @kind gate @suite toolchain @why `buildTarget` 没有真的传给 cmake（单条变异又变成"重链全仓 exe"）；或收尾那次全量重建被 `--target` 污染 / 根本没做（`out/` 里留着变异版 exe ⇒ 之后拿它测 A/B 是静默错） */
/**
 * tools/test/mutate-build-target.test.mjs —— `tools/mutate-check.mjs` 的 **`buildTarget` 与收尾全量重建**守卫
 *
 * 判据（用例名就是下面这些小写字母）：
 *   a. **带字段 ⇒ 传 `--target <名>`**：`runCtestGuard` 收到的构建调用必须把 `{target}` 透传给
 *      `cmakeBuildArgs` 的构造点（`--build <dir> --target <名>`）。
 *   b. **不带字段 ⇒ 没有 `--target`**：缺省行为**不变**（全量构建）。
 *   c. **收尾那次是全量**：`finalizeBuildDirs` 的构建调用**不带** `target`
 *      （否则"回到基线"就成了"只把守卫那个 target 回到基线"，其它 exe 留在变异版）。
 *   d. `cmakeBuildArgs` 是**唯一**构造 argv 的地方（用纯函数断言 argv 形状，不 spawn cmake）。
 *
 * ★ 用**注入的假 build / 假 ctest**（`opts.build` / `opts.command`，与 `mutate-timeout.test.mjs`
 *   同一套注入点）⇒ ⛔ 本文件不编任何 C++、不碰真实构建目录、只写 `.tmp/`（gitignored）。
 */
import { after, before, test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { cmakeBuildArgs, finalizeBuildDirs, runCtestGuard } from '../mutate-check.mjs';

const TMP = path.join(process.cwd(), '.tmp', 'mutate-build-target-test');
/** 假构建目录（`runCtestGuard` / `finalizeBuildDirs` 都要求 `CMakeCache.txt` 在场） */
const BUILD_DIR = 'fake-out';

before(() => {
  fs.rmSync(TMP, { recursive: true, force: true });
  fs.mkdirSync(path.join(TMP, BUILD_DIR), { recursive: true });
  fs.writeFileSync(path.join(TMP, BUILD_DIR, 'CMakeCache.txt'), '# fake\n', 'utf8');
});

after(() => {
  fs.rmSync(TMP, { recursive: true, force: true });
});

/** 记录每次构建调用；返回成功（与 `spawnSync` 的返回形状一致） */
function recordingBuild() {
  const calls = [];
  const build = (logFd, dir, opts = {}) => {
    calls.push({ dir, opts });
    return { status: 0 };
  };
  build.calls = calls;
  return build;
}

test('a) 带 buildTarget ⇒ 构建调用收到 target（argv 里出现 --target）', async () => {
  const build = recordingBuild();
  const r = await runCtestGuard('aot-window-profile', null, BUILD_DIR, {
    root: TMP,
    build,
    buildTarget: 'lift-aot-window-profile',
    // 假 ctest：立刻 exit 0（不编 C++、不碰真 ctest）
    command: process.execPath,
    args: ['-e', '0'],
  });
  assert.equal(r.status, 0);
  assert.equal(build.calls.length, 1, '这次守卫应当恰好触发一次构建');
  assert.deepEqual(build.calls[0].opts, { target: 'lift-aot-window-profile' });
  // argv 形状（d 的机械证据）：`--target` 与名字是**相邻的两个 argv**
  assert.deepEqual(cmakeBuildArgs(BUILD_DIR, 'lift-aot-window-profile'), [
    '--build',
    BUILD_DIR,
    '--target',
    'lift-aot-window-profile',
  ]);
});

test('b) 不带 buildTarget ⇒ 没有 --target（缺省行为不变）', async () => {
  const build = recordingBuild();
  const r = await runCtestGuard('aot-window-profile', null, BUILD_DIR, {
    root: TMP,
    build,
    command: process.execPath,
    args: ['-e', '0'],
  });
  assert.equal(r.status, 0);
  assert.equal(build.calls.length, 1);
  assert.equal(build.calls[0].opts.target, null, '缺省必须是 null（= 全量）');
  assert.deepEqual(cmakeBuildArgs(BUILD_DIR, null), ['--build', BUILD_DIR]);
});

test('c) 收尾的全量重建⇒ 构建调用不带 target', () => {
  const build = recordingBuild();
  const fin = finalizeBuildDirs(null, [BUILD_DIR], { root: TMP, build });
  assert.equal(fin.ok, true);
  assert.deepEqual(fin.dirs, [BUILD_DIR]);
  assert.equal(build.calls.length, 1);
  // ⛔ 收尾那次**不许**带 target（带 target 就把"回到基线"缩成了"守卫那个 target 回到基线"）
  assert.equal('target' in build.calls[0].opts, false, 'finalize 的构建 opts 里不许出现 target 键');
});

test('d) 收尾重建失败 ⇒ 必须报失败（不许 exit 0 掩盖陈旧 exe）', () => {
  const build = () => ({ status: 1 });
  const fin = finalizeBuildDirs(null, [BUILD_DIR], { root: TMP, build });
  assert.equal(fin.ok, false);
  assert.equal(fin.failedDir, BUILD_DIR);
  assert.match(String(fin.why), /退出码 1/);
});

test('e) 没配置的构建目录不进收尾（那是"本机跑不了"，不是"要重建"）', () => {
  const build = recordingBuild();
  const fin = finalizeBuildDirs(null, ['no-such-dir'], { root: TMP, build });
  assert.equal(fin.ok, true);
  assert.deepEqual(fin.dirs, []);
  assert.equal(build.calls.length, 0);
});
