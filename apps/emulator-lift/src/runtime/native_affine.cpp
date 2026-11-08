// emulator-lift · native override：Live2D `BDAffine` 逐顶点仿射（sub_4CCC60）
//
// ★ 逐字依据（`.lst` 0x4CCC60..0x4CCD34；`.c` 只作阅读辅助，⛔ 不当锚）：
//   · `__stdcall retn 1Ch` ⇒ 7 个栈参数（28 B）；`[ebp+8]`（`cpu.arg(0)`）**全程未读**；
//     实参 = `cpu.arg(1..6)` = state / src / dst / count / i0 / stride。
//   · 0x4CCC67: `[state+0x24] ?: [state+0x20]` ⇒ inner 对象（下文 `obj`）。
//   · 0x4CCC71..0x4CCC84: `θ = [obj+0x14] * flt_52F1B0`，`fsin` / `fcos`（同一角度）。
//   · 0x4CCC86..0x4CCCA3: `r=[state+0x10]`；`sx=[obj+0x18]==0 ? +1 : -1`、`sy=[obj+0x19]==0 ? +1 : -1`。
//   · 0x4CCCA6..0x4CCCD2: 四系数 A/B/C/D（`fchs` 在 B 上）+ 平移 `[obj+4]` / `[obj+8]`。
//   · 0x4CCCEE..0x4CCD21: 逐点 `(ix,iy) → (out0,out1)`；`ecx += stride`、`eax += 4*stride`。
//     ★ 每步**先读两个再写两个**；`stride == 1` 时相邻两点的 `dst[i+1]` / `dst[i]` 会重叠，后写赢。
//     ★ 循环上界 = `count*stride` 的 **32 位回绕**，比较是**有符号**的。
//   · 0x4CCD2C: 返回 `EAX = dst + 4*(i0+1+N*stride)`（进循环时），否则 inner 对象。
// ★ x87 逐位：算术走 SoftFloat `extF80`；`fsin`/`fcos` 走 `x87ToDouble → std::sin/cos →
//   x87FromDouble`（与 `exec_fpu.cpp` 的 `transcendental()` 同一条路径）⇒ 运算符顺序照抄即可逐位相同。
//   ⛔ 不许用 `double` 重写：那会差最后一位 float，破坏逐字节 A/B。
// ★ 状态字：解释器每个 FP 指令都 `syncStatus`；本实现只在末尾补一次（OR 是可交换的，
//   且 `fsin`/`fcos` 之后没有任何指令改 C0..C3 —— 逐条核过 exec_fpu.cpp）。
#include "runtime/native_affine.h"

#include <cmath>
#include <cstring>

#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/x86.h"

