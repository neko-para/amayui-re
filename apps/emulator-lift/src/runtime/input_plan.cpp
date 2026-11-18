// emulator-lift · 输入投递层（AOT 臂：在 guest 读点推进配方）—— 口径见 `input_plan.h` 的头注。
//
// ⛔ 本文件**不是**第二份输入状态：状态只有 `WindowManager` 一份。这里只有"到期了没有"的推进 +
//   计数 + 待抬起队列；施加输入的**每一处**都走 `runtime/input.h` 既有的 `input::*`（与 SDL /
//   旧脚本同一条路径）。
// ★ **解析与门判定不在这里**：配方是 `runtime/recipe.h` 的那一份（**唯一一份**解析器与 step 模型；
//   机器内驱动器 `InteractionGraph` 读的也是它）。本文件只做两件事：① 到点把指令施加到
//   `WindowManager`；② 采身份 / 读位置（后者与 `readScriptPositionSample` 同一份实现）。
// ★ 墙钟只出现在 **`wait`** 一处（宿主单调时钟，非阻塞 deadline）；两个门只看 guest 内存
//   （脚本身份 + PC），⛔ 不看时钟、不看帧号。
#include "runtime/input_plan.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/clock.h"       // VirtualClock（暂停补偿的唯一读数：pausedTotalMs）
#include "runtime/cursor_map.h"  // referenceToClientPixel（1280×720 参考系 → 客户区，唯一公式）
#include "runtime/input.h"       // input::mouseMove / mouseButton（唯一施加路径）
#include "runtime/memory.h"      // GuestMemory（身份采样只经 MarkerSampler 读它）
#include "runtime/vfs.h"         // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

// 待抬起的**兜底**刻度：连续这么多个读点都没有 `inputSampleCount` 前进（队列类读点不采样）
// ⇒ 仍然抬起。定数、非时钟（`wait` 是唯一读墙钟的指令）。
constexpr std::uint64_t kForceReleaseReads = 16;

// 计划消息的 `MSG.time`：**没有** guest 时钟来源（plan 不在 tick 上下文里被调用）⇒ 恒 0。
// 引擎不读它（`interaction-model.md` 的门不碰 MSG.time）。
constexpr std::uint32_t kPlanMessageTimeMs = 0;

// 与 `present.cpp` 的 `nowNs()` 同源：宿主单调时钟（⛔ 不是 `clock.h` 的 guest 虚拟时钟）。
std::uint64_t hostNowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string u64s(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

// `--input-plan-trace` 的 `kind=` 词表（**单 token**，⛔ 不用 `stepKindName` 的空格式：
// trace 行要能被 `grep kind=click` 这类机械判据切）。
const char* traceKindToken(StepKind k) {
    switch (k) {
        case StepKind::AwaitScript: return "await-script";
        case StepKind::AwaitPosition: return "await-position";
        case StepKind::Wait: return "wait";
        case StepKind::Click: return "click";
        case StepKind::ProfileBegin: return "profile-begin";
        case StepKind::ProfileEnd: return "profile-end";
        case StepKind::Quit: return "quit";
    }
    return "?";
}

const char* mouseButtonToken(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
    }
    return "?";
}

}  // namespace

const char* inputPointName(InputPoint point) {
    switch (point) {
        case InputPoint::KeyLevel: return "KeyLevel";
        case InputPoint::CursorPos: return "CursorPos";
        case InputPoint::PeekMsg: return "PeekMessage";
        case InputPoint::GetMsg: return "GetMessage";
        case InputPoint::MsgWait: return "MsgWait";
        case InputPoint::WaitMessage: return "WaitMessage";
        case InputPoint::QueueStatus: return "QueueStatus";
        case InputPoint::Translate: return "Translate";
        case InputPoint::Dispatch: return "Dispatch";
        case InputPoint::SetCursorPos: return "SetCursorPos";
        case InputPoint::FileRead: return "FileRead";
        case InputPoint::Count: break;
    }
    return "?";
}

