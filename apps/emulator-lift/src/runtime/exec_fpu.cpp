// emulator-lift · x87（S4：语料里 47 个形态）+ MMX/SSE 整数打包
//
// ★ **D49（2026-11-15 用户裁决）：x87 的实数模型统一成 `double`。**
//   「这里 x87 统一用 double 模拟实现。不追求 80bit 完全一致。」
//   ⇒ 算术内核不再是 Berkeley SoftFloat 的 `extF80`，而是 `runtime/fpu_bridge.h` 里那份
//     **double 模型**（值怎么算）+ 本文件的**寄存器栈 / tag / 控制字 / 状态字 / 条件码**。
//   ★ **一份模型，两臂共用**：AOT 发射器把同一套语义发成普通 LLVM 浮点指令
//     （`src/aot/lift_gen_emit.cpp` 的 `emitX87*`）⇒ 差分夹具对 x87 仍然有意义。
//   ⛔ 不许"AOT 用 double、解释器留 extF80"：那会造出两个 x87 模型，差异还是静默的。
//
// ★ 接受的偏差（与 `fpu_bridge.h` 文件头同一份清单）：尾数 53 位 · PC 字段无效 ·
//   四则一律就近舍入（RC 只影响 `fist`/`fistp`/`frndint`/`fisttp`）· 异常位只建模 IE/ZE/OE ·
//   NaN 传播 payload / pseudo-denormal 不追。
//
// ★ 超越函数（`fsin` `fcos` `fsincos` `fptan` `fpatan` `fyl2x` `fyl2xp1` `f2xm1` `fscale`
//   `fprem` `fprem1` `fxtract`）走**宿主 libm**，实现是 `fpu_bridge.cpp` 的那几个函数 ——
//   生成代码的 `lift_x87_*` helper 调的是**同一份**（⛔ 不在发射器里再写一遍公式）。
//   1 ulp 偏差登记照旧：Intel SDM 明说这些助记符是 *implementation defined*。
//
// ★ MMX 复用 x87 寄存器槽的**低 64 位**（Intel：MMX 与 x87 共用物理寄存器）⇒ 现在就是
//   `double` 槽的 8 字节。SSE 的 XMM 在 `X86::xmm[8][4]`。`emms` 把 tag word 置全 empty。

// ★ SSE 浮点 / 128 位搬移是独立一族（`Op::Sse` + `execSse()`，见文件下半部）；语料 0 条，
//   但按 Intel SDM 实现（不再是"未实现 ⇒ 响亮失败"）。
// `movsd`（SSE2 标量 double）与串操作 `movsd` 同名 ⇒ 由解码器按操作数形态分派（见 decode.cpp）。
// ⛔ 未实现形态（例如 SSE 的 packed 浮点 addps / mulps 等不在 15 条闭集合里的）仍然走
//   `unimplemented()` 响亮失败（`addps` 仍落 `Op::Other`，守卫见 tests/dispatch.cpp）。
#include <cmath>
#include <cstdio>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/fpu_bridge.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// ---- x87 状态字位（Intel SDM Vol.1 §8.1.4）----
enum : std::uint16_t {
    SW_IE = 0x0001,  // invalid
    SW_DE = 0x0002,  // denormal
    SW_ZE = 0x0004,  // divide-by-zero
    SW_OE = 0x0008,  // overflow
    SW_UE = 0x0010,  // underflow
    SW_PE = 0x0020,  // precision
    SW_SF = 0x0040,  // stack fault
    SW_ES = 0x0080,  // exception summary
    SW_C0 = 0x0100,
    SW_C1 = 0x0200,
    SW_C2 = 0x0400,
    SW_C3 = 0x4000,
    SW_CC = 0x4700,  // C0 | C1 | C2 | C3
    SW_TOP = 0x3800,
};

double* stSlot(X86& cpu, int i) { return &cpu.fpu.regs[cpu.fpu.phys(i)]; }
const double* stConst(const X86& cpu, int i) { return &cpu.fpu.regs[cpu.fpu.phys(i)]; }

int stIndex(const Operand& op, int def) {
    return (op.kind == OpKind::Reg) ? static_cast<int>(regSpec(op.reg).index) : def;
}

void setCC(X86& cpu, bool c0, bool c1, bool c2, bool c3) {
    std::uint16_t s = cpu.fpu.status;
    s = static_cast<std::uint16_t>(s & ~SW_CC);
    if (c0) s |= SW_C0;
    if (c1) s |= SW_C1;
    if (c2) s |= SW_C2;
    if (c3) s |= SW_C3;
    cpu.fpu.status = s;
}

// 挂账的浮点异常位 → x87 状态字（取走并清空；对应"读状态字"这一步）
// ★ 任务 0 / D55：**IE / ZE / OE / UE 都会被置**（PE 恒 0，见 `fpu_bridge.h` 文件头 §5）；
//   **ES（异常汇总）= 状态字里任一异常位非零** —— 与 D49 之前的实现同一条（真机也这样，
//   栈上溢 = IE + ES）。SF（stack fault 位）仍恒 0（本轮不补，见 `fpu_bridge.h` §5）。
void syncStatus(X86& cpu) {
    const std::uint8_t f = x87TakeExceptionFlags();
    std::uint16_t s = cpu.fpu.status;
    if (f & kX87PE) s |= SW_PE;
    if (f & kX87UE) s |= SW_UE;
    if (f & kX87OE) s |= SW_OE;
    if (f & kX87ZE) s |= SW_ZE;
    if (f & kX87IE) s |= SW_IE;
    if ((s & 0x003Fu) != 0) s |= SW_ES;  // 汇总位：任一异常位 ⇒ ES

    s = static_cast<std::uint16_t>((s & ~SW_TOP) | ((cpu.fpu.top & 7u) << 11));
    cpu.fpu.status = s;
}

void markValid(X86& cpu, int physical) {
    cpu.fpu.setValid(physical);
    if (x87IsZero(cpu.fpu.regs[physical])) cpu.fpu.setZero(physical);
}

void push(X86& cpu, double v) {
    const int next = (cpu.fpu.top - 1) & 7;
    if (!cpu.fpu.empty(next)) {
        // 栈上溢：置 C1 + invalid（Intel：x87 报 stack overflow 后退回原状）
        x87RaiseInvalid();
        setCC(cpu, false, true, false, false);
        return;
    }
    cpu.fpu.top = static_cast<std::uint8_t>(next);
    cpu.fpu.regs[cpu.fpu.top] = v;
    markValid(cpu, cpu.fpu.top);
}

void pop(X86& cpu) {
    cpu.fpu.setEmpty(cpu.fpu.top);
    cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.top + 1) & 7);
    // ★ 检查点 2（D49）：**把状态字的 TOP 镜像一并更新**（原来是"下一次 syncStatus 才更新"）。
    //   理由：生成代码（`lift_gen_emit.cpp` 的 `x87Pop`）也这么做 ⇒ 两条臂的状态字逐位可比。
    //   这条改动对解释器**不可观察**：状态字的唯一读出口（`fnstsw`/`fnstenv`/`fnsave`）本来
    //   都会先 `syncStatus`；改的只是"两次读之间那个没人看的字段"。
    cpu.fpu.status =
        static_cast<std::uint16_t>((cpu.fpu.status & ~SW_TOP) | ((cpu.fpu.top & 7u) << 11));
}

void storeSt0(X86& cpu, double v) {
    *stSlot(cpu, 0) = v;
    markValid(cpu, cpu.fpu.phys(0));
    syncStatus(cpu);
}

// 取内存操作数（按它的宽度 4 / 8 / 10）；返回 false ⇒ 不支持
bool loadMemValue(X86& cpu, const MemOperand& m, double* out) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 4: {
            std::uint8_t b[4];
            for (int i = 0; i < 4; ++i) b[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
            *out = x87LoadMem(b, 4);
            return true;
        }
        case 8: {
            std::uint8_t b[8];
            for (int i = 0; i < 8; ++i) b[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
            *out = x87LoadMem(b, 8);
            return true;
        }
        case 10: {
            std::uint8_t b[10];
            for (int i = 0; i < 10; ++i) b[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
            *out = x87LoadMem(b, 10);
            return true;
        }
        default: return false;
    }
}

bool loadIntMem(X86& cpu, const MemOperand& m, double* out) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 2: *out = x87FromI16(static_cast<std::int16_t>(cpu.mem->rd16(ea))); return true;
        case 4: *out = x87FromI32(static_cast<std::int32_t>(cpu.mem->rd32(ea))); return true;
        case 8: *out = x87FromI64(static_cast<std::int64_t>(cpu.mem->rd64(ea))); return true;
        default: return false;
    }
}

void storeMemValue(X86& cpu, const MemOperand& m, double v) {
    const std::uint32_t ea = cpu.eaOf(m);
    std::uint8_t b[10] = {};
    const std::uint8_t size = m.size ? m.size : 4;
    x87StoreMem(v, b, size);
    for (int i = 0; i < size; ++i) cpu.mem->wr8(ea + static_cast<std::uint32_t>(i), b[i]);
}

// ★ `fist`/`fistp` 按**当前 RC** 舍入（`fisttp` 恒朝零）—— D49 §3 的"整数转换至少要把
//   截断 vs 就近分对"。⚠ 与旧实现的判据保持一致：`m16` 也走 i32 的转换路径再截低 16 位
//   （所以 16 位越界**不会**置 IE —— 这是既有口径，本轮不发明新语义）。
void storeIntMem(X86& cpu, const MemOperand& m, double v, X87Round rc) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 2: cpu.mem->wr16(ea, static_cast<std::uint16_t>(x87ToI32(v, rc))); break;
        case 4: cpu.mem->wr32(ea, static_cast<std::uint32_t>(x87ToI32(v, rc))); break;
        case 8: cpu.mem->wr64(ea, static_cast<std::uint64_t>(x87ToI64(v, rc))); break;
        default: break;
    }
}

