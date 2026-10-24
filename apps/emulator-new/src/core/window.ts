/**
 * 启动期的**窗口 / 图形设备 / 事件入口**（`data/spec/chapters/06-window.md` 的参考实现，平台无关核心）。
 *
 * ★ 纯核心：**不 import** `node:*` / `electron` / `pixi.js` / DOM，只 import 核心内的类型。
 *   平台侧要做的事全在 `BootHost`（`WindowHost & MessageHost & GraphicsHost`）里：
 *   核心只**发请求、收句柄 / 结果**；请求参数是**领域字段**（`clientWidth` / `style` / `menuResourceId`…），
 *   ⛔ 核心不认识任何 Win32 结构体 / 消息号 / D3D 结构体。
 * ★ 同步 / 异步的分界：`WindowHost` 的**结果型**请求（查尺寸 / 建窗 / 取菜单）允许 `MaybePromise`
 *   （Electron 的 renderer 桥是异步的；单元测试 fake 仍可同步返回）；`MessageHost` 的泵接口保持同步
 *   （renderer 侧用本地队列）。`GraphicsHost` 全是 `Promise` —— 设备创建要等平台的图形后端真的就绪
 *   （Electron 侧现在由 renderer 自己的 Pixi 立即回答；CLI 仍是 headless）。
 *   同步返回假句柄会让 `CORE-BOOT-GRAPHICS-INIT` 的
 *   "失败 ⇒ 写错误通道 + 结束启动（不建主循环 / 不装载根脚本）"退化成事后补救。
 *   ⇒ `bootWindow` / `runStartup` / 图形阶段是 `async`，按 spec 次序 `await` 每一步。
 *
 * ★ 规则对应（五条，逐条可追）：
 *   1. `CORE-BOOT-WINDOW-CLASS`   —— `registerWindowClass`（类名 / 实例 / 图标 / 光标 / 背景刷 / 菜单名 / 风格）
 *   2. `CORE-BOOT-WINDOW-CREATE`  —— 形态字 S（`display:ScreenMode` + `display:ForceScreen`）→ 两种形态 → 显示 / 重绘
 *   3. `CORE-BOOT-WINDOW-ATTACH`  —— 解绑输入法 → 记录服务窗口槽 → 仅窗口态安装菜单
 *   4. `CORE-BOOT-GRAPHICS-INIT`  —— `set:CreateObject` **两个独立位**选路线（0 旧式 / 1 三维，键 3 两条都跑）；
 *      设备创建的每个参数都由配置键推导（呈现间隔 / 是否全屏 / 后缓冲三分支 / 多重采样 / 重试序列），
 *      设备建成之后按 spec 发对话框模式 → 能力查询 → 32 位格式检查 → 清渲染目标；
 *      失败按逐步骤表写**分类日志**并决定是否结束启动（⛔ 不静默；"错误文本 ≠ 致命"）；
 *      日志落点 = 配置根 [+分隔符] + `set:RegFilePath` [+分隔符] + 固定文件名（`joinEnginePath`），
 *      在**窗口附加之后、图形设备之前**打开一次。
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

/** 分辨率校验的**高宽比阈值**（spec「校验分辨率」：`r ≥ 0.7` ⇒ 窄档池，否则宽档池） */
export const RESOLUTION_ASPECT_THRESHOLD = 0.7;

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

/* ───────────── 设备创建请求的实参：键 → 值的映射（spec「设备创建请求的实参」） ───────────── */

/** 设备类型（spec 表：1 ⇒ 硬件；其它非 0 ⇒ 参考；0 ⇒ 依次试 1、2） */
export type DeviceType = 'hal' | 'reference';
/** 顶点处理（spec 表：1 ⇒ 硬件、2 ⇒ 软件、3 ⇒ 混合；0 ⇒ 依次试 1、2、3） */
export type VertexProcessing = 'software' | 'hardware' | 'mixed';
/** 交换效果（窗口化 ⇒ 复制；全屏 ⇒ 丢弃；多重采样启用时强制丢弃） */
export type SwapEffect = 'copy' | 'discard';

/**
 * `display:PresentInterval` → 请求里的"等待垂直回扫次数"。
 * 0 ⇒ 立即（`0`）；1 / 2 / 3 / 4 ⇒ `1 / 2 / 4 / 8`；**其它值 ⇒ 平台默认（`null`）**。
 * ★ 键缺 ⇒ 读 0 ⇒ 立即；★ 3 → 4、4 → 8 不是笔误：请求里的是"档位值"，不是配置原值。
 */
export function presentationIntervalOf(configValue: number): number | null {
  switch (configValue) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    default: return null;
  }
}

/**
 * `display:DeviceType` → 设备类型的尝试顺序（spec 表：0 ⇒ 依次试 1、2）。
 * ★ 返回值是**顺序表**：调用方按顺序尝试、任一成功即停（见 `deviceCreationAttempts`）。
 */
export function deviceTypeCandidates(configValue: number): DeviceType[] {
  if (configValue === 0) return ['hal', 'reference'];
  return [configValue === 1 ? 'hal' : 'reference'];
}

/**
 * `display:VertexProcessing` → 顶点处理的尝试顺序（spec 表：0 ⇒ 依次试 1、2、3）。
 * ★ 返回值是**顺序表**：调用方按顺序尝试、任一成功即停（见 `deviceCreationAttempts`）。
 */
export function vertexProcessingCandidates(configValue: number): VertexProcessing[] {
  if (configValue === 0) return ['hardware', 'software', 'mixed'];
  if (configValue === 1) return ['hardware'];
  if (configValue === 2) return ['software'];
  return ['mixed'];
}

/** 一次设备创建尝试的两个取值（其余呈现参数每次尝试都一样） */
export interface DeviceCreationAttempt {
  deviceType: DeviceType;
  vertexProcessing: VertexProcessing;
}

/**
 * 设备创建的**尝试序列**（spec「尝试序列」）：设备类型是**外层**、顶点处理是**内层**；
 * 每个新的设备类型都把顶点处理**重置为 1**（内层每次都从顺序表的起点开始）。
 * - 两键都为 0 ⇒ 6 次：(1,1) → (1,2) → (1,3) → (2,1) → (2,2) → (2,3)；
 * - 只有设备类型为 0 ⇒ 2 次；只有顶点处理为 0 ⇒ 3 次；都不为 0 ⇒ 1 次。
 * ★ 停止条件只有一个：某次尝试成功；枚举耗尽仍是失败（由调用方收口）。
 */
export function deviceCreationAttempts(
  deviceTypeKey: number,
  vertexProcessingKey: number,
): DeviceCreationAttempt[] {
  const attempts: DeviceCreationAttempt[] = [];
  for (const deviceType of deviceTypeCandidates(deviceTypeKey)) {
    for (const vertexProcessing of vertexProcessingCandidates(vertexProcessingKey)) {
      attempts.push({ deviceType, vertexProcessing });
    }
  }
  return attempts;
}

/**
 * 旧式绘制路线的**行为位**（spec「旧式绘制路线的实参」）：
 * `display:UseDirectDrawEmuration` 原值 **或上**（形态字 `display:ScreenMode` ≠ 0 ⇒ 2 / = 0 ⇒ 0）。
 * ★ 旧式路线**不看** `display:VirtualFullScreen`（那是三维路线独有的规则）；
 *   `display:FullScreenBit` 是**独立的第 4 实参**，⛔ 不进这里、也不与任何键做或。
 */
export function legacyBehaviorBitsOf(useDirectDrawEmuration: number, screenMode: number): number {
  return (useDirectDrawEmuration | (screenMode !== 0 ? 2 : 0)) >>> 0;
}

/* ───────────── 日志文本的类别（spec「失败后果」：错误 / 警告 / 诊断（无前缀）） ───────────── */

/** 日志文本类别：错误 / 警告 / 诊断（无前缀） */
export type GraphicsTextCategory = 'error' | 'warning' | 'diagnostic';

