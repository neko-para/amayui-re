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

void x87One(Ext80 out) { store(i32_to_extF80(1), out); }
void x87Zero(Ext80 out) { store(i32_to_extF80(0), out); }
void x87Pi(Ext80 out) { store(f64_to_extF80(f64FromDouble(3.14159265358979323846)), out); }
void x87Ln2(Ext80 out) { store(f64_to_extF80(f64FromDouble(0.69314718055994530942)), out); }
void x87Lg2(Ext80 out) { store(f64_to_extF80(f64FromDouble(0.30102999566398119521)), out); }
void x87L2e(Ext80 out) { store(f64_to_extF80(f64FromDouble(1.44269504088896340736)), out); }
void x87L2t(Ext80 out) { store(f64_to_extF80(f64FromDouble(3.32192809488736234787)), out); }
void x87Ln10(Ext80 out) { store(f64_to_extF80(f64FromDouble(2.30258509299404568402)), out); }

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
    // fscale：ST0 * 2^trunc(ST1)。上游这版没有 scalbn ⇒ 直接改 80 位编码的指数域
    // （x87 的 fscale 本来就是这个语义；这里只处理正规值 / 零，溢出按饱和处理）。
    extFloat80_t r = as(a);
    const std::int64_t e = static_cast<std::int64_t>(extF80_to_i64(as(b), softfloat_round_minMag, false));
    const std::uint16_t sign = static_cast<std::uint16_t>(r.signExp & 0x8000u);
    std::int32_t exp = static_cast<std::int32_t>(r.signExp & 0x7FFFu);
    const bool is_zero_or_denormal = (r.signif == 0) || (exp == 0 && r.signif != 0);
    if (!is_zero_or_denormal) {
        const std::int64_t ne = static_cast<std::int64_t>(exp) + e;
        if (ne <= 0) {
            exp = 0;
            r.signif = 0;
        } else if (ne >= 0x7FFF) {
            exp = 0x7FFF;
            r.signif = 0x8000000000000000ull;
            softfloat_raiseFlags(softfloat_flag_overflow);
        } else {
            exp = static_cast<std::int32_t>(ne);
        }
    }
    r.signExp = static_cast<std::uint16_t>(sign | static_cast<std::uint16_t>(exp));
    store(r, out);
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
