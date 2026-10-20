/**
 * 执行器：取指 → 选处理器 → 执行 → 按操作数个数槽推进。
 * ★ 规则只来自 `data/spec`：
 *   - 切分与推进：`chapters/10-format.md` 的「切分与操作数个数」「opcode 域与缺省处理器」「执行如何结束」；
 *   - 操作数：`chapters/40-pools.md` 的五种访问原语；
 *   - 单条指令：`chapters/60-ops.md`。
 * ★ 没有实现的 opcode **不执行**、不 no-op：报缺口并停在它前面（缺口是 spec 的机器清单）。
 * ★ 本文件不碰文件系统。
 */
import type { OpcodeRow } from './spec.ts';
import type { ConfigStore } from './config.ts';
import { HANDLERS } from './handlers.ts';

export interface Operand {
  type: number;
  raw: number;
}

export interface Frame {
  /** 局部整型池：编码后的位模式（初值 = ENC(0)） */
  localInt: Map<number, number>;
  /** 局部浮点池：裸 32 位位模式（规范不保证初值） */
  localFloat: Map<number, number>;
  /**
   * 本帧的**返回栈**（位置 = 同脚本内 dword 序号；见 CORE-FRAME-CALL / CORE-FRAME-RET）。
   * ★ 每帧一份：换帧就换成另一条栈（引擎给每帧各留一段固定容量，容量属机器细节）。
   */
  retStack: number[];
  /** 帧记录里的"本条指令的位置"（帧+0x6C；写入者是 0x71，读者未定） */
  position: number;
}

/** 纹理记录：来源文件（-1 = 无）、颜色键、空白创建标记（见 CORE-OPS-TEXTURE-*） */
export interface TextureRecord {
  file: number;
  color: number;
  blank: number;
}

/** 纹理槽里的对象：只有"哪种纹理"是规范可观察的（内部资源属外部模块） */
export interface TextureObject {
  kind: 'normal' | 'divided';
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
  colorTo: number;
  /** 操作数里"读者未找到"的槽（按操作数序号记，只为不丢数据） */
  unknown: Map<number, number>;
}

/** 网格（见 CORE-OPS-SCENE-320/322/323）：三个缓冲在本层只保留可观察的部分 */
export interface Mesh {
  /** 位 0 = 已存在；位 1 = 有颜色渐变窗 */
  flags: number;
  count: number;
  op10: number;
  /** 第 1 个缓冲（逐顶点 1.0）与第 3 个缓冲（逐顶点颜色） */
  weights: number[];
  colors: number[];
  /** 第 2/3/4 与第 7/8 个操作数（浮点） */
  scale: [number, number, number];
  uv: [number, number];
  start: number;
  delay: number;
  duration: number;
  colorFrom: number;
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
 * UI 列表项记录（见 CORE-OPS-LIST-SET）：`0x70` 按组号写入的那 8 个字段。
 * ★ 字段的领域用途未定（布局层只有"记录 + 偏移"）；这里只按机器身份存值，不命名、不解释。
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

/** 影片对象（见 CORE-OPS-MOVIE）：只保留可观察的引擎数据 */
export interface MovieObject {
  kind: 'movie';
  slot: number;
  resourceId: number;
  opt: number;
  started: boolean;
}

/** 诊断：实现必须记账的"不可解释之处"（⛔ 不许静默补值） */
export interface Diagnostic {
  kind: string;
  index?: number;
}

/** 场景：纹理 / 绘制项 / 计时窗（见 60-ops.md 纹理族、70-vm.md 计时窗） */
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

/** 挂起的父脚本（call-script 的栈帧）：body / ip / 帧记录 / 该帧自己的长度槽 */
export interface ScriptFrame {
  body: Buffer;
  ip: number;
  frame: Frame;
  /** 父帧在调用点的长度槽（call-script 恒为 2·argc+1 = 3） */
  length: number;
}

/** 外部请求留痕：规范不规定外部效果，但参考实现要把"发出过什么请求"记下来 */
export interface EffectRecord {
  kind: string;
  detail: Record<string, unknown>;
}

export type Stop =
  | { kind: 'exit' }
  | { kind: 'stop-opcode'; index: number; offset: number; opcode: number }
  | { kind: 'unimplemented'; index: number; offset: number; opcode: number; argc: number }
  | { kind: 'unknown-opcode'; index: number; offset: number; opcode: number }
  | { kind: 'truncated'; index: number; offset: number }
  | { kind: 'step-limit'; index: number }
  | { kind: 'tick-limit'; index: number };

export interface RunResult {
  steps: number;
  index: number;
  ip: number;
  stop: Stop;
}

export type Handler = (m: Machine, ops: Operand[]) => void;

export interface MachineOptions {
  /** 脚本 body（头部之后的全部字节；指令流从第 0 字节开始） */
  body: Buffer;
  /** 指令表（决定 argc） */
  table: Map<number, OpcodeRow>;
  /** 整型编码的 key：运行期状态，必须显式给（不许有默认值） */
  key: number;
  /** 虚拟时钟步长（毫秒）：**仿真参数**，不是引擎常量（引擎没有固定帧率） */
  tickMs?: number;
  /** 未写过的浮点槽的读法：`throw`（缺省）或 `zero`（显式选择 + 记诊断） */
  uninitializedFloat?: 'throw' | 'zero';
  /** 配置库（`55-config.md`）；缺省 = 未建立（配置 opcode 会响亮失败） */
  config?: ConfigStore;
  /** 换脚本装载器（真实入口由安装目录提供；`--script` 调试口没有它） */
  loadScript?: (id: number) => Buffer | null;
}

export class Machine {
  body: Buffer;
  readonly table: Map<number, OpcodeRow>;
  readonly key: number;
  readonly handlers: Map<number, Handler> = HANDLERS;
  /** 虚拟时钟步长（毫秒；仿真参数） */
  readonly tickMs: number;
  /** 未写过的浮点槽策略 */
  readonly uninitializedFloat: 'throw' | 'zero';
  /** 配置库（缺省 undefined：配置 opcode 响亮失败，而不是静默 0） */
  readonly config: ConfigStore | undefined;
  /** 换脚本用的装载器（统一文件 id → body）：缺省 = 不支持 call-script（响亮失败） */
  readonly loadScript: ((id: number) => Buffer | null) | undefined;
  /** 挂起的父脚本栈（call-script 进入时压、exit 回父时弹） */
  frames: ScriptFrame[] = [];

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

