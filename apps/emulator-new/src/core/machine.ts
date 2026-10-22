/**
 * 执行器：取指 → 选处理器 → 执行 → 按操作数个数槽推进。
 * ★ 规则只来自 `data/spec`：
 *   - 切分与推进：`chapters/10-format.md` 的「切分与操作数个数」「opcode 域与缺省处理器」「执行如何结束」；
 *   - 操作数：`chapters/40-pools.md` 的五种访问原语；
 *   - 单条指令：`chapters/60-ops-*.md`。
 * ★ 没有实现的 opcode **不执行**、不 no-op：报缺口并停在它前面（缺口是 spec 的机器清单）。
 * ★ 本文件不碰文件系统、不碰 Node：脚本是 `Uint8Array`，取件由注入的 `loadScript` 回调完成。
 * ★ 需要 IO 的部分（`0x3` / `0x6` / `0x9` 的换脚本）是 `await` 的：`run` 返回 `Promise`，
 *   处理器返回 `void | Promise<void>` —— 核心因此能整份跑在浏览器 / worker 里。
 */
import { readU32LE } from './bytes.ts';
import type { OpcodeRow } from './spec.ts';
import type { ConfigStore } from './config.ts';
import { generateIntKey } from './values.ts';
import { HANDLERS } from './handlers.ts';
import { initialRenderState } from './render.ts';
import type { RenderHost, RenderState, TextureKind } from './render.ts';
import type { MovieHandle, MovieHost } from './movie.ts';

export interface Operand {
  type: number;
  raw: number;
}

export interface Frame {
  /** 局部整型池：编码后的位模式（初值 = ENC(0)） */
  localInt: Map<number, number>;
  /** 局部浮点池：裸 32 位位模式（规范不保证初值） */
  localFloat: Map<number, number>;
  /** 局部指针池（type 12）：槽里装**地址**，不编解码 */
  localPtr: Map<number, number>;
  /** 局部浮点指针池（type 13）：槽里装**地址** */
  localFloatPtr: Map<number, number>;
  /** 局部字符串指针池（type 14）：槽里装**地址** */
  localStringPtr: Map<number, number>;
  /**
   * 本帧的**返回栈**（位置 = 同脚本内 dword 序号；见 CORE-FRAME-CALL / CORE-FRAME-RET）。
   * ★ 每帧一份：换帧就换成另一条栈（引擎给每帧各留一段固定容量，容量属机器细节）。
   */
  retStack: number[];
  /** 帧记录里的"本条指令的位置"（帧+0x6C；写入者是 0x71，读者未定） */
  position: number;
  /** 0x7b 登记的两个本地块进入目标（位置，不是字节地址）。 */
  localMain?: number;
  localSub?: number;
  /** 进入本地块时记下的深度（0x7c 的深度判据）。 */
  localDepth?: number;
}

/** 纹理记录：来源文件（-1 = 无）、颜色键、空白创建标记（见 CORE-OPS-TEXTURE-*） */
export interface TextureRecord {
  file: number;
  color: number;
  blank: number;
}

/** 纹理槽里的对象：种类 / 逻辑尺寸 / 外部表面（像素由平台持有） */
export interface TextureObject {
  kind: TextureKind;
  width: number;
  height: number;
}

/** 绘制项（见 CORE-OPS-TEXTURE-DRAW / CORE-OPS-SCENE-*） */
export interface DrawItem {
  /** 位 0 = 有效；位 1 = 有颜色渐变窗 */
  flags: number;
  textureId: number;
  left: number;
  top: number;
  right: number;
  bottom: number;
  /** 第 7/8 个操作数转成的浮点，以及第三个位（恒 0） */
  aux: [number, number, number];
  /** 颜色渐变窗（毫秒）：起点 0 = 未盖章 */
  start: number;
  delay: number;
  duration: number;
  colorFrom: number;
  /** 这一步用于绘制的颜色（派生色）；窗进行中由插值写入，⛔ 不写回 colorFrom */
  colorNow: number;
  colorTo: number;
  /** 操作数里"读者未找到"的槽（按操作数序号记，只为不丢数据） */
  unknown: Map<number, number>;
  /** 0x217 / 0x219 的两组三元向量；默认 0。 */
  vec1: [number, number, number];
  vec2: [number, number, number];
  /** 六个 4×4 矩阵（列主序，与 D3D 一致）：0 缩放、2 旋转、4 平移、5 第四个平移。 */
  matrices: number[][];
  /** 0x19c / 0x220 的「已配置」格（G3）；0x220 还写 G0/G1/G2。 */
  config: number;
  g0: number;
  g1: number;
  g2: number;
  /** 场景 setter 的按字段偏移写入的整数 / 浮点格（偏移由 bindings 提供，正文不写）。 */
  fields: Map<number, number>;
  /** 场景 setter 的按字段偏移写入的矩阵（与 matrices[6] 分开，避免与 1fd/1fe/1ff/220 混用）。 */
  extraMatrices: Map<number, number[]>;
}

/** 缓冲地址（0x320 的第 4 / 7 / 8 个操作数；只记录，装配属外部模块） */
export interface BufferAddress {
  pool: 'globalInt' | 'localInt' | 'globalFloat' | 'localFloat';
  index: number;
}

/** 网格（见 CORE-OPS-SCENE-320/322/323）：三个缓冲在本层只保留可观察的部分 */
export interface Mesh {
  /** 位 0 = 已存在；位 1 = 有颜色渐变窗 */
  flags: number;
  count: number;
  op10: number;
  /** 前两个逐顶点缓冲：第 2 / 3 个操作数地址处 n 个 32 位字的原样拷贝 */
  copies: [number[], number[]];
  /** 第三个逐顶点缓冲（逐顶点颜色） */
  colors: number[];
  /** 交给顶点缓冲对象装配的三个数组地址（第 4 / 7 / 8 个操作数；内容属外部模块） */
  vbo: BufferAddress[];
  start: number;
  delay: number;
  duration: number;
  colorFrom: number;
  /** 这一步用于绘制的颜色（派生色）；窗进行中由插值写入，⛔ 不写回 colorFrom */
  colorNow: number;
  colorTo: number;
  unknown: Map<number, number>;
}

