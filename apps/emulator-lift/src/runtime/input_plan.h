// emulator-lift · 输入**投递层**（S1 骨架 + S2′ 门 + S4 AOT 控制面）
//
// 口径来源：`REQ-01M7G7EMX3JR272D8FVM7JFY9D`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）
//   ⇒ `.tmp/aot/input/design.md`（原设计，有错）+ `.tmp/aot/input-review/review.md`（复审，逐条纠错）。
//
// =====================================================================================
// 三条铁律（写在这里 = 后来人的第一道闸；改之前先读一遍）
//
// 1. **计划不拥有任何 guest 可见状态**。唯一状态源仍是 `WindowManager`（`host_window.h` 的
//    `InputSource` 段就是它的合同）。计划只经 `runtime/input.h` 既有的施加函数
//    （`mouseMove` / `mouseButton` / `key`）把输入交给它 —— 与 SDL 真人输入
//    **同一段代码**。⛔ 不存在"注入只发消息、电平没动"的第二份状态。
//
// 2. **计划不在执行循环里被调用**。允许的调用点只有 guest 自己的输入读点（11 个投递入口，
//    见 `input.h`）。⛔ `src/runtime/machine.cpp` / `src/aot/aot_run.cpp` 里出现
//    `InputPlan` / `input::` / `deliver(` / `atPoint(` / `note(` 就是回归
//    （守卫 `tools/test/input-plan-shape.test.mjs` ①）。
//
// 3. ★ **能施加 step 的 API 只有 `atPoint()` 一条，而它只经那四个投递入口到达，
//    且四个入口都要求一个 guest 读上下文（`WindowManager&` + `InputPoint`）。**
//    ⇒ 这是复审 S-1 点名的全篇最危险处：原设计的 `note(WindowManager&, …)` 既能施加 step
//      又被允许留在 `Machine::sampleMarker()`（它就在 `Machine::run` 的**块边界 `if`** 里）
//      ⇒ 那等于"块边界 pump"。**结构性堵死**：`note()` **不接受 `WindowManager`**、不持有
//      计划指针、只累加事件计数；到期 step 只能由**下一个读点**消费。
//    ⚠ 如果你发现需要给记账函数传 `WindowManager&`，那说明设计错了 —— 停下报告。
//
// =====================================================================================
// S4 + S2′ 的形状
//
//   · **门只做 `{Always, Presents}`**（复审裁定的最小集）。`Presents` = guest **累计** Present
//     帧数 ≥ n（绝对），由 `present.cpp` 在 `++frame_count_` 同一刻 `note(PlanEventKind::Frame)`
//     推 ⇒ **guest 可见、确定、两臂同一份**。⛔ 墙钟不参与"哪一步生效"（本文件里一次时钟都不取）。
//   · ⛔ 不做 `Reads` / `Insns` / `MessageTaken` / `FileReadDone` / `GuestBlocked`：复审裁定它们对
//     "到某界面之后按键"都不是必需，且 `Reads` 的刻度不是读点次数（见父单正文）。
//   · **flush-before-read**：`atPoint()` 在**读之前**把到期 step 施加到 `WindowManager`。
//     电平类（`GetAsyncKeyState` / `GetCursorPos`）读时按当前状态求值；队列类（`PeekMessage*` /
//     `GetMessage*`）push 进 FIFO —— 两者由 `input::*` 同一条路径落地，**同一次读就能看到**。
//   · **待抬起队列**：按下之后一定被明确抬起（键盘 keyup / 鼠标键松开）。判据只用**计数**：
//     `inputSampleCount()` 前进过一拍（与既有 `click` 的 auto-hold 同一条"一次按下一定被观察到"），
//     或连续 `kForceReleaseReads` 个读点兜底；★ 收尾由调用方 `releaseAll()` 保证**跑结束前清空**。
//   · **配方格式 = `--interaction-graph` 的线性写法的一个子集**（⛔ 没有第二种 DSL）：
//         await always            ← 起手：第一个 guest 读点就按
//         click 640 360           ← 动作行（同一 step）：click / key
//         await presents 120      ← guest 累计 Present 到第 120 帧之后
//         key 13
//     解析复用的是 `InteractionGraph::loadText(..., allow_plan_conds=true)` 那一份**同一份**解析器；
//     本文件只把 `Node` IR 投影成"门 + 动作"，并**拒绝**超出最小集的门 / 动作（带行号）。
//
// 判据（三条守卫，见 `tools/test/input-plan-shape.test.mjs` 与 CTest `input-plan-read-points` /
// `cli-aot-flags`）：
//   ① 源码形态：执行循环里 0 命中；
//   ② 读点闭环（**按谓词**，不按名单）：读 guest 可见输入态/消息队列的已注册 shim
//     ⟺ 函数体里调了投递入口 —— 两侧都不许单边为真；
//   ③ 关系断言：`plan.deliverCalls(point)` 与 `ShimDef::call_count` 是**两个独立来源**，逐点相等。
// =====================================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/interaction_graph.h"  // ★ 复用 CondKind / GraphAction 词汇（⛔ 不造第二种格式）

