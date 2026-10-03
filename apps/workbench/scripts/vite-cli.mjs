/**
 * apps/workbench/scripts/vite-cli.mjs — **构建入口**（`pnpm build` / `pnpm dev` 都走这里）
 *
 * ## 为什么不让 `vite` 自己当入口
 * 受限沙箱里**不许 spawn 子进程并捕获它的输出**（要开命名管道 ⇒ `EPERM`）。Vite 在 Windows 上
 * 启动时有一次 `exec('net use')`——`optimizeSafeRealPathSync()` 用它探测"有没有映射的网络盘"。
 * 它在沙箱里直接抛 `spawn EPERM`，整个构建在**加载配置之前**就炸了。
 *
 * ⇒ 这里把 `child_process.exec` / `execFile` 换成一个"空结果"的桩：那次探测于是得到
 *   "没有网络盘"（**与真实机器上的结论一致**：本仓不依赖映射盘），Vite 继续走正常路径。
 *
 * ★ 它**不改变产物**，也不改变任何格式/业务规则：构建本身不需要 spawn。
 * ★ 只在**本进程**里生效，进程退出即消失。
 */
import cp from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/** 只在 Windows 上有那次探测；别的平台原样不动 */
function stubSpawnOnlyForProbe() {
  if (process.platform !== 'win32') return;
  const empty = (cmd, opts, cb) => {
    const done = typeof opts === 'function' ? opts : cb;
    if (typeof done === 'function') queueMicrotask(() => done(null, '', ''));
    return { on() {}, once() {}, kill() {}, unref() {} };
  };
  cp.exec = empty;
  cp.execFile = empty;
}

export async function main(argv = process.argv.slice(2)) {
  stubSpawnOnlyForProbe();
  const cmd = argv[0] ?? 'build';
  const vite = await import('vite');
  if (cmd === 'dev') {
    const server = await vite.createServer({ server: { port: 5199 } });
    await server.listen();
    server.printUrls();
    return 0;
  }
  if (cmd === 'preview') {
    const server = await vite.preview({ preview: { port: 5199 } });
    server.printUrls();
    return 0;
  }
  await vite.build();
  return 0;
}

const isDirectRun = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isDirectRun) {
  main()
    .then((code) => {
      process.exitCode = code ?? 0;
    })
    .catch((err) => {
      process.stderr.write(`ERROR: ${err?.stack ?? err}\n`);
      process.exitCode = 1;
    });
}