// 二元算术：dst 与 src 的取法覆盖 `fadd st(i), st` / `fadd st, st(i)` / `fadd [mem]` 三种形态
bool arith(X86& cpu, const Insn& in, Op op) {
    double a = 0.0;
    double b = 0.0;
    int dst_index = 0;  // 结果写回哪个 ST（0 = ST0）
    if (in.op0().kind == OpKind::Mem) {
        if (!loadMemValue(cpu, in.op0().mem, &b)) return unimplemented(cpu, in, "x87 算术的内存操作数宽度不支持");
        a = *stConst(cpu, 0);
        dst_index = 0;
    } else {
        // 寄存器形态：op0 是目标 ST(i)，op1 是源（省略时源 = ST0）
        const int i0 = static_cast<int>(regSpec(in.op0().reg).index);
        dst_index = i0;
        a = *stConst(cpu, i0);
        if (in.op1().kind == OpKind::Reg) {
            const int i1 = static_cast<int>(regSpec(in.op1().reg).index);
            b = *stConst(cpu, i1);
        } else if (in.op1().kind == OpKind::Mem) {
            if (!loadMemValue(cpu, in.op1().mem, &b)) return unimplemented(cpu, in, "x87 算术的内存操作数宽度不支持");
        } else {
            b = *stConst(cpu, 0);
        }
    }
    // ★ D49：四则一律就近舍入（RC **不**参与；⛔ 不逐 op fesetround）
    double r = 0.0;
    switch (op) {
        case Op::Fadd: r = x87Add(a, b); break;
        case Op::Fsub: r = x87Sub(a, b); break;
        case Op::Fsubr: r = x87Sub(b, a); break;
        case Op::Fmul: r = x87Mul(a, b); break;
        case Op::Fdiv: r = x87Div(a, b); break;
        case Op::Fdivr: r = x87Div(b, a); break;
        default: return unimplemented(cpu, in, "x87 算术族没有这条语义");
    }
    *stSlot(cpu, dst_index) = r;
    markValid(cpu, cpu.fpu.phys(dst_index));
    syncStatus(cpu);
    return true;
}

bool integerArith(X86& cpu, const Insn& in, Op op) {
    if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fiadd 族只支持内存操作数");
    double b = 0.0;
    if (!loadIntMem(cpu, in.op0().mem, &b)) return unimplemented(cpu, in, "fiadd 族的整数宽度不支持");
    const double a = *stConst(cpu, 0);
    double r = 0.0;
    switch (op) {
        case Op::Fiadd: r = x87Add(a, b); break;
        case Op::Fisub: r = x87Sub(a, b); break;
        case Op::Fisubr: r = x87Sub(b, a); break;
        case Op::Fimul: r = x87Mul(a, b); break;
        case Op::Fidiv: r = x87Div(a, b); break;
        case Op::Fidivr: r = x87Div(b, a); break;
        default: return unimplemented(cpu, in, "fi 算术族没有这条语义");
    }
    *stSlot(cpu, 0) = r;
    markValid(cpu, cpu.fpu.phys(0));
    syncStatus(cpu);
    return true;
}

bool compare(X86& cpu, const Insn& in, bool pop_after, bool quiet) {
    double b = 0.0;
    // ★ **操作数形态在这里不统一**（实测 Zydis，见 `tests/conformance_cases.cpp` 的 `x87-fucomp`）：
    //     `fcom st(i)`  / `fcomp st(i)`  ⇒ **一个**操作数（就是源）；
    //     `fucom st(i)` / `fucomp st(i)` ⇒ **两个**操作数 `st0, st(i)`（Zydis 把隐含的 ST(0) 也列出来）；
    //     `fcompp` / `fucompp`           ⇒ **零个**操作数（隐含 ST(1)）。
    //   旧实现只看 `op0` ⇒ 对 `fucom`/`fucomp` 拿到的源是 **ST(0) 自己**（永远"相等"，C3=1），
    //   是**静默算错**（语料里 fucomp 11 条 / fucom 1 条）。⇒ 先看 `op1` 是不是寄存器。
    if (in.op1().kind == OpKind::Reg) {
        b = *stConst(cpu, stIndex(in.op1(), 1));
    } else if (in.op0().kind == OpKind::Reg) {
        b = *stConst(cpu, stIndex(in.op0(), 1));
    } else if (in.op0().kind == OpKind::Mem) {
        if (!loadMemValue(cpu, in.op0().mem, &b)) return unimplemented(cpu, in, "fcom 的内存操作数宽度不支持");
    } else {
        // ★ **无显式操作数**的 `fcom` / `fcomp` / `fucom` / `fucomp` / `fcompp` / `fucompp`
        //   比的是 **ST(1)**，不是 ST(0)（Intel SDM：隐式源操作数是 ST(1)）。
        b = *stConst(cpu, 1);
    }
    // FCOM 对 QNaN 也要报 invalid；FUCOM 是 quiet（只有 SNaN 报）—— 判据在 `x87Compare` 里唯一一份。
    const int cmp = x87Compare(*stConst(cpu, 0), b, quiet);
    if (cmp == 2) {
        setCC(cpu, true, false, true, true);  // 无序：C3=C2=C0=1
    } else if (cmp == 0) {
        setCC(cpu, false, false, false, true);
    } else if (cmp < 0) {
        setCC(cpu, true, false, false, false);
    } else {
        setCC(cpu, false, false, false, false);
    }
    syncStatus(cpu);
    if (pop_after) pop(cpu);
    return true;
}

// `fcomi` / `fcomip` / `fucomi` / `fucomip`：结果进 **EFLAGS**（ZF/PF/CF），不动 C0..C3
bool compareEflags(X86& cpu, const Insn& in, bool pop_after, bool quiet) {
    const int i = (in.op1().kind == OpKind::Reg) ? stIndex(in.op1(), 0) : stIndex(in.op0(), 1);
    const double b = *stConst(cpu, i);
    const int cmp = x87Compare(*stConst(cpu, 0), b, quiet);
    cpu.setOf(false);
    cpu.setSf(false);
    cpu.setAf(false);
    if (cmp == 2) {  // 无序：ZF=PF=CF=1
        cpu.setZf(true);
        cpu.setPf(true);
        cpu.setCf(true);
    } else if (cmp == 0) {
        cpu.setZf(true);
        cpu.setPf(false);
        cpu.setCf(false);
    } else if (cmp < 0) {
        cpu.setZf(false);
        cpu.setPf(false);
        cpu.setCf(true);
    } else {
        cpu.setZf(false);
        cpu.setPf(false);
        cpu.setCf(false);
    }
    syncStatus(cpu);
    if (pop_after) pop(cpu);
    return true;
}

bool fcmov(X86& cpu, const Insn& in) {
    bool take = false;
    switch (in.op) {
        case Op::Fcmovb: take = cpu.cf(); break;
        case Op::Fcmove: take = cpu.zf(); break;
        case Op::Fcmovbe: take = cpu.cf() || cpu.zf(); break;
        case Op::Fcmovu: take = cpu.pf(); break;
        case Op::Fcmovnb: take = !cpu.cf(); break;
        case Op::Fcmovne: take = !cpu.zf(); break;
        case Op::Fcmovnbe: take = !cpu.cf() && !cpu.zf(); break;
        case Op::Fcmovnu: take = !cpu.pf(); break;
        default: return unimplemented(cpu, in, "fcmov 族没有这条条件");
    }
    const int i = (in.op1().kind == OpKind::Reg) ? stIndex(in.op1(), 1) : stIndex(in.op0(), 1);
    if (take) {
        *stSlot(cpu, 0) = *stConst(cpu, i);
        markValid(cpu, cpu.fpu.phys(0));
    }
    return true;
}

void doFninit(X86& cpu) {
    cpu.fpu.reset();
    x87ClearExceptionFlags();
}

// ---- 环境 / 状态保存区（32 位保护模式：28 字节环境 + 8×10 字节寄存器栈）----
void doSaveEnv(X86& cpu, std::uint32_t ea) {
    cpu.mem->wr32(ea + 0, cpu.fpu.control);            // FCW（低 16 位有效）
    cpu.mem->wr32(ea + 4, cpu.fpu.status);             // FSW（低 16 位有效）
    cpu.mem->wr32(ea + 8, cpu.fpu.tag);                // FTW（低 16 位有效）
    cpu.mem->wr32(ea + 12, 0);                         // FIP（本仓不跟踪指令指针）
    cpu.mem->wr32(ea + 16, 0);                         // FCS + FOP
    cpu.mem->wr32(ea + 20, 0);                         // FDP
    cpu.mem->wr32(ea + 24, 0);                         // FDS + 填充
}

void doLoadEnv(X86& cpu, std::uint32_t ea) {
    cpu.fpu.control = cpu.mem->rd16(ea + 0);
    cpu.fpu.status = cpu.mem->rd16(ea + 4);
    cpu.fpu.tag = cpu.mem->rd16(ea + 8);
    cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.status >> 11) & 7u);
    // ★ D49：控制字只存下来（PC 不生效，RC 由 `fist*`/`frndint` 现取）⇒ 这里没有别的动作。
}

void doFnsave(X86& cpu, std::uint32_t ea) {
    doSaveEnv(cpu, ea);
    for (int i = 0; i < 8; ++i) {
        // ★ 寄存器是 double ⇒ 以 **10 字节扩展格式**写进保存区（加宽是精确的；D49 的偏差只体现
        //   在低 11 位尾数恒 0）。`frstor` 读回来时再收窄 ⇒ 往返逐位一致（对可表示的 double）。
        std::uint8_t b[10] = {};
        x87StoreMem(cpu.fpu.regs[cpu.fpu.phys(i)], b, 10);
        for (int k = 0; k < 10; ++k) {
            cpu.mem->wr8(ea + 28u + static_cast<std::uint32_t>(i) * 10u + static_cast<std::uint32_t>(k), b[k]);
        }
    }
    doFninit(cpu);  // INTEL：FNSAVE 之后 FPU 处于 init 状态
}

