// emulator-lift · 输入**投递层**（AOT 臂入口：在 guest **读点**推进同一份配方）
//
// 口径来源：`REQ-01M7HBG0NYH957M9EK3JVMF5CC`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）；
//   投递层本身的口径来自 `REQ-01M7G7EMX3JR272D8FVM7JFY9D`。
//   设计/落点/偏差 ⇒ `docs/03-emulator/input-plan.md`（⛔ 本文件不复述那篇的读数）。
// ★ **语法与顺序模型不在这里**：配方由 `runtime/recipe.h` 解析（**唯一一份**；机器内驱动器
//   `InteractionGraph` 读的也是它），
//   本文件只负责"在 guest 读点上把到期的那一条指令**施加**到 `WindowManager`"。
//   ⛔ 不要在这里再写一份解析 / 再解释一次"门成立是什么意思"。
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
// 能力面 = **共享的那一份**（`runtime/recipe.h`；⛔ 本臂没有自己的指令表）
//   六条指令 + 行尾 `timeout` 全认：`await script` / `await position` / `wait <ms>` /
//   `click x y [l|r]` / `profile_begin|end <label>` / `quit`（`timeout <ms>` 只能写在两个 `await`
//   那一行的末尾）。本臂只决定它们**落在哪个宿主对象**上：
//   · `click` → `input::mouseMove` / `mouseButton`（唯一施加路径）+ 待抬起队列；
//   · `wait` → 非阻塞 deadline（宿主单调毫秒），到期由**下一个读点**消费（★ 固定时长、没有守卫）；
//   · 门上的 `timeout` → 本臂**唯一**的尺 = 宿主墙钟（`stepTimeoutMs` 给宽度；本臂不给缺省值 ⇒
//     没写 `timeout` 就没有守卫，卡住由 `--time-limit` / `--max-frames` 这层看门狗报）；
//   · `profile_begin|end` → 本臂没有区间 profiler（`WindowProfiler` 是解释器块循环的仪器）
//     ⇒ 落成**宿主墙钟区间读数**（一次性两行：begin / end + 区间读数），可与 `--profile` 的
//     整轮读数对齐；⛔ 不假装开了窗口 profiler；
//   · `quit` → `WindowManager::postQuit(0)`（与解释器臂**同一个**交付点：`machine.cpp` 的
//     `quit` 也是投 WM_QUIT）⇒ guest 的消息循环拿到 WM_QUIT 后自己退出；本臂随即不再推进。
//   · **按文件顺序**推进：从第一条未消费的指令开始，遇到不成立的指令就停（⛔ 不越过它）。
//   · 身份来源 = `MarkerSampler`（**纯读 guest 内存**，`runtime/engine_marker.h`），由装配点
//     调 `attachGuestMemory()` 挂进来；`base==0` / `valid=false` ⇒ 门**不成立**（继续等，⛔ 不报错）。
//   · 位置门同样只读 guest 内存（`readScriptPositionSample`，与图臂**同一份**）；读不到 ⇒ 门
//     **不成立**（继续等），并记一个内部计数 `positionMisses()`（缺省不打印）。
//   · 坐标在投递前过 `cursor_map.h` 的 `referenceToClientPixel()`（**唯一公式**）：
//     目标 = 主窗口客户区 `HostWindow::client_w/h`（headless 也有；0 ⇒ 退回 1280×720）；
//     `mouseMove` / `mouseButton(down)` / `mouseButton(up)` 共用**同一个**映射结果。
//
// ★ **第二条生产者 = 控制层的立即队列**（M1 · `REQ-01M7J2P5S27VX7WMRR3ZC9K2E8`）：
//   控制层的 `click` 在**暂停态**入队（`enqueueClick`），由**下一个 guest 读点**（`atPoint`）
//   **先于**配方当步施加。它 **不推进** 配方的 `next_` / `applied_`（配方里未到期的门照旧等），
//   但走**同一段**施加核心（`applyClickAt` ⇒ 同一份 `referenceToClientPixel` + 同一份
//   `input::*` + 同一份 down→待抬起记账），并计入 `fired()`。⛔ 不许为它复制第二套坐标映射 /
//   第二套按钮记账 / 第二个施加点（守卫：`lift-control-endpoint` 的 click 用例）。
//
// ★ **完成度是判据**（`REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置）：跑完时 `pending != 0`
//   （任务没走完）或某一步的 `timeout` 到期 ⇒ `outcome()` 不是 `Done` ⇒ CLI 层按
//   `recipeOutcomeExitCode()` 给 **5** 并点名那一步（`failureReport()` 带 `文件:行` + 门/动作）。
//   ⛔ 不许只打一行警告仍 exit 0。
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

