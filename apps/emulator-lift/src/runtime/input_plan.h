// emulator-lift · 输入**投递层**（S1 骨架 + 四条指令的 AOT 输入配方）
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
// 配方格式（**四条指令**，平铺；`#` 注释、空行忽略）
//
//   await script TITLE      ← 电平门：当前引擎脚本身份 == TITLE 即通过（不是边沿）。
//                             名字 ASCII 大小写不敏感、**两侧**都忽略 `.bin`（`scriptNameMatches`）。
//   await position TITLE 0x039C 0x0460
//                           ← **脚本内位置**的电平门（半开区间 `[FROM, TO)`）：
//                             script 匹配 **且** `FROM <= pos < TO` 才通过，否则继续等
//                             （⛔ 不报错、不超时）。`FROM` / `TO` 必须是带 `0x` 前缀的 hex；
//                             解析期校验 `FROM < TO`；脚本名**不在解析期校验**（名字匹配是运行期的事）。
//                             `pos = header_len + (frame[cur].+0x04 − frame[cur].+0x00)`
//                             = 脚本体内**绝对字节偏移**（4 字节对齐），与 `dist/views/data/*.txt`
//                             的 `label_%08x` **同一坐标系**；`header_len` 见下面的常量。
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
//   · 位置门同样只读 guest 内存（`frame[cur].+0x00/+0x04`，EA `0x40EE57` / `0x412825`）；
//     `base==0` / `pc` 不在脚本体缓冲里 / 读不到 ⇒ 门**不成立**（继续等），并记一个内部计数
//     `positionMisses()`（缺省不打印）。
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

// =====================================================================================
// `await position` 的两格读数 + 头长（**全部是 guest 内存读**，不依赖 Machine / X86）
//
// 取证（`.tmp/script-pos/research.md` §1；锚都是 `.lst` 的 EA）：
//   · `frame[cur].+0x00`（= `Engine+0x5D894+0x78*cur`）= **脚本体缓冲基址**：
//     写 `0x40EE57`（`GlobalAlloc` 的返回值）；`0x40F69E` 是 `ReadFile` 的 `lpBuffer`。
//   · `frame[cur].+0x04`（= `Engine+0x5D898+0x78*cur`）= **PC**：`0x412825` 取 opcode、
//     `0x41347D..0x413487` 每条指令步进。它是**指进脚本体缓冲的 guest 指针**。
//   · ⇒ `pos = header_len + (pc − base)`，与 `dist/views/data/*.txt` 的 `label_%08x` 同一坐标系。
//
// ★ `header_len` **不是**从 guest 内存读的，这一点与本文件原先的猜测不同（逐字核过装载器）：
//   `0x40EE67` 把 0x20 字节头读进**栈**缓冲 `[ebp+Buffer]`，`0x40EFF2` 把
//   `sub_header_length` 读进**栈**变量 `[ebp+var_160]`，`0x40F01C` 把 sub-header 读进
//   `[ebp+var_15C]`；随后 `0x40F69E` 才 `ReadFile` 进 `frame[cur]+0x00` —— 此时文件游标已在
//   `0x20 + sub_header_length`。⇒ **缓冲区从 `header_len` 处开始，头本身在 guest 里没有落点**
//   （全函数 4 处 `lpNumberOfBytesRead` 都是栈地址，读数也没有写回 guest）。
//   ⇒ 只能用语料事实：941/941 视图签名 `SYS4450`、`sub_header_length = 0x1C`
//   ⇒ `header_len = 0x20 + 0x1C = 0x3C`（v5 的 `0x44` 在本语料零出现）。
//   ⛔ 别把它改成"读 `base` 处的签名"：`base` 处是**第一条指令的 opcode**。
// =====================================================================================
inline constexpr std::uint32_t kFrameBodyBaseOffset = 0x00000000u;  // 记录 +0x00 = 脚本体缓冲基址
inline constexpr std::uint32_t kFramePcOffset = 0x00000004u;         // 记录 +0x04 = PC
inline constexpr std::uint32_t kScriptHeaderLen = 0x0000003Cu;       // 0x20 + sub_header_length（SYS4450）
// 装载长度的**上界**（引擎不把 `ReadFile` 的字节数写回 guest ⇒ 精确长度读不到）。
// 本语料最大的脚本 1.5 MB；超过这个数的 `pc − base` 一定是"读到了别的对象"（防跨区伪 pos）。
inline constexpr std::uint32_t kScriptBodyMaxBytes = 0x01000000u;

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
    // 已消费的**指令**数（含 `await script` / `await position` / `wait`；= `next_`）。
    std::uint64_t stepsApplied() const { return applied_; }
    // **已施加的 `click`** 数（摘要 `fired=`；`await` / `wait` 不算"动作"）。
    std::uint64_t fired() const { return fired_; }
    std::uint64_t releases() const { return released_; }
    // 位置门"这一刻读不到位置"的次数（base==0 / pc 越界 / 未提交）。⛔ 缺省不打印（诊断用）。
    std::uint64_t positionMisses() const { return position_misses_; }
    std::uint64_t presenting() const { return event_calls_[static_cast<std::size_t>(PlanEventKind::Frame)]; }
    std::size_t pendingReleases() const { return releases_pending_; }
    const std::string& origin() const { return origin_; }
    // 一行摘要（**一次性**输出；⛔ 不许每帧打）。
    std::string summary() const;

private:
    // 平铺指令流的**四条**指令（⛔ 不是图语法：一个 step 不再"自带一个门 + 一个动作"）。
    enum class InstrKind : std::uint8_t { AwaitScript = 0, Wait = 1, Click = 2, AwaitPosition = 3 };

    struct PlanStep {
        InstrKind kind = InstrKind::AwaitScript;
        // AwaitScript / AwaitPosition
        std::string script;                 // 名字原文（⛔ 不参与判定：判定走 `scriptNameMatches`）
        // AwaitPosition（半开区间 `[pos_from, pos_to)`；label 坐标 = 脚本体内绝对字节偏移）
        std::uint32_t pos_from = 0;
        std::uint32_t pos_to = 0;
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
    // 读一次"脚本内位置"（label 坐标）。读不到 ⇒ false（门不成立）；⛔ 不抛、不报错。
    bool readScriptPosition(std::uint32_t* pos);
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
    std::uint64_t position_misses_ = 0;  // 位置门读不到位置的次数（诊断；缺省不打印）
    std::uint64_t read_points_ = 0;     // 读点序号（待抬起的兜底刻度）
    std::size_t releases_pending_ = 0;
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::array<std::uint64_t, kPlanEventCount> event_calls_{};
};

}  // namespace lift
