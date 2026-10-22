/**
 * Electron **主进程**入口（`package.json` 的 `main`）：窗口壳 + 启动顺序 + 两条可视化入口。
 *
 * ★ 这里只做"壳"：参数 → 平台宿主 → 核心的启动顺序 → 退出清理。
 *   核心（`core/**`）一行都不认识 Electron；`../node/**` 提供文件系统与 `data/spec` 的读取点。
 *
 * ★ 启动顺序（`06-window.md`）：注册窗口类 → 建窗 → 显示 / 重绘 → 解绑 IME → 装菜单 →
 *   **图形设备** → **根脚本** → 周期定时器 → 消息泵。
 *   核心的 `analyzeInstall` 把"读根脚本"夹在配置库之后、脚本执行之前 ⇒ 用两个集成钩子把它拆开：
 *     1. `onBeforeScript`（读完配置、读根脚本**之前**）→ `bootWindow(...)`（到图形设备就绪为止）；
 *     2. `onMachine`（`new Machine` 之后、`machine.run` 之前）→ `session.beginMainLoop()`（建周期定时器）；
 *     3. `onIteration`（`70-vm.md` 的迭代头）→ `session.pump()`（窗口 / 定时器消息）+ 节流推场景。
 *   ⛔ 图形设备失败 ⇒ `onBeforeScript` 抛错 ⇒ 根脚本根本不会被读、脚本不会跑（spec 的共享出口）。
 *
 * ★ 三种模式（**除 `--install` 外都不是游戏入口**）：
 *   - `--install <dir> --key <u32> [--root-id 0] [--config <file>]`：真实入口；
 *   - `--demo`：**非游戏**的可视化自检 —— 用真实的 core handlers 在一个空 `Machine` 上驱动
 *     `0x1f8` / `0x204` / `0x1fb` / `0x33f` / `0x20d`，验证窗口 + Pixi 后端 + 场景推送这条链路。
 *     ⛔ 它不是规范结论、不读游戏安装；
 *   - `--smoke`：与 `--demo` 同一套，等 renderer ready + 场景推完后 `capturePage()` 截图到
 *     `.tmp/emulator-new-electron-smoke.png`，退出码 0 / 1。给自动化验证用。
 *   什么都不给 ⇒ 打开一个**说明窗口**（HTML 文本），⛔ 不伪装成"已启动"。
 *
 * ★ 退出：`EngineExit`（核心在泵里取到退出消息时抛）**不是失败** ⇒ 退出清理 + `app.quit()`；
 *   其它异常 ⇒ 打印 + 清理 + 退出码 1（`app.exit(1)`）。
 * ★ `userData` 落到 `<repo>/.tmp/electron-userdata`（可用 `AMAYUI_USER_DATA` 覆盖）：
 *   否则 Chromium 会往 `~/Library/Application Support/...` 写缓存（沙箱里会失败）。
 * ★ DSH 沙箱里需要 `--no-sandbox`，但**不在代码里写死**（正常桌面启动不该关沙箱）：
 *   `node_modules/.bin/electron --no-sandbox apps/emulator-new`。
 */
import { app, BrowserWindow } from 'electron';
import fs from 'node:fs';
import path from 'node:path';

import { ConfigStore } from '../../core/config.ts';
import { Machine, WAIT_BIT } from '../../core/machine.ts';
import type { Operand } from '../../core/machine.ts';
import { analyzeInstall } from '../../core/runner.ts';
import type { CoreEnv, ScriptReport } from '../../core/runner.ts';
import { EngineExit, bootWindow } from '../../core/window.ts';
import type { WindowEntry, WindowSession } from '../../core/window.ts';
import { nodeHost } from '../node/host.ts';
import { REPO_ROOT, loadConfigKeys, loadOpcodeTable } from '../node/spec.ts';
import { ElectronWindowHost, PROCESS_INSTANCE, RESOURCE_INSTANCE } from './window-host.ts';
import type { LogLevel } from './render-host.ts';
import type { RendererInit } from './protocol.ts';

