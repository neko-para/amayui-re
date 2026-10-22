/**
 * Electron **preload**：把 `protocol.ts` 的 `AmayuiBridge` 用 `contextBridge` 暴露成
 * renderer 里的 `window.amayui`。
 *
 * ★ 只做转发：不认识 core、不认识游戏语义、不做渲染。core 已经在 renderer 里，
 *   所以跨线的是「host / boot 请求 + 窗口消息 + 事件」，不是逐次绘制调用。
 * ★ `contextIsolation: true` + `nodeIntegration: false`：renderer 拿不到 `require` / `ipcRenderer`。
 * ★ 频道名是字面量；main 侧 `window-host.ts` 用同一组字符串。
 * ★ preload 是 `.ts`，由 Electron 45 的 Node 24 直接剥类型；静态 import 只保留运行期需要的
 *   `electron`，其余全是 `import type`。
 */
import { contextBridge, ipcRenderer } from 'electron';

import type { AmayuiBridge, DebugEvent, RendererEvent, RendererReady, StartJob, WindowMessage } from './protocol.ts';

const CHANNEL_BRIDGE_INVOKE = 'bridge:invoke';
const CHANNEL_BRIDGE_START = 'bridge:start';
const CHANNEL_BRIDGE_WINDOW = 'bridge:window';
const CHANNEL_RENDERER_EVENT = 'renderer:event';
const CHANNEL_DEBUG_EVENT = 'debug:event';

const bridge: AmayuiBridge = {
  ready(info: RendererReady): void {
    ipcRenderer.send(CHANNEL_RENDERER_EVENT, { kind: 'ready', info } satisfies RendererEvent);
  },

  onStart(handler: (job: StartJob) => void): void {
    ipcRenderer.on(CHANNEL_BRIDGE_START, (_event, job: StartJob) => { handler(job); });
  },

  onWindowMessage(handler: (message: WindowMessage) => void): void {
    ipcRenderer.on(CHANNEL_BRIDGE_WINDOW, (_event, message: WindowMessage) => { handler(message); });
  },

  invoke(method, params): Promise<unknown> {
    return ipcRenderer.invoke(CHANNEL_BRIDGE_INVOKE, method, params) as Promise<unknown>;
  },

  report(event: RendererEvent): void {
    ipcRenderer.send(CHANNEL_RENDERER_EVENT, event);
  },

  onDebug(handler: (event: DebugEvent) => void): void {
    ipcRenderer.on(CHANNEL_DEBUG_EVENT, (_event, payload: DebugEvent) => { handler(payload); });
  },
};

contextBridge.exposeInMainWorld('amayui', bridge);
