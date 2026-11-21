// emulator-lift · 插件回调预算的**唯一**实现（口径见 `budget.h` 的头注）。
//
// ★ 为什么单独一个 TU（而不是 inline 进头）：这是**两把钟**的唯一来源 —— `pluginNowNs()`
//   （宿主墙钟）与 `pluginThreadCpuNs()`（用户 JS 的 CPU 时间）的实现都只此一份。
//   放进头里会让每个 include 的 TU 各有一份包装（可内联，但"哪份钟"会变成每个 TU 各说一遍）。
// ★ 两把钟的分工（⛔ 别混，见 `budget.h` 头注）：用户 JS 的 2 ms 用 CPU 钟；宿主安全网 20 ms 用墙钟。
#include "runtime/budget.h"

#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>  // clock_gettime / CLOCK_THREAD_CPUTIME_ID
#endif

namespace lift::plugin {

std::uint64_t pluginNowNs() {
    using Clock = std::chrono::steady_clock;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(ns.count());
}

#if defined(_WIN32)
namespace {

// 本线程的周期计数（只在本线程**真的在跑**时前进 ⇒ 抢占 / 睡眠不计）。0 = 不可用。
std::uint64_t threadCycles() {
    unsigned long long cycles = 0;
    if (::QueryThreadCycleTime(::GetCurrentThread(), &cycles) == 0) return 0;
    return static_cast<std::uint64_t>(cycles);
}

// `cycles → ns` 的换算系数：4 个 ~200 µs 的忙等窗口，取 `max(cycles / wall)`。
// ★ 抢占只会把 wall 抬高（ratio 变低）⇒ max 是频率的**下界**；误差方向 = CPU 少记
//   （预算偏宽，⛔ 不会误杀）。返回 0 = 标定失败（调用方退回 `GetThreadTimes`）。
// ★ 只在首次使用时跑一次（4 × 200 µs ≈ 0.8 ms）。`script_quickjs.cpp` 的 `call()` 在计时窗之前
//   **预热**它 ⇒ 那 0.8 ms 不进 `js_cpu_ns` 预算窗（第一次回调的 `gap_us` 会看到它）。
double calibrateCyclesPerNs() {
    using Clock = std::chrono::steady_clock;
    double best = 0.0;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t c0 = threadCycles();
        const auto t0 = Clock::now();
        while (std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count() < 200'000) {
        }
        const auto t1 = Clock::now();
        const std::uint64_t c1 = threadCycles();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (ns <= 0 || c1 <= c0) continue;
        const double ratio = static_cast<double>(c1 - c0) / static_cast<double>(ns);
        if (ratio > best) best = ratio;
    }
    return best;
}

// 兜底：`GetThreadTimes` 的 kernel + user（FILETIME 的 100 ns **单位**，⚠ 但**粒度**是整 tick）。
// ⚠ 实测本机（win32 / 2026-11-21）读数只能是 **15.625 ms** 的整数倍 ⇒ 能判"跑飞"、判不了 2 ms
//   （为什么不能当主源，见 `budget.h` 头注）。
std::uint64_t threadTimesNs() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetThreadTimes(::GetCurrentThread(), &creation, &exit, &kernel, &user) == 0) return 0;
    const auto toNs = [](const FILETIME& ft) {
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return static_cast<std::uint64_t>(value.QuadPart) * 100ull;  // FILETIME 单位 = 100 ns
    };
    return toNs(kernel) + toNs(user);
}

}  // namespace
#endif

std::uint64_t pluginThreadCpuNs() {
#if defined(_WIN32)
    // ★ 主源 = 线程周期计数（ns 级）；`GetThreadTimes` 只在它不可用时兜底。
    // ★ **标定必须在读 `cycles` 之前**：标定本身烧 ~0.8 ms CPU。若先读 cycles 再标定，
    //   第一次调用返回的是"标定前"的周期数 ⇒ 那一格的差值会**凭空多出 0.8 ms**
    //   （实测：修这个之前 `start` 的 js_cpu_us ≈ 914，修完后 ≈ 148）。
    static const double cycles_per_ns = calibrateCyclesPerNs();
    if (cycles_per_ns > 0.0) {
        const std::uint64_t cycles = threadCycles();
        if (cycles != 0) {
            return static_cast<std::uint64_t>(static_cast<double>(cycles) / cycles_per_ns);
        }
    }
    return threadTimesNs();
#else
    struct timespec ts {};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;  // 读不到 ⇒ 0（单调不减仍成立）
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

bool budgetExceeded(const Budget& budget, std::uint64_t start_ns, std::uint64_t now_ns) {
    if (budget.deadline_ns == 0) return false;  // 0 = 不限时间
    if (now_ns < start_ns) return false;        // 钟回退 ⇒ 不是"跑飞"
    return now_ns - start_ns >= budget.deadline_ns;
}

std::uint64_t hostSafetyNetNs(const Budget& budget) {
    if (budget.deadline_ns == 0) return 0;  // 不限 ⇒ 安全网也不限（⛔ 不凭空造一个上限）
    constexpr std::uint64_t kMax = ~std::uint64_t{0};
    if (budget.deadline_ns > kMax / kHostSafetyNetFactor) return kMax;  // 饱和：不溢出成一个更小的数
    return budget.deadline_ns * kHostSafetyNetFactor;
}

std::string callbackBudgetReason(const char* call, const std::string& name, bool host) {
    std::string out = call != nullptr && call[0] != '\0' ? call : "callback";
    out += ": callback budget exceeded";
    if (host) out += " (host)";
    if (!name.empty()) {
        out += ": ";
        out += name;
    }
    return out;
}

CallbackDiag& callbackDiag() {
    // ★ 进程级单槽（单线程 guest 循环）；⛔ 不参与判据，见头注。
    static CallbackDiag diag;
    return diag;
}

}  // namespace lift::plugin