#include "runtime/engine_marker.h"
#include "runtime/recipe.h"

namespace lift {

class WindowManager;
class GuestMemory;
class VirtualClock;

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
    bool armed() const { return recipe_.active(); }

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

    // ★ **暂停补偿**（控制层 S1）：本臂的守卫钟 = 宿主单调毫秒 − 累计暂停毫秒。
    //   为什么必须有：每步的 `timeout` 用的是本文件**自己的** `steady_clock`（不是 guest 的
    //   `VirtualClock`）⇒ 只冻结 guest 时钟时，一次 2 s 暂停会把配方里 `timeout 1000` 的窗口走掉
    //   （裁决：`body-m0.md` 末节"冻结必须同时补偿这两把钟"）。
    //   `clock` = 宿主那份 `VirtualClock`（`pausedTotalMs()` 是唯一读数）；nullptr = 不减（缺省档逐位不变）。
    //   ⛔ 不在这里另存一份暂停账：暂停账的唯一来源是 `VirtualClock`。
    void attachClock(const VirtualClock* clock);

    // ★ **窗口 profile 的标记钩子**（`profile_begin` / `profile_end` 的唯一外部消费者）。
    //   · 为什么是裸函数指针 + `void*`：缺省 nullptr ⇒ 每个读点只多一次判空（⛔ 不用
    //     `std::function`：那是一次间接跳转 + 可能的分配，而"不开窗零成本"是硬口径）。
    //   · ⛔ **不改配方语法**：钩子只在既有 `applyProfileMark` 的**成功开/收口**处回调
    //     （被忽略的重复 `begin` / 无主的 `end` 不回调 —— 它们没开窗，也就不该有关窗）。
    using ProfileMarkHook = void (*)(void* ctx, const char* label, bool begin);
    void setProfileMarkHook(void* ctx, ProfileMarkHook fn) {
        mark_ctx_ = ctx;
        mark_fn_ = fn;
    }

    // ★ **唯一**能施加 step 的入口（也只经 `runtime/input.h` 的四个投递入口到达）。
    //   flush-before-read：先把到期 step 落地到 `WindowManager`，**再**让 shim 去读。
    //   ★ 同一个读点内的顺序：待抬起 → **立即队列（控制层 `click`）** → 配方当步。
    void atPoint(WindowManager& wm, InputPoint point);

