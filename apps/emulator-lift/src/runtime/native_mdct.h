// emulator-lift · native override `sub_4E0B40` = libvorbis `mdct_backward`（FFT/MDCT 内核）
//
// ★ 为什么是它：点击播放语音时 `sub_48D610`（Ogg 读）→ `sub_4D90C0` → `sub_4E2A30`（每包一次）
//   → `sub_4E0B40`（每包一次，`calls=147` / 一个语音）独占 **163–173 ms**（区间 profiler 两次实测，
//   `LIFT_WINDOW_EDGE_TOP=40` 的 `sub_4E2A30 -> sub_4E0B40`；`inclusive`，含它下面的 FFT 链）。同一段里其余大头是 Huffman / 码本
//   解码（`sub_4E4370` 251 ms）与每次起播重建解码器（`sub_4B6570` 219 ms）—— **都不是** FFT。
//
// ★ 逐字依据：**xiph vorbis v1.0.1 `lib/mdct.c`（v1.32）的 `mdct_backward`**。版本判据（`.lst`）：
//   - `sub_4E0030` = `mdct_init`：`[lookup+0]=n`、`[+4]=log2n`（`__CIlog` + `_floor` + `__ftol2_sse`
//     算 `rint(log(n)/log 2)`）、`[+8]=trig`（`cos/sin` 循环）、`[+0Ch]=bitrev`；
//   - `sub_4E08D0` = `mdct_butterflies`：`esi = [lookup+4] - 6` 后 `jle`（= `if(--stages>0)`）、
//     按 `1<<i` 分组、末段 `for(j=0;j<points;j+=32) sub_4E03E0`；
//   - `sub_4E0760` = `mdct_butterfly_generic`（`r0=x1[6]-x2[6]`、`x2[6]=r1*T[1]+r0*T[0]`、
//     `x2[7]=r1*T[0]-r0*T[1]` 那一族；`T += trigint` 四次）；
//   - `sub_4E09D0` = `mdct_bitreverse`（`init@eax`、`x@ecx`；`T = trig+n`）；
//   - `sub_4E0600` / `sub_4E03E0` / `sub_4E02E0` / `sub_4E0230` = `butterfly_first` / `_32` / `_16` / `_8`；
//   - 调用点 `0x4E2EAA`：`push pcm[i]; push pcm[i]; push look->mdct[W]; call sub_4E0B40`
//     ⇒ **`in == out`（原地）**；返回后 `EAX` **不被读**（唯一调用点之后是 `inc esi; add esp,0Ch`）。
//
// ★ 已知偏差（登记，**不是缺陷**）：本实现**不是逐位等价**。
//   偏差**不来自寄存器精度**：执行这条链的上下文是 **feeder 线程**（`AudioEngine` 从 `kSub4B6C30`
//   起跑，并把 override 表拷一份过去；`audio_cpu_.fpu.reset()` 把控制字设回 **0x037F = 80-bit**，
//   与 `mdct/*` 两臂夹具的 bare Machine 缺省一致。CRT 的 `__setdefaultprecision` / `_PC_53`
//   只作用在主线程，被 feeder 的 reset 盖掉）。
//   真因是 **guest 编译后的 float 溢出槽**：`REG_TYPE` 是 `float`，MSVC 在 x87 上把大部分临时量
//   留在寄存器（80-bit），但**一部分溢出到 32-bit 栈槽**（`.lst` 0x4E02F7 / 0x4E0308 / 0x4E0626 的
//   `fstp [ebp+var_*]`，`var_*` 是 `dword ptr`）。本实现用 `double` 走"全程寄存器"那一支、
//   **不复现这些溢出点** ⇒ 每个溢出点的值最多差 1 ulp float（实测 1.0e-7…1.5e-7 相对峰值）。
//   为什么仍选 `double` 而不是 `float`：`float` 会把**所有**中间量都压到 24-bit（比 guest 差一个量级）；
//   `double` 的 53-bit 寄存器段只差 1e-16，远小于溢出槽的 1e-7。⛔ 不要把它归因为 "x87 80-bit vs
//   double"：把夹具精度改成 53-bit 复测，七个用例的 `max|d|` 一个数都不变（主导项是溢出槽，不是位宽）。
//   ⇒ `EAX` 登记为 0：guest 结束时它是 `out` 派生指针的残留（Hex-Rays 因此猜成 `float *`），
//     但唯一调用点不读它；
//   ⇒ 不复刻 x87 的 PE/UE 粘性异常位（调用链上没人读它们，见 `docs/03-emulator/design-notes.md`）；
//   ⇒ 可用 `all,-sub_4E0B40` 隔离。
//
// ★ 五条契约（`native_overrides.h`）逐条：
//   ① 契约可按 EA 核验：入口 EA 0x4E0B40 = `.lst` 的 `mdct_backward` 首指令 `push ebp`；
//   ② 准入零写：全部 9 个回退原因都在**任何写之前**判掉（`MdctWhy`）；
//   ③ 纯核 + 分支覆盖：`mdctBackwardHost` 不碰 guest，逐条照抄源语义（含 `in == out`）；
//   ④ 数值档：`tests/native_override_assets.cpp` 的 `mdct/*` 用例跑**解释器臂 vs native 臂**，
//      逐值比 `max|Δ|`（容差见该文件）；
//   ⑤ 已知偏差登记 + 可隔离（见上）。
#pragma once

