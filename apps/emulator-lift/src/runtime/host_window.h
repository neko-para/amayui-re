// emulator-lift · headless 窗口 / 消息队列 / 计时器（P0）
//
// ★ 口径（用户 2026-11-04）：先做 **headless 窗口**，但**消息泵语义必须完整**。
//   这里不建 SDL 窗口；P3 只换后端（SDL 事件 → 同一个队列）。
// ★ 句柄都是确定性整数（0x00010000 起）；guest 只当不透明令牌。
// ★ 消息结构 = Win32 MSG 的 x86 布局（28 字节）：hwnd/message/wParam/lParam/time/pt.x/pt.y。
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "runtime/region.h"

namespace lift {

struct X86;

struct WinMessage {
    std::uint32_t hwnd = 0;
    std::uint32_t message = 0;
    std::uint32_t wparam = 0;
    std::uint32_t lparam = 0;
    std::uint32_t time = 0;
    std::int32_t pt_x = 0;
    std::int32_t pt_y = 0;
};

struct WindowClassInfo {
    std::string name;
    std::uint32_t proc = 0;
    std::uint32_t style = 0;
    std::uint32_t atom = 0;
};

struct HostWindow {
    std::uint32_t hwnd = 0;
    std::string class_name;
    std::uint32_t proc = 0;
    std::uint32_t style = 0;
    std::uint32_t exstyle = 0;
    std::uint32_t menu = 0;
    std::uint32_t userdata = 0;
    int x = 0, y = 0, outer_w = 0, outer_h = 0;
    int client_w = 0, client_h = 0;
    bool visible = false;
    bool dirty = false;
    bool destroyed = false;
    // ★ M2 · P4：窗口的**无效区**（update region，**客户区坐标**）。
    //   `dirty` 保留为它的**布尔镜像**（= `!update_region.empty()`）：`UpdateWindow` 仍按 `dirty`
    //   决定发不发 WM_PAINT。两者只经 `WindowManager::{invalidate*,validate*}` 一起改
    //   （⛔ 不许一处真一处假 —— 那会让 `GetUpdateRgn` 与 WM_PAINT 说两套话）。
    GdiRegion update_region;
};

struct HostTimer {
    std::uint32_t hwnd = 0;
    std::uint32_t id = 0;
    std::uint64_t due_100ns = 0;
    std::uint32_t elapse_ms = 0;
};

class WindowManager {
public:
    static constexpr std::uint32_t kFirstWindow = 0x00010000u;
    static constexpr std::uint32_t kFirstClass = 0x00011000u;

    // 屏幕 / 非客户区尺寸：固定值（可复现；不是真显示器）
    static constexpr int kScreenW = 1280;
    static constexpr int kScreenH = 720;
    static constexpr int kFrameX = 3;
    static constexpr int kFrameY = 3;
    static constexpr int kCaption = 19;
    static constexpr int kMenu = 19;

    std::uint32_t registerClass(const std::string& name, std::uint32_t proc, std::uint32_t style);
    std::uint32_t createWindow(const std::string& cls, std::uint32_t style, std::uint32_t exstyle, int x,
                               int y, int w, int h, std::uint32_t menu);
    HostWindow* find(std::uint32_t hwnd);
    const HostWindow* find(std::uint32_t hwnd) const;
    bool destroyWindow(std::uint32_t hwnd);
    std::size_t windowCount() const { return windows_.size(); }

    // 消息队列（不弹 SDL；P3 由后端往同一个队列里 post）
    void post(const WinMessage& msg);
    bool peek(WinMessage* out, bool remove);
    // 只看第 index 条（0 = 队首），**不取走**。`MsgWaitForMultipleObjects` 要"扫描整个队列找匹配
    // 唤醒掩码的消息"，只看队首会让"不匹配的消息挡在前面"时误报 WAIT_TIMEOUT。
    bool peekAt(std::size_t index, WinMessage* out) const;
    bool get(X86& cpu, WinMessage* out);  // headless：先 pump 计时器；无消息 ⇒ false（调用方报错）
    // ★ P3 宿主事件源（SDL 窗口）：队列空时调用它；它负责把 SDL 事件翻译成 WinMessage 并 post。
    //   未安装 ⇒ `get` 保持旧语义（返回 false，调用方 abort）；安装后 ⇒ 阻塞到有消息或窗口关闭。
    //   回调必须自己限时（SDL_WaitEventTimeout(1..10ms)），⛔ 不许永久阻塞宿主。
    using HostEventPump = std::function<void(X86&)>;
    void setHostEventPump(HostEventPump pump);
    void clearHostEventPump();
    bool hasHostEventPump() const { return static_cast<bool>(host_event_pump_); }
    // 主窗口（第一个未销毁；本作单窗口；0 = 还没有窗口）。关窗消息的 hwnd 用它。
    std::uint32_t primaryWindow() const;
    // 影子键盘状态：WM_KEYDOWN/UP 与 GetAsyncKeyState 同源（⛔ 两套状态会打架）。
    void setKeyDown(std::uint32_t vk, bool down);
    bool keyDown(std::uint32_t vk) const;
    void clearKeys();
    void postQuit(std::uint32_t code);
    bool quitPosted() const { return quit_; }
    std::uint32_t quitCode() const { return quit_code_; }
    std::size_t queueSize() const { return queue_.size(); }

