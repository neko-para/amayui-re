// emulator-lift · 语义分发 + 标志位公共部分 + **响亮失败**的诊断
//
// ⛔ 这里是"未实现就红"的最后一道闸：`Op::Other` / 未实现的族一律走 `unimplemented()`，
//   打印 EA + 助记符 + 字节 + 寄存器 + guest 返回栈，然后**停止**（plan §6 S3）。
//   ⛔ 绝不允许 no-op —— 那会让"少写一条语义"变成静默的错误数据流。
#include "runtime/exec.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/machine.h"
#include "runtime/x86.h"

namespace lift {
namespace {

bool parityEven(std::uint8_t v) {
    v ^= static_cast<std::uint8_t>(v >> 4);
    v &= 0x0F;
    // 0x6996 的位 = **奇校验**（popcount 为奇 ⇒ 1）⇒ PF（偶校验）要取反。
    // 旧实现直接返回该位 ⇒ PF 全部反了（审计 T7 的 PF 夹具暴露）。
    return ((0x6996u >> v) & 1u) == 0;
}

}  // namespace

void setLogicFlags(X86& cpu, std::uint32_t result, std::uint8_t size) {
    std::uint32_t mask = 0xFFFFFFFFu;
    if (size == 1) mask = 0xFFu;
    if (size == 2) mask = 0xFFFFu;
    const std::uint32_t r = result & mask;
    cpu.setZf(r == 0);
    cpu.setSf((r & (mask ^ (mask >> 1))) != 0);
    cpu.setPf(parityEven(static_cast<std::uint8_t>(r & 0xFFu)));
    cpu.setCf(false);
    cpu.setOf(false);
    cpu.setAf(false);
}

void setAddFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint32_t result, std::uint8_t size,
                 bool with_carry) {
    std::uint64_t mask = 0xFFFFFFFFull;
    if (size == 1) mask = 0xFFull;
    if (size == 2) mask = 0xFFFFull;
    const std::uint64_t r = static_cast<std::uint64_t>(result) & mask;
    const std::uint64_t carry_in = with_carry && cpu.cf() ? 1ull : 0ull;
    const std::uint64_t full = (static_cast<std::uint64_t>(a) & mask) + (static_cast<std::uint64_t>(b) & mask) + carry_in;
    cpu.setCf(full > mask);
    const std::uint64_t sign = (mask + 1ull) >> 1;
    const bool sa = ((static_cast<std::uint64_t>(a) & mask) & sign) != 0;
    const bool sb = ((static_cast<std::uint64_t>(b) & mask) & sign) != 0;
    const bool sr = (r & sign) != 0;
    cpu.setOf(sa == sb && sr != sa);
    cpu.setAf((((static_cast<std::uint64_t>(a) & mask) ^ (static_cast<std::uint64_t>(b) & mask) ^ full) & 0x10ull) != 0);
    cpu.setZf(r == 0);
    cpu.setSf(sr);
    cpu.setPf(parityEven(static_cast<std::uint8_t>(r & 0xFFu)));
}

void setSubFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint32_t result, std::uint8_t size,
                 bool with_borrow) {
    std::uint64_t mask = 0xFFFFFFFFull;
    if (size == 1) mask = 0xFFull;
    if (size == 2) mask = 0xFFFFull;
    const std::uint64_t r = static_cast<std::uint64_t>(result) & mask;
    const std::uint64_t borrow_in = with_borrow && cpu.cf() ? 1ull : 0ull;
    const std::uint64_t av = static_cast<std::uint64_t>(a) & mask;
    const std::uint64_t bv = static_cast<std::uint64_t>(b) & mask;
    cpu.setCf(av < bv + borrow_in);
    const std::uint64_t sign = (mask + 1ull) >> 1;
    const bool sa = (av & sign) != 0;
    const bool sb = (bv & sign) != 0;
    const bool sr = (r & sign) != 0;
    cpu.setOf(sa != sb && sr != sa);
    cpu.setAf(((av ^ bv ^ r) & 0x10ull) != 0);
    cpu.setZf(r == 0);
    cpu.setSf(sr);
    cpu.setPf(parityEven(static_cast<std::uint8_t>(r & 0xFFu)));
}

