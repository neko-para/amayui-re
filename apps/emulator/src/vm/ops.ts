/**
 * apps/emulator/src/vm/ops.ts —— **指令语义**（★ 零 Node 依赖）
 *
 * ## 本文件里每一条的"可信度"分成三档，**逐条标出来**（不许混着写）
 * | 档 | 含义 | 判据 |
 * |---|---|---|
 * | **A** | 本仓已登记的语义（有守卫回语料复核） | `apps/emulator/src/model/numeric-ops.ts` |
 * | **B** | 旧仓观测索引给了 handler 符号与语义，**本仓尚未逐字复核** | 注释里点名 handler；复核单见需求树 |
 * | **C** | 还没解出来 —— **不写**，遇到就抛 | 见 `unimplemented()` |
 * ★ 为什么把 B 档单独标出来：B 档的实现**可能是对的**，但它的可信度与 A 档不同 ——
 *   而"把 B 当 A 用"是本仓最容易发生的一次静默降级（K2/K3 整条知识线就是为这件事存在的）。
 *
 * ## ★ `disposition` 与 `detail.applied` 是**两个正交的事实**（别混）
 * * `disposition`（在 `host/effects.ts` 里定义）= **宿主有没有这张能力**：headless 没有播放器 ⇒
 *   `audio.movie.play` 记 `logged-only`；没有输入源 ⇒ `input.poll` 记 `not-provided`。
 * * `detail.applied` = **引擎态有没有因此改变**：例如 `set-vertex-color` 指到一个不存在的网格 ⇒
 *   `applied: false`（但宿主能力是齐的 ⇒ 仍是 `modeled`）。
 * ⇒ 两者混起来会出现"因为找不到网格，所以看起来像宿主没实现"这种无法排查的日志。
 *
 * ## ★ 操作数一律按 u32 读，符号由用它的那条指令决定
 * 见 `vm/operand.ts` 头注。颜色分量到处要做 `< 0 ⇒ 取当前值` 的回退，所以本文件里
 * `asInt32(...)` 出现得很频繁 —— 那不是噪音，那是**引擎的语义**。
 */

import type { InstrArg } from '../model/iterate.ts';
import type { LoadedScript } from './script.ts';
import type { Machine, ScriptFrame } from './machine.ts';
import { asFloat, asInt32, asUint32, readOperand, writeOperand } from './operand.ts';
import { instructionIndexAt, labelByteOffsetOf } from './script.ts';
import type { OperandValue } from './operand.ts';

/** 一个 handler 拿到的东西（**够用就好**：不要把手伸进 machine 的内部状态） */
export interface VmContext {
  machine: Machine;
  frame: ScriptFrame;
  script: LoadedScript;
  /** 当前指令 */
  ins: { opcode: number; name: string; argc: number; args: InstrArg[]; index: number };
}

/** handler 的形状 */
export type Handler = (ctx: VmContext) => void;

/** 未实现：**响亮失败**，并把"这是未取证"与"这是坏数据"分开 */
function unimplemented(ctx: VmContext, why: string): never {
  throw new Error(
    `指令 0x${ctx.ins.opcode.toString(16)}（${ctx.ins.name || '无名'}）在本批未实现：${why} —— ` +
    `这不是坏脚本，是我们的模型还没有这一段（见 apps/emulator/src/vm/ops.ts）`,
  );
}

// ───────────────────────────────────────────────────────── 操作数读取的小工具

/** 读第 `i` 个操作数的**数值**（int 族按 u32） */
function num(ctx: VmContext, i: number): number {
  const r = readOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script }, ctx.ins.args[i], i);
  if (r.value === null) {
    // ★ 未初始化：引擎的池在装载时被填过初值（`ENC(key,0)`），而**本仓还没取证"填多少、填哪几个池"**
    //   ⇒ 这里按 0 继续，但**每一次都留痕**：于是"这次跑依赖了未初始化的池"是可见的，不是静默的。
    ctx.machine.note('uninitialized-read', `${r.where}（指令 0x${ctx.ins.opcode.toString(16)} 操作数 #${i}）`);
    return 0;
  }
  return asUint32(r.value);
}

/** 读第 `i` 个操作数的**浮点值**（int 立即数 → 浮点；LOGO 的 `float-mov (global-float 9) 500` 就是这条） */
function fnum(ctx: VmContext, i: number): number {
  const r = readOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script }, ctx.ins.args[i], i);
  if (r.value === null) {
    ctx.machine.note('uninitialized-read', `${r.where}（浮点，指令 0x${ctx.ins.opcode.toString(16)} 操作数 #${i}）`);
    return 0;
  }
  return asFloat(r.value);
}

/** 写第 `i` 个操作数 */
function put(ctx: VmContext, i: number, value: OperandValue): void {
  writeOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script }, ctx.ins.args[i], i, value);
}

/** 第 `i` 个操作数的**槽位号**（数组基址那一族用它：引擎把"槽号"本身当基址，不去读槽里的值） */
const slotIndexOf = (ctx: VmContext, i: number): number => ctx.ins.args[i].rawData >>> 0;

// ───────────────────────────────────────────────────────── A 档：纯数值族

/** 二元整数运算（结果一律 u32；符号由各条自己决定） */
const binInt = (ctx: VmContext, f: (a: number, b: number) => number): void => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  put(ctx, 0, f(a, b) >>> 0);
};

