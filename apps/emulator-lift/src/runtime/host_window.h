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

#include "runtime/input_device.h"
#include "runtime/region.h"

namespace lift {

struct X86;
class InputPlan;
class ReadPointObserver;
class UiInputRouter;
class WindowManager;

// ★ S4：状态收口的**唯一**投影入口（§3.6）。设备只返回待投影的 `LevelRef`，这两个 free function
//   逐条 post up；它们是 `WindowManager::releaseProgramLevels` / `dropOsLevelsState` 的 friend。
namespace input {
std::vector<LevelRef> releaseProgram(WindowManager& wm, std::uint32_t time_ms);
std::vector<LevelRef> dropOsLevels(WindowManager& wm, std::uint32_t time_ms);
}  // namespace input

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
// 输入状态（★ **唯一输入状态源**：guest 侧所有输入 API 都读 `WindowManager` 的那一份）
// ★ 2026-11-19（S1 → S4）：输入状态在 `runtime/input_device.h` / `.cpp` 的 `InputDevice`
//   （本类以私有成员 `device_` 持有）。S4 起设备是**最终形态**：单一租约（`lease_`）+
//   逐电平来源（`level_owner_`）+ `WriteResult` 转移；⛔ 不再有 v1 的三套 owner。
//   `InputSource` / `MouseButton` / 按钮 ↔ VK 映射也都在 `runtime/input_device.h`（本类 include 它）。
// ★ 本类的公开 setter / getter 保留**同名**与同一个 `checkMainThread` 站点名，内部转 `device_`；
//   `setKeyDown` / `setMouseButton` 返回 `WriteResult`（`input::*` 据此只在电平转移时 post）。
// =====================================================================================
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
    // ★ 从**队首**起找**第一条匹配 `pred`** 的消息：匹配只决定"看哪一条"，不匹配的**原地保留**（FIFO）。
    //   这一条是 `PeekMessageA` 的 hWnd / wMsgFilterMin / wMsgFilterMax 过滤的实现原语
    //   （文档：PeekMessage 只取一条**匹配**的消息；不匹配的留在队列里）。
    //   · `remove == false`（`PM_NOREMOVE`）⇒ 什么都不取走；
    //   · `remove == true`（`PM_REMOVE`）⇒ 只取走**那一条**，其余消息（含它前面的）相对顺序不变；
    //   · 队列空且 `postQuitMessage` 过 ⇒ `WM_QUIT` 是**队尾**的一条虚拟消息（与 `peekAt` 同形），
    //     它也参与 `pred` 匹配；`remove == true` 时消费掉它。
    //   ⛔ 不实现成"逐条取走不匹配的"：那条路会静默丢消息（本单修的正是它）。
    bool peekMatching(const std::function<bool(const WinMessage&)>& pred, WinMessage* out, bool remove);
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
    // ★ 来源规则见 `runtime/input_device.h` 的 `InputSource` / `Driver`；`setKeyDown(vk,down)` = OS。
    //   ★ S4：返回 `WriteResult{accepted, change}` —— `accepted==true && change==None` 是「接受但不投影」
    //     （§3.2 #7/#8/#9/#13/#14/#16）。
    WriteResult setKeyDown(std::uint32_t vk, bool down);
    WriteResult setKeyDown(InputSource src, std::uint32_t vk, bool down);
    bool keyDown(std::uint32_t vk) const;
    void clearKeys();  // 全部清（旧口径 / 测试；整机复位：电平 / 来源标签 / 租约）
    // 脚本（Program）抬起的电平只读快照（收口前打印 / 测试用；⛔ 不改状态）。
    std::vector<LevelRef> programHeldLevels() const;
    // 鼠标按钮位图（WM_* 的 wParam）。★ S3：**只读派生**自设备的 VK 电平
    //   （`key_level_[VK_LBUTTON/RBUTTON/MBUTTON]`），⛔ 没有第二份 `mouse_buttons_`。
    std::uint32_t mouseButtons() const {
        checkMainThread("window.mouse");
        return device_.buttons();
    }
    // S4：写按钮 = 写它的 VK 电平（位图是派生）。
    WriteResult setMouseButton(MouseButton b, bool down, InputSource src);
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
    // 光标（M2 · 输入族；**headless 虚拟状态**）
    // ★ 口径（2026-11-04）：不建 SDL 光标、不读 .cur；句柄 / 位置 / 显示计数都是
    //   **确定性宿主状态**，语义按 Win32 文档照做（返回旧句柄 / 新显示计数 / TRUE），
    //   ⛔ 不返回随机值、⛔ 不假装成功却什么都不记。
    // ★ 虚拟光标位置初值 = **(0,0)**：headless 没有鼠标输入，屏幕原点是"还没有输入"的
    //   中性状态（选它而不是客户区中心：后者会让 `ScreenToClient` 在上窗口之前给出负坐标）。
    // ★ `ShowCursor` 的语义是**显示计数**（真机初值 0，`>= 0` 才显示）：显示 +1 / 隐藏 -1，
    //   返回值 = **新计数**（不是布尔、不是旧值）。
    // ★ `WindowManager::setCapture/capture`（Win32 capture 的死状态）已在 S1 删除：零调用者。
    // =====================================================================================
    std::uint32_t setCursor(std::uint32_t hcursor);  // 返回**旧** HCURSOR（Win32 语义）
    std::uint32_t cursor() const {
        checkMainThread("window.cursor");
        return cursor_;
    }
    // ★ guest 自己的 `SetCursorPos`：走 `Driver::Guest`（S4 / Q7）——**永远接受**、不改租约 / 电平、
    //   不发消息（§3.2 #1）。
    void setCursorPos(int x, int y);
    // ★ 第 4 个参数是来源：`Script`（= `Driver::Program`）一旦写过位置就取得租约，之后 `Os` 的移动 /
    //   上升被拒绝（规则在 `runtime/input_device.h` 的 `Driver` / §3.2）。
    bool setCursorClientPos(std::uint32_t hwnd, int x, int y, InputSource src);
    void setCursorClientPos(std::uint32_t hwnd, int x, int y) {
        setCursorClientPos(hwnd, x, y, InputSource::Os);
    }
    // 当前租约是不是 Program（= 注入 / 配方 / 控制层在驱动）。S4 取代 `cursorOwnedByScript()`。
    bool programDriving() const {
        checkMainThread("window.cursor");
        return device_.programDriving();
    }
    // 屏幕坐标 → 客户区坐标（`SetCursorPos` / `GetCursorPos` 的反向；与 setCursorClientPos 同一份原点）。
    void cursorClientPos(std::uint32_t hwnd, int* x, int* y) const;
    void getCursorPos(int* x, int* y) const {
        checkMainThread("window.cursor");
        device_.cursorScreen(x, y);
    }
    // ★ `--cursor-warp` 的回灌抑制**不在这一层**：`WindowManager` 只保存 guest 虚拟状态；
    //   回灌抑制需要**窗口客户区像素**（warp 落点那一对，clamp 之前），那一层拿不到 ⇒ 它在
    //   `runtime/present_sdl.cpp` 的 SDL motion 入口（`runtime/warp_echo.h` 的 FIFO）。
    int showCursor(bool show);  // 显示 +1 / 隐藏 -1，返回**新**显示计数

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
    // ★ 投递层接线（S1；`REQ-01M7G7EMX3JR272D8FVM7JFY9D`）
    //   **非拥有指针**（形状照 `presenter_.setFrameSampler(&sampler_)`）：计划的生命周期在
    //   宿主装配处（S4），`WindowManager` 只借它。`nullptr` = 没有计划 = 缺省档 ⇒ 每个读点
    //   只有**一次判空**（`inputPlan()` 是 inline 裸读，⛔ 不要在这里加 checkMainThread；
    //   那会把"1 条 null 检查"变成"线程检查 + 判空"）。
    //   ★ 只有 `setInputPlan` 是状态入口（算进主线程亲和性账），读指针不算。
    //   ⚠ `setInputPlan` 必须在 `machine.cpp` / `aot_run.cpp` 的**循环外**调用（S4 的装配点）。
    // =====================================================================================
    void setInputPlan(InputPlan* plan);
    InputPlan* inputPlan() const { return input_plan_; }