bool condHolds(Cond cc, std::uint32_t eflags) {
    const bool cf = (eflags & FLAG_CF) != 0;
    const bool pf = (eflags & FLAG_PF) != 0;
    const bool zf = (eflags & FLAG_ZF) != 0;
    const bool sf = (eflags & FLAG_SF) != 0;
    const bool of = (eflags & FLAG_OF) != 0;
    switch (cc) {
        case Cond::O: return of;
        case Cond::NO: return !of;
        case Cond::B: return cf;
        case Cond::AE: return !cf;
        case Cond::E: return zf;
        case Cond::NE: return !zf;
        case Cond::BE: return cf || zf;
        case Cond::A: return !cf && !zf;
        case Cond::S: return sf;
        case Cond::NS: return !sf;
        case Cond::P: return pf;
        case Cond::NP: return !pf;
        case Cond::L: return sf != of;
        case Cond::GE: return sf == of;
        case Cond::LE: return zf || (sf != of);
        case Cond::G: return !zf && (sf == of);
        case Cond::None:
        default: return false;
    }
}

bool opIsControl(Op op) {
    switch (op) {
        case Op::Jmp:
        case Op::Jcc:
        case Op::Call:
        case Op::Ret:
        case Op::Hlt:
        case Op::Int3:
        case Op::Int:
            return true;
        default:
            return false;
    }
}

bool opIsX87(Op op) { return op >= Op::Fld && op <= Op::F2xm1; }

bool opImplemented(Op op) {
    switch (op) {
        case Op::Invalid:
        case Op::Other:
            return false;
        default:
            return true;
    }
}