/** 二元比较（结果 0 / 1） */
const cmpInt = (ctx: VmContext, f: (a: number, b: number) => boolean): void => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  put(ctx, 0, f(asInt32(a), asInt32(b)) ? 1 : 0);
};

const opAdd: Handler = (ctx) => binInt(ctx, (a, b) => a + b);
const opSub: Handler = (ctx) => binInt(ctx, (a, b) => a - b);
const opMul: Handler = (ctx) => binInt(ctx, (a, b) => Math.imul(a, b));
const opDiv: Handler = (ctx) => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  if (asInt32(b) === 0) throw new Error('div: 除数为 0（引擎在这一点上会抛）');
  put(ctx, 0, Math.trunc(asInt32(a) / asInt32(b)) >>> 0);
};
const opMod: Handler = (ctx) => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  if (asInt32(b) === 0) throw new Error('mod: 除数为 0（引擎在这一点上会抛）');
  put(ctx, 0, (asInt32(a) % asInt32(b)) >>> 0);
};
const opMov: Handler = (ctx) => put(ctx, 0, num(ctx, 1));
const opAnd: Handler = (ctx) => binInt(ctx, (a, b) => a & b);
const opOr: Handler = (ctx) => binInt(ctx, (a, b) => a | b);
const opSar: Handler = (ctx) => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  put(ctx, 0, (asInt32(a) >> (b & 31)) >>> 0);
};
const opShl: Handler = (ctx) => {
  const a = num(ctx, 1);
  const b = num(ctx, 2);
  put(ctx, 0, (a << (b & 31)) >>> 0);
};
const opEq: Handler = (ctx) => cmpInt(ctx, (a, b) => a === b);
const opNe: Handler = (ctx) => cmpInt(ctx, (a, b) => a !== b);
const opLt: Handler = (ctx) => cmpInt(ctx, (a, b) => a < b);
const opLte: Handler = (ctx) => cmpInt(ctx, (a, b) => a <= b);
const opGr: Handler = (ctx) => cmpInt(ctx, (a, b) => a > b);
const opGre: Handler = (ctx) => cmpInt(ctx, (a, b) => a >= b);

/** `bit-set` / `bit-reset`：位号 > 0x1F ⇒ 打错误串后继续，**不写 op1** */
const opBitSet: Handler = (ctx) => {
  const bit = num(ctx, 1);
  if (bit > 0x1f) { ctx.machine.note('bit-index-oob', `bit-set 位号 ${bit}`); return; }
  put(ctx, 0, (num(ctx, 0) | (1 << bit)) >>> 0);
};
const opBitReset: Handler = (ctx) => {
  const bit = num(ctx, 1);
  if (bit > 0x1f) { ctx.machine.note('bit-index-oob', `bit-reset 位号 ${bit}`); return; }
  put(ctx, 0, (num(ctx, 0) & ~(1 << bit)) >>> 0);
};
/** `check-bit`：**位号是 op3**（`op1 = ((1<<op3) & op2) != 0`） */
const opCheckBit: Handler = (ctx) => {
  const value = num(ctx, 1);
  const bit = num(ctx, 2);
  if (bit > 0x1f) { ctx.machine.note('bit-index-oob', `check-bit 位号 ${bit}`); put(ctx, 0, 0); return; }
  put(ctx, 0, ((1 << bit) & value) !== 0 ? 1 : 0);
};

/**
 * `0x60 random` —— **本族唯一的非确定源**（A 档：语义 = `op1 = rand() % op2`；`op2 == 0` ⇒ 先写 `op1 = 0` 再抛）。
 *
 * ★★ 三条口径，每条都是为了"不把不可复现藏起来"：
 * 1. **随机源是注入的**（`Instance.random`）。没有它 ⇒ **抛**，不许退回 `Math.random()`
 *    —— 那会让"同种子同日志"这条判据变成假象（而失效方式极隐蔽：两次跑都"正常"）。
 * 2. **每一次取数都发一条 `system.random.draw`**：于是"这一趟依赖了多少次随机"是产物的一部分。
 * 3. `op2 == 0` **先写 0 再抛**（引擎就是这个次序：写操作数在前、抛异常在后）——
 *    反过来写会让"抛之前的操作数状态"与引擎不一致。
 *
 * ★ 已登记的附带观察（`numeric-ops.ts` 的 `touchesEngineState`）：体内还有一个**上限 12 的重掷计数器**
 *   （`Engine+0x69330`）。**为什么本批不实现它**：登记的语义只到 `rand() % op2`，
 *   而"在什么条件下重掷"没有取证 ⇒ 实现一个猜出来的重试循环比不实现更糟（它会让取数次数错）。
 *   ⇒ 登记在需求树，不在这里编。
 */
const opRandom: Handler = (ctx) => {
  const rng = ctx.machine.instance.random;
  const modulus = num(ctx, 1);
  if (modulus === 0) {
    put(ctx, 0, 0);
    throw new Error('random: op2 == 0（引擎先写 op1 = 0，再抛）');
  }
  if (!rng) {
    throw new Error(
      'random: 本实例**没有随机源** —— 非确定性是引擎态的一部分，不许由核心自己变出来。' +
      '前端必须注入（见 host/random.ts 与 host/instance.ts 的 `random`）',
    );
  }
  const draw = rng.nextU32();
  ctx.machine.effect('system', 'random.draw', 'modeled', {
    source: rng.label, drawIndex: rng.draws(), value: draw >>> 0, modulus,
  });
  put(ctx, 0, (draw % modulus) >>> 0);
};

