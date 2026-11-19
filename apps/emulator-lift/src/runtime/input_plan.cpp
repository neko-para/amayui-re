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
#include "runtime/input.h"       // input::mouseButton / releaseProgram（唯一施加 / 收口路径）
#include "runtime/memory.h"      // GuestMemory（身份采样只经 MarkerSampler 读它）
#include "runtime/vfs.h"         // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

// ★ 三条闸门的常量在 `PointerCore`（`runtime/pointer_core.h`）—— 守卫要引用同一份数，
//   ⛔ 不在这里再写一遍。
// ★ 计划消息的 `MSG.time`（恒 0）也在 `pointer_core.h`：`kPointerMessageTimeMs`（core 与
//   `closeSessionIfDone` 用同一个数；⛔ 不各写一份 0）。

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
        case StepKind::MouseMove: return "mouse-move";
        case StepKind::MouseDown: return "mouse-down";
        case StepKind::MouseUp: return "mouse-up";
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
    // ★ 指针那一半（`pending_` / `armed_` / `fired_` / 闸门读数 / 写预算 / 会话旗标）搬到
    //   `PointerCore` ⇒ 复位也搬过去（字段集合与旧 `clear()` 逐个对齐，见 `clearPointerState`）。
    clearPointerState();
    immediate_.clear();
    identity_ = MarkerSample{};
    next_ = 0;
    applied_ = 0;
    position_misses_ = 0;
    read_points_ = 0;
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
    // ★ S7 / Q2：**本读点的写预算**（门 / `wait` / `profile_*` / `quit` 不吃预算）。
    //   下面的四段（releaseDue / pressDue / drainImmediate / flush）都在写过一条之后停。
    writes_this_point_ = 0;
    // ★ 闸门 ③ 的刻度 = "**本读点之前**完成过几个 CursorPos 读点"：本读点自己的读还没发生，
    //   所以若是位置读点，得把刚 +1 的那个数扣掉（否则"move 与 down 同读点"又能满足它）。
    cursor_reads_done_ = deliverCalls(InputPoint::CursorPos);
    if (point == InputPoint::CursorPos && cursor_reads_done_ > 0) --cursor_reads_done_;
    // ★ 会话收口（解释器臂的 `finished() || quit_requested_` 同口径）：缺省档 `session_closed_`
    //   恒 true ⇒ 每个读点只多一次 bool 判断（⛔ 不在热路径上做 256 键的清理）。
    closeSessionIfDone(wm);
    // ★ 早退条件多了 **armed_ 非空** 这一项：光标已落地、还差一拍才按下的那些也是"有待办"。
    if (!recipe_.active() && pending_.empty() && armed_.empty() && immediate_.empty()) return;
    // ★ **本读点的读数**（W2：core 不持有钟 / 读点计数 / 投递计数 ⇒ 值参数传进去）。
    //   一次读点只取一次钟：旧实现 `releaseDue` 在每个待抬起项上各取一次（阈值是 250/500 ms 的
    //   兜底 ⇒ 语义不变，见 `pointer_core.h` 头注的"唯一偏离"）。
    const PointerPoint pt{point,
                          nowMs(),
                          deliverCalls(InputPoint::KeyLevel),
                          cursor_reads_done_,
                          read_points_,
                          0};
    // ① 到期的**抬起**先落地（同一次读点内、读之前）。
    releaseDue(wm, pt);
    // ② ★ 再把到期的 `armed_` **按下**（阶段 ②；闸门 ③ 见 `pressDue`）。
    //   ⛔ 顺序不许反：先按下再抬起会让"新的一次 click + 上一次的抬起"在同一个读点里发生
    //   ⇒ guest 只看到 up（第二次按下被吞掉）。
    pressDue(wm, pt);
    // ③ ★ **立即队列先于配方当步**（判据 2 的前半）：控制层的 click / move 在这里落地，
    //    走**同一段** `armClickAt` / `input::mouseMove`、计入 `fired_`，但**不动** `next_` /
    //    `applied_` / 守卫账。
    //    放在 `quit_requested_` / `failed_` 的早退**之前**：控制层已经对客户端答了 `ok:true`，
    //    不该因为**配方自己的**失败 / quit 把它静默丢掉（它只等下一个读点）。
    drainImmediate(wm, pt);
    // ④ `quit` 之后不再推进；守卫到期之后同样不再推进（⛔ 不许"失败后继续点"）。
    if (quit_requested_ || failed_) return;
    // ⑤ flush-before-read：按文件顺序把到期指令消费到 `WindowManager`（⛔ 不越过未到期的指令）。
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
        // ★ **写预算**（S7 / Q2）：本读点已经写过一条写动作 ⇒ 不再消费**写** step（留 `next_`），
        //   但**门 / `wait` / `profile_*` / `quit` 照旧可以继续消费**（它们不是写动作）。
        if (writes_this_point_ != 0 && s.isInputWrite()) return;
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
            // ★ **连点串行化（Q8-A）**：这个按钮还按着（第一条的 down 还没抬起）⇒ **不消费**该 step
            //   （留 `next_`，下一个读点再试）。⛔ 删掉了 `armClickAt` 的"同按钮先抬起" —— 那会让
            //   第一条 click 的 down 一次都不被 guest 读到。
            if (buttonPending(s.button)) return;
            applyClick(wm, s, pt);
            ++fired_;
            writes_this_point_ = 1;  // ★ 臂上的那一次 `input::mouseMove` 就是一条写动作
            // trace 的 `plan:<文件>:<行>` 要指向**施加它的那条 step** ⇒ `applyClick` 把 `s.line`
            // 放进 `PointerPoint::line`，由 `armClickAt` 记在臂上 / 待抬起的记录上
            // （抬起那一行因此也能点名同一个 `文件:行`；立即队列的 click 没有 step ⇒ 0）。
        } else if (s.kind == StepKind::MouseMove) {
            applyMouseMove(wm, s, pt);
            writes_this_point_ = 1;
        } else if (s.kind == StepKind::MouseDown || s.kind == StepKind::MouseUp) {
            // ★ `mouse_down`：同一按钮还在 `pending_`（click 的 down 没抬）⇒ 不消费（否则那条 up
            //   会把这次的 hold 一起清掉）。`mouse_up` 直写。
            if (s.kind == StepKind::MouseDown && buttonPending(s.button)) return;
            // ★ `mouse_down` 的**轻闸门**（Q3）：上一条 `mouse_move` 之后必须完成过一次 CursorPos
            //   读点，或墙钟兜底到期；否则不消费（留 `next_`）。⛔ 不套闸门①/②。
            if (s.kind == StepKind::MouseDown) {
                const std::uint64_t now = nowMs();
                const bool cursor_after_move = cursor_reads_done_ > last_move_cursor_done_;
                const bool wallclock = now >= last_move_ms_ && now - last_move_ms_ >= kPressFallbackMs;
                if (!cursor_after_move && !wallclock) return;
            }
            applyMouseButton(wm, s, /*down=*/s.kind == StepKind::MouseDown);
            writes_this_point_ = 1;
        } else if (s.kind == StepKind::ProfileBegin || s.kind == StepKind::ProfileEnd) {
            applyProfileMark(s, nowMs());
        } else {  // Quit
            applyQuit(wm);
        }
        ++applied_;
        ++next_;
        guard_armed_ = false;  // 下一条指令重新起算（同一次读点内继续 flush）
        traceStepConsumed(s, wm.inputSampleCount(), point, nullptr, "");  // ★ opt-in（缺省 nullptr ⇒ 一次判空、零输出）
        // ★ `quit` 之后不再推进（`InputScript` 的 `if (quit_requested_) continue` 同口径）：
        //   但它**自己**已经算走完 ⇒ 末尾的 `quit` 不会让 pending 变非 0。
        if (quit_requested_) return;
    }
}

