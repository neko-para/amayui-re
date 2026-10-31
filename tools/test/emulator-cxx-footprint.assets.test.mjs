/** @env assets @kind contract @why 足迹 dump 不确定、或动态 opcode 不在静态足迹里（"下界 ⊆ 上界"不成立） */
/**
 * tools/test/emulator-cxx-footprint.assets.test.mjs —— 真实足迹产物的契约（要播放器 + 游戏安装）
 *
 * 守三件事：
 * ① **确定性**：同一条 route 跑两次，`--dump-footprint` 的输出**逐字节相同**（没有时间戳 / 墙钟 / 路径）。
 * ② **三张表都有内容**：`op`（动态）、`static-op`（上界）、`cell`；空文件不能当"跑过了"。
 * ③ **下界 ⊆ 上界**：每个真的执行过的 opcode 都必须出现在"这次装载过的脚本"的静态足迹里
 *    （否则静态解码漏脚本 / 漏 opcode，`判不了` 那一桶会失真）。
 *
 * ★ 缺播放器或安装目录 ⇒ `t.skip`（不把"这台机器没跑"记成 pass）。
 * ★ 产物落 `.cache/`（生成物，不入库）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { REPO_ROOT } from '../lib/paths.mjs';
import { DEFAULT_INSTALL, EMULATOR_EXE } from '../routes.mjs';
import { parseFootprint } from '../lib/residual-footprint.mjs';

const exe = path.join(REPO_ROOT, EMULATOR_EXE);
const install = process.env.AMAYUI_INSTALL || DEFAULT_INSTALL;

function runDump(outFile) {
  return spawnSync(
    exe,
    [
      '--install', install,
      '--skip-logo',
      '--play-headless',
      '--input-script', path.join(REPO_ROOT, 'apps', 'emulator-cxx', 'tests', 'input', 'route-title.txt'),
      '--anchor-script', '21092',
      '--expect-script', '21092',
      '--tick-ms', '16',
      '--max-steps', '600000',
      '--dump-footprint', outFile,
    ],
    { cwd: REPO_ROOT, stdio: 'ignore' },
  );
}

test('title route 的足迹：两次逐字节相同、三表非空、动态 ⊆ 静态', (t) => {
  if (!fs.existsSync(exe)) {
    t.skip(`缺播放器 ${EMULATOR_EXE}（先构建 apps/emulator-cxx）`);
    return;
  }
  if (!fs.existsSync(install)) {
    t.skip(`缺游戏安装目录：${install}（用 AMAYUI_INSTALL 覆盖）`);
    return;
  }
  const dir = path.join(REPO_ROOT, '.cache', 'footprint-test');
  fs.mkdirSync(dir, { recursive: true });
  const first = path.join(dir, 'title-1.jsonl');
  const second = path.join(dir, 'title-2.jsonl');

  const a = runDump(first);
  assert.equal(a.error, undefined, `播放器跑不起来：${a.error?.message}`);
  assert.equal(a.status, 0, 'title route 没跑通（--expect-script 没达成）');
  const b = runDump(second);
  assert.equal(b.status, 0, '第二次跑 title route 没跑通');

  const bytesA = fs.readFileSync(first);
  const bytesB = fs.readFileSync(second);
  assert.ok(bytesA.equals(bytesB), '同一条 route 两次 dump 不一致（足迹掺了易变项）');

  const footprint = parseFootprint(bytesA.toString('utf8'), { file: 'title.jsonl' });
  assert.ok(footprint.ops.size > 0, '动态 op 表是空的');
  assert.ok(footprint.staticOps.size > 0, '静态 op 表是空的');
  assert.ok(footprint.cells.size > 0, 'cell 表是空的');
  // 两个写统计：title 上 bits 可能为 0（没置过门位），但 VM 格写一定有。
  assert.ok(footprint.writers.cell.size > 0, 'cell writer 表是空的（VM 格写没有被归因到 opcode）');

  // 写统计里的 opcode 必须是**真的执行过**的（writer 记录只能在 note_op 之后产生）。
  const writerOpcodes = new Set();
  for (const ops of footprint.writers.cell.values()) for (const opcode of ops) writerOpcodes.add(opcode);
  for (const ops of footprint.writers.bit.values()) for (const opcode of ops) writerOpcodes.add(opcode);
  const notExecuted = [...writerOpcodes].filter((opcode) => !footprint.ops.has(opcode));
  assert.deepEqual(notExecuted, [], `writer 记录里有没执行过的 opcode：${notExecuted.map((x) => `0x${x.toString(16)}`).join(',')}`);

  const missing = [...footprint.ops.keys()].filter((opcode) => !footprint.staticOps.has(opcode));
  assert.deepEqual(missing, [], `动态执行过、静态足迹里没有的 opcode：${missing.map((x) => `0x${x.toString(16)}`).join(',')}`);
});
