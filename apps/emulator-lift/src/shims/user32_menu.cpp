// emulator-lift · user32 菜单 / 模态（SCOPE §1：整体 no-op）
//
// ★ 用户口径（2026-11-04）：菜单 / 模态按 **no-op** 处理，不是硬 trap。判据（SCOPE §1）：
//   · `LoadMenuA`    → 返回**非空假句柄**（guest 只把它当不透明令牌；真去解引用它没有意义）；
//   · `SetMenu`      → TRUE（顺手把窗口的菜单句柄记下来，`GetMenu` 才自洽）；
//   · `GetMenu`      → 窗口的菜单句柄（**没有菜单 = 0**，这是 Win32 的真值；⛔ 不凭空造句柄）；
//   · `GetSubMenu`   → 0（假菜单没有项 ⇒ Win32 就是 NULL；⛔ 不造第二层假句柄）；
//   · `GetSystemMenu`→ **非空假句柄**（有 WS_SYSMENU 的窗口在真机上总有系统菜单）；
//   · `RemoveMenu` / `DestroyMenu` / `DrawMenuBar` → TRUE；
//   · `TrackPopupMenu` → 0（立即返回，不阻塞、不等待）；
//   · `MessageBoxA`  → IDOK(1)（"用户没改 / 直接继续"）。
// ★ 每一条都打一行 `[shim]`（SCOPE §3 第 3 条：⛔ 不许静默 no-op），经 `addStub` 登记
//   `data/lift/overrides.json`（`lift sync` 自动进清单）。
// ★ 它们**不属于** D6 七族（user32 窗口 / 消息族），所以不触发 `seven-family-no-stub`。
//
// ★ 实测调用点（`.lst`，锚 EA；都是"有菜单才做"的分支）：
//   · `0x004B9E1B LoadMenuA` → `0x004B9E6F GetSubMenu(hMenu, 2)` → `TrackPopupMenu` →
//     `0x004B9E7D DestroyMenu`（右键菜单：整条链都是 no-op）；
//   · `0x004B98C2 GetSystemMenu(hwnd, FALSE)` → `0x004B98D0 RemoveMenu(hMenu, 0xF030, 0)`
//     （WM_SYSCOMMAND 里去掉"最大化"项）；
//   · `0x004BB0ED GetMenu(hwnd)` → `SetMenu`（经函数指针）→ `0x004BB0FE DrawMenuBar(hwnd)`
//     （`display:ScreenMode` 分支）；另有 `0x004B9A35 / 0x00414239 DrawMenuBar`。
#include <cstdint>

#include "runtime/host_window.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// 假菜单句柄：非 0、稳定、一眼能看出不是真 HANDLE 的值。
constexpr std::uint32_t kFakeMenu = 0x00010001u;
// 假系统菜单句柄（与 LoadMenuA 的假句柄区分开，日志里能看出用的是哪一条路径）。
constexpr std::uint32_t kFakeSystemMenu = 0x00010003u;

HostWindow* findWindow(X86& cpu, std::uint32_t hwnd) {
    return cpu.windows ? cpu.windows->find(hwnd) : nullptr;
}

void shimLoadMenuA(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hInstance / lpMenuName（2 个栈槽；不解析资源）
    shimLog("user32", "LoadMenuA",
            "菜单整体 no-op ⇒ 返回非空假句柄 0x%08X（SCOPE §1；不解析菜单资源）", kFakeMenu);
    call.result = kFakeMenu;
}

void shimSetMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd / hMenu
    // ★ SCOPE §1 判 TRUE；顺手记下窗口的菜单句柄（`GetMenu` 才自洽）。
    //   窗口句柄无效时**仍返回 TRUE**（SCOPE 的 stub 口径；`tests/shim_abi.cpp` 也钉了这一条）。
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (w) w->menu = call.u32(1);
    shimLog("user32", "SetMenu",
            "hwnd=0x%08X hMenu=0x%08X ⇒ TRUE（SCOPE §1 菜单整体 no-op；%s）", call.u32(0), call.u32(1),
            w ? "已记到窗口状态" : "窗口不存在 ⇒ 只返回 TRUE、不记状态");
    call.result = 1;
}

void shimGetMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd
    // ★ Win32：窗口没有菜单 ⇒ NULL（0）。⛔ 不要为了"非 0 假句柄"给一个不存在的菜单：
    //   引擎拿它去 SetMenu / DrawMenuBar，真值 0 才能让它走"没有菜单"的分支。
    HostWindow* w = findWindow(cpu, call.u32(0));
    const std::uint32_t menu = w ? w->menu : 0;
    shimLog("user32", "GetMenu",
            "hwnd=0x%08X ⇒ HMENU=0x%08X（%s；菜单整体 no-op，不建真菜单）", call.u32(0), menu,
            w ? (menu ? "窗口上记着的菜单句柄" : "窗口没有菜单 ⇒ Win32 的真值 0") : "窗口不存在 ⇒ 0");
    call.result = menu;
}

void shimGetSubMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / nPos
    // ★ 假菜单里没有任何项 ⇒ Win32 的真值就是 NULL(0)；随后 `TrackPopupMenu(0,…)` 立即返回 0。
    shimLog("user32", "GetSubMenu",
            "hMenu=0x%08X nPos=%d ⇒ NULL（假菜单没有子菜单项；SCOPE §1 菜单整体 no-op）", call.u32(0),
            call.i32(1));
    call.result = 0;
}

