// emulator-lift · x87 最小子集（M1a 只要"能跑通启动路径"，不做全量 47 个形态）
//
// ★ 范围（用户口径）：`fld` / `fst` / `fstp` / `fild` / `fist` / `fistp` / 四则（含 pop 形式）/
//   `fcom` 族 / `fnstsw` / `fxch` / 控制字读写 / 常量 / `fchs` `fabs` `fninit` `fwait` `ftst` `fsqrt`。
// ★ 算术全部转给 SoftFloat（D3）；这里只做**栈 / tag / 条件码 / 控制字**。
// ⛔ 超越函数（`fsin` `fcos` `fptan` `fpatan` `fyl2x` `f2xm1`）留给 M1b：执行到就**响亮失败**
//   （它们在本作语料里合计约 10 条，且启动路径不碰）。
#include <cstdio>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/fpu_bridge.h"
#include "runtime/x86.h"

namespace lift {
namespace {

std::uint8_t* stSlot(X86& cpu, int i) { return cpu.fpu.regs[cpu.fpu.phys(i)]; }
const std::uint8_t* stConst(const X86& cpu, int i) { return cpu.fpu.regs[cpu.fpu.phys(i)]; }

void markValid(X86& cpu, int physical) {
    cpu.fpu.setValid(physical);
    if (x87IsZero(cpu.fpu.regs[physical])) cpu.fpu.setZero(physical);
}

void push(X86& cpu, const Ext80 v) {
    const int next = (cpu.fpu.top - 1) & 7;
    if (!cpu.fpu.empty(next)) {
        // 栈上溢：x87 会置 C1 并产生 invalid
        cpu.fpu.status |= 0x0200u;
        return;
    }
    cpu.fpu.top = static_cast<std::uint8_t>(next);
    std::memcpy(cpu.fpu.regs[cpu.fpu.top], v, 16);
    markValid(cpu, cpu.fpu.top);
}

void pop(X86& cpu) {
    cpu.fpu.setEmpty(cpu.fpu.top);
    cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.top + 1) & 7);
}

void setCC(X86& cpu, bool c0, bool c1, bool c2, bool c3) {
    std::uint16_t s = cpu.fpu.status;
    s = static_cast<std::uint16_t>(s & ~0x4700u);  // 清 C0(8) C1(9) C2(10) C3(14)
    if (c0) s |= 0x0100u;
    if (c1) s |= 0x0200u;
    if (c2) s |= 0x0400u;
    if (c3) s |= 0x4000u;
    cpu.fpu.status = s;
}

void syncStatus(X86& cpu) {
    const std::uint8_t f = x87TakeExceptionFlags();
    std::uint16_t s = cpu.fpu.status;
    if (f & 1) s |= 0x0020u;   // PE 精度
    if (f & 2) s |= 0x0010u;   // UE 下溢
    if (f & 4) s |= 0x0008u;   // OE 上溢
    if (f & 8) s |= 0x0004u;   // ZE 除零
    if (f & 16) s |= 0x0001u;  // IE 非法
    s = static_cast<std::uint16_t>((s & ~0x3800u) | ((cpu.fpu.top & 7u) << 11));  // TOP
    cpu.fpu.status = s;
}

// 取内存操作数（按它的宽度）；返回 false ⇒ 不支持
bool loadMemValue(X86& cpu, const MemOperand& m, Ext80 out) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 4: {
            std::uint8_t b[4];
            for (int i = 0; i < 4; ++i) b[i] = cpu.mem->rd8(ea + i);
            x87MemToReg(b, out, 4);
            return true;
        }
        case 8: {
            std::uint8_t b[8];
            for (int i = 0; i < 8; ++i) b[i] = cpu.mem->rd8(ea + i);
            x87MemToReg(b, out, 8);
            return true;
        }
        case 10: {
            std::uint8_t b[10];
            for (int i = 0; i < 10; ++i) b[i] = cpu.mem->rd8(ea + i);
            x87MemToReg(b, out, 10);
            return true;
        }
        default: return false;
    }
}