// —— 浮点族（A 档）——
// ★ 浮点结果**不做 `>>> 0`**：那是整数归一化。float 池存的直接是 JS number。
const opFloatMov: Handler = (ctx) => put(ctx, 0, fnum(ctx, 1));
const opFloatBin = (f: (a: number, b: number) => number): Handler => (ctx) => {
  const a = fnum(ctx, 1);
  const b = fnum(ctx, 2);
  put(ctx, 0, f(a, b));
};
const opFabs: Handler = (ctx) => put(ctx, 0, Math.abs(fnum(ctx, 1)));
/** `0x2D6`：整数 op2 → 浮点 op1（★ 与 `0x2D5 float-mov` 的差别就在这里） */
const opIntToFloat: Handler = (ctx) => put(ctx, 0, fnum(ctx, 1));

// ───────────────────────────────────────────────────────── B 档：控制与流程

/**
 * `0x02 exit` —— 跨脚本**返回调用层**（`cur = frame.caller`）。
 * ★ B 档（handler 旧仓记为 `sub_41A820`；`>=0` 切帧 / `-10` / `-11` / 其它抛退出的三支，
 *   本仓只实现**顶层退出**与**返回调用者**两支 —— 另两支的语义锚还没复核，故不写）。
 */
const opExit: Handler = (ctx) => {
  const f = ctx.frame;
  if (f.caller < 0) {
    ctx.machine.effect('system', 'script.exit', 'modeled', { script: f.scriptName, depth: ctx.machine.frames.length });
    ctx.machine.diag.stopReason = 'exit';
    throw new ExitScript();
  }
  ctx.machine.frames.pop();
  ctx.machine.effect('system', 'script.exit', 'modeled', { script: f.scriptName, returnedTo: f.caller });
};

/** 顶层 `exit` 的信号（**不是错误**：主循环会把它翻译成停止原因 `exit`） */
export class ExitScript extends Error {
  constructor() {
    super('顶层脚本 exit');
    this.name = 'ExitScript';
  }
}

/**
 * `0x21C wait` —— 置等待门（B 档；handler 旧仓记为 `sub_41A260`，写 `effect_flags |= 0x400`）。
 * ★ 门**不是** no-op：它挡住主循环派发，直到场景里的计时窗跑完（见 `machine.ts` 头注）。
 */
const opWait: Handler = (ctx) => {
  const reason = ctx.machine.setWaitGate();
  ctx.machine.note('wait-gate-set', `waitingOn=${reason}`);
};

/**
 * `0x101 poll-input` —— **采样并丢弃已积累的输入，再标记"从现在起等输入"**（B 档，已回语料复核）。
 *
 * 取证（`.lst`）：`sub_419CC0` @ `0x419CC0`，`.lst:38249-38269`。它调 `sub_478090` 把这一瞬的
 * 键鼠/手柄状态**写进输入掩码字段**，紧接着 `38263 mov dword ptr [edi],0` 把它**清零**，
 * 再清"未消费输入"闩锁，最后写两个"等输入"状态字段。
 * ⇒ 语义**不是**"把这次采样留下"，而是"**把已积累的输入丢掉、从现在起等新的**"。
 * ★ 这一条最容易被实现成"读一次输入并保存" —— 那样脚本里那些"等一次点击"的循环会
 *   **看起来正常**地空转，而玩家永远点不动（一次静默的行为差异）。
 */
const opPollInput: Handler = (ctx) => {
  const input = ctx.machine.instance.input;
  if (!input) {
    // ★ headless **没有输入源** —— 那是"没有这张能力"，不是"读到了没有按键"
    ctx.machine.effect('input', 'poll', 'not-provided', {
      note: 'headless 没有输入源；掩码未刷新（引擎此时会真的去采样 OS 键鼠状态）',
      maskDiscarded: true,
    });
    return;
  }
  const s = input.poll();
  ctx.machine.effect('input', 'poll', 'modeled', {
    buttons: s.buttons >>> 0, wheel: s.wheel | 0, mouseX: s.mouseX, mouseY: s.mouseY,
    // ★ 这两条是**已复核**的引擎语义（见上）：采样结果随后被丢弃、闩锁被清
    maskDiscarded: true, latchCleared: true,
  });
};

/**
 * `0x1a7 comment` —— **no-op**（脚本里的注释行；`argc = 1`，操作数是那句注释文本）。
 *
 * ★ 判据（取证，锚 = EA）：handler = `sub_4191B0` @ `0x4191B0`（分派表项 `.text:004162FE`，
 * `.lst` 37340-37347）。它**整个体只有两条指令**：
 * ```
 *   this[30*cur + 95805] = 3      ; 帧+0x5D8F4 = 3 = 2*argc+1 —— **分派器协议的长度字**
 *   return this[95776]            ; 读 cur（Engine+0x5D880）
 * ```
 * ⇒ 除了"写本指令的长度字"（那是**每条** handler 都必须做的协议写，在本模型里自动成立），
 *   它**什么都不做**。所以把它实现成 no-op 是**有判据的**，不是"看名字猜的"。
 * ★ 顺带：这条也再次印证了"长度字 = 2·argc+1"那条恒等式。
 */