void doFrstor(X86& cpu, std::uint32_t ea) {
    doLoadEnv(cpu, ea);
    for (int i = 0; i < 8; ++i) {
        std::uint8_t b[10] = {};
        for (int k = 0; k < 10; ++k) {
            b[k] = cpu.mem->rd8(ea + 28u + static_cast<std::uint32_t>(i) * 10u + static_cast<std::uint32_t>(k));
        }
        cpu.fpu.regs[cpu.fpu.phys(i)] = x87LoadMem(b, 10);
    }
    cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.status >> 11) & 7u);
}

void doFxam(X86& cpu) {
    bool c0 = false;
    bool c2 = false;
    bool c3 = false;
    bool sign = false;
    if (cpu.fpu.empty(cpu.fpu.phys(0))) {
        c3 = true;
        c0 = true;
    } else {
        sign = x87SignBit(*stConst(cpu, 0));
        switch (x87Classify(*stConst(cpu, 0))) {
            case X87Class::Nan: c0 = true; break;
            case X87Class::Normal: c2 = true; break;
            case X87Class::Infinity: c2 = true; c0 = true; break;
            case X87Class::Zero: c3 = true; break;
            case X87Class::Denormal: c3 = true; c2 = true; break;
        }
    }
    setCC(cpu, c0, sign, c2, c3);  // C1 = 符号位
}

// 超越函数 / 实现定义族：全走 `fpu_bridge.cpp` 的宿主 libm 入口（D49：一份模型，两臂共用
// —— 生成代码的 `lift_x87_*` helper 调的是**同一批**函数）。
bool transcendental(X86& cpu, const Insn& in) {
    const double x = *stConst(cpu, 0);
    switch (in.op) {
        case Op::Fsin:
        case Op::Fcos: {
            // Intel：|ST(0)| >= 2^63 ⇒ C2=1，ST(0) 不变
            if (!x87TrigInRange(x)) {
                setCC(cpu, false, false, true, false);
                syncStatus(cpu);
                return true;
            }
            storeSt0(cpu, in.op == Op::Fsin ? x87Sin(x) : x87Cos(x));
            setCC(cpu, false, false, false, false);
            return true;
        }
        case Op::Fsincos: {
            if (!x87TrigInRange(x)) {
                setCC(cpu, false, false, true, false);
                syncStatus(cpu);
                return true;
            }
            // Intel：正弦写 ST(0)，**再把余弦压栈** ⇒ 最终 ST(0)=cos、ST(1)=sin
            *stSlot(cpu, 0) = x87Sin(x);
            markValid(cpu, cpu.fpu.phys(0));
            push(cpu, x87Cos(x));
            setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::Fptan: {
            storeSt0(cpu, x87Tan(x));
            push(cpu, 1.0);  // Intel：结果留在 ST(1)，再把 1.0 压栈
            setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::Fpatan: {
            const double y = *stConst(cpu, 1);
            *stSlot(cpu, 1) = x87Atan2(y, x);  // 写 ST(1)，然后 pop ⇒ 结果落到 ST(0)
            markValid(cpu, cpu.fpu.phys(1));
            pop(cpu);
            setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::Fyl2x:
        case Op::Fyl2xp1: {
            const double y = *stConst(cpu, 1);
            const double v = (in.op == Op::Fyl2x) ? x87Log2(x) : x87Log2p1(x);
            *stSlot(cpu, 1) = x87Mul(y, v);
            markValid(cpu, cpu.fpu.phys(1));
            pop(cpu);
            setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::F2xm1: {
            storeSt0(cpu, x87Exp2m1(x));
            setCC(cpu, false, false, false, false);
            return true;
        }
        default: return unimplemented(cpu, in, "超越函数族没有这条语义");
    }
}

// =====================================================================================
// MMX / SSE：整数打包（`Op::Packed`）
// ★ 执行宽度 = 目的操作数的宽度：MM 寄存器 / m64 = 8 字节，XMM = 16 字节；两种形态共用同一套
//   按 lane 写的代码。⛔ 浮点 SSE（addps / movaps 这类）语料里没有 ⇒ 不解码 ⇒ 响亮失败。
// =====================================================================================
struct PackVec {
    std::uint8_t b[16] = {};
};

std::uint16_t pkRd16(const std::uint8_t* p, int i) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[i]) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[i + 1]) << 8));
}
void pkWr16(std::uint8_t* p, int i, std::uint16_t v) {
    p[i] = static_cast<std::uint8_t>(v & 0xFFu);
    p[i + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}
std::uint32_t pkRd32(const std::uint8_t* p, int i) {
    return static_cast<std::uint32_t>(p[i]) | (static_cast<std::uint32_t>(p[i + 1]) << 8) |
           (static_cast<std::uint32_t>(p[i + 2]) << 16) | (static_cast<std::uint32_t>(p[i + 3]) << 24);
}
void pkWr32(std::uint8_t* p, int i, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) p[i + k] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFFu);
}
std::uint64_t pkRd64(const std::uint8_t* p, int i) {
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v |= static_cast<std::uint64_t>(p[i + k]) << (8 * k);
    return v;
}
void pkWr64(std::uint8_t* p, int i, std::uint64_t v) {
    for (int k = 0; k < 8; ++k) p[i + k] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFFu);
}

