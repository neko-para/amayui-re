/**
 * 启动期的**窗口 / 图形设备 / 事件入口**（`data/spec/chapters/06-window.md` 的参考实现，平台无关核心）。
 *
 * ★ 纯核心：**不 import** `node:*` / `electron` / `pixi.js` / DOM，只 import 核心内的类型。
 *   平台侧要做的事全在 `BootHost`（`WindowHost & MessageHost & GraphicsHost`）里：
 *   核心只**发请求、收句柄 / 结果**；请求参数是**领域字段**（`clientWidth` / `style` / `menuResourceId`…），
 *   ⛔ 核心不认识任何 Win32 结构体 / 消息号 / D3D 结构体。
 * ★ 同步 / 异步的分界：`WindowHost` / `MessageHost` 是**同步**（建窗、收发消息、定时器都是即时的平台调用）；
 *   `GraphicsHost` 全是 `Promise` —— 设备创建要等平台的图形后端真的就绪（Electron 侧要等 renderer 的
 *   Pixi / WebGL 初始化）才知道成功与否；同步返回假句柄会让 `CORE-BOOT-GRAPHICS-INIT` 的
 *   "失败 ⇒ 写错误通道 + 结束启动（不建主循环 / 不装载根脚本）"退化成事后补救。
 *   ⇒ `bootWindow` / `runStartup` / 图形阶段是 `async`，按 spec 次序 `await` 每一步。
 *
 * ★ 规则对应（五条，逐条可追）：
 *   1. `CORE-BOOT-WINDOW-CLASS`   —— `registerWindowClass`（类名 / 实例 / 图标 / 光标 / 背景刷 / 菜单名 / 风格）
 *   2. `CORE-BOOT-WINDOW-CREATE`  —— 形态字 S（`display:ScreenMode` + `display:ForceScreen`）→ 两种形态 → 显示 / 重绘
 *   3. `CORE-BOOT-WINDOW-ATTACH`  —— 解绑输入法 → 记录服务窗口槽 → 仅窗口态安装菜单
 *   4. `CORE-BOOT-GRAPHICS-INIT`  —— `set:CreateObject` 最低位选路线；失败 ⇒ 写错误通道 + 结束启动（⛔ 不静默）
 *   5. `CORE-BOOT-EVENT-LOOP`     —— 周期定时器 / 泵（窥视 → 取一条 → 翻译 → 派发）/ 四类消息 / 悬停定时器
 *
 * ★ 顺序（`06-window.md` 开头）：注册类 → 建窗 → 显示 / 重绘 → 解绑输入法 → 装菜单 → 图形设备 →
 *   （根脚本在图形设备与周期定时器之间，不属本文件）→ 周期定时器 → 消息泵。
 *   ⇒ `bootWindow` 在**图形设备之后、根脚本之前**停下（否则会把根脚本夹到错误的位置）；
 *   调用方在根脚本装载完毕后再调 `session.beginMainLoop()`。
 *
 * ★ 失败 = **共享出口**：返回的 `WindowSession` 带 `failure`，没有窗口 / 没有图形对象；
 *   ⛔ 不显示任何消息（本接口**没有**弹窗类请求 —— 结构上就不可能弹），图形路线的失败写进错误通道
 *   （`session.errors` + `failure.errorText`），⛔ 不许静默。
 *
 * ★ 忽略必须**显式**：`session.ignored` 逐条记 `{ disposition: 'ignored', rule, what, reason, residual }`。
 *   这些是"06-window 明确不规定 / 本层不处理"的项（含用户允许忽略的菜单内容 / IME / 图标细节 / 风格位含义）。
 *   ⛔ 不许把**本条规则要求的请求**塞进 ignored 来逃掉；测试守着这件事。
 *
 * ★ 退出异常：泵里 `GetMessage` 报告退出消息（返回 0）⇒ 抛 `EngineExit`（已证的唯一结束路径；
 *   其它返回路径见残差 `RES-BOOT-LOOP-RETURN`，本文件不实现）。
 */

import type { ConfigStore } from './config.ts';

/* ─────────────────────────── 领域常量（引擎内建，⛔ 不来自配置 / 脚本） ─────────────────────────── */

/** 规则 id（`06-window.md` 的 `<!-- rule: … -->` 标记） */
export const RULE_WINDOW_CLASS = 'CORE-BOOT-WINDOW-CLASS';
export const RULE_WINDOW_CREATE = 'CORE-BOOT-WINDOW-CREATE';
export const RULE_WINDOW_ATTACH = 'CORE-BOOT-WINDOW-ATTACH';
export const RULE_GRAPHICS_INIT = 'CORE-BOOT-GRAPHICS-INIT';
export const RULE_EVENT_LOOP = 'CORE-BOOT-EVENT-LOOP';

/** 五条规则的全集（ignored 的 `rule` 只能落在这五个里） */
export const WINDOW_RULES = [
  RULE_WINDOW_CLASS,
  RULE_WINDOW_CREATE,
  RULE_WINDOW_ATTACH,
  RULE_GRAPHICS_INIT,
  RULE_EVENT_LOOP,
] as const;

/** 窗口类名（同时是建窗时用的类名；⛔ 与窗口标题不是同一个东西） */
export const WINDOW_CLASS_NAME = 'ARCGameEngine';
/** 类风格（`0x40`；位含义属平台侧，本规范不解释） */
export const WINDOW_CLASS_STYLE = 0x40;
/** 图标资源名（从**辅助宿主模块**给出的资源实例取；大 / 小图标同一份） */
export const WINDOW_ICON_RESOURCE_NAME = 'IDI_ICON1';
/** 光标：系统预定义标准箭头（引擎传 `0x7F00`） */
export const WINDOW_CURSOR_RESOURCE_ID = 0x7f00;
/** 背景刷：系统预定义黑色实心刷（引擎传 `4`） */
export const WINDOW_BACKGROUND_BRUSH = 4;
/** 菜单名：空（`0`） */
export const WINDOW_MENU_NAME = 0;
/** 类附加 / 窗口附加：`0` / `0` */
export const WINDOW_CLASS_EXTRA = 0;
export const WINDOW_WINDOW_EXTRA = 0;

/** 窗口态风格字（固定值） */
export const WINDOWED_STYLE = 0x90cb0000;
/** 窗口态扩展风格字：恒 `0` */
export const WINDOWED_EX_STYLE = 0;
/** 窗口态菜单资源号（从辅助宿主模块实例装载） */
export const WINDOW_MENU_RESOURCE_ID = 110;

/** 占满屏态风格字（固定值） */
export const FULLSCREEN_STYLE = 0x90000000;
/** 占满屏态扩展风格字（固定非 0 值；由形态决定，⛔ 不是逐位拼出来的） */
export const FULLSCREEN_EX_STYLE = 0x8;

/**
 * "使用平台默认位置"哨兵（`set:SaveWinPos == 0` 时位置字段用它）。
 * ★ 按无符号看是 `0x80000000`；按有符号 32 位看是 `−2147483648`。核心原样传，由宿主解释。
 */
export const DEFAULT_WINDOW_POSITION = 0x80000000;

/** 窗口标题缓冲上限（`0x100` 字节；建窗当时的内容本版不规定，见 ignored） */
export const WINDOW_TITLE_MAX_BYTES = 0x100;

