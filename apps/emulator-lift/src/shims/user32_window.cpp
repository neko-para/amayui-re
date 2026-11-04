// emulator-lift · user32 窗口 / 消息泵（P0；headless）
//
// ★ 用户口径（2026-11-04）：先 headless 窗口，但**消息泵语义完整**。
//   `CreateWindowExA` 不建 SDL 窗口；窗口句柄 / 客户区 / 消息队列由 `runtime/host_window` 管。
// ★ 回调：WndProc 是 guest 代码，shim 通过 `GuestCallStack` 交还控制权（见 runtime/guest_call.h）。
//   `SendMessage` / `DispatchMessage` / `UpdateWindow` / `DestroyWindow` 都是“shim → guest → shim”。
// ★ P0 只做启动到 DDraw 之前需要的子集；region / 输入 / SDL 后端在 M2b。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/guest_call.h"
#include "runtime/host_window.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kWmNull = 0x0000;
constexpr std::uint32_t kWmCreate = 0x0001;
constexpr std::uint32_t kWmDestroy = 0x0002;
constexpr std::uint32_t kWmMove = 0x0003;
constexpr std::uint32_t kWmSize = 0x0005;
constexpr std::uint32_t kWmActivate = 0x0006;
constexpr std::uint32_t kWmSetFocus = 0x0007;
constexpr std::uint32_t kWmPaint = 0x000F;
constexpr std::uint32_t kWmClose = 0x0010;
constexpr std::uint32_t kWmQuit = 0x0012;
constexpr std::uint32_t kWmEraseBkgnd = 0x0014;
constexpr std::uint32_t kWmShowWindow = 0x0018;
constexpr std::uint32_t kWmSetCursor = 0x0020;
constexpr std::uint32_t kWmGetMinMaxInfo = 0x0024;
constexpr std::uint32_t kWmNcCreate = 0x0081;
constexpr std::uint32_t kWmTimer = 0x0113;
constexpr std::uint32_t kWmKeyDown = 0x0100;
constexpr std::uint32_t kWmSysKeyDown = 0x0104;

constexpr std::uint32_t kPmNoRemove = 0x0000u;
constexpr std::uint32_t kPmRemove = 0x0001u;

constexpr std::int32_t kGwlWndProc = -4;
constexpr std::int32_t kGwlStyle = -16;
constexpr std::int32_t kGwlExStyle = -20;
constexpr std::int32_t kGwlUserData = -21;

// WNDCLASSEXA (x86) 布局
constexpr std::uint32_t kClassStyle = 4;
constexpr std::uint32_t kClassProc = 8;
constexpr std::uint32_t kClassName = 40;

std::uint32_t readRect(X86& cpu, std::uint32_t ea, std::int32_t* l, std::int32_t* t, std::int32_t* r,
                       std::int32_t* b) {
    if (!ea || !cpu.mem->committed(ea, 16)) return 0;
    *l = static_cast<std::int32_t>(cpu.mem->rd32(ea));
    *t = static_cast<std::int32_t>(cpu.mem->rd32(ea + 4));
    *r = static_cast<std::int32_t>(cpu.mem->rd32(ea + 8));
    *b = static_cast<std::int32_t>(cpu.mem->rd32(ea + 12));
    return 1;
}

void writeRect(X86& cpu, std::uint32_t ea, std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
    if (!ea) return;
    cpu.mem->wr32(ea, static_cast<std::uint32_t>(l));
    cpu.mem->wr32(ea + 4, static_cast<std::uint32_t>(t));
    cpu.mem->wr32(ea + 8, static_cast<std::uint32_t>(r));
    cpu.mem->wr32(ea + 12, static_cast<std::uint32_t>(b));
}

HostWindow* requireWindow(X86& cpu, std::uint32_t hwnd, const char* api) {
    if (!cpu.windows) shimAbort("user32", api, "cpu.windows 为空（Machine 没装配窗口管理器）");
    HostWindow* w = cpu.windows->find(hwnd);
    if (!w) shimAbort("user32", api, "窗口句柄 0x%08X 不存在", hwnd);
    return w;
}

