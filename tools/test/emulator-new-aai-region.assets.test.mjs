/** @env assets @kind contract @why 真实安装目录的扩展索引（APPEND01..05.AAI）没被枚举/装载，或区号 / 槽空 / 越界三类被折叠成同一个错误，扩展区取件会退化成"读不到"而基础索引测试仍然全绿 */
/**
 * tools/test/emulator-new-aai-region.assets.test.mjs —— 真实 `*.AAI`（扩展索引）的守卫
 *
 * ## 判据是什么（`CORE-FS-EXT-INDEX-KEY` / `CORE-FS-INDEX-STACK`）
 * 1. `readById(0x01000000)` 不抛，且名字 = `$1$AUTORUN.BIN`（`APPEND01.AAI` 文件表第 0 条）——
 *    把"区号 → 索引 → 该索引自己的下标"整条链压成一个可观察结果；
 * 2. `readById(0x7F000000)`（区号 127，没有这份 AAI）抛的是"**未装载**"，不是"id 越界"——
 *    契约里这是两类（`CORE-FS-FETCH-FAIL` 第 3 类 vs 第 1 类）；
 * 3. 名字反查不区分大小写：`$1$autorun.bin` 命中 `0x01000000`。
 *
 * ★ **不断言 `side`**：这台安装根里已经有补丁放进去的 `$N$*` 松散件；若将来 `$1$AUTORUN.BIN`
 *   也被放成松散件，`side` 会变成 `loose` —— 那是**正确**行为（松散优先），不该让守卫红。
 *
 * ★ 样本缺席（fresh clone / 没装游戏）⇒ `t.skip`，不是红。
 *
 * 运行：`pnpm test:assets`
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

import { REPO_ROOT } from '../lib/paths.mjs';
import { loadManifest } from '../lib/manifest.mjs';
import { rootFromManifest } from '../lib/patch.mjs';
import { Install } from '../../apps/emulator-new/src/core/install.ts';
import { nodeHost } from '../../apps/emulator-new/src/platform/node/host.ts';

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

test('★ readById(0x01000000) 不抛，名字 = $1$AUTORUN.BIN（扩展索引真的装载了）', async (t) => {
  if (skip) { t.skip(skip); return; }
  const install = await Install.open(nodeHost, installDir);

  const source = await install.readById(0x01000000);
  assert.equal(source.id, 0x01000000);
  assert.equal(source.name, '$1$AUTORUN.BIN', 'APPEND01.AAI 文件表第 0 条（区号来自头内 +264）');
  assert.ok(source.bytes.length > 0, '取件必须真的拿到内容（不是空字节）');
  assert.equal(source.bytes.length, source.entryLength, '没有松散件时长度 = TOC 登记长度');
  assert.match(source.archive ?? '', /^APPEND01\.ALF$/i, '归档名来自该索引自己的归档表（不加盘符前缀）');
});

test('★ readById(0x7F000000) 抛"未装载"而不是"越界"（两类失败彼此可分）', async (t) => {
  if (skip) { t.skip(skip); return; }
  const install = await Install.open(nodeHost, installDir);

  const error = await install.readById(0x7f000000).then(
    () => null,
    (err) => err instanceof Error ? err.message : String(err),
  );
  assert.ok(error !== null, '区号 127 没有装载 ⇒ 必须失败');
  assert.match(error, /^aai-not-loaded:127$/,
    `★ 抛的必须是"扩展索引未装载"，⛔ 不是 script-id-out-of-range（实际：${error}）`);
  assert.doesNotMatch(error, /script-id-out-of-range/);
});

test('★ readByName("$1$autorun.bin") 大小写不敏感 ⇒ 0x01000000', async (t) => {
  if (skip) { t.skip(skip); return; }
  const install = await Install.open(nodeHost, installDir);

  const source = await install.readByName('$1$autorun.bin');
  assert.ok(source !== null, '小写名字必须命中扩展索引');
  assert.equal(source.id, 0x01000000);
  assert.equal(source.name, '$1$AUTORUN.BIN');
});