#include <cstdint>

namespace lift {

struct X86;
class GuestMemory;

// 纯核：**不做 guest 访存**（测试与 `nativeMdct4E0B40` 共用）。
//   `trig` 长度 = n + n/4；`bitrev` 长度 = n/4；`in` = n/2；`out` = n（**允许 in == out**：两段旋转只读
//   `in`、`mdct_bitreverse` 只读已写过的后半段 ⇒ 与"不同缓冲"逐字节等价，测试钉着这一条）。
//   `n` = 2 的幂、log2n = log2(n)。
//   ★ **`bitrev` 的每个值必须在 `[0, n/2-2]`** —— 取**真表的范围**（`mdct_init` 的 `((~acc)&mask)-1`：
//     偶数项 `[2, n/2-2]`、奇数项 `[0, n/2-4]`，**最大值 `n/2-2` 就在 `[0]`**；夹具表复现同一范围）。
//     `x0 = out + n2 + bit` ⇒ 读 `[n/2, n)` = **已写过的那半段**（这条同时是"原地调用安全"的依据）。
//     原因：纯核用宿主裸指针算 `x0` ⇒ 坏表会变成**宿主野指针**（不是 guest fault，没有页表兜底）。
//     `nativeMdct4E0B40` 在写之前逐项检查（`MdctWhy::BitrevOutOfRange`）。
void mdctBackwardHost(int n, int log2n, const float* trig, const std::int32_t* bitrev, const float* in, float* out);

// 回退原因分桶（值 = `NativeOverride::declines[]` 的下标；必须 < kDeclineSlots）。
enum class MdctWhy : std::uint8_t {
    InitUnreadable = 0,  // `init` 那 16 字节不在已提交页
    SizeOutOfRange = 1,  // n 不在 [64, 8192]
    NotPowerOfTwo = 2,   // n 不是 2 的幂
    Log2Mismatch = 3,    // `[init+4]` != log2(n)（只信自己算的，防越界）
    InUnreadable = 4,    // `in` 的 n/2 个 float 不可读
    OutUnreadable = 5,   // `out` 的 n 个 float 不可写
    TrigUnreadable = 6,  // `trig` 的 n + n/4 个 float 不可读
    BitrevUnreadable = 7,   // `bitrev` 的 n/4 个 int32 不可读
    BitrevOutOfRange = 8,   // `bitrev` 里有值不在 [0, n/2-2] ⇒ 要么 `x0 = x + bit[k]` 越界（宿主野指针），要么读未写的半段
};
// ★ guest 入口 `nativeMdct4E0B40` 的声明在 `native_overrides.h`（与其它实现一致，避免两份声明）。

}  // namespace lift
