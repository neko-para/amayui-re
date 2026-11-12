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

// ---------------------------------------------------------------------------------------
// ★ Phase-0 读数（REQ-01M75WWEDC20GEH7QVSHPHH4GP）：持有占空 / 持有直方图 / 等待直方图
//   / 每个 shim 的 max_ns。**全部只读**，并且只在 `arm(profile)` 或 `LIFT_AUDIO_TRACE` 打开时
//   才写（⛔ 默认运行不读时钟、不写这些原子）。口径见 `guest_token.h` 的 Phase-0 那一组。
// ---------------------------------------------------------------------------------------
// 五桶的边界（ns）：<1 / 1–5 / 5–20 / 20–50 / >50 ms。
constexpr std::uint64_t kBucketBoundsNs[4] = {1'000'000ull, 5'000'000ull, 20'000'000ull, 50'000'000ull};

int bucketOf(std::uint64_t ns) {
    if (ns < kBucketBoundsNs[0]) return 0;
    if (ns < kBucketBoundsNs[1]) return 1;
    if (ns < kBucketBoundsNs[2]) return 2;
    if (ns < kBucketBoundsNs[3]) return 3;
    return 4;
}

std::uint64_t steadyNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 一"角色"的读数：0 = 主循环线程，1 = 其它执行流（音频 feeder / 夹具线程）。
// 为什么用原子而不是裸 uint64：角色 1 可能不止一条线程（今天的 feeder 只一条，但**不靠**这个前提），
// 且摘要可能在 run 期间被打；代价只落在"已经打开记账"的那条路径上。
struct Phase0RoleStat {
    std::atomic<std::uint64_t> hold_ns{0};
    std::atomic<std::uint64_t> hold_max_ns{0};
    std::atomic<std::uint64_t> hold_count{0};
    std::atomic<std::uint64_t> hold_hist[GuestToken::kPhase0Buckets];
    std::atomic<std::uint64_t> waits{0};
    std::atomic<std::uint64_t> wait_ns{0};
    std::atomic<std::uint64_t> wait_hist[GuestToken::kPhase0Buckets];
    std::atomic<std::uint64_t> last_release_ns{0};  // 该角色最后一次放令牌（用于 span 的终点）
    Phase0RoleStat() {
        for (auto& b : hold_hist) b.store(0, std::memory_order_relaxed);
        for (auto& b : wait_hist) b.store(0, std::memory_order_relaxed);
    }
    void reset() {
        hold_ns.store(0, std::memory_order_relaxed);
        hold_max_ns.store(0, std::memory_order_relaxed);
        hold_count.store(0, std::memory_order_relaxed);
        for (auto& b : hold_hist) b.store(0, std::memory_order_relaxed);
        waits.store(0, std::memory_order_relaxed);
        wait_ns.store(0, std::memory_order_relaxed);
        for (auto& b : wait_hist) b.store(0, std::memory_order_relaxed);
        last_release_ns.store(0, std::memory_order_relaxed);
    }
};

Phase0RoleStat g_phase0_role[GuestToken::kPhase0Roles];
// 本轮**首个**持有的时刻（CAS 只在 0→now 那一次成功；之后每个 acquire 只多一次 relaxed 读）。
std::atomic<std::uint64_t> g_phase0_span_begin_ns{0};

// 每个 shim id 的**最长一次**宿主墙钟 / 记账次数。容量 = 桩页槽数上限（`image.h` 的
// `shim_slots` 缺省 0x2000 ⇒ id 必然 < 8192）；超出的进 overflow，⛔ 不静默丢。
constexpr std::uint32_t kShimMaxSlots = 8192;
std::atomic<std::uint64_t> g_shim_max_ns[kShimMaxSlots];
std::atomic<std::uint64_t> g_shim_max_calls[kShimMaxSlots];
std::atomic<std::uint64_t> g_shim_max_overflow{0};

// 本线程**当前这一段持有**的起点（0 = 没在记账 / 没持有）。只有持有者线程读写。
thread_local std::uint64_t t_hold_begin_ns = 0;
// 这一段持有属于**哪台令牌**（进程里有单例 + 夹具里可能另建实例；TLS 的计时不能串台。
// ⛔ 不要用"本线程只可能持有一台"这个假设）。
thread_local const GuestToken* t_hold_owner = nullptr;
// 本线程的角色：`arm()` 把调用线程标成 0（主循环）；其它线程缺省 1（feeder / 夹具）。
thread_local int t_phase0_role = GuestToken::kPhase0RoleOther;

