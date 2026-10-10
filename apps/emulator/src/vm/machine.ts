/**
 * apps/emulator/src/vm/machine.ts —— **执行核心**（★ 零 Node 依赖）
 *
 * ✅ 按"第几条指令"推进脚本、维护帧**槽数组 + `cur`** 与池、在等待门挡住时推进时钟；它是**引擎态**的持有者
 *    （因此进状态分区表 —— 见文件末尾的 `STATE_PARTITION`）。
 * ★★ **帧不是栈，是"槽数组 + cur"**：引擎的帧区有 40 个 per-cur 记录，`Engine[0x5D880]` 是**当前槽号**；
 *    `0x6 load-frame` 逐字是"保存旧 cur → `cur = op2` → 在槽 `op2` 上建记录 → **恢复旧 cur**"
 *    （锚 = EA `0x41C84E` / `0x41C854` / `0x41C89A` / `0x41C8A6`）⇒ "往任意槽写记录、装完再切回来"
 *    用栈（`frames.push`）**表达不了**。栈模型的另一半后果：`pushFrame` 拿"栈深"当 `cur`，
 *    于是槽号只能等于深度 —— 而引擎里 `0x3 call-script` 是 `cur++` 后装载、`0x6` 是**任意槽**。
 * ❌ 不知道文件从哪来（那是 `host/fs.ts`）、不知道画到哪去（那是宿主的能力）；**不含**任何镜像偏移/EA。
 *
 * ★ 主循环写成**显式两态**：门关着 ⇒ `tick()`（时钟前进 + 场景推进，门可能因此变开），
 *   门开着 ⇒ `step()`（派发一条指令）；`0x21C wait` 就是那个门的开关。口径与理由见知识台账：
 *   `data/ledger/`（域 `Emulator`，subject `vm/machine-two-state-loop-wait-gate`）。
 *
 * ★ `ip` 的语义是**指令下标**（不是字节地址）：引擎按 dword 偏移推进（每条指令推进 `2*argc+1`
 *   个 dword，那个数它自己写进帧里），本模型的推进规则是"handler 没改过就 +1" ⇒ 一个偏移都没有；
 *   代价是控制流 handler **必须显式**改 `ip`（刻意的：跳转是一次语义动作，不该藏在算术里）。
 * ★ `ScriptFrame` 里**没有**脚本对象，只有 `scriptName`（+ `ip`）：脚本内容在 `Machine.scripts` 里 ——
 *   它是**外部内容**（与文件系统同类），不是引擎态：快照里不该塞进整份脚本字节。
 * ★ 可变字段一律**公开**（不用 `#`）。口径与理由见知识台账：`data/ledger/`
 *   （域 `Emulator`，subject `model/state-partition-reflective`）。
 */

import { GlobalPools, LocalPools } from '../model/pools.ts';
import { SceneModel } from '../model/scene.ts';
import { EngineScalars } from '../model/engine-scalars.ts';
import { AddressSpace } from '../model/address-space.ts';
import { OPCODE_TABLE } from '@amayui/age-format/src/asm/runtime.mts';
import type { EffectDisposition, EffectRecord } from '../host/effects.ts';
import type { Instance } from '../host/instance.ts';
import type { Instr } from '../model/iterate.ts';
import { loadScript, localCountList } from './script.ts';
import type { LoadedScript } from './script.ts';
import { HANDLERS, ExitScript } from './ops.ts';
import type { VmContext } from './ops.ts';

/**
 * 帧上的标志位。★ 目前只建模了**一个**：等待门。其余（跳读/自动/共存消息…）都还没有承载面 ——
 *   登记在需求树，不在这里编一个数。
 */
export const FRAME_FLAGS = {
  /** `0x21C wait` 置位；门开之前主循环**不派发**脚本指令 */
  WAIT_GATE: 0x400,
} as const;

/**
 * 帧**槽**数（= 知识层 `FRAME_LAYOUT.count` 的 40）。
 *
 * ★ 两个用它的人，同一个事实、含义不同：`0x6 load-frame` 检查 `op2 >= FRAME_SLOT_COUNT` ⇒ 抛那句
 *   「ファイルの階層が深すぎます．最大は%dです．」（逐字 `0x41C85A cmp eax,28h` / `0x41C85D jl`），
 *   而**模型**这边是"`slots` 数组不许被写到第 40 格"。
 * ★ 它**不是**偏移/EA：是本层的数组上界（这个布局数字属于知识层）。
 */
export const FRAME_SLOT_COUNT = 40;

/** "门在等什么"的分类（**闭集合**；`'none'` = 压根没有判据来源，那要记账，不许当成"没有动画"） */
export type WaitReason = 'scene-window' | 'host' | 'none';