/**
 * 语音组（见 CORE-OPS-VOICE-STOP）：`0x2f6` 按组号重置的那些字段。
 * ★ 引擎里这些是**每组 4 字节步长的数组**（`0x2f6` 只清其中 8 格：语音对象内 6 格 + 引擎级 2 格 +
 *   另两格在语音对象里但由别的写入点维护）。字段的**领域用途多数未定** ⇒ 不命名、不解释；
 *   唯一已定的是 `subsystem[3]` 的低 2 位 = 重算「是否还有语音在播」时的**跳过判据**（等于 3 的组被跳过）。
 */
export interface VoiceGroup {
  /** 语音对象里的 4 个字段 */
  subsystem: [number, number, number, number];
  /** 引擎级的 2 个字段 + 语音对象里的 2 个字段（`0x2f6` 清的那 4 格） */
  engine: [number, number, number, number];
}

/**
 * UI 列表项记录（见 CORE-OPS-LIST-SET / LIST-SUBSYSTEM-SET73）：
 * `0x70` / `0x79` / `0x1c1` / `0x303` 与 `0x73` 按组号写入的字段。
 * ★ 字段的领域用途未定（布局层只有"记录 + 偏移"）；这里只按机器身份存值，不命名、不解释。
 * ★ 引擎在子系统构造时预建组 0..9 的 10 条记录（`LIST_GROUP_COUNT`），`ListRecord` 的零值
 *   是"预建记录"的初值，不是"缺记录时新建"的兜底。
 */
export interface ListRecord {
  /** 记录 +0x0C */
  f0c: number;
  /** 记录 +0x10 */
  f10: number;
  /** 记录 +0x14 */
  f14: number;
  /** 记录 +0x18 */
  f18: number;
  /** 记录 +0x1C */
  f1c: number;
  /** 记录 +0x20 */
  f20: number;
  /** 记录 +0x24 */
  f24: number;
  /** 记录 +0x28 */
  f28: number;
  /** 记录 +0x3C..+0x60（`0x73` 写的连续十格） */
  f3c: number;
  f40: number;
  f44: number;
  f48: number;
  f4c: number;
  f50: number;
  f54: number;
  f58: number;
  f5c: number;
  f60: number;
  /** 记录 +0x7C */
  f7c: number;
  /** 记录 +0x80 */
  f80: number;
  /** 记录 +0x64 */
  f64: number;
  /** 记录 +0x68 / +0x6C */
  f68: number;
  f6c: number;
  /** 记录 +0x114 / +0x118 */
  f114: number;
  f118: number;
  /** 记录 +0x120 / +0x124 */
  f120: number;
  f124: number;
  /** 该组是否被登记进列表（`0x70` 置 1） */
  registered: number;
}

/** UI 列表组域：引擎在子系统构造时预建 10 条记录（组 0..9） */
export const LIST_GROUP_COUNT = 10;

/** 一条全 0 的列表记录（构造期预建；字段身份见 `ListRecord`） */
export function emptyListRecord(): ListRecord {
  return {
    f0c: 0, f10: 0, f14: 0, f18: 0, f1c: 0, f20: 0, f24: 0, f28: 0,
    f3c: 0, f40: 0, f44: 0, f48: 0, f4c: 0, f50: 0, f54: 0, f58: 0, f5c: 0, f60: 0,
    f64: 0, f68: 0, f6c: 0, f114: 0, f118: 0, f120: 0, f124: 0, f7c: 0, f80: 0, registered: 0,
  };
}

/** 影片对象（见 CORE-OPS-MOVIE）：只保留可观察的引擎数据 */
export interface MovieObject {
  kind: 'movie';
  slot: number;
  resourceId: number;
  opt: number;
  started: boolean;
  /** 平台侧句柄；不在核心解释其形状 */
  handle: MovieHandle | null;
  /** 选项字第 0 位：播完回到开头，不销毁 */
  loop: boolean;
  /** 最近一次后端报告的当前位置 / 停止位置（秒） */
  currentTime: number;
  stopTime: number;
}

/** 诊断：实现必须记账的"不可解释之处"（⛔ 不许静默补值） */
export interface Diagnostic {
  kind: string;
  index?: number;
}

/** 场景：纹理 / 绘制项 / 计时窗（见 60-ops-texture.md 纹理族、70-vm.md 计时窗） */
export interface SceneState {
  /** 脏标记（有东西被改过 ⇒ 1） */
  dirty: number;
  textures: Map<number, TextureRecord>;
  textureObjects: Map<number, TextureObject>;
  drawItems: Map<number, DrawItem>;
  /** 网格表（按编号取记录；缺则建默认记录） */
  meshes: Map<number, Mesh>;
  /** "场景还有动画在跑"（等待门看它；见 CORE-TIMING-WINDOW） */
  animating: number;
  /** 三张辅助键表：规范只要求"按键删除"，内容语义不在本版 */
  auxTables: [Map<number, unknown>, Map<number, unknown>, Map<number, unknown>];
  /** `0x258` / `0x259` 的两张 1000 项标志表（每项两个位）。 */
  flagTableA: number[];
  flagTableB: number[];
  /** `0x2da` 的 CG 参数记录：编号 → 七个整数格。 */
  cgRecords: Map<number, number[]>;
  /** 引擎级每 id 对象槽（纹理三条会清它；影片写它） */
  objectSlots: Map<number, unknown>;
  /** 场景错误缓冲（失败路径写文本；外部呈现不读） */
  errorBuffer: string;
  /** "有活动对象"（影片置 1；等待门的对象状态判据在本版不建模） */
  hasObjects: number;
  /** 场景时钟（毫秒）与计时窗（见 CORE-TIMING-*） */
  now: number;
  prev: number;
  windowDuration: number;
  windowStart: number;
  windowCancel: number;
  windowBusy: number;
}

/**
 * **一帧的引擎态记录**（帧区里的一格）：body / ip / 局部帧 / 父帧号 / 该帧自己的长度槽。
 *
 * ★ 帧区不是栈：引擎是**定长 40 槽、按帧号索引**的数组 + 一个"当前帧号"（`cur`）。
 *   栈（只动末端）表达不了 `0x6 装载帧`（往任意槽写、`cur` 不动）与 `0x8 换帧`（切到任意已装载槽），
 *   也表达不了"父帧号 ≠ 当前号 − 1"的返回（`0x2` 要回到**父帧号**，且只在相邻时才释放）。
 */
