// emulator-lift · 整数 / 逻辑 / 移位 / 乘除 / 位 语义（S3 的"前 40 助记符"主体）
//
// ★ 所有写回都走 `writeOp` / `writeMem`（8 位高半与 16 位的合并规则在 x86.cpp 里只写一次）。
// ★ 标志位：CF/OF/AF/PF/ZF/SF 全部显式算，⛔ 不依赖宿主 flags（那在另一台机器上就变了）。
// ★ 除零 / 商溢出是 **CPU 异常**：这里打印诊断并停止（M1 不做 SEH 展开，plan §9 R1）。
#include <cstdio>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/x86.h"

namespace lift {
namespace {

inline std::uint32_t maskOf(std::uint8_t size) {
    return size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}
inline unsigned bitsOf(std::uint8_t size) { return size == 1 ? 8u : (size == 2 ? 16u : 32u); }
inline bool msbOf(std::uint32_t v, std::uint8_t size) { return (v & (1u << (bitsOf(size) - 1))) != 0; }

inline std::uint8_t opSize(const Operand& op, std::uint8_t fallback = 4) {
    return op.size ? op.size : fallback;
}

inline bool parityFromLow(std::uint32_t v) {
    std::uint8_t x = static_cast<std::uint8_t>(v & 0xFFu);
    x ^= static_cast<std::uint8_t>(x >> 4);
    x &= 0x0Fu;
    return ((0x6996u >> x) & 1u) != 0;
}

bool cpuFault(X86& cpu, const Insn& in, const char* why) {
    std::fprintf(stderr, "\n=== guest CPU exception ===\n  why : %s\n  ea  : %08X (%s)\n", why, in.ea,
                 in.mnemonic ? in.mnemonic : "?");
    dumpAtEa(cpu, in.ea, stderr);
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = why;
    return false;
}

// 读写一个操作数，按它的宽度截断
std::uint32_t rd(X86& cpu, const Operand& op) { return cpu.readOp(op) & maskOf(op.size ? op.size : 4); }

void wr(X86& cpu, const Operand& op, std::uint32_t v) { cpu.writeOp(op, v & maskOf(op.size ? op.size : 4)); }

std::int64_t signOf(std::uint32_t v, std::uint8_t size) {
    switch (size) {
        case 1: return static_cast<std::int8_t>(v & 0xFFu);
        case 2: return static_cast<std::int16_t>(v & 0xFFFFu);
        default: return static_cast<std::int32_t>(v);
    }
}

std::uint8_t shiftCount(X86& cpu, const Insn& in) {
    std::uint32_t c = 1;
    if (in.op1().kind == OpKind::Reg) {
        c = cpu.readReg(in.op1().reg) & 0xFFu;
    } else if (in.op1().kind == OpKind::Imm) {
        c = static_cast<std::uint32_t>(in.op1().imm);
    }
    return static_cast<std::uint8_t>(c & 0x1Fu);
}

bool doShift(X86& cpu, const Insn& in) {
    const std::uint8_t size = opSize(in.op0());
    const std::uint32_t mask = maskOf(size);
    const unsigned bits = bitsOf(size);
    const std::uint32_t a = rd(cpu, in.op0());
    const std::uint8_t c = shiftCount(cpu, in);
    if (c == 0) return true;  // 计数 0：什么都不改（x86 语义）

    std::uint32_t r = a;
    bool cf = cpu.cf();
    bool of = cpu.of();
    switch (in.op) {
        case Op::Shl:
            if (c >= bits) {
                r = 0;
                cf = (c == bits) ? ((a & 1u) != 0) : false;
            } else {
                cf = ((a >> (bits - c)) & 1u) != 0;
                r = (a << c) & mask;
            }
            of = msbOf(r, size) != cf;
            break;
        case Op::Shr:
            if (c >= bits) {
                r = 0;
                cf = (c == bits) ? msbOf(a, size) : false;
            } else {
                cf = ((a >> (c - 1)) & 1u) != 0;
                r = (a >> c) & mask;
            }
            of = c == 1 ? msbOf(a, size) : of;
            break;
        case Op::Sar: {
            const std::int32_t sa = static_cast<std::int32_t>(signOf(a, size));
            const int sh = (c > bits) ? static_cast<int>(bits) : static_cast<int>(c);
            cf = ((sa >> (sh - 1)) & 1) != 0;
            r = static_cast<std::uint32_t>(sa >> sh) & mask;
            of = false;
            break;
        }
        case Op::Rol: {
            const std::uint8_t k = static_cast<std::uint8_t>(c % bits);
            r = ((a << k) | (a >> ((bits - k) % bits))) & mask;
            cf = (r & 1u) != 0;
            of = msbOf(r, size) != cf;
            break;
        }
        case Op::Ror: {
            const std::uint8_t k = static_cast<std::uint8_t>(c % bits);
            r = ((a >> k) | (a << ((bits - k) % bits))) & mask;
            cf = msbOf(r, size);
            of = (c == 1) ? (msbOf(a, size) != msbOf(r, size)) : of;
            break;
        }
        case Op::Rcl: {
            const std::uint8_t k = static_cast<std::uint8_t>(c % (bits + 1));
            std::uint32_t v = a;
            for (std::uint8_t i = 0; i < k; ++i) {
                const bool top = msbOf(v, size);
                v = ((v << 1) | (cf ? 1u : 0u)) & mask;
                cf = top;
            }
            r = v;
            of = msbOf(r, size) != cf;
            break;
        }
        case Op::Rcr: {
            const std::uint8_t k = static_cast<std::uint8_t>(c % (bits + 1));
            std::uint32_t v = a;
            for (std::uint8_t i = 0; i < k; ++i) {
                const bool low = (v & 1u) != 0;
                v = (v >> 1) | (cf ? (1u << (bits - 1)) : 0u);
                cf = low;
            }
            r = v & mask;
            of = (c == 1) ? (msbOf(a, size) != msbOf(r, size)) : of;
            break;
        }
        default:
            return true;
    }
    cpu.setCf(cf);
    cpu.setOf(of);
    if (in.op == Op::Shl || in.op == Op::Shr || in.op == Op::Sar) {
        cpu.setZf((r & mask) == 0);
        cpu.setSf(msbOf(r, size));
        cpu.setPf(parityFromLow(r));
    }
    wr(cpu, in.op0(), r);
    return true;
}

bool doShxd(X86& cpu, const Insn& in) {
    const std::uint8_t size = opSize(in.op0());
    const std::uint32_t mask = maskOf(size);
    const unsigned bits = bitsOf(size);
    const std::uint8_t c = shiftCount(cpu, in);
    const std::uint32_t dst = rd(cpu, in.op0());
    const std::uint32_t src = rd(cpu, in.op1());
    if (c == 0 || c >= bits) return true;
    std::uint32_t r = dst;
    bool cf = cpu.cf();
    if (in.op == Op::Shld) {
        const std::uint64_t wide = (static_cast<std::uint64_t>(dst) << bits) | src;
        r = static_cast<std::uint32_t>((wide << c) >> bits) & mask;
        cf = ((dst >> (bits - c)) & 1u) != 0;
    } else {
        const std::uint64_t wide = (static_cast<std::uint64_t>(src) << bits) | dst;
        r = static_cast<std::uint32_t>(wide >> c) & mask;
        cf = ((dst >> (c - 1)) & 1u) != 0;
    }
    cpu.setCf(cf);
    cpu.setOf(msbOf(dst, size) != msbOf(r, size));
    cpu.setZf(r == 0);
    cpu.setSf(msbOf(r, size));
    cpu.setPf(parityFromLow(r));
    wr(cpu, in.op0(), r);
    return true;
}

bool doMul(X86& cpu, const Insn& in) {
    const std::uint8_t size = opSize(in.op0());
    const std::uint32_t src = rd(cpu, in.op0());
    if (size == 1) {
        const std::uint32_t r = (cpu.eax() & 0xFFu) * (src & 0xFFu);
        cpu.writeReg(Reg::AX, r & 0xFFFFu);
        const bool hi = (r & 0xFF00u) != 0;
        cpu.setCf(hi);
        cpu.setOf(hi);
        cpu.setZf((r & 0xFFFFu) == 0);
        cpu.setSf(msbOf(r, 2));
    } else if (size == 2) {
        const std::uint32_t r = (cpu.eax() & 0xFFFFu) * (src & 0xFFFFu);
        cpu.writeReg(Reg::AX, r & 0xFFFFu);
        cpu.writeReg(Reg::DX, (r >> 16) & 0xFFFFu);
        const bool hi = (r >> 16) != 0;
        cpu.setCf(hi);
        cpu.setOf(hi);
    } else {
        const std::uint64_t r = static_cast<std::uint64_t>(cpu.eax()) * static_cast<std::uint64_t>(src);
        cpu.eax() = static_cast<std::uint32_t>(r & 0xFFFFFFFFu);
        cpu.edx() = static_cast<std::uint32_t>(r >> 32);
        const bool hi = cpu.edx() != 0;
        cpu.setCf(hi);
        cpu.setOf(hi);
    }
    return true;
}

bool doImul(X86& cpu, const Insn& in) {
    const bool has_dst = in.op1().kind != OpKind::None;
    const bool has_imm = in.op2().kind != OpKind::None;
    if (!has_dst) {
        // imul r/m  —— 与 mul 同形，但按有符号算
        const std::uint8_t size = opSize(in.op0());
        const std::int64_t src = signOf(rd(cpu, in.op0()), size);
        if (size == 1) {
            const std::int64_t r = static_cast<std::int8_t>(cpu.eax() & 0xFFu) * src;
            cpu.writeReg(Reg::AX, static_cast<std::uint32_t>(r) & 0xFFFFu);
            const bool ovf = r < -128 || r > 127;
            cpu.setCf(ovf);
            cpu.setOf(ovf);
        } else if (size == 2) {
            const std::int64_t r = static_cast<std::int16_t>(cpu.eax() & 0xFFFFu) * src;
            cpu.writeReg(Reg::AX, static_cast<std::uint32_t>(r) & 0xFFFFu);
            cpu.writeReg(Reg::DX, static_cast<std::uint32_t>(r >> 16) & 0xFFFFu);
            const bool ovf = r < -32768 || r > 32767;
            cpu.setCf(ovf);
            cpu.setOf(ovf);
        } else {
            const std::int64_t r = static_cast<std::int64_t>(static_cast<std::int32_t>(cpu.eax())) * src;
            cpu.eax() = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r) & 0xFFFFFFFFu);
            cpu.edx() = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r) >> 32);
            const bool ovf = r < INT32_MIN || r > INT32_MAX;
            cpu.setCf(ovf);
            cpu.setOf(ovf);
        }
        return true;
    }
    // ★ 两种"显式目标"形态的**被乘数来源不同**（x86 语义）：
    //   · `imul r, r/m`        （0F AF /r）      ⇒ r = r * r/m
    //   · `imul r, r/m, imm8/32`（6B /r ib、69 /r id）⇒ r = **r/m** * imm
    //   先前把两种都当成"op0 * op1"，于是三操作数形态拿**目标寄存器**（初值 0）去乘 ——
    //   `6B C1 07`（imul eax,ecx,7）算出 0 而不是 42（conformance: imul3-imm8 / imul3-imm32）。
    const std::uint8_t size = opSize(in.op0());
    const Operand& multiplicand = has_imm ? in.op1() : in.op0();
    const std::int64_t a = signOf(rd(cpu, multiplicand), size);
    std::int64_t b = 0;
    if (has_imm) {
        // 立即数按**有效操作数宽度**符号扩展（与 decode.cpp 的口径一致；imm8 也走这里）
        b = static_cast<std::int32_t>(static_cast<std::uint32_t>(in.op2().imm));
        if (size == 1) b = static_cast<std::int8_t>(b & 0xFF);
    } else {
        b = signOf(rd(cpu, in.op1()), size);
    }
    const std::int64_t r = a * b;
    const std::uint32_t low = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r) & 0xFFFFFFFFu) & maskOf(size);
    const std::int64_t back = signOf(low, size);
    const bool ovf = back != r;
    cpu.setCf(ovf);
    cpu.setOf(ovf);
    wr(cpu, in.op0(), low);
    return true;
}

