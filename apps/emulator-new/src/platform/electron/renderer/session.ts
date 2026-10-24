/**
 * renderer 里的 core 会话：**这里才真正跑引擎**。
 *
 * ★ 2026-10 重构后的分层：
 *   - `Host`（文件 / 环境）由 `BridgeHost` 走 `window.amayui.invoke('host.*')` 到 main；
 *   - `BootHost`（窗口 / 消息 / 图形）由 `BridgeBootHost` 走 `boot.*`，图形部分直接接本地 Pixi；
 *   - `RenderHost` 由 `LocalRenderHost` 直接调 `PixiRenderHost`（不再跨 IPC）；
 *   - `analyzeInstall` / `bootWindow` / `Machine` / `HANDLERS` 全部在这个文件里调用。
 *
 * ★ 同步 / 异步：core 的 `MessageHost` 泵接口保持同步 ⇒ `BridgeBootHost` 维护本地队列；
 *   main 只负责把 BrowserWindow 事件推过来。定时器也在 renderer 本地跑（`setInterval`）。
 * ★ 启动窗口必须先存在（renderer 自己跑在里面）⇒ main 的 `open()` 建 bootstrap 窗口，
 *   core 的 `createWindow` 只调整同一个窗口；见 `window-host.ts` 的头注释。
 */
import { ConfigStore } from '../../../core/config.ts';
import { buildConfigKeys, buildOpcodeTable } from '../../../core/spec.ts';
import { Machine, WAIT_BIT } from '../../../core/machine.ts';
import type { DrawItem, Operand, SceneCommandRecord } from '../../../core/machine.ts';
import { analyzeInstall } from '../../../core/runner.ts';
import type { CoreEnv } from '../../../core/runner.ts';
import { EngineExit, bootWindow } from '../../../core/window.ts';
import type {
  BootHost,
  D3d9Handle,
  DeviceHandle,
  DeviceParams,
  DisplayMode,
  DrawObjectHandle,
  DrawObjectParams,
  FrameMetrics,
  LegacyDrawParams,
  PlatformHandle,
  PostableWindowMessage,
  ScreenSize,
  WindowClassHandle,
  WindowClassParams,
  WindowCreateParams,
  WindowHandle,
  WindowMessage,
} from '../../../core/window.ts';
import type { DrawTextRequest, RenderHost, SurfaceHandle, TextureCreateRequest, TextureUploadRequest } from '../../../core/render.ts';
import type { Host } from '../../../core/install.ts';
import type {
  AmayuiBridge,
  PlatformInfo,
  RendererBootInfo,
  SceneSnapshot,
  SceneViewSnapshot,
  StartJob,
  SurfaceRef,
  TextureSnapshot,
  DrawItemSnapshot,
  DrawItemGroupSnapshot,
  DrawItemSourceWindowSnapshot,
  MeshSnapshot,
  RenderCall,
  SceneCommandSnapshot,
} from '../protocol.ts';
import { dirnamePath, joinPath, pathFromEngine } from './paths.ts';
import { SceneOrderNotes } from './scene-order.ts';
import { applyPresentationPolicy } from './presentation.ts';
import { LocalMovieHost } from './local-movie-host.ts';
import { DomInputHost } from './input-host.ts';
import type { DisplayFit } from './display-fit.ts';
import { computeDisplayFit } from './display-fit.ts';
import type { PresentationPolicy } from './presentation.ts';
import type { PixiRenderHost } from './pixi-host.ts';

/**
 * 桥接调用计时：按 method 累计 count / total / max，每 200 次或单次 ≥ 50ms 写一条日志。
 * 目的：让“INITCONFIG4 卡很久”能区分是 FS 往返还是指令本身。
 */
const bridgeTimings = new Map<string, { count: number; total: number; max: number }>();

function logBridgeTiming(bridge: AmayuiBridge, method: Parameters<AmayuiBridge['invoke']>[0], ms: number): void {
  const item = bridgeTimings.get(method) ?? { count: 0, total: 0, max: 0 };
  item.count += 1;
  item.total += ms;
  if (ms > item.max) item.max = ms;
  bridgeTimings.set(method, item);
  if (ms >= 50) {
    bridge.report({ kind: 'log', level: 'info', text: `[timing] slow ${method} ${ms.toFixed(1)}ms` });
  } else if (item.count % 200 === 0) {
    bridge.report({
      kind: 'log',
      level: 'info',
      text: `[timing] ${method} count=${item.count} total=${item.total.toFixed(0)}ms max=${item.max.toFixed(1)}ms`,
    });
  }
}

async function timedInvoke<T>(bridge: AmayuiBridge, method: Parameters<AmayuiBridge['invoke']>[0], params: unknown): Promise<T> {
  const started = performance.now();
  try {
    return await bridge.invoke(method, params) as T;
  } finally {
    logBridgeTiming(bridge, method, performance.now() - started);
  }
}

const QUIT = Symbol('quit-message');
type QueueEntry = WindowMessage | typeof QUIT;

const PROCESS_INSTANCE = 1;
const RESOURCE_INSTANCE = 1;
const SHOW_MODE_NORMAL = 1;
const WINDOW_TITLE = 'ARCGameEngine';

/* ───────────────── 附加组视图（记录第 2 位 ⇒ 消费者侧的派生输入） ───────────────── */

/**
 * 附加组视图（`DrawItemSnapshot.group`）—— 条目首字第 2 位 = 1 时才取。
 *
 * ★ 语义名来自 `CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 6 条；**机器格 → 语义名**的对应只有
 *   **消费者侧读点**的证据（`.tmp/spec-draft/batch4/texconsumer/evidence.md` §3.2 / §3.3 B6）：
 *   `+0x20C / +0x220`（颜色通道起点 / 周期）、`+0x21C / +0x230`（源矩形通道起点 / 周期）、
 *   `+0x238` 步进数、`+0x23C` 每步数、`+0x240` 附加组终色。
 *   ⇒ 这是"**观察到的读点**"，不是已取证的领域命名（写侧命名属残差）—— 偏移只属于机器层，
 *     这里的语义名只是给规划器一个**输入槽**。
 *
 * ★ 未取证 / 未建模的项**显式留空**（⛔ 不编常数、不编公式）：
 *   - `colorEnd`：`+0x240` 在**已建模**的写入方里没有写点 ⇒ `null`（不混色）；
 *   - `stepX / stepY`：步进量的公式属残差 `RES-TEX-DRAW-CONSUMER-RECTSTEP` ⇒ `0`；
 *   - `lastStepIndex`：核心没有"每帧记步号"的状态 ⇒ `null`（于是不标脏）；
 *   - `transform`：三张附加组矩阵里哪一张是"变换通道"未取证 ⇒ `null`。
 */
const GROUP_OFFSETS = {
  colorStart: 0x20c,
  colorPeriod: 0x220,
  colorEnd: 0x240,
  rectStart: 0x21c,
  rectPeriod: 0x230,
} as const;

