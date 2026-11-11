// emulator-lift · native override `sub_4E0B40` = libvorbis `mdct_backward` 的实现
//
// ★ 逐字依据见 `native_mdct.h`（版本判据 + `.lst` 锚 + 已知偏差）。
// ★ 本文件分两段：
//   ① 纯核 `mdctBackwardHost` —— 照抄 xiph vorbis v1.0.1 `lib/mdct.c` 的 `mdct_backward`
//      与其 `mdct_butterflies` / `mdct_butterfly_*` / `mdct_bitreverse`；
//   ② guest 入口 `nativeMdct4E0B40` —— 只做准入（9 个回退原因）与 flat 指针换算。
// ★ 为什么中间值用 `double` 而不是 `float`：跑这条链的是 **feeder 线程**（`audio_cpu_.fpu.reset()`
//   ⇒ 控制字 0x037F = 80-bit），所以"寄存器段"是 80-bit；guest 的 `REG_TYPE` 虽是 `float`，编译后
//   **只在溢出槽**（`.lst` 0x4E02F7 / 0x4E0308 / 0x4E0626 的 `fstp [ebp+var_*]`，dword）才舍入到
//   32-bit。用 `float` 会把所有中间量压到 24-bit（比 guest 差一个量级）；用 `double` 只有寄存器段的
//   53-bit 差异（1e-16，远小于溢出槽的 1e-7），且不复现溢出点 ⇒ 实测 1.0e-7…1.5e-7。
//   ⛔ 不要写成 "x87 80-bit vs double"：把夹具精度改成 53-bit 复测，七个用例的 `max|d|` 一个数都不变。
#include "runtime/native_mdct.h"

#include <vector>

#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/x86.h"

