// emulator-lift · **pure**：消费方 TU 必须拿到**真 TLS** 的 SoftFloat 状态（CTest: softfloat-tls）
//
// 红了意味着什么（判据）—— 下面三条**同时**要成立，任何一条不成立 ⇒ 退出码非 0：
//   ① **前置 include 生效**：本 TU 在**任何 include 之前**就看得见 `THREAD_LOCAL` / `LITTLEENDIAN`
//      —— 这两个宏只有 `platform.h` 定义；`softfloat.h` 只在"**没定义**"时才给出**空的**兜底
//      （`source/include/softfloat.h:53-54`）。此刻两者都还没被任何头定义过 ⇒ 只可能来自
//      消费者的**前置** include（`third_party/berkeley-softfloat/CMakeLists.txt` 的
//      `target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)`；非 MSVC 臂是
//      `-include platform.h`）。
//   ② **同一个对象**：`softfloat_raiseFlags()` 是 `source/softfloat_state.c` 里以真 TLS 编出来的；
//      本 TU **自己读** `softfloat_exceptionFlags` 必须读到它置的位。漏前置 include 时实测
//      这里是 `1 → 0`：编译 + 链接都过，库写的位在本 TU 里**看不见**（本 TU 读的是 TLS
//      **模板**里的静态字节，库写的是**每线程 TLS 块**）。本单的起因就是这一类：
//      复审在 macOS 上实测读到 `flags=248`（`REQ-01M773EFSYYE97YKEA6TQEQ53J`）。
//   ③ **每线程一份**：两条线程各写 `softfloat_exceptionFlags` / `softfloat_roundingMode` 的
//      **不同**值，用 `std::barrier` 把"两边都写完"变成**同步边**之后各自读回，必须读到
//      **自己的**值；且 `&softfloat_exceptionFlags` / `&softfloat_roundingMode` 在两条线程里
//      **不同**。共享态下（`THREAD_LOCAL` 退回空）两条线程读写的是同一份内存：最后写入者赢
//      ⇒ 读到的值**不可能同时**等于两个不同的期望值 ⇒ 至少一条断言必红，与调度无关。
//
// ★ 本 TU **不许** include `platform.h` —— 那正是要守的场景（"下一个 include 就会中"）：
//   本文件的第一条 include 就是 `<softfloat.h>`。上面那几行 `#if defined(...)` 是**预处理期读数**
//   （不 include 任何头），它必须待在第一条 include 之前才读得到"前置 include 有没有先跑"。
// ★ 实测（2026-11-14，windows-x64 / clang-cl 22.1.2，RelWithDebInfo）：
//   · 无前置 include：链接**成功**，`softfloat_raiseFlags(inexact)` 之后本 TU 读到 `flags=0`；
//   · 有 `/FIplatform.h`：同上读到 `flags=1`。
//     两次都是真跑出来的读数 —— ⛔ 不是"看起来应该没问题"。
// ★ 为什么断言一律回**主线程**做：`lift_test::g_checks` / `g_failures` 是普通 int，从两条线程里
//   ++ 是数据竞争（UB，计数会丢）⇒ worker 只**记**读数，主线程判断。
// ★ 不依赖 guest / Machine / 游戏安装 / LFS ⇒ 顶层无条件注册 = **pure** 档。
// ★ 它守的那条 CMake 改动：把 `INTERFACE` 改成 `PRIVATE`（= 消费者拿不到前置 include）
//   ⇒ 本用例红（`tools/mutate-check.mjs` 的 `ctest:softfloat-tls` 施加这一处）。

// ── 预处理期读数（★ 必须放在**任何 include 之前**）────────────────────────────────────
// ⛔ 不要写成编译期 `#error`：构建失败在 `tools/mutate-check.mjs` 里算"**不可判定**"，
//   只有**运行时**断言才算"守卫红了"。所以这里只把状态记成常量，判在 main 里。
#if defined(THREAD_LOCAL) && defined(LITTLEENDIAN)
#define LIFT_SF_SAW_PLATFORM_H 1
#else
#define LIFT_SF_SAW_PLATFORM_H 0
#endif

#define LIFT_SF_STR_1(x) #x
#define LIFT_SF_STR(x) LIFT_SF_STR_1(x)
#if defined(THREAD_LOCAL)
static const char* const kThreadLocalSpelling = LIFT_SF_STR(THREAD_LOCAL);
#else
static const char* const kThreadLocalSpelling = "(未定义：platform.h 没有被前置 include)";
#endif

// ★ 本 TU 的**第一条 include**（⛔ 不 include platform.h）：漏前置 include 的那条路径就长这样。
//   SoftFloat 的头是 C 头（没有 `extern "C"` 守卫）⇒ C++ 侧自己包一层，
//   与 `src/runtime/fpu_bridge.cpp` / `tests/fpu_thread.cpp` 同形。
extern "C" {
#include <softfloat.h>
}

#include <barrier>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "check.h"