/**
 * 一段脚本的执行状态（**引擎态**）。
 * ★ 没有脚本对象、没有字节数组 —— 只有"我叫什么、走到第几条、我这一帧的池、门在等什么"。
 * ★ 它是**某一号槽上的帧记录**（见 `Machine.slots` / `Machine.cur`）：引擎每帧一份记录，
 *   `0x6 load-frame` 能在**任意槽**上建一份（`cur` 只是"现在是哪一号"）。
 */
export class ScriptFrame {
  /** 脚本名（引擎侧的名字，例如 `LOGO.BIN`） */
  readonly scriptName: string;
  /** **槽号**（引擎里 `0x5D880` 的 `cur` 就是这个数：`0..39`；本模型也用它算地址无关的身份） */
  readonly cur: number;
  /** 本帧的 local 池 */
  readonly locals: LocalPools;
  /**
   * ★ **本帧的 6 个 local 计数**（脚本头那 6 个 `local_*` 声明，**按池序**：第 i 项 = 第 i 个 local 池）。
   *
   * 为什么在**帧**上而不在池定义上：引擎把它们写进**每帧一份的帧记录**里
   * （知识层 `LOCAL_POOL_SLOTS[i].count` = 记录 `+0x08 + 4i`（绝对 `0x5D89C + 4i`）；装载器 `sub_40ED40` 的"读计数 → `operator new[]` → 写基址"
   * 三连，逐字见 `layout.mts` 的 `LOCAL_POOL_SLOTS` 头注），而**池定义**里带 `count` 是
   * `tools/test/emulator-model.test.mjs` 明令不许的（"记录内计数槽属于布局层"）。
   * ★ "声明顺序 = 池序"这条**位置对应**由守卫 `tools/test/emulator-model.test.mjs` 的
   *   「脚本头 6 个 local 声明 … 位置对应」用例钉住（语料判据：第 i 处计数 store 落在 `记录+0x08+4i`）。
   * ★ 记录基址 = `0x5D894`（知识层 `FRAME_LAYOUT.base`）；旧基址（`0x5D880`）记法要 **+0x14**。
   * ★ 缺省 `[]` = "这份记录不是按脚本头建的"（例如测试桩）—— **不许**拿它当"计数是 0"。
   */
  readonly localCounts: readonly number[];
  /** 当前指令下标 */
  ip = 0;
  /** 标志位（见 `FRAME_FLAGS`） */
  flags = 0;
  /** 调用者帧序号；`-1` = 没有调用层（顶层） */
  caller = -1;
  /** 等待门是**什么时候**被置起来的（毫秒）；`null` = 门没在等 */
  waitSinceMs: number | null = null;
  /** 门在等什么（`wait` 那一刻的快照） */
  waitReason: WaitReason = 'none';
  /**
   * ★ **本帧的子程序返回栈**（`0x8f call` 压、`0x05 ret` 弹）。
   *
   * 取证（锚 = EA）：引擎在帧内维护一对结构 —— `帧+0x5EE04`（"层数"计数器）与
   * `帧+0x5EEA4`（**每帧 256 槽**的返回点表，`256*cur` 就是它在 `Engine` 里的 stride）。
   * `call` 压入 `(PC−脚本基址)>>2 + 3`（= 调用点的**下一条**），`ret` 弹出来写回 PC。
   * ⇒ **两者都是同帧的**，与 `exit`（回到上一层帧）是两回事。
   *
   * ★ 这里存的是**指令下标**（不是字节偏移）：本模型没有字节 PC，`ip` 就是"第几条"。
   *   引擎压的 `序号+3`（3 = `call` 本身占的 dword 数）换算过来恰好是 `ip + 1`。
   */
  readonly returnStack: number[] = [];

  /**
   * @param space 地址空间（ADR 第 ② 步）—— 给了它，本帧的 `int`/`ptr` 池就**存在区域里**（一份数据）。
   *   缺省 `null` ⇒ 走老的 `Map` 路径（迁移中途的兼容路径；终态是"只有区域"）。
   * @param localCounts 本帧的 6 个 local 计数（按池序；见字段注）。缺省 `[]` = 未给（⛔ 不等于"都是 0"）。
   */
  constructor(
    scriptName: string, cur: number, key: number, caller = -1, space: AddressSpace | null = null,
    localCounts: readonly number[] = [],
  ) {
    this.scriptName = scriptName;
    this.cur = cur;
    this.locals = new LocalPools(key, undefined, { space });
    this.caller = caller;
    this.localCounts = [...localCounts];
  }