namespace lift {
namespace {

constexpr std::uint32_t kAngleScaleEa = 0x0052F1B0u;  // `flt_52F1B0`（角度 → 弧度）
constexpr std::uint32_t kStateBytes = 0x28u;          // `+0x10` / `+0x20` / `+0x24`
constexpr std::uint32_t kObjBytes = 0x1Au;            // `+0x04/+0x08/+0x14/+0x18/+0x19`
constexpr double kSinLimit = 9223372036854775808.0;   // 2^63（fsin 的 C2 界）

float bitsToFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool fpuStackEmpty(const X86& cpu) {
    for (int i = 0; i < 8; ++i)
        if (!cpu.fpu.empty(i)) return false;
    return true;
}

// 复刻 exec_fpu.cpp 的 `syncStatus`：把 SoftFloat 的粘性异常位并进 x87 状态字。
// ★ 解释器每条 FP 指令 sync 一次 ⇒ 末态是各次异常位的 OR；这里一次取走同一批位，结果相同。
// ★ C0..C3：本函数里只有 `fsin`/`fcos` 会写它们（都清 0），之后的算术 / 存取 / fxch / fchs
//   都不改 CC ⇒ 末尾清一次与逐条 sync 等价。
// ★ TOP：本函数净栈深 0 ⇒ 与入口相同；仍然按解释器的写法回填一次。
void syncX87Status(X86& cpu) {
    std::uint16_t s = cpu.fpu.status;
    const std::uint8_t f = x87TakeExceptionFlags();
    if (f & 1u) s |= 0x0020u;   // PE（inexact）
    if (f & 2u) s |= 0x0010u;   // UE（underflow）
    if (f & 4u) s |= 0x0008u;   // OE（overflow）
    if (f & 8u) s |= 0x0004u;   // ZE（divide-by-zero）
    if (f & 16u) s |= 0x0001u;  // IE（invalid）
    if (s & 0x003Fu) s |= 0x0080u;                                                     // ES
    s = static_cast<std::uint16_t>(s & ~0x4700u);                                      // C0..C3 = 0
    s = static_cast<std::uint16_t>((s & ~0x3800u) | ((cpu.fpu.top & 7u) << 11));       // TOP
    cpu.fpu.status = s;
}

}  // namespace

bool affineAngleInRange(float angle, float angle_scale) {
    const double a = angle;
    const double s = angle_scale;
    if (!std::isfinite(a) || !std::isfinite(s)) return false;
    return std::fabs(a * s) < kSinLimit / 4.0;
}

bool affineCoeffs(const Ext80 angle, const Ext80 angle_scale, const Ext80 radius, bool flip_x, bool flip_y,
                  AffineCoeffs* out) {
    if (out == nullptr) return false;
    Ext80 theta;
    x87Mul(angle, angle_scale, theta);  // fld [obj+0x14]; fmul flt_52F1B0
    const double d = x87ToDouble(theta);
    if (!(std::fabs(d) < kSinLimit)) return false;  // 防御性：正常路径由 affineAngleInRange 判掉
    Ext80 sn, cs;
    x87FromDouble(std::sin(d), sn);  // fsin（transcendental() 同路径）
    x87FromDouble(std::cos(d), cs);  // fcos
    Ext80 sx, sy, t1, t2, t3, b, c, dd;
    x87FromI32(flip_x ? -1 : 1, sx);  // fild ±1（`[obj+0x18] == 0` ⇒ +1）
    x87FromI32(flip_y ? -1 : 1, sy);
    x87Mul(sx, radius, t1);   // fmul st, st(2)      ⇒ sx*r
    x87Mul(t1, cs, t2);       // fld st; fmul st,st(4) ⇒ sx*r*cos = A
    x87Mul(radius, sy, t3);   // fxch st(2); fmulp st(3),st ⇒ r*sy
    x87Mul(t3, sn, b);        // fld st(2); fmul st,st(5) ⇒ r*sy*sin
    x87ChangeSign(b);         // fchs                ⇒ B = -(r*sy*sin)
    x87Mul(sn, t1, dd);       // fxch st(1); fmulp st(5),st ⇒ sin*(sx*r) = D
    x87Mul(cs, t3, c);        // fxch st(2); fmulp st(3),st ⇒ cos*(r*sy) = C
    std::memcpy(out->a, t2, 16);
    std::memcpy(out->b, b, 16);
    std::memcpy(out->c, c, 16);
    std::memcpy(out->d, dd, 16);
    return true;
}

void affinePoint(const AffineCoeffs& k, const Ext80 ix, const Ext80 iy, const Ext80 add4, const Ext80 add8,
                 Ext80 out0, Ext80 out1) {
    Ext80 p, q, s, r;
    // out0 = (iy*B) + (ix*A) + [obj+4]（faddp 的 st1+st0 顺序）
    x87Mul(iy, k.b, p);
    x87Mul(ix, k.a, q);
    x87Add(p, q, s);
    x87Add(s, add4, r);
    std::memcpy(out0, r, 16);
    // out1 = (iy*C) + (ix*D) + [obj+8]
    x87Mul(iy, k.c, p);
    x87Mul(ix, k.d, q);
    x87Add(p, q, s);
    x87Add(s, add8, r);
    std::memcpy(out1, r, 16);
}

bool nativeAffine4CCC60(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    const std::uint32_t state = cpu.arg(1);  // a2（[ebp+0Ch]）
    const std::uint32_t src = cpu.arg(2);    // a3
    const std::uint32_t dst = cpu.arg(3);    // a4
    const std::int32_t count = static_cast<std::int32_t>(cpu.arg(4));   // a5
    const std::int32_t i0 = static_cast<std::int32_t>(cpu.arg(5));      // a6
    const std::int32_t stride = static_cast<std::int32_t>(cpu.arg(6));  // a7

    // ── ① 准入（全部在**任何 FPU 副作用 / guest 写**之前）────────────────────────────────
    if (state == 0) return false;
    if (static_cast<std::uint64_t>(state) + kStateBytes > GuestMemory::kSpace) return false;
    if (!mem.committed(state, kStateBytes)) return false;
    const std::uint32_t o24 = mem.rd32(state + 0x24u);
    const std::uint32_t obj = o24 != 0u ? o24 : mem.rd32(state + 0x20u);
    if (obj == 0) return false;
    if (static_cast<std::uint64_t>(obj) + kObjBytes > GuestMemory::kSpace) return false;
    if (!mem.committed(obj, kObjBytes)) return false;
    if (!mem.committed(kAngleScaleEa, 4u)) return false;
    if (!fpuStackEmpty(cpu)) return false;  // guest 会把栈推到 8；非空栈在 guest 里是溢出路径

    // 空循环：`cmp ecx, edx; jge`（32 位有符号；`bound = count*stride` 是 32 位回绕）
    const std::int32_t bound = static_cast<std::int32_t>(static_cast<std::uint32_t>(count) *
                                                         static_cast<std::uint32_t>(stride));
    if (bound <= i0) {
        *result = obj;  // 跳过循环时 EAX 仍是 inner 对象
        return true;
    }
    if (stride <= 0 || i0 < 0) return false;  // 非空循环：步长 <= 0 / 负起点 ⇒ 交解释器（可能死循环）
    const std::int64_t n = (static_cast<std::int64_t>(bound) - i0 + stride - 1) / stride;
    const std::int64_t last = static_cast<std::int64_t>(i0) + (n - 1) * stride;
    if (last < 0 || last > 2147483646ll) return false;  // `last+1` 还要能落进 int32
    const std::uint64_t span = 4ull * static_cast<std::uint64_t>(last - i0 + 2);  // 覆盖 [i0, last+1]
    const std::uint64_t src_lo = static_cast<std::uint64_t>(src) + 4ull * static_cast<std::uint64_t>(i0);
    const std::uint64_t dst_lo = static_cast<std::uint64_t>(dst) + 4ull * static_cast<std::uint64_t>(i0);
    if (src_lo + span > GuestMemory::kSpace || dst_lo + span > GuestMemory::kSpace) return false;
    if (!mem.committed(static_cast<std::uint32_t>(src_lo), static_cast<std::size_t>(span))) return false;
    if (!mem.committed(static_cast<std::uint32_t>(dst_lo), static_cast<std::size_t>(span))) return false;
    const std::uint32_t angle_bits = mem.rd32(obj + 0x14u);
    const std::uint32_t scale_bits = mem.rd32(kAngleScaleEa);
    if (!affineAngleInRange(bitsToFloat(angle_bits), bitsToFloat(scale_bits))) return false;

    // ── ② 从这里开始不再回退（所有 guest 写都在下面）────────────────────────────────────
    x87ApplyControlWord(cpu.fpu.control);
    std::uint8_t raw[4];
    Ext80 angle, scale, radius, add4, add8;
    std::memcpy(raw, &angle_bits, 4);
    x87MemToReg(raw, angle, 4);
    std::memcpy(raw, &scale_bits, 4);
    x87MemToReg(raw, scale, 4);
    const std::uint32_t radius_bits = mem.rd32(state + 0x10u);
    std::memcpy(raw, &radius_bits, 4);
    x87MemToReg(raw, radius, 4);
    x87MemToReg(mem.host(obj + 0x04u), add4, 4);
    x87MemToReg(mem.host(obj + 0x08u), add8, 4);
    AffineCoeffs k;
    if (!affineCoeffs(angle, scale, radius, mem.rd8(obj + 0x18u) != 0u, mem.rd8(obj + 0x19u) != 0u, &k)) {
        x87TakeExceptionFlags();  // 不可达（范围已判过）；保持"回退无 x87 残留"
        return false;
    }
    const std::uint8_t* sp = mem.host(static_cast<std::uint32_t>(src_lo));
    std::uint8_t* dp = mem.host(static_cast<std::uint32_t>(dst_lo));
    for (std::int64_t i = i0; i <= last; i += stride) {
        const std::size_t off = static_cast<std::size_t>(4 * (i - i0));
        Ext80 ix, iy, o0, o1;
        x87MemToReg(sp + off, ix, 4);      // fld [src+i]
        x87MemToReg(sp + off + 4, iy, 4);  // fld [src+i+1]
        affinePoint(k, ix, iy, add4, add8, o0, o1);
        std::uint8_t b0[4], b1[4];
        x87RegToMem(o0, b0, 4);  // fstp dword [dst+i]
        std::memcpy(dp + off, b0, 4);
        x87RegToMem(o1, b1, 4);  // fstp dword [dst+i+1]
        std::memcpy(dp + off + 4, b1, 4);
    }
    syncX87Status(cpu);
    // 出口 EAX：`dst + 4*(i0+1+N*stride)`（32 位回绕；与 guest 的 add 序列同值）
    *result = static_cast<std::uint32_t>(static_cast<std::uint64_t>(dst) +
                                        4ull * static_cast<std::uint64_t>(last + stride + 1));
    return true;
}

}  // namespace lift
