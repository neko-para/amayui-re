/**
 * `ElectronWindowHost` —— `core/window.ts` 的 `BootHost`（`WindowHost & MessageHost & GraphicsHost`）
 * 在 **Electron 主进程**上的实现，外加一个**同步消息队列**模拟窗口过程的消息流。
 *
 * ★ 结构：
 *   - **WindowHost（同步）**：`BrowserWindow` 建窗 / 显示 / 隐藏 / 销毁；窗口类是**逻辑记账**
 *     （Electron 没有窗口类）；菜单 / 输入法 / 图标 / 光标在 Electron 侧**没有对应物** ⇒ 显式 ignored；
 *   - **MessageHost（同步）**：数组队列 + 退出哨兵。`BrowserWindow` 的事件（close / focus / blur）
 *     与 `setInterval` 的定时器到期都**入队**，由核心的 `WindowSession.pump()`（窥视 → 取一条 →
 *     翻译 → 派发）在脚本迭代头消费；
 *   - **GraphicsHost（async）**：两条互斥路线。本实现**只有 Pixi / WebGL 一条后端** ⇒
 *     旧式绘制路线（`set:CreateObject` 最低位 = 1）**显式失败**（⛔ 不偷偷用 Pixi 假装旧式路线）；
 *     三维设备路线把 `createDevice` 翻译成"等 renderer ready + 发 `init`"。
 *
 * ★ 为什么 `createDevice` 要等 renderer：真的设备（WebGL 上下文 / `RenderTexture` 能力）
 *   只有渲染进程初始化完才知道有没有。同步返一个假句柄会让
 *   `CORE-BOOT-GRAPHICS-INIT` 的"失败 ⇒ 写错误通道 + 结束启动"退化成事后补救。
 *
 * ★ **显式忽略**（`this.ignored`，逐条带原因）：图标 / 光标 / 背景刷 / 类风格位 / 顶部菜单 /
 *   IME 解绑 / 菜单安装 / 重画菜单栏 / **鼠标输入转发** / 外框尺寸求和。
 *   ⛔ 这里不把"核心要求做的事"标成忽略来逃掉 —— 建窗 / 显示 / 重绘 / 定时器 / 泵 / 销毁 /
 *   退出消息都是**真做**的。
 */
import { BrowserWindow, powerSaveBlocker, screen } from 'electron';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

import { DEFAULT_WINDOW_POSITION } from '../../core/window.ts';
import type {
  BootHost,
  D3d9Handle,
  DeviceHandle,
  DeviceParams,
  DisplayMode,
  DrawObjectHandle,
  DrawObjectParams,
  FrameMetrics,
  GraphicsHost,
  LegacyDrawParams,
  MessageHost,
  OperateMenuParams,
  PlatformHandle,
  PostableWindowMessage,
  ScreenSize,
  WindowClassHandle,
  WindowClassParams,
  WindowCreateParams,
  WindowHandle,
  WindowHost,
  WindowMessage,
} from '../../core/window.ts';
import { ElectronRenderHost } from './render-host.ts';
import type { LogFn } from './render-host.ts';

/** 窗口标题（引擎的类名 / 标题不是同一个东西；标题实际内容未取证，见 `RES-BOOT-WINDOW-TITLE`） */
const WINDOW_TITLE = 'ARCGameEngine';
/** 进程实例的占位句柄（Electron 没有 `HINSTANCE`；核心只把它原样转发，不解释） */
export const PROCESS_INSTANCE = 1;
/** 辅助宿主模块的资源实例占位句柄（图标 / 菜单资源的来源；Electron 侧没有对应物） */
export const RESOURCE_INSTANCE = 1;
/** 所有窗口 / 类 / 设备句柄都用 1（单窗口；核心只比较"是不是 null"） */
const SINGLE_HANDLE = 1;

/** 退出哨兵的入队标记（`getMessage()` 取到它时返回 `null`） */
const QUIT = Symbol('quit-message');

/** 队列里的一条：核心的领域消息，或退出哨兵 */
type QueueEntry = WindowMessage | typeof QUIT;

/** 宿主侧显式忽略的一条（⛔ 不许静默：原因必须非空） */
export interface HostIgnored {
  what: string;
  reason: string;
}

