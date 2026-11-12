// emulator-lift · guest 执行令牌（**容器临界区**）
//
// ★ 不变式（**一句话**，2026-11-08 性能裁决后的口径）：
//     **任一时刻最多一个宿主线程持有令牌；guest 可见的共享容器只在令牌里被访问**
//     （GuestHeap / HandleTable / Vfs / ComRegistry / SurfaceArena / guest 内存账本 …）。
//   ⇒ guest **指令执行**可以并行（主线程 / 音频 feeder 各跑各的），但所有 **shim 调用**持令牌
//     （`ShimRegistry::invoke`）—— 因为容器访问全部发生在 shim 实现里；主线程另按块持令牌。
//
// ★ 为什么不是"整次 guest 执行持令牌"（历史方案，已否决）：音频 feeder 一次 fill 约
//   0.3–0.4 s CPU；与主线程的 13.8 s CPU 相加 ≈ 32.8 s ⇒ 单令牌串行后 wall 不可能低于这个和
//   （实测 34.8–39.1 s，目标 17.4 s）；并行基线（两核）wall 15.1 s。A/B 见需求单
//   `REQ-01M6X64J338E6YDQXJB9RKAFSG` 与 `.tmp/audiord/`。
//
// ★ 取/放规则（新代码必须照这个写，否则会红）：
//   - 主线程：`Machine::run` 的**每个块边界**取一次，块结束放。
//   - 音频 feeder：`feederEvent` 的 prologue/epilogue（读槽 / 写 CS 与在飞计数 / 建上下文）取；
//     **整次 fill 的 guest 指令执行不取**；fill 里的每次 shim 调用由 `ShimRegistry::invoke` 取。
//   - `pump()` 里任何会铺 guest 帧的路径也要取。
//   - 任何**会阻塞**的宿主等待都必须 `Yield`（临时让出令牌）后再等：guest CRITICAL_SECTION、
//     `GetMessageA` 的宿主事件泵、`WaitForSingleObject(喂料线程)`。
//     ⛔ 持令牌阻塞 = 死锁（另一条执行流永远等不到令牌去 Leave）。
//   - ★ **Yield 之后必须重新校验**：让出期间令牌可能被别人拿走，而别人**有权改容器**
//     （建/销 surface、alloc/free、改句柄表…）⇒ 让出前读到的**指针 / 迭代器 / 已查表结论
//     一律作废**；取回之后要重新 `check()`（确认自己真的又拿着）并**重新查表**。
//     ⛔ 不许拿 Yield 之前的 `GuestSurface*` / `std::map` 迭代器直接用（那是 use-after-erase）。
//     判据见 `tests/guest_token.cpp` 的 `guest-token/yield-revalidate`。
//
// ★ 违规计数（把不变量变成可观测）：`check(site)` 在"未持令牌"时 `++violations`。
//   主线程块（`machine.block`）、`GuestHeap` 的 alloc/free/realloc、`HandleTable::add` 都会 check；
//   `--assert-guest-token` 让"违规 > 0"变成非 0 退出码。⛔ 这些 check 在 shim 里执行 ⇒ 天然
//   覆盖 feeder 的容器访问；新增"绕过 shim 的容器入口"必须显式 acquire 令牌。
//
// ★★ 诊断开关（**默认关**，只给"派发层每块交多少固定税"的 A/B 用）：
//   `LIFT_TOKEN_DIAG_SKIP_LOCK=1` ⇒ `arm()` 把 `diag_skip_lock_` 置真，此后 `acquire/tryAcquire/
//   release/YieldGuard` **完全不碰 `mutex_`**（只做 `owner_/depth_` 与原子记账）。
//   - 目的：把"每块一次未竞争 mutex"这块钱从墙钟里**移除**，看它值多少（诊断臂，不是产品行为）。
//   - ⛔ 只允许**单线程**工况（`--audio off` ⇒ 没有音频 feeder 真线程）。并发下它等于把容器互斥
//     静默关掉 —— 所以 `Machine::run` 启动时用**默认级别**喊一条告警，摘要里也留一行
//     `token-diag : skip-lock=1`（⛔ 不许让它悄悄影响读数）。
//   - ★ 它**不影响** `--assert-guest-token` 的可信度：`violations_` / `concurrent_` / `max_concurrent_`
//     全部照旧记账（它们与 mutex 无关）。⇒ 打开时若真的出现两条执行流，"违规计数 / 持有者数 > 1"
//     仍然照旧判红 —— 这正是"该开关只能在单线程用"的机械判据。
//   - 读点在 `arm()`（每次 run 一次），⛔ 不在热路径读环境变量。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace lift {

class GuestToken {
public:
    GuestToken() = default;
    ~GuestToken() = default;
    GuestToken(const GuestToken&) = delete;
    GuestToken& operator=(const GuestToken&) = delete;

