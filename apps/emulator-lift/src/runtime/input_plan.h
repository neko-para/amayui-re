// emulator-lift · 输入**投递层**（S1 骨架 + 三条指令的 AOT 输入配方）
//
// 口径来源：`REQ-01M7HBG0NYH957M9EK3JVMF5CC`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）；
//   投递层本身的口径来自 `REQ-01M7G7EMX3JR272D8FVM7JFY9D`。
//   设计/落点/偏差 ⇒ `docs/03-emulator/input-plan.md`（⛔ 本文件不复述那篇的读数）。
//
// =====================================================================================
// 三条铁律（写在这里 = 后来人的第一道闸；改之前先读一遍）
//
// 1. **计划不拥有任何 guest 可见状态**。唯一状态源仍是 `WindowManager`（`host_window.h` 的
//    `InputSource` 段就是它的合同）。计划只经 `runtime/input.h` 既有的施加函数
//    （`mouseMove` / `mouseButton`）把输入交给它 —— 与 SDL 真人输入**同一段代码**。
//    ⛔ 不存在"注入只发消息、电平没动"的第二份状态。
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
// 配方格式（**三条指令**，平铺；`#` 注释、空行忽略）
//
//   await script TITLE      ← 电平门：当前引擎脚本身份 == TITLE 即通过（不是边沿）。
//                             名字 ASCII 大小写不敏感、**两侧**都忽略 `.bin`（`scriptNameMatches`）。
//   wait 500                ← **宿主单调**毫秒（`std::chrono::steady_clock`）；正整数。
//   click 1070 480 right    ← x/y = **1280×720 参考系**；按钮词裸写 `left`/`right`（省略 = left，
//                             ⛔ 不收 `=left`）。
//
//   · **按文件顺序**推进：`atPoint()` 从第一个未消费的指令开始，遇到不成立的指令就停
//     （⛔ 不越过它）。
//   · `await always` / `await presents <n>` / `key <VK>` / 图那套 `node/when/action/next/
//     expect/timeout` ⇒ **解析期硬错**（`文件:行号: 未知指令：…`，调用方 `exit 2`）。
//   · `wait` 是**非阻塞**的：只记 deadline，⛔ 不许 sleep（`atPoint` 在 shim 栈里）；
//     到期由**下一个读点**消费。起点 = 上一条指令生效的那个读点（因此有 0..一个迭代周期的
//     偏差，**不可评估**；`wait` 只作粗推进，⛔ 不当判据 / 不作跨臂比）。
//   · 身份来源 = `MarkerSampler`（**纯读 guest 内存**，`runtime/engine_marker.h`），由装配点
//     调 `attachGuestMemory()` 挂进来；`base==0` / `valid=false` ⇒ 门**不成立**（继续等，⛔ 不报错）。
//   · 坐标在投递前过 `cursor_map.h` 的 `referenceToClientPixel()`（**唯一公式**）：
//     目标 = 主窗口客户区 `HostWindow::client_w/h`（headless 也有；0 ⇒ 退回 1280×720）；
//     `mouseMove` / `mouseButton(down)` / `mouseButton(up)` 共用**同一个**映射结果。
//   · ⛔ 没有每步超时：卡住由 `--time-limit` / `--max-frames` 这层看门狗报。
//
// 判据（见 `tools/test/input-plan-shape.test.mjs` 与 CTest `input-plan-read-points` /
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

#include "runtime/interaction_graph.h"  // GraphCond 词汇 / `scriptNameMatches` / `MarkerSample`

namespace lift {

class WindowManager;
class GuestMemory;

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

// 内部判定点 → 计划的事件词汇（闭集合；今天只消费 `Frame`）。
// ★ 本枚举现在就存在，是为了让 `note()` 的形状**从第一天起**就是"只记账"：
//   ⛔ 不开放字符串、⛔ 不携带 `WindowManager`。
enum class PlanEventKind : std::uint8_t {
    ScriptEdge = 0,  // 引擎脚本身份边沿（解释器 sampleMarker；AOT 没有 ⇒ S3b 已 drop）
    Frame,           // 每次 Present —— **只是摘要读数**（`presents=`），⛔ 不再是门
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
    // 有一个非空的指令表 ⇒ armed。⛔ 本函数不看墙钟、不看帧号。
    bool armed() const { return !steps_.empty(); }

    // ---- 装载（CLI 层；失败 ⇒ false + err 带 `origin:行: 原因`）----
    bool loadFile(const std::string& path, std::string* err);
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    void clear();

    // ---- 装配点（`aot_main.cpp`；`atPoint()` 签名不变）----
    // ★ 身份来源：`MarkerSampler` 只读 guest 内存。⛔ 在 `host.init()` 之前挂没有意义
    //   （地址空间还没有页表 ⇒ 读到空）；解析期也⛔ 不校验身份（H7）。
    void attachGuestMemory(const GuestMemory* mem);

