// emulator-lift · user32 菜单 / 模态（SCOPE §1：整体 no-op）
//
// ★ 用户口径（2026-11-04）：菜单 / 模态按 **no-op** 处理，不是硬 trap。判据（SCOPE §1）：
//   · `LoadMenuA`    → 返回**非空假句柄**（guest 只把它当不透明令牌；真去解引用它没有意义）；
//   · `SetMenu`      → TRUE；
//   · `TrackPopupMenu` → 0（立即返回，不阻塞、不等待）；
//   · `MessageBoxA`  → IDOK(1)（"用户没改 / 直接继续"）。
// ★ 每一条都打一行 `[shim]`（SCOPE §3 第 3 条：⛔ 不许静默 no-op），并登记在
//   `data/lift/overrides.json`（`addStub` ⇒ `lift sync` 自动进清单）。
// ★ 它们**不属于** D6 七族（user32 窗口 / 消息族），所以不触发 `seven-family-no-stub`。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// 假菜单句柄：非 0、稳定、一眼能看出不是真 HANDLE 的值。
constexpr std::uint32_t kFakeMenu = 0x00010001u;

void shimLoadMenuA(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hInstance / lpMenuName（2 个栈槽；不解析资源）
    shimLog("user32", "LoadMenuA",
            "菜单整体 no-op ⇒ 返回非空假句柄 0x%08X（SCOPE §1；不解析菜单资源）", kFakeMenu);
    call.result = kFakeMenu;
}

void shimSetMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hWnd / hMenu
    shimLog("user32", "SetMenu", "菜单整体 no-op ⇒ 返回 TRUE（SCOPE §1；没有 Win32 菜单栏）");
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
    // 顺序 = 原来的 4 条 addUnimplemented 的顺序（trace 的 id 顺序不变）。
    s.addStub("user32", "LoadMenuA", Conv::Stdcall, 2, &shimLoadMenuA,
              "SCOPE §1 菜单整体 no-op：LoadMenuA 返回非空假句柄（不解析菜单资源）");
    s.addStub("user32", "SetMenu", Conv::Stdcall, 2, &shimSetMenu,
              "SCOPE §1 菜单整体 no-op：SetMenu 返回 TRUE（没有 Win32 菜单栏）");
    s.addStub("user32", "TrackPopupMenu", Conv::Stdcall, 7, &shimTrackPopupMenu,
              "SCOPE §1 菜单整体 no-op：TrackPopupMenu 立即返回 0（不阻塞）");
    s.addStub("user32", "MessageBoxA", Conv::Stdcall, 4, &shimMessageBoxA,
              "SCOPE §1 模态 no-op：MessageBoxA 返回 IDOK(1)（关闭确认 / 存档命名直接继续）");
}

}  // namespace lift
