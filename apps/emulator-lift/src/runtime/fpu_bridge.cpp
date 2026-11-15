// emulator-lift · x87 的**实数模型**（D49：`double`）：实现
//
// ★ 这里**没有**地址空间、没有寄存器栈、没有 tag word —— 只回答"值怎么算"。
//   状态（8 个槽 / TOP / tag / 状态字 / 控制字）在 `exec_fpu.cpp`（解释器）与
//   `src/aot/lift_gen_emit.cpp`（发射器）里各有一份**同一语义**的实现。
//
// ★ 为什么保留"挂账异常位"这条通道（而不是把异常位直接写进 X87State）：
//   1. 解释器现有 20 处 `syncStatus()` 的调用形状不必改（取走 + 映射进状态字）；
//   2. `fpu-thread-flags` 那条守卫（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`）要的正是
//      "这份挂账**每线程一份**" —— 音频 feeder 是第二条 guest 执行流。
//   ⇒ 存放是 `thread_local`，⛔ 不要退回进程级全局。
#include "runtime/fpu_bridge.h"

#include <cmath>
#include <cstring>

namespace lift {
namespace {

// ★ 每线程一份（见文件头的第 2 条）。⛔ 不加 `static`：那会把 TLS 变成"每 TU 一份"。
thread_local std::uint8_t g_x87_flags = 0;

void raise(std::uint8_t bits) { g_x87_flags = static_cast<std::uint8_t>(g_x87_flags | bits); }

// ---------------- x87 的 10 字节扩展精度内存格式 ↔ double ----------------
// 布局（小端）：[0..7] = 尾数（显式整数位在 bit63）· [8..9] = 符号 + 15 位偏置指数（bias 16383）
double m80ToDouble(const std::uint8_t* p) {
    std::uint64_t signif = 0;
    std::uint16_t se = 0;
    std::memcpy(&signif, p, 8);
    std::memcpy(&se, p + 8, 2);
    const bool neg = (se & 0x8000u) != 0;
    const unsigned e = static_cast<unsigned>(se & 0x7FFFu);
    if (e == 0x7FFFu) {
        if ((signif << 1) == 0) return neg ? -INFINITY : INFINITY;
        // NaN：保住 quiet 位（m80 的 bit62 ⇒ double 的 bit51）与高 52 位尾数
        std::uint64_t frac = (signif >> 11) & 0xFFFFFFFFFFFFFull;
        if (frac == 0) frac = 0x8000000000000ull;  // 尾数全是低 11 位 ⇒ 强行留成 NaN
        std::uint64_t bits = (static_cast<std::uint64_t>(neg) << 63) | (0x7FFull << 52) | frac;
        double d = 0;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    if (e == 0 && signif == 0) return neg ? -0.0 : 0.0;
    // 值 = signif · 2^(biased − 16383 − 63)；`e == 0`（pseudo-denormal）按 biased = 1 算
    const int biased = (e == 0) ? 1 : static_cast<int>(e);
    const double m = static_cast<double>(signif);  // 就近到 53 位
    const double v = std::ldexp(m, biased - 16383 - 63);
    return neg ? -v : v;
}

void doubleToM80(double v, std::uint8_t* p) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    const bool neg = (bits >> 63) != 0;
    const unsigned exp = static_cast<unsigned>((bits >> 52) & 0x7FFu);
    std::uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
    std::uint64_t signif = 0;
    unsigned se = 0;
    if (exp == 0x7FFu) {
        se = 0x7FFFu;
        // inf ⇒ 整数位 1、余 0；NaN ⇒ 整数位 1 + quiet 位 + payload（bit51 ⇒ bit62）
        signif = (frac == 0) ? 0x8000000000000000ull : (0x8000000000000000ull | (frac << 11));
    } else if (exp == 0 && frac == 0) {
        se = 0;
        signif = 0;
    } else if (exp == 0) {
        // double 次正规：先规格化（bit52 置位）再按正规公式落位
        int sh = 0;
        while ((frac & 0x10000000000000ull) == 0) {
            frac <<= 1;
            ++sh;
        }
        signif = 0x8000000000000000ull | ((frac & 0xFFFFFFFFFFFFFull) << 11);
        se = static_cast<unsigned>(16383 - 1022 - sh);
    } else {
        signif = 0x8000000000000000ull | (frac << 11);
        se = static_cast<unsigned>(static_cast<int>(exp) - 1023 + 16383);
    }
    if (neg) se |= 0x8000u;
    std::memset(p, 0, 10);
    std::memcpy(p, &signif, 8);
    const std::uint16_t se16 = static_cast<std::uint16_t>(se);
    std::memcpy(p + 8, &se16, 2);
}

// 按 RC 取整（`frndint` / 整数转换共用唯一一份）。NaN / ±inf 原样返回。
double roundByRc(double v, X87Round rc) {
    if (std::isnan(v) || std::isinf(v)) return v;
    switch (rc) {
        case X87Round::Down: return std::floor(v);
        case X87Round::Up: return std::ceil(v);
        case X87Round::Zero: return std::trunc(v);
        default: return std::nearbyint(v);  // 宿主默认 = 就近偶数（D49：⛔ 不逐 op fesetround）
    }
}

std::int32_t toI32(double v, X87Round rc) {
    const double r = roundByRc(v, rc);
    // ±2^31 是边界：|r| ≥ 2^31 越界（−2^31 本身**在**范围内）
    if (!(r >= -2147483648.0 && r < 2147483648.0)) {
        raise(kX87IE);
        return static_cast<std::int32_t>(0x80000000u);
    }
    return static_cast<std::int32_t>(r);
}

std::int64_t toI64(double v, X87Round rc) {
    const double r = roundByRc(v, rc);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        raise(kX87IE);
        return static_cast<std::int64_t>(0x8000000000000000ull);
    }
    return static_cast<std::int64_t>(r);
}

}  // namespace