std::uint32_t defWindowProcHost(std::uint32_t hwnd, std::uint32_t msg, std::uint32_t wparam,
                                std::uint32_t lparam) {
    (void)hwnd;
    (void)wparam;
    (void)lparam;
    switch (msg) {
        case kWmNcCreate: return 1;
        case kWmEraseBkgnd: return 1;
        case kWmSetCursor: return 1;
        case kWmGetMinMaxInfo: return 0;
        case kWmClose: return 0;
        case kWmPaint: return 0;
        default: return 0;
    }
}

// 启动一次 WndProc 调用。`cleanup` = 发起 shim 的返回清理字节（4 + argc*4）。
bool startWndProc(X86& cpu, std::uint32_t hwnd, std::uint32_t msg, std::uint32_t wparam,
                  std::uint32_t lparam, std::uint32_t cleanup, const char* api, GuestContinuation cont,
                  std::string* err) {
    HostWindow* w = cpu.windows ? cpu.windows->find(hwnd) : nullptr;
    if (!w || w->proc == 0) {
        if (err) *err = "窗口没有 guest WndProc";
        return false;
    }
    if (!cpu.calls) {
        if (err) *err = "cpu.calls 为空";
        return false;
    }
    std::vector<std::uint32_t> args{hwnd, msg, wparam, lparam};
    return cpu.calls->start(cpu, w->proc, args, cleanup, api ? api : "WndProc", std::move(cont), err);
}

void finishShim(ShimCall& call, bool started) {
    if (started) call.take_control = true;
}

// ---- 窗口类 / 创建 / 销毁 ----
void shimRegisterClassExA(X86& cpu, ShimCall& call) {
    const std::uint32_t cls = call.u32(0);
    if (!cls || !cpu.mem->committed(cls, 0x30)) {
        cpu.shim.last_error = 87;
        call.result = 0;
        shimLog("user32", "RegisterClassExA", "WNDCLASSEXA 指针无效 ⇒ 0 + 87");
        return;
    }
    const std::uint32_t style = cpu.mem->rd32(cls + kClassStyle);
    const std::uint32_t proc = cpu.mem->rd32(cls + kClassProc);
    const std::string name = cpu.mem->readCString(cpu.mem->rd32(cls + kClassName));
    const std::uint32_t atom = cpu.windows->registerClass(name, proc, style);
    shimLog("user32", "RegisterClassExA", "class=\"%s\" proc=%08X ⇒ atom=%08X（headless）", name.c_str(), proc,
            atom);
    call.result = atom;
}

void shimCreateWindowExA(X86& cpu, ShimCall& call) {
    const std::uint32_t exstyle = call.u32(0);
    const std::string cls = call.str(1);
    const std::string title = call.str(2);
    const std::uint32_t style = call.u32(3);
    const int x = call.i32(4);
    const int y = call.i32(5);
    const int w = call.i32(6);
    const int h = call.i32(7);
    const std::uint32_t menu = call.u32(9);
    const std::uint32_t param = call.u32(11);
    const std::uint32_t hwnd = cpu.windows->createWindow(cls, style, exstyle, x, y, w, h, menu);
    HostWindow* win = cpu.windows->find(hwnd);
    if (win) win->userdata = param;
    shimLog("user32", "CreateWindowExA",
            "class=\"%s\" style=%08X %dx%d ⇒ hwnd=%08X（headless；客户区 %dx%d；title=\"%s\"）", cls.c_str(), style,
            w, h, hwnd, win ? win->client_w : 0, win ? win->client_h : 0, title.c_str());
    if (win && win->proc != 0) {
        // 真实 Win32：CreateWindowEx 同步发 WM_NCCREATE / WM_CREATE。P0 只发 WM_CREATE
        // （WM_NCCREATE 的 CREATESTRUCT 尚未建模；本作 WndProc 对两者都走 DefWindowProc）。
        std::string err;
        const bool started = startWndProc(
            cpu, hwnd, kWmCreate, 0, 0, 4u + call.stdcall_bytes, "CreateWindowExA/WM_CREATE",
            [hwnd](X86& c, std::uint32_t result, std::uint32_t* out) {
                if (result == 0xFFFFFFFFu) {
                    if (c.windows) c.windows->destroyWindow(hwnd);
                    *out = 0;
                } else {
                    *out = hwnd;
                }
                return true;
            },
            &err);
        if (!started) shimAbort("user32", "CreateWindowExA", "启动 WM_CREATE 失败：%s", err.c_str());
        finishShim(call, true);
        return;
    }
    call.result = hwnd;
}

