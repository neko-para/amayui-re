// emulator-lift · 插件回调预算的**唯一**实现（口径见 `budget.h` 的头注）。
//
// ★ 为什么单独一个 TU（而不是 inline 进头）：这是**钟**的唯一来源 —— `pluginNowNs()` 的实现只此一份。
//   放进头里会让每个 include 的 TU 各有一份 `steady_clock::now()` 包装（可内联，但"哪份钟"会变成
//   每个 TU 各说一遍）；搬成 TU 之后只有 `trigger_runner.cpp` 一处调它。
#include "runtime/budget.h"

#include <chrono>
#include <cstdint>

namespace lift::plugin {

std::uint64_t pluginNowNs() {
    using Clock = std::chrono::steady_clock;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(ns.count());
}

bool budgetExceeded(const Budget& budget, std::uint64_t start_ns, std::uint64_t now_ns) {
    if (budget.deadline_ns == 0) return false;  // 0 = 不限墙钟
    if (now_ns < start_ns) return false;        // 钟回退 ⇒ 不是"跑飞"
    return now_ns - start_ns >= budget.deadline_ns;
}

}  // namespace lift::plugin