    // 开始一轮 run：清零本轮计数并打开违规计数（`Machine::run` 调；压力夹具也调）。
    // 未 arm 时 `check()` 恒真（不计数）—— 只用 `GuestHeap` 的纯单线程夹具（没有 guest 执行）
    // 不会被误报；Machine 销毁阶段（run 之后）也不会污染上一轮的账。
    // ★ `profile` = 本轮是否要 **Phase-0 读数**（`--profile`）；`arm()` 里再或上 `LIFT_AUDIO_TRACE`。
    //   缺省 false ⇒ 默认运行**一分钱都不花**（热路径只多一次 bool 读，见下面 Phase-0 那一组）。
    void arm(bool profile = false);
    void disarm() { armed_.store(false, std::memory_order_relaxed); }
    bool armed() const { return armed_.load(std::memory_order_relaxed); }

    // 阻塞取令牌；同一线程可重入（深度计数）。
    void acquire();
    // 非阻塞取；成功返回 true。
    bool tryAcquire();
    // 放令牌；未持有 / 深度不符 ⇒ ++violations（⛔ 不静默）。
    void release();
    bool heldByCurrentThread() const;

    // 检查点：未持令牌 ⇒ ++violations 并返回 false。热路径只多一次 relaxed 原子读。
    bool check(const char* site) const;

    std::uint64_t violations() const { return violations_.load(std::memory_order_relaxed); }
    const char* firstViolationSite() const { return first_site_.load(std::memory_order_relaxed); }
    // 同时**真正**持有令牌的线程数（重入不重复计数）。正确实现下恒 <= 1。
    unsigned concurrent() const { return concurrent_.load(std::memory_order_relaxed); }
    unsigned maxConcurrent() const { return max_concurrent_.load(std::memory_order_relaxed); }

    // ★ 诊断开关（见文件头）：本轮 `arm()` 时从 `LIFT_TOKEN_DIAG_SKIP_LOCK` 读出。
    //   真 ⇒ acquire/release 不碰 `mutex_`（⛔ 只允许单线程工况；`--assert-guest-token` 的
    //   violations / max-concurrent 判据**不受影响**）。摘要行 = `token-diag : skip-lock=<0|1>`。
    bool diagSkipLock() const { return diag_skip_lock_; }

    // ---- ★ M1 计量：**等令牌**的独立账（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）----
    //   为什么要独立一栏：shim 的 `ShimDef::ns` 口径是「shim 实现自身」—— 它**不是**等待时间。
    //   这一栏把"想拿令牌 → 真拿到"的那段墙钟分出来，供 M2 排序"谁在持令牌做阻塞型宿主等待"。
    //   `waits()` = **真的被挡住**的取令牌次数（重入 / 未竞争不计：未竞争路径不读时钟）。
    //   `waitNs()` = 这些等待的累计墙钟（steady_clock；与 `ns` **不可相加**：两段时间不重叠，
    //               但也不覆盖同一段 —— 与 guest 块表 / shim 表的关系一样）。
    //   `lastAcquireWaitNs()` = **调用线程**最近一次 acquire 等了多少 ns（重入 / 未竞争 ⇒ 0）。
    //   ⛔ 这三者是计量，不是契约：拿它们做判据时只能断言"真的分开了"，⛔ 不要断言具体数值。
    std::uint64_t waits() const { return waits_.load(std::memory_order_relaxed); }
    std::uint64_t waitNs() const { return wait_ns_.load(std::memory_order_relaxed); }
    std::uint64_t lastAcquireWaitNs() const;

