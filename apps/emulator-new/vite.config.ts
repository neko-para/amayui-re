/**
 * apps/emulator-new/vite.config.ts —— **只**构建渲染进程（Electron renderer 的 Pixi 页面）。
 *
 * ★ 本文件的落点是 `apps/emulator-new` 的 app 根，所以 `root` 指到 renderer 目录、`outDir` 用
 *   `new URL('./dist/renderer', import.meta.url)` 解析成**相对 app 根**的绝对路径：
 *   Vite 的 `build.outDir` 是**相对 `root`** 解析的，写 `'./dist/renderer'` 会落到
 *   `src/platform/electron/renderer/dist/renderer`（错的地方）。
 * ★ `base: './'`：产物由 Electron 用 `file://` 装载，绝对路径 `/assets/…` 会 404。
 * ★ `target: 'chrome130'`：渲染进程只跑在 Electron 45 的 Chromium 里，不需要向下兼容。
 * ★ 只依赖 `vite` 与 `pixi.js`（都在 `package.json` 的 devDependencies / dependencies 里）：
 *   没有 React / 插件 / 别名 —— 渲染进程只用 Pixi + 原生 DOM。
 * ★ 这里**不做**主进程 / preload 的打包：它们由 Electron 直接跑 Node 的 `.ts` 剥壳（或后续单独决定）。
 */
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';

/** renderer 页面目录（Vite 的 `root`）—— `index.html` 与 `renderer.ts` 都在这里 */
const rendererRoot = fileURLToPath(new URL('./src/platform/electron/renderer', import.meta.url));
/** 产物落点（相对 **app 根**）：`apps/emulator-new/dist/renderer` */
const outDir = fileURLToPath(new URL('./dist/renderer', import.meta.url));

export default defineConfig({
  root: rendererRoot,
  base: './',
  build: {
    outDir,
    emptyOutDir: true,
    target: 'chrome130',
    // 渲染进程只有一张 HTML 入口（index.html 在 root 里，Vite 默认就认它）
    sourcemap: true,
  },
});