    void pumpTimers(X86& cpu);
    std::uint32_t setTimer(X86& cpu, std::uint32_t hwnd, std::uint32_t id, std::uint32_t elapse_ms);
    bool killTimer(std::uint32_t hwnd, std::uint32_t id);
    std::size_t timerCount() const { return timers_.size(); }

    // 非客户区扣除（客户区 = 外层 - 边框 - 标题栏 - 菜单）；样式里没有 WS_CAPTION 就不扣标题栏。
    static void clientSizeFor(std::uint32_t style, int outer_w, int outer_h, int* client_w, int* client_h);

    // =====================================================================================
    // 光标 / 捕获（M2 · 输入族；**headless 虚拟状态**）
    // ★ 口径（2026-11-04）：不建 SDL 光标、不读 .cur；句柄 / 位置 / 显示计数 / 捕获都是
    //   **确定性宿主状态**，语义按 Win32 文档照做（返回旧句柄 / 新显示计数 / TRUE），
    //   ⛔ 不返回随机值、⛔ 不假装成功却什么都不记。
    // ★ 虚拟光标位置初值 = **(0,0)**：headless 没有鼠标输入，屏幕原点是"还没有输入"的
    //   中性状态（选它而不是客户区中心：后者会让 `ScreenToClient` 在上窗口之前给出负坐标）。
    // ★ `ShowCursor` 的语义是**显示计数**（真机初值 0，`>= 0` 才显示）：显示 +1 / 隐藏 -1，
    //   返回值 = **新计数**（不是布尔、不是旧值）。
    // =====================================================================================
    std::uint32_t setCursor(std::uint32_t hcursor);  // 返回**旧** HCURSOR（Win32 语义）
    std::uint32_t cursor() const { return cursor_; }
    void setCursorPos(int x, int y);
    void getCursorPos(int* x, int* y) const {
        if (x) *x = cursor_x_;
        if (y) *y = cursor_y_;
    }
    int showCursor(bool show);                     // 显示 +1 / 隐藏 -1，返回**新**显示计数
    std::uint32_t setCapture(std::uint32_t hwnd);  // 返回**旧**捕获窗口（0 = 没有）
    std::uint32_t capture() const { return capture_; }

    // =====================================================================================
    // 无效区（update region；M2 · P4 / B7）
    // ★ 语义（Win32 文档）：`InvalidateRect` 往无效区里**并**矩形、`ValidateRect` / `ValidateRgn`
    //   从中**减**矩形；`GetUpdateRgn` 只读（**不**改变无效区）；`UpdateWindow` 发完 WM_PAINT 后清空。
    // ★ `rect == nullptr` ⇒ 整个客户区（InvalidateRect / ValidateRect 的真机语义）。
    // ★ 坐标一律**客户区**；`dirty` 是 `!update_region.empty()` 的镜像，由这三个函数统一维护。
    // =====================================================================================
    void invalidateRect(HostWindow& w, const RegionRect* rect);
    void validateRect(HostWindow& w, const RegionRect* rect);
    void validateRegion(HostWindow& w, const GdiRegion& region);  // 无效区 −= region
    void validateAll(HostWindow& w);                          // 无效区清空

private:
    std::vector<WindowClassInfo> classes_;
    std::map<std::uint32_t, HostWindow> windows_;
    std::deque<WinMessage> queue_;
    std::vector<HostTimer> timers_;
    std::uint32_t next_window_ = kFirstWindow;
    std::uint32_t next_class_ = kFirstClass;
    std::uint32_t next_timer_ = 1;
    bool quit_ = false;
    std::uint32_t quit_code_ = 0;
    HostEventPump host_event_pump_;
    std::uint8_t key_down_[256] = {};
    // 光标 / 捕获（见上面的方法注释）
    std::uint32_t cursor_ = 0;
    std::uint32_t capture_ = 0;
    std::int32_t cursor_x_ = 0;
    std::int32_t cursor_y_ = 0;
    int cursor_show_count_ = 0;
};

}  // namespace lift