X87Round x87RoundOf(std::uint16_t control) {
    return static_cast<X87Round>((control >> 10) & 3u);
}

// ---------------- 内存 ↔ 寄存器 ----------------
X87Val x87LoadMem(const std::uint8_t* mem, unsigned size) {
    switch (size) {
        case 4: {
            float f = 0.0f;
            std::memcpy(&f, mem, 4);
            return static_cast<double>(f);  // m32 → double 是**精确**的
        }
        case 8: {
            double d = 0;
            std::memcpy(&d, mem, 8);
            return d;
        }
        case 10:
        default: return m80ToDouble(mem);
    }
}

void x87StoreMem(X87Val v, std::uint8_t* mem, unsigned size) {
    switch (size) {
        case 4: {
            const float f = static_cast<float>(v);  // 就近偶数（x87 的 fst m32 同样按 RC；见文件头 §3）
            std::memcpy(mem, &f, 4);
            return;
        }
        case 8: std::memcpy(mem, &v, 8); return;
        case 10:
        default: doubleToM80(v, mem); return;
    }
}

// ---------------- 整数 ↔ double ----------------
// ★ i64 → double 在 |v| > 2^53 时会丢低位（53 位尾数）—— D49 接受的偏差（旧 extF80 路径是精确的）。
X87Val x87FromI16(std::int16_t v) { return static_cast<double>(v); }
X87Val x87FromI32(std::int32_t v) { return static_cast<double>(v); }
X87Val x87FromI64(std::int64_t v) { return static_cast<double>(v); }

std::int32_t x87ToI32(X87Val v, X87Round rc) { return toI32(v, rc); }
std::int64_t x87ToI64(X87Val v, X87Round rc) { return toI64(v, rc); }
std::int32_t x87ToI32Trunc(X87Val v) { return toI32(v, X87Round::Zero); }
std::int64_t x87ToI64Trunc(X87Val v) { return toI64(v, X87Round::Zero); }

