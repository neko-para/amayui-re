/**
 * `ElectronRenderHost` —— 核心 `RenderHost`（`src/core/render.ts`）的 **IPC 实现** + 场景快照推送。
 *
 * ★ 分工（见 `protocol.ts`）：
 *   - 核心只发**领域请求**（纹理槽号 / 坐标 / 文本）；本类把每个请求翻译成一条 `RenderCall`，
 *     经 `webContents.send('render:call', call)` 发给渲染进程，等 `ipcMain.on('render:result')` 回来；
 *   - 平台对象（`RenderTexture` / 后缓冲）**不能跨进程**：线上只有数字 `surfaceId`，
 *     本类把它包成核心的不透明 `SurfaceHandle`（核心一个成员都不读）；
 *   - 失败**只记日志**、按 `RenderHost` 的契约返回 `null` / `false`（或对 `drawText` 静默）：
 *     ⛔ 不把平台错误伪装成引擎错误 —— `0x20d` 的失败文本由**核心**写 `scene.errorBuffer`。
 *
 * ★ 请求 / 响应：`Map<id, {resolve, timer}>` + 10s 超时。超时只 reject 本类的 promise（调用点记日志），
 *   迟到的 `render:result` 被丢弃并留一条 warn。
 * ★ 渲染进程 ready：`renderer:ready` 的 Promise（15s 超时）。设备创建（`createDevice`）等它 ——
 *   ⛔ 不等就发 `init` 会得到一个没人处理的调用，`createDevice` 的"失败"会退化成事后补救。
 * ★ 场景快照（`SceneSnapshot`）**不是**规范状态，只是可序列化视图；推送**节流**（缺省 16ms / 强制）：
 *   - `syncScene()`：时间节流（主循环每个迭代头可以放心调）；
 *   - 在 `getRenderSurface` / `drawText` 之前，若该纹理的形状**还没推过**（renderer 不知道宽高就建不出
 *     `RenderTexture`）⇒ 强制推一次；否则走时间节流。
 *
 * ★ 本文件是**主进程**代码：允许 `electron` / `node:*`；核心（`src/core/**`）仍然一行都不认识它们。
 */
import { ipcMain } from 'electron';
import type { BrowserWindow } from 'electron';

import type { Machine } from '../../core/machine.ts';
import type { DrawTextRequest, RenderHost, SurfaceHandle } from '../../core/render.ts';
import type {
  DrawItemSnapshot,
  PushMessage,
  RenderCall,
  RenderResult,
  RendererInit,
  RendererReady,
  SceneSnapshot,
  SurfaceRef,
  TextureSnapshot,
} from './protocol.ts';

/** 日志级别（主进程 stdout；⛔ 不往渲染进程控制台写业务结论） */
export type LogLevel = 'info' | 'warn' | 'error';
export type LogFn = (level: LogLevel, text: string) => void;

/**
 * IPC 频道名。
 * ★ `protocol.ts` 只有**类型**（没有运行期常量），所以频道名的唯一落点是这里；
 *   `preload.ts` 必须用**同样的字面量**（它不能 import 本文件：那会在渲染进程里拉进 `ipcMain`）。
 */
export const CHANNEL_RENDER_READY = 'renderer:ready';
export const CHANNEL_RENDER_CALL = 'render:call';
export const CHANNEL_RENDER_RESULT = 'render:result';
export const CHANNEL_RENDER_PUSH = 'render:push';
export const CHANNEL_RENDERER_LOG = 'renderer:log';

/** 单次 IPC 调用的超时（毫秒） */
export const DEFAULT_CALL_TIMEOUT_MS = 10_000;
/** 渲染进程 ready 的等待上限（毫秒） */
export const DEFAULT_READY_TIMEOUT_MS = 15_000;
/** 场景推送节流（毫秒） */
export const DEFAULT_SCENE_THROTTLE_MS = 16;