void InputPlan::clear() {
    recipe_.clear();
    pending_.clear();
    immediate_.clear();
    identity_ = MarkerSample{};
    next_ = 0;
    applied_ = 0;
    fired_ = 0;
    released_ = 0;
    position_misses_ = 0;
    read_points_ = 0;
    releases_pending_ = 0;
    wait_started_ = false;
    wait_deadline_ms_ = 0;
    guard_armed_ = false;
    guard_started_ms_ = 0;
    guard_ms_ = 0;
    failed_ = false;
    failed_index_ = 0;
    failed_elapsed_ms_ = 0;
    failed_guard_ms_ = 0;
    quit_requested_ = false;
    profile_open_ = false;
    profile_label_.clear();
    profile_started_ms_ = 0;
    profile_reads_at_begin_ = 0;
    profile_windows_ = 0;
    deliver_calls_.fill(0);
    event_calls_.fill(0);
}

void InputPlan::attachGuestMemory(const GuestMemory* mem) {
    // 只换来源、不重置计数：装配点在解析之后、`host.init()` 之后（见头注 H7）。
    mem_ = mem;
    identity_ = MarkerSample{};
    marker_sampler_.resetCounters();
}

void InputPlan::setClockForTest(NowFn now_ms) { now_fn_ = now_ms; }

void InputPlan::attachClock(const VirtualClock* clock) { clock_ = clock; }

std::uint64_t InputPlan::nowMs() const {
    const std::uint64_t raw = now_fn_ != nullptr ? now_fn_() : hostNowMs();
    if (clock_ == nullptr) return raw;  // 缺省档：逐位不变（没有控制层就没有暂停）
    // ★ 暂停区间**不计入**配方守卫钟（裁决：`body-m0.md` 末节）。
    const std::uint64_t paused = clock_->pausedTotalMs();
    return raw > paused ? raw - paused : 0;
}

bool InputPlan::loadFile(const std::string& path, std::string* err) {
    Recipe r;
    if (!r.loadFile(path, err)) {
        clear();
        return false;
    }
    clear();
    recipe_ = std::move(r);
    return true;
}

bool InputPlan::loadText(const std::string& text, const std::string& origin, std::string* err) {
    Recipe r;
    if (!r.loadText(text, origin, err)) {
        clear();
        return false;
    }
    clear();
    recipe_ = std::move(r);
    return true;
}

// 同 `(cur,id)` + 名字表签名不变 ⇒ 复用缓存的名字（否则每个读点都要付一次 CP932 解析）。
void InputPlan::refreshIdentity() {
    if (mem_ == nullptr) {
        identity_ = MarkerSample{};
        return;
    }
    const MarkerSample ident = marker_sampler_.sampleIdentity(*mem_);
    if (ident.valid && identity_.valid && ident.cur == identity_.cur && ident.id == identity_.id &&
        identity_.name_table_sig == marker_sampler_.nameTableSignature(*mem_, ident.id)) {
        return;  // 同一身份 ⇒ 上次解析出的名字仍有效
    }
    MarkerSample s = ident;
    if (s.valid) marker_sampler_.resolveName(*mem_, &s);
    identity_ = std::move(s);
}