namespace lift {

class WindowManager;

// 11 个投递点（**闭集合**；读路径已由复审穷举核过）：
//   9 个读点 + 1 个写点（`SetCursorPos`）+ 1 个事件源（`ReadFile` —— ⛔ 它不是"输入族"，
//   只是"文件读完"的事件源；守卫 ② 用 `std::fread(` 这个谓词把它与输入族分开判）。
// ⛔ 排除：`timeGetTime` / `GetTickCount`（时钟不是输入）与 `WaitForSingleObject` 族
//   （今天 `shimAbort`）。`SetCursor` / `ShowCursor` 只改 HCURSOR / 显示计数，不参与。
enum class InputPoint : std::uint8_t {
    KeyLevel = 0,    // user32!GetAsyncKeyState —— 电平
    CursorPos,       // user32!GetCursorPos —— 电平
    PeekMsg,         // user32!PeekMessageA —— 队列
    GetMsg,          // user32!GetMessageA —— 队列（+ 阻塞点，S5）
    MsgWait,         // user32!MsgWaitForMultipleObjects —— 队列（按 dwWakeMask 扫，不取走）
    WaitMessage,     // user32!WaitMessage —— 队列
    QueueStatus,     // user32!GetQueueStatus —— 队列状态位
    Translate,       // user32!TranslateMessage —— 派生电平（shift/caps）
    Dispatch,        // user32!DispatchMessageA —— 队列消费端（顺序要对）
    SetCursorPos,    // user32!SetCursorPos —— **写点**（所有权：不被计划顶掉）
    FileRead,        // kernel32!ReadFile —— **事件源**（⛔ 不算输入读点）
    Count,
};

constexpr std::size_t kInputPointCount = static_cast<std::size_t>(InputPoint::Count);
const char* inputPointName(InputPoint point);

// 内部判定点 → 计划的事件词汇（闭集合；本单只消费 `Frame`）。
// ★ 本枚举现在就存在，是为了让 `note()` 的形状**从第一天起**就是"只记账"：
//   ⛔ 不开放字符串、⛔ 不携带 `WindowManager`。
enum class PlanEventKind : std::uint8_t {
    ScriptEdge = 0,  // 引擎脚本身份边沿（解释器 sampleMarker；AOT 没有 ⇒ S3b 已 drop）
    Frame,           // ★ 每次 Present（= `presents` 门，同一个量）—— 本单唯一生产者
    ScreenSample,    // 帧采样（与 Frame 同一触发点；本单不消费）
    MessageTaken,    // guest 取走一条消息（本单不消费）
    GuestBlocked,    // 空队列、即将进阻塞泵（本单不消费）
    FileReadDone,    // 一次 guest 读文件完成（本单不消费）
    Count,
};

constexpr std::size_t kPlanEventCount = static_cast<std::size_t>(PlanEventKind::Count);

// =====================================================================================
// `InputPlan` —— 宿主对象（**不拥有任何 guest 可见状态**）
// =====================================================================================
class InputPlan {
public:
    // 有一个非空的 step 表 ⇒ armed。⛔ 本函数不看墙钟、不看帧号。
    bool armed() const { return !steps_.empty(); }

