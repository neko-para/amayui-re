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
import { addressOfOperand, asFloat, asInt32, asUint32, floatFromBits, GLOBAL_POOL_BY_TYPE_TAG, readOperand, readOperandAsText, writeOperand } from './operand.ts';
import { instructionIndexAt, labelByteOffsetOf } from './script.ts';
import type { OperandValue } from './operand.ts';
import { localPoolByTypeTag } from '../model/pools.ts';
import { ENGINE_SCALAR_ARRAYS, ENGINE_SCALAR_WRITES } from '@amayui/age-format/src/engine/layout.mts';
import { encInt, encZero } from '@amayui/age-format/src/asm/value-codec.mts';

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
  const r = readOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script, space: ctx.machine.space }, ctx.ins.args[i], i);
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
  const r = readOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script, space: ctx.machine.space }, ctx.ins.args[i], i);
  if (r.value === null) {
    ctx.machine.note('uninitialized-read', `${r.where}（浮点，指令 0x${ctx.ins.opcode.toString(16)} 操作数 #${i}）`);
    return 0;
  }
  return asFloat(r.value);
}

/** 写第 `i` 个操作数 */
function put(ctx: VmContext, i: number, value: OperandValue): void {
  writeOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script, space: ctx.machine.space }, ctx.ins.args[i], i, value);
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

// ───────────────────────────────────────────────────────── 启动链前段（"薄转发 / 写标量"那一批）

/**
 * ## 为什么这一批是**表驱动**的，而不是 16 个手写函数
 *
 * 启动链前段的 handler 形状高度重复，只有两种：
 * 1. **读操作数 → 写引擎的某个 dword 标量**（观察登记在 `layout.mts` 的 `ENGINE_SCALAR_WRITES`）；
 * 2. **转发进某个子系统**（`sub_45D660(Engine+0x14D30, op1..op5)` 这种，语义在**被调方**里）。
 *
 * ⇒ 手写 16 遍只会把同一件事写 16 次，而且每次都要问"偏移写哪"（本仓禁止）。表驱动还带来一个好处：
 * **这一批"没建模到什么程度"是一眼可见的**（见下面的 `kind`），而不是散在 16 个函数体里。
 *
 * ## ★ 三条不许越的线
 * * ⛔ **偏移/EA 不进本文件**：标量的名字来自 `layout.mts`（知识层），本文件只按名字引用。
 * * ⛔ **不发明语义**：`kind: 'forward'` 的那些**没有**被建模 —— 每次执行都发一条
 *   `logged-only` 记录，写明**被调符号**与实参 ⇒ "我们跳过了这次子系统调用"是**可见的**，
 *   不是静默空操作（本仓最忌讳的正是后者）。日志会告诉我们启动链**真的**依赖哪些子系统。
 * * ⛔ **不猜 argc**：每条都照 handler 体自己写的长度字核对过（见 `handlers.mts` 的注释）。
 */
type PrologueEntry =
  /** 体里除了协议写什么都没有 ⇒ 真 no-op（判据：`sub_419690` 的整个体） */
  | { opcode: number; kind: 'noop'; why: string }
  /** 读操作数 → 写标量；标量名由 `ENGINE_SCALAR_WRITES` 给 */
  | { opcode: number; kind: 'scalar' }
  /** 转发进子系统（**未建模**）：逐次记 `logged-only`，附被调符号 */
  | { opcode: number; kind: 'forward'; callee: string; argc: number; note: string };

const PROLOGUE: PrologueEntry[] = [
  { opcode: 0x1a8, kind: 'noop', why: '`sub_419690` 体只有两条：写长度字 + 读 cur ⇒ 无操作数、无可执行语义' },
  { opcode: 0x149, kind: 'scalar' },
  { opcode: 0x21b, kind: 'scalar' },
  { opcode: 0x252, kind: 'scalar' },
  { opcode: 0x88, kind: 'scalar' },
  { opcode: 0x78, kind: 'scalar' },
  { opcode: 0x2db, kind: 'scalar' },
  { opcode: 0x2f6, kind: 'forward', callee: 'sub_4BB9F0/sub_404CB0', argc: 1, note: '按 op1 清某个 per-slot 状态（4 个字段）后重算一处' },
  { opcode: 0x1ca, kind: 'forward', callee: '(vtable+12)', argc: 1, note: '对 `Engine+0xAA614` 的对象走 vtable 调用，实参含 op1 与一个静态字符串' },
  { opcode: 0x324, kind: 'forward', callee: 'sub_453530', argc: 0, note: '无操作数；实参取自 `Engine` 的某个字段' },
  { opcode: 0x32f, kind: 'forward', callee: 'sub_49A150', argc: 1, note: '转发进 `Engine+0x4ED10` 那个容器（与 draw-item/计时窗同族）' },
  { opcode: 0x70, kind: 'forward', callee: 'sub_45D660', argc: 5, note: '转发进 `Engine+0x14D30` 那个子系统，argc 5' },
  { opcode: 0x71, kind: 'forward', callee: 'sub_45EC60/sub_48FFB0', argc: 1, note: '转发 + 两处整块拷贝 + 一次 vtable 调用（体较长，未逐句建模）' },
  { opcode: 0x73, kind: 'forward', callee: 'sub_453AD0/+', argc: 10, note: '前段最长的一条（10 个操作数）' },
  { opcode: 0x79, kind: 'forward', callee: 'sub_4563A0', argc: 3, note: '转发进 `Engine+0x14D30`' },
  { opcode: 0x1c1, kind: 'forward', callee: 'sub_4563D0', argc: 3, note: '转发进 `Engine+0x14D30`' },
  // ★ `0x2f8`：当前启动链的停止点（SYSTEM4 的 `call` 跳进的那个子程序的头三条）。
  //   体（`sub_4268D0`）：`sub_4B6940(Engine+0x48E8, op1 + 12, op2)` —— 注意**第一个实参是 `op1 + 12`**
  //   （对操作数做了算术）。记进日志时也照原样记，别把 `+12` 抹掉：那正是"它要一个结构里的字段"的线索。
  { opcode: 0x2f8, kind: 'forward', callee: 'sub_4B6940', argc: 2, note: '转发进 `Engine+0x48E8`；第一个实参 = `op1 + 12`（对操作数做了算术），第二个 = op2' },
  { opcode: 0x303, kind: 'forward', callee: 'sub_456600', argc: 3, note: '转发进 `Engine+0x14D30`（同族里 argc 3 的那条）' },
  // ★ `0x308`（`sub_426B20`，argc 1）：读 op1 → `sub_407B20(dword_55E1BC, Engine[96981], op1)`，
  //   **返回值不写回操作数**（handler 的返回值交给派发器）⇒ 只记欠账。
  { opcode: 0x308, kind: 'forward', callee: 'sub_407B20', argc: 1, note: '实参 = (全局对象 dword_55E1BC, Engine[96981], op1)' },
];