const opComment: Handler = () => { /* 见上：只有协议写，故无可执行语义 */ };

// ───────────────────────────────────────────────────────── 控制流与调用

/** label 操作数的**哨兵**：`0xFFFFFFFF` = 这条分支没有目标（就落下去） */
const NO_LABEL = 0xffffffff;

/**
 * 跳到第 `i` 个操作数指的 label。
 * @returns 真的跳了吗（哨兵 ⇒ false = 落下去）
 * ★ 跳到的偏移**必须**正好是一条指令的起点：落在指令中间是**结构性错误**
 *   （脚本坏了，或者 label 的换算口径变了）⇒ 抛，不猜"最近的那条"。
 */
function jumpToLabel(ctx: VmContext, i: number): boolean {
  const raw = ctx.ins.args[i].rawData >>> 0;
  if (raw === NO_LABEL) return false;
  const off = labelByteOffsetOf(ctx.script, raw);
  const idx = instructionIndexAt(ctx.script, off);
  if (idx === null) {
    throw new Error(
      `label 指向的不是指令起点：0x${off.toString(16)}（${ctx.script.name} 的第 ${ctx.ins.index} 条，操作数 #${i}）` +
      ` —— 落在指令中间说明脚本结构或 label 换算口径有问题`,
    );
  }
  ctx.frame.ip = idx;
  return true;
}

/**
 * `0x8c jmp`：无条件 PC 重定向到 `op1`（label）；`0xFFFFFFFF`（−1）⇒ **什么都不做**（落下）。
 *
 * 取证（锚 = EA）：handler = `sub_4203D0` @ `0x4203D0`（分派表项 `Engine+0x0A52CC` ⇒
 * `(0xA52CC−0xA509C)/4 = 0x8C` ✓）。逐字：
 * ```
 *   this[30*cur + 95805] = 3                 ; 帧+0x5D8F4（长度字）= 1+2*1
 *   r = sub_41BF50(this, 1)                  ; op1
 *   if (r != -1) { this[30*cur+95782] = this[30*cur+95781] + 4*r;  ; PC ← 脚本基址 + 4*op1
 *                  this[30*cur + 95805] = 0; }                     ; ★ 长度字清 0
 * ```
 * ★ **为什么清长度字**：长度字是分派器用来"自动推进 PC"的。PC 已经被改写 ⇒ 必须清 0，
 *   否则分派器会在**新 PC** 上再加一次本指令的长度。在本模型里这条由 `Machine.step()` 的
 *   规则表达（"handler 改过 `ip` 就不 +1"）⇒ **语义等价，不需要那个字段**。
 * ★ 哨兵 `-1` ⇒ **静默落下**（引擎只是跳过整段 if）。我上一版在这里**抛错**，那是比引擎更严
 *   —— 已改成与引擎同形（no-op + 留痕）。
 */
const opJmp: Handler = (ctx) => {
  if (!jumpToLabel(ctx, 0)) {
    ctx.machine.note('jmp-no-label', `${ctx.script.name}#${ctx.ins.index} 的 op1 是哨兵（−1）⇒ 落下`);
  }
};

/**
 * `0xa0 jcc`：`op1 != 0` ⇒ 跳到 `op2`（但 `op2 == −1` 时**不跳**）；`op1 == 0` ⇒ 跳到 `op3`。
 * 三条里任何一条的目标是 `0xFFFFFFFF` ⇒ 该支"没有目标" = 落下。
 *
 * 取证（锚 = EA）：handler = `sub_4209B0` @ `0x4209B0`（表项 `Engine+0x0A531C` ⇒ `(0xA531C−0xA509C)/4 = 0xA0` ✓）。逐字：
 * ```
 *   4209CF call sub_41BF50 (push 1)      ; op1 = 条件
 *   4209D6 test eax,eax / 4209D8 jz  4209EA   ; op1==0 ⇒ 目标改用 op3
 *   4209DC call sub_41BF50 (push 2)      ; op2
 *   4209E1 cmp  eax,0FFFFFFFFh / 4209E4 jz 420A42   ; op2==−1 ⇒ 不跳、直接 retn
 *   4209E6 push 2 / jmp 4209F8                    ; 目标 = op2
 *   4209EA push 3                                 ; 目标 = op3
 *   420A13 lea edx,[ecx+eax*4] / 420A23 mov [帧+5D898h],edx   ; PC ← 脚本基址 + 4*raw
 * ```
 * ★ 这与"脚本自身的控制流"给出的形状**一致**（三处独立互证见旧注）：全部实测站点里
 *   `op2` 都是哨兵 ⇒ 可观测行为就是"条件非 0 落下、为 0 跳 `op3`"。
 */
const opJcc: Handler = (ctx) => {
  const cond = num(ctx, 0);
  const jumped = cond !== 0 ? jumpToLabel(ctx, 1) : jumpToLabel(ctx, 2);
  if (!jumped) {
    // 落在"没目标"的那一支 ⇒ 落下去。**记一笔**：这说明脚本里那条分支是"关掉"的，
    // 而"关掉的分支"与"我们算错了 label"在日志里必须分得开。
    ctx.machine.note('jcc-fallthrough', `${ctx.script.name}#${ctx.ins.index} cond=${cond} 落空`);
  }
};

