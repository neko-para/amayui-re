/** @env assets @kind product @why 启动期整屏白闪回归：只要启动链里出现一条"接近纯白的 present"就说明白闪回来了（而这条以前只能靠人眼看窗口） */
/**
 * `tools/test/emulator-new-flash-probe.assets.test.mjs` —— 启动期白闪的**自动判据**
 *
 * ## 它判什么（两条一起成立才算过）
 * ① **确实呈现过**：renderer 的 `[present] frames=N` 至少到 N ≥ 10 —— 否则"没有白闪"与"根本没有帧"在日志上一样；
 * ② **没有白闪**：整段日志里没有 `[flash-probe] white present`（产品自带的探针：每次 present 读后缓冲中心 1 像素，
 *    RGB ≥ 235 才记）。
 *
 * ## 为什么是 assets 档
 * 要**真游戏安装**（`links/install`）与 Electron（会开窗口）。★ 它不看像素对不对，只看"有没有整屏白"。
 */
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const INSTALL = path.join(ROOT, 'links', 'install');
const DIST = path.join(ROOT, 'apps', 'emulator-new', 'dist', 'renderer', 'index.html');

test('启动期白闪：呈现过 ≥10 帧且没有一条 white present', (t) => {
  if (!fs.existsSync(INSTALL)) return t.skip('没有游戏安装（links/install）');
  if (!fs.existsSync(DIST)) return t.skip('renderer 未构建（pnpm --filter @amayui/emulator-new build:renderer）');
  const log = path.join(os.tmpdir(), `amayui-flash-${process.pid}.log`);
  // ★ 沙箱里不能捕获子进程输出（管道要命名管道 ⇒ EPERM）⇒ 用文件描述符重定向。
  const fd = fs.openSync(log, 'w');
  try {
    // ★ 用包内的 electron 可执行文件，不要经 npx（`spawnSync` 直接调 `npx` 在沙箱里起不来 ⇒ 日志会是空的）。
    const bin = path.join(ROOT, 'apps', 'emulator-new', 'node_modules', '.bin', 'electron');
    spawnSync(bin, ['--no-sandbox', '.', '--install', INSTALL, '--skip-logo'], {
      cwd: path.join(ROOT, 'apps', 'emulator-new'),
      stdio: ['ignore', fd, fd],
      timeout: 150_000,
    });
  } finally {
    fs.closeSync(fd);
  }
  const text = fs.readFileSync(log, 'utf8');
  fs.rmSync(log, { force: true });

  const frames = [...text.matchAll(/\[present\] frames=(\d+)/g)].map((m) => Number(m[1]));
  const maxFrames = frames.length > 0 ? Math.max(...frames) : 0;
  assert.ok(maxFrames >= 10, `启动链里呈现的帧太少（max frames=${maxFrames}）⇒ 这条判据不成立，先查启动链`);
  const white = text.split('\n').filter((line) => line.includes('[flash-probe] white present'));
  assert.deepEqual(white, [], `启动期出现整屏白 present：\n${white.join('\n')}`);
});