/** 周期定时器：标识 2、周期 10000 毫秒、无回调（引擎常量，⛔ 不是仿真参数） */
export const PERIODIC_TIMER_ID = 2;
export const PERIODIC_TIMER_PERIOD_MS = 10000;
/** 鼠标悬停定时器：标识 1、周期 100 毫秒 */
export const HOVER_TIMER_ID = 1;
export const HOVER_TIMER_PERIOD_MS = 100;
/** "禁止系统空闲"的刷新值（`SetThreadExecutionState(3)`） */
export const EXECUTION_STATE_KEEP_AWAKE = 3;
/** 三维设备路线：创建 Direct3D 9 对象时请求的 SDK 版本号 */
export const D3D9_SDK_VERSION = 32;
/** 适配器显示模式：查询适配器 0 */
export const ADAPTER_INDEX = 0;
/** 辅助宿主模块「操作菜单」入口的安装类别的号（启动期只观察到 0 / 1 / 2；本版只按 spec 传 1） */
export const MENU_CATEGORY_INSTALL = 1;

/**
 * 图形设备初始化失败时写进错误通道的**内建文本**。
 * ★ 规范只要求"把一条内建错误文本写进错误通道"，文本的**类别与内容**未取证（`RES-BOOT-GFX-TEXT`）
 *   ⇒ 本实现用一个实现侧常量占位；内容不是规范结论。
 */
export const GRAPHICS_INIT_ERROR_TEXT = 'graphics-init-failed';

/* ─────────────────────────── 平台句柄（对核心不透明） ─────────────────────────── */

/**
 * 平台句柄：对核心**不透明**的数字。核心只把它从 `entry` 收进来、再原样发给平台，
 * ⛔ 不比较、不运算、不解释（唯一例外是"是不是 null"）。
 */
export type PlatformHandle = number;
export type WindowClassHandle = PlatformHandle;
export type WindowHandle = PlatformHandle;
export type D3d9Handle = PlatformHandle;
export type DeviceHandle = PlatformHandle;
export type DrawObjectHandle = PlatformHandle;

/* ─────────────────────────── 平台报告的量 ─────────────────────────── */

/**
 * 平台报告的窗口外框**四类单边厚度**（引擎只用这四个量，各查询一次）。
 *
 * ★ 引擎**纯手算**窗口尺寸（`06-window.md`「窗口创建」；EA `0x4BA4DC`–`0x4BA517`）：
 *   - 宽 = 客户区宽 + **2 ×** 边框宽（`GetSystemMetrics(7)` 只查一次，`lea ebx,[edx+eax*2]`）；
 *   - 高 = 客户区高 + **2 ×** 边框高 + 标题高 + 菜单高（`GetSystemMetrics(8)` 一次、`add edi,edi`；
 *     菜单高 `0x0F`、标题高 `4` 只进高）。
 *   ⛔ **没有 `AdjustWindowRect`**：平台只报告单边厚度，宽高由引擎加出来；左右 / 上下**对称**
 *   是引擎的假设（它没有查左右各自的厚度）。
 */
export interface FrameMetrics {
  /** 边框宽（左右各一份） */
  frameWidth: number;
  /** 边框高（上下各一份） */
  frameHeight: number;
  /** 标题高（只进高） */
  captionHeight: number;
  /** 菜单高（只进高；只有窗口态才有菜单） */
  menuHeight: number;
}

/** 平台报告的屏幕尺寸（占满屏态用） */
export interface ScreenSize {
  width: number;
  height: number;
}

/** 适配器显示模式（三维设备路线第 2 步） */
export interface DisplayMode {
  width: number;
  height: number;
  /** 像素格式标识（位含义属平台侧） */
  format: number;
}

/* ─────────────────────────── 窗口 / 消息 / 图形 的平台接口 ─────────────────────────── */

/** 注册窗口类的参数（领域字段；⛔ 不是 `WNDCLASSEX`） */
export interface WindowClassParams {
  /** 固定类名 `ARCGameEngine`；建窗时用同一个字符串 */
  className: string;
  /** 类风格 `0x40`（位含义属平台侧） */
  classStyle: number;
  /** 类的实例 = **入口参数给的进程实例** */
  instance: PlatformHandle;
  /** 图标资源名 `IDI_ICON1`（大 / 小图标同一份） */
  iconResourceName: string;
  /** 图标 / 菜单所在的**资源实例**（来自辅助宿主模块；⛔ 与进程实例不是同一个句柄来源） */
  resourceInstance: PlatformHandle;
  /** 系统预定义光标 `0x7F00` */
  cursorResourceId: number;
  /** 系统预定义背景刷 `4` */
  backgroundBrush: number;
  /** 菜单名 `0` */
  menuName: number;
  /** 类附加 `0` */
  classExtra: number;
  /** 窗口附加 `0` */
  windowExtra: number;
}

/** 建窗参数（领域字段；⛔ 不是 `CreateWindowExA` 的实参栈） */
export interface WindowCreateParams {
  className: string;
  /** 窗口标题（建窗当时标题缓冲的内容本版不规定 ⇒ 传空串，见 ignored） */
  title: string;
  /** 实例 = 入口给的进程实例（与图标 / 菜单的资源实例不同） */
  instance: PlatformHandle;
  /** 父窗口：空 */
  parent: null;
  /** 附加参数：空 */
  param: null;
  /** x / y（窗口态可来自配置；否则是 `0x80000000` 哨兵或 `(0,0)`） */
  x: number;
  y: number;
  /** **窗口尺寸** = 客户区 + 外框（窗口态由引擎算好）；占满屏态 = 屏幕尺寸 */
  width: number;
  height: number;
  /** 客户区尺寸（信息字段：窗口态来自 `set:WinX` / `set:WinY`，占满屏态 = 屏幕尺寸） */
  clientWidth: number;
  clientHeight: number;
  /** 风格字（窗口态 `0x90CB0000`；占满屏态 `0x90000000`） */
  style: number;
  /** 扩展风格字（窗口态 `0`；占满屏态 `0x8`） */
  exStyle: number;
  /** 窗口态 = 菜单资源号 `110`；占满屏态 = `null`（无菜单） */
  menuResourceId: number | null;
}

/** 「操作菜单」辅助入口的启动期实参（⛔ 菜单内容由辅助模块生成，核心只转发） */
export interface OperateMenuParams {
  /** 类别号（启动期 = 1；其它类别号的语义未取证） */
  category: number;
  window: WindowHandle;
  /** 该窗口的菜单句柄（来自 `getMenu`；可能为空） */
  menu: PlatformHandle | null;
  /** 第 4 实参（启动期观察到 0） */
  extra: number;
}

/** 发给自己的一条消息（本层只用到应用激活） */
export type PostableWindowMessage = 'activate-app';

/**
 * 窗口类 / 窗口 / 显示 / 菜单 / 生命周期请求。
 * ★ 每个方法都是一次**平台请求**（核心只发请求、收句柄 / 结果）。
 */