void InputPlan::atPoint(WindowManager& wm, InputPoint point) {
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < deliver_calls_.size()) ++deliver_calls_[i];
    ++read_points_;
    // ★ 早退条件多了**立即队列非空**这一项：控制层的 click 可能在**没有配方**的运行里进来
    //   （队列恒空时这里与改动前逐位相同 ⇒ 默认档不变）。
    if (!recipe_.active() && pending_.empty() && immediate_.empty()) return;
    // ① 到期 / 兜底的**待抬起**先落地（同一次读点内、读之前）。
    releaseDue(wm, point);
    // ①' ★ **立即队列先于配方当步**（判据 2 的前半）：控制层的 click 在这里施加，
    //    走**同一段** `applyClickAt`、计入 `fired_`，但**不动** `next_` / `applied_` / 守卫账。
    //    放在 `quit_requested_` / `failed_` 的早退**之前**：控制层已经对客户端答了 `ok:true`，
    //    不该因为**配方自己的**失败 / quit 把它静默丢掉（它只等下一个读点）。
    drainImmediate(wm, point);
    // ② `quit` 之后不再推进；守卫到期之后同样不再推进（⛔ 不许"失败后继续点"）。
    if (quit_requested_ || failed_) return;
    // ③ flush-before-read：按文件顺序把到期指令消费到 `WindowManager`（⛔ 不越过未到期的指令）。
    bool identity_fresh = false;
    while (next_ < recipe_.size()) {
        const Step& s = recipe_.steps()[static_cast<std::size_t>(next_)];
        // ★ **每步守卫**（共享宽度 = `stepTimeoutMs`）：本臂只有一把尺 = 宿主墙钟。
        //   本臂不给自己缺省值（0）⇒ 没写 `timeout` 的配方在这里一次时钟也不读（零成本）。
        if (!guard_armed_) {
            const std::uint64_t guard = stepTimeoutMs(s, 0);
            if (guard != 0) {
                guard_armed_ = true;
                guard_ms_ = guard;
                guard_started_ms_ = nowMs();
            }
        }
        if (guard_armed_) {
            const std::uint64_t elapsed = nowMs() - guard_started_ms_;
            if (stepWindowExpired(elapsed, guard_ms_)) {
                failTimeout(static_cast<std::size_t>(next_), elapsed, guard_ms_);
                return;
            }
        }
        if (s.isGate()) {
            // ★ **电平**门：身份当前就是它 ⇒ 通过；身份读不到（base==0 / valid=false）⇒ 不成立。
            if (mem_ == nullptr) {
                if (s.kind == StepKind::AwaitPosition) ++position_misses_;
                return;  // 没有身份来源 ⇒ 继续等（⛔ 不报错）
            }
            if (!identity_fresh) {
                refreshIdentity();
                identity_fresh = true;
            }
            if (!identity_.valid) {
                if (s.kind == StepKind::AwaitPosition) ++position_misses_;
                return;
            }
            // ★ 位置是**同一层**的第二个条件：script 匹配 **且** `FROM <= pos < TO`。
            //   读不到 ⇒ 不成立（继续等）；⛔ 不报错、不超时、不施加。
            ScriptPositionSample pos;
            if (s.kind == StepKind::AwaitPosition) {
                pos = readScriptPositionSample(*mem_, identity_);
                if (!pos.valid) {
                    ++position_misses_;
                    return;
                }
            }
            if (!stepGateHolds(s, &identity_, pos)) return;
        } else if (s.kind == StepKind::Wait) {
            // ★ **非阻塞**：只记 deadline（起点 = 第一次到达这条指令的读点）；到期由下个读点消费。
            const std::uint64_t now = nowMs();
            if (!wait_started_) {
                wait_started_ = true;
                wait_deadline_ms_ = now + s.wait_ms;
            }
            if (now < wait_deadline_ms_) return;
            wait_started_ = false;
        } else if (s.kind == StepKind::Click) {
            applyClick(wm, s);
            ++fired_;
            // trace 的 `plan:<文件>:<行>` 要指向**施加它的那条 step** ⇒ 记在待抬起的记录上
            // （抬起那一行因此也能点名同一个 `文件:行`；立即队列的 click 没有 step ⇒ 0）。
            if (!pending_.empty()) pending_.back().line = s.line;
        } else if (s.kind == StepKind::ProfileBegin || s.kind == StepKind::ProfileEnd) {
            applyProfileMark(s, nowMs());
        } else {  // Quit
            applyQuit(wm);
        }
        ++applied_;
        ++next_;
        guard_armed_ = false;  // 下一条指令重新起算（同一次读点内继续 flush）
        traceStepConsumed(s, wm.inputSampleCount(), point, "");  // ★ opt-in（缺省 nullptr ⇒ 一次判空、零输出）
        // ★ `quit` 之后不再推进（`InputScript` 的 `if (quit_requested_) continue` 同口径）：
        //   但它**自己**已经算走完 ⇒ 末尾的 `quit` 不会让 pending 变非 0。
        if (quit_requested_) return;
    }
}