    // ---- 装载（CLI 层；失败 ⇒ false + err 带 `origin:行: 原因`）----
    bool loadFile(const std::string& path, std::string* err);
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    void clear();

    // ★ **唯一**能施加 step 的入口（也只经 `runtime/input.h` 的四个投递入口到达）。
    //   flush-before-read：先把到期 step 落地到 `WindowManager`，**再**让 shim 去读。
    void atPoint(WindowManager& wm, InputPoint point);

    // ★ 记账函数：**不接受 `WindowManager`**、不持有计划指针、只能累加计数。
    //   ⇒ 它**不可能**施加到期 step；到期 step 只能由**下一个读点**消费。
    //   ⛔ 别把它改成 `note(WindowManager&, …)`：那会把"块边界 pump"放回 `sampleMarker`。
    void note(PlanEventKind kind);

    // ★ 收尾：把**所有**待抬起的键 / 按钮抬起（⚠ 不是 step、不推进入口）。
    //   调用点只有一次性的收口（AOT 的 `aot_main` 跑完之后 / CTest 的收尾）——
    //   保证"下一次 run 不会看到键一直按着"。返回抬起个数。
    std::size_t releaseAll(WindowManager& wm);

    // 读数（守卫 ③ 的关系断言 / 摘要行用；⛔ 不是性能判据）。
    std::uint64_t deliverCalls(InputPoint point) const;
    std::uint64_t noteCalls(PlanEventKind kind) const;
    std::uint64_t steps() const { return static_cast<std::uint64_t>(steps_.size()); }
    std::uint64_t stepsApplied() const { return applied_; }
    std::uint64_t releases() const { return released_; }
    std::uint64_t presenting() const { return event_calls_[static_cast<std::size_t>(PlanEventKind::Frame)]; }
    std::size_t pendingReleases() const { return releases_pending_; }
    const std::string& origin() const { return origin_; }
    // 一行摘要（**一次性**输出；⛔ 不许每帧打）。
    std::string summary() const;

private:
    struct PlanStep {
        CondKind gate = CondKind::Always;  // ★ 只会有 Always / Presents（`loadText` 拒绝其余）
        std::uint64_t presents = 0;        // Presents：目标**绝对**帧数
        GraphAction action;
        int line = 0;
    };
    struct PendingRelease {
        bool is_key = false;
        std::uint32_t vk = 0;
        MouseButton button = MouseButton::Left;
        int x = 0;
        int y = 0;
        std::uint64_t press_samples = 0;  // 按下那一刻的 `inputSampleCount`
        std::uint64_t press_reads = 0;    // 按下那一刻的读点序号（兜底抬起用）
    };

    bool gateHolds(const PlanStep& s) const;
    void applyStep(WindowManager& wm, const PlanStep& s);
    void releaseDue(WindowManager& wm);
    void emitRelease(WindowManager& wm, const PendingRelease& r);

    std::vector<PlanStep> steps_;
    std::vector<PendingRelease> pending_;
    std::string origin_;
    std::uint64_t next_ = 0;          // 下一个待判的 step（顺序推进，⛔ 不回跳）
    std::uint64_t applied_ = 0;
    std::uint64_t released_ = 0;
    std::uint64_t read_points_ = 0;   // 读点序号（待抬起的兜底刻度）
    std::size_t releases_pending_ = 0;
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::array<std::uint64_t, kPlanEventCount> event_calls_{};
};

}  // namespace lift
