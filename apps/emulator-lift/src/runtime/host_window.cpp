#include "runtime/host_window.h"

#include <algorithm>
#include <cstdio>

#include "runtime/clock.h"
#include "runtime/x86.h"

namespace lift {
namespace {
constexpr std::uint32_t kWsCaption = 0x00C00000u;
constexpr std::uint32_t kWmQuit = 0x0012u;
constexpr std::uint32_t kWmTimer = 0x0113u;
constexpr std::uint32_t kTimerIdAutoBase = 0x00010000u;
}  // namespace

std::uint32_t WindowManager::registerClass(const std::string& name, std::uint32_t proc, std::uint32_t style) {
    for (const WindowClassInfo& c : classes_)
        if (c.name == name) return c.atom;
    WindowClassInfo c;
    c.name = name;
    c.proc = proc;
    c.style = style;
    c.atom = next_class_;
    next_class_ += 4u;
    classes_.push_back(c);
    return c.atom;
}

void WindowManager::clientSizeFor(std::uint32_t style, int outer_w, int outer_h, int* client_w, int* client_h) {
    int cw = outer_w;
    int ch = outer_h;
    if (style & kWsCaption) {
        cw -= 2 * kFrameX;
        ch -= 2 * kFrameY + kCaption;
        if (style & 0x00080000u) ch -= kMenu;  // WS_SYSMENU 只是近似：本作 CreateWindow 传了菜单
    } else {
        // WS_POPUP / borderless：外层就是客户区
    }
    if (cw < 1) cw = outer_w;
    if (ch < 1) ch = outer_h;
    *client_w = cw;
    *client_h = ch;
}

std::uint32_t WindowManager::createWindow(const std::string& cls, std::uint32_t style, std::uint32_t exstyle, int x,
                                          int y, int w, int h, std::uint32_t menu) {
    HostWindow win;
    win.hwnd = next_window_;
    next_window_ += 4u;
    win.class_name = cls;
    win.style = style;
    win.exstyle = exstyle;
    win.menu = menu;
    win.x = x;
    win.y = y;
    win.outer_w = w;
    win.outer_h = h;
    clientSizeFor(style, w, h, &win.client_w, &win.client_h);
    win.visible = (style & 0x10000000u) != 0;  // WS_VISIBLE
    for (const WindowClassInfo& c : classes_)
        if (c.name == cls) win.proc = c.proc;
    windows_[win.hwnd] = win;
    return win.hwnd;
}

HostWindow* WindowManager::find(std::uint32_t hwnd) {
    auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

const HostWindow* WindowManager::find(std::uint32_t hwnd) const {
    auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

bool WindowManager::destroyWindow(std::uint32_t hwnd) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return false;
    it->second.destroyed = true;
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [hwnd](const HostTimer& t) { return t.hwnd == hwnd; }),
                  timers_.end());
    windows_.erase(it);
    return true;
}

void WindowManager::post(const WinMessage& msg) { queue_.push_back(msg); }

bool WindowManager::peek(WinMessage* out, bool remove) {
    if (queue_.empty()) {
        if (quit_) {
            WinMessage q;
            q.message = kWmQuit;
            q.wparam = quit_code_;
            if (out) *out = q;
            if (remove) quit_ = false;
            return true;
        }
        return false;
    }
    if (out) *out = queue_.front();
    if (remove) queue_.pop_front();
    return true;
}

bool WindowManager::get(X86& cpu, WinMessage* out) {
    pumpTimers(cpu);
    return peek(out, true);
}

void WindowManager::postQuit(std::uint32_t code) {
    quit_ = true;
    quit_code_ = code;
}

void WindowManager::pumpTimers(X86& cpu) {
    if (!cpu.clock) return;
    const std::uint64_t now = cpu.clock->now100ns();
    for (;;) {
        auto it = std::find_if(timers_.begin(), timers_.end(), [now](const HostTimer& t) { return t.due_100ns <= now; });
        if (it == timers_.end()) break;
        WinMessage m;
        m.hwnd = it->hwnd;
        m.message = kWmTimer;
        m.wparam = it->id;
        m.time = static_cast<std::uint32_t>(now / 10000ull);
        queue_.push_back(m);
        if (it->elapse_ms == 0) {
            timers_.erase(it);
        } else {
            it->due_100ns = now + static_cast<std::uint64_t>(it->elapse_ms) * 10000ull;
        }
    }
}

std::uint32_t WindowManager::setTimer(X86& cpu, std::uint32_t hwnd, std::uint32_t id, std::uint32_t elapse_ms) {
    if (id == 0) id = kTimerIdAutoBase + next_timer_++;
    for (HostTimer& t : timers_) {
        if (t.hwnd == hwnd && t.id == id) {
            t.elapse_ms = elapse_ms;
            t.due_100ns = (cpu.clock ? cpu.clock->now100ns() : 0) + static_cast<std::uint64_t>(elapse_ms) * 10000ull;
            return id;
        }
    }
    HostTimer t;
    t.hwnd = hwnd;
    t.id = id;
    t.elapse_ms = elapse_ms;
    t.due_100ns = (cpu.clock ? cpu.clock->now100ns() : 0) + static_cast<std::uint64_t>(elapse_ms) * 10000ull;
    timers_.push_back(t);
    return id;
}

bool WindowManager::killTimer(std::uint32_t hwnd, std::uint32_t id) {
    const auto before = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [hwnd, id](const HostTimer& t) { return t.hwnd == hwnd && t.id == id; }),
                  timers_.end());
    return timers_.size() != before;
}

}  // namespace lift