/**
 * 给守卫看的**表摘要**（`opcode → kind`）—— 守卫要能断言"表里每一行都真的注册进了 `HANDLERS`"。
 * ★ 为什么必须能断言：`prologueHandlers()` 只把 `noop`/`forward` 两态映射成函数，`scalar` 那几条是
 *   **具名常量手工接上**的 ⇒ "往表里加一行 `scalar` 却忘了接常量"是一个不会被类型系统抓住的疏漏。
 */
export const PROLOGUE_OPCODES: readonly { opcode: number; kind: PrologueEntry['kind'] }[] =
  Object.freeze(PROLOGUE.map((e) => Object.freeze({ opcode: e.opcode, kind: e.kind })));

/** 知识层登记过的标量名（模型引用的名字必须在这里 —— 否则就是模型自己编了个偏移） */
const SCALAR_NAMES = new Set(ENGINE_SCALAR_WRITES.map((w) => w.name));

/** 按 opcode 归拢"要写哪些标量"（名字与形态都来自知识层，本文件不重复它们） */
const SCALARS_BY_OPCODE = new Map<number, (typeof ENGINE_SCALAR_WRITES)[number][]>();
for (const w of ENGINE_SCALAR_WRITES) {
  const list = SCALARS_BY_OPCODE.get(w.opcode) ?? [];
  list.push(w);
  SCALARS_BY_OPCODE.set(w.opcode, list);
}

/** 取一个**知识层登记过**的标量名（没登记 ⇒ 抛：那说明模型引用了知识层不认识的名字） */
function scalarName(name: string): string {
  if (!SCALAR_NAMES.has(name)) {
    throw new Error(`模型引用了知识层没登记的标量槽「${name}」—— 见 age-format/src/engine/layout.mts 的 ENGINE_SCALAR_WRITES`);
  }
  return name;
}

/** `bswap24`：在**低 24 位内**把字节序倒过来（`b0<<16 | b1<<8 | b2` —— 像 BGR↔RGB）。
 *  判据（锚 = EA）：`0x76` 的体逐字 `this[21664] = BYTE2(v2) + ((BYTE1(v2) + ((u8)v2 << 8)) << 8)`。*/
const bswap24 = (v: number): number => (((v & 0xff) << 16) | (v & 0xff00) | ((v >>> 16) & 0xff)) >>> 0;

/** 生成一个"写标量"handler（每条登记都自带它的写形态） */
function scalarHandler(opcode: number, extra?: (ctx: VmContext, v: number) => void): Handler {
  const writes = SCALARS_BY_OPCODE.get(opcode) ?? [];
  // ★★ 引擎**明确不支持**这条命令：派发表里没有它 ⇒ 落到 `rep stosd` 预填的默认 handler
  //    （`sub_418E30`，体是抛「このコマンドはサポートされていません．」）。
  //    ⛔ 这与"本批还没实现"**不是一回事** —— 走到这里意味着**分支走错了**（真实流程不该执行它），
  //    所以错误信息必须把这件事说清，否则两种完全不同的处境会长得一样。
  if (writes.some((w) => w.form === 'unsupported')) {
    const sym = writes[0].handler;
    return () => {
      throw new Error(
        `opcode 0x${opcode.toString(16)} **引擎明确不支持**（派发表没有该 opcode 的登记 ⇒ 默认 handler ${sym} ` +
        `抛「このコマンドはサポートされていません．」）—— 这不是"本批未实现"，而是**走到了不该走的分支**`,
      );
    };
  }
  if (!writes.length) throw new Error(`opcode 0x${opcode.toString(16)} 声称写标量，但知识层没有对应登记`);
  return (ctx) => {
    const v = num(ctx, 0);
    for (const w of writes) {
      // ★ `const`：引擎写的是**常量**（逐字里就是立即数），与操作数无关
      const value = w.form === 'const' ? (w as { value?: number }).value!
        : w.form === 'op2' ? num(ctx, 1)
          : w.form === 'bool(op1)' ? (v !== 0 ? 1 : 0)
            : w.form === 'bswap24(op1)' ? bswap24(v) : v;
      // ★ 知识层登记了 `max` ⇒ 这是**引擎自己**的范围检查（越界它抛 C++ 异常）——
      //   本层照抄：⛔ 不许 clamp、不许静默截断（那会把"脚本写错了"变成"值变了一点"）。
      const capped = (w as { max?: number }).max;
      if (capped !== undefined && value > capped) {
        throw new Error(`opcode 0x${opcode.toString(16)} 的操作数 ${value} 超出引擎的范围检查（> 0x${capped.toString(16)}）—— 引擎这里抛异常`);
      }
      ctx.machine.scalars.write(scalarName(w.name), value);
      // ★★ 写完标量之后那次子系统调用**必须记进欠账**（知识层的 `callsAfter`）。
      //    少了这一条，"handler 里未建模的那次调用"就不会出现在保真欠账里 ⇒ 日志显得比实际干净。
      for (const callee of w.callsAfter ?? []) {
        ctx.machine.effect('system', 'engine.forward', 'logged-only', {
          opcode: `0x${opcode.toString(16)}`, callee, args: [v],
          note: '标量写**之后**的一次未建模子系统调用（知识层 `callsAfter`）',
        });
      }
    }
    extra?.(ctx, v);
    ctx.machine.effect('system', 'engine.scalar.write', 'modeled', {
      opcode: `0x${opcode.toString(16)}`,
      slots: writes.map((w) => w.name),
      value: v,
      note: '槽的**含义未定**（知识层只登记了"谁写它"）—— 本层只保证"写进去的读出来还是它"',
    });
  };
}

const opScalar149 = scalarHandler(0x149);
const opScalar21b = scalarHandler(0x21b);
const opScalar252 = scalarHandler(0x252);
const opScalar76 = scalarHandler(0x76);
const opScalar77 = scalarHandler(0x77);
const opScalar1a4 = scalarHandler(0x1a4);
const opScalar2ee = scalarHandler(0x2ee);
const opScalarFe = scalarHandler(0xfe);
const opScalar10f = scalarHandler(0x10f);
const opScalar248 = scalarHandler(0x248);
/**
 * `0x25b`（无名，handler `sub_425E20`，argc 1）：**标量写 + 条件子系统调用**。
 *
 * 逐字：
 * ```
 *   result = sub_41BF50(this, 1)     ; op1
 *   this[92379] = 2                  ; ★ 常量 2（`form: 'const'`）
 *   this[92381] = result             ; ★ op1
 *   if (!this[167990]) { v3 = sub_41BF50(this, 1); return sub_408440(this, v3); }
 * ```
 * ★ 那次调用是**条件**的（读 `Engine.d167990`）—— 所以**不能**用无条件的 `callsAfter` 登记：
 *   那会把"引擎会调"与"引擎不调"混成同一种表现，保真欠账也会**多算**。
 * ★ `Engine.d167990` 的**写入点还没取证**：本层读到的是标量默认值 0 ⇒ 条件**总是成立** ⇒
 *   当条件不成立时，我会**多记一笔**欠账（不会少记）—— 这个方向是安全的，且记在这里以备考证。
 */