bool InputPlan::enqueueClick(int ref_x, int ref_y, MouseButton button) {
    // ★ **有界**（判据 5）：满了就返回 false，由控制层回 `bad_request`。⛔ 不丢旧的、⛔ 不静默扩容器。
    //   ⚠ 这里**不做映射**：参考坐标原样排队，映射只在落地那一刻由 `armClickAt` 做**一次**
    //   （否则"排队时的客户端尺寸"与"落地时的客户端尺寸"会变成两套口径）。
    if (immediate_.size() >= kImmediateClickCapacity) return false;
    ImmediateOp c;
    c.is_move = false;
    c.x = ref_x;
    c.y = ref_y;
    c.button = button;
    immediate_.push_back(c);
    session_closed_ = false;  // 新的注入工作 ⇒ 所有权要重新握在脚本手里（见 closeSessionIfDone）
    return true;
}

bool InputPlan::enqueueMove(int ref_x, int ref_y) {
    // ★ 纯 move：与 click **同一条**立即队列 / 同一个上界 / 同一个落点（下一个 guest 读点），
    //   只走 `input::mouseMove`（⛔ 不碰按钮电平、⛔ 不进 armed_）。
    if (immediate_.size() >= kImmediateClickCapacity) return false;
    ImmediateOp m;
    m.is_move = true;
    m.x = ref_x;
    m.y = ref_y;
    immediate_.push_back(m);
    session_closed_ = false;
    return true;
}