export interface ScriptFrame {
  /** 统一文件 id（装载时记下；0x3 / 0x6 的实参）。根帧缺省不着录 */
  id?: number;
  body: Uint8Array;
  ip: number;
  frame: Frame;
  /** 父帧号（引擎记录里的 caller 格）：−1 = 无父帧（根帧） */
  parent: number;
  /** 该帧自己的「操作数个数」槽（引擎记录里的那一格）：该帧恢复执行时按它推进 */
  length: number;
}

/** 帧区容量：引擎是 **40 槽**（上限按**帧号**判，不按"已占用了几个槽"） */
export const FRAME_SLOT_COUNT = 40;

/** 外部请求留痕：规范不规定外部效果，但参考实现要把"发出过什么请求"记下来 */
export interface EffectRecord {
  kind: string;
  detail: Record<string, unknown>;
}

/**
 * 效果的**归属**（只用于输出分级，不改变效果本身）：
 * - `internal`：引擎数据已被建模的写（`string.set` / `array.*` / `engine.scalar.*`）——CLI 已经"处理"了，不是待办；
 * - `external`：真正交给平台 / 外部模块的请求（音频、纹理、输入、宿主字体表）；
 * - `unmodeled`：请求目标尚未建模（`engine.forward` / `subsystem.request` / `vtable.request` …）。
 * ★ `--trace` 默认只打印 `external + unmodeled`；`--trace-all` 才包含 `internal`。
 */
export type EffectScope = 'internal' | 'external' | 'unmodeled';

const INTERNAL_EFFECT_KINDS = new Set([
  'string.set',
  'array.fill',
  'array.lookup',
  'array.copy',
  'engine.scalar.write',
  'engine.scalar.pair',
  'engine.indirect-write',
]);

const UNMODELED_EFFECT_KINDS = new Set([
  'engine.request',
  'engine.forward',
  'engine.event-flush',
  'subsystem.request',
  'vtable.request',
  'container.request',
  'ui.list.request',
  'text.request',
]);

/** 一个效果 kind 的归属（未知 kind 按外部请求处理，宁可多打印也不静默）。 */
export function effectScope(kind: string): EffectScope {
  if (INTERNAL_EFFECT_KINDS.has(kind)) return 'internal';
  if (UNMODELED_EFFECT_KINDS.has(kind) || kind.endsWith('-unmodeled')) return 'unmodeled';
  return 'external';
}

export type Stop =
  | { kind: 'exit' }
  | { kind: 'stop-opcode'; index: number; offset: number; opcode: number }
  | { kind: 'unimplemented'; index: number; offset: number; opcode: number; argc: number }
  | { kind: 'unknown-opcode'; index: number; offset: number; opcode: number }
  | { kind: 'truncated'; index: number; offset: number }
  | { kind: 'step-limit'; index: number }
  | { kind: 'tick-limit'; index: number }
  /** 门位上有一个本版**不服务**的位（例：影片位 0x2000）—— 这是"停在这里"，不是异常 */
  | { kind: 'gate'; index: number; bit: number };

export interface RunResult {
  steps: number;
  index: number;
  ip: number;
  stop: Stop;
}

/**
 * 指令处理器：同步实现返回 `void`；需要取件的（`0x3` / `0x6` / `0x9`）返回 `Promise<void>`。
 * ★ 分派器**总是** `await` 它 ⇒ 同步处理器不会多付一次微任务。
 */
export type Handler = (m: Machine, ops: Operand[]) => void | Promise<void>;

export interface MachineOptions {
  /** 脚本 body（头部之后的全部字节；指令流从第 0 字节开始） */
  body: Uint8Array;
  /** 指令表（决定 argc） */
  table: Map<number, OpcodeRow>;
  /**
   * 整型编码的 key：运行期状态。缺省 = 实现按规范生成一个非 0 值（`generateIntKey`）；
   * 显式给只用于可复现的测试 / 位模式对照。⛔ 0 不是合法值（会给 `codec-key-placeholder` 诊断）。
   */
  key?: number;
  /** 虚拟时钟步长（毫秒）：**仿真参数**，不是引擎常量（引擎没有固定帧率） */
  tickMs?: number;
  /**
   * **宿主毫秒时钟**（可选）：给了就覆盖虚拟步长 —— `tick()` 读它而不是 `now += tickMs`。
   * ★ 规范说时间从宿主来（`CORE-TIMING-CLOCK`）；headless 的虚拟步长是**显式仿真参数**，
   *   窗口前端应传真实时钟，否则 5 秒的渐变会在约 1 秒墙钟内被快进完。
   * ★ 返回值应是单调递增的毫秒（整数或小数都可；本层原样使用）。
   */
  clock?: () => number;
  /** 未写过的浮点槽的读法：`throw`（缺省）或 `zero`（显式选择 + 记诊断） */
  uninitializedFloat?: 'throw' | 'zero';
  /** 配置库（`55-config.md`）；缺省 = 未建立（配置 opcode 会响亮失败） */
  config?: ConfigStore;
  /** 换脚本装载器（真实入口由安装目录提供；`--script` 调试口没有它） */
  loadScript?: (id: number) => Promise<Uint8Array | null>;
  /**
   * **纹理文件装载器**：统一文件 id → 整份文件字节（`0x1f9` 用；⛔ 不剥脚本头）。
   * ★ 缺省 `undefined` = 没有文件来源：`0x1f9` 响亮失败，不伪造成功记录。
   */
  loadImage?: (id: number) => Promise<Uint8Array | null>;
  /**
   * **宿主字体名表**（`0x2de` 的输入）：由宿主提供，**不是**引擎状态。
   * ★ 缺省 `[]` = "宿主没有提供任何字体名" ⇒ 查表一律未命中 ⇒ 走引擎自己的回退分支
   *   （`CHECKCONFIG` 把找不到的字体名换成回退名）。⛔ 本仓**不伪造**系统字体枚举。
   */
  fontNames?: string[];
  /**
   * **渲染宿主**（`60-ops-render.md` 的平台边界）：`0x20d` / `0x204` 的外部请求走它。
   * ★ 缺省 = **没有渲染宿主**：取不到表面 / 设不了目标 ⇒ `0x20d` 走错误通道（⛔ 不假装成功），
   *   `0x204` 的入口条件 ② 因此不成立（什么都不做），并记一条 `render-host-missing` 诊断。
   * ★ 平台实现（Electron / Pixi）与 CLI 的 `HeadlessRenderHost` 都满足 `RenderHost`。
   */
  renderHost?: RenderHost;
  /** 影片播放的平台边界（`0x20f` / 影片对象表服务）；缺省 = 没有后端 ⇒ 影片位一直挡住门 */
  movieHost?: MovieHost;
  /**
   * **宿主随机源**（`0x60` 的输入）：返回 `[0,1)` 的浮点数（本层换算成 u32）。
   * ★ 缺省 = `Math.random`；规范只规定"取一次、再用余数"，序列属宿主实现。
   */
  random?: () => number;
  /**
   * **迭代头的平台泵点**（`70-vm.md` 的 `CORE-VM-LOOP`：每次迭代先在迭代头按节流决定是否泵一次宿主消息）。
   * ★ `run` 的每一轮在**读时钟 / 过门之前**调它一次（泵完仍继续本迭代的其余步骤）。
   *   Electron 主循环把 `WindowSession.pump()` 接在这里，窗口 close / timer 消息才能在脚本执行期间派发。
   * ★ 缺省 `undefined` ⇒ **一次都不调**（不进微任务、不改既有行为）。
   */
  onIteration?: () => void | Promise<void>;
}