export interface WindowHost {
  /** 查询窗口外框四类尺寸（窗口态建窗前显式调用） */
  frameMetrics(): FrameMetrics;
  /** 查询屏幕宽 / 高（占满屏态建窗前显式调用） */
  screenSize(): ScreenSize;
  /** 注册窗口类；`null` = 注册失败（引擎看到的返回 0） */
  registerWindowClass(params: WindowClassParams): WindowClassHandle | null;
  /** 建顶层窗口；`null` = 建窗失败（句柄为空） */
  createWindow(params: WindowCreateParams): WindowHandle | null;
  /** `ShowWindow(window, showMode)`：显示方式**原样转发**入口参数，返回值不检查 */
  showWindow(window: WindowHandle, showMode: number): void;
  /** `UpdateWindow(window)`：返回值不检查 */
  updateWindow(window: WindowHandle): void;
  /** `ImmAssociateContext(window, context)`：`context = null` 就是引擎传的 0（显式解绑） */
  associateInputContext(window: WindowHandle, context: PlatformHandle | null): void;
  /**
   * `GetMenu(window)`：取该窗口的菜单句柄（一次**显式**请求；窗口态装菜单前才调）。
   * ★ 引擎不检查空句柄：拿不到就原样把空值转给辅助入口（平台行为，不是引擎分支）。
   */
  getMenu(window: WindowHandle): PlatformHandle | null;
  /**
   * 辅助宿主模块的「操作菜单」入口（`dword_55E1B8`）的启动期调用：
   * `(类别号 1, hWnd, hMenu, 0)` —— 四个实参都是引擎原样转发的值。
   */
  operateMenu(params: OperateMenuParams): void;
  /** `DrawMenuBar(window)` */
  drawMenuBar(window: WindowHandle): void;
  /** 窗口是不是前台窗口（周期定时器到期判据的第三项） */
  isForegroundWindow(window: WindowHandle): boolean;
  /** 隐藏窗口（收到"已销毁"消息时的第一步） */
  hideWindow(window: WindowHandle): void;
  /** 销毁窗口（收到"关闭"消息时的最后一步） */
  destroyWindow(window: WindowHandle): void;
  /** `PostQuitMessage(exitCode)`：正常退出的唯一触发方式 */
  postQuitMessage(exitCode: number): void;
  /** 向窗口自己投递一条消息（周期定时器到期时可能补发应用激活） */
  postWindowMessage(window: WindowHandle, message: PostableWindowMessage): void;
}

/** 消息泵 + 定时器 + 空闲抑制（平台接口） */
export interface MessageHost {
  /** `SetThreadExecutionState(flags)` */
  setThreadExecutionState(flags: number): void;
  /** `SetTimer(window, id, periodMs, callback)`；`callback: null` = 无回调函数（引擎这样建定时器） */
  setTimer(window: WindowHandle, params: { id: number; periodMs: number; callback: null }): void;
  /** `KillTimer(window, id)` */
  killTimer(window: WindowHandle, id: number): void;
  /** `PeekMessage(…, remove = false)`：**不移除**，只判"有没有" */
  peekMessage(params: { remove: false }): boolean;
  /** `GetMessage(…)` 取**一条**；`null` = 返回 0（取到退出消息，引擎级退出） */
  getMessage(): WindowMessage | null;
  /** `TranslateMessage(message)` */
  translateMessage(message: WindowMessage): void;
}

/** 旧式绘制路线的参数（"参数含窗口、客户区尺寸、行为位"） */
export interface LegacyDrawParams {
  window: WindowHandle;
  clientWidth: number;
  clientHeight: number;
  /** 行为位：来源未取证（见 ignored `legacy-behavior-bits`） */
  behaviorBits: number;
}

/** 三维设备路线的设备参数（领域字段；键 → 参数映射未取证，见 ignored `device-param-mapping`） */
export interface DeviceParams {
  d3d: D3d9Handle;
  window: WindowHandle;
  /** 屏幕形态字 S */
  screenMode: number;
  /** 适配器显示模式报告的像素格式 */
  adapterFormat: number;
  /** 全屏宽 / 高：配置值；为 0 ⇒ 取适配器显示模式（桌面）尺寸 */
  fullscreenWidth: number;
  fullscreenHeight: number;
  deviceType: 'hal' | 'reference' | 'software';
  vertexProcessing: 'software' | 'hardware' | 'mixed';
  presentationInterval: number;
  multisample: { type: number; quality: number };
  /** 纹理位深（取自 `display:FullScreenBit`；映射未取证） */
  textureBitDepth: number;
  pixelShaderVersion: number;
  /** 是否全屏呈现（与"窗口态 / 占满屏态"**不是**同一个量；组合表未取证） */
  fullscreenPresent: boolean;
}

/** 上层绘制对象的参数（三维设备路线第 4 步） */
export interface DrawObjectParams {
  device: DeviceHandle;
  /** 纹理宽 / 高（取自 `set:TexWidth` / `set:TexHeight`） */
  textureWidth: number;
  textureHeight: number;
  /** 像素着色器配置（来源未取证 ⇒ 由平台查询设备） */
  pixelShader: number;
}

/**
 * 图形设备（两条互斥路线）。
 * ★ 全部 `Promise`：每一步都要等平台给出**真实结果**（`null` = 失败）；⛔ 不许先返一个假句柄再补错误。
 */
export interface GraphicsHost {
  /** 旧式绘制路线；`null` = 失败 */
  createLegacyDraw(params: LegacyDrawParams): Promise<DrawObjectHandle | null>;
  /** 三维路线 1：创建 Direct3D 9 对象（SDK 版本号 32）；`null` = 失败 */
  createD3d9(params: { sdkVersion: number }): Promise<D3d9Handle | null>;
  /** 三维路线 2：查询适配器显示模式（适配器 0）；`null` = 失败 */
  queryAdapterDisplayMode(params: { d3d: D3d9Handle; adapter: number }): Promise<DisplayMode | null>;
  /** 三维路线 3：创建设备；`null` = 失败 */
  createDevice(params: DeviceParams): Promise<DeviceHandle | null>;
  /** 三维路线 4a：读设备的像素着色器配置（不能失败的查询） */
  readPixelShaderConfig(params: { device: DeviceHandle }): Promise<number>;
  /** 三维路线 4b：建立上层绘制对象；`null` = 失败 */
  createDrawObject(params: DrawObjectParams): Promise<DrawObjectHandle | null>;
}

/** 组合接口：`bootWindow` 需要的全部平台能力 */
export type BootHost = WindowHost & MessageHost & GraphicsHost;

/* ─────────────────────────── 入口参数 ─────────────────────────── */

/** 入口参数（`_WinMain@16` 拿到的三个量） */
export interface WindowEntry {
  /** 进程实例：窗口类的实例、建窗的实例参数都来自它 */
  instance: PlatformHandle;
  /** 辅助宿主模块给出的**资源实例**（图标 / 菜单资源所在；⛔ 与进程实例不同来源） */
  resourceInstance: PlatformHandle;
  /** 入口给的显示方式（`ShowWindow` **原样转发**，引擎不覆盖） */
  showMode: number;
}

/**
 * 未取证的图形参数映射（`RES-BOOT-GFX-PARAMS` / `RES-BOOT-GFX-FULLSCREEN-COMBO`）。
 * ★ 规范只到"参数来自哪些键"这一层；这些字段由调用方注入，缺省值是本实现的选择（见 `session.ignored`）。
 */
export interface GraphicsParamChoices {
  deviceType: 'hal' | 'reference' | 'software';
  vertexProcessing: 'software' | 'hardware' | 'mixed';
  presentationInterval: number;
  multisample: { type: number; quality: number };
  pixelShaderVersion: number;
  /** 旧式绘制路线的"行为位" */
  legacyBehaviorBits: number;
  /** 覆盖"是否全屏呈现"（缺省 = 屏幕形态字为 0） */
  fullscreenPresent: boolean;
}

