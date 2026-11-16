// emulator-lift · guest 退出码 → **宿主进程**退出码（唯一一份映射）
//
// ★ 为什么单独一个头：**两条臂都要用同一条映射**，而它们的 include 面不同 ——
//   · 解释器臂：`main.cpp` 读 `machine.guestExitCode()`（= `cpu_.shim.exit_code`）；
//   · AOT 臂：`aot_main.cpp` 读 `host.cpu().shim.exit_code`。
//   但 `aot_main.cpp` / `aot_host.h` 明令 **⛔ 不 include `machine.h`**（那里带着 `Machine` 的
//   完整定义 + 整棵内核包含树）。⇒ 把这条纯映射搬到这里：只含 `constexpr` 纯函数，
//   两臂各 include 本文件，⛔ 不把 `Machine` 拖进 AOT。
//   ★ 映射只有一份（不是"两边各写一遍、靠人保持同步"）：`tests/guest_exit.cpp` 直接钉它。
//
// n == 0 ⇒ 0（正常收尾）；n != 0 ⇒ 2（与 decode-error 同档：这是"引擎自己判定失败"）。
// ⛔ 不把 guest 的任意 n 直接当宿主退出码：主机的退出码语义是 0..255 的 shell 约定，
//   直接透传会让 `exit(259)` 之类的值被截断 / 误读成别的状态（真机上 Windows 会截到 8 位、
//   POSIX 一样只留低 8 位）⇒ 这里只回答"正常 / 非正常"这一位；原值仍在 guest 可观测的
//   `cpu.shim.exit_code` 与 warn 日志里（具体数值不丢）。
//
// ★ 位置约束：这是调用方（`main` / `aot_main` 的最终 `return`）**最后**一道分流；
//   run 本身失败（`exitCodeForRun` ⇒ 2）与消费者闸门（4 / 5）都**优先**。
#pragma once

#include <cstdint>

namespace lift {

// `constexpr` 而不是普通 `inline`：纯映射不依赖状态，将来要编译期用也拿得到；
// 但**守卫**（`tests/guest_exit.cpp`）故意走运行期断言 —— 变异自检要的是"改坏 ⇒ 用例红"，
// 编译期断言会把变异表现成构建失败（在 `tools/mutate-check.mjs` 里是"不可判定"）。
inline constexpr int exitCodeForGuestExit(std::uint32_t guest_exit_code) noexcept {
    return guest_exit_code == 0u ? 0 : 2;
}

}  // namespace lift