export class Machine {
  readonly table: Map<number, OpcodeRow>;
  readonly key: number;
  readonly handlers: Map<number, Handler> = HANDLERS;
  /** 虚拟时钟步长（毫秒；仿真参数） */
  readonly tickMs: number;
  /** 宿主毫秒时钟（缺省 undefined ⇒ 用 `now += tickMs` 的虚拟时钟） */
  readonly clock: (() => number) | undefined;
  /** 未写过的浮点槽策略 */
  readonly uninitializedFloat: 'throw' | 'zero';
  /** 配置库（缺省 undefined：配置 opcode 响亮失败，而不是静默 0） */
  readonly config: ConfigStore | undefined;
  /** 换脚本用的装载器（统一文件 id → body）：缺省 = 不支持 call-script（响亮失败） */
  readonly loadScript: ((id: number) => Promise<Uint8Array | null>) | undefined;
  /** 纹理文件装载器（统一文件 id → 整份字节）：缺省 = `0x1f9` 响亮失败 */
  readonly loadImage: ((id: number) => Promise<Uint8Array | null>) | undefined;
  /** 宿主字体名表（`0x2de` 的输入；缺省空表 ⇒ 查表未命中 ⇒ 引擎走回退分支） */
  readonly fontNames: string[];
  /** 渲染宿主（缺省 `undefined` = 没有后端；见 `MachineOptions.renderHost`） */
  readonly renderHost: RenderHost | undefined;
  /** 影片播放后端（缺省 `undefined` = 没有后端；见 `MachineOptions.movieHost`） */
  readonly movieHost: MovieHost | undefined;
  /** 宿主随机源（`0x60`；缺省 `Math.random`，见 `MachineOptions.random`） */
  readonly random: () => number;
  /** 迭代头的平台泵点（缺省 `undefined` = 不泵；见 `MachineOptions.onIteration`） */
  readonly onIteration: (() => void | Promise<void>) | undefined;
  /**
   * `0x60` 的引擎级计数器：每执行一次 +1，超过 12 归 0（`0..12`）。
   * ★ 身份未取证（读者未知）⇒ 只按规范记状态，不进 `scalars`（那是"被 opcode 写过的格"的通用桶）。
   */
  randomCounter = 0;
  /**
   * `0x74` / `0x7f` 共用的引擎级参数格；`null` = 还没有被 `0x74` 写过。
   * ★ 规范说初值未观察；本层用 `null` 让"先读后写"响亮失败，而不是伪造 0。
   */
  paramCell: number | null = null;
  /**
   * 0x1cf / 0x1bf 的**引擎级状态字**；0x1bf 还可能写 `statusNeighbor`（相邻格）。
   * ★ 它与状态选择字（门字）**不是**同一个字（见 70-vm 的术语段）。
   */
  statusWord = 0;
  /** 0x1bf 的“相邻格”；领域身份未取证。 */
  statusNeighbor = 0;
  /** 0x1cc 读的**状态格**（C）；由别的路径写。 */
  statusCell = 0;
  /** 0x19a 读的**格 X**（原值，不归一）。 */
  flagX = 0;
  /** 0x1b6 / 0x1b7 读写的**格 Y**（归一成 0 / 1）。 */
  flagY = 0;
  /** 0x0d9 的“一格非 0 时清另一个字第 12 位”；两个字的身份未取证。 */
  d9Cell = 0;
  d9Word = 0;
  /** 0x1f4 / 0x1f5 的执行区间状态（区间忙 / 嵌套计数 / 两个时间戳）。 */
  bracketBusy = 0;
  bracketDepth = 0;
  bracketNow = 0;
  bracketPrev = 0;
  /** 队列占用标记；本实现还没有待处理命令队列，泵在空队列上是空操作。 */
  queuePressure = 0;
  /** 0x1ad：当前层号与它落进的引擎格（复位值 −1）。 */
  currentLayer = 0;
  layerCell = -1;
  /** 0x1ce：乱数启用开关 / 启用状态 / 种子。 */
  rngSwitch = 0;
  rngEnabled = 0;
  rngSeed = 0;
  /** 0x7c 的存档字 / 存档位置 / 深度 / 四格 / 待处理标记（进入本地块的那条指令写下）。 */
  localSaveWord = 0;
  localSavePos = 0;
  localSaveDepth = 0;
  localCellA = 0;
  localCellB = 0;
  localCellC = 0;
  localCellD = 0;
  localPending = 0;
  /** 0x19b / 0x19c 的七个引擎格 A/B/C/D/E/F/G（见 60-ops-scalar 的 MODE-BIT）。 */
  modeA = 0;
  modeB = 0;
  modeC = 0;
  modeD = 0;
  modeE = 0;
  modeF = 0;
  modeG = 0;
  /**
   * `0xa1` / `0xa2` / `0xa3` 的**命名表**（文本键 → 整数）。
   * ★ 规范只规定"按键可查 / 覆盖 / 重置"；哈希与探测属实现结构 ⇒ 本层用 `Map`。
   */
  namedTable = new Map<string, number>();
  /**
   * **渲染状态**（`60-ops-render.md`）：当前渲染目标、场景混合的两个格、字体对象。
   * ★ spec 说当前目标是"场景记录里的一个整数字段"、混合的两个格在"场景持有的效应对象"上；
   *   本层把它们收在 `render` 这**一处**（单一真源，⛔ 不往 `scene` 里再复制一份）。
   * ★ 初值见 `initialRenderState()`（目标 = 后缓冲哨兵、模式 = 0、颜色 = 无颜色）。
   */
  render: RenderState = initialRenderState();
  /**
   * **show-logo 标志**（引擎 `Engine+0x5EB5C`）：初始化置 **1**（逐字 `0x415AE0 mov [esi+5EB5Ch], 1`）。
   * `0x130` 读它；`0x9` 的复位会清 0（**本层未建模**，见 `0x9` 条目与残差）。
   * ★ 单独成字段而不是塞进 `scalars`：`scalars` 的语义是"被 opcode 写过的格"，种一个初值会污染
   *   "这条指令不改任何标量"那类判据（`emulator-new-forward.test.mjs` 等）。
   */
  showLogo = 1;
  /**
   * ★★ **帧区**：定长 40 槽、按**帧号**索引；空槽是显式 `null`（不是"洞"）。
   * 帧**不是栈**：`0x6` 往任意槽写记录而 `cur` 不动、`0x8` 切到任意已装载槽、
   * `0x2` 回到**父帧号**（且只在「父号 + 1 == 当前号」时释放那一槽）。
   */
  slots: (ScriptFrame | null)[] = [];
  /** 当前帧号（引擎 `Engine[0x5D880]`）；根帧占 0 号槽 */
  cur = 0;
  /**
   * **进入过的脚本 id**（`0x3 call-script` 装进来的、`0x8 call-frame` 切进去的）。
   * ★ 只是记账，不参与执行；用途：让"启动链到达了哪个脚本"成为**可断言**的事实（不用靠 step 数猜）。
   */
  enteredIds: number[] = [];

