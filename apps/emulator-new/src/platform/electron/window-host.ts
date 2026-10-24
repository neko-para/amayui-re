/**
 * Electron **主进程的窗口壳**：只负责 BrowserWindow / 显示器 / 生命周期 / IPC 桥。
 *
 * ★ 2026-10 架构调整后，core（安装取件、配置、启动顺序、VM、场景）**整份在 renderer 里**。
 *   本文件不再实现 `core/window.ts` 的 `BootHost`，也不再持有 `Machine`：
 *   - renderer 通过 `window.amayui.invoke('boot.*')` 发窗口请求；
 *   - main 把 BrowserWindow 的事件（close / focus / blur）推给 renderer；
 *   - renderer 的 core 在本地 `pump()` 消费这些消息。
 *
 * ★ 为什么不是"core 在 main、renderer 只画"：这样每次 `0x20d` / `0x204` / 场景快照都要跨 IPC，
 *   而它们本来就是逐指令发生的；把 VM + 场景 + Pixi 放在同一进程可以省掉这条热路径，
 *   也让 `RenderHost` 的不透明句柄不必跨线。
 *
 * ★ 本文件**不认识 spec / 游戏语义**：它只转 `boot.*` 的领域参数。
 * ★ 窗口必须**先于 renderer 存在**（renderer 本身要跑在这个 BrowserWindow 里）：
 *   `open()` 建一个隐藏的 game bootstrap 窗口加载页面；core 的 `createWindow` 再把同一窗口
 *   调整成配置要求的尺寸 / 形态 / 标题并 `show()`。
 * ★ 另外建一个**独立 debug 窗口**（`?debug=1`）：只读显示 game renderer 的 diagnostics 投影；
 *   它不跑 core，关闭它不影响 game。game 窗口则保持只有 canvas。
 */
import { app, BrowserWindow, ipcMain, powerSaveBlocker, screen } from 'electron';
import type { IpcMainEvent } from 'electron';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

import type {
  BridgeMethod,
  DebugEvent,
  PlatformInfo,
  RendererEvent,
  RendererReady,
  StartJob,
  WindowMessage,
} from './protocol.ts';
import { nodeHost } from '../node/host.ts';

export type LogLevel = 'info' | 'warn' | 'error';
export type LogFn = (level: LogLevel, text: string) => void;

const CHANNEL_BRIDGE_INVOKE = 'bridge:invoke';
const CHANNEL_BRIDGE_START = 'bridge:start';
const CHANNEL_BRIDGE_WINDOW = 'bridge:window';
const CHANNEL_RENDERER_EVENT = 'renderer:event';
const CHANNEL_DEBUG_EVENT = 'debug:event';

const WINDOW_TITLE = 'ARCGameEngine';
const SINGLE_HANDLE = 1;
const READY_TIMEOUT_MS = 15_000;

/** 未取证的窗口标题内容（spec 的 `RES-BOOT-WINDOW-TITLE`）：与窗口类名分开，仅作标题占位 */
const BOOTSTRAP_WIDTH = 1280;
const BOOTSTRAP_HEIGHT = 720;

export interface ElectronWindowHostOptions {
  appRoot: string;
  preloadPath?: string;
  log?: LogFn;
  /** 手动开闸的命令种类（拼进 renderer URL 的 `?commandExecKinds=`；缺省不拼） */
  commandExecKinds?: number[];
}

/**
 * main 侧窗口壳。
 * ★ `open()` 必须在使用任何 `boot.*` 请求前调用；`dispose()` 幂等。
 */
export class ElectronWindowHost {
  private readonly appRoot: string;
  private readonly preloadPath: string;
  private readonly log: LogFn;
  private readonly rendererQuery: string;
  private win: BrowserWindow | null = null;
  /** 独立 debug 窗口（只读诊断投影；不跑 core） */
  private debugWin: BrowserWindow | null = null;
  private destroyed = false;
  private loadFailure: string | null = null;
  /** 用户点关闭时是否拦下来交给 renderer 的窗口过程（demo / smoke 放行） */
  private preventClose = true;
  /** 关闭请求交给 renderer 后，若主循环已经结束 / 卡住，强制收尾的兜底定时器 */
  private closeFallback: ReturnType<typeof setTimeout> | null = null;
  private powerSaveBlockerId: number | null = null;
  /** 第一次 show 是否已真正执行（避免重复 show / 抢在首帧前显示） */
  private shown = false;
  private pendingShow = false;
  private firstFrameSeen = false;
  private readyToShow = false;
  private showFallback: ReturnType<typeof setTimeout> | null = null;

