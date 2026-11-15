// emulator-lift · **生成代码契约**：x87 状态在 `X86` 里的字节偏移（唯一一份常量）
//
// ★ 为什么要这个文件：D49 之后 x87 的实数状态在 `X86`（`%cpu`）里，而 AOT 发射器
//   （`src/aot/lift_gen_emit.cpp`）**只拿到一个 `ptr %cpu`**，它必须把 `top` / 8 个 double 槽 /
//   状态字 / 控制字 / tag word 的偏移算成 `getelementptr i8, ptr %cpu, i64 <常量>`。
//   ⛔ 这些数字**不许**散落在发射器各处（一处写错 = 静默读写错字段，而且只有真跑才看得出来）。
//
// ★ 判据在 `src/runtime/lift_abi.cpp`：那里有一组 `static_assert(offsetof(...))`，把下面每个
//   常量与真实的 `offsetof(X86, fpu) + offsetof(X87State, …)` 对齐。⇒ **改了 `x86.h` 的
//   `X87State` 字段顺序 / 类型，这里不同步就构建红**（C++ 编译期，不是运行期）。
//
// ★ 为什么是**字面量**而不是 `offsetof` 表达式：发射器是独立 TU（`emulator-lift-gen`），
//   让它只依赖这份 20 行的头，而不是把 `x86.h` 的整条 include 链（memory / shims / ……）拖进去。
//   字面量 + `lift_abi.cpp` 的断言 = "显式契约"，比"自动推导"更能暴露"改了却没同步"这件事
//   （自动推导会让改动静默通过）。
//
// ⚠ 脆弱点（如实记账）：常量是手写的 ⇒ 只有在 `lift_abi.cpp` 被编译进当前构建时才有保护。
//   如果哪天有人把 `lift_abi.cpp` 从两个臂里拿掉（或给它套 `#if`），这些断言会一起消失，
//   而生成物仍然"能跑"（读到错的字段，语义静默错）。⇒ `lift_abi.cpp` 必须始终被编译。
#pragma once

#include <cstdint>

namespace lift {
namespace x87layout {

// `offsetof(X86, fpu) + offsetof(X87State, regs)` —— 8 个 double 槽的起点（每个 8 字节）
constexpr std::uint64_t kRegs = 64;
// `… + offsetof(X87State, control)` —— 控制字（u16）
constexpr std::uint64_t kControl = 128;
// `… + offsetof(X87State, status)` —— 状态字（u16；C0..C3 / 异常位 / TOP 镜像）
// ★ 生成代码直接读写这一格 ⇒ **异常位的语义也是契约**（任务 0 / D55）：
//   IE=0x0001 · ZE=0x0004 · OE=0x0008 · UE=0x0010 · **PE=0x0020 恒 0** · ES=0x0080（= 任一异常位非零）
//   ⇒ **任何按 PE 分支的 guest 路径在生成代码里都走"精确"那一支**（`double` 模型判不出 inexact，
//   见 `fpu_bridge.h` 文件头 §5；⛔ 不为此把每条 x87 变回宿主调用）。
constexpr std::uint64_t kStatus = 130;
// `… + offsetof(X87State, top)` —— 栈顶索引（u8，0..7）
constexpr std::uint64_t kTop = 132;
// `… + offsetof(X87State, tag)` —— tag word（u16，每槽 2 位）
constexpr std::uint64_t kTag = 134;
// 一个 double 槽的宽度（`sizeof(double)`）
constexpr std::uint64_t kSlotSize = 8;

// ---- 2026-11-15（本轮：`lift_one` 调用点 → 0）：`fs:`/`gs:` 与 SSE 状态也在 `X86` 里 ----
// ★ 为什么能进这一份契约：它们和 x87 状态同一条理由 —— 生成代码只有一个 `ptr %cpu`，
//   而这些字段的偏移必须是**唯一一份常量**（散落在发射器里 = 静默读写错字段）。
// ★ 语义真源：`x86.cpp:68-75`（`X86::eaOf` 的 `ea += fs_base/gs_base`）、
//   `exec_fpu.cpp:1020-1125`（`execSse` 直接读写 `cpu.xmm` / `cpu.mxcsr`）。
constexpr std::uint64_t kFsBase = 52;   // `offsetof(X86, fs_base)`（u32）
constexpr std::uint64_t kGsBase = 56;   // `offsetof(X86, gs_base)`（u32）
constexpr std::uint64_t kXmm = 136;     // `offsetof(X86, xmm)`（8 × 16B）
constexpr std::uint64_t kXmmStride = 16;
constexpr std::uint64_t kMxcsr = 264;   // `offsetof(X86, mxcsr)`（u32）
// `offsetof(X86, seg)` —— 6 个段选择子（u16：ES CS SS DS FS GS）。
// ★ 语义真源：`x86.cpp:31`（读：`return seg[spec.index]`）、`:55`（写：`seg[i] = value & 0xFFFF`）
constexpr std::uint64_t kSeg = 40;
constexpr std::uint64_t kSegStride = 2;

}  // namespace x87layout
}  // namespace lift
