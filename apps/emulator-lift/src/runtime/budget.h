// emulator-lift · 插件回调的**预算**（用户 JS 的 **CPU** deadline + 操作数上限）—— C1b
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
// ★ **宿主调用暂停**（2026-11-25，缺陷单 `REQ-01M86VRT1YP53CJS0M885FCCJM`）：同一条原则也罩
//   **用户在 JS 里调用宿主能力**的那一段 —— **本轮只接 `ctx.profile.end`**（生成窗口报告 + 写系统
//   剪贴板，缺陷现场 `js_cpu_us=2217`、后续窗口 2.56–2.90 ms CPU）。`script_quickjs.cpp` 的
//   `HostPause` 在调用宿主前后各取 `pluginThreadCpuNs()`，差值累加进 `Impl::paused_ns`（**每次回调
//   归零**）；`js_cpu_ns` 与中断回调的 deadline 比较都扣掉它。⛔ 其余宿主能力（`ctx.log` /
//   `profile.begin` / `edit`）**未接**；真出现超预算再按同一原则逐个接，⛔ 不要把它写成"整类宿主能力
//   都已暂停"。⛔ 这不是放宽 2 ms：用户 JS 自己的 CPU 一分不减，真跑飞的忙循环照样响亮失败
//   （`plugin/callback-js-band`）。
// ★ **宿主侧另有安全网** = `deadline_ns × kHostSafetyNetFactor`（2 ms ⇒ **20 ms**）：它防的是
//   "宿主段被卡死 ⇒ guest 被拖住"，不是"用户 JS 跑飞" ⇒ 超了照样响亮失败，但 reason 带 `(host)`
//   （与用户 JS 超限的 reason 可区分；拼法只有 `callbackBudgetReason()` 一处）。
// ★ `op_limit` 本切片**不消费**：C1 的 handler 是 C++（没有"步数"概念）；它的真消费者是 C2 的 QJS
//   （QJS interrupt handler 数 op）。⇒ 这里只声明 + 保留形状，⛔ 不编一个假的 op 计数出来。
// ★ **两把钟，别混**（2026-11-21 用户裁决：2 ms 预算改用 CPU 时钟）：
//   · 用户 JS 的 **2 ms** 用 `pluginThreadCpuNs()`（**本线程 CPU 时间**，见下）。被 OS 抢占 /
//     等 I/O / 睡 都**不算**进它 —— 这正是缺陷 `REQ-01M7VVDV69MJXRHNDGR50H7VHC` 的修法：
//     一次只做 log+click+建 2 个 trigger 的回调不可能真花 9 ms（墙钟读数被抢占污染）。
//   · 宿主安全网 **20 ms** 继续用 `pluginNowNs()`（**墙钟**）。⛔ 不要把它也换成 CPU 钟：
//     它防的是"宿主段卡住 ⇒ guest 被拖住"，而"被抢占"恰好是它**应该**抓到的现象。
//   ⛔ 两把钟都**不接** guest 虚拟时钟、⛔ 不扣暂停；与 `AfterDelay` / timeout 的墙钟各自独立。
//
// ★ **`pluginThreadCpuNs()` 的读数粒度（实测，⛔ 守卫不要按"2 ms 精确可测"写断言）**：
//   · POSIX：`clock_gettime(CLOCK_THREAD_CPUTIME_ID)`，纳秒级。
//   · Windows：**主源 = `QueryThreadCycleTime`**（线程周期计数，本机 ≈ ns 级）；首次使用时
//     用 4 个 ~200 µs 的忙等窗口把 cycles 标定成 ns（取 max(cycles/wall) —— 抢占只会抬高 wall；
//     本机实测 **≈3.97 cycles/ns**，只做一次）。这次标定烧 ~0.8 ms CPU ⇒ `call()` 在计时窗**之前**
//     预热它（见 `script_quickjs.cpp`），⛔ 不进 2 ms 判据窗。
//     ⚠ **MSDN 的已知限制**：`QueryThreadCycleTime` **不保证精确** —— CPU 频率变化（节能 / turbo）
//     与跨核迁移都可能让计数与真实 CPU 时间有偏差 ⇒ 它只服务"2 ms 判据"，⛔ **不要**把它读成
//     "精确 CPU 时间"、也⛔ 不要拿它做计费 / 性能读数。
//     ⚠ **为什么不是 `GetThreadTimes`**：它是**按系统时钟 tick 记账**的（100 ns 单位只是字段宽度）。
//     实测本机（win32 / 2026-11-21）：读数**只能**是 **15.625 ms** 的整数倍 —— 0.5 ms 的忙等读到
//     0 或 156250；窗口里跨过一次 tick 边界就会**多记** 15.625 ms ⇒ 2 ms 判据的**假阳性**率约
//     `cpu_ms / 15.6`（实测 40 个 0.5 ms 窗口里 1 次）。⇒ 它只作 `QueryThreadCycleTime` 失败时
//     的**兜底**（那时粒度就是 15.625 ms；能判"跑飞"，判不了 2 ms）。
//   · 读数**单调不减**（同一线程内可直接相减）；⛔ 不保证与墙钟同源、⛔ 不保证跨线程可比。
#pragma once