    // ---- ★ Phase-0 读数组（REQ-01M75WWEDC20GEH7QVSHPHH4GP）：**只读**、默认关 ----
    //   为什么单开一组：上面的 `waits()/waitNs()` 只有**等待侧**；而"主线程一次不间断持有多久"
    //   完全没有读数 —— 那 1.3 s 的分解是 `边界数 × 交接延迟`，所以"43 ms 是真的一次长持有，
    //   还是抢不到（公平性）"决定了修法（M0 把阻塞等待移出令牌 vs 直接 M1 缩作用域）。
    //   · 开关 = `arm(profile)` 的入参（`--profile`）∪ `LIFT_AUDIO_TRACE`（**唯一读点在 `arm()`**，
    //     ⛔ 不在 acquire/release 里读环境变量）。默认关 ⇒ 热路径只多一次 bool 读、**不读时钟**。
    //   · 角色：0 = 调 `arm()` 的那条线程（`Machine::run` 的主循环线程），1 = 其它执行流
    //     （音频 feeder / 夹具线程）。为什么分角色：持有占空只对**主线程**有意义；
    //     等待直方图的主体是 feeder —— 混在一起就把两个问题搅成一个。
    //   · 五桶口径：<1 / 1–5 / 5–20 / 20–50 / >50 **ms**（宿主墙钟）。桶边界与需求单一致。
    //   · ⛔ 这些都是**计量**，不是契约：它们随机器与调度变，⛔ 不要拿具体数值当判据。
    static constexpr int kPhase0Buckets = 5;
    static constexpr int kPhase0Roles = 2;
    static constexpr int kPhase0RoleMain = 0;
    static constexpr int kPhase0RoleOther = 1;
    // 一"角色"的读数**快照**（`phase0Readings()` 拷出来；⛔ 不要把内部原子暴露出去）。
    struct Phase0Readings {
        std::uint64_t hold_ns = 0;                     // 累计**持令牌**的墙钟
        std::uint64_t hold_max_ns = 0;                 // **最长一次不间断持有**（Yield 把一段切成两段）
        std::uint64_t hold_count = 0;                  // 持有段数
        std::uint64_t hold_hist[kPhase0Buckets] = {};  // 持有直方图（ms 桶，见上）
        std::uint64_t waits = 0;                       // 真的被挡住的取令牌次数
        std::uint64_t wait_ns = 0;                     // 这些等待的累计墙钟
        std::uint64_t wait_hist[kPhase0Buckets] = {};  // 等待直方图（ms 桶）
        std::uint64_t last_release_ns = 0;             // 该角色最后一次放令牌的 steady_clock（ns）
    };
    bool phase0Enabled() const { return phase0_enabled_; }
    Phase0Readings phase0Readings(int role) const;
    // "总墙钟" = 本轮**首个**持有 → 各角色**最后**一次释放（steady_clock 差；0 = 还没持有过）。
    std::uint64_t phase0SpanNs() const;
    // 每个 shim 的**最长一次**宿主墙钟（`ShimRegistry::invoke` 在 `fn` 之后调；只记 max + 次数，
    // ⛔ 不记每次的分布）。`shim_id` 超出记账表容量 ⇒ 不静默丢：进 `shimMaxOverflow()`。
    void noteShimSpan(std::uint32_t shim_id, std::uint64_t ns);
    std::uint64_t shimMaxNs(std::uint32_t shim_id) const;
    std::uint64_t shimMaxCalls(std::uint32_t shim_id) const;
    std::uint64_t shimMaxOverflow() const;

    // 只给夹具（进程内的独立用例）：清计数。⛔ 运行期不许调。
    void resetForTest();

    // RAII：取令牌 / 放令牌。用在块边界与 feeder 的每次 guest 执行。
    class Guard {
    public:
        explicit Guard(GuestToken& token) : token_(&token) { token_->acquire(); }
        ~Guard() {
            if (token_) token_->release();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        GuestToken* token_;
    };

    // RAII：临时**让出**令牌去等一个宿主条件（guest CS / 宿主事件泵 / join），等完再取回。
    // 未持有时是无操作（例如 run 结束后 `audio_.end()` 里 join feeder）。
    class YieldGuard {
    public:
        explicit YieldGuard(GuestToken& token);
        ~YieldGuard();
        YieldGuard(const YieldGuard&) = delete;
        YieldGuard& operator=(const YieldGuard&) = delete;
        bool released() const { return released_; }

    private:
        GuestToken& token_;
        int depth_ = 0;
        bool released_ = false;
    };

private:
    static void* threadMark();
    void noteTake();
    void noteDrop();
    // ★ Phase-0 的记账点：全部自带 `phase0_enabled_` 门（⛔ 默认路径不进这些函数，见各调用点）。
    void noteHoldBegin();                 // 最外层取到令牌：开一段持有
    void noteHoldEnd();                   // 最外层放出令牌（或 Yield）：收一段持有
    void noteWait(std::uint64_t waited_ns);  // 真的被挡住的一次等待（分桶）
    void resetPhase0Stats();              // 每轮清零（只在打开时调 ⇒ 默认运行不扫表）

    std::mutex mutex_;
    std::atomic<void*> owner_{nullptr};
    int depth_ = 0;  // 只有 owner 线程读写（持 mutex_ 或重入）
    // ★ `check()` 是 const（调用点是 const 容器方法）⇒ 计数成员必须 mutable。
    mutable std::atomic<std::uint64_t> violations_{0};
    mutable std::atomic<const char*> first_site_{nullptr};
    std::atomic<unsigned> concurrent_{0};
    std::atomic<unsigned> max_concurrent_{0};
    std::atomic<bool> armed_{false};
    // ★ M1：等令牌的累计账（只在**真的被挡**时写 ⇒ 未竞争路径不付时钟开销）。
    std::atomic<std::uint64_t> waits_{0};
    std::atomic<std::uint64_t> wait_ns_{0};
    // ★ 诊断开关（文件头）：`arm()` 每次 run 重读环境变量；热路径只读这个 bool。
    bool diag_skip_lock_ = false;
    // ★ Phase-0 读数组（见上）：`arm()` 的**唯一**快照点。写者只有 `arm()`（run 起点、无线程持有时），
    //   读者是同一线程的 acquire/release 与 feeder（feeder 在 `arm()` 之后才被创建 ⇒ 线程创建点同步）。
    bool phase0_enabled_ = false;
};

// 一台 Machine 一份（进程级单例；本线的 Machine 也是进程级）。
GuestToken& guestToken();

}  // namespace lift