function groupSnapshotOf(item: DrawItem): DrawItemGroupSnapshot | null {
  if ((item.flags & 0x4) === 0) return null;
  const field = (offset: number): number => item.fields.get(offset) ?? 0;
  const end = item.fields.get(GROUP_OFFSETS.colorEnd);
  return {
    colorStart: field(GROUP_OFFSETS.colorStart),
    colorPeriod: field(GROUP_OFFSETS.colorPeriod),
    colorEnd: end === undefined ? null : end,
    rectStart: field(GROUP_OFFSETS.rectStart),
    rectPeriod: field(GROUP_OFFSETS.rectPeriod),
    stepX: 0,
    stepY: 0,
    lastStepIndex: null,
    transform: null,
  };
}

/**
 * 源矩形窗口（`DrawItemSnapshot.sourceWindow`；`0x239` 排上、`0x239` 的「已存在」门通过才写）。
 *
 * ★ 位 1（有窗）与颜色窗**共用**：颜色窗单独置位时这五格是**默认 0**（持续时间 ≤ 0 ⇒ 4.1 不动作），
 *   ⇒ 位 1 为 1 而记录里没有 `sourceWindow` 不是"缺字段"，是"没排源矩形窗"。
 * ★ 位 1 为 0 ⇒ `null`（消费侧的门根本不会走到 4.1）。
 */
function sourceWindowSnapshotOf(item: DrawItem): DrawItemSourceWindowSnapshot | null {
  if ((item.flags & 0x2) === 0) return null;
  const window = item.sourceWindow;
  if (window === null) {
    return { delay: 0, duration: 0, steps: 0, perRow: 0, switchWord: 0 };
  }
  return { ...window };
}

/**
 * 一条**场景命令记录** → 快照（`SceneSnapshot.commands`）。
 *
 * ★ **只投具名格**：`raw` 是读操作数留痕（记录自己的注释写明它**不是**命令档的输入）⇒ ⛔ 不投。
 * ★ **区间 A / B 的挪移**：种类 0 / 2 的两段在记录里是 `segmentA` / `segmentB`；
 *   种类 1 / 3 只有 `rangeStart` / `rangeCount` 那一对 ⇒ `rangeA` 取它、`rangeB` 投**显式空**
 *   `{ start: 0, count: 0 }`（⛔ 不要留 `undefined` 让调用方各自编）。
 * ★ 未被本实现建模的格（种类 1 的 `flag` / 目标矩形、种类 3 的四对插值）不在快照的闭集合里：
 *   S0/S1 只执行**结构**；要用它们时再投影，⛔ 不要先塞默认值。
 */
function commandSnapshotOf(commandKey: number, record: SceneCommandRecord): SceneCommandSnapshot {
  const twoSegments = record.kind === 0 || record.kind === 2;
  return {
    kind: record.kind,
    commandKey,
    timestamp: record.timestamp,
    delay: record.delay,
    duration: record.duration,
    textureId: record.textureId,
    rangeA: twoSegments
      ? { start: record.segmentA[0], count: record.segmentA[1] }
      : { start: record.rangeStart, count: record.rangeCount },
    rangeB: twoSegments
      ? { start: record.segmentB[0], count: record.segmentB[1] }
      : { start: 0, count: 0 },
    mode: record.mode,
    split: record.split,
    ownerKey: record.ownerKey,
    rect: { ...record.rect },
    flag: record.flag,
    mask: record.mask,
  };
}

function messageOf(error: unknown): string {
  return error instanceof Error ? `${error.message}\n${error.stack ?? ''}` : String(error);
}

/* ──────────────────────────── Host（文件 / 环境） ──────────────────────────── */

class BridgeHost implements Host {
  private readonly bridge: AmayuiBridge;
  private readonly platform: PlatformInfo;

  constructor(bridge: AmayuiBridge, platform: PlatformInfo) {
    this.bridge = bridge;
    this.platform = platform;
  }

  join(...parts: string[]): string {
    return joinPath(this.platform.sep, ...parts);
  }

  dirname(file: string): string {
    return dirnamePath(this.platform.sep, file);
  }

  pathFromEngine(text: string): string {
    return pathFromEngine(this.platform.sep, text);
  }

  env(name: string): string | null {
    if (name === 'APPDATA') return this.platform.env.APPDATA;
    if (name === 'XDG_CONFIG_HOME') return this.platform.env.XDG_CONFIG_HOME;
    return null;
  }

  homeDir(): string {
    return this.platform.homeDir;
  }

  async readBytes(file: string): Promise<Uint8Array> {
    return await timedInvoke(this.bridge, 'host.readBytes', { path: file }) as Uint8Array;
  }

  async readBytesMaybe(file: string): Promise<Uint8Array | null> {
    const result = await timedInvoke(this.bridge, 'host.readBytesMaybe', { path: file });
    return (result ?? null) as Uint8Array | null;
  }

  async readSlice(file: string, offset: number, length: number): Promise<Uint8Array> {
    return await timedInvoke(this.bridge, 'host.readSlice', { path: file, offset, length }) as Uint8Array;
  }

  async readText(file: string): Promise<string> {
    return await timedInvoke(this.bridge, 'host.readText', { path: file }) as string;
  }

  async isFile(file: string): Promise<boolean> {
    return await timedInvoke(this.bridge, 'host.isFile', { path: file }) as boolean;
  }

  async listDir(dir: string): Promise<string[]> {
    return await timedInvoke(this.bridge, 'host.listDir', { dir }) as string[];
  }

  async ensureReadDir(dir: string): Promise<void> {
    await timedInvoke(this.bridge, 'host.ensureReadDir', { dir });
  }

  async writeBytes(file: string, data: Uint8Array): Promise<void> {
    await timedInvoke(this.bridge, 'host.writeBytes', { path: file, data });
  }
}

/* ──────────────────────────── BootHost（窗口 / 消息 / 图形） ──────────────────────────── */

class BridgeBootHost implements BootHost {
  private readonly bridge: AmayuiBridge;
  private readonly pixi: PixiRenderHost;
  private readonly presentation: PresentationPolicy;
  private readonly queue: QueueEntry[] = [];
  private readonly timers = new Map<number, ReturnType<typeof setInterval>>();
  private foreground = false;
  private firstFrameSent = false;

  constructor(bridge: AmayuiBridge, pixi: PixiRenderHost, presentation: PresentationPolicy) {
    this.bridge = bridge;
    this.pixi = pixi;
    this.presentation = presentation;
    bridge.onWindowMessage((message) => {
      if (message.kind === 'activate') this.foreground = message.active;
      this.queue.push(message);
    });
  }

  /* WindowHost */

  async frameMetrics(): Promise<FrameMetrics> {
    return await timedInvoke(this.bridge, 'boot.frameMetrics', {}) as FrameMetrics;
  }

  async screenSize(): Promise<ScreenSize> {
    return await timedInvoke(this.bridge, 'boot.screenSize', {}) as ScreenSize;
  }

