/**
 * apps/emulator/src/vm/machine.ts —— **执行核心**（★ 零 Node 依赖）
 *
 * ## 它是什么 / 不是什么
 * ✅ 它按"第几条指令"推进脚本、维护帧栈与池、在等待门挡住时推进时钟。
 * ✅ 它是**引擎态**的持有者（因此进状态分区表 —— 见文件末尾的 `STATE_PARTITION`）。
 * ❌ 它**不**知道文件从哪来（那是 `host/fs.ts`）、**不**知道画到哪去（那是宿主的能力）。
 * ❌ 它**不含**任何镜像偏移/EA（本仓硬口径）。
 *
 * ## ★ 主循环的形状（为什么"执行一条指令"与"过一帧"是两件事）
 * 引擎的主循环每帧做两件事之一：**派发脚本指令**，或者（等待门挡住时）**继续跑这一帧的其它事**。
 * `0x21C wait` 就是那个门的开关：置位之后，主循环在门开之前**不再派发脚本指令**。
 * ⇒ 写成显式两态循环：
 * ```
 * 门关着？ ── 是 ──▶ tick()：时钟前进、场景推进（门可能因此变开）
 *           └─ 否 ──▶ step()：执行一条指令
 * ```
 * ★ 为什么不许把 `wait` 写成"no-op 直接过"：版权页那 5 秒的窗**就是**这个门在等的东西。
 *   把门删掉 = 把那 5 秒连同"窗有没有跑完"这条判据一起删掉 —— 那不是加速，是把观测面做没了。
 *
 * ## ★ ip 的语义：**指令下标**，不是字节地址
 * 引擎按 dword 偏移推进（每条指令推进 `2*argc+1` 个 dword，那个数它自己写进帧里）。
 * 本模型里 `ip` 是**第几条指令**，推进规则是"handler 没改过就 +1"。
 * ⇒ 模型里一个偏移都没有；代价是控制流 handler **必须显式**改 `ip`
 *   （这是刻意的：跳转是一次语义动作，不该藏在算术里）。
 *
 * ## ★ 帧与脚本分离
 * `ScriptFrame` 里**没有**脚本对象，只有 `scriptName`（+ `ip`）。脚本内容在 `Machine.scripts` 里 ——
 * 它是**外部内容**（与文件系统同类），不是引擎态：快照里不该塞进整份脚本字节。
 *
 * ## ★ 可变字段一律**公开**（不用 `#`）
 * 状态分区守卫靠**反射**核"每个可变字段都表了态"。`#private` 字段对反射**不可见** ——
 * 用它藏可变状态 = 让那条守卫静默失效。⇒ 本类不藏：每个可变字段都在文末的表里。
 */

import { GlobalPools, LocalPools } from '../model/pools.ts';
import { SceneModel } from '../model/scene.ts';
import { OPCODE_TABLE } from '@amayui/age-format/src/asm/runtime.mts';
import type { EffectDisposition, EffectRecord } from '../host/effects.ts';
import type { Instance } from '../host/instance.ts';
import type { Instr } from '../model/iterate.ts';
import { loadScript } from './script.ts';
import type { LoadedScript } from './script.ts';
import { HANDLERS, ExitScript } from './ops.ts';
import type { VmContext } from './ops.ts';

/**
 * 帧上的标志位。
 * ★ 目前只建模了**一个**：等待门。其余（跳读/自动/共存消息…）都还没有承载面 ——
 *   登记在需求树，不在这里编一个数。
 */
export const FRAME_FLAGS = {
  /** `0x21C wait` 置位；门开之前主循环**不派发**脚本指令 */
  WAIT_GATE: 0x400,
} as const;

/** "门在等什么"的分类（**闭集合**；`'none'` = 压根没有判据来源，那要记账，不许当成"没有动画"） */
export type WaitReason = 'scene-window' | 'host' | 'none';

/**
 * 一段脚本的执行状态（**引擎态**）。
 * ★ 没有脚本对象、没有字节数组 —— 只有"我叫什么、走到第几条、我这一帧的池、门在等什么"。
 */
export class ScriptFrame {
  /** 脚本名（引擎侧的名字，例如 `LOGO.BIN`） */
  readonly scriptName: string;
  /** 帧序号（引擎里是 `0..39` 的 `cur`；模拟器只用它做**身份**，不用它算地址） */
  readonly cur: number;
  /** 本帧的 local 池 */
  readonly locals: LocalPools;
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

  constructor(scriptName: string, cur: number, key: number, caller = -1) {
    this.scriptName = scriptName;
    this.cur = cur;
    this.locals = new LocalPools(key);
    this.caller = caller;
  }