  /** 引擎级状态选择字：整字为 0 才执行指令（CORE-VM-GATE） */
  flags = 0;
  /** 输入字（0x101 采样后清零；本层只记账） */
  inputWord = 0;
  /** 虚拟时钟（毫秒） */
  now = 0;
  /** 门关着的迭代次数（每 tick 一次） */
  ticks = 0;
  /** 实现必须记账的"不可解释之处" */
  diagnostics: Diagnostic[] = [];
  /** 语义未定、但**确实**被写的引擎级标量（键是实现侧标签，不是引擎字段名） */
  scalars = new Map<string, number>();

  /** 已执行的指令条数（不是 ip / 4：控制流可以往回跳） */
  steps = 0;
  halted = false;
  /** 0x01 / 0x0c8 的退出码；`halted` 为 true 时它才有效。 */
  exitCode = 0;

  /** 全局整型池：编码后的位模式 */
  globalInt = new Map<number, number>();
  /** 全局浮点池：裸 32 位位模式 */
  globalFloat = new Map<number, number>();
  /** 全局字符串池（字节串，latin1；`55-config.md` 的脚本访问只用到 type 5） */
  globalString = new Map<number, string>();
  /** 全局整型引用池（type 6）：槽里装**地址** */
  globalIntRef = new Map<number, number>();
  /** 全局浮点引用池（type 7）：槽里装**地址** */
  globalFloatRef = new Map<number, number>();
  /** 全局字符串引用池（type 8）：槽里装**地址** */
  globalStringRef = new Map<number, number>();
  /** 场景状态（纹理 / 绘制项 / 计时窗） */
  scene: SceneState = {
    dirty: 0,
    textures: new Map(),
    textureObjects: new Map(),
    drawItems: new Map(),
    meshes: new Map(),
    animating: 0,
    auxTables: [new Map(), new Map(), new Map()],
    flagTableA: new Array(1000).fill(0),
    flagTableB: new Array(1000).fill(0),
    cgRecords: new Map(),
    objectSlots: new Map(),
    errorBuffer: '',
    hasObjects: 0,
    now: 0,
    prev: 0,
    windowDuration: 0,
    windowStart: 0,
    windowCancel: 0,
    windowBusy: 0,
  };
  /**
   * 声音对象（引擎里是一个子对象；语音对象在它 `+0x408` 存着指向它的指针）。
   * ★ `backend` 是**显式状态**，不是常量：后端不存在时引擎不清语音对象里的 4 格、不发任何
   *   播放查询、也不设置音量（各处都有一道门），标志直接 0。参考实现建模为 `present`
   *   （真实机器上后端存在），但分支必须可被守卫选中。
   * ★ `buffers` = 15 个通道（0..14）：`playing` 是引擎从缓冲对象读的那一位；
   *   本层没有任何指令会把它置 1 ⇒ "全不在播"是**可断言的前提**，不是猜的常量。
   * ★ `volumes` = 每通道的音量格（`0x2f8` 写入，夹在 ±10000）。
   */
  sound: {
    backend: 'present' | 'absent';
    buffers: Map<number, { playing: number }>;
    volumes: Map<number, number>;
    /** 声音对象的错误文本缓冲（非法通道号等失败路径写它） */
    errorBuffer: string;
  } = { backend: 'present', buffers: new Map(), volumes: new Map(), errorBuffer: '' };
  /** UI 列表（见 CORE-OPS-LIST-SET）：按组号索引的记录 */
  /** 列表 / 消息子系统的默认组（`0x80` 写；初值 1，见 60-ops-list） */
  ui: { records: Map<number, ListRecord>; defaultGroup: number } = { records: new Map(), defaultGroup: 1 };
  /** 语音对象：三个组的按组索引状态（`0x2f6` 重置其中 8 格） */
  voice: { groups: Map<number, VoiceGroup> } = { groups: new Map() };
  /** 发给外部模块的请求（按发生顺序） */
  effects: EffectRecord[] = [];

  // ── 当前帧的访问（⛔ 单一真源：都是槽 `cur` 上那份记录，不另存副本） ─────────────

  /** 当前帧记录（槽 `cur`）；空槽 ⇒ 响亮失败（"没有帧"不是"帧是空的"） */
  private get rec(): ScriptFrame {
    const f = this.slots[this.cur];
    if (!f) throw new Error(`没有活动帧：槽 ${this.cur} 是空的`);
    return f;
  }

  /** 当前脚本 body */
  get body(): Uint8Array { return this.rec.body; }
  set body(v: Uint8Array) { this.rec.body = v; }