  async registerWindowClass(params: WindowClassParams): Promise<WindowClassHandle | null> {
    return await timedInvoke(this.bridge, 'boot.registerWindowClass', params) as WindowClassHandle | null;
  }

  async createWindow(params: WindowCreateParams): Promise<WindowHandle | null> {
    return await timedInvoke(this.bridge, 'boot.createWindow', applyPresentationPolicy(params, this.presentation)) as WindowHandle | null;
  }

  showWindow(window: WindowHandle, showMode: number): void {
    void timedInvoke(this.bridge, 'boot.showWindow', { window, showMode });
  }

  updateWindow(window: WindowHandle): void {
    void timedInvoke(this.bridge, 'boot.updateWindow', { window });
  }

  associateInputContext(window: WindowHandle, context: PlatformHandle | null): void {
    void timedInvoke(this.bridge, 'boot.associateInputContext', { window, context });
  }

  async getMenu(window: WindowHandle): Promise<PlatformHandle | null> {
    return await timedInvoke(this.bridge, 'boot.getMenu', { window }) as PlatformHandle | null;
  }

  operateMenu(params: { category: number; window: WindowHandle; menu: PlatformHandle | null; extra: number }): void {
    void timedInvoke(this.bridge, 'boot.operateMenu', params);
  }

  drawMenuBar(window: WindowHandle): void {
    void timedInvoke(this.bridge, 'boot.drawMenuBar', { window });
  }

  isForegroundWindow(window: WindowHandle): boolean {
    void window;
    return this.foreground;
  }

  hideWindow(window: WindowHandle): void {
    void timedInvoke(this.bridge, 'boot.hideWindow', { window });
  }

  destroyWindow(window: WindowHandle): void {
    void timedInvoke(this.bridge, 'boot.destroyWindow', { window });
  }

  postQuitMessage(exitCode: number): void {
    void exitCode;
    this.queue.push(QUIT);
  }

  postWindowMessage(window: WindowHandle, message: PostableWindowMessage): void {
    void window;
    if (message === 'activate-app') this.queue.push({ kind: 'activate', active: true });
  }

  /* MessageHost */

  setThreadExecutionState(flags: number): void {
    void timedInvoke(this.bridge, 'boot.setThreadExecutionState', { flags });
  }

  setTimer(window: WindowHandle, params: { id: number; periodMs: number; callback: null }): void {
    void window;
    const existing = this.timers.get(params.id);
    if (existing !== undefined) clearInterval(existing);
    const timer = setInterval(() => { this.queue.push({ kind: 'timer', timerId: params.id }); }, params.periodMs);
    this.timers.set(params.id, timer);
  }

  killTimer(window: WindowHandle, id: number): void {
    void window;
    const timer = this.timers.get(id);
    if (timer === undefined) return;
    clearInterval(timer);
    this.timers.delete(id);
  }

  peekMessage(params: { remove: false }): boolean {
    void params;
    return this.queue.length > 0;
  }

  getMessage(): WindowMessage | null {
    const entry = this.queue.shift();
    if (entry === undefined) return null;
    if (entry === QUIT) return null;
    return entry;
  }

  translateMessage(message: WindowMessage): void {
    void message;
  }

  /* GraphicsHost */

  async createLegacyDraw(params: LegacyDrawParams): Promise<DrawObjectHandle | null> {
    void params;
    this.log('error', '旧式绘制路线没有后端（只有 Pixi/WebGL）⇒ 按 spec 的失败路径结束启动');
    return null;
  }

  async createD3d9(params: { sdkVersion: number }): Promise<D3d9Handle | null> {
    void params;
    return 1;
  }

  async queryAdapterDisplayMode(params: { d3d: D3d9Handle; adapter: number }): Promise<DisplayMode | null> {
    void params;
    const size = await timedInvoke(this.bridge, 'boot.screenSize', {}) as ScreenSize;
    return { width: size.width, height: size.height, format: 0 };
  }

  async createDevice(params: DeviceParams): Promise<DeviceHandle | null> {
    const width = Math.max(1, Math.round(window.innerWidth));
    const height = Math.max(1, Math.round(window.innerHeight));
    try {
      await this.pixi.execute({
        id: 1,
        method: 'init',
        params: {
          width,
          height,
          // 模拟器不支持全屏：诊断按实际窗口报告，避免“窗口 1280×720、标签却写全屏”
          screenMode: this.presentation.ignoreFullscreen ? 1 : params.screenMode,
          fullscreenPresent: this.presentation.ignoreFullscreen ? false : params.fullscreenPresent,
          background: 0,
        },
      });
    } catch (error) {
      this.log('error', `Pixi init 失败：${messageOf(error)}`);
      return null;
    }
    this.log('info', `createDevice → Pixi ${width}x${height}（${this.presentation.ignoreFullscreen ? '全屏已忽略 ⇒ 窗口态' : `screenMode ${params.screenMode}`}）`);
    // ★ 窗口显示必须晚于 init 的 renderer.resize：可见 canvas 的尺寸同步在 init 内完成，
    //   否则窗口先可见、之后 resize 重分配 canvas，会出一帧白。
    if (!this.firstFrameSent) {
      this.firstFrameSent = true;
      void this.bridge.invoke('host.firstFrame', {}).catch(() => {});
    }
    return 1;
  }

  async readPixelShaderConfig(params: { device: DeviceHandle }): Promise<number> {
    void params;
    return 0;
  }

  async createDrawObject(params: DrawObjectParams): Promise<DrawObjectHandle | null> {
    void params;
    return 1;
  }

  private log(level: 'info' | 'warn' | 'error', text: string): void {
    this.bridge.report({ kind: 'log', level, text });
  }
}

/* ──────────────────────────── RenderHost（本地 Pixi） ──────────────────────────── */

/**
 * 显示适配的**上下文**（`CORE-SCENE-DISPLAY-FIT` 的输入里不来自渲染器状态的那几部分）。
 * ★ 由启动路径（`onBeforeScript` / `runDemo`）在窗口 + 图形建成之后填一次；
 *   `null` = 还没到那一步 ⇒ 场景快照的 `view` 仍是 `null`（消费者按"无相机 / 无缩放"档算并记缺口）。
 */
export interface DisplayFitContext {
  /** 配置（VF 开关 / 类型 / 宽高比 / 参考尺寸族 `display:FullScreenWidth/Height` / `set:WinX/Y`） */
  config: ConfigStore;
  /**
   * **桌面尺寸**（引擎初始化时来自适配器显示模式 `GetAdapterDisplayMode`；本实现取宿主
   * `boot.screenSize` = 主显示器尺寸）。
   * ★ `null` = 宿主没有报 ⇒ 用客户区顶上，并在诊断里记一条 `display-fit-ref-assumed`（⛔ 不静默编造）。
   */
  desktop: { width: number; height: number } | null;
  /**
   * 形态字 S（`display:ScreenMode` 经 `display:ForceScreen` 归一后的值）。
   * ★ 场景的屏幕形态副本 `F` = `S != 0 ? 1 : 0`（窗口态 1 / 占满屏态 0）。
   *   ⛔ 模拟器的 `ignoreFullscreen` 只改窗口外观，不改这个引擎级形态字（与 `window.ts` 同口径）。
   */
  screenMode: number;
}

