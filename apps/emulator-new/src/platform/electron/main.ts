/**
 * Electron **主进程**入口：窗口壳 + 启动参数 + 生命周期。
 *
 * ★ 2026-10 架构调整：**core 整份在 renderer 里**（安装取件 / 配置 / 启动顺序 / VM / 场景 / Pixi）。
 *   main 只做三件事：
 *     1. 建 bootstrap BrowserWindow 并加载 renderer；
 *     2. 读 `data/spec` 的纯数据、把启动参数交给 renderer；
 *     3. 处理 `host.*` / `boot.*` RPC 与进程退出。
 *   ⛔ main 不再 import `core/**`，也不再有 `Machine` / `analyzeInstall` / `ElectronRenderHost`。
 *
 * ★ 窗口顺序的硬约束：renderer 必须跑在某个 BrowserWindow 里 ⇒ 这个窗口**先于** core 存在。
 *   `boot.createWindow` 不再新建窗口，而是把 bootstrap 窗口调整成配置要求的尺寸 / 形态并显示。
 *
 * ★ 三种模式（都会打开一个**独立只读 debug 窗口**；game 窗口只留 canvas）：
 *   - `--install <dir> [--key <u32>] [--root-id 0] [--config <file>]`：真实入口；
 *   - `--demo`：非游戏可视化自检（renderer 里用真实 core handlers 驱动）；
 *   - `--smoke`：同 demo，main 在 `demo-ready` 后截图并退出（同时校验 debug 窗口收到投影）；
 *   什么都不给 ⇒ 说明窗口（不启动 core，也不建 debug 窗口）。
 */
import { app, BrowserWindow } from 'electron';
import fs from 'node:fs';
import path from 'node:path';

import { ElectronWindowHost } from './window-host.ts';
import type { LogLevel } from './window-host.ts';
import type { DemoJob, InstallJob } from './protocol.ts';
import { REPO_ROOT, loadConfigKeys, loadOpcodeTable } from '../node/spec.ts';

/** app 根：`apps/emulator-new`（`dist/renderer` 与 preload 都相对它） */
const APP_ROOT = path.join(REPO_ROOT, 'apps', 'emulator-new');
/** 持久日志：不依赖终端是否捕获 stdout（Electron 从快捷方式起时也能回看） */
const LOG_FILE = path.join(REPO_ROOT, '.tmp', 'emulator-new.log');
const WINDOW_TITLE = 'ARCGameEngine';
const SMOKE_TIMEOUT_MS = 60_000;

const USAGE = [
  '用法：',
  '  electron --no-sandbox apps/emulator-new --install <dir> [--key <u32>] [--root-id 0] [--config <file>] [--skip-logo]',
  '  electron --no-sandbox apps/emulator-new --demo',
  '  electron --no-sandbox apps/emulator-new --smoke',
  '',
  '  --install  真实入口（core 在 renderer：索引 → 配置 → 窗口 / 图形设备 → 根脚本 → 主循环）',
  '  --demo     非游戏的可视化自检（真实 core handlers 驱动 0x1f8 / 0x204 / 0x1fb / 0x33f / 0x20d）',
  '  --smoke    同 --demo，截图到 .tmp/emulator-new-electron-smoke.png 后退出（退出码 0 / 1）',
  '  --skip-logo  仅 --install：show-logo 初值置 0（跳过 LOGO，直接到 INIT / TITLE）',
  '  --command-exec-kinds 0,1,2  手动开闸命令档（默认关；仅用于像素对照）',
  '',
  '  --no-sandbox 是 Chromium 的开关（DSH 沙箱里需要），不是本程序定义的。',
].join('\n');

interface Args {
  install: string | null;
  key: number | null;
  rootId: number;
  config: string | null;
  demo: boolean;
  smoke: boolean;
  skipLogo: boolean;
  commandExecKinds: number[];
}

let activeHost: ElectronWindowHost | null = null;

/* ──────────────────────────── 日志 / 参数 ──────────────────────────── */

