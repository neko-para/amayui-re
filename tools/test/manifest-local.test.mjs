/** @env pure @kind gate @why 本机私有覆盖（平台路径）失效 —— 换一台机器就红、或本机覆盖悄悄改写了仓库事实 */
/**
 * tools/test/manifest-local.test.mjs — **本机私有清单覆盖**（`corpus/assets.local.json`）的守卫
 *
 * 它守三件事，每件都有具体事故在背后：
 * ① **覆盖真的生效**：`roots` 逐键覆盖（win32 的 `E:\…` 与 macOS 的 `/Users/…` 同一份清单两套路径）。
 *    ★ 失效症状是"换了平台什么都找不到"，而**报错会指向路径本身**、看起来像素材没到位 —— 极难定位。
 * ② **只覆盖 `roots`**：条目（来源与去向）是**仓库事实**，本机私有文件不许改它。
 *    否则"这台机器上清单少一条"会表现成"仓库里少一条"。
 * ③ **它不入库、且写路径确定**：`.gitignore` 命中 `*.local.json`；同输入 ⇒ 同字节（可 diff、可手改）。
 *
 * 运行：`pnpm test`（`@env pure`：只读仓库内文本 + 纯函数 + 临时目录）
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { localManifestFor, loadManifest, readLocalOverrides, saveLocalOverrides } from '../lib/manifest.mjs';
import { DEFAULT_LOCAL_MANIFEST, DEFAULT_MANIFEST, REPO_ROOT } from '../lib/paths.mjs';

/** 一个临时"仓库"：`assets.json` + 可选的同名 `assets.local.json` */
function fixture({ manifest, local }) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-manifest-'));
  const manifestPath = path.join(dir, 'assets.json');
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2));
  if (local !== undefined) {
    fs.writeFileSync(path.join(dir, 'assets.local.json'), typeof local === 'string' ? local : JSON.stringify(local, null, 2));
  }
  return { dir, manifestPath };
}

const base = { schemaVersion: 1, roots: { oldRepo: 'E:\\Games\\Eushully\\天結', staging: '.staging' }, entries: [] };

test('★ 本机私有覆盖逐键覆盖 roots（其余 roots 不受影响）', () => {
  const { manifestPath } = fixture({ manifest: base, local: { roots: { oldRepo: '/Users/nekosu/Documents/Projects/amayui-cn' } } });
  const m = loadManifest(manifestPath);
  assert.equal(m.roots.oldRepo, '/Users/nekosu/Documents/Projects/amayui-cn', '覆盖没生效 ⇒ 平台相关的路径会全部指向另一台机器');
  assert.equal(m.roots.staging, '.staging', '没被覆盖的 root 必须原样保留');
  assert.deepEqual(m.entries, [], '覆盖不许碰条目');
});

test('★ 本机私有覆盖可以新增 roots 键（新平台的来源根）', () => {
  const { manifestPath } = fixture({ manifest: base, local: { roots: { gameInstall: '/tmp/game' } } });
  const m = loadManifest(manifestPath);
  assert.equal(m.roots.gameInstall, '/tmp/game');
  assert.equal(m.roots.oldRepo, base.roots.oldRepo);
});

test('★ 覆盖文件不在场 ⇒ 清单原样（这不是错误）', () => {
  const { manifestPath } = fixture({ manifest: base });
  assert.deepEqual(loadManifest(manifestPath).roots, base.roots);
  assert.equal(readLocalOverrides(path.join(path.dirname(manifestPath), 'assets.local.json')), null);
});

test('★ 条目不许被本机覆盖：多出的顶层键一律抛（"本机少一条"不许伪装成"仓库少一条"）', () => {
  const { manifestPath } = fixture({ manifest: base, local: { roots: { oldRepo: '/x' }, entries: [] } });
  assert.throws(() => loadManifest(manifestPath), /只认 "roots"/);
});

test('★ 覆盖里 roots 的形态非法 ⇒ 抛（不静默忽略）', () => {
  for (const bad of [{ roots: { oldRepo: '' } }, { roots: { oldRepo: 7 } }, { roots: [] }]) {
    const { manifestPath } = fixture({ manifest: base, local: bad });
    assert.throws(() => loadManifest(manifestPath), /roots/);
  }
});

test('★ 写路径确定：同输入同字节、键序稳定、可回读', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'amayui-manifest-w-'));
  const p = path.join(dir, 'assets.local.json');
  saveLocalOverrides({ staging: '.staging', oldRepo: '/a' }, p);
  const first = fs.readFileSync(p, 'utf8');
  saveLocalOverrides({ oldRepo: '/a', staging: '.staging' }, p);
  assert.equal(fs.readFileSync(p, 'utf8'), first, '同输入必须同字节（否则两台机器 diff 永远不干净）');
  assert.deepEqual(readLocalOverrides(p).roots, { oldRepo: '/a', staging: '.staging' });
  // ★ key 序 = 清单的 ROOT_KEY_ORDER（oldRepo 在前），不是调用方给的顺序
  assert.ok(first.indexOf('oldRepo') < first.indexOf('staging'));
});

test('★ 同目录约定：assets.json ↔ assets.local.json（换一份清单也找得到它的覆盖）', () => {
  assert.equal(localManifestFor(DEFAULT_MANIFEST), DEFAULT_LOCAL_MANIFEST);
  assert.equal(localManifestFor('/x/y/assets.json'), path.join('/x/y', 'assets.local.json'));
  assert.equal(path.basename(DEFAULT_LOCAL_MANIFEST), 'assets.local.json');
  assert.equal(path.dirname(DEFAULT_LOCAL_MANIFEST), path.dirname(DEFAULT_MANIFEST));
});

test('★ 本机私有覆盖**不入库**：`.gitignore` 必须命中 `*.local.json`', () => {
  const gi = fs.readFileSync(path.join(REPO_ROOT, '.gitignore'), 'utf8');
  assert.ok(
    gi.split('\n').some((l) => l.trim() === '*.local.json'),
    '`.gitignore` 里必须有 `*.local.json` —— 否则平台相关的绝对路径会被提交（两台机器互相打架）',
  );
});

test('★ 两层实现必须一致：app 侧的 `rootFromAssetsJson` 与 tools 侧的 `loadManifest`（真清单）', async () => {
  // ★ 为什么钉这条：manifest 的加载器有**两处实现**（工具层 `tools/lib/manifest.mjs`；
  //   模拟器前端 `apps/emulator/frontends/headless/paths.ts` —— 它不能 import 工具层）。
  //   两处对"本机私有覆盖"的处理一旦分叉，症状是 app 侧整批 assets 用例**静默 skip**
  //   （"游戏安装不在场"），而工具侧一切正常 —— 实测踩过。AGENTS：需要两处就**钉住**。
  const { rootFromAssetsJson } = await import('../../apps/emulator/frontends/headless/paths.ts');
  const m = loadManifest(DEFAULT_MANIFEST);
  for (const name of ['oldRepo', 'gameInstall', 'gameSaves', 'staging']) {
    assert.equal(
      rootFromAssetsJson(REPO_ROOT, name) ?? null,
      typeof m.roots[name] === 'string' ? m.roots[name] : null,
      `roots.${name}：app 侧与 tools 侧必须给出同一个值`,
    );
  }
});