bool doDiv(X86& cpu, const Insn& in, bool sign) {
    const std::uint8_t size = opSize(in.op0());
    const std::uint32_t src = rd(cpu, in.op0());
    if (src == 0) return cpuFault(cpu, in, "除零（#DE）");
    if (sign) {
        if (size == 1) {
            const std::int16_t dividend = static_cast<std::int16_t>(cpu.eax() & 0xFFFFu);
            const std::int16_t divisor = static_cast<std::int8_t>(src & 0xFF);
            if (divisor == 0) return cpuFault(cpu, in, "除零（#DE）");
            const std::int32_t q = dividend / divisor;
            const std::int32_t r = dividend % divisor;
            if (q < -128 || q > 127) return cpuFault(cpu, in, "idiv 商溢出（#DE）");
            cpu.writeReg(Reg::AL, static_cast<std::uint32_t>(q) & 0xFFu);
            cpu.writeReg(Reg::AH, static_cast<std::uint32_t>(r) & 0xFFu);
        } else if (size == 2) {
            const std::int32_t dividend = static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(cpu.edx() & 0xFFFFu) << 16) | (cpu.eax() & 0xFFFFu));
            const std::int32_t divisor = static_cast<std::int16_t>(src & 0xFFFFu);
            const std::int32_t q = dividend / divisor;
            const std::int32_t r = dividend % divisor;
            if (q < -32768 || q > 32767) return cpuFault(cpu, in, "idiv 商溢出（#DE）");
            cpu.writeReg(Reg::AX, static_cast<std::uint32_t>(q) & 0xFFFFu);
            cpu.writeReg(Reg::DX, static_cast<std::uint32_t>(r) & 0xFFFFu);
        } else {
            const std::int64_t dividend = static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(cpu.edx()) << 32) | cpu.eax());
            const std::int64_t divisor = static_cast<std::int32_t>(src);
            const std::int64_t q = dividend / divisor;
            const std::int64_t r = dividend % divisor;
            if (q < INT32_MIN || q > INT32_MAX) return cpuFault(cpu, in, "idiv 商溢出（#DE）");
            cpu.eax() = static_cast<std::uint32_t>(static_cast<std::uint64_t>(q) & 0xFFFFFFFFu);
            cpu.edx() = static_cast<std::uint32_t>(static_cast<std::uint64_t>(r) & 0xFFFFFFFFu);
        }
    } else {
        if (size == 1) {
            const std::uint32_t dividend = cpu.eax() & 0xFFFFu;
            const std::uint32_t q = dividend / src;
            const std::uint32_t r = dividend % src;
            if (q > 0xFFu) return cpuFault(cpu, in, "div 商溢出（#DE）");
            cpu.writeReg(Reg::AL, q);
            cpu.writeReg(Reg::AH, r);
        } else if (size == 2) {
            const std::uint32_t dividend = ((cpu.edx() & 0xFFFFu) << 16) | (cpu.eax() & 0xFFFFu);
            const std::uint32_t q = dividend / src;
            const std::uint32_t r = dividend % src;
            if (q > 0xFFFFu) return cpuFault(cpu, in, "div 商溢出（#DE）");
            cpu.writeReg(Reg::AX, q);
            cpu.writeReg(Reg::DX, r);
        } else {
            const std::uint64_t dividend = (static_cast<std::uint64_t>(cpu.edx()) << 32) | cpu.eax();
            const std::uint64_t q = dividend / src;
            const std::uint64_t r = dividend % src;
            if (q > 0xFFFFFFFFull) return cpuFault(cpu, in, "div 商溢出（#DE）");
            cpu.eax() = static_cast<std::uint32_t>(q);
            cpu.edx() = static_cast<std::uint32_t>(r);
        }
    }
    return true;
}

}  // namespace

