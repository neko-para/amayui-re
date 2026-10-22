/**
 * Electron 主进程 ↔ 渲染进程（Pixi）之间的**线协议**。
 *
 * ★ 本文件只有类型（`interface` / `type` / `export type`），没有运行期代码，也没有 Node / DOM import：
 *   主进程、preload、renderer 三边共用同一份契约，避免各写一套字符串。
 * ★ 分工：
 *   - 主进程的 `ElectronRenderHost` 实现核心的 `RenderHost`，把每次请求翻译成 `RenderCall`；
 *   - preload 只做 `contextBridge` 转发，不认识业务；
 *   - renderer 的 `PixiRenderHost` 执行 `RenderCall` 并 `reply`。
 * ★ 平台对象（`RenderTexture` / 后缓冲）**不能跨进程**：跨线的是数字 `surfaceId`；
 *   主进程把 `surfaceId` 包成核心的 `SurfaceHandle`（不透明），核心原样传回。
 */

/** 渲染进程启动后回给主进程的“我准备好了”（Pixi / WebGL 已初始化） */
export interface RendererReady {
  renderer: 'pixi';
  /** Pixi 实际选中的渲染器（webgl / webgpu；失败时 renderer 不 ready） */
  backend: string;
  width: number;
  height: number;
}

/** 主进程给渲染进程的初始化参数（来自 `06-window.md` 的建窗结果） */
export interface RendererInit {
  width: number;
  height: number;
  /** 形态字 S：0 = 占满屏态，非 0 = 窗口态 */
  screenMode: number;
  /** 是否全屏呈现（设备参数里的布尔，与 screenMode 不是同一个量） */
  fullscreenPresent: boolean;
  /** 背景色（ARGB 的 0xRRGGBB；窗口背景刷是黑色） */
  background: number;
}

/** 一份纹理槽的可序列化快照（像素不在核心里，见 `60-ops-texture.md`） */
export interface TextureSnapshot {
  id: number;
  kind: 'normal' | 'divided';
  width: number;
  height: number;
  /** 来源文件号（-1 = 无来源；本层没有图像解码，见 `RES-RENDER-TEXT-SURFACE`） */
  file: number;
  /** 1 = 空白创建（`0x1f8`） */
  blank: number;
}

/** 一条绘制项的可序列化快照（`0x1fb` / `0x202` / `0x203`） */
export interface DrawItemSnapshot {
  key: number;
  /** 位 0 = 有效；位 1 = 有颜色渐变窗 */
  flags: number;
  textureId: number;
  left: number;
  top: number;
  right: number;
  bottom: number;
  /** 当前颜色（ARGB；-1 = 无颜色） */
  color: number;
  /** 两个辅助浮点（`0x1fb` 的第 7 / 8 个操作数） */
  aux: [number, number, number];
}

/** 场景快照：主进程在每个渲染节拍推给 renderer（不是规范状态，只是可序列化视图） */
export interface SceneSnapshot {
  /** 客户区尺寸（像素；窗口态 = set:WinX/Y，占满屏 = 屏幕） */
  width: number;
  height: number;
  textures: TextureSnapshot[];
  drawItems: DrawItemSnapshot[];
  /** 场景混合（`0x33f` 的两个格；模式原样，颜色高 8 位 = 透明度） */
  blend: { mode: number; color: number };
  /** 当前渲染目标：纹理槽号，或 -1 = 后缓冲（与核心 `RENDER_TARGET_BACK_BUFFER` 一致） */
  target: number;
}

/** 主进程 → renderer 的一次调用（`id` 由主进程递增；renderer 原样回传） */
export type RenderCall =
  | { id: number; method: 'init'; params: RendererInit }
  | { id: number; method: 'getRenderSurface'; params: { textureId: number } }
  | { id: number; method: 'getBackBuffer'; params: Record<string, never> }
  | { id: number; method: 'setRenderTarget'; params: { surfaceId: number } }
  | { id: number; method: 'releaseSurface'; params: { surfaceId: number } }
  | {
    id: number;
    method: 'drawText';
    params: {
      textureId: number;
      x: number;
      y: number;
      /** 核心的 latin1 字节串；renderer 负责按 cp932 解成 Unicode（见 `RES-RENDER-TEXT-ENCODING`） */
      text: string;
      /** 字体对象当前状态（`0x204` 不是从操作数取字体 / 颜色）；缺省 = 平台用实现侧默认 */
      font: string | null;
      color: number | null;
      /** 字体高度（`0x75`；默认 24，已证） */
      size?: number;
      /** 字重（`0x2BD`；700 = bold，0 = normal） */
      weight?: number;
      /** 副色（`Font+0x554`；默认 0） */
      shadowColor?: number;
      /** 绘制模式（`Font+0x55C`；0/1/2/3，默认 1 = 带 (1,1) 阴影） */
      drawMode?: number;
      /** 纵向对位模式（`Font+0x313D0`；==1 时 y 修正，本层默认 0） */
      alignMode?: number;
    };
  }
  | { id: number; method: 'present'; params: Record<string, never> };

/** renderer → 主进程的结果（`ok: false` 只表示平台实现失败，不是引擎错误） */
export interface RenderResult {
  id: number;
  ok: boolean;
  result?: unknown;
  error?: string;
}

/** 跨线的表面标识：renderer 分配、主进程只转发 */
export interface SurfaceRef {
  surfaceId: number;
  kind: 'texture' | 'backbuffer';
}

/** 主进程推给 renderer 的状态（不需要回执） */
export type PushMessage =
  | { kind: 'scene'; scene: SceneSnapshot }
  | { kind: 'status'; level: 'info' | 'warn' | 'error'; text: string }
  | { kind: 'boot'; info: RendererInit & { title: string; ignored: string[] } };

/** preload 暴露给 renderer 的 API（`window.amayui`） */
export interface AmayuiBridge {
  /** renderer 初始化完成（Pixi ready）后调用一次 */
  ready(info: RendererReady): void;
  /** 主进程发来的调用 */
  onCall(handler: (call: RenderCall) => void): void;
  /** 回复一次调用 */
  reply(result: RenderResult): void;
  /** 主进程推来的状态 */
  onPush(handler: (message: PushMessage) => void): void;
  /** 日志（写到主进程 stdout；⛔ 不往 renderer 控制台写业务结论） */
  log(text: string): void;
}

declare global {
  interface Window {
    amayui: AmayuiBridge;
  }
}
