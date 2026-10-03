import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

/**
 * 客户端构建配置。
 *
 * ★ `outDir = dist/web`：服务端（`apps/workbench/server.ts`）把它当**根**服务（白名单式）。
 * ★ `server.proxy`：开发时页面由 Vite dev server 出，`/api` 转到本地的 `server.ts`
 *   ⇒ `node scripts/vite-cli.mjs dev` + `node server.ts --dev` 就能改前端（见 README）。
 */
export default defineConfig({
  plugins: [vue()],
  build: { outDir: 'dist/web', emptyOutDir: true },
  server: {
    port: 5199,
    proxy: { '/api': { target: 'http://127.0.0.1:7788', changeOrigin: false } },
  },
});