    // =====================================================================================
    // ★ **立即队列**（控制层 `click` 的唯一落点；M1）
    //   · 生产者 = 控制层（**主线程**的 `Control::serve()`，只在暂停态）；
    //   · 消费者 = 本层：下一个 guest 读点（`atPoint`），**先于**配方当步；
    //   · ⛔ 不推进配方（`next_` / `applied_` / 守卫账一位不动）⇒ 未到期的门照旧等；
    //   · 施加走 `applyClickAt`（与配方 `click` **同一段**代码）并计入 `fired()`；
    //   · **有界**：满了 `enqueueClick` 返回 false ⇒ 控制层回 `bad_request`（⛔ 静默丢）。
    //   ★ 没有控制层时这条队列**恒空** ⇒ `atPoint` 的早退条件与默认档逐字节不变。
    // =====================================================================================
    static constexpr std::size_t kImmediateClickCapacity = 64;
    // `ref_x` / `ref_y` = **1280×720 参考坐标**（与配方 `click x y` 同一口径；已由控制层校验）。
    // 返回 false = 队列已满（⛔ 不排队、不丢旧的）。
    bool enqueueClick(int ref_x, int ref_y, MouseButton button);
    std::size_t queuedClicks() const { return immediate_.size(); }

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
    std::uint64_t steps() const { return static_cast<std::uint64_t>(recipe_.size()); }
    // 已消费的**指令**数（含 `await script` / `await position` / `wait`；= `next_`）。
    std::uint64_t stepsApplied() const { return applied_; }
    // **还没消费**的指令数（摘要 `pending=`；= steps − applied）。≠ 0 ⇒ 任务没走完。
    std::uint64_t pending() const {
        return static_cast<std::uint64_t>(recipe_.size()) - applied_;
    }
    // **已施加的 `click`** 数（摘要 `fired=`；`await` / `wait` / `profile_*` / `quit` 不算"动作"）。
    // ★ **两种生产者同一口径**：配方 step 与控制层立即队列的 click 都记这里
    //   （判据 1 的"两边 fired 相同"读的就是这一格）。
    std::uint64_t fired() const { return fired_; }
    std::uint64_t releases() const { return released_; }
    // 位置门"这一刻读不到位置"的次数（base==0 / pc 越界 / 未提交）。⛔ 缺省不打印（诊断用）。
    std::uint64_t positionMisses() const { return position_misses_; }
    std::uint64_t presenting() const {
        return event_calls_[static_cast<std::size_t>(PlanEventKind::Frame)];
    }
    std::size_t pendingReleases() const { return releases_pending_; }
    // 已**正常收口**的区间数（`profile_begin` + `profile_end` 配对；读数，⛔ 不是性能判据）。
    std::uint64_t profileWindows() const { return profile_windows_; }
    bool profileOpen() const { return profile_open_; }
    const std::string& origin() const { return recipe_.origin(); }
    // 一行摘要（**一次性**输出；⛔ 不许每帧打）。
    std::string summary() const;

    // ---- 完成度（**判据**；`aot_main` 据此给退出码）----
    //   `TimedOut` = 某一步的守卫到期；`Running` = 跑完了还有指令没消费（任务没走完）；
    //   `Done` = 全部指令走完。⚠ 「跑完」= guest 侧停机（`--max-frames` / `--time-limit` / 退出），
    //   ⛔ 不是"计划自己说完成"。
    RecipeOutcome outcome() const;
    bool failed() const { return failed_; }
    // 没走完 / 超时时该打什么（多行；**点名** `文件:行` + 门/动作 + 进度）。`Done` ⇒ 空串。
    //   ★ `aot_main` 直接写 stderr；⛔ 不在这里打（同一个结论只在一个出口）。
    std::string failureReport() const;
    // 当前"卡住的那一条"（`TimedOut` / `Running` 且 pending ≠ 0；否则 nullptr）。
    const Step* currentStep() const;
    // 第 `index` 条指令（越界 ⇒ nullptr；报错定位 / 守卫用）。
    const Step* stepAt(std::size_t index) const {
        return index < recipe_.size() ? &recipe_.steps()[index] : nullptr;
    }
    // **宿主停止请求**的句柄（`quit` 一落地就为真；交给 `AotRunOptions::stop_requested`）。
    //   ★ 只有这一个生产者：`quit` 指令。⛔ 不要用它表达"配方没走完"（那是 `outcome()`）。
    const bool* quitFlag() const { return &quit_requested_; }
    bool quitRequested() const { return quit_requested_; }

private:
    struct PendingRelease {
        MouseButton button = MouseButton::Left;
        int x = 0;                       // **已缩放**的客户区坐标（move / down / up 共用）
        int y = 0;
        std::uint64_t press_samples = 0;  // 按下那一刻的 `inputSampleCount`
        std::uint64_t press_reads = 0;    // 按下那一刻的读点序号（兜底抬起用）
    };

    // 立即队列的一条：**1280×720 参考坐标**（⛔ 还没映射 —— 映射只发生在施加那一刻的那一次）。
    struct ImmediateClick {
        int x = 0;
        int y = 0;
        MouseButton button = MouseButton::Left;
    };

