#include "runtime/present.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

#include "runtime/screen_sample.h"

namespace lift {
namespace {
constexpr std::uint64_t kLatchAfterFailures = 3;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

PresentBreakdown& presentBreakdown() {
    static PresentBreakdown breakdown;
    return breakdown;
}

HeadlessPresenter::HeadlessPresenter() = default;
HeadlessPresenter::~HeadlessPresenter() = default;

void HeadlessPresenter::setSink(std::unique_ptr<PresentSink> sink) {
    sink_ = std::move(sink);
    sink_failures_ = 0;
    last_sink_error_.clear();
    sink_latched_ = false;
}

void HeadlessPresenter::clearSink() { setSink(nullptr); }

void HeadlessPresenter::noteSinkFailure(const std::string& why) {
    ++sink_failures_;
    last_sink_error_ = why.empty() ? "sink 未给出原因" : why;
    if (sink_failures_ >= kLatchAfterFailures) sink_latched_ = true;
    if (sink_failures_ == 1) {
        // 第一次失败就要可见；latch 后只是停止 submitFrame，窗口事件泵/关窗仍然继续。
        std::fprintf(stderr, "[present] sink 失败：%s（后续帧继续尝试；连续 %llu 次后停止提交）\n",
                     last_sink_error_.c_str(), (unsigned long long)kLatchAfterFailures);
    }
}

bool HeadlessPresenter::present(const SurfaceArena& arena, const GuestSurface& surface,
                                const std::string& dump_path, std::string* err) {
    // ★ 先记录（与今天完全一致）：显示失败不能改变 frameCount / lastHash / dump。
    PresentBreakdown& pb = presentBreakdown();
    const std::uint64_t hash_begin = nowNs();
    last_hash_ = arena.hash(surface);
    pb.hash_ns = nowNs() - hash_begin;
    ++frame_count_;
    last_dump_.clear();

    // ★ 采样点（交互模型的 `screen` 源）：**只在这里**。帧号 / hash 已经算好 ⇒ 判定侧拿到的
    //   就是"这一次呈现"的真实观测值；没挂 sampler 时整段是零成本。
    if (sampler_ != nullptr) sampler_->observe(arena, surface, frame_count_, last_hash_);
    // 最后帧的**描述符**（不拷像素）：失败时要补写一张画面，见 dumpLastFrame()。
    last_arena_ = &arena;
    last_surface_ = surface;
    last_surface_hash_ = last_hash_;
    have_last_surface_ = true;

    if (sink_ && !sink_latched_) {
        std::string sink_err;
        bool ok = false;
        const std::uint64_t sink_begin = nowNs();
        try {
            ok = sink_->submitFrame(arena, surface, &sink_err);
        } catch (const std::exception& e) {
            sink_err = std::string("sink 异常：") + e.what();
        } catch (...) {
            sink_err = "sink 异常（非 std）";
        }
        pb.sink_ns = nowNs() - sink_begin;
        if (!ok) noteSinkFailure(sink_err);
    } else {
        pb.sink_ns = 0;
    }

    if (!dump_path.empty()) {
        if (!arena.dumpBmp(surface, dump_path, err)) return false;
        last_dump_ = dump_path;
    }
    return true;
}

void HeadlessPresenter::pumpHostEvents() {
    // ★ latch 后仍要泵事件：否则失败后窗口关不掉（事件永远不进 SDL 队列）。
    if (!sink_) return;
    try {
        sink_->pumpHostEvents();
    } catch (...) {
        noteSinkFailure("sink 事件泵异常");
    }
}

bool HeadlessPresenter::stopRequested() const {
    // ★ latch 后仍要读关窗请求：失败回退 headless 不能把窗口变成“关不掉”。
    if (!sink_) return false;
    try {
        return sink_->stopRequested();
    } catch (...) {
        return false;
    }
}

std::string HeadlessPresenter::sinkDescription() const { return sink_ ? sink_->describe() : std::string(); }

bool HeadlessPresenter::dumpLastFrame(const std::string& host_path, bool* hash_still_matches,
                                      std::string* err) const {
    if (!have_last_surface_ || last_arena_ == nullptr) {
        if (err) *err = "还没有 Present 过（没有最后帧可写）";
        return false;
    }
    // ★ 如实报告像素是否仍是**被采样到的那一帧**：guest 可能已经在对这块后缓冲画下一帧了。
    //   ⛔ 不一致也照样写（诊断价值大于"干净"），但调用方要把这句话打出来。
    if (hash_still_matches != nullptr) {
        *hash_still_matches = last_arena_->hash(last_surface_) == last_surface_hash_;
    }
    return last_arena_->dumpBmp(last_surface_, host_path, err);
}


}  // namespace lift
