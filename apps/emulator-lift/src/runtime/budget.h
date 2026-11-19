// emulator-lift · 插件回调的**预算**（墙钟 deadline + 操作数上限）—— C1b
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2 第 458 行（"每次 start/onTrigger deadline + op limit；
//       超限 ⇒ 清 armed + fail(owner)"）、§6 行 12（预算守卫）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §5.7（本头就是那一段的落地）。
//
// ★ 与 D5 同口径：2 ms / 100k ops。两个数**只有一份**（⛔ 不要在别处再写一遍）。
// ★ `op_limit` 本切片**不消费**：C1 的 handler 是 C++（没有"步数"概念）；它的真消费者是 C2 的 QJS
//   （QJS interrupt handler 数 op）。⇒ 这里只声明 + 保留形状，⛔ 不编一个假的 op 计数出来。
// ★ 钟：`pluginNowNs()` = 宿主**单调**纳秒（`steady_clock`）。⛔ 不接 guest 虚拟时钟、⛔ 不扣暂停
//   （预算判的是"宿主被回调占了多久"，与 guest 时间无关；与 `AfterDelay` / timeout 的墙钟同源）。
#pragma once

#include <cstdint>

namespace lift::plugin {

// 一次回调的墙钟上限（D5 裁决：2 ms）。超了 = 回调跑飞（宿主被卡住）。
inline constexpr std::uint64_t kCallbackDeadlineNs = 2'000'000ull;
// 一次回调的操作数上限（D5 裁决：100k）。⚠ C1 不消费（见头注）；C2 的 QJS 才数。
inline constexpr std::uint32_t kCallbackOpLimit = 100'000u;

struct Budget {
    // 0 = **不限墙钟**（只有 op_limit 那一半生效）—— 给"明确要放宽"的调用方一个干净的口子。
    std::uint64_t deadline_ns = kCallbackDeadlineNs;
    std::uint32_t op_limit = kCallbackOpLimit;
};

// 宿主单调纳秒（`std::chrono::steady_clock`；`budget.cpp` 是唯一定义）。
std::uint64_t pluginNowNs();

// 是否超预算。判据 = `now_ns - start_ns >= deadline_ns`（**含等号**：正好到点即超）。
// 钟回退（`now_ns < start_ns`）不算超：那是"读数坏了"，不是"回调跑飞"。
bool budgetExceeded(const Budget& budget, std::uint64_t start_ns, std::uint64_t now_ns);

}  // namespace lift::plugin
