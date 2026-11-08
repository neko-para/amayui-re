#include "runtime/host_window.h"

#include <algorithm>
#include <cstdio>

#include "runtime/clock.h"
#include "runtime/guest_token.h"
#include "runtime/input.h"  // 按钮 ↔ VK / MK_* 位（**唯一一份**映射）
#include "runtime/x86.h"

namespace lift {
namespace {
constexpr std::uint32_t kWsCaption = 0x00C00000u;
constexpr std::uint32_t kWmQuit = 0x0012u;
constexpr std::uint32_t kWmTimer = 0x0113u;
constexpr std::uint32_t kTimerIdAutoBase = 0x00010000u;
}  // namespace

const char* inputSourceName(InputSource src) { return src == InputSource::Script ? "script" : "os"; }

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

bool WindowManager::peekAt(std::size_t index, WinMessage* out) const {
    if (index < queue_.size()) {
        if (out) *out = queue_[index];
        return true;
    }
    // 队列之外：`WM_QUIT` 是"队列里的一条虚拟消息"（真机也是 post 队列里的一项），排在最后。
    if (quit_ && index == queue_.size()) {
        WinMessage q;
        q.message = kWmQuit;
        q.wparam = quit_code_;
        if (out) *out = q;
        return true;
    }
    return false;
}

bool WindowManager::get(X86& cpu, WinMessage* out) {
    // ★ headless（没有事件源）：保持 P0 语义 —— 只 pump 计时器，空队列返回 false。
    // ★ 装了 SDL 事件源：GetMessageA 必须**阻塞**到有消息或窗口关闭；事件源自己限时等待
    //   （SDL_WaitEventTimeout），这里只负责反复 pump，直到队列里出现消息（含 WM_QUIT 虚拟项）。
    for (;;) {
        pumpTimers(cpu);
        if (peek(out, true)) return true;
        if (!host_event_pump_) return false;
        // ★ 单令牌（guest_token.h）：事件泵在窗口模式下会 `SDL_WaitEventTimeout(5ms)`；主线程
        //   若持令牌睡觉，feeder 就拿不到令牌去解码 ⇒ 音频欠载。让出令牌再等，醒来取回。
        //   纯单线程夹具（没持令牌 / 没事件源）里 Yield 是无操作。
        {
            GuestToken::YieldGuard yield(guestToken());
            host_event_pump_(cpu);
        }
    }
}

void WindowManager::setHostEventPump(HostEventPump pump) {
    host_event_pump_ = std::move(pump);
}

void WindowManager::clearHostEventPump() {
    host_event_pump_ = nullptr;
}

std::uint32_t WindowManager::primaryWindow() const {
    for (const auto& entry : windows_) {
        if (!entry.second.destroyed) return entry.first;
    }
    return 0;
}

void WindowManager::setKeyDown(std::uint32_t vk, bool down) { setKeyDown(InputSource::Os, vk, down); }

bool WindowManager::setKeyDown(InputSource src, std::uint32_t vk, bool down) {
    if (vk >= 256u) return false;
    // ★ 所有权规则（文件上方 `InputSource`）：脚本拥有的键，OS 的按下 / 抬起都不生效。
    if (src == InputSource::Os && key_owner_[vk] == static_cast<std::uint8_t>(InputSource::Script)) {
        return false;
    }
    // 脚本抬起 ⇒ 该键交还 OS（`down == false` 时所有者清空）。
    key_owner_[vk] = down ? static_cast<std::uint8_t>(src) : static_cast<std::uint8_t>(InputSource::Os);
    key_down_[vk] = down ? 1u : 0u;
    return true;
}

bool WindowManager::keyDown(std::uint32_t vk) const { return vk < 256u && key_down_[vk] != 0; }

bool WindowManager::keyOwnedByScript(std::uint32_t vk) const {
    return vk < 256u && key_owner_[vk] == static_cast<std::uint8_t>(InputSource::Script);
}

bool WindowManager::mouseButtonOwnedByScript(MouseButton b) const {
    const auto idx = static_cast<std::size_t>(b);
    return idx < kMouseButtonCount && button_owner_[idx] == static_cast<std::uint8_t>(InputSource::Script);
}

bool WindowManager::setMouseButton(MouseButton b, bool down, InputSource src) {
    const auto idx = static_cast<std::size_t>(b);
    if (idx >= kMouseButtonCount) return false;
    const std::uint32_t vk = input::vkForMouseButton(b);
    if (!setKeyDown(src, vk, down)) return false;
    button_owner_[idx] = down ? static_cast<std::uint8_t>(src) : static_cast<std::uint8_t>(InputSource::Os);
    if (down) {
        mouse_buttons_ |= input::flagForMouseButton(b);
    } else {
        mouse_buttons_ &= ~input::flagForMouseButton(b);
    }
    return true;
}

void WindowManager::clearKeys() {
    for (std::size_t i = 0; i < 256u; ++i) {
        key_down_[i] = 0;
        key_owner_[i] = 0;
    }
    for (std::uint8_t& o : button_owner_) o = 0;
    mouse_buttons_ = 0;
}

void WindowManager::clearOsInput() {
    for (std::size_t i = 0; i < 256u; ++i) {
        if (key_owner_[i] == static_cast<std::uint8_t>(InputSource::Script)) continue;
        key_down_[i] = 0;
    }
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        if (button_owner_[i] == static_cast<std::uint8_t>(InputSource::Script)) continue;
        mouse_buttons_ &= ~input::flagForMouseButton(static_cast<MouseButton>(i));
        button_owner_[i] = 0;
    }
}

