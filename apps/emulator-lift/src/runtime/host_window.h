// emulator-lift · headless 窗口 / 消息队列 / 计时器（P0）
//
// ★ 口径（用户 2026-11-04）：先做 **headless 窗口**，但**消息泵语义必须完整**。
//   这里不建 SDL 窗口；P3 只换后端（SDL 事件 → 同一个队列）。
// ★ 句柄都是确定性整数（0x00010000 起）；guest 只当不透明令牌。
// ★ 消息结构 = Win32 MSG 的 x86 布局（28 字节）：hwnd/message/wParam/lParam/time/pt.x/pt.y。
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

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
    bool get(X86& cpu, WinMessage* out);  // headless：先 pump 计时器；无消息 ⇒ false（调用方报错）
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
};

}  // namespace lift