  /**
   * 规范化快照（纯数据）。★ `engine` 类字段恰好就是它的顶层键（由守卫核）—— 口径与理由见知识台账：
   * `data/ledger/`（域 `Emulator`，subject `model/state-partition-engine-is-snapshot-keys`）。
   * ★ `scriptName` 与 `cur` **在**快照里：恢复一段执行必须知道"跑的是哪份脚本、这是第几号帧"，
   *   否则恢复出来的实例连下一步该派发哪条都不知道。
   * ★ `localCounts` 也在：引擎把它放在**帧记录里**（记录 `+0x08..+0x1C`，见字段注）⇒ 它是引擎态；
   *   不放进快照就等于"恢复后这份记录的计数没了"，而它决定池的几何。
   */
  snapshot(): {
    scriptName: string; cur: number;
    ip: number; flags: number; caller: number; waitSinceMs: number | null; waitReason: WaitReason;
    locals: ReturnType<LocalPools['snapshot']>;
    returnStack: number[];
    localCounts: number[];
  } {
    return {
      scriptName: this.scriptName, cur: this.cur,
      ip: this.ip, flags: this.flags, caller: this.caller,
      waitSinceMs: this.waitSinceMs, waitReason: this.waitReason,
      locals: this.locals.snapshot(),
      returnStack: [...this.returnStack],
      localCounts: [...this.localCounts],
    };
  }

  /** `0x8f call` 压一个返回点（**指令下标**，= 调用点的下一条） */
  pushReturn(index: number): void {
    this.returnStack.push(index);
  }

  /**
   * `0x05 ret` 弹一个返回点。
   * ★ 空栈 ⇒ `null`：引擎在空栈时**直接 retn**（`cmp edx,0FFFFFFFFh / jz locret_41AA4D`）——
   *   既不改 PC、也不换帧。⇒ 调用方**什么都不做**，而不是"没得返就退出"。
   */
  popReturn(): number | null {
    const v = this.returnStack.pop();
    return v === undefined ? null : v;
  }
}

/** 机器级的诊断（**不进快照**：不是引擎态，而且会随步数无限涨） */
export interface MachineDiagnostics {
  /** 已派发的指令条数 */
  steps: number;
  /** 停下来的原因（一行） */
  stopReason: string;
  /** "引擎不会检查"的访问留痕（计数；不存明细 —— 明细会随步数无限涨） */
  oobByKind: Map<string, number>;
}

/** `run()` 的结果 */
export interface RunResult {
  steps: number;
  /** 过了多少帧（= 虚拟时间 / 帧步长） */
  ticks: number;
  reason: StopReason;
  atMs: number;
  frame: number;
  /** 最后执行的那条指令（可能没有） */
  last: { opcode: number; name: string; index: number } | null;
}

/** 停下来的原因（闭集合 —— 下游按它做断言） */
export type StopReason =
  /** 命中调用方给的停止条件（**该指令已经执行完**） */
  | { kind: 'instruction'; opcode: number; name: string; index: number }
  /** 顶层脚本 `exit`（程序退出） */
  | { kind: 'exit' }
  /** `ip` 越出指令区（脚本尾，或跳转跳飞了） */
  | { kind: 'ip-out-of-range'; ip: number }
  /** 步数上限 */
  | { kind: 'max-steps'; limit: number }
  /** handler 抛了（**不吞**：原样带出来，并附上当时的位置） */
  | { kind: 'error'; message: string; script: string; opcode: number; name: string; index: number };

/** `run()` 的选项 */
export interface RunOptions {
  /** 步数上限（缺省取环境里的 `maxSteps`） */
  maxSteps?: number;
  /**
   * 命中这个 opcode 时停下（**该指令会先被执行**，于是它的副作用在日志里）。
   * ★ 为什么是 opcode 而不是"第几条"：opcode 是**内容**（脚本改了它还在），下标是**位置**
   *   （改一行就全变）。用位置当停止条件 = 停止线会随无关改动漂移。
   */
  stopAtOpcode?: number | null;
  /** 帧数上限（防止门永远不开时空转） */
  maxTicks?: number;
}

/**
 * 执行核心。**引擎态**：`globals` / `slots` / `cur` / `scene` / `frameNo`。
 *
 * ★ `scripts` 与 `instance` 是**宿主类**字段：前者是外部内容，后者是注入的服务。
 */