void shimDestroyWindow(X86& cpu, ShimCall& call) {
    const std::uint32_t hwnd = call.u32(0);
    HostWindow* w = requireWindow(cpu, hwnd, "DestroyWindow");
    const bool has_proc = w->proc != 0;
    if (has_proc) {
        std::string err;
        const bool started = startWndProc(
            cpu, hwnd, kWmDestroy, 0, 0, 4u + call.stdcall_bytes, "DestroyWindow/WM_DESTROY",
            [hwnd, &cpu](X86&, std::uint32_t, std::uint32_t* out) {
                if (cpu.windows) cpu.windows->destroyWindow(hwnd);
                *out = 1;
                return true;
            },
            &err);
        if (!started) shimAbort("user32", "DestroyWindow", "启动 WM_DESTROY 失败：%s", err.c_str());
        finishShim(call, true);
        return;
    }
    cpu.windows->destroyWindow(hwnd);
    call.result = 1;
}

void shimShowWindow(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "ShowWindow");
    const std::uint32_t cmd = call.u32(1);
    const bool old = w->visible;
    w->visible = cmd != 0;
    shimLog("user32", "ShowWindow", "hwnd=%08X cmd=%u ⇒ visible=%d（headless）", w->hwnd, cmd, w->visible ? 1 : 0);
    if (w->proc != 0 && cmd != 0) {
        // 真实 Win32：ShowWindow 会发 WM_SIZE（本 P0 不发 WM_SHOWWINDOW / WM_MOVE）。
        const std::uint32_t lparam = (static_cast<std::uint32_t>(static_cast<std::uint16_t>(w->client_h)) << 16) |
                                     static_cast<std::uint16_t>(w->client_w);
        std::string err;
        const bool started = startWndProc(
            cpu, w->hwnd, kWmSize, 0, lparam, 4u + call.stdcall_bytes, "ShowWindow/WM_SIZE",
            [old](X86&, std::uint32_t, std::uint32_t* out) {
                *out = old ? 1u : 0u;
                return true;
            },
            &err);
        if (!started) shimAbort("user32", "ShowWindow", "启动 WM_SIZE 失败：%s", err.c_str());
        finishShim(call, true);
        return;
    }
    call.result = old ? 1 : 0;
}

void shimUpdateWindow(X86& cpu, ShimCall& call) {
    const std::uint32_t hwnd = call.u32(0);
    HostWindow* w = requireWindow(cpu, hwnd, "UpdateWindow");
    if (!w->dirty) {
        call.result = 1;
        return;
    }
    if (w->proc == 0) {
        w->dirty = false;
        call.result = 1;
        return;
    }
    std::string err;
    const bool started = startWndProc(
        cpu, hwnd, kWmPaint, 0, 0, 4u + call.stdcall_bytes, "UpdateWindow/WM_PAINT",
        [hwnd, &cpu](X86&, std::uint32_t, std::uint32_t* out) {
            if (HostWindow* ww = cpu.windows->find(hwnd)) ww->dirty = false;
            *out = 1;
            return true;
        },
        &err);
    if (!started) shimAbort("user32", "UpdateWindow", "启动 WM_PAINT 失败：%s", err.c_str());
    finishShim(call, true);
}

