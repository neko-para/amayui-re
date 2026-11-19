// emulator-lift · 输入配方的**机器内驱动器**（块边界推进；动作经 `InputScript` 落地）
//
// ★ 2026-11-16：用户入口 `--interaction-graph`（+ `--interaction-trace` / `--interaction-fail-dump`）
//   已退役（用户裁决 `REQ-01M7HQ57XV66FZQA5ECGHWD71N`；配方只经 AOT 臂 `--input-plan`）。
//   本类**保留为驱动器**：`Machine::tickInteraction` 仍接在块边界与宿主事件泵上，pure CTest
//   `input-expect` 用它当夹具（"配了 expect-change 之后又挂一份配方 ⇒ 配置期拒绝"那条口径）。
//   ⛔ 今天**没有任何 CLI 参数**能装载它 —— 要复活入口必须重新登记（不是改一行解析）。
//
// ★ 语法 / 顺序模型 / 能力面 ⇒ `runtime/recipe.h` 的文件头（**唯一真源**）。
//   本文件只是"同一份配方在块边界上的推进器"：⛔ 不解析、⛔ 不重新解释"门成立是什么意思"。
// ★ 与 `--input-plan`（AOT 臂）的**唯一**差别：
//   · 推进点 = **块边界**（`Machine::tickInteraction`；plan 在 guest 读点）；
//   · 动作不直接写 `WindowManager`，而是经 `InputScript::appendText` 追加**绝对触发**
//     （`at frame <当前+1>`）—— 由它保证"一次按下一定被 guest 观察到"（见 `runtime/input_script.h`）。
//     ⛔ 不要在这里另写一套按下 / 抬起：那会绕过已守卫的采样语义。
//   · 位置读数在块边界上取一次（`runtime/engine_marker.h` 的 `readScriptPositionSample`，与 plan **同一份**）。
// ★ 失败一律响亮：状态进 `Failed` 并算好一份多行报告（`failureReport()`），⛔ 不 warning-only。
//   顺序模型只有一种失败：`timeout`（某一指令的守卫到期）。"任务没走完"由调用方判（`done()`）；
//   完成度的**结论与退出码口径**只有一份（`recipe.h` 的 `RecipeOutcome` /
//   `recipeOutcomeExitCode`，= 5）—— 两臂同一个数，见 `src/main.cpp` 与 `src/aot/aot_main.cpp`。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/engine_marker.h"
#include "runtime/host_window.h"
#include "runtime/input_script.h"
#include "runtime/recipe.h"

namespace lift {

enum class GraphFailure : std::uint8_t { None = 0, Timeout = 1 };
const char* graphFailureName(GraphFailure f);

// 缺省 `timeout`（配方里没写 `timeout` 的**门**用它；trace 里标 `(default)`）。
// ★ 这是**入口给的策略**（不是能力面）：本臂的"到期"有两把尺（guest 毫秒 + 墙钟停滞兜底），
//   所以给得起 5000 ms 的缺省；AOT 臂只有宿主墙钟一把尺且它的看门狗是 `--time-limit` /
//   `--max-frames` ⇒ 那边不给缺省。宽度公式只有一份：`recipe.h` 的 `stepTimeoutMs`
//   （★ `wait <ms>` 在那里恒为 0：固定时长、没有守卫）。
constexpr std::uint64_t kDefaultStepTimeoutMs = 5000;

class InteractionGraph {
public:
    // ---- 装载（CLI 层；失败 ⇒ false + err 带 `origin:行: 原因`；成功 ⇒ 旧配方被替换）----
    bool loadFile(const std::string& path, std::string* err);
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    void clear();

    bool active() const { return recipe_.active(); }
    const std::vector<Step>& steps() const { return recipe_.steps(); }
    const std::string& origin() const { return recipe_.origin(); }

    // ★ 采样门控（`Machine::run` 的块入口钩子只问它）：配方里**有门** ⇒ 必须采身份。
    //   ⛔ 漏了它 ⇒ 门恒不成立（`last_marker_sample_` 永远是无效样本）。
    bool needsIdentitySamples() const;
    // ★ 有 `await position` ⇒ 块边界上要读一次位置（`tick` 的 `position` 参数）。
    bool needsPositionSamples() const;
    // ★ 恒 true：`wait <ms>` 是**宿主墙钟** ⇒ 同一份配方两次跑的完成时刻不可复现。
    //   调用方据此把 trace 标成 `NON-DIFFABLE`（真源 §4）。
    bool usesWallClock() const { return true; }

    // ---- 结果 ----
    bool done() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Failed; }
    GraphFailure failure() const { return failure_; }
    const std::string& failureDetail() const { return failure_detail_; }
    // 失败时的完整报告（多行；`main` 直接写 stderr）：指令 / 等的是什么 / 等了多久 / 最后身份 / 位置。
    std::string failureReport() const { return failure_report_; }