export interface BootWindowOptions {
  host: BootHost;
  /** 已建立的配置库（发行默认表 + 玩家文件；`55-config.md`） */
  config: ConfigStore;
  entry: WindowEntry;
  /** 未取证的图形参数（缺省值见 `GraphicsParamChoices`） */
  graphics?: Partial<GraphicsParamChoices>;
  /**
   * 周期定时器到期时两个引擎格（"状态选择字里的一个位在效" + "另一个引擎级格为 0"）的**判据**。
   * ★ 两者的身份 / 位号未取证（`RES-BOOT-TIMER-CONDITION`）⇒ 核心不读引擎格，由引擎侧注入合取结果。
   * 未注入 ⇒ 不补发应用激活（`session.ignored` 里有一条显式记账）。
   */
  timerActivationCondition?: () => boolean;
  /**
   * "关闭"消息里引擎自己的收尾（保存 / 释放）。
   * ★ 收尾项未逐项取证（`RES-BOOT-CLOSE-CLEANUP`）⇒ 内容由引擎侧注入；
   *   本层只保证它在**杀周期定时器之前**、`destroyWindow` 之前被调用。
   */
  engineTeardown?: () => void;
}

/* ─────────────────────────── 请求留痕 / 忽略标记 / 失败 ─────────────────────────── */

/** 一次平台请求（`via: 'platform'`）或一次引擎内部转发（`via: 'engine'`） */
export interface TraceRecord {
  /** `06-window.md` 的规则 id */
  rule: string;
  /** 领域请求名（稳定字符串；测试按它断言顺序） */
  request: string;
  via: 'platform' | 'engine';
  /** 领域字段（⛔ 不含任何 Win32 / D3D 结构体） */
  args: Record<string, unknown>;
}

/**
 * **显式忽略**：规范不规定、或本层不处理的请求 / 职责。
 * ★ 忽略必须留痕（`disposition: 'ignored'` + 规则 id + 原因），⛔ 不许静默跳过。
 */
export interface IgnoredRequest {
  disposition: 'ignored';
  rule: string;
  /** 被忽略的请求 / 职责（领域名） */
  what: string;
  /** 为什么（⛔ 不许空） */
  reason: string;
  /** 规范残差 id（`06-window.md` 的 `RES-…`）；没有则缺省 */
  residual?: string;
}

/** 图形路线（由 `set:CreateObject` 最低位选） */
export type GraphicsRoute = 'legacy-draw' | 'three-dimensional';

/** 图形路线里失败的那一步 */
export type GraphicsStep =
  | 'create-legacy-draw'
  | 'create-d3d9'
  | 'query-adapter-display-mode'
  | 'create-device'
  | 'create-draw-object';

/**
 * **共享出口**：启动在这里结束。
 * ⛔ 不显示任何消息（本接口没有弹窗请求）；⛔ 不继续后续阶段（窗口 / 图形对象 / 主循环都为空）。
 */
export interface BootFailure {
  rule: typeof RULE_WINDOW_CLASS | typeof RULE_WINDOW_CREATE | typeof RULE_GRAPHICS_INIT;
  reason: 'class-register-failed' | 'window-create-failed' | 'graphics-init-failed';
  /** 图形路线里失败的步骤；其它失败为 `null` */
  step: GraphicsStep | null;
  /** 写进错误通道的文本（只有图形路线有；其它失败为 `null` —— 共享出口不产生用户可见输出） */
  errorText: string | null;
  sharedExit: true;
}

/* ─────────────────────────── 消息（窗口过程收到的类别） ─────────────────────────── */

/**
 * 窗口过程收到的消息（**领域类别**，⛔ 不是 Win32 消息号）。
 * ★ 只覆盖本层要处理的生命周期 / 定时器 / 悬停几类；其余类别见 `session.ignored`（`RES-BOOT-WNDPROC-CLOSURE`）。
 */
export type WindowMessage =
  | { kind: 'close' }
  | { kind: 'destroy' }
  | { kind: 'timer'; timerId: number }
  | { kind: 'mousemove'; inClient: boolean }
  | { kind: 'mouseleave' }
  | { kind: 'mousedown' }
  | { kind: 'activate'; active: boolean };

/* ─────────────────────────── 退出异常 ─────────────────────────── */

/**
 * 引擎级退出异常：泵里 `GetMessage` 返回 0（取到退出消息）时**在这一条路径上**抛出，
 * 主循环因此不以返回值结束。入口处兜住它，随后进入退出清理。
 * ★ 这是**已证**的唯一结束路径；其它返回路径见残差 `RES-BOOT-LOOP-RETURN`（本层不实现）。
 */
export class EngineExit extends Error {
  constructor(message = 'engine-exit: 取到退出消息（GetMessage 返回 0）') {
    super(message);
    this.name = 'EngineExit';
  }
}

/* ─────────────────────────── 会话 ─────────────────────────── */

/**
 * 一次启动的**窗口 / 图形 / 事件会话**：持有平台句柄、请求留痕与忽略清单。
 * 平台把窗口过程的消息转给 `handleMessage`；主循环用 `beginMainLoop` + `pump`。
 */
export class WindowSession {
  readonly host: BootHost;
  readonly config: ConfigStore;
  readonly entry: WindowEntry;
  /** 全部请求 / 引擎内部转发的**有序**留痕（含图形路线的每一步） */
  readonly trace: TraceRecord[] = [];
  /** 显式忽略清单（见 `IgnoredRequest`） */
  readonly ignored: IgnoredRequest[] = [];
  /** 错误通道（只有图形路线失败会写文本；⛔ 失败不许静默） */
  readonly errors: string[] = [];

  /** 窗口类（注册失败时为 `null`） */
  windowClass: WindowClassHandle | null = null;
  /** 引擎的窗口（建窗失败时为 `null`） */
  window: WindowHandle | null = null;
  /** **服务窗口槽**（建窗成功后写；此后一切"服务窗口"引用都用它） */
  serviceWindow: WindowHandle | null = null;
  /** 图形设备（旧式路线没有设备对象 ⇒ `null`；三维路线成功时有值） */
  device: DeviceHandle | null = null;
  /** 上层绘制对象（两条路线成功时都有值） */
  drawObject: DrawObjectHandle | null = null;
  /** 图形路线（`set:CreateObject` 最低位；未到该阶段为 `null`） */
  graphicsRoute: GraphicsRoute | null = null;
  /** 形态字 S（`display:ScreenMode` 经 `display:ForceScreen` 归一后的值） */
  screenMode = 0;
  /** 共享出口（`null` = 启动继续到了"图形设备就绪"） */
  failure: BootFailure | null = null;

  private periodicTimerActive = false;
  private hoverTimerActive = false;
  /** 占满屏态量到的屏幕尺寸（旧式绘制路线要用客户区尺寸，不再问一次） */
  private screenWidth = 0;
  private screenHeight = 0;
  private readonly graphics: {
    deviceType: 'hal' | 'reference' | 'software';
    vertexProcessing: 'software' | 'hardware' | 'mixed';
    presentationInterval: number;
    multisample: { type: number; quality: number };
    pixelShaderVersion: number;
    legacyBehaviorBits: number;
    fullscreenPresent: boolean | null;
  };
  private readonly timerActivationCondition: (() => boolean) | null;
  private readonly engineTeardown: (() => void) | null;

  constructor(opts: BootWindowOptions) {
    this.host = opts.host;
    this.config = opts.config;
    this.entry = opts.entry;
    this.timerActivationCondition = opts.timerActivationCondition ?? null;
    this.engineTeardown = opts.engineTeardown ?? null;
    const g = opts.graphics ?? {};
    this.graphics = {
      deviceType: g.deviceType ?? 'hal',
      vertexProcessing: g.vertexProcessing ?? 'software',
      presentationInterval: g.presentationInterval ?? 0,
      multisample: g.multisample ?? { type: 0, quality: 0 },
      pixelShaderVersion: g.pixelShaderVersion ?? 0,
      legacyBehaviorBits: g.legacyBehaviorBits ?? 0,
      fullscreenPresent: g.fullscreenPresent ?? null,
    };
  }

