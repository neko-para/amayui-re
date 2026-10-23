/**
 * 场景绘制顺序（**引擎行为；无命令记录时已成篇**）。
 *
 * ★ **绘制顺序是引擎行为，属于 spec 的范围**：引擎在渲染步进里遍历场景、决定先画谁；
 *   "最终像素 / D3D 调用格式"才是平台边界。⛔ 不能因为 `0x20c` 把步进请求交给渲染子系统，
 *   就说"谁先谁后由平台自由选择"。
 * ★ 现状：本版 spec 的 `CORE-SCENE-DRAW-ORDER` 已写**无命令记录**时的 key 严格升序 /
 *   同键网格先于绘制项；本实现按这条合同。**有命令记录**时的两档顺序仍未成篇（0x20c 残差）。
 * ★ 观察到：TITLE 用 key **0** 的全屏黑色网格当背景、key 10/100/… 的绘制项当前景、
 *   key **200000** 的透明网格当覆盖层；LOGO 用 key 200002/200003 的网格与 key 200000 的影片项。
 *   按 key 升序既让 TITLE 的黑色背景落到最下面，也保持 LOGO 的影片覆盖层在最后。
 * ★ 相同 key 时**网格先于绘制项**（网格先发出，绘制项压在上面）；同类内部保持表内顺序。
 */
import type { DrawItemSnapshot, MeshSnapshot, SceneSnapshot } from '../protocol.ts';

export type SceneDrawable =
  | { kind: 'item'; key: number; item: DrawItemSnapshot }
  | { kind: 'mesh'; key: number; mesh: MeshSnapshot };

/** 把绘制项与网格合成一条按 key 升序的绘制序列（同 key 稳定） */
export function orderedDrawables(scene: SceneSnapshot): SceneDrawable[] {
  const entries: { drawable: SceneDrawable; seq: number }[] = [];
  // 负键不参与绘制（CORE-SCENE-DRAW-ORDER）；表内同键的后写覆盖由核心的记录表保证。
  scene.drawItems.forEach((item, index) => {
    if (item.key < 0) return;
    entries.push({ drawable: { kind: 'item', key: item.key, item }, seq: index });
  });
  const itemCount = scene.drawItems.length;
  scene.meshes.forEach((mesh, index) => {
    if (mesh.key < 0) return;
    entries.push({ drawable: { kind: 'mesh', key: mesh.key, mesh }, seq: itemCount + index });
  });
  entries.sort((a, b) => {
    if (a.drawable.key !== b.drawable.key) return a.drawable.key < b.drawable.key ? -1 : 1;
    // 同键：网格先发出（绘制项后发出，压在上面）；同类保持表内顺序。
    if (a.drawable.kind !== b.drawable.kind) return a.drawable.kind === 'mesh' ? -1 : 1;
    return a.seq - b.seq;
  });
  return entries.map((entry) => entry.drawable);
}