const opSetMessage25b = scalarHandler(0x25b, (ctx, v) => {
  const gate = ctx.machine.scalars.read('Engine.d167990');
  if (gate === 0) {
    ctx.machine.effect('system', 'engine.forward', 'logged-only', {
      opcode: '0x25b', callee: 'sub_408440', args: [v],
      note: '**条件**转发（逐字 `if (!this[167990]) sub_408440(this, op1)`）—— 未建模；d167990 的来源未取证 ⇒ 条件视为成立',
    });
  } else {
    ctx.machine.note('forward-skipped', `0x25b：Engine.d167990 = ${gate} ⇒ 引擎**不**调 sub_408440（逐字条件不成立）`);
  }
});
/** 0x110/0x111/0x112：引擎明确不支持（见 ENGINE_SCALAR_WRITES 里那三条的注释） */
const opUnsupported110 = scalarHandler(0x110);
const opUnsupported111 = scalarHandler(0x111);
const opUnsupported112 = scalarHandler(0x112);
const opScalar78 = scalarHandler(0x78);
const opScalar2db = scalarHandler(0x2db);

/**
 * `0x88`：写两个标量（都 = op1），**外加两个条件副作用**（取证：`sub_41FAB0` 体）
 * ```
 *   if (op1) this[122368] = 1; else this[174801] &= ~0x8000000;
 * ```
 * ★ 那两个槽**不在** `ENGINE_SCALAR_WRITES` 里：那张表登记的形态是"写成 op1 原值"，
 *   而这两条是**常量写 / 位清除**，形态不同 ⇒ 本批**只记录、不建模**它们，并留痕。
 *   ⇒ 这是一个**已知欠账**（已登记进需求树），不假装它已经解决。
 */
const opScalar88 = scalarHandler(0x88, (ctx, v) => {
  const side = v !== 0 ? '常量写 1（`this[122368] = 1`）' : '位清除（`this[174801] &= ~0x8000000`）';
  ctx.machine.effect('system', 'engine.scalar.bits', 'logged-only', {
    opcode: '0x88', side,
    note: '★ 形态是"常量/位操作"而非"= op1" ⇒ **未登记进知识层**、目前只记录不建模（欠账见需求树）',
  });
});

const opNoop1a8: Handler = () => { /* 体只有协议写（取证见 PROLOGUE 表里的 why） */ };

/** 转发类：**不建模**，但**逐次留痕**（写明被调符号与实参值）—— 绝不静默空操作 */
function forwardHandler(entry: Extract<PrologueEntry, { kind: 'forward' }>): Handler {
  return (ctx) => {
    const args: number[] = [];
    for (let i = 0; i < entry.argc; i += 1) args.push(num(ctx, i));
    ctx.machine.effect('system', 'engine.forward', 'logged-only', {
      opcode: `0x${entry.opcode.toString(16)}`, callee: entry.callee, args, note: entry.note,
    });
    ctx.machine.note('forward-not-modelled', `0x${entry.opcode.toString(16)} → ${entry.callee}（子系统未建模，只记录）`);
  };
}

/**
 * ★★ **"对象表 + 字段写"一族**（`Engine+0x15144[op1]` 那些）—— 带守卫的对象字段写。
 *
 * ## 形状（两条已取的证，同形）
 * ```
 * 0x212  sub_423A30：v = this[op1 + 21585]; if (v) *(v + 100) = op2
 * 0x25d  sub_425EF0：v = this[op1 + 21585]; if (v) { *(v + 276) = op2; *(v + 280) = op3 }
 * ```
 * 表 = `Engine + 4*21585` = **`Engine+0x15144`**，按 `op1`（槽号）取指针；**表项为空 ⇒ 什么都不做**
 * （不抛、不报错 —— 又一处"静默"）。
 *
 * ## ★ 为什么这一族只**记欠账**、不建模
 * 表里那些对象是**别处创建**的，而创建它们的子系统正是本批**跳过**的那些（`engine.forward` 一族）。
 * ⇒ 现阶段的模型里这张表**恒为空**，这些 handler 只会走 `if` 的假支。本层**如实记录**
 * "引擎本会写 `[对象+偏移] = 操作数`，但对象不在场"，并计入保真欠账。
 * ⛔ **不许**记成"写成功了" —— 那会让后面的分歧无从追溯。
 *
 * ## 机械量出的族规模（可复算）
 * `.c` 里引用 `+ 21585]` 的函数共 **9** 个：`sub_408F10 · sub_41A420 · sub_41EEF0 · sub_4200C0 ·
 * sub_420110 · sub_4213F0 · sub_423A30 · sub_423A80 · sub_425EF0`。
 * 其中已确认是 opcode handler 的 = **2**（`0x212` / `0x25d`）⇒ 另外 7 个待逐个核（见需求树）。
 */
const OBJECT_FIELD_WRITES: {
  opcode: number;
  argc: number;
  handler: string;
  /** 写哪些字段（元素顺序照抄体里的读序无关，这里按"字段偏移升序"写） */
  fields: { from: 'op2' | 'op3'; offset: number }[];
}[] = [
  { opcode: 0x212, argc: 2, handler: 'sub_423A30', fields: [{ from: 'op2', offset: 100 }] },
  { opcode: 0x25d, argc: 3, handler: 'sub_425EF0', fields: [{ from: 'op2', offset: 276 }, { from: 'op3', offset: 280 }] },
  // ★ 同族第三条（启动链 `#52/#53/#54/#56/#58/#60/#62/#63` 就是它，共 7 处）
  { opcode: 0x213, argc: 3, handler: 'sub_423A80', fields: [{ from: 'op2', offset: 104 }, { from: 'op3', offset: 108 }] },
];

/** 生成"带守卫的对象字段写"handler（读全部操作数 ⇒ 操作数有问题会当场抛出，而不是被静默吞掉） */
function objectFieldWriter(entry: (typeof OBJECT_FIELD_WRITES)[number]): Handler {
  return (ctx) => {
    const slot = num(ctx, 0);
    const values: Record<string, number> = {};
    for (let i = 1; i < entry.argc; i += 1) values[`op${i + 1}`] = num(ctx, i);
    ctx.machine.effect('system', 'engine.object-field-write', 'logged-only', {
      opcode: `0x${entry.opcode.toString(16)}`,
      slot,
      writes: entry.fields.map((f) => ({ field: `+${f.offset}`, value: values[f.from] })),
      applied: false,
      reason: '目标对象不在场：创建它的子系统尚未建模（见 engine.forward 那些记录）—— 这些写**没有发生**',
    });
  };
}