/**
 * `0x8f call`：**同帧**子程序调用 —— 先把"调用点的下一条"压进**本帧**的返回栈，
 * 再把 PC 重定向到 `op1` 指的 label；`op1 == −1`（哨兵）⇒ **什么都不做**（引擎是"先压后撤"，
 * 净效果等于不压也不跳）。
 *
 * 取证（锚 = EA）：handler = `sub_420560` @ `0x420560`（表项 `Engine+0x0A52D8` ⇒
 * `(0xA52D8−0xA509C)/4 = 0x8F` ✓）。逐字：
 * ```
 *   this[30*cur + 95805] = 3
 *   this[256*cur + 97193 + this[cur + 97153]++] = ((PC − 脚本基址) >> 2) + 3;   ; ★ 压返回点
 *   if (sub_41BF50(this,1) == -1) { --this[cur + 97153]; ... }                  ; 哨兵 ⇒ 撤销
 *   else { PC ← 脚本基址 + 4*op1; this[30*cur + 95805] = 0; }
 * ```
 * 偏移逐字对上：`97153*4 = 0x5EE04`（层数）、`97193*4 = 0x5EEA4`（返回表，**每帧 256 槽**）、
 * 压入的 `(PC−基址)>>2 + 3` = 调用点的**下一条**（3 = `call` 自己占的 dword 数）。
 * ★ **它不换帧**（不 `inc [5D880h]`、不写 caller 回链）—— 换帧的是 `call-script`(0x03)。
 */
const opCall: Handler = (ctx) => {
  const raw = ctx.ins.args[0].rawData >>> 0;
  if (raw === NO_LABEL) {
    ctx.machine.note('call-no-label', `${ctx.script.name}#${ctx.ins.index} 的 op1 是哨兵（−1）⇒ 不压不跳`);
    return;
  }
  const next = ctx.frame.ip + 1; // ★ 引擎压的 `序号+3`（3 = call 的 dword 数）换算成下标就是 ip+1
  ctx.frame.pushReturn(next);
  if (!jumpToLabel(ctx, 0)) throw new Error('call：非哨兵的 label 却解析失败（不该发生）');
  ctx.machine.effect('system', 'script.exit', 'modeled', { via: 'call', script: ctx.script.name, returnTo: next, depth: ctx.frame.returnStack.length });
};

/**
 * `0x03 call-script`：`op1` = **统一文件 id**（已登记进台账：id == ALF 条目下标，根脚本 = id 0）。
 * 取字节（问宿主）→ 压一个**新帧**（caller = 当前帧）→ 执行权交给它。
 * ★ 与 `0x8f call` 的差别：这一条**换帧**（深度 +1），那一条是**同帧**子程序调用。
 */
const opCallScript: Handler = (ctx) => {
  const id = num(ctx, 0);
  const script = ctx.machine.loadScriptById(id, { asRoot: false }); // 只登记字节；压帧在下面（顺序清楚）
  const caller = ctx.frame;
  const frame = ctx.machine.pushFrame(script.name, caller.cur);
  ctx.machine.effect('system', 'script.load', 'modeled', {
    via: 'call-script', id, target: script.name, from: caller.scriptName,
    instructions: script.instructions.length, callerCur: caller.cur, newCur: frame.cur,
  });
};

/**
 * `0x05 ret`：**同帧**返回 —— 弹本帧的返回栈，PC 回到 `0x8f call` 压下的那一点。
 * **空栈 ⇒ 什么都不做**（引擎逐字：`cmp edx,0FFFFFFFFh / jz locret_41AA4D` ⇒ 直接 `retn`）。
 *
 * ★★ 这一条曾经实现错（**弹帧**，即当成 `exit`），已按取证订正：
 * `ret` **不换帧、不读 caller 回链、不改 `cur`** —— 它与 `call` 成对，作用域是**同一帧内**。
 * 换帧的返回是 `exit`(0x02)（读 `帧+0x5D8CC` → `Engine+0x5D884` → `Engine+0x5D880`）。
 */
const opRet: Handler = (ctx) => {
  const target = ctx.frame.popReturn();
  if (target === null) {
    ctx.machine.note('ret-empty', `${ctx.script.name}#${ctx.ins.index} 返回栈为空 ⇒ 落下（引擎在此直接 retn）`);
    return; // ★ 不换帧、不改 PC —— 与引擎同形
  }
  ctx.frame.ip = target; // ★ 改过 ip ⇒ `Machine.step()` 不会再 +1
  ctx.machine.effect('system', 'script.exit', 'modeled', { via: 'ret', script: ctx.script.name, returnedTo: target, depth: ctx.frame.returnStack.length });
};

// ───────────────────────────────────────────────────────── B 档：纹理与绘制项
// handler 符号（旧仓观测索引；**本仓尚未逐字复核**，复核单见需求树）：
//   0x1F7 detach-texture = sub_422BC0 · 0x1F8 create-texture = sub_422C20 ·
//   0x1F9 set-texture    = sub_422CB0 · 0x1FA release-texture = sub_422E00 ·
//   0x1FB draw-texture   = sub_422E70

