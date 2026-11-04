// emulator-lift · x87 ↔ Berkeley SoftFloat（`extF80`）的桥
//
// ★ 这里**没有**我们自己写的浮点算术：四则 / 开方 / 取余 / 取整 / 比较全部转给 SoftFloat。
//   理由（plan §4 D3）：extF80 就是 x86 的 80 位扩展；PC/RC 与 `extF80_roundingPrecision` /
//   `softfloat_roundingMode` 一一对应；纯整数实现 ⇒ 三个 preset 逐位一致。
// ★ 一个**关键的巧合**（省掉一层转换）：SoftFloat 的 `extFloat80_t`（8086 特化，LITTLEENDIAN）
//   布局是 `{uint64 signif; uint16 signExp;}`，长度 16 —— 与 x86 的 10 字节内存格式
//   **前 10 字节逐字节相同**。所以 guest 寄存器槽里存的就是它，内存 ↔ 寄存器只在
//   4/8 字节（float/double）这两个宽度上需要真转换。
// ⚠ 已知边界（plan §4 D3）：SoftFloat 不复现 x87 的 pseudo-denormal / unnormal 表示。
#include "runtime/fpu_bridge.h"

#include <cmath>
#include <cstring>

extern "C" {
// ★ 必须先 include `platform.h`：`LITTLEENDIAN` 决定 `extFloat80_t` 的**字段顺序**
//   （小端 = `{uint64 signif; uint16 signExp;}`，大端 = 反序）。库侧的 `.c` 都先 include 它，
//   而这里原先只 include `softfloat.h` ⇒ 同一个结构体在**库侧与我们的 TU 里布局不同**：
//   `signif` / `signExp` 互相错位，于是 `x87ChangeSign` / `x87Abs` / `x87IsZero` / `x87IsNan` /
//   `x87Scale` 全在错的偏移上动手（实测：`fchs` 翻到 significand 的低位字，`-1.5` 不变号）。
//   ⛔ 不要删这一行，也不要挪到 `softfloat.h` 之后（那就白写了）。
#include "platform.h"
#include "softfloat.h"
}

