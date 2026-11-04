// emulator-lift · user32 输入 / 光标 / 监控器 / 系统参数族（M2；headless 虚拟状态，语义按 Win32 文档）
//
// ★ 口径（2026-11-04 用户裁决，见任务卡 §2 与 SCOPE §1）：
//   · **可以按 headless 真做**（状态真记在 `runtime/host_window`，返回值是 Win32 语义的真值）：
//       `SetCursor`（返回**旧**光标并记录）、`GetCursorPos` / `SetCursorPos`（headless 虚拟光标位置，
//       初值 **(0,0)**）、`ShowCursor`（增减内部**显示计数**并返回**新计数**）、
//       `MonitorFromWindow` / `GetMonitorInfoA`（**单显示器**确定值）、
//       `SystemParametersInfoA`（只认引擎真正用的 action，见下）、
//       `MsgWaitForMultipleObjects`（无对象可等 ⇒ **走消息队列语义**）。
//   · **确定性伪句柄 + 日志**（本线不建真光标，照 P0 `LoadCursorA` 的口径）：
//       `LoadCursorFromFileA` —— ⛔ **不读 .cur 文件**。
//   · `UnhookWindowsHookEx`：本线没有 `SetWindowsHookEx` 导入 ⇒ 没有可解的钩子；返回 TRUE + 日志。
//   · ⛔ 不建 SDL 光标 / 不做真实鼠标输入（P3 才接 SDL 事件源）；⛔ 不返回随机值。
//
// ★ 实现落点：光标 / 捕获状态在 `WindowManager`（host_window.h/.cpp 的"光标 / 捕获"段）。
// ★ 实测调用点（`.lst`，锚 EA）：
//   · `0x004B8692 call SetCursor`（`sub_4B8680`：应用 `[esi+408h]` 的 HCURSOR，之后 `add [esi+42Ch],1`；
//     **不读**返回值 —— 但真机语义仍是"返回旧光标"，照做）。
//   · `0x00404040 / 0x004040C0 / 0x004771E4 call GetCursorPos`（随后 `ScreenToClient` ⇒ 鼠标位置跟踪）。
//   · `0x00406852 / 0x0042202E / 0x004B93EC call SetCursorPos`（切显示模式后把光标放到屏幕中心）。
//   · `0x00414080 call MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST=2)` →
//     `0x004140B2 call GetMonitorInfoA`（随后用 `mi.rcMonitor` 算窗口尺寸）。
//   · `0x00408773 call SystemParametersInfoA(0x1014=SPI_GETSELECTIONFADE, 0, &out, 0)`；
//     `0x00494DED call SystemParametersInfoA(0x5C=SPI_GETWINDOWSEXTENSION, 0, 0, 0)`（NT 4 判据分支）；
//     `0x004762F0 → SystemParametersInfoA(0x1015=SPI_SETSELECTIONFADE, 0, p, 0)`。
//     ⇒ 只认这三条；其余 action 按 Win32 失败值 FALSE + 日志（⛔ 不静默成功）。
//   · `0x004B8BDE call LoadCursorFromFileA(临时文件)`（引擎先把 .cur 写进临时文件再加载）。
//   · `0x004CF949 call MsgWaitForMultipleObjects`（`sub_4CF8C0`：先 0ms 轮询对象，再等消息，超时 ≤10ms）。
//   · `0x004760DE call UnhookWindowsHookEx(hhk)`。
#include <cstdint>
#include <string>

#include "runtime/clock.h"
#include "runtime/host_window.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---- Win32 常量（只写用到的；⛔ 不抄整张表）----
constexpr std::uint32_t kWaitObject0 = 0x00000000u;
constexpr std::uint32_t kWaitTimeout = 0x00000102u;  // 258

constexpr std::uint32_t kWmPaint = 0x000F;
constexpr std::uint32_t kWmQuit = 0x0012;
constexpr std::uint32_t kWmTimer = 0x0113;

constexpr std::uint32_t kQsPostMessage = 0x0008u;
constexpr std::uint32_t kQsTimer = 0x0010u;
constexpr std::uint32_t kQsPaint = 0x0020u;