    // ★ 测试缝：注入"宿主单调毫秒"时钟。nullptr = 生产路径（`std::chrono::steady_clock`）。
    //   ⛔ 只给 CTest 用 ⇒ `wait` 的判据不需要真睡（判据 3）。
    using NowFn = std::uint64_t (*)();
    void setClockForTest(NowFn now_ms);

    // ★ **唯一**能施加 step 的入口（也只经 `runtime/input.h` 的四个投递入口到达）。
    //   flush-before-read：先把到期 step 落地到 `WindowManager`，**再**让 shim 去读。
    void atPoint(WindowManager& wm, InputPoint point);

    // ★ 记账函数：**不接受 `WindowManager`**、不持有计划指针、只能累加计数。
    //   ⇒ 它**不可能**施加到期 step；到期 step 只能由**下一个读点**消费。
    //   ⛔ 别把它改成 `note(WindowManager&, …)`：那会把"块边界 pump"放回 `sampleMarker`。
    void note(PlanEventKind kind);

    // ★ 收尾：把**所有**待抬起的按钮抬起（⚠ 不是 step、不推进入口）。
    //   调用点只有一次性的收口（AOT 的 `aot_main` 跑完之后 / CTest 的收尾）——
    //   保证"下一次 run 不会看到按钮一直按着"。返回抬起个数。
    std::size_t releaseAll(WindowManager& wm);

    // 读数（守卫 ③ 的关系断言 / 摘要行用；⛔ 不是性能判据）。
    std::uint64_t deliverCalls(InputPoint point) const;
    std::uint64_t noteCalls(PlanEventKind kind) const;
    std::uint64_t steps() const { return static_cast<std::uint64_t>(steps_.size()); }
    // 已消费的**指令**数（含 `await script` / `wait`；= `next_`）。
    std::uint64_t stepsApplied() const { return applied_; }
    // **已施加的 `click`** 数（摘要 `fired=`；`await` / `wait` 不算"动作"）。
    std::uint64_t fired() const { return fired_; }
    std::uint64_t releases() const { return released_; }
    std::uint64_t presenting() const { return event_calls_[static_cast<std::size_t>(PlanEventKind::Frame)]; }
    std::size_t pendingReleases() const { return releases_pending_; }
    const std::string& origin() const { return origin_; }
    // 一行摘要（**一次性**输出；⛔ 不许每帧打）。
    std::string summary() const;

private:
    // 平铺指令流的三种指令（⛔ 不是图语法：一个 step 不再"自带一个门 + 一个动作"）。
    enum class InstrKind : std::uint8_t { AwaitScript = 0, Wait = 1, Click = 2 };

    struct PlanStep {
        InstrKind kind = InstrKind::AwaitScript;
        // AwaitScript
        std::string script;                 // 名字原文（⛔ 不参与判定：判定走 `scriptNameMatches`）
        // Wait（运行时状态：第一次到达这条指令的那个读点 = 起点）
        std::uint64_t wait_ms = 0;
        std::uint64_t wait_deadline_ms = 0;
        bool wait_started = false;
        // Click
        int x = 0;                          // 1280×720 参考系
        int y = 0;
        MouseButton button = MouseButton::Left;
        int line = 0;
    };
    struct PendingRelease {
        MouseButton button = MouseButton::Left;
        int x = 0;                       // **已缩放**的客户区坐标（move / down / up 共用）
        int y = 0;
        std::uint64_t press_samples = 0;  // 按下那一刻的 `inputSampleCount`
        std::uint64_t press_reads = 0;    // 按下那一刻的读点序号（兜底抬起用）
    };

    // 采一次身份（同 `(cur,id)` + 名字表签名不变 ⇒ 复用缓存的名字，不重解析）。
    void refreshIdentity();
    std::uint64_t nowMs() const;
    void applyStep(WindowManager& wm, const PlanStep& s);
    void releaseDue(WindowManager& wm);
    void emitRelease(WindowManager& wm, const PendingRelease& r);

    std::vector<PlanStep> steps_;
    std::vector<PendingRelease> pending_;
    std::string origin_;
    const GuestMemory* mem_ = nullptr;  // 装配点挂；nullptr ⇒ `await script` 恒不成立
    MarkerSampler marker_sampler_;
    MarkerSample identity_;             // 最近一次采样（名字按上面那条规则缓存）
    NowFn now_fn_ = nullptr;            // 测试缝（nullptr = steady_clock）
    std::uint64_t next_ = 0;            // 下一个待判的指令（顺序推进，⛔ 不回跳）
    std::uint64_t applied_ = 0;         // 已消费指令数
    std::uint64_t fired_ = 0;           // 已施加 click 数
    std::uint64_t released_ = 0;
    std::uint64_t read_points_ = 0;     // 读点序号（待抬起的兜底刻度）
    std::size_t releases_pending_ = 0;
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::array<std::uint64_t, kPlanEventCount> event_calls_{};
};

}  // namespace lift
