#include "runtime/host_window.h"

#include <algorithm>
#include <cstdio>

#include "runtime/clock.h"
#include "runtime/guest_token.h"
#include "runtime/x86.h"

namespace lift {
namespace {
constexpr std::uint32_t kWsCaption = 0x00C00000u;
constexpr std::uint32_t kWmQuit = 0x0012u;
constexpr std::uint32_t kWmTimer = 0x0113u;
constexpr std::uint32_t kTimerIdAutoBase = 0x00010000u;
}  // namespace

const char* inputSourceName(InputSource src) { return src == InputSource::Script ? "script" : "os"; }

// ★ 主线程亲和性（**不是**令牌 —— 见 host_window.h 类头那一段与 machine.cpp 的 M0 注释）。
//   本类的状态是**主线程独占的宿主对象**：块边界的 harness tick 与 `GetMessageA` 的宿主事件泵
//   都在**合法让出令牌**的窗口里写它 ⇒ 这里判的是"谁在碰"，⛔ 不是"持不持牌"。
//   · 没有 owner（显式清空）⇒ 什么都不做：纯夹具不制造假红；
//   · 当前线程 = owner ⇒ 什么都不做（正常路径的全部代价 = 一次 thread::id 比较）；
//   · 否则 ⇒ `off_thread_++` + 第一次在 stderr 喊一行（CAS，热路径不刷屏）。
//   ⛔ **不改变任何返回值 / 行为**：违规必须能活着进摘要（`--assert-guest-token` 才判得了红）。
void WindowManager::checkMainThread(const char* site) const {
    const std::thread::id owner = owner_;
    if (owner == std::thread::id{}) return;
    if (std::this_thread::get_id() == owner) return;
    off_thread_.fetch_add(1, std::memory_order_relaxed);
    const char* expected = nullptr;
    if (!first_off_site_.compare_exchange_strong(expected, site, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "[window-thread] 违规：WindowManager 的状态被非主线程访问：site=%s ⇒ "
                 "wm 是主线程独占宿主对象（M0 口径）；真跑时 `--assert-guest-token` 会判红\n",
                 site ? site : "?");
}

void WindowManager::resetOffThread() {
    off_thread_.store(0, std::memory_order_relaxed);
    first_off_site_.store(nullptr, std::memory_order_relaxed);
}

std::uint32_t WindowManager::registerClass(const std::string& name, std::uint32_t proc, std::uint32_t style) {
    checkMainThread("window.class");
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
    checkMainThread("window.create");
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
    checkMainThread("window.find");
    auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

const HostWindow* WindowManager::find(std::uint32_t hwnd) const {
    checkMainThread("window.find");
    auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

bool WindowManager::destroyWindow(std::uint32_t hwnd) {
    checkMainThread("window.destroy");
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return false;
    it->second.destroyed = true;
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [hwnd](const HostTimer& t) { return t.hwnd == hwnd; }),
                  timers_.end());
    windows_.erase(it);
    return true;
}

void WindowManager::post(const WinMessage& msg) {
    checkMainThread("window.post");
    queue_.push_back(msg);
}

bool WindowManager::peek(WinMessage* out, bool remove) {
    checkMainThread("window.peek");
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

bool WindowManager::peekMatching(const std::function<bool(const WinMessage&)>& pred, WinMessage* out,
                                 bool remove) {
    checkMainThread("window.peekMatching");
    // ★ FIFO 语义：从**队首**起线性找第一条匹配的。匹配只决定"看哪一条" ——
    //   不匹配的消息**原地保留**（⛔ 不许学旧实现那样 `peek(true)` 逐条取走：那是静默丢消息）。
    //   `queue_.erase(it)` 只摘掉命中的那一条，其余消息（含它前面的）相对顺序不变。
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (pred && !pred(*it)) continue;
        if (out) *out = *it;
        if (remove) queue_.erase(it);
        return true;
    }
    // 队列之外：`WM_QUIT` 是"队列里的一条虚拟消息"（与 `peekAt` / `peek` 同形，排在队尾）。
    // ★ 它也参与 `pred`：`PeekMessage(hwnd=A)` 不该取走 hwnd=0 的 WM_QUIT（它不匹配）。
    if (quit_) {
        WinMessage q;
        q.message = kWmQuit;
        q.wparam = quit_code_;
        if (pred && !pred(q)) return false;
        if (out) *out = q;
        if (remove) quit_ = false;
        return true;
    }
    return false;
}

bool WindowManager::peekAt(std::size_t index, WinMessage* out) const {
    checkMainThread("window.peekAt");
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
    checkMainThread("window.get");
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
    checkMainThread("window.primary");
    for (const auto& entry : windows_) {
        if (!entry.second.destroyed) return entry.first;
    }
    return 0;
}

WriteResult WindowManager::setKeyDown(std::uint32_t vk, bool down) {
    return setKeyDown(InputSource::Os, vk, down);
}

// ★ 输入状态（S4）：设备是唯一状态源；`InputSource` → `Driver` 的映射是**唯一**一份
//   （`Script` = Program：配方 / 控制层 / `InputScript` / 交互图从不并发，§3.4）。
//   这里只保留**同名转发** + 原来的 `checkMainThread` 站点名（⛔ 站点名一个都不许改）。
WriteResult WindowManager::setKeyDown(InputSource src, std::uint32_t vk, bool down) {
    checkMainThread("window.key");
    return device_.setKey(vk, down, src == InputSource::Script ? Driver::Program : Driver::Os);
}

bool WindowManager::keyDown(std::uint32_t vk) const {
    checkMainThread("window.key");
    return device_.keyDown(vk);
}

std::vector<LevelRef> WindowManager::programHeldLevels() const {
    checkMainThread("window.key");
    return device_.programHeldLevels();
}

WriteResult WindowManager::setMouseButton(MouseButton b, bool down, InputSource src) {
    checkMainThread("window.mouse");
    if (static_cast<std::size_t>(b) >= kMouseButtonCount) return WriteResult{};
    // ★ 搬家前这里经 `setKeyDown` 再记一次 `window.key`（站点名与计数逐字保留；S1 纯搬家）。
    checkMainThread("window.key");
    return device_.setButton(b, down, src == InputSource::Script ? Driver::Program : Driver::Os);
}

void WindowManager::clearKeys() {
    checkMainThread("window.key");
    device_.clearKeys();
}

void WindowManager::postQuit(std::uint32_t code) {
    checkMainThread("window.quit");
    quit_ = true;
    quit_code_ = code;
}

void WindowManager::pumpTimers(X86& cpu) {
    checkMainThread("window.timer");
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
    checkMainThread("window.timer");
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
    checkMainThread("window.timer");
    const auto before = timers_.size();
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [hwnd, id](const HostTimer& t) { return t.hwnd == hwnd && t.id == id; }),
                  timers_.end());
    return timers_.size() != before;
}