/** `set-texture` 的 `op3` 颜色归一化：`< 0 ⇒ 0`，否则 `0xFFrrggbb`（A 通道被强置 `0xFF`） */
const normalizeTextureColor = (c: number): number => (asInt32(c) < 0 ? 0 : (0xff000000 | (c & 0xffffff)) >>> 0);

/**
 * `0x1F9 set-texture`：`op1 = imgid`、`op2 = 槽`、`op3 = 颜色`。
 * ★ 引擎在这一条里**同步读文件 + 解码**（所以同一条指令序列里紧接着问尺寸必然一致）。
 *   headless **不做**这件事 ⇒ 记两条：绑定（`modeled`）+ 资源请求（`logged-only`）。
 *   ★ 两条都要：只记"绑定成功"会让"这份安装里其实没有这张图"看不出来。
 */
const opSetTexture: Handler = (ctx) => {
  const imgid = num(ctx, 0);
  const slot = num(ctx, 1);
  const argb = normalizeTextureColor(num(ctx, 2));
  const applied = ctx.machine.scene.bindTexture(slot, imgid, argb);
  ctx.machine.effect('render', 'texture.bind', 'modeled', { slot, imgid, colorArgb: argb, applied: Boolean(applied) });
  ctx.machine.effect('resource', 'read.request', 'logged-only', {
    what: 'image', imgid, intoSlot: slot, note: 'headless 不读图、不解码；只记"引擎会在这里读一份图"',
  });
};

/** `0x1F8 create-texture`：`op1 = 槽`、`op2/3 = 宽高`、`op4 = 纹理种类`；建的是**空白离屏表面**（不读文件） */
const opCreateTexture: Handler = (ctx) => {
  const slot = num(ctx, 0);
  const w = num(ctx, 1);
  const h = num(ctx, 2);
  const kind = num(ctx, 3);
  ctx.machine.scene.createTexture(slot, w, h);
  ctx.machine.effect('render', 'texture.create', 'modeled', { slot, width: w, height: h, kind, offscreen: true });
};

/** `0x1FA release-texture`：`op1 = 槽`；释放该槽（`applied: false` = 本来就空） */
const opReleaseTexture: Handler = (ctx) => {
  const slot = num(ctx, 0);
  const applied = ctx.machine.scene.releaseTexture(slot);
  ctx.machine.effect('render', 'texture.release', 'modeled', { slot, applied });
};

/** `0x1FB draw-texture`：`op1 = handle`、`op2 = 纹理槽`、`op3..6 = 源 x/y/w/h`、`op7/8 = 目标 x/y` */
const opDrawTexture: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const slot = num(ctx, 1);
  const src = { x: num(ctx, 2), y: num(ctx, 3), w: num(ctx, 4), h: num(ctx, 5) };
  const dst = { x: num(ctx, 6), y: num(ctx, 7) };
  ctx.machine.scene.drawTexture(handle, slot, src, dst);
  ctx.machine.effect('render', 'draw-item.draw', 'modeled', { handle, slot, src, dst });
};

/**
 * `0x1F7 detach-texture`：`op1 = handle`、`op2 = count`。
 * ★ `count <= 1` ⇒ 摘单个；`count > 1` ⇒ 摘 **`[handle, handle + count)`**（左闭右开）。
 */
const opDetachTexture: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const count = num(ctx, 1);
  const removed = ctx.machine.scene.detach(handle, count);
  ctx.machine.effect('render', 'draw-item.detach', 'modeled', {
    handle, count, range: count > 1 ? [handle, handle + count - 1] : null, removed,
  });
};

// ───────────────────────────────────────────────────────── B 档：颜色与网格
//   0x202 set-draw-color         = sub_4231F0 · 0x203 set-draw-color-alpha = sub_4232C0
//   0x320 create-mesh            = sub_432150 · 0x322 set-vertex-color     = sub_426C20
//   0x323 set-vertex-color-alpha = sub_426CF0

/** 颜色分量的 clamp / 回退：`> 255 ⇒ 255`、`< 0 ⇒ 取当前值的该通道` */
const channelWithFallback = (raw: number, current: number, shift: number): number => {
  const v = asInt32(raw);
  if (v > 255) return 255;
  if (v < 0) return (current >>> shift) & 0xff;
  return v & 0xff;
};

/** 组装 ARGB（A 通道强置回退/clamp 规则） */
const argbOf = (alphaRaw: number, rgbRaw: number, current: number): number => {
  const a = channelWithFallback(alphaRaw, current, 24);
  const r = channelWithFallback(rgbRaw, current, 16);
  const g = channelWithFallback(rgbRaw, current, 8);
  const b = channelWithFallback(rgbRaw, current, 0);
  return ((a << 24) | (r << 16) | (g << 8) | b) >>> 0;
};

/**
 * `0x203 set-draw-color-alpha`：`op1 = 图元`、`op2 = 数值（**语义未定**）`、`op3 = α`、`op4 = RGB24`。
 * ★ 取证：它写两个字段 —— 那个数值、以及**当前工作色（FROM）**；**不建窗**（不置"有窗"标志、
 *   不碰 delay/dur、不置 pending）。⇒ 它可以在窗跑着的时候随时改 FROM，窗会从新的 FROM 继续插值。
 * ★ `op2` 落在绘制项的某个 dword 上，而那个字段的**读取点没拿到** ⇒ **语义未定**，
 *   ⛔ 不许叫它 "blend mode"（那是旧仓文档的命名，没有判据）。本层只把它**原样存下来**。
 */