export interface ElectronWindowHostOptions {
  /** app 根（`apps/emulator-new`）：`dist/renderer/index.html` 与 preload 都相对它解析 */
  appRoot: string;
  /** 覆盖 preload 路径（缺省 `<appRoot>/src/platform/electron/preload.ts`） */
  preloadPath?: string;
  log?: LogFn;
}

/**
 * `BootHost` 的 Electron 实现。
 * ★ 构造后 `renderHost` 立即可用（IPC 监听在构造时注册，早于页面加载）。
 */
export class ElectronWindowHost implements BootHost, WindowHost, MessageHost, GraphicsHost {
  readonly renderHost: ElectronRenderHost;
  /** 显式忽略清单（平台侧；核心的 `session.ignored` 是另一份，两者都要留痕） */
  readonly ignored: HostIgnored[] = [];

  private readonly appRoot: string;
  private readonly preloadPath: string;
  private readonly log: LogFn;
  private readonly loggedOnce = new Set<string>();

  private win: BrowserWindow | null = null;
  private windowClass: WindowClassParams | null = null;
  private created: WindowCreateParams | null = null;
  /** 显示方式（`ShowWindow` 原样转发；本层只记账） */
  private showMode: number | null = null;
  private destroyed = false;
  /** 用户点关闭时是否拦下来交给引擎的窗口过程（demo 模式放行） */
  private preventClose = true;
  private closeQueued = false;
  /** 页面加载失败的原因（非空 ⇒ 图形设备创建必须失败） */
  private loadFailure: string | null = null;

  private readonly queue: QueueEntry[] = [];
  private readonly timers = new Map<number, NodeJS.Timeout>();
  private powerSaveBlockerId: number | null = null;

  constructor(options: ElectronWindowHostOptions) {
    this.appRoot = options.appRoot;
    this.preloadPath = options.preloadPath ?? path.join(options.appRoot, 'src', 'platform', 'electron', 'preload.ts');
    this.log = options.log ?? ((level, text) => {
      if (level === 'error') console.error(text);
      else if (level === 'warn') console.warn(text);
      else console.log(text);
    });
    this.renderHost = new ElectronRenderHost({
      browserWindow: () => (this.destroyed ? null : this.win),
      clientSize: () => this.clientSize(),
      log: this.log,
    });
    // ★ 输入转发**未接线**（显式 ignored，⛔ 不伪造消息）：
    //   Electron 主进程没有 mousemove / mouseleave / mousedown（那些事件在渲染进程的 DOM 里）；
    //   要把它们接进来得走 preload → IPC → 窗口过程，本实现没有这条链路 ⇒ 核心的
    //   悬停定时器（标识 1）在 Electron 链路上不会被建起来（核心自己的契约测试仍覆盖它）。
    this.markIgnored(
      'input.forwarding',
      '鼠标移动 / 离开 / 按下没有接线：Electron 主进程没有 mousemove 一类事件（在渲染进程的 DOM 里），'
        + '本实现不伪造消息 ⇒ 悬停定时器（标识 1）在这条链路上不会建立；按键 / 滚轮 / 光标同样未接',
    );
  }

  /* ──────────────────────────── 宿主自己的小工具（不是 BootHost 的一部分） ──────────────────────────── */

  /** 当前窗口（`null` = 还没建 / 已销毁）；`main` 用它截屏 */
  browserWindow(): BrowserWindow | null {
    return this.destroyed ? null : this.win;
  }

  /** 客户区尺寸（场景快照 / renderer init 用；没建窗时退回主显示器尺寸） */
  clientSize(): { width: number; height: number } {
    if (this.created !== null) return { width: this.created.clientWidth, height: this.created.clientHeight };
    const size = screen.getPrimaryDisplay().size;
    return { width: size.width, height: size.height };
  }

  /**
   * 放行用户的关闭请求（**demo / smoke 用**：没有引擎主循环去消费 `{kind:'close'}`，
   * 拦下来就永远关不掉）。`--install` 入口**不调**它 —— 关闭必须走引擎的窗口过程。
   */
  allowUserClose(): void {
    this.preventClose = false;
  }