  /** 当前指令字节偏移 */
  get ip(): number { return this.rec.ip; }
  set ip(v: number) { this.rec.ip = v; }

  /** 当前帧的局部池 / 返回栈 */
  get frame(): Frame { return this.rec.frame; }
  set frame(v: Frame) { this.rec.frame = v; }

  /** 「操作数个数」槽（**当前帧**那一格）：分派器在每条 handler 前写 2·argc+1，执行完按它推进 */
  get operandCount(): number { return this.rec.length; }
  set operandCount(v: number) { this.rec.length = v; }

  constructor(opts: MachineOptions) {
    this.table = opts.table;
    this.key = opts.key ?? generateIntKey();
    this.tickMs = opts.tickMs ?? 16;
    this.clock = opts.clock;
    this.uninitializedFloat = opts.uninitializedFloat ?? 'throw';
    this.config = opts.config;
    // key 非 0 是引擎初始化期的不变量；宿主给 0 = 模型处在引擎不可能的状态。
    // 只在入口记一次（不是每条 0x6 都记；0x6 的内联前置只是同一不变量的一个相遇点）。
    if (this.key === 0) this.diagnostics.push({ kind: 'codec-key-placeholder' });
    // 列表子系统构造时预建 10 条记录（组 0..9）；`0x70`/`0x73` 等域内写入不再需要“缺则建”。
    for (let group = 0; group < LIST_GROUP_COUNT; group += 1) this.ui.records.set(group, emptyListRecord());
    // `renderer-init` 阶段（06-window / boot）会把配置 `set:DrawMode` 缓存到 `Engine+0xA30D0`
    // （= dword 166964）。该格同时是 0x73 的“第二请求”门与 0x70 的浮点/绘制分岔门。
    // ★ 只有给了配置库（安装入口）才播种；--script / 单元测试没有 config ⇒ 保持未建模。
    if (opts.config) this.scalars.set('Engine.d166964', opts.config.getInt('set:DrawMode'));
    this.loadScript = opts.loadScript;
    this.loadImage = opts.loadImage;
    this.fontNames = [...(opts.fontNames ?? [])];
    this.renderHost = opts.renderHost;
    this.movieHost = opts.movieHost;
    this.random = opts.random ?? Math.random;
    this.onIteration = opts.onIteration;
    // 根脚本落在 **0 号槽**（引擎：根帧的父帧号是 −1，当前帧号从 0 起）
    this.slots = [{ body: opts.body, ip: 0, frame: this.freshFrame(), parent: -1, length: 0 }];
    this.cur = 0;
  }

  /**
   * 门关着时的一次迭代：服务**一个**状态位。
   * ★ 本版只有"等待位"：场景动画还在跑 ⇒ 推进时钟（`scene` 的窗在 `stepScene` 里推进），
   *   否则清位、下次迭代继续执行指令。
   */
  private serviceFlags(): Stop | null {
    if ((this.flags & WAIT_BIT) !== 0) {
      if (this.scene.animating === 0) {
        this.flags &= ~WAIT_BIT;
        return null;
      }
      this.tick();
      return null;
    }
    if ((this.flags & MOVIE_BIT) !== 0) {
      // 没有影片后端：这是本实现的“未建模”边界，保留原来的门停因，不空转。
      if (this.movieHost === undefined) return { kind: 'gate', index: this.steps, bit: this.flags };
      // 影片位服务（CORE-VM-MOVIE）：只做“清位 → 对象表还有影片就再置 → 让门继续关着”
      this.flags &= ~MOVIE_BIT;
      if (this.hasMovieSlots()) {
        this.flags |= MOVIE_BIT;
        // 服务一位也算一次迭代：推进仿真时钟，避免无影片后端时纯空转（不是引擎常量）
        this.tick();
      }
      return null;
    }
    // 其余门位本版不服务 ⇒ **停下来**并报告：它是"这里没有建模"，不是"执行出错"
    return { kind: 'gate', index: this.steps, bit: this.flags };
  }

  /** 虚拟时钟前进一格，并把场景时钟推到新读数（引擎每迭代读一次毫秒时钟） */
  tick(): void {
    // ★ 宿主时钟优先：窗口前端传真实毫秒时钟；headless 不传 ⇒ 虚拟步长（显式仿真参数）。
    //   不在这里做"至少走 tickMs"的夹取：宿主时钟慢/快都应如实反映；仿真侧要固定步长就显式传 tickMs。
    this.now = this.clock ? this.clock() : this.now + this.tickMs;
    this.ticks += 1;
    this.scene.prev = this.scene.now;
    this.scene.now = this.now;
  }

  /**
   * 门之前的影片对象表服务（CORE-VM-MOVIE）。
   * ★ 只在“有活动对象”标记为 1 时扫描；该标记是本条自己的闸，不是影片位。
   * ★ 没有影片后端时保留对象并继续置“有活动对象”：门会一直关着（未建模，不伪造播完）。
   */
  serviceMovies(): void {
    if (this.scene.hasObjects === 0) return;
    const host = this.movieHost;
    let any = 0;
    for (const [slot, value] of [...this.scene.objectSlots]) {
      const obj = value as MovieObject;
      if (obj.kind !== 'movie') continue;
      if (!obj.started) continue;
      any = 1;
      const state = obj.handle !== null ? host?.poll(obj.handle) ?? null : null;
      if (state !== null) {
        obj.currentTime = state.currentTime;
        obj.stopTime = state.stopTime;
      }
      const done = state !== null && state.stopTime > 0 && state.currentTime >= state.stopTime;
      if (!done) continue;
      if (obj.loop) {
        if (obj.handle !== null) {
          host?.rewind(obj.handle);
          host?.play(obj.handle);
        }
        obj.currentTime = 0;
        this.flags |= MOVIE_BIT;
        continue;
      }
      if (obj.handle !== null) {
        if ((obj.opt & 3) !== 2) host?.fillTexture(slot);
        host?.close(obj.handle);
      }
      this.scene.objectSlots.delete(slot);
      any = 0;
      for (const rest of this.scene.objectSlots.values()) {
        const r = rest as MovieObject;
        if (r.kind === 'movie' && r.started) any = 1;
      }
    }
    this.scene.hasObjects = any;
  }

  /** 对象表里是否还有非空影片槽（影片位服务用） */
  private hasMovieSlots(): boolean {
    for (const value of this.scene.objectSlots.values()) {
      const obj = value as MovieObject;
      if (obj.kind === 'movie' && obj.started) return true;
    }
    return false;
  }

