// emulator-lift · shim 族注册入口（七族的总装配点）
//
// ★ 结构：每个族文件**各导出一个** `registerXxxShims(ShimRegistry&)`；本头统一声明，
//   由 `registerKernel32Shims`（register.cpp）按**固定顺序**调用。
//   顺序 = `ShimDef::id` = trace 顺序 ⇒ **它也是判据的一部分**（改顺序 = 改 trace）。
// ★ 本头同时放七族共用的两种"响亮度"（SCOPE §3：⛔ 不许静默 no-op）：
//   · `shimAbort`：参数非法 / 状态崩坏 / 该实现却做不了 ⇒ 打 `[shim] <module>!<name>: <原因>` 后 abort；
//   · `shimLog`  ：能返回 **Windows 真实失败值**的路径（缺 DLL / 名字找不到 / 点名过的偏差）⇒ 只留一行。
//   ⛔ 两者都不许被"返回成功但什么都没做"替代。
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "runtime/host_encoding.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "runtime/handles.h"
#include "runtime/log.h"
#include "runtime/shims.h"

namespace lift {

// S7 启动路径上补的 shim（见 kernel32_bootstrap.cpp）
void registerBootstrapShims(ShimRegistry& shims);

// ★ 派生文件（`pnpm tools lift sync --write` 生成，⛔ 不要手改）：
//   PE 导入表里有、但既没实现也没登记的条目 ⇒ 全部 addUnimplemented（带 reason）。
//   它让"静态缺口 = 0 条无理由"可机械判定（`pnpm tools lift check`）。
void registerImportGaps(ShimRegistry& shims);


// 七族的唯一入口（Machine 装配时调一次）。定义在 register.cpp。
void registerKernel32Shims(ShimRegistry& shims);

// 族文件各自导出的注册函数（调用顺序见 register.cpp）。
void registerErrorTlsShims(ShimRegistry& shims);  // kernel32_error_tls.cpp
void registerMemShims(ShimRegistry& shims);       // kernel32_mem.cpp
void registerSyncShims(ShimRegistry& shims);      // kernel32_sync.cpp
void registerProcessShims(ShimRegistry& shims);   // kernel32_process.cpp
void registerFileShims(ShimRegistry& shims);      // kernel32_file.cpp（文件与路径族，S6）
void registerTimeShims(ShimRegistry& shims);      // kernel32_time.cpp
void registerStringShims(ShimRegistry& shims);    // kernel32_string.cpp（码页与字符串族）
void registerAgercShims(ShimRegistry& shims);     // kernel32_agerc.cpp（AGERC 真实模块 + 三个导出 stub）
void registerUser32MenuShims(ShimRegistry& shims);  // user32_menu.cpp（菜单 / 模态 no-op，SCOPE §1）
void registerUser32WindowShims(ShimRegistry& shims);  // user32_window.cpp（P0 窗口 / 消息泵）
void registerUser32InputShims(ShimRegistry& shims);   // user32_input.cpp（M2 输入 / 光标族，headless 真语义）
void registerUser32ResourceShims(ShimRegistry& shims); // user32_resource.cpp（AGE.EXE 图标 / 光标）
void registerGdiBootShims(ShimRegistry& shims);       // gdi32_boot.cpp（P0 启动最小件）
// ★ P2：GDI 对象表 / DC（gdi32_object.cpp）与字体 / 度量 / 字形 / 文本（gdi32_font.cpp）。
//   `registerGdiFontShims` 是**唯一**对外入口（它内部先调 registerGdiObjectShims）。
void registerGdiObjectShims(ShimRegistry& shims);
void registerGdiFontShims(ShimRegistry& shims);
// ★ P4：GDI region 七条（CreateRectRgn / CreateEllipticRgnIndirect / CreatePolygonRgn / CombineRgn /
//   GetRegionData / GetRgnBox / PtInRegion）。区域对象走 `HandleTable`（`HandleKind::GdiRegion`）。
void registerGdiRegionShims(ShimRegistry& shims);  // gdi32_region.cpp
void registerShell32Shims(ShimRegistry& shims);       // shell32.cpp（特殊目录 / --appdata + imm32 没有 IME）
// ★ P4 小族：文件/路径邻域的两条（引擎在启动路径上真调它们，`.lst` 0x0049037D / 0x004903A3）。
void registerShlwapiShims(ShimRegistry& shims);       // shlwapi.cpp（PathRemoveFileSpecA）
void registerImagehlpShims(ShimRegistry& shims);      // imagehlp.cpp（MakeSureDirectoryPathExists）
void registerAdvapi32RegistryShims(ShimRegistry& shims);  // advapi32_registry.cpp（注册表 no-op，SCOPE §1）
void registerWinmmAudioShims(ShimRegistry& shims);  // winmm_audio.cpp（音频 no-op，SCOPE §1）
// ★ M2 · DirectSound silent 层（SCOPE §1「音频 = 延后实现（silent）」）：
//   `LoadLibraryA("DSOUND.DLL")` 的假模块句柄（表在 kernel32_process.cpp 的 `kLoadableModules`）
//   + `GetProcAddress("DirectSoundCreate")` 的**动态导出** + IDirectSound8 / IDirectSoundBuffer8 的
//   取证槽（`Lock` 给**真 guest 内存**，`Play` / `Set*` 记账）。未实现的槽 = loud trap。
void registerDsoundShims(ShimRegistry& shims);      // dsound.cpp
void registerWinmmJoystickShims(ShimRegistry& shims);  // winmm_joystick.cpp（摇杆永久不做，SCOPE §1）
void registerVideoShims(ShimRegistry& shims);     // mf_video.cpp（视频 / 媒体 no-op，SCOPE §1）
// ★ M4 起步：DirectShow / AMultiMediaStream 的**形状层**（CoCreateInstance + 影片接口立即完成）。
//   目标不是解码，而是让"准备播放视频"不再停在 ole32!CoCreateInstance。
void registerQuartzVideoShims(ShimRegistry& shims);  // quartz_video.cpp
void registerVersionShims(ShimRegistry& shims);   // kernel32_version.cpp（版本资源查询：真实失败码）
void registerDdrawShims(ShimRegistry& shims);     // ddraw.cpp（P1：DirectDrawCreate 动态导出 + COM 方法表）
// ★ M2 · D3D9 最小设备簇（用户 2026-11-05 裁决"确认需要实现 D3D 接口"）：
//   `d3d9!Direct3DCreate9` 静态导入 + IDirect3D9 / IDirect3DDevice9 的取证槽（其余槽 = loud trap）。
//   状态 / 像素层在 runtime/d3d9.{h,cpp}，导出 + vtable 在 shims/d3d9.cpp。
void registerD3D9Shims(ShimRegistry& shims);      // d3d9.cpp
// ★ M2 · D3DX9 Sprite 簇（第一帧的直接缺口）：`d3dx9_43!D3DXCreateSprite` 静态导入 +
//   ID3DXSprite 的取证槽（+0x18/+0x1C = loud trap）。状态 / 批次 / 合成在 runtime/d3dx9_sprite.{h,cpp}，
//   导出 + vtable 在 shims/d3dx9.cpp。
void registerD3DX9Shims(ShimRegistry& shims);     // d3dx9.cpp
// P1 / G1 的"**本 OS 没有这个导出**"表：`GetProcAddress`（kernel32_process.cpp）查它来给出比
// "模块里没有这个导出"更准确的日志（⛔ 不注册成 shim —— 真机对这些名字返回 NULL，
// 引擎据此走回退分支；注册成 thunk 会让引擎以为"这个 OS 有它"）。
const char* dynamicExportAbsentReason(const char* module, const char* name);

// ---- 共用的诊断打印（分级 / 分类见 runtime/log.h）----
// ★ 默认 `warn`：`shimLog`（Info）默认**不显示**；`shimLogWarn` / `shimLogDebug` 明确选级别。
//   `shimAbort` 永远显示（Error 不受闸门影响），因为它是"该实现却做不到"的硬失败。
inline void shimVPrintAt(LogLevel level, std::uint32_t category, const char* module, const char* name,
                         const char* fmt, va_list ap) {
    char prefix[320];
    std::snprintf(prefix, sizeof(prefix), "[shim] %s!%s: ", module ? module : "?",
                  name ? name : "?");
    lift::logV(level, category, prefix, fmt, ap);
}

// 兼容旧调用：Info / shim 分类。
inline void shimVPrint(const char* module, const char* name, const char* fmt, va_list ap) {
    shimVPrintAt(LogLevel::Info, LogShim, module, name, fmt, ap);
}

// "这条做不到" ⇒ 打一行然后**停**（参数非法 / 无效句柄 / 该实现却没实现）。
// 用 HandleTable::invalidHandle() 的路径不必再叠一层（它自己打印并停）。
//
// ★ Windows 上必须先关掉 CRT 的 abort 行为（2026-11-04 实测）：默认 `abort()` 会弹
//   **模态断言框 / WER 报告框**，在无人的流水线里就是**整轮挂死**（对话框等人点）。
//   `_set_abort_behavior(0, …)` 之后 `abort()` 只是「打了日志、退出码 3」，日志照样在 stderr 上。
//   同一处理在 `tests/check.h` 里也做了一遍（测试进程不经过本头也要免疫）。
inline void suppressCrashDialogs() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

[[noreturn]] inline void shimAbort(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(LogLevel::Error, LogShim, module, name, fmt, ap);
    va_end(ap);
    suppressCrashDialogs();
    restoreConsoleUtf8();  // abort 不跑析构；控制台码页必须显式恢复
    std::abort();
}

// 默认正常事件（Info / shim 分类）；默认 `warn` 模式下不显示。
inline void shimLog(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(LogLevel::Info, LogShim, module, name, fmt, ap);
    va_end(ap);
}

inline void shimLogAt(LogLevel level, std::uint32_t category, const char* module, const char* name,
                      const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(level, category, module, name, fmt, ap);
    va_end(ap);
}

inline void shimLogWarn(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(LogLevel::Warn, LogShim, module, name, fmt, ap);
    va_end(ap);
}

inline void shimLogDebug(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(LogLevel::Debug, LogShim, module, name, fmt, ap);
    va_end(ap);
}

inline void shimLogError(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrintAt(LogLevel::Error, LogShim, module, name, fmt, ap);
    va_end(ap);
}

}  // namespace lift