// ---- 光标（headless 虚拟状态；设备侧语义见 `runtime/input_device.h` 的 `Driver` / §3.2）----

std::uint32_t WindowManager::setCursor(std::uint32_t hcursor) {
    checkMainThread("window.cursor");
    const std::uint32_t old = cursor_;
    cursor_ = hcursor;
    return old;
}

void WindowManager::setCursorPos(int x, int y) {
    checkMainThread("window.cursor");
    // guest 自己的 SetCursorPos：`Driver::Guest` **永远接受**，不改租约 / 电平，也不发消息（§3.2 #1）。
    device_.moveToScreen(x, y, Driver::Guest);
}

bool WindowManager::setCursorClientPos(std::uint32_t hwnd, int x, int y, InputSource src) {
    checkMainThread("window.cursor");
    // ★ 先判租约（被拒时**不查窗口表**；与搬家前的顺序一致）。
    if (src == InputSource::Os && device_.programDriving()) return false;
    // ★ 窗口原点投影留在 `WindowManager`（窗口表在它这里；见设计 §3.6）。
    const HostWindow* w = find(hwnd);
    return device_.moveToScreen(x + (w ? w->x : 0), y + (w ? w->y : 0),
                                src == InputSource::Script ? Driver::Program : Driver::Os);
}

