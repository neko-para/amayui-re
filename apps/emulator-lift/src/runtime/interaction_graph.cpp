// emulator-lift · 输入配方的解释器臂入口（块边界推进 / trace / 失败报告）
//
// ★ 语义真源是 `runtime/recipe.h`（语法 + 顺序模型 + 门判定）与 `docs/03-emulator/interaction-model.md`
//   （口径 / 判据）。本文件只做那两处**已经写下来**的事，⛔ 不复述 schema。
// ★ 三条实现口径（都是为了"同一份观测 ⇒ 同一串判定"）：
//   1. **判定只读传进来的观测值**（`MarkerSample` / `ScriptPositionSample`）：本文件不读 guest 内存、
//      不碰像素、不看时钟性格。`wall_ms` 只用于**停滞兜底**（guest 阻塞在 GetMessageA 时 guest 时钟
//      冻住，否则整轮会挂死）。
//   2. **动作不在这里直接改窗口状态**：全部经 `actuator_.appendText()` 追加**绝对触发**，
//      由 `InputScript` 去发 WM_* 并保证"一次按下一定被 guest 观察到"（见 `input_script.h` 顶部）。
//      为什么用 `frame + 1`：本 tick 在块边界上，动作若落在同一帧就可能与本 tick 的注入顺序打架；
//      +1 让它必然在**下一次 Present 之后**生效 —— 确定性，且与调用方的 tick 顺序无关。
//   3. **失败一律响亮**：状态进 `Failed` 并算好一份多行报告（`failureReport()`），⛔ 不 warning-only。
#include "runtime/interaction_graph.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "runtime/vfs.h"