std::int64_t pkClamp(std::int64_t v, std::int64_t lo, std::int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool pkIsMm(const Operand& op) {
    return op.kind == OpKind::Reg && regSpec(op.reg).cls == RegClass::X87;
}
bool pkIsVec(const Operand& op) { return pkIsMm(op) || op.kind == OpKind::Xmm; }

bool pkLoad(X86& cpu, const Operand& op, PackVec& v, int* bytes) {
    std::memset(v.b, 0, sizeof(v.b));
    switch (op.kind) {
        case OpKind::Reg: {
            const RegSpec sp = regSpec(op.reg);
            if (sp.cls == RegClass::X87) {
                // MMX 复用 x87 物理槽的低 64 位（小端 ⇒ 低 8 字节）。
                // ★ **不能过 `phys()`**：`ST(i) = phys(TOP+i)` 是 x87 的栈旋转，而 MM0..MM7 是
                //   **固定的物理槽 R0..R7**（Intel SDM Vol.1 §8.1.2 / §9.2：MMX 寄存器别名的是
                //   8 个 x87 数据寄存器本身，与 TOP 无关）。用 `phys()` 会让 TOP≠0 时的 MMX
                //   读写落到别的槽上 —— 那是**静默算错**（同一份 phys() 既写又读会自洽地掩盖它，
                //   所以 `mmx-*` 那几条从 TOP=0 开始的夹具都照不到）。守卫：`mmx-top-rotated`。
                std::memcpy(v.b, &cpu.fpu.regs[sp.index], 8);
                *bytes = 8;
                return true;
            }
            if (sp.cls == RegClass::Gpr32 || sp.cls == RegClass::Gpr16 || sp.cls == RegClass::Gpr8) {
                const std::uint32_t d = cpu.readReg(op.reg);
                pkWr32(v.b, 0, d);
                *bytes = 4;
                return true;
            }
            return false;
        }
        case OpKind::Xmm: {
            if (op.vec >= 8) return false;
            std::memcpy(v.b, cpu.xmm[op.vec], 16);
            *bytes = 16;
            return true;
        }
        case OpKind::Mem: {
            const int n = op.mem.size;
            if (n != 4 && n != 8 && n != 16) return false;
            const std::uint32_t ea = cpu.eaOf(op.mem);
            for (int i = 0; i < n; ++i) v.b[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
            *bytes = n;
            return true;
        }
        default: return false;
    }
}

bool pkStore(X86& cpu, const Operand& op, const PackVec& v, int bytes) {
    switch (op.kind) {
        case OpKind::Reg: {
            const RegSpec sp = regSpec(op.reg);
            if (sp.cls != RegClass::X87) return false;
            // MMX 写只覆盖低 64 位；★ 不动 tag word（Intel：MMX 指令不影响 x87 tag word）。
            // ★ 同样**不能过 `phys()`**（理由见 `pkLoad`）：MM0..MM7 = 固定物理槽 R0..R7。
            std::memcpy(&cpu.fpu.regs[sp.index], v.b, 8);
            return true;
        }
        case OpKind::Xmm: {
            if (op.vec >= 8) return false;
            std::memcpy(cpu.xmm[op.vec], v.b, 16);
            return true;
        }
        case OpKind::Mem: {
            const std::uint32_t ea = cpu.eaOf(op.mem);
            for (int i = 0; i < bytes; ++i) cpu.mem->wr8(ea + static_cast<std::uint32_t>(i), v.b[i]);
            return true;
        }
        default: return false;
    }
}

void pkUnpack(std::uint8_t* d, const std::uint8_t* s, int bytes, int elem, bool high) {
    std::uint8_t a[16];
    std::uint8_t b[16];
    std::memcpy(a, d, static_cast<std::size_t>(bytes));
    std::memcpy(b, s, static_cast<std::size_t>(bytes));
    const int half = bytes / 2;
    const int elems = half / elem;
    const int off = high ? half : 0;
    for (int i = 0; i < elems; ++i) {
        std::memcpy(d + (2 * i) * elem, a + off + i * elem, static_cast<std::size_t>(elem));
        std::memcpy(d + (2 * i + 1) * elem, b + off + i * elem, static_cast<std::size_t>(elem));
    }
}

void pkPack(PkOp op, std::uint8_t* d, const std::uint8_t* s, int bytes) {
    std::uint8_t a[16];
    std::uint8_t b[16];
    std::memcpy(a, d, static_cast<std::size_t>(bytes));
    std::memcpy(b, s, static_cast<std::size_t>(bytes));
    if (op == PkOp::Packsswb || op == PkOp::Packuswb) {  // 源：8/16 位词 → 8 位字节
        const int n = bytes / 2;
        for (int i = 0; i < n; ++i) {
            const std::int64_t va = static_cast<std::int16_t>(pkRd16(a, i * 2));
            const std::int64_t vb = static_cast<std::int16_t>(pkRd16(b, i * 2));
            if (op == PkOp::Packsswb) {
                d[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(pkClamp(va, -128, 127)));
                d[n + i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(pkClamp(vb, -128, 127)));
            } else {
                d[i] = static_cast<std::uint8_t>(pkClamp(va, 0, 255));
                d[n + i] = static_cast<std::uint8_t>(pkClamp(vb, 0, 255));
            }
        }
    } else {  // Packssdw：源 32 位双字 → 16 位词
        const int n = bytes / 4;
        for (int i = 0; i < n; ++i) {
            const std::int64_t va = static_cast<std::int32_t>(pkRd32(a, i * 4));
            const std::int64_t vb = static_cast<std::int32_t>(pkRd32(b, i * 4));
            pkWr16(d, (i) * 2, static_cast<std::uint16_t>(pkClamp(va, -32768, 32767)));
            pkWr16(d, (n + i) * 2, static_cast<std::uint16_t>(pkClamp(vb, -32768, 32767)));
        }
    }
}

void pkShift(PkOp op, std::uint8_t* d, int bytes, std::uint32_t count) {
    switch (op) {
        case PkOp::Psllw:
            for (int i = 0; i < bytes / 2; ++i) {
                pkWr16(d, i * 2, count > 15 ? 0 : static_cast<std::uint16_t>(pkRd16(d, i * 2) << count));
            }
            break;
        case PkOp::Pslld:
            for (int i = 0; i < bytes / 4; ++i) {
                pkWr32(d, i * 4, count > 31 ? 0 : pkRd32(d, i * 4) << count);
            }
            break;
        case PkOp::Psllq:
            for (int i = 0; i < bytes / 8; ++i) {
                pkWr64(d, i * 8, count > 63 ? 0 : pkRd64(d, i * 8) << count);
            }
            break;
        case PkOp::Psrlw:
            for (int i = 0; i < bytes / 2; ++i) {
                pkWr16(d, i * 2, count > 15 ? 0 : static_cast<std::uint16_t>(pkRd16(d, i * 2) >> count));
            }
            break;
        case PkOp::Psrld:
            for (int i = 0; i < bytes / 4; ++i) {
                pkWr32(d, i * 4, count > 31 ? 0 : pkRd32(d, i * 4) >> count);
            }
            break;
        case PkOp::Psrlq:
            for (int i = 0; i < bytes / 8; ++i) {
                pkWr64(d, i * 8, count > 63 ? 0 : pkRd64(d, i * 8) >> count);
            }
            break;
        case PkOp::Psraw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::int32_t v = static_cast<std::int16_t>(pkRd16(d, i * 2));
                pkWr16(d, i * 2, count > 15 ? static_cast<std::uint16_t>(v < 0 ? 0xFFFF : 0)
                                            : static_cast<std::uint16_t>(v >> count));
            }
            break;
        case PkOp::Psrad:
            for (int i = 0; i < bytes / 4; ++i) {
                const std::int32_t v = static_cast<std::int32_t>(pkRd32(d, i * 4));
                // ★ 计数 > 31：整 lane 填符号位（不是"计数取低 5 位"）
                pkWr32(d, i * 4, count > 31 ? static_cast<std::uint32_t>(v >> 31) : static_cast<std::uint32_t>(v >> count));
            }
            break;
        default: break;
    }
}

void pkBinary(PkOp op, std::uint8_t* d, const std::uint8_t* s, int bytes) {
    switch (op) {
        case PkOp::Pand: for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(d[i] & s[i]); break;
        case PkOp::Pandn: for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(~d[i] & s[i]); break;
        case PkOp::Por: for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(d[i] | s[i]); break;
        case PkOp::Pxor: for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(d[i] ^ s[i]); break;

        case PkOp::PaddB:
            for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(d[i] + s[i]);
            break;
        case PkOp::PaddW:
            for (int i = 0; i < bytes / 2; ++i)
                pkWr16(d, i * 2, static_cast<std::uint16_t>(pkRd16(d, i * 2) + pkRd16(s, i * 2)));
            break;
        case PkOp::PaddD:
            for (int i = 0; i < bytes / 4; ++i) pkWr32(d, i * 4, pkRd32(d, i * 4) + pkRd32(s, i * 4));
            break;
        case PkOp::PaddQ:
            for (int i = 0; i < bytes / 8; ++i) pkWr64(d, i * 8, pkRd64(d, i * 8) + pkRd64(s, i * 8));
            break;
        case PkOp::PsubB:
            for (int i = 0; i < bytes; ++i) d[i] = static_cast<std::uint8_t>(d[i] - s[i]);
            break;
        case PkOp::PsubW:
            for (int i = 0; i < bytes / 2; ++i)
                pkWr16(d, i * 2, static_cast<std::uint16_t>(pkRd16(d, i * 2) - pkRd16(s, i * 2)));
            break;
        case PkOp::PsubD:
            for (int i = 0; i < bytes / 4; ++i) pkWr32(d, i * 4, pkRd32(d, i * 4) - pkRd32(s, i * 4));
            break;
        case PkOp::PsubQ:
            for (int i = 0; i < bytes / 8; ++i) pkWr64(d, i * 8, pkRd64(d, i * 8) - pkRd64(s, i * 8));
            break;

        case PkOp::PaddSb:
            for (int i = 0; i < bytes; ++i) {
                const std::int64_t v = static_cast<std::int8_t>(d[i]) + static_cast<std::int8_t>(s[i]);
                d[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(pkClamp(v, -128, 127)));
            }
            break;
        case PkOp::PaddSw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::int64_t v = static_cast<std::int16_t>(pkRd16(d, i * 2)) +
                                       static_cast<std::int16_t>(pkRd16(s, i * 2));
                pkWr16(d, i * 2, static_cast<std::uint16_t>(pkClamp(v, -32768, 32767)));
            }
            break;
        case PkOp::PaddUsb:
            for (int i = 0; i < bytes; ++i) {
                const int v = d[i] + s[i];
                d[i] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
            }
            break;
        case PkOp::PaddUsw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::uint32_t v = static_cast<std::uint32_t>(pkRd16(d, i * 2)) + pkRd16(s, i * 2);
                pkWr16(d, i * 2, static_cast<std::uint16_t>(v > 0xFFFFu ? 0xFFFFu : v));
            }
            break;

        case PkOp::PsubSb:
            for (int i = 0; i < bytes; ++i) {
                const std::int64_t v = static_cast<std::int8_t>(d[i]) - static_cast<std::int8_t>(s[i]);
                d[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(pkClamp(v, -128, 127)));
            }
            break;
        case PkOp::PsubSw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::int64_t v = static_cast<std::int16_t>(pkRd16(d, i * 2)) -
                                       static_cast<std::int16_t>(pkRd16(s, i * 2));
                pkWr16(d, i * 2, static_cast<std::uint16_t>(pkClamp(v, -32768, 32767)));
            }
            break;
        case PkOp::PsubUsb:
            for (int i = 0; i < bytes; ++i) {
                const int v = static_cast<int>(d[i]) - static_cast<int>(s[i]);
                d[i] = static_cast<std::uint8_t>(v < 0 ? 0 : v);
            }
            break;
        case PkOp::PsubUsw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::int32_t v =
                    static_cast<std::int32_t>(pkRd16(d, i * 2)) - static_cast<std::int32_t>(pkRd16(s, i * 2));
                pkWr16(d, i * 2, static_cast<std::uint16_t>(v < 0 ? 0 : v));
            }
            break;

        case PkOp::PcmpeqB:
            for (int i = 0; i < bytes; ++i) d[i] = (d[i] == s[i]) ? 0xFFu : 0x00u;
            break;
        case PkOp::PcmpeqW:
            for (int i = 0; i < bytes / 2; ++i)
                pkWr16(d, i * 2, pkRd16(d, i * 2) == pkRd16(s, i * 2) ? 0xFFFFu : 0x0000u);
            break;
        case PkOp::PcmpeqD:
            for (int i = 0; i < bytes / 4; ++i)
                pkWr32(d, i * 4, pkRd32(d, i * 4) == pkRd32(s, i * 4) ? 0xFFFFFFFFu : 0u);
            break;
        case PkOp::PcmpgtB:
            for (int i = 0; i < bytes; ++i)
                d[i] = (static_cast<std::int8_t>(d[i]) > static_cast<std::int8_t>(s[i])) ? 0xFFu : 0x00u;
            break;
        case PkOp::PcmpgtW:
            for (int i = 0; i < bytes / 2; ++i)
                pkWr16(d, i * 2, static_cast<std::int16_t>(pkRd16(d, i * 2)) >
                                          static_cast<std::int16_t>(pkRd16(s, i * 2))
                                      ? 0xFFFFu
                                      : 0x0000u);
            break;
        case PkOp::PcmpgtD:
            for (int i = 0; i < bytes / 4; ++i)
                pkWr32(d, i * 4, static_cast<std::int32_t>(pkRd32(d, i * 4)) >
                                          static_cast<std::int32_t>(pkRd32(s, i * 4))
                                      ? 0xFFFFFFFFu
                                      : 0u);
            break;

        case PkOp::Punpcklbw: pkUnpack(d, s, bytes, 1, false); break;
        case PkOp::Punpcklwd: pkUnpack(d, s, bytes, 2, false); break;
        case PkOp::Punpckldq: pkUnpack(d, s, bytes, 4, false); break;
        case PkOp::Punpcklqdq: pkUnpack(d, s, bytes, 8, false); break;
        case PkOp::Punpckhbw: pkUnpack(d, s, bytes, 1, true); break;
        case PkOp::Punpckhwd: pkUnpack(d, s, bytes, 2, true); break;
        case PkOp::Punpckhdq: pkUnpack(d, s, bytes, 4, true); break;
        case PkOp::Punpckhqdq: pkUnpack(d, s, bytes, 8, true); break;

        case PkOp::Packsswb:
        case PkOp::Packssdw:
        case PkOp::Packuswb: pkPack(op, d, s, bytes); break;

        case PkOp::Pmullw:
            for (int i = 0; i < bytes / 2; ++i)
                pkWr16(d, i * 2, static_cast<std::uint16_t>(
                                      (static_cast<std::uint32_t>(pkRd16(d, i * 2)) * pkRd16(s, i * 2)) & 0xFFFFu));
            break;
        case PkOp::Pmulhw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::int32_t p = static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(d, i * 2))) *
                                       static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(s, i * 2)));
                pkWr16(d, i * 2, static_cast<std::uint16_t>(static_cast<std::uint32_t>(p) >> 16));
            }
            break;
        case PkOp::Pmulhuw:
            for (int i = 0; i < bytes / 2; ++i) {
                const std::uint32_t p = static_cast<std::uint32_t>(pkRd16(d, i * 2)) * pkRd16(s, i * 2);
                pkWr16(d, i * 2, static_cast<std::uint16_t>(p >> 16));
            }
            break;
        case PkOp::Pmuludq:
            for (int i = 0; i < bytes / 4; i += 2) {
                const std::uint64_t p = static_cast<std::uint64_t>(pkRd32(d, i * 4)) *
                                        static_cast<std::uint64_t>(pkRd32(s, i * 4));
                pkWr64(d, i * 4, p);
            }
            break;
        case PkOp::Pmaddwd:
            for (int i = 0; i < bytes / 4; ++i) {
                const std::int32_t v = static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(d, i * 4))) *
                                           static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(s, i * 4))) +
                                       static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(d, i * 4 + 2))) *
                                           static_cast<std::int32_t>(static_cast<std::int16_t>(pkRd16(s, i * 4 + 2)));
                pkWr32(d, i * 4, static_cast<std::uint32_t>(v));
            }
            break;

        default: break;
    }
}

}  // namespace