  /** 周期定时器是否在效（标识 2） */
  get hasPeriodicTimer(): boolean {
    return this.periodicTimerActive;
  }

  /** 鼠标悬停定时器是否在效（标识 1） */
  get hasHoverTimer(): boolean {
    return this.hoverTimerActive;
  }

  /* ── 启动 ───────────────────────────────────────────────────────────────── */

  /**
   * 按 `06-window.md` 的顺序执行到"图形设备就绪"（含启动期那次直接的空闲抑制请求）。
   * ★ 失败 ⇒ 停在共享出口（`failure` 非空、窗口 / 图形对象为空、之后的请求一条都不发）。
   * ★ `async`：图形设备的每次请求都要等平台**真的**给出结果（Electron 侧的设备创建要等
   *   renderer 的图形后端初始化完成）；同步窗口请求仍在本函数里按顺序发。
   */
  async runStartup(): Promise<void> {
    this.declareIgnored();
    if (!this.registerWindowClass()) return;
    if (!this.createWindow()) return;
    this.attachWindow();
    if (!(await this.initGraphics())) return;
    // 事件入口规则：启动期在**设备初始化之后、根脚本之前**直接请求过一次空闲抑制。
    this.record(RULE_EVENT_LOOP, 'set-thread-execution-state', {
      flags: EXECUTION_STATE_KEEP_AWAKE,
      reason: 'startup-after-graphics',
    });
    this.host.setThreadExecutionState(EXECUTION_STATE_KEEP_AWAKE);
  }

  /* ── 1. 窗口类注册（CORE-BOOT-WINDOW-CLASS） ─────────────────────────────── */

  private registerWindowClass(): boolean {
    this.record(RULE_WINDOW_CLASS, 'register-window-class', {
      className: WINDOW_CLASS_NAME,
      classStyle: WINDOW_CLASS_STYLE,
      instance: this.entry.instance,
      iconResourceName: WINDOW_ICON_RESOURCE_NAME,
      resourceInstance: this.entry.resourceInstance,
      cursorResourceId: WINDOW_CURSOR_RESOURCE_ID,
      backgroundBrush: WINDOW_BACKGROUND_BRUSH,
      menuName: WINDOW_MENU_NAME,
      classExtra: WINDOW_CLASS_EXTRA,
      windowExtra: WINDOW_WINDOW_EXTRA,
    });
    const cls = this.host.registerWindowClass({
      className: WINDOW_CLASS_NAME,
      classStyle: WINDOW_CLASS_STYLE,
      instance: this.entry.instance,
      iconResourceName: WINDOW_ICON_RESOURCE_NAME,
      resourceInstance: this.entry.resourceInstance,
      cursorResourceId: WINDOW_CURSOR_RESOURCE_ID,
      backgroundBrush: WINDOW_BACKGROUND_BRUSH,
      menuName: WINDOW_MENU_NAME,
      classExtra: WINDOW_CLASS_EXTRA,
      windowExtra: WINDOW_WINDOW_EXTRA,
    });
    if (cls === null) {
      // 共享出口：不显示消息、不建窗口、不装载后续
      this.fail(RULE_WINDOW_CLASS, 'class-register-failed', null, null);
      return false;
    }
    this.windowClass = cls;
    return true;
  }

  /* ── 2. 窗口创建（CORE-BOOT-WINDOW-CREATE） ──────────────────────────────── */

  private createWindow(): boolean {
    // 形态判据：S = display:ScreenMode（缺键 ⇒ getInt 的 0）；ForceScreen 恰为 1 ⇒ S := 1 且先写回配置。
    let screenMode = this.config.getInt('display:ScreenMode');
    if (this.config.getInt('display:ForceScreen') === 1) {
      screenMode = 1;
      this.record(RULE_WINDOW_CREATE, 'write-config', { key: 'display:ScreenMode', value: 1 });
      this.config.setInt('display:ScreenMode', 1);
    }
    this.screenMode = screenMode;

    let params: WindowCreateParams;
    if (screenMode !== 0) {
      params = this.windowedCreateParams();
    } else {
      params = this.fullscreenCreateParams();
    }

    this.record(RULE_WINDOW_CREATE, 'create-window', { ...params });
    const window = this.host.createWindow(params);
    if (window === null) {
      // 共享出口：先释放引擎对象由调用方负责；本层不显示消息、不继续
      this.fail(RULE_WINDOW_CREATE, 'window-create-failed', null, null);
      return false;
    }
    this.window = window;

    // 建窗成功后**无条件**显示 + 重绘；两件请求的返回值都不检查（ShowWindow 的显示方式原样转发）。
    this.record(RULE_WINDOW_CREATE, 'show-window', { showMode: this.entry.showMode });
    this.host.showWindow(window, this.entry.showMode);
    this.record(RULE_WINDOW_CREATE, 'update-window', {});
    this.host.updateWindow(window);
    return true;
  }

  /**
   * 窗口态：客户区来自 `set:WinX` / `set:WinY`；窗口尺寸 = 客户区 + 平台报告的四类单边厚度。
   * ★ 这两个键在**索引装载时**就被引擎抄进缓存，建窗读的是缓存：**不判 0、没有兜底**
   *   ⇒ 值为 0 就原样建 0 客户区的窗口（由宿主决定怎么处理），⛔ 核心不编默认分辨率。
   */
  private windowedCreateParams(): WindowCreateParams {
    const clientWidth = this.config.getInt('set:WinX');
    const clientHeight = this.config.getInt('set:WinY');
    this.record(RULE_WINDOW_CREATE, 'measure-window-frame', {});
    const frame = this.host.frameMetrics();
    // ★ 引擎**纯手算**（⛔ 没有 AdjustWindowRect）：四类量都是"单边厚度"，
    //   宽 = 客户区宽 + 2 × 边框宽；高 = 客户区高 + 2 × 边框高 + 标题高 + 菜单高。
    const width = clientWidth + 2 * frame.frameWidth;
    const height = clientHeight + 2 * frame.frameHeight + frame.captionHeight + frame.menuHeight;
    // 位置：set:SaveWinPos 非 0 ⇒ (set:WinPosLeft, set:WinPosTop)；0 ⇒ 平台默认位置哨兵。
    // ★ 两个坐标键是**整数运行期命名键**，缺键读 0；核心照传，⛔ 不替引擎编分辨率 / 位置。
    const savePosition = this.config.getInt('set:SaveWinPos');
    const x = savePosition !== 0 ? this.config.getInt('set:WinPosLeft') : DEFAULT_WINDOW_POSITION;
    const y = savePosition !== 0 ? this.config.getInt('set:WinPosTop') : DEFAULT_WINDOW_POSITION;
    return {
      className: WINDOW_CLASS_NAME,
      title: '',
      instance: this.entry.instance,
      parent: null,
      param: null,
      x,
      y,
      width,
      height,
      clientWidth,
      clientHeight,
      style: WINDOWED_STYLE,
      exStyle: WINDOWED_EX_STYLE,
      menuResourceId: WINDOW_MENU_RESOURCE_ID,
    };
  }

  /** 占满屏态：屏幕尺寸、`(0,0)`、另一套风格字、无菜单 */
  private fullscreenCreateParams(): WindowCreateParams {
    this.record(RULE_WINDOW_CREATE, 'measure-screen', {});
    const screen = this.host.screenSize();
    this.screenWidth = screen.width;
    this.screenHeight = screen.height;
    return {
      className: WINDOW_CLASS_NAME,
      title: '',
      instance: this.entry.instance,
      parent: null,
      param: null,
      x: 0,
      y: 0,
      width: screen.width,
      height: screen.height,
      clientWidth: screen.width,
      clientHeight: screen.height,
      style: FULLSCREEN_STYLE,
      exStyle: FULLSCREEN_EX_STYLE,
      menuResourceId: null,
    };
  }