/**
 * 日志文本的**落点**（spec「日志通道的落点与丢弃」）：
 * - `file` = `system:OutErrorLog` 非 0 且宿主把日志文件打开成功 ⇒ 文本真的会进那个文件；
 * - `unavailable` = 该键为 0 / 缺键（不打开）、或打开失败 ⇒ 句柄无效，**整条文本被静默丢弃**。
 * ★ `graphicsLog` 只表示"引擎产生了这条文本"，不表示"它到了磁盘"；两者不是同一件事。
 * ★ 核心**不持有**文件句柄（文件系统全走 `Host`）：本字段只是落点状态，纯数据。
 */
export type GraphicsLogSink = 'unavailable' | 'file';

/**
 * 图形路线里会写日志的步骤 —— 比 `GraphicsStep`（失败步骤）宽：
 * 另含两处"有文本但不中止"的步骤，以及实现侧"宿主缺可选方法"的诊断。
 */
export type GraphicsLogStep = GraphicsOutcomeStep | 'host-missing-method' | 'config-missing-key';

/** 一条带**类别**的日志（⛔ 不是自由文本：类别 + 步骤 + 文本三件套） */
export interface GraphicsLogEntry {
  category: GraphicsTextCategory;
  step: GraphicsLogStep;
  text: string;
}

/** 逐步骤的日志类别与后果（spec「失败后果」的逐步骤表） */
export interface GraphicsStepOutcome {
  /** 写哪一类日志；`null` = 这一步**不写日志**（文本直接送给用户） */
  log: GraphicsTextCategory | null;
  /** 是否结束启动 */
  fatal: boolean;
}

/** 逐步骤表里的行（含两处"有文本但不中止"的步骤，它们不是失败步骤） */
export type GraphicsOutcomeStep = GraphicsStep | 'set-dialog-box-mode' | 'check-texture-format-32';

/**
 * spec「失败后果」的**逐步骤表**（纯数据，守卫可直接钉）。
 * ★ 关键一条：`set-dialog-box-mode` 写**错误**类文本却 `fatal: false` ——
 *   "文本前缀 = 致命性"是错的，实现必须按本表的 `fatal` 决定是否中止。
 * ★ 旧式路线与"上层绘制对象"**不写日志**：它们的文本直接送用户（本层记在 `failure.errorText`）。
 */
export const GRAPHICS_STEP_OUTCOME = {
  'create-legacy-draw': { log: null, fatal: true },
  'create-d3d9': { log: 'error', fatal: true },
  'query-adapter-display-mode': { log: 'error', fatal: true },
  'create-device': { log: 'error', fatal: true },
  'validate-resolution': { log: 'diagnostic', fatal: true },
  'set-dialog-box-mode': { log: 'error', fatal: false },
  'query-device-caps': { log: 'error', fatal: true },
  'check-texture-format-32': { log: 'warning', fatal: false },
  'create-draw-object': { log: null, fatal: true },
} as const satisfies Record<GraphicsOutcomeStep, GraphicsStepOutcome>;

/**
 * 图形设备初始化失败时写进错误通道的**内建文本**。
 * ★ 规范只要求"把一条内建错误文本写进错误通道"，文本的**类别与内容**未取证（`RES-BOOT-GFX-TEXT`）
 *   ⇒ 本实现用一个实现侧常量占位；内容不是规范结论。
 * ★ 设备创建重试穷尽时**复用"取显示模式"的文本**（spec「失败后果」）——这里同一个占位常量就是它。
 */
export const GRAPHICS_INIT_ERROR_TEXT = 'graphics-init-failed';

/**
 * 32 位纹理格式不可用时写进**警告通道**的内建文本（spec：不可用只写一条警告，**不**结束启动）。
 * ★ 警告文本的内容不是规范结论（`RES-BOOT-GFX-TEXT`）。
 */
export const TEXTURE_FORMAT_32_WARNING_TEXT = 'texture-format-32-unavailable';

/**
 * 对话框绘制模式失败的**错误类**文本（spec：写错误文本但**不**结束启动）。
 * ★ 与上面的失败文本分开：类别相同、后果不同 ⇒ ⛔ 不许用它推断启动是否结束。
 */
export const GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT = 'graphics-dialog-box-mode-failed';

/**
 * 分辨率校验失败时写进**诊断通道**的一行（spec：诊断 + "请求的分辨率不可接受"，随后结束启动）。
 * ★ 同时**不写回**请求宽 / 高（`resolvedFullScreen` 保持 `null`，后缓冲仍按配置键 / 客户区算）。
 */
export const GRAPHICS_RESOLUTION_REJECTED_TEXT = 'requested-resolution-not-acceptable';

/* ─────────────── 日志文件的路径（spec「日志通道的落点与丢弃」） ─────────────── */

/** 日志文件名（引擎内建常量，⛔ 不来自配置） */
export const ERROR_LOG_FILE_NAME = 'Error.log';

/** 整条日志路径的上限（spec：256 **字节**；超长按字节截断后照用，引擎不检查返回值） */
export const ERROR_LOG_MAX_PATH_BYTES = 0x100;

/**
 * 引擎的路径分隔符：引擎写的是反斜杠。
 * ★ 核心**不按平台换**（核心不 import `node:path`）；"引擎路径 → 宿主路径"是平台层的事
 *   （`Host.pathFromEngine` 的同一口径）。
 */
export const ENGINE_PATH_SEPARATOR = '\\';

/** `joinEnginePath` 的输入（三个领域字段 + 一个上限） */
export interface EnginePathParts {
  /** **已解析**的配置根（`set:UseAppDataFolder` 恰为 1 ⇒ 用户本地应用数据目录；其它值 ⇒ 空串） */
  root: string;
  /** `set:RegFilePath` 的原值（不展开、不补分隔符） */
  relativeDirectory: string;
  fileName: string;
  /** 整条路径的字节上限；缺省 = `ERROR_LOG_MAX_PATH_BYTES`（256） */
  maxPathBytes?: number;
}

/**
 * 纯函数：按引擎规则拼一条路径 —— `配置根 [+分隔符] + 相对目录 [+分隔符] + 固定文件名`。
 *
 * ★ **两个分隔符各自的条件是"拼到此处的结果非空"**，⛔ 不是"根非空"、也不是"相对目录非空"：
 *   - 根与相对目录都空 ⇒ **裸文件名**（相对进程工作目录，⛔ 不是 `\Error.log`）；
 *   - 根非空、相对目录空 ⇒ **连续两个分隔符**；
 *   - 相对目录自带尾分隔符 ⇒ 也**不去重**（`C:\base\` + `savedata` ⇒ `C:\base\\savedata\…`）。
 *   ⇒ ⛔ 不许用 `path.join` / `path.resolve`：它们会折叠重复分隔符、吃掉空段，与本合同不符。
 * ★ 超长 ⇒ 按**字节**截断（引擎传给路径构造器的缓冲上限就是 256 字节）。
 *   核心不认识 ANSI 代码页 ⇒ 字节数按 UTF-8 计（纯 ASCII 路径与 ANSI 逐字节一致；非 ASCII 是保守近似）。
 */
export function joinEnginePath(parts: EnginePathParts): string {
  const maxPathBytes = parts.maxPathBytes ?? ERROR_LOG_MAX_PATH_BYTES;
  let path = parts.root;
  if (path !== '') path += ENGINE_PATH_SEPARATOR;
  path += parts.relativeDirectory;
  if (path !== '') path += ENGINE_PATH_SEPARATOR;
  path += parts.fileName;
  return truncateToUtf8Bytes(path, maxPathBytes);
}

/** 按 UTF-8 字节数截断（不切断多字节序列 / 代理对） */
function truncateToUtf8Bytes(text: string, maxBytes: number): string {
  if (maxBytes <= 0) return '';
  const encoder = new TextEncoder();
  if (encoder.encode(text).byteLength <= maxBytes) return text;
  let out = '';
  let used = 0;
  for (const ch of text) {
    const size = encoder.encode(ch).byteLength;
    if (used + size > maxBytes) break;
    used += size;
    out += ch;
  }
  return out;
}

/**
 * 日志落点的**引擎侧**参数（领域字段）：核心给出"文件名 / 相对目录 / 是否用配置根 / 上限 / 已解析的配置根"，
 * 宿主只负责探存在性 → 递归建目录 → **覆盖**打开。
 * ★ `useAppDataFolder` 判的是**恰为 1**（⛔ 不是"非 0"）—— spec 明写"恰为 1 ⇒ 用户应用数据目录；其它值 ⇒ 空串"。
 */
