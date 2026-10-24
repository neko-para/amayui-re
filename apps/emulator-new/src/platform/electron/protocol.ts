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

/**
 * 绘制项的**本步变换**（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 7 条交给绘制入口的那份矩阵）。
 * ★ 列主序 4×4（与核心的矩阵口径一致）；`valid = false` ⇒ 绘制入口**不应用**它。
 */
export interface DrawItemTransformSnapshot {
  matrix: number[];
  valid: boolean;
}

/**
 * **附加组**视图（条目首字第 2 位 = 1 时，消费者按它派生主绘制的输入 —— spec 第 6 条）。
 *
 * ★ 字段名是 **spec 的语义名**，不是机器字段名。消费者侧**读到**的那几格有证据
 *   （`.tmp/spec-draft/batch4/texconsumer/evidence.md` §3.2 / §3.3），但 **"哪一格叫什么"仍是残差**：
 *   `- 附加组字段（+0x20C / +0x21C / +0x220 / +0x230 / +0x238 / +0x23C / +0x240）的领域命名未取证`。
 * ★ ⇒ 没有取值的字段一律用 `null` / 0 并**在注释里写明"未取证"**：⛔ 不编常数、不编公式。
 */
export interface DrawItemGroupSnapshot {
  /** 颜色通道：起点戳（毫秒；0 = 未盖章 ⇒ 引擎在首次派生时盖章） */
  colorStart: number;
  /** 颜色通道：周期（毫秒；≤ 0 ⇒ 不派生色） */
  colorPeriod: number;
  /** 附加组**终色**（ARGB）；`null` = 这一格没有取值（本实现没有它的写入方）⇒ 不混色 */
  colorEnd: number | null;
  /** 源矩形通道：起点戳（毫秒） */
  rectStart: number;
  /** 源矩形通道：周期（毫秒；≤ 0 ⇒ 不判跨步） */
  rectPeriod: number;
  /**
   * 源矩形的**步进量**（左右同加 `stepX`、上下同加 `stepY`）。
   * ★ 步进量的**公式**属残差 `RES-TEX-DRAW-CONSUMER-RECTSTEP` ⇒ 本实现没有它的模型 ⇒ 恒 0。
   */
  stepX: number;
  stepY: number;
  /** 上一帧记下的**步号**（跨步 ⇒ 场景标脏）；`null` = 没有记录（本实现没有每帧记录步号的状态） */
  lastStepIndex: number | null;
  /** 附加组的**变换通道**（列主序 4×4）；`null` = 未取证是哪一张矩阵 ⇒ 不乘 */
  transform: number[] | null;
}

/**
 * **源矩形窗口**的五格（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 4.1；由 `0x239` 排上）。
 * ★ 窗起点**不在这里**：它与颜色窗**共用** {@link DrawItemSnapshot.start}（0 = 未盖章）。
 */
export interface DrawItemSourceWindowSnapshot {
  /** 延迟（毫秒） */
  delay: number;
  /** 持续时间（毫秒）；≤ 0 ⇒ 4.1 整段不运行 */
  duration: number;
  /** 步进总数 */
  steps: number;
  /** 每行步数（列数） */
  perRow: number;
  /** 开关字：位 1 = 延迟内原样、位 0 = 到期末帧（否则塌空） */
  switchWord: number;
}

/** 一条绘制项的可序列化快照（`0x1fb` / `0x202` / `0x203`） */
export interface DrawItemSnapshot {
  key: number;
  /** 位 0 = 有效；位 1 = 有窗（颜色窗 / 源矩形窗共用）；位 2 = 附加组；位 16（`0x10000`）= 本轮已消费（命令档写；本实现还没有写入方） */
  flags: number;
  textureId: number;
  /** `0x1fb` 的第 3..6 个操作数算出的四条边（角色本版未定；平台按实现使用） */
  left: number;
  top: number;
  right: number;
  bottom: number;
  /** 这一步用于绘制的颜色（派生色，ARGB；-1 = 无颜色 ⇒ 消费侧按"不透明白"处理） */
  color: number;
  /** 两个辅助浮点（`0x1fb` 的第 7 / 8 个操作数；角色本版未定） */
  aux: [number, number, number];
  /**
   * **窗起点**（毫秒；0 = 未盖章）。颜色窗 / 变换窗 / 源矩形窗**共用**这一个起点
   * （`0x202` / `0x21e` / `0x239` 族都把它清 0）⇒ 消费侧的 4.1 也读它。
   */
  start: number;
  /**
   * 本步变换；`undefined` / `null` = 本实现**还没有**它的模型（`sub_49AA30` 的矩阵派生未建模）
   * ⇒ 消费侧的规划器用单位阵并记一条缺口（⛔ 不代表引擎的初值）。
   */
  transform?: DrawItemTransformSnapshot | null;
  /** 附加组视图；`undefined` / `null` = 首字第 2 位没置 / 没有字段（见 {@link DrawItemGroupSnapshot}） */
  group?: DrawItemGroupSnapshot | null;
  /**
   * **源矩形窗口**（4.1）；`null` / `undefined` = 没排过（那五格是默认 0 ⇒ 持续时间 ≤ 0 ⇒ 整段不运行）。
   * ★ 只有"有窗"位（首字第 1 位）为 1 时消费侧才会走到这一段。
   */
  sourceWindow?: DrawItemSourceWindowSnapshot | null;
}

