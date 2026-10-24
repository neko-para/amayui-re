/** @env pure @kind contract @why 场景绘制顺序退回"先所有绘制项、再所有网格"会让 TITLE 的 key 0 黑色背景盖住前景（整屏黑），而 LOGO 的影片覆盖层仍可能正常 —— 只有合成顺序的守卫才会红 */
/**
 * tools/test/emulator-new-scene-order.test.mjs —— 场景合成顺序（**引擎行为；spec 待成篇**）
 *
 * 判据是 spec 的 `CORE-SCENE-DRAW-ORDER`（无命令记录时）：TITLE 用 key 0 的全屏黑网格当背景、
 * key 10/100/… 的绘制项当前景、key 200000 的网格当覆盖层；两类对象按 **key 升序**放在同一条序列里。
 * 同键时**网格先于绘制项**（绘制项后发出、压在上面）。
 * ★ **负键口径按表分**：网格 / 辅助表的负键不参与（0 是哨兵），**绘制项不做符号检查**（负键绘制项照发）。
 * ★ 有场景命令记录时的两档顺序（命令档）**未建模**；这个守卫只覆盖收尾扫描那一档。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { orderedDrawables, scrubSceneTexture, snapshotReferencesTexture } from '../../apps/emulator-new/src/platform/electron/renderer/scene-order.ts';

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

test('★ 负键口径**按表分**：负键绘制项被发出（排最前），负键网格不参与', () => {
  // CORE-SCENE-DRAW-ORDER 的纠错点：收尾扫描用 0 当"该表已没有可出条目"的哨兵 ⇒
  // 网格 / 辅助表 A / 辅助表 B 的负键不参与；**绘制项是最后候选，不做符号检查**。
  const scene = {
    drawItems: [item(-1), item(5)],
    meshes: [mesh(-2), mesh(3)],
  };
  assert.deepEqual(
    orderedDrawables(scene).map((entry) => `${entry.kind}#${entry.key}`),
    ['item#-1', 'mesh#3', 'item#5'],
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

/*
 * ── 释放纹理后的快照剥离（`0x1fa` → `pixi-host.releaseTexture`） ──
 * ★ 这条守卫红意味着：`scrubSceneTexture` 的**范围**被改了。它钉住的事实是
 *   **剥完 ≠ 快照变新了** —— 网格不引用纹理，所以剥完之后快照仍能画出"上一页的底色"。
 *   这正是两次"白闪修复"没生效的原因（36f9444 在中途 present、2f0f362 只剥一个 id）：
 *   版权页是"白底网格 + 前景纹理"，剥掉前景纹理之后剩下的就是**整屏白**。
 * ⇒ 结论（也是本用例的判据）：剥离只能喂给**帧边界**上的呈现，⛔ 不能拿它去中途 present。
 */
test('★ 剥离只删该 id 的纹理与绘制项引用；网格仍留下（剥完不是"新快照"）', () => {
  const scene = {
    width: 1280,
    height: 720,
    textures: [{ id: 42 }, { id: 43 }],
    drawItems: [item(200000), item(300000)],
    meshes: [mesh(200002), mesh(200003)],
  };
  scene.drawItems[0].textureId = 42;
  scene.drawItems[1].textureId = 9;

  const scrubbed = scrubSceneTexture(scene, 42);
  assert.deepEqual(scrubbed.textures.map((t) => t.id), [43], '只删被释放的那个纹理槽');
  assert.deepEqual(scrubbed.drawItems.map((i) => i.key), [300000], '只删引用该 id 的绘制项');
  assert.deepEqual(scrubbed.meshes.map((m) => m.key), [200002, 200003],
    '网格不看纹理 ⇒ 全部留下（于是"上一页的底色"仍会被画出来）');
  assert.equal(scrubbed.width, 1280, '其余字段原样带过（纯函数，不改快照的其它部分）');
  assert.equal(scrubSceneTexture(scene, 42) !== scrubbed, true, '每次返回新对象（不改入参）');
  assert.deepEqual(scene.textures.map((t) => t.id), [42, 43], '入参不被修改');
});

test('★ 快照引用自检：纹理表命中也算"被引用"（影片覆盖层诊断用）', () => {
  const scene = {
    textures: [{ id: 42 }],
    drawItems: [{ key: 1, textureId: 9 }],
  };
  assert.equal(snapshotReferencesTexture(scene, 42), true);
  assert.equal(snapshotReferencesTexture(scene, 9), true);
  assert.equal(snapshotReferencesTexture(scene, 7), false);
});