export class Machine {
  /** 注入的宿主服务（文件系统 / 副作用 / 时钟 / 配置 / 可选能力） */
  readonly instance: Instance;
  /** 引擎级池（跨脚本保留、不随装载脚本帧而重建） */
  readonly globals: GlobalPools;
  /**
   * ★★ **帧槽数组**（引擎的 40 个 per-cur 帧记录；空槽是 `null`）。
   *
   * 形状为什么是"槽"而不是"栈"：`0x6 load-frame` 的逐字是**在槽 `op2` 上建记录、装完恢复旧 `cur`**
   * （锚 = EA `0x41C84E`（存旧 cur）· `0x41C854`（`cur = op2`）· `0x41C89A`（`call sub_40ED40` 建记录）
   * · `0x41C8A6`（恢复 cur））⇒ "往**任意**槽写、写完切回来"这件事，栈（`push`/`pop` 只能动末端）
   * 表达不了；而且栈模型会把"槽号"和"深度"绑死。
   * ★ 槽是**稀疏**的：`slots[26]` 有记录时 8..25 可以是空的（预装帧就是这样）。
   * 快照要带上**整条**数组（含空槽）—— 见 `snapshot()`。
   */
  readonly slots: (ScriptFrame | null)[];
  /**
   * **当前槽号**（引擎 `Engine[0x5D880]` 那一格；`slots[cur]` 就是活动帧）。
   * ★ 初值 `-1` = "还没有活动帧"：`pushFrame` 是 `slots[++cur]`，所以**根脚本落在槽 0**
   *   （与引擎一致：`0x5D880` 是槽号，根脚本占 0 号槽）。
   */
  cur: number;
  /** 场景模型（绘制项 / 纹理槽 / 网格 / 计时窗）——**引擎态**，见 `model/scene.ts` 头注 */
  readonly scene: SceneModel;
  /** 已装载脚本的缓存（外部内容，不是引擎态） */
  readonly scripts: Map<string, LoadedScript>;
  /**
   * ★ **引擎标量槽**（`model/engine-scalars.ts`）—— 启动链前段那批"读操作数 → 写一个引擎标量"的
   * handler 就写在这里。它是**引擎态**（快照要带上，否则恢复后那些槽会静默变回 0）。
   * ⛔ 本层只存值、不解释槽的含义（名字来自知识层 `layout.mts` 的 `ENGINE_SCALAR_WRITES`）。
   * 口径与理由见知识台账：`data/ledger/`（域 `Emulator`，subject `model/engine-scalars-names-from-knowledge`）。
   */
  readonly scalars: EngineScalars;
  /**
   * ★★ **引擎的地址空间**（`model/address-space.ts`）—— 指针族（operand type 6/7/8/c/d/e）与
   * "按地址连写 N 格"（`fill-zero`/`set-array-to`/`lookup-array`）要用的那块地基。形状与理由
   * （扁平 / 按分配顺序发号 / 稀疏 / 窗口独占 / u32 预算）见知识台账：`data/ledger/`
   * （域 `Emulator`，subject `model/address-space-flat`）。
   *
   * ★ 现状（诚实写清）：**已建、已进快照、增长已接到副作用日志**，但**池还没有绑到它上面**
   * ⇒ 目前没有 handler 真的往里写。这是 ADR `REQ-01M4ARC3CPM00CC1KC4Q3HF550` 第 ② 步的**半步**：
   * 下一步是把 int/ptr 池的存储换成这里的区域（一份数据），判据 = 既有守卫全绿 +
   * "经地址空间读到的池值 == 经池 API 读到的值"；容量策略见决策
   * `REQ-01M4B969TBWVERFCB1MXS2Q2E1`（初值 0 + 按需增长、**每次增长留痕**）。
   */
  readonly space: AddressSpace;
  /** 诊断 */
  readonly diag: MachineDiagnostics;
  /** 帧号（主循环的"第几帧"） */
  frameNo = 0;
  /** 副作用记录的序号源（**机器发号**，不靠 sink 的内部长度 —— 那个会被 `drain()` 打乱） */
  effectCounter = 0;
  /** 这一次等待门的"已经记过 block 了"标志（免得每帧记一条，把日志淹掉） */
  gateBlockLogged = false;

  constructor(instance: Instance) {
    this.instance = instance;
    // ★ 顺序要求：**先建地址空间**，再建池 —— 池的 int/ptr 族要绑到它的区域上（ADR 第 ② 步）。
    this.space = new AddressSpace({
      onGrow: (e) => this.effect('system', 'region.grow', 'modeled', {
        region: e.tag, index: e.index, from: e.from, to: e.to, note: e.note,
      }),
    });
    this.globals = new GlobalPools(instance.env.codecKey, undefined, { space: this.space });
    this.slots = [];
    this.cur = -1;
    this.scene = new SceneModel();
    this.scalars = new EngineScalars();
    this.scripts = new Map();
    this.diag = { steps: 0, stopReason: '', oobByKind: new Map() };
  }

  // ── 基本访问 ──────────────────────────────────────────────────────────────

  /** 当前帧（槽 `cur` 上的那一份记录） */
  get frame(): ScriptFrame {
    const f = this.slots[this.cur];
    if (!f) throw new Error('没有活动帧：先 loadScriptBytes() 再 run()');
    return f;
  }

  /** 顶层脚本帧（槽 `0`） */
  get root(): ScriptFrame | undefined {
    return this.slots[0] ?? undefined;
  }