  /* ── 3. 窗口附加（CORE-BOOT-WINDOW-ATTACH） ──────────────────────────────── */

  /**
   * 建窗成功后的附加请求（次序照 `0x4BB0C2`–`0x4BB10A`）：
   * 解绑输入法 →（**仅窗口态**）`GetMenu` → 辅助入口「操作菜单」(类别 1, hWnd, hMenu, 0)
   * → `DrawMenuBar` → **最后**写服务窗口槽。
   * ★ 启动期**没有 `SetMenu`**：菜单是建窗时 `hMenu` 实参装上去的（`menuResourceId` 已表达）。
   */
  private attachWindow(): void {
    const window = this.window as WindowHandle;
    // 1. 显式解绑输入法（第二参数 0）；返回值不检查
    this.record(RULE_WINDOW_ATTACH, 'associate-input-context', { context: null });
    this.host.associateInputContext(window, null);
    // 2. 仅窗口态：取该窗口的菜单，交给辅助入口（类别 1），再重画菜单栏。
    //    占满屏态没有菜单 ⇒ 这三步都不做。
    if (this.screenMode !== 0) {
      this.record(RULE_WINDOW_ATTACH, 'get-menu', { window });
      const menu = this.host.getMenu(window);
      const params: OperateMenuParams = {
        category: MENU_CATEGORY_INSTALL,
        window,
        menu,
        extra: 0,
      };
      // ★ 菜单内容不是引擎建的：核心只转发窗口 / 菜单句柄 / 类别号，⛔ 不生成任何菜单项
      this.record(RULE_WINDOW_ATTACH, 'operate-menu', { ...params });
      this.host.operateMenu(params);
      this.record(RULE_WINDOW_ATTACH, 'draw-menu-bar', {});
      this.host.drawMenuBar(window);
    }
    // 3. 记录服务窗口槽（所有形态都做；次序在附加请求之后）
    this.record(RULE_WINDOW_ATTACH, 'record-service-window', { window }, 'engine');
    this.serviceWindow = window;
  }

  /* ── 4. 图形设备初始化（CORE-BOOT-GRAPHICS-INIT） ────────────────────────── */

  private async initGraphics(): Promise<boolean> {
    const createObject = this.config.getInt('set:CreateObject');
    const legacy = (createObject & 1) !== 0; // 最低位 = 1 ⇒ 旧式绘制；= 0 ⇒ 三维设备
    this.graphicsRoute = legacy ? 'legacy-draw' : 'three-dimensional';
    return legacy ? await this.initLegacyDraw() : await this.initThreeDimensional();
  }

  private async initLegacyDraw(): Promise<boolean> {
    const window = this.window as WindowHandle;
    const windowed = this.screenMode !== 0;
    const clientWidth = windowed ? this.config.getInt('set:WinX') : this.screenWidth;
    const clientHeight = windowed ? this.config.getInt('set:WinY') : this.screenHeight;
    const args: LegacyDrawParams = {
      window,
      clientWidth,
      clientHeight,
      behaviorBits: this.graphics.legacyBehaviorBits,
    };
    this.record(RULE_GRAPHICS_INIT, 'create-legacy-draw', { ...args });
    const drawObject = await this.host.createLegacyDraw(args);
    if (drawObject === null) {
      this.failGraphics('create-legacy-draw');
      return false;
    }
    this.drawObject = drawObject;
    return true;
  }

  private async initThreeDimensional(): Promise<boolean> {
    const window = this.window as WindowHandle;
    // 1. 创建 Direct3D 9 对象
    this.record(RULE_GRAPHICS_INIT, 'create-d3d9', { sdkVersion: D3D9_SDK_VERSION });
    const d3d = await this.host.createD3d9({ sdkVersion: D3D9_SDK_VERSION });
    if (d3d === null) {
      this.failGraphics('create-d3d9');
      return false;
    }
    // 2. 查询适配器显示模式（适配器 0）：桌面宽 / 高 / 格式
    this.record(RULE_GRAPHICS_INIT, 'query-adapter-display-mode', { adapter: ADAPTER_INDEX });
    const mode = await this.host.queryAdapterDisplayMode({ d3d, adapter: ADAPTER_INDEX });
    if (mode === null) {
      this.failGraphics('query-adapter-display-mode');
      return false;
    }
    // 3. 创建设备
    const configuredWidth = this.config.getInt('display:FullScreenWidth');
    const configuredHeight = this.config.getInt('display:FullScreenHeight');
    const deviceParams: DeviceParams = {
      d3d,
      window,
      screenMode: this.screenMode,
      adapterFormat: mode.format,
      fullscreenWidth: configuredWidth !== 0 ? configuredWidth : mode.width,
      fullscreenHeight: configuredHeight !== 0 ? configuredHeight : mode.height,
      deviceType: this.graphics.deviceType,
      vertexProcessing: this.graphics.vertexProcessing,
      presentationInterval: this.graphics.presentationInterval,
      multisample: { ...this.graphics.multisample },
      textureBitDepth: this.config.getInt('display:FullScreenBit'),
      pixelShaderVersion: this.graphics.pixelShaderVersion,
      fullscreenPresent: this.graphics.fullscreenPresent ?? this.screenMode === 0,
    };
    this.record(RULE_GRAPHICS_INIT, 'create-device', { ...deviceParams });
    const device = await this.host.createDevice(deviceParams);
    if (device === null) {
      this.failGraphics('create-device');
      return false;
    }
    this.device = device;
    // 4. 读纹理宽 / 高与像素着色器配置，建立上层绘制对象
    const textureWidth = this.config.getInt('set:TexWidth');
    const textureHeight = this.config.getInt('set:TexHeight');
    this.record(RULE_GRAPHICS_INIT, 'read-pixel-shader-config', { device });
    const pixelShader = await this.host.readPixelShaderConfig({ device });
    const drawParams: DrawObjectParams = { device, textureWidth, textureHeight, pixelShader };
    this.record(RULE_GRAPHICS_INIT, 'create-draw-object', { ...drawParams });
    const drawObject = await this.host.createDrawObject(drawParams);
    if (drawObject === null) {
      this.failGraphics('create-draw-object');
      return false;
    }
    this.drawObject = drawObject;
    return true;
  }

  /** 图形路线失败：写内建错误文本进错误通道，并结束启动（共享出口） */
  private failGraphics(step: GraphicsStep): void {
    this.errors.push(GRAPHICS_INIT_ERROR_TEXT);
    this.fail(RULE_GRAPHICS_INIT, 'graphics-init-failed', step, GRAPHICS_INIT_ERROR_TEXT);
  }

  private fail(
    rule: BootFailure['rule'],
    reason: BootFailure['reason'],
    step: GraphicsStep | null,
    errorText: string | null,
  ): void {
    this.failure = { rule, reason, step, errorText, sharedExit: true };
  }

  /* ── 5. 事件入口（主循环前置 / 泵 / 窗口过程） ───────────────────────────── */