namespace lift {
namespace {

extFloat80_t& as(Ext80 v) { return *reinterpret_cast<extFloat80_t*>(v); }
const extFloat80_t& as(const Ext80 v) { return *reinterpret_cast<const extFloat80_t*>(v); }

void store(extFloat80_t v, Ext80 out) { std::memcpy(out, &v, sizeof(extFloat80_t)); }

float32_t f32FromBits(std::uint32_t bits) {
    float32_t f;
    f.v = bits;
    return f;
}
float64_t f64FromBits(std::uint64_t bits) {
    float64_t f;
    f.v = bits;
    return f;
}
float64_t f64FromDouble(double d) {
    float64_t f;
    std::memcpy(&f.v, &d, 8);
    return f;
}

}  // namespace

void x87MemToReg(const std::uint8_t* mem, Ext80 out, std::uint8_t size) {
    extFloat80_t v{};
    switch (size) {
        case 4: {
            std::uint32_t bits = 0;
            std::memcpy(&bits, mem, 4);
            v = f32_to_extF80(f32FromBits(bits));
            break;
        }
        case 8: {
            std::uint64_t bits = 0;
            std::memcpy(&bits, mem, 8);
            v = f64_to_extF80(f64FromBits(bits));
            break;
        }
        case 10:
        default:
            // 与 x86 内存格式逐字节相同（见文件头）
            std::memset(out, 0, 16);
            std::memcpy(out, mem, 10);
            return;
    }
    store(v, out);
}

void x87RegToMem(const Ext80 in, std::uint8_t* mem, std::uint8_t size) {
    const extFloat80_t v = as(in);
    switch (size) {
        case 4: {
            const float32_t f = extF80_to_f32(v);
            std::memcpy(mem, &f.v, 4);
            break;
        }
        case 8: {
            const float64_t f = extF80_to_f64(v);
            std::memcpy(mem, &f.v, 8);
            break;
        }
        case 10:
        default:
            std::memcpy(mem, &v, 10);
            break;
    }
}

void x87FromI16(std::int16_t v, Ext80 out) { store(i32_to_extF80(v), out); }
void x87FromI32(std::int32_t v, Ext80 out) { store(i32_to_extF80(v), out); }
void x87FromI64(std::int64_t v, Ext80 out) { store(i64_to_extF80(v), out); }
void x87FromU32(std::uint32_t v, Ext80 out) { store(ui32_to_extF80(v), out); }
void x87FromU64(std::uint64_t v, Ext80 out) { store(ui64_to_extF80(v), out); }

std::int32_t x87ToI32(const Ext80 in, bool truncate) {
    return static_cast<std::int32_t>(
        extF80_to_i32(as(in), truncate ? softfloat_round_minMag : softfloat_roundingMode, false));
}
std::int64_t x87ToI64(const Ext80 in, bool truncate) {
    return static_cast<std::int64_t>(
        extF80_to_i64(as(in), truncate ? softfloat_round_minMag : softfloat_roundingMode, false));
}

// ★ x87 的"超越常数"（FLD1/FLDZ/FLDPI/FLDL2E/FLDL2T/FLDLG2/FLDLN2）压栈的是**64 位有效位**的
//   扩展精度常数，不是 double（53 位）舍入值（Intel SDM Vol.2 对每条 FLD 常数都写明"该常数按
//   扩展精度四舍五入到 64 位有效位"）。旧实现走 `f64_to_extF80(字面量)` ⇒ 低 11 位恒为 0，
//   与真机**逐位不同**（差 ~2^-64 相对，肉眼看不见，但"逐位"这句话就不成立了）。
//   ⇒ 这里直接写 64 位有效位的位模式，值由一次独立的 BigInt 定点（2^-400）推导得到：
//        π    = 16·atan(1/5) − 4·atan(1/239)（Machin）
//        ln2  = 2·atanh(1/3)        ln5 = 2·ln2 + 2·atanh(1/9)      ln10 = ln2 + ln5
//        log2(e) = 1/ln2            log10(2) = ln2/ln10              log2(10) = ln10/ln2
//     再按"最近偶数"取 64 位有效位。求法与判据写在 `docs/s4-fpu-audit.md`（含 10 字节位模式），
//     守卫逐字节钉住：`x87-fldpi` / `x87-fldln2` / `x87-fldlg2` / `x87-fldl2e` / `x87-fldl2t`。
static void x87Const(std::uint16_t signExp, std::uint64_t signif, Ext80 out) {
    std::memset(out, 0, 16);
    // 小端：`extFloat80_t` = {uint64 signif; uint16 signExp}，前 10 字节就是 x86 的内存格式
    std::memcpy(out, &signif, 8);
    std::memcpy(out + 8, &signExp, 2);
}
void x87One(Ext80 out) { store(i32_to_extF80(1), out); }
void x87Zero(Ext80 out) { store(i32_to_extF80(0), out); }
void x87Pi(Ext80 out) { x87Const(0x4000u, 0xC90FDAA22168C235ull, out); }
void x87Ln2(Ext80 out) { x87Const(0x3FFEu, 0xB17217F7D1CF79ACull, out); }
void x87Lg2(Ext80 out) { x87Const(0x3FFDu, 0x9A209A84FBCFF799ull, out); }
void x87L2e(Ext80 out) { x87Const(0x3FFFu, 0xB8AA3B295C17F0BCull, out); }
void x87L2t(Ext80 out) { x87Const(0x4000u, 0xD49A784BCD1B8AFEull, out); }
void x87Ln10(Ext80 out) { x87Const(0x4000u, 0x935D8DDDAAA8AC17ull, out); }

void x87Add(const Ext80 a, const Ext80 b, Ext80 out) { store(extF80_add(as(a), as(b)), out); }
void x87Sub(const Ext80 a, const Ext80 b, Ext80 out) { store(extF80_sub(as(a), as(b)), out); }
void x87Mul(const Ext80 a, const Ext80 b, Ext80 out) { store(extF80_mul(as(a), as(b)), out); }
void x87Div(const Ext80 a, const Ext80 b, Ext80 out) { store(extF80_div(as(a), as(b)), out); }
void x87Sqrt(const Ext80 a, Ext80 out) { store(extF80_sqrt(as(a)), out); }
void x87RoundToInt(const Ext80 a, Ext80 out) {
    store(extF80_roundToInt(as(a), softfloat_roundingMode, false), out);
}
void x87Rem(const Ext80 a, const Ext80 b, Ext80 out) {
    // x87 的 fprem 是"部分余数"（可能没算完，C2=1）；SoftFloat 的 rem 一次算完 ⇒ C2 恒 0。
    // 这在数值上是同样的结果（fprem 循环到 C2=0 的终值），差别只在中间步数。
    store(extF80_rem(as(a), as(b)), out);
}
void x87Scale(const Ext80 a, const Ext80 b, Ext80 out) {
    // fscale：ST0 * 2^trunc(ST1)。★ 不再手改指数域（那会把下溢做成 pseudo-denormal）——
    // 而是**构造 2^e** 再走 `extF80_mul`：溢出 / 下溢 / 精度标志全部由 SoftFloat 给，
    // 乘 2 的幂本身是精确的（不会误置 inexact）。
    const double d = x87ToDouble(b);
    std::int64_t e = 0;
    if (std::isnan(d)) {
        store(as(a), out);  // fscale 的 NaN 语义不在本仓关键路径上：原样返回，别静默改值
        return;
    }
    if (d > 20000.0) e = 20000;
    else if (d < -20000.0) e = -20000;
    else e = static_cast<std::int64_t>(d < 0 ? -std::floor(-d) : std::floor(d));

    extFloat80_t p{};
    const std::uint16_t sign = static_cast<std::uint16_t>(as(a).signExp & 0x8000u);
    if (e > 16383) {
        // 结果必然上溢：交给 mul 之前的饱和（置 OE 后返回 inf）
        p.signExp = 0x7FFF;
        p.signif = 0x8000000000000000ull;
        softfloat_raiseFlags(softfloat_flag_overflow);
    } else if (e >= -16382) {
        p.signExp = static_cast<std::uint16_t>(16383 + e);
        p.signif = 0x8000000000000000ull;
    } else if (e >= -16445) {
        // 次正规的 2^e（signif 的整数位放在 bit63 ⇒ 值 = signif * 2^-16445）
        p.signExp = 0;
        p.signif = 1ull << static_cast<unsigned>(e + 16445);
    } else {
        p.signExp = 0;
        p.signif = 0;
        softfloat_raiseFlags(softfloat_flag_underflow);
    }
    if (sign) p.signExp = static_cast<std::uint16_t>(p.signExp | 0x8000u);
    store(extF80_mul(as(a), p), out);
}
void x87ChangeSign(Ext80 v) { as(v).signExp ^= 0x8000u; }
void x87Abs(Ext80 v) { as(v).signExp &= 0x7FFFu; }

int x87Compare(const Ext80 a, const Ext80 b) {
    const extFloat80_t av = as(a);
    const extFloat80_t bv = as(b);
    if (extF80_isSignalingNaN(av) || extF80_isSignalingNaN(bv)) {
        softfloat_raiseFlags(softfloat_flag_invalid);
        return 2;
    }
    // ★ 无序（quiet NaN）也是 `cmp == 2`：`extF80_eq/lt` 对 QNaN 都是 false，
    //   不先判 NaN 会把它读成 "greater" ⇒ fcom 的 C3/C2/C0 全错（实测夹具 x87-fcom-unordered）。
    if (x87IsNan(a) || x87IsNan(b)) return 2;
    if (extF80_eq(av, bv)) return 0;
    if (extF80_lt(av, bv)) return -1;
    return 1;
}
bool x87IsNan(const Ext80 a) {
    const extFloat80_t v = as(a);
    const std::uint16_t exp = v.signExp & 0x7FFFu;
    return exp == 0x7FFFu && (v.signif & 0x7FFFFFFFFFFFFFFFull) != 0;
}
bool x87IsZero(const Ext80 a) {
    const extFloat80_t v = as(a);
    return (v.signExp & 0x7FFFu) == 0 && v.signif == 0;
}

bool x87IsInf(const Ext80 a) {
    const extFloat80_t v = as(a);
    return (v.signExp & 0x7FFFu) == 0x7FFFu && (v.signif << 1) == 0;
}
bool x87SignBit(const Ext80 a) { return (as(a).signExp & 0x8000u) != 0; }

X87Class x87Classify(const Ext80 a) {
    const extFloat80_t v = as(a);
    const std::uint16_t e = static_cast<std::uint16_t>(v.signExp & 0x7FFFu);
    if (e == 0x7FFFu) return (v.signif << 1) != 0 ? X87Class::Nan : X87Class::Infinity;
    if (e == 0) return v.signif == 0 ? X87Class::Zero : X87Class::Denormal;
    return X87Class::Normal;
}

void x87Extract(const Ext80 a, Ext80 significand, Ext80 exponent) {
    // FXTTRACT：ST(0) = 尾数（[1,2)，指数域 = bias），ST(1) = 指数（带符号整数，extF80 表示）
    const extFloat80_t v = as(a);
    const std::uint16_t sign = static_cast<std::uint16_t>(v.signExp & 0x8000u);
    const std::uint16_t e = static_cast<std::uint16_t>(v.signExp & 0x7FFFu);
    extFloat80_t s{};
    extFloat80_t x{};
    if (e == 0 && v.signif == 0) {
        // ±0 ⇒ 尾数 ±0、指数 −∞（Intel：不置 invalid）
        s.signExp = sign;
        s.signif = 0;
        x.signExp = static_cast<std::uint16_t>(0x7FFFu | sign);
        x.signif = 0x8000000000000000ull;
    } else if (e == 0x7FFFu) {
        s = v;  // inf / NaN 原样穿过（本仓不作伪值）
        x = v;
    } else {
        std::uint64_t m = v.signif;
        std::int32_t biased = e;
        if (e == 0) {  // 次正规：先规格化，指数随之下降
            int sh = 0;
            while ((m & 0x8000000000000000ull) == 0) {
                m <<= 1;
                ++sh;
            }
            biased = static_cast<std::int32_t>(1 - sh);
        }
        s.signExp = static_cast<std::uint16_t>(0x3FFFu | sign);
        s.signif = m;
        x = i32_to_extF80(static_cast<std::int32_t>(biased) - 16383);
    }
    store(s, significand);
    store(x, exponent);
}

void x87RemTrunc(const Ext80 a, const Ext80 b, Ext80 out) {
    // x87 的 FPREM 是**商朝零截断**的余数（fmod 语义）；SoftFloat 的 `extF80_rem` 是
    // IEEE 余数（商取最近偶数，= FPREM1）。两者只在商的奇偶性上差一个 |b|：
    //   令 r = a − n·b（n = 最近偶数商）。若 sign(r) 与 sign(a) 相同（或 r = 0），
    //   则 trunc(a/b) 就是 n ⇒ 结果 = r；否则 trunc(a/b) = n ∓ 1 ⇒ 结果 = r + sign(a)·|b|。
    // ★ 这一步是**精确**的：真余数的有效位不超过 64 位，SoftFloat 的加减不会引入舍入。
    const extFloat80_t av = as(a);
    const extFloat80_t bv = as(b);
    extFloat80_t r = extF80_rem(av, bv);
    const std::uint16_t re = static_cast<std::uint16_t>(r.signExp & 0x7FFFu);
    if (re == 0x7FFFu) {  // NaN / inf：rem 已经给出结果（并置了 invalid）
        store(r, out);
        return;
    }
    if (r.signif == 0 && re == 0) {
        store(r, out);
        return;
    }
    const bool sa = (av.signExp & 0x8000u) != 0;
    const bool sr = (r.signExp & 0x8000u) != 0;
    if (sa == sr) {
        store(r, out);
        return;
    }
    extFloat80_t shift = bv;
    shift.signExp = static_cast<std::uint16_t>(shift.signExp & 0x7FFFu);  // |b|
    if (sa) shift.signExp = static_cast<std::uint16_t>(shift.signExp | 0x8000u);  // sign(a)·|b|
    store(extF80_add(r, shift), out);
}

double x87ToDouble(const Ext80 a) {
    const float64_t f = extF80_to_f64(as(a));
    double d = 0;
    std::memcpy(&d, &f.v, 8);
    return d;
}
void x87FromDouble(double d, Ext80 out) { store(f64_to_extF80(f64FromDouble(d)), out); }

std::uint8_t x87PeekExceptionFlags() { return softfloat_exceptionFlags; }
void x87ClearExceptionFlags() { softfloat_exceptionFlags = 0; }
void x87RaiseInvalid() { softfloat_raiseFlags(softfloat_flag_invalid); }

void x87ApplyControlWord(std::uint16_t control) {
    switch ((control >> 10) & 3u) {
        case 0: softfloat_roundingMode = softfloat_round_near_even; break;
        case 1: softfloat_roundingMode = softfloat_round_min; break;
        case 2: softfloat_roundingMode = softfloat_round_max; break;
        default: softfloat_roundingMode = softfloat_round_minMag; break;
    }
    switch ((control >> 8) & 3u) {
        case 0: extF80_roundingPrecision = 32; break;
        case 2: extF80_roundingPrecision = 64; break;
        default: extF80_roundingPrecision = 80; break;
    }
    softfloat_detectTininess = softfloat_tininess_afterRounding;
}

std::uint8_t x87TakeExceptionFlags() {
    const std::uint8_t f = softfloat_exceptionFlags;
    softfloat_exceptionFlags = 0;
    return f;
}

}  // namespace lift
