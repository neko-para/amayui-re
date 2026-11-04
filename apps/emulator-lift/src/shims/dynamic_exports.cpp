// emulator-lift · G1 的"**本 OS 没有这个导出**"表（P1）
//
// ★ 为什么需要它（plan §3 F2）：引擎有一批**动态解析点**（`LoadLibrary`/`GetProcAddress`），
//   在真机上拿到 NULL 时会走"这个 Windows 没有该 API"的分支。这些名字**不能**注册成 shim ——
//   一注册 `GetProcAddress` 就返回非 0，引擎会以为 OS 支持它，然后真的去调用 ⇒ 语义反了。
//   ⇒ 真机行为就是 **NULL + ERROR_PROC_NOT_FOUND(127)**；本表只负责让日志说清"是哪一种
//   没有"（"这个 OS 没这个导出" vs "模块里没有这个导出名"）。
//
// ★ 判据（`pnpm tools lift check` 之外的可再校验观察）：引擎的 4 个 `SHGetFolderPathA` 站点
//   与触摸/手势族都**不在导入表**里 —— 导入表里没有它们 ⇒ 只能动态解析（F2 已列）。
//   ⛔ 不给它们做影子实现（plan §10：触摸/手势返回 NULL = "这个 OS 没有"）。
#include "shims/register.h"

namespace lift {
namespace {

struct AbsentExport {
    const char* module;  // 归一化后的模块 key（小写、无 .dll）
    const char* name;
    const char* why;
};

// ★ 逐条点名（⛔ 不按模块整族放行）：
constexpr AbsentExport kAbsentExports[] = {
    // 触摸 / 手势：XP 的 user32 没有这些导出（Vista+ 才有）。
    // 引擎据此禁用触摸路径（plan §3 F2 / §10）。
    {"user32", "RegisterTouchWindow", "XP 的 user32 没有触摸 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "UnregisterTouchWindow", "XP 的 user32 没有触摸 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "SetGestureConfig", "XP 的 user32 没有手势 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "GetTouchInputInfo", "XP 的 user32 没有触摸 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "GetGestureInfo", "XP 的 user32 没有手势 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "CloseGestureInfoHandle", "XP 的 user32 没有手势 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    {"user32", "CloseTouchInputHandle", "XP 的 user32 没有触摸 API（Vista+ 才有）⇒ 真机返回 NULL + 127"},
    // shell32：引擎有 4 处动态解析 `SHGetFolderPathA`；本模拟器按"老 shell32"回答 ⇒
    // 引擎回退到已实现的 `SHGetSpecialFolderPathA`（shell32.cpp）。
    {"shell32", "SHGetFolderPathA",
     "本模拟器模拟的 shell32 不提供 SHGetFolderPathA ⇒ 引擎回退 SHGetSpecialFolderPathA（已实现）"},
    // comctl32：`_TrackMouseEvent` 是 comctl32 4.71+ 才有的导出；老 comctl32 没有。
    {"comctl32", "_TrackMouseEvent",
     "老 comctl32 没有 _TrackMouseEvent（4.71+ 才有）⇒ 真机返回 NULL + 127"},
    // kernel32：防休眠。本模拟器没有电源状态可改 ⇒ 按"这个 OS 没有"回答（⛔ 不影子实现）。
    {"kernel32", "SetThreadExecutionState",
     "本模拟器不建模电源状态 ⇒ 按「没有这个导出」返回 NULL + 127（引擎据此跳过防休眠）"},
};

bool sameIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

}  // namespace

const char* dynamicExportAbsentReason(const char* module, const char* name) {
    if (!module || !name) return nullptr;
    for (const AbsentExport& e : kAbsentExports) {
        if (sameIgnoreCase(e.module, module) && sameIgnoreCase(e.name, name)) return e.why;
    }
    return nullptr;
}

}  // namespace lift
