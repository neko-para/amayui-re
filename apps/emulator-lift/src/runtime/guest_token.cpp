// emulator-lift · guest 执行令牌实现（单令牌 / GIL；见 guest_token.h 的不变量）
#include "runtime/guest_token.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace lift {
namespace {
// 每个线程一个**唯一地址**当身份标记：比 hash(thread::id) 精确，比 mutex 读快。
// ⚠ 线程退出后该地址可能被新线程复用 —— 令牌在每次 release 时清 owner，且宿主线程是
//   长生命周期的主线程 / feeder / mixer，不依赖"线程 id 永不复用"。
thread_local char t_thread_marker = 0;

// ★ M1（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）：**调用线程最近一次 acquire 等了多久**（ns）。
//   只在 `acquire()` / `YieldGuard` 取回时写；重入与未竞争 ⇒ 0。
//   ⛔ 不要把它当累计量（累计在 `GuestToken::waitNs()`）。
thread_local std::uint64_t t_last_wait_ns = 0;

// ★ 诊断开关的读点（**只在 `arm()` 里调一次**，⛔ 不进热路径）：
//   `LIFT_TOKEN_DIAG_SKIP_LOCK` 存在、非空、且不是 "0" ⇒ 打开（其余一律关 = 缺省）。
//   文件头说明了它为什么存在、为什么只允许单线程工况、以及为什么它不影响 --assert-guest-token。
bool diagSkipLockRequested() {
    const char* v = std::getenv("LIFT_TOKEN_DIAG_SKIP_LOCK");
    if (v == nullptr || v[0] == '\0') return false;
    if (std::strcmp(v, "0") == 0) return false;
    return true;
}

// 阻塞取锁并返回**等了多少 ns**（两次 steady_clock 包住 `lock()`）。
// ★ 只给"已经确认会被挡"的路径调（先 `try_lock` 探一次）⇒ 未竞争的热路径读不到时钟。
//   `mutex` 是 `GuestToken::mutex_` 的引用形参：本函数在同一个 TU 里，不需要友元。
std::uint64_t blockWaitNs(std::mutex& mutex) {
    const auto t0 = std::chrono::steady_clock::now();
    mutex.lock();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}
}  // namespace

void* GuestToken::threadMark() { return &t_thread_marker; }

std::uint64_t GuestToken::lastAcquireWaitNs() const { return t_last_wait_ns; }

void GuestToken::noteTake() {
    const unsigned now = concurrent_.fetch_add(1, std::memory_order_relaxed) + 1u;
    unsigned prev = max_concurrent_.load(std::memory_order_relaxed);
    while (now > prev &&
           !max_concurrent_.compare_exchange_weak(prev, now, std::memory_order_relaxed)) {
        // prev 由 compare_exchange_weak 更新；重试直到写入成功或已有更大值
    }
}

void GuestToken::noteDrop() { concurrent_.fetch_sub(1, std::memory_order_relaxed); }

void GuestToken::acquire() {
    void* me = threadMark();
    if (owner_.load(std::memory_order_relaxed) == me) {  // 可重入：只加深度
        ++depth_;
        t_last_wait_ns = 0;  // 重入不算"等"（令牌本来就在自己手里）
        return;
    }
    // ★ M1：唯一的取锁点。先 `try_lock` 探一次竞争 ——
    //   · 成功 ⇒ 没等（`t_last_wait_ns = 0`，**不读时钟**，热路径零额外开销）；
    //   · 失败 ⇒ 真的会被挡：用 `blockWaitNs` 把这次等待记账，再继续（等待期间不持令牌）。
    //   ⛔ 记账不是契约（⛔ 不要拿具体数值当判据）；它只回答"等令牌到底占多少"。
    // ★ 诊断臂（文件头）：`diag_skip_lock_` 为真 ⇒ **完全不碰 mutex_**（连 try_lock 都不做）。
    //   ⛔ 只允许单线程工况；violations / concurrent 照旧记账 ⇒ --assert-guest-token 仍然有效。
    if (diag_skip_lock_) {
        t_last_wait_ns = 0;
    } else if (mutex_.try_lock()) {
        t_last_wait_ns = 0;
    } else {
        const std::uint64_t waited = blockWaitNs(mutex_);
        t_last_wait_ns = waited;
        waits_.fetch_add(1, std::memory_order_relaxed);
        wait_ns_.fetch_add(waited, std::memory_order_relaxed);
    }
    owner_.store(me, std::memory_order_relaxed);
    depth_ = 1;
    noteTake();
}

bool GuestToken::tryAcquire() {
    void* me = threadMark();
    t_last_wait_ns = 0;  // 非阻塞取 ⇒ 从定义上就没有"等"（失败也一样：没等）
    if (owner_.load(std::memory_order_relaxed) == me) {
        ++depth_;
        return true;
    }
    // ★ 诊断臂（文件头）：跳过 mutex ⇒ 这里也不 try_lock（否则诊断臂仍会碰到锁）。
    if (!diag_skip_lock_ && !mutex_.try_lock()) return false;
    owner_.store(me, std::memory_order_relaxed);
    depth_ = 1;
    noteTake();
    return true;
}