// =====================================================================================
// MMX / SSE 的执行入口（`Op::Packed`）
// =====================================================================================
bool execPacked(X86& cpu, const Insn& in) {
    switch (in.pk) {
        case PkOp::None:
            return unimplemented(cpu, in, "Packed 指令没有子语义（解码器漏了映射）");
        case PkOp::Ems:
            // Intel：EMMS 把 x87 tag word 置全 empty（ST 栈变空），TOP 不动
            cpu.fpu.tag = 0xFFFFu;
            return true;
        case PkOp::Movd:
        case PkOp::Movq: {
            const bool q = (in.pk == PkOp::Movq);
            const int width = q ? 8 : 4;
            const Operand& dst = in.op0();
            const Operand& src = in.op1();
            PackVec v;
            int nb = 0;
            if (!pkLoad(cpu, src, v, &nb)) return unimplemented(cpu, in, "movd/movq 的源操作数形态不支持");
            (void)nb;
            if (pkIsVec(dst)) {
                // 写进 mm / xmm 都是**零扩展**：高 (16 − width) 字节清零
                for (int i = width; i < 16; ++i) v.b[i] = 0;
                if (!pkStore(cpu, dst, v, width)) return unimplemented(cpu, in, "movd/movq 的目标操作数形态不支持");
                return true;
            }
            if (dst.kind == OpKind::Reg) {
                if (q) return unimplemented(cpu, in, "32 位 guest 里没有 movq 到通用寄存器");
                cpu.writeReg(dst.reg, pkRd32(v.b, 0));
                return true;
            }
            if (dst.kind == OpKind::Mem) {
                if (!pkStore(cpu, dst, v, width)) return unimplemented(cpu, in, "movd/movq 的内存目标写失败");
                return true;
            }
            return unimplemented(cpu, in, "movd/movq 的目标操作数形态不支持");
        }
        default: break;
    }

    if (!pkIsVec(in.op0())) return unimplemented(cpu, in, "MMX/SSE 的打包指令要求目标是 mm / xmm 寄存器");
    PackVec a;
    int an = 0;
    if (!pkLoad(cpu, in.op0(), a, &an)) return unimplemented(cpu, in, "MMX/SSE 的目标操作数读不出来");
    const int bytes = (an == 16) ? 16 : 8;

    if (in.pk >= PkOp::Psllw && in.pk <= PkOp::Psrad) {
        std::uint32_t count = 0;
        if (in.op1().kind == OpKind::Imm) {
            // `psllw mm, imm8`：计数就是 imm8（零扩展；解码器可能把它符号扩成 32 位 ⇒ 取低 8 位）
            count = static_cast<std::uint32_t>(in.op1().imm & 0xFFu);
        } else {
            PackVec b;
            int bn = 0;
            if (!pkLoad(cpu, in.op1(), b, &bn)) return unimplemented(cpu, in, "MMX/SSE 的移位计数操作数不支持");
            // ★ 寄存器 / 内存形态的计数是**完整的 64 位值**（Intel SDM Vol.2 PSLLW…PSRAD：
            //   "If the source operand is a register or a 64-bit memory operand, the full 64-bit value
            //    is used"），只有 imm8 形态才被编码宽度限制成 8 位。
            //   旧实现一律 `& 0xFF` ⇒ 计数 256 被读成 0（本该整 lane 清 0 / 填符号位），是**静默算错**。
            //   这里只饱和到 uint32（`pkShift` 的判据是 `> 15 / > 31 / > 63`，饱和不影响判定）。
            const std::uint64_t full = pkRd64(b.b, 0);
            count = full > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(full);
        }
        pkShift(in.pk, a.b, bytes, count);
        if (!pkStore(cpu, in.op0(), a, bytes)) return unimplemented(cpu, in, "MMX/SSE 的目标写失败");
        return true;
    }

    PackVec b;
    int bn = 0;
    if (!pkLoad(cpu, in.op1(), b, &bn)) return unimplemented(cpu, in, "MMX/SSE 的源操作数形态不支持");
    const bool low_unpack = in.pk == PkOp::Punpcklbw || in.pk == PkOp::Punpcklwd || in.pk == PkOp::Punpckldq;
    if (bn != 8 && bn != 16 && !(low_unpack && bn == 4)) {
        return unimplemented(cpu, in, "MMX/SSE 的源宽度必须是 64 / 128 位（punpckl* 允许 m32）");
    }
    pkBinary(in.pk, a.b, b.b, bytes);
    if (!pkStore(cpu, in.op0(), a, bytes)) return unimplemented(cpu, in, "MMX/SSE 的目标写失败");
    return true;
}