void WindowManager::cursorClientPos(std::uint32_t hwnd, int* x, int* y) const {
    checkMainThread("window.cursor");
    const HostWindow* w = find(hwnd);
    int screen_x = 0;
    int screen_y = 0;
    device_.cursorScreen(&screen_x, &screen_y);
    if (x) *x = screen_x - (w ? w->x : 0);
    if (y) *y = screen_y - (w ? w->y : 0);
}

int WindowManager::showCursor(bool show) {
    checkMainThread("window.cursor");
    // 真机实现：显示计数 +1 / -1；计数 < 0 ⇒ 光标不可见。返回值是**新计数**。
    cursor_show_count_ += show ? 1 : -1;
    return cursor_show_count_;
}

// ---- 输入采样计数（guest 侧电平读点的单调计数；见 host_window.h 那一段的"为什么"）----
std::uint64_t WindowManager::noteInputSample(std::uint64_t guest_ms) {
    checkMainThread("window.sample");
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

// ---- 投递层接线（S1）：非拥有指针；**唯一**的状态入口（读取端 `inputPlan()` 是 inline 裸读，
//      刻意**不**走 checkMainThread —— 读点快路径的成本必须是"一条 null 检查"）。
void WindowManager::setInputPlan(InputPlan* plan) {
    checkMainThread("window.inputplan");
    input_plan_ = plan;
}

// ---- 读点观察者（C2a；`c2-decisions.md` D-C2-1）：非拥有指针。
//   ★ 只有**装**算状态入口；读端（`readPointObserver()`）是 inline 裸读 —— 它落在每个读点的
//     热路径上，那里的成本必须仍然只是"一条 null 检查"。
void WindowManager::setReadPointObserver(ReadPointObserver* observer) {
    checkMainThread("window.readpoint");
    read_point_observer_ = observer;
}

// ---- 叠层注入路由（S9）：非拥有指针；两个方向都算状态入口（站点名 `window.ui-input`）。
//   ★ 读也走 checkMainThread：调用点只在 click 的臂上 / 抬起（不是每个读点）⇒ 不在热路径上。
//   ★ 装/卸的**条件**（`overlay && ui_script`）由装配者（`SdlWindow::Impl`）判，这里只存指针
//     —— ⛔ 不要让 `WindowManager` 知道叠层开没开（那是第二份状态）。
void WindowManager::setUiInputRouter(UiInputRouter* router) {
    checkMainThread("window.ui-input");
    ui_input_router_ = router;
}

UiInputRouter* WindowManager::uiInputRouter() const {
    checkMainThread("window.ui-input");
    return ui_input_router_;
}

// ---- 无效区（update region；M2 · P4 / B7）----
//
// ★ 口径见 host_window.h。这里只保证两件事：
//   ① `dirty` 永远是 `!update_region.empty()` 的镜像（两处一起改，⛔ 不许一处真一处假）；
//   ② 结果区域**总是规范化**的（`GdiRegion::combine` 负责）⇒ `GetUpdateRgn` / `GetRgnBox` /
//      `GetRegionData` 对同一块无效区给出同一份矩形表。
void WindowManager::invalidateRect(HostWindow& w, const RegionRect* rect) {
    checkMainThread("window.region");
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
    checkMainThread("window.region");
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
    checkMainThread("window.region");
    if (!region.empty() && !w.update_region.empty()) {
        GdiRegion next;
        next.combine(w.update_region, region, RegionCombine::Diff);
        w.update_region = next;
    }
    w.dirty = !w.update_region.empty();
}

void WindowManager::validateAll(HostWindow& w) {
    checkMainThread("window.region");
    w.update_region.setEmpty();
    w.dirty = false;
}

}  // namespace lift
