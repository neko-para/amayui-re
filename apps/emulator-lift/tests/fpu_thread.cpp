// emulator-lift · **pure**：x87 的异常位"挂账"必须**每线程一份**（CTest: fpu-thread-flags）
//
// 红了意味着什么（判据）：
//   ① 单线程 sanity：两条臂各自真的置了**自己的**那一位、且没有另一位
//      （A = `x87Div(1, 0)` ⇒ `kX87ZE`；B = `x87Sqrt(-1)` ⇒ `kX87IE`）。
//      先钉这一条，是为了让 ②③ 的失败只可能来自**串扰**，而不是"op 本来就不置位"。
//   ② 两条线程逐轮 `std::barrier`（C++20）**强制交错**：
//        `arrive_and_wait()`（对齐开始）→ `x87ClearExceptionFlags()` → 自己的 op →
//        `arrive_and_wait()`（两边都**已置位**）→ `x87TakeExceptionFlags()` →
//        `arrive_and_wait()`（两边都读完，再进下一轮）。
//      A 取到的标志里**必须仍有** `kX87ZE`（`missing_own == 0`）——共享状态下另一条线程的
//      `clear` 会把它偷走。
//   ③ A 取到的标志里**不许有** `kX87IE`（`foreign == 0`）——共享状态下 B 的位会串进来。
//      B 线程对 ②③ 对称（自己的 = IE，别人的 = ZE）。
//   ④ 两条线程各自跑满 N = 20000 轮（`rounds == kRounds`）——防止"循环没跑"被读成绿。
//   ⇒ 把 `fpu_bridge.cpp` 里那份 `thread_local std::uint8_t g_x87_flags` 的 `thread_local`
//     删成普通全局（进程级共享）⇒ barrier ② 之后两家的位**同时在场**：第一个 `take` 读到对方
//     的位 ⇒ `foreign > 0`，第二个 `take` 读到 0 ⇒ `missing_own > 0` —— **必然红**
//     （不靠"2 万轮撞上竞态"）。反过来，TLS 态下两条流的状态互不可见 ⇒ **确定绿**。
//     （`tools/mutate-check.mjs` 的 `ctest:fpu-thread-flags` 就施加这一处。）
//
// ★ 为什么这是**正确性**而不是优化：音频 feeder 是第二条 guest 执行流（`src/runtime/audio.cpp`
//   起自己的 `X86`），而异常位是"运算置位、读状态字取走"的读改写通道 ⇒ 共享会互相偷 / 污染
//   x87 状态字的异常位（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`）。
// ★ **D49 之后这条守卫仍然成立**：x87 换成 `double` 模型，但"异常位挂账"这条通道还在
//   （`fpu_bridge.cpp` 的 `thread_local`），只是**只有 IE / ZE / OE 会被置**（PE / UE 不建模）。
//   ⇒ 本用例的两条 op 相应改成 `1/0`（ZE）与 `sqrt(-1)`（IE）；旧口径用的 `1/3`（PE）在
//     double 模型里不再置位（D49 允许的偏差，见 `fpu_bridge.h` 文件头 §5）。
// ★ 不依赖 guest / Machine / 游戏安装 / LFS：只调 `runtime/fpu_bridge.h` 的公开面 ⇒ 顶层注册 = pure。
// ★ 断言一律回**主线程**做：`lift_test::g_checks` / `g_failures` 是普通 int，从两条线程里 ++ 是
//   数据竞争（UB，且计数会丢）⇒ worker 只**记**读数，主线程判断。
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "check.h"
#include "runtime/fpu_bridge.h"

