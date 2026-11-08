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
//
// ★ 违规计数（把不变量变成可观测）：`check(site)` 在"未持令牌"时 `++violations`。
//   主线程块（`machine.block`）、`GuestHeap` 的 alloc/free/realloc、`HandleTable::add` 都会 check；
//   `--assert-guest-token` 让"违规 > 0"变成非 0 退出码。⛔ 这些 check 在 shim 里执行 ⇒ 天然
//   覆盖 feeder 的容器访问；新增"绕过 shim 的容器入口"必须显式 acquire 令牌。
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
    void arm();
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

    std::mutex mutex_;
    std::atomic<void*> owner_{nullptr};
    int depth_ = 0;  // 只有 owner 线程读写（持 mutex_ 或重入）
    // ★ `check()` 是 const（调用点是 const 容器方法）⇒ 计数成员必须 mutable。
    mutable std::atomic<std::uint64_t> violations_{0};
    mutable std::atomic<const char*> first_site_{nullptr};
    std::atomic<unsigned> concurrent_{0};
    std::atomic<unsigned> max_concurrent_{0};
    std::atomic<bool> armed_{false};
};

// 一台 Machine 一份（进程级单例；本线的 Machine 也是进程级）。
GuestToken& guestToken();

}  // namespace lift
