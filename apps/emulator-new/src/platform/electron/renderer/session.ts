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
import type { Operand } from '../../../core/machine.ts';
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
import type { DrawTextRequest, RenderHost, SurfaceHandle } from '../../../core/render.ts';
import type { Host } from '../../../core/install.ts';
import type {
  AmayuiBridge,
  PlatformInfo,
  RendererBootInfo,
  SceneSnapshot,
  StartJob,
  SurfaceRef,
  TextureSnapshot,
  DrawItemSnapshot,
  RenderCall,
} from '../protocol.ts';
import { dirnamePath, joinPath, pathFromEngine } from './paths.ts';
import type { PixiRenderHost } from './pixi-host.ts';

const QUIT = Symbol('quit-message');
type QueueEntry = WindowMessage | typeof QUIT;

const PROCESS_INSTANCE = 1;
const RESOURCE_INSTANCE = 1;
const SHOW_MODE_NORMAL = 1;
const WINDOW_TITLE = 'ARCGameEngine';

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
    return await this.bridge.invoke('host.readBytes', { path: file }) as Uint8Array;
  }

  async readBytesMaybe(file: string): Promise<Uint8Array | null> {
    const result = await this.bridge.invoke('host.readBytesMaybe', { path: file });
    return (result ?? null) as Uint8Array | null;
  }

  async readSlice(file: string, offset: number, length: number): Promise<Uint8Array> {
    return await this.bridge.invoke('host.readSlice', { path: file, offset, length }) as Uint8Array;
  }

  async readText(file: string): Promise<string> {
    return await this.bridge.invoke('host.readText', { path: file }) as string;
  }

  async isFile(file: string): Promise<boolean> {
    return await this.bridge.invoke('host.isFile', { path: file }) as boolean;
  }

  async ensureReadDir(dir: string): Promise<void> {
    await this.bridge.invoke('host.ensureReadDir', { dir });
  }

  async writeBytes(file: string, data: Uint8Array): Promise<void> {
    await this.bridge.invoke('host.writeBytes', { path: file, data });
  }
}

/* ──────────────────────────── BootHost（窗口 / 消息 / 图形） ──────────────────────────── */

class BridgeBootHost implements BootHost {
  private readonly bridge: AmayuiBridge;
  private readonly pixi: PixiRenderHost;
  private readonly queue: QueueEntry[] = [];
  private readonly timers = new Map<number, ReturnType<typeof setInterval>>();
  private foreground = false;

  constructor(bridge: AmayuiBridge, pixi: PixiRenderHost) {
    this.bridge = bridge;
    this.pixi = pixi;
    bridge.onWindowMessage((message) => {
      if (message.kind === 'activate') this.foreground = message.active;
      this.queue.push(message);
    });
  }

  /* WindowHost */

  async frameMetrics(): Promise<FrameMetrics> {
    return await this.bridge.invoke('boot.frameMetrics', {}) as FrameMetrics;
  }

  async screenSize(): Promise<ScreenSize> {
    return await this.bridge.invoke('boot.screenSize', {}) as ScreenSize;
  }

  async registerWindowClass(params: WindowClassParams): Promise<WindowClassHandle | null> {
    return await this.bridge.invoke('boot.registerWindowClass', params) as WindowClassHandle | null;
  }

  async createWindow(params: WindowCreateParams): Promise<WindowHandle | null> {
    return await this.bridge.invoke('boot.createWindow', params) as WindowHandle | null;
  }

  showWindow(window: WindowHandle, showMode: number): void {
    void this.bridge.invoke('boot.showWindow', { window, showMode });
  }

  updateWindow(window: WindowHandle): void {
    void this.bridge.invoke('boot.updateWindow', { window });
  }

  associateInputContext(window: WindowHandle, context: PlatformHandle | null): void {
    void this.bridge.invoke('boot.associateInputContext', { window, context });
  }

  async getMenu(window: WindowHandle): Promise<PlatformHandle | null> {
    return await this.bridge.invoke('boot.getMenu', { window }) as PlatformHandle | null;
  }

  operateMenu(params: { category: number; window: WindowHandle; menu: PlatformHandle | null; extra: number }): void {
    void this.bridge.invoke('boot.operateMenu', params);
  }

  drawMenuBar(window: WindowHandle): void {
    void this.bridge.invoke('boot.drawMenuBar', { window });
  }

  isForegroundWindow(window: WindowHandle): boolean {
    void window;
    return this.foreground;
  }

  hideWindow(window: WindowHandle): void {
    void this.bridge.invoke('boot.hideWindow', { window });
  }