// ───────────────────────────────────────────────────────── 配置（`load-int` / `save-int`）

/**
 * 配置的**键**：引擎是**运行时拼**出来的（**没有静态键表**）。
 *
 * 取证（锚 = EA，见台账 `KN-01M4ASXQ587J7G7E6E5R3R2Y2K`）：`load-int` = `sub_42DF40`、
 * `save-int` = `sub_434F60`，两者都是
 * ```
 *   wsprintfA(buf, "%c%8.8x", 3, <操作数 1 的整数值>)     ; 类型码 3 = 整型（字面量就是 3，不是 'K'）
 * ```
 * ⇒ 键 = 一个字节 `\x03` + **8 位十六进制**（`%8.8x`：至少 8 位、零填充）。
 * ★ 字符串版（`load-string`/`save-string`）同形但类型码是 **5**，而且值是 28 字节的字符串元素
 *   ⇒ 那两条要等字符串池落地（见需求树）。
 */
function configKeyInt(value: number): string {
  return `\u0003${(value >>> 0).toString(16).padStart(8, '0')}`;
}

/** 键里那个字节不可打印 ⇒ 打日志时转义（否则日志里出现控制字符，没法读也没法 diff） */
function printableKey(key: string): string {
  return `\\x${key.charCodeAt(0).toString(16).padStart(2, '0')}${key.slice(1)}`;
}

/**
 * ★ **已知欠账**：键取自另一个取值原语 `sub_418A30`（`load-int`/`save-int` 都用它），
 *   而本模型只实现了 `sub_41BF50`。两者**在哪些 type 上一致、哪些上不一致没有取证**
 *   ⇒ 这里用 `sub_41BF50` 的读法**近似**，并把这件事写进每一条副作用记录（⛔ 不静默）。
 */
const KEY_PRIMITIVE_NOTE = '键取自 sub_41BF50 的读法；引擎用的是 sub_418A30（**未建模**，两者差异未取证）';

/**
 * `0x1a3 load-int`：**查不到 ⇒ 0**（不是"不写"），然后把结果**写回 op1**。
 * 逐字：`v3 = sub_428E00(配置对象, key); v4 = v3 ? *v3 : 0; sub_42B4B0(this, 1, v4)`。
 */
const opLoadInt: Handler = (ctx) => {
  const k = num(ctx, 0);
  const key = configKeyInt(k);
  const raw = ctx.machine.instance.config.get(key);
  const value = raw === undefined ? 0 : Number.parseInt(raw, 10) | 0;
  writeOperand({ locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script, space: ctx.machine.space }, ctx.ins.args[0], 0, value);
  ctx.machine.effect('system', 'config.read', 'modeled', {
    opcode: '0x1a3', key: printableKey(key), stored: raw ?? null, value,
    note: `查不到 ⇒ 0（引擎逐字如此）· ${KEY_PRIMITIVE_NOTE}`,
  });
};

/**
 * `0x1a2 save-int`：把 op1 的值存进配置，**键由同一个值算出**。
 * 逐字：`v3 = sub_41BF50(this,1)`（要存的值）· `v2 = sub_418A30(this,1)`（键的来源）· `sub_434D00(配置对象, key, &v3)`。
 * ★ 本仓的 `ConfigStore` 是**文本**接口（配置文件就是文本）⇒ 这里把整型存成**十进制文本**；
 *   这是**宿主表示**的选择，不是引擎语义（引擎那一格是个 dword）。
 */
const opSaveInt: Handler = (ctx) => {
  const v = num(ctx, 0);
  const key = configKeyInt(v);
  ctx.machine.instance.config.set(key, String(v | 0));
  ctx.machine.effect('system', 'config.write', 'modeled', {
    opcode: '0x1a2', key: printableKey(key), value: v,
    note: `整型以十进制文本存进宿主 ConfigStore（宿主表示）· ${KEY_PRIMITIVE_NOTE}`,
  });
};

// ───────────────────────────────────────────────────────── 字符串（`set-string` / `save-string`）

/** 每个 handler 都要的那个"操作数上下文"（三处各写一遍容易写漏，收成一处） */
function operandCtx(ctx: VmContext) {
  return { locals: ctx.frame.locals, globals: ctx.machine.globals, script: ctx.script, space: ctx.machine.space };
}

/**
 * `0x192 set-string`：把**操作数 2 的文本**写进**操作数 1**（目标按 op1 的 type 分派到不同串池）。
 *
 * 取证（锚 = EA）：handler = `sub_433660`（分派表项 `Engine+0x0A56E4` ⇒ `(0xA56E4−0xA509C)/4 = 0x192` ✓）。
 * * 它用 `sub_42A420(this, v3, 2)` **一次把两个操作数都读成文本** ⇒ 源侧走的是**文本**原语，
 *   所以 `type 2`（内联字符串）在这里是合法的源（见 `readOperandAsText` 头注）。
 * * 落点由 `sub_433310` 按 **op1 的 type** 分派：type 5 → 全局串池、type 11 → 局部串池、
 *   type 8/14 另有分支，**其余 type 抛 `Command_Type_Exception`**。本模型里这一步由 `writeOperand`
 *   的池分派承担（它同样按 type 选池；不支持的 type 会抛）。
 * ★ 它是启动链里出现最多的一条（7731 处）：`INITCONFIG0.BIN` 用它把默认字体名写进全局串。
 */
const opSetString: Handler = (ctx) => {
  const text = readOperandAsText(operandCtx(ctx), ctx.ins.args[1], 1);
  writeOperand(operandCtx(ctx), ctx.ins.args[0], 0, text);
  ctx.machine.effect('system', 'string.set', 'modeled', {
    opcode: '0x192', text,
    targetType: `0x${ctx.ins.args[0].type.toString(16)}`, targetIndex: ctx.ins.args[0].rawData >>> 0,
    sourceType: `0x${ctx.ins.args[1].type.toString(16)}`,
  });
};

/**
 * `0x1a9 save-string`：把**操作数 1** 那个字符串格子的内容存进配置。
 *
 * 取证（锚 = EA，台账 `KN-01M4ASXQ587J7G7E6E5R3R2Y2K`）：handler = `sub_434FE0`。
 * * **键**由 `sub_418AE0` 算出 —— 对 **type 5 直接返回下标**、对 8/14 用 `(指针 − 全局串池基址)/28` 反算下标；
 *   然后 `wsprintfA(buf, "%c%8.8x", 5, 那个下标)` ⇒ 键 = `\x05` + **8 位十六进制**（与整型版同形，类型码 5）。
 * * **值**是该 28 字节字符串元素的内容；写进**字符串配置对象**（`Engine+0xAA5A4`）的 `sub_434E00`。
 * ★ 本仓的 `ConfigStore` 是文本接口 ⇒ 字符串按原样存（这一路**没有**表示损失）。
 * ★ **未支持**：type 8/14 的"反算下标"（需要地址空间，见 ADR）⇒ 遇到就抛，不猜。
 */