bool loadIntMem(X86& cpu, const MemOperand& m, Ext80 out) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 2: x87FromI16(static_cast<std::int16_t>(cpu.mem->rd16(ea)), out); return true;
        case 4: x87FromI32(static_cast<std::int32_t>(cpu.mem->rd32(ea)), out); return true;
        case 8: x87FromI64(static_cast<std::int64_t>(cpu.mem->rd64(ea)), out); return true;
        default: return false;
    }
}

void storeMemValue(X86& cpu, const MemOperand& m, const Ext80 v) {
    const std::uint32_t ea = cpu.eaOf(m);
    std::uint8_t b[10] = {};
    const std::uint8_t size = m.size ? m.size : 4;
    x87RegToMem(v, b, size);
    cpu.mem->writeBytes(ea, b, size);
}

void storeIntMem(X86& cpu, const MemOperand& m, const Ext80 v) {
    const std::uint32_t ea = cpu.eaOf(m);
    switch (m.size) {
        case 2: cpu.mem->wr16(ea, static_cast<std::uint16_t>(x87ToI32(v, false))); break;
        case 4: cpu.mem->wr32(ea, static_cast<std::uint32_t>(x87ToI32(v, false))); break;
        case 8: cpu.mem->wr64(ea, static_cast<std::uint64_t>(x87ToI64(v, false))); break;
        default: break;
    }
}

// 二元算术：dst 与 src 的取法覆盖 `fadd st(i), st` / `fadd st, st(i)` / `fadd [mem]` 三种形态
bool arith(X86& cpu, const Insn& in, Op op) {
    Ext80 a;
    Ext80 b;
    int dst_index = 0;  // 结果写回哪个 ST（0 = ST0）
    if (in.op0().kind == OpKind::Mem) {
        if (!loadMemValue(cpu, in.op0().mem, b)) return unimplemented(cpu, in, "x87 算术的内存操作数宽度不支持");
        std::memcpy(a, stConst(cpu, 0), 16);
        dst_index = 0;
    } else {
        // 寄存器形态：op0 是目标 ST(i)，op1 是源（省略时源 = ST0）
        const int i0 = static_cast<int>(regSpec(in.op0().reg).index);
        dst_index = i0;
        std::memcpy(a, stConst(cpu, i0), 16);
        if (in.op1().kind == OpKind::Reg) {
            const int i1 = static_cast<int>(regSpec(in.op1().reg).index);
            std::memcpy(b, stConst(cpu, i1), 16);
        } else if (in.op1().kind == OpKind::Mem) {
            if (!loadMemValue(cpu, in.op1().mem, b)) return unimplemented(cpu, in, "x87 算术的内存操作数宽度不支持");
        } else {
            std::memcpy(b, stConst(cpu, 0), 16);
        }
    }
    x87ApplyControlWord(cpu.fpu.control);
    Ext80 r;
    switch (op) {
        case Op::Fadd: x87Add(a, b, r); break;
        case Op::Fsub: x87Sub(a, b, r); break;
        case Op::Fsubr: x87Sub(b, a, r); break;
        case Op::Fmul: x87Mul(a, b, r); break;
        case Op::Fdiv: x87Div(a, b, r); break;
        case Op::Fdivr: x87Div(b, a, r); break;
        default: return unimplemented(cpu, in, "x87 算术族没有这条语义");
    }
    std::memcpy(stSlot(cpu, dst_index), r, 16);
    markValid(cpu, cpu.fpu.phys(dst_index));
    syncStatus(cpu);
    return true;
}