#include <cstdint>
#include <string>

namespace lift::plugin {

// 一次回调的 **CPU 时间**上限（D5 裁决：2 ms）。★ 只罩**用户 JS 本体**（见头注）；超了 = 用户 JS 跑飞。
inline constexpr std::uint64_t kCallbackDeadlineNs = 2'000'000ull;
// 一次回调的操作数上限（D5 裁决：100k）。⚠ C1 不消费（见头注）；C2 的 QJS 才数。
inline constexpr std::uint32_t kCallbackOpLimit = 100'000u;
// 宿主段安全网的倍数（2 ms ⇒ 20 ms）。⛔ 它不参与"用户 JS 是否超限"，只防"宿主段卡死拖住 guest"。
inline constexpr std::uint64_t kHostSafetyNetFactor = 10;

struct Budget {
    // 0 = **不限时间**（只有 op_limit 那一半生效）—— 给"明确要放宽"的调用方一个干净的口子。
    // ★ 单位是**用户 JS 本体的 CPU 时间**（`pluginThreadCpuNs()`），⛔ 不是墙钟。
    std::uint64_t deadline_ns = kCallbackDeadlineNs;
    std::uint32_t op_limit = kCallbackOpLimit;
};

// 宿主**墙钟**单调纳秒（`std::chrono::steady_clock`；`budget.cpp` 是唯一定义）。
// ★ 只用于**宿主安全网**（`hostSafetyNetNs()`）与诊断；⛔ 不要拿它判用户 JS 的 2 ms。
std::uint64_t pluginNowNs();

// **本线程 CPU 时间**的纳秒读数（kernel + user；不含被抢占 / 睡眠 / 等 I/O）。
// ★ 只用于**用户 JS 本体的 2 ms**（`script_quickjs.cpp` 的 deadline 起点与中断回调比较、
//   `plugin_host.cpp` 的 `js_late` 判据）。⛔ 宿主安全网继续用 `pluginNowNs()`（见头注）。
// ★ 粒度见文件头注（Windows 主源 ≈ ns 级；`GetThreadTimes` 兜底 = 15.625 ms 整 tick）。
std::uint64_t pluginThreadCpuNs();

// 是否超预算。判据 = `now_ns - start_ns >= deadline_ns`（**含等号**：正好到点即超）。
// 钟回退（`now_ns < start_ns`）不算超：那是"读数坏了"，不是"回调跑飞"。
// ★ 两把钟都适用：`pluginNowNs()`（墙钟）与 `pluginThreadCpuNs()`（CPU）；⛔ 不要交叉相减。
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
//   `js_cpu_ns` = 用户 JS 本体的 **CPU** 时间（2 ms 判据）；`host_ns` 只在 10× 宿主安全网上
//   （墙钟，reason 带 `(host)`）。`plugin_host.cpp` 量 `host_ns` 并逐回调打一行；
//   `script_quickjs.cpp` 把 `js_ns`（墙钟）/ `js_cpu_ns`（CPU）/ `build_ns` / `collect_ns` 与
//   `module_load_ns` 填进来（`inner_overran` 由中断回调置）。
// ★ 为什么用一个静态槽：两侧都要同一份读数，而 `PluginScript` 接口不该为诊断长一个方法。
//   guest 循环是单线程 ⇒ 一个槽就够（⛔ 不要当线程安全的东西用）。
// =========================================================================================
struct CallbackDiag {
    std::uint64_t host_ns = 0;         // `PluginHost::runCallback` 自己量的一段（≈ runner 的宿主段窗口；墙钟）
    std::uint64_t inner_ns = 0;        // `call()` 从**进入**量到返回（build + js + collect；诊断用，⛔ 不是预算窗）
    std::uint64_t build_ns = 0;        // 组 ctx / 事件对象（每次回调新建；**不在** 2 ms 里）
    std::uint64_t js_ns = 0;           // 用户 JS 本体的**墙钟**（含 `ctx.log` 的 stderr 写；只作对照读数）
    std::uint64_t js_cpu_ns = 0;       // 用户 JS 本体的**CPU 时间** = **预算窗**（2 ms 判据用这个）
    // 宿主调用暂停的**读数**（`ctx.profile.end` 一类宿主能力的 CPU；已从 `js_cpu_ns` 里扣掉）。
    // ⛔ 不是判据：它只回答"这段时间是谁的"（`js_cpu_us` 小、`paused_us` 大 = 宿主替用户付的）。
    std::uint64_t host_pause_ns = 0;
    std::uint64_t collect_ns = 0;      // 返回值 → TriggerSet（**不在** 2 ms 里）
    std::uint64_t module_load_ns = 0;  // 模块装载（**在预算起点之前**）
    bool inner_overran = false;        // QJS 中断回调是否真的打断过（deadline / op_limit）
};
CallbackDiag& callbackDiag();

}  // namespace lift::plugin