function textureShape(kind: string, width: number, height: number): string {
  return `${kind}:${width}x${height}`;
}

class LocalRenderHost implements RenderHost {
  private readonly pixi: PixiRenderHost;
  private readonly bridge: AmayuiBridge;
  private machine: Machine | null = null;
  private nextId = 1;
  private lastScenePushAt = 0;
  private lastHoldPresentAt = 0;
  /** 旧模拟器的 #holdFrames：全屏遮盖消失、替换层还没到时保持上一帧。 */
  private coverHoldFrames = 0;
  private lastCovered = false;
  private firstFrameSent = false;
  /** 上一次快照里的可绘制键；用于“新建条目首帧不 present”。 */
  private lastDrawableKeys = new Set<string>();
  /** 上一帧是否“有动画”，用于记录渐变收尾帧。 */
  private lastAnimating = 0;
  private lastFadeLogAt = 0;
  private capturedTextFade = false;
  private fadeSeq = 0;
  private readonly pushedTextures = new Map<number, string>();
  private readonly surfaceIds = new WeakMap<object, SurfaceRef>();
  /**
   * 命令档的诊断记录器（**只报一次**）。
   * ★ 命令表非空 ⇒ 引擎有"命令档 + 收尾扫描"两档；本实现**规划命令档**，执行由 `pixi-host` 按 `commandExecKinds` 开闸（缺省关）⇒
   *   诊断分两条显式记账：`scene-command-pass-planned`（规划了什么）与
   *   `scene-command-pass-render-unmodeled`（关闸时仍未真的画，⛔ 不假装）。
   *   规划器是纯函数（`scene-command-plan.ts`），执行 / 像素操作仍未建模。
   */
  private readonly orderNotes = new SceneOrderNotes();
  /** 显示适配上下文（窗口 + 图形建成后由启动路径填；`null` ⇒ 场景快照的 `view` 为 `null`） */
  private display: DisplayFitContext | null = null;
  /** 上一轮适配结果（退化出口 `T ∉ {0,1,2}` 要"保持旧值"）；`null` = 还没算过 */
  private lastFit: DisplayFit | null = null;
  /** 一次性 / 变化时才报的诊断（避免每帧刷屏） */
  private readonly fitNotes = new Set<string>();
  private lastFitSignature = '';

  constructor(pixi: PixiRenderHost, bridge: AmayuiBridge) {
    this.pixi = pixi;
    this.bridge = bridge;
  }

  attachMachine(machine: Machine | null): void {
    this.machine = machine;
    this.pushedTextures.clear();
    this.lastScenePushAt = 0;
  }

  /**
   * 接上显示适配的输入（`CORE-SCENE-DISPLAY-FIT`）。
   * ★ 只在窗口 + 图形建成之后调一次；`null` = 断开（诊断 / demo 用不到时）。
   */
  attachDisplayContext(context: DisplayFitContext | null): void {
    this.display = context;
    this.lastFit = null;
    this.fitNotes.clear();
    this.lastFitSignature = '';
  }

  /** 当前快照里是否有一个不透明、覆盖整个视口的元素（网格或绘制项）。 */
  private coversViewport(scene: ReturnType<LocalRenderHost['snapshot']>): boolean {
    const width = 1280;
    const height = 720;
    for (const mesh of scene.meshes) {
      if ((mesh.color >>> 24) === 0) continue;
      if (mesh.count !== 4 || mesh.copies[0].length < 4 || mesh.copies[1].length < 4) continue;
      const xs = mesh.copies[0].slice(0, 4).map((bits) => {
        const buffer = new ArrayBuffer(4);
        new DataView(buffer).setUint32(0, bits >>> 0, true);
        return new DataView(buffer).getFloat32(0, true);
      });
      const ys = mesh.copies[1].slice(0, 4).map((bits) => {
        const buffer = new ArrayBuffer(4);
        new DataView(buffer).setUint32(0, bits >>> 0, true);
        return new DataView(buffer).getFloat32(0, true);
      });
      const minX = Math.min(...xs);
      const maxX = Math.max(...xs);
      const minY = Math.min(...ys);
      const maxY = Math.max(...ys);
      if (minX <= 0 && minY <= 0 && maxX >= width && maxY >= height) return true;
    }
    for (const item of scene.drawItems) {
      if ((item.color >>> 24) === 0) continue;
      if (item.left <= 0 && item.top <= 0 && item.right >= width && item.bottom >= height) return true;
    }
    return false;
  }

  /** 推一次场景（节流；`force` 跳过节流）。返回是否真的推了。 */
  syncScene(force = false): boolean {
    const machine = this.machine;
    if (machine === null) return false;
    const now = Date.now();
    if (!force && now - this.lastScenePushAt < 16) return false;
    this.lastScenePushAt = now;
    // ★ 命令档的时间戳盖章在**快照之前**（引擎在渲染步首次遍历时盖）；只盖 timestamp === 0 的记录。
    machine.stampCommands();
    machine.advanceCommands();
    const scene = this.snapshot(machine);
    if (!this.capturedTextFade) {
      const textItem = scene.drawItems.find((item) => item.key === 200001);
      if (textItem !== undefined && ((textItem.color >>> 24) & 0xff) === 255) {
        this.capturedTextFade = true;
      }
    }
    if (scene.animating === 1 && now - this.lastFadeLogAt >= 100) {
      this.lastFadeLogAt = now;
      const meshes = scene.meshes.map((m) => 'm' + m.key + ':color=' + (m.color >>> 0).toString(16) + ':v0=' + (m.colors[0] ?? -1).toString(16)).join(' ');
      const items = scene.drawItems.map((d) => 'd' + d.key + ':color=' + (d.color >>> 0).toString(16) + ':tex=' + d.textureId + ':rect=' + d.left + ',' + d.top + ',' + d.right + ',' + d.bottom).join(' ');
      this.log('info', '[fade] meshes=' + scene.meshes.length + ' drawItems=' + scene.drawItems.length + ' ' + meshes + ' ' + items);
    }
    if (this.lastAnimating === 1 && scene.animating === 0) {
      const meshes = scene.meshes.map((m) => 'm' + m.key + ':color=' + (m.color >>> 0).toString(16) + ':v0=' + (m.colors[0] ?? -1).toString(16)).join(' ');
      const items = scene.drawItems.map((d) => 'd' + d.key + ':color=' + (d.color >>> 0).toString(16) + ':tex=' + d.textureId + ':rect=' + d.left + ',' + d.top + ',' + d.right + ',' + d.bottom).join(' ');
      this.log('info', '[fade-end] meshes=' + scene.meshes.length + ' drawItems=' + scene.drawItems.length + ' ' + meshes + ' ' + items);
    }
    this.lastAnimating = scene.animating;
    // ★ 帧批次边界：只在 VM 进入等待/门（flags != 0）时提交。
    //   VM 连续执行指令期间（包括场景有动画）都可能处于页面构建的中间态，
    //   ⛔ 不 apply、不 present；这正是旧模拟器“一批指令一次 present”的口径。
    machine.scene.windowStamped = false;
    if (!force && machine.flags === 0) {
      return false;
    }
    const covered = this.coversViewport(scene);
    if (!force && this.lastCovered && !covered && this.coverHoldFrames < 60) {
      this.coverHoldFrames += 1;
      this.log('info', '[present-hold] 全屏遮盖消失、替换层未到 ⇒ 保持上一帧 ' + this.coverHoldFrames);
      return false;
    }
    if (covered || this.coverHoldFrames > 0) {
      this.lastCovered = covered;
      this.coverHoldFrames = 0;
    }
    this.pixi.applyScene(scene);
    for (const texture of scene.textures) {
      this.pushedTextures.set(texture.id, textureShape(texture.kind, texture.width, texture.height));
    }
    return true;
  }

