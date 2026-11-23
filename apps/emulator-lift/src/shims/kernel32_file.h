// emulator-lift · 文件句柄 → 宿主 `FILE*` 的**只读**旁路（给 winmm mmio 用）
//
// ★ 为什么需要：`sub_4BBEF0`（WAV 头解析）用 `mmioOpenA(0, &pmmioinfo, 0)`，把
//   `pmmioinfo.adwInfo[0]` 设成 `kernel32!CreateFileA` 返回的 **guest 文件句柄**；
//   mmio 必须沿同一个文件指针读 RIFF / fmt / data，不能另开一份文件。
// ★ 只暴露 `FILE*`，不暴露 `FileState`（访问位 / 共享 / 路径仍归 kernel32_file.cpp）；
//   mmio 实现用 `fseek`/`fread`/`ftell`，并在 `mmioClose` 时**不关闭**这个 `FILE*`
//   （guest 句柄的 CloseHandle 才是唯一关闭点）。
#pragma once

#include <cstdint>
#include <cstdio>

namespace lift {

struct X86;

// 有效 `HandleKind::File` ⇒ 它的 `std::FILE*`；否则 nullptr（不打印 / 不 abort，
// 因为 mmio 自己会返回真实失败码）。
std::FILE* liftFileStream(X86& cpu, std::uint32_t handle);

// ★ 诊断（`LIFT_VFS_TIMING=1`）：按**帧**给 `CreateFileA` 的分段计时切组（见 kernel32_file.cpp
//   的 `vfstime` 段）。`frame` = 宿主 Present 的帧号（与 `--profile-slow-frames` 的 `n=` 同一个计数）。
//   关档时是一次静态 bool 判定 ⇒ 默认档零开销。
//   为什么由宿主帧循环调用：shim 里读不到帧号，而"装载帧 vs 点击帧"的对比必须有帧边界；
//   AOT 臂的帧边界唯一落在 `aot_run.cpp` 的 Present 收口处。
void vfsTimingFrameBoundary(std::uint64_t frame);

}  // namespace lift
