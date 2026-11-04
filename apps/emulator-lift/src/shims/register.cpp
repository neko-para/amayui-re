// emulator-lift · shim 注册总表（七族 + "已知但 M1a 不做"的缺口清单）
//
// ★ 顺序是判据：`ShimDef::id` 按 `add` 的先后分配，trace 里出现的就是这个顺序 ⇒
//   改这里的调用顺序 = 改 trace。族的顺序固定为：
//     错误/TLS → 内存 → 同步 → 进程/模块 → 计时
// ★ 末尾那一段是 **A5 缺口报告的输入**：本批（S5 + S6 前三族 + CRT 启动路径）之外的已知导入，
//   一律 `addUnimplemented` —— 它们**分到唯一桩地址**，执行到就打印 module!name 并停下，
//   同时进 `importedButUnimplemented()` / `calledUnimplemented()`。
//   ⛔ 不许把它们注册成"返回成功的空实现"（SCOPE §3）。
// ★ 维护口径：将来哪一族真做起来了，就把它在**本文件末尾**对应的行删掉、改在族文件里 `add`
//   —— 一处名字只能注册一次（`find()` 取第一条，重复注册会让 trace 归属含糊）。
#include "shims/register.h"

namespace lift {

void registerKernel32Shims(ShimRegistry& shims) {
    // ---- 七族（每族一个文件；族内顺序也固定）----
    registerErrorTlsShims(shims);  // kernel32_error_tls.cpp：错误码 / TLS / Interlocked
    registerMemShims(shims);       // kernel32_mem.cpp：Heap* / Local* / Global* / Virtual*
    registerSyncShims(shims);      // kernel32_sync.cpp：临界区 / 事件 / 信号量 / 互斥量 / 等待
    registerProcessShims(shims);   // kernel32_process.cpp：模块 / 命令行 / 版本 / 退出
    registerFileShims(shims);      // kernel32_file.cpp：文件 / 路径 / 目录 / Find*（S6）
    registerTimeShims(shims);      // kernel32_time.cpp：tick / FILETIME / SYSTEMTIME / winmm
    registerStringShims(shims);    // kernel32_string.cpp：wsprintfA（varargs）/ lstrlenA / 码页
    registerAgercShims(shims);     // kernel32_agerc.cpp：AGERC 三个导出（SCOPE §1 模态 no-op）
    registerUser32MenuShims(shims);  // user32_menu.cpp：菜单 / 模态 no-op（SCOPE §1）
    registerUser32WindowShims(shims);  // user32_window.cpp：P0 窗口 / 完整消息泵
    registerUser32ResourceShims(shims); // user32_resource.cpp：AGE.EXE 图标 / 光标
    registerGdiBootShims(shims);       // gdi32_boot.cpp：P0 启动最小件
    registerShell32Shims(shims);       // shell32.cpp：特殊目录（--appdata）
    registerVersionShims(shims);   // kernel32_version.cpp：版本资源查询（真实失败码 1813）

    // =================================================================================
    // 已知但 M1a 不做（A5 缺口清单）—— 模块名一律小写（查表大小写不敏感）
    // =================================================================================

    // ---- kernel32：文件 / 路径 / 进程创建 / 环境 ----
    // ★ S6 的文件与路径族已由 kernel32_file.cpp 实现（CreateFileA/W · Read/WriteFile ·
    //   SetFilePointer(Ex) · GetFileSize · FindFirst/NextFileA/FindClose · 目录/属性/删除/改名/复制 ·
    //   GetCurrentDirectoryA/SetCurrentDirectoryA · GetTempPathA/GetTempFileNameA ·
    //   GetFullPathNameA / GetSystemDirectoryA / GetWindowsDirectoryA · SetFileTime …）。
    // ★ 注意：`GetEnvironmentVariableA` 已由 kernel32_bootstrap.cpp **实现**（env 块读取）。
    //   它曾在这里又被登记成"未实现" ⇒ register.cpp 先注册、bootstrap 后注册 ⇒ find() 命中的是
    //   未实现那条，实现了也永远用不上。`pnpm tools lift check` 的 no-dup-registration 守这条。
    //
    // ★ `CreateProcessA` 的登记已挪到 kernel32_process.cpp（与"进程 / 模块信息族"同文件）。

    // ---- advapi32：注册表（用户 2026-11-04：与菜单 / 模态一样 no-op，见 SCOPE §1）----
    registerAdvapi32RegistryShims(shims);

    // ---- user32：窗口 / 消息 / 菜单 ----
    // ⚠ 菜单与模态是 SCOPE §1 点名的**可接受偏差**（将来落地时要按 stub 口径：返回合理值 + 不打桩的日志 +
    //   overrides.json 登记），但那是 M2 的窗口族，⛔ 不是本批"假装成功"。
    const char* kUser32 = "M2 窗口 / 消息族（plan D4：L3；不属 M1）";
    // ★ P0 已实现：RegisterClassExA / CreateWindowExA / DestroyWindow / ShowWindow / UpdateWindow /
    //   PeekMessageA / GetMessageA / TranslateMessage / DispatchMessageA / DefWindowProcA /
    //   SendMessageA / PostMessageA / PostQuitMessage / SetTimer / KillTimer / GetSystemMetrics /
    //   GetClientRect / GetWindowRect / ...（见 user32_window.cpp）。
    //   仍未实现（且当前导入表里没有直呼）：RegisterClassA / GetDC / ReleaseDC / SetFocus。
    shims.addUnimplemented("user32", "RegisterClassA", kUser32);
    shims.addUnimplemented("user32", "GetDC", kUser32);
    shims.addUnimplemented("user32", "ReleaseDC", kUser32);
    shims.addUnimplemented("user32", "SetFocus", kUser32);

    // ---- gdi32：2D 图形 ----
    const char* kGdi32 = "M2 GDI / 2D 图形族（不属 M1）";
    shims.addUnimplemented("gdi32", "CreateCompatibleDC", kGdi32);
    shims.addUnimplemented("gdi32", "CreateDIBSection", kGdi32);
    shims.addUnimplemented("gdi32", "SelectObject", kGdi32);
    shims.addUnimplemented("gdi32", "DeleteDC", kGdi32);
    shims.addUnimplemented("gdi32", "DeleteObject", kGdi32);
    shims.addUnimplemented("gdi32", "BitBlt", kGdi32);
    shims.addUnimplemented("gdi32", "GetDeviceCaps", kGdi32);
    shims.addUnimplemented("gdi32", "SetBkMode", kGdi32);
    shims.addUnimplemented("gdi32", "SetTextColor", kGdi32);

    // ---- d3d9：3D ----
    shims.addUnimplemented("d3d9", "Direct3DCreate9",
                           "M3 D3D9 簇（SCOPE §1：3D 不做 / 走 stub；不属 M1）");

    // ---- 音频 / 视频：用户 2026-11-04 裁决"延后实现，当前 no-op"（SCOPE §1）----
    registerWinmmAudioShims(shims);
    registerWinmmJoystickShims(shims);
    registerVideoShims(shims);

    // ---- 派生缺口（**最后**）：导入表里有、上面既没实现也没登记的条目 ----
    // 由 `pnpm tools lift sync --write` 生成；理由按模块分组（见 tools/lift.mjs 的 MODULE_REASONS）。
    registerImportGaps(shims);
}

}  // namespace lift