  /**
   * **活动调用链的帧数** = `cur + 1`。
   * ★ ⛔ 它**不数**那些不在链上的槽（`0x6 load-frame` 预装在槽 26 的记录不在活动链里）——
   *   所以别拿 `slots` 的长度当深度。
   */
  get depth(): number {
    return this.cur + 1;
  }

  get clockMs(): number {
    return this.instance.clock.nowMs();
  }

  /**
   * ★ **按统一文件 id 装载**（引擎自己的寻址方式）：`call-script <id>` 与"装载根脚本"都走它。
   *   与 `loadScriptBytes` 的分工：本函数"**从哪拿字节**"（问 `instance.scripts`），后者负责
   *   "拿到之后怎么建帧" ⇒ 两条路径（直装 / 启动链）共用同一份建帧逻辑。
   *   口径与理由（按 id 不按名字、取不到必须响亮失败）见知识台账：`data/ledger/`
   *   （域 `Emulator`，subject `host/scripts-by-file-id`）。
   * @param asRoot true = 压一个新帧（根脚本）；false = **只登记**进缓存（`call-script` 会自己压帧）
   */
  loadScriptById(id: number, opts: { asRoot?: boolean } = {}): LoadedScript {
    const loader = this.instance.scripts;
    if (!loader) {
      throw new Error(
        `本实例没有脚本来源（\`call-script\`/根脚本要按统一文件 id ${id} 取字节）—— ` +
        `见 host/scripts.ts 与 host/instance.ts 的 \`scripts\``,
      );
    }
    const got = loader.loadById(id);
    if (!got) throw new Error(`统一文件 id ${id}（0x${id.toString(16)}）取不到脚本（来源：${loader.label}）`);
    return this.loadScriptBytes(got.name, got.bytes, opts);
  }

  /** 当前帧的脚本（装载缓存里的那份） */
  scriptOf(frame: ScriptFrame = this.frame): LoadedScript {
    const s = this.scripts.get(frame.scriptName);
    if (!s) throw new Error(`脚本 ${frame.scriptName} 还没装载（帧记录里的名字与 scripts 缓存不同步）`);
    return s;
  }

  // ── 装载 ──────────────────────────────────────────────────────────────────

  /**
   * 从字节装载一份脚本（**不读文件**：字节由调用方给）。
   *
   * @param asRoot true（缺省）= 作为**顶层脚本**压一个新帧；false = 只登记进缓存（供 `call-script` 用）
   */
  loadScriptBytes(name: string, bytes: Uint8Array, opts: { asRoot?: boolean } = {}): LoadedScript {
    const script = loadScript(name, bytes, { table: OPCODE_TABLE });
    this.scripts.set(name, script);
    for (const note of script.notes) this.note('script-note', `${name} ${note}`);
    if (opts.asRoot ?? true) {
      this.pushFrame(name, -1, localCountList(script.header));
    }
    this.effect('system', 'script.load', 'modeled', {
      script: name,
      bytes: bytes.length,
      instructions: script.instructions.length,
      headerLen: script.headerLen,
    });
    return script;
  }

  /**
   * 在**下一个槽**（`slots[++cur]`）上建一份新帧记录（`call-script` 与装载根脚本共用）。
   * ★ `caller` 是**调用者帧的 `cur`**（引擎里它是记录 `+0x38`（绝对 `0x5D8CC`）那条回链；`exit`/`ret` 靠它回去）。
   *   顶层帧的 caller = `-1` ⇒ 顶层 `exit` = 程序退出（已取证：`-1` 既不是 -10 也不是 -11，
   *   引擎在那里抛 `Command_Exit_Exception`，主循环接住后关窗退出）。
   * ★ 与 `loadScriptAt()` 的分工：这一条走**调用链**（`cur+1`，槽号与深度同步增长），
   *   那一条是 `0x6 load-frame` 的**任意槽**（⛔ 不动 `cur`）。
   * @param localCounts 本帧的 6 个 local 计数（按池序）；没给 ⇒ 空（⛔ 不等于"都是 0"，见字段注）
   */
  pushFrame(scriptName: string, caller = -1, localCounts: readonly number[] = []): ScriptFrame {
    const slot = this.cur + 1;
    const f = new ScriptFrame(scriptName, slot, this.instance.env.codecKey, caller, this.space, localCounts);
    this.slots[slot] = f;
    this.cur = slot;
    return f;
  }

  /** 弹掉当前帧（`exit` 的正面支：`slots[cur--] = null`）；没有活动帧 ⇒ 抛（"没有帧"不是"回到顶层"） */
  popFrame(): ScriptFrame {
    const f = this.slots[this.cur];
    if (!f) throw new Error('没有活动帧：popFrame 没有可弹出的帧');
    this.slots[this.cur] = null;
    this.cur -= 1;
    return f;
  }