export interface ElectronRenderHostOptions {
  /**
   * 取当前窗口。**惰性**：宿主在窗口建出来之前就构造好了（IPC 监听必须早于页面加载）。
   * 返回 `null` / 已销毁的窗口 ⇒ 调用失败（记日志，不抛给核心）。
   */
  browserWindow(): BrowserWindow | null;
  /** 客户区尺寸（场景快照的 `width` / `height`；窗口还没建出来时由调用方给屏幕尺寸） */
  clientSize(): { width: number; height: number };
  log?: LogFn;
  callTimeoutMs?: number;
  readyTimeoutMs?: number;
  sceneThrottleMs?: number;
}

/** 一次待回执的调用 */
interface PendingCall {
  resolve(result: RenderResult): void;
  reject(error: Error): void;
  timer: NodeJS.Timeout;
}

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

/**
 * 渲染宿主（主进程侧）。
 * ★ 实现核心的 `RenderHost`（`getRenderSurface` / `setRenderTarget` / `getBackBuffer` /
 *   `releaseSurface` / `drawText` / `present`），另外给平台层提供：
 *   `waitForReady`（设备创建）、`pushBoot` / `pushStatus` / `syncScene`（状态推送）。
 */
export class ElectronRenderHost implements RenderHost {
  private readonly browserWindow: () => BrowserWindow | null;
  private readonly clientSize: () => { width: number; height: number };
  private readonly log: LogFn;
  private readonly callTimeoutMs: number;
  private readonly readyTimeoutMs: number;
  private readonly sceneThrottleMs: number;

  private nextCallId = 1;
  private readonly pending = new Map<number, PendingCall>();
  /** 表面句柄 → renderer 分配的 `surfaceId`（`SurfaceHandle` 对核心不透明，本类用对象身份反查） */
  private readonly surfaceIds = new WeakMap<object, { surfaceId: number; kind: 'texture' | 'backbuffer' }>();

  private readonly readyPromise: Promise<RendererReady>;
  private resolveReady: (info: RendererReady) => void = () => {};
  private readyInfo: RendererReady | null = null;

  private machine: Machine | null = null;
  /** 只记一次的日志键（避免每条指令刷屏） */
  private lastScenePushAt = 0;
  /** 已推给 renderer 的纹理形状（"renderer 知道这个槽多大"的判据；变了就强制推） */
  private readonly pushedTextures = new Map<number, string>();

  private readonly onResult = (_event: unknown, result: RenderResult): void => { this.settle(result); };
  private readonly onReady = (_event: unknown, info: RendererReady): void => { this.handleReady(info); };
  private readonly onRendererLog = (_event: unknown, text: unknown): void => {
    this.log('info', `[renderer] ${typeof text === 'string' ? text : String(text)}`);
  };

  constructor(options: ElectronRenderHostOptions) {
    this.browserWindow = options.browserWindow;
    this.clientSize = options.clientSize;
    this.log = options.log ?? ((level, text) => {
      if (level === 'error') console.error(text);
      else if (level === 'warn') console.warn(text);
      else console.log(text);
    });
    this.callTimeoutMs = options.callTimeoutMs ?? DEFAULT_CALL_TIMEOUT_MS;
    this.readyTimeoutMs = options.readyTimeoutMs ?? DEFAULT_READY_TIMEOUT_MS;
    this.sceneThrottleMs = options.sceneThrottleMs ?? DEFAULT_SCENE_THROTTLE_MS;

    this.readyPromise = new Promise<RendererReady>((resolve) => { this.resolveReady = resolve; });

    // ★ 监听必须在建窗 / 加载页面**之前**注册（renderer 的 ready 可能在主进程准备好之前就发出来）。
    ipcMain.on(CHANNEL_RENDER_RESULT, this.onResult);
    ipcMain.on(CHANNEL_RENDER_READY, this.onReady);
    ipcMain.on(CHANNEL_RENDERER_LOG, this.onRendererLog);
  }

  /** 解绑 IPC 监听并让所有在飞的调用失败（退出清理；幂等） */
  dispose(): void {
    ipcMain.removeListener(CHANNEL_RENDER_RESULT, this.onResult);
    ipcMain.removeListener(CHANNEL_RENDER_READY, this.onReady);
    ipcMain.removeListener(CHANNEL_RENDERER_LOG, this.onRendererLog);
    for (const [, entry] of this.pending) {
      clearTimeout(entry.timer);
      entry.reject(new Error('render-host-disposed'));
    }
    this.pending.clear();
    this.machine = null;
  }