function log(level: LogLevel, text: string): void {
  const line = `[electron] ${text}`;
  if (level === 'error') console.error(line);
  else if (level === 'warn') console.warn(line);
  else console.log(line);
  try {
    fs.mkdirSync(path.dirname(LOG_FILE), { recursive: true });
    fs.appendFileSync(LOG_FILE, `${line}\n`);
  } catch {
    /* 日志失败不能影响启动 */
  }
}

function parseU32(text: string, what: string): number {
  const value = /^0x/i.test(text) ? Number.parseInt(text.slice(2), 16) : Number.parseInt(text, 10);
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new Error(`${what} 必须是 u32：${text}`);
  return value >>> 0;
}

/**
 * 解析本程序参数。
 * ★ `process.argv` 里有 Electron 自己的东西：`[electron 二进制, '--no-sandbox', '.', …我们的参数…]`
 *   ⇒ 位置参数跳过；Chromium 认识的开关也跳过。`--real-time` 是 CLI 选项，不是 Electron 的；
 *   它在 main 侧没有语义（Electron 本来就用宿主毫秒时钟）。
 */
function parseArgs(argv: string[]): Args {
  const out: Args = { install: null, key: null, rootId: 0, config: null, demo: false, smoke: false, skipLogo: false, commandExecKinds: [] };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith('-')) continue;
    if (arg === '--install') out.install = argv[++i] ?? null;
    else if (arg === '--key') {
      const key = parseU32(argv[++i] ?? '', '--key');
      if (key === 0) throw new Error('--key 必须是运行期非 0 值；省略 --key 让实现自动生成');
      out.key = key;
    }
    else if (arg === '--root-id') out.rootId = parseU32(argv[++i] ?? '', '--root-id');
    else if (arg === '--config') out.config = argv[++i] ?? null;
    else if (arg === '--demo') out.demo = true;
    else if (arg === '--smoke') out.smoke = true;
    else if (arg === '--skip-logo') out.skipLogo = true;
    else if (arg === '--command-exec-kinds') {
      const raw = argv[++i] ?? '';
      out.commandExecKinds = raw.split(',').map((part) => Number(part.trim())).filter((value) => Number.isInteger(value) && value >= 0 && value <= 3);
    }
    else {
      const name = arg.replace(/^--?/, '').split('=')[0] ?? '';
      if (app.commandLine.hasSwitch(name)) continue;
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  if (out.install !== null && (out.demo || out.smoke)) throw new Error('--install 与 --demo / --smoke 只能给一个');
  if (out.skipLogo && out.install === null) throw new Error('--skip-logo 只用于 --install');
  if (out.demo && out.smoke) throw new Error('--demo 与 --smoke 只能给一个');
  return out;
}

/* ──────────────────────────── 退出清理 ──────────────────────────── */

function cleanup(): void {
  const host = activeHost;
  activeHost = null;
  if (host === null) return;
  try {
    host.dispose();
  } catch (err) {
    console.error('[electron] 退出清理失败：', err);
  }
}

function fail(text: string, err?: unknown): void {
  console.error(`[electron] ${text}`);
  if (err instanceof Error) console.error(err.stack ?? err.message);
  else if (err !== undefined) console.error(err);
  cleanup();
  app.exit(1);
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => { setTimeout(resolve, ms); });
}