void InputPlan::drainImmediate(WindowManager& wm, const PointerPoint& pt) {
    // ★ 只消费立即队列：**不碰** `next_` / `applied_` / `wait_started_` / 守卫账 ⇒ 配方里未到期的门
    //   照旧等（判据 2 的"步进指针不动"）。每条 click +1 `fired_`（与配方 click 同一读数口径）；
    //   move 不是"动作" ⇒ 不计 `fired_`（它没有按下 / 抬起，⛔ 不冒充一次点击）。
    // ★ **S7 / Q2**：一次读点最多落**一条**写动作 ⇒ 剩下的**留在队首**（`queuedClicks()` 非 0）；
    //   同一按钮还没抬起（`pending_`）⇒ 停在队首（连点串行化），⛔ 不在这里强制抬起。
    while (!immediate_.empty()) {
        if (writes_this_point_ != 0) return;  // 本读点的写预算已经用完 ⇒ 其余留队
        const ImmediateOp c = immediate_.front();
        if (!c.is_move && buttonPending(c.button)) return;  // 队首的 click 等这一次抬起
        immediate_.erase(immediate_.begin());
        if (c.is_move) {
            armMoveAt(wm, c.x, c.y, pt);
        } else {
            armClickAt(wm, c.x, c.y, c.button, pt);
            ++fired_;
        }
        writes_this_point_ = 1;
        // ★ trace（opt-in）：控制层的 click / move 与配方 click 走同一段施加核心 ⇒ 读数也要能
        //   直接对齐（D 的判据面：同一屏上"控制层 click 落在哪种读点"是唯一能解释两边差异的读数）。
        //   行里 `origin=control` + `plan:<文件>:0`（0 = 不是配方 step）点名它；move 显式写 `kind=move`
        //   （它没有 StepKind —— ⛔ 不为一条控制命令改配方词表）。
        if (trace_out_ != nullptr) {
            Step control_step;
            control_step.kind = StepKind::Click;
            control_step.line = 0;
            traceStepConsumed(control_step, wm.inputSampleCount(), pt.point,
                              c.is_move ? "move" : nullptr, " origin=control");
        }
    }
}

void InputPlan::applyClick(WindowManager& wm, const Step& s, const PointerPoint& at) {
    // 配方 step 的壳：只把 `Step` 里的三个值（+ 行号）交给**同一段**核心（⛔ 不复制映射 / 记账）。
    PointerPoint pt = at;
    pt.line = s.line;  // trace 的 `plan:<文件>:<行>`（臂上 / 待抬起的记录都点名它）
    armClickAt(wm, s.x, s.y, s.button, pt);
}

void InputPlan::applyMouseMove(WindowManager& wm, const Step& s, const PointerPoint& at) {
    // ★ 与 `click` 的**第一段**、控制层的 `move` 同一条落点（同一份坐标映射 + 同一份 `input::mouseMove`）。
    armMoveAt(wm, s.x, s.y, at);
}