  /**
   * ★★ **在指定槽上建一份帧记录** —— `0x6 load-frame` 的核心，**不改 `cur`**。
   *
   * 逐字（锚 = EA，`sub_41C7C0`）：`0x41C84E mov [esi+5D884h],ecx`（存旧 cur）→
   * `0x41C854 mov [esi+5D880h],eax`（`cur = op2`）→ `0x41C89A call sub_40ED40`（**在槽 `cur` 上建记录**）
   * → `0x41C8A6 mov [esi+5D880h],ecx`（**恢复旧 cur**）。
   * ⇒ 本函数就是中间那一步的**净效果**：记录落在槽 `slot` 上、`cur` 一个字节都不动。
   *   "临时把 `cur` 切过去再切回来"是引擎实现那个净效果的**手段**，不是它的语义 —— 所以本层
   *   不需要真的切 `cur`（切了反而会让主循环在错的帧上跑）。
   *
   * ★ **建了什么**（已建的部分）：帧记录本身 —— 脚本名 / `cur` = 槽号 / **本帧自己的 local 池**
   *   （`LocalPools`，与活动帧的那份是**两个对象**）/ 脚本头那 6 个 local 计数（按池序）。
   * ★ **欠什么**（⛔ 不许当成"全建好了"）：`sub_40ED40` 里的其余字段与表 —— 三组 `(长度, 指针)`、
   *   `ip`（记录 `+0x04`）、记录 `+0x00` 的脚本缓冲、`array_container`(记录 `+0x70`) …
   *   那些在本模型里没有承载面 ⇒ `0x6` 每次仍发一条欠账记录（见 `vm/ops.ts` 的 `opLoadFrame`）。
   * ★ **池的初值**：引擎装载时给池填过初值（int 族是 `enc_zero`，见知识层 `EVIDENCE.encZero`），
   *   而"每个池填多少、填哪几格"**没有取证** ⇒ 这里**不填**（`LocalPools` 的容量本来就未知）；
   *   计数只**记在帧记录上**（`localCounts`），不冒充容量。
   *
   * @param slot 目标槽号（`0..FRAME_SLOT_COUNT-1`）
   * @param script 已装载的脚本（字节来源的选取在调用方 —— 与 `loadScriptById` 的分工同形）
   */
  loadFrameAt(slot: number, script: LoadedScript): ScriptFrame {
    if (!Number.isInteger(slot) || slot < 0 || slot >= FRAME_SLOT_COUNT) {
      throw new Error(`load-frame 的槽号 ${slot} 不在 0..${FRAME_SLOT_COUNT - 1} 之内 —— 这不是脚本的问题，是调用点算错了`);
    }
    const f = new ScriptFrame(script.name, slot, this.instance.env.codecKey, -1, this.space, localCountList(script.header));
    // ★ 槽是**一整条**数组（空槽是 `null`）：`slots[26]` 有记录时 8..25 显式是空槽，不是"洞"
    //   （洞在 JSON 往返里会变成 `null`，"快照逐字节相同"那条判据就说不清了）
    while (this.slots.length <= slot) this.slots.push(null);
    this.slots[slot] = f;
    return f;
  }

  // ── 记账（副作用 / 留痕） ─────────────────────────────────────────────────

  /** 发一条副作用记录（`seq` / `frame` / `atMs` 在这里补齐 —— 调用点只描述"发生了什么"） */
  effect(
    domain: EffectRecord['domain'],
    action: string,
    disposition: EffectDisposition,
    detail: Record<string, unknown> = {},
  ): void {
    const seq = this.effectCounter;
    this.effectCounter += 1;
    this.instance.effects.emit({ seq, frame: this.frameNo, atMs: this.clockMs, domain, action, disposition, detail });
  }

  /** 记一次"引擎不会检查"的访问（计数，不存明细 —— 明细会随步数无限涨） */
  note(kind: string, detail = ''): void {
    const key = detail ? `${kind}: ${detail}` : kind;
    this.diag.oobByKind.set(key, (this.diag.oobByKind.get(key) ?? 0) + 1);
  }

  /** 置等待门（`0x21C wait` 的语义面：置位 + 记下"此刻在等什么"） */
  setWaitGate(): WaitReason {
    const f = this.frame;
    const at = this.clockMs;
    const reason: WaitReason = this.scene.hasPendingWindows(at)
      ? 'scene-window'
      : this.instance.gate
        ? (this.instance.gate.pending() ? 'host' : 'none')
        : 'none';
    f.flags |= FRAME_FLAGS.WAIT_GATE;
    f.waitSinceMs = at;
    f.waitReason = reason;
    this.gateBlockLogged = false;
    return reason;
  }