const opSaveString: Handler = (ctx) => {
  const arg = ctx.ins.args[0];
  if (arg.type !== 5 && arg.type !== 0xb) {
    throw new Error(
      `save-string 的操作数 type 0x${arg.type.toString(16)}：键的下标反算只对字符串池（type 5/11）取证过` +
      `（type 8/14 要按指针与池基址反算，需要地址空间）⇒ 拒绝猜`,
    );
  }
  const index = arg.rawData >>> 0;
  const key = `\u0005${index.toString(16).padStart(8, '0')}`;
  const text = readOperandAsText(operandCtx(ctx), arg, 0);
  ctx.machine.instance.config.set(key, text);
  ctx.machine.effect('system', 'config.write', 'modeled', {
    opcode: '0x1a9', key: printableKey(key), value: text, kind: 'string',
    note: '字符串按原样存进宿主 ConfigStore（这一路没有表示损失）· 键的下标来自 sub_418AE0（type 5 = 直接用下标）',
  });
};

// ───────────────────────────────────────────────────────── 数组填充（`fill-zero` / `set-array-to`）

/**
 * 取址语义（`sub_42AEA0`）：把操作数解析成"**哪个池的第几格**"，供"按地址连写 N 格"用。
 * ★ `fill-zero` / `set-array-to` 的第一个操作数走的是**取址**原语（不是取值）
 *   ⇒ 它们写的是**从那一格开始连续 N 格**。
 */
type FillTarget =
  | { where: 'global'; pool: string; kind: string; index: number }
  | { where: 'local'; typeTag: number; kind: string; index: number };

function targetOf(ctx: VmContext, i: number): FillTarget {
  const type = ctx.ins.args[i].type;
  const index = ctx.ins.args[i].rawData >>> 0;
  const g = GLOBAL_POOL_BY_TYPE_TAG[type];
  if (g) return { where: 'global', pool: g, kind: g, index };
  const l = localPoolByTypeTag(type);
  if (l) return { where: 'local', typeTag: type, kind: l.kind, index };
  throw new Error(`操作数 #${i} 的 type 0x${type.toString(16)} 不是可寻址的池槽（取址原语只覆盖 3..14）`);
}

/**
 * 往 (池, 起始下标) 连写 `count` 格。
 * ★ 值按**池的族**决定：int 族池里存的是**解码后**的值 ⇒ 写 `intValue`；
 *   float 族池里引擎写的是那个 **dword**、而池按 float32 读 ⇒ 写"该 dword 当 float32 看"的那个数。
 *   （键为 0 时 `encZero`/`encInt` 与字面值重合，**真键下不是** —— 两者必须分开。）
 * ★ 全局池按**名字**寻址、局部池按 **type tag** 寻址（`LocalPools` 的既有口径）。
 */
function fillRange(ctx: VmContext, t: FillTarget, count: number, intValue: number, rawDword: number): void {
  const valueOf = (kind: string): number => (kind === 'float' ? floatFromBits(rawDword >>> 0) : intValue);
  if (t.where === 'global') {
    for (let k = 0; k < count; k += 1) ctx.machine.globals.write(t.pool, t.index + k, valueOf(t.kind));
  } else {
    for (let k = 0; k < count; k += 1) ctx.frame.locals.write(t.typeTag, t.index + k, valueOf(t.kind));
  }
}

/** 日志里的池标识（两种寻址方式在日志里要能分开看） */
const poolLabel = (t: FillTarget): string => (t.where === 'global' ? t.pool : `local(type 0x${t.typeTag.toString(16)})`);

/**
 * `0x6c fill-zero`：把从 op1 **地址**开始的 op2 个 dword 填成 `Engine+0x5EC90`（= **encZero**）。
 *
 * ★★ **助记名骗人**：它填的**不是 0**。
 * 取证（锚 = EA）：handler = `sub_42CE70`（表项 `Engine+0x0A524C`），体逐字：
 * ```
 *   v2 = sub_42AEA0(this, 1)                     ; 取址（目标地址）
 *   result = sub_41BF50(this, 2)                 ; 个数（<= 0 ⇒ 什么都不做）
 *   if (result > 0) do { *v2++ = this[97060]; } while (--result);
 * ```
 * `this[97060]` = `Engine + 4*97060` = **`Engine+0x5EC90`** —— 那一格在引擎构造时被写成 `ENC(key,0)`
 * （本仓 `layout.mts` 的 `EVIDENCE.encZero` 与 `value-codec.mts` 的 `encZeroField` 都登记了它）。
 */
const opFillZero: Handler = (ctx) => {
  const t = targetOf(ctx, 0);
  const count = num(ctx, 1);
  if (count > 0) fillRange(ctx, t, count, 0, encZero(ctx.machine.instance.env.codecKey));
  ctx.machine.effect('system', 'array.fill', 'modeled', {
    opcode: '0x6c', where: t.where, pool: poolLabel(t), from: t.index, count: Math.max(0, count),
    filled: 'encZero（DEC 后是 0；float 池按 float32 解释）',
    note: '★ 助记名 `fill-zero` 骗人：填的是 Engine+0x5EC90 = ENC(key,0)，不是字面 0',
  });
};

/**
 * `0x2d8 set-array-to`：把从 op1 **地址**开始的 op3 个 dword 填成 **`ENC(key, op2)`**。
 * 取证（锚 = EA）：handler = `sub_430CF0`，体逐字：
 * ```
 *   v2 = sub_42AEA0(this, 1)                                          ; 取址
 *   v5 = __ROL4__(this[97059] ^ __ROR4__(sub_41BF50(this,2), 7), 21)   ; = ENC(key, op2)
 *   result = sub_41BF50(this, 3)                                      ; 个数
 *   if (result > 0) memset32(v2, v5, result);
 * ```
 * `this[97059]` = `Engine+0x5EC8C` = **键**；那个表达式**正是本仓登记的 ENC**
 * （`ror 7 → xor key → rol 21`，见 `value-codec.mts`）⇒ 又一处独立互证。
 */
const opSetArrayTo: Handler = (ctx) => {
  const t = targetOf(ctx, 0);
  const value = num(ctx, 1);
  const count = num(ctx, 2);
  if (count > 0) fillRange(ctx, t, count, value, encInt(value, ctx.machine.instance.env.codecKey));
  ctx.machine.effect('system', 'array.fill', 'modeled', {
    opcode: '0x2d8', where: t.where, pool: poolLabel(t), from: t.index, count: Math.max(0, count), value,
    filled: 'ENC(key, op2)', note: '式子与 value-codec 的 ENC 逐字一致（独立互证）',
  });
};