void InputPlan::applyMouseButton(WindowManager& wm, const Step& s, bool down) {
    // ★ 坐标 = **设备当前客户区位置**（⛔ 不带 x/y、⛔ 不用按下点回跳）：拖拽的 up 因此落在新点上。
    //   写后回读由 `input::mouseButton` 自己做（消息的 lParam 与 `GetCursorPos` 说同一个位置）。
    int client_x = 0;
    int client_y = 0;
    wm.cursorClientPos(wm.primaryWindow(), &client_x, &client_y);
    input::mouseButton(wm, client_x, client_y, s.button, down, InputSource::Script,
                       kPointerMessageTimeMs);
    session_closed_ = false;  // 新的注入工作 ⇒ 所有权要重新握在脚本手里（见 closeSessionIfDone）
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

void InputPlan::closeSessionIfDone(WindowManager& wm) {
    // ★ 与解释器臂 `InputScript::tick` 的收口**同一处**：`finished() || quit_requested_`。
    //   · 配方走完（`next_ >= size`）/ `quit` ⇒ 一次注入会话结束；
    //   · **纯控制层**的运行（没有配方）：一次 move / click 就是一次会话 ⇒ 落地并抬起之后也就结束。
    //   ⇒ 交还 Program 的 hold（§3.4 第 6 条）：`input::releaseProgram` 清 Program 抬起的电平并
    //     **逐条 post up**；⛔ 不再有「只改状态、不投影」的 `releaseScriptOwnership`。
    //   ★ 必须等 `pending_` / `armed_` / `immediate_` 全空：按下还没抬起就把光标还给 OS，
    //     真实鼠标会在按下与抬起之间把它挪走（那就等于把本轮的修法又撤了）。
    if (session_closed_) return;
    if (!pending_.empty() || !armed_.empty() || !immediate_.empty()) return;
    if (!quit_requested_ && recipe_.active() && next_ < recipe_.size()) return;
    // ★ `atPoint` 的**开头**调用（早退之前）⇒ 收口的 up 在**本读点**入队，guest 本读点的
    //   `PeekMessage` 就能取到，且电平已经是 0（§3.4 第 6 条）。
    input::releaseProgram(wm, kPointerMessageTimeMs);
    session_closed_ = true;
}

std::size_t InputPlan::releaseAll(WindowManager& wm) {
    // ★ 全抬 + 会话收口的**唯一**实现已搬进 `PointerCore::releaseAllPending`
    //   （`pending_` → up、`armed_` 丢弃、`input::releaseProgram`、`session_closed_ = true`）。
    //   本类只保留这个名字（`aot_main` / CTest 的调用点不动、行为逐位不变）。
    return releaseAllPending(wm);
}

// ---------------------------------------------------------------------------------------
// `--input-plan-trace`（opt-in；口径见 `input_plan.h` 的 `setTrace`）
// ---------------------------------------------------------------------------------------
void InputPlan::traceStepConsumed(const Step& s, std::uint64_t samples_now, InputPoint point,
                                  const char* kind_token, const char* extra_tag) {
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
                 kind_token != nullptr ? kind_token : traceKindToken(s.kind),
                 extra_tag != nullptr ? extra_tag : "",
                 identity_.valid ? identity_.name.c_str() : "?", pos_text,
                 static_cast<unsigned long long>(presenting()), seq_text,
                 static_cast<unsigned long long>(read_points_), static_cast<unsigned long long>(samples_now),
                 inputPointName(point), pending_.size());
    std::fflush(trace_out_);
}

void InputPlan::traceRelease(const PendingRelease& r, bool key_seen, bool wallclock, std::uint64_t samples_now,
                             InputPoint point) {
    if (trace_out_ == nullptr) return;
    // 两条腿都成立时 `reason` 写 `keylevel+wallclock`（读数要能看出"是电平读点先到还是兜底先到"）。
    const char* reason =
        key_seen && wallclock ? "keylevel+wallclock" : (key_seen ? "keylevel" : "wallclock");
    // ★ `samples=` / `press_samples=` 是 `WindowManager::inputSampleCount()` 的读数（**不是闸门**）：
    //   `samples == press_samples` ⇒ 按下之后连**位置**读点都没有；真正回答"按键被看见没有"的是
    //   `key_reads=` / `press_key_reads=`（唯一来源 = `InputPlan::deliverCalls(KeyLevel)`）。
    std::fprintf(trace_out_,
                 "plan:%s:%d release kind=click button=%s reason=%s samples=%llu press_samples=%llu "
                 "key_reads=%llu press_key_reads=%llu reads=%llu press_reads=%llu at=%s\n",
                 recipe_.origin().empty() ? "(control)" : recipe_.origin().c_str(), r.line,
                 mouseButtonToken(r.button), reason,
                 static_cast<unsigned long long>(samples_now),
                 static_cast<unsigned long long>(r.press_samples),
                 static_cast<unsigned long long>(deliverCalls(InputPoint::KeyLevel)),
                 static_cast<unsigned long long>(r.press_key_reads),
                 static_cast<unsigned long long>(read_points_),
                 static_cast<unsigned long long>(r.press_reads), inputPointName(point));
    std::fflush(trace_out_);
}

void InputPlan::releaseTraceHook(void* ctx, const PendingRelease& r, bool key_seen, bool wallclock,
                                 std::uint64_t samples_now, InputPoint point) {
    // `PointerCore::releaseDue` 的抬起钩子：`plan:<文件>:<行> reason=…` 的文本属于配方口径 ⇒
    //   留在本类（core 只把读数交出来）。`ctx` 恒 = 本对象（见 `setTrace`）。
    static_cast<InputPlan*>(ctx)->traceRelease(r, key_seen, wallclock, samples_now, point);
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
    std::string out(buf);
    // ★ S9（D2 / R3）：只在**真的吞过**注入点击时追加 `ui_consumed=N`。默认档（没有叠层 / 没命中）
    //   这一行**逐字不变** —— 那份文本是 `fault_report` / `input-plan` 摘要的既有判据面。
    if (ui_consumed_ != 0) {
        out += " ui_consumed=";
        out += std::to_string(ui_consumed_);
    }
    return out;
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