void shimDefWindowProcA(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t msg = call.u32(1);
    call.result = defWindowProcHost(hwnd, msg, call.u32(2), call.u32(3));
}

// ---- 窗口状态 ----
void shimGetClientRect(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "GetClientRect");
    writeRect(cpu, call.u32(1), 0, 0, w->client_w, w->client_h);
    call.result = 1;
}

void shimGetWindowRect(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "GetWindowRect");
    writeRect(cpu, call.u32(1), w->x, w->y, w->x + w->outer_w, w->y + w->outer_h);
    call.result = 1;
}

void shimClientToScreen(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "ClientToScreen");
    std::int32_t l, t, r, b;
    if (!readRect(cpu, call.u32(1), &l, &t, &r, &b)) {
        call.result = 0;
        return;
    }
    writeRect(cpu, call.u32(1), l + w->x, t + w->y, r + w->x, b + w->y);
    call.result = 1;
}

void shimScreenToClient(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "ScreenToClient");
    std::int32_t l, t, r, b;
    if (!readRect(cpu, call.u32(1), &l, &t, &r, &b)) {
        call.result = 0;
        return;
    }
    writeRect(cpu, call.u32(1), l - w->x, t - w->y, r - w->x, b - w->y);
    call.result = 1;
}

void shimSetWindowPos(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "SetWindowPos");
    const std::uint32_t flags = call.u32(6);
    if (!(flags & 0x0002u)) {  // SWP_NOMOVE
        w->x = call.i32(1);
        w->y = call.i32(2);
    }
    if (!(flags & 0x0001u)) {  // SWP_NOSIZE
        w->outer_w = call.i32(3);
        w->outer_h = call.i32(4);
        WindowManager::clientSizeFor(w->style, w->outer_w, w->outer_h, &w->client_w, &w->client_h);
    }
    w->dirty = true;
    shimLog("user32", "SetWindowPos", "hwnd=%08X pos=%d,%d size=%dx%d flags=%08X（headless；不主动发 WM_SIZE）",
            w->hwnd, w->x, w->y, w->outer_w, w->outer_h, flags);
    call.result = 1;
}

void shimGetWindowLongA(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "GetWindowLongA");
    const std::int32_t idx = call.i32(1);
    switch (idx) {
        case kGwlStyle: call.result = w->style; break;
        case kGwlExStyle: call.result = w->exstyle; break;
        case kGwlUserData: call.result = w->userdata; break;
        case kGwlWndProc: call.result = w->proc; break;
        default:
            shimLog("user32", "GetWindowLongA", "nIndex=%d 未建模 ⇒ 0", idx);
            call.result = 0;
            break;
    }
}

void shimSetWindowLongA(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "SetWindowLongA");
    const std::int32_t idx = call.i32(1);
    const std::uint32_t value = call.u32(2);
    std::uint32_t prev = 0;
    switch (idx) {
        case kGwlStyle: prev = w->style; w->style = value; break;
        case kGwlExStyle: prev = w->exstyle; w->exstyle = value; break;
        case kGwlUserData: prev = w->userdata; w->userdata = value; break;
        case kGwlWndProc: prev = w->proc; w->proc = value; break;
        default:
            shimLog("user32", "SetWindowLongA", "nIndex=%d 未建模 ⇒ 返回值 0", idx);
            break;
    }
    WindowManager::clientSizeFor(w->style, w->outer_w, w->outer_h, &w->client_w, &w->client_h);
    call.result = prev;
}