constexpr std::uint32_t kMonitorDefaultToNull = 0x00000000u;
constexpr std::uint32_t kMonitorDefaultToPrimary = 0x00000001u;
constexpr std::uint32_t kMonitorDefaultToNearest = 0x00000002u;

constexpr std::uint32_t kSpiGetWindowSExtension = 0x005C;  // SPI_GETWINDOWSEXTENSION
constexpr std::uint32_t kSpiGetSelectionFade = 0x1014;     // SPI_GETSELECTIONFADE
constexpr std::uint32_t kSpiSetSelectionFade = 0x1015;     // SPI_SETSELECTIONFADE

// 伪句柄 / 确定性句柄：非 0、稳定、一眼能看出不是真 HANDLE。
// ★ 与 `user32_resource.cpp` 的 kFakeCursor(0x00020002) 同一口径（本线只有一个"stock 光标"）。
constexpr std::uint32_t kFakeCursor = 0x00020002u;
// 单显示器句柄（真机是内核/驱动返回的不透明值；本线固定一台 1280x720 的显示器）。
constexpr std::uint32_t kPrimaryMonitor = 0x00030001u;

WindowManager* requireWindows(X86& cpu, const char* api) {
    if (!cpu.windows) shimAbort("user32", api, "cpu.windows 为空（Machine 没装配窗口管理器）");
    return cpu.windows;
}

// ---- SetCursor(HCURSOR) → 旧 HCURSOR ----
// ★ Win32：不校验句柄（NULL 合法 = 隐藏光标）；返回值是**前一个**光标句柄。
void shimSetCursor(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "SetCursor");
    const std::uint32_t hcursor = call.u32(0);
    const std::uint32_t old = wm->setCursor(hcursor);
    shimLog("user32", "SetCursor",
            "hCursor=0x%08X ⇒ 旧 HCURSOR=0x%08X（headless 虚拟光标状态；不建真光标）", hcursor, old);
    call.result = old;
}

// ---- GetCursorPos(LPPOINT) → BOOL ----
// ★ Win32：lpPoint==NULL ⇒ FALSE + ERROR_INVALID_PARAMETER(87)。
//   本线单线程、无宿主鼠标 ⇒ 恒成功，写虚拟位置（初值 (0,0)，SetCursorPos 之后就是它设的值）。
void shimGetCursorPos(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "GetCursorPos");
    const std::uint32_t ea = call.u32(0);
    if (!ea || !cpu.mem->committed(ea, 8)) {
        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER
        shimLog("user32", "GetCursorPos", "lpPoint=0x%08X 不可写 ⇒ FALSE + 87", ea);
        call.result = 0;
        return;
    }
    int x = 0, y = 0;
    wm->getCursorPos(&x, &y);
    cpu.mem->wr32(ea, static_cast<std::uint32_t>(x));
    cpu.mem->wr32(ea + 4, static_cast<std::uint32_t>(y));
    shimLog("user32", "GetCursorPos", "lpPoint=(%d,%d)（headless 虚拟光标位置）", x, y);
    call.result = 1;
}

// ---- SetCursorPos(int x, int y) → BOOL ----
// ★ Win32：只有"当前桌面不允许写光标位置"才 FALSE；headless 恒 TRUE，但**必须真的改状态**
//   （后续 GetCursorPos 读到的就是它）。
void shimSetCursorPos(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "SetCursorPos");
    const int x = call.i32(0);
    const int y = call.i32(1);
    wm->setCursorPos(x, y);
    shimLog("user32", "SetCursorPos", "(%d,%d)（headless 虚拟光标位置）", x, y);
    call.result = 1;
}

// ---- ShowCursor(BOOL bShow) → 新显示计数 ----
// ★ Win32：内部**显示计数**（不是布尔）；bShow!=0 ⇒ +1，bShow==0 ⇒ -1；返回**新计数**。
//   真机初值 0 ⇒ 第一次 ShowCursor(TRUE) 返回 1。返回值可能为负（调用方据此判断可见性）。
void shimShowCursor(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "ShowCursor");
    const bool show = call.u32(0) != 0;
    const int count = wm->showCursor(show);
    shimLog("user32", "ShowCursor", "bShow=%d ⇒ 新显示计数=%d（>=0 才显示）", show ? 1 : 0, count);
    call.result = static_cast<std::uint32_t>(count);
}