const char* opName(Op op) {
    switch (op) {
#define LIFT_OP_CASE(x) case Op::x: return #x;
        LIFT_OP_CASE(Invalid)
        LIFT_OP_CASE(Mov) LIFT_OP_CASE(Movzx) LIFT_OP_CASE(Movsx) LIFT_OP_CASE(Lea) LIFT_OP_CASE(Xchg)
        LIFT_OP_CASE(Push) LIFT_OP_CASE(Pop) LIFT_OP_CASE(Pushad) LIFT_OP_CASE(Popad) LIFT_OP_CASE(Pushfd)
        LIFT_OP_CASE(Popfd) LIFT_OP_CASE(Xadd) LIFT_OP_CASE(Cmpxchg)
        LIFT_OP_CASE(Add) LIFT_OP_CASE(Or) LIFT_OP_CASE(And) LIFT_OP_CASE(Sub) LIFT_OP_CASE(Xor)
        LIFT_OP_CASE(Cmp) LIFT_OP_CASE(Test) LIFT_OP_CASE(Inc) LIFT_OP_CASE(Dec) LIFT_OP_CASE(Neg)
        LIFT_OP_CASE(Not) LIFT_OP_CASE(Adc) LIFT_OP_CASE(Sbb)
        LIFT_OP_CASE(Shl) LIFT_OP_CASE(Shr) LIFT_OP_CASE(Sar) LIFT_OP_CASE(Rol) LIFT_OP_CASE(Ror)
        LIFT_OP_CASE(Rcl) LIFT_OP_CASE(Rcr) LIFT_OP_CASE(Shld) LIFT_OP_CASE(Shrd)
        LIFT_OP_CASE(Mul) LIFT_OP_CASE(Imul) LIFT_OP_CASE(Div) LIFT_OP_CASE(Idiv) LIFT_OP_CASE(Cdq)
        LIFT_OP_CASE(Cwd) LIFT_OP_CASE(Cwde) LIFT_OP_CASE(Cbw) LIFT_OP_CASE(Bswap)
        LIFT_OP_CASE(Bt) LIFT_OP_CASE(Bts) LIFT_OP_CASE(Btr) LIFT_OP_CASE(Btc) LIFT_OP_CASE(Bsf)
        LIFT_OP_CASE(Bsr)
        LIFT_OP_CASE(Jmp) LIFT_OP_CASE(Jcc) LIFT_OP_CASE(Call) LIFT_OP_CASE(Ret) LIFT_OP_CASE(Leave)
        LIFT_OP_CASE(Nop) LIFT_OP_CASE(Hlt) LIFT_OP_CASE(Int3) LIFT_OP_CASE(Int) LIFT_OP_CASE(Setcc)
        LIFT_OP_CASE(Cmovcc) LIFT_OP_CASE(Cpuid) LIFT_OP_CASE(Sahf) LIFT_OP_CASE(Lahf) LIFT_OP_CASE(Pause)
        LIFT_OP_CASE(Movs) LIFT_OP_CASE(Stos) LIFT_OP_CASE(Lods) LIFT_OP_CASE(Scas) LIFT_OP_CASE(Cmps)
        LIFT_OP_CASE(Fld) LIFT_OP_CASE(Fst) LIFT_OP_CASE(Fstp) LIFT_OP_CASE(Fild) LIFT_OP_CASE(Fist)
        LIFT_OP_CASE(Fistp) LIFT_OP_CASE(Fisttp) LIFT_OP_CASE(Fadd) LIFT_OP_CASE(Fsub) LIFT_OP_CASE(Fsubr)
        LIFT_OP_CASE(Fmul) LIFT_OP_CASE(Fdiv) LIFT_OP_CASE(Fdivr) LIFT_OP_CASE(Faddp) LIFT_OP_CASE(Fsubp)
        LIFT_OP_CASE(Fsubrp) LIFT_OP_CASE(Fmulp) LIFT_OP_CASE(Fdivp) LIFT_OP_CASE(Fdivrp) LIFT_OP_CASE(Fiadd)
        LIFT_OP_CASE(Fisub) LIFT_OP_CASE(Fisubr) LIFT_OP_CASE(Fimul) LIFT_OP_CASE(Fidiv) LIFT_OP_CASE(Fidivr)
        LIFT_OP_CASE(Fcom) LIFT_OP_CASE(Fcomp) LIFT_OP_CASE(Fcompp) LIFT_OP_CASE(Fucom) LIFT_OP_CASE(Fucomp)
        LIFT_OP_CASE(Fucompp) LIFT_OP_CASE(Fcomi) LIFT_OP_CASE(Fcomip) LIFT_OP_CASE(Fucomi) LIFT_OP_CASE(Fucomip)
        LIFT_OP_CASE(Fcmovb) LIFT_OP_CASE(Fcmove) LIFT_OP_CASE(Fcmovbe) LIFT_OP_CASE(Fcmovu)
        LIFT_OP_CASE(Fcmovnb) LIFT_OP_CASE(Fcmovne) LIFT_OP_CASE(Fcmovnbe) LIFT_OP_CASE(Fcmovnu)
        LIFT_OP_CASE(Fnstsw) LIFT_OP_CASE(Fldcw) LIFT_OP_CASE(Fnstcw) LIFT_OP_CASE(Fldenv)
        LIFT_OP_CASE(Fnstenv) LIFT_OP_CASE(Fsave) LIFT_OP_CASE(Fnsave) LIFT_OP_CASE(Frstor)
        LIFT_OP_CASE(Fxch) LIFT_OP_CASE(Ffree) LIFT_OP_CASE(Fdecstp) LIFT_OP_CASE(Fincstp) LIFT_OP_CASE(Fnop)
        LIFT_OP_CASE(Fld1) LIFT_OP_CASE(Fldz) LIFT_OP_CASE(Fldpi) LIFT_OP_CASE(Fldl2e) LIFT_OP_CASE(Fldl2t)
        LIFT_OP_CASE(Fldlg2) LIFT_OP_CASE(Fldln2) LIFT_OP_CASE(Fchs) LIFT_OP_CASE(Fabs) LIFT_OP_CASE(Fninit)
        LIFT_OP_CASE(Fclex) LIFT_OP_CASE(Fwait) LIFT_OP_CASE(Ftst) LIFT_OP_CASE(Fsqrt) LIFT_OP_CASE(Frndint)
        LIFT_OP_CASE(Fprem) LIFT_OP_CASE(Fprem1) LIFT_OP_CASE(Fscale) LIFT_OP_CASE(Fxam) LIFT_OP_CASE(Fxtract)
        LIFT_OP_CASE(Fsin) LIFT_OP_CASE(Fcos) LIFT_OP_CASE(Fsincos) LIFT_OP_CASE(Fptan) LIFT_OP_CASE(Fpatan)
        LIFT_OP_CASE(Fyl2x) LIFT_OP_CASE(Fyl2xp1) LIFT_OP_CASE(F2xm1)
        LIFT_OP_CASE(Packed)
        LIFT_OP_CASE(Sse)
        LIFT_OP_CASE(Other)
#undef LIFT_OP_CASE
        default: return "?";
    }
}