// ───────────────────────────────────────────────────────── ★ 有返回值的"转发"（**不许**当 logged-only）

/**
 * ★★ **这一类 handler 与"转发"（`kind: 'forward'`）必须分开对待**，否则会造成**静默分歧**。
 *
 * 转发那批（`engine.forward`）是**语句**：跳过一次子系统调用，脚本的后续行为不依赖它的结果
 * ⇒ `logged-only` 是诚实的（缺的是副作用）。
 *
 * 但还有一类是**函数调用**：它的返回值**写回操作数 1**，而脚本会拿那个值去分支/存盘。
 * 对它只记一笔 = 让 op1 保留**旧值** ⇒ 后续分支走错，**而日志一切正常**（最难查的一类）。
 *
 * ⇒ 本层对这类 handler 的规矩是：**要么真实现，要么响亮失败**（带上"缺哪条取证"），
 *   ⛔ 不许给一个"看起来合理"的假值（0 也不行）。
 *
 * 下表登记**已确认为这一类、但语义尚未取证**的 opcode —— 它们的"响亮失败"是**有意**的，
 * 不是"还没轮到"。新增一条时把 handler 符号与逐字判据写在同一条注释里。
 */
/**
 * ★ 空表是**正常状态**：这一类里没有任何一条"语义未取证"的了（取证到位就该从这里删掉、改成真实现）。
 *   表还在的理由是"这一类必须被显式对待"——下次遇到同类，往这里加一行即可得到**响亮失败**。
 */
const VALUE_PRODUCING_UNVERIFIED: { opcode: number; handler: string; why: string }[] = [];

/**
 * `0x2de`（argc 2）：在**宿主字体名表**里查一个名字，把**下标**（未命中 ⇒ **-1**）写回 op1。
 *
 * 取证（锚 = EA）：handler = `sub_430DF0`（表项 `Engine+0x0A5D58`），体逐字：
 * ```
 *   v2 = sub_41B640(this, 2)              ; ★ 第四个取值原语：操作数 → cp932 文本（**不是** sub_41BF50）
 *   v3 = sub_428990(this + 21324, v2)     ; this + 21324*4 = Engine+0x14D30
 *   return sub_42B4B0(this, 1, v3)        ; 写回 op1
 * ```
 * `sub_428990`（`0x428990`）逐字：`[this+0x313C0]`/`[this+0x313C4]` 是 vector 的 begin/end，
 * 元素 **0x20** 字节（`sar ecx,5`），元素里 +0 是 std::string 数据、+0x10 是 size（+0x14 与 0x10 比决定堆/内联）；
 * 逐元素 `memcmp` 且要求长度相等；**键首字节是 `'@'`(0x40) 时跳过它再比**（`0x4289C8`/`0x4289D0`/`0x4289D3`）；
 * 命中 ⇒ `eax = 0 基下标`（`0x428A4C`），未命中 ⇒ `or eax,-1`（`0x428A38`）。**无副作用**。
 * ⇒ 那个 vector 是**字体名表**（判据：`0x459B56–0x459B6A` 拿 `LOGFONT+0x1C` = `lfFaceName` 去查它）。
 * ★ **表的内容是宿主事实** ⇒ 从 `instance.fonts` 取；没提供（`null`）⇒ 响亮失败。
 * ★ 顺带：`sub_41B640` 对 `type 2` 用的是**同一条** `^0xFF` + "存储字高字节 0FFh 即停" 的判据
 *   ⇒ 与 `vm/script.ts` 的 `inlineString` 是**独立互证**。
 */
const opFontIndex2de: Handler = (ctx) => {
  const fonts = ctx.machine.instance.fonts;
  if (!fonts) {
    throw new Error('0x2de 要在宿主的字体名表里查名字，而本实例没有提供 `fonts`（见 host/instance.ts）—— 拒绝假装"表里什么都没有"');
  }
  const raw = readOperandAsText(operandCtx(ctx), ctx.ins.args[1], 1);
  // ★ 跳过前导 '@'（引擎逐字如此：`cmp byte ptr [eax],40h` / `setz cl` / `add eax,ecx`）
  const key = raw.startsWith('@') ? raw.slice(1) : raw;
  const index = fonts.indexOf(key);
  const value = index < 0 ? -1 : index;
  writeOperand(operandCtx(ctx), ctx.ins.args[0], 0, value);
  ctx.machine.effect('system', 'font.lookup', 'modeled', {
    opcode: '0x2de', key, index: value, tableSize: fonts.length,
    note: index < 0
      ? '★ 未命中 ⇒ -1（引擎逐字 `or eax,-1`）。**表来自宿主**：headless 目前给的是空表 ⇒ 这一支与真机可能不同'
      : '命中 ⇒ 0 基下标',
  });
};

/**
 * `0x61 lookup-array`：把 `base[op3]` —— **目标是指针时给"那一格的地址"** —— 写进 op1。
 *
 * 取证（锚 = EA）：handler = `sub_42CB00`（表项 `Engine+0x0A5220`），体逐字：
 * ```
 *   v4 = sub_41BF50(this, 3)              ; idx = op3
 *   v2 = sub_42AEA0(this, 2)              ; base = **op2 那一格的地址**（取址原语）
 *   sub_418CC0(this, 1, v2, v4, -1, -1)   ; 写进 op1
 * ```
 * 目标语义由 `sub_418CC0` 的写法钉住：它算 `lea edx,[esi+edx*4]`（= `base + idx*4`）之后
 * **`mov [ecx+eax*4],edx`** —— 对**指针类**目标存的是**那个地址本身**（⛔ 不是那一格的值）；
 * 4 字节步长对应 6/7/12/13 族、28 字节步长对应 8/14 族。
 * ★ 这条判据让 `INITCONFIG4` 的循环**说得通**：`lookup-array (local-ptr 0) (global-int A) (i)`
 *   取到 `&A[i]`，接着 `save-int (local-ptr 0)` 解引用它 ⇒ 存的是 `A[i]` 的**值**。
 *   若把指针目标当成"存值"，那一步就会去解引用一个**编码过的整数**（而两条路都"看起来正常"）。
 * ★ 本批只支持 4 字节族（6/12）；28 字节族（8/14，字符串指针）会抛（要 SSO 那块，见需求树）。
 */
