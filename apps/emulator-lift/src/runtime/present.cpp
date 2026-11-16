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
// ★ 冻结副本的**上限**（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）：`pitch*height` 是 guest 给的数，
//   先查乘法溢出（uint32 × int32 ⇒ uint64 不会溢出）再查这个上限；> 64 MB 直接拒
//   （1280×720×32bpp = 3.7 MB，4K 一帧还有余）。拒绝 ⇒ 计数 + warn 一次 + **退回写现场**。
constexpr std::uint64_t kFreezeMaxBytes = 64ull * 1024ull * 1024ull;

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

void HeadlessPresenter::noteFreezeFailure(const std::string& why) {
    // ★ 与 `noteDumpFailure` 同一条口径：**计数** + **只第一次**喊。
    //   失败 ⇒ `frozen_valid_` 保持 false ⇒ 截图**退回写现场**（⛔ 不新增错误码、⛔ 不拿更旧的一帧冒充）。
    frozen_valid_ = false;
    ++freeze_failures_;
    if (!freeze_failure_warned_) {
        freeze_failure_warned_ = true;
        logAt(LogLevel::Warn, LogD3d9, "[present] ",
              "最后帧冻结失败（第 %llu 次 / 累计 %llu）：%s ⇒ 这一帧的 screenshot 退回写现场后缓冲"
              "（= 修复前的行为）；后续失败只计数（不刷屏），总数见 run summary 的 freezeFailures=",
              static_cast<unsigned long long>(freeze_failures_),
              static_cast<unsigned long long>(freeze_failures_), why.c_str());
    }
}

void HeadlessPresenter::setFreezeFrame(bool on) {
    freeze_ = on;
    // 关掉 ⇒ 副本立刻作废（⛔ 不拿"上一次开着的帧"冒充）。
    if (!on) frozen_valid_ = false;
}

void HeadlessPresenter::freezePresentedFrame(const SurfaceArena& arena, const GuestSurface& surface) {
    // ★ 尺寸校验（`pitch*height` 来自 guest）：负/零尺寸、超上限都直接拒。
    if (surface.width <= 0 || surface.height <= 0 || surface.pitch == 0) {
        noteFreezeFailure("尺寸非法（width/height/pitch 有 0 或负）");
        return;
    }
    const std::uint64_t bytes =
        static_cast<std::uint64_t>(surface.pitch) * static_cast<std::uint64_t>(surface.height);
    if (bytes > kFreezeMaxBytes) {
        noteFreezeFailure("尺寸超上限（pitch*height > 64 MB）");
        return;
    }
    const std::uint8_t* p = arena.pixels(surface);
    if (p == nullptr) {
        noteFreezeFailure("surface 没有像素");
        return;
    }
    try {
        frozen_pixels_.assign(p, p + bytes);
    } catch (...) {
        noteFreezeFailure("分配失败（std::bad_alloc）");
        return;
    }
    // ★ 描述符**一起存**：截图时⛔ 不从现场 surface 重算尺寸 / 格式 / pitch。
    frozen_desc_ = surface;
    frozen_valid_ = true;
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
    // 最后帧的**描述符**：永远记（退回路径要用）；像素**只在控制层启用时**拷成副本。
    last_arena_ = &arena;
    last_surface_ = surface;
    last_surface_hash_ = last_hash_;
    have_last_surface_ = true;

    // ★ **冻结点**（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`；design-review §2 裁决的落点）：
    //   位置 = `last_surface_hash_ = last_hash_` **之后**、sink 分支 **之前**。四条判据：
    //   (a) 在 `++frame_count_` 之后 ⇒ 冻结的就是这一帧；
    //   (b) 在 hash 之后 ⇒ 不动 `lastHash` 读数（`present-tail` / `frame-log` / §12.6 指纹口径逐位不变）；
    //   (c) 在 sink 之前 ⇒ 冻结字节可证明就是被 hash 的字节，且 sink 失败时 present 对 guest 仍算成功
    //       ⇒ "shot == 被呈现的那一帧"与窗口成败无关；
    //   (d) 在 Present 闸门之前 ⇒ 否则闸门那一拍没有副本。
    //   ⛔ 不要挪进 Present 闸门里（撞判据 5：每拍推进一帧）；⛔ `--dump-frame` 不走副本（独立对照）。
    //   缺省（没有控制层）⇒ 一次 `freeze_` 判空，零拷贝。
    if (freeze_) {
        const std::uint64_t freeze_begin = nowNs();
        freezePresentedFrame(arena, surface);
        pb.freeze_ns = nowNs() - freeze_begin;
    } else {
        pb.freeze_ns = 0;
    }

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
    // ★ **冻结副本路径**（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）：写 `present()` 当场冻的那一帧。
    //   ⛔ 不读 guest 后缓冲 —— 那块字节在 Present 之后会被下一帧的 `Clear(0)` / 回读改写；
    //   `--dump-frame` 之所以一直正常，是因为它写在 Present 之内（回读之后），那是**独立对照**。
    //   `hash_still_matches` = 在这份**将被写出的字节**上算 hash，与记录的 `last_surface_hash_` 比
    //   （同一帧冻的 ⇒ 恒 true：这张 BMP 就是 presenter 记录的那一帧）。
    if (freeze_ && frozen_valid_) {
        if (hash_still_matches != nullptr) {
            *hash_still_matches = hashPixels(frozen_desc_, frozen_pixels_.data()) == last_surface_hash_;
        }
        return dumpBmpRaw(frozen_desc_, frozen_pixels_.data(), host_path, err);
    }
    // ★ **退回路径**（没开冻结 / 冻结失败）：写现场 = 今天的行为，⛔ 不新增错误码 / 不新增 why。
    //   如实报告像素是否仍是被采样到的那一帧（guest 可能已经在对这块后缓冲画下一帧了）。
    if (hash_still_matches != nullptr) {
        *hash_still_matches =
            hashPixels(last_surface_, last_arena_->pixels(last_surface_)) == last_surface_hash_;
    }
    return last_arena_->dumpBmp(last_surface_, host_path, err);
}



}  // namespace lift
