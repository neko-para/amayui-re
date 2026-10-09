/**
 * apps/emulator/src/host/instance.ts —— **实例装配与多实例隔离**（★ 核心层：零 Node 依赖）
 *
 * "实例" = 一次模拟运行所需的**全部宿主侧服务**（环境 + 两个根、fs、副作用 sink、时钟、配置、
 * 动画门、呈现器、输入、随机源、脚本、字体表）；它是 `src/vm/machine.ts` 那个引擎对象的**外圈**。
 *
 * ★★ 多实例隔离要落成**可枚举的机械判据**，不是"注意别共享"：两个实例之间不许有任何一处共享
 *   可变对象，用户根必须不同（共用会让"这个 bug 能不能复现"取决于跑的顺序，且不报任何错），
 *   安装根允许相同（只读来源）。本层**所有**输入都是构造参数、没有一处读进程状态。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/instance-no-shared-state`）。
 * ★ 能力缺失不许静默（`gate` / `present` ⇒ 记 `not-provided`；`writable` ⇒ 任何写操作抛）。
 * ★ `Instance` **不**进状态分区表：它的字段全是**注入进来的服务**（宿主类）—— 快照一个服务没有意义
 *   （恢复不该把调用者的闭包换掉）；引擎态在 `src/vm/machine.ts`（`Machine` 与 `ScriptFrame`）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/state-partition-engine-is-snapshot-keys`）。
 */

import type { Clock } from './clock.ts';
import type { ConfigStore } from './config.ts';
import type { EffectSink } from './effects.ts';
import type { LayeredFilesystem } from './fs.ts';
import type { RandomSource } from './random.ts';
import type { ScriptLoader } from './scripts.ts';
import type { EngineEnvironment } from './environment.ts';
import { assertInstanceId } from './environment.ts';

/**
 * "引擎还有没有没跑完的动画/转场"——引擎里它是主循环调的一族谓词（`0x400` 等待门的判决点）。
 *
 * ★ 它是**宿主提供**的，因为这条判据的输入（计时窗、转场表）属于渲染子系统；
 *   而**没有**它的时候必须能被看见（见文件头的能力缺失表）。
 */
export interface AnimationGate {
  readonly label: string;
  /** 有挂起的动画/转场 ⇒ true（此时主循环**不派发**脚本指令） */
  pending(): boolean;
}

/** 把"当前这一帧的画面"呈现出去的能力（像素 / 声音）。headless **故意不提供**。 */
export interface Presenter {
  readonly label: string;
  /** 呈现一帧（`atMs` = 虚拟时刻）。失败要么抛、要么在返回值里说清楚，**不许静默** */
  present(atMs: number): void;
}

/** 一次输入采样的结果（引擎侧的名字：按钮掩码 / 滚轮 / 鼠标位置） */
export interface InputSample {
  /** 按钮掩码（鼠标左键 = bit4 —— 见旧仓 `sub_477150`；本仓尚未逐字复核，故只作**透传**） */
  buttons: number;
  wheel: number;
  mouseX: number;
  mouseY: number;
}

/**
 * 输入来源（键盘 / 鼠标 / 手柄）。
 * ★ headless 通常**不提供**它 —— 于是 `0x101 poll-input` 记 `not-provided`；这与"提供一张永远
 *   返回 0 的假输入"是**两件不同的事**（后者会让"等一次点击"的循环看起来正常地空转）。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/effects-disposition-three-state`）。
 */
export interface InputSource {
  readonly label: string;
  /** 采一次样（**同步**：引擎的一条指令内就要拿到结果） */
  poll(): InputSample;
}

/** 装配一个实例所需的**全部**服务（都是注入的；没有一处读进程状态） */
export interface InstanceServices {
  env: EngineEnvironment;
  fs: LayeredFilesystem;
  effects: EffectSink;
  clock: Clock;
  config: ConfigStore;
  /** 动画门；不给 ⇒ 等待门立即放行，但**记 `not-provided`** */
  gate?: AnimationGate | null;
  /** 呈现器；不给 ⇒ 记 `not-provided`（headless 的正常形态） */
  present?: Presenter | null;
  /** 输入来源；不给 ⇒ `poll-input` 记 `not-provided`（headless 的正常形态） */
  input?: InputSource | null;
  /**
   * 随机源（`0x60 random`）。
   * ★ 不给 ⇒ 那条指令**响亮失败**（不是静默取 0、也不是偷偷用 `Math.random()`）——
   *   "非确定性"是**引擎态的一部分**，不许由核心自己变出来。
   *   ★ 而**真实前端必须给**：语料里 `0x60` 出现 22 次 ⇒ 游戏确实用到它。
   *     这条由守卫核（`tools/test/emulator-headless-logo.assets.test.mjs` 断言实例有随机源，
   *     且 label 以 `seeded(0x` 开头）。
   *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/random-seeded-injection`）。
   */
  random?: RandomSource | null;
  /**
   * 脚本来源（`call-script <id>` 执行期要字节）。
   * ★ 不给 ⇒ `call-script` **响亮失败** —— 因为引擎在**执行期**才去取脚本，
   *   "取不到"绝不许表现成"这份脚本是空的"（那会让后面每条指令都在错的上下文里跑）。
   */
  scripts?: ScriptLoader | null;
  /**
   * **宿主提供的字体名表**（`0x2de` 要查它）。
   *
   * 取证（锚 = EA）：`0x2de` 的体是
   * ```
   *   v2 = sub_41B640(this, 2)            ; ★ 第四个取值原语：操作数 → cp932 文本
   *   v3 = sub_428990(Engine+0x14D30, v2) ; 在元素 0x20 字节的 vector 里**线性扫描**（内含 std::string，
   *                                       ;   memcmp 精确相等、**跳过前导 '@'**）⇒ 命中给 0 基下标、否则 **-1**
   *   sub_42B4B0(this, 1, v3)             ; 写回 op1
   * ```
   * 那个 vector 是**字体名表**（判据：`0x459B56–0x459B6A` 拿 `LOGFONT+0x1C`（= `lfFaceName`）去查它）。
   * ⇒ **表里有什么是宿主事实**（这台机器装了哪些字体），不是引擎语义。
   * ★ 不给（`null`）⇒ 该条指令**响亮失败**（不许假装"表里什么都没有"）；
   *   给**空表**（`[]`）是一个**显式的宿主选择**，并且每次查找都会留痕。
   */
  fonts?: readonly string[] | null;
}