namespace lift {
namespace {

constexpr int kMinN = 64;    // mdct.h 的约束 `[64 <= n]`
constexpr int kMaxN = 8192;  // vorbis 最大块 = 8192（`blocksize_1` 上限）
constexpr std::uint32_t kInitBytes = 16;

// `mdct_lookup` 的字段偏移（`.lst` 0x4E0030 的四条 store：n / log2n / trig / bitrev）。
constexpr std::uint32_t kOffN = 0;
constexpr std::uint32_t kOffLog2N = 4;
constexpr std::uint32_t kOffTrig = 8;
constexpr std::uint32_t kOffBitrev = 12;

// ---- mdct.h 的常量（非 `MDCT_INTEGERIZED` 那一档；`sub_4E0B40` 全是 `fld dword` ⇒ float）----
constexpr float kPI3_8 = 0.38268343236508977175f;
constexpr float kPI2_8 = 0.70710678118654752441f;
constexpr float kPI1_8 = 0.92387953251128675613f;

using Reg = double;  // 见文件头：近似 x87 的 80-bit 中间值
inline float store(Reg v) { return static_cast<float>(v); }
inline Reg ld(float v) { return static_cast<Reg>(v); }

// ---------------- mdct.c 的本地 static 函数（逐行照抄） ----------------

// 8 point butterfly (in place, 4 register)
void butterfly8(float* x) {
    Reg r0 = ld(x[6]) + ld(x[2]);
    Reg r1 = ld(x[6]) - ld(x[2]);
    Reg r2 = ld(x[4]) + ld(x[0]);
    Reg r3 = ld(x[4]) - ld(x[0]);

    x[6] = store(r0 + r2);
    x[4] = store(r0 - r2);

    r0 = ld(x[5]) - ld(x[1]);
    r2 = ld(x[7]) - ld(x[3]);
    x[0] = store(r1 + r0);
    x[2] = store(r1 - r0);

    r0 = ld(x[5]) + ld(x[1]);
    r1 = ld(x[7]) + ld(x[3]);
    x[3] = store(r2 + r3);
    x[1] = store(r2 - r3);
    x[7] = store(r1 + r0);
    x[5] = store(r1 - r0);
}

// 16 point butterfly (in place, 4 register)
void butterfly16(float* x) {
    Reg r0 = ld(x[1]) - ld(x[9]);
    Reg r1 = ld(x[0]) - ld(x[8]);

    x[8] = store(ld(x[8]) + ld(x[0]));
    x[9] = store(ld(x[9]) + ld(x[1]));
    x[0] = store((r0 + r1) * ld(kPI2_8));
    x[1] = store((r0 - r1) * ld(kPI2_8));

    r0 = ld(x[3]) - ld(x[11]);
    r1 = ld(x[10]) - ld(x[2]);
    x[10] = store(ld(x[10]) + ld(x[2]));
    x[11] = store(ld(x[11]) + ld(x[3]));
    x[2] = store(r0);
    x[3] = store(r1);

    r0 = ld(x[12]) - ld(x[4]);
    r1 = ld(x[13]) - ld(x[5]);
    x[12] = store(ld(x[12]) + ld(x[4]));
    x[13] = store(ld(x[13]) + ld(x[5]));
    x[4] = store((r0 - r1) * ld(kPI2_8));
    x[5] = store((r0 + r1) * ld(kPI2_8));

    r0 = ld(x[14]) - ld(x[6]);
    r1 = ld(x[15]) - ld(x[7]);
    x[14] = store(ld(x[14]) + ld(x[6]));
    x[15] = store(ld(x[15]) + ld(x[7]));
    x[6] = store(r0);
    x[7] = store(r1);

    butterfly8(x);
    butterfly8(x + 8);
}

// 32 point butterfly (in place, 4 register)
void butterfly32(float* x) {
    Reg r0 = ld(x[30]) - ld(x[14]);
    Reg r1 = ld(x[31]) - ld(x[15]);

    x[30] = store(ld(x[30]) + ld(x[14]));
    x[31] = store(ld(x[31]) + ld(x[15]));
    x[14] = store(r0);
    x[15] = store(r1);

    r0 = ld(x[28]) - ld(x[12]);
    r1 = ld(x[29]) - ld(x[13]);
    x[28] = store(ld(x[28]) + ld(x[12]));
    x[29] = store(ld(x[29]) + ld(x[13]));
    x[12] = store(r0 * ld(kPI1_8) - r1 * ld(kPI3_8));
    x[13] = store(r0 * ld(kPI3_8) + r1 * ld(kPI1_8));

    r0 = ld(x[26]) - ld(x[10]);
    r1 = ld(x[27]) - ld(x[11]);
    x[26] = store(ld(x[26]) + ld(x[10]));
    x[27] = store(ld(x[27]) + ld(x[11]));
    x[10] = store((r0 - r1) * ld(kPI2_8));
    x[11] = store((r0 + r1) * ld(kPI2_8));

    r0 = ld(x[24]) - ld(x[8]);
    r1 = ld(x[25]) - ld(x[9]);
    x[24] = store(ld(x[24]) + ld(x[8]));
    x[25] = store(ld(x[25]) + ld(x[9]));
    x[8] = store(r0 * ld(kPI3_8) - r1 * ld(kPI1_8));
    x[9] = store(r1 * ld(kPI3_8) + r0 * ld(kPI1_8));

    r0 = ld(x[22]) - ld(x[6]);
    r1 = ld(x[7]) - ld(x[23]);
    x[22] = store(ld(x[22]) + ld(x[6]));
    x[23] = store(ld(x[23]) + ld(x[7]));
    x[6] = store(r1);
    x[7] = store(r0);

    r0 = ld(x[4]) - ld(x[20]);
    r1 = ld(x[5]) - ld(x[21]);
    x[20] = store(ld(x[20]) + ld(x[4]));
    x[21] = store(ld(x[21]) + ld(x[5]));
    x[4] = store(r1 * ld(kPI1_8) + r0 * ld(kPI3_8));
    x[5] = store(r1 * ld(kPI3_8) - r0 * ld(kPI1_8));

    r0 = ld(x[2]) - ld(x[18]);
    r1 = ld(x[3]) - ld(x[19]);
    x[18] = store(ld(x[18]) + ld(x[2]));
    x[19] = store(ld(x[19]) + ld(x[3]));
    x[2] = store((r1 + r0) * ld(kPI2_8));
    x[3] = store((r1 - r0) * ld(kPI2_8));

    r0 = ld(x[0]) - ld(x[16]);
    r1 = ld(x[1]) - ld(x[17]);
    x[16] = store(ld(x[16]) + ld(x[0]));
    x[17] = store(ld(x[17]) + ld(x[1]));
    x[0] = store(r1 * ld(kPI3_8) + r0 * ld(kPI1_8));
    x[1] = store(r1 * ld(kPI1_8) - r0 * ld(kPI3_8));

    butterfly16(x);
    butterfly16(x + 16);
}

// N point first stage butterfly (in place, 2 register)
void butterflyFirst(const float* T, float* x, int points) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    do {
        Reg r0 = ld(x1[6]) - ld(x2[6]);
        Reg r1 = ld(x1[7]) - ld(x2[7]);
        x1[6] = store(ld(x1[6]) + ld(x2[6]));
        x1[7] = store(ld(x1[7]) + ld(x2[7]));
        x2[6] = store(r1 * ld(T[1]) + r0 * ld(T[0]));
        x2[7] = store(r1 * ld(T[0]) - r0 * ld(T[1]));

        r0 = ld(x1[4]) - ld(x2[4]);
        r1 = ld(x1[5]) - ld(x2[5]);
        x1[4] = store(ld(x1[4]) + ld(x2[4]));
        x1[5] = store(ld(x1[5]) + ld(x2[5]));
        x2[4] = store(r1 * ld(T[5]) + r0 * ld(T[4]));
        x2[5] = store(r1 * ld(T[4]) - r0 * ld(T[5]));

        r0 = ld(x1[2]) - ld(x2[2]);
        r1 = ld(x1[3]) - ld(x2[3]);
        x1[2] = store(ld(x1[2]) + ld(x2[2]));
        x1[3] = store(ld(x1[3]) + ld(x2[3]));
        x2[2] = store(r1 * ld(T[9]) + r0 * ld(T[8]));
        x2[3] = store(r1 * ld(T[8]) - r0 * ld(T[9]));

        r0 = ld(x1[0]) - ld(x2[0]);
        r1 = ld(x1[1]) - ld(x2[1]);
        x1[0] = store(ld(x1[0]) + ld(x2[0]));
        x1[1] = store(ld(x1[1]) + ld(x2[1]));
        x2[0] = store(r1 * ld(T[13]) + r0 * ld(T[12]));
        x2[1] = store(r1 * ld(T[12]) - r0 * ld(T[13]));

        x1 -= 8;
        x2 -= 8;
        T += 16;
    } while (x2 >= x);
}

// N/stage point generic N stage butterfly (in place, 2 register)
void butterflyGeneric(const float* T, float* x, int points, int trigint) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    do {
        Reg r0 = ld(x1[6]) - ld(x2[6]);
        Reg r1 = ld(x1[7]) - ld(x2[7]);
        x1[6] = store(ld(x1[6]) + ld(x2[6]));
        x1[7] = store(ld(x1[7]) + ld(x2[7]));
        x2[6] = store(r1 * ld(T[1]) + r0 * ld(T[0]));
        x2[7] = store(r1 * ld(T[0]) - r0 * ld(T[1]));
        T += trigint;

        r0 = ld(x1[4]) - ld(x2[4]);
        r1 = ld(x1[5]) - ld(x2[5]);
        x1[4] = store(ld(x1[4]) + ld(x2[4]));
        x1[5] = store(ld(x1[5]) + ld(x2[5]));
        x2[4] = store(r1 * ld(T[1]) + r0 * ld(T[0]));
        x2[5] = store(r1 * ld(T[0]) - r0 * ld(T[1]));
        T += trigint;

        r0 = ld(x1[2]) - ld(x2[2]);
        r1 = ld(x1[3]) - ld(x2[3]);
        x1[2] = store(ld(x1[2]) + ld(x2[2]));
        x1[3] = store(ld(x1[3]) + ld(x2[3]));
        x2[2] = store(r1 * ld(T[1]) + r0 * ld(T[0]));
        x2[3] = store(r1 * ld(T[0]) - r0 * ld(T[1]));
        T += trigint;

        r0 = ld(x1[0]) - ld(x2[0]);
        r1 = ld(x1[1]) - ld(x2[1]);
        x1[0] = store(ld(x1[0]) + ld(x2[0]));
        x1[1] = store(ld(x1[1]) + ld(x2[1]));
        x2[0] = store(r1 * ld(T[1]) + r0 * ld(T[0]));
        x2[1] = store(r1 * ld(T[0]) - r0 * ld(T[1]));
        T += trigint;

        x1 -= 8;
        x2 -= 8;
    } while (x2 >= x);
}