// =====================================================================================
// SSE 标量单精度 + 128 位搬移 / 位运算（`Op::Sse`）
//
// ★ 为什么单独一族：这 15 条形态里，`xorps` 是**按位**的、`movaps` 是**整份 128 位**的，
//   而 `movss` / `movsd` / `cvtsi2ss` / `addss` 一族是**标量**的（只动低 32 / 低 64 位）。它们的共同点只是
//   "操作数是 XMM"；塞进 `PkOp` 的整数 lane 语义会把这几类混成一套（NaN / 舍入 / invalid
//   与整数溢出 / 饱和根本不是一回事）。
//
// ★ 128 位语义**逐条**钉住（出处 = Intel SDM Vol.2 各条目的 Operation 伪码）：
//   · `MOVSS xmm1, xmm2`（legacy，寄存器源）：DEST[31:0] := SRC[31:0]；DEST[MAXVL-1:32] **Unmodified**。
//   · `MOVSS xmm1, m32`（legacy，内存源）：DEST[31:0] := SRC[31:0]；**DEST[127:32] := 0**。
//   · `MOVSS m32, xmm1`：只写 4 字节（DEST[31:0] := SRC[31:0]）。
//   · `MOVSD xmm1, xmm2`（legacy，寄存器源）：DEST[63:0] := SRC[63:0]；DEST[127:64] **Unmodified**。
//   · `MOVSD xmm1, m64`（legacy，内存源）：DEST[63:0] := SRC；**DEST[127:64] := 0**。
//   · `MOVSD m64, xmm1`：只写 8 字节。
//   · `CVTSI2SS xmm1, r/m32`（legacy）：DEST[31:0] := Convert；**DEST[MAXVL-1:32] Unmodified**
//     —— 这正是"现行 SDM 文本"的口径（VEX 形态才复制 SRC1 的高位并清 MAXVL:128；本仓只解 legacy）。
//   · `ADDSS` / `SUBSS` / `MULSS` / `DIVSS xmm1, xmm2/m32`：只改 DEST[31:0]，高 96 位不动。
//   · `XORPS`：整份 128 位的按位异或，**不写任何 flag**（SDM：Flags Affected = None）。
//   · `COMISS` / `UCOMISS`：写 EFLAGS 的 ZF/PF/CF，**OF/SF/AF 清 0**；不碰 x87 的 C0..C3。
//   · `MOVAPS` / `MOVDQA` 要求 m128 **16 字节对齐**（否则 #GP）；`MOVUPS` / `MOVDQU` 不要求。
//
// ★ 对齐违例的取舍（本仓没有 guest 异常投递）：**响亮失败**（`unimplemented()` 打印 EA + 助记符 +
//   字节 + 寄存器后停机），⛔ 不静默按 MOVUPS 处理 —— 那会把"程序有 bug"变成静默的数据搬运。
//
// ★ 与 x87 / MMX 的交互：XMM 是**独立**寄存器文件，这里的指令**不碰** tag word / TOP / 状态字；
//   反方向 `emms` 也不碰 XMM（由 `execPacked` 的 `PkOp::Ems` 保证）。
//   浮点异常也不投递宿主异常：宿主 IEEE 默认环境（就近舍入、异常全屏蔽）下 NaN / ∞ 只是值。
// =====================================================================================
namespace {

bool sseIsNaN(std::uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u;
}
// SNaN = NaN 且 quiet 位（尾数最高位 bit22）为 0（Intel Vol.1 §4.8.3.5）
bool sseIsSnan(std::uint32_t bits) { return sseIsNaN(bits) && (bits & 0x00400000u) == 0u; }

float sseAsF32(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}
std::uint32_t sseBits32(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}

// invalid（#I）在**本模型**里的落点：MXCSR.IE。默认 MXCSR 异常全屏蔽 ⇒ 真机也只置位、不 trap，
//   且 EFLAGS 仍然更新（未屏蔽时 EFLAGS 不更新 —— 本仓没有投递路径，见 `X86::mxcsr` 的注释）。
void sseRaiseInvalid(X86& cpu) { cpu.mxcsr |= 0x00000001u; }

// 标量源：xmm 的低 32 位 / m32
bool sseScalarSrc(X86& cpu, const Operand& op, std::uint32_t* out) {
    if (op.kind == OpKind::Xmm) {
        if (op.vec >= 8) return false;
        *out = cpu.xmm[op.vec][0];
        return true;
    }
    if (op.kind == OpKind::Mem && op.mem.size == 4) {
        *out = cpu.mem->rd32(cpu.eaOf(op.mem));
        return true;
    }
    return false;
}

// 128 位源：xmm 整份 / m128
bool sseLoad128(X86& cpu, const Operand& op, std::uint8_t out[16]) {
    if (op.kind == OpKind::Xmm) {
        if (op.vec >= 8) return false;
        std::memcpy(out, cpu.xmmBytes(op.vec), 16);
        return true;
    }
    if (op.kind == OpKind::Mem && op.mem.size == 16) {
        const std::uint32_t ea = cpu.eaOf(op.mem);
        for (int i = 0; i < 16; ++i) out[i] = cpu.mem->rd8(ea + static_cast<std::uint32_t>(i));
        return true;
    }
    return false;
}

bool sseStore128(X86& cpu, const Operand& op, const std::uint8_t src[16]) {
    if (op.kind != OpKind::Mem || op.mem.size != 16) return false;
    const std::uint32_t ea = cpu.eaOf(op.mem);
    for (int i = 0; i < 16; ++i) cpu.mem->wr8(ea + static_cast<std::uint32_t>(i), src[i]);
    return true;
}

// MOVAPS / MOVDQA 的 m128 对齐检查（SDM：未对齐 ⇒ #GP）。非内存操作数一律通过。
// ⛔ 传**字符串字面量**给 `unimplemented()`：它把指针存进 `cpu.stop_why`（栈缓冲会悬空）。
bool sseRequireAligned(X86& cpu, const Operand& op, const Insn& in) {
    if (op.kind != OpKind::Mem) return true;
    if ((cpu.eaOf(op.mem) & 15u) == 0u) return true;
    return unimplemented(cpu, in, "MOVAPS/MOVDQA 的 m128 操作数未按 16 字节对齐（SDM：对齐违例 #GP）");
}

}  // namespace

