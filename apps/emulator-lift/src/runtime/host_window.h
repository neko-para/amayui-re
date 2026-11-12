// emulator-lift · headless 窗口 / 消息队列 / 计时器（P0）
//
// ★ 口径（用户 2026-11-04）：先做 **headless 窗口**，但**消息泵语义必须完整**。
//   这里不建 SDL 窗口；P3 只换后端（SDL 事件 → 同一个队列）。
// ★ 句柄都是确定性整数（0x00010000 起）；guest 只当不透明令牌。
// ★ 消息结构 = Win32 MSG 的 x86 布局（28 字节）：hwnd/message/wParam/lParam/time/pt.x/pt.y。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <thread>
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

// =====================================================================================
// 输入来源（★ **唯一输入状态源**：guest 侧所有输入 API 都读 `WindowManager` 的那一份）
// ★ 2026-11-08：这里有两个来源 —— `Os`（SDL 真实事件）与 `Script`（`--input-script` 注入）。
//   两者**走同一段应用代码**（`runtime/input.h`），所以不存在"注入只发消息、GetAsyncKeyState 仍读
//   OS"的第二份状态。
// ★ **优先级（显式规则，互相不污染）**：
//   · 每个 VK 有一个所有者（`Os` 默认 / `Script`）。脚本按下某键 ⇒ 该键归脚本；期间 OS 的
//     按下 / 抬起 / **焦点丢失清键** 一律**不生效**（注入不因窗口失焦而丢）。
//   · 脚本 `key_up` / `mouse_up` 把该键**交还** OS（状态置抬起、所有者清空）。
//   · 光标：脚本第一次 `mouse_move` 起归脚本；期间 OS 的鼠标移动**不生效**（不更新也不发消息，
//     保持 `GetCursorPos` 与 `WM_MOUSEMOVE` 的 lParam 一致）；OS 的鼠标按键同样不生效
//     （它们的坐标来自 OS 光标，而那份位置没有被采用 —— 半截输入比拒绝更糟）。
//   · 脚本**结束**（动作跑完 / `quit`）⇒ `releaseScriptOwnership()`：释放脚本按下的键 / 按钮、
//     交还光标所有权 —— 真人接着玩不会撞上"卡住的键"。
// =====================================================================================
enum class InputSource : std::uint8_t { Os = 0, Script = 1 };
enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2 };
constexpr std::size_t kMouseButtonCount = 3;