namespace lift {
namespace {

std::string hex8(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
    return buf;
}

std::string u64s(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

// 只看文件名：trace 是**可 diff** 的产物（真源 §4），⛔ 不能把宿主的 checkout 路径写进去
// （换一台机器 / 换一个目录就不同 ⇒ 同一路径的两次运行 diff 出差异）。
std::string baseNameOf(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

}  // namespace

const char* graphFailureName(GraphFailure f) {
    switch (f) {
        case GraphFailure::None: return "none";
        case GraphFailure::Timeout: return "timeout";
    }
    return "?";
}

const char* InteractionGraph::stateName() const {
    switch (state_) {
        case State::Idle: return "idle";
        case State::Gate: return "gate";
        case State::Acting: return "acting";
        case State::Done: return "done";
        case State::Failed: return "failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------
// 装载
// ---------------------------------------------------------------------------------------
bool InteractionGraph::loadFile(const std::string& path, std::string* err) {
    Recipe r;
    if (!r.loadFile(path, err)) {
        clear();
        return false;
    }
    clear();
    recipe_ = std::move(r);
    return true;
}

bool InteractionGraph::loadText(const std::string& text, const std::string& origin, std::string* err) {
    Recipe r;
    if (!r.loadText(text, origin, err)) {
        clear();
        return false;
    }
    clear();
    recipe_ = std::move(r);
    return true;
}

void InteractionGraph::clear() {
    recipe_.clear();
    state_ = State::Idle;
    current_ = 0;
    failure_ = GraphFailure::None;
    failure_detail_.clear();
    failure_report_.clear();
    wait_start_guest_ms_ = 0;
    wait_start_frame_ = 0;
    wait_start_insns_ = 0;
    wait_start_wall_ms_ = 0;
    last_progress_wall_ms_ = 0;
    last_progress_frame_ = 0;
    last_progress_insns_ = 0;
    issued_ = false;
    applied_before_ = 0;
    wait_pending_ = false;
    wait_due_wall_ms_ = 0;
    identity_ = nullptr;
    fail_identity_ = MarkerSample{};
    position_ = ScriptPositionSample{};
    fail_position_ = ScriptPositionSample{};
    trace_header_done_ = false;  // 新配方重新开一份头（`trace_` 是调用方的汇，不清它）
    transitions_ = 0;
    actions_issued_ = 0;
    actuator_.clear();
}

bool InteractionGraph::needsIdentitySamples() const {
    for (const Step& s : recipe_.steps()) {
        if (s.kind == StepKind::AwaitScript || s.kind == StepKind::AwaitPosition) return true;
    }
    return false;
}

bool InteractionGraph::needsPositionSamples() const {
    for (const Step& s : recipe_.steps()) {
        if (s.kind == StepKind::AwaitPosition) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// trace
// ---------------------------------------------------------------------------------------
void InteractionGraph::setTrace(std::FILE* out, bool diffable) {
    trace_ = out;
    trace_diffable_ = diffable;
    trace_header_done_ = false;
}

void InteractionGraph::writeTraceHeader() {
    if (trace_ == nullptr || trace_header_done_) return;
    trace_header_done_ = true;
    std::fprintf(trace_, "# interaction trace v1\n");
    // ★ 首行标 NON-DIFFABLE（真源 §4）：`wait <ms>` 是宿主墙钟，完成时刻不可复现。
    std::fprintf(trace_, "%s\n", trace_diffable_ ? "# diffable" : "# NON-DIFFABLE");
}

void InteractionGraph::traceLine(const std::string& line) {
    if (trace_ == nullptr) return;
    std::fprintf(trace_, "%s\n", line.c_str());
    std::fflush(trace_);  // 失败 / 崩掉时 trace 必须已经落盘（它就是事后唯一的判据）
}

// ---------------------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------------------
const Step* InteractionGraph::current() const {
    // ★ Idle 时还没进任何指令 ⇒ 返回 nullptr（调用方据此打 `-`）。
    if (state_ == State::Idle || state_ == State::Done || state_ == State::Failed) return nullptr;
    if (current_ >= recipe_.size()) return nullptr;
    return &recipe_.steps()[current_];
}

void InteractionGraph::noteProgress(std::uint64_t frame, std::uint64_t insns, std::uint64_t wall_ms) {
    // ★ "有进展" = guest 指令**或**帧号变了。为什么不是"只看 insns"：房间空闲时帧会停、而 guest
    //   还在跑循环（insns 涨）；反过来 guest 阻塞在 GetMessageA 时 insns 停、帧也可能停 —— 那才是
    //   真停滞。两个都停 ⇒ 兜底时钟重新起算。
    if (frame == last_progress_frame_ && insns == last_progress_insns_) return;
    last_progress_frame_ = frame;
    last_progress_insns_ = insns;
    last_progress_wall_ms_ = wall_ms;
}

bool InteractionGraph::windowExpired(std::uint64_t timeout_ms, std::uint64_t guest_ms,
                                     std::uint64_t wall_ms) const {
    // ★ "到期"的比较**只有一份**（`recipe.h` 的 `stepWindowExpired`：严格大于，`>` 不是 `>=`）。
    // 尺 ①：guest 毫秒（"等的那一条一直没成立"）
    if (stepWindowExpired(guest_ms > wait_start_guest_ms_ ? guest_ms - wait_start_guest_ms_ : 0, timeout_ms)) {
        return true;
    }
    // 尺 ②：墙钟停滞兜底（guest 阻塞在 GetMessageA 时 guest 时钟是冻住的，光看 ① 会把整轮挂死）
    if (stepWindowExpired(wall_ms > last_progress_wall_ms_ ? wall_ms - last_progress_wall_ms_ : 0, timeout_ms)) {
        return true;
    }
    return false;
}

bool InteractionGraph::issueAction(const Step& s, std::uint64_t frame) {
    // ★ 绝对触发 = **下一个 Present**（+1）。理由见文件头第 2 条。
    const std::string at = "at frame " + u64s(frame + 1) + "\n";
    std::string text;
    switch (s.kind) {
        case StepKind::Click: {
            // ★ 按钮词只在**非 left** 时发：left 是缺省 ⇒ 时间线收到的仍是逐字节相同的 `click <x> <y>`
            //   （trace 可 diff、老配方/老 trace 不因这条特性而变）。
            const std::string button_word = s.button == MouseButton::Left ? std::string() : std::string(" right");
            text = at + "click " + std::to_string(s.x) + " " + std::to_string(s.y) + button_word + "\n";
            break;
        }
        case StepKind::ProfileBegin:
            text = at + "profile_begin " + s.label + "\n";
            break;
        case StepKind::ProfileEnd:
            text = at + "profile_end " + s.label + "\n";
            break;
        case StepKind::Quit:
            text = at + "quit\n";
            break;
        // 门与 `wait` 不追加任何动作（`wait` 只记到期点；门在进入时已经成立）。
        case StepKind::AwaitScript:
        case StepKind::AwaitPosition:
        case StepKind::Wait:
            return true;
    }
    std::string err;
    if (!actuator_.appendText(text, recipe_.origin() + ":" + s.id, &err)) {
        // ★ 不是失败：`InputScript` 的前置（动作全部已应用 / 没有未完成的抬起）会在几个块边界内
        //   自己满足（抬起等 guest 采样 / 等 hold 到期）。这里只留一行诊断，下一个块边界重试；
        //   真等不到就由窗口到期报 timeout。
        std::fprintf(stderr, "[interaction-graph] 动作暂缓（%s）：%s\n", s.id.c_str(), err.c_str());
        return false;
    }
    applied_before_ = actuator_.appliedCount();
    ++actions_issued_;
    return true;
}

void InteractionGraph::enterStep(std::size_t index, std::uint64_t frame, std::uint64_t guest_ms,
                                 std::uint64_t insns, std::uint64_t wall_ms) {
    current_ = index;
    wait_start_frame_ = frame;
    wait_start_insns_ = insns;
    wait_start_guest_ms_ = guest_ms;
    wait_start_wall_ms_ = wall_ms;
    last_progress_frame_ = frame;
    last_progress_insns_ = insns;
    last_progress_wall_ms_ = wall_ms;
    issued_ = false;
    wait_pending_ = false;
    wait_due_wall_ms_ = 0;
    ++transitions_;
    const Step& s = recipe_.steps()[index];
    std::string line = "step " + s.id + " enter frame=" + u64s(frame) + " insn=" + u64s(insns) +
                       " guest_ms=" + u64s(guest_ms) + " what=" + stepText(s);
    if (s.timeout_ms == 0) {
        const std::uint64_t width = timeoutOf(s);
        // `wait` 没有守卫（0）⇒ 别在 trace 里印 `timeout=0(default)`，那看起来像"守卫是 0 ms"。
        line += width == 0 ? std::string(" timeout=none") : " timeout=" + u64s(width) + "(default)";
    }
    traceLine(line);
    if (s.isGate()) {
        state_ = State::Gate;
        return;
    }
    state_ = State::Acting;
    if (s.kind == StepKind::Wait) {
        // ★ `wait` 不碰输入（纯延迟）⇒ 不追加任何动作，只记到期点（宿主墙钟）。
        wait_pending_ = true;
        wait_due_wall_ms_ = wall_ms + s.wait_ms;
        applied_before_ = actuator_.appliedCount();
        ++actions_issued_;
        return;
    }
    issued_ = issueAction(s, frame);
    if (issued_ && s.kind == StepKind::Quit) {
        // ★ `quit` 的语义就是"这次运行到此为止"：⛔ 不要等 actuator 的 applied 计数 ——
        //   `Machine::run` 在**同一个块边界**就会看到 `quitRequested()` 停下来，状态机再没有
        //   机会推进 ⇒ trace 会停在"进了这一步"，读起来像"半截失败"。
        traceLine("action " + stepText(s) + " applied");
        advance(frame, guest_ms, insns, wall_ms);
    }
}

void InteractionGraph::advance(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                               std::uint64_t wall_ms) {
    const Step& s = recipe_.steps()[current_];
    traceLine("step " + s.id + " done");
    ++transitions_;
    if (current_ + 1 >= recipe_.size()) {
        state_ = State::Done;
        traceLine("task done steps=" + u64s(recipe_.size()) + " transitions=" + u64s(transitions_) +
                  " actions=" + u64s(actions_issued_));
        return;
    }
    enterStep(current_ + 1, frame, guest_ms, insns, wall_ms);
}

void InteractionGraph::fail(GraphFailure kind, const std::string& why, const Step& waited,
                            std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns) {
    if (state_ == State::Failed) return;  // ⛔ 失败只报一次（调用方可能会继续 tick）
    state_ = State::Failed;
    failure_ = kind;
    ++transitions_;

    // ---- 快照：失败之后调用方还会继续喂新的观测 ⇒ 报告必须打**那一刻**的值 ----
    fail_identity_ = identity_ != nullptr ? *identity_ : MarkerSample{};
    fail_position_ = position_;
    const InputScript::ReleaseStats rel = actuator_.releaseStats();
    const InputScript::ReleaseInfo last = actuator_.lastRelease();
    const std::uint64_t applied = actuator_.appliedCount();
    const std::uint64_t actions = actuator_.actionCount();

    const std::uint64_t timeout = timeoutOf(waited);
    failure_detail_ = std::string(graphFailureName(kind)) + "：指令 `" + stepText(waited) + "`（" + waited.id +
                      "）超过 " + u64s(timeout) + " ms（frame " + u64s(wait_start_frame_) + ".." + u64s(frame) +
                      "，insn " + u64s(wait_start_insns_) + ".." + u64s(insns) + "，guest_ms " +
                      u64s(wait_start_guest_ms_) + ".." + u64s(guest_ms) + "）" +
                      (why.empty() ? std::string() : "；" + why);

    std::string r;
    r += std::string("interaction-graph: FAIL ") + graphFailureName(kind) + "\n";
    r += "  graph    : " + recipe_.origin() + "\n";
    r += "  step     : " + waited.id + "（行 " + std::to_string(waited.line) + "）\n";
    r += "  waiting  : " + stepText(waited) + "\n";
    // ★ 门带身份 ⇒ 报告必须写**最后一次身份**（cur / id / name；降级要标）：
    //   "等 TITLE 超时"没有身份读数时无法判断是"还在别的屏"还是"识别源失明"。
    if (waited.kind == StepKind::AwaitScript || waited.kind == StepKind::AwaitPosition) {
        if (fail_identity_.valid) {
            r += "  identity : cur=" + u64s(fail_identity_.cur) + " id=" + hex8(fail_identity_.id) +
                 " name=" + fail_identity_.name + (fail_identity_.name_degraded ? "（降级）" : "") + "\n";
        } else {
            r += "  identity : (一次有效身份采样都没有)\n";
        }
    }
    if (waited.kind == StepKind::AwaitPosition) {
        if (fail_position_.valid) {
            r += "  position : pos=" + hex8(fail_position_.pos) + " cur=" + u64s(fail_position_.cur) + "\n";
        } else {
            r += std::string("  position : (读不到：") + (fail_position_.why != nullptr ? fail_position_.why : "?") +
                 ")\n";
        }
    }
    r += "  window   : frame=" + u64s(wait_start_frame_) + ".." + u64s(frame) + "  insn=" +
         u64s(wait_start_insns_) + ".." + u64s(insns) + "  guest_ms=" + u64s(wait_start_guest_ms_) + ".." +
         u64s(guest_ms) + "  timeout=" + u64s(timeout) + "ms" +
         (waited.timeout_ms != 0 ? std::string() : std::string("(default)")) + "\n";
    r += "  actuator : applied=" + u64s(applied) + "/" + u64s(actions) + " releases=" + u64s(rel.releases) +
         "(auto=" + u64s(rel.auto_releases) + ",explicit=" + u64s(rel.explicit_releases) + ")" +
         " deferred_ups=" + u64s(rel.deferred_ups) + " bounded_no_sample=" + u64s(rel.bounded_no_sample) +
         " quit=" + (actuator_.quitRequested() ? "1" : "0") +
         " finished=" + (actuator_.finished() ? "1" : "0") + "\n";
    if (last.valid) {
        r += std::string("             last_release=") + (last.is_key ? "key" : "mouse") +
             " press_frame=" + u64s(last.press_frame) + " release_frame=" + u64s(last.release_frame) +
             " press_ms=" + u64s(last.press_ms) + " release_ms=" + u64s(last.release_ms) +
             " auto=" + (last.auto_hold ? "1" : "0") + "\n";
    }
    if (!why.empty()) r += "  why      : " + why + "\n";
    failure_report_ = r;

    traceLine(std::string("FAIL ") + graphFailureName(kind) + " step=" + waited.id + " waited_frame=" +
              u64s(wait_start_frame_) + ".." + u64s(frame) + " waited_insn=" + u64s(wait_start_insns_) + ".." +
              u64s(insns) + " waited_guest_ms=" + u64s(wait_start_guest_ms_) + ".." + u64s(guest_ms));
}

bool InteractionGraph::tick(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms,
                            std::uint64_t insns, std::uint64_t wall_ms, const MarkerSample* identity,
                            const ScriptPositionSample* position) {
    (void)wm;  // 动作只经 actuator；`wm` 留给调用方形状（与 plan 的入口签名对称）
    if (recipe_.steps().empty()) return false;
    if (state_ == State::Done || state_ == State::Failed) return false;  // ⛔ 失败 / 完成不重复报
    // ★ 判定只读**这次传进来的**观测（⛔ 不在本类里采样）。
    identity_ = identity;
    position_ = position != nullptr ? *position : ScriptPositionSample{};
    writeTraceHeader();

    if (state_ == State::Idle) {
        traceLine("task start origin=" + baseNameOf(recipe_.origin()) + " steps=" + u64s(recipe_.size()));
        enterStep(0, frame, guest_ms, insns, wall_ms);
        return true;
    }

    const Step& s = recipe_.steps()[current_];
    switch (state_) {
        case State::Gate: {
            noteProgress(frame, insns, wall_ms);  // 停滞兜底的时钟：insns / frame 变了就是"有进展"
            if (stepGateHolds(s, identity, position_)) {
                traceLine("when " + stepText(s) + " hit");
                ++transitions_;
                advance(frame, guest_ms, insns, wall_ms);
                return true;
            }
            if (windowExpired(timeoutOf(s), guest_ms, wall_ms)) {
                fail(GraphFailure::Timeout, "条件在窗口内一次都没成立", s, frame, guest_ms, insns);
                return true;
            }
            return false;
        }
        case State::Acting: {
            if (!issued_) {
                // actuator 还没准备好接动作（还有未应用的动作 / 未完成的抬起）：保留这条指令，
                // 下个块边界重试 —— ⛔ 不丢、⛔ 不因此判失败（除窗口到期）。
                if (issueAction(s, frame)) {
                    issued_ = true;
                } else if (windowExpired(timeoutOf(s), guest_ms, wall_ms)) {
                    fail(GraphFailure::Timeout, "动作一直没能追加进 InputScript（等待窗口到期）", s, frame, guest_ms,
                         insns);
                    return true;
                } else {
                    return false;
                }
            }
            bool applied = false;
            if (wait_pending_) {
                // ★ `wait <ms>` = 宿主墙钟（与 `--input-plan` 同一口径）。
                applied = wall_ms >= wait_due_wall_ms_;
            } else {
                // ★ 动作真的被应用 = 追加进去的那一批**全部**应用完。
                applied = actuator_.appliedCount() > applied_before_ &&
                          actuator_.appliedCount() == actuator_.actionCount();
            }
            if (!applied) {
                // ★ **必须**在这里也有到期判定：`InputScript` 的帧停滞兜底是**一次性**的
                //   （`stall_fired_`），一旦折叠过，后面的 `at frame` 动作就没有第二次兜底。
                //   没有这一段时，动作永远不完成 ⇒ 配方永远停在 `acting`：不失败、不 done、不退出。
                if (windowExpired(timeoutOf(s), guest_ms, wall_ms)) {
                    fail(GraphFailure::Timeout, "动作在窗口内一直没能应用（等待窗口到期）", s, frame, guest_ms, insns);
                    return true;
                }
                return false;
            }
            wait_pending_ = false;
            traceLine("action " + stepText(s) + " applied");
            ++transitions_;
            advance(frame, guest_ms, insns, wall_ms);
            return true;
        }
        case State::Idle:
        case State::Done:
        case State::Failed:
            return false;
    }
    return false;
}

}  // namespace lift