// ---------------- 常量 ----------------
// ★ 旧实现压的是"64 位有效位的扩展精度常数"（与真机逐位相同）。D49 之后寄存器是 double
//   ⇒ 常数只能到 53 位尾数，**这与真机的差**是 D49 明写接受的第一条偏差。
X87Val x87One() { return 1.0; }
X87Val x87Zero() { return 0.0; }
X87Val x87Pi() { return 3.14159265358979323846264338327950288; }
X87Val x87Ln2() { return 0.693147180559945309417232121458176568; }
X87Val x87Lg2() { return 0.301029995663981195213738894724493027; }
X87Val x87L2e() { return 1.44269504088896340735992468100189214; }
X87Val x87L2t() { return 3.32192809488736234787031942948939018; }

// ---------------- 四则 ----------------
// ★ 异常位（任务 0 / D55 的裁决）：OE / UE 由**结果结构**判定，判据只有一份（下面这个函数）；
//   PE **恒 0**（[fpu_bridge.h](fpu_bridge.h) 文件头 §5）。生成代码发同一份判据的 IR
//   （`FnEmit::x87RaiseFloatFlags`）⇒ 两臂逐位可比。
//   OE ⇐ r 是 ±inf 且 a、b **都有限**（把"运算上溢到 inf"与"inf 传播 / 除零"分开）；
//   UE ⇐ r 是次正规（`|r| < DBL_MIN && r != 0`；⚠ 真机还要"且不精确"，本模型没有 PE ⇒ 按结构判）。
void x87RaiseOverflowUnderflow(X87Val a, X87Val b, X87Val r) {
    if (std::isinf(r) && std::isfinite(a) && std::isfinite(b)) raise(kX87OE);
    if (std::fpclassify(r) == FP_SUBNORMAL) raise(kX87UE);
}

X87Val x87Add(X87Val a, X87Val b) {
    const X87Val r = a + b;
    x87RaiseOverflowUnderflow(a, b, r);
    return r;
}
X87Val x87Sub(X87Val a, X87Val b) {
    const X87Val r = a - b;
    x87RaiseOverflowUnderflow(a, b, r);
    return r;
}
X87Val x87Mul(X87Val a, X87Val b) {
    const X87Val r = a * b;
    x87RaiseOverflowUnderflow(a, b, r);
    return r;
}
X87Val x87Div(X87Val a, X87Val b) {
    if (std::isinf(a) && std::isinf(b)) raise(kX87IE);
    else if (b == 0.0) {
        if (a == 0.0 || std::isnan(a)) raise(kX87IE);
        else raise(kX87ZE);
    }
    const X87Val r = a / b;
    // ★ `b == 0` 的 inf 是 ZE 的结果，⛔ 不是上溢 ⇒ 不判 OE（UE 不受影响，inf 不是次正规）。
    if (b != 0.0) x87RaiseOverflowUnderflow(a, b, r);
    return r;
}
X87Val x87Sqrt(X87Val a) {
    if (a < 0.0) raise(kX87IE);
    return std::sqrt(a);
}
X87Val x87RoundToInt(X87Val v, X87Round rc) { return roundByRc(v, rc); }
// `fprem1` = IEEE 余数（商取最近偶数）；`fprem` = 商朝零截断（fmod）。
// ★ C2（"没算完"）在本模型里恒 0：double 一次算得完（与旧 SoftFloat 路径同口径）。
X87Val x87Rem(X87Val a, X87Val b) {
    if (b == 0.0 || std::isinf(a)) raise(kX87IE);
    return std::remainder(a, b);
}
X87Val x87RemTrunc(X87Val a, X87Val b) {
    if (b == 0.0 || std::isinf(a)) raise(kX87IE);
    return std::fmod(a, b);
}
X87Val x87Scale(X87Val a, X87Val b) {
    if (std::isnan(b)) return a;  // 与旧实现一致：NaN 指数 ⇒ 原样返回（⛔ 不静默改值）
    int e = 0;
    if (b > 20000.0) e = 20000;
    else if (b < -20000.0) e = -20000;
    else e = static_cast<int>(std::trunc(b));
    const X87Val r = std::ldexp(a, e);
    // `fscale` 也会上溢 / 下溢：按同一条判据（指数操作数用 1.0 —— 它是 scale 的"次数"，不是值）
    x87RaiseOverflowUnderflow(a, 1.0, r);
    return r;
}
X87Val x87ChangeSign(X87Val v) { return -v; }  // 只翻符号位（含 NaN：`-v` 就是符号位取反）
X87Val x87Abs(X87Val v) { return std::fabs(v); }