export interface ErrorLogPathSpec {
  fileName: string;
  /** `set:RegFilePath` 的原值（不展开、不补分隔符） */
  relativeDirectory: string;
  /** `set:UseAppDataFolder === 1`（恰为 1） */
  useAppDataFolder: boolean;
  /** 整条路径的字节上限 */
  maxPathBytes: number;
  /**
   * **已解析**的配置根（纯数据）：`useAppDataFolder` 为真 ⇒ 宿主 `appDataDir()` 给的目录；否则 `''`。
   * ★ 宿主直接把它当 `joinEnginePath` 的 `root` —— 打开时不必再问一次平台。
   */
  baseDirectory: string;
}

/* ─────────────────────────── 平台句柄（对核心不透明） ─────────────────────────── */

/**
 * 平台句柄：对核心**不透明**的数字。核心只把它从 `entry` 收进来、再原样发给平台，
 * ⛔ 不比较、不运算、不解释（唯一例外是"是不是 null"）。
 */
export type PlatformHandle = number;

/**
 * 平台调用的同步 / 异步分界：核心本身不关心宿主是同步返回还是 Promise。
 * ★ 结果型请求（查尺寸 / 建窗 / 取菜单）允许 `Promise`：Electron 的 renderer 桥是异步的；
 *   单元测试里的 fake 仍可直接返回同步值（`await` 对两者都成立）。
 * ★ ⛔ 不要把它套到 `MessageHost` 的 `peekMessage` / `getMessage` 上：消息泵在每条指令前都要问，
 *   renderer 侧用本地队列保持同步；异步只能用于启动期的一次性请求。
 */
export type MaybePromise<T> = T | Promise<T>;
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

/** 逐档判定的七个符号（spec「校验分辨率」：判定专属，⛔ 不钉字节与排版） */
export type ResolutionVerdictSymbol = '×' | '―' | '△' | '○' | '▲' | '■' | '●';

/** 一档显示模式的判定（逐档诊断 + 收口输入） */
export interface ResolutionModeVerdict {
  width: number;
  height: number;
  /** 模式自己的**高宽比** `r = height / width`（spec 判定的量） */
  ratio: number;
  symbol: ResolutionVerdictSymbol;
}

/** 一次分辨率校验的结果 */
export interface ResolutionVerdict {
  /**
   * 选中的一档；`null` = 收口后没有任何一档 ⇒ **不写回**请求宽 / 高、结束启动
   * （spec「失败」：请求宽 / 高保持原值）。
   */
  picked: { width: number; height: number } | null;
  /** 逐档判定（与输入 `modes` **同序**；诊断用） */
  perMode: ResolutionModeVerdict[];
}

/**
 * 分辨率校验的**纯函数**判据（spec「校验分辨率」§3 的伪代码逐字实现）。
 *
 * 判定（按 spec 的次序，⛔ 别按"最接近"重写）：
 * 1. **尺寸闸**：模式宽 < W 或模式高 < H ⇒ `×` 出局（⛔ 不许"缩小凑合"）；
 * 2. **分流**（按**模式自己**的 `r`）：`r ≥ 0.7` ⇒ 窄档池（`●` / `▲` / `■` / `―`）；
 *    `r < 0.7` ⇒ 宽档池（`○` / `△` / `×` / `―`）；
 * 3. **比值闸**：`r < R` ⇒ 出局（宽档 `×` / 窄档 `■`）；`r = R` ⇒ 进池（`○` / `●`）；
 *    `r > R` ⇒ 进池（`△` / `▲`）；
 * 4. **池的替换**：每个池只留**一档**；新模式只有宽与高都**不大于**池里现档时才替换，否则记 `―`
 *    ⇒ 池里是"过闸的**最小一档**"，⛔ 不是"比值最接近"；
 * 5. **收口**（两支不对称，⛔ 别合并成"取两者更小的"）：
 *    `R ≥ 0.7` ⇒ 先取宽档池，若窄档池的宽**严格更小**则改取窄档池；
 *    `R < 0.7` ⇒ 宽档池非空就取它（忽略窄档池），空才取窄档池。
 *
 * ★ 纯函数：不读配置、不发请求、不写会话（守卫可直接钉）。
 *
 * @param modes 适配器枚举出来的可用模式（按宿主给的顺序）
 * @param requestedWidth 请求对的宽（`display:FullScreenWidth` 整对覆盖或客户区宽）
 * @param requestedHeight 请求对的高
 */