void WindowManager::releaseScriptOwnership() {
    for (std::size_t i = 0; i < 256u; ++i) {
        if (key_owner_[i] != static_cast<std::uint8_t>(InputSource::Script)) continue;
        key_down_[i] = 0;
        key_owner_[i] = 0;
    }
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        if (button_owner_[i] != static_cast<std::uint8_t>(InputSource::Script)) continue;
        mouse_buttons_ &= ~input::flagForMouseButton(static_cast<MouseButton>(i));
        button_owner_[i] = 0;
    }
    cursor_script_owned_ = false;
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

// ---- 光标 / 捕获（headless 虚拟状态；语义见 host_window.h 的注释）----

std::uint32_t WindowManager::setCursor(std::uint32_t hcursor) {
    const std::uint32_t old = cursor_;
    cursor_ = hcursor;
    return old;
}

void WindowManager::setCursorPos(int x, int y) {
    cursor_x_ = x;
    cursor_y_ = y;
}

bool WindowManager::setCursorClientPos(std::uint32_t hwnd, int x, int y, InputSource src) {
    // ★ 脚本拥有光标期间，OS 的鼠标移动不生效（见 InputSource）：这样 `GetCursorPos` 与
    //   `WM_MOUSEMOVE` 的 lParam 永远说同一个位置。
    if (src == InputSource::Os && cursor_script_owned_) return false;
    if (src == InputSource::Script) cursor_script_owned_ = true;
    const HostWindow* w = find(hwnd);
    cursor_x_ = x + (w ? w->x : 0);
    cursor_y_ = y + (w ? w->y : 0);
    return true;
}

void WindowManager::cursorClientPos(std::uint32_t hwnd, int* x, int* y) const {
    const HostWindow* w = find(hwnd);
    if (x) *x = cursor_x_ - (w ? w->x : 0);
    if (y) *y = cursor_y_ - (w ? w->y : 0);
}

int WindowManager::showCursor(bool show) {
    // 真机实现：显示计数 +1 / -1；计数 < 0 ⇒ 光标不可见。返回值是**新计数**。
    cursor_show_count_ += show ? 1 : -1;
    return cursor_show_count_;
}

std::uint32_t WindowManager::setCapture(std::uint32_t hwnd) {
    const std::uint32_t old = capture_;
    capture_ = hwnd;
    return old;
}