void butterflies(int log2n, const float* trig, float* x, int points) {
    const float* T = trig;
    int stages = log2n - 5;
    int i, j;

    if (--stages > 0) butterflyFirst(T, x, points);

    for (i = 1; --stages > 0; i++) {
        for (j = 0; j < (1 << i); j++) butterflyGeneric(T, x + (points >> i) * j, points >> i, 4 << i);
    }

    for (j = 0; j < points; j += 32) butterfly32(x + j);
}

void bitreverse(int n, const std::int32_t* bitrev, const float* trig, float* x) {
    float* w0 = x;
    float* w1 = x = w0 + (n >> 1);
    const float* T = trig + n;

    do {
        const float* x0 = x + bitrev[0];
        const float* x1 = x + bitrev[1];

        Reg r0 = ld(x0[1]) - ld(x1[1]);
        Reg r1 = ld(x0[0]) + ld(x1[0]);
        Reg r2 = r1 * ld(T[0]) + r0 * ld(T[1]);
        Reg r3 = r1 * ld(T[1]) - r0 * ld(T[0]);

        w1 -= 4;

        r0 = (ld(x0[1]) + ld(x1[1])) * 0.5;
        r1 = (ld(x0[0]) - ld(x1[0])) * 0.5;

        w0[0] = store(r0 + r2);
        w1[2] = store(r0 - r2);
        w0[1] = store(r1 + r3);
        w1[3] = store(r3 - r1);

        x0 = x + bitrev[2];
        x1 = x + bitrev[3];

        r0 = ld(x0[1]) - ld(x1[1]);
        r1 = ld(x0[0]) + ld(x1[0]);
        r2 = r1 * ld(T[2]) + r0 * ld(T[3]);
        r3 = r1 * ld(T[3]) - r0 * ld(T[2]);

        r0 = (ld(x0[1]) + ld(x1[1])) * 0.5;
        r1 = (ld(x0[0]) - ld(x1[0])) * 0.5;

        w0[2] = store(r0 + r2);
        w1[0] = store(r0 - r2);
        w0[3] = store(r1 + r3);
        w1[1] = store(r3 - r1);

        T += 4;
        bitrev += 4;
        w0 += 4;
    } while (w0 < w1);
}

}  // namespace