bool InputPlan::enqueueClick(int ref_x, int ref_y, MouseButton button) {
    // ★ **有界**（判据 5）：满了就返回 false，由控制层回 `bad_request`。⛔ 不丢旧的、⛔ 不静默扩容器。
    //   ⚠ 这里**不做映射**：参考坐标原样排队，映射只在施加那一刻由 `applyClickAt` 做**一次**
    //   （否则"排队时的客户端尺寸"与"施加时的客户端尺寸"会变成两套口径）。
    if (immediate_.size() >= kImmediateClickCapacity) return false;
    ImmediateClick c;
    c.x = ref_x;
    c.y = ref_y;
    c.button = button;
    immediate_.push_back(c);
    return true;
}

void InputPlan::drainImmediate(WindowManager& wm, InputPoint point) {
    // ★ 只消费立即队列：**不碰** `next_` / `applied_` / `wait_started_` / 守卫账 ⇒ 配方里未到期的门
    //   照旧等（判据 2 的"步进指针不动"）。每条 +1 `fired_`（与配方 click 同一读数口径）。
    while (!immediate_.empty()) {
        const ImmediateClick c = immediate_.front();
        immediate_.erase(immediate_.begin());
        applyClickAt(wm, c.x, c.y, c.button);
        ++fired_;
        // ★ trace（opt-in）：控制层的 click 与配方 click 走同一段施加核心 ⇒ 读数也要能直接对齐
        //   （D 的判据面：同一屏上"控制层 click 落在哪种读点"是唯一能解释两边差异的读数）。
        //   行里 `origin=control` + `plan:<文件>:0`（0 = 不是配方 step）点名它。
        if (trace_out_ != nullptr) {
            Step control_step;
            control_step.kind = StepKind::Click;
            control_step.line = 0;
            traceStepConsumed(control_step, wm.inputSampleCount(), point, " origin=control");
        }
    }
}

void InputPlan::applyClick(WindowManager& wm, const Step& s) {
    // 配方 step 的壳：只把 `Step` 里的三个值交给**同一段**核心（⛔ 不复制映射 / 记账）。
    applyClickAt(wm, s.x, s.y, s.button);
}

void InputPlan::applyClickAt(WindowManager& wm, int ref_x, int ref_y, MouseButton button) {
    // ★ 这是**唯一**的 click 施加核心（配方 step 与控制层立即队列都到这里；判据 1 的"同一 apply 通道"）。
    // ★ 坐标**算一次**：`mouseMove` / `mouseButton(down)` / 之后抬起都读同一对值
    //   （⛔ 两处各算一次 ⇒ move 到 A、消息 lParam 是 B 的静默漂移）。
    int client_x = 0;
    int client_y = 0;
    int target_w = 0;
    int target_h = 0;
    if (const HostWindow* w = wm.find(wm.primaryWindow())) {
        target_w = w->client_w;  // 0 / 没有窗口 ⇒ referenceToClientPixel 退回 1280×720
        target_h = w->client_h;
    }
    referenceToClientPixel(target_w, target_h, ref_x, ref_y, &client_x, &client_y);
    // ★ 与 `input.cpp` 的脚本路径同一段代码：先光标（消息 `WM_MOUSEMOVE` + 电平），再按下。
    input::mouseMove(wm, client_x, client_y, InputSource::Script, kPlanMessageTimeMs);
    PendingRelease r;
    r.button = button;
    r.x = client_x;
    r.y = client_y;
    r.press_samples = wm.inputSampleCount();
    r.press_reads = read_points_;
    input::mouseButton(wm, r.x, r.y, r.button, /*down=*/true, InputSource::Script, kPlanMessageTimeMs);
    pending_.push_back(r);
    ++releases_pending_;
}