// `LIFT_AUDIO_TRACE` 存在即开（与 `shims.cpp` 的 `audioTraceEnabled()` 同口径）。
// ★ 唯一读点 = `arm()`（⛔ 不在 acquire/release 里读环境变量 —— 那是每块一次的系统调用级开销）。
bool audioTraceRequested() { return std::getenv("LIFT_AUDIO_TRACE") != nullptr; }

inline Phase0RoleStat& roleStat(int role) {
    return g_phase0_role[role == GuestToken::kPhase0RoleMain ? GuestToken::kPhase0RoleMain
                                                             : GuestToken::kPhase0RoleOther];
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
        // ★ Phase-0：等待直方图（分桶）。只在记账打开时写（⛔ 默认不读时钟、不写桶）。
        if (phase0_enabled_) noteWait(waited);
    }
    owner_.store(me, std::memory_order_relaxed);
    depth_ = 1;
    noteTake();
    // ★ Phase-0：最外层取到令牌 ⇒ 开一段"持有"（起点在真拿到之后，⛔ 不含等待）。
    if (phase0_enabled_) noteHoldBegin();
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
    if (phase0_enabled_) noteHoldBegin();
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
    // ★ Phase-0：持有段在**真放锁之后**收口 —— ⛔ 不把记账塞进临界区（那会改变被观测的持有时长）。
    if (phase0_enabled_) noteHoldEnd();
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

void GuestToken::arm(bool profile) {
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
    // ★ Phase-0 读数组（REQ-01M75WWEDC20GEH7QVSHPHH4GP）：开关的**唯一快照点**。
    //   `--profile` 由 `Machine::run` 传进来；`LIFT_AUDIO_TRACE` 在这里读一次（默认关）。
    //   打开时清零本轮账（8192 个 shim 槽 + 两份角色账只在**打开时**扫，默认运行一分钱不花）。
    phase0_enabled_ = profile || audioTraceRequested();
    t_phase0_role = kPhase0RoleMain;  // 本次 run 的主循环线程就是 arm() 的调用者
    t_hold_begin_ns = 0;
    t_hold_owner = nullptr;
    if (phase0_enabled_) resetPhase0Stats();
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
    // ★ Phase-0：夹具缺省**不记账**（要读它就直接 `arm(true)`）。
    phase0_enabled_ = false;
    t_hold_begin_ns = 0;
    t_hold_owner = nullptr;
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
    // ★ Phase-0：Yield 把"一段不间断持有"切成两段（让出期间**不算**持有）⇒ 在这里收口一段。
    //   ⛔ 不改 Yield 的任何语义：记账在 unlock 之后，且默认关。
    if (token_.phase0_enabled_) token_.noteHoldEnd();
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
        if (token_.phase0_enabled_) token_.noteWait(waited);
    }
    token_.owner_.store(threadMark(), std::memory_order_relaxed);
    token_.depth_ = depth_;
    token_.noteTake();
    // ★ Phase-0：取回之后开**新的一段**持有（Yield 前那段已在 ctor 里收口）。
    if (token_.phase0_enabled_) token_.noteHoldBegin();
}

// ---------------------------------------------------------------------------------------
// ★ Phase-0 读数的记账点 / 读点（口径见 guest_token.h 的 Phase-0 那一组）
// ---------------------------------------------------------------------------------------
void GuestToken::noteHoldBegin() {
    if (!phase0_enabled_) return;
    const std::uint64_t now = steadyNs();
    // span 起点 = 本轮**第一次**有人拿到令牌；之后每个 acquire 只多一次 relaxed 读。
    if (g_phase0_span_begin_ns.load(std::memory_order_relaxed) == 0) {
        std::uint64_t expected = 0;
        g_phase0_span_begin_ns.compare_exchange_strong(expected, now, std::memory_order_relaxed);
    }
    t_hold_begin_ns = now;
    t_hold_owner = this;
}

void GuestToken::noteHoldEnd() {
    if (!phase0_enabled_ || t_hold_begin_ns == 0 || t_hold_owner != this) return;
    const std::uint64_t now = steadyNs();
    const std::uint64_t dur = now - t_hold_begin_ns;
    t_hold_begin_ns = 0;
    t_hold_owner = nullptr;
    Phase0RoleStat& s = roleStat(t_phase0_role);
    s.hold_ns.fetch_add(dur, std::memory_order_relaxed);
    s.hold_count.fetch_add(1, std::memory_order_relaxed);
    s.hold_hist[bucketOf(dur)].fetch_add(1, std::memory_order_relaxed);
    std::uint64_t prev = s.hold_max_ns.load(std::memory_order_relaxed);
    while (dur > prev && !s.hold_max_ns.compare_exchange_weak(prev, dur, std::memory_order_relaxed)) {
        // prev 由 compare_exchange_weak 更新；重试直到写入成功或已有更大值（同 noteTake 的写法）
    }
    // span 的终点 = 各角色**最后**一次释放的时刻 ⇒ 单写者（该角色自己的线程），不需要 CAS。
    s.last_release_ns.store(now, std::memory_order_relaxed);
}

