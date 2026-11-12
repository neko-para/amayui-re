// emulator-lift · **pure**：SoftFloat 的 x87 异常位必须**每线程一份**（CTest: fpu-thread-flags）
//
// 红了意味着什么（判据）：
//   ① 单线程 sanity：两条臂各自真的置了**自己的**那一位、且没有另一位
//      （A = `x87Div(1,3)` ⇒ `softfloat_flag_inexact`；B = `x87Sqrt(-1)` ⇒ `softfloat_flag_invalid`）。
//      先钉这一条，是为了让 ②③ 的失败只可能来自**串扰**，而不是"op 本来就不置位"。
//   ② 两条线程逐轮 `std::barrier`（C++20）**强制交错**：
//        `arrive_and_wait()`（对齐开始）→ `x87ClearExceptionFlags()` → 自己的 op →
//        `arrive_and_wait()`（两边都**已置位**）→ `x87TakeExceptionFlags()` →
//        `arrive_and_wait()`（两边都读完，再进下一轮）。
//      A 取到的标志里**必须仍有** `softfloat_flag_inexact`（`missing_own == 0`）——
//      共享状态下另一条线程的 `clear` 会把它偷走。
//   ③ A 取到的标志里**不许有** `softfloat_flag_invalid`（`foreign == 0`）——
//      共享状态下 B 的位会串进来。B 线程对 ②③ 对称（自己的 = invalid，别人的 = inexact）。
//   ④ 两条线程各自跑满 N = 20000 轮（`rounds == kRounds`）——防止"循环没跑"被读成绿。
//   ⇒ 把 `third_party/berkeley-softfloat/platform.h` 的 `THREAD_LOCAL` 改回空（进程级共享）⇒
//     barrier ② 之后两家的位**同时在场**：第一个 `take` 读到对方的位 ⇒ `foreign > 0`，
//     第二个 `take` 读到 0 ⇒ `missing_own > 0` —— **必然红**（不靠"2 万轮撞上竞态"）。
//     反过来，TLS 态下两条流的状态互不可见 ⇒ **确定绿**，与调度无关。
//     （`tools/mutate-check.mjs` 的 `ctest:fpu-thread-flags` 就施加这一处；复审实测旧的"只堆轮数"
//     版本在同一变异二进制上连跑 10 次只有 9 次红 —— 那 ~10% 假绿正是 barrier 要消灭的。）
//
// ★ 为什么这是**正确性**而不是优化：音频 feeder 是第二条 guest 执行流（`src/runtime/audio.cpp`
//   起自己的 `X86`），而 `softfloat_exceptionFlags` 是模块级变量、`x87TakeExceptionFlags()` 是
//   "读 + 清"的读改写（`src/runtime/fpu_bridge.cpp`）⇒ 共享会互相偷 / 污染 x87 状态字的 PE 位，
//   PCM 段的"逐位一致"就不成立（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`）。
// ★ 不依赖 guest / Machine / 游戏安装 / LFS：只调 `runtime/fpu_bridge.h` 的公开面 ⇒ 顶层注册 = pure。
// ★ 断言一律回**主线程**做：`lift_test::g_checks` / `g_failures` 是普通 int，从两条线程里 ++ 是
//   数据竞争（UB，且计数会丢）⇒ worker 只**记**读数，主线程判断。
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "check.h"
#include "runtime/fpu_bridge.h"

// SoftFloat 的异常位常量只在 `softfloat.h` 里；C++ 侧必须走 `extern "C"`，并且与
// `src/runtime/fpu_bridge.cpp` 一样**先 include `platform.h`**（`LITTLEENDIAN` 参与
// `extFloat80_t` 的字段顺序；`softfloat.h` 自带的"THREAD_LOCAL 空"兜底会让漏 include 静默读垃圾）。
extern "C" {
#include "platform.h"
#include "softfloat.h"
}

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

// `inexact == true` ⇒ A 臂（1 / 3）；`false` ⇒ B 臂（sqrt(-1)）。
// ★ 三条 `arrive_and_wait` 把"两边都已置位"与"两边都读完"变成**同步边**：共享态下不可能出现
//   "一条线程从头跑到尾、另一条还没开始"那种不交错的情形 ⇒ 变异必然被抓住。
void runArm(std::barrier<>& sync, bool inexact, RoundStats& s) {
    lift::Ext80 a{};
    lift::Ext80 b{};
    lift::Ext80 out{};
    if (inexact) {
        lift::x87FromI32(1, a);
        lift::x87FromI32(3, b);
    } else {
        lift::x87FromI32(-1, a);  // sqrt(负数) ⇒ SoftFloat 置 invalid 并给 default NaN
    }
    const std::uint8_t own =
        static_cast<std::uint8_t>(inexact ? softfloat_flag_inexact : softfloat_flag_invalid);
    const std::uint8_t other =
        static_cast<std::uint8_t>(inexact ? softfloat_flag_invalid : softfloat_flag_inexact);

    for (int i = 0; i < kRounds; ++i) {
        sync.arrive_and_wait();          // ① 对齐开始（两边都进本轮了）
        lift::x87ClearExceptionFlags();  // 每轮从干净状态开始：取到的只可能来自本轮
        if (inexact) {
            lift::x87Div(a, b, out);
        } else {
            lift::x87Sqrt(a, out);
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
std::uint8_t oneShot(bool inexact) {
    lift::Ext80 a{};
    lift::Ext80 b{};
    lift::Ext80 out{};
    lift::x87ClearExceptionFlags();
    if (inexact) {
        lift::x87FromI32(1, a);
        lift::x87FromI32(3, b);
        lift::x87Div(a, b, out);
    } else {
        lift::x87FromI32(-1, a);
        lift::x87Sqrt(a, out);
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
        std::printf("fpu-thread-flags: single-thread div=%u sqrt=%u (inexact=%d invalid=%d)\n",
                    static_cast<unsigned>(f_div), static_cast<unsigned>(f_sqrt),
                    static_cast<int>(softfloat_flag_inexact),
                    static_cast<int>(softfloat_flag_invalid));
        LIFT_CHECK((f_div & softfloat_flag_inexact) != 0);
        LIFT_CHECK((f_div & softfloat_flag_invalid) == 0);
        LIFT_CHECK((f_sqrt & softfloat_flag_invalid) != 0);
        LIFT_CHECK((f_sqrt & softfloat_flag_inexact) == 0);
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

    g_current_case = "fpu-thread-flags/inexact-thread-only";
    expectClean("A(1/3)", a, static_cast<std::uint32_t>(kRounds));
    g_current_case = "fpu-thread-flags/invalid-thread-only";
    expectClean("B(sqrt -1)", b, static_cast<std::uint32_t>(kRounds));

    return finish("fpu-thread-flags");
}
