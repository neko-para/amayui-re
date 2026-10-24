/** @env pure @kind safety @why `run` 的"缺 write:true 只出计划 / 任务名闭集合 / 部署方总闸"在**接线层**坏了：宿主会替调用方真的跑构建或起窗口 */
/**
 * plugins/pnpm-priv/test/plugin.test.mjs —— `run` 那一路的**接线**判据
 *
 * 与 `ops.test.mjs` 的分工：那边测 `lib/ops.mjs` 的纯逻辑（io 可注入）；这里测 **`index.mjs` 的
 * execute 真的按 plan 分支** —— 特别是 **dry-run 不启动任何东西**：这条只有跑一遍才看得见
 * （`planOp` 是纯函数，它"没启动"是废话）。
 *
 * ★ 本文件只走 dry-run 与"被拒"两条路 ⇒ **不会** spawn `vite build` / Electron。
 *   踩过的坑：`plugins/deploy/test/plugin.test.mjs` 里清理写在第一个调用之后，
 *   而后面那次"被拒的调用"**也要留痕** ⇒ 文件又造出来、再没人删。所以这里把 `rmSync` 放 `finally`。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { TASK_NAMES } from '../lib/ops.mjs';

const PLUGIN_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const REPO_ROOT = path.resolve(PLUGIN_DIR, '..', '..');
/** build-renderer 的产物（`dist/` 是生成物、已 gitignore ⇒ 干净 clone 上可能不存在） */
const ARTIFACT = path.join(REPO_ROOT, 'apps', 'emulator-new', 'dist', 'renderer', 'index.html');

test('★ `pnpm_priv op=run` 是闭接口：dry-run 一个都不启动、未知任务与被禁的转发直接拒，且走到 execute 的调用都留痕', async () => {
  const mod = await import('../index.mjs');
  assert.equal(mod.name, 'amayui-pnpm-priv');
  assert.equal(typeof mod.apply, 'function');

  const registered = [];
  fs.mkdirSync(path.join(REPO_ROOT, '.tmp'), { recursive: true });
  const tmpDir = fs.mkdtempSync(path.join(REPO_ROOT, '.tmp', 'pnpm-priv-run-'));
  // 传**相对仓库根**的路径（插件按 repoRoot 解析落点，见 lib/audit.mjs）
  const auditFile = path.relative(REPO_ROOT, path.join(tmpDir, 'audit.log'));
  try {
    mod.apply({ tools: { register: (d) => registered.push(d) } }, { auditFile });
    assert.equal(registered.length, 1, 'apply 只注册一个工具');
    const tool = registered[0];
    assert.equal(tool.name, 'pnpm_priv');
    assert.ok(tool.parameters?.properties?.op?.enum?.includes('run'), 'op 枚举里要有 run');
    assert.deepEqual(tool.parameters.properties.task.enum, TASK_NAMES, 'task 的枚举必须就是 TASKS 表（模型连别的名字都给不出）');

    const before = fs.existsSync(ARTIFACT) ? fs.statSync(ARTIFACT).mtimeMs : null;

    // ① dry-run：缺 write:true ⇒ 只出计划。**判据是产物没被碰** —— 计划里写着 build-renderer 的 argv，
    //    所以"这条没启动"不是靠读源码相信的。
    const dry = await tool.execute({ op: 'run', task: 'build-renderer', repoRoot: REPO_ROOT }, {});
    assert.equal(dry.ok, true);
    assert.match(dry.summary, /没有启动任何东西/);
    assert.match(dry.summary, /build:renderer/, '计划里要能看见它**本来会**跑什么');
    if (before !== null) {
      assert.equal(fs.statSync(ARTIFACT).mtimeMs, before, 'dry-run 不许真的构建（产物 mtime 不许变）');
    }

    // ② 未知任务：**schema 那一层**就挡掉（`task` 的 enum 就是 TASKS 表）；
    //    planOp 里还有第二层（见 ops.test.mjs —— 直接调 planOp 能绕过 schema）。
    await assert.rejects(
      () => tool.execute({ op: 'run', task: 'shell', repoRoot: REPO_ROOT, write: true }, {}),
      /must be one of/,
      '未知任务名必须被拒（连 execute 都进不去）',
    );

    // ③ 部署方总闸：allowRun:false ⇒ run 一律拒（换一个 apply 实例，同一个审计文件）
    const strict = [];
    mod.apply({ tools: { register: (d) => strict.push(d) } }, { auditFile, allowRun: false });
    const gated = await strict[0].execute({ op: 'run', task: 'smoke', repoRoot: REPO_ROOT, write: true }, {});
    assert.equal(gated.ok, false);
    assert.match(gated.summary, /allowRun/);

    // ④ 走到 execute 的两次调用各留一行：dry-run 也留痕，且 run 的行带任务名
    //    （被 schema 挡掉的那次不算 —— 它连 execute 都没进，没有动作可记）
    const lines = fs.readFileSync(path.join(REPO_ROOT, auditFile), 'utf8').trim().split('\n');
    assert.equal(lines.length, 2, '两次到达 execute 的调用两行');
    assert.match(lines[0], /op=run /);
    assert.match(lines[0], /write=0 /);
    assert.match(lines[0], /note=run build-renderer/);
    assert.match(lines[1], /ok=0 /);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
});