const char* inputSourceName(InputSource src);

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

    // =====================================================================================
    // ★ **主线程亲和性守卫**（REQ-01M76SXW0Y3R8QX1F94AB85M9H 的 2026-11-12 裁决）
    //
    // ⛔ 这**不是**令牌（`guest_token.h` 的那一套）。`WindowManager` 按 M0 的口径是**主线程独占
    //   的宿主对象**（`machine.cpp` 里 harness tick 的原注释）：块边界的 harness tick 与
    //   `GetMessageA` 的宿主事件泵都在**合法让出令牌**的窗口里写它 ⇒ 在这里补
    //   `guestToken().check()` 会把 M0 的设计判成 bug。
    // ★ 但"主线程独占"今天靠的是"feeder 的 `X86::windows == nullptr`"，没有任何断言钉住它
    //   ⇒ M1 之后 feeder 侧只要拿到 wm 指针就会**静默**越界。这里守的正是这一类：
    //   非 owner 线程碰**33 个状态入口 + 13 个 inline 读取器**（宿主事件泵的三个接线器
    //   `setHostEventPump` / `clearHostEventPump` / `hasHostEventPump` 不在此列 —— 它们只动
    //   宿主回调 `host_event_pump_`，不是 guest 可见状态）⇒ 计数 + 第一次在 stderr 喊一行；
    //   ⛔ **返回值 / 行为一律不变**。
    // ★ 站点名 = `window.<op>`（能一眼看出是哪一处，⛔ 不用一个 `window.manager` 糊全部）。
    //   嵌套调用（`get` 里调 `pumpTimers`/`peek`、`setCursorClientPos` 里调 `find`）会多次计数
    //   ⇒ 判据只能写 `>= 1` / `>= 2`，⛔ 不写死"每次恰好 +1"。
    // =====================================================================================
    // owner 线程 = **构造这个对象的线程**（成员初始值），`Machine::run` 起始处再显式确认一次
    //   （同一线程会再设成自己；换线程跑同一台 Machine 时由它改口径）。
    // ★ `owner_ == std::thread::id{}`（显式 `setOwnerThread(std::thread::id{})` 清空）⇒
    //   **不计数**：没有 owner 的纯夹具不制造假红。
    void setOwnerThread(std::thread::id id) { owner_ = id; }
    // 非 owner 线程碰入口的累计次数 / 第一次的站点名（nullptr = 一次都没有）。
    unsigned long long offThread() const { return off_thread_.load(std::memory_order_relaxed); }
    const char* firstOffThreadSite() const { return first_off_site_.load(std::memory_order_relaxed); }
    // 清账（夹具用）：计数归零 + first site 归 nullptr（⇒ 下一次违规会重新喊一行）。
    void resetOffThread();

    std::uint32_t registerClass(const std::string& name, std::uint32_t proc, std::uint32_t style);
    std::uint32_t createWindow(const std::string& cls, std::uint32_t style, std::uint32_t exstyle, int x,
                               int y, int w, int h, std::uint32_t menu);
    HostWindow* find(std::uint32_t hwnd);
    const HostWindow* find(std::uint32_t hwnd) const;
    bool destroyWindow(std::uint32_t hwnd);
    std::size_t windowCount() const {
        checkMainThread("window.count");
        return windows_.size();
    }

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
    // ★ 来源规则见文件上方 `InputSource`；`setKeyDown(vk,down)` = OS（旧调用口径）。
    void setKeyDown(std::uint32_t vk, bool down);
    bool setKeyDown(InputSource src, std::uint32_t vk, bool down);  // false = 被所有者拒绝
    bool keyDown(std::uint32_t vk) const;
    bool keyOwnedByScript(std::uint32_t vk) const;
    void clearKeys();      // 全部清（旧口径 / 测试）
    void clearOsInput();   // 焦点丢失：只清 **OS 拥有** 的键与按钮电平
    // 脚本结束：释放脚本按下的键 / 按钮（置抬起）并把所有权交还 OS。
    void releaseScriptOwnership();
    // 鼠标按钮位图（WM_* 的 wParam / `mouse_buttons` 的**唯一**一份；SDL 与脚本共用）。
    std::uint32_t mouseButtons() const {
        checkMainThread("window.mouse");
        return mouse_buttons_;
    }
    bool setMouseButton(MouseButton b, bool down, InputSource src);  // 同时写 VK 电平
    bool mouseButtonOwnedByScript(MouseButton b) const;
    void postQuit(std::uint32_t code);
    bool quitPosted() const {
        checkMainThread("window.quit");
        return quit_;
    }
    std::uint32_t quitCode() const {
        checkMainThread("window.quit");
        return quit_code_;
    }
    std::size_t queueSize() const {
        checkMainThread("window.count");
        return queue_.size();
    }

    void pumpTimers(X86& cpu);
    std::uint32_t setTimer(X86& cpu, std::uint32_t hwnd, std::uint32_t id, std::uint32_t elapse_ms);
    bool killTimer(std::uint32_t hwnd, std::uint32_t id);
    std::size_t timerCount() const {
        checkMainThread("window.count");
        return timers_.size();
    }

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
    std::uint32_t cursor() const {
        checkMainThread("window.cursor");
        return cursor_;
    }
    // ★ 第 4 个参数是来源：`Script` 一旦写过位置就拥有光标，之后 `Os` 的移动被拒绝（见 InputSource）。
    void setCursorPos(int x, int y);  // guest 自己调 SetCursorPos（不改所有权）
    bool setCursorClientPos(std::uint32_t hwnd, int x, int y, InputSource src);
    void setCursorClientPos(std::uint32_t hwnd, int x, int y) {
        setCursorClientPos(hwnd, x, y, InputSource::Os);
    }
    bool cursorOwnedByScript() const {
        checkMainThread("window.cursor");
        return cursor_script_owned_;
    }
    // 屏幕坐标 → 客户区坐标（`SetCursorPos` / `GetCursorPos` 的反向；与 setCursorClientPos 同一份原点）。
    void cursorClientPos(std::uint32_t hwnd, int* x, int* y) const;
    void getCursorPos(int* x, int* y) const {
        checkMainThread("window.cursor");
        if (x) *x = cursor_x_;
        if (y) *y = cursor_y_;
    }
    int showCursor(bool show);                     // 显示 +1 / 隐藏 -1，返回**新**显示计数
    std::uint32_t setCapture(std::uint32_t hwnd);  // 返回**旧**捕获窗口（0 = 没有）
    std::uint32_t capture() const {
        checkMainThread("window.capture");
        return capture_;
    }

    // =====================================================================================
    // ★ 输入采样计数（2026-11-08；**注入层的判据**，不是 guest 语义）
    // ★ 为什么需要它：`--input-script` 的 `click` 曾经把 down / up 放在同一 tick（实测相隔
    //   20k 条指令 ≈ 0.7 ms guest 时间），而该界面的输入采样只有 ~20 Hz（实测 `GetCursorPos`
    //   880 次 / 43.96 s）⇒ **按下被完全漏掉**（`.tmp/exitstall/report.md`：frames 100–108
    //   逐字节相同）。修法不是让写脚本的人手算 hold，而是让注入层自己回答"guest 到底有没有
    //   看过按下状态" ⇒ 在 guest 侧的**电平读点**上维护一个单调计数，`mouse_up` / `key_up`
    //   等到它 +1 之后才落地。
    // ★ 计数点只有**电平读点**（见 `user32_input.cpp` 的 GetCursorPos / `user32_window.cpp`
    //   的 GetAsyncKeyState）：
    //   · ⛔ **不含 PeekMessageA** —— 它在这条实现线里被调 219,164 次 / 400M 条指令（实测，
    //     `.tmp/exitstall/logs/prof-stall.log` 第 16 名，≈ 每 1.8k 条指令一次），把它算成采样点
    //     会让"等一次采样"退化成 ~0.2 ms ⇒ 等于没等；而且它取走的 `WM_*` 在队列里**不会丢**，
    //     "按下被漏掉"从来不是它的问题。
    //   · `GetKeyState` 本线**没有实现**（`rg GetKeyState src/` = 0 命中）⇒ 没有这个计数点。
    //   · 实测这条实现线里 GetAsyncKeyState 一次也没被这条 UI 调用（同上一份 profile）⇒ 真正
    //     提供 20 Hz 心跳的是 `GetCursorPos`（引擎每迭代的位置采样）。
    // ★ 计数**不改任何返回值**（⛔ 不是 guest 语义 / 不是采样率）；它只回答"注入层还要不要等"。
    // =====================================================================================
    std::uint64_t inputSampleCount() const {
        checkMainThread("window.sample");
        return input_samples_;
    }
    // 采样周期估计（guest ms；0 = 样本不足，还不知道）。自动 hold 的**安全下限**用它，
    // 规则 = **最近若干个采样间隔的最大值**（见 `noteInputSample` 的实现注释）。
    std::uint64_t inputSamplePeriodMs() const {
        checkMainThread("window.sample");
        return input_sample_period_ms_;
    }
    // 最近一次采样的 guest 时刻（自动 hold 的"看到之后又等了一个周期"从它算）。
    std::uint64_t lastInputSampleMs() const {
        checkMainThread("window.sample");
        return last_input_sample_ms_;
    }
    // guest 侧读点调用一次。返回**新**计数。`guest_ms` = guest 虚拟时钟（采样间隔估计用）。
    std::uint64_t noteInputSample(std::uint64_t guest_ms);

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
    // 每个 VK / 按钮的所有者（InputSource 的整数值；0 = OS，1 = 脚本）
    std::uint8_t key_owner_[256] = {};
    std::uint8_t button_owner_[kMouseButtonCount] = {};
    std::uint32_t mouse_buttons_ = 0;
    bool cursor_script_owned_ = false;
    // 光标 / 捕获（见上面的方法注释）
    std::uint32_t cursor_ = 0;
    std::uint32_t capture_ = 0;
    std::int32_t cursor_x_ = 0;
    std::int32_t cursor_y_ = 0;
    int cursor_show_count_ = 0;
    // ★ 输入采样计数（见 `inputSampleCount` 那一段）：单调计数 + 最近间隔
    std::uint64_t input_samples_ = 0;
    std::uint64_t input_sample_period_ms_ = 0;  // 最近 K 个采样间隔的**最大值**（周期估计）
    std::uint64_t last_input_sample_ms_ = 0;
    bool have_input_sample_ = false;
    // 上一次采样间隔的小环（K = 8）：同一个"输入迭代"里往往有**多次**读点
    // （GetAsyncKeyState + GetCursorPos…），它们之间的间隔只有 0~1 ms；取**最大值**才代表
    // "迭代周期"，取最近一次会被这种同迭代内的小间隔污染成 1 ms（⇒ 安全下限失效）。
    static constexpr std::size_t kInputIntervalRing = 8;
    std::uint64_t input_intervals_[kInputIntervalRing] = {};
    std::uint32_t input_interval_count_ = 0;
    std::uint32_t input_interval_next_ = 0;

    // =====================================================================================
    // ★ 主线程亲和性账（**不是**令牌；见类头那一段与 `host_window.cpp` 的 `checkMainThread`）
    //   `owner_` 的初值 = **构造这个对象的那个线程**（`Machine` 是在主线程构造的；
    //   `Machine::run` 起始处再显式 `setOwnerThread(std::this_thread::get_id())` 确认）。
    //   成员是三份**可变诊断账**：`checkMainThread` 是 const 成员（所有读取器都是 const）⇒ mutable。
    // =====================================================================================
    // 入口的主线程检查点：非 owner ⇒ `off_thread_++` + 第一次 stderr 喊一行。
    // ⚠ 与 `GuestToken::check` / `SurfaceArena::checkToken` 同一条口径：**不改变返回值 / 行为**。
    void checkMainThread(const char* site) const;

    std::thread::id owner_ = std::this_thread::get_id();
    mutable std::atomic<unsigned long long> off_thread_{0};
    mutable std::atomic<const char*> first_off_site_{nullptr};
};

}  // namespace lift