bool integerArith(X86& cpu, const Insn& in, Op op) {
    if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fiadd 族只支持内存操作数");
    Ext80 b;
    if (!loadIntMem(cpu, in.op0().mem, b)) return unimplemented(cpu, in, "fiadd 族的整数宽度不支持");
    Ext80 a;
    std::memcpy(a, stConst(cpu, 0), 16);
    x87ApplyControlWord(cpu.fpu.control);
    Ext80 r;
    switch (op) {
        case Op::Fiadd: x87Add(a, b, r); break;
        case Op::Fisub: x87Sub(a, b, r); break;
        case Op::Fisubr: x87Sub(b, a, r); break;
        case Op::Fimul: x87Mul(a, b, r); break;
        case Op::Fidiv: x87Div(a, b, r); break;
        case Op::Fidivr: x87Div(b, a, r); break;
        default: return unimplemented(cpu, in, "fi 算术族没有这条语义");
    }
    std::memcpy(stSlot(cpu, 0), r, 16);
    markValid(cpu, cpu.fpu.phys(0));
    syncStatus(cpu);
    return true;
}

bool compare(X86& cpu, const Insn& in, bool pop_after, bool quiet) {
    (void)quiet;
    Ext80 b;
    if (in.op0().kind == OpKind::Reg) {
        const int i = static_cast<int>(regSpec(in.op0().reg).index);
        std::memcpy(b, stConst(cpu, i), 16);
    } else if (in.op0().kind == OpKind::Mem) {
        if (!loadMemValue(cpu, in.op0().mem, b)) return unimplemented(cpu, in, "fcom 的内存操作数宽度不支持");
    } else {
        // ★ **无显式操作数**的 `fcom` / `fcomp` / `fucom` / `fucomp` / `fcompp` / `fucompp`
        //   比的是 **ST(1)**，不是 ST(0)（Intel SDM：隐式源操作数是 ST(1)）。
        //   先前取 ST(0) ⇒ 变成"ST0 与自己比"，恒相等、C3 恒 0 —— 那会让依赖比较结果的分支走错，
        //   也是 conformance `x87-fcompp` / `x87-fucompp` 红的原因。
        std::memcpy(b, stConst(cpu, 1), 16);
    }
    x87ApplyControlWord(cpu.fpu.control);
    const int cmp = x87Compare(stConst(cpu, 0), b);
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

void doFninit(X86& cpu) {
    cpu.fpu.reset();
    x87ApplyControlWord(cpu.fpu.control);
    x87TakeExceptionFlags();
}

}  // namespace