  private readonly readyPromise: Promise<RendererReady>;
  private resolveReady: (info: RendererReady) => void = () => {};
  private readyInfo: RendererReady | null = null;

  private readonly donePromise: Promise<Extract<RendererEvent, { kind: 'done' }>>;
  private resolveDone: (event: Extract<RendererEvent, { kind: 'done' }>) => void = () => {};
  private doneEvent: Extract<RendererEvent, { kind: 'done' }> | null = null;

  private readonly demoReadyPromise: Promise<void>;
  private resolveDemoReady: () => void = () => {};
  private demoReady = false;

  private readonly onEvent = (event: IpcMainEvent, payload: RendererEvent): void => {
    // 只接受 **game** renderer 的事件；debug 窗口不跑 core，也不许把它的日志混进来。
    if (this.win === null || event.sender !== this.win.webContents) return;
    this.handleEvent(payload);
  };

  constructor(options: ElectronWindowHostOptions) {
    this.appRoot = options.appRoot;
    this.preloadPath = options.preloadPath ?? path.join(options.appRoot, 'src', 'platform', 'electron', 'preload.ts');
    this.rendererQuery = options.commandExecKinds && options.commandExecKinds.length > 0
      ? '?commandExecKinds=' + options.commandExecKinds.join(',')
      : '';
    this.log = options.log ?? ((level, text) => {
      if (level === 'error') console.error(text);
      else if (level === 'warn') console.warn(text);
      else console.log(text);
    });
    this.readyPromise = new Promise<RendererReady>((resolve) => { this.resolveReady = resolve; });
    this.donePromise = new Promise((resolve) => { this.resolveDone = resolve; });
    this.demoReadyPromise = new Promise<void>((resolve) => { this.resolveDemoReady = resolve; });

    ipcMain.handle(CHANNEL_BRIDGE_INVOKE, async (_event, method: BridgeMethod, params: unknown) => {
      return await this.dispatch(method, params);
    });
    ipcMain.on(CHANNEL_RENDERER_EVENT, this.onEvent);
  }

  /* ──────────────────────────── 生命周期 ──────────────────────────── */