namespace {

// 一条线程的**读数**（不在这里断言，见文件头最后一条）。
struct ThreadReadout {
    const void* flags_addr = nullptr;       // &softfloat_exceptionFlags
    const void* rounding_addr = nullptr;    // &softfloat_roundingMode
    unsigned initial_flags = 0;             // 进本线程时读到的值（打印用）
    unsigned via_library = 0;               // ② 库 `softfloat_raiseFlags()` 置的位，本 TU 读不读得到
    unsigned flags_after = 0;               // ③ barrier 之后读回
    unsigned rounding_after = 0;
};

// ★ 三条 `arrive_and_wait` 把"两边都**已写完**"与"两边都**已读完**"变成**同步边**：
//   共享态下不可能出现"一条线程从头跑到尾、另一条还没开始"那种不交错的情形 ⇒ 变异必被抓。
void runArm(std::barrier<>& sync, std::uint8_t own_flags, std::uint8_t own_rounding,
            ThreadReadout& r) {
    r.initial_flags = softfloat_exceptionFlags;

    // ---- ② 先清零，再让**库**置位，最后由本 TU 读 ----
    // 两步访问必须落到**同一个** TLS 对象上；漏前置 include 时本 TU 的访问不是 TLS 访问 ⇒ 读回 0。
    softfloat_exceptionFlags = 0;
    softfloat_raiseFlags(softfloat_flag_inexact);
    r.via_library = softfloat_exceptionFlags;

    // ---- ③ 两条线程写**不同**的值，同步之后各读各的 ----
    sync.arrive_and_wait();  // ① 两边都进本阶段
    softfloat_exceptionFlags = own_flags;
    softfloat_roundingMode = own_rounding;
    sync.arrive_and_wait();  // ② 两边都**已写完**（共享态下此刻只有一个最终值）
    r.flags_after = softfloat_exceptionFlags;
    r.rounding_after = softfloat_roundingMode;
    r.flags_addr = &softfloat_exceptionFlags;
    r.rounding_addr = &softfloat_roundingMode;
    sync.arrive_and_wait();  // ③ 两边都读完，再让任何一条退出
}

// 两条线程写的值：**互不相同**、且都不等于 ② 里用到的 `softfloat_flag_*` 位
// （共享态下"最后写入者赢"⇒ 两条断言里至少一条必失配）。
constexpr std::uint8_t kFlagsA = 0xA1;
constexpr std::uint8_t kFlagsB = 0x5C;
constexpr std::uint8_t kRoundingA = 2;  // softfloat_round_min
constexpr std::uint8_t kRoundingB = 3;  // softfloat_round_max

void printReadout(const char* who, const ThreadReadout& r) {
    std::printf(
        "softfloat-tls: %s initial-flags=%u via-library=%u flags=%u rounding=%u "
        "flags-addr=%p rounding-addr=%p\n",
        who, r.initial_flags, r.via_library, r.flags_after, r.rounding_after, r.flags_addr,
        r.rounding_addr);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---- ① 前置 include 真的先跑了 ----
    g_current_case = "softfloat-tls/platform-h-force-included";
    std::printf(
        "softfloat-tls: THREAD_LOCAL before any include = %s ; saw platform.h = %d ; "
        "sizeof(uint_fast8_t)=%d\n",
        kThreadLocalSpelling, LIFT_SF_SAW_PLATFORM_H, static_cast<int>(sizeof(uint_fast8_t)));
    LIFT_CHECK_EQ_U32(LIFT_SF_SAW_PLATFORM_H, 1u);

    // ---- ②③ 两条线程 ----
    std::barrier<> sync(2);
    ThreadReadout a;
    ThreadReadout b;
    std::thread ta([&] { runArm(sync, kFlagsA, kRoundingA, a); });
    std::thread tb([&] { runArm(sync, kFlagsB, kRoundingB, b); });
    ta.join();
    tb.join();
    printReadout("A", a);
    printReadout("B", b);

    // ② 库写的位，本 TU 必须读得到（同一个对象）。
    g_current_case = "softfloat-tls/library-flag-visible";
    const std::uint32_t one = static_cast<std::uint32_t>(softfloat_flag_inexact);
    LIFT_CHECK_EQ_U32(a.via_library, one);
    LIFT_CHECK_EQ_U32(b.via_library, one);

    // ③ 每线程一份：各读各的值。
    g_current_case = "softfloat-tls/per-thread-exception-flags";
    LIFT_CHECK_EQ_U32(a.flags_after, static_cast<std::uint32_t>(kFlagsA));
    LIFT_CHECK_EQ_U32(b.flags_after, static_cast<std::uint32_t>(kFlagsB));

    g_current_case = "softfloat-tls/per-thread-rounding-mode";
    LIFT_CHECK_EQ_U32(a.rounding_after, static_cast<std::uint32_t>(kRoundingA));
    LIFT_CHECK_EQ_U32(b.rounding_after, static_cast<std::uint32_t>(kRoundingB));

    // ③ 地址必须不同（同一份内存 ⇒ 同一个地址）。
    g_current_case = "softfloat-tls/distinct-tls-address";
    LIFT_CHECK(a.flags_addr != b.flags_addr);
    LIFT_CHECK(a.rounding_addr != b.rounding_addr);
    LIFT_CHECK(a.flags_addr != nullptr);
    LIFT_CHECK(b.flags_addr != nullptr);

    return finish("softfloat-tls");
}