const opSetDrawColorAlpha: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const param = num(ctx, 1);
  const item = ctx.machine.scene.items.get(handle);
  const current = item ? item.workingArgb : 0xffffffff;
  const argb = argbOf(num(ctx, 2), num(ctx, 3), current);
  const applied = ctx.machine.scene.setDrawColorAlpha(handle, param, argb);
  ctx.machine.effect('render', 'draw-item.color', 'modeled', {
    handle, param, paramSemantics: 'unverified', argb, window: false, applied,
  });
};

/** `0x202 set-draw-color`：`op1 = 图元`、`op2 = delay`、`op3 = dur`、`op4 = α`、`op5 = rgb`；**排一个计时窗** */
const opSetDrawColor: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const delayMs = num(ctx, 1);
  const durMs = num(ctx, 2);
  const item = ctx.machine.scene.items.get(handle);
  const current = item ? item.workingArgb : 0xffffffff;
  const argb = argbOf(num(ctx, 3), num(ctx, 4), current);
  const applied = ctx.machine.scene.setDrawColorWindow(handle, delayMs, durMs, argb);
  ctx.machine.effect('render', 'draw-item.color', 'modeled', { handle, delayMs, durMs, argb, window: true, applied });
};

/**
 * `0x320 create-mesh`：`op1 = handle`、`op2..op8 = 数组基址（**槽号**，第 i 项取 `slot + i`）`、
 * `op9 = 顶点数`、`op10 = 模式`。
 * ★ B 档里最不确定的一条：`op2..op8` 到底哪些是 x/y/z、哪些是颜色与 uv 数组**还没复核**
 *   ⇒ 本批**只**用 `op9`/`op10` 建网格并**把基址槽号原样记进日志**，不解释几何。
 *   `op9 <= 0` 时引擎报「頂点数%dは不正です．」⇒ 这里抛（与引擎同形：不静默建一个空网格）。
 */
const opCreateMesh: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const bases = [1, 2, 3, 4, 5, 6, 7].map((i) => slotIndexOf(ctx, i));
  const vertexCount = num(ctx, 8);
  const mode = num(ctx, 9);
  if (vertexCount <= 0) throw new Error(`create-mesh: 顶点数 ${vertexCount} 不正（引擎在此报「頂点数%dは不正です．」）`);
  ctx.machine.scene.createMesh(handle, vertexCount, mode);
  ctx.machine.effect('render', 'mesh.create', 'modeled', {
    handle, vertexCount, mode, arrayBaseSlotIndexes: bases,
    // ★ 已取证的部分：op2/op3/op4 = X/Y/Z 数组基址；op5/op6 = **加密的顶点色数组**
    //   （op5 → 色高 8 位、op6 → 色低 24 位，用**与 DEC 同一对常量、同一个 key** 解码）；
    //   op7/op8 = texcoord 数组基址；op9 = 顶点数；op10 = 一个整数（落在项上，**读取点没拿到**）。
    colorsDecoded: false,
    note: '本仓**不**解码顶点色（要按 4 字节步长逐元素 DEC 数组；见需求树）⇒ 网格的逐顶点色是占位值',
  });
};

/**
 * `0x322 set-vertex-color`：`op1 = 网格`、`op2 = 数值（**语义未定**）`、`op3 = α`、`op4 = RGB24`。
 * ★ 两处**已订正**（上一版照旧仓的命名写错了）：
 *   1. 它**不是逐顶点写** —— 逐顶点色是 `create-mesh` 灌进来的数组；这一条只写"当前色（FROM）"；
 *   2. 它**不建窗**（不置"有窗"标志、不碰 delay/dur、不置 pending）；
 *   3. `op2` 落在网格项的某个 dword 上，**读取点没拿到** ⇒ 语义未定（⛔ 不许当成"顶点下标"）。
 * ★ 家族不对称（别按名字照抄）：绘制色那边是 **5 参的 `set-draw-color` 建窗**；
 *   顶点色这边是 **5 参的 `set-vertex-color-alpha` 建窗**、这一条（4 参）只设当前色。
 */
const opSetVertexColor: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const param = num(ctx, 1);
  const mesh = ctx.machine.scene.meshes.get(handle);
  const current = mesh ? mesh.fromArgb : 0xffffffff;
  const argb = argbOf(num(ctx, 2), num(ctx, 3), current);
  const applied = ctx.machine.scene.setMeshColor(handle, param, argb);
  ctx.machine.effect('render', 'mesh.vertex-color', 'modeled', {
    handle, param, paramSemantics: 'unverified', argb, window: false, applied,
  });
};

/** `0x323 set-vertex-color-alpha`：`op1 = 网格`、`op2 = delay`、`op3 = dur`、`op4 = α`、`op5 = rgb`；**排计时窗** */
const opSetVertexColorAlpha: Handler = (ctx) => {
  const handle = num(ctx, 0);
  const delayMs = num(ctx, 1);
  const durMs = num(ctx, 2);
  const mesh = ctx.machine.scene.meshes.get(handle);
  const current = mesh ? mesh.fromArgb : 0xffffffff;
  const argb = argbOf(num(ctx, 3), num(ctx, 4), current);
  const applied = ctx.machine.scene.setVertexColorWindow(handle, delayMs, durMs, argb);
  ctx.machine.effect('render', 'mesh.vertex-color', 'modeled', { handle, delayMs, durMs, argb, window: true, applied });
};