void mdctBackwardHost(int n, int log2n, const float* trig, const std::int32_t* bitrev, const float* in, float* out) {
    const int n2 = n >> 1;
    const int n4 = n >> 2;

    // ---- rotate（两段；`in` 只读，`out` 从头到尾只被写）----
    {
        const float* iX = in + n2 - 7;
        float* oX = out + n2 + n4;
        const float* T = trig + n4;

        do {
            oX -= 4;
            oX[0] = store(-ld(iX[2]) * ld(T[3]) - ld(iX[0]) * ld(T[2]));
            oX[1] = store(ld(iX[0]) * ld(T[3]) - ld(iX[2]) * ld(T[2]));
            oX[2] = store(-ld(iX[6]) * ld(T[1]) - ld(iX[4]) * ld(T[0]));
            oX[3] = store(ld(iX[4]) * ld(T[1]) - ld(iX[6]) * ld(T[0]));
            iX -= 8;
            T += 4;
        } while (iX >= in);
    }
    {
        const float* iX = in + n2 - 8;
        float* oX = out + n2 + n4;
        const float* T = trig + n4;

        do {
            T -= 4;
            oX[0] = store(ld(iX[4]) * ld(T[3]) + ld(iX[6]) * ld(T[2]));
            oX[1] = store(ld(iX[4]) * ld(T[2]) - ld(iX[6]) * ld(T[3]));
            oX[2] = store(ld(iX[0]) * ld(T[1]) + ld(iX[2]) * ld(T[0]));
            oX[3] = store(ld(iX[0]) * ld(T[0]) - ld(iX[2]) * ld(T[1]));
            iX -= 8;
            oX += 4;
        } while (iX >= in);
    }

    butterflies(log2n, trig, out + n2, n2);
    bitreverse(n, bitrev, trig, out);

    // ---- rotate + window ----
    {
        float* oX1 = out + n2 + n4;
        float* oX2 = out + n2 + n4;
        const float* iX = out;
        const float* T = trig + n2;

        do {
            oX1 -= 4;

            oX1[3] = store(ld(iX[0]) * ld(T[1]) - ld(iX[1]) * ld(T[0]));
            oX2[0] = store(-(ld(iX[0]) * ld(T[0]) + ld(iX[1]) * ld(T[1])));

            oX1[2] = store(ld(iX[2]) * ld(T[3]) - ld(iX[3]) * ld(T[2]));
            oX2[1] = store(-(ld(iX[2]) * ld(T[2]) + ld(iX[3]) * ld(T[3])));

            oX1[1] = store(ld(iX[4]) * ld(T[5]) - ld(iX[5]) * ld(T[4]));
            oX2[2] = store(-(ld(iX[4]) * ld(T[4]) + ld(iX[5]) * ld(T[5])));

            oX1[0] = store(ld(iX[6]) * ld(T[7]) - ld(iX[7]) * ld(T[6]));
            oX2[3] = store(-(ld(iX[6]) * ld(T[6]) + ld(iX[7]) * ld(T[7])));

            oX2 += 4;
            iX += 8;
            T += 8;
        } while (iX < oX1);

        const float* iX2 = out + n2 + n4;
        float* oX3 = out + n4;
        float* oX4 = oX3;

        do {
            oX3 -= 4;
            iX2 -= 4;

            oX4[0] = -(oX3[3] = iX2[3]);
            oX4[1] = -(oX3[2] = iX2[2]);
            oX4[2] = -(oX3[1] = iX2[1]);
            oX4[3] = -(oX3[0] = iX2[0]);

            oX4 += 4;
        } while (oX4 < iX2);

        const float* iX3 = out + n2 + n4;
        float* oX5 = out + n2 + n4;
        float* oX6 = out + n2;
        do {
            oX5 -= 4;
            oX5[0] = iX3[3];
            oX5[1] = iX3[2];
            oX5[2] = iX3[1];
            oX5[3] = iX3[0];
            iX3 += 4;
        } while (oX5 > oX6);
    }
}