    // =====================================================================================
    // ★ **读点观察者**（C2a；父级裁决 `c2-decisions.md` D-C2-1）
    //   **非拥有指针**（形状逐字照 `setInputPlan` / `inputPlan`）：生命周期 = 装配者
    //   （`aot_main` 的 `PluginHost`），`WindowManager` 只借它。`nullptr` = 没有观察者 ⇒
    //   `input.h` 的四个投递口每个读点只多**一次判空**（⛔ 不取时钟、不碰容器、不加第三个 `if`）。
    //   ★ `setReadPointObserver` 是状态入口（算进主线程亲和性账，站点名 `window.readpoint`）；
    //     **读指针不算**：它在每个读点的热路径上，读指针就是一次裸读。
    //   ⚠ 必须在 `machine.cpp` / `aot_run.cpp` 的**循环外**装配（与 `setInputPlan` 同一条口径）。
    //   ★ 为什么不去 `InputPlan` 上挂回调：那让核心认识配方（`core-shape` 的禁用词 + 合同 §6 行 1）。
    // =====================================================================================
    void setReadPointObserver(ReadPointObserver* observer);
    ReadPointObserver* readPointObserver() const { return read_point_observer_; }

    // =====================================================================================
    // ★ 叠层注入路由（S9；`docs/03-emulator/overlay.md` D6 / D2）
    //   **非拥有指针**：生命周期 = 装配者（`SdlWindow::Impl`，由 `attachWindowManager` /
    //   `setOverlay` 装/卸）。`nullptr` = 没有叠层 / `--ui-input=pass` 未装 / 叠层已关 ⇒
    //   `InputPlan` 的 click 路径只多一次判空（⛔ 没有第二份命中 / 捕获状态）。
    //   ★ 与 `setInputPlan` 不同：**读指针也算状态入口**（站点名 `window.ui-input`）——它只用
    //     在 click 的臂上 / 抬起（不是每个读点），所以线程检查的代价不在热路径上。
    //   ★ 谁装、装的条件（`overlay && ui_script`）不在这里判：`WindowManager` 只存指针。
    // =====================================================================================
    void setUiInputRouter(UiInputRouter* router);
    UiInputRouter* uiInputRouter() const;

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
    // ★ S4：状态收口只经这两个 private 方法（§3.6 / §3.7）——它们只返回待投影的 `LevelRef`，
    //   消息由 `input::releaseProgram` / `input::dropOsLevels` 逐条 post。
    //   ⛔ 不再保留 `clearOsInput` / `releaseScriptOwnership` 这两个「只改状态、不投影」的公开入口。
    friend std::vector<LevelRef> input::releaseProgram(WindowManager& wm, std::uint32_t time_ms);
    friend std::vector<LevelRef> input::dropOsLevels(WindowManager& wm, std::uint32_t time_ms);
    std::vector<LevelRef> releaseProgramLevels() {
        checkMainThread("window.key");
        return device_.release(Driver::Program);
    }
    std::vector<LevelRef> dropOsLevelsState() {
        checkMainThread("window.key");
        return device_.dropOsLevels();
    }

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
    // ★ 输入状态（S4：单一租约 + 逐电平来源都在设备里；写 API private + friend）。
    InputDevice device_;
    // 光标（见上面的方法注释）
    std::uint32_t cursor_ = 0;
    int cursor_show_count_ = 0;
    // ★ 输入采样计数（见 `inputSampleCount` 那一段）：单调计数 + 最近间隔
    std::uint64_t input_samples_ = 0;
    std::uint64_t input_sample_period_ms_ = 0;  // 最近 K 个采样间隔的**最大值**（周期估计）
    std::uint64_t last_input_sample_ms_ = 0;
    bool have_input_sample_ = false;
    // ★ 投递层（S1）：非拥有指针；nullptr = 没有计划（缺省档）。
    InputPlan* input_plan_ = nullptr;
    // ★ 读点观察者（C2a）：非拥有指针；nullptr = 没有观察者（缺省档）。见方法注释。
    ReadPointObserver* read_point_observer_ = nullptr;
    // ★ 叠层注入路由（S9）：非拥有指针；nullptr = 没有叠层（缺省档）。见方法注释。
    UiInputRouter* ui_input_router_ = nullptr;
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
