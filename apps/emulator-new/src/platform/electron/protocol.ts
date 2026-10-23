/**
 * Electron **主进程 ↔ 渲染进程**之间的桥接协议。
 *
 * ★ 架构（2026-10 重构后）：**core 整份跑在 renderer 里**。
 *   - renderer：安装取件（经 `Host` 桥）、配置、启动顺序、VM、场景、Pixi 呈现；
 *   - main：只保留 Electron 窗口壳、文件系统 / 规范表读取、进程生命周期。
 *   因此跨线的不再是"每个绘制调用"，而是"文件 / 窗口请求 + 事件"。
 *
 * ★ 本文件只有类型（`interface` / `type` / `export type`），没有运行期代码；
 *   main / preload / renderer 三边共用同一份契约。平台对象（RenderTexture / BrowserWindow）
 *   一律不跨线：跨线的只有数字句柄与可结构化克隆的纯数据。
 */
import type { ConfigKeySchema, OpcodeRow } from '../../core/spec.ts';
import type { WindowMessage } from '../../core/window.ts';

export type { WindowMessage };

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

/** 启动信息（diagnostics 用；不是规范结论） */
export interface RendererBootInfo extends RendererInit {
  title: string;
  ignored: string[];
}

/** 一份纹理槽的可序列化快照（像素走 `uploadTexture` 请求，不每帧复制进快照） */
export interface TextureSnapshot {
  id: number;
  kind: 'normal' | 'divided';
  width: number;
  height: number;
  /** 来源文件号（-1 = 无来源；解码后的像素由平台持有） */
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
  /** `0x1fb` 的第 3..6 个操作数算出的四条边（角色本版未定；平台按实现使用） */
  left: number;
  top: number;
  right: number;
  bottom: number;
  /** 这一步用于绘制的颜色（派生色，ARGB；-1 = 无颜色） */
  color: number;
  /** 两个辅助浮点（`0x1fb` 的第 7 / 8 个操作数；角色本版未定） */
  aux: [number, number, number];
}

/** 一个网格的可序列化快照（`0x320` / `0x322` / `0x323`；逐顶点布局属外部模块） */
export interface MeshSnapshot {
  key: number;
  flags: number;
  count: number;
  /** 前两个逐顶点缓冲的原样拷贝（LOGO 的 4 顶点全屏四边形里是 x / y 两组） */
  copies: [number[], number[]];
  /** 逐顶点颜色（`0x322` 与计时窗重烘后的值） */
  colors: number[];
  /** 这一步用于绘制的颜色（派生色，ARGB；-1 = 无颜色） */
  color: number;
}

/** 场景快照：renderer 内部的 core → Pixi 视图（不是规范状态） */
export interface SceneSnapshot {
  /** 客户区尺寸（像素；窗口态 = set:WinX/Y，占满屏 = 屏幕） */
  width: number;
  /**
   * 生成这份场景时**当前帧**的诊断视图（名称 / id / slot / ip）。
   * ★ 只给 diagnostics 用，⛔ 不参与绘制；名称来自 `Machine.scriptName`（平台注入）。
   */
  script: { slot: number; id: number | null; name: string | null; ip: number; parent: number } | null;
  height: number;
  textures: TextureSnapshot[];
  drawItems: DrawItemSnapshot[];
  meshes: MeshSnapshot[];
  /** 场景混合（`0x33f` 的两个格；模式原样，颜色高 8 位 = 透明度） */
  blend: { mode: number; color: number };
  /** 当前渲染目标：纹理槽号，或 -1 = 后缓冲 */
  target: number;
}

/** 平台路径 / 环境（renderer 里 `Host` 的同步部分用；异步 IO 走 `invoke`） */
export interface PlatformInfo {
  /** 宿主路径分隔符（POSIX `/`；Windows `\`） */
  sep: string;
  homeDir: string;
  /** 只传核心可能问的变量；缺省 = null（与 `Host.env` 同口径） */
  env: { APPDATA: string | null; XDG_CONFIG_HOME: string | null };
}

/** 一次 `--install` 的启动参数（main 读 spec 后发给 renderer） */
export interface InstallJob {
  mode: 'install';
  installDir: string;
  rootId: number;
  /** 可选的显式 codec key（可复现对照）；缺省由 renderer 的 Machine 运行期生成非 0 值 */
  key?: number;
  /** show-logo 标志初值（0 = 跳过 LOGO；缺省 = 引擎初始化值 1） */
  showLogo?: number;
  configPath: string | null;
  /** `data/spec/format/opcode-table.json` 的行（纯数据） */
  opcodes: OpcodeRow[];
  configKeys: ConfigKeySchema;
  platform: PlatformInfo;
}