// ───────────────────────────────────────────────────────── B 档：音频

/**
 * `0x20F play-movie`：`op1 = 影片资源 id`、`op2 = **影片对象表的槽号**`、`op3 = 音类别选择器`。
 *
 * 取证（`.lst`）：`sub_4237B0` @ `0x4237B0`，`.lst:53321-53479`。
 * * **异步**：体内**没有等待循环**（159 行里只有一次调用链 + 两次抛异常），末尾直接 `retn`；
 *   它只"建对象 + 打开文件 + 置两个状态位"，**每帧推进在主循环里**。
 *   ⇒ ★ 所以"执行到 play-movie"**不等于**"影片播完了"：播放在主循环的后续帧里发生。
 * * `op2` 是**槽号** —— 而且它与纹理那三条指令（`create-texture` 的 op1 / `set-texture` 的 op2 /
 *   `release-texture` 的 op1）**是同一下标空间**：引擎那一张"按槽的对象表"被纹理与影片**共用**。
 *   ★ 判据不必回语料：**LOGO.BIN 自己就是证据** —— 它先在槽 `2a` 上 `set-texture`，
 *   之后 `release-texture 2a` + `create-texture 2a …`，最后 `play-movie 5247 2a …`
 *   **还是槽 `2a`**：影片的纹理就落在那个槽上。
 *   （我上一版把两者写成"两回事"，那是错的 —— 已订正。）
 * * `op1` 经一个"资源 id → 路径"的函数变成文件名（那个函数的语义本批未建模）。
 * * 它置的位里有一位是"有 movie 在动"，主循环据此决定要不要跑对象循环。
 *
 * ★ headless **没有视频播放器** ⇒ `logged-only`（"我知道我跳过了什么"那一档）。
 *   ★ 那些状态位属知识层，本批**不**写进模型（模型里不出现偏移）。
 */
const opPlayMovie: Handler = (ctx) => {
  const movieId = num(ctx, 0);
  const slot = num(ctx, 1);
  const mode = num(ctx, 2);
  ctx.machine.effect('audio', 'movie.play', 'logged-only', {
    movieId, slot, mode,
    async: true, startsMoviePlayer: true, movieTableModelled: false,
    note: 'headless 没有视频播放器：只记录"引擎会在这里起播一段影片"，且**不**建模影片对象表',
  });
  ctx.machine.effect('resource', 'read.request', 'logged-only', { what: 'movie', movieId, intoSlot: slot });
};

// ───────────────────────────────────────────────────────── 表

/**
 * opcode → handler。
 *
 * ★ **表里没有的 opcode = 本批未实现**，`Machine.step()` 会把它变成停止原因 `error`
 *   （**不静默跳过**：跳过会让后面每条指令都在错的上下文里跑，而日志依然"正常"）。
 * ★ `0x60 random` **在表里**（随机源由前端注入；没有源就响亮失败）——
 *   它不再是"故意不实现"：用户口径是"游戏确实用到的能力都要支持"，而语料里它出现 22 次。
 */
export const HANDLERS: Record<number, Handler> = {
  // —— A 档：整数双目 / 单目 / 位 / 比较 ——
  0x50: opAdd, 0x51: opSub, 0x52: opMul, 0x53: opDiv, 0x54: opMod, 0x55: opMov,
  0x56: opAnd, 0x57: opOr, 0x58: opSar, 0x59: opShl,
  0x5a: opEq, 0x5b: opNe, 0x5c: opLt, 0x5d: opLte, 0x5e: opGr, 0x5f: opGre,
  0x135: opBitSet, 0x136: opBitReset, 0x13f: opCheckBit,
  // —— A 档：非确定源（随机源**注入**；缺席 ⇒ 响亮失败，见 opRandom）——
  0x60: opRandom,
  // —— A 档：浮点 ——
  0x191: opFabs,
  0x2d0: opFloatBin((a, b) => a + b),
  0x2d1: opFloatBin((a, b) => a - b),
  0x2d2: opFloatBin((a, b) => a * b),
  0x2d3: opFloatBin((a, b) => a / b),
  0x2d4: opFloatBin((a, b) => a % b),
  0x2d5: opFloatMov,
  0x2d6: opIntToFloat,

  // —— B 档：控制与流程 ——
  0x02: opExit,
  0x03: opCallScript,
  0x05: opRet,
  0x8c: opJmp,
  0x8f: opCall,
  0xa0: opJcc,
  0x1a7: opComment,
  0x101: opPollInput,
  0x21c: opWait,

  // —— B 档：纹理与绘制项 ——
  0x1f7: opDetachTexture,
  0x1f8: opCreateTexture,
  0x1f9: opSetTexture,
  0x1fa: opReleaseTexture,
  0x1fb: opDrawTexture,

  // —— B 档：颜色与网格 ——
  0x202: opSetDrawColor,
  0x203: opSetDrawColorAlpha,
  0x320: opCreateMesh,
  0x322: opSetVertexColor,
  0x323: opSetVertexColorAlpha,

  // —— B 档：音频 ——
  0x20f: opPlayMovie,
};