/** app 根：`apps/emulator-new`（`dist/renderer` 与 preload 都相对它） */
const APP_ROOT = path.join(REPO_ROOT, 'apps', 'emulator-new');
/** 窗口标题（与窗口类名分开；标题的实际内容未取证，见 `RES-BOOT-WINDOW-TITLE`） */
const WINDOW_TITLE = 'ARCGameEngine';
/**
 * `ShowWindow` 的显示方式：本壳没有 `WinMain` 给它，取 `SW_SHOWNORMAL`。
 * ★ 这是**壳的选择**，⛔ 不是从引擎观测到的值（核心只负责"原样转发"）。
 */
const SHOW_MODE_NORMAL = 1;
/** `--smoke` 的总超时（毫秒） */
const SMOKE_TIMEOUT_MS = 60_000;

const USAGE = [
  '用法：',
  '  electron --no-sandbox apps/emulator-new --install <dir> --key <u32> [--root-id 0] [--config <file>]',
  '  electron --no-sandbox apps/emulator-new --demo',
  '  electron --no-sandbox apps/emulator-new --smoke',
  '',
  '  --install  真实入口（读 06-window 的顺序：窗口类 → 建窗 → 图形设备 → 根脚本 → 定时器 → 泵）',
  '  --demo     非游戏的可视化自检（真实 core handlers 驱动 0x1f8 / 0x204 / 0x1fb / 0x33f / 0x20d）',
  '  --smoke    同 --demo，截图到 .tmp/emulator-new-electron-smoke.png 后退出（退出码 0 / 1）',
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
}

/** 当前活跃的平台宿主（退出清理用；`null` = 没建过） */
let activeHost: ElectronWindowHost | null = null;

/* ──────────────────────────── 日志 / 参数 ──────────────────────────── */

function log(level: LogLevel, text: string): void {
  const line = `[electron] ${text}`;
  if (level === 'error') console.error(line);
  else if (level === 'warn') console.warn(line);
  else console.log(line);
}

function parseU32(text: string, what: string): number {
  const value = /^0x/i.test(text) ? Number.parseInt(text.slice(2), 16) : Number.parseInt(text, 10);
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw new Error(`${what} 必须是 u32：${text}`);
  return value >>> 0;
}

/**
 * 解析本程序的参数。
 * ★ `process.argv` 里有 Electron 自己的东西：`[electron 二进制, '--no-sandbox', '.', …我们的参数…]`
 *   ⇒ 位置参数（app 路径）跳过；Chromium 认识的开关（用 `app.commandLine.hasSwitch` 判）也跳过。
 *   ⛔ 不把 `--no-sandbox` 写死在代码里，而是"认得它属于 Chromium，不当成自己的参数"。
 */