// ---- LoadCursorFromFileA(LPCSTR) → HCURSOR ----
// ★ 本线**不建真光标**（照 P0 `LoadCursorA` 的口径）：不读文件、不解析 .cur，返回确定性伪句柄 + 日志。
//   ⛔ 不要改成读 .cur —— 光标形状是平台呈现（P3），不是本切片的语义。
void shimLoadCursorFromFileA(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::uint32_t name_ea = call.u32(0);
    if (name_ea == 0) {
        // Win32：lpFileName==NULL ⇒ NULL（+ 87）。引擎不会走这条，但不许凭空给句柄。
        shimLog("user32", "LoadCursorFromFileA", "lpFileName=NULL ⇒ NULL（Win32 失败值）");
        call.result = 0;
        return;
    }
    const std::string path = call.str(0);
    shimLog("user32", "LoadCursorFromFileA",
            "lpFileName=\"%s\" ⇒ 确定性伪 HCURSOR 0x%08X（本线不建真光标、⛔ 不读 .cur；P3 接 SDL 时再改）",
            path.c_str(), kFakeCursor);
    call.result = kFakeCursor;
}

// ---- MonitorFromWindow(HWND, DWORD) → HMONITOR ----
// ★ Win32 的三种 dwFlags：DEFAULTTONULL(0) 拿不到就给 NULL；DEFAULTTOPRIMARY(1) / DEFAULTTONEAREST(2)
//   拿不到就给主显示器。本线只有**一台**显示器 ⇒ 有效窗口恒给 kPrimaryMonitor。
void shimMonitorFromWindow(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "MonitorFromWindow");
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t flags = call.u32(1);
    const bool have_window = hwnd != 0 && wm->find(hwnd) != nullptr;
    std::uint32_t result = kPrimaryMonitor;
    if (!have_window) {
        // 没有窗口（或句柄无效）：真机的"最近显示器"仍会给出主显示器；DEFAULTTONULL 给 NULL。
        if (flags == kMonitorDefaultToNull) {
            result = 0;
        } else if (flags != kMonitorDefaultToPrimary && flags != kMonitorDefaultToNearest) {
            shimLog("user32", "MonitorFromWindow", "dwFlags=%u 不是 0/1/2 ⇒ 按 DEFAULTTONEAREST 处理",
                    flags);
        }
    }
    shimLog("user32", "MonitorFromWindow",
            "hwnd=0x%08X dwFlags=%u（%s）⇒ HMONITOR=0x%08X（单显示器确定性值）", hwnd, flags,
            have_window ? "有效窗口" : "没有窗口", result);
    call.result = result;
}