  // ── 单步 ──────────────────────────────────────────────────────────────────

  /**
   * 执行一条指令。
   * @returns 停止原因（`null` = 正常执行完，可以继续）
   */
  step(): StopReason | null {
    const frame = this.frame;
    const script = this.scriptOf(frame);
    const ins: Instr | undefined = script.instructions[frame.ip];
    if (!ins) return { kind: 'ip-out-of-range', ip: frame.ip };

    const handler = HANDLERS[ins.opcode];
    const startIp = frame.ip;
    if (!handler) {
      // ★ 未知 opcode **不静默跳过**：跳过会让后面每条指令都在错的上下文里跑，而日志依然"正常"
      return {
        kind: 'error',
        message: `opcode 0x${ins.opcode.toString(16)} 没有 handler（本批未实现）`,
        script: frame.scriptName, opcode: ins.opcode, name: ins.name, index: ins.index,
      };
    }

    const ctx: VmContext = { machine: this, frame, script, ins };
    try {
      handler(ctx);
    } catch (err) {
      // ★ 顶层 `exit` **不是错误**：它是脚本的正常出口，主循环要把它翻译成停止原因 `exit`
      if (err instanceof ExitScript) return { kind: 'exit' };
      const message = err instanceof Error ? err.message : String(err);
      return { kind: 'error', message, script: frame.scriptName, opcode: ins.opcode, name: ins.name, index: ins.index };
    }
    this.diag.steps += 1;
    // ★ 推进规则：handler 没改过 `ip` ⇒ +1。改过 = 它做了一次跳转/调用/返回（那是显式语义动作）。
    if (frame.ip === startIp) frame.ip = startIp + 1;
    return null;
  }

  // ── 帧推进（等待门关着的时候） ────────────────────────────────────────────

  /**
   * 过一帧：时钟前进 → 场景推进（门可能因此变开）。
   * ★ **不**派发脚本指令 —— 那正是"门关着"的意思。
   */
  tick(): void {
    this.frameNo += 1;
    // ★ "有就推、没有就算"：虚拟时钟实现 `advance`（headless 走这条），真实时钟不实现（它自己会走）
    this.instance.clock.advance?.(this.instance.env.frameMs);
    const at = this.clockMs;
    // 锁存：排窗之后的**下一帧**才起算（于是"还没锁存"是一个能观察到的状态）
    for (const w of this.scene.latchWindows(at)) {
      this.effect('render', 'anim.window', 'modeled', {
        kind: w.kind, handle: w.handle, delayMs: w.window.delayMs, durMs: w.window.durMs, latchedAtMs: at,
      });
    }
    for (const w of this.scene.completeWindows(at)) {
      this.effect('render', 'anim.done', 'modeled', {
        kind: w.kind, handle: w.handle, toArgb: w.window.toArgb, doneAtMs: at,
      });
    }
  }

  /** 有没有呈现能力（headless ⇒ 没有；那**不是缺陷**而是它的语义） */
  get canPresent(): boolean {
    return this.instance.canPresent();
  }

  // ── 主循环 ────────────────────────────────────────────────────────────────

  /**
   * 跑到停止条件为止：每一步要么**派发一条指令**，要么**过一帧**。
   */
  run(opts: RunOptions = {}): RunResult {
    const maxSteps = opts.maxSteps ?? this.instance.env.maxSteps;
    const maxTicks = opts.maxTicks ?? this.instance.env.maxSteps;
    const stopOpcode = opts.stopAtOpcode ?? null;
    let last: RunResult['last'] = null;
    let reason: StopReason = { kind: 'max-steps', limit: maxSteps };
    let steps = 0;

    for (;;) {
      if (steps >= maxSteps) { reason = { kind: 'max-steps', limit: maxSteps }; break; }
      if (this.frameNo > maxTicks) { reason = { kind: 'max-steps', limit: maxTicks }; break; }

      // —— 1. 等待门 ——
      const frame = this.frame;
      if (frame.flags & FRAME_FLAGS.WAIT_GATE) {
        const now = this.clockMs;
        const scenePending = this.scene.hasPendingWindows(now);
        const hostPending = this.instance.gate ? this.instance.gate.pending() : false;
        if (scenePending || hostPending) {
          if (!this.gateBlockLogged) {
            this.gateBlockLogged = true;
            this.effect('system', 'gate.block', frame.waitReason === 'none' ? 'not-provided' : 'modeled', {
              atMs: now, waitingOn: frame.waitReason,
              scenePending, hostPending,
            });
          }
          this.tick();
          continue;
        }
        frame.flags &= ~FRAME_FLAGS.WAIT_GATE;
        const since = frame.waitSinceMs ?? now;
        this.effect('system', 'gate.open', frame.waitReason === 'none' ? 'not-provided' : 'modeled', {
          waitedMs: now - since, waitingOn: frame.waitReason,
        });
        frame.waitSinceMs = null;
        continue;
      }

      // —— 2. 派发一条 ——
      const ins = this.scriptOf(frame).instructions[frame.ip];
      if (!ins) { reason = { kind: 'ip-out-of-range', ip: frame.ip }; break; }
      const outcome = this.step();
      if (outcome) { reason = outcome; break; }
      steps += 1;
      last = { opcode: ins.opcode, name: ins.name, index: ins.index };
      if (stopOpcode !== null && ins.opcode === stopOpcode) {
        reason = { kind: 'instruction', opcode: ins.opcode, name: ins.name, index: ins.index };
        break;
      }
    }

    this.diag.stopReason = describeStop(reason);
    return { steps, ticks: this.frameNo, reason, atMs: this.clockMs, frame: this.frameNo, last };
  }

