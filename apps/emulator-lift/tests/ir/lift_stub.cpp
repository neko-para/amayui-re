// emulator-lift · `tests/ir/lift_436D00.ll` 的**替身 TU**（只在找不到 clang 时编）
//
// ★ 为什么需要它（复审 §3.4）："缺 clang ⇒ 测试体 `LIFT_SKIP(77)`"只覆盖**运行期**。
//   若 `add_executable` 依赖一个"没有 clang 就不存在"的 `.obj`，**链接期**就红了 ——
//   `add_test` 在最外层（pure 档）这条纪律会因此把整棵树弄红。
//   ⇒ 缺 clang 时链接这一份：同名符号返回哨兵；测试体用 `LIFT_IR_CLANG` 判 77。
// ★ ⛔ 它**不是**第二份实现：没有任何语义，只保证"符号在、链接过得去"。
//   （测试体在缺 clang 时第一件事就是 LIFT_SKIP(77)，不会拿它当 lifted 代码跑。）
#include <cstdint>

#include "runtime/lift_abi.h"

extern "C" std::uint32_t lift_cc_436D00(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;  // 哨兵：trampoline 立刻结束（缺 clang 时测试体已 SKIP）
}
