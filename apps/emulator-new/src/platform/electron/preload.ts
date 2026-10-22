/**
 * Electron **preload**：把主进程 ↔ 渲染进程的**线协议**（`protocol.ts` 的 `AmayuiBridge`）
 * 用 `contextBridge` 暴露成渲染进程里的 `window.amayui`。
 *
 * ★ 本文件**只做转发**：不认识业务、不解释场景快照、不做渲染。渲染进程的 `PixiRenderHost`
 *   调 `ready` / `onCall` / `reply` / `onPush` / `log`，主进程的 `ElectronRenderHost` 处理另一头。
 * ★ `contextIsolation: true` + `nodeIntegration: false`：渲染进程拿不到 `require` / `ipcRenderer`，
 *   只有 `window.amayui` 这一组函数（参数与返回值都必须是可结构化克隆的纯数据 —— 协议里没有平台对象）。
 * ★ 频道名是**字面量**，与 `render-host.ts` 的 `CHANNEL_*` 常量必须一致。
 *   ⛔ 本文件不能 import 那份常量：那会把主进程的 `ipcMain` 拉进渲染进程的 preload。
 * ★ 为什么 preload 是 `.ts` 而不用构建：Electron 45 的 Node 24 直接跑 `.ts`（类型剥壳）。
 *   静态 `import` 只保留在**运行期**需要的那一条（`electron`）；协议类型用 `import type`（整条擦掉）。
 */
import { contextBridge, ipcRenderer } from 'electron';

import type { AmayuiBridge, PushMessage, RenderCall, RenderResult, RendererReady } from './protocol.ts';

const CHANNEL_RENDER_READY = 'renderer:ready';
const CHANNEL_RENDER_CALL = 'render:call';
const CHANNEL_RENDER_RESULT = 'render:result';
const CHANNEL_RENDER_PUSH = 'render:push';
const CHANNEL_RENDERER_LOG = 'renderer:log';

const bridge: AmayuiBridge = {
  /** 渲染进程初始化完成（Pixi / WebGL 就绪）后调一次 */
  ready(info: RendererReady): void {
    ipcRenderer.send(CHANNEL_RENDER_READY, info);
  },

  /** 主进程发来的调用（渲染进程的回执走 `reply`） */
  onCall(handler: (call: RenderCall) => void): void {
    ipcRenderer.on(CHANNEL_RENDER_CALL, (_event, call: RenderCall) => { handler(call); });
  },

  /** 回复一次调用（`id` 必须原样回传；`ok: false` 只表示平台实现失败） */
  reply(result: RenderResult): void {
    ipcRenderer.send(CHANNEL_RENDER_RESULT, result);
  },

  /** 主进程推来的状态（场景快照 / 状态文本 / 启动信息；不需要回执） */
  onPush(handler: (message: PushMessage) => void): void {
    ipcRenderer.on(CHANNEL_RENDER_PUSH, (_event, message: PushMessage) => { handler(message); });
  },

  /** 日志（写到主进程 stdout；⛔ 不往渲染进程控制台写业务结论） */
  log(text: string): void {
    ipcRenderer.send(CHANNEL_RENDERER_LOG, text);
  },
};

contextBridge.exposeInMainWorld('amayui', bridge);
