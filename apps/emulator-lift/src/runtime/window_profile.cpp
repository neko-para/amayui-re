// emulator-lift · 区间 profiler 的实现（口径见 window_profile.h 的文件头）
//
// ★ 这里**不做任何打印格式的活**：打印在 machine.cpp 的 `printWindowReport`（它才有 `.lst`
//   符号与 shim 名字）。本文件只负责"把墙钟切进互斥的桶里"。
#include "runtime/window_profile.h"

#include <algorithm>
#include <cstdio>

#include "runtime/x86.h"
#include "runtime/present.h"
#include "runtime/shims.h"

namespace lift {
namespace {

std::uint64_t nsBetween(WindowProfiler::Clock::time_point a, WindowProfiler::Clock::time_point b) {
    if (b <= a) return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

}  // namespace

const char* WindowProfiler::hostSlotName(int slot) {
    switch (slot) {
        case HostAudio: return "audio";
        case HostEventPump: return "event-pump";
        case HostInput: return "input-script";
        case HostTimer: return "timer";
        default: return "?";
    }
}

void WindowProfiler::begin(const std::string& label, std::uint64_t frame, std::uint64_t insns,
                           std::uint64_t guest_ms) {
    label_ = label;
    begin_wall_ = Clock::now();
    begin_frame_ = frame;
    begin_insns_ = insns;
    begin_guest_ms_ = guest_ms;
    guest_ns_ = 0;
    shim_ns_ = 0;
    native_ns_ = 0;
    interp_ns_ = 0;
    for (std::uint64_t& v : host_ns_) v = 0;
    iterations_ = 0;
    iter_attributed_ns_ = 0;
    iter_start_ = begin_wall_;
    prev_end_valid_ = false;
    block_ns_delta_.clear();
    stack_.clear();
    incl_ns_.clear();
    top_calls_.clear();
    calls_by_fn_.clear();
    unbalanced_ = 0;
    open_at_end_ = 0;
    active_ = true;
    finished_ = false;
}

void WindowProfiler::end(std::uint64_t frame, std::uint64_t insns, std::uint64_t guest_ms) {
    if (!active_) return;
    end_wall_ = Clock::now();
    end_frame_ = frame;
    end_insns_ = insns;
    end_guest_ms_ = guest_ms;
    // 窗口结束时还开着的调用帧：按"到现在为止"收口并计数（⛔ 不静默丢）。
    closeAllFrames(end_wall_, true);
    active_ = false;
    finished_ = true;
}

void WindowProfiler::snapshotBlocks(const BlockCache& cache) {
    block_ns_at_begin_.clear();
    for (const auto& kv : cache.guestBlocks()) {
        if (kv.second) block_ns_at_begin_.emplace(kv.first, kv.second->ns);
    }
}

void WindowProfiler::snapshotShims(const ShimRegistry& shims) {
    shim_ns_at_begin_.clear();
    shim_calls_at_begin_.clear();
    for (const ShimDef& d : shims.all()) {
        shim_ns_at_begin_.push_back(d.ns);
        shim_calls_at_begin_.push_back(d.call_count);
    }
}

void WindowProfiler::snapshotPresent() {
    const PresentBreakdown& pb = presentBreakdown();
    present_at_begin_[0] = pb.readback_ns;
    present_at_begin_[1] = pb.hash_ns;
    present_at_begin_[2] = pb.stats_ns;
    present_at_begin_[3] = pb.sink_ns;
}

void WindowProfiler::finishBlocks(const BlockCache& cache) {
    block_ns_delta_.clear();
    for (const auto& kv : cache.guestBlocks()) {
        if (!kv.second) continue;
        const auto it = block_ns_at_begin_.find(kv.first);
        const std::uint64_t before = (it == block_ns_at_begin_.end()) ? 0 : it->second;
        if (kv.second->ns > before) block_ns_delta_.emplace(kv.first, kv.second->ns - before);
    }
}

void WindowProfiler::finishShims(const ShimRegistry& shims) {
    const std::vector<ShimDef>& defs = shims.all();
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const std::uint64_t before_ns = i < shim_ns_at_begin_.size() ? shim_ns_at_begin_[i] : 0;
        const std::uint64_t before_calls = i < shim_calls_at_begin_.size() ? shim_calls_at_begin_[i] : 0;
        const std::uint64_t ns = defs[i].ns > before_ns ? defs[i].ns - before_ns : 0;
        const std::uint64_t calls = defs[i].call_count > before_calls ? defs[i].call_count - before_calls : 0;
        if (ns == 0 && calls == 0) continue;
        // 只留"有耗时或有调用"的项；打印时再排序取前 N。
        shim_rows_.push_back(WindowShimRow{defs[i].id, ns, calls});
    }
}

void WindowProfiler::finishPresent() {
    const PresentBreakdown& pb = presentBreakdown();
    auto delta = [](std::uint64_t now, std::uint64_t before) { return now > before ? now - before : 0ull; };
    gpu_readback_ns_ = delta(pb.readback_ns, present_at_begin_[0]);
    gpu_hash_ns_ = delta(pb.hash_ns, present_at_begin_[1]);
    gpu_stats_ns_ = delta(pb.stats_ns, present_at_begin_[2]);
    gpu_sink_ns_ = delta(pb.sink_ns, present_at_begin_[3]);
}

bool WindowProfiler::takeReport(const std::function<std::uint32_t(std::uint32_t)>& fn_key,
                                WindowReport* out) {
    if (!finished_ || out == nullptr) return false;
    WindowReport r;
    r.label = label_;
    r.wall_ns = nsBetween(begin_wall_, end_wall_);
    r.guest_ns = guest_ns_;
    r.shim_ns = shim_ns_;
    r.native_ns = native_ns_;
    r.interp_ns = interp_ns_;
    for (int i = 0; i < HostSlotCount; ++i) {
        r.host_ns[i] = host_ns_[i];
        r.host_total_ns += host_ns_[i];
    }
    const std::uint64_t sum = r.guest_ns + r.shim_ns + r.native_ns + r.interp_ns + r.host_total_ns;
    r.unattributed_ns = static_cast<std::int64_t>(r.wall_ns) - static_cast<std::int64_t>(sum);
    r.begin_frame = begin_frame_;
    r.end_frame = end_frame_;
    r.begin_insns = begin_insns_;
    r.end_insns = end_insns_;
    r.begin_guest_ms = begin_guest_ms_;
    r.end_guest_ms = end_guest_ms_;
    r.iterations = iterations_;
    r.unbalanced = unbalanced_;
    r.open_at_end = open_at_end_;
    r.fns.resize(0);
    r.shims.resize(0);

    // ---- guest 函数表：self（块增量，按函数入口聚合）----
    {
        std::unordered_map<std::uint32_t, std::uint64_t> self;
        for (const auto& kv : block_ns_delta_) {
            const std::uint32_t key = fn_key(kv.first);
            self[key] += kv.second;
        }
        for (const auto& kv : self) {
            WindowFnRow row;
            row.key = kv.first;
            row.self_ns = kv.second;
            const auto it = incl_ns_.find(kv.first);
            if (it != incl_ns_.end()) row.inclusive_ns = it->second;
            const auto ct = calls_by_fn_.find(kv.first);
            if (ct != calls_by_fn_.end()) row.calls = ct->second;
            r.fns.push_back(row);
        }
        // 有 inclusive 但没有 self 的（例如整段被 override 掉）也补一行 —— ⛔ 不丢证据。
        for (const auto& kv : incl_ns_) {
            if (self.find(kv.first) != self.end()) continue;
            WindowFnRow row;
            row.key = kv.first;
            row.inclusive_ns = kv.second;
            const auto ct = calls_by_fn_.find(kv.first);
            if (ct != calls_by_fn_.end()) row.calls = ct->second;
            r.fns.push_back(row);
        }
        std::sort(r.fns.begin(), r.fns.end(), [](const WindowFnRow& a, const WindowFnRow& b) {
            if (a.self_ns != b.self_ns) return a.self_ns > b.self_ns;
            if (a.inclusive_ns != b.inclusive_ns) return a.inclusive_ns > b.inclusive_ns;
            return a.key < b.key;
        });
    }

    // ---- shim 表（增量）----
    r.shims = shim_rows_;
    std::sort(r.shims.begin(), r.shims.end(), [](const WindowShimRow& a, const WindowShimRow& b) {
        if (a.ns != b.ns) return a.ns > b.ns;
        if (a.calls != b.calls) return a.calls > b.calls;
        return a.id < b.id;
    });
    // Present 次数由 Machine 打印时补（只有它认得出 `com!IDirect3DDevice9::Present`）
    r.gpu_readback_ns = gpu_readback_ns_;
    r.gpu_hash_ns = gpu_hash_ns_;
    r.gpu_stats_ns = gpu_stats_ns_;
    r.gpu_sink_ns = gpu_sink_ns_;

    *out = std::move(r);
    // 清状态：⛔ 不留在那里被下一个窗口误用
    finished_ = false;
    block_ns_at_begin_.clear();
    block_ns_delta_.clear();
    shim_ns_at_begin_.clear();
    shim_calls_at_begin_.clear();
    shim_rows_.clear();
    incl_ns_.clear();
    top_calls_.clear();
    calls_by_fn_.clear();
    stack_.clear();
    return true;
}

void WindowProfiler::addGuest(std::uint64_t ns) {
    if (!active_) return;
    guest_ns_ += ns;
    iter_attributed_ns_ += ns;
}

void WindowProfiler::addShim(std::uint64_t ns) {
    if (!active_) return;
    shim_ns_ += ns;
    iter_attributed_ns_ += ns;
}

void WindowProfiler::addNative(std::uint64_t ns) {
    if (!active_) return;
    native_ns_ += ns;
    iter_attributed_ns_ += ns;
}

void WindowProfiler::addHost(int slot, std::uint64_t ns) {
    if (!active_) return;
    if (slot < 0 || slot >= HostSlotCount) return;
    host_ns_[slot] += ns;
    iter_attributed_ns_ += ns;
}

void WindowProfiler::iterationBegin(Clock::time_point now) {
    if (!active_) return;
    // ★ 起点优先用**上一个迭代的终点**：两次迭代之间的那几十纳秒（退出守卫 → while 条件 →
    //   循环体开头）也是窗口内的墙钟，丢掉它就会在 `unattributed` 里累积成整毫秒。
    iter_start_ = prev_end_valid_ ? prev_end_ : now;
    iter_attributed_ns_ = 0;
    ++iterations_;
}

void WindowProfiler::iterationEnd(Clock::time_point now) {
    if (!active_) return;
    const std::uint64_t elapsed = nsBetween(iter_start_, now);
    // 已记段比整段还长 ⇒ 有重叠（例如 shim 里又嵌套了块）。⛔ 不在这里吞掉：
    // 少记的部分由 unattributed 暴露（它会是负的）。
    if (elapsed > iter_attributed_ns_) interp_ns_ += elapsed - iter_attributed_ns_;
    prev_end_ = now;
    prev_end_valid_ = true;
    iter_start_ = now;
    iter_attributed_ns_ = 0;
}

void WindowProfiler::attributeFrame(const Frame& f, Clock::time_point now) {
    if (f.fn_key == 0) return;
    const std::uint64_t dur = nsBetween(f.start, now);
    auto it = top_calls_.find(f.fn_key);
    if (it != top_calls_.end() && it->second > 0) --it->second;
    // ★ 递归：只算**最外层**那一帧 ⇒ 同一函数的 inclusive 不会把内层帧的时间算两遍。
    //   判据 = 弹出这一刻，栈上是否还有同函数的帧（`top_calls_` 在压帧时已 +1）。
    if (it == top_calls_.end() || it->second == 0) {
        incl_ns_[f.fn_key] += dur;
    }
}

void WindowProfiler::closeAllFrames(Clock::time_point now, bool count_open) {
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) attributeFrame(*it, now);
    if (count_open) open_at_end_ += stack_.size();
    stack_.clear();
    top_calls_.clear();
}