bool execSse(X86& cpu, const Insn& in) {
    switch (in.sse) {
        case SseOp::None:
            return unimplemented(cpu, in, "SSE 指令没有子语义（解码器漏了映射）");

        // ---- 128 位搬移（整份覆盖）----
        case SseOp::Movaps:
        case SseOp::Movups:
        case SseOp::Movdqa:
        case SseOp::Movdqu: {
            const bool aligned = (in.sse == SseOp::Movaps || in.sse == SseOp::Movdqa);
            const Operand& dst = in.op0();
            const Operand& src = in.op1();
            if (dst.kind == OpKind::Xmm) {
                if (aligned && !sseRequireAligned(cpu, src, in)) return false;
                std::uint8_t buf[16] = {};
                if (!sseLoad128(cpu, src, buf)) return unimplemented(cpu, in, "SSE 搬移的源必须是 xmm / m128");
                if (dst.vec >= 8) return unimplemented(cpu, in, "SSE 搬移的目标 xmm 下标越界");
                std::memcpy(cpu.xmmBytes(dst.vec), buf, 16);  // 整份 128 位覆盖
                return true;
            }
            if (dst.kind == OpKind::Mem) {
                if (src.kind != OpKind::Xmm || src.vec >= 8) {
                    return unimplemented(cpu, in, "SSE 搬移到内存时源必须是 xmm");
                }
                if (aligned && !sseRequireAligned(cpu, dst, in)) return false;
                if (!sseStore128(cpu, dst, cpu.xmmBytes(src.vec))) {
                    return unimplemented(cpu, in, "SSE 搬移的内存目标必须是 m128");
                }
                return true;
            }
            return unimplemented(cpu, in, "SSE 搬移的操作数形态不支持");
        }

        // ---- movss：标量 + 高 96 位语义（见文件头）----
        case SseOp::Movss: {
            const Operand& dst = in.op0();
            const Operand& src = in.op1();
            if (dst.kind == OpKind::Xmm) {
                if (dst.vec >= 8) return unimplemented(cpu, in, "movss 的目标 xmm 下标越界");
                if (src.kind == OpKind::Xmm) {
                    if (src.vec >= 8) return unimplemented(cpu, in, "movss 的源 xmm 下标越界");
                    // legacy reg-reg：DEST[MAXVL-1:32] 不修改
                    cpu.xmm[dst.vec][0] = cpu.xmm[src.vec][0];
                    return true;
                }
                if (src.kind == OpKind::Mem) {
                    if (src.mem.size != 4) return unimplemented(cpu, in, "movss 的源必须是 m32");
                    cpu.xmm[dst.vec][0] = cpu.mem->rd32(cpu.eaOf(src.mem));
                    // legacy mem 源：DEST[127:32] 清 0
                    cpu.xmm[dst.vec][1] = 0;
                    cpu.xmm[dst.vec][2] = 0;
                    cpu.xmm[dst.vec][3] = 0;
                    return true;
                }
                return unimplemented(cpu, in, "movss 的源操作数形态不支持");
            }
            if (dst.kind == OpKind::Mem) {
                if (src.kind != OpKind::Xmm || src.vec >= 8) {
                    return unimplemented(cpu, in, "movss 存内存时源必须是 xmm");
                }
                if (dst.mem.size != 4) return unimplemented(cpu, in, "movss 的内存目标必须是 m32");
                cpu.mem->wr32(cpu.eaOf(dst.mem), cpu.xmm[src.vec][0]);  // 只写 4 字节
                return true;
            }
            return unimplemented(cpu, in, "movss 的目标操作数形态不支持");
        }

        // ---- movsd：SSE2 标量 double（legacy；与串操作 `movsd` 的碰撞见解码器的形态分派）----
        // ★ SDM：reg-reg ⇒ DEST[63:0] := SRC[63:0]、DEST[127:64] **Unmodified**；
        //         m64 源 ⇒ DEST[63:0] := SRC、**DEST[127:64] := 0**；
        //         m64 目标 ⇒ 只写 8 字节（邻近字节一个都不许动）。
        case SseOp::Movsd: {
            const Operand& dst = in.op0();
            const Operand& src = in.op1();
            if (dst.kind == OpKind::Xmm) {
                if (dst.vec >= 8) return unimplemented(cpu, in, "movsd 的目标 xmm 下标越界");
                if (src.kind == OpKind::Xmm) {
                    if (src.vec >= 8) return unimplemented(cpu, in, "movsd 的源 xmm 下标越界");
                    cpu.xmm[dst.vec][0] = cpu.xmm[src.vec][0];  // 低 64 位
                    cpu.xmm[dst.vec][1] = cpu.xmm[src.vec][1];
                    return true;  // 高 64 位原样保留
                }
                if (src.kind == OpKind::Mem) {
                    if (src.mem.size != 8) return unimplemented(cpu, in, "movsd 的源必须是 m64");
                    const std::uint32_t ea = cpu.eaOf(src.mem);
                    cpu.xmm[dst.vec][0] = cpu.mem->rd32(ea);
                    cpu.xmm[dst.vec][1] = cpu.mem->rd32(ea + 4u);
                    cpu.xmm[dst.vec][2] = 0;  // legacy mem 源：DEST[127:64] 清 0
                    cpu.xmm[dst.vec][3] = 0;
                    return true;
                }
                return unimplemented(cpu, in, "movsd 的源操作数形态不支持");
            }
            if (dst.kind == OpKind::Mem) {
                if (src.kind != OpKind::Xmm || src.vec >= 8) {
                    return unimplemented(cpu, in, "movsd 存内存时源必须是 xmm");
                }
                if (dst.mem.size != 8) return unimplemented(cpu, in, "movsd 的内存目标必须是 m64");
                const std::uint32_t ea = cpu.eaOf(dst.mem);
                cpu.mem->wr32(ea, cpu.xmm[src.vec][0]);       // 只写 8 字节
                cpu.mem->wr32(ea + 4u, cpu.xmm[src.vec][1]);
                return true;
            }
            return unimplemented(cpu, in, "movsd 的目标操作数形态不支持");
        }

        // ---- xorps：整份 128 位按位异或（不写 flag）----
        case SseOp::Xorps: {
            if (in.op0().kind != OpKind::Xmm || in.op0().vec >= 8) {
                return unimplemented(cpu, in, "xorps 的目标必须是 xmm");
            }
            std::uint8_t b[16] = {};
            if (!sseLoad128(cpu, in.op1(), b)) return unimplemented(cpu, in, "xorps 的源必须是 xmm / m128");
            std::uint8_t* d = cpu.xmmBytes(in.op0().vec);
            for (int i = 0; i < 16; ++i) d[i] = static_cast<std::uint8_t>(d[i] ^ b[i]);
            return true;
        }

        // ---- cvtsi2ss：有符号 32 位整数 → 标量单精度 ----
        case SseOp::Cvtsi2ss: {
            if (in.op0().kind != OpKind::Xmm || in.op0().vec >= 8) {
                return unimplemented(cpu, in, "cvtsi2ss 的目标必须是 xmm");
            }
            std::int32_t v = 0;
            if (in.op1().kind == OpKind::Reg) {
                v = static_cast<std::int32_t>(cpu.readReg(in.op1().reg));
            } else if (in.op1().kind == OpKind::Mem && in.op1().mem.size == 4) {
                v = static_cast<std::int32_t>(cpu.mem->rd32(cpu.eaOf(in.op1().mem)));
            } else {
                return unimplemented(cpu, in, "cvtsi2ss 的源必须是 r/m32");
            }
            // 用 float 而不是 double：舍入到 binary32（= 就近偶数，MXCSR.RC 的复位值）。
            // SDM：legacy 形态 DEST[MAXVL-1:32] **Unmodified** ⇒ 高 96 位原样保留。
            cpu.xmm[in.op0().vec][0] = sseBits32(static_cast<float>(v));
            return true;
        }

        // ---- cvttss2si：标量单精度 → 有符号 32 位整数（**朝零截断**）----
        case SseOp::Cvttss2si: {
            if (in.op0().kind != OpKind::Reg) return unimplemented(cpu, in, "cvttss2si 的目标必须是 r32");
            std::uint32_t bits = 0;
            if (!sseScalarSrc(cpu, in.op1(), &bits)) {
                return unimplemented(cpu, in, "cvttss2si 的源必须是 xmm / m32");
            }
            // SDM：NaN / ±∞ / 越界 ⇒ **invalid** + 整数不定值 0x80000000；否则朝零截断
            //   （⛔ 不是四舍五入 —— `cvtt` 的 t 就是 truncate）。
            std::uint32_t result = 0x80000000u;
            bool invalid = false;
            const float f = sseAsF32(bits);
            if (std::isnan(f) || std::isinf(f)) {
                invalid = true;
            } else {
                // float → double 是精确的；trunc 后是整数 ⇒ 区间判断足够
                const double t = std::trunc(static_cast<double>(f));
                // ±2^31 是边界：|x| ≥ 2^31 越界（-2^31 本身**在**范围内）
                if (t >= 2147483648.0 || t < -2147483648.0) {
                    invalid = true;
                } else {
                    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(t));
                }
            }
            if (invalid) sseRaiseInvalid(cpu);
            cpu.writeReg(in.op0().reg, result);
            return true;
        }

        // ---- addss / subss / mulss / divss：只改低 32 位 ----
        case SseOp::Addss:
        case SseOp::Subss:
        case SseOp::Mulss:
        case SseOp::Divss: {
            if (in.op0().kind != OpKind::Xmm || in.op0().vec >= 8) {
                return unimplemented(cpu, in, "SSE 标量算术的目标必须是 xmm");
            }
            std::uint32_t b = 0;
            if (!sseScalarSrc(cpu, in.op1(), &b)) {
                return unimplemented(cpu, in, "SSE 标量算术的源必须是 xmm / m32");
            }
            const float x = sseAsF32(cpu.xmm[in.op0().vec][0]);
            const float y = sseAsF32(b);
            float r = 0.0f;
            switch (in.sse) {
                case SseOp::Addss: r = x + y; break;
                case SseOp::Subss: r = x - y; break;
                case SseOp::Mulss: r = x * y; break;
                default: r = x / y; break;  // Divss（0/0 ⇒ NaN、x/0 ⇒ ±∞，宿主默认不 trap）
            }
            // ★ 宿主 IEEE binary32 与 SDM 的标量单精度在**值**上一致（NaN 传播 / 就近舍入都对得上）；
            //   但 MXCSR 的 DE/ZE/OE/UE/PE 与 invalid 的 quiet/signaling 区分**未建模**（没有消费者）。
            cpu.xmm[in.op0().vec][0] = sseBits32(r);
            return true;  // 高 96 位原样保留
        }

        // ---- comiss / ucomiss：EFLAGS（不是 x87 的 C0..C3）----
        case SseOp::Comiss:
        case SseOp::Ucomiss: {
            if (in.op0().kind != OpKind::Xmm || in.op0().vec >= 8) {
                return unimplemented(cpu, in, "comiss/ucomiss 的目标必须是 xmm");
            }
            std::uint32_t b = 0;
            if (!sseScalarSrc(cpu, in.op1(), &b)) {
                return unimplemented(cpu, in, "comiss/ucomiss 的源必须是 xmm / m32");
            }
            const std::uint32_t a = cpu.xmm[in.op0().vec][0];
            const bool a_nan = sseIsNaN(a);
            const bool b_nan = sseIsNaN(b);
            // SDM 的 Operation 伪码：UNORDERED ⇒ ZF,PF,CF := 111；GREATER ⇒ 000；LESS ⇒ 001；
            // EQUAL ⇒ 100；**OF, AF, SF := 0**。
            bool cf = false;
            bool pf = false;
            bool zf = false;
            if (a_nan || b_nan) {
                cf = true;
                pf = true;
                zf = true;
            } else {
                const float x = sseAsF32(a);
                const float y = sseAsF32(b);
                if (x < y) cf = true;
                else if (x == y) zf = true;
            }
            cpu.setCf(cf);
            cpu.setPf(pf);
            cpu.setZf(zf);
            cpu.setOf(false);
            cpu.setSf(false);
            cpu.setAf(false);
            // ★ QNaN 与 SNaN 的差别（SDM 的 Description）：**COMISS 对 QNaN 也置 #I**，
            //   UCOMISS 只对 SNaN 置 #I。默认 MXCSR 全屏蔽 ⇒ 只置 IE 位、不 trap、EFLAGS 照写。
            const bool snan = (a_nan && sseIsSnan(a)) || (b_nan && sseIsSnan(b));
            const bool qnan = (a_nan && !sseIsSnan(a)) || (b_nan && !sseIsSnan(b));
            if (snan || (qnan && in.sse == SseOp::Comiss)) sseRaiseInvalid(cpu);
            return true;
        }

        default:
            return unimplemented(cpu, in, "SSE 族没有这条语义（闭集合里没登记）");
    }
}

