/**
 * Node 入口：`node apps/emulator-new/src/main.ts ...`（`package.json` 的 `emulator-new` 脚本）。
 *
 * ★ 这里只做一件事：把 CLI 接上进程的 **argv / 退出码**。分析入口在 `core/runner.ts`，
 *   Node 的文件系统实现在 `platform/node/`；核心不 import 任何 `node:*`。
 * ★ 用 `process.exitCode` 而不是 `process.exit()`：让 stdout 正常刷完（输出可能是 `--json` 的大对象）。
 */
import { main } from './platform/node/cli.ts';

if (process.argv[1] && process.argv[1].endsWith('main.ts')) {
  process.exitCode = await main(process.argv.slice(2));
}
