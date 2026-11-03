// emulator-lift · 控制流语义（jcc / jmp / call / ret / setcc / cmovcc / int / hlt）
//
// ★ 约定（README §2.2）：控制流指令**只改 eip**，然后回 dispatcher 重新取块。
//   ⛔ 不在块内直接跳到另一块的代码里 —— 那样"块"就不再是执行单位了。
// ★ `ret` 弹返回地址；`ret N` 还要再抬 esp（stdcall 被调方弹参）。
#include <cstdio>

#include "runtime/exec.h"
#include "runtime/x86.h"

namespace lift {
namespace {

std::uint32_t branchTarget(const Insn& in) {
    // 相对跳转：Zydis 给的是相对当前指令末尾的位移
    if (in.op0().kind == OpKind::Imm) {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(in.ea) + in.length +
                                          static_cast<std::int64_t>(static_cast<std::int32_t>(in.op0().imm)));
    }
    return 0;
}

bool trapInt(X86& cpu, const Insn& in, const char* why) {
    std::fprintf(stderr, "\n=== guest interrupt (not modelled in M1) ===\n  %s\n  ea : %08X (%s)\n", why, in.ea,
                 in.mnemonic ? in.mnemonic : "?");
    dumpAtEa(cpu, in.ea, stderr);
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = why;
    return false;
}

}  // namespace

bool execControl(X86& cpu, const Insn& in) {
    switch (in.op) {
        case Op::Jmp: {
            cpu.eip = (in.op0().kind == OpKind::Imm) ? branchTarget(in) : cpu.readOp(in.op0());
            return true;
        }
        case Op::Jcc: {
            cpu.eip = in.ea + in.length;
            if (condHolds(in.cc, cpu.eflags)) cpu.eip = branchTarget(in);
            return true;
        }
        case Op::Call: {
            const std::uint32_t ret = in.ea + in.length;
            const std::uint32_t target = (in.op0().kind == OpKind::Imm) ? branchTarget(in) : cpu.readOp(in.op0());
            cpu.push32(ret);
            cpu.eip = target;
            return true;
        }
        case Op::Ret: {
            const std::uint32_t ret = cpu.pop32();
            if (in.op0().kind == OpKind::Imm) {
                cpu.esp() += static_cast<std::uint32_t>(in.op0().imm) & 0xFFFFu;
            }
            cpu.eip = ret;
            return true;
        }
        case Op::Hlt: {
            cpu.running = false;
            cpu.stop_why = "hlt";
            return false;
        }
        case Op::Int3: {
            return trapInt(cpu, in, "int 3（调试陷阱；M1 不做 SEH / 调试器，plan §9 R1）");
        }
        case Op::Int: {
            return trapInt(cpu, in, "软件中断（M1 不建模中断表）");
        }
        default:
            return unimplemented(cpu, in, "控制流族没有这条语义");
    }
}

}  // namespace lift