void shimGetWindowPlacement(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "GetWindowPlacement");
    const std::uint32_t ea = call.u32(1);
    if (!ea || !cpu.mem->committed(ea, 44)) {
        call.result = 0;
        return;
    }
    cpu.mem->wr32(ea, 44);
    cpu.mem->wr32(ea + 4, 0);
    cpu.mem->wr32(ea + 8, 1);  // SW_SHOWNORMAL
    cpu.mem->wr32(ea + 12, static_cast<std::uint32_t>(-1));
    cpu.mem->wr32(ea + 16, static_cast<std::uint32_t>(-1));
    cpu.mem->wr32(ea + 20, static_cast<std::uint32_t>(-1));
    cpu.mem->wr32(ea + 24, static_cast<std::uint32_t>(-1));
    writeRect(cpu, ea + 28, w->x, w->y, w->x + w->outer_w, w->y + w->outer_h);
    call.result = 1;
}

void shimInvalidateRect(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "InvalidateRect");
    w->dirty = call.u32(1) == 0 ? true : true;
    call.result = 1;
}

void shimValidateRect(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "ValidateRect");
    w->dirty = false;
    call.result = 1;
}

void shimGetUpdateRgn(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "GetUpdateRgn");
    (void)w;
    // NULLREGION = 1；region 本体在 M2b（P4）。这里只让 WM_PAINT 路径不炸。
    call.result = 1;
}

void shimValidateRgn(X86& cpu, ShimCall& call) {
    HostWindow* w = requireWindow(cpu, call.u32(0), "ValidateRgn");
    w->dirty = false;
    call.result = 1;
}

void shimGetSystemMetrics(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::int32_t idx = call.i32(0);
    switch (idx) {
        case 0: call.result = WindowManager::kScreenW; break;   // SM_CXSCREEN
        case 1: call.result = WindowManager::kScreenH; break;   // SM_CYSCREEN
        case 2: call.result = 17; break;                        // SM_CXVSCROLL
        case 3: call.result = 17; break;                        // SM_CYHSCROLL
        case 4: call.result = WindowManager::kCaption; break;   // SM_CYCAPTION
        case 5: call.result = 1; break;                         // SM_CXBORDER
        case 6: call.result = 1; break;                         // SM_CYBORDER
        case 7: call.result = WindowManager::kFrameX; break;    // SM_CXFIXEDFRAME
        case 8: call.result = WindowManager::kFrameY; break;    // SM_CYFIXEDFRAME
        case 9: call.result = 17; break;                        // SM_CYVTHUMB
        case 10: call.result = 17; break;                       // SM_CXHTHUMB
        case 11: call.result = 32; break;                       // SM_CXICON
        case 12: call.result = 32; break;                       // SM_CYICON
        case 13: call.result = 32; break;                       // SM_CXCURSOR
        case 14: call.result = 32; break;                       // SM_CYCURSOR
        case 15: call.result = WindowManager::kMenu; break;     // SM_CYMENU
        // ★ SM_SWAPBUTTON(23)：引擎真的问过（`.lst` 0x0040876B 附近 / boot 实测）。
        //   0 = 左右键没交换（真机默认值；headless 没有"用户设置"⇒ 用默认）。
        case 23: call.result = 0; break;
        case 28: call.result = 112; break;                      // SM_CXMIN
        case 29: call.result = 27; break;                       // SM_CYMIN
        default:
            shimLog("user32", "GetSystemMetrics", "nIndex=%d 未建模 ⇒ 0", idx);
            call.result = 0;
            break;
    }
}

void shimGetAsyncKeyState(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call.u32(0);
    call.result = 0;  // headless：没有键按下（P3 接 SDL 影子状态）
}

void shimGetForegroundWindow(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = 0;  // P0 没有“前台”概念；engine 只拿它做比较
}

void shimGetParent(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;
}

void shimIsIconic(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;
}

void shimSetRect(X86& cpu, ShimCall& call) {
    writeRect(cpu, call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4));
    call.result = 1;
}

void shimSetRectEmpty(X86& cpu, ShimCall& call) {
    writeRect(cpu, call.u32(0), 0, 0, 0, 0);
    call.result = 1;
}