const char* condName(Cond cc) {
    switch (cc) {
        case Cond::None: return "?";
        case Cond::O: return "o";
        case Cond::NO: return "no";
        case Cond::B: return "b";
        case Cond::AE: return "ae";
        case Cond::E: return "e";
        case Cond::NE: return "ne";
        case Cond::BE: return "be";
        case Cond::A: return "a";
        case Cond::S: return "s";
        case Cond::NS: return "ns";
        case Cond::P: return "p";
        case Cond::NP: return "np";
        case Cond::L: return "l";
        case Cond::GE: return "ge";
        case Cond::LE: return "le";
        case Cond::G: return "g";
    }
    return "?";
}

bool execInsn(X86& cpu, const Insn& in) {
    if (in.op == Op::Invalid) return unimplemented(cpu, in, "解码得到 Invalid");
    if (in.op == Op::Other) return unimplemented(cpu, in, "助记符认识但 M1a 没有实现这条语义");
    if (opIsControl(in.op)) return execControl(cpu, in);
    switch (in.op) {
        case Op::Movs:
        case Op::Stos:
        case Op::Lods:
        case Op::Scas:
        case Op::Cmps:
            return execString(cpu, in);
        default:
            break;
    }
    if (opIsX87(in.op)) return execFpu(cpu, in);
    if (in.op == Op::Packed) return execPacked(cpu, in);
    // ★ SSE 浮点 / 128 位搬移是**独立一族**（`Op::Sse`），⛔ 不塞进 `Op::Packed` 的整数 lane 语义。
    if (in.op == Op::Sse) return execSse(cpu, in);
    return execInteger(cpu, in);
}

void dumpRegisters(const X86& cpu, std::FILE* out) {
    std::fprintf(out, "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n", cpu.r[kEax], cpu.r[kEcx], cpu.r[kEdx], cpu.r[kEbx]);
    std::fprintf(out, "  esp=%08X ebp=%08X esi=%08X edi=%08X\n", cpu.r[kEsp], cpu.r[kEbp], cpu.r[kEsi], cpu.r[kEdi]);
    std::fprintf(out, "  eip=%08X eflags=%08X [%c%c%c%c%c%c%c]\n", cpu.eip, cpu.eflags, cpu.of() ? 'O' : '-',
                 cpu.df() ? 'D' : '-', cpu.sf() ? 'S' : '-', cpu.zf() ? 'Z' : '-', cpu.af() ? 'A' : '-',
                 cpu.pf() ? 'P' : '-', cpu.cf() ? 'C' : '-');
    std::fprintf(out, "  x87: top=%u control=%04X status=%04X tag=%02X\n", cpu.fpu.top, cpu.fpu.control,
                 cpu.fpu.status, cpu.fpu.tag);
}