    // 采一次身份（同 `(cur,id)` + 名字表签名不变 ⇒ 复用缓存的名字，不重解析）。
    void refreshIdentity();
    std::uint64_t nowMs() const;
    // ★ **唯一**的 click 施加核心：配方 step 与立即队列**都**走它
    //   （同一份 `referenceToClientPixel` + 同一份 `input::*` + 同一份 down→待抬起记账）。
    void applyClickAt(WindowManager& wm, int ref_x, int ref_y, MouseButton button);
    // 配方 step 的壳：只把 `Step` 的三个值交给上面那一段（⛔ 不复制映射 / 记账）。
    void applyClick(WindowManager& wm, const Step& s);
    // 立即队列消费（同一次读点内、配方之前；⛔ 不碰 `next_` / `applied_`）。
    void drainImmediate(WindowManager& wm);
    // 区间边界的**本臂落点**（没有窗口 profiler ⇒ 一次性两行宿主墙钟读数）。
    void applyProfileMark(const Step& s, std::uint64_t now_ms);
    // `quit` 的**本臂落点**：与解释器臂同一个交付点 = `WindowManager::postQuit(0)`。
    void applyQuit(WindowManager& wm);
    // 某一步的守卫到期 ⇒ 记下是哪一条（下标）/ 等了多久（**唯一**失败面；⛔ 不 warning-only）。
    void failTimeout(std::size_t index, std::uint64_t elapsed_ms, std::uint64_t guard_ms);
    void releaseDue(WindowManager& wm);
    void emitRelease(WindowManager& wm, const PendingRelease& r);

    Recipe recipe_;
    std::vector<PendingRelease> pending_;
    // 控制层投进来的 click（**第二条生产者**；有界，见 `kImmediateClickCapacity`）。
    std::vector<ImmediateClick> immediate_;
    const GuestMemory* mem_ = nullptr;  // 装配点挂；nullptr ⇒ 门恒不成立
    MarkerSampler marker_sampler_;
    MarkerSample identity_;             // 最近一次采样（名字按上面那条规则缓存）
    NowFn now_fn_ = nullptr;            // 测试缝（nullptr = steady_clock）
    const VirtualClock* clock_ = nullptr;  // 暂停补偿（nullptr = 不减；见 attachClock）
    std::uint64_t next_ = 0;            // 下一个待判的指令（顺序推进，⛔ 不回跳）
    std::uint64_t applied_ = 0;         // 已消费指令数
    std::uint64_t fired_ = 0;           // 已施加 click 数
    std::uint64_t released_ = 0;
    std::uint64_t position_misses_ = 0;  // 位置门读不到位置的次数（诊断；缺省不打印）
    std::uint64_t read_points_ = 0;     // 读点序号（待抬起的兜底刻度）
    std::size_t releases_pending_ = 0;
    // `wait` 的 deadline（宿主毫秒；起点 = 第一次到达这条指令的那个读点）
    bool wait_started_ = false;
    std::uint64_t wait_deadline_ms_ = 0;
    // 每步守卫的窗口（宿主毫秒；宽度 = `stepTimeoutMs(s, 0)`；**0 = 这条指令没有窗口**）
    bool guard_armed_ = false;
    std::uint64_t guard_started_ms_ = 0;
    std::uint64_t guard_ms_ = 0;
    // 失败面（`TimedOut`）：点名用的那一条 + 读数
    bool failed_ = false;
    std::size_t failed_index_ = 0;
    std::uint64_t failed_elapsed_ms_ = 0;
    std::uint64_t failed_guard_ms_ = 0;
    // `quit` 之后不再推进（⛔ 不许把 quit 后面的指令静默施加掉）
    // ★ 这个 bool 同时是**宿主停止请求**的句柄（`quitFlag()`）：AOT 的 run 循环每个块边界读它
    //   ⇒ 与解释器臂 `Machine::run` 读 `actuator().quitRequested()` **同一条口径**。
    bool quit_requested_ = false;
    // `profile_begin|end` 的区间账（一次性读数；⛔ 不是性能判据）
    bool profile_open_ = false;
    std::string profile_label_;
    std::uint64_t profile_started_ms_ = 0;
    std::uint64_t profile_reads_at_begin_ = 0;
    std::uint64_t profile_windows_ = 0;  // 正常收口的窗口数（begin+end 配对）
    // ★ 窗口 profile 的标记钩子（缺省 nullptr ⇒ 每个读点一次判空；见 `setProfileMarkHook`）
    void* mark_ctx_ = nullptr;
    ProfileMarkHook mark_fn_ = nullptr;
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::array<std::uint64_t, kPlanEventCount> event_calls_{};
};

}  // namespace lift