// ---- 消息队列 ----
void writeMsg(X86& cpu, std::uint32_t ea, const WinMessage& m) {
    if (!ea) return;
    cpu.mem->wr32(ea + 0, m.hwnd);
    cpu.mem->wr32(ea + 4, m.message);
    cpu.mem->wr32(ea + 8, m.wparam);
    cpu.mem->wr32(ea + 12, m.lparam);
    cpu.mem->wr32(ea + 16, m.time);
    cpu.mem->wr32(ea + 20, static_cast<std::uint32_t>(m.pt_x));
    cpu.mem->wr32(ea + 24, static_cast<std::uint32_t>(m.pt_y));
}

bool readMsg(X86& cpu, std::uint32_t ea, WinMessage* m) {
    if (!ea || !cpu.mem->committed(ea, 28)) return false;
    m->hwnd = cpu.mem->rd32(ea + 0);
    m->message = cpu.mem->rd32(ea + 4);
    m->wparam = cpu.mem->rd32(ea + 8);
    m->lparam = cpu.mem->rd32(ea + 12);
    m->time = cpu.mem->rd32(ea + 16);
    m->pt_x = static_cast<std::int32_t>(cpu.mem->rd32(ea + 20));
    m->pt_y = static_cast<std::int32_t>(cpu.mem->rd32(ea + 24));
    return true;
}

void shimPeekMessageA(X86& cpu, ShimCall& call) {
    const std::uint32_t msg_ea = call.u32(0);
    const std::uint32_t hwnd_filter = call.u32(1);
    const std::uint32_t min_filter = call.u32(2);
    const std::uint32_t max_filter = call.u32(3);
    const std::uint32_t remove = call.u32(4);
    cpu.windows->pumpTimers(cpu);
    WinMessage m;
    for (;;) {
        if (!cpu.windows->peek(&m, false)) {
            call.result = 0;
            return;
        }
        if (hwnd_filter && m.hwnd != hwnd_filter) {
            // 简化：本作只有主窗口；过滤不匹配时把它留在队列里会死循环 ⇒ 直接取走（P0 只支持单窗口）
            cpu.windows->peek(&m, true);
            continue;
        }
        if (min_filter || max_filter) {
            if ((min_filter && m.message < min_filter) || (max_filter && m.message > max_filter)) {
                cpu.windows->peek(&m, true);
                continue;
            }
        }
        break;
    }
    writeMsg(cpu, msg_ea, m);
    if (remove & kPmRemove) cpu.windows->peek(&m, true);
    call.result = 1;
}

void shimGetMessageA(X86& cpu, ShimCall& call) {
    WinMessage m;
    if (!cpu.windows->get(cpu, &m)) {
        shimAbort("user32", "GetMessageA",
                  "headless 下没有消息可等（P0 只由 Peek 驱动；P3 接 SDL 后端）；"
                  "若要阻塞等待，需要在后端补事件源");
    }
    if (m.message == kWmQuit) {
        call.result = 0;
        return;
    }
    writeMsg(cpu, call.u32(0), m);
    call.result = 1;
}

void shimTranslateMessage(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 1;  // P0 不生成 WM_CHAR（P3 接键盘后补）
}

void shimDispatchMessageA(X86& cpu, ShimCall& call) {
    const std::uint32_t msg_ea = call.u32(0);
    WinMessage m;
    if (!readMsg(cpu, msg_ea, &m)) {
        call.result = 0;
        return;
    }
    HostWindow* w = cpu.windows->find(m.hwnd);
    if (!w || w->proc == 0) {
        call.result = defWindowProcHost(m.hwnd, m.message, m.wparam, m.lparam);
        return;
    }
    std::string err;
    const bool started = startWndProc(
        cpu, m.hwnd, m.message, m.wparam, m.lparam, 4u + call.stdcall_bytes, "DispatchMessageA/WndProc",
        [](X86&, std::uint32_t r, std::uint32_t* out) {
            *out = r;
            return true;
        },
        &err);
    if (!started) shimAbort("user32", "DispatchMessageA", "启动 WndProc 失败：%s", err.c_str());
    finishShim(call, true);
}