  /**
   * 主循环**即将被调用之前**建周期定时器（标识 2、10000 毫秒、无回调）。
   * ★ 根脚本装载夹在图形设备与这一步之间，所以它不放在 `runStartup` 里。
   */
  beginMainLoop(): void {
    if (this.failure !== null) throw new Error(`window-session-failed:${this.failure.reason}`);
    const window = this.window;
    if (window === null) throw new Error('window-session-not-ready: 没有窗口，不能进入主循环');
    if (this.periodicTimerActive) return;
    this.record(RULE_EVENT_LOOP, 'set-timer', {
      timerId: PERIODIC_TIMER_ID,
      periodMs: PERIODIC_TIMER_PERIOD_MS,
      callback: null,
    });
    this.host.setTimer(window, {
      id: PERIODIC_TIMER_ID,
      periodMs: PERIODIC_TIMER_PERIOD_MS,
      callback: null,
    });
    this.periodicTimerActive = true;
  }

  /**
   * 消息泵的一趟：**窥视（不移除）→ 有消息就取一条 → 翻译 → 派发 → 回到窥视**，直到队列空。
   * ★ `GetMessage` 返回 0（退出消息）⇒ 抛 `EngineExit`（已证的唯一结束路径）。
   * @returns 本趟派发的消息条数
   */
  pump(): number {
    this.requireRunning();
    let dispatched = 0;
    for (;;) {
      this.record(RULE_EVENT_LOOP, 'peek-message', { remove: false });
      if (!this.host.peekMessage({ remove: false })) return dispatched;
      this.record(RULE_EVENT_LOOP, 'get-message', {});
      const message = this.host.getMessage();
      if (message === null) throw new EngineExit();
      this.record(RULE_EVENT_LOOP, 'translate-message', { message: message.kind });
      this.host.translateMessage(message);
      // DispatchMessage 调用的就是本层的事件入口（窗口过程）
      this.handleMessage(message);
      dispatched += 1;
    }
  }

  /**
   * 窗口过程（平台把消息转给这里）。
   * 本层处理：关闭 / 销毁 / 定时器 / 鼠标移动 / 离开 / 按下 / 激活；其余类别见 `ignored`。
   */
  handleMessage(message: WindowMessage): void {
    const window = this.window;
    if (window === null) throw new Error('window-session-not-ready: 没有窗口（启动已走共享出口）');
    this.record(RULE_EVENT_LOOP, 'dispatch-message', { message: message.kind }, 'engine');
    switch (message.kind) {
      case 'close': {
        // 先引擎自己的收尾（内容未取证 ⇒ 注入），再杀周期定时器，最后销毁窗口。
        // ★ 杀定时器是**无条件**的（spec 只说"杀掉那个周期定时器"，没有"先查它是否还在"这一步）
        this.record(RULE_EVENT_LOOP, 'engine-close-cleanup', { source: 'close' }, 'engine');
        this.engineTeardown?.();
        this.record(RULE_EVENT_LOOP, 'kill-timer', {
          timerId: PERIODIC_TIMER_ID,
          reason: 'close',
        });
        this.host.killTimer(window, PERIODIC_TIMER_ID);
        this.periodicTimerActive = false;
        this.record(RULE_EVENT_LOOP, 'destroy-window', { reason: 'close' });
        this.host.destroyWindow(window);
        return;
      }
      case 'destroy': {
        // 先隐藏，再投递退出消息
        this.record(RULE_EVENT_LOOP, 'hide-window', { reason: 'destroy' });
        this.host.hideWindow(window);
        this.record(RULE_EVENT_LOOP, 'post-quit-message', { exitCode: 0 });
        this.host.postQuitMessage(0);
        return;
      }
      case 'timer': {
        if (message.timerId === PERIODIC_TIMER_ID) {
          // 刷新"禁止系统空闲"；再按两个引擎格 + 前台窗口的合取补发应用激活
          this.record(RULE_EVENT_LOOP, 'set-thread-execution-state', {
            flags: EXECUTION_STATE_KEEP_AWAKE,
            reason: 'periodic-timer',
          });
          this.host.setThreadExecutionState(EXECUTION_STATE_KEEP_AWAKE);
          const engineCondition = this.timerActivationCondition?.() ?? false;
          if (engineCondition) {
            this.record(RULE_EVENT_LOOP, 'is-foreground-window', {});
            if (this.host.isForegroundWindow(window)) {
              this.record(RULE_EVENT_LOOP, 'post-window-message', { message: 'activate-app' });
              this.host.postWindowMessage(window, 'activate-app');
            }
          }
          return;
        }
        if (message.timerId === HOVER_TIMER_ID) {
          // 悬停定时器的到期行为本版不规定（显式记账；不发请求）
          this.record(RULE_EVENT_LOOP, 'hover-timer-expiry', { timerId: HOVER_TIMER_ID }, 'engine');
          return;
        }
        throw new Error(`window-message-unknown-timer:${message.timerId}`);
      }
      case 'mousemove': {
        if (message.inClient) this.ensureHoverTimer();
        else this.killHoverTimer('mousemove-out-of-client');
        return;
      }
      case 'mouseleave': {
        this.killHoverTimer('mouseleave');
        return;
      }
      case 'mousedown': {
        this.killHoverTimer('mousedown');
        return;
      }
      case 'activate': {
        // 转发给引擎的激活处理（本层只留痕；引擎侧接线见调用方）
        this.record(RULE_EVENT_LOOP, 'engine-activate', { active: message.active }, 'engine');
        return;
      }
    }
  }

  /** 鼠标在客户区内 ⇒ 建悬停定时器（标识 1、100 毫秒；已在效则不重复建） */
  private ensureHoverTimer(): void {
    if (this.hoverTimerActive) return;
    const window = this.window as WindowHandle;
    this.record(RULE_EVENT_LOOP, 'set-timer', {
      timerId: HOVER_TIMER_ID,
      periodMs: HOVER_TIMER_PERIOD_MS,
      callback: null,
      reason: 'mousemove-in-client',
    });
    this.host.setTimer(window, { id: HOVER_TIMER_ID, periodMs: HOVER_TIMER_PERIOD_MS, callback: null });
    this.hoverTimerActive = true;
  }

  /** 光标离开 / 按下键 ⇒ 杀悬停定时器 */
  private killHoverTimer(reason: string): void {
    if (!this.hoverTimerActive) return;
    const window = this.window as WindowHandle;
    this.record(RULE_EVENT_LOOP, 'kill-timer', { timerId: HOVER_TIMER_ID, reason });
    this.host.killTimer(window, HOVER_TIMER_ID);
    this.hoverTimerActive = false;
  }

  private requireRunning(): void {
    if (this.failure !== null) throw new Error(`window-session-failed:${this.failure.reason}`);
    if (this.window === null) throw new Error('window-session-not-ready: 没有窗口，不能泵消息');
  }

  private record(rule: string, request: string, args: Record<string, unknown>, via: 'platform' | 'engine' = 'platform'): void {
    this.trace.push({ rule, request, via, args: { ...args } });
  }

  private ignore(rule: string, what: string, reason: string, residual?: string): void {
    this.ignored.push({ disposition: 'ignored', rule, what, reason, ...(residual ? { residual } : {}) });
  }