void shimGetSystemMenu(X86& cpu, ShimCall& call) {
    // 参数：hWnd / bRevert
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (!w) {
        shimLog("user32", "GetSystemMenu", "hwnd=0x%08X 不存在 ⇒ NULL（Win32 失败值）", call.u32(0));
        call.result = 0;
        return;
    }
    shimLog("user32", "GetSystemMenu",
            "hwnd=0x%08X bRevert=%d ⇒ 非空假 HMENU 0x%08X（系统菜单 no-op；⛔ 不建真菜单）", call.u32(0),
            call.i32(1), kFakeSystemMenu);
    call.result = kFakeSystemMenu;
}

void shimDestroyMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu
    shimLog("user32", "DestroyMenu", "hMenu=0x%08X ⇒ TRUE（假句柄，没有资源要释放）", call.u32(0));
    call.result = 1;
}

void shimRemoveMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / uPosition / uFlags
    shimLog("user32", "RemoveMenu",
            "hMenu=0x%08X uPosition=0x%04X uFlags=0x%04X ⇒ TRUE（假菜单没有项可删；SCOPE §1 no-op）",
            call.u32(0), call.u32(1), call.u32(2));
    call.result = 1;
}

void shimDrawMenuBar(X86& cpu, ShimCall& call) {
    // 参数：hWnd
    HostWindow* w = findWindow(cpu, call.u32(0));
    if (!w) {
        shimLog("user32", "DrawMenuBar", "hwnd=0x%08X 不存在 ⇒ FALSE（Win32 失败值）", call.u32(0));
        call.result = 0;
        return;
    }
    shimLog("user32", "DrawMenuBar", "hwnd=0x%08X ⇒ TRUE（没有菜单栏可重画；SCOPE §1 no-op）", call.u32(0));
    call.result = 1;
}

void shimTrackPopupMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hMenu / uFlags / x / y / nReserved / hWnd / prcRect（7 个栈槽）
    shimLog("user32", "TrackPopupMenu", "菜单整体 no-op ⇒ 立即返回 0（SCOPE §1；⛔ 不阻塞）");
    call.result = 0;
}

void shimMessageBoxA(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hWnd / lpText / lpCaption / uType
    shimLog("user32", "MessageBoxA", "模态 no-op ⇒ 返回 IDOK(1)（SCOPE §1；关闭确认 / 存档命名直接继续）");
    call.result = 1;
}

}  // namespace

void registerUser32MenuShims(ShimRegistry& s) {
    // 顺序 = 原来的 4 条 addUnimplemented 的顺序 + 其余菜单族（trace 的 id 顺序不变）。
    s.addStub("user32", "LoadMenuA", Conv::Stdcall, 2, &shimLoadMenuA,
              "SCOPE §1 菜单整体 no-op：LoadMenuA 返回非空假句柄（不解析菜单资源）");
    s.addStub("user32", "SetMenu", Conv::Stdcall, 2, &shimSetMenu,
              "SCOPE §1 菜单整体 no-op：SetMenu 返回 TRUE（没有 Win32 菜单栏）");
    s.addStub("user32", "TrackPopupMenu", Conv::Stdcall, 7, &shimTrackPopupMenu,
              "SCOPE §1 菜单整体 no-op：TrackPopupMenu 立即返回 0（不阻塞）");
    s.addStub("user32", "MessageBoxA", Conv::Stdcall, 4, &shimMessageBoxA,
              "SCOPE §1 模态 no-op：MessageBoxA 返回 IDOK(1)（关闭确认 / 存档命名直接继续）");
    // ★ 菜单族其余条目（`.lst` 里有调用点，见文件头）：同样按 SCOPE §1 的 no-op 口径。
    s.addStub("user32", "GetMenu", Conv::Stdcall, 1, &shimGetMenu,
              "SCOPE §1 菜单整体 no-op：GetMenu 返回窗口的菜单句柄（没有菜单 = 0，Win32 真值）");
    s.addStub("user32", "GetSubMenu", Conv::Stdcall, 2, &shimGetSubMenu,
              "SCOPE §1 菜单整体 no-op：GetSubMenu 返回 NULL（假菜单没有子菜单项）");
    s.addStub("user32", "GetSystemMenu", Conv::Stdcall, 2, &shimGetSystemMenu,
              "SCOPE §1 菜单整体 no-op：GetSystemMenu 返回非空假句柄（不建真系统菜单）");
    s.addStub("user32", "RemoveMenu", Conv::Stdcall, 3, &shimRemoveMenu,
              "SCOPE §1 菜单整体 no-op：RemoveMenu 返回 TRUE（假菜单没有项可删）");
    s.addStub("user32", "DestroyMenu", Conv::Stdcall, 1, &shimDestroyMenu,
              "SCOPE §1 菜单整体 no-op：DestroyMenu 返回 TRUE（假句柄，没有资源要释放）");
    s.addStub("user32", "DrawMenuBar", Conv::Stdcall, 1, &shimDrawMenuBar,
              "SCOPE §1 菜单整体 no-op：DrawMenuBar 返回 TRUE（没有菜单栏可重画）");
}

}  // namespace lift