  private sceneBeforeTexture(textureId: number): void {
    const machine = this.machine;
    if (machine === null) return;
    const object = machine.scene.textureObjects.get(textureId);
    const shape = object ? textureShape(object.kind, object.width, object.height) : 'none';
    if (this.pushedTextures.get(textureId) !== shape) {
      this.syncScene(true);
      return;
    }
    this.syncScene();
  }

  async getRenderSurface(textureId: number): Promise<SurfaceHandle | null> {
    this.sceneBeforeTexture(textureId);
    try {
      const ref = await this.call<SurfaceRef>('getRenderSurface', { textureId });
      if (!ref) return null;
      return this.surfaceHandle(ref);
    } catch (error) {
      this.log('error', `getRenderSurface(${textureId}) 失败：${messageOf(error)}`);
      return null;
    }
  }

  async setRenderTarget(surface: SurfaceHandle): Promise<boolean> {
    const ref = this.surfaceIds.get(surface as unknown as object);
    if (ref === undefined) return false;
    try {
      await this.call('setRenderTarget', { surfaceId: ref.surfaceId });
      return true;
    } catch (error) {
      this.log('error', `setRenderTarget(${ref.surfaceId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  async getBackBuffer(): Promise<SurfaceHandle | null> {
    try {
      const ref = await this.call<SurfaceRef>('getBackBuffer', {});
      return this.surfaceHandle(ref);
    } catch (error) {
      this.log('error', `getBackBuffer 失败：${messageOf(error)}`);
      return null;
    }
  }

  async releaseSurface(surface: SurfaceHandle): Promise<void> {
    const ref = this.surfaceIds.get(surface as unknown as object);
    if (ref === undefined) return;
    try {
      await this.call('releaseSurface', { surfaceId: ref.surfaceId });
    } catch (error) {
      this.log('error', `releaseSurface(${ref.surfaceId}) 失败：${messageOf(error)}`);
    }
  }

  async createTexture(request: TextureCreateRequest): Promise<boolean> {
    try {
      return this.pixi.createTexture(request);
    } catch (error) {
      this.log('error', `createTexture(${request.textureId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  async uploadTexture(request: TextureUploadRequest): Promise<boolean> {
    try {
      return this.pixi.uploadTexture(request);
    } catch (error) {
      this.log('error', `uploadTexture(${request.textureId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  async releaseTexture(textureId: number): Promise<void> {
    try {
      this.pixi.releaseTexture(textureId);
    } catch (error) {
      this.log('error', `releaseTexture(${textureId}) 失败：${messageOf(error)}`);
    }
    // ★ 释放改了场景 ⇒ 下一次 `syncScene()` **不许**被 16ms 节流掉：帧边界上的下一次迭代
    //   会推一份**新快照**再 present（0x1fa 之后核心往往紧接着 0x1f8 / 换页）。
    //   ⛔ 不要在这里直接 present：平台侧手里只有上一份快照，画出来就是"上一页闪一帧"（白闪）。
    this.lastScenePushAt = 0;
  }

  async drawText(request: DrawTextRequest): Promise<boolean> {
    this.sceneBeforeTexture(request.textureId);
    try {
      await this.call('drawText', {
        textureId: request.textureId,
        x: request.x,
        y: request.y,
        text: request.text,
        font: request.font,
        color: request.color,
        size: request.size,
        weight: request.weight,
        shadowColor: request.shadowColor,
        drawMode: request.drawMode,
        alignMode: request.alignMode,
      });
      return true;
    } catch (error) {
      this.log('error', `drawText(纹理 ${request.textureId}) 失败：${messageOf(error)}`);
      return false;
    }
  }

  async present(): Promise<void> {
    try {
      await this.call('present', {});
    } catch (error) {
      this.log('error', `present 失败：${messageOf(error)}`);
    }
  }

  /** 非帧边界：重复提交上一份已提交场景（不重建），避免合成器合到空缓冲。 */
  presentLast(): void {
    // ★ 可见层是 2D canvas：跳过 present 时它自然保持上一帧，
    //   再提交 sceneRoot 只会把可能被中间态改坏的场景画出来。
    void this.lastHoldPresentAt;
  }

  /** 收尾：强制推一次最新场景并 present（避免停在旧快照上，见白屏问题） */
  async flush(): Promise<void> {
    this.syncScene(true);
    await this.present();
  }

  private async call<T>(method: RenderCall['method'], params: unknown): Promise<T> {
    const id = this.nextId;
    this.nextId += 1;
    const call = { id, method, params } as unknown as RenderCall;
    const result = await this.pixi.execute(call);
    return result as T;
  }

  private surfaceHandle(ref: SurfaceRef): SurfaceHandle {
    const handle = { surfaceId: ref.surfaceId, kind: ref.kind };
    this.surfaceIds.set(handle, ref);
    return handle as unknown as SurfaceHandle;
  }

  private snapshot(machine: Machine): SceneSnapshot {
    const { width, height } = this.pixi.canvasSize();
    const textures: TextureSnapshot[] = [];
    for (const [id, object] of machine.scene.textureObjects) {
      const record = machine.scene.textures.get(id);
      textures.push({
        id,
        kind: object.kind,
        width: object.width,
        height: object.height,
        file: record?.file ?? -1,
        blank: record?.blank ?? 0,
      });
    }
    const drawItems: DrawItemSnapshot[] = [];
    for (const [key, item] of machine.scene.drawItems) {
      // ★ 四条边（left/top/right/bottom）同时是**种类 2 抹除矩形**的唯一来源（spec：抹除矩形来自
      //   同键绘制项）⇒ `scene-order.ts:commandPlanInput` 把它们投影成 `tables.itemRects`，
      //   ⛔ 别在规划器 / 平台侧另算一份矩形。
      drawItems.push({
        key,
        flags: item.flags,
        textureId: item.textureId,
        left: item.left,
        top: item.top,
        right: item.right,
        bottom: item.bottom,
        color: item.colorNow,
        aux: [item.aux[0], item.aux[1], item.aux[2]],
        start: item.start,
        group: groupSnapshotOf(item),
        sourceWindow: sourceWindowSnapshotOf(item),
      });
    }
    const meshes: MeshSnapshot[] = [];
    for (const [key, mesh] of machine.scene.meshes) {
      meshes.push({
        key,
        flags: mesh.flags,
        count: mesh.count,
        copies: [[...mesh.copies[0]], [...mesh.copies[1]]],
        colors: [...mesh.colors],
        color: mesh.colorNow,
      });
    }
    const commands: SceneCommandSnapshot[] = [];
    // ★ 遍历序是**命令键升序**（命令表按命令键有序）；`Map` 只保插入次序 ⇒ 这里显式排序。
    for (const [commandKey, record] of [...machine.scene.commands].sort((a, b) => a[0] - b[0])) {
      commands.push(commandSnapshotOf(commandKey, record));
    }
    const scene: SceneSnapshot = {
      width,
      height,
      script: machine.currentFrameInfo(),
      textures,
      drawItems,
      meshes,
      blend: { mode: machine.render.blendMode, color: machine.render.blendColor },
      target: machine.render.target,
      clock: machine.scene.now,
      // ★ 显示适配（`CORE-SCENE-DISPLAY-FIT`）：窗口 + 图形建成后由 `display-fit.ts` 的纯函数算，
      //   `null` 只发生在"还没走到那一步"（消费者按无相机档算并记 `scene-view-unmodeled`）。
      //   ⛔ 不要在这里就地算几何：公式只有一份真源（`computeDisplayFit`）。
      view: this.viewSnapshot(width, height),
      // ★ 场景自身记录（`0x229` 写）：作用键区间与三元平移**已建模**；由记录派生额外变换的
      //   `sub_49AA30` **未建模** ⇒ `matrix: null`（规划器用单位阵 + `self-transform-matrix-unmodeled`），
      //   ⛔ 不按平移现拼一个矩阵（平移已折进那份未建模的派生里，现拼会重复平移）。
      selfRecord: {
        translate: { ...machine.scene.selfRecord.translate },
        keyStart: machine.scene.selfRecord.keyStart,
        keyCount: machine.scene.selfRecord.keyCount,
        matrix: null,
      },
      commands,
      // 「有动画」是核心已有的格（等待门也读它）；命令档的收尾与清表判据都用它。
      animating: machine.scene.animating,
      // ★ 核心**没有**「强制完成」这一格（写者：场景推进 / 种类 2 的类型 < 0，都未建模）
      //   ⇒ 显式投影 0，并在规划器里记缺口；⛔ 这个 0 不是引擎初值。
      forcedComplete: machine.scene.forcedComplete,
      // ★ 渲染器能力档未建模：`display:PixelShader` 是**版本号**（如 3），版本 → 档（0/1/2）的
      //   映射未取证 ⇒ `null`（规划器按 ≥2 跑 + `scene-command-tier-unmodeled`）。
      shaderTier: null,
    };
    // ★ 命令档规划诊断（本层只记账，⛔ 不执行命令档）：命令表非空的帧才可能报。
    for (const note of this.orderNotes.next(scene)) this.log('warn', `场景绘制顺序：${note}`);
    return scene;
  }

  /**
   * 按当前画布 / 配置算一次显示适配，并投影成消费者（`scene-draw-plan.ts`）的档位输入。
   *
   * ★ 输入全部来自配置 + 宿主（**没有一条来自脚本或存档**）：
   *   - `screenW/H` = 当前画布（本实现 = Pixi 画布 = 实际后缓冲；见 `pixi-host.canvasSize`）；
   *   - `vfOn` = `display:VirtualFullScreen != 0`；`T` = `display:VirtualFullScreenType`（VF 关 ⇒ 0）；
   *   - `A` = `display:AspectMode`；
   *   - `refW/refH`：VF 开 = `max(桌面, set:WinX/Y)` 逐分量；VF 关 = `display:FullScreenWidth/Height`，缺则客户区；
   *   - 画布：VF 关时先摆成参考尺寸（引擎初始化那条路）；VF 开时被公式覆写（退化出口保持上一轮）。
   * ★ 每帧算一次是有意的：画布随窗口变化（显示设置改变的等价物），公式是纯的、代价可忽略；
   *   ⛔ 结果只进**快照**，不写回任何引擎状态（这四类参数没有脚本 / 存档写入者）。
   */
  private viewSnapshot(screenW: number, screenH: number): SceneViewSnapshot | null {
    const context = this.display;
    if (context === null) return null;
    const { config, desktop, screenMode } = context;
    const vfOn = config.getInt('display:VirtualFullScreen') !== 0;
    const vfType = vfOn ? config.getInt('display:VirtualFullScreenType') : 0;
    const aspectMode = config.getInt('display:AspectMode');
    let refW: number;
    let refH: number;
    if (vfOn) {
      let baseW = screenW;
      let baseH = screenH;
      if (desktop !== null) {
        baseW = desktop.width;
        baseH = desktop.height;
      } else if (this.fitNoteOnce('display-fit-ref-assumed')) {
        // 宿主没有桌面尺寸 ⇒ 用客户区顶上，但必须留痕（⛔ 不静默编造）
        this.log('warn', '显示适配：宿主没有桌面尺寸（boot.screenSize）⇒ 参考尺寸按客户区算（display-fit-ref-assumed）');
      }
      refW = Math.max(baseW, config.getInt('set:WinX'));
      refH = Math.max(baseH, config.getInt('set:WinY'));
    } else {
      // 「缺则客户区」：整对为 0 就是配置没写全屏分辨率；0 进不了公式（t = refH/refW）⇒ 逐分量回退
      const fullW = config.getInt('display:FullScreenWidth');
      const fullH = config.getInt('display:FullScreenHeight');
      refW = fullW !== 0 ? fullW : screenW;
      refH = fullH !== 0 ? fullH : screenH;
    }
    const canvasW = vfOn ? (this.lastFit?.canvasW ?? screenW) : refW;
    const canvasH = vfOn ? (this.lastFit?.canvasH ?? screenH) : refH;
    const fit = computeDisplayFit({
      screenW,
      screenH,
      vfOn,
      vfType,
      aspectMode,
      refW,
      refH,
      canvasW,
      canvasH,
      previous: this.lastFit ?? undefined,
    });
    // ★ 退化出口（T ∉ {0,1,2}）：原点 / 视口 / 缩放保持旧值 ⇒ 不覆盖上一轮结果
    if (!fit.degenerate) this.lastFit = fit;
    else if (this.fitNoteOnce('display-fit-degenerate')) {
      this.log('warn', `显示适配：虚拟全屏类型 ${vfType} 非法（∉ {0,1,2}）⇒ 原点 / 缩放保持旧值，仅视图矩阵置单位阵`);
    }
    // 诊断：只在这几个格变化时报一行（⛔ 不每帧刷屏）
    const fixedResolution = screenMode !== 0;
    const signature = `${vfOn ? vfType : 'off'}/${aspectMode}/${screenW}x${screenH}/${refW}x${refH}/${fixedResolution}`;
    if (signature !== this.lastFitSignature) {
      this.lastFitSignature = signature;
      this.log(
        'info',
        `显示适配：VF=${vfOn ? vfType : 'off'} A=${aspectMode} 后缓冲 ${screenW}x${screenH} 参考 ${refW}x${refH}`
          + ` ⇒ 画布 ${fit.canvasW}x${fit.canvasH} 原点 ${fit.originX},${fit.originY}`
          + ` 视口 ${fit.viewportW}x${fit.viewportH} 缩放 ${fit.scaleX},${fit.scaleY}`
          + `${fixedResolution ? ' · 固定分辨率模式（取值器短路缩放 1 / 原点 0）' : ''}`
          + `${fit.degenerate ? ' · 退化出口（T 非法）' : ''}`,
      );
    }
    return {
      // 兼容字段：规则的缩放 / 偏移短路由 fixedResolution 表达（会话侧不再用 noCamera 表达缺口）
      noCamera: false,
      scaleX: fit.scaleX,
      scaleY: fit.scaleY,
      offsetX: fit.originX,
      offsetY: fit.originY,
      displayMode: vfType,
      fixedResolution,
      // F = 形态字 S != 0 ⇒ 窗口态 1 / 占满屏态 0（见 `DisplayFitContext.screenMode`）
      screenForm: fixedResolution ? 1 : 0,
      viewMatrix: fit.view.slice(),
    };
  }

  /** 一次性诊断（同一个 code 只报一次；返回 true = 本次真的要报） */
  private fitNoteOnce(code: string): boolean {
    if (this.fitNotes.has(code)) return false;
    this.fitNotes.add(code);
    return true;
  }

  private log(level: 'info' | 'warn' | 'error', text: string): void {
    this.bridge.report({ kind: 'log', level, text });
  }
}

/* ──────────────────────────── demo（非游戏自检） ──────────────────────────── */

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

const DEMO_TEXT = String.fromCharCode(
  0x93, 0x56, 0x8c, 0x8b, 0x82, 0xa2, 0x83, 0x4c, 0x83, 0x83, 0x83, 0x62,
  0x83, 0x58, 0x83, 0x8b, 0x83, 0x7d, 0x83, 0x43, 0x83, 0x58, 0x83, 0x5e, 0x81, 0x5b,
) + ' / Amayui CN';
const DEMO_TEXT_SLOT = 1;

function intOperand(value: number): Operand {
  return { type: 0, raw: value >>> 0 };
}

async function applyOpcode(machine: Machine, opcode: number, ops: Operand[]): Promise<void> {
  const handler = machine.handlers.get(opcode);
  if (handler === undefined) throw new Error(`demo: 指令表里没有 opcode 0x${opcode.toString(16)} 的处理器`);
  machine.operandCount = 2 * ops.length + 1;
  await handler(machine, ops);
}

async function driveDemoScene(machine: Machine): Promise<void> {
  await applyOpcode(machine, 0x1f8, [intOperand(1), intOperand(512), intOperand(128), intOperand(1)]);
  machine.globalString.set(DEMO_TEXT_SLOT, DEMO_TEXT);
  await applyOpcode(machine, 0x204, [intOperand(1), intOperand(16), intOperand(16), { type: 5, raw: DEMO_TEXT_SLOT }]);
  await applyOpcode(machine, 0x1fb, [
    intOperand(1), intOperand(1), intOperand(0), intOperand(0),
    intOperand(512), intOperand(128), intOperand(0), intOperand(0),
  ]);
  await applyOpcode(machine, 0x33f, [intOperand(0), intOperand(255), intOperand(0xffffff)]);
  await applyOpcode(machine, 0x20d, [intOperand(1000)]);
}

/* ──────────────────────────── 启动入口 ──────────────────────────── */

function bootInfoOf(
  session: Awaited<ReturnType<typeof bootWindow>>,
  presentation: PresentationPolicy,
): {
  width: number;
  height: number;
  screenMode: number;
  fullscreenPresent: boolean;
  background: number;
  title: string;
  ignored: string[];
} {
  const ignoredFullscreen = presentation.ignoreFullscreen && session.screenMode === 0;
  return {
    width: Math.max(1, Math.round(window.innerWidth)),
    height: Math.max(1, Math.round(window.innerHeight)),
    // 模拟器把占满屏降级成窗口：诊断也按窗口态报告，避免显示“全屏”却实际是 1280×720 窗口
    screenMode: ignoredFullscreen ? 1 : session.screenMode,
    fullscreenPresent: ignoredFullscreen ? false : session.screenMode === 0,
    background: 0,
    title: WINDOW_TITLE,
    ignored: [
      ...(ignoredFullscreen
        ? [`fullscreen-ignored: 模拟器不支持全屏，按 ${presentation.width}×${presentation.height} 窗口运行`]
        : []),
      ...session.ignored.map((item) => `${item.rule} / ${item.what}`),
    ],
  };
}

export interface SessionHooks {
  boot(info: RendererBootInfo): void;
}

async function bootSession(config: ConfigStore, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<{
  session: Awaited<ReturnType<typeof bootWindow>>;
  bootHost: BridgeBootHost;
}> {
  const presentation: PresentationPolicy = {
    ignoreFullscreen: true,
    width: Math.max(1, config.getInt('set:WinX')),
    height: Math.max(1, config.getInt('set:WinY')),
  };
  const bootHost = new BridgeBootHost(bridge, pixi, presentation);
  const session = await bootWindow({
    host: bootHost,
    config,
    entry: {
      instance: PROCESS_INSTANCE,
      resourceInstance: RESOURCE_INSTANCE,
      showMode: SHOW_MODE_NORMAL,
    },
  });
  if (session.failure !== null) {
    throw new Error(
      `窗口 / 图形阶段失败：rule=${session.failure.rule} reason=${session.failure.reason} `
        + `step=${session.failure.step ?? '-'} errorText=${session.failure.errorText ?? '-'} `
        + `错误通道=${session.errors.length > 0 ? session.errors.join(' / ') : '空'}`,
    );
  }
  const boot = bootInfoOf(session, presentation);
  bridge.report({ kind: 'boot', info: boot });
  hooks.boot(boot);
  return { session, bootHost };
}

async function runInstall(job: Extract<StartJob, { mode: 'install' }>, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<void> {
  const host = new BridgeHost(bridge, job.platform);
  const env: CoreEnv = {
    host,
    table: buildOpcodeTable(job.opcodes),
    configKeys: buildConfigKeys(job.configKeys),
    renderHost: new LocalRenderHost(pixi, bridge),
  };
  const render = env.renderHost as LocalRenderHost;
  let windowSession: Awaited<ReturnType<typeof bootWindow>> | null = null;
  let machineRef: Machine | null = null;
  let lastYieldAt = 0;
  let lastProgressAt = 0;
  const clockOrigin = Date.now();
  const inputHost = new DomInputHost(pixi.canvasElement());

  let report;
  try {
    report = await analyzeInstall(
    env,
    job.installDir,
    job.rootId,
    {
      key: job.key,
      showLogo: job.showLogo,
      clock: () => Date.now() - clockOrigin,
      onProgress: (info) => {
        bridge.report({
          kind: 'log',
          level: 'info',
          text: `[progress] step=${info.step} script=${info.script} ip=0x${info.ip.toString(16)} last=${info.ms.toFixed(0)}ms`,
        });
      },
      onSlowStep: (info) => {
        bridge.report({
          kind: 'log',
          level: 'info',
          text: `[slow] opcode=0x${info.opcode.toString(16)} step=${info.step} ${info.ms.toFixed(1)}ms script=${info.script}`,
        });
      },
      inputHost,
      sleepHost: async (ms: number) => { await new Promise<void>((resolve) => { setTimeout(resolve, ms); }); },
      onBeforeScript: async (ctx) => {
        // 影片后端要按统一 id 取 archive 里的字节；ctx.install 是这一层唯一持有 Install 的地方
        env.movieHost = new LocalMovieHost(pixi, async (id) => (await ctx.install.readById(id)).bytes);
        const booted = await bootSession(ctx.config, bridge, pixi, hooks);
        windowSession = booted.session;
        // 显示适配的输入：桌面尺寸（`boot.screenSize` = 主显示器尺寸）+ 同一份配置 + 形态字。
        // ★ 宿主没报桌面尺寸（抛 / 缺）时传 `null` ⇒ `viewSnapshot` 用客户区顶上并记 `display-fit-ref-assumed`。
        let desktop: { width: number; height: number } | null = null;
        try {
          const size = await booted.bootHost.screenSize();
          desktop = { width: size.width, height: size.height };
        } catch (error) {
          bridge.report({ kind: 'log', level: 'warn', text: `显示适配：取桌面尺寸失败 ⇒ 参考尺寸按客户区算（display-fit-ref-assumed）：${messageOf(error)}` });
        }
        render.attachDisplayContext({
          config: ctx.config,
          desktop,
          screenMode: booted.session.screenMode,
        });
      },
      onMachine: async (machine) => {
        machineRef = machine;
        render.attachMachine(machine);
        if (windowSession === null) throw new Error('集成钩子次序错：onMachine 早于 onBeforeScript');
        windowSession.beginMainLoop();
      },
      onIteration: async () => {
        const session = windowSession;
        if (session === null) return;
        const progressNow = Date.now();
        if (machineRef !== null && progressNow - lastProgressAt >= 1000) {
          lastProgressAt = progressNow;
          const frame = machineRef.slots[machineRef.cur] ?? null;
          bridge.report({
            kind: 'log',
            level: 'info',
            text: `[progress] step=${machineRef.steps} cur=${machineRef.cur} script=${frame?.id === undefined ? 'root' : '0x' + frame.id.toString(16)} ip=0x${(frame?.ip ?? 0).toString(16)} flags=0x${(machineRef.flags >>> 0).toString(16)} anim=${machineRef.scene.animating}`,
          });
        }
        session.pump();
        if (render.syncScene()) await render.present();
        else render.presentLast();
        // ★ 渲染步收尾：本帧无动画 ⇒ 清命令表；随后清「强制完成」。必须在 present 之后。
        if (machineRef !== null) machineRef.finishRenderStep();
        if (machineRef !== null && (machineRef.flags & WAIT_BIT) !== 0) {
          await new Promise<void>((resolve) => { setTimeout(resolve, 4); });
          lastYieldAt = Date.now();
          return;
        }
        const now = Date.now();
        if (now - lastYieldAt >= 4) {
          // ★ 先等、再把 lastYieldAt 记成“回来之后”的时间。
          //   若先记时间，setTimeout(0) 在 Windows 上常要 8ms ⇒ 下一次迭代又满足 ≥4ms ⇒ 每条指令都让出一次
          //   （实测只有 ~125 条/秒）。记在 await 之后，才有 4ms 的连续执行窗口。
          await new Promise<void>((resolve) => { setTimeout(resolve, 0); });
          lastYieldAt = Date.now();
        }
      },
    },
    { config: job.configPath },
    );
  } finally {
    inputHost.destroy();
  }

  await render.flush();
  bridge.report({ kind: 'done', ok: true, report });
}

async function runDemo(job: Extract<StartJob, { mode: 'demo' }>, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<void> {
  const config = new ConfigStore(job.configKeys);
  for (const [key, value] of DEMO_CONFIG) config.setInt(key, value);
  const { session, bootHost } = await bootSession(config, bridge, pixi, hooks);
  const render = new LocalRenderHost(pixi, bridge);
  // demo 也接显示适配（同一份公式；`DEMO_CONFIG` 里 ScreenMode = 1 ⇒ 固定分辨率模式生效）
  let desktop: { width: number; height: number } | null = null;
  try {
    const size = await bootHost.screenSize();
    desktop = { width: size.width, height: size.height };
  } catch (error) {
    bridge.report({ kind: 'log', level: 'warn', text: `demo：取桌面尺寸失败 ⇒ 参考尺寸按客户区算（display-fit-ref-assumed）：${messageOf(error)}` });
  }
  render.attachDisplayContext({ config, desktop, screenMode: session.screenMode });
  const machine = new Machine({ body: new Uint8Array(0), table: new Map(), renderHost: render });
  render.attachMachine(machine);
  await driveDemoScene(machine);
  await render.flush();
  bridge.report({
    kind: 'log',
    level: 'info',
    text: `demo 场景：纹理 ${machine.scene.textureObjects.size} 个 · 绘制项 ${machine.scene.drawItems.size} 个 · `
      + `目标 ${machine.render.target} · 错误缓冲 ${machine.scene.errorBuffer === '' ? '空' : machine.scene.errorBuffer}`,
  });
  bridge.report({ kind: 'demo-ready' });
  await bridge.invoke('boot.allowClose', {});
  if (session.hasPeriodicTimer) bridge.report({ kind: 'status', level: 'warn', text: 'demo 不应有周期定时器' });
}

/** renderer 的启动入口：main 发来 `start` 后调用一次。 */
export async function runStartJob(job: StartJob, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<void> {
  try {
    if (job.mode === 'install') await runInstall(job, bridge, pixi, hooks);
    else await runDemo(job, bridge, pixi, hooks);
  } catch (error) {
    if (error instanceof EngineExit) {
      // 正常退出：core 取到退出消息；renderer 只需要告诉 main 可以收尾。
      bridge.report({ kind: 'done', ok: true });
      return;
    }
    bridge.report({ kind: 'log', level: 'error', text: messageOf(error) });
    bridge.report({ kind: 'done', ok: false, error: messageOf(error) });
  }
}