void GuestToken::release() {
    void* me = threadMark();
    if (owner_.load(std::memory_order_relaxed) != me) {
        // 未持有就 release / 在别的线程 release：这是一条实现 bug，计数但不改状态
        //（⛔ 不能去 unlock 一把本线程没持有的 mutex —— 那是 UB）。
        violations_.fetch_add(1, std::memory_order_relaxed);
        const char* expected = nullptr;
        first_site_.compare_exchange_strong(expected, "GuestToken::release(非持有者)",
                                            std::memory_order_relaxed);
        return;
    }
    if (depth_ > 1) {
        --depth_;
        return;
    }
    depth_ = 0;
    owner_.store(nullptr, std::memory_order_relaxed);
    noteDrop();
    // ★ 诊断臂（文件头）：没锁过就⛔ 不许 unlock（那是 UB）。
    if (!diag_skip_lock_) mutex_.unlock();
}

bool GuestToken::heldByCurrentThread() const {
    return owner_.load(std::memory_order_relaxed) == threadMark();
}

bool GuestToken::check(const char* site) const {
    if (!armed_.load(std::memory_order_relaxed)) return true;  // 没用令牌的纯夹具：不计数
    if (heldByCurrentThread()) return true;
    violations_.fetch_add(1, std::memory_order_relaxed);
    const char* expected = nullptr;
    first_site_.compare_exchange_strong(expected, site, std::memory_order_relaxed);
    return false;
}

void GuestToken::arm() {
    // 一轮 run 的起点：清零**本轮**计数（上一次 run / 上一次 Machine 的账不能串进来）。
    // ⛔ 不要清零 owner_/depth_：那会和 mutex_ 的锁状态不一致（arm 只应在无持有者时调）。
    violations_.store(0, std::memory_order_relaxed);
    first_site_.store(nullptr, std::memory_order_relaxed);
    max_concurrent_.store(0, std::memory_order_relaxed);
    // ★ M1 的"等令牌"账也是**每轮**的（摘要是"这一轮等了多少"，不是进程累计）。
    waits_.store(0, std::memory_order_relaxed);
    wait_ns_.store(0, std::memory_order_relaxed);
    t_last_wait_ns = 0;
    // ★ 诊断开关的**唯一读点**（每次 run 一次）：⛔ 不在块边界读环境变量。
    diag_skip_lock_ = diagSkipLockRequested();
    armed_.store(true, std::memory_order_relaxed);
}

void GuestToken::resetForTest() {
    // 调用方必须保证**没有线程持有令牌**（夹具在起线程 / 取令牌之前调）。
    violations_.store(0, std::memory_order_relaxed);
    first_site_.store(nullptr, std::memory_order_relaxed);
    concurrent_.store(0, std::memory_order_relaxed);
    max_concurrent_.store(0, std::memory_order_relaxed);
    waits_.store(0, std::memory_order_relaxed);
    wait_ns_.store(0, std::memory_order_relaxed);
    t_last_wait_ns = 0;
    diag_skip_lock_ = false;  // 夹具的缺省臂 = 正常取锁（下一次 arm() 再按环境变量决定）
    armed_.store(false, std::memory_order_relaxed);
}

GuestToken::YieldGuard::YieldGuard(GuestToken& token) : token_(token) {
    if (!token_.heldByCurrentThread()) return;
    depth_ = token_.depth_;
    token_.depth_ = 0;
    token_.owner_.store(nullptr, std::memory_order_relaxed);
    token_.noteDrop();
    // ★ 诊断臂：诊断臂里 `mutex_` 从没被锁过 ⇒ ⛔ 不 unlock（UB）。
    if (!token_.diag_skip_lock_) token_.mutex_.unlock();
    released_ = true;
}

GuestToken::YieldGuard::~YieldGuard() {
    if (!released_) return;
    // ★ 取回也可能**被挡**（让出这段窗口里别人可能正拿着令牌）⇒ 与 `acquire()` 同一个
    //   记账口径（先探一次；真的等才读时钟）。⛔ 不要在这里插 `check()`：此刻自己确实
    //   还没拿到令牌，那不是违规，而是 Yield 语义本身。
    // ★ 诊断臂：整段不碰 mutex（诊断臂从来没有锁可放，也就没有锁可取）。
    if (token_.diag_skip_lock_) {
        t_last_wait_ns = 0;
    } else if (token_.mutex_.try_lock()) {
        t_last_wait_ns = 0;
    } else {
        const std::uint64_t waited = blockWaitNs(token_.mutex_);
        t_last_wait_ns = waited;
        token_.waits_.fetch_add(1, std::memory_order_relaxed);
        token_.wait_ns_.fetch_add(waited, std::memory_order_relaxed);
    }
    token_.owner_.store(threadMark(), std::memory_order_relaxed);
    token_.depth_ = depth_;
    token_.noteTake();
}

GuestToken& guestToken() {
    static GuestToken token;
    return token;
}

}  // namespace lift