// =====================================================================================
// x87 执行入口
// =====================================================================================
bool execFpu(X86& cpu, const Insn& in) {
    switch (in.op) {
        case Op::Fld: {
            double v = 0.0;
            if (in.op0().kind == OpKind::Reg) {
                v = *stConst(cpu, stIndex(in.op0(), 0));
            } else if (in.op0().kind == OpKind::Mem) {
                if (!loadMemValue(cpu, in.op0().mem, &v)) return unimplemented(cpu, in, "fld 的宽度不支持");
            } else {
                return unimplemented(cpu, in, "fld 的操作数形态不支持");
            }
            push(cpu, v);
            syncStatus(cpu);
            return true;
        }
        case Op::Fild: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fild 只支持内存操作数");
            double v = 0.0;
            if (!loadIntMem(cpu, in.op0().mem, &v)) return unimplemented(cpu, in, "fild 的整数宽度不支持");
            push(cpu, v);
            syncStatus(cpu);
            return true;
        }
        case Op::Fst:
        case Op::Fstp: {
            const double v = *stConst(cpu, 0);
            if (in.op0().kind == OpKind::Reg) {
                const int i = stIndex(in.op0(), 0);
                if (i != 0) {  // `fst st(0)` 是空操作
                    *stSlot(cpu, i) = v;
                    markValid(cpu, cpu.fpu.phys(i));
                }
            } else if (in.op0().kind == OpKind::Mem) {
                storeMemValue(cpu, in.op0().mem, v);
            } else {
                return unimplemented(cpu, in, "fst 的操作数形态不支持");
            }
            if (in.op == Op::Fstp) pop(cpu);
            syncStatus(cpu);
            return true;
        }
        case Op::Fist:
        case Op::Fistp: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fist 只支持内存操作数");
            storeIntMem(cpu, in.op0().mem, *stConst(cpu, 0), x87RoundOf(cpu.fpu.control));
            if (in.op == Op::Fistp) pop(cpu);
            syncStatus(cpu);
            return true;
        }
        case Op::Fisttp: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fisttp 只支持内存操作数");
            storeIntMem(cpu, in.op0().mem, *stConst(cpu, 0), X87Round::Zero);  // 朝零截断（不看 RC）
            pop(cpu);
            syncStatus(cpu);
            return true;
        }
        case Op::Fadd:
        case Op::Fsub:
        case Op::Fsubr:
        case Op::Fmul:
        case Op::Fdiv:
        case Op::Fdivr:
            return arith(cpu, in, in.op);
        case Op::Faddp:
        case Op::Fsubp:
        case Op::Fsubrp:
        case Op::Fmulp:
        case Op::Fdivp:
        case Op::Fdivrp: {
            // `faddp st(i), st` ⇒ ST(i) = ST(i) op ST0，然后 pop
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 1;
            Insn copy = in;
            copy.ops[0].kind = OpKind::Reg;
            copy.ops[0].reg = static_cast<Reg>(static_cast<int>(Reg::ST0) + i);
            copy.ops[1].kind = OpKind::Reg;
            copy.ops[1].reg = Reg::ST0;
            Op base = Op::Fadd;
            switch (in.op) {
                case Op::Faddp: base = Op::Fadd; break;
                case Op::Fsubp: base = Op::Fsub; break;
                case Op::Fsubrp: base = Op::Fsubr; break;
                case Op::Fmulp: base = Op::Fmul; break;
                case Op::Fdivp: base = Op::Fdiv; break;
                default: base = Op::Fdivr; break;
            }
            const bool ok = arith(cpu, copy, base);
            if (ok) pop(cpu);
            return ok;
        }
        case Op::Fiadd:
        case Op::Fisub:
        case Op::Fisubr:
        case Op::Fimul:
        case Op::Fidiv:
        case Op::Fidivr:
            return integerArith(cpu, in, in.op);
        case Op::Fcom:
        case Op::Fucom:
            return compare(cpu, in, false, in.op == Op::Fucom);
        case Op::Fcomp:
        case Op::Fucomp:
            return compare(cpu, in, true, in.op == Op::Fucomp);
        case Op::Fcompp:
        case Op::Fucompp: {
            const bool ok = compare(cpu, in, false, in.op == Op::Fucompp);
            if (ok) {
                pop(cpu);
                pop(cpu);
            }
            return ok;
        }
        case Op::Fcomi: return compareEflags(cpu, in, false, false);
        case Op::Fcomip: return compareEflags(cpu, in, true, false);
        case Op::Fucomi: return compareEflags(cpu, in, false, true);
        case Op::Fucomip: return compareEflags(cpu, in, true, true);
        case Op::Fcmovb:
        case Op::Fcmove:
        case Op::Fcmovbe:
        case Op::Fcmovu:
        case Op::Fcmovnb:
        case Op::Fcmovne:
        case Op::Fcmovnbe:
        case Op::Fcmovnu:
            return fcmov(cpu, in);
        case Op::Fnstsw: {
            syncStatus(cpu);
            const std::uint16_t sw = cpu.fpu.status;
            if (in.op0().kind == OpKind::Reg) {
                cpu.writeReg(in.op0().reg, sw);
            } else if (in.op0().kind == OpKind::Mem) {
                cpu.mem->wr16(cpu.eaOf(in.op0().mem), sw);
            } else {
                // `fnstsw` 无操作数形式：写到 AX
                cpu.writeReg(Reg::AX, sw);
            }
            return true;
        }
        case Op::Fldcw: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fldcw 只支持内存操作数");
            cpu.fpu.control = cpu.mem->rd16(cpu.eaOf(in.op0().mem));
            // ★ D49：PC 位**不生效**，RC 只在 `fist`/`fistp`/`frndint` 处现取（⛔ 不 fesetround）
            return true;
        }
        case Op::Fnstcw: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fnstcw 只支持内存操作数");
            cpu.mem->wr16(cpu.eaOf(in.op0().mem), cpu.fpu.control);
            return true;
        }
        case Op::Fldenv: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fldenv 只支持内存操作数");
            doLoadEnv(cpu, cpu.eaOf(in.op0().mem));
            return true;
        }
        case Op::Fnstenv: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fnstenv 只支持内存操作数");
            syncStatus(cpu);
            doSaveEnv(cpu, cpu.eaOf(in.op0().mem));
            return true;
        }
        case Op::Fnsave: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fnsave 只支持内存操作数");
            syncStatus(cpu);
            doFnsave(cpu, cpu.eaOf(in.op0().mem));
            return true;
        }
        case Op::Frstor: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "frstor 只支持内存操作数");
            doFrstor(cpu, cpu.eaOf(in.op0().mem));
            return true;
        }
        case Op::Fxch: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 1;
            const int p0 = cpu.fpu.phys(0);
            const int p1 = cpu.fpu.phys(i);
            const double tmp = cpu.fpu.regs[p0];
            cpu.fpu.regs[p0] = cpu.fpu.regs[p1];
            cpu.fpu.regs[p1] = tmp;
            return true;
        }
        case Op::Ffree: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 0;
            cpu.fpu.setEmpty(cpu.fpu.phys(i));
            return true;
        }
        case Op::Fdecstp:
            cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.top + 7) & 7);
            syncStatus(cpu);
            return true;
        case Op::Fincstp:
            cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.top + 1) & 7);
            syncStatus(cpu);
            return true;
        case Op::Fnop:
            return true;
        case Op::Fld1: push(cpu, x87One()); syncStatus(cpu); return true;
        case Op::Fldz: push(cpu, x87Zero()); syncStatus(cpu); return true;
        case Op::Fldpi: push(cpu, x87Pi()); syncStatus(cpu); return true;
        case Op::Fldln2: push(cpu, x87Ln2()); syncStatus(cpu); return true;
        case Op::Fldlg2: push(cpu, x87Lg2()); syncStatus(cpu); return true;
        case Op::Fldl2e: push(cpu, x87L2e()); syncStatus(cpu); return true;
        case Op::Fldl2t: push(cpu, x87L2t()); syncStatus(cpu); return true;
        case Op::Fchs:
            *stSlot(cpu, 0) = x87ChangeSign(*stConst(cpu, 0));
            markValid(cpu, cpu.fpu.phys(0));
            return true;
        case Op::Fabs:
            *stSlot(cpu, 0) = x87Abs(*stConst(cpu, 0));
            markValid(cpu, cpu.fpu.phys(0));
            return true;
        case Op::Fninit:
            doFninit(cpu);
            return true;
        case Op::Fclex:
            x87TakeExceptionFlags();
            cpu.fpu.status = static_cast<std::uint16_t>(
                cpu.fpu.status & ~(SW_IE | SW_DE | SW_ZE | SW_OE | SW_UE | SW_PE | SW_SF | SW_ES));
            return true;
        case Op::Fwait:
            return true;
        case Op::Ftst: {
            const int cmp = x87Compare(*stConst(cpu, 0), 0.0, /*quiet=*/false);
            if (cmp == 2) setCC(cpu, true, false, true, true);
            else if (cmp == 0) setCC(cpu, false, false, false, true);
            else if (cmp < 0) setCC(cpu, true, false, false, false);
            else setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::Fsqrt: {
            storeSt0(cpu, x87Sqrt(*stConst(cpu, 0)));
            return true;
        }
        case Op::Frndint: {
            storeSt0(cpu, x87RoundToInt(*stConst(cpu, 0), x87RoundOf(cpu.fpu.control)));
            return true;
        }
        case Op::Fprem:
        case Op::Fprem1: {
            const double a = *stConst(cpu, 0);
            const double b = *stConst(cpu, 1);
            const double r = (in.op == Op::Fprem) ? x87RemTrunc(a, b) : x87Rem(a, b);
            syncStatus(cpu);  // 真运算的异常位先落账
            // 商的低 3 位 → C0(bit2) / C3(bit1) / C1(bit0)；C2=1 表示"没算完"
            bool c0 = false;
            bool c1 = false;
            bool c2 = false;
            bool c3 = false;
            const double qv = x87Div(x87Sub(a, r), b);
            if (std::fabs(qv) < 4611686018427387904.0) {  // 2^62
                const std::int64_t q = x87ToI64Trunc(qv);
                c0 = ((q >> 2) & 1) != 0;
                c3 = ((q >> 1) & 1) != 0;
                c1 = (q & 1) != 0;
            } else {
                c2 = true;
            }
            x87ClearExceptionFlags();  // 丢弃"算商位"产生的异常位（与旧实现同口径）
            *stSlot(cpu, 0) = r;
            markValid(cpu, cpu.fpu.phys(0));
            setCC(cpu, c0, c1, c2, c3);
            return true;
        }
        case Op::Fscale: {
            storeSt0(cpu, x87Scale(*stConst(cpu, 0), *stConst(cpu, 1)));
            return true;
        }
        case Op::Fxam:
            doFxam(cpu);
            syncStatus(cpu);
            return true;
        case Op::Fxtract: {
            double sig = 0.0;
            double exp = 0.0;
            x87Extract(*stConst(cpu, 0), &sig, &exp);
            *stSlot(cpu, 0) = sig;
            markValid(cpu, cpu.fpu.phys(0));
            push(cpu, exp);  // 指数压栈 ⇒ 最终 ST(0)=指数、ST(1)=尾数
            syncStatus(cpu);
            return true;
        }
        case Op::Fsin:
        case Op::Fcos:
        case Op::Fsincos:
        case Op::Fptan:
        case Op::Fpatan:
        case Op::Fyl2x:
        case Op::Fyl2xp1:
        case Op::F2xm1:
            return transcendental(cpu, in);
        default:
            return unimplemented(cpu, in, "x87 族没有这条语义（S4 的闭集合里没登记）");
    }
}


// =====================================================================================
// AOT 生成代码的 x87 helper 入口（检查点 2 / D49）
// =====================================================================================
// ★ `lift_x87_sem(cpu, op)`（`lift_abi.cpp`）转发到这里。它配一个**只带 op** 的 `Insn` 调
//   `execFpu` ⇒ 超越函数族 / fprem / fscale / fxtract / fxam 的语义与解释器**逐位同一份**
//   （⛔ 不在发射器里重写公式，也不另起一份 double 实现）。
// ★ 只接受**无内存操作数**的 op：`unsupportedReasonOf`（发射器）只把这一族路由过来；
//   其余 op 会走 `execFpu` 里需要操作数的分支（`op0().kind == None`）⇒ `unimplemented` 响亮失败。
bool execX87NoOperand(X86& cpu, Op op) {
    Insn in;
    in.op = op;
    in.mnemonic = opName(op);
    return execFpu(cpu, in);
}
}  // namespace lift