void InputPlan::applyProfileMark(const Step& s, std::uint64_t now_ms) {
    // ★ 本臂**没有**区间 profiler（`WindowProfiler` 是解释器块循环的仪器：它的分项来自
    //   `BlockCache` / `ShimRegistry` / 派发循环，AOT 臂一个都没有）⇒ 落成**宿主墙钟区间读数**：
    //   begin / end 各一行一次性输出，区间宽度可与 `--profile` 的整轮读数 / `frame-ms:` 对齐。
    //   ⛔ 不假装开了窗口 profiler（那会是"看起来有分项、其实是空的"假读数）。
    if (s.kind == StepKind::ProfileBegin) {
        if (profile_open_) {
            std::fprintf(stderr,
                         "[profile-window] ⚠ 上一个 profile_begin(%s) 还没收口，忽略 %s（行 %d）\n",
                         profile_label_.c_str(), s.label.c_str(), s.line);
            return;
        }
        profile_open_ = true;
        profile_label_ = s.label;
        profile_started_ms_ = now_ms;
        profile_reads_at_begin_ = read_points_;
        std::printf("[profile-window] begin label=%s wall_ms=%s reads=%s（AOT 臂：区间读数，不是分项 profiler）\n",
                    s.label.c_str(), u64s(now_ms).c_str(), u64s(read_points_).c_str());
        std::fflush(stdout);
        // ★ 窗口 profile 的开窗（唯一外部消费者；见 `setProfileMarkHook`）。
        if (mark_fn_ != nullptr) mark_fn_(mark_ctx_, s.label.c_str(), /*begin=*/true);
        return;
    }
    if (!profile_open_) {
        std::fprintf(stderr, "[profile-window] ⚠ profile_end(%s) 没有对应的 profile_begin（行 %d）⇒ 忽略\n",
                     s.label.c_str(), s.line);
        return;
    }
    const std::uint64_t elapsed = now_ms - profile_started_ms_;
    const std::uint64_t reads = read_points_ - profile_reads_at_begin_;
    if (s.label != profile_label_) {
        // label 只用于对齐 / 报错（与解释器臂同口径）⇒ 不匹配是**响亮**的提示，不是失败。
        std::fprintf(stderr,
                     "[profile-window] ⚠ profile_end(%s) 与 begin(%s) 的 label 不一致（行 %d）；仍按同一窗口收口\n",
                     s.label.c_str(), profile_label_.c_str(), s.line);
    }
    std::printf("[profile-window] end label=%s wall_ms=%s elapsed_ms=%s reads=%s\n", profile_label_.c_str(),
                u64s(now_ms).c_str(), u64s(elapsed).c_str(), u64s(reads).c_str());
    std::fflush(stdout);
    // ★ 收窗 + 打窗口表（在既有 `[profile-window] end` 之后 ⇒ 旧的逐行格式逐字不变）。
    if (mark_fn_ != nullptr) mark_fn_(mark_ctx_, profile_label_.c_str(), /*begin=*/false);
    profile_open_ = false;
    profile_label_.clear();
    ++profile_windows_;
}

void InputPlan::applyQuit(WindowManager& wm) {
    // ★ 与解释器臂**同一个**交付点：`Machine::pumpScript` 消费 `InputScript::quitRequested()` 时
    //   也是 `cpu.windows->postQuit(0)`（一次性；WM_QUIT 是队列尾的虚拟项）⇒ guest 的消息循环
    //   拿到它之后自己退出，run 以"guest 正常退出"收口。
    //   ⛔ 不在这里另造一种"宿主直接杀进程"的语义（那会让两臂的 quit 不同）。
    quit_requested_ = true;
    wm.postQuit(0);
    std::printf("[input-plan] quit：已投 WM_QUIT（read point %s；后续指令不再推进）\n",
                u64s(read_points_).c_str());
    std::fflush(stdout);
}