/** 一次 `--demo` / `--smoke` 的启动参数（不读游戏安装） */
export interface DemoJob {
  mode: 'demo';
  configKeys: ConfigKeySchema;
  platform: PlatformInfo;
  smoke: boolean;
}

export type StartJob = InstallJob | DemoJob;

/** renderer → main 的事件（`ready` / 日志 / 启动信息 / 完成） */
export type RendererEvent =
  | { kind: 'ready'; info: RendererReady }
  | { kind: 'log'; level: 'info' | 'warn' | 'error'; text: string }
  | { kind: 'boot'; info: RendererBootInfo }
  | { kind: 'status'; level: 'info' | 'warn' | 'error'; text: string }
  | { kind: 'demo-ready' }
  | { kind: 'diagnostics'; data: unknown }
  | { kind: 'done'; ok: boolean; report?: unknown; error?: string };

/** game renderer → main → **debug window** 的只读事件（诊断 / 启动信息 / 日志） */
export type DebugEvent =
  | { kind: 'diagnostics'; data: unknown }
  | { kind: 'boot'; info: RendererBootInfo }
  | { kind: 'status'; level: 'info' | 'warn' | 'error'; text: string }
  | { kind: 'log'; level: 'info' | 'warn' | 'error'; text: string };

/** renderer → main 的同步请求方法（全部由 `Host` / `BootHost` 桥接层调用） */
export type BridgeMethod =
  // 文件系统（Host 的异步部分；join / dirname / env / homeDir 在 renderer 本地）
  | 'host.readBytes'
  | 'host.readBytesMaybe'
  | 'host.readSlice'
  | 'host.readText'
  | 'host.isFile'
  | 'host.ensureReadDir'
  | 'host.writeBytes'
  // 窗口 / 图形启动（main 侧的 BrowserWindow 与显示器信息）
  | 'boot.frameMetrics'
  | 'boot.screenSize'
  | 'boot.registerWindowClass'
  | 'boot.createWindow'
  | 'boot.showWindow'
  | 'boot.updateWindow'
  | 'boot.associateInputContext'
  | 'boot.getMenu'
  | 'boot.operateMenu'
  | 'boot.drawMenuBar'
  | 'boot.isForegroundWindow'
  | 'boot.hideWindow'
  | 'boot.destroyWindow'
  | 'boot.postQuitMessage'
  | 'boot.postWindowMessage'
  | 'boot.setThreadExecutionState'
  | 'boot.allowClose';

/** preload 暴露给 renderer 的 API（`window.amayui`） */
export interface AmayuiBridge {
  /** renderer 初始化完成（Pixi ready）后调用一次 */
  ready(info: RendererReady): void;
  /** main 的启动指令（在 ready 之后到达；handler 必须在调用 ready 前注册） */
  onStart(handler: (job: StartJob) => void): void;
  /** main 转来的窗口消息（close / activate / …） */
  onWindowMessage(handler: (message: WindowMessage) => void): void;
  /** 一次 host / boot 请求（异步；失败 ⇒ reject） */
  invoke(method: BridgeMethod, params: unknown): Promise<unknown>;
  /** 日志 / 状态 / 启动信息 / 完成事件 */
  report(event: RendererEvent): void;
  /** 只在 debug window 里用：接收 game renderer 的只读诊断投影 */
  onDebug(handler: (event: DebugEvent) => void): void;
}

/* ──────────────────────────── 旧的手工 mock 协议（只给 PixiRenderHost.execute） ──────────────────────────── */

/** 主进程 → renderer 的一次渲染调用（现在只用于无 preload 的手工 mock；正式路径在 renderer 内部直调） */
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

/** renderer → 主进程的结果（旧 mock 路径用；正式路径不经过 IPC） */
export interface RenderResult {
  id: number;
  ok: boolean;
  result?: unknown;
  error?: string;
}

/** 跨线的表面标识：renderer 分配、旧 mock 路径只转发 */
export interface SurfaceRef {
  surfaceId: number;
  kind: 'texture' | 'backbuffer';
}

declare global {
  interface Window {
    amayui: AmayuiBridge;
  }
}