async function withTimeout<T>(promise: Promise<T>, ms: number, what: string): Promise<T> {
  return await new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${what} 超时（${ms}ms）`)), ms);
    void promise.then(
      (value) => { clearTimeout(timer); resolve(value); },
      (error) => { clearTimeout(timer); reject(error); },
    );
  });
}

/* ──────────────────────────── 启动装配 ──────────────────────────── */

async function openWindowHost(commandExecKinds: number[]): Promise<ElectronWindowHost> {
  const host = new ElectronWindowHost({ appRoot: APP_ROOT, log, commandExecKinds });
  activeHost = host;
  await host.open();
  const ready = await host.waitForReady();
  if (ready === null) throw new Error('renderer 没有在超时内上报 ready（Pixi / WebGL 初始化失败？）');
  return host;
}

async function runInstall(args: Args): Promise<void> {
  const installDir = args.install;
  if (installDir === null) { fail('内部错误：--install 没有值'); return; }
  try {
    const host = await openWindowHost(args.commandExecKinds);
    const [table, configKeys] = await Promise.all([loadOpcodeTable(), loadConfigKeys()]);
    const job: InstallJob = {
      mode: 'install',
      installDir,
      rootId: args.rootId,
      key: args.key ?? undefined,
      showLogo: args.skipLogo ? 0 : undefined,
      configPath: args.config,
      opcodes: [...table.values()],
      configKeys,
      platform: host.platformInfo(),
    };
    host.start(job);
    const done = await host.waitForDone();
    if (!done.ok) {
      fail(`renderer 执行失败${done.error ? `：${done.error}` : ''}`);
      return;
    }
    printReport(done.report);
    cleanup();
    app.quit();
  } catch (err) {
    fail('启动 / 执行失败（退出码 1）', err);
  }
}

async function runDemo(args: Args): Promise<void> {
  try {
    const host = await openWindowHost(args.commandExecKinds);
    const configKeys = await loadConfigKeys();
    const job: DemoJob = {
      mode: 'demo',
      configKeys,
      platform: host.platformInfo(),
      smoke: args.smoke,
    };
    host.start(job);

    // demo 只发 demo-ready（不跑引擎主循环）；等它或错误 done，二者先到者为准。
    const outcome = await withTimeout(Promise.race([
      host.waitForDemoReady().then(() => ({ kind: 'ready' as const })),
      host.waitForDone().then((done) => ({ kind: 'done' as const, done })),
    ]), SMOKE_TIMEOUT_MS, '--demo 等待 renderer');

    if (outcome.kind === 'done') {
      if (!outcome.done.ok) fail(`demo 失败${outcome.done.error ? `：${outcome.done.error}` : ''}`);
      else fail('demo 在 ready 之前就结束了（不是可交互的 demo）');
      return;
    }

    if (args.smoke) {
      // 两个窗口的 smoke：game 只要 canvas；debug 必须真的收到投影。
      const debug = host.debugWindow();
      if (debug === null || debug.isDestroyed()) throw new Error('debug 窗口没有创建');
      await delay(500);
      const debugText = await debug.webContents.executeJavaScript(
        'document.getElementById("diagnostics")?.innerText ?? ""',
      ) as string;
      if (!debugText.includes('diagnostics')) throw new Error('debug 窗口没有收到 diagnostics 投影');
      const win = host.browserWindow();
      if (win === null || win.isDestroyed()) throw new Error('没有窗口可截屏');
      const gamePanelHidden = await win.webContents.executeJavaScript(
        'document.getElementById("diagnostics")?.hidden === true',
      ) as boolean;
      if (!gamePanelHidden) throw new Error('game 窗口仍然显示 diagnostics 面板（应只有 canvas）');
      const image = await win.webContents.capturePage();
      if (image.isEmpty()) throw new Error('capturePage 返回空图像');
      const file = path.join(REPO_ROOT, '.tmp', 'emulator-new-electron-smoke.png');
      await fs.promises.mkdir(path.dirname(file), { recursive: true });
      await fs.promises.writeFile(file, image.toPNG());
      const size = image.getSize();
      log('info', `--smoke OK · 截图 ${file}（${size.width}x${size.height}）`);
      cleanup();
      app.exit(0);
      return;
    }

    host.allowClose();
    log('info', '--demo：这是**非游戏**的可视化自检（不是 spec 结论）；关闭窗口即退出');
  } catch (err) {
    fail('demo 启动失败', err);
  }
}

/* ──────────────────────────── 报告输出 ──────────────────────────── */

function asRecord(value: unknown): Record<string, unknown> | null {
  return typeof value === 'object' && value !== null ? value as Record<string, unknown> : null;
}

function printReport(raw: unknown): void {
  const report = asRecord(raw);
  if (report === null) {
    log('warn', 'renderer 没有回传报告');
    return;
  }
  const source = asRecord(report.source);
  if (source?.kind === 'install') {
    console.log(`[electron] 安装   ${String(source.dir)}`);
    console.log(`[electron] 脚本   id ${String(source.id)} · ${String(source.name)} · ${String(source.actualLength)} 字节`);
  }
  const config = asRecord(report.config);
  if (config !== null) {
    console.log(`[electron] 配置   ${String(config.path)} · 来源 ${String(config.source)}`);
  }
  console.log(`[electron] 执行   ${String(report.steps ?? 0)} 条 · 停因 ${String(report.stopText ?? report.stop)}`);
  const diagnostics = Array.isArray(report.diagnostics) ? report.diagnostics : [];
  console.log(`[electron] 时钟   ${String(report.ticks ?? 0)} 格 · ${String(report.atMs ?? 0)}ms · 诊断 ${diagnostics.length} 条`);
  const entered = Array.isArray(report.entered) ? report.entered : [];
  if (entered.length > 0) console.log(`[electron] 进入过 ${entered.join(', ')}`);
  const gap = asRecord(report.gap);
  if (gap !== null) {
    const opcode = Number(gap.opcode);
    console.log(`[electron] 缺口   opcode 0x${opcode.toString(16)} @ 第 ${String(gap.index)} 条（${String(gap.reason)}）`);
  }
}

/* ──────────────────────────── 进程生命周期 ──────────────────────────── */

// ★ userData 必须在 app ready 之前设置：否则 Chromium 会往默认目录写缓存
app.setPath('userData', process.env.AMAYUI_USER_DATA ?? path.join(REPO_ROOT, '.tmp', 'electron-userdata'));

app.on('window-all-closed', () => { app.quit(); });
app.on('before-quit', () => { cleanup(); });

async function main(): Promise<void> {
  let args: Args;
  try {
    args = parseArgs(process.argv.slice(2));
  } catch (err) {
    console.error(`[electron] ${err instanceof Error ? err.message : String(err)}`);
    console.error(USAGE);
    app.exit(2);
    return;
  }
  if (args.install !== null) {
    await runInstall(args);
    return;
  }
  if (args.demo || args.smoke) {
    await runDemo(args);
    return;
  }
  showUsageWindow();
}

void app.whenReady().then(async () => {
  try {
    await main();
  } catch (err) {
    fail('未捕获的启动异常', err);
  }
});

/* ──────────────────────────── 说明窗口（没有参数） ──────────────────────────── */

function showUsageWindow(): void {
  const win = new BrowserWindow({
    width: 780,
    height: 560,
    title: `${WINDOW_TITLE} — 未启动`,
    backgroundColor: '#11161c',
    autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true },
  });
  const html = `<!doctype html>
<html lang="zh">
<meta charset="utf-8">
<title>${WINDOW_TITLE} — 未启动</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; padding: 28px 32px; background: #11161c; color: #d7dee8;
         font: 14px/1.6 -apple-system, "Segoe UI", "Hiragino Sans", sans-serif; }
  h1 { font-size: 18px; margin: 0 0 6px; color: #eaf1fa; }
  p.lead { margin: 0 0 18px; color: #9fb0c3; }
  pre { background: #0b0f14; border: 1px solid #24303d; border-radius: 8px;
        padding: 14px 16px; overflow: auto; font-size: 13px; color: #cfe3ff; }
  code { color: #ffd479; }
  .note { margin-top: 16px; color: #8b9cad; font-size: 13px; }
</style>
<h1>${WINDOW_TITLE} — 窗口壳已启动，但**没有**跑引擎</h1>
<p class="lead">这是说明窗口，不是游戏画面。没有收到 <code>--install</code> / <code>--demo</code> / <code>--smoke</code>，所以既没有建窗、也没有初始化图形设备。</p>
<pre>electron --no-sandbox apps/emulator-new --install &lt;dir&gt; [--key &lt;u32&gt;] [--config &lt;file&gt;]
electron --no-sandbox apps/emulator-new --demo     # 非游戏的可视化自检（0x1f8 / 0x204 / 0x1fb / 0x33f / 0x20d）
electron --no-sandbox apps/emulator-new --smoke    # 同一套 + 截图到 .tmp/emulator-new-electron-smoke.png</pre>
<p class="note">--demo / --smoke 需要先构建渲染进程（<code>vite build</code> ⇒ <code>dist/renderer/index.html</code>）。<br>
关闭这个窗口即退出。</p>
</html>`;
  void win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`);
  log('info', '没有启动参数 ⇒ 打开说明窗口（不调用 core）');
}