void InputPlan::failTimeout(std::size_t index, std::uint64_t elapsed_ms, std::uint64_t guard_ms) {
    if (failed_) return;  // ⛔ 失败只报一次（读点还会继续来）
    failed_ = true;
    failed_index_ = index;
    failed_elapsed_ms_ = elapsed_ms;
    failed_guard_ms_ = guard_ms;
}

void InputPlan::releaseDue(WindowManager& wm, InputPoint point) {
    for (std::size_t i = 0; i < pending_.size();) {
        const PendingRelease& r = pending_[i];
        // ① guest 至少又**采样**过一拍（与 `input_script` 的 auto hold 同一条"一次按下一定被
        //    观察到"的判据，但**只用计数**、⛔ 不看毫秒）。+1 是"观察过按下"之后再过一拍。
        const bool sampled = wm.inputSampleCount() > r.press_samples + 1;
        // ② 兜底：队列类读点不采样 ⇒ 连续 kForceReleaseReads 个读点后仍然抬起（定数，非时钟）。
        const bool forced = read_points_ > r.press_reads + kForceReleaseReads;
        if (!sampled && !forced) {
            ++i;
            continue;
        }
        // ★ trace（opt-in）：抬起走的是哪条腿 = "guest 到底有没有机会看到按下"的唯一读数
        //   （D 的判据面：`reason=forced` + `samples==press_samples` ⇒ 兜底先于任何一次采样）。
        traceRelease(r, sampled, forced, wm.inputSampleCount(), point);
        emitRelease(wm, r);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        --releases_pending_;
    }
}

void InputPlan::emitRelease(WindowManager& wm, const PendingRelease& r) {
    // 抬起复用**按下时存下的同一对**（已缩放的）客户区坐标。
    input::mouseButton(wm, r.x, r.y, r.button, /*down=*/false, InputSource::Script, kPlanMessageTimeMs);
    ++released_;
}

std::size_t InputPlan::releaseAll(WindowManager& wm) {
    std::size_t n = 0;
    for (const PendingRelease& r : pending_) {
        emitRelease(wm, r);
        ++n;
    }
    pending_.clear();
    releases_pending_ = 0;
    return n;
}

// ---------------------------------------------------------------------------------------
// `--input-plan-trace`（opt-in；口径见 `input_plan.h` 的 `setTrace`）
// ---------------------------------------------------------------------------------------
void InputPlan::traceStepConsumed(const Step& s, std::uint64_t samples_now, InputPoint point,
                                  const char* extra_tag) {
    if (trace_out_ == nullptr) return;  // 缺省档：一次判空、零输出
    // ★ 只在开档时**额外**采一次身份 / 位置（缺省档一步也不多读 guest 内存）。
    //   门那一步刚刚采过（缓存命中）；动作那一步在这里采 ⇒ `click` 的行也有 script/pos。
    refreshIdentity();
    ScriptPositionSample pos;
    if (mem_ != nullptr && identity_.valid) pos = readScriptPositionSample(*mem_, identity_);
    char pos_text[16];
    if (pos.valid) {
        std::snprintf(pos_text, sizeof(pos_text), "%08X", pos.pos);
    } else {
        std::snprintf(pos_text, sizeof(pos_text), "?");
    }
    char seq_text[24];
    if (state_seq_fn_ != nullptr) {
        std::snprintf(seq_text, sizeof(seq_text), "%llu",
                      static_cast<unsigned long long>(state_seq_fn_(state_seq_ctx_)));
    } else {
        std::snprintf(seq_text, sizeof(seq_text), "-");  // 没开控制层 ⇒ 没有这个数（⛔ 不编造 0）
    }
    std::fprintf(trace_out_,
                 "plan:%s:%d applied kind=%s%s script=%s pos=%s frame=%llu state_seq=%s reads=%llu "
                 "samples=%llu at=%s held=%zu\n",
                 recipe_.origin().empty() ? "(control)" : recipe_.origin().c_str(), s.line,
                 traceKindToken(s.kind), extra_tag != nullptr ? extra_tag : "",
                 identity_.valid ? identity_.name.c_str() : "?", pos_text,
                 static_cast<unsigned long long>(presenting()), seq_text,
                 static_cast<unsigned long long>(read_points_), static_cast<unsigned long long>(samples_now),
                 inputPointName(point), pending_.size());
    std::fflush(trace_out_);
}

