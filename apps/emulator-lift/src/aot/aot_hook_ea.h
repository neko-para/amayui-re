// emulator-lift · **宿主语义钩子的判定点 EA**（唯一一份；`lift-gen` 与 AOT 执行臂共用）
//
// ★ 为什么单独一个头：这两个常量有两类消费者，而它们的依赖面必须保持最小 ——
//   · AOT 执行臂（`aot_run.cpp` / `aot_host.h`）：在派发边界上做 skip-logo / recovery 的判定；
//   · 构建期生成器（`lift-gen`）：S7 之后控制流在生成代码里**不再逐函数回派发器**
//     ⇒ 若某个钩子 EA 落在一条纯生成代码的路径中间，它就再也见不到 trampoline ⇒
//     **`--skip-logo` / recovery 绕过会静默失效**（那会改变 300/3000 帧的读数与 boot 路径）。
//   生成器把这两个 EA 当作"**必须留在派发边界**"的检查点（`GenStructure::hook_eas`）：
//   指向它们的静态边一律发 `ret i32 <EA>`（一次派发），⛔ 不合并进同一段生成代码。
//   ⚠ 这不是"按 EA 特判语义"：钩子本来就是宿主侧的 EA 键控检查点（与解释器同一口径），
//     这里只是保证它在 S7 之下仍然**看得见**。
#pragma once

#include <cstdint>

namespace lift {
namespace aot {

// `--skip-logo` 的钩子点 = `0x004BAA33` —— `call sub_415640` 的 **resume EA**
// （生成器把每条 call 的下一条 EA 加进入口集 ⇒ 它在派发表里）。
// ⚠ **不能**照抄解释器的 `0x004BAA37`：那只是同一函数内部的块（`bb_4BAA37` 由
// `bb_4BAA33` 的 `jmp short` 直接并入），派发表里**没有**它 ⇒ 挂那里永不命中。
// 两点之间只有一条 `jmp short`（无语义）⇒ 在这里写那一格与解释器在 `0x4BAA37` 写等价，
// 且同样发生在 `0x004BAA3A` 的 `mov ds:dword_55E1BC, eax`（指针发布）**之前**。
constexpr std::uint32_t kSkipLogoHookEa = 0x004BAA33u;

// `--accept-recovery-dialog` 的默认"绕过"判定点（真源 `machine.cpp:47` 的 `kRecoveryCheckEa`）：
// 到达这个 EA 时把 EAX 置 0（一次）。S7 之下同样必须留在派发边界（见文件头）。
constexpr std::uint32_t kRecoveryCheckEa = 0x004BA718u;

// 生成器 / 执行臂共用的检查点集合（升序；⛔ 不要只改一处）。
constexpr std::uint32_t kHostHookEas[] = {kRecoveryCheckEa, kSkipLogoHookEa};

}  // namespace aot
}  // namespace lift