  ip = 0;
  /** 已执行的指令条数（不是 ip / 4：控制流可以往回跳） */
  steps = 0;
  /** 「操作数个数」槽：分派器在每条 handler 前写成 2·argc+1，执行完按它推进 */
  operandCount = 0;
  halted = false;

  frame: Frame = { localInt: new Map(), localFloat: new Map(), retStack: [], position: 0 };
  /** 全局整型池：编码后的位模式 */
  globalInt = new Map<number, number>();
  /** 全局浮点池：裸 32 位位模式 */
  globalFloat = new Map<number, number>();
  /** 全局字符串池（字节串，latin1；`55-config.md` 的脚本访问只用到 type 5） */
  globalString = new Map<number, string>();
  /** 场景状态（纹理 / 绘制项 / 计时窗） */
  scene: SceneState = {
    dirty: 0,
    textures: new Map(),
    textureObjects: new Map(),
    drawItems: new Map(),
    meshes: new Map(),
    animating: 0,
    auxTables: [new Map(), new Map(), new Map()],
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
  ui: { records: Map<number, ListRecord> } = { records: new Map() };
  /** 语音对象：三个组的按组索引状态（`0x2f6` 重置其中 8 格） */
  voice: { groups: Map<number, VoiceGroup> } = { groups: new Map() };
  /** 发给外部模块的请求（按发生顺序） */
  effects: EffectRecord[] = [];
  constructor(opts: MachineOptions) {
    this.body = opts.body;
    this.table = opts.table;
    this.key = opts.key;
    this.tickMs = opts.tickMs ?? 16;
    this.uninitializedFloat = opts.uninitializedFloat ?? 'throw';
    this.config = opts.config;
    this.loadScript = opts.loadScript;
  }

  /**
   * 门关着时的一次迭代：服务**一个**状态位。
   * ★ 本版只有"等待位"：场景动画还在跑 ⇒ 推进时钟（`scene` 的窗在 `stepScene` 里推进），
   *   否则清位、下次迭代继续执行指令。
   */
  private serviceFlags(): void {
    if ((this.flags & WAIT_BIT) !== 0) {
      this.stepScene();
      if (this.scene.animating === 0) {
        this.flags &= ~WAIT_BIT;
        return;
      }
      this.tick();
      return;
    }
    throw new Error(`unimplemented-gate-bit:0x${this.flags.toString(16)}`);
  }

  /** 虚拟时钟前进一格，并把场景时钟推到新读数（引擎每迭代读一次毫秒时钟） */
  tick(): void {
    this.now += this.tickMs;
    this.ticks += 1;
    this.scene.prev = this.scene.now;
    this.scene.now = this.now;
  }

  /**
   * 场景帧步：清"有动画"标志，再逐项推进颜色渐变窗（单位毫秒；起点 0 = 未盖章）。
   * ★ 收尾不对称：网格清"有窗"位，绘制项**不清**（真实差异，见 CORE-TIMING-WINDOW）。
   */
  stepScene(): void {
    this.scene.animating = 0;
    const now = this.scene.now;
    for (const item of this.scene.drawItems.values()) {
      if ((item.flags & 2) === 0) continue;
      // 已经收尾过（窗长 0、终色已清）⇒ 不再进入：绘制项收尾**不清**"有窗"位（真实不对称）
      if (item.duration <= 0) continue;
      if (item.start === 0) item.start = now;
      if (item.duration > 0 && now < item.start + item.delay + item.duration) {
        if (now > item.start + item.delay) {
          const t = (now - item.start - item.delay) / item.duration;
          item.colorFrom = blendColor(item.colorFrom, item.colorTo, t);
        }
        this.scene.animating = 1;
      } else {
        item.delay = 0;
        item.duration = 0;
        item.colorFrom = item.colorTo;
        item.colorTo = -1;
        // 绘制项收尾**不清**位 1（与网格不对称）
      }
    }
    for (const mesh of this.scene.meshes.values()) {
      if ((mesh.flags & 2) === 0) continue;
      if (mesh.duration <= 0) continue;
      if (mesh.start === 0) mesh.start = now;
      if (mesh.duration > 0 && now < mesh.start + mesh.delay + mesh.duration) {
        if (now > mesh.start + mesh.delay) {
          const t = (now - mesh.start - mesh.delay) / mesh.duration;
          mesh.colorFrom = blendColor(mesh.colorFrom, mesh.colorTo, t);
        }
        this.scene.animating = 1;
      } else {
        mesh.delay = 0;
        mesh.duration = 0;
        mesh.colorFrom = mesh.colorTo;
        mesh.colorTo = -1;
        mesh.flags &= ~2;
      }
    }
  }

  /** 空白帧（新脚本用）：池与返回栈都重新建 */
  freshFrame(): Frame {
    return { localInt: new Map(), localFloat: new Map(), retStack: [], position: 0 };
  }

  /** 压一层父脚本（call-script 进入前调用） */
  pushScriptFrame(length: number): void {
    this.frames.push({ body: this.body, ip: this.ip, frame: this.frame, length });
  }

  /** 回父脚本：恢复 body / ip / 帧，并把「操作数个数」槽设成父帧自己的长度（引擎的算术） */
  restoreScriptFrame(): void {
    const parent = this.frames.pop();
    if (!parent) return;
    this.body = parent.body;
    this.ip = parent.ip;
    this.frame = parent.frame;
    this.operandCount = parent.length;
  }

  /** 读当前指令（不执行）。越界 / 未知 opcode 返回 null 并附停因。 */
  decodeAt(ip: number): { opcode: number; row: OpcodeRow; argc: number; ops: Operand[] } | { stop: Stop } {
    if (ip < 0 || ip + 4 > this.body.length) {
      return { stop: { kind: 'truncated', index: this.steps, offset: ip } };
    }
    const opcode = this.body.readUInt32LE(ip);
    const row = this.table.get(opcode);
    if (!row) return { stop: { kind: 'unknown-opcode', index: this.steps, offset: ip, opcode } };
    const len = 4 + 8 * row.argc;
    if (ip + len > this.body.length) {
      return { stop: { kind: 'truncated', index: this.steps, offset: ip } };
    }
    const ops: Operand[] = [];
    for (let k = 0; k < row.argc; k += 1) {
      ops.push({ type: this.body.readUInt32LE(ip + 4 + 8 * k), raw: this.body.readUInt32LE(ip + 8 + 8 * k) });
    }
    return { opcode, row, argc: row.argc, ops };
  }

  /**
   * 跑到：显式停止条件 / 退出 / 缺口 / 步数上限。
   * ★ 缺口：没有 handler 的 opcode **不执行**，`ip` 停在它前面（由调用方决定是红还是记缺口）。
   */
  run(opts: { stopAtOpcode?: number; maxSteps?: number; maxTicks?: number } = {}): RunResult {
    const maxSteps = opts.maxSteps ?? 10_000_000;
    const maxTicks = opts.maxTicks ?? 10_000_000;
    for (;;) {
      if (this.halted) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'exit' } };
      if (this.steps >= maxSteps) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'step-limit', index: this.steps } };
      if (this.ticks >= maxTicks) return { steps: this.steps, index: this.steps, ip: this.ip, stop: { kind: 'tick-limit', index: this.steps } };
      // 门（CORE-VM-GATE）：状态字非 0 ⇒ 本迭代只服务一个位，不执行指令
      if (this.flags !== 0) {
        this.serviceFlags();
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
      handler(this, dec.ops);
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