// ---- 输入采样计数（guest 侧电平读点的单调计数；见 host_window.h 那一段的"为什么"）----
std::uint64_t WindowManager::noteInputSample(std::uint64_t guest_ms) {
    if (have_input_sample_) {
        const std::uint64_t dt = guest_ms > last_input_sample_ms_ ? guest_ms - last_input_sample_ms_ : 0;
        // ★ 只采"正常采样间隔"：0（同一毫秒内的多次读）会给出 0 的下限，>1 s 的样本是
        //   加载 / 卡顿（拿它当周期会把下限推到秒级 ⇒ 每次点击都像挂住）。
        if (dt > 0 && dt <= 1000) {
            input_intervals_[input_interval_next_] = dt;
            input_interval_next_ = (input_interval_next_ + 1) % kInputIntervalRing;
            if (input_interval_count_ < kInputIntervalRing) ++input_interval_count_;
            // ★ 周期 = 环里的**最大值**而不是最近一次：同一个输入迭代里通常有多次读点
            //   （GetAsyncKeyState 读按钮 + GetAsyncKeyState 读键 + GetCursorPos），它们之间
            //   只隔 0~1 ms；取最近一次会把"迭代周期"污染成 1 ms ⇒ 安全下限形同虚设。
            //   取最大值 ≈ 迭代周期（真实界面的采样周期是**稳定**的，单个伪样本只会让它偏保守）。
            std::uint64_t period = 0;
            for (std::uint32_t i = 0; i < input_interval_count_; ++i) {
                if (input_intervals_[i] > period) period = input_intervals_[i];
            }
            input_sample_period_ms_ = period;
        }
    }
    have_input_sample_ = true;
    last_input_sample_ms_ = guest_ms;
    return ++input_samples_;
}

// ---- 无效区（update region；M2 · P4 / B7）----
//
// ★ 口径见 host_window.h。这里只保证两件事：
//   ① `dirty` 永远是 `!update_region.empty()` 的镜像（两处一起改，⛔ 不许一处真一处假）；
//   ② 结果区域**总是规范化**的（`GdiRegion::combine` 负责）⇒ `GetUpdateRgn` / `GetRgnBox` /
//      `GetRegionData` 对同一块无效区给出同一份矩形表。
void WindowManager::invalidateRect(HostWindow& w, const RegionRect* rect) {
    GdiRegion add;
    if (rect == nullptr) {
        add.setRect(0, 0, w.client_w, w.client_h);  // NULL ⇒ 整个客户区（真机语义）
    } else if (!rect->empty()) {
        add.setRect(rect->left, rect->top, rect->right, rect->bottom);
    }
    // 空矩形 / 空客户区 ⇒ 并进空区域 = 什么都不加（真机也是空操作）。
    if (!add.empty()) {
        GdiRegion next;
        next.combine(w.update_region, add, RegionCombine::Or);
        w.update_region = next;
    }
    w.dirty = !w.update_region.empty();
}

void WindowManager::validateRect(HostWindow& w, const RegionRect* rect) {
    if (rect == nullptr) {
        validateAll(w);  // NULL ⇒ 整窗有效（真机语义）
        return;
    }
    GdiRegion remove;
    if (!rect->empty()) remove.setRect(rect->left, rect->top, rect->right, rect->bottom);
    if (!remove.empty() && !w.update_region.empty()) {
        GdiRegion next;
        next.combine(w.update_region, remove, RegionCombine::Diff);
        w.update_region = next;
    }
    w.dirty = !w.update_region.empty();
}

void WindowManager::validateRegion(HostWindow& w, const GdiRegion& region) {
    if (!region.empty() && !w.update_region.empty()) {
        GdiRegion next;
        next.combine(w.update_region, region, RegionCombine::Diff);
        w.update_region = next;
    }
    w.dirty = !w.update_region.empty();
}

void WindowManager::validateAll(HostWindow& w) {
    w.update_region.setEmpty();
    w.dirty = false;
}

}  // namespace lift