/**
 * 一个模拟实例。**构造即冻结**：字段都是 `readonly`，因为"跑到一半把 sink 换掉"
 * 会让日志出现两段不可比的记录（而那正是排查时最不想要的东西）。
 */
export class Instance {
  readonly id: string;
  readonly env: EngineEnvironment;
  readonly fs: LayeredFilesystem;
  readonly effects: EffectSink;
  readonly clock: Clock;
  readonly config: ConfigStore;
  readonly gate: AnimationGate | null;
  readonly present: Presenter | null;
  readonly input: InputSource | null;
  readonly random: RandomSource | null;
  readonly scripts: ScriptLoader | null;
  /** 宿主字体名表（`0x2de` 查它）；`null` = 没提供 ⇒ 那条指令响亮失败 */
  readonly fonts: readonly string[] | null;

  constructor(services: InstanceServices) {
    this.id = assertInstanceId(services.env.instanceId);
    this.env = services.env;
    this.fs = services.fs;
    this.effects = services.effects;
    this.clock = services.clock;
    this.config = services.config;
    this.gate = services.gate ?? null;
    this.present = services.present ?? null;
    this.input = services.input ?? null;
    this.random = services.random ?? null;
    this.scripts = services.scripts ?? null;
    this.fonts = services.fonts ?? null;
  }

  /** 这个实例有没有"动画未完成"的判据来源？（没有 ⇒ 等待门的行为必须被显式记账） */
  hasAnimationGate(): boolean {
    return this.gate !== null;
  }

  /** 有没有呈现能力？（headless ⇒ false，且这不是缺陷而是语义） */
  canPresent(): boolean {
    return this.present !== null;
  }
}

/**
 * **多实例注册表**：id → 实例。
 *
 * 它只做一件核心的事：**id 唯一**。看起来单薄，但它挡掉的是一类很难查的事故 ——
 * 两个实例共用一个 id ⇒ 落点、日志、存档目录全部重合，而**不会报任何错**
 * （表现是"偶尔读到上一次的存档"，而那取决于跑的顺序）。
 */
export class InstanceRegistry {
  readonly byId: Map<string, Instance>;

  constructor() {
    this.byId = new Map();
  }

  register(instance: Instance): Instance {
    const existing = this.byId.get(instance.id);
    if (existing) {
      throw new Error(
        `实例 id 已被占用：${instance.id} —— 两个实例共用一个 id 会让落点/日志/存档全部重合，` +
        `而且不会报任何错（表现是"偶尔读到上一次的存档"）`,
      );
    }
    this.byId.set(instance.id, instance);
    return instance;
  }

  get(id: string): Instance | undefined {
    return this.byId.get(id);
  }

  /** 全部实例（**id 升序** ⇒ 同样一组实例给出同样顺序） */
  list(): Instance[] {
    const ids = [...this.byId.keys()].sort();
    return ids.map((id) => this.byId.get(id) as Instance);
  }

  get size(): number {
    return this.byId.size;
  }

  /** 注销（幂等；返回"是否真的注销了一个"） */
  unregister(id: string): boolean {
    return this.byId.delete(id);
  }
}

/**
 * ★ **隔离判据的机械形式**：两个实例之间**不许有任何一处共享可变对象**。
 *
 * 共享面会随字段增加而增加，而"新加一个服务、忘了它要每实例一份"**不会报错** ⇒ 把"比哪些东西"
 * 变成一份**可枚举的清单**（而不是在守卫里手写一堆 `notEqual`），新增服务时在这里加一行。
 *   口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `host/instance-no-shared-state`）。
 */
export function sharedMutableState(a: Instance, b: Instance): string[] {
  const parts: [string, unknown, unknown][] = [
    ['fs', a.fs, b.fs],
    ['effects', a.effects, b.effects],
    ['clock', a.clock, b.clock],
    ['config', a.config, b.config],
    ['gate', a.gate, b.gate],
    ['present', a.present, b.present],
    ['input', a.input, b.input],
    ['random', a.random, b.random],
    ['scripts', a.scripts, b.scripts],
    ['fonts', a.fonts, b.fonts],
  ];
  const shared: string[] = [];
  for (const [name, x, y] of parts) {
    if (x !== null && x === y) shared.push(name);
  }
  // ★ **用户根必须不同**（同身份 ⇒ 后跑的会读到前一个写的存档/配置 ⇒ 复现性取决于跑的顺序）；
  //   **安装根可以相同**（只读来源 —— 见 `fs.ts` 的构造期"同身份即拒"）。
  if (a.env.user.identity.toLowerCase() === b.env.user.identity.toLowerCase()) shared.push('env.user.identity');
  return shared;
}