void dumpGuestStack(const X86& cpu, std::FILE* out, int depth) {
    if (!cpu.mem) return;
    const std::uint32_t base = cpu.r[kEsp];
    std::fprintf(out, "  guest stack (esp=%08X, %d words):\n", base, depth);
    for (int i = 0; i < depth; i += 4) {
        std::fprintf(out, "    %08X:", base + static_cast<std::uint32_t>(i) * 4);
        for (int j = 0; j < 4 && i + j < depth; ++j) {
            const std::uint32_t ea = base + static_cast<std::uint32_t>(i + j) * 4;
            if (cpu.mem->committed(ea, 4)) {
                std::fprintf(out, " %08X", cpu.mem->rd32(ea));
            } else {
                std::fprintf(out, " ????????");
            }
        }
        std::fprintf(out, "\n");
    }
    // ebp 链（只在看起来像 guest 栈时走；否则不编造）
    std::uint32_t frame = cpu.r[kEbp];
    std::fprintf(out, "  ebp chain:\n");
    for (int i = 0; i < 8; ++i) {
        if (!cpu.mem->committed(frame, 8)) break;
        const std::uint32_t prev = cpu.mem->rd32(frame);
        const std::uint32_t ret = cpu.mem->rd32(frame + 4);
        std::fprintf(out, "    [%08X] prev=%08X ret=%08X\n", frame, prev, ret);
        if (prev <= frame) break;
        frame = prev;
    }
}