void InputPlan::traceRelease(const PendingRelease& r, bool sampled, bool forced, std::uint64_t samples_now,
                             InputPoint point) {
    if (trace_out_ == nullptr) return;
    // 两条腿都成立时 `reason` 写 `sampled+forced`（读数要能看出"是采样先到还是兜底先到"）。
    const char* reason = sampled && forced ? "sampled+forced" : (sampled ? "sampled" : "forced");
    // ★ `samples=` 是 `WindowManager::inputSampleCount()` 的**当前值**（不是读点数）：
    //   `samples == press_samples` ⇒ guest 在按下之后**一次也没有**采样 ⇒ 它永远看不到那个电平；
    //   `samples == press_samples+1` ⇒ 它看过按下，但兜底还是先到了（`sampled` 那条腿要 +2）。
    std::fprintf(trace_out_,
                 "plan:%s:%d release kind=click button=%s reason=%s samples=%llu press_samples=%llu "
                 "reads=%llu press_reads=%llu at=%s\n",
                 recipe_.origin().empty() ? "(control)" : recipe_.origin().c_str(), r.line,
                 mouseButtonToken(r.button), reason,
                 static_cast<unsigned long long>(samples_now),
                 static_cast<unsigned long long>(r.press_samples), static_cast<unsigned long long>(read_points_),
                 static_cast<unsigned long long>(r.press_reads), inputPointName(point));
    std::fflush(trace_out_);
}

void InputPlan::printFaultSummary(std::FILE* out) const {
    // ★ 零分配（fault 在信号上下文里；见 `memory.h` 的 `FaultTextHook`）：只用 fputs / snprintf 定长缓冲。
    //   数字与 `summary()` / `outcome()` **同一份** ⇒ fault 文本与正常收口的读数可以直接对账。
    if (!recipe_.active() || out == nullptr) return;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "  input-plan: steps=%llu fired=%llu pending=%llu presents=%llu released=%llu held=%zu "
                  "outcome=%s\n",
                  static_cast<unsigned long long>(steps()), static_cast<unsigned long long>(fired_),
                  static_cast<unsigned long long>(pending()), static_cast<unsigned long long>(presenting()),
                  static_cast<unsigned long long>(released_), pending_.size(),
                  recipeOutcomeName(outcome()));
    std::fputs(buf, out);
    const Step* s = currentStep();
    if (s != nullptr) {
        char step_buf[256];
        std::snprintf(step_buf, sizeof(step_buf), "  plan-step  : %s:%d kind=%s（%s）\n",
                      recipe_.origin().c_str(), s->line, stepKindName(s->kind),
                      failed_ ? "守卫到期" : "还没消费");
        std::fputs(step_buf, out);
    }
}

void InputPlan::note(PlanEventKind kind) {
    // ★ 只记账。⛔ 不接受 `WindowManager` ⇒ 结构上不可能从这里施加 step（见头注铁律 3）。
    const std::size_t i = static_cast<std::size_t>(kind);
    if (i < event_calls_.size()) ++event_calls_[i];
}

std::uint64_t InputPlan::deliverCalls(InputPoint point) const {
    const std::size_t i = static_cast<std::size_t>(point);
    return i < deliver_calls_.size() ? deliver_calls_[i] : 0;
}