void shimPostMessageA(X86& cpu, ShimCall& call) {
    WinMessage m;
    m.hwnd = call.u32(0);
    m.message = call.u32(1);
    m.wparam = call.u32(2);
    m.lparam = call.u32(3);
    m.time = cpu.clock ? cpu.clock->nowMs() : 0;
    if (!cpu.windows->find(m.hwnd) && m.hwnd != 0) {
        shimLog("user32", "PostMessageA", "hwnd=%08X 不存在 ⇒ 仍入队（P0 不校验跨线程/窗口）", m.hwnd);
    }
    cpu.windows->post(m);
    call.result = 1;
}

void shimSendMessageA(X86& cpu, ShimCall& call) {
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t msg = call.u32(1);
    const std::uint32_t wp = call.u32(2);
    const std::uint32_t lp = call.u32(3);
    HostWindow* w = cpu.windows->find(hwnd);
    if (!w || w->proc == 0) {
        call.result = defWindowProcHost(hwnd, msg, wp, lp);
        return;
    }
    std::string err;
    const bool started = startWndProc(
        cpu, hwnd, msg, wp, lp, 4u + call.stdcall_bytes, "SendMessageA/WndProc",
        [](X86&, std::uint32_t r, std::uint32_t* out) {
            *out = r;
            return true;
        },
        &err);
    if (!started) shimAbort("user32", "SendMessageA", "启动 WndProc 失败：%s", err.c_str());
    finishShim(call, true);
}

void shimPostQuitMessage(X86& cpu, ShimCall& call) {
    const std::uint32_t code = call.u32(0);
    cpu.windows->postQuit(code);
    shimLog("user32", "PostQuitMessage", "exit_code=%u", code);
    call.result = 0;
}

void shimWaitMessage(X86& cpu, ShimCall& call) {
    cpu.windows->pumpTimers(cpu);
    if (cpu.clock) cpu.clock->advance100ns(10000ull);  // 1 ms 虚拟时间；P0 不阻塞宿主
    call.result = 1;
}

void shimIsDialogMessageA(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;  // 本作没有 Win32 对话框（AGERC 是 stub）
}

void shimGetQueueStatus(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call.u32(0);
    call.result = 0;  // P0：保守返回“没有队列状态”；引擎只在少数路径读
}

void shimSetTimer(X86& cpu, ShimCall& call) {
    const std::uint32_t id = cpu.windows->setTimer(cpu, call.u32(0), call.u32(1), call.u32(2));
    shimLog("user32", "SetTimer", "hwnd=%08X id=%u elapse=%u ⇒ id=%u", call.u32(0), call.u32(1), call.u32(2), id);
    call.result = id;
}

void shimKillTimer(X86& cpu, ShimCall& call) {
    const bool ok = cpu.windows->killTimer(call.u32(0), call.u32(1));
    call.result = ok ? 1 : 0;
}

void shimPostThreadMessageA(X86& cpu, ShimCall& call) {
    (void)cpu;
    shimLog("user32", "PostThreadMessageA", "线程消息未建模（P0 单线程）⇒ TRUE");
    call.result = 1;
}

void shimRegisterWindowMessageA(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::string name = call.str(0);
    // 确定性：0xC000 + 简单 FNV-1a 低 12 位（只用于本进程内的注册消息）
    std::uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= c;
        h *= 16777619u;
    }
    call.result = 0xC000u + (h & 0x0FFFu);
}


}  // namespace

