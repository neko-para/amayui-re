// emulator-lift · 输入**投递点**（11 个投递点的闭集合）
//
// ★ 为什么单独一个头（2026-11-19 · C1a 行为不变的机械抽取）：`PointerCore`
//   （`runtime/pointer_core.h`）要用这份枚举判"闸门看的是不是 KeyLevel 读点"，而
//   `input_plan.h` 又要 include `pointer_core.h` ⇒ 枚举若仍住在 `input_plan.h` 就是**循环
//   include**（`#pragma once` 会让枚举在用到时还没声明）。⇒ 类型搬到这里，`input_plan.h`
//   只留一行 `#include`。
// ★ 文本逐字搬自 `input_plan.h`（W1）；**唯一定义仍在 `input_plan.cpp`**：`inputPointName()`
//   的定义一个字节没动（⛔ 不要在这里加 out-of-line 定义，那会变成两个 TU 的同一份词表）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace lift {

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

}  // namespace lift