  /**
   * 场景帧步：清"有动画"标志，再逐项推进颜色渐变窗（单位毫秒；起点 0 = 未盖章）。
   * 窗进行中：插值写 **colorNow（派生色）/ 网格顶点缓冲**，⛔ 不写回 colorFrom；
   * 到期 ⇒ 延迟 / 持续时间清 0、当前颜色 ← 终色、终色 ← 无颜色，并清"有窗"位；
   * 绘制项与网格**都如此**（见 CORE-TIMING-WINDOW）。
   */
  stepScene(): void {
    this.scene.animating = 0;
    const now = this.scene.now;
    for (const item of this.scene.drawItems.values()) {
      if ((item.flags & 2) === 0) continue;
      // 持续时间 ≤ 0（从未排窗，或已到期）⇒ 这一趟不推进
      if (item.duration <= 0) continue;
      if (item.start === 0) item.start = now;
      if (item.duration > 0 && now <= item.start + item.delay + item.duration) {
        if (now > item.start + item.delay) {
          const t = (now - item.start - item.delay) / item.duration;
          item.colorNow = blendColor(item.colorFrom, item.colorTo, t);
        } else {
          item.colorNow = item.colorFrom;
        }
        this.scene.animating = 1;
      } else {
        item.delay = 0;
        item.duration = 0;
        item.colorFrom = item.colorTo;
        item.colorNow = item.colorFrom;
        item.colorTo = -1;
        item.flags &= ~2;
      }
    }
    for (const mesh of this.scene.meshes.values()) {
      if ((mesh.flags & 2) === 0) continue;
      if (mesh.duration <= 0) continue;
      if (mesh.start === 0) mesh.start = now;
      if (mesh.duration > 0 && now <= mesh.start + mesh.delay + mesh.duration) {
        if (now > mesh.start + mesh.delay) {
          const t = (now - mesh.start - mesh.delay) / mesh.duration;
          mesh.colorNow = blendColor(mesh.colorFrom, mesh.colorTo, t);
        } else {
          mesh.colorNow = mesh.colorFrom;
        }
        if (mesh.colorNow !== -1) mesh.colors = mesh.colors.map(() => mesh.colorNow >>> 0);
        this.scene.animating = 1;
      } else {
        mesh.delay = 0;
        mesh.duration = 0;
        mesh.colorFrom = mesh.colorTo;
        mesh.colorNow = mesh.colorFrom;
        if (mesh.colorNow !== -1) mesh.colors = mesh.colors.map(() => mesh.colorNow >>> 0);
        mesh.colorTo = -1;
        mesh.flags &= ~2;
      }
    }
  }

  /** 空白帧（新脚本用）：池与返回栈都重新建 */
  freshFrame(): Frame {
    return {
      localInt: new Map(), localFloat: new Map(),
      localPtr: new Map(), localFloatPtr: new Map(), localStringPtr: new Map(),
      retStack: [], position: 0,
    };
  }

  /**
   * `0x3 换脚本`：在「当前帧号 + 1」那个槽上建子帧并切过去（引擎的"深度 +1"）。
   * ★ 父帧记录**保持原样**（ip 停在调用点、长度槽已由分派器写成 3），所以子脚本结束时
   *   回到父帧、由父帧自己的长度槽推进到"下一条"。
   * ★ 深度按**槽号**判：`cur + 1 >= 40` 才是超限（⛔ 不是按"数组里已有几帧"）。
   */
  enterScript(body: Uint8Array, id?: number): ScriptFrame {
    const slot = this.cur + 1;
    if (slot >= FRAME_SLOT_COUNT) {
      throw new Error(`call-script 深度超限：目标帧号 ${slot} ≥ ${FRAME_SLOT_COUNT}（引擎抛「階層が深すぎます」）`);
    }
    const rec: ScriptFrame = { id, body, ip: 0, frame: this.freshFrame(), parent: this.cur, length: 0 };
    if (id !== undefined) this.enteredIds.push(id);
    this.slots[slot] = rec;
    this.cur = slot;
    return rec;
  }

  /**
   * `0x2 结束当前脚本`的正面支：回到**父帧号**那一帧。
   *
   * 引擎的三步（⛔ 不是"弹栈顶"）：
   * 1. 当前帧号 ← 记录里的**父帧号**；
   * 2. **有条件释放**：只有「父帧号 + 1 == 当前帧号」才把当前槽清空；否则只切号，
   *    目标帧仍留在它那个槽上（`0x8 换帧` 切进来的已装载帧就是这样）；
   * 3. 从父帧号起**向下（帧号递减）跳过空槽**，直到落在非空槽上；降到 0 以下 ⇒ 错误。
   *
   * @returns `true` = 回到父帧（`operandCount` 已设成父帧自己的长度槽）；
   *          `false` = 没有父帧（根帧）⇒ 整个执行结束
   */
  returnToCaller(): boolean {
    const f = this.rec;
    const parent = f.parent;
    if (parent < 0) return false;
    if (parent + 1 === this.cur) this.slots[this.cur] = null;
    let next = parent;
    while (next >= 0 && !this.slots[next]) next -= 1;
    if (next < 0) throw new Error('exit：从父帧号往下全是空槽（引擎在帧号降到 0 以下时抛）');
    this.cur = next;
    this.operandCount = this.slots[next]!.length;
    return true;
  }

  /**
   * `0x6 装载帧`：把 body 装进**指定槽**，**不动当前帧号**（引擎"临时切 cur、装完恢复"的净效果）。
   * ★ 记录里的父帧号 ← 装载时正在执行的帧号（引擎逐字取 `0x6` 存下的旧 cur）。
   */
  loadFrameAt(slot: number, body: Uint8Array, id?: number): ScriptFrame {
    if (!Number.isInteger(slot) || slot < 0 || slot >= FRAME_SLOT_COUNT) {
      throw new Error(`load-frame 的槽号 ${slot} 不在 0..${FRAME_SLOT_COUNT - 1} 之内`);
    }
    const rec: ScriptFrame = { id, body, ip: 0, frame: this.freshFrame(), parent: this.cur, length: 0 };
    while (this.slots.length <= slot) this.slots.push(null);
    this.slots[slot] = rec;
    return rec;
  }

