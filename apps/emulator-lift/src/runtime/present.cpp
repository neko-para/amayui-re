#include "runtime/present.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

#include "runtime/log.h"
#include "runtime/input_plan.h"  // PlanEventKind::Frame（`--input-plan` 的 presents 门）
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

void HeadlessPresenter::noteDumpFailure(const std::string& path, const std::string& why) {
    // ★ 与 sink 失败同一条口径：**计数**（进 run summary 的 `dumpFailures=`）+ **只第一次**喊。
    //   ⛔ 不每帧刷屏：帧导出是宿主诊断，1280×720@60 下一旦坏了就是 60 行/秒。
    ++dump_failures_;
    if (!dump_failure_warned_) {
        dump_failure_warned_ = true;
        logAt(LogLevel::Warn, LogD3d9, "[present] ",
              "帧导出失败：%s（%s）⇒ 继续跑（guest 的 Present 不受影响）；后续失败只计数，"
              "见 run summary 的 dumpFailures=",
              path.c_str(), why.c_str());
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
    // ★ `--input-plan` 的 `presents` 门 emit 点：与 `++frame_count_` **同一刻**（guest 可见、确定）。
    //   ⛔ 这里只记账（`note()` 不接受 `WindowManager`）⇒ 不可能在 Present 里施加 step。
    if (input_plan_ != nullptr) input_plan_->note(PlanEventKind::Frame);
    // ★ `present-tail` 的**唯一**盖章点：与 `frame_count_` 同一刻记下 (insns / ns)。
    //   口径是"最后一次 Present **调用**"——这里在 sink / dump **之前**，所以 sink 失败的那次也算
    //   （与 `++frame_count_` 同一条契约）。只读解引用注入的计数器 ⇒ 不碰 guest 状态。
    last_present_insns_ = insn_source_ != nullptr ? *insn_source_ : 0;
    last_present_ns_ = nowNs();
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
        // ★ 帧导出是**宿主诊断**：写失败只记账（第一次 warn + 计数），⛔ **不**让 guest 的 Present 失败。
        //   为什么必须这样（REQ-01M73YBCC3DZ05Y9NSDTRKTMMY）：旧实现 `return false` ⇒ `d3d9::present`
        //   在 `--max-frames` 的停机检查**之前**返回 ⇒ 目录不存在时永远到不了帧数上限，进程空转到
        //   别的停止条件（实测 546 s）。目录已由 `main` 在开跑前建好（那是第一道闸），这里只兜
        //   "跑到一半写不进去"（磁盘满 / 权限被撤 / 目标是目录）—— 同样不许把整轮拖住。
        std::string dump_err;
        if (arena.dumpBmp(surface, dump_path, &dump_err)) {
            last_dump_ = dump_path;
        } else {
            noteDumpFailure(dump_path, dump_err);
            if (err != nullptr) *err = dump_err;  // 失败原因仍交给调用方（诊断用；不改变返回值）
        }
    }
    // ★ **控制层 Present 闸门**（S2）：位置 = sink（与 `--dump-frame`）**之后**、返回之前。
    //   理由（design-review §1 / §2b）：此刻 `++frame_count_` 与 `last_surface_hash_` 已定稿、
    //   帧已交给 sink（`paused_at:"present"` 因此成立）；⛔ 不放在 sink 之前 —— 那样窗口里会是上一帧。
    //   回调可以阻塞（`pause` / `sample`）；没接控制层时只是**一次判空**。
    if (gate_fn_ != nullptr) {
        try {
            (void)gate_fn_(gate_user_);
        } catch (...) {
            // 控制层是只读宿主面：它的异常**不许**改变 guest 的 Present 结果（与 sink 同一条兜底）。
        }
    }
    return true;
}

void HeadlessPresenter::noteControlResume() {
    if (sink_ != nullptr) sink_->noteControlResume();
}

const char* HeadlessPresenter::controlStopReason() const {
    return sink_ != nullptr ? sink_->controlStopReason() : nullptr;
}

// ★ 运行摘要 `present-tail` 的墙钟：最后一次 Present 那一刻 → **现在**（`dumpRunSummary` 在 run 收口
//   之后才读，所以"现在"就是收口时刻）。还没 Present 过 ⇒ 0.0（⛔ 不拿 epoch 差冒充）。
double HeadlessPresenter::msSinceLastPresent() const {
    if (last_present_ns_ == 0) return 0.0;
    return static_cast<double>(nowNs() - last_present_ns_) / 1e6;
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