// ---- GetMonitorInfoA(HMONITOR, LPMONITORINFO) → BOOL ----
// ★ 写满 `MONITORINFO`（40 字节）：cbSize 原样保留（Win32 要求调用方填 40）、
//   rcMonitor / rcWork = 屏幕矩形（本线没有任务栏 ⇒ work = monitor）、dwFlags = MONITORINFOF_PRIMARY(1)。
void shimGetMonitorInfoA(X86& cpu, ShimCall& call) {
    (void)requireWindows(cpu, "GetMonitorInfoA");
    const std::uint32_t hmon = call.u32(0);
    const std::uint32_t ea = call.u32(1);
    if (!ea || !cpu.mem->committed(ea, 40)) {
        cpu.shim.last_error = 87;
        shimLog("user32", "GetMonitorInfoA", "lpmi=0x%08X 不可写（需要 40 字节）⇒ FALSE + 87", ea);
        call.result = 0;
        return;
    }
    const std::uint32_t cb = cpu.mem->rd32(ea);
    if (cb != 40) {
        cpu.shim.last_error = 87;
        shimLog("user32", "GetMonitorInfoA", "mi.cbSize=%u ≠ sizeof(MONITORINFO)=40 ⇒ FALSE + 87", cb);
        call.result = 0;
        return;
    }
    if (hmon != kPrimaryMonitor) {
        cpu.shim.last_error = 87;
        shimLog("user32", "GetMonitorInfoA", "hMonitor=0x%08X 不是本线的单显示器 0x%08X ⇒ FALSE + 87", hmon,
                kPrimaryMonitor);
        call.result = 0;
        return;
    }
    const auto wr = [&](std::uint32_t at, std::int32_t v) {
        cpu.mem->wr32(at, static_cast<std::uint32_t>(v));
    };
    // MONITORINFO: cbSize(0) rcMonitor(4..19) rcWork(20..35) dwFlags(36)
    wr(ea + 4, 0);
    wr(ea + 8, 0);
    wr(ea + 12, WindowManager::kScreenW);
    wr(ea + 16, WindowManager::kScreenH);
    wr(ea + 20, 0);
    wr(ea + 24, 0);
    wr(ea + 28, WindowManager::kScreenW);
    wr(ea + 32, WindowManager::kScreenH);
    wr(ea + 36, 1);  // MONITORINFOF_PRIMARY
    shimLog("user32", "GetMonitorInfoA",
            "hMonitor=0x%08X ⇒ rcMonitor=rcWork=(0,0,%d,%d) dwFlags=MONITORINFOF_PRIMARY（单显示器）", hmon,
            WindowManager::kScreenW, WindowManager::kScreenH);
    call.result = 1;
}

// ---- SystemParametersInfoA(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) ----
// ★ 只认引擎真正用的 **三条**（`.lst` 调用点见文件头）：
//   · 0x1014 SPI_GETSELECTIONFADE：`pvParam` 写 BOOL。headless 没有选择淡入动画 ⇒ 写 0 + TRUE；
//   · 0x1015 SPI_SETSELECTIONFADE：接受设置（本线无动画可影响）⇒ TRUE + 日志（不改任何状态）；
//   · 0x005C SPI_GETWINDOWSEXTENSION：Win9x 专属；NT 家族返回 FALSE（本线按 NT 口径，与版本 shim 一致）。
//   ⛔ 其余 action 一律 FALSE + 日志（Win32 失败值），⛔ 不许"返回 TRUE 但什么都不做"。
void shimSystemParametersInfoA(X86& cpu, ShimCall& call) {
    (void)requireWindows(cpu, "SystemParametersInfoA");
    const std::uint32_t action = call.u32(0);
    const std::uint32_t ui_param = call.u32(1);
    const std::uint32_t pv = call.u32(2);
    const std::uint32_t win_ini = call.u32(3);
    switch (action) {
        case kSpiGetSelectionFade: {
            if (!pv || !cpu.mem->committed(pv, 4)) {
                cpu.shim.last_error = 87;
                shimLog("user32", "SystemParametersInfoA",
                        "SPI_GETSELECTIONFADE(0x1014)：pvParam=0x%08X 不可写 ⇒ FALSE + 87", pv);
                call.result = 0;
                return;
            }
            cpu.mem->wr32(pv, 0);  // FALSE = 不用选择淡入动画（headless 无动画）
            shimLog("user32", "SystemParametersInfoA",
                    "SPI_GETSELECTIONFADE(0x1014) ⇒ *pvParam=FALSE（headless 无选择淡入动画）TRUE");
            call.result = 1;
            return;
        }
        case kSpiSetSelectionFade:
            shimLog("user32", "SystemParametersInfoA",
                    "SPI_SETSELECTIONFADE(0x1015)：uiParam=%u pvParam=0x%08X fWinIni=%u ⇒ TRUE"
                    "（本线没有选择淡入动画可影响；⛔ 不静默忽略：这行日志就是它的效果）",
                    ui_param, pv, win_ini);
            call.result = 1;
            return;
        case kSpiGetWindowSExtension:
            shimLog("user32", "SystemParametersInfoA",
                    "SPI_GETWINDOWSEXTENSION(0x5C) ⇒ FALSE（Win9x 专属；本线按 NT 口径，与版本 shim 一致）");
            call.result = 0;
            return;
        default:
            shimLog("user32", "SystemParametersInfoA",
                    "uiAction=0x%04X（uiParam=%u pvParam=0x%08X fWinIni=%u）未建模（引擎未用到）⇒ FALSE"
                    "（Win32 失败值；⛔ 不返回成功）",
                    action, ui_param, pv, win_ini);
            call.result = 0;
            return;
    }
}