/**
 * 场景对象（相机）给"绘制项的消费者"的档位输入（`CORE-OPS-TEXTURE-DRAW-CONSUMER` 第 5.1 条）。
 * ★ 由 `CORE-SCENE-DISPLAY-FIT` 算出来的那几格填（`renderer/display-fit.ts` 的产出）；
 *   领域名多处未定（显示模式的 0/1/2、保留编号 38 / 39）⇒ 这里只按**数值**记。
 */
export interface SceneViewSnapshot {
  /**
   * **兼容字段**（保留给旧调用方）：`true` ⇒ 缩放按 1、偏移按 0。
   * ★ 规则落地后它的语义由 {@link fixedResolution} 接管（会话侧恒传 `false`）；
   *   ⛔ 不要再用它表达"本实现没有模型"—— 没有模型时整个 `view` 是 `null`。
   */
  noCamera: boolean;
  /** 场景缩放（浮点；`cam+0x4E0/0x4E4`）—— **公式算出的原值**（固定分辨率模式不在这里短路） */
  scaleX: number;
  scaleY: number;
  /** 场景级偏移的两个**整数**（`cam+0x490/0x494`）= 视口原点 —— 同样是原值 */
  offsetX: number;
  offsetY: number;
  /** 显示模式（`cam+0x48C` 的值 = 虚拟全屏类型；领域身份未定） */
  displayMode: number;
  /**
   * **固定分辨率模式**（`display:ScreenMode != 0`）：它**不进适配公式**，只在取值器里把
   * 缩放短路成 1、原点短路成 0；逆缩放矩阵因此退化成单位阵，但**仍然乘一次**。
   */
  fixedResolution: boolean;
  /**
   * 场景的**屏幕形态副本** `F`（窗口态 1 / 占满屏态 0）：
   * `F != 0` ⇒ 两张适配矩阵都不乘（位置也不按缩放折回）。缺省 = 0（占满屏态）。
   */
  screenForm: number;
  /** 场景视图矩阵（列主序 4×4）；`null` = 本实现没有它的模型（⛔ 不用假矩阵顶上） */
  viewMatrix: number[] | null;
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

/**
 * **场景自身记录**里被投影出来的那几格（`CORE-OPS-SCENE-SELF-TRANSFORM-229`；`0x229` 写）。
 * ★ 消费者（6.1）只用 {@link matrix}；三元平移留给诊断 —— 它**已经折进**那份派生矩阵，
 *   ⛔ 不要在消费者里再按平移算一次（会重复平移）。
 */
export interface SceneSelfRecordSnapshot {
  /** 三元平移（记录 +0x18 / +0x1C / +0x20；浮点） */
  translate: { x: number; y: number; z: number };
  /** 作用键区间起点（+0x458）；领域：额外变换的作用范围下界 */
  keyStart: number;
  /** 作用键区间计数（+0x45C）；0 ⇒ 不派生额外变换 */
  keyCount: number;
  /**
   * 由场景自身记录派生的**额外变换**（列主序 4×4）。
   * `null` = 本实现**没有**它的派生模型（`sub_49AA30` 未建模）⇒ 规划器用单位阵并记缺口，
   * ⛔ 不编一个矩阵。
   */
  matrix: number[] | null;
}

/**
 * 一段**绘制键区间**（起点键 / 个数）。范围 `[start, start + count)`。
 * ★ 起点与个数都是记录里的两格；⛔ 不把两段合成一段。
 */
export interface SceneCommandRangeSnapshot {
  start: number;
  count: number;
}

/**
 * 一条**场景命令记录**的可序列化快照（`CORE-OPS-SCENE-COMMAND-RECORDS`；命令表的排序键在快照里）。
 *
 * ★ 只投影**具名格**：`SceneCommandRecord.raw` 是读操作数留痕，记录自己的注释已写明它**不是**
 *   命令档的输入 ⇒ ⛔ 不进快照。
 * ★ 区间 A / B：种类 0 / 2 用两段（区间 A 只喂第一块画布、区间 B 只喂第二块）；
 *   种类 1 / 3 只有唯一区间 ⇒ `rangeA` = 它，`rangeB` = **显式空** `{ start: 0, count: 0 }`
 *   （⛔ 不是"没这一格"）。
 * ★ 其余未取证 / 未建模的格（种类 3 的插值对、种类 1 的目标矩形与标志）**不在本快照**：
 *   命令档 S0/S1 只需要执行结构；要用它们时再投影，⛔ 不要先编默认值塞进来。
 */
export interface SceneCommandSnapshot {
  /** 记录首格 = 命令种类：0 = `0x223`、1 = `0x24d`、2 = `0x24f`、3 = `0x250` / `0x251` */
  kind: number;
  /** **命令键** = 命令表的排序键（遍历序是它升序，⛔ 不是插入次序） */
  commandKey: number;
  /** 时间戳格：0 = **未盖章** ⇒ 命令档首次遍历到它时盖成**本帧场景时刻** */
  timestamp: number;
  /** 延迟（毫秒） */
  delay: number;
  /** 持续时间（毫秒；种类 2 的分割幅 < 1 错误支与中止路径都把它清 0） */
  duration: number;
  /** 本条自己的纹理编号（种类 1 = 影片纹理；种类 0 / 2 / 3 = 命令自己的纹理） */
  textureId: number;
  /** 区间 A（种类 0 / 2 = 第一段；种类 1 / 3 = 唯一区间） */
  rangeA: SceneCommandRangeSnapshot;
  /** 区间 B（种类 0 / 2 = 第二段；种类 1 / 3 = 显式空 `{ start: 0, count: 0 }`） */
  rangeB: SceneCommandRangeSnapshot;
  /** 种类 2：**类型 / 模式**格（合法域 = 闭区间 [0, 11]；分割幅 < 1 的错误支记 −1）；其它种类 = 默认 0 */
  mode: number;
  /** 种类 2：**分割幅**（"图案被分成几道"；错误支记 1）；其它种类 = 默认 0 */
  split: number;
  /** **命令键在记录里的那份副本**（种类 2 用它去绘制项表找同键绘制项 ⇒ 抹除矩形） */
  ownerKey: number;
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
  /**
   * 场景时钟（毫秒）。★ 附加组派生用它**自己的**周期（起点戳 + 周期），⛔ 不与主计时窗合并。
   */
  clock: number;
  /**
   * 场景对象（相机）的档位输入；`null` = **没有显示适配上下文**（还没建窗口 / 图形 /
   * 会话没拿到配置）⇒ 绘制项的消费者按"无相机 / 无缩放"档算（缩放 1、偏移 0）并记一条缺口。
   * ★ 正常路径（`onBeforeScript` 之后）恒非 `null`：由 `renderer/display-fit.ts` 的 `computeDisplayFit` 填。
   */
  view: SceneViewSnapshot | null;
  /**
   * 场景自身记录（`0x229` 写；消费者 6.1 用它派生额外变换）。
   * ★ 记录一定存在（场景构造时填默认值）⇒ 不是 `null`；但它的 `matrix` 可以是 `null`（派生未建模）。
   */
  selfRecord: SceneSelfRecordSnapshot;
  /**
   * **场景命令表**的快照（按命令键升序）。空数组 = 命令表为空 ⇒ 命令档无动作。
   * ★ 命令记录**不跨"动画结束"存活**（渲染步收尾按「有动画」整表清空，见
   *   `CORE-SCENE-DRAW-ORDER` 的「命令表的生命周期」）⇒ 这张表随时可能是空的。
   */
  commands: SceneCommandSnapshot[];
  /**
   * 本帧「有动画」场景格（`SceneState.animating`）。命令档的**整档收尾**按它决定是否把场景标脏，
   * 渲染步收尾按它决定是否**整表清空**。
   */
  animating: number;
  /**
   * 本帧「强制完成」档。★ 本实现的核心**没有**这一格（写者是场景推进 / 种类 2 的类型 < 0，
   * 都没有建模）⇒ 会话侧恒投影 0；⛔ 这个 0 **不代表**引擎初值，只是"没有模型"。
   */
  forcedComplete: number;
  /**
   * **着色器能力档**：0 / 1 / 2（2 = 支持 PS 2.0），决定「区间之下」那一趟是否发生。
   * ★ `null` = **未建模**：`display:PixelShader` 是版本号（3 等），版本 → 档的映射未取证
   *   ⇒ 规划器按"≥ 2"跑并记 `scene-command-tier-unmodeled`（⛔ 不把 null 当 2 这个结论）。
   */
  shaderTier: number | null;
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
  | 'host.listDir'
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
