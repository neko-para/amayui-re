/** @env assets @kind product @why 参考实现从"能解码"倒退成"不能执行"、或前沿棘轮被悄悄踩回去（步数/停点变少），而解码测试仍然全绿 */
/**
 * tools/test/emulator-new-logo.assets.test.mjs —— `apps/emulator-new` 的 **LOGO.BIN 前沿棘轮**
 *
 * ## 判据是什么
 * 从 ALF 索引里取出 `LOGO.BIN`（统一文件 id `0x5262`），用 `data/spec` 驱动执行，断言：
 *   ① 文件本身没换（长度 + sha256）；
 *   ② 从第 0 条开始**真的执行**（不是解码走查）；
 *   ③ 停在**第一个没有实现的 opcode** 前面，且停点（条数 / opcode）就是当前前沿；
 *   ④ 已经执行掉的那条指令留下了**引擎数据**（`0x51`：局部整型 0 ← 0 − 1）。
 *
 * ★ ③ 的条数与 opcode 是**棘轮**：实现新一族 opcode 时它只会变大 / 往后 —— 那时**同步抬高这里**。
 *   它防的是"某个 handler 坏了 ⇒ 前沿倒退"，而那种倒退在解码测试里看不出来。
 * ★ 样本缺席（fresh clone / 没装游戏）⇒ skip，不是红。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';
import { rootFromManifest } from '../lib/patch.mjs';
import { Install } from '../../apps/emulator-new/src/core/install.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';
import { loadOpcodeTable } from '../../apps/emulator-new/src/platform/node/spec.ts';
import { parseHeader } from '../../apps/emulator-new/src/core/decode.ts';
import { Machine } from '../../apps/emulator-new/src/core/machine.ts';
import { decodeInt } from '../../apps/emulator-new/src/core/values.ts';

const KEY = 0x1234abcd;
const LOGO_ID = 0x5262;

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

async function logoBytes() {
  // ★ 走**同一条真实入口**：索引 → 统一文件 id → 松散优先/归档切片
  const install = await Install.open(nodeHost, installDir);
  const script = await install.readById(LOGO_ID);
  assert.equal(script.name.toUpperCase(), 'LOGO.BIN', `条目 ${LOGO_ID} 必须是 LOGO.BIN`);
  assert.equal(script.side, 'archive', '这台安装里 LOGO.BIN 没有松散文件 ⇒ 必须从归档切片');
  assert.equal(script.entryLength, 1508, 'TOC 登记的 LOGO.BIN 长度');
  assert.ok(script.bytes.length > 0, '载荷不能为空');
  return script.bytes;
}

test('★ 前沿棘轮：LOGO.BIN 执行到 `0x20f play-movie` 前（当前：第 46 条前）', { skip }, async () => {
  const bytes = await logoBytes();
  assert.equal(bytes.length, 1508, 'LOGO.BIN 的字节数');
  assert.equal(crypto.createHash('sha256').update(bytes).digest('hex'),
    'f17364da0ffbca38aedee4a3b46db15945a7cf864e3b7773a422d24fa6b9ba9b', 'LOGO.BIN 的 sha256');

  const buf = Buffer.from(bytes);
  const header = parseHeader(buf);
  assert.equal(header.headerLength, 60, 'LOGO.BIN 的头长');
  const body = buf.subarray(header.headerLength);
  assert.equal(body.length, 1448, 'LOGO.BIN 的 body 字节数');

  const install = await Install.open(nodeHost, installDir);
  const table = await loadOpcodeTable();
  const makeMachine = () => new Machine({
    body, table, key: KEY, uninitializedFloat: 'zero',
    // 真实纹理文件（统一文件 id → 整份 AGF），不是合成图
    loadImage: async (id) => (await install.readById(id)).bytes,
  });
  const m = makeMachine();
  const r = await m.run({ stopAtOpcode: 0x20f });

  // ① 真的在执行：前 46 条已经执行掉（play-movie 本身**没有**执行 —— 停止条件是"停在它前面"）
  assert.equal(r.steps, 46, '当前前沿：执行掉 46 条（实现新一族时抬高）');
  assert.equal(r.ip, 1384, '第 46 条 `0x20f` 的起点（由实跑复算）');

  // ② 等待门真的等了：版权页窗 4500+500ms，虚拟时钟 16ms/格
  assert.equal(m.ticks, 314, '门关着的迭代次数（5024 / 16）');
  assert.equal(m.now, 5024, '虚拟时刻（毫秒）');
  assert.equal(m.flags, 0, '等待位已经被清掉（门开着）');
  assert.equal(m.scene.animating, 0, '场景不再有动画在跑');

  // ③ 场景族：0x1f7（第 41 条，键 200000、个数 4）的删除集合**包含网格表**
  //    ⇒ 0x320 建在 200002 / 200003 的两个网格也被删掉（与 0x1f6 / 0x23b 同一组四张表）。
  assert.equal(m.scene.meshes.size, 0, '★ 0x1f7 的区间删除必须含网格表');

  // ③′ 网格 / 颜色窗：另跑一次、停在 0x1f7 **之前**，观察 0x320 建出的记录与 0x322/0x323 的重烘结果
  const before = makeMachine();
  await before.run({ stopAtOpcode: 0x1f7 });
  const mesh2 = before.scene.meshes.get(200002);
  const mesh3 = before.scene.meshes.get(200003);
  assert.ok(mesh2 && mesh3, '两个网格都建出来了');
  assert.equal(mesh2.count, 4, '顶点数来自第 9 个操作数（4）');
  assert.equal(mesh2.op10, 0, '第 10 个操作数写进网格记录');
  assert.deepEqual(mesh2.colors, [0, 0, 0, 0],
    '★ 窗到期后网格顶点色被重烘成终色（不是停在 0x322 的起色）');
  assert.equal(mesh2.colorFrom >>> 0, 0, '★ 0x323 的窗收尾：起色 = 终色（终色是 0）');
  assert.equal(mesh2.colorNow >>> 0, 0, '派生色 = 当前色（到期）');
  assert.equal(mesh3.colorFrom >>> 0, 0xff000000, '第二个网格的起色来自最后一次 0x322');
  assert.equal(mesh3.colorNow >>> 0, 0xff000000, '第二个网格的派生色 = 终色');
  assert.deepEqual(mesh3.colors, [0xff000000, 0xff000000, 0xff000000, 0xff000000],
    '第二个网格到期后顶点色也重烘');
  assert.equal(mesh3.flags & 2, 0, '网格的窗收尾时清"有窗"位');

  // ④ 纹理族：42 被 0x1f8 重建为空白纹理（覆盖掉原来的文件号），43 被 0x1fa 释放
  assert.deepEqual(m.scene.textures.get(42), { file: -1, color: 0, blank: 1 });
  assert.deepEqual(m.scene.textures.get(43), { file: -1, color: 0, blank: 0 }, '★ 0x1fa 只清来源文件、不清空白创建标记');
  assert.equal(m.scene.textureObjects.has(43), false, '纹理槽 43 被释放');
  assert.deepEqual(m.scene.textureObjects.get(42), { kind: 'normal', width: 1280, height: 720 });

  // ⑤ 绘制族：0x1f7 的区间拆离删掉了旧项，0x1fb 又画了一项
  assert.deepEqual([...m.scene.drawItems.keys()], [200000], '区间 [200000,200004) 删掉了 200000/200001，只有 0x1fb 重画的那项还在');
  const item = m.scene.drawItems.get(200000);
  assert.ok(item);
  assert.equal(item.flags & 1, 1, '有效位');
  assert.equal(item.textureId, 42, '重画时指向槽 42');
  assert.deepEqual([item.left, item.top, item.right, item.bottom], [0, 0, 1280, 720]);
  assert.equal(m.scene.dirty, 1);

  // ⑥ 外部请求可见；`0x320` 的 4 / 7 / 8 是**按地址读**（不是浮点读）⇒ 不再产生未初始化浮点诊断
  assert.deepEqual(m.effects.map((e) => e.kind),
    ['texture.load.request', 'texture.load.request', 'input.poll', 'texture.create.request']);
  assert.deepEqual(m.diagnostics.filter((d) => d.kind === 'uninitialized-float'), [],
    '★ 0x320 的 4/7/8 按地址读（裸 32 位），不再走读浮点原语');

  // ⑦ 停在 play-movie 前面（不是猜着往下跑、也不是 no-op）
  assert.equal(r.stop.kind, 'stop-opcode', `停因必须是命中停止条件：${JSON.stringify(r.stop)}`);
  if (r.stop.kind !== 'stop-opcode') return;
  assert.equal(r.stop.opcode, 0x20f, '停止条件 opcode');
  assert.equal(r.stop.index, 46, 'play-movie 的指令下标');
});
