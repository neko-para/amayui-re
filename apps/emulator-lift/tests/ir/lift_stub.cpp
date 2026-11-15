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

// ★ 2026-11-15：`tests/lift_ir_proto.cpp` 新增的两条**寄存器计数移位**差分 case
//   （`0x41880A` / `0x4C6E64`）也在 `LIFT_IR_CASE_GEN` 下引用同名入口 —— 而缺 clang 时
//   `LIFT_IR_CASE_GEN` 仍是定义的（它由 CMake 无条件给 `lift-ir-gen-*`）⇒ 这里必须有同名符号，
//   否则那两条 target 的**链接**会断（被测的 `.ll` 不在，跑不到这一段：测试体先 LIFT_SKIP(77)）。
extern "C" std::uint32_t lift_cc_41880A(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_4C6E64(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_4E4B98(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}

// ★ 2026-11-15 · S3-b：条件融合（`cmp`/`test` + `jcc`）的六条差分 case 的入口。理由同上：
//   缺 clang 时 `LIFT_IR_CASE_GEN` 仍然有定义（CMake 无条件给 `lift-ir-gen-*`）⇒ 必须有同名符号。
extern "C" std::uint32_t lift_cc_47A7F3(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_49EE49(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_404DEA(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_4200F7(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_4DD199(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_4D19D3(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}

// ★ 2026-11-15 · S3-c：`div`/`idiv` 与 flags 的两条 oracle（`0x48D500` / `0x419F0D`，见 §2d）。
//   理由同上：缺 clang 时 `LIFT_IR_CASE_GEN` 仍然有定义 ⇒ 必须有同名符号，否则链接断。
extern "C" std::uint32_t lift_cc_48D500(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
extern "C" std::uint32_t lift_cc_419F0D(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