  /**
   * `0x8 换帧`：切到**已装载**的某一槽，并从那一帧的**开头**执行。
   * ★ **只改三格**：父帧号 ← 调用者、ip ← 0、长度槽 ← 0；返回栈 / 局部池 / body **都不动**
   *   （所以同一帧被切进来两次时，返回栈是累积的 —— 引擎语义）。
   * ★ 调用者的 ip 不动：它仍停在 `0x8` 上，回来时由它自己的长度槽推进。
   */
  callFrameAt(slot: number): ScriptFrame {
    if (!Number.isInteger(slot) || slot < 0 || slot >= FRAME_SLOT_COUNT) {
      throw new Error(`call-frame 的目标帧号 ${slot} 不在 0..${FRAME_SLOT_COUNT - 1} 之内（引擎不做范围检查）`);
    }
    const target = this.slots[slot] ?? null;
    if (!target) {
      throw new Error(`call-frame：槽 ${slot} 上没有装载脚本（引擎抛「この階層にはファイルが読み込まれていません．Depth=${slot}」）`);
    }
    target.parent = this.cur;
    target.ip = 0;
    this.cur = slot;
    this.operandCount = 0;
    if (target.id !== undefined) this.enteredIds.push(target.id);
    return target;
  }

  /** 读当前指令（不执行）。越界 / 未知 opcode 返回 null 并附停因。 */
  decodeAt(ip: number): { opcode: number; row: OpcodeRow; argc: number; ops: Operand[] } | { stop: Stop } {
    if (ip < 0 || ip + 4 > this.body.length) {
      return { stop: { kind: 'truncated', index: this.steps, offset: ip } };
    }
    const opcode = readU32LE(this.body, ip);
    const row = this.table.get(opcode);
    if (!row) return { stop: { kind: 'unknown-opcode', index: this.steps, offset: ip, opcode } };
    const len = 4 + 8 * row.argc;
    if (ip + len > this.body.length) {
      return { stop: { kind: 'truncated', index: this.steps, offset: ip } };
    }
    const ops: Operand[] = [];
    for (let k = 0; k < row.argc; k += 1) {
      ops.push({ type: readU32LE(this.body, ip + 4 + 8 * k), raw: readU32LE(this.body, ip + 8 + 8 * k) });
    }
    return { opcode, row, argc: row.argc, ops };
  }

  /**
   * 跑到：显式停止条件 / 退出 / 缺口 / 步数上限。
   * ★ 缺口：没有 handler 的 opcode **不执行**，`ip` 停在它前面（由调用方决定是红还是记缺口）。
   * ★ `async`：处理器可以等待 IO（换脚本 / 装载帧）；分派循环 `await` 它，异常变成同一个 rejection。
   * ★ 每次迭代头先调一次注入的 `onIteration`（平台消息泵；缺省不调、连一次 `await` 都不多付）。
   */
  async run(opts: { stopAtOpcode?: number; maxSteps?: number; maxTicks?: number } = {}): Promise<RunResult> {
    const maxSteps = opts.maxSteps ?? 10_000_000;
    // ★ 虚拟时钟用迭代数兜底（防死循环）；**宿主时钟**下迭代数与墙钟不成比例
    //   （5 秒的渐变可能跑出上千万次门迭代）⇒ 不把迭代数当时间上限，
    //   由窗口关闭 / 平台自己的超时来停止。CLI 不传 clock ⇒ 仍是 10M 的安全阀。
    const maxTicks = opts.maxTicks ?? (this.clock ? Number.POSITIVE_INFINITY : 10_000_000);
    for (;;) {
      // 迭代头（CORE-VM-LOOP）：先泵一次宿主消息（节流由平台 / 注入方决定），再读时钟、过门。
      // ★ 泵完仍继续本迭代的其余步骤；缺省不泵（连一次 `await` 都不多付）。
      if (this.onIteration) await this.onIteration();
      this.stepScene();
      this.serviceMovies();
      if (this.halted) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'exit' } };
      if (this.steps >= maxSteps) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'step-limit', index: this.steps } };
      if (this.ticks >= maxTicks) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'tick-limit', index: this.steps } };
      // 门（CORE-VM-GATE）：状态字非 0 ⇒ 本迭代只服务一个位，不执行指令
      if (this.flags !== 0) {
        const gate = this.serviceFlags();
        if (gate) return { steps: this.steps, index: this.steps, ip: this.ip, stop: gate };
        continue;
      }
      const at = this.ip;
      const dec = this.decodeAt(at);
      if ('stop' in dec) return { steps: this.steps, index: this.steps, ip: this.ip, stop: dec.stop };
      if (opts.stopAtOpcode !== undefined && dec.opcode === opts.stopAtOpcode) {
        return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'stop-opcode', index: this.steps, offset: at, opcode: dec.opcode } };
      }
      const handler = this.handlers.get(dec.opcode);
      if (!handler) {
        return {
          steps: this.steps, index: this.steps, ip: this.ip,
          stop: { kind: 'unimplemented', index: this.steps, offset: at, opcode: dec.opcode, argc: dec.argc },
        };
      }
      // 分派器协议：handler 之前在「操作数个数」槽写 2·argc+1；handler 可以清零（控制转移）
      this.operandCount = 2 * dec.argc + 1;
      await handler(this, dec.ops);
      this.ip += 4 * this.operandCount;
      this.steps += 1;
    }
  }
}

/** 等待位（CORE-OPS-WAIT / CORE-VM-STATE：本版只规定这一个位的语义） */
export const WAIT_BIT = 0x400;

/** 影片位（CORE-OPS-MOVIE：置起后门会关；本版不实现它的服务分支） */
export const MOVIE_BIT = 0x2000;

/** 按 t 线性混合两个 ARGB 颜色（t 会夹到 [0,1]） */
function blendColor(from: number, to: number, t: number): number {
  const k = t < 0 ? 0 : t > 1 ? 1 : t;
  const ch = (v: number, shift: number): number => (v >>> shift) & 0xff;
  const mix = (a: number, b: number): number => Math.round(a + (b - a) * k) & 0xff;
  const out = ((mix(ch(from, 24), ch(to, 24)) << 24)
    | (mix(ch(from, 16), ch(to, 16)) << 16)
    | (mix(ch(from, 8), ch(to, 8)) << 8)
    | mix(ch(from, 0), ch(to, 0))) >>> 0;
  return out;
}