  // ── 快照 ──────────────────────────────────────────────────────────────────

  /**
   * 规范化快照（纯数据）。★ `engine` 类字段恰好就是它的顶层键（由守卫核；口径见 `ScriptFrame.snapshot` 的指针）
   * ★★ 帧部分带的是 **`cur` + 整条 `slots`（含空槽）**：只存"活动链"是不够的 ——
   *   `0x6 load-frame` 会把记录放进**不在链上**的槽（实测启动链：槽 26/28/29/30/31），
   *   丢掉它们等于"恢复后那些预装记录凭空消失"。空槽用 `null` **显式**占位（不是洞）⇒ JSON 往返逐字节相同。
   */
  snapshot(): {
    globals: ReturnType<GlobalPools['snapshot']>;
    slots: (ReturnType<ScriptFrame['snapshot']> | null)[];
    cur: number;
    scene: ReturnType<SceneModel['snapshot']>;
    scalars: ReturnType<EngineScalars['snapshot']>;
    space: ReturnType<AddressSpace['snapshot']>;
    frameNo: number;
  } {
    return {
      globals: this.globals.snapshot(),
      slots: Array.from({ length: this.slots.length }, (_, i) => this.slots[i]?.snapshot() ?? null),
      cur: this.cur,
      scene: this.scene.snapshot(),
      scalars: this.scalars.snapshot(),
      space: this.space.snapshot(),
      frameNo: this.frameNo,
    };
  }
}

/** 停止原因 → 一行（日志与断言都用它，避免两处各写一遍措辞） */
export function describeStop(r: StopReason): string {
  switch (r.kind) {
    case 'instruction': return `停在指令 0x${r.opcode.toString(16)}（${r.name || '无名'}，第 ${r.index} 条）—— 已执行`;
    case 'exit': return '顶层脚本 exit（程序退出）';
    case 'ip-out-of-range': return `ip ${r.ip} 越出指令区`;
    case 'max-steps': return `达到上限 ${r.limit}`;
    case 'error': return `handler 抛错：${r.message}（**${r.script}** 的第 ${r.index} 条 = 0x${r.opcode.toString(16)} ${r.name || '无名'}）`;
  }
}

/**
 * ★ 状态分区（**可执行形式**）：类名 → 字段名 → 类别。
 *
 * 口径与 `model/pools.ts` 的同一张表一致：**`engine` 类的字段恰好就是快照的顶层键**。口径与理由
 * 见知识台账：`data/ledger/`（域 `Emulator`，subject `model/state-partition-engine-is-snapshot-keys`）。
 * 这里只列**持有引擎态**的类：
 * * `Machine` —— `globals` / `slots` / `cur` / `scene` / `scalars` / `space` / `frameNo` 是引擎态；
 *   `instance`（注入的服务）与 `scripts`（外部内容）是宿主类；`diag` / `effectCounter` / `gateBlockLogged` 是诊断。
 * * `ScriptFrame` —— 七个字段全是引擎态；它**没有**宿主字段（脚本内容在 `Machine.scripts` 里）。
 * ★ 本表由 `tools/test/emulator-state-partition.test.mjs` 反射核对（新增可变字段忘了归类就红）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  Machine: {
    instance: 'host', scripts: 'host',
    diag: 'diagnostic', effectCounter: 'diagnostic', gateBlockLogged: 'diagnostic',
    globals: 'engine', slots: 'engine', cur: 'engine', scene: 'engine', scalars: 'engine', space: 'engine', frameNo: 'engine',
  },
  ScriptFrame: {
    scriptName: 'engine', cur: 'engine', locals: 'engine',
    ip: 'engine', flags: 'engine', caller: 'engine', waitSinceMs: 'engine', waitReason: 'engine',
    returnStack: 'engine',
    localCounts: 'engine',
  },
};
