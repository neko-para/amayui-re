// emulator-lift · guest 执行令牌实现（单令牌 / GIL；见 guest_token.h 的不变量）
#include "runtime/guest_token.h"

namespace lift {
namespace {
// 每个线程一个**唯一地址**当身份标记：比 hash(thread::id) 精确，比 mutex 读快。
// ⚠ 线程退出后该地址可能被新线程复用 —— 令牌在每次 release 时清 owner，且宿主线程是
//   长生命周期的主线程 / feeder / mixer，不依赖"线程 id 永不复用"。
thread_local char t_thread_marker = 0;
}  // namespace

void* GuestToken::threadMark() { return &t_thread_marker; }

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
        return;
    }
    mutex_.lock();
    owner_.store(me, std::memory_order_relaxed);
    depth_ = 1;
    noteTake();
}

bool GuestToken::tryAcquire() {
    void* me = threadMark();
    if (owner_.load(std::memory_order_relaxed) == me) {
        ++depth_;
        return true;
    }
    if (!mutex_.try_lock()) return false;
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
    mutex_.unlock();
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
    armed_.store(true, std::memory_order_relaxed);
}

void GuestToken::resetForTest() {
    // 调用方必须保证**没有线程持有令牌**（夹具在起线程 / 取令牌之前调）。
    violations_.store(0, std::memory_order_relaxed);
    first_site_.store(nullptr, std::memory_order_relaxed);
    concurrent_.store(0, std::memory_order_relaxed);
    max_concurrent_.store(0, std::memory_order_relaxed);
    armed_.store(false, std::memory_order_relaxed);
}

GuestToken::YieldGuard::YieldGuard(GuestToken& token) : token_(token) {
    if (!token_.heldByCurrentThread()) return;
    depth_ = token_.depth_;
    token_.depth_ = 0;
    token_.owner_.store(nullptr, std::memory_order_relaxed);
    token_.noteDrop();
    token_.mutex_.unlock();
    released_ = true;
}

GuestToken::YieldGuard::~YieldGuard() {
    if (!released_) return;
    token_.mutex_.lock();
    token_.owner_.store(threadMark(), std::memory_order_relaxed);
    token_.depth_ = depth_;
    token_.noteTake();
}

GuestToken& guestToken() {
    static GuestToken token;
    return token;
}

}  // namespace lift