  /**
   * 显式忽略清单（⛔ 不许静默跳过）：每一条都指到规范残差或本层边界。
   * ★ 顺序固定（测试按 `rule + what` 断言存在性）。
   */
  private declareIgnored(): void {
    this.ignore(
      RULE_WINDOW_CLASS,
      'boot-preamble',
      '单实例信号 / 辅助宿主模块装载 / COM 初始化属"启动前段"：06-window 不单独立条，'
        + '本层只把它们的产物（进程实例 / 资源实例）当入口句柄接收',
      'RES-BOOT-RESOURCE-INSTANCE',
    );
    this.ignore(
      RULE_WINDOW_CLASS,
      'icon-fallback',
      '图标资源缺失时平台返回空句柄，引擎不检查（RES-BOOT-CLASS-ICON-FALLBACK）⇒ '
        + '本层只转发资源名 + 资源实例，不对图标句柄做任何检查',
      'RES-BOOT-CLASS-ICON-FALLBACK',
    );
    this.ignore(
      RULE_WINDOW_CLASS,
      'class-style-bits',
      '类风格 `0x40` 的位含义属平台侧（本规范不解释）⇒ 本层只原样传值，不解释、不做外观断言',
      'RES-BOOT-CLASS-STYLE',
    );
    this.ignore(
      RULE_WINDOW_CREATE,
      'window-title',
      '建窗当时标题缓冲的实际内容本版不规定（RES-BOOT-WINDOW-TITLE）⇒ 本层传空串占位；'
        + '标题上限 0x100 字节只作为常量记账',
      'RES-BOOT-WINDOW-TITLE',
    );
    this.ignore(
      RULE_WINDOW_CREATE,
      'decoration-limit',
      '建窗之后还有一个"限制窗口装饰"的请求，由未取证的辅助对象决定（RES-BOOT-WINDOW-AERO）⇒ 本层不发该请求',
      'RES-BOOT-WINDOW-AERO',
    );
    this.ignore(
      RULE_WINDOW_CREATE,
      'windowed-client-vs-screen',
      '`set:WinX` / `set:WinY` 在分辨率变更路径上又被当作屏幕尺寸（RES-BOOT-WINDOW-CLIENT-VS-SCREEN）⇒ '
        + '本层只按启动期的客户区语义使用',
      'RES-BOOT-WINDOW-CLIENT-VS-SCREEN',
    );
    this.ignore(
      RULE_WINDOW_CREATE,
      'window-style-bits',
      '两个风格字的位含义属平台侧（RES-BOOT-WINDOW-STYLE）⇒ 本层只原样传值',
      'RES-BOOT-WINDOW-STYLE',
    );
    this.ignore(
      RULE_WINDOW_ATTACH,
      'ime-rebind',
      '解绑之后引擎是否在别处重新绑定输入法本版不规定（RES-BOOT-ATTACH-IME）⇒ 本层不提供重绑请求',
      'RES-BOOT-ATTACH-IME',
    );
    this.ignore(
      RULE_WINDOW_ATTACH,
      'menu-category-semantics',
      '「操作菜单」类别号的语义未取证（RES-BOOT-ATTACH-MENU-CATEGORY）⇒ 启动期只按 spec 传 1，不解释其它类别',
      'RES-BOOT-ATTACH-MENU-CATEGORY',
    );
    this.ignore(
      RULE_WINDOW_ATTACH,
      'menu-content',
      '菜单内容不是引擎建的：核心只把 GetMenu 拿到的句柄 + 类别号 1 + 第 4 实参 0 原样转给辅助入口，'
        + '再请求重画菜单栏；⛔ 不生成、不编辑任何菜单项（内容属于辅助宿主模块的资源）',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'device-param-mapping',
      '键 → 设备参数的逐位映射未取证（RES-BOOT-GFX-PARAMS）⇒ 未映射的字段（设备类型 / 顶点处理 / 呈现间隔 / '
        + '多重采样 / 像素着色器版本）取注入值或本实现的默认值；纹理位深取 `display:FullScreenBit`（映射亦未取证）',
      'RES-BOOT-GFX-PARAMS',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'fullscreen-present-combination',
      '窗口态 / 占满屏态与"是否全屏呈现"的完整组合表未规定（RES-BOOT-GFX-FULLSCREEN-COMBO）⇒ '
        + '默认取 `S == 0`，可由注入覆盖',
      'RES-BOOT-GFX-FULLSCREEN-COMBO',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'legacy-behavior-bits',
      '旧式绘制路线的"行为位"取值与来源未取证（06-window 只说"参数含行为位"）⇒ 默认 0，可注入',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'pixel-shader-config-source',
      '"像素着色器配置"读的是哪一层的哪个键未取证 ⇒ 本层按"设备能力查询"取平台结果',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'error-text',
      '错误文本的类别与内容未取证（RES-BOOT-GFX-TEXT）⇒ 本层写实现侧的内建常量（内容不是规范结论）',
      'RES-BOOT-GFX-TEXT',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'device-lost',
      '设备丢失后的重建属运行期（RES-BOOT-GFX-DEVICE-LOST）⇒ 本层不处理',
      'RES-BOOT-GFX-DEVICE-LOST',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'timer-activation-condition',
      '周期定时器到期时两个引擎格（"状态选择字里的一个位"的位号 / "另一个引擎级格"的身份）未取证'
        + '（RES-BOOT-TIMER-CONDITION）⇒ 核心不读引擎格，合取结果由注入的判据给出；未注入则不合取、不补发',
      'RES-BOOT-TIMER-CONDITION',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'pump-second-site',
      '泵的第二处（睡眠 + 窥视）的睡眠时长属《执行与时间》（RES-BOOT-PUMP-SECOND-PATH）⇒ '
        + '本层只实现"窥视 → 取一条 → 翻译 → 派发"那一处',
      'RES-BOOT-PUMP-SECOND-PATH',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'window-proc-other-messages',
      '窗口过程处理的全部消息类别未逐条登记（RES-BOOT-WNDPROC-CLOSURE）⇒ 本层只做生命周期 / 定时器 / '
        + '悬停几类；最小最大跟踪尺寸、光标、系统菜单、按键 / 鼠标 / 滚轮与绘制转发都不在本层入口集合',
      'RES-BOOT-WNDPROC-CLOSURE',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'hover-timer-expiry',
      '悬停定时器（标识 1）的到期行为本版不规定 ⇒ 到期只记账、不发请求',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'close-cleanup-items',
      '"关闭"消息里引擎自己的收尾没有逐项取证（RES-BOOT-CLOSE-CLEANUP）⇒ 内容由注入的 `engineTeardown` 提供；'
        + '本层只保证它在杀周期定时器与销毁窗口之前',
      'RES-BOOT-CLOSE-CLEANUP',
    );
    this.ignore(
      RULE_EVENT_LOOP,
      'loop-return-paths',
      '除"取到退出消息 → 抛异常"外的结束路径未取证（RES-BOOT-LOOP-RETURN）⇒ 本层只实现已证路径，'
        + '主循环不返回',
      'RES-BOOT-LOOP-RETURN',
    );
  }
}

/**
 * 按 `06-window.md` 的顺序执行启动期的窗口 / 图形设备阶段，返回会话。
 *
 * 步骤：注册窗口类 → 建窗 → 显示 / 重绘 → 解绑输入法 + 记录服务窗口槽 + 仅窗口态装菜单
 *       → 图形设备（`set:CreateObject` 最低位选路线）→ 启动期直接请求一次空闲抑制。
 * ★ 失败 ⇒ 共享出口：`session.failure` 非空，窗口 / 图形对象为空，之后的请求一条都不发（⛔ 不弹消息）。
 * ★ 根脚本装载夹在图形设备与周期定时器之间：调用方装完根脚本后调 `session.beginMainLoop()`。
 * ★ `async`：图形设备的请求是 `Promise`（设备创建要等平台的图形后端真的就绪，见 `GraphicsHost`）；
 *   窗口 / 消息请求仍是同步的 —— 本函数按 spec 次序 `await` 每一步，失败即停。
 */
export async function bootWindow(opts: BootWindowOptions): Promise<WindowSession> {
  const session = new WindowSession(opts);
  await session.runStartup();
  return session;
}