  /** 退出清理（幂等）：解绑 IPC、清定时器、停空闲抑制、销毁窗口 */
  dispose(): void {
    for (const [, timer] of this.timers) clearInterval(timer);
    this.timers.clear();
    this.renderHost.dispose();
    if (this.powerSaveBlockerId !== null) {
      if (powerSaveBlocker.isStarted(this.powerSaveBlockerId)) powerSaveBlocker.stop(this.powerSaveBlockerId);
      this.powerSaveBlockerId = null;
    }
    this.destroyed = true;
    const win = this.win;
    this.win = null;
    if (win !== null && !win.isDestroyed()) win.destroy();
  }

  private markIgnored(what: string, reason: string): void {
    if (this.ignored.some((entry) => entry.what === what)) return;
    this.ignored.push({ what, reason });
    this.log('warn', `[ignored] ${what} —— ${reason}`);
  }

  private logOnce(key: string, level: 'info' | 'warn' | 'error', text: string): void {
    if (this.loggedOnce.has(key)) return;
    this.loggedOnce.add(key);
    this.log(level, text);
  }

  private enqueue(entry: QueueEntry): void {
    this.queue.push(entry);
  }

  /* ──────────────────────────── WindowHost（同步） ──────────────────────────── */

  /**
   * 注册窗口类：Electron 没有窗口类 ⇒ 只**记录一个逻辑类**并返回句柄 1。
   * ★ 图标 / 光标 / 背景刷 / 类风格位 / 菜单名**全部只记账**（显式 ignored）：
   *   这些是 Win32 的外观概念，Electron 侧没有对应请求；⛔ 不假装设过。
   */
  registerWindowClass(params: WindowClassParams): WindowClassHandle | null {
    this.windowClass = params;
    this.markIgnored(
      'window-class.icon',
      `图标资源名 ${params.iconResourceName}（资源实例 ${params.resourceInstance}）是 Win32 的类属性；`
        + 'Electron 的窗口图标只能来自文件 / NativeImage，本实现不伪造图标',
    );
    this.markIgnored(
      'window-class.cursor',
      `系统预定义光标 ${params.cursorResourceId} 是 Win32 的类属性；Electron 由 CSS cursor 控制，本实现不改`,
    );
    this.markIgnored(
      'window-class.background-brush',
      `背景刷 ${params.backgroundBrush} 是 GDI 概念；本实现用 BrowserWindow 的 backgroundColor（#000000）`,
    );
    this.markIgnored(
      'window-class.class-style',
      `类风格 0x${params.classStyle.toString(16)} 的位含义属平台侧（RES-BOOT-CLASS-STYLE）；只记账，不解释、不设`,
    );
    this.markIgnored(
      'window-class.menu-name',
      `菜单名 ${params.menuName}（空菜单）是 Win32 的类属性；Electron 没有窗口类菜单`,
    );
    this.log('info', `registerWindowClass(${params.className}) → 逻辑类句柄 ${SINGLE_HANDLE}（无真实窗口类）`);
    return SINGLE_HANDLE;
  }

