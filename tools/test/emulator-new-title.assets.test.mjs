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
const LOGO_ID = 0x5262;
const INIT_ID = 0x5263;

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

/**
 * TITLE 的 `Version 0.00.0000` = 静态 `Version` 词 + 两个静态小数点（绘制键 118/119）
 * + **三条 0x23b** 画的 1/2/4 位数字（绘制键 111..117）。
 * 0x23b 一旦退回"未建模"，这七项就消失、画面只剩 `Version . .` —— 这正是本轮缺陷。
 */
function assertVersionDigits(machine, result) {
  const runs = (result.effects ?? []).filter((e) => e.kind === 'scene.cg.number');
  assert.deepEqual(runs.map((e) => e.detail.items), [1, 2, 4], 'TITLE 的三条 0x23b 必须各画 1/2/4 位');
  assert.deepEqual(runs.map((e) => e.detail.id), [0x6f, 0x70, 0x72], '三条 0x23b 的绘制键起点');
  assert.ok(machine !== null, 'onMachine 必须把实例交出来');
  const digits = [];
  for (let key = 0x6f; key <= 0x75; key += 1) {
    const item = machine.scene.drawItems.get(key);
    assert.ok(item, `0x23b 必须建出绘制项 0x${key.toString(16)}`);
    assert.equal(item.flags & 1, 1, '绘制项必须有"有效"位');
    assert.equal(item.textureId, 4, '数字取自纹理 4（SO004.AGF）');
    digits.push(item);
  }
  // 七位都是补零的 0；源格子来自 0x2da 记录 x0=1356、步长 14。
  for (const item of digits) {
    assert.deepEqual([item.left, item.top, item.right, item.bottom], [0x54c, 0x32b, 0x54c + 12, 0x32b + 15],
      '补零位的源矩形是数字 0 的格子（1356,811,1368,826）');
  }
  // 两个小数点是静态绘制项：源矩形 1342,811,12,15 —— 不是数字格子。
  for (const key of [118, 119]) {
    const period = machine.scene.drawItems.get(key);
    assert.ok(period, `静态小数点绘制项 ${key} 必须存在`);
    assert.deepEqual([period.left, period.top, period.right, period.bottom], [1342, 811, 1354, 826],
      '76/77 的源矩形是小数点；把 1342 当数字 0 是缺陷的成因之一');
  }
}

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
  assert.ok((result.entered ?? []).includes(LOGO_ID), '默认链必须进入 LOGO.BIN（否则 skip 用例的“不含 LOGO”是空断言）');
  assert.ok((result.entered ?? []).includes(INIT_ID), '默认链必须进入 INIT.BIN');
  assert.equal(result.frame?.id, TITLE_ID, '收口时当前帧必须是 TITLE（debug / CLI 的名称来自这里）');
  assert.equal(result.frame?.name, 'TITLE.BIN', '当前帧名称必须解析成 TITLE.BIN');
  assert.equal(result.input?.keyTotal, 12, 'SYSTEM4 的 0xfe c 必须把键总数设成 12');
  assert.ok((result.input?.mouseDispatches ?? 0) > 0, '鼠标回调泵必须真的派发过');
  assert.ok((result.input?.keyDispatches ?? 0) > 0, '按键派发必须执行过');

  const dispatched = (result.effects ?? []).filter((e) => e.kind === 'input.key.dispatch');
  assert.ok(dispatched.some((e) => e.detail.key === 1), `键 1 必须被派发：${JSON.stringify(dispatched.slice(0, 3))}`);

  const unmodeled = new Set((result.effects ?? [])
    .filter((e) => e.kind === 'unmodeled.subsystem')
    .map((e) => e.detail.opcode));
  for (const opcode of [0x341, 0x345, 0x34e, 0x352, 0x2c7]) {
    assert.equal(unmodeled.has(opcode), true, `未建模子系统 opcode 0x${opcode.toString(16)} 必须留下请求记录`);
  }
  assert.equal(unmodeled.has(0x23b), false, '0x23b（Version 数字）已建模，不许再留成未建模请求');
  assert.equal((result.effects ?? []).some((e) => e.kind === 'audio.wav.load'), true, '音频装载请求必须留下记录');
  assert.equal((result.effects ?? []).some((e) => e.kind === 'audio.bgm.play'), true, 'BGM 请求必须留下记录');
  assertVersionDigits(machine, result);
});


test('★ --skip-logo：show-logo 初值 0 ⇒ 启动链跳过 LOGO.BIN，直接到 TITLE', { skip }, async () => {
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
  let titleAt = null;
  const result = await analyzeInstall(env, installDir, 0, {
    key: KEY,
    showLogo: 0,
    inputHost,
    movieHost,
    sleepHost: async () => {},
    includeEffects: true,
    maxSteps: 1_000_000,
    onMachine: (m) => { machine = m; },
    onIteration: () => {
      if (machine === null) return undefined;
      if (!machine.enteredIds.includes(TITLE_ID)) return undefined;
      if (titleAt === null) titleAt = machine.steps;
      if (machine.steps > titleAt + 2000) {
        return { kind: 'input-idle', index: machine.steps, now: machine.now };
      }
      return undefined;
    },
  }, { config: path.join(REPO_ROOT, '.tmp', 'title-skip-logo.ini') });

  assert.equal(result.stop, 'input-idle', `必须收口：${result.stopText}`);
  assert.equal((result.entered ?? []).includes(LOGO_ID), false, 'show-logo=0 ⇒ 不许进入 LOGO.BIN');
  assert.equal((result.entered ?? []).includes(INIT_ID), true, 'show-logo=0 ⇒ 必须走 jcc 的 0 支进入 INIT.BIN');
  assert.equal((result.entered ?? []).includes(TITLE_ID), true, '跳过 LOGO 后仍必须到达 TITLE.BIN');
  assert.equal(result.frame?.name, 'TITLE.BIN', '当前帧仍应解析成 TITLE.BIN');
  assertVersionDigits(machine, result);
});
