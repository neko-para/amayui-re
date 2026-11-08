// emulator-lift · 指令式输入时间线（`--input-script`）
//
// ★ 目的（2026-11-08）：把「在某个时刻点一下 OPTION / 按一下键」从**手工 PowerShell 动作**
//   （SetForegroundWindow → 挪真实光标 → SendInput → 等 250~350 ms）变成**可复现的一次运行**。
//   注入不走 SDL、不读 OS 状态：它只调用 `runtime/input.h` 的那一段应用代码，与真人输入
//   汇进 `WindowManager` 的**同一份**状态（影子键电平 / 鼠标按钮位图 / 虚拟光标）。
//
// ★ 语法（一行一条；`#` 起注释；换行或 `;` 分隔）：
//
//     at frame N              时间线锚点 = 第 N 个 Present（1 起；-present headless 也计数）
//     at guest_ms T           时间线锚点 = guest 虚拟时钟 T 毫秒（确定性；见下）
//     after N frames          锚点 = 上一个帧锚点 + N
//     after N ms              锚点 = 上一个毫秒锚点 + N
//     wait N frames|ms        推进锚点（并记一个 **no-op 动作**：脚本要等它到期才收口 ⇒ 尾部 `wait`
//                             可以"保持按住 / 保持等待"；没有尾部 `wait` 的动作会在应用完立刻收口）
//
//     mouse_move X Y          guest 客户区坐标（= 引擎 1280×720 虚拟屏），顺带把光标给脚本
//     mouse_down [left|right|middle]     用脚本上一次的坐标
//     mouse_up   [left|right|middle]
//     click X Y [left|right|middle] [hold N]   move + down 在触发帧；up 在 +N 帧（N 缺省 1）
//     key_down VK / key_up VK Win32 虚拟键码（十进制或 0x 十六进制，1..255）
//     quit                    本动作触发时结束这次运行（`RunResult.reason = Stopped`）
//
// ★ 三条口径（都由"注入必须与真人输入同一层"推出）：
//   1. **一条动作只应用一次**。同一 tick 里：`click` 的延迟抬起先处理（它属于上一帧），到期的明动作
//      按**文件顺序**应用。⇒ 同一帧里"上一 click 的 up + 本帧显式 mouse_down"读作 down。
//   2. `at frame` 的"帧"= `HeadlessPresenter::frameCount()`（每次 D3D9 Present 自增）。触发发生在
//      Present 之后的块边界 ⇒ guest 在**下一帧**的取指里看到新状态。**一帧内 down+up 不算点击**
//      （guest 可能一次也采不到"按下"）⇒ `click` 缺省隔一帧抬起（`hold` 是**帧**数）。
//   3. `at guest_ms` 读虚拟时钟（缺省 = 每条指令 100 ns）。`--present window` 是 real-time 时钟
//      ⇒ 那一档的毫秒不可复现，**用帧**；headless（缺省）两档都确定性。
//   ★ 收口（动作跑完 / `quit`）：脚本仍按着的键 / 按钮会**按同一组 WM_* 抬起**再交还所有权；未到期
//     的 `click` 抬起也在这一刻补发（否则 guest 的 WndProc / 事件锁存会停在"按下"）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/host_window.h"

namespace lift {

class InputScript {
public:
    enum class Kind : std::uint8_t {
        MouseMove,
        MouseDown,
        MouseUp,
        KeyDown,
        KeyUp,
        Click,
        Wait,  // no-op 动作：让脚本**等到**这个时刻才收口（尾部 `wait` 可以保持按住）
        Quit,
    };

    struct Action {
        Kind kind = Kind::Quit;
        bool by_ms = false;        // true ⇒ 触发看 at_ms；false ⇒ 看 at_frame
        std::uint64_t at_frame = 0;
        std::uint64_t at_ms = 0;
        int x = 0;
        int y = 0;
        MouseButton button = MouseButton::Left;
        std::uint32_t vk = 0;
        std::uint32_t hold_frames = 1;  // Click：down 之后隔多少帧抬起（>= 1）
        std::string text;               // 原始行（日志 / 报错定位）
        bool applied = false;
    };

    // click 的"抬起"是**延迟动作**（跨帧），单独排队；否则一次 tick 里 down+up 会被 guest 漏掉。
    struct Release {
        std::uint64_t due_frame = 0;
        int x = 0;
        int y = 0;
        MouseButton button = MouseButton::Left;
    };

    // 读脚本文件（UTF-8；Windows 走 `openHostFileUtf8`）并解析。失败 ⇒ false + err（带 行号）。
    bool loadFile(const std::string& path, std::string* err);
    // 直接解析文本（测试 / 内嵌用）；origin 只进报错与日志。
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    void clear();

    bool active() const { return !actions_.empty(); }
    bool finished() const { return active() && applied_ == actions_.size() && releases_.empty(); }
    bool quitRequested() const { return quit_requested_; }
    // `quit` 的 WM_QUIT 只投一次（guest 可能正阻塞在 GetMessageA）。由 `Machine::tickInputScript` 消费。
    bool quitPostPending() const { return quit_requested_ && !quit_posted_; }
    void markQuitPosted() { quit_posted_ = true; }
    // 换脚本 / 丢弃脚本：把仍按着的键 / 按钮按 WM_* 抬起、交还所有权，再清时间线
    //（⛔ 不静默留下"卡住的键"）。`clear()` 只清时间线，不发消息 —— 那是收口用的。
    void abandon(WindowManager& wm, std::uint32_t time_ms);
    std::size_t actionCount() const { return actions_.size(); }
    std::size_t appliedCount() const { return applied_; }
    const std::string& source() const { return source_; }
    const std::vector<Action>& actions() const { return actions_; }

    // 每个块边界调用（`Machine::run`，以及 guest 阻塞在 GetMessageA 时的宿主事件泵）。
    // `frame` = Present 计数；`now_100ns` = 虚拟时钟。返回 true = 本次 tick 应用了动作。
    bool tick(WindowManager& wm, std::uint64_t frame, std::uint64_t now_100ns);

    static std::uint64_t msFrom100ns(std::uint64_t now_100ns) { return now_100ns / 10000ull; }

private:
    struct Trigger {
        bool by_ms = false;
        std::uint64_t value = 0;
    };

    void apply(WindowManager& wm, Action& a, std::uint64_t frame, std::uint32_t time_ms);
    // 收口：把脚本仍按着的键 / 按钮按同一组 WM_* 抬起（⛔ 只清状态不发消息会让 guest 的事件锁存卡住）。
    void releaseOwnedInputs(WindowManager& wm, std::uint32_t time_ms);

    std::string source_;
    std::vector<Action> actions_;
    std::vector<Release> releases_;
    std::size_t applied_ = 0;
    bool quit_requested_ = false;
    bool quit_posted_ = false;
    bool finish_logged_ = false;
    // 解析期的相对基准（`after` / `wait` 用；`at` 会重置）
    bool have_frame_anchor_ = false;
    bool have_ms_anchor_ = false;
    std::uint64_t anchor_frame_ = 0;
    std::uint64_t anchor_ms_ = 0;
    // 脚本自己的光标（**client** 坐标）：`mouse_down/up` 不带坐标时用它。
    bool have_pos_ = false;
    int last_x_ = 0;
    int last_y_ = 0;
};

}  // namespace lift