  /**
   * 建窗。
   * ★ `useContentSize: true` ⇒ `width` / `height` 是**客户区**尺寸：平台管外框，所以核心那边
   *   "客户区 + 外框尺寸"的显式求和只是记账（见 `frameMetrics`）。
   * ★ 位置：核心在"使用平台默认位置"时给哨兵 `0x80000000` ⇒ 不传 x / y，让平台自己选。
   * ★ 形态：核心用扩展风格字区分两态（窗口态恒 0 / 占满屏态固定 `0x8`）⇒ 非 0 = 全屏。
   *   （与 `menuResourceId === null` 是同一个判据，这里取扩展风格字这一条。）
   */
  createWindow(params: WindowCreateParams): WindowHandle | null {
    if (this.win !== null && !this.win.isDestroyed()) {
      this.log('warn', 'createWindow：已经有一个窗口，返回同一个句柄');
      return SINGLE_HANDLE;
    }
    this.created = params;
    const fullscreen = params.exStyle !== 0;
    const useDefaultPosition = (params.x >>> 0) === (DEFAULT_WINDOW_POSITION >>> 0);
    if (!useDefaultPosition && (params.y >>> 0) === (DEFAULT_WINDOW_POSITION >>> 0)) {
      this.log('warn', `createWindow：y 是默认位置哨兵而 x 是 ${params.x}（核心两格应当同时是哨兵）⇒ 只按 x 判`);
    }

    const win = new BrowserWindow({
      show: false, // 建窗不显示：显示是核心的下一条请求（ShowWindow）
      width: params.clientWidth,
      height: params.clientHeight,
      useContentSize: true,
      // 位置：哨兵（0x80000000）= 让平台选；否则原样用配置里的坐标
      ...(useDefaultPosition ? {} : { x: params.x, y: params.y }),
      fullscreen,
      autoHideMenuBar: true,
      backgroundColor: '#000000',
      title: WINDOW_TITLE,
      webPreferences: {
        preload: this.preloadPath,
        contextIsolation: true,
        nodeIntegration: false,
        // ★ renderer 的 preload 要 import 'electron' 并跑 Node 的类型剥壳 ⇒ 不能开 sandbox
        sandbox: false,
      },
    });
    this.win = win;
    this.destroyed = false;

    win.on('close', (event) => {
      if (!this.destroyed && this.preventClose) event.preventDefault();
      // 用户点关闭 = 窗口过程的"关闭"消息（只入队一次；核心在那一支里做收尾 → 杀定时器 → 销毁窗口）
      if (!this.closeQueued) {
        this.closeQueued = true;
        this.enqueue({ kind: 'close' });
      }
    });
    win.on('focus', () => this.enqueue({ kind: 'activate', active: true }));
    win.on('blur', () => this.enqueue({ kind: 'activate', active: false }));
    win.on('closed', () => {
      this.destroyed = true;
      this.win = null;
      this.log('info', '窗口已关闭（closed）');
    });
    win.webContents.on('did-fail-load', (_event, code, description, url, isMainFrame) => {
      if (!isMainFrame) return;
      this.loadFailure = `did-fail-load:${code}:${description}（${url}）`;
      this.log('error', `renderer 页面加载失败：${this.loadFailure}`);
    });
    win.webContents.on('render-process-gone', (_event, details) => {
      this.loadFailure = `render-process-gone:${details.reason}`;
      this.log('error', `渲染进程结束：${this.loadFailure}`);
    });

    // renderer 页面：`dist/renderer/index.html`（Vite 产物；由 `vite.config.ts` 构建）
    const htmlPath = path.join(this.appRoot, 'dist', 'renderer', 'index.html');
    if (!existsSync(htmlPath)) {
      // ⛔ 不假装加载成功：让 createDevice 失败（图形设备创建是启动里唯一能"等平台真结果"的关口）
      this.loadFailure = `missing-renderer-bundle:${htmlPath}`;
      this.log('error', `找不到 ${htmlPath} —— 先构建渲染进程（vite build）；本次启动的图形设备创建会失败`);
    } else {
      // 不 await：createWindow 是同步请求；"加载好没好"由 createDevice 等 ready 来判
      void win.loadURL(pathToFileURL(htmlPath).toString()).catch((err: unknown) => {
        this.loadFailure = `load-rejected:${err instanceof Error ? err.message : String(err)}`;
        this.log('error', `renderer 页面加载被拒：${this.loadFailure}`);
      });
    }
    this.log('info', `createWindow：客户区 ${params.clientWidth}x${params.clientHeight} · fullscreen=${fullscreen} · 逻辑句柄 ${SINGLE_HANDLE}`);
    return SINGLE_HANDLE;
  }

  /** `ShowWindow`：显示方式**原样记账**（核心不覆盖它），真实动作只有 `show()` */
  showWindow(window: WindowHandle, showMode: number): void {
    this.showMode = showMode;
    const win = this.win;
    if (win === null) return;
    this.log('info', `showWindow(句柄 ${window}, showMode ${showMode})`);
    win.show();
  }