  /**
   * 建 **debug 窗口** + 隐藏的 game 窗口，并让 game 窗口加载 renderer 页面。
   * ★ debug 窗口先建、立刻显示：它就是“之前调试用的界面”的独立版本；
   *   game 窗口保持隐藏，等 core 的 `createWindow` 调整尺寸 / 形态后再 `show()`。
   * ★ 两个窗口共用同一个 preload；debug 页面走 `?debug=1` 分支，只监听 `debug:event`。
   */
  async open(): Promise<void> {
    if (this.win !== null) return;
    const htmlPath = path.join(this.appRoot, 'dist', 'renderer', 'index.html');
    if (!fs.existsSync(htmlPath)) {
      this.loadFailure = `missing-renderer-bundle:${htmlPath}`;
      throw new Error(`${this.loadFailure} —— 先构建渲染进程（vite build）`);
    }

    // ★ 临时禁用 debug 窗口：它是第二个 BrowserWindow，会独立参与 GPU/合成，
    //   也是启动阶段“右上角白色东西”的嫌疑源。先排除它对白闪的影响。
    this.debugWin = null;

    const win = new BrowserWindow({
      // ★ 旧模拟器口径：创建即可见（黑底）。不再有隐藏→show 的状态转换。
      show: true,
      width: BOOTSTRAP_WIDTH,
      height: BOOTSTRAP_HEIGHT,
      useContentSize: true,
      // ★ 旧模拟器固定窗口；可缩放窗口在 show/布局时会多一次非客户区状态。
      resizable: false,
      backgroundColor: '#000000',
      title: WINDOW_TITLE,
      autoHideMenuBar: true,
      webPreferences: {
        preload: this.preloadPath,
        contextIsolation: true,
        nodeIntegration: false,
        // preload 要 import electron 并跑 Node 类型剥壳 ⇒ 不能开 sandbox
        sandbox: false,
        // 画面是游戏主循环驱动的；被遮挡时也不要停 rAF/合成。
        backgroundThrottling: false,
      },
    });
    this.win = win;
    // ★ 旧模拟器口径：窗口创建即可见（黑底），不在“首帧/init 完成后”再 show。
    //   一次性 show/激活发生在任何游戏内容之前，避免它在后续页面附近插入一帧。
    win.show();
    this.shown = true;
    this.destroyed = false;
    // ★ ready-to-show 之前 show 会露出未合成的白底（Windows 实测）：先记着，等首帧可显示再 show。
    win.once('ready-to-show', () => {
      this.readyToShow = true;
      // ★ 不在这里 show：ready-to-show 只代表页面已加载，WebGL 还没提交第一帧。
      //   等 renderer 的 host.firstFrame 再显示，避开“窗口可见但无内容”的 DWM 白底竞态。
    });

    win.on('close', (event) => {
      if (!this.destroyed && this.preventClose) {
        event.preventDefault();
        this.sendWindowMessage({ kind: 'close' });
        // ★ macOS 的关闭按钮只把窗口藏起来；而这个实现把 close 转给 renderer 的窗口过程，
        //   由它 hideWindow + postQuitMessage。若 VM 已经结束（例如停在未实现 opcode）或
        //   renderer 不再泵消息，close 就永远没有消费者，进程会一直存活（Windows 上同样
        //   可能发生，只是时序不同）。这里给“交给 renderer”一个兜底：超时仍未 done 就
        //   destroy + quit；正常路径下 done 会先到，dispose() 会清掉定时器。
        this.scheduleCloseFallback();
        return;
      }
      // demo / smoke / 已经完成销毁：放行；`window-all-closed` 会退出。
    });
    win.on('focus', () => { this.sendWindowMessage({ kind: 'activate', active: true }); });
    win.on('blur', () => { this.sendWindowMessage({ kind: 'activate', active: false }); });
    win.on('closed', () => {
      const wasDestroyed = this.destroyed;
      this.clearCloseFallback();
      this.destroyed = true;
      this.win = null;
      this.log('info', 'game 窗口已关闭（closed）');
      if (!wasDestroyed) this.failDone('window-closed-before-renderer-done');
      const debug = this.debugWin;
      if (debug !== null && !debug.isDestroyed()) debug.close();
    });
    win.webContents.on('did-fail-load', (_event, code, description, url, isMainFrame) => {
      if (!isMainFrame) return;
      this.loadFailure = `did-fail-load:${code}:${description}（${url}）`;
      this.log('error', `renderer 页面加载失败：${this.loadFailure}`);
    });
    win.webContents.on('render-process-gone', (_event, details) => {
      this.loadFailure = `render-process-gone:${details.reason}`;
      this.log('error', `渲染进程结束：${this.loadFailure}`);
      this.failDone(this.loadFailure);
    });

    try {
      await win.loadURL(pathToFileURL(htmlPath).toString() + this.rendererQuery);
    } catch (err) {
      this.loadFailure = `load-rejected:${err instanceof Error ? err.message : String(err)}`;
      throw new Error(this.loadFailure);
    }
    this.log('info', `game bootstrap 窗口已加载 renderer（${BOOTSTRAP_WIDTH}x${BOOTSTRAP_HEIGHT}）`);
  }