function parseArgs(argv: string[]): Args {
  const out: Args = { install: null, key: null, rootId: 0, config: null, demo: false, smoke: false };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (!arg.startsWith('-')) continue; // Electron 的 app 路径 / 游离位置参数
    if (arg === '--install') out.install = argv[++i] ?? null;
    else if (arg === '--key') out.key = parseU32(argv[++i] ?? '', '--key');
    else if (arg === '--root-id') out.rootId = parseU32(argv[++i] ?? '', '--root-id');
    else if (arg === '--config') out.config = argv[++i] ?? null;
    else if (arg === '--demo') out.demo = true;
    else if (arg === '--smoke') out.smoke = true;
    else {
      const name = arg.replace(/^--?/, '').split('=')[0] ?? '';
      if (app.commandLine.hasSwitch(name)) continue; // Chromium / Electron 自己的开关
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  if (out.install !== null && (out.demo || out.smoke)) throw new Error('--install 与 --demo / --smoke 只能给一个');
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

/** 启动失败 / 异常的统一出口（退出码 1） */
function fail(text: string, err?: unknown): void {
  console.error(`[electron] ${text}`);
  if (err instanceof Error) console.error(err.stack ?? err.message);
  else if (err !== undefined) console.error(err);
  cleanup();
  app.exit(1);
}

/* ──────────────────────────── 真实入口：--install ──────────────────────────── */

function entry(): WindowEntry {
  return {
    // 进程实例 / 资源实例：Electron 侧都只是占位句柄（核心只原样转发，不解释）
    instance: PROCESS_INSTANCE,
    resourceInstance: RESOURCE_INSTANCE,
    showMode: SHOW_MODE_NORMAL,
  };
}

async function runInstall(args: Args): Promise<void> {
  const installDir = args.install;
  if (installDir === null) { fail('内部错误：--install 没有值'); return; }
  if (args.key === null) {
    console.error('[electron] --install 需要 --key <u32>（整型编码的运行期 key，规范不许有默认值）');
    console.error(USAGE);
    cleanup();
    app.exit(2);
    return;
  }

  const host = new ElectronWindowHost({ appRoot: APP_ROOT, log });
  activeHost = host;
  const table = await loadOpcodeTable();
  const configKeys = await loadConfigKeys();
  const env: CoreEnv = { host: nodeHost, table, configKeys, renderHost: host.renderHost };

  /** 窗口会话（`onBeforeScript` 建立；`onIteration` 用它泵消息） */
  let session: WindowSession | null = null;
  /** 主循环的 Machine（`onIteration` 看它的门位；等待时按墙钟睡，避免空转烧 CPU） */
  let machineRef: Machine | null = null;
  /** 上一次把 macrotask 让给 Electron 的时间（约 4ms 一次；见 `onIteration`） */
  let lastYieldAt = 0;
  /** 真实时钟原点：窗口前端用宿主毫秒时钟（否则 5s 渐变会被虚拟步长快进） */
  const clockOrigin = Date.now();

  try {
    const report = await analyzeInstall(
      env,
      installDir,
      args.rootId,
      {
        key: args.key,
        // ★ 宿主毫秒时钟：窗口前端的渐变 / 等待必须按墙钟走（CLI 仍用虚拟 tickMs）
        clock: () => Date.now() - clockOrigin,
        // ① 读完配置、读根脚本之前：窗口 / 图形设备阶段（到"图形设备就绪"为止）
        onBeforeScript: async (ctx) => {
          const booted = await bootWindow({
            host,
            config: ctx.config,
            entry: entry(),
            // 未取证的图形参数映射（RES-BOOT-GFX-PARAMS）：不覆盖核心缺省
            // 引擎级的"关闭收尾"内容未取证（RES-BOOT-CLOSE-CLEANUP）⇒ 不注入 ⇒ 核心显式记一条 ignored
          });
          session = booted;
          if (booted.failure !== null) {
            // ★ 共享出口：不再读根脚本、不跑脚本（06-window 的失败后果）
            const failure = booted.failure;
            throw new Error(
              `窗口 / 图形阶段失败：rule=${failure.rule} reason=${failure.reason} `
                + `step=${failure.step ?? '-'} errorText=${failure.errorText ?? '-'} `
                + `错误通道=${booted.errors.length > 0 ? booted.errors.join(' / ') : '空'}`,
            );
          }
          const size = host.clientSize();
          const init: RendererInit = {
            width: size.width,
            height: size.height,
            screenMode: booted.screenMode,
            // 与核心缺省一致（未注入 graphics 覆盖）：全屏呈现 = 屏幕形态字为 0
            fullscreenPresent: booted.screenMode === 0,
            background: 0,
          };
          host.renderHost.pushBoot(init, WINDOW_TITLE, booted.ignored.map((item) => `${item.rule} / ${item.what}`));
          log('info',
            `窗口 / 图形设备就绪：图形路线 ${booted.graphicsRoute ?? '?'} · 形态 ${booted.screenMode} · `
              + `客户区 ${size.width}x${size.height} · 错误通道 ${booted.errors.length} 条 · 显式忽略 ${booted.ignored.length} 条`);
        },
        // ② new Machine 之后、machine.run 之前：根脚本已装载 ⇒ 建周期定时器
        onMachine: async (machine) => {
          machineRef = machine;
          host.renderHost.attachMachine(machine);
          if (session === null) throw new Error('集成钩子次序错：onMachine 早于 onBeforeScript');
          session.beginMainLoop();
          log('info', '根脚本已装载 ⇒ 建周期定时器（标识 2 / 10000ms），进入主循环');
        },
        // ③ 迭代头：泵窗口 / 定时器消息（EngineExit 从这里穿出去）+ 节流推场景 + 呈现节拍
        onIteration: async () => {
          const current = session;
          if (current === null) return;
          current.pump();
          // ★ 呈现节拍是**平台侧的选择**：引擎不请求呈现（切换目标与呈现的先后未取证，
          //   `RES-RENDER-TARGET-ORDER`）⇒ 本壳按场景推送的节流（16ms）顺带 present 一次，
          //   否则窗口永远停在 init 清屏后的样子。⛔ 这不是 spec 结论。
          if (host.renderHost.syncScene()) void host.renderHost.present();
          // ★★ 等待门关着时按**墙钟**睡：`Machine.tick()` 读的是宿主时钟，空转会烧 CPU，
          //   也不会让 5 秒渐变更快结束。这里睡 4ms，门真到期才放行。
          if (machineRef !== null && (machineRef.flags & WAIT_BIT) !== 0) {
            await delay(4);
            lastYieldAt = Date.now();
            return;
          }
          // ★★ 让 macrotask：`Machine.run` 是紧循环；只 `await` 一个非 Promise 不会让
          //   `setInterval` / BrowserWindow 的 close 事件有机会跑 ⇒ `pump()` 永远看不到它们。
          //   约 4ms 让一次（不是每条指令都让，否则吞吐掉一个量级）。
          const now = Date.now();
          if (now - lastYieldAt >= 4) {
            lastYieldAt = now;
            await new Promise<void>((resolve) => { setTimeout(resolve, 0); });
          }
        },
      },
      { config: args.config },
    );
    printReport(report);
    log('info', '脚本执行结束（不是"退出消息"那条路径）⇒ 退出清理');
  } catch (err) {
    if (err instanceof EngineExit) {
      // ★ 正常退出：引擎级的退出异常，不是失败
      log('info', `收到引擎退出异常（EngineExit）⇒ 退出清理：${err.message}`);
      cleanup();
      app.quit();
      return;
    }
    fail('启动 / 执行失败（退出码 1）', err);
    return;
  }
  cleanup();
  app.quit();
}

function printReport(report: ScriptReport & { source: Record<string, unknown> }): void {
  const source = report.source;
  if (source.kind === 'install') {
    console.log(`[electron] 安装   ${String(source.dir)}`);
    console.log(`[electron] 脚本   id ${String(source.id)} · ${String(source.name)} · ${String(source.actualLength)} 字节`);
  }
  if (report.config !== undefined) {
    console.log(`[electron] 配置   ${report.config.path} · 来源 ${report.config.source}`);
  }
  console.log(`[electron] 执行   ${report.steps ?? 0} 条 · 停因 ${report.stopText ?? report.stop}`);
  console.log(`[electron] 时钟   ${report.ticks ?? 0} 格 · ${report.atMs ?? 0}ms · 诊断 ${report.diagnostics?.length ?? 0} 条`);
  if (report.entered && report.entered.length > 0) {
    console.log(`[electron] 进入过 ${report.entered.join(', ')}`);
  }
  if (report.gap !== null) {
    console.log(`[electron] 缺口   opcode 0x${report.gap.opcode.toString(16)} @ 第 ${report.gap.index} 条（${report.gap.reason}）`);
  }
}

/* ──────────────────────────── 可视化自检：--demo / --smoke ──────────────────────────── */

/**
 * demo 配置（⛔ **不是** spec 结论，只是一个能把窗口 + 图形设备阶段跑起来的合成配置）：
 * 窗口态 1280×720、三维设备路线（`set:CreateObject` 最低位 0）、纹理 1024×1024。
 */
const DEMO_CONFIG: readonly (readonly [string, number])[] = [
  ['display:ScreenMode', 1],
  ['set:WinX', 1280],
  ['set:WinY', 720],
  ['set:CreateObject', 0],
  ['set:TexWidth', 1024],
  ['set:TexHeight', 1024],
  ['display:FullScreenWidth', 1280],
  ['display:FullScreenHeight', 720],
];

/**
 * demo 文本画进纹理 1 的 (16,16)。
 * ★ 用 **CP932 字节串**（latin1 一字节一字符）写「天結いキャッスルマイスター」，
 *   这样 smoke 同时验证 Shift-JIS 解码 + Amayui CN 的日文字形；后半段 ASCII 便于对照。
 */
const DEMO_TEXT = String.fromCharCode(
  0x93, 0x56, 0x8c, 0x8b, 0x82, 0xa2, 0x83, 0x4c, 0x83, 0x83, 0x83, 0x62,
  0x83, 0x58, 0x83, 0x8b, 0x83, 0x7d, 0x83, 0x43, 0x83, 0x58, 0x83, 0x5e, 0x81, 0x5b,
) + ' / Amayui CN';
/** demo 文本所在的全局字符串槽（`0x204` 的第 4 个操作数按 type 5 读它） */
const DEMO_TEXT_SLOT = 1;

function intOperand(value: number): Operand {
  return { type: 0, raw: value >>> 0 };
}

/** 用**真实的 core handler** 跑一条指令（`machine.handlers` 就是 `HANDLERS`） */
async function applyOpcode(machine: Machine, opcode: number, ops: Operand[]): Promise<void> {
  const handler = machine.handlers.get(opcode);
  if (handler === undefined) throw new Error(`demo: 指令表里没有 opcode 0x${opcode.toString(16)} 的处理器`);
  // 分派器协议：handler 之前在「操作数个数」槽写 2·argc+1
  machine.operandCount = 2 * ops.length + 1;
  await handler(machine, ops);
}

/**
 * 在空 `Machine` 上驱动一条最小场景（全部走真实 handler）：
 *   0x1f8 建纹理 1（512×128）→ 0x204 往纹理 1 画 ASCII 文本 → 0x1fb 建绘制项（key 1 / texture 1 / 0,0,512,128）
 *   → 0x33f 设场景混合（0 / 255 / 0xffffff）→ 0x20d 把渲染目标切回后缓冲（≥1000 那一支）。
 * ★ 这同时验证了 `0x20d`（取表面 / 取后缓冲 / 设目标 / 归还）、`0x33f`（两个混合格）、
 *   `0x204`（三条入口条件 + `drawText` 请求）三条的真实 handler 路径。
 */
async function driveDemoScene(machine: Machine): Promise<void> {
  // texture 1：512×128、种类 1（normal）
  await applyOpcode(machine, 0x1f8, [intOperand(1), intOperand(512), intOperand(128), intOperand(1)]);
  machine.globalString.set(DEMO_TEXT_SLOT, DEMO_TEXT);
  // draw-text(纹理 1, x=16, y=16, 文本)
  await applyOpcode(machine, 0x204, [intOperand(1), intOperand(16), intOperand(16), { type: 5, raw: DEMO_TEXT_SLOT }]);
  // 绘制项 key 1：texture 1、矩形 (0,0)-(512,128)
  await applyOpcode(machine, 0x1fb, [
    intOperand(1), // 1: key
    intOperand(1), // 2: textureId
    intOperand(0), // 3: left
    intOperand(0), // 4: top
    intOperand(512), // 5: right 基准
    intOperand(128), // 6: bottom 基准
    intOperand(0), // 7: aux[0]
    intOperand(0), // 8: aux[1]
  ]);
  // 场景混合：模式 0 / 透明度 255 / 颜色 0xffffff
  await applyOpcode(machine, 0x33f, [intOperand(0), intOperand(255), intOperand(0xffffff)]);
  // 渲染目标 = 后缓冲（槽号 ≥ 1000 那一支）
  await applyOpcode(machine, 0x20d, [intOperand(1000)]);
}

async function runDemo(args: Args): Promise<void> {
  const host = new ElectronWindowHost({ appRoot: APP_ROOT, log });
  activeHost = host;
  const configKeys = await loadConfigKeys();
  const config = new ConfigStore(configKeys);
  for (const [key, value] of DEMO_CONFIG) config.setInt(key, value);

  let session: WindowSession;
  try {
    session = await bootWindow({ host, config, entry: entry() });
  } catch (err) {
    fail('demo：窗口 / 图形设备阶段抛错', err);
    return;
  }
  if (session.failure !== null) {
    fail(`demo：窗口 / 图形设备阶段失败（${session.failure.rule} / ${session.failure.reason} / ${session.failure.step ?? '-'}）`);
    return;
  }

  // 推送启动信息（与 `--install` 里同一个做法）：renderer 据此显示标题 / 形态 / **本层显式忽略的项**。
  // ★ 此时 renderer 已经 ready（`createDevice` 等过它）⇒ 推一条 `boot` 就能被收到。
  const size = host.clientSize();
  host.renderHost.pushBoot(
    {
      width: size.width,
      height: size.height,
      screenMode: session.screenMode,
      // 与核心缺省一致（未注入 graphics 覆盖）：全屏呈现 = 屏幕形态字为 0
      fullscreenPresent: session.screenMode === 0,
      background: 0,
    },
    WINDOW_TITLE,
    session.ignored.map((item) => `${item.rule} / ${item.what}`),
  );

  const render = host.renderHost;
  // 空 Machine：body 空、指令表空、key 0 —— 只为拿它的 scene / render 状态与真实 handler
  const machine = new Machine({ body: new Uint8Array(0), table: new Map(), key: 0, renderHost: render });
  render.attachMachine(machine);
  try {
    await driveDemoScene(machine);
  } catch (err) {
    fail('demo：驱动场景失败', err);
    return;
  }
  render.syncScene({ force: true });
  // ★ renderer 只在 present 时真的画一帧（平台侧选择；引擎不请求呈现，见 `RES-RENDER-TARGET-ORDER`）
  await render.present();
  log('info',
    `demo 场景：纹理 ${machine.scene.textureObjects.size} 个 · 绘制项 ${machine.scene.drawItems.size} 个 · `
      + `目标 ${machine.render.target} · 错误缓冲 ${machine.scene.errorBuffer === '' ? '空' : machine.scene.errorBuffer}`);

  if (args.smoke) {
    await runSmoke(host);
    return;
  }
  host.allowUserClose();
  log('info', '--demo：这是**非游戏**的可视化自检（不是 spec 结论）；关闭窗口即退出');
}

async function delay(ms: number): Promise<void> {
  await new Promise<void>((resolve) => { setTimeout(resolve, ms); });
}

async function runSmoke(host: ElectronWindowHost): Promise<void> {
  const watchdog = setTimeout(() => {
    console.error(`[electron] --smoke 超时（${SMOKE_TIMEOUT_MS}ms）`);
    cleanup();
    app.exit(1);
  }, SMOKE_TIMEOUT_MS);
  try {
    const ready = await host.renderHost.waitForReady();
    if (ready === null) throw new Error('renderer 没有在超时内上报 ready');
    // 再推两次场景并各 present 一次：renderer 可能已经画了一帧，保险起见让它拿到最新快照
    host.renderHost.syncScene({ force: true });
    await host.renderHost.present();
    await delay(400);
    host.renderHost.syncScene({ force: true });
    await host.renderHost.present();
    await delay(600);
    const win = host.browserWindow();
    if (win === null) throw new Error('没有窗口可截屏');
    const image = await win.webContents.capturePage();
    if (image.isEmpty()) throw new Error('capturePage 返回空图像');
    const file = path.join(REPO_ROOT, '.tmp', 'emulator-new-electron-smoke.png');
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    await fs.promises.writeFile(file, image.toPNG());
    const size = image.getSize();
    log('info', `--smoke OK：backend=${ready.backend} · 截图 ${file}（${size.width}x${size.height}）`);
    clearTimeout(watchdog);
    cleanup();
    app.exit(0);
  } catch (err) {
    clearTimeout(watchdog);
    console.error('[electron] --smoke 失败：', err instanceof Error ? err.message : err);
    cleanup();
    app.exit(1);
  }
}

/* ──────────────────────────── 说明窗口（没有参数时） ──────────────────────────── */

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
<pre>electron --no-sandbox apps/emulator-new --install &lt;dir&gt; --key &lt;u32&gt; [--root-id 0] [--config &lt;file&gt;]
electron --no-sandbox apps/emulator-new --demo     # 非游戏的可视化自检（0x1f8 / 0x204 / 0x1fb / 0x33f / 0x20d）
electron --no-sandbox apps/emulator-new --smoke    # 同一套 + 截图到 .tmp/emulator-new-electron-smoke.png</pre>
<p class="note">--demo / --smoke 需要先构建渲染进程（<code>vite build</code> ⇒ <code>dist/renderer/index.html</code>）。<br>
关闭这个窗口即退出。</p>
</html>`;
  void win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`);
  log('info', '没有启动参数 ⇒ 打开说明窗口（不调用 bootWindow）');
}

/* ──────────────────────────── 进程生命周期 ──────────────────────────── */

// ★ userData 必须在 app ready 之前设置：否则 Chromium 会往默认目录（~/Library/Application Support/…）写缓存
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
