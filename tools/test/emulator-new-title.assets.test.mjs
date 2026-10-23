/** @env assets @kind product @why 真实启动链到不了 TITLE 输入环，或到了却不派发按键回调（音频/ADV/Live2D 跳过把它带坏了），而合成脚本守卫仍然全绿 */
/**
 * tools/test/emulator-new-title.assets.test.mjs —— **真实安装 → TITLE 输入环** 的守卫
 *
 * 判据：
 * 1. 从 SYSTEM4.BIN（id 0）走完整启动链，进入 TITLE.BIN（id 0x5264）；
 * 2. 音频 / ADV 文字 / Live2D 的未建模 opcode 只记请求并推进（不抛、不卡死）；
 * 3. 脚本化输入能驱动按键回调：`0x100` 派发键 1、回调写完返回，主循环继续；
 * 4. 输入脚本耗尽 + 空闲预算后以 `input-idle` 收口（CLI 的显式仿真边界）。
 *
 * ★ 样本缺席 ⇒ skip，不是红。
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';
import { rootFromManifest } from '../lib/patch.mjs';
import { analyzeInstall } from '../../apps/emulator-new/src/core/runner.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';
import { loadConfigKeys, loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { parseInputScript, ScriptedInputHost } from '../../apps/emulator-new/src/platform/node/input.ts';

const KEY = 0x1234abcd;
const TITLE_ID = 0x5264;

const manifest = loadManifest();
let installDir = null;
try {
  installDir = rootFromManifest(manifest, 'gameInstall');
} catch {
  installDir = null;
}
const skip = installDir && fs.existsSync(path.join(installDir, 'SYS4INI.BIN'))
  ? false
  : '游戏安装不在场（corpus/assets.json 的 roots.gameInstall 下没有 SYS4INI.BIN）';

const env = { host: nodeHost, table: await loadOpcodeTable(), configKeys: await loadConfigKeys() };

test('★ TITLE 输入环：真实启动链 → TITLE，键 1 派发回调并 input-idle 收口', { skip }, async () => {
  const inputHost = new ScriptedInputHost(parseInputScript('8000 key 1 down; 8100 key 1 up'));
  const handles = new Set();
  const movieHost = {
    open: () => { const handle = {}; handles.add(handle); return handle; },
    play: () => {},
    poll: () => ({ currentTime: 1, stopTime: 1, ended: true }),
    rewind: () => {},
    close: (handle) => { handles.delete(handle); },
    fillTexture: () => {},
  };

  let machine = null;
  let loopStartedAt = null;
  const result = await analyzeInstall(env, installDir, 0, {
    key: KEY,
    inputHost,
    movieHost,
    sleepHost: async () => {},
    includeEffects: true,
    maxSteps: 1_000_000,
    onMachine: (m) => { machine = m; },
    onIteration: () => {
      if (machine === null) return undefined;
      if ((machine.input.mouseDispatches > 0 || machine.input.keyDispatches > 0) && loopStartedAt === null) {
        loopStartedAt = machine.now;
      }
      if (loopStartedAt !== null && inputHost.exhausted) {
        const idleFrom = Math.max(inputHost.appliedUntil, loopStartedAt);
        if (machine.now - idleFrom >= 500) return { kind: 'input-idle', index: machine.steps, now: machine.now };
      }
      return undefined;
    },
  }, { config: path.join(REPO_ROOT, '.tmp', 'title-input-missing.ini') });

  assert.equal(result.stop, 'input-idle', `必须以 input-idle 收口：${result.stopText}`);
  assert.ok((result.entered ?? []).includes(TITLE_ID), '启动链必须进入 TITLE.BIN');
  assert.equal(result.input?.keyTotal, 12, 'SYSTEM4 的 0xfe c 必须把键总数设成 12');
  assert.ok((result.input?.mouseDispatches ?? 0) > 0, '鼠标回调泵必须真的派发过');
  assert.ok((result.input?.keyDispatches ?? 0) > 0, '按键派发必须执行过');

  const dispatched = (result.effects ?? []).filter((e) => e.kind === 'input.key.dispatch');
  assert.ok(dispatched.some((e) => e.detail.key === 1), `键 1 必须被派发：${JSON.stringify(dispatched.slice(0, 3))}`);

  const unmodeled = new Set((result.effects ?? [])
    .filter((e) => e.kind === 'unmodeled.subsystem')
    .map((e) => e.detail.opcode));
  for (const opcode of [0x341, 0x345, 0x34e, 0x352, 0x2c7, 0x23b]) {
    assert.equal(unmodeled.has(opcode), true, `未建模子系统 opcode 0x${opcode.toString(16)} 必须留下请求记录`);
  }
  assert.equal((result.effects ?? []).some((e) => e.kind === 'audio.wav.load'), true, '音频装载请求必须留下记录');
  assert.equal((result.effects ?? []).some((e) => e.kind === 'audio.bgm.play'), true, 'BGM 请求必须留下记录');
});
