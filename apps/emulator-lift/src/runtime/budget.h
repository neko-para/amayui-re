// emulator-lift · 插件回调的**预算**（墙钟 deadline + 操作数上限）—— C1b
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2 第 458 行（"每次 start/onTrigger deadline + op limit；
//       超限 ⇒ 清 armed + fail(owner)"）、§6 行 12（预算守卫）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §5.7（本头就是那一段的落地）。
//
// ★ 与 D5 同口径：2 ms / 100k ops。两个数**只有一份**（⛔ 不要在别处再写一遍）。
// ★ **2 ms 只罩用户 JS 本体**（2026-11-21 口径收窄；用户裁决 D5 可改）：
//   deadline 的起点 = 组完 ctx / 事件对象、**即将调用插件函数之前**（`script_quickjs.cpp` 的 `call()`）。
//   ⇒ **模块装载**（`call()` 之前）、**ctx / 事件对象构造**、**返回值 → TriggerSet 收集**、宿主段调度
//   **都不计**。理由：这些是宿主替用户代码付的成本；把它们算进预算会让"一次正常的回调"偶发红
//   （缺陷单 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`）。
// ★ **宿主侧另有安全网** = `deadline_ns × kHostSafetyNetFactor`（2 ms ⇒ **20 ms**）：它防的是
//   "宿主段被卡死 ⇒ guest 被拖住"，不是"用户 JS 跑飞" ⇒ 超了照样响亮失败，但 reason 带 `(host)`
//   （与用户 JS 超限的 reason 可区分；拼法只有 `callbackBudgetReason()` 一处）。
// ★ `op_limit` 本切片**不消费**：C1 的 handler 是 C++（没有"步数"概念）；它的真消费者是 C2 的 QJS
//   （QJS interrupt handler 数 op）。⇒ 这里只声明 + 保留形状，⛔ 不编一个假的 op 计数出来。
// ★ 钟：`pluginNowNs()` = 宿主**单调**纳秒（`steady_clock`）。⛔ 不接 guest 虚拟时钟、⛔ 不扣暂停
//   （预算判的是"宿主被回调占了多久"，与 guest 时间无关；与 `AfterDelay` / timeout 的墙钟同源）。
#pragma once

#include <cstdint>
#include <string>

namespace lift::plugin {

// 一次回调的墙钟上限（D5 裁决：2 ms）。★ 只罩**用户 JS 本体**（见头注）；超了 = 用户 JS 跑飞。
inline constexpr std::uint64_t kCallbackDeadlineNs = 2'000'000ull;
// 一次回调的操作数上限（D5 裁决：100k）。⚠ C1 不消费（见头注）；C2 的 QJS 才数。
inline constexpr std::uint32_t kCallbackOpLimit = 100'000u;
// 宿主段安全网的倍数（2 ms ⇒ 20 ms）。⛔ 它不参与"用户 JS 是否超限"，只防"宿主段卡死拖住 guest"。
inline constexpr std::uint64_t kHostSafetyNetFactor = 10;

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

// 宿主段安全网的纳秒上限（`deadline_ns × kHostSafetyNetFactor`；`deadline_ns == 0` = 不限 ⇒ 仍不限）。
// ★ 调用方把它当成一个 `deadline_ns` 用（`budgetExceeded(Budget{hostSafetyNetNs(b), ...}, ...)`）。
std::uint64_t hostSafetyNetNs(const Budget& budget);

// 超预算 reason 的**唯一**拼法：`<call>: callback budget exceeded[ (host)][: <name>]`。
// ★ 两侧共用（`TriggerRunner` 的宿主安全网 / `PluginHost` 的用户 JS 判据）⇒ 守卫与 e2e 按同一个
//   字符串判"是哪一侧超的"；`host == true` 的那条**必须**带 `(host)`（⛔ 不要与用户 JS 超限混淆）。
std::string callbackBudgetReason(const char* call, const std::string& name, bool host);

// =========================================================================================
// 回调耗时诊断（REQ-01M7VVDV69MJXRHNDGR50H7VHC 待查 #3）
//
// ★ **不是预算的一部分**：⛔ 不进默认输出、不进 `--help`。进判据的只有两样（见 `PluginHost::runCallback`）：
//   `js_ns` = 用户 JS 本体（2 ms 判据）；`host_ns` 只在 10× 宿主安全网上（reason 带 `(host)`）。
//   `plugin_host.cpp` 量 `host_ns` 并逐回调打一行；`script_quickjs.cpp` 把
//   `js_ns` / `build_ns` / `collect_ns` 与 `module_load_ns` 填进来（`inner_overran` 由中断回调置）。
// ★ 为什么用一个静态槽：两侧都要同一份读数，而 `PluginScript` 接口不该为诊断长一个方法。
//   guest 循环是单线程 ⇒ 一个槽就够（⛔ 不要当线程安全的东西用）。
// =========================================================================================
struct CallbackDiag {
    std::uint64_t host_ns = 0;         // `PluginHost::runCallback` 自己量的一段（≈ runner 的宿主段窗口）
    std::uint64_t inner_ns = 0;        // `call()` 从**进入**量到返回（build + js + collect；诊断用，⛔ 不是预算窗）
    std::uint64_t build_ns = 0;        // 组 ctx / 事件对象（每次回调新建；**不在** 2 ms 里）
    std::uint64_t js_ns = 0;           // 用户 JS 本体（**预算窗**；含 `ctx.log` 的 stderr 写）
    std::uint64_t collect_ns = 0;      // 返回值 → TriggerSet（**不在** 2 ms 里）
    std::uint64_t module_load_ns = 0;  // 模块装载（**在预算起点之前**）
    bool inner_overran = false;        // QJS 中断回调是否真的打断过（deadline / op_limit）
};
CallbackDiag& callbackDiag();

}  // namespace lift::plugin
