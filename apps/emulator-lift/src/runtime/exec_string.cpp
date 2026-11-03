// emulator-lift · 串操作语义（movs / stos / lods / scas / cmps + rep）
//
// ★ 方向由 DF 决定（`std` / `cld` 已经是普通整数语义？— 不：`cld`/`std` 是独立助记符，
//   本作语料里没有，所以 DF 只由 `popfd`/`sahf` 改。这里按 DF 正负推进）。
// ★ 元素宽度取解码器的 `opsz`（`rep movsd` ⇒ 4）。`rep` = 0xF3，`repne` = 0xF2。
// ★ `rep movsd` / `rep stosd` 是 S3 点名要做的两条（语料里有 300 / 18 处）。
#include <cstdio>

#include "runtime/exec.h"
#include "runtime/x86.h"

namespace lift {
namespace {

std::uint32_t readElem(X86& cpu, std::uint32_t ea, std::uint8_t size) {
    switch (size) {
        case 1: return cpu.mem->rd8(ea);
        case 2: return cpu.mem->rd16(ea);
        default: return cpu.mem->rd32(ea);
    }
}

void writeElem(X86& cpu, std::uint32_t ea, std::uint8_t size, std::uint32_t v) {
    switch (size) {
        case 1: cpu.mem->wr8(ea, static_cast<std::uint8_t>(v & 0xFFu)); break;
        case 2: cpu.mem->wr16(ea, static_cast<std::uint16_t>(v & 0xFFFFu)); break;
        default: cpu.mem->wr32(ea, v); break;
    }
}

bool par(std::uint32_t v) {
    std::uint8_t x = static_cast<std::uint8_t>(v & 0xFFu);
    x ^= static_cast<std::uint8_t>(x >> 4);
    x &= 0x0Fu;
    return ((0x6996u >> x) & 1u) != 0;
}

void setCmpFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint8_t size) {
    setSubFlags(cpu, a, b, a - b, size, false);
    cpu.setPf(par(a - b));
}

}  // namespace

bool execString(X86& cpu, const Insn& in) {
    const std::uint8_t size = in.opsz ? in.opsz : 4;
    const std::uint32_t step = size;
    const bool rep = in.rep == 0xF3;
    const bool repne = in.rep == 0xF2;

    auto advance = [&](std::uint32_t& reg) {
        if (cpu.df()) {
            reg -= step;
        } else {
            reg += step;
        }
    };

    // 一次元素操作；返回是否需要"提前结束 rep"（scas / cmps 的 repe / repne）
    auto once = [&]() -> bool {
        switch (in.op) {
            case Op::Movs: {
                const std::uint32_t v = readElem(cpu, cpu.esi(), size);
                writeElem(cpu, cpu.edi(), size, v);
                advance(cpu.esi());
                advance(cpu.edi());
                return false;
            }
            case Op::Stos: {
                const std::uint32_t v = cpu.eax() & (size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                writeElem(cpu, cpu.edi(), size, v);
                advance(cpu.edi());
                return false;
            }
            case Op::Lods: {
                const std::uint32_t v = readElem(cpu, cpu.esi(), size);
                cpu.writeReg(size == 1 ? Reg::AL : (size == 2 ? Reg::AX : Reg::EAX), v);
                advance(cpu.esi());
                return false;
            }
            case Op::Scas: {
                const std::uint32_t a = cpu.eax() & (size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                const std::uint32_t b = readElem(cpu, cpu.edi(), size);
                setCmpFlags(cpu, a, b, size);
                advance(cpu.edi());
                return true;
            }
            case Op::Cmps: {
                const std::uint32_t a = readElem(cpu, cpu.esi(), size);
                const std::uint32_t b = readElem(cpu, cpu.edi(), size);
                setCmpFlags(cpu, a, b, size);
                advance(cpu.esi());
                advance(cpu.edi());
                return true;
            }
            default:
                return false;
        }
    };

    if (!rep && !repne) {
        once();
        return true;
    }
    if (cpu.ecx() == 0) return true;  // rep 且计数 0 ⇒ 一条都不执行
    while (cpu.ecx() != 0) {
        once();
        cpu.ecx() -= 1;
        if (cpu.ecx() == 0) break;
        const bool cmp_like = (in.op == Op::Scas || in.op == Op::Cmps);
        if (cmp_like) {
            if (rep && !cpu.zf()) break;      // repe：不相等就停
            if (repne && cpu.zf()) break;     // repne：相等就停
        }
    }
    return true;
}

}  // namespace lift