const opLookupArray: Handler = (ctx) => {
  const dest = ctx.ins.args[0];
  if (dest.type !== 0x6 && dest.type !== 0xc) {
    throw new Error(
      `lookup-array 的目标 type 0x${dest.type.toString(16)}：本批只实现 4 字节族（0x6 全局 / 0xc 局部）；` +
      `8/14（字符串指针，28 字节步长）要等 SSO 那块`,
    );
  }
  const idx = num(ctx, 2);
  const base = addressOfOperand(operandCtx(ctx), ctx.ins.args[1], 1);
  const addr = (base + 4 * idx) >>> 0;
  writeOperand(operandCtx(ctx), dest, 0, addr);
  ctx.machine.effect('system', 'array.lookup', 'modeled', {
    opcode: '0x61', base: `0x${base.toString(16)}`, index: idx, wroteAddress: `0x${addr.toString(16)}`,
  });
};

/** `copy-local-array` 一次最多拷多少格（★ 元素个数来自**脚本字节**，坏 raw 不许把内存吃光） */
const COPY_ARRAY_MAX = 1 << 20;

/**
 * `0x64 copy-local-array`：把脚本里**一块解码后的整数**逐格 ENC 后写进 `op1` 的地址。
 *
 * 取证（锚 = EA）：handler = `sub_42CBE0`，体逐字：
 * ```
 *   v2 = sub_42AEA0(this, 1)                                    ; dest = **op1 那一格的地址**
 *   v3 = 代码区基址 + 4*sub_41BF50(this,2) + 4                  ; src  = 块基址 + 4*raw + 4
 *   result = *(代码区基址 + 4*sub_41BF50(this,2))               ; count = **块首 u32**
 *   if (result > 0) do {
 *     result = __ROL4__(this[97059] ^ __ROR4__(*(v2 + v5), 7), 21);   ; ★ ENC(源 dword)
 *     *v2++ = result;                                            ; 逐格写
 *   } while (--v6);
 * ```
 * ⇒ 文件口径：块首 = `headerLen + 4*raw`（`帧+0x5D894` 就是代码区基址），数据从 `+4` 起。
 * ★ 值的编码是**无条件**的（源码里没有按目标池分派），所以这里也直接写 `ENC(源)`；
 *   对 `encoded` 池这与"经池 API 写"逐位相同，对非编码池则正是引擎的行为。
 * ★ `count <= 0` ⇒ 引擎**什么都不做**（逐字 `if (result > 0)`）—— 这里是记一笔后返回，不抛。
 */
const opCopyLocalArray: Handler = (ctx) => {
  const dest = addressOfOperand(operandCtx(ctx), ctx.ins.args[0], 0);
  const raw = num(ctx, 1) >>> 0;
  const blockOff = ctx.script.headerLen + 4 * raw;
  const bytes = ctx.script.bytes;
  const u32 = (o: number): number =>
    ((bytes[o]! | (bytes[o + 1]! << 8) | (bytes[o + 2]! << 16) | (bytes[o + 3]! << 24)) >>> 0);
  if (blockOff + 4 > bytes.length) {
    throw new Error(`copy-local-array 的块首越出脚本：raw=${raw} ⇒ ${blockOff}（脚本 ${bytes.length} B）`);
  }
  const count = u32(blockOff);
  if (count === 0) {
    ctx.machine.note('copy-local-array-empty', `raw=${raw} 的块元素个数为 0 ⇒ 引擎什么都不做`);
    return;
  }
  if (count > COPY_ARRAY_MAX) {
    throw new Error(`copy-local-array 的块声明了 ${count} 个元素（> ${COPY_ARRAY_MAX}）—— raw=${raw} 口径错了？`);
  }
  if (blockOff + 4 + 4 * count > bytes.length) {
    throw new Error(`copy-local-array 的块越出脚本：${count} 格从 ${blockOff + 4} 起（脚本 ${bytes.length} B）`);
  }
  const key = ctx.frame.locals.key;
  // ★ 目标按需增长（决策 REQ-01M4B969TBWVERFCB1MXS2Q2E1）：一次把要写到的最后一格覆盖上
  ctx.machine.space.ensureAddress(dest + 4 * (count - 1), 'copy-local-array 目标按需增长');
  for (let i = 0; i < count; i += 1) {
    ctx.machine.space.writeU32(dest + 4 * i, encInt(u32(blockOff + 4 + 4 * i), key), `copy-local-array 第 ${i} 格`);
  }
  ctx.machine.effect('system', 'array.copy', 'modeled', {
    opcode: '0x64', dest: `0x${dest.toString(16)}`, count, block: `0x${blockOff.toString(16)}`,
  });
};

/**
 * `0x10c`（无名，handler `sub_4220B0`，argc 2）：**带范围检查的间接引擎态写**。
 *
 * 逐字：
 * ```
 *   v2     = sub_41BF50(this, 2)                 ; op2 = 索引
 *   result = sub_41BF50(this, 1)                 ; op1 = 值
 *   if (result > 0x1F) throw (aSetkeymulti, 65541)
 *   this[this[v2 + 1690] + 1434] = result        ; ★ 写目标是**两次索引**：Engine[1434 + Engine[1690 + op2]]
 * ```
 * ★ 范围检查（`<= 0x1F`）照抄 —— 那是引擎自己抛的异常，不是"顺手加的守卫"。
 * ★ 写目标**只记录、不建模**：`Engine+0x1A68`（= `1690*4`）那张索引表的内容**没有取证**
 *   （没有已知的写入点）⇒ 猜"它是 0"就等于凭空造一条结论。于是这里发一条 `logged-only`，
 *   它会出现在 `[保真欠账]` 里（⛔ 不许静默）。
 */
const opSetKeyMulti: Handler = (ctx) => {
  const value = num(ctx, 0);
  const slotIndex = num(ctx, 1);
  if (value > 0x1f) {
    throw new Error(`0x10c 的操作数 ${value} 超出引擎的范围检查（> 0x1f）—— 引擎这里抛 aSetkeymulti 异常`);
  }
  ctx.machine.effect('system', 'engine.indirect-write', 'logged-only', {
    opcode: '0x10c', slotIndex, value,
    note: '写目标是 `Engine[1434 + Engine[1690 + op2]]` —— 索引表 `Engine+0x1A68` 的内容未取证 ⇒ 只记录、不建模（保真欠账）',
  });
};

/**
 * `ENGINE_SCALAR_ARRAYS` 的通用 handler：`Engine[base + 索引操作数] = 值操作数`。
 *
 * ★ 索引**必须进键名**（`Engine.d551` / `Engine.d552`…）：这类写入的下标来自操作数，
 *   若所有写入都落到同一个键上，两次不同的写会互相覆盖，而日志看不出异常。
 * ★ `outOfRange: 'skip'` ⇒ 越界**静默跳过**（照抄引擎），但记一笔（可见）。
 */