export function pickResolution(
  modes: readonly DisplayMode[],
  requestedWidth: number,
  requestedHeight: number,
): ResolutionVerdict {
  // R = 请求高 / 请求宽；A = 宽档池（r < 0.7）、B = 窄档池（r ≥ 0.7）—— 与 spec 伪代码同名。
  const R = requestedHeight / requestedWidth;
  let wide: ResolutionModeVerdict | null = null;
  let narrow: ResolutionModeVerdict | null = null;
  const perMode: ResolutionModeVerdict[] = [];
  for (const mode of modes) {
    const ratio = mode.height / mode.width;
    const entry: ResolutionModeVerdict = { width: mode.width, height: mode.height, ratio, symbol: '×' };
    if (mode.width < requestedWidth || mode.height < requestedHeight) {
      // 1. 尺寸闸
      entry.symbol = '×';
    } else if (ratio >= RESOLUTION_ASPECT_THRESHOLD) {
      // 2' / 3 / 4. 窄档池（B）：先过比值闸，再判池替换
      if (ratio === R) {
        if (narrow !== null && (narrow.width < mode.width || narrow.height < mode.height)) entry.symbol = '―';
        else { narrow = entry; entry.symbol = '●'; }
      } else if (ratio > R) {
        if (narrow !== null && (narrow.width < mode.width || narrow.height < mode.height)) entry.symbol = '―';
        else { narrow = entry; entry.symbol = '▲'; }
      } else {
        entry.symbol = '■';
      }
    } else if (ratio === R) {
      // 2 / 3 / 4. 宽档池（A）：先过比值闸，再判池替换
      if (wide !== null && (wide.width < mode.width || wide.height < mode.height)) entry.symbol = '―';
      else { wide = entry; entry.symbol = '○'; }
    } else if (ratio > R) {
      if (wide !== null && (wide.width < mode.width || wide.height < mode.height)) entry.symbol = '―';
      else { wide = entry; entry.symbol = '△'; }
    } else {
      entry.symbol = '×';
    }
    perMode.push(entry);
  }
  // 5. 收口（两支不对称）：结果统一落到 wide（spec 的 A）。
  if (R >= RESOLUTION_ASPECT_THRESHOLD) {
    if (narrow !== null && (wide === null || narrow.width < wide.width)) wide = narrow;
  } else if (wide === null) {
    wide = narrow;
  }
  return {
    picked: wide === null ? null : { width: wide.width, height: wide.height },
    perMode,
  };
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
  frameMetrics(): MaybePromise<FrameMetrics>;
  /** 查询屏幕宽 / 高（占满屏态建窗前显式调用） */
  screenSize(): MaybePromise<ScreenSize>;
  /** 注册窗口类；`null` = 注册失败（引擎看到的返回 0） */
  registerWindowClass(params: WindowClassParams): MaybePromise<WindowClassHandle | null>;
  /** 建顶层窗口；`null` = 建窗失败（句柄为空） */
  createWindow(params: WindowCreateParams): MaybePromise<WindowHandle | null>;
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
  getMenu(window: WindowHandle): MaybePromise<PlatformHandle | null>;
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

/**
 * 旧式绘制路线的参数（spec「旧式绘制路线的实参」）。
 * ★ 这条路线**没有**设备 / 呈现参数，只有一次"建立绘制对象"的请求；五个实参如下。
 * ⛔ `set:AlwaysBackupSurface` 不是实参（它只写进引擎自己的一个格子）；
 * ⛔ `display:VirtualFullScreen` 在旧式路线上**没有**作用（"窗口 / 占屏"只看形态字）。
 */
export interface LegacyDrawParams {
  window: WindowHandle;
  /** 客户区宽 / 高（引擎级缓存；与三维路线第 3 / 4 实参同一个来源） */
  clientWidth: number;
  clientHeight: number;
  /**
   * 行为位 = `display:UseDirectDrawEmuration` **原值** 或上（形态字 ≠ 0 ⇒ 2 / = 0 ⇒ 0）。
   * 见 `legacyBehaviorBitsOf`。
   * ★ 位 1 是"窗口化请求"；**对象内部**可能在两次判断后把它清掉（spec「旧式绘制对象内部的"窗口化"降级」）。
   *   那是对象内部的事：⛔ 本层不建模、也不据此改任何引擎级形态字（`session.screenMode` 仍是配置值）。
   */
  behaviorBits: number;
  /** `display:FullScreenBit` **原值直传**（独立实参，⛔ 不与任何键做或） */
  fullscreenBit: number;
}

/**
 * 三维设备路线的设备参数（领域字段）。**全部由配置键推导**（spec「设备创建请求的实参」）：
 * ⛔ 调用方不再注入设备类型 / 顶点处理 / 呈现间隔 / 多重采样 / 是否全屏 —— 那五个键是合同的输入。
 */
export interface DeviceParams {
  d3d: D3d9Handle;
  window: WindowHandle;
  /** 屏幕形态字 S（`display:ScreenMode` 经 `display:ForceScreen` 归一后的值） */
  screenMode: number;
  /** 后缓冲格式：适配器显示模式的格式（⛔ 不来自配置） */
  adapterFormat: number;
  /** 后缓冲宽 / 高（spec「后缓冲尺寸」三条互斥分支的结果） */
  backBufferWidth: number;
  backBufferHeight: number;
  /** 设备类型（`display:DeviceType`；取 0 时这里是"顺序表第一个"） */
  deviceType: DeviceType;
  /** 顶点处理（`display:VertexProcessing`；取 0 时这里是"顺序表第一个"） */
  vertexProcessing: VertexProcessing;
  /** 呈现间隔：`0` = 立即、`n` = 等待 n 个回扫；`null` = 平台默认（`display:PresentInterval` 其它值） */
  presentationInterval: number | null;
  /** 多重采样：类型原样来自 `display:MultiSample`（0 ⇒ 不启用）；quality 本版不规定 ⇒ 0 */
  multisample: { type: number; quality: number };
  /** 交换效果（窗口化 ⇒ `copy`；全屏 ⇒ `discard`；多重采样启用时强制 `discard`） */
  swapEffect: SwapEffect;
  /** 后缓冲可锁定（固定开；多重采样启用时关） */
  backBufferLockable: boolean;
  /** 是否全屏呈现（与"窗口态 / 占满屏态"**不是**同一个量，见 spec 成立条件） */
  fullscreenPresent: boolean;
}

/** 上层绘制对象的参数（设备建成之后的最后一步） */
export interface DrawObjectParams {
  device: DeviceHandle;
  /** 纹理宽 / 高（取自 `set:TexWidth` / `set:TexHeight`） */
  textureWidth: number;
  textureHeight: number;
  /**
   * 纹理位深：`display:TextureBit`。
   * ★ 设备建成**之后**才读，用来配置上层绘制对象 —— ⛔ 不是设备创建请求的参数（spec 明确）。
   */
  textureBitDepth: number;
  /** 像素着色器版本：`display:PixelShader`（同上：不是设备创建请求的参数） */
  pixelShader: number;
}

/** 设备能力（最大纹理宽 / 高；spec「查询设备能力」） */
export interface DeviceCaps {
  maxTextureWidth: number;
  maxTextureHeight: number;
}

/**
 * 图形设备（两条互斥路线）。
 * ★ 全部 `Promise`：每一步都要等平台给出**真实结果**（`null` = 失败）；⛔ 不许先返一个假句柄再补错误。
 * ★ 设备建成之后的四个请求是**可选方法**（本层的新增能力）：宿主没有提供时核心记**诊断**
 *   （`session.diagnostics`），⛔ 不静默跳过。设备能力查询缺席 ⇒ 无法做 spec 的"纹理能力下限"致命比较，
 *   这就是一条实现侧缺口，必须显式记账。
 */
export interface GraphicsHost {
  /** 旧式绘制路线；`null` = 失败 */
  createLegacyDraw(params: LegacyDrawParams): Promise<DrawObjectHandle | null>;
  /** 三维路线 1：创建 Direct3D 9 对象（SDK 版本号 32）；`null` = 失败 */
  createD3d9(params: { sdkVersion: number }): Promise<D3d9Handle | null>;
  /** 三维路线 2：查询适配器显示模式（适配器 0）；`null` = 失败 */
  queryAdapterDisplayMode(params: { d3d: D3d9Handle; adapter: number }): Promise<DisplayMode | null>;
  /**
   * 三维路线 3a（**可选**）：枚举该适配器可用的显示模式（分辨率校验用）。
   * ★ 缺席 ⇒ 无法执行 spec 第 3 步的枚举 / 诊断 / 写回 ⇒ 显式记账（⛔ 不静默跳过）。
   */
  enumerateAdapterDisplayModes?(params: { d3d: D3d9Handle; adapter: number }): Promise<DisplayMode[]>;
  /** 三维路线 4：创建设备；`null` = 失败（**重试序列**见 `deviceCreationAttempts`，成功即停） */
  createDevice(params: DeviceParams): Promise<DeviceHandle | null>;
  /**
   * 三维路线 4a（可选）：请求对话框绘制模式（参数 = 开）。
   * ★ 返回 `false` = 这一次请求失败 ⇒ 写**错误**类文本但**不**中止（spec 唯一一处）。
   */
  setDialogBoxMode?(params: { device: DeviceHandle; enabled: boolean }): Promise<boolean>;
  /** 三维路线 4b（可选）：查询设备能力（最大纹理宽 / 高） */
  queryDeviceCaps?(params: { device: DeviceHandle }): Promise<DeviceCaps>;
  /** 三维路线 4c（可选）：32 位纹理格式可用性检查（`false` ⇒ 只写一条警告） */
  checkTextureFormat32?(params: { device: DeviceHandle }): Promise<boolean>;
  /** 三维路线 4d（可选）：清一次渲染目标 */
  clearRenderTarget?(params: { device: DeviceHandle }): Promise<void>;
  /**
   * 三维路线 4e：建立上层绘制对象；`null` = 失败
   */
  createDrawObject(params: DrawObjectParams): Promise<DrawObjectHandle | null>;
  /**
   * 日志落点（**可选**，spec「日志通道的落点与丢弃」）：`system:OutErrorLog` **非 0** 时，宿主按 `spec`
   * 拼路径并**覆盖**打开那个固定的日志文件；返回 `true` = 句柄可用（`session.logSink = 'file'`），
   * `false` = 打开失败（`'unavailable'`，之后的文本**静默丢弃** —— 不写第二处、不弹框、不影响启动）。
   * ★ 核心**不做 IO**、但**持有拼接规则**：宿主必须用
   *   `joinEnginePath({ root: spec.baseDirectory, relativeDirectory: spec.relativeDirectory, … })`
   *   （⛔ 不是 `path.join` 的折叠语义），再按 spec 探存在性 → 递归建目录 → 覆盖打开。
   * ★ 缺席 ⇒ 本层无法知道落点 ⇒ 记一条**诊断**并按 `'unavailable'` 处理（⛔ 不静默当成功）。
   */
  openErrorLog?(spec: ErrorLogPathSpec): Promise<boolean>;
  /**
   * 配置根（**可选**，spec「日志通道的落点与丢弃」）：`set:UseAppDataFolder` **恰为 1** 时才问。
   * ★ 返回**用户本地应用数据目录**（现代系统上是 Local，⛔ 不是 Roaming）；核心只把它当路径的 `root`。
   * ★ 缺席 ⇒ 这一层无法展开"配置根"⇒ 记一条**诊断**并按 `'unavailable'` 处理（⛔ 不静默当成功）。
   */
  appDataDir?(): Promise<string>;
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

export interface BootWindowOptions {
  host: BootHost;
  /** 已建立的配置库（发行默认表 + 玩家文件；`55-config.md`） */
  config: ConfigStore;
  entry: WindowEntry;
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

/**
 * 图形路线（`set:CreateObject` 的**两个独立位**：位 0 = 旧式绘制、位 1 = 三维设备；
 * 键 = 3 时两条都跑，次序 = 旧式在前）。
 */
export type GraphicsRoute = 'legacy-draw' | 'three-dimensional';

/** 图形路线里失败的那一步 */
export type GraphicsStep =
  | 'create-legacy-draw'
  | 'create-d3d9'
  | 'query-adapter-display-mode'
  | 'validate-resolution'
  | 'create-device'
  | 'query-device-caps'
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
  /**
   * **带类别的日志**（spec「失败后果」：错误 / 警告 / 诊断三类前缀）。
   * ★ 这是唯一写入口；下面的 `errors` / `warnings` / `diagnostics` 是它的三个通道视图
   *   （按类别过滤）——⛔ 不要绕过 `logGraphics` 直接往通道里塞自由文本。
   * ★ **记录 ≠ 送达**：本数组只表示"引擎产生了这条文本"；真到磁盘的只有 `logSink === 'file'`
   *   时（落点无效 ⇒ 整条文本被引擎**静默丢弃**，见 `GraphicsLogSink`）。
   */
  readonly graphicsLog: GraphicsLogEntry[] = [];
  /**
   * 日志落点（spec「日志通道的落点与丢弃」）：由 `system:OutErrorLog` + 路径参数（`ErrorLogPathSpec`）
   * + 宿主的 `openErrorLog` 决定；打开时机在**窗口附加之后、图形设备之前**（见 `runStartup`）。
   * ★ 缺省 `'unavailable'`（该键为 0 / 缺键 ⇒ 引擎**不打开**）；⛔ 丢弃不影响任何判定，
   *   也不改变 `errors` / `warnings` / `diagnostics` 的内容（文本仍照记）。
   */
  logSink: GraphicsLogSink = 'unavailable';

  /** 错误通道（错误类文本；⛔ 写了错误文本**不一定**中止，见 `GRAPHICS_STEP_OUTCOME`） */
  get errors(): string[] {
    return this.textsOf('error');
  }

  /**
   * 警告通道（spec：32 位纹理格式不可用**只**写一条警告文本，⛔ 不结束启动）。
   * ★ 错误 / 警告是两个通道：把"只警告"写进 `errors` 会让启动被误判成失败。
   */
  get warnings(): string[] {
    return this.textsOf('warning');
  }

  /**
   * 诊断通道（无前缀：分辨率校验的两段诊断 + 实现侧"宿主缺可选方法"）。
   * ⛔ 不许静默跳过：每一条都点名缺的是哪个方法、因而没能发送哪个 spec 请求。
   */
  get diagnostics(): string[] {
    return this.textsOf('diagnostic');
  }

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
  /**
   * 实际跑过的图形路线（`set:CreateObject` 的两个**独立位**）。
   * ★ 键 = 3 ⇒ `['legacy-draw', 'three-dimensional']`（旧式在前）。
   */
  graphicsRoutes: GraphicsRoute[] = [];
  /** 第一条实际跑的图形路线（兼容字段；未到该阶段 / 两个位都为 0 ⇒ `null`） */
  graphicsRoute: GraphicsRoute | null = null;
  /** 形态字 S（`display:ScreenMode` 经 `display:ForceScreen` 归一后的值） */
  screenMode = 0;
  /**
   * 是否全屏呈现（三维路线在建设备时算出的值；未到该阶段为 `null`）。
   * ★ 与 `screenMode` **不是**同一个量：形态字 S = 0 表示"占满屏形态"，但 `display:VirtualFullScreen`
   *   非 0 时呈现仍是窗口化的（spec「设备创建请求的实参」）。
   */
  fullscreenPresent: boolean | null = null;
  /** 纹理位深（`display:TextureBit`；设备建成之后才读，用于配置上层绘制对象） */
  textureBitDepth = 0;
  /** 共享出口（`null` = 启动继续到了"图形设备就绪"） */
  failure: BootFailure | null = null;

  private periodicTimerActive = false;
  private hoverTimerActive = false;
  /** 占满屏态量到的屏幕尺寸（旧式绘制路线要用客户区尺寸，不再问一次） */
  private screenWidth = 0;
  private screenHeight = 0;
  /**
   * 分辨率校验**写回**的全屏宽 / 高（spec：挑一个最接近的可用分辨率写回请求宽高）。
   * ★ 只有 `display:VirtualFullScreen = 0` 分支会写；窗口化时后缓冲仍取客户区（写回不可见）。
   */
  private resolvedFullScreen: { width: number; height: number } | null = null;
  private readonly timerActivationCondition: (() => boolean) | null;
  private readonly engineTeardown: (() => void) | null;

  constructor(opts: BootWindowOptions) {
    this.host = opts.host;
    this.config = opts.config;
    this.entry = opts.entry;
    this.timerActivationCondition = opts.timerActivationCondition ?? null;
    this.engineTeardown = opts.engineTeardown ?? null;
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
    if (!(await this.registerWindowClass())) return;
    if (!(await this.createWindow())) return;
    await this.attachWindow();
    // ★ 日志文件在**窗口附加之后、图形设备初始化之前**打开一次（spec「时机」；每次启动一次）——
    //   窗口阶段的文本没有落点，所以这一步不能更早；图形阶段每条文本都要先知道落点。
    await this.resolveLogSink();
    if (!(await this.initGraphics())) return;
    // 事件入口规则：启动期在**设备初始化之后、根脚本之前**直接请求过一次空闲抑制。
    this.record(RULE_EVENT_LOOP, 'set-thread-execution-state', {
      flags: EXECUTION_STATE_KEEP_AWAKE,
      reason: 'startup-after-graphics',
    });
    this.host.setThreadExecutionState(EXECUTION_STATE_KEEP_AWAKE);
  }

  /* ── 1. 窗口类注册（CORE-BOOT-WINDOW-CLASS） ─────────────────────────────── */

  private async registerWindowClass(): Promise<boolean> {
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
    const cls = await this.host.registerWindowClass({
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

  private async createWindow(): Promise<boolean> {
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
      params = await this.windowedCreateParams();
    } else {
      params = await this.fullscreenCreateParams();
    }

    this.record(RULE_WINDOW_CREATE, 'create-window', { ...params });
    const window = await this.host.createWindow(params);
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
  private async windowedCreateParams(): Promise<WindowCreateParams> {
    const clientWidth = this.config.getInt('set:WinX');
    const clientHeight = this.config.getInt('set:WinY');
    this.record(RULE_WINDOW_CREATE, 'measure-window-frame', {});
    const frame = await this.host.frameMetrics();
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
  private async fullscreenCreateParams(): Promise<WindowCreateParams> {
    this.record(RULE_WINDOW_CREATE, 'measure-screen', {});
    const screen = await this.host.screenSize();
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
  private async attachWindow(): Promise<void> {
    const window = this.window as WindowHandle;
    // 1. 显式解绑输入法（第二参数 0）；返回值不检查
    this.record(RULE_WINDOW_ATTACH, 'associate-input-context', { context: null });
    this.host.associateInputContext(window, null);
    // 2. 仅窗口态：取该窗口的菜单，交给辅助入口（类别 1），再重画菜单栏。
    //    占满屏态没有菜单 ⇒ 这三步都不做。
    if (this.screenMode !== 0) {
      this.record(RULE_WINDOW_ATTACH, 'get-menu', { window });
      const menu = await this.host.getMenu(window);
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

  /**
   * 日志落点（spec「日志通道的落点与丢弃」）：`system:OutErrorLog` **非 0** ⇒ 请求宿主**按引擎的路径规则**
   * 覆盖打开那个固定的日志文件；该键为 0 / 缺键 ⇒ 引擎**不打开**；打开失败 ⇒ 句柄同样无效。
   * 三种"没有句柄"的情形都收敛到 `'unavailable'`：此后文本仍被记录，但引擎会**静默丢弃**它们
   * （⛔ 不写第二处、不弹框、不影响启动是否继续）。
   *
   * ★ 路径是**引擎规则**（见 `joinEnginePath`）：配置根 [+分隔符] + `set:RegFilePath` [+分隔符] + 固定文件名；
   *   配置根 = `set:UseAppDataFolder` **恰为 1** ⇒ 宿主给的用户本地应用数据目录，其它值 ⇒ 空串。
   *   核心只把**领域字段**（含已解析的配置根）交给宿主，⛔ 不在这里做 IO。
   * ★ 打开时机：**窗口附加之后、图形设备初始化之前**（见 `runStartup`）。
   * ★ 宿主缺 `openErrorLog`（或 `set:UseAppDataFolder` 恰为 1 时缺 `appDataDir`）是**实现侧能力缺口**
   *   （这一层无法执行 spec 的"非 0 ⇒ 打开"）⇒ 各记一条诊断，⛔ 不静默当成功；
   *   缺方法的诊断只在"该打开却没处打开"时发（键为 0 时本来就该不打开）。
   */
  private async resolveLogSink(): Promise<void> {
    if (this.readConfigInt('system:OutErrorLog') === 0) {
      this.logSink = 'unavailable';
      return;
    }
    const openErrorLog = this.host.openErrorLog;
    if (openErrorLog === undefined) {
      this.logGraphics(
        'diagnostic',
        'host-missing-method',
        'graphics-host-missing-method:openErrorLog —— 宿主没有提供可选的 `openErrorLog`，无法执行 spec'
          + '「日志通道的落点与丢弃」里"该键非 0 ⇒ 打开日志文件"那一步；日志落点按 unavailable 处理'
          + '（之后每条文本都会被静默丢弃）',
      );
      this.logSink = 'unavailable';
      return;
    }
    // 配置根：**恰为 1** ⇒ 用户本地应用数据目录；其它值（含 0 / 2 / 缺键）⇒ 空串。
    const useAppDataFolder = this.readConfigInt('set:UseAppDataFolder') === 1;
    let baseDirectory = '';
    if (useAppDataFolder) {
      const appDataDir = this.host.appDataDir;
      if (appDataDir === undefined) {
        this.logGraphics(
          'diagnostic',
          'host-missing-method',
          'graphics-host-missing-method:appDataDir —— 宿主没有提供可选的 `appDataDir`，无法执行 spec'
            + '「日志通道的落点与丢弃」里"`set:UseAppDataFolder` 恰为 1 ⇒ 配置根 = 用户本地应用数据目录"'
            + '那一步；日志落点按 unavailable 处理（之后每条文本都会被静默丢弃）',
        );
        this.logSink = 'unavailable';
        return;
      }
      this.record(RULE_GRAPHICS_INIT, 'app-data-dir', { key: 'set:UseAppDataFolder' });
      baseDirectory = await appDataDir.call(this.host);
    }
    const pathSpec: ErrorLogPathSpec = {
      fileName: ERROR_LOG_FILE_NAME,
      relativeDirectory: this.readConfigString('set:RegFilePath'),
      useAppDataFolder,
      maxPathBytes: ERROR_LOG_MAX_PATH_BYTES,
      baseDirectory,
    };
    this.record(RULE_GRAPHICS_INIT, 'open-error-log', { ...pathSpec });
    const opened = await openErrorLog.call(this.host, pathSpec);
    this.logSink = opened ? 'file' : 'unavailable';
  }

  /**
   * 读一个整数配置键（**带缺键诊断**）。
   * ★ spec「日志通道的落点与丢弃」：读一个不存在的配置键会返回 0，但**先弹一次模态错误框**
   *   （键名 + "不正なキー"）—— 这条对本规则读的所有键都成立。
   * ★ 本层**没有**弹窗类请求（结构上就弹不出来）⇒ 不建模 UI，只记一条诊断 `config-missing-key:<key>`；
   *   ⛔ 不静默、也不伪造一个假的模态框。
   */
  private readConfigInt(key: string): number {
    this.noteMissingConfigKey(key);
    return this.config.getInt(key);
  }

  /** 读一个字符串配置键（缺键诊断同 `readConfigInt`） */
  private readConfigString(key: string): string {
    this.noteMissingConfigKey(key);
    return this.config.getString(key);
  }

  /** 缺键 ⇒ 一条诊断（⛔ 缺键 ≠ 显式写 0：后者是"在的"）。 */
  private noteMissingConfigKey(key: string): void {
    if (this.config.has(key)) return;
    this.logGraphics('diagnostic', 'config-missing-key', `config-missing-key:${key}`);
  }

  /**
   * 图形设备初始化（spec「图形设备初始化」）：`set:CreateObject` 的**两个独立位**。
   * 位 0 = 旧式绘制、位 1 = 三维设备 —— ⛔ 不是 if/else：键 = 3 时两条都跑（旧式在前，失败即停）。
   * 两个位都为 0 ⇒ 两条都不跑（不是"默认跑三维"）。
   */
  private async initGraphics(): Promise<boolean> {
    const createObject = this.config.getInt('set:CreateObject');
    const legacy = (createObject & 1) !== 0;
    const threeDimensional = (createObject & 2) !== 0;
    if (legacy) {
      this.noteGraphicsRoute('legacy-draw');
      if (!(await this.initLegacyDraw())) return false;
    }
    if (threeDimensional) {
      this.noteGraphicsRoute('three-dimensional');
      if (!(await this.initThreeDimensional())) return false;
    }
    return true;
  }

  /** 记一条实际跑过的路线（`graphicsRoute` 兼容字段 = 第一条） */
  private noteGraphicsRoute(route: GraphicsRoute): void {
    this.graphicsRoutes.push(route);
    if (this.graphicsRoute === null) this.graphicsRoute = route;
  }

  /**
   * 旧式绘制路线（一次"建立绘制对象"的请求）。
   * ★ 实参全部按 spec「旧式绘制路线的实参」推导：
   *   行为位 = `display:UseDirectDrawEmuration` 原值 |（形态字 ≠ 0 ⇒ 2）；
   *   `display:FullScreenBit` 是**独立**的第 4 实参（⛔ 不与任何键做或）；
   *   客户区宽 / 高来自引擎级缓存（与三维路线第 3 / 4 实参同一个来源）。
   * ★ 旧式路线**不看** `display:VirtualFullScreen`；`set:AlwaysBackupSurface` 不是实参。
   */
  private async initLegacyDraw(): Promise<boolean> {
    const window = this.window as WindowHandle;
    const args: LegacyDrawParams = {
      window,
      clientWidth: this.clientWidth(),
      clientHeight: this.clientHeight(),
      behaviorBits: legacyBehaviorBitsOf(
        this.config.getInt('display:UseDirectDrawEmuration'),
        this.screenMode,
      ),
      fullscreenBit: this.config.getInt('display:FullScreenBit'),
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

  /** 三维设备路线：建 D3D9 → 取显示模式 →（VFS=0）分辨率校验 → 建设备（重试）→ 设备之后的四步 */
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
    // 3. 分辨率校验（只做 `display:VirtualFullScreen = 0` 分支）：诊断 + 写回请求宽 / 高；失败致命。
    //    ★ 诊断② 的数值是**适配器显示模式的格式字段**（spec），所以把第 2 步查到的 `format` 带进去。
    if (!(await this.validateResolution(d3d, mode.format))) return false;
    // 4. 创建设备：**每个参数都由配置键推导**（spec「设备创建请求的实参」）。
    //    ★ 呈现参数**只在这里构造一次**，重试时只换设备类型 / 顶点处理这两个取值。
    const virtualFullscreen = this.config.getInt('display:VirtualFullScreen');
    // 是否窗口化 = (形态字 S ≠ 0) 或（虚拟全屏非 0）；是否全屏呈现取反。
    const fullscreenPresent = !((this.screenMode !== 0) || (virtualFullscreen !== 0));
    this.fullscreenPresent = fullscreenPresent;
    const multisampleType = this.config.getInt('display:MultiSample');
    const multisampleEnabled = multisampleType !== 0;
    const backBuffer = this.backBufferSize(mode);
    const presentation: Omit<DeviceParams, 'deviceType' | 'vertexProcessing'> = {
      d3d,
      window,
      screenMode: this.screenMode,
      adapterFormat: mode.format,
      backBufferWidth: backBuffer.width,
      backBufferHeight: backBuffer.height,
      presentationInterval: presentationIntervalOf(this.config.getInt('display:PresentInterval')),
      multisample: { type: multisampleType, quality: 0 },
      // 交换效果：窗口化 ⇒ 复制；全屏 ⇒ 丢弃；多重采样启用时强制丢弃。
      swapEffect: multisampleEnabled ? 'discard' : (fullscreenPresent ? 'discard' : 'copy'),
      backBufferLockable: !multisampleEnabled,
      fullscreenPresent,
    };
    // 尝试序列：设备类型外层、顶点处理内层（每个新设备类型都把顶点处理重置为 1）；成功即停。
    const attempts = deviceCreationAttempts(
      this.config.getInt('display:DeviceType'),
      this.config.getInt('display:VertexProcessing'),
    );
    let device: DeviceHandle | null = null;
    let created: DeviceParams | null = null;
    for (const [index, attempt] of attempts.entries()) {
      const params: DeviceParams = { ...presentation, ...attempt };
      this.record(RULE_GRAPHICS_INIT, 'create-device-attempt', {
        attempt: index,
        attempts: attempts.length,
        deviceType: attempt.deviceType,
        vertexProcessing: attempt.vertexProcessing,
      });
      device = await this.host.createDevice(params);
      if (device !== null) {
        created = params;
        break;
      }
    }
    if (device === null || created === null) {
      // 枚举耗尽仍是失败：**复用"取显示模式"的文本**（spec「失败后果」）
      this.failGraphics('create-device');
      return false;
    }
    this.record(RULE_GRAPHICS_INIT, 'create-device', { ...created });
    this.device = device;
    // 5. 设备建成之后：对话框绘制模式 → 设备能力（纹理能力下限是**致命**）→
    //    32 位纹理格式检查（**只警告**）→ 清一次渲染目标。
    if (!(await this.applyPostDeviceRequests(device))) return false;
    // 6. 按 `display:TextureBit` / `display:PixelShader` 建立上层的绘制对象
    //    ★ 这两个键**不是**设备创建请求的参数：设备建成之后才读。
    this.textureBitDepth = this.config.getInt('display:TextureBit');
    const drawParams: DrawObjectParams = {
      device,
      textureWidth: this.config.getInt('set:TexWidth'),
      textureHeight: this.config.getInt('set:TexHeight'),
      textureBitDepth: this.textureBitDepth,
      pixelShader: this.config.getInt('display:PixelShader'),
    };
    this.record(RULE_GRAPHICS_INIT, 'create-draw-object', { ...drawParams });
    const drawObject = await this.host.createDrawObject(drawParams);
    if (drawObject === null) {
      this.failGraphics('create-draw-object');
      return false;
    }
    this.drawObject = drawObject;
    return true;
  }

  /**
   * 分辨率校验（spec「三维设备路线」第 3 步）。
   * - 只在 `display:VirtualFullScreen = 0` 的分支里做（其余分支直接返回成功）；
   * - 请求对：**整对**取 `display:FullScreenWidth` / `FullScreenHeight`（任一非 0 时），否则客户区对；
   * - 枚举适配器可用模式、逐档写诊断（请求 / 清单 / 每档一个符号）、按 `pickResolution` 收口，
   *   选中的宽 / 高**写回**请求宽 / 高（`resolvedFullScreen`）；
   * - 没有可接受的分辨率 ⇒ **不写回**、写一行诊断（`GRAPHICS_RESOLUTION_REJECTED_TEXT`）、结束启动。
   * ★ 判据是已证实的引擎行为（`RES-BOOT-GFX-RESOLUTION-PICK` 已关闭）⇒ 恒按 spec「校验分辨率」跑，
   *   ⛔ 不再由调用方注入挑选规则。
   * @param adapterFormat 第 2 步查到的适配器显示模式的**格式字段**（诊断② 印的那个数；⛔ 不是宽）
   * @returns `false` = 没有可接受的分辨率（已写诊断并结束启动）
   */
  private async validateResolution(d3d: D3d9Handle, adapterFormat: number | undefined): Promise<boolean> {
    if (this.config.getInt('display:VirtualFullScreen') !== 0) return true;
    const request = this.requestedFullScreenPair();
    const enumerate = this.host.enumerateAdapterDisplayModes;
    if (enumerate === undefined) return true; // 宿主能力缺口：`declareIgnored` 已记账
    this.record(RULE_GRAPHICS_INIT, 'enumerate-adapter-display-modes', { d3d, adapter: ADAPTER_INDEX });
    const modes = await enumerate.call(this.host, { d3d, adapter: ADAPTER_INDEX });
    this.logGraphics(
      'diagnostic',
      'validate-resolution',
      `requested-resolution:${request.width}x${request.height}`,
    );
    // 诊断②：本机可用分辨率**一览** —— 那一行的数值是**显示模式的格式字段**（spec「诊断的形态」②：
    // 颜色模式 / 格式号），⛔ 不是宽。宿主没报格式 ⇒ 印 `format=?`（不编一个数）。
    this.logGraphics(
      'diagnostic',
      'validate-resolution',
      `available-display-modes:format=${typeof adapterFormat === 'number' ? adapterFormat : '?'};`
        + (modes.map((m) => `${m.width}x${m.height}`).join(',') || '(none)'),
    );
    const verdict = pickResolution(modes, request.width, request.height);
    // 逐档一行（符号集合是七元闭集；规范只钉对应关系，⛔ 不钉字节与排版）
    for (const entry of verdict.perMode) {
      this.logGraphics(
        'diagnostic',
        'validate-resolution',
        `mode-verdict:${entry.width}x${entry.height}:${entry.symbol}`,
      );
    }
    if (verdict.picked === null) {
      // 失败：请求宽 / 高**保持原值**（resolvedFullScreen 不写）；诊断 + 结束启动
      this.failGraphics('validate-resolution', GRAPHICS_RESOLUTION_REJECTED_TEXT);
      return false;
    }
    this.resolvedFullScreen = { width: verdict.picked.width, height: verdict.picked.height };
    this.logGraphics(
      'diagnostic',
      'validate-resolution',
      `picked-resolution:${verdict.picked.width}x${verdict.picked.height}`,
    );
    return true;
  }

  /**
   * 分辨率校验的**请求对**（spec「校验分辨率」）：`display:FullScreenWidth` **或**
   * `display:FullScreenHeight` **任一非 0** ⇒ 整对取这两个键的值（允许其中一个为 0）；
   * 两者都为 0 ⇒ 客户区宽 / 高。
   * ★ **整对覆盖，⛔ 不做逐分量回退**（把 `0` 换回客户区是错的）。
   */
  private requestedFullScreenPair(): { width: number; height: number } {
    const width = this.config.getInt('display:FullScreenWidth');
    const height = this.config.getInt('display:FullScreenHeight');
    if (width !== 0 || height !== 0) return { width, height };
    return { width: this.clientWidth(), height: this.clientHeight() };
  }

  /**
   * 后缓冲尺寸（spec「后缓冲尺寸」：三条互斥，按顺序取第一条成立的）。
   * ★ 这里的"窗口化"是**形态字** S ≠ 0（建窗时选的两种形态之一）——"是否全屏呈现"是另一个量
   *   （`fullscreenPresent`）；否则第 2 条不可达（它要求 S = 0 且虚拟全屏非 0）。
   * ★ 第 3 档取"分辨率校验写回的值"（若校验跑过），否则取 `display:FullScreenWidth` / `Height`。
   */
  private backBufferSize(desktop: DisplayMode): { width: number; height: number } {
    // 1. 窗口化 ⇒ 客户区宽 / 高（引擎级缓存：窗口态来自 set:WinX / set:WinY）
    if (this.screenMode !== 0) return { width: this.clientWidth(), height: this.clientHeight() };
    // 2. 全屏 + 虚拟全屏非 0 + 虚拟全屏类型 = 1 ⇒ 桌面宽 / 高（第 2 步查到的显示模式）
    const virtualFullscreen = this.config.getInt('display:VirtualFullScreen');
    if (virtualFullscreen !== 0 && this.config.getInt('display:VirtualFullScreenType') === 1) {
      return { width: desktop.width, height: desktop.height };
    }
    // 3. 其余全屏 ⇒ 分辨率校验写回值 / display:FullScreenWidth / Height（取 0 ⇒ 客户区宽 / 高）
    const configuredWidth = this.resolvedFullScreen?.width ?? this.config.getInt('display:FullScreenWidth');
    const configuredHeight = this.resolvedFullScreen?.height ?? this.config.getInt('display:FullScreenHeight');
    return {
      width: configuredWidth !== 0 ? configuredWidth : this.clientWidth(),
      height: configuredHeight !== 0 ? configuredHeight : this.clientHeight(),
    };
  }

  /** 客户区宽（窗口态 = `set:WinX`；占满屏态 = 建窗时量到的屏幕宽） */
  private clientWidth(): number {
    return this.screenMode !== 0 ? this.config.getInt('set:WinX') : this.screenWidth;
  }

  /** 客户区高（同 `clientWidth`） */
  private clientHeight(): number {
    return this.screenMode !== 0 ? this.config.getInt('set:WinY') : this.screenHeight;
  }

  /**
   * 设备建成之后的四个请求（spec「三维设备路线」第 4 步，**次序是合同**）。
   * ★ 四个都是**可选**宿主方法：缺席时记一条诊断（⛔ 不静默）；
   *   其中"设备能力查询"缺席意味着**无法**执行 spec 的纹理能力下限致命比较 —— 这必须看得见。
   * ★ 后果按 spec「失败后果」的逐步骤表：
   *   对话框绘制模式失败 ⇒ **错误**类文本但**不**中止；
   *   32 位纹理格式不可用 ⇒ **警告**类文本、不中止；
   *   纹理能力不足 ⇒ 错误类 + **结束启动**。
   * @returns `false` = 纹理能力不足（已写错误通道并结束启动）
   */
  private async applyPostDeviceRequests(device: DeviceHandle): Promise<boolean> {
    // (1) 对话框绘制模式（参数 = 开）。★ 失败**写错误类文本但不中止**（spec 唯一一处）。
    const setDialogBoxMode = this.host.setDialogBoxMode;
    if (setDialogBoxMode === undefined) {
      this.noteMissingHostMethod('dialog-box-mode', 'setDialogBoxMode', '对话框绘制模式');
    } else {
      const params = { device, enabled: true };
      this.record(RULE_GRAPHICS_INIT, 'set-dialog-box-mode', { ...params });
      const ok = await setDialogBoxMode.call(this.host, params);
      if (ok === false) {
        // ⛔ 不调用 failGraphics：写了错误文本 ≠ 结束启动（按逐步骤表的 fatal 列决定）
        this.logGraphics('error', 'set-dialog-box-mode', GRAPHICS_DIALOG_BOX_MODE_ERROR_TEXT);
      }
    }
    // (2) 设备能力（最大纹理宽 / 高）并与 set:TexWidth / set:TexHeight 比较：不足 ⇒ 失败
    const queryDeviceCaps = this.host.queryDeviceCaps;
    if (queryDeviceCaps === undefined) {
      this.noteMissingHostMethod('device-caps-query', 'queryDeviceCaps', '设备能力查询（纹理能力下限比较）');
    } else {
      this.record(RULE_GRAPHICS_INIT, 'query-device-caps', { device });
      const caps = await queryDeviceCaps.call(this.host, { device });
      const textureWidth = this.config.getInt('set:TexWidth');
      const textureHeight = this.config.getInt('set:TexHeight');
      if (caps.maxTextureWidth < textureWidth || caps.maxTextureHeight < textureHeight) {
        // 能力不足是**致命**的（spec「失败后果」）：写错误通道 + 结束启动
        this.failGraphics('query-device-caps');
        return false;
      }
    }
    // (3) 32 位纹理格式可用性检查：不可用**只写一条警告**，不结束启动
    const checkTextureFormat32 = this.host.checkTextureFormat32;
    if (checkTextureFormat32 === undefined) {
      this.noteMissingHostMethod('texture-format-32-check', 'checkTextureFormat32', '32 位纹理格式可用性检查');
    } else {
      this.record(RULE_GRAPHICS_INIT, 'check-texture-format-32', { device });
      if (!(await checkTextureFormat32.call(this.host, { device }))) {
        this.logGraphics('warning', 'check-texture-format-32', TEXTURE_FORMAT_32_WARNING_TEXT);
      }
    }
    // (4) 清一次渲染目标
    const clearRenderTarget = this.host.clearRenderTarget;
    if (clearRenderTarget === undefined) {
      this.noteMissingHostMethod('clear-render-target', 'clearRenderTarget', '清渲染目标');
    } else {
      this.record(RULE_GRAPHICS_INIT, 'clear-render-target', { device });
      await clearRenderTarget.call(this.host, { device });
    }
    return true;
  }

  /**
   * 可选宿主方法缺席 ⇒ 记一条**显式诊断**。
   * ⛔ 不许静默：spec 要求的请求没发出去，必须在会话里留下可查的痕迹。
   */
  private noteMissingHostMethod(what: string, method: string, purpose: string): void {
    this.logGraphics(
      'diagnostic',
      'host-missing-method',
      `graphics-host-missing-method:${method} —— 宿主没有提供可选的 \`${method}\`，`
        + `无法发送 spec 第 4 步的「${purpose}」请求（${what}）`,
    );
  }

  /** 写一条**带类别**的日志（三条通道都是它的视图；⛔ 不要绕过它直接 push） */
  private logGraphics(category: GraphicsTextCategory, step: GraphicsLogStep, text: string): void {
    this.graphicsLog.push({ category, step, text });
  }

  /** 某一类别的文本（通道视图） */
  private textsOf(category: GraphicsTextCategory): string[] {
    return this.graphicsLog.filter((entry) => entry.category === category).map((entry) => entry.text);
  }

  /**
   * 图形路线失败：按 spec「失败后果」的逐步骤表写日志（该步不写日志就只给用户可见文本），并结束启动。
   * ★ ⛔ 不要用"写了错误文本"推断是否中止 —— 是否中止由 `GRAPHICS_STEP_OUTCOME.fatal` / 调用点决定。
   */
  private failGraphics(step: GraphicsStep, text = GRAPHICS_INIT_ERROR_TEXT): void {
    const outcome = GRAPHICS_STEP_OUTCOME[step];
    if (outcome.log !== null) this.logGraphics(outcome.log, step, text);
    this.fail(RULE_GRAPHICS_INIT, 'graphics-init-failed', step, text);
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
      'device-type-not-distinguishable',
      '当前后端（Pixi/WebGL）不区分"硬件设备 / 参考设备 / 硬件·软件·混合顶点处理" ⇒ 核心按 spec 的序列'
        + '真的重试（每失败一次就发下一次），但后端的这六次是**同一个请求**；核心侧序列可被守卫钉住，'
        + '后端侧的区分能力不存在，⛔ 不假装。',
    );
    this.ignore(
      RULE_GRAPHICS_INIT,
      'fullscreen-present-combination',
      '三维路线的"是否全屏呈现"已由 spec 钉住（形态字 S 与虚拟全屏键的或）；**旧式绘制路线**下的组合仍未取证'
        + '（RES-BOOT-GFX-FULLSCREEN-COMBO）⇒ 本层只在三维路线计算，旧式路线不消费该组合',
      'RES-BOOT-GFX-FULLSCREEN-COMBO',
    );
    if (this.host.enumerateAdapterDisplayModes === undefined) {
      this.ignore(
        RULE_GRAPHICS_INIT,
        'resolution-enumeration',
        '宿主没有提供 `enumerateAdapterDisplayModes` ⇒ 无法执行 spec 第 3 步的分辨率枚举 / 诊断 / 写回；'
          + '这是实现侧能力缺口（不是"引擎不做"），⛔ 不静默',
      );
    }
    this.ignore(
      RULE_GRAPHICS_INIT,
      'error-text',
      '错误文本的类别与内容未取证（RES-BOOT-GFX-TEXT）⇒ 本层写实现侧的内建常量（内容不是规范结论）；'
        + '32 位纹理格式不可用的警告文本同样只是实现侧占位',
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
 *       → 图形设备（`set:CreateObject` 两个独立位选路线）→ 启动期直接请求一次空闲抑制。
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