bool execInteger(X86& cpu, const Insn& in) {
    switch (in.op) {
        case Op::Nop:
        case Op::Pause:
            return true;

        case Op::Mov:
            wr(cpu, in.op0(), rd(cpu, in.op1()));
            return true;

        case Op::Movzx: {
            const std::uint32_t v = cpu.readOp(in.op1()) & maskOf(opSize(in.op1()));
            wr(cpu, in.op0(), v);
            return true;
        }
        case Op::Movsx: {
            const std::int64_t v = signOf(cpu.readOp(in.op1()), opSize(in.op1()));
            wr(cpu, in.op0(), static_cast<std::uint32_t>(v));
            return true;
        }
        case Op::Lea: {
            if (in.op1().kind != OpKind::Mem) return cpuFault(cpu, in, "lea 的第二操作数不是内存");
            cpu.writeReg(in.op0().reg, cpu.eaOf(in.op1().mem));
            return true;
        }
        case Op::Xchg: {
            const std::uint32_t a = rd(cpu, in.op0());
            const std::uint32_t b = rd(cpu, in.op1());
            wr(cpu, in.op0(), b);
            wr(cpu, in.op1(), a);
            return true;
        }
        case Op::Push: {
            cpu.push32(rd(cpu, in.op0()));
            return true;
        }
        case Op::Pop: {
            wr(cpu, in.op0(), cpu.pop32());
            return true;
        }
        case Op::Pushad: {
            const std::uint32_t sp = cpu.esp();
            // 压栈顺序 EAX ECX EDX EBX ESP EBP ESI EDI（ESP 压的是原值）
            cpu.push32(cpu.r[kEax]);
            cpu.push32(cpu.r[kEcx]);
            cpu.push32(cpu.r[kEdx]);
            cpu.push32(cpu.r[kEbx]);
            cpu.push32(sp);
            cpu.push32(cpu.r[kEbp]);
            cpu.push32(cpu.r[kEsi]);
            cpu.push32(cpu.r[kEdi]);
            return true;
        }
        case Op::Popad: {
            // 压栈顺序 EAX ECX EDX EBX ESP EBP ESI EDI ⇒ 弹栈反序；ESP 的值被丢弃
            for (int i = 7; i >= 0; --i) {
                const std::uint32_t v = cpu.pop32();
                if (i != kEsp) cpu.r[i] = v;
            }
            return true;
        }
        case Op::Pushfd: {
            cpu.pushFlags();
            return true;
        }
        case Op::Popfd: {
            cpu.popFlags();
            return true;
        }
        case Op::Xadd: {
            const std::uint32_t dst = rd(cpu, in.op0());
            const std::uint32_t src = rd(cpu, in.op1());
            const std::uint32_t r = dst + src;
            setAddFlags(cpu, dst, src, r, opSize(in.op0()), false);
            wr(cpu, in.op0(), r);
            wr(cpu, in.op1(), dst);
            return true;
        }
        case Op::Cmpxchg: {
            const std::uint8_t size = opSize(in.op0());
            const std::uint32_t dst = rd(cpu, in.op0());
            const std::uint32_t acc = cpu.eax() & maskOf(size);
            const std::uint32_t src = rd(cpu, in.op1());
            const std::uint32_t r = acc - dst;
            setSubFlags(cpu, acc, dst, r, size, false);
            if (acc == dst) {
                wr(cpu, in.op0(), src);
            } else {
                cpu.writeReg(Reg::EAX, (cpu.eax() & ~maskOf(size)) | (dst & maskOf(size)));
            }
            return true;
        }
        case Op::Add:
        case Op::Or:
        case Op::And:
        case Op::Sub:
        case Op::Xor:
        case Op::Cmp:
        case Op::Test:
        case Op::Adc:
        case Op::Sbb: {
            const std::uint8_t size = opSize(in.op0());
            const std::uint32_t a = rd(cpu, in.op0());
            const std::uint32_t b = rd(cpu, in.op1());
            switch (in.op) {
                case Op::Add: {
                    const std::uint32_t r = a + b;
                    setAddFlags(cpu, a, b, r, size, false);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Adc: {
                    const std::uint32_t r = a + b + (cpu.cf() ? 1u : 0u);
                    setAddFlags(cpu, a, b, r, size, true);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Sub: {
                    const std::uint32_t r = a - b;
                    setSubFlags(cpu, a, b, r, size, false);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Sbb: {
                    const std::uint32_t r = a - b - (cpu.cf() ? 1u : 0u);
                    setSubFlags(cpu, a, b, r, size, true);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Cmp: {
                    const std::uint32_t r = a - b;
                    setSubFlags(cpu, a, b, r, size, false);
                    return true;
                }
                case Op::Test: {
                    const std::uint32_t r = a & b;
                    setLogicFlags(cpu, r, size);
                    return true;
                }
                case Op::And: {
                    const std::uint32_t r = a & b;
                    setLogicFlags(cpu, r, size);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Or: {
                    const std::uint32_t r = a | b;
                    setLogicFlags(cpu, r, size);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                case Op::Xor: {
                    const std::uint32_t r = a ^ b;
                    setLogicFlags(cpu, r, size);
                    wr(cpu, in.op0(), r);
                    return true;
                }
                default: return true;
            }
        }
        case Op::Inc:
        case Op::Dec: {
            const std::uint8_t size = opSize(in.op0());
            const std::uint32_t a = rd(cpu, in.op0());
            const std::uint32_t b = 1;
            const std::uint32_t r = (in.op == Op::Inc) ? a + 1 : a - 1;
            const bool saved_cf = cpu.cf();
            if (in.op == Op::Inc) {
                setAddFlags(cpu, a, b, r, size, false);
            } else {
                setSubFlags(cpu, a, b, r, size, false);
            }
            cpu.setCf(saved_cf);  // inc/dec 不动 CF
            wr(cpu, in.op0(), r);
            return true;
        }
        case Op::Neg: {
            const std::uint8_t size = opSize(in.op0());
            const std::uint32_t a = rd(cpu, in.op0());
            const std::uint32_t r = 0u - a;
            setSubFlags(cpu, 0, a, r, size, false);
            wr(cpu, in.op0(), r);
            return true;
        }
        case Op::Not: {
            const std::uint8_t size = opSize(in.op0());
            wr(cpu, in.op0(), ~rd(cpu, in.op0()) & maskOf(size));
            return true;
        }
        case Op::Shl:
        case Op::Shr:
        case Op::Sar:
        case Op::Rol:
        case Op::Ror:
        case Op::Rcl:
        case Op::Rcr:
            return doShift(cpu, in);
        case Op::Shld:
        case Op::Shrd:
            return doShxd(cpu, in);
        case Op::Mul:
            return doMul(cpu, in);
        case Op::Imul:
            return doImul(cpu, in);
        case Op::Div:
            return doDiv(cpu, in, false);
        case Op::Idiv:
            return doDiv(cpu, in, true);
        case Op::Cdq:
            cpu.edx() = (cpu.eax() & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            return true;
        case Op::Cwd:
            cpu.writeReg(Reg::DX, (cpu.eax() & 0x8000u) ? 0xFFFFu : 0u);
            return true;
        case Op::Cwde:
            cpu.eax() = (cpu.eax() & 0x8000u) ? (cpu.eax() | 0xFFFF0000u) : (cpu.eax() & 0xFFFFu);
            return true;
        case Op::Cbw:
            cpu.writeReg(Reg::AX, (cpu.eax() & 0x80u) ? (cpu.eax() | 0xFF00u) : (cpu.eax() & 0xFFu));
            return true;
        case Op::Bswap: {
            const std::uint32_t v = cpu.readReg(in.op0().reg);
            cpu.writeReg(in.op0().reg, ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) |
                                            ((v >> 24) & 0xFFu));
            return true;
        }
        case Op::Bt:
        case Op::Bts:
        case Op::Btr:
        case Op::Btc: {
            const std::uint8_t size = opSize(in.op0());
            const std::uint32_t bit = rd(cpu, in.op1()) & (bitsOf(size) - 1);
            const std::uint32_t v = rd(cpu, in.op0());
            cpu.setCf(((v >> bit) & 1u) != 0);
            std::uint32_t nv = v;
            if (in.op == Op::Bts) nv |= (1u << bit);
            if (in.op == Op::Btr) nv &= ~(1u << bit);
            if (in.op == Op::Btc) nv ^= (1u << bit);
            if (in.op != Op::Bt) wr(cpu, in.op0(), nv);
            return true;
        }
        case Op::Bsf:
        case Op::Bsr: {
            const std::uint8_t size = opSize(in.op1());
            const std::uint32_t v = rd(cpu, in.op1()) & maskOf(size);
            cpu.setZf(v == 0);
            if (v != 0) {
                if (in.op == Op::Bsf) {
                    unsigned i = 0;
                    while (((v >> i) & 1u) == 0) ++i;
                    wr(cpu, in.op0(), i);
                } else {
                    unsigned i = bitsOf(size) - 1;
                    while (((v >> i) & 1u) == 0) --i;
                    wr(cpu, in.op0(), i);
                }
            }
            return true;
        }
        case Op::Cpuid: {
            // ★ 确定性判据（plan §8.3-2）：固定常量描述一台**虚构** CPU，⛔ 不接宿主 CPU。
            const std::uint32_t leaf = cpu.eax();
            if (leaf == 0) {
                cpu.eax() = 1;  // 最大 leaf
                cpu.ebx() = 0x756E6547u;  // "Genu"
                cpu.edx() = 0x49656E69u;  // "ineI"
                cpu.ecx() = 0x6C65746Eu;  // "ntel"
            } else if (leaf == 1) {
                cpu.eax() = 0x000006F2u;  // family 6 / model 15 / stepping 2
                cpu.ebx() = 0x00000800u;  // brand=0 / clflush=8 / logical=1
                cpu.ecx() = 0x00000000u;  // 不声明任何扩展特性
                cpu.edx() = 0x00000001u;  // 只声明 FPU
            } else if (leaf == 0x80000000u) {
                cpu.eax() = 0x80000004u;
                cpu.ebx() = cpu.ecx() = cpu.edx() = 0;
            } else {
                cpu.eax() = cpu.ebx() = cpu.ecx() = cpu.edx() = 0;
            }
            return true;
        }
        case Op::Sahf: {
            const std::uint32_t ah = (cpu.eax() >> 8) & 0xFFu;
            cpu.eflags = (cpu.eflags & ~0x00D5u) | (ah & 0x00D5u) | 0x2u;
            return true;
        }
        case Op::Lahf: {
            const std::uint32_t v = (cpu.eflags & 0xD5u) | 0x2u;
            cpu.writeReg(Reg::AH, v);
            return true;
        }
        case Op::Setcc: {
            // ★ setcc / cmovcc **不是**控制流（不结束块）—— 它们只是"按条件写值"。
            //   放在整数族里，块边界就仍然只由 jcc/jmp/call/ret 决定（README §2.2）。
            wr(cpu, in.op0(), condHolds(in.cc, cpu.eflags) ? 1u : 0u);
            return true;
        }
        case Op::Cmovcc: {
            if (condHolds(in.cc, cpu.eflags)) wr(cpu, in.op0(), rd(cpu, in.op1()));
            return true;
        }
        case Op::Leave: {
            cpu.esp() = cpu.ebp();
            cpu.ebp() = cpu.pop32();
            return true;
        }
        default:
            return unimplemented(cpu, in, "整数族没有这条语义");
    }
}

}  // namespace lift