namespace {

// 轮数：barrier 让"每轮是否交错"变成确定的，所以这个数只决定"跨轮有没有别的机会"，
// 不影响红的必然性（20000 轮在本机只有几十毫秒）。
constexpr int kRounds = 20000;

// 一条线程的**读数**（不在这里断言，见文件头最后一条）。
struct RoundStats {
    std::uint32_t rounds = 0;
    std::uint32_t missing_own = 0;  // 自己那一位**没**取到
    std::uint32_t foreign = 0;      // 取到了**别人的**位
    bool recorded = false;          // 只记第一次失败时的 f
    std::uint8_t first_bad = 0;
};

// `ze == true` ⇒ A 臂（1 / 0）；`false` ⇒ B 臂（sqrt(-1)）。
// ★ 三条 `arrive_and_wait` 把"两边都已置位"与"两边都读完"变成**同步边**：共享态下不可能出现
//   "一条线程从头跑到尾、另一条还没开始"那种不交错的情形 ⇒ 变异必然被抓住。
void runArm(std::barrier<>& sync, bool ze, RoundStats& s) {
    const std::uint8_t own = ze ? lift::kX87ZE : lift::kX87IE;
    const std::uint8_t other = ze ? lift::kX87IE : lift::kX87ZE;

    for (int i = 0; i < kRounds; ++i) {
        sync.arrive_and_wait();          // ① 对齐开始（两边都进本轮了）
        lift::x87ClearExceptionFlags();  // 每轮从干净状态开始：取到的只可能来自本轮
        if (ze) {
            (void)lift::x87Div(1.0, 0.0);
        } else {
            (void)lift::x87Sqrt(-1.0);  // 负数开方 ⇒ 置 invalid
        }
        sync.arrive_and_wait();  // ② 两边都**已置位**（共享态下此刻两个位同时在场）
        const std::uint8_t f = lift::x87TakeExceptionFlags();
        ++s.rounds;
        if ((f & own) == 0) ++s.missing_own;
        if ((f & other) != 0) ++s.foreign;
        if (!s.recorded && (s.missing_own != 0 || s.foreign != 0)) {
            s.recorded = true;
            s.first_bad = f;
        }
        sync.arrive_and_wait();  // ③ 两边都读完，再进下一轮（下一轮的 clear 不会抢跑）
    }
}

// 单线程跑一组 "clear → op → take"（sanity 用）。
std::uint8_t oneShot(bool ze) {
    lift::x87ClearExceptionFlags();
    if (ze) {
        (void)lift::x87Div(1.0, 0.0);
    } else {
        (void)lift::x87Sqrt(-1.0);
    }
    return lift::x87TakeExceptionFlags();
}

// 主线程上的判定：把读数打出来再断言（红的时候能看到"哪条线程、多少轮、第一次是什么 f"）。
void expectClean(const char* who, const RoundStats& s, std::uint32_t expected_rounds) {
    std::printf("fpu-thread-flags: %s rounds=%u missing-own=%u foreign=%u first-bad=0x%02X\n", who,
                s.rounds, s.missing_own, s.foreign, static_cast<unsigned>(s.first_bad));
    LIFT_CHECK_EQ_U32(s.rounds, expected_rounds);
    LIFT_CHECK_EQ_U32(s.missing_own, 0u);
    LIFT_CHECK_EQ_U32(s.foreign, 0u);
}

}  // namespace

int main() {
    using namespace lift;
    using namespace lift_test;

    // ---- ① 单线程 sanity：两条臂各自置位、且不置别人的位 ----
    g_current_case = "fpu-thread-flags/single-thread";
    {
        const std::uint8_t f_div = oneShot(true);
        const std::uint8_t f_sqrt = oneShot(false);
        std::printf("fpu-thread-flags: single-thread div0=%u sqrt(-1)=%u (ze=%d ie=%d)\n",
                    static_cast<unsigned>(f_div), static_cast<unsigned>(f_sqrt),
                    static_cast<int>(kX87ZE), static_cast<int>(kX87IE));
        LIFT_CHECK((f_div & kX87ZE) != 0);
        LIFT_CHECK((f_div & kX87IE) == 0);
        LIFT_CHECK((f_sqrt & kX87IE) != 0);
        LIFT_CHECK((f_sqrt & kX87ZE) == 0);
    }

    // ---- ②③ 两条线程逐轮 barrier 强制交错，各 20000 轮 ----
    // ★ 参与者只有两条 worker（主线程不参与）；两个 `join()` 在最后，barrier ③ 之后不会再有人等。
    std::barrier<> sync(2);
    RoundStats a;
    RoundStats b;
    std::thread ta([&] { runArm(sync, true, a); });
    std::thread tb([&] { runArm(sync, false, b); });
    ta.join();
    tb.join();

    g_current_case = "fpu-thread-flags/ze-thread-only";
    expectClean("A(1/0)", a, static_cast<std::uint32_t>(kRounds));
    g_current_case = "fpu-thread-flags/invalid-thread-only";
    expectClean("B(sqrt -1)", b, static_cast<std::uint32_t>(kRounds));

    return finish("fpu-thread-flags");
}