// 队列里最早一条消息的"等待掩码类"（QS_*）。本线只有 hwnd 消息队列，没有线程发送消息 / 输入消息。
std::uint32_t queueClass(const WinMessage& m) {
    switch (m.message) {
        case kWmPaint: return kQsPaint;
        case kWmTimer: return kQsTimer;
        default: return kQsPostMessage;  // WM_QUIT / 用户消息 / 我们自己 post 的 WM_* 都算 post 队列
    }
}

// ---- MsgWaitForMultipleObjects(nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask) → DWORD ----
// ★ 口径（任务卡 §2）：**无对象可等 ⇒ 走消息队列语义**。
//   · 先 pump 计时器（消息可能由 WM_TIMER 产生）；队列里有匹配 dwWakeMask 的消息
//     ⇒ `WAIT_OBJECT_0 + nCount`（= 消息可用），**不取走**消息（Win32 语义：调用方自己 Peek）；
//   · 没有消息且 `dwMilliseconds == 0` ⇒ `WAIT_TIMEOUT`（轮询，不是阻塞）；
//   · 没有消息且 `0 < dwMilliseconds < INFINITE` ⇒ 推进**虚拟时钟** dwMilliseconds 后 `WAIT_TIMEOUT`
//     （headless 没有事件源；确定性虚拟时间，照 `WaitMessage` 的口径）；
//   · `dwMilliseconds == INFINITE` ⇒ **硬错误**（headless 下永远不会有输入 ⇒ 真等就是死锁；
//     照 `GetMessageA` 的同一口径响亮失败，⛔ 不假装等到）。
//   · `nCount > 0` 且需要等对象（没有消息、timeout != 0）⇒ **硬错误**（SCOPE §1「等待（未 signaled）」）：
//     本线没有"对象 signaled 状态"可查，⛔ 不凭空返回 WAIT_OBJECT_0 冒充对象已 signaled。
void shimMsgWaitForMultipleObjects(X86& cpu, ShimCall& call) {
    WindowManager* wm = requireWindows(cpu, "MsgWaitForMultipleObjects");
    const std::uint32_t n_count = call.u32(0);
    const std::uint32_t handles = call.u32(1);
    const std::uint32_t wait_all = call.u32(2);
    const std::uint32_t timeout = call.u32(3);
    const std::uint32_t wake_mask = call.u32(4);
    if (n_count > 0 && (!handles || !cpu.mem->committed(handles, n_count * 4))) {
        shimAbort("user32", "MsgWaitForMultipleObjects",
                  "pHandles=0x%08X 读不出 %u 个句柄（调用点 EA=0x%08X）", handles, n_count, call.ret_ea);
    }
    wm->pumpTimers(cpu);
    // 只看队首是不对的：可能有"不匹配掩码"的消息挡在前面 —— 逐条扫（不取走）。
    // ★ 本线的队列是单个 FIFO；Win32 的"按掩码分类的多个队列"是建模简化，日志里明说。
    // ★ `queueSize() + 1`：`WM_QUIT` 是队列末尾的一条虚拟消息（见 WindowManager::peekAt）。
    // ★ `dwWakeMask == 0` ⇒ 只等对象、不等消息（Win32 语义）⇒ `queueClass & 0 == 0` 自然不匹配。
    const std::size_t n = wm->queueSize();
    for (std::size_t i = 0; i <= n; ++i) {
        WinMessage m;
        if (!wm->peekAt(i, &m)) break;
        if ((queueClass(m) & wake_mask) != 0) {
            shimLog("user32", "MsgWaitForMultipleObjects",
                    "nCount=%u dwMilliseconds=%u dwWakeMask=0x%04X ⇒ WAIT_OBJECT_0+%u（队首第 %zu 条消息 "
                    "msg=0x%04X 匹配；不取走）",
                    n_count, timeout, wake_mask, n_count, i, m.message);
            call.result = kWaitObject0 + n_count;
            return;
        }
    }
    if (timeout == 0) {
        shimLog("user32", "MsgWaitForMultipleObjects",
                "nCount=%u dwMilliseconds=0 dwWakeMask=0x%04X ⇒ WAIT_TIMEOUT（轮询，无消息）", n_count,
                wake_mask);
        call.result = kWaitTimeout;
        return;
    }
    if (n_count > 0) {
        shimAbort("user32", "MsgWaitForMultipleObjects",
                  "需要等待 %u 个对象（pHandles=0x%08X, fWaitAll=%u）且队列里没有匹配消息："
                  "本线没有对象 signaled 状态可查 ⇒ 不阻塞（SCOPE §1「等待（未 signaled）」）",
                  n_count, handles, wait_all);
    }
    if (timeout == 0xFFFFFFFFu) {
        shimAbort("user32", "MsgWaitForMultipleObjects",
                  "nCount=0 dwMilliseconds=INFINITE 且没有消息：headless 没有事件源 ⇒ 真等就是死锁"
                  "（照 GetMessageA 的口径响亮失败；P3 接 SDL 事件源后这条才会变成「等待」）");
    }
    // 有限超时：推进虚拟时钟（确定性），然后如实返回 WAIT_TIMEOUT。
    if (cpu.clock) cpu.clock->advance100ns(static_cast<std::uint64_t>(timeout) * 10000ull);
    shimLog("user32", "MsgWaitForMultipleObjects",
            "nCount=0 dwMilliseconds=%u dwWakeMask=0x%04X ⇒ WAIT_TIMEOUT（无事件源；虚拟时钟前进 %u ms）",
            timeout, wake_mask, timeout);
    call.result = kWaitTimeout;
}

