// emulator-lift · guest 退出码 → 宿主进程退出码的**纯映射**守卫（REQ-01M7HP2YRZG222E0NE2H6MW6NK）
//
// 判据（红了意味着什么）：
//   ① `exitCodeForGuestExit(0) == 0`；
//   ② 任意非 0（含 1 / 3 / 127 / 128 / 255 / 256 / 259 / 0x7FFFFFFF / 0xFFFFFFFF）⇒ **2**，
//      且**不等于 n**（⛔ 不透传 guest 的任意值）。
//
// ★ 为什么这条是**单独一个头**（`runtime/guest_exit.h`）的守卫：AOT 臂 `aot_main.cpp` 明令
//   ⛔ 不 include `machine.h`（那里带 `Machine` 的完整定义），却要与解释器臂用**同一条**映射
//   ⇒ 映射必须在只含 `constexpr` 的头里。本文件**故意只 include `runtime/guest_exit.h`**
//   （不 include `machine.h`）——它就是"AOT 臂能拿到这条纯函数而不拖进内核"的编译期证据。
// ★ 判据走**运行期**断言（不是 `static_assert`）：映射被改坏时本用例必须**跑起来并红**，
//   这样 `tools/mutate-check.mjs` 的 `ctest:lift-guest-exit` 才验得到"它会红"；
//   编译期断言会让变异表现成"构建失败"，那在变异自检里是**不可判定**、不是红。
// ★ ⚠ 这是**弱守卫**：AOT 臂的 e2e 进程退出码面结构性测不到（`exit-code` 的夹具是合成 PE，
//   而 AOT 只跑被 pin 的 PE）⇒ 本文件只能钉"映射本身"，钉不到"AOT main 有没有接线"。
//   接线靠代码审阅 + 手工读数（`--max-insns 1` 确认 r.ok() 分支没被改坏）见报告。
#include <cstdint>
#include <initializer_list>

#include "check.h"
#include "runtime/guest_exit.h"

int main() {
    using namespace lift_test;

    // ---- ① 0 ⇒ 0 ----
    g_current_case = "guest-exit/zero";
    LIFT_CHECK_EQ_I64(lift::exitCodeForGuestExit(0u), 0);

    // ---- ② 非 0 ⇒ 2（且不透传 n）----
    g_current_case = "guest-exit/nonzero";
    for (std::uint32_t n : {1u, 3u, 127u, 128u, 255u, 256u, 259u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        const int code = lift::exitCodeForGuestExit(n);
        LIFT_CHECK_EQ_I64(code, 2);                            // 与 decode-error 同档
        LIFT_CHECK(static_cast<std::uint32_t>(code) != n);     // ⛔ 不是透传 n
    }

    return finish("lift-guest-exit");
}