void GuestToken::noteWait(std::uint64_t waited_ns) {
    if (!phase0_enabled_) return;
    Phase0RoleStat& s = roleStat(t_phase0_role);
    s.waits.fetch_add(1, std::memory_order_relaxed);
    s.wait_ns.fetch_add(waited_ns, std::memory_order_relaxed);
    s.wait_hist[bucketOf(waited_ns)].fetch_add(1, std::memory_order_relaxed);
}

void GuestToken::resetPhase0Stats() {
    for (int r = 0; r < kPhase0Roles; ++r) g_phase0_role[r].reset();
    g_phase0_span_begin_ns.store(0, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kShimMaxSlots; ++i) {
        g_shim_max_ns[i].store(0, std::memory_order_relaxed);
        g_shim_max_calls[i].store(0, std::memory_order_relaxed);
    }
    g_shim_max_overflow.store(0, std::memory_order_relaxed);
}

GuestToken::Phase0Readings GuestToken::phase0Readings(int role) const {
    Phase0Readings out;
    if (role < 0 || role >= kPhase0Roles) return out;
    const Phase0RoleStat& s = g_phase0_role[role];
    out.hold_ns = s.hold_ns.load(std::memory_order_relaxed);
    out.hold_max_ns = s.hold_max_ns.load(std::memory_order_relaxed);
    out.hold_count = s.hold_count.load(std::memory_order_relaxed);
    for (int b = 0; b < kPhase0Buckets; ++b) out.hold_hist[b] = s.hold_hist[b].load(std::memory_order_relaxed);
    out.waits = s.waits.load(std::memory_order_relaxed);
    out.wait_ns = s.wait_ns.load(std::memory_order_relaxed);
    for (int b = 0; b < kPhase0Buckets; ++b) out.wait_hist[b] = s.wait_hist[b].load(std::memory_order_relaxed);
    out.last_release_ns = s.last_release_ns.load(std::memory_order_relaxed);
    return out;
}

std::uint64_t GuestToken::phase0SpanNs() const {
    const std::uint64_t begin = g_phase0_span_begin_ns.load(std::memory_order_relaxed);
    if (begin == 0) return 0;
    std::uint64_t end = 0;
    for (int r = 0; r < kPhase0Roles; ++r) {
        const std::uint64_t e = g_phase0_role[r].last_release_ns.load(std::memory_order_relaxed);
        if (e > end) end = e;
    }
    return end > begin ? end - begin : 0;
}

void GuestToken::noteShimSpan(std::uint32_t shim_id, std::uint64_t ns) {
    if (!phase0_enabled_) return;
    if (shim_id >= kShimMaxSlots) {
        // ⛔ 不静默丢：超容量的 id 记进 overflow（摘要在打了这行才看得见，见 dumpRunSummary）。
        g_shim_max_overflow.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_shim_max_calls[shim_id].fetch_add(1, std::memory_order_relaxed);
    std::uint64_t prev = g_shim_max_ns[shim_id].load(std::memory_order_relaxed);
    while (ns > prev &&
           !g_shim_max_ns[shim_id].compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
        // 同 noteHoldEnd：只记**最大值**，不记分布（⛔ 热路径不存每次读数）。
    }
}

std::uint64_t GuestToken::shimMaxNs(std::uint32_t shim_id) const {
    return shim_id < kShimMaxSlots ? g_shim_max_ns[shim_id].load(std::memory_order_relaxed) : 0;
}

std::uint64_t GuestToken::shimMaxCalls(std::uint32_t shim_id) const {
    return shim_id < kShimMaxSlots ? g_shim_max_calls[shim_id].load(std::memory_order_relaxed) : 0;
}

std::uint64_t GuestToken::shimMaxOverflow() const {
    return g_shim_max_overflow.load(std::memory_order_relaxed);
}

GuestToken& guestToken() {
    static GuestToken token;
    return token;
}

}  // namespace lift
