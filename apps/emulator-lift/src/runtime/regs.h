// emulator-lift · guest 寄存器的唯一标识
//
// ★ 为什么要有这一层：解码器（Zydis）与语义层（exec_*.cpp）之间只认这个枚举。
//   换解码器只改 decode.cpp 的映射，语义层不动。
// ★ 8 位寄存器分低半（AL..BL，byte_off=0）与高半（AH..BH，byte_off=1）：
//   写 AL 必须保留 EAX 的高 24 位，写 AH 必须保留其余 24 位 —— 由 X86::writeReg 统一处理，
//   ⛔ 语义层不要自己拼字节。
#pragma once

#include <cstdint>

namespace lift {

enum class Reg : std::uint8_t {
    None = 0,
    // ---- 32 位通用寄存器（index = gpr[] 下标）----
    EAX = 1, ECX = 2, EDX = 3, EBX = 4, ESP = 5, EBP = 6, ESI = 7, EDI = 8,
    // ---- 16 位（低半）----
    AX = 9, CX = 10, DX = 11, BX = 12, SP = 13, BP = 14, SI = 15, DI = 16,
    // ---- 8 位低半 ----
    AL = 17, CL = 18, DL = 19, BL = 20,
    // ---- 8 位高半 ----
    AH = 21, CH = 22, DH = 23, BH = 24,
    // ---- x87 栈（ST0..ST7；逻辑编号，物理编号 = (top + i) & 7）----
    ST0 = 25, ST1 = 26, ST2 = 27, ST3 = 28, ST4 = 29, ST5 = 30, ST6 = 31, ST7 = 32,
    // ---- 段寄存器 ----
    ES = 33, CS = 34, SS = 35, DS = 36, FS = 37, GS = 38,
    Count = 39,
};

enum class RegClass : std::uint8_t { None = 0, Gpr8, Gpr16, Gpr32, X87, Seg };

struct RegSpec {
    RegClass cls;
    std::uint8_t index;    // GPR 0..7 / x87 0..7 / 段 0..5
    std::uint8_t size;     // 字节宽度：1 / 2 / 4 / 10 / 2
    std::uint8_t byte_off; // 8 位 GPR 在 32 位寄存器里的字节偏移（0 或 1）
};

constexpr RegSpec regSpec(Reg r) noexcept {
    using C = RegClass;
    const auto i = static_cast<std::uint8_t>(r);
    if (i >= 1 && i <= 8) return {C::Gpr32, static_cast<std::uint8_t>(i - 1), 4, 0};
    if (i >= 9 && i <= 16) return {C::Gpr16, static_cast<std::uint8_t>(i - 9), 2, 0};
    if (i >= 17 && i <= 20) return {C::Gpr8, static_cast<std::uint8_t>(i - 17), 1, 0};
    if (i >= 21 && i <= 24) return {C::Gpr8, static_cast<std::uint8_t>(i - 21), 1, 1};
    if (i >= 25 && i <= 32) return {C::X87, static_cast<std::uint8_t>(i - 25), 10, 0};
    if (i >= 33 && i <= 38) return {C::Seg, static_cast<std::uint8_t>(i - 33), 2, 0};
    return {C::None, 0, 0, 0};
}

constexpr bool isGpr(Reg r) noexcept {
    const RegClass c = regSpec(r).cls;
    return c == RegClass::Gpr8 || c == RegClass::Gpr16 || c == RegClass::Gpr32;
}

constexpr bool isGpr32(Reg r) noexcept { return regSpec(r).cls == RegClass::Gpr32; }
constexpr bool isX87(Reg r) noexcept { return regSpec(r).cls == RegClass::X87; }

// EFLAGS 位定义（用真位位置：pushfd / popfd / sahf 因此是平凡的）
enum : std::uint32_t {
    FLAG_CF = 0x00000001u,
    FLAG_PF = 0x00000004u,
    FLAG_AF = 0x00000010u,
    FLAG_ZF = 0x00000040u,
    FLAG_SF = 0x00000080u,
    FLAG_DF = 0x00000400u,
    FLAG_OF = 0x00000800u,
    FLAG_IF = 0x00000200u,
};

enum GprIndex : std::uint8_t { kEax = 0, kEcx = 1, kEdx = 2, kEbx = 3, kEsp = 4, kEbp = 5, kEsi = 6, kEdi = 7 };

}  // namespace lift