// ---- UnhookWindowsHookEx(HHOOK) → BOOL ----
// ★ 本线**没有** `SetWindowsHookEx` 导入（`.lst` 里没有调用点）⇒ 不存在可解的钩子。
//   照 SCOPE §3 的 no-op 口径：返回合理值 TRUE + 日志（⛔ 不静默）。
void shimUnhookWindowsHookEx(X86& cpu, ShimCall& call) {
    (void)cpu;
    shimLog("user32", "UnhookWindowsHookEx",
            "hhk=0x%08X ⇒ TRUE（本线没有 SetWindowsHookEx 导入 ⇒ 不存在可解的钩子；no-op + 日志）",
            call.u32(0));
    call.result = 1;
}

}  // namespace

void registerUser32InputShims(ShimRegistry& s) {
    // 顺序：光标 → 光标文件 → 显示器 → 系统参数 → 消息等待 → 钩子（trace 的 id 顺序 = 本顺序）。
    s.add("user32", "SetCursor", Conv::Stdcall, 1, &shimSetCursor);
    s.add("user32", "GetCursorPos", Conv::Stdcall, 1, &shimGetCursorPos);
    s.add("user32", "SetCursorPos", Conv::Stdcall, 2, &shimSetCursorPos);
    s.add("user32", "ShowCursor", Conv::Stdcall, 1, &shimShowCursor);
    s.add("user32", "LoadCursorFromFileA", Conv::Stdcall, 1, &shimLoadCursorFromFileA);
    s.add("user32", "MonitorFromWindow", Conv::Stdcall, 2, &shimMonitorFromWindow);
    s.add("user32", "GetMonitorInfoA", Conv::Stdcall, 2, &shimGetMonitorInfoA);
    s.add("user32", "SystemParametersInfoA", Conv::Stdcall, 4, &shimSystemParametersInfoA);
    s.add("user32", "MsgWaitForMultipleObjects", Conv::Stdcall, 5, &shimMsgWaitForMultipleObjects);
    s.add("user32", "UnhookWindowsHookEx", Conv::Stdcall, 1, &shimUnhookWindowsHookEx);
}

}  // namespace lift