namespace {
bool decline(X86& cpu, MdctWhy why) {
    if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
    return false;
}

int exactLog2(int n) {
    int k = 0;
    while ((1 << k) < n) ++k;
    return k;
}
}  // namespace

bool nativeMdct4E0B40(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t init = cpu.arg(0);
    const std::uint32_t in_ea = cpu.arg(1);
    const std::uint32_t out_ea = cpu.arg(2);

    if (!mem.committed(init, kInitBytes)) return decline(cpu, MdctWhy::InitUnreadable);

    const std::int32_t n = static_cast<std::int32_t>(mem.rd32(init + kOffN));
    const std::int32_t log2n = static_cast<std::int32_t>(mem.rd32(init + kOffLog2N));
    const std::uint32_t trig_ea = mem.rd32(init + kOffTrig);
    const std::uint32_t bitrev_ea = mem.rd32(init + kOffBitrev);

    if (n < kMinN || n > kMaxN) return decline(cpu, MdctWhy::SizeOutOfRange);
    if ((n & (n - 1)) != 0) return decline(cpu, MdctWhy::NotPowerOfTwo);
    if (log2n != exactLog2(n)) return decline(cpu, MdctWhy::Log2Mismatch);

    const int n2 = n >> 1;
    const int n4 = n >> 2;

    if (!mem.committed(in_ea, static_cast<std::size_t>(n2) * 4u)) return decline(cpu, MdctWhy::InUnreadable);
    if (!mem.committed(out_ea, static_cast<std::size_t>(n) * 4u)) return decline(cpu, MdctWhy::OutUnreadable);
    if (!mem.committed(trig_ea, static_cast<std::size_t>(n + n4) * 4u)) return decline(cpu, MdctWhy::TrigUnreadable);
    if (!mem.committed(bitrev_ea, static_cast<std::size_t>(n4) * 4u)) return decline(cpu, MdctWhy::BitrevUnreadable);

    // ★ `bitrev` 的**值域**必须自己判：纯核用 `out + n/2 + bit[k]` 直接算宿主指针，
    //   越界不是 guest fault（没有页表兜底）而是宿主野指针 ⇒ 读任意宿主内存。
    //   值域取**真表的范围** `[0, n2-2]`（`mdct_init` 的 `((~acc)&mask)-1`：偶数项 `[2, n2-2]`、
    //   奇数项 `[0, n2-4]`，**最大值 `n2-2` 就在 `[0]`**）。为什么不用更宽的"内存安全界"
    //   `[-n2, n2-2]`：负值会让 `mdct_bitreverse` 读 `out[0, n2)`（**还没写过**的半段）⇒ 输出依赖
    //   缓冲旧内容，"原地调用安全"就不再是不变量。收紧到 `[0, n2-2]` 后，被接受的表只读已写半段。
    //   ⛔ 别把上界再收紧到 `n/4`（那条实测会让**每一个 n** 立刻 decline、`all` 里这一项永远回退）。
    {
        const auto* bit = reinterpret_cast<const std::int32_t*>(mem.host(bitrev_ea));
        for (int i = 0; i < n4; ++i) {
            if (bit[i] < 0 || bit[i] > n2 - 2) return decline(cpu, MdctWhy::BitrevOutOfRange);
        }
    }

    const auto* in_p = reinterpret_cast<const float*>(mem.host(in_ea));
    auto* out_p = reinterpret_cast<float*>(mem.host(out_ea));
    const auto* trig_p = reinterpret_cast<const float*>(mem.host(trig_ea));
    const auto* bitrev_p = reinterpret_cast<const std::int32_t*>(mem.host(bitrev_ea));

    // `in` 只读：拷一份再跑。⚠ 这一步**不是**原地安全的前提（两段旋转读 `in`、写 `out[n/2,n)`；
    //   `mdct_bitreverse` 读 `out[n/2,n)`、写 `out[0,n/2)` ⇒ 区段本来就不重叠，见 native_mdct.h）；
    //   它只是把纯核的 `in` 钉成"绝对只读"，免得将来改动在别处引入别名假设。
    const std::vector<float> in_copy(in_p, in_p + n2);
    mdctBackwardHost(n, log2n, trig_p, bitrev_p, in_copy.data(), out_p);

    // `EAX` 登记为 0：guest 结束时 EAX 是 `out` 派生指针的残留值（Hex-Rays 因此把返回类型猜成
    // `float *`），但**唯一调用点** 0x4E2EAA 之后不读它（`inc esi; add esp,0Ch; cmp; jl`）⇒ 按 0 写。
    *result = 0;
    return true;
}

}  // namespace lift