  destroyWindow(window: WindowHandle): void {
    void this.bridge.invoke('boot.destroyWindow', { window });
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
    void this.bridge.invoke('boot.setThreadExecutionState', { flags });
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
    const size = await this.bridge.invoke('boot.screenSize', {}) as ScreenSize;
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
          screenMode: params.screenMode,
          fullscreenPresent: params.fullscreenPresent,
          background: 0,
        },
      });
    } catch (error) {
      this.log('error', `Pixi init 失败：${messageOf(error)}`);
      return null;
    }
    this.log('info', `createDevice → Pixi ${width}x${height}（screenMode ${params.screenMode}）`);
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

function textureShape(kind: string, width: number, height: number): string {
  return `${kind}:${width}x${height}`;
}

class LocalRenderHost implements RenderHost {
  private readonly pixi: PixiRenderHost;
  private readonly bridge: AmayuiBridge;
  private machine: Machine | null = null;
  private nextId = 1;
  private lastScenePushAt = 0;
  private readonly pushedTextures = new Map<number, string>();
  private readonly surfaceIds = new WeakMap<object, SurfaceRef>();

  constructor(pixi: PixiRenderHost, bridge: AmayuiBridge) {
    this.pixi = pixi;
    this.bridge = bridge;
  }

  attachMachine(machine: Machine | null): void {
    this.machine = machine;
    this.pushedTextures.clear();
    this.lastScenePushAt = 0;
  }

  /** 推一次场景（节流；`force` 跳过节流）。返回是否真的推了。 */
  syncScene(force = false): boolean {
    const machine = this.machine;
    if (machine === null) return false;
    const now = Date.now();
    if (!force && now - this.lastScenePushAt < 16) return false;
    this.lastScenePushAt = now;
    const scene = this.snapshot(machine);
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
      drawItems.push({
        key,
        flags: item.flags,
        textureId: item.textureId,
        left: item.left,
        top: item.top,
        right: item.right,
        bottom: item.bottom,
        color: item.colorFrom,
        aux: [item.aux[0], item.aux[1], item.aux[2]],
      });
    }
    return {
      width,
      height,
      textures,
      drawItems,
      blend: { mode: machine.render.blendMode, color: machine.render.blendColor },
      target: machine.render.target,
    };
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

function bootInfoOf(session: Awaited<ReturnType<typeof bootWindow>>): {
  width: number;
  height: number;
  screenMode: number;
  fullscreenPresent: boolean;
  background: number;
  title: string;
  ignored: string[];
} {
  return {
    width: Math.max(1, Math.round(window.innerWidth)),
    height: Math.max(1, Math.round(window.innerHeight)),
    screenMode: session.screenMode,
    fullscreenPresent: session.screenMode === 0,
    background: 0,
    title: WINDOW_TITLE,
    ignored: session.ignored.map((item) => `${item.rule} / ${item.what}`),
  };
}

export interface SessionHooks {
  boot(info: RendererBootInfo): void;
}

async function bootSession(config: ConfigStore, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<{
  session: Awaited<ReturnType<typeof bootWindow>>;
  bootHost: BridgeBootHost;
}> {
  const bootHost = new BridgeBootHost(bridge, pixi);
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
  const boot = bootInfoOf(session);
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
  const clockOrigin = Date.now();

  const report = await analyzeInstall(
    env,
    job.installDir,
    job.rootId,
    {
      key: job.key,
      clock: () => Date.now() - clockOrigin,
      onBeforeScript: async (ctx) => {
        const booted = await bootSession(ctx.config, bridge, pixi, hooks);
        windowSession = booted.session;
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
        session.pump();
        if (render.syncScene()) void render.present();
        if (machineRef !== null && (machineRef.flags & WAIT_BIT) !== 0) {
          await new Promise<void>((resolve) => { setTimeout(resolve, 4); });
          lastYieldAt = Date.now();
          return;
        }
        const now = Date.now();
        if (now - lastYieldAt >= 4) {
          lastYieldAt = now;
          await new Promise<void>((resolve) => { setTimeout(resolve, 0); });
        }
      },
    },
    { config: job.configPath },
  );

  await render.flush();
  bridge.report({ kind: 'done', ok: true, report });
}

async function runDemo(job: Extract<StartJob, { mode: 'demo' }>, bridge: AmayuiBridge, pixi: PixiRenderHost, hooks: SessionHooks): Promise<void> {
  const config = new ConfigStore(job.configKeys);
  for (const [key, value] of DEMO_CONFIG) config.setInt(key, value);
  const { session } = await bootSession(config, bridge, pixi, hooks);
  const render = new LocalRenderHost(pixi, bridge);
  const machine = new Machine({ body: new Uint8Array(0), table: new Map(), key: 0, renderHost: render });
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