    // ---- trace（一行一事件；字段只用确定性量）----
    void setTrace(std::FILE* out, bool diffable);
    bool traceEnabled() const { return trace_ != nullptr; }

    // ---- 动作出口：追加进这个 `InputScript`（`Machine` 负责 tick 它）----
    InputScript& actuator() { return actuator_; }
    const InputScript& actuator() const { return actuator_; }

    // 块边界推进。返回 true = 本 tick 追加了动作（或发生了状态迁移）。
    //   `identity` = 最近一次身份采样（可为 nullptr）；`position` = 这一刻的位置读数
    //   （`needsPositionSamples()` 为真时由调用方取；nullptr / invalid ⇒ 位置门不成立）。
    bool tick(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
              std::uint64_t wall_ms, const MarkerSample* identity, const ScriptPositionSample* position);

    // ---- 可观测账（守卫 / run summary / 失败报告用）----
    // 当前指令（Idle/Done/Failed 时为 nullptr）。
    const Step* current() const;
    std::uint64_t stateChanges() const { return transitions_; }
    std::uint64_t actionsIssued() const { return actions_issued_; }
    const char* stateName() const;

private:
    enum class State : std::uint8_t { Idle = 0, Gate = 1, Acting = 2, Done = 3, Failed = 4 };

    // 本臂的守卫宽度 = 共享公式 + 本臂缺省（`recipe.h` 的 `stepTimeoutMs`；⛔ 不再写第二份）。
    //   ★ `wait <ms>` 在共享公式里恒为 0（固定时长、没有守卫）⇒ 这里不再有 `ms + 缺省` 那种补丁。
    std::uint64_t timeoutOf(const Step& s) const { return stepTimeoutMs(s, kDefaultStepTimeoutMs); }
    // 进入第 i 条指令：门 ⇒ 等；动作 ⇒ 立刻施加（quit 一发即收口）。
    void enterStep(std::size_t index, std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                   std::uint64_t wall_ms);
    // 动作已应用（或没有动作）⇒ 推进到下一指令（末条 ⇒ Done）。
    void advance(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms);
    // 追加动作到 actuator。返回 false = actuator 还没准备好（本 tick 不丢命中，下个块边界重试）。
    bool issueAction(const Step& s, std::uint64_t frame);
    void fail(GraphFailure kind, const std::string& why, const Step& waited, std::uint64_t frame,
              std::uint64_t guest_ms, std::uint64_t insns);
    // 两把尺都到期才叫到期：`guest_ms` 超过窗口 + `insns`/`frame` 都不推进的墙钟超过窗口
    bool windowExpired(std::uint64_t timeout_ms, std::uint64_t guest_ms, std::uint64_t wall_ms) const;
    // 停滞兜底的记账：`insns` 或 `frame` 变了 ⇒ "最近一次有进展"就是现在
    void noteProgress(std::uint64_t frame, std::uint64_t insns, std::uint64_t wall_ms);
    void writeTraceHeader();
    void traceLine(const std::string& line);

    Recipe recipe_;
    State state_ = State::Idle;
    std::size_t current_ = 0;
    GraphFailure failure_ = GraphFailure::None;
    std::string failure_detail_;
    std::string failure_report_;
    // 等待窗口的起点（判定只读单调计数 + 这几个时刻）
    std::uint64_t wait_start_guest_ms_ = 0;
    std::uint64_t wait_start_frame_ = 0;
    std::uint64_t wait_start_insns_ = 0;
    std::uint64_t wait_start_wall_ms_ = 0;
    std::uint64_t last_progress_wall_ms_ = 0;
    std::uint64_t last_progress_frame_ = 0;
    std::uint64_t last_progress_insns_ = 0;
    // 动作步的账：`issued_` = 已追加进 actuator；`applied_before_` = 追加时的 applied 计数
    bool issued_ = false;
    std::uint64_t applied_before_ = 0;
    // ★ S6：图自己的"最后一次 mouse_move 的坐标"（`mouse_down/up` 的临时解析器要求 `have_pos_`，
    //   所以 `issueAction` 要在同一段文本里补一条同点 `mouse_move`）。解析期已保证 down/up 前面
    //   一定有一条 `mouse_move` ⇒ 这里只防御性判空。
    int last_move_x_ = 0;
    int last_move_y_ = 0;
    bool have_move_ = false;
    // Wait 動作的到期点（宿主墙钟）
    bool wait_pending_ = false;
    std::uint64_t wait_due_wall_ms_ = 0;
    // 本次 tick 的观测（失败报告要快照它们：失败之后调用方还会继续 tick）
    const MarkerSample* identity_ = nullptr;
    MarkerSample fail_identity_;
    ScriptPositionSample position_;
    ScriptPositionSample fail_position_;
    // trace
    std::FILE* trace_ = nullptr;
    bool trace_diffable_ = true;
    bool trace_header_done_ = false;
    // 账
    std::uint64_t transitions_ = 0;
    std::uint64_t actions_issued_ = 0;
    InputScript actuator_;
};

}  // namespace lift