  /* ──────────────────────────── 渲染进程 ready / 状态推送 ──────────────────────────── */

  private handleReady(info: RendererReady): void {
    if (this.readyInfo !== null) return;
    this.readyInfo = info;
    this.log('info', `renderer ready：backend=${info.backend} ${info.width}x${info.height}`);
    this.resolveReady(info);
  }

  /** 渲染进程上报过 ready 吗（不发请求） */
  get rendererReady(): RendererReady | null {
    return this.readyInfo;
  }

  /** 等渲染进程 ready；超时 ⇒ `null`（**不抛**：设备创建把它翻译成"平台失败"） */
  async waitForReady(timeoutMs: number = this.readyTimeoutMs): Promise<RendererReady | null> {
    if (this.readyInfo !== null) return this.readyInfo;
    return await new Promise<RendererReady | null>((resolve) => {
      let done = false;
      const timer = setTimeout(() => {
        if (done) return;
        done = true;
        this.log('error', `渲染进程 ${timeoutMs}ms 内没有上报 ready`);
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

  /**
   * 设备创建（`GraphicsHost.createDevice` 的 IPC 部分）：等 ready，再发 `init`。
   * @returns `true` = 渲染后端已按这批参数初始化
   */
  async initRenderer(params: RendererInit): Promise<boolean> {
    const ready = await this.waitForReady();
    if (ready === null) return false;
    try {
      await this.call('init', params);
      return true;
    } catch (err) {
      this.log('error', `renderer init 失败：${message(err)}`);
      return false;
    }
  }

  /** 推送启动信息（`PushMessage` 的 `boot` 分支；渲染进程可以据此画"未启动 / 已启动"） */
  pushBoot(info: RendererInit, title: string, ignored: string[]): void {
    this.push({ kind: 'boot', info: { ...info, title, ignored } });
  }

  /** 推送一条状态文本 */
  pushStatus(level: LogLevel, text: string): void {
    this.push({ kind: 'status', level, text });
  }

  /** 把 machine 挂上来（场景快照的唯一数据源）；`null` = 卸下 */
  attachMachine(machine: Machine | null): void {
    this.machine = machine;
    if (machine === null) return;
    // 挂上就算"renderer 什么都不知道"⇒ 下一次 syncScene 强制推
    this.pushedTextures.clear();
    this.lastScenePushAt = 0;
  }

  /* ──────────────────────────── 场景快照 ──────────────────────────── */

  /**
   * 把当前场景推给 renderer（时间节流）。
   * ★ 主循环的迭代头可以放心调：16ms 内重复调用直接返回（不建快照、不发 IPC）。
   * ★ `force: true`：跳过节流（`--demo` / `--smoke` 的收尾、纹理形状变化）。
   * @returns `true` = 这一趟真的推了一条（调用方可以据此安排呈现节拍）
   */
  syncScene(options: { force?: boolean } = {}): boolean {
    const machine = this.machine;
    if (machine === null) return false;
    const now = Date.now();
    if (options.force !== true && now - this.lastScenePushAt < this.sceneThrottleMs) return false;
    this.lastScenePushAt = now;
    const scene = this.snapshot(machine);
    this.push({ kind: 'scene', scene });
    for (const texture of scene.textures) {
      this.pushedTextures.set(texture.id, textureShape(texture.kind, texture.width, texture.height));
    }
    return true;
  }

  /**
   * 发绘制请求之前：renderer 必须已经知道这个纹理槽的宽高（否则建不出 `RenderTexture`）。
   * 形状没推过 / 变了 ⇒ 强制推一次；否则只按时间节流。
   */
  private sceneBeforeTexture(textureId: number): void {
    const machine = this.machine;
    if (machine === null) return;
    const object = machine.scene.textureObjects.get(textureId);
    const shape = object ? textureShape(object.kind, object.width, object.height) : 'none';
    if (this.pushedTextures.get(textureId) !== shape) {
      this.syncScene({ force: true });
      return;
    }
    this.syncScene();
  }

  /** 场景 → 可序列化快照（⛔ 不是规范状态：字段全是 `protocol.ts` 的视图） */
  private snapshot(machine: Machine): SceneSnapshot {
    const { width, height } = this.clientSize();
    const textures: TextureSnapshot[] = [];
    for (const [id, object] of machine.scene.textureObjects) {
      const record = machine.scene.textures.get(id);
      textures.push({
        id,
        kind: object.kind,
        width: object.width,
        height: object.height,
        // -1 = 无来源文件（本层没有图像解码，见 `RES-RENDER-TEXT-SURFACE`）
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
        // 当前颜色：`colorFrom` 是核心的"当前颜色"格；-1 = 无颜色（⛔ 不是 0）
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

  private push(message: PushMessage): void {
    const win = this.browserWindow();
    if (win === null || win.isDestroyed()) {
      this.log('warn', `没有窗口，丢弃推送 ${message.kind}`);
      return;
    }
    win.webContents.send(CHANNEL_RENDER_PUSH, message);
  }

  /* ──────────────────────────── 核心 RenderHost ──────────────────────────── */

  /**
   * 一次只读询问：这个纹理槽的渲染表面（`0x20d` 取表面 / `0x204` 入口条件 ② 共用）。
   * ★ renderer 的回复是 `SurfaceRef`（`{surfaceId, kind}`，见 `protocol.ts`）—— 跨线的只有数字；
   *   本类把它包成核心的不透明句柄。取不到 / 平台失败 ⇒ `null`。
   */
  async getRenderSurface(textureId: number): Promise<SurfaceHandle | null> {
    this.sceneBeforeTexture(textureId);
    try {
      const ref = await this.call<SurfaceRef | null>('getRenderSurface', { textureId });
      if (ref === null || ref === undefined) return null;
      return this.surfaceHandle(ref.surfaceId, ref.kind);
    } catch (err) {
      this.log('error', `getRenderSurface(${textureId}) 失败：${message(err)}`);
      return null;
    }
  }

  /** 把一份表面设为当前渲染目标；`false` = 平台失败（`0x20d` 的失败路径由核心写错误通道） */
  async setRenderTarget(surface: SurfaceHandle): Promise<boolean> {
    const ref = this.surfaceIds.get(surface as unknown as object);
    if (ref === undefined) {
      this.log('error', 'setRenderTarget：收到本宿主没造过的表面句柄');
      return false;
    }
    try {
      await this.call('setRenderTarget', { surfaceId: ref.surfaceId });
      return true;
    } catch (err) {
      this.log('error', `setRenderTarget(${ref.surfaceId}) 失败：${message(err)}`);
      return false;
    }
  }

  /** 平台后缓冲（`0x20d` 的 ≥ 1000 那一支）：renderer 侧固定 `surfaceId = 0`（`SurfaceRef` 的 kind = backbuffer） */
  async getBackBuffer(): Promise<SurfaceHandle | null> {
    try {
      const ref = await this.call<SurfaceRef | null>('getBackBuffer', {});
      return this.surfaceHandle(ref?.surfaceId ?? 0, 'backbuffer');
    } catch (err) {
      this.log('error', `getBackBuffer 失败：${message(err)}`);
      return null;
    }
  }

  /** 归还一份表面引用（成功与失败路径都归还；只记日志） */
  async releaseSurface(surface: SurfaceHandle): Promise<void> {
    const ref = this.surfaceIds.get(surface as unknown as object);
    if (ref === undefined) {
      this.log('warn', 'releaseSurface：收到本宿主没造过的表面句柄');
      return;
    }
    try {
      await this.call('releaseSurface', { surfaceId: ref.surfaceId });
    } catch (err) {
      this.log('error', `releaseSurface(${ref.surfaceId}) 失败：${message(err)}`);
    }
  }

  /**
   * 把一段文本画进某个纹理槽（`0x204` 的唯一一次绘制请求）。
   *
   * 返回 `true` = 请求被受理；`false` = **平台侧加锁 / 取矩形失败**（核心据此写错误通道
   * `LockTexture エラー`）。⇒ 本方法**原样返回 `RenderResult.ok`**，IPC 失败 / 超时同样翻译成
   * `false`，⛔ 不吞成"成功"。
   *
   * ★ 字体对象状态（`protocol.ts` 的 `RenderCall['drawText']` 可选字段）**全部过线**：
   *   `size`（默认 24）/ `weight` / `shadowColor` / `drawMode`（默认 1）/ `alignMode`。
   *   renderer 侧对这些可选字段有自己的缺省；本层照核心给的转发（⛔ 不在这里改写它们的值）。
   */
  async drawText(request: DrawTextRequest): Promise<boolean> {
    this.sceneBeforeTexture(request.textureId);
    try {
      const result = await this.callResult('drawText', {
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
      if (!result.ok) {
        this.log('error', `drawText(纹理 ${request.textureId} @${request.x},${request.y}) 平台拒绝：${result.error ?? '（没有 error 文本）'}`);
      }
      return result.ok;
    } catch (err) {
      this.log('error', `drawText(纹理 ${request.textureId} @${request.x},${request.y}) 失败：${message(err)}`);
      return false;
    }
  }

  /**
   * 可选：把当前后缓冲提交到屏幕。
   * ★ 核心**不调用**它（呈现时序未取证，见 `RES-RENDER-TARGET-ORDER`）⇒ 只给平台入口用。
   */
  async present(): Promise<void> {
    try {
      await this.call('present', {});
    } catch (err) {
      this.log('error', `present 失败：${message(err)}`);
    }
  }

  /* ──────────────────────────── IPC 请求 / 响应 ──────────────────────────── */

  /**
   * 发一条 `RenderCall` 并等**回执本身**（含 `ok`，不因 `ok: false` 抛）。
   * ★ 给"平台失败也是业务结果"的调用点用（`drawText`：`false` = 加锁失败，由核心写错误通道）。
   * @throws 窗口不在 / 超时
   */
  private async callResult(method: RenderCall['method'], params: unknown): Promise<RenderResult> {
    const win = this.browserWindow();
    if (win === null || win.isDestroyed()) throw new Error(`render-window-missing:${method}`);
    const id = this.nextCallId;
    this.nextCallId += 1;
    // ★ 联合类型的构造：`method` 与 `params` 的组合由调用点保证（每个方法只发自己那一种）
    const call = { id, method, params } as unknown as RenderCall;
    return await new Promise<RenderResult>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`render-call-timeout:${method}:${this.callTimeoutMs}ms`));
      }, this.callTimeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      win.webContents.send(CHANNEL_RENDER_CALL, call);
    });
  }

  /**
   * 发一条 `RenderCall` 并等**成功的结果**（`ok: false` 也变成异常）。
   * @throws 窗口不在 / 超时 / renderer 报 `ok: false`
   */
  private async call<T>(method: RenderCall['method'], params: unknown): Promise<T> {
    const result = await this.callResult(method, params);
    if (!result.ok) throw new Error(`render-call-failed:${method}:${result.error ?? '（没有 error 文本）'}`);
    return result.result as T;
  }

  private settle(result: RenderResult): void {
    if (typeof result?.id !== 'number') {
      this.log('warn', 'render:result 没有数字 id，丢弃');
      return;
    }
    const entry = this.pending.get(result.id);
    if (entry === undefined) {
      this.log('warn', `render:result 的 id ${result.id} 没有在飞的请求（超时后被丢弃？）`);
      return;
    }
    this.pending.delete(result.id);
    clearTimeout(entry.timer);
    entry.resolve(result);
  }

  /** `surfaceId` → 核心的不透明句柄（核心一个成员都不读；本类用对象身份反查） */
  private surfaceHandle(surfaceId: number, kind: 'texture' | 'backbuffer'): SurfaceHandle {
    const handle = { surfaceId, kind };
    this.surfaceIds.set(handle, { surfaceId, kind });
    return handle as unknown as SurfaceHandle;
  }
}

function textureShape(kind: string, width: number, height: number): string {
  return `${kind}:${width}x${height}`;
}
