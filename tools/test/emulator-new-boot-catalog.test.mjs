/** @env pure @kind gate @why 启动阶段被静默跳过（窗口 / 配置 / 自更新 / 定时器没有处置记录），或某阶段被谎称"已建模" */
/**
 * tools/test/emulator-new-boot-catalog.test.mjs —— 启动阶段记账（保真欠账）的守卫
 *
 * `apps/emulator-new` 从"根脚本 id 0"开始执行，但引擎的启动在脚本之前还有一长串宿主阶段。
 * 这个守卫钉住：**每个阶段都有处置与理由**，且关键阶段的处置不得被悄悄改好。
 *
 * ★ 判据为什么是这些：
 *   - `display-config` 只到 `partial`：配置库已建立，但显示/窗口的宿主副作用没建模；
 *     标成 modeled 会把"设备没读"藏起来，标成 not-modeled 又会否认已有的配置库；
 *   - `index-open` / `script-load` 必须真的是 modeled（否则"真实入口"只是说法）；
 *   - 每个阶段必须有可溯源的 EA（`_WinMain@16` 里的指令起点）。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { BOOT_DISPOSITIONS, BOOT_STAGES, startupReport } from '../../apps/emulator-new/src/boot.ts';

test('★ 启动阶段：id 唯一、处置合法、理由非空、EA 可溯源', () => {
  assert.ok(BOOT_STAGES.length >= 10, `阶段数（实际 ${BOOT_STAGES.length}）`);
  const ids = new Set();
  for (const s of BOOT_STAGES) {
    assert.ok(s.id && !ids.has(s.id), `阶段 id 必须非空且唯一：${s.id}`);
    ids.add(s.id);
    assert.ok(BOOT_DISPOSITIONS.includes(s.disposition), `${s.id} 的处置非法：${s.disposition}`);
    assert.ok(s.what.length > 0, `${s.id} 缺 what`);
    assert.ok(s.why.length > 0, `${s.id} 缺 why（跳过必须给理由）`);
    assert.match(s.anchor, /^0x[0-9a-f]+$/, `${s.id} 的 anchor 必须是 EA`);
  }
});

test('★ 关键阶段的处置：入口/根脚本 = modeled；窗口/定时器 = 宿主；配置 = 部分；引擎/主循环 = 部分', () => {
  const byId = new Map(BOOT_STAGES.map((s) => [s.id, s]));
  assert.equal(byId.get('index-open')?.disposition, 'modeled');
  assert.equal(byId.get('script-load')?.disposition, 'modeled');
  assert.equal(byId.get('window-create')?.disposition, 'host-only');
  assert.equal(byId.get('timer')?.disposition, 'host-only');
  assert.equal(byId.get('self-update')?.disposition, 'host-only');
  assert.equal(byId.get('engine-construct')?.disposition, 'partial');
  assert.equal(byId.get('main-loop')?.disposition, 'partial');
  // ★ 配置：库已建立，但显示设备副作用未建模 ⇒ 只能 partial
  assert.equal(byId.get('display-config')?.disposition, 'partial',
    '配置库已建立（默认表 + 玩家文件）；标成 modeled 会藏起未建模的显示副作用');
});

test('★ 汇总：counts 与阶段一一对应（不手写数字）', () => {
  const r = startupReport();
  assert.equal(r.stages.length, BOOT_STAGES.length);
  const total = Object.values(r.counts).reduce((a, b) => a + b, 0);
  assert.equal(total, BOOT_STAGES.length);
  assert.equal(r.counts.modeled, BOOT_STAGES.filter((s) => s.disposition === 'modeled').length);
  assert.equal(r.counts['not-modeled'], 0, '配置补上后不应再有完全未建模的阶段');
  // 返回的是副本：改它不污染常量
  r.stages[0].disposition = 'modeled';
  assert.notEqual(BOOT_STAGES[0].disposition, 'modeled');
});