function scalarArrayHandler(spec: (typeof ENGINE_SCALAR_ARRAYS)[number]): Handler {
  return (ctx) => {
    const idx = num(ctx, spec.indexOperand);
    const value = num(ctx, spec.valueOperand);
    const maxValue = (spec as { maxValue?: number }).maxValue;
    const bad = idx > spec.maxIndex || (maxValue !== undefined && value > maxValue);
    if (bad) {
      // ★ 越界口径**由知识层说了算**（它是逐字读出来的）：`skip` = 引擎静默跳过、`throw` = 引擎抛异常。
      //   ⛔ 不许把两者统一 —— 那会把"引擎会崩"与"引擎当无事发生"变成同一种表现。
      if (spec.outOfRange === 'skip') {
        ctx.machine.note('scalar-array-skip', `opcode 0x${spec.opcode.toString(16)} 越界（索引 ${idx} > 0x${spec.maxIndex.toString(16)}${maxValue !== undefined ? ` 或值 ${value} > 0x${maxValue.toString(16)}` : ''}）⇒ 引擎**跳过**这次写（不抛）`);
        return;
      }
      throw new Error(
        `opcode 0x${spec.opcode.toString(16)} 越界：索引 ${idx}（上界 ${spec.maxIndex}）` +
        `${maxValue !== undefined ? ` / 值 ${value}（上界 0x${maxValue.toString(16)}）` : ''} —— 引擎这里抛异常`,
      );
    }
    const key = `${spec.name}+${idx}`;
    ctx.machine.scalars.write(key, value);
    ctx.machine.effect('system', 'engine.scalar.array-write', 'modeled', {
      opcode: `0x${spec.opcode.toString(16)}`, slot: key, index: idx, value,
    });
  };
}

/** 生成"响亮失败"的 handler：错误信息里必须写明**缺哪条取证**，不许只说不支持 */
const unverifiedValueProducer = (e: (typeof VALUE_PRODUCING_UNVERIFIED)[number]): Handler => (ctx) => {
  throw new Error(
    `opcode 0x${e.opcode.toString(16)} 的 handler（${e.handler}）**存在**，但它的返回值语义未取证 ⇒ 本层拒绝执行。` +
    `★ 这类 handler 与"转发"不同：它的结果会写回操作数并影响后续分支，给假值会造成**静默分歧**。` +
    `缺的取证：${e.why}`,
  );
  void ctx;
};

/** 由 `PROLOGUE` 表生成注册项（★ 新增一条 = 在表里加一行，不必写函数） */
function prologueHandlers(): Record<number, Handler> {
  const out: Record<number, Handler> = {};
  for (const e of PROLOGUE) {
    if (e.kind === 'noop') out[e.opcode] = opNoop1a8;
    else if (e.kind === 'forward') out[e.opcode] = forwardHandler(e);
  }
  return {
    ...out,
    0x149: opScalar149, 0x21b: opScalar21b, 0x252: opScalar252,
    0x76: opScalar76, 0x77: opScalar77, 0x1a4: opScalar1a4, 0x2ee: opScalar2ee, 0xfe: opScalarFe, 0x10f: opScalar10f, 0x25b: opSetMessage25b, 0x248: opScalar248, 0x10c: opSetKeyMulti,
    0x110: opUnsupported110, 0x111: opUnsupported111, 0x112: opUnsupported112,
  0x107: scalarArrayHandler(ENGINE_SCALAR_ARRAYS[0]),
  0x10b: scalarArrayHandler(ENGINE_SCALAR_ARRAYS[1]),
  0x30a: scalarArrayHandler(ENGINE_SCALAR_ARRAYS[2]), 0x78: opScalar78, 0x2db: opScalar2db, 0x88: opScalar88,
  };
}

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

/**
 * `0x1aa load-string`：把配置里那个字符串读回**操作数 1**。**查不到 ⇒ 空串**（⛔ 不是 0）。
 *
 * 取证（锚 = EA）：handler = `sub_433A70`（表项 `Engine+0x0A5744` ⇒ `(0xA5744−0xA509C)/4 = 0x1AA` ✓），体逐字：
 * ```
 *   this[30*cur + 95805] = 3                  ; 长度字 ⇒ argc 1
 *   v2 = sub_418AE0(this, 1)                  ; 键的下标（type 5 ⇒ 直接是 raw）
 *   v3 = sub_429390(this + 5191, 5, v2)       ; 查字符串配置对象（Engine+0x511C；表在其 +0x464 = Engine+0x5580）
 *   return sub_433310(this, 1, (int)v3)       ; 把结果写回 op1（与 set-string **同一个**写目标函数）
 * ```
 * ★ 查不到时 `sub_429390` 返回的是**一个静态空串**的地址 ⇒ 本层读回 **`''`**。
 * ★ 与 `save-string`（`sub_434FE0`）对称：同一套键、同一个对象。★ `Engine+0x511C` 与
 *   `save-string` 那侧看到的 `Engine+0x5580` 是**同一个对象的两层**（`0x511C + 281*4 = 0x5580`），
 *   不是两个不同的对象 —— 这条把早先一处"对象 EA 不一致"的记录订正了。
 */
const opLoadString: Handler = (ctx) => {
  const arg = ctx.ins.args[0];
  if (arg.type !== 5 && arg.type !== 0xb) {
    throw new Error(
      `load-string 的操作数 type 0x${arg.type.toString(16)}：键的下标只对字符串池（type 5/11）取证过` +
      `（type 8/14 要按指针与池基址反算，需要地址空间）⇒ 拒绝猜`,
    );
  }
  const index = arg.rawData >>> 0;
  const key = `\u0005${index.toString(16).padStart(8, '0')}`;
  const stored = ctx.machine.instance.config.get(key);
  const text = stored ?? '';
  writeOperand(operandCtx(ctx), arg, 0, text);
  ctx.machine.effect('system', 'config.read', 'modeled', {
    opcode: '0x1aa', key: printableKey(key), stored: stored ?? null, value: text, kind: 'string',
    note: '查不到 ⇒ **空串**（引擎返回一个静态空串的地址，不是 0）',
  });
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
  0x1a2: opSaveInt,
  0x1a3: opLoadInt,
  0x192: opSetString,
  0x1a9: opSaveString,
  0x1aa: opLoadString,
  0x6c: opFillZero,
  0x2d8: opSetArrayTo,
  0x2de: opFontIndex2de,
  0x61: opLookupArray,
  0x64: opCopyLocalArray,
  // ★ 有返回值但语义未取证的那些：**注册成响亮失败**（不是"还没轮到"，见上面的长注释）
  ...Object.fromEntries(VALUE_PRODUCING_UNVERIFIED.map((e) => [e.opcode, unverifiedValueProducer(e)])),
  0x1a7: opComment,
  0x101: opPollInput,
  0x21c: opWait,

  // —— 启动链前段（表驱动；`forward` 那些**未建模**、逐次留痕）——
  ...prologueHandlers(),
  ...Object.fromEntries(OBJECT_FIELD_WRITES.map((e) => [e.opcode, objectFieldWriter(e)])),

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
