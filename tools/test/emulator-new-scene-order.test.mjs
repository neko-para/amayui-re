/** @env pure @kind contract @why 场景绘制顺序退回"先所有绘制项、再所有网格"会让 TITLE 的 key 0 黑色背景盖住前景（整屏黑），而 LOGO 的影片覆盖层仍可能正常 —— 只有合成顺序的守卫才会红 */
/**
 * tools/test/emulator-new-scene-order.test.mjs —— 场景合成顺序（**引擎行为；spec 待成篇**）
 *
 * 判据是 spec 的 `CORE-SCENE-DRAW-ORDER`（无命令记录时）：TITLE 用 key 0 的全屏黑网格当背景、
 * key 10/100/… 的绘制项当前景、key 200000 的网格当覆盖层；两类对象按 **key 升序**放在同一条序列里。
 * 同键时**网格先于绘制项**（绘制项后发出、压在上面）。
 * ★ 有场景命令记录时的两档顺序仍未成篇；这个守卫只覆盖无命令记录的帧。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { orderedDrawables } from '../../apps/emulator-new/src/platform/electron/renderer/scene-order.ts';

const item = (key) => ({ key });
const mesh = (key) => ({ key });

test('★ TITLE 场景：key 0 黑色背景网格先于前景绘制项，key 200000 覆盖层最后', () => {
  const scene = {
    drawItems: [item(10), item(100), item(200)],
    meshes: [mesh(200000), mesh(0)],
  };
  assert.deepEqual(
    orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`),
    ['mesh#0', 'item#10', 'item#100', 'item#200', 'mesh#200000'],
    '按 key 升序混合两类对象；否则整屏黑色网格会盖住 TITLE 前景',
  );
});

test('★ LOGO 场景：影片绘制项 200000 仍在 200002/200003 网格之前（影片槽最后另画）', () => {
  const scene = {
    drawItems: [item(200000)],
    meshes: [mesh(200002), mesh(200003)],
  };
  assert.deepEqual(
    orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`),
    ['item#200000', 'mesh#200002', 'mesh#200003'],
  );
});

test('负键条目不参与绘制', () => {
  const scene = {
    drawItems: [item(-1), item(5)],
    meshes: [mesh(-2), mesh(3)],
  };
  assert.deepEqual(
    orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`),
    ['mesh#3', 'item#5'],
  );
});

test('同 key：网格先于绘制项，且各自保持表内顺序', () => {
  const scene = {
    drawItems: [item(7), item(7)],
    meshes: [mesh(7), mesh(7)],
  };
  const out = orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`);
  assert.deepEqual(out, ['mesh#7', 'mesh#7', 'item#7', 'item#7']);
});