void WindowProfiler::pushCall(std::uint32_t fn_key, std::uint32_t ret_ea) {
    if (!active_) return;
    Frame f;
    f.fn_key = fn_key;
    f.ret_ea = ret_ea;
    f.start = Clock::now();
    stack_.push_back(f);
    // 调用计数只在**最外层**那一次进入时 +1（递归的内层帧不算一次新的"进入"）。
    if (top_calls_[fn_key]++ == 0) ++calls_by_fn_[fn_key];
}

void WindowProfiler::popRet(std::uint32_t eip) {
    if (!active_) return;
    if (stack_.empty()) return;  // 最外层 `ret` 落到哨兵：没有帧可弹（不是错误）
    if (stack_.back().ret_ea == eip) {
        Frame f = stack_.back();
        stack_.pop_back();
        attributeFrame(f, Clock::now());
        return;
    }
    // ★ 对不上：可能是尾调用（jmp 进另一个函数）或宿主铺的 guest 回调帧。
    //   往下找"返回地址正好是 eip"的帧；找到就把它上面的都收掉（那些是误压的）。
    for (std::size_t i = stack_.size(); i > 0; --i) {
        if (stack_[i - 1].ret_ea != eip) continue;
        const auto now = Clock::now();
        while (stack_.size() > i) {
            Frame f = stack_.back();
            stack_.pop_back();
            ++unbalanced_;
            attributeFrame(f, now);
        }
        Frame f = stack_.back();
        stack_.pop_back();
        attributeFrame(f, now);
        return;
    }
    ++unbalanced_;  // 解不出：只计数，⛔ 不瞎弹
}

void WindowProfiler::popNoReturn(std::uint32_t fn_key) {
    if (!active_) return;
    if (stack_.empty()) {
        ++unbalanced_;
        return;
    }
    if (stack_.back().fn_key == fn_key) {
        Frame f = stack_.back();
        stack_.pop_back();
        attributeFrame(f, Clock::now());
        return;
    }
    ++unbalanced_;
}

}  // namespace lift