  /** `UpdateWindow`：让 Chromium 排一次重绘（`WebContents.invalidate()`） */
  updateWindow(window: WindowHandle): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) return;
    try {
      win.webContents.invalidate();
    } catch (err) {
      this.log('warn', `updateWindow(${window}) 的 invalidate 失败：${err instanceof Error ? err.message : String(err)}`);
    }
  }

  /**
   * `ImmAssociateContext(window, 0)`：**显式忽略**。
   * ★ Electron 没有"输入法上下文句柄"这个可解绑的对象（IME 由 Chromium 的输入管线管），
   *   而且 renderer 是 HTML 页面、没有任何输入控件 ⇒ 这里没有可做的等价动作。
   *   ⛔ 不假装调用了某个 API。
   */
  associateInputContext(window: WindowHandle, context: PlatformHandle | null): void {
    this.markIgnored(
      'window-attach.associate-input-context',
      `Electron 没有可解绑的输入法上下文（请求：窗口 ${window} 的上下文设为 ${context === null ? '空' : String(context)}）；`
        + 'IME 由 Chromium 输入管线管理，本实现没有等价请求',
    );
  }

  /**
   * `GetMenu(window)`：**显式忽略**，返回 `null`。
   * ★ Electron 的菜单是应用级 `Menu`（applicationMenu），不是"窗口的菜单句柄"；
   *   本实现的窗口没有菜单 ⇒ 没有可返回的句柄。核心不检查空值（原样转给辅助入口）。
   */
  getMenu(window: WindowHandle): PlatformHandle | null {
    this.markIgnored(
      'window-attach.get-menu',
      `GetMenu(窗口 ${window}) ⇒ 空：Electron 没有"窗口菜单句柄"（菜单是 applicationMenu，`
        + '不是建窗时挂上去的窗口菜单）；核心会把空值原样转给辅助入口',
    );
    return null;
  }

  /**
   * 辅助宿主模块的「操作菜单」入口（启动期 `(类别 1, hWnd, hMenu, 0)`）：**显式忽略**。
   * ★ 菜单内容不是引擎建的（它只是把辅助模块里的菜单资源交给这个入口）；Electron 侧没有这个
   *   辅助模块、也没有菜单资源 ⇒ 没有可转交的对象。⛔ 不伪造菜单。
   * ★ 启动期**没有 `SetMenu`**（菜单是建窗时的 `hMenu` 实参）⇒ 本实现也不加这一请求。
   */
  operateMenu(params: OperateMenuParams): void {
    this.markIgnored(
      'window-attach.operate-menu',
      `辅助宿主模块的「操作菜单」入口（类别 ${params.category}，窗口 ${params.window}，`
        + `菜单 ${params.menu === null ? '空' : String(params.menu)}，第 4 实参 ${params.extra}）在 Electron 侧不存在：`
        + '没有辅助模块、没有菜单资源可转交（菜单内容不是引擎建的）',
    );
  }

  /** `DrawMenuBar(window)`：**显式忽略**（上一条已经把菜单安装整个忽略了） */
  drawMenuBar(window: WindowHandle): void {
    this.markIgnored(
      'window-attach.draw-menu-bar',
      `没有菜单栏可重画（窗口 ${window}）；Electron 的菜单是 applicationMenu，不是窗口菜单`,
    );
  }

  /** 窗口是不是前台窗口（周期定时器到期判据的第三项） */
  isForegroundWindow(window: WindowHandle): boolean {
    const win = this.win;
    if (win === null || win.isDestroyed()) return false;
    void window;
    return win.isFocused();
  }

  /** 隐藏窗口（收到"已销毁"消息的第一步） */
  hideWindow(window: WindowHandle): void {
    const win = this.win;
    if (win === null || win.isDestroyed()) {
      this.log('warn', `hideWindow(${window})：窗口已经不在（销毁后的第一格）`);
      return;
    }
    win.hide();
  }

  /**
   * 销毁窗口（收到"关闭"消息的最后一步），随后**投递"已销毁"消息**（模拟 `WM_DESTROY`）。
   * ★ Electron 的 `destroy()` 不触发 `close` 事件（文档明说），所以这里不会自己再入队一条 close。
   */
  destroyWindow(window: WindowHandle): void {
    const win = this.win;
    this.destroyed = true; // 先置位：close 事件（若被触发）必须放行
    if (win !== null && !win.isDestroyed()) win.destroy();
    this.log('info', `destroyWindow(句柄 ${window}) → 入队 {kind:'destroy'}`);
    this.enqueue({ kind: 'destroy' });
  }

  /** `PostQuitMessage(exitCode)`：退出哨兵入队（`getMessage()` 取到它时返回 `null` ⇒ 核心抛 `EngineExit`） */
  postQuitMessage(exitCode: number): void {
    this.log('info', `postQuitMessage(${exitCode}) → 入队退出哨兵`);
    this.enqueue(QUIT);
  }

  /** 向窗口自己投递一条消息（周期定时器到期时可能补发应用激活） */
  postWindowMessage(window: WindowHandle, message: PostableWindowMessage): void {
    void window;
    if (message === 'activate-app') {
      this.enqueue({ kind: 'activate', active: true });
      return;
    }
    // 类型上只有 'activate-app'；真出现别的值说明类型被改坏了
    throw new Error(`post-window-message-unknown:${String(message)}`);
  }

  /* ──────────────────────────── MessageHost（同步） ──────────────────────────── */

  /** `SetThreadExecutionState(3)`：用 Electron 的 `powerSaveBlocker` 阻止显示器休眠（只启一次） */
  setThreadExecutionState(flags: number): void {
    this.logOnce('thread-execution-state', 'info',
      `setThreadExecutionState(${flags}) → powerSaveBlocker.start('prevent-display-sleep')（退出时 stop）`);
    if (this.powerSaveBlockerId !== null) return;
    this.powerSaveBlockerId = powerSaveBlocker.start('prevent-display-sleep');
  }

  /** `SetTimer(window, id, periodMs, NULL)`：`setInterval` 到期入队一条 `{kind:'timer'}` */
  setTimer(window: WindowHandle, params: { id: number; periodMs: number; callback: null }): void {
    void window;
    const existing = this.timers.get(params.id);
    if (existing !== undefined) clearInterval(existing);
    const timer = setInterval(() => {
      this.enqueue({ kind: 'timer', timerId: params.id });
    }, params.periodMs);
    this.timers.set(params.id, timer);
    this.log('info', `setTimer(id ${params.id}, ${params.periodMs}ms, callback=${params.callback})`);
  }

  /** `KillTimer(window, id)` */
  killTimer(window: WindowHandle, id: number): void {
    void window;
    const timer = this.timers.get(id);
    if (timer === undefined) return;
    clearInterval(timer);
    this.timers.delete(id);
    this.log('info', `killTimer(id ${id})`);
  }

  /** `PeekMessage(remove = false)`：只看队列有没有东西（**不移除**） */
  peekMessage(params: { remove: false }): boolean {
    void params;
    return this.queue.length > 0;
  }

  /** `GetMessage(...)`：取一条；取到退出哨兵 ⇒ `null`（核心抛 `EngineExit`） */
  getMessage(): WindowMessage | null {
    const entry = this.queue.shift();
    if (entry === undefined) return null;
    if (entry === QUIT) return null;
    return entry;
  }

  /** `TranslateMessage`：本层没有要翻译的东西（显式 no-op） */
  translateMessage(message: WindowMessage): void {
    void message;
  }

  /* ──────────────────────────── GraphicsHost（async） ──────────────────────────── */

  /**
   * 旧式绘制路线：**没有后端** ⇒ 返回 `null`（spec 的失败路径：核心写错误通道 + 结束启动）。
   * ★ 本实现只有 Pixi / WebGL 一条后端。⛔ 绝不拿 Pixi 假装"旧式绘制对象" ——
   *   那是把一条未实现的路线伪装成成功，`CORE-BOOT-GRAPHICS-INIT` 的守卫就失去意义。
   */
  async createLegacyDraw(params: LegacyDrawParams): Promise<DrawObjectHandle | null> {
    this.log('error',
      `旧式绘制路线没有后端（窗口 ${params.window}，客户区 ${params.clientWidth}x${params.clientHeight}，`
        + `行为位 ${params.behaviorBits}）⇒ 按 spec 的失败路径结束启动；`
        + '本实现只提供 Pixi / WebGL 一条后端（把 set:CreateObject 最低位设 0 走三维设备路线）');
    this.markIgnored(
      'graphics.legacy-draw',
      '旧式绘制路线未实现（只有 Pixi/WebGL 一条后端）⇒ 该路线按 spec 的失败路径返回失败，不伪造成功',
    );
    return null;
  }

  /** 三维路线 1：Direct3D 9 对象 —— 本实现的"设备对象"是渲染进程里的 Pixi，这里只给句柄 */
  async createD3d9(params: { sdkVersion: number }): Promise<D3d9Handle | null> {
    this.log('info', `createD3d9(sdkVersion ${params.sdkVersion}) → 逻辑句柄 ${SINGLE_HANDLE}（Pixi 后端）`);
    return SINGLE_HANDLE;
  }

  /**
   * 三维路线 2：适配器显示模式 = 主显示器尺寸 + 像素格式 0。
   * ★ `format` 的位含义属平台侧；本实现没有等价枚举 ⇒ **固定 0 是占位**（⛔ 不是取证过的格式）。
   */
  async queryAdapterDisplayMode(params: { d3d: D3d9Handle; adapter: number }): Promise<DisplayMode | null> {
    void params;
    const size = screen.getPrimaryDisplay().size;
    this.logOnce('adapter-display-mode', 'info',
      `queryAdapterDisplayMode → ${size.width}x${size.height}，format 0（占位：没有等价的像素格式枚举）`);
    return { width: size.width, height: size.height, format: 0 };
  }

  /**
   * 三维路线 3：创建设备 = **等 renderer ready + 发 `init`**。
   * ★ 页面加载失败 / renderer 15s 没 ready / `init` 被 renderer 拒绝 ⇒ 一律 `null`
   *   （核心写内建错误文本进错误通道并结束启动）。
   */
  async createDevice(params: DeviceParams): Promise<DeviceHandle | null> {
    if (this.loadFailure !== null) {
      this.log('error', `createDevice 失败：renderer 页面没加载成功（${this.loadFailure}）`);
      return null;
    }
    const size = this.clientSize();
    const ok = await this.renderHost.initRenderer({
      width: size.width,
      height: size.height,
      screenMode: params.screenMode,
      fullscreenPresent: params.fullscreenPresent,
      background: 0,
    });
    if (!ok) {
      this.log('error', 'createDevice 失败：renderer 没有就绪或拒绝 init');
      return null;
    }
    this.log('info', `createDevice 成功 → 设备句柄 ${SINGLE_HANDLE}（renderer ${size.width}x${size.height}，screenMode ${params.screenMode}）`);
    return SINGLE_HANDLE;
  }

  /**
   * 三维路线 4a：读设备的像素着色器配置。
   * ★ 来源未取证（`RES-BOOT-GFX-PARAMS` / `pixel-shader-config-source`）⇒ 本实现返回 0（占位，⛔ 不是观察值）。
   */
  async readPixelShaderConfig(params: { device: DeviceHandle }): Promise<number> {
    void params;
    this.logOnce('pixel-shader-config', 'info', 'readPixelShaderConfig → 0（来源未取证；占位值，不是观察值）');
    return 0;
  }

  /** 三维路线 4b：上层绘制对象 —— 逻辑句柄（真实的 `RenderTexture` / 场景在 renderer 里） */
  async createDrawObject(params: DrawObjectParams): Promise<DrawObjectHandle | null> {
    this.log('info',
      `createDrawObject(纹理 ${params.textureWidth}x${params.textureHeight}，像素着色器 ${params.pixelShader}) → 逻辑句柄 ${SINGLE_HANDLE}`);
    return SINGLE_HANDLE;
  }

  /**
   * 窗口外框四类尺寸：**全 0**。
   * ★ 核心用 `useContentSize` 之外的算法"客户区 + 外框"，而 Electron 的 `useContentSize: true`
   *   让平台直接管外框 ⇒ 这次显式求和只做**记账**，真实客户区尺寸由 Electron 保证。
   *   ⛔ 不去量标题栏 / 菜单栏的像素再喂回去 —— 那会让窗口尺寸被重复相加。
   */
  frameMetrics(): FrameMetrics {
    this.markIgnored(
      'frame-metrics',
      '窗口外框尺寸全返回 0：Electron 用 useContentSize 让平台管外框 ⇒ 核心的"客户区 + 外框"显式求和'
        + '只是记账，真实客户区尺寸由平台保证（RES-BOOT-WINDOW-STYLE 的位含义也未取证）',
    );
    // 四个"单边厚度"（边框宽 / 边框高 / 标题高 / 菜单高）全 0 —— 见上面的 ignored 原因
    return { frameWidth: 0, frameHeight: 0, captionHeight: 0, menuHeight: 0 };
  }

  /** 屏幕宽 / 高（占满屏态建窗前显式询问）：主显示器尺寸 */
  screenSize(): ScreenSize {
    const size = screen.getPrimaryDisplay().size;
    return { width: size.width, height: size.height };
  }
}