  /**
   * 规范化快照（纯数据）。★ `engine` 类字段恰好就是它的顶层键（由守卫核）。
   * ★ `scriptName` 与 `cur` **在**快照里：恢复一段执行必须知道"跑的是哪份脚本、这是第几号帧"，
   *   否则恢复出来的实例连下一步该派发哪条都不知道。
   */
  snapshot(): {
    scriptName: string; cur: number;
    ip: number; flags: number; caller: number; waitSinceMs: number | null; waitReason: WaitReason;
    locals: ReturnType<LocalPools['snapshot']>;
    returnStack: number[];
  } {
    return {
      scriptName: this.scriptName, cur: this.cur,
      ip: this.ip, flags: this.flags, caller: this.caller,
      waitSinceMs: this.waitSinceMs, waitReason: this.waitReason,
      locals: this.locals.snapshot(),
      returnStack: [...this.returnStack],
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
 * 执行核心。**引擎态**：`globals` / `frames` / `scene` / `frameNo`。
 *
 * ★ `scripts` 与 `instance` 是**宿主类**字段：前者是外部内容，后者是注入的服务。
 */
export class Machine {
  /** 注入的宿主服务（文件系统 / 副作用 / 时钟 / 配置 / 可选能力） */
  readonly instance: Instance;
  /** 引擎级池（跨脚本保留、不随装载脚本帧而重建） */
  readonly globals: GlobalPools;
  /** 帧栈（`frames[0]` 是顶层） */
  readonly frames: ScriptFrame[];
  /** 场景模型（绘制项 / 纹理槽 / 网格 / 计时窗）——**引擎态**，见 `model/scene.ts` 头注 */
  readonly scene: SceneModel;
  /** 已装载脚本的缓存（外部内容，不是引擎态） */
  readonly scripts: Map<string, LoadedScript>;
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
    this.globals = new GlobalPools(instance.env.codecKey);
    this.frames = [];
    this.scene = new SceneModel();
    this.scripts = new Map();
    this.diag = { steps: 0, stopReason: '', oobByKind: new Map() };
  }

  // ── 基本访问 ──────────────────────────────────────────────────────────────

  /** 当前帧（栈顶） */
  get frame(): ScriptFrame {
    const f = this.frames[this.frames.length - 1];
    if (!f) throw new Error('没有活动帧：先 loadScriptBytes() 再 run()');
    return f;
  }

  /** 顶层脚本帧（`frames[0]`） */
  get root(): ScriptFrame | undefined {
    return this.frames[0];
  }

  get clockMs(): number {
    return this.instance.clock.nowMs();
  }

  /**
   * ★ **按统一文件 id 装载**（引擎自己的寻址方式）：`call-script <id>` 与"装载根脚本"都走它。
   *
   * 与 `loadScriptBytes` 的分工：本函数负责"**从哪拿字节**"（问 `instance.scripts`），
   * 后者负责"拿到之后怎么建帧"。⇒ 两条路径（直装 / 启动链）共用同一份建帧逻辑。
   *
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
    if (!s) throw new Error(`脚本 ${frame.scriptName} 还没装载（frames 里的名字与 scripts 缓存不同步）`);
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
      this.pushFrame(name, -1);
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
   * 压一个**新帧**（`call-script` 与装载根脚本共用）。
   * ★ `caller` 是**调用者帧的 `cur`**（引擎里它是 `帧+0x4C` 那条回链；`exit`/`ret` 靠它回去）。
   *   顶层帧的 caller = `-1` ⇒ 顶层 `exit` = 程序退出（已取证：`-1` 既不是 -10 也不是 -11，
   *   引擎在那里抛 `Command_Exit_Exception`，主循环接住后关窗退出）。
   */
  pushFrame(scriptName: string, caller = -1): ScriptFrame {
    const f = new ScriptFrame(scriptName, this.frames.length, this.instance.env.codecKey, caller);
    this.frames.push(f);
    return f;
  }

  /** 弹掉当前帧（`exit`/`ret` 的正面支）；栈空 ⇒ 抛（"没有帧"不是"回到顶层"） */
  popFrame(): ScriptFrame {
    const f = this.frames.pop();
    if (!f) throw new Error('帧栈空了：popFrame 没有可弹出的帧');
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

  /** 规范化快照（纯数据）。★ `engine` 类字段恰好就是它的顶层键（由守卫核） */
  snapshot(): {
    globals: ReturnType<GlobalPools['snapshot']>;
    frames: ReturnType<ScriptFrame['snapshot']>[];
    scene: ReturnType<SceneModel['snapshot']>;
    frameNo: number;
  } {
    return {
      globals: this.globals.snapshot(),
      frames: this.frames.map((f) => f.snapshot()),
      scene: this.scene.snapshot(),
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
 * 口径与 `model/pools.ts` 的同一张表一致（见那里的长注）：**`engine` 类的字段恰好就是快照的顶层键**。
 * 这里只列**持有引擎态**的类：
 * * `Machine` —— `globals` / `frames` / `scene` / `frameNo` 是引擎态；`instance`（注入的服务）与
 *   `scripts`（外部内容）是宿主类；`diag` / `effectCounter` / `gateBlockLogged` 是诊断。
 * * `ScriptFrame` —— 六个字段全是引擎态；它**没有**宿主字段（脚本内容在 `Machine.scripts` 里）。
 * ★ 本表由 `tools/test/emulator-state-partition.test.mjs` 反射核对（新增可变字段忘了归类就红）。
 */
export const STATE_PARTITION: Record<string, Record<string, string>> = {
  Machine: {
    instance: 'host', scripts: 'host',
    diag: 'diagnostic', effectCounter: 'diagnostic', gateBlockLogged: 'diagnostic',
    globals: 'engine', frames: 'engine', scene: 'engine', frameNo: 'engine',
  },
  ScriptFrame: {
    scriptName: 'engine', cur: 'engine', locals: 'engine',
    ip: 'engine', flags: 'engine', caller: 'engine', waitSinceMs: 'engine', waitReason: 'engine',
    returnStack: 'engine',
  },
};