  /** 等 renderer 的 Pixi / WebGL ready（超时 ⇒ `null`，不抛） */
  async waitForReady(timeoutMs = READY_TIMEOUT_MS): Promise<RendererReady | null> {
    if (this.readyInfo !== null) return this.readyInfo;
    return await new Promise<RendererReady | null>((resolve) => {
      let done = false;
      const timer = setTimeout(() => {
        if (done) return;
        done = true;
        this.log('error', `renderer ${timeoutMs}ms 内没有上报 ready`);
        resolve(null);
      }, timeoutMs);
      void this.readyPromise.then((info) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        resolve(info);
      });
    });
  }

  /** 把启动指令发给 renderer（`onStart` handler 已在 ready 前注册） */
  start(job: StartJob): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) throw new Error('window-host: 没有窗口，不能发 start');
    win.webContents.send(CHANNEL_BRIDGE_START, job);
  }

  /** 等 renderer 的 `done`（install 的脚本结束 / demo 的 smoke 结束） */
  async waitForDone(): Promise<Extract<RendererEvent, { kind: 'done' }>> {
    if (this.doneEvent !== null) return this.doneEvent;
    return await this.donePromise;
  }

  /** 等 demo 场景 present 完（`--demo` 非 smoke 也会到这一步） */
  async waitForDemoReady(): Promise<void> {
    if (this.demoReady) return;
    await this.demoReadyPromise;
  }

  browserWindow(): BrowserWindow | null {
    return this.destroyed ? null : this.win;
  }

  /** 独立 debug 窗口（可能已被用户关闭） */
  debugWindow(): BrowserWindow | null {
    return this.debugWin;
  }

  /** 客户区尺寸（当前 BrowserWindow；没建窗时退回主显示器尺寸） */
  clientSize(): { width: number; height: number } {
    const win = this.browserWindow();
    if (win !== null && !win.isDestroyed()) {
      const [width, height] = win.getContentSize();
      return { width, height };
    }
    const size = screen.getPrimaryDisplay().size;
    return { width: size.width, height: size.height };
  }

  /** 放行关闭（demo / smoke：没有引擎主循环去消费 close） */
  allowClose(): void {
    this.preventClose = false;
  }

  private scheduleCloseFallback(): void {
    if (this.closeFallback !== null) return;
    this.closeFallback = setTimeout(() => {
      this.closeFallback = null;
      if (this.destroyed) return;
      this.log('warn', '关闭兜底：renderer 未在超时内完成 close（主循环可能已结束）⇒ 强制销毁窗口并退出');
      this.dispose();
      app.quit();
    }, 1500);
  }

  private clearCloseFallback(): void {
    if (this.closeFallback === null) return;
    clearTimeout(this.closeFallback);
    this.closeFallback = null;
  }

  /** 退出清理（幂等） */
  dispose(): void {
    this.clearCloseFallback();
    this.destroyed = true;
    ipcMain.removeHandler(CHANNEL_BRIDGE_INVOKE);
    ipcMain.removeListener(CHANNEL_RENDERER_EVENT, this.onEvent);
    if (this.powerSaveBlockerId !== null) {
      if (powerSaveBlocker.isStarted(this.powerSaveBlockerId)) powerSaveBlocker.stop(this.powerSaveBlockerId);
      this.powerSaveBlockerId = null;
    }
    const debug = this.debugWin;
    this.debugWin = null;
    if (debug !== null && !debug.isDestroyed()) debug.destroy();
    const win = this.win;
    this.win = null;
    if (win !== null && !win.isDestroyed()) win.destroy();
  }

  /** renderer 的同步部分需要的平台信息（路径分隔符 / 家目录 / 两个配置根环境变量） */
  platformInfo(): PlatformInfo {
    return {
      sep: path.sep,
      homeDir: nodeHost.homeDir(),
      env: {
        APPDATA: nodeHost.env('APPDATA'),
        XDG_CONFIG_HOME: nodeHost.env('XDG_CONFIG_HOME'),
      },
    };
  }

  /* ──────────────────────────── renderer 事件 ──────────────────────────── */

  private handleEvent(event: RendererEvent): void {
    switch (event.kind) {
      case 'ready':
        if (this.readyInfo === null) {
          this.readyInfo = event.info;
          this.log('info', `renderer ready：backend=${event.info.backend} ${event.info.width}x${event.info.height}`);
          this.resolveReady(event.info);
        }
        return;
      case 'log':
        this.log(event.level, `[renderer] ${event.text}`);
        this.sendDebug({ kind: 'log', level: event.level, text: event.text });
        return;
      case 'status':
        this.log(event.level, `[renderer] ${event.text}`);
        this.sendDebug({ kind: 'status', level: event.level, text: event.text });
        return;
      case 'boot':
        this.log('info', `boot：${event.info.width}x${event.info.height} · screenMode ${event.info.screenMode}`);
        this.sendDebug({ kind: 'boot', info: event.info });
        return;
      case 'diagnostics':
        this.sendDebug({ kind: 'diagnostics', data: event.data });
        return;
      case 'demo-ready':
        if (!this.demoReady) {
          this.demoReady = true;
          this.resolveDemoReady();
        }
        return;
      case 'done':
        if (this.doneEvent === null) {
          this.doneEvent = event;
          this.resolveDone(event);
        }
        return;
    }
  }

  /** renderer 没发 done 就消失（崩溃 / 被关）：让等待方拿到失败，不要挂死。 */
  private failDone(error: string): void {
    if (this.doneEvent !== null) return;
    this.doneEvent = { kind: 'done', ok: false, error };
    this.resolveDone(this.doneEvent);
  }

  /** game renderer 的只读投影 → debug 窗口（debug 不跑 core、不回写状态） */
  private sendDebug(event: DebugEvent): void {
    const debug = this.debugWin;
    if (debug === null || debug.isDestroyed()) return;
    debug.webContents.send(CHANNEL_DEBUG_EVENT, event);
  }

  private sendWindowMessage(message: WindowMessage): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) return;
    win.webContents.send(CHANNEL_BRIDGE_WINDOW, message);
  }

  /* ──────────────────────────── IPC 请求分发 ──────────────────────────── */

  private async dispatch(method: BridgeMethod, params: unknown): Promise<unknown> {
    const p = (params ?? {}) as Record<string, unknown>;
    switch (method) {
      case 'host.readBytes':
        return await nodeHost.readBytes(String(p.path));
      case 'host.readBytesMaybe':
        return await nodeHost.readBytesMaybe(String(p.path));
      case 'host.readSlice':
        return await nodeHost.readSlice(String(p.path), Number(p.offset), Number(p.length));
      case 'host.readText':
        return await nodeHost.readText(String(p.path));
      case 'host.isFile':
        return await nodeHost.isFile(String(p.path));
      case 'host.listDir':
        return await nodeHost.listDir(String(p.dir));
      case 'host.ensureReadDir':
        return await nodeHost.ensureReadDir(String(p.dir));
      case 'host.firstFrame':
        this.firstFrameSeen = true;
        if (this.pendingShow) void this.showNow();
        return undefined;
      case 'host.writeBytes': {
        const data = p.data;
        if (!(data instanceof Uint8Array)) throw new Error('host.writeBytes: data 必须是 Uint8Array');
        return await nodeHost.writeBytes(String(p.path), data);
      }
      case 'boot.frameMetrics':
        return this.frameMetrics();
      case 'boot.screenSize':
        return this.screenSize();
      case 'boot.registerWindowClass':
        return this.registerWindowClass(p);
      case 'boot.createWindow':
        return this.createWindow(p);
      case 'boot.showWindow':
        this.showWindow();
        return undefined;
      case 'boot.updateWindow':
        this.updateWindow();
        return undefined;
      case 'boot.associateInputContext':
        this.associateInputContext();
        return undefined;
      case 'boot.getMenu':
        return this.getMenu();
      case 'boot.operateMenu':
        this.operateMenu(p);
        return undefined;
      case 'boot.drawMenuBar':
        this.drawMenuBar();
        return undefined;
      case 'boot.isForegroundWindow':
        return this.isForegroundWindow();
      case 'boot.hideWindow':
        this.hideWindow();
        return undefined;
      case 'boot.destroyWindow':
        this.destroyWindow();
        return undefined;
      case 'boot.postQuitMessage':
        this.log('info', `postQuitMessage(${String(p.exitCode ?? 0)})（renderer 本地队列；main 只记账）`);
        return undefined;
      case 'boot.postWindowMessage':
        this.log('info', `postWindowMessage(${String(p.message)})（renderer 本地队列；main 只记账）`);
        return undefined;
      case 'boot.setThreadExecutionState':
        this.setThreadExecutionState();
        return undefined;
      case 'boot.allowClose':
        this.allowClose();
        return undefined;
      default: {
        const unknown = method as string;
        throw new Error(`bridge-method-unknown:${unknown}`);
      }
    }
  }

  /* ──────────────────────────── boot.* 实现 ──────────────────────────── */

  private frameMetrics(): { frameWidth: number; frameHeight: number; captionHeight: number; menuHeight: number } {
    this.log('warn', '[ignored] frame-metrics —— Electron 用 useContentSize 让平台管外框；四类单边厚度全 0（只记账）');
    return { frameWidth: 0, frameHeight: 0, captionHeight: 0, menuHeight: 0 };
  }

  private screenSize(): { width: number; height: number } {
    const size = screen.getPrimaryDisplay().size;
    return { width: size.width, height: size.height };
  }

  private registerWindowClass(params: Record<string, unknown>): number {
    this.log('info', `registerWindowClass(${String(params.className ?? '?')}) → 逻辑类句柄 ${SINGLE_HANDLE}（无真实窗口类）`);
    return SINGLE_HANDLE;
  }

  /**
   * 把 bootstrap 窗口调整成 core 要的形态。
   * ★ 这是 core 在 renderer 里之后唯一可行的做法：窗口必须先存在才能加载 renderer；
   *   `createWindow` 不再"新建"，而是**配置同一个窗口**。
   */
  private createWindow(params: Record<string, unknown>): number {
    const win = this.win;
    if (win === null || win.isDestroyed()) throw new Error('boot.createWindow: bootstrap 窗口不存在');
    const clientWidth = Number(params.clientWidth);
    const clientHeight = Number(params.clientHeight);
    const fullscreen = Number(params.exStyle) !== 0;
    const x = Number(params.x);
    const y = Number(params.y);
    const defaultPosition = (x >>> 0) === 0x80000000 && (y >>> 0) === 0x80000000;

    // ★ 只在值真的不同时才改窗口：setContentSize / setFullScreen / setTitle 都会触发
    //   一次非客户区重绘（DWM 可能先画一帧默认白）。窗口构造时已是 1280x720 + 标题，
    //   这里的调用在旧模拟器里不存在。
    if (!fullscreen) {
      const [currentWidth, currentHeight] = win.getContentSize();
      if (currentWidth !== clientWidth || currentHeight !== clientHeight) {
        win.setContentSize(clientWidth, clientHeight);
      }
      if (!defaultPosition) {
        const [currentX, currentY] = win.getPosition();
        if (currentX !== x || currentY !== y) win.setPosition(x, y);
      }
    }
    if (win.isFullScreen() !== fullscreen) win.setFullScreen(fullscreen);
    this.log('info', `createWindow：客户区 ${clientWidth}x${clientHeight} · fullscreen=${fullscreen} · 逻辑句柄 ${SINGLE_HANDLE}`);
    return SINGLE_HANDLE;
  }

  private showWindow(): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) return;
    if (this.shown) {
      this.log('info', 'showWindow → 窗口创建时已可见（无 show 状态转换）');
      return;
    }
    // ★ 即使 ready-to-show 已到，也等第一帧真正提交后再显示。
    // ready-to-show 还没到：先挂起。若事件一直不来（隐藏窗口被节流），300ms 后兜底显示。
    this.pendingShow = true;
    if (this.showFallback === null) {
      this.showFallback = setTimeout(() => {
        this.showFallback = null;
        if (this.pendingShow) void this.showNow();
      }, 5000);
    }
    this.log('info', 'showWindow → 等 ready-to-show 后再显示（避免白底）');
  }

  /**
   * 窗口级白闪取证（逐帧）：beginFrameSubscription 每收到一帧 compositor 结果就采样，
   * 不靠 40ms 轮询（capturePage 本身就要几十 ms，短白闪会漏）。
   * 四角+中心 ≥4 点接近白才存 PNG；最多 5 张。
   */
  private startWindowFlashProbe(win: BrowserWindow): void {
    const until = Date.now() + 120000;
    let hits = 0;
    let frames = 0;
    const onFrame = (image: Electron.NativeImage): void => {
      frames += 1;
      if (frames % 60 === 0) this.log('info', `win-flash：已收到 ${frames} 帧`);
      if (Date.now() > until || hits >= 5 || win.isDestroyed()) {
        try { win.webContents.endFrameSubscription(); } catch { /* ignore */ }
        return;
      }
      try {
        const size = image.getSize();
        const bitmap = image.getBitmap() as unknown as Uint8Array;
        if (size.width <= 0 || size.height <= 0 || bitmap.length < 4) return;
        const points = [[0.5, 0.5], [0.1, 0.1], [0.9, 0.1], [0.1, 0.9], [0.9, 0.9]];
        let white = 0;
        for (const [px, py] of points) {
          const x = Math.min(size.width - 1, Math.floor(size.width * px!));
          const y = Math.min(size.height - 1, Math.floor(size.height * py!));
          const off = (y * size.width + x) * 4;
          if ((bitmap[off] ?? 0) >= 235 && (bitmap[off + 1] ?? 0) >= 235 && (bitmap[off + 2] ?? 0) >= 235) white += 1;
        }
        if (white < 4) return;
        hits += 1;
        const file = path.join(path.resolve(this.appRoot, '..', '..', '.tmp'), 'win-flash-' + hits + '.png');
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, image.toPNG());
        this.log('warn', 'win-flash：逐帧采样白帧 ' + file);
      } catch { /* 取证失败不影响窗口 */ }
    };
    try {
      win.webContents.beginFrameSubscription(false, onFrame);
      this.log('info', 'win-flash：逐帧 compositor 探针已开启（120s / 最多 5 张）');
      setTimeout(() => {
        try { win.webContents.endFrameSubscription(); } catch { /* ignore */ }
      }, 120000);
    } catch (err) {
      this.log('warn', 'win-flash：beginFrameSubscription 失败：' + (err instanceof Error ? err.message : String(err)));
    }
  }


  /** 真正 show；只执行一次。★ 先等到首帧是黑（capturePage 采样）再显示。 */
  private async showNow(): Promise<void> {
    const win = this.win;
    if (win === null || win.isDestroyed() || this.shown) return;
    this.shown = true;
    this.pendingShow = false;
    if (this.showFallback !== null) {
      clearTimeout(this.showFallback);
      this.showFallback = null;
    }
    // ★ 不做任何 capturePage：隐藏窗口的 capturePage 会异步触发一次合成/读回，
    //   完成时机不可控，可能在后续页面渐变附近插入一帧白。
    // ★ 不用 show()+focus()：激活/焦点切换会让 DWM 先合成一帧窗口底色。
    win.setBackgroundColor('#000000');
    win.showInactive();
    // ★ 不主动 focus：聚焦/激活是另一个一次性的 DWM 合成时机。用户点击窗口时自然获得焦点。
    this.log('info', 'showWindow → showInactive 显示 bootstrap 窗口（不主动 focus）');
  }

  /**
   * 一次异步截图取证：只记中心像素，**不阻塞 show**。
   * ★ 隐藏窗口的 capturePage 可能拖到首帧甚至更晚 ⇒ 绝不能拿它当 show 的前置。
   */
  private async sampleFirstPaint(win: BrowserWindow): Promise<void> {
    try {
      const image = await win.webContents.capturePage();
      const size = image.getSize();
      const bitmap = image.getBitmap() as unknown as Uint8Array;
      if (size.width <= 0 || size.height <= 0 || bitmap.length < 4) return;
      const x = Math.floor(size.width / 2);
      const y = Math.floor(size.height / 2);
      const off = (y * size.width + x) * 4;
      const b = bitmap[off] ?? 0;
      const g = bitmap[off + 1] ?? 0;
      const r = bitmap[off + 2] ?? 0;
      const a = bitmap[off + 3] ?? 0;
      this.log('info', `first-paint 中心像素 rgba(${r},${g},${b},${a})`);
    } catch (err) {
      this.log('warn', `first-paint 采样失败：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  private updateWindow(): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) return;
    try {
      win.webContents.invalidate();
    } catch (err) {
      this.log('warn', `updateWindow → invalidate 失败：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  private associateInputContext(): void {
    this.log('warn', '[ignored] window-attach.associate-input-context —— Electron 没有可解绑的输入法上下文');
  }

  private getMenu(): null {
    this.log('warn', '[ignored] window-attach.get-menu —— Electron 没有窗口菜单句柄');
    return null;
  }

  private operateMenu(params: Record<string, unknown>): void {
    this.log('warn', `[ignored] window-attach.operate-menu —— 辅助模块菜单入口不存在（category=${String(params.category ?? '?')}）`);
  }

  private drawMenuBar(): void {
    this.log('warn', '[ignored] window-attach.draw-menu-bar —— 没有菜单栏可重画');
  }

  private isForegroundWindow(): boolean {
    const win = this.win;
    return win !== null && !win.isDestroyed() && win.isFocused();
  }

  private hideWindow(): void {
    const win = this.win;
    if (win !== null && !win.isDestroyed()) win.hide();
  }

  /**
   * core 的 destroy 步骤：先隐藏，再把 `{kind:'destroy'}` 推回 renderer。
   * ★ 不在这里 `win.destroy()`：renderer 还要执行 `postQuitMessage` 并上报 `done`；
   *   真正的销毁在 main 收到 done / 用户关闭后由 `dispose()` 完成。
   */
  private destroyWindow(): void {
    this.hideWindow();
    this.sendWindowMessage({ kind: 'destroy' });
    this.log('info', "destroyWindow → 隐藏窗口并入队 {kind:'destroy'}");
  }

  private setThreadExecutionState(): void {
    if (this.powerSaveBlockerId !== null) return;
    this.powerSaveBlockerId = powerSaveBlocker.start('prevent-display-sleep');
    this.log('info', "setThreadExecutionState → powerSaveBlocker.start('prevent-display-sleep')");
  }
}