std::uint64_t InputPlan::noteCalls(PlanEventKind kind) const {
    const std::size_t i = static_cast<std::size_t>(kind);
    return i < event_calls_.size() ? event_calls_[i] : 0;
}

std::string InputPlan::summary() const {
    // 一行、**一次性**（⛔ 不许每帧打：`AGENTS.md` §12.8 第 3 条）。
    // ★ `presents=` 只是**读数**（`present.cpp` 推的 guest 累计 Present 数），⛔ 不是门。
    // ★ `pending=` = **还没走完的指令数**（判据：≠ 0 ⇒ 任务没走完 ⇒ exit 5，见 `outcome()`）。
    //   位置在 `fired=` 之后（`steps= fired= pending=` 是读这一行的最短形式；旧的四个字段照旧在）。
    char buf[320];
    std::snprintf(buf, sizeof(buf), "steps=%llu fired=%llu pending=%llu presents=%llu released=%llu held=%zu",
                  static_cast<unsigned long long>(recipe_.size()),
                  static_cast<unsigned long long>(fired_),
                  static_cast<unsigned long long>(pending()),
                  static_cast<unsigned long long>(presenting()),
                  static_cast<unsigned long long>(released_), pending_.size());
    return std::string(buf);
}

RecipeOutcome InputPlan::outcome() const {
    // ★ 判据只有一条：**配方的走完数**。`TimedOut` 是"没走完 + 知道是哪一条"；
    //   `Running` 是"跑完了仍然还有指令没消费"（= 任务没走完，⛔ 不是成功）。
    if (failed_) return RecipeOutcome::TimedOut;
    if (recipe_.active() && next_ >= recipe_.size()) return RecipeOutcome::Done;
    return RecipeOutcome::Running;
}

const Step* InputPlan::currentStep() const {
    // 失败时点名**到期的那一条**；没失败就点名**第一条还没消费的**（= 卡在哪）。
    if (failed_) {
        return failed_index_ < recipe_.size() ? &recipe_.steps()[failed_index_] : nullptr;
    }
    if (next_ < recipe_.size()) return &recipe_.steps()[static_cast<std::size_t>(next_)];
    return nullptr;
}

std::string InputPlan::failureReport() const {
    const Step* s = currentStep();
    if (s == nullptr) return std::string();
    std::string r;
    if (failed_) {
        r += "input-plan: FAIL timeout\n";
    } else {
        r += "input-plan: FAIL incomplete（配方没有走完）\n";
    }
    // ★ 点名必须有 `文件:行`（`REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置：人能照着修）
    r += "  plan     : " + recipe_.origin() + ":" + std::to_string(s->line) + "\n";
    r += "  step     : " + s->id + "（行 " + std::to_string(s->line) + "，" +
         (s->isGate() ? "门" : "动作") + "）\n";
    r += "  waiting  : " + stepText(*s) + "\n";
    if (failed_) {
        r += "  window   : 守卫 " + u64s(failed_guard_ms_) + " ms（宿主墙钟；本臂只有这一把尺）已等 " +
             u64s(failed_elapsed_ms_) + " ms ⇒ 到期\n";
    } else if (s->isGate()) {
        r += "  说明     : 跑完时这个门还**没有**成立 ⇒ 任务没走完（⛔ 不是成功）\n";
    } else {
        r += "  说明     : 跑完时这条**动作**还没轮到（前面的门没走完 / 被 `quit` 截断）⇒ "
             "任务没走完（⛔ 不是成功）\n";
    }
    r += "  progress : steps=" + u64s(steps()) + " applied=" + u64s(applied_) + " pending=" + u64s(pending()) +
         " fired=" + u64s(fired_) + " reads=" + u64s(read_points_) + "\n";
    if (failed_) {
        r += "  说明     : 只在**本臂**有的那把尺上判（宿主墙钟）；guest 侧还在不在跑不影响这条结论\n";
    }
    return r;
}

}  // namespace lift