bool execFpu(X86& cpu, const Insn& in) {
    switch (in.op) {
        case Op::Fld: {
            Ext80 v;
            if (in.op0().kind == OpKind::Reg) {
                const int i = static_cast<int>(regSpec(in.op0().reg).index);
                std::memcpy(v, stConst(cpu, i), 16);
            } else if (in.op0().kind == OpKind::Mem) {
                if (!loadMemValue(cpu, in.op0().mem, v)) return unimplemented(cpu, in, "fld 的宽度不支持");
            } else {
                return unimplemented(cpu, in, "fld 的操作数形态不支持");
            }
            push(cpu, v);
            syncStatus(cpu);
            return true;
        }
        case Op::Fild: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fild 只支持内存操作数");
            Ext80 v;
            if (!loadIntMem(cpu, in.op0().mem, v)) return unimplemented(cpu, in, "fild 的整数宽度不支持");
            push(cpu, v);
            syncStatus(cpu);
            return true;
        }
        case Op::Fst:
        case Op::Fstp: {
            const std::uint8_t* v = stConst(cpu, 0);
            if (in.op0().kind == OpKind::Reg) {
                const int i = static_cast<int>(regSpec(in.op0().reg).index);
                std::memcpy(stSlot(cpu, i), v, 16);
                markValid(cpu, cpu.fpu.phys(i));
            } else if (in.op0().kind == OpKind::Mem) {
                x87ApplyControlWord(cpu.fpu.control);
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
            x87ApplyControlWord(cpu.fpu.control);
            storeIntMem(cpu, in.op0().mem, stConst(cpu, 0));
            if (in.op == Op::Fistp) pop(cpu);
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
        case Op::Fnstsw: {
            x87ApplyControlWord(cpu.fpu.control);
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
            x87ApplyControlWord(cpu.fpu.control);
            return true;
        }
        case Op::Fnstcw: {
            if (in.op0().kind != OpKind::Mem) return unimplemented(cpu, in, "fnstcw 只支持内存操作数");
            cpu.mem->wr16(cpu.eaOf(in.op0().mem), cpu.fpu.control);
            return true;
        }
        case Op::Fxch: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 1;
            const int p0 = cpu.fpu.phys(0);
            const int p1 = cpu.fpu.phys(i);
            std::uint8_t tmp[16];
            std::memcpy(tmp, cpu.fpu.regs[p0], 16);
            std::memcpy(cpu.fpu.regs[p0], cpu.fpu.regs[p1], 16);
            std::memcpy(cpu.fpu.regs[p1], tmp, 16);
            return true;
        }
        case Op::Ffree: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 0;
            cpu.fpu.setEmpty(cpu.fpu.phys(i));
            return true;
        }
        case Op::Fld1: { Ext80 v; x87One(v); push(cpu, v); return true; }
        case Op::Fldz: { Ext80 v; x87Zero(v); push(cpu, v); return true; }
        case Op::Fldpi: { Ext80 v; x87Pi(v); push(cpu, v); return true; }
        case Op::Fldln2: { Ext80 v; x87Ln2(v); push(cpu, v); return true; }
        case Op::Fldlg2: { Ext80 v; x87Lg2(v); push(cpu, v); return true; }
        case Op::Fldl2e: { Ext80 v; x87L2e(v); push(cpu, v); return true; }
        case Op::Fldl2t: { Ext80 v; x87L2t(v); push(cpu, v); return true; }
        case Op::Fchs:
            x87ChangeSign(stSlot(cpu, 0));
            markValid(cpu, cpu.fpu.phys(0));
            return true;
        case Op::Fabs:
            x87Abs(stSlot(cpu, 0));
            markValid(cpu, cpu.fpu.phys(0));
            return true;
        case Op::Fninit:
            doFninit(cpu);
            return true;
        case Op::Fwait:
            return true;
        case Op::Ftst: {
            Ext80 zero;
            x87Zero(zero);
            const int cmp = x87Compare(stConst(cpu, 0), zero);
            if (cmp == 2) setCC(cpu, true, false, true, true);
            else if (cmp == 0) setCC(cpu, false, false, false, true);
            else if (cmp < 0) setCC(cpu, true, false, false, false);
            else setCC(cpu, false, false, false, false);
            return true;
        }
        case Op::Fsqrt: {
            x87ApplyControlWord(cpu.fpu.control);
            Ext80 r;
            x87Sqrt(stConst(cpu, 0), r);
            std::memcpy(stSlot(cpu, 0), r, 16);
            markValid(cpu, cpu.fpu.phys(0));
            syncStatus(cpu);
            return true;
        }
        case Op::Frndint: {
            x87ApplyControlWord(cpu.fpu.control);
            Ext80 r;
            x87RoundToInt(stConst(cpu, 0), r);
            std::memcpy(stSlot(cpu, 0), r, 16);
            markValid(cpu, cpu.fpu.phys(0));
            syncStatus(cpu);
            return true;
        }
        case Op::Fprem: {
            x87ApplyControlWord(cpu.fpu.control);
            Ext80 r;
            x87Rem(stConst(cpu, 0), stConst(cpu, 1), r);
            std::memcpy(stSlot(cpu, 0), r, 16);
            markValid(cpu, cpu.fpu.phys(0));
            // C2=1 表示没算完（需要再执行一次）；SoftFloat 的 rem 一次算完 ⇒ C2=0
            setCC(cpu, false, false, false, false);
            syncStatus(cpu);
            return true;
        }
        case Op::Fscale: {
            Ext80 r;
            x87Scale(stConst(cpu, 0), stConst(cpu, 1), r);
            std::memcpy(stSlot(cpu, 0), r, 16);
            markValid(cpu, cpu.fpu.phys(0));
            syncStatus(cpu);
            return true;
        }
        default:
            return unimplemented(cpu, in, "x87 超越函数留给 M1b（plan §4 D3 的边界）");
    }
}

}  // namespace lift