// ---------------- 比较 / 分类 ----------------
bool x87IsNan(X87Val v) { return std::isnan(v); }
bool x87IsSnan(X87Val v) {
    if (!std::isnan(v)) return false;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    return (bits & 0x0008000000000000ull) == 0;  // quiet 位（bit51）为 0 ⇒ signaling
}
bool x87IsZero(X87Val v) { return v == 0.0; }  // ±0 都算 zero（tag 01 的判据）
bool x87IsInf(X87Val v) { return std::isinf(v); }
bool x87SignBit(X87Val v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    return (bits >> 63) != 0;
}

int x87Compare(X87Val a, X87Val b, bool quiet) {
    const bool na = std::isnan(a);
    const bool nb = std::isnan(b);
    if (na || nb) {
        // `fcom`（quiet=false）对 QNaN 也置 IE；`fucom`（quiet=true）只对 SNaN 置。
        if (!quiet || x87IsSnan(a) || x87IsSnan(b)) raise(kX87IE);
        return 2;
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

X87Class x87Classify(X87Val v) {
    if (std::isnan(v)) return X87Class::Nan;
    if (std::isinf(v)) return X87Class::Infinity;
    if (v == 0.0) return X87Class::Zero;
    if (std::fpclassify(v) == FP_SUBNORMAL) return X87Class::Denormal;
    return X87Class::Normal;
}

void x87Extract(X87Val a, X87Val* significand, X87Val* exponent) {
    if (std::isnan(a) || std::isinf(a)) {  // inf / NaN 原样穿过（本仓不作伪值）
        *significand = a;
        *exponent = a;
        return;
    }
    if (a == 0.0) {  // ±0 ⇒ 尾数 ±0、指数 −∞（Intel：不置 invalid）
        *significand = a;
        *exponent = -INFINITY;
        return;
    }
    int e = 0;
    const X87Val m = std::frexp(a, &e);  // a = m·2^e，m ∈ [0.5,1)
    *significand = m * 2.0;              // [1,2)
    *exponent = static_cast<X87Val>(e - 1);
}

// ---------------- 超越函数（宿主 libm：唯一的进出口） ----------------
X87Val x87Sin(X87Val x) { return std::sin(x); }
X87Val x87Cos(X87Val x) { return std::cos(x); }
X87Val x87Tan(X87Val x) { return std::tan(x); }
X87Val x87Atan2(X87Val y, X87Val x) { return std::atan2(y, x); }
X87Val x87Log2(X87Val x) { return std::log2(x); }
X87Val x87Log2p1(X87Val x) { return std::log2(x + 1.0); }
X87Val x87Exp2m1(X87Val x) { return std::exp2(x) - 1.0; }
bool x87TrigInRange(X87Val x) { return std::fabs(x) < 9223372036854775808.0; }

// ---------------- 异常位（挂账通道） ----------------
std::uint8_t x87PeekExceptionFlags() { return g_x87_flags; }
void x87ClearExceptionFlags() { g_x87_flags = 0; }
void x87RaiseInvalid() { raise(kX87IE); }
std::uint8_t x87TakeExceptionFlags() {
    const std::uint8_t f = g_x87_flags;
    g_x87_flags = 0;
    return f;
}

}  // namespace lift