void registerUser32WindowShims(ShimRegistry& s) {
    s.add("user32", "RegisterClassExA", Conv::Stdcall, 1, &shimRegisterClassExA);
    s.add("user32", "CreateWindowExA", Conv::Stdcall, 12, &shimCreateWindowExA);
    s.add("user32", "DestroyWindow", Conv::Stdcall, 1, &shimDestroyWindow);
    s.add("user32", "ShowWindow", Conv::Stdcall, 2, &shimShowWindow);
    s.add("user32", "UpdateWindow", Conv::Stdcall, 1, &shimUpdateWindow);
    s.add("user32", "DefWindowProcA", Conv::Stdcall, 4, &shimDefWindowProcA);
    s.add("user32", "GetClientRect", Conv::Stdcall, 2, &shimGetClientRect);
    s.add("user32", "GetWindowRect", Conv::Stdcall, 2, &shimGetWindowRect);
    s.add("user32", "ClientToScreen", Conv::Stdcall, 2, &shimClientToScreen);
    s.add("user32", "ScreenToClient", Conv::Stdcall, 2, &shimScreenToClient);
    s.add("user32", "SetWindowPos", Conv::Stdcall, 7, &shimSetWindowPos);
    s.add("user32", "GetWindowLongA", Conv::Stdcall, 2, &shimGetWindowLongA);
    s.add("user32", "SetWindowLongA", Conv::Stdcall, 3, &shimSetWindowLongA);
    s.add("user32", "GetWindowPlacement", Conv::Stdcall, 2, &shimGetWindowPlacement);
    s.add("user32", "InvalidateRect", Conv::Stdcall, 3, &shimInvalidateRect);
    s.add("user32", "ValidateRect", Conv::Stdcall, 2, &shimValidateRect);
    s.add("user32", "GetUpdateRgn", Conv::Stdcall, 3, &shimGetUpdateRgn);
    s.add("user32", "ValidateRgn", Conv::Stdcall, 2, &shimValidateRgn);
    s.add("user32", "GetSystemMetrics", Conv::Stdcall, 1, &shimGetSystemMetrics);
    s.add("user32", "GetAsyncKeyState", Conv::Stdcall, 1, &shimGetAsyncKeyState);
    s.add("user32", "GetForegroundWindow", Conv::Stdcall, 0, &shimGetForegroundWindow);
    s.add("user32", "GetParent", Conv::Stdcall, 1, &shimGetParent);
    s.add("user32", "IsIconic", Conv::Stdcall, 1, &shimIsIconic);
    s.add("user32", "SetRect", Conv::Stdcall, 5, &shimSetRect);
    s.add("user32", "SetRectEmpty", Conv::Stdcall, 1, &shimSetRectEmpty);

    s.add("user32", "PeekMessageA", Conv::Stdcall, 5, &shimPeekMessageA);
    s.add("user32", "GetMessageA", Conv::Stdcall, 4, &shimGetMessageA);
    s.add("user32", "TranslateMessage", Conv::Stdcall, 1, &shimTranslateMessage);
    s.add("user32", "DispatchMessageA", Conv::Stdcall, 1, &shimDispatchMessageA);
    s.add("user32", "PostMessageA", Conv::Stdcall, 4, &shimPostMessageA);
    s.add("user32", "SendMessageA", Conv::Stdcall, 4, &shimSendMessageA);
    s.add("user32", "PostQuitMessage", Conv::Stdcall, 1, &shimPostQuitMessage);
    s.add("user32", "WaitMessage", Conv::Stdcall, 0, &shimWaitMessage);
    s.add("user32", "IsDialogMessageA", Conv::Stdcall, 2, &shimIsDialogMessageA);
    s.add("user32", "GetQueueStatus", Conv::Stdcall, 1, &shimGetQueueStatus);
    s.add("user32", "SetTimer", Conv::Stdcall, 4, &shimSetTimer);
    s.add("user32", "KillTimer", Conv::Stdcall, 2, &shimKillTimer);
    s.add("user32", "PostThreadMessageA", Conv::Stdcall, 4, &shimPostThreadMessageA);
    s.add("user32", "RegisterWindowMessageA", Conv::Stdcall, 1, &shimRegisterWindowMessageA);
}

}  // namespace lift