namespace {

// 这个地址在 guest 地址空间的**哪个区域**里？（诊断唯一目的：把"数据地址被当 eip"与
// "真代码里的语义缺口"分开。⛔ 不改 dispatch 语义，判据仍是 execInsn 的返回值。）
void dumpRegionOf(const X86& cpu, std::uint32_t ea, std::FILE* out) {
    if (!cpu.mem) return;
    const Region* r = cpu.mem->regionOf(ea);
    if (!r) {
        std::fprintf(out, "  region : (未登记：%08X 不属于任何已声明的区域)  committed=%s\n", ea,
                     cpu.mem->committed(ea, 1) ? "yes" : "no");
        return;
    }
    std::fprintf(out, "  region : %s  [%08X,%08X)  name=\"%s\"\n", regionKindName(r->kind), r->begin, r->end,
                 r->name);
}

// 区域种类 ⇒ 是不是**数据区**（把这里的字节当 eip/指令 = 控制流已经跑飞，不是缺 import）。
bool isDataRegion(RegionKind kind) {
    switch (kind) {
        case RegionKind::Heap:
        case RegionKind::Stack:
        case RegionKind::StackGuard:
        case RegionKind::Teb:
        case RegionKind::Peb:
            return true;
        default:
            return false;
    }
}

// 最近 24 条（`X86::noteInsn` 的环形缓冲 —— 每条指令都维护，零额外成本）。
// ★ 前 8 条给带操作数的反汇编：只打助记符看不出"那条 call 的目标是谁 / 立即数是多少"。
void dumpRecentInsns(const X86& cpu, std::FILE* out) {
    // ★ feeder 线程的 CPU **不维护** ring：feeder 的块执行路径刻意不调 `noteInsn`
    //   （Ogg 解码热路径；见 `audio.cpp` 的 `runFeederGuest`）⇒ 它里面的是**启动时从主 CPU 拷来的
    //   快照**。⛔ 不要把这段当 feeder 的现场 —— feeder 侧的现场看 `[esp]` / `region` / guest stack。
    //   判据用公开只读面 `AudioEngine::onFeederThread()`（本线程是不是 feeder），⛔ 不加新状态。
    if (cpu.audio != nullptr && cpu.audio->onFeederThread()) {
        std::fprintf(out,
                     "  （feeder 线程的 CPU：feeder 不维护 noteInsn ring ⇒ 下面是启动时的快照，"
                     "不是 feeder 现场）\n");
    }
    if (cpu.last_count <= 0) return;
    std::fprintf(out, "  --- 最近执行的 %d 条指令（最近的在最上）---\n", cpu.last_count);
    for (int i = 0; i < cpu.last_count; ++i) {
        const int idx = (cpu.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
        std::fprintf(out, "  %08X %s   (eflags=%08X)\n", cpu.last_eip[idx],
                     cpu.last_mnemonic[idx] ? cpu.last_mnemonic[idx] : "?", cpu.last_flags[idx]);
    }
    Machine* m = activeMachine();
    if (!m) return;
    std::fprintf(out, "  --- 反汇编（含操作数，最近 %d 条）---\n", cpu.last_count < 8 ? cpu.last_count : 8);
    for (int i = 0; i < 8 && i < cpu.last_count; ++i) {
        const int idx = (cpu.last_head - 1 - i + 2 * X86::kLastInsns) % X86::kLastInsns;
        std::fprintf(out, "  %08X %s\n", cpu.last_eip[idx], m->decoder().text(*cpu.mem, cpu.last_eip[idx]).c_str());
    }
}

}  // namespace（三个诊断 helper 只在本文件用；⛔ 不给它们外部链接）

void dumpAtEa(X86& cpu, std::uint32_t ea, std::FILE* out) {
    std::string text;
    if (cpu.lst) text = cpu.lst->textAt(ea);
    if (text.empty() && cpu.cache && cpu.mem) {
        // `.lst` 里没有这一行（例如在 CRT 的 COLLAPSED 区里）⇒ 现场反汇编
        if (Machine* m = activeMachine()) text = m->decoder().text(*cpu.mem, ea);
    }
    std::fprintf(out, "  at %08X : %s\n", ea, text.empty() ? "(没有文本：既不在 .lst 里，也解不出来)" : text.c_str());
}

bool unimplemented(X86& cpu, const Insn& in, const char* why) {
    std::fprintf(stderr, "\n=== unimplemented instruction (loud failure) ===\n");
    std::fprintf(stderr, "  why    : %s\n", why ? why : "(unspecified)");
    std::fprintf(stderr, "  ea     : %08X  mnemonic=%s  op=%s  len=%u\n", in.ea,
                 in.mnemonic ? in.mnemonic : "?", opName(in.op), static_cast<unsigned>(in.length));
    dumpRegionOf(cpu, in.ea, stderr);
    bool data_eip = false;
    if (cpu.mem) {
        const Region* r = cpu.mem->regionOf(in.ea);
        data_eip = r && isDataRegion(r->kind);
    }
    if (cpu.mem && cpu.mem->committed(in.ea, in.length)) {
        std::fprintf(stderr, "  bytes  :");
        for (std::uint8_t i = 0; i < in.length; ++i) std::fprintf(stderr, " %02X", cpu.mem->rd8(in.ea + i));
        std::fprintf(stderr, "\n");
    }
    dumpAtEa(cpu, in.ea, stderr);
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    // ★ 这一行是**诊断定性**，不是结论：eip 落在数据区 ⇒ 控制流已经被数据带跑，缺的**不是** import
    //   （那一类会停在桩页 `0x7FFE…` 并打 `=== unimplemented import ===`）。调用点 = [esp]。
    if (data_eip) {
        std::fprintf(stderr,
                     "\n  ⚠⚠ 数据地址当 eip：%08X 落在数据区（不是代码）⇒ 这是控制流被数据带跑，"
                     "**不是缺 import**。\n     调用点 = 上面 guest stack 的 [esp]（那条 call 的返回地址）；"
                     "上一条控制流指令见上面的 ring。\n",
                     in.ea);
    }
    dumpRecentInsns(cpu, stderr);
    cpu.running = false;
    cpu.stop_why = why;
    return false;
}

}  // namespace lift
