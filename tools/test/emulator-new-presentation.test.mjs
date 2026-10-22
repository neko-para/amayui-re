/** @env pure @kind contract @why 模拟器明确不支持全屏：占满屏请求必须降级成 1280×720 窗口，否则画面会按 1728×1084 画布 1:1 贴左上角 */
/**
 * tools/test/emulator-new-presentation.test.mjs —— 全屏降级策略守卫
 *
 * 模拟器产品决策（不是 spec 结论）：`screenMode=0` 的“占满屏”请求不实现显示器模式切换，
 * 平台把 `exStyle != 0` 的全屏参数改成 `set:WinX/Y` 的普通窗口。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { applyPresentationPolicy } from '../../apps/emulator-new/src/platform/electron/renderer/presentation.ts';

const POLICY = { ignoreFullscreen: true, width: 1280, height: 720 };

function fullscreenParams() {
  return {
    className: 'ARCGameEngine',
    title: '',
    instance: 1,
    parent: null,
    param: null,
    x: 0,
    y: 0,
    width: 1728,
    height: 1117,
    clientWidth: 1728,
    clientHeight: 1117,
    style: 0x90000000,
    exStyle: 0x8,
    menuResourceId: null,
  };
}

test('★ 占满屏请求被降级成 1280×720 窗口（exStyle 清 0，使用平台默认位置）', () => {
  const out = applyPresentationPolicy(fullscreenParams(), POLICY);
  assert.equal(out.exStyle, 0, 'main 用 exStyle 判 fullscreen ⇒ 必须清 0');
  assert.equal(out.clientWidth, 1280);
  assert.equal(out.clientHeight, 720);
  assert.equal(out.width, 1280);
  assert.equal(out.height, 720);
  assert.equal(out.x >>> 0, 0x80000000, '不替窗口编位置，交平台默认位置');
  assert.equal(out.y >>> 0, 0x80000000);
  assert.equal(out.style, 0x90000000, '风格字原样保留（平台不看它判全屏）');
});

test('★ 窗口态参数原样返回（降级只作用于占满屏）', () => {
  const windowed = { ...fullscreenParams(), exStyle: 0, width: 1280, height: 720 + 60 };
  assert.equal(applyPresentationPolicy(windowed, POLICY), windowed, '窗口态对象必须原样返回');
});

test('★ 显式关闭降级时全屏参数不被改写（测试 / 未来平台可复用）', () => {
  const params = fullscreenParams();
  assert.equal(applyPresentationPolicy(params, { ...POLICY, ignoreFullscreen: false }), params);
});
