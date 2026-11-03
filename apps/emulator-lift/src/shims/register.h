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

#include "runtime/handles.h"
#include "runtime/shims.h"

namespace lift {

// S7 启动路径上补的 shim（见 kernel32_bootstrap.cpp）
void registerBootstrapShims(ShimRegistry& shims);


// 七族的唯一入口（Machine 装配时调一次）。定义在 register.cpp。
void registerKernel32Shims(ShimRegistry& shims);

// 族文件各自导出的注册函数（调用顺序见 register.cpp）。
void registerErrorTlsShims(ShimRegistry& shims);  // kernel32_error_tls.cpp
void registerMemShims(ShimRegistry& shims);       // kernel32_mem.cpp
void registerSyncShims(ShimRegistry& shims);      // kernel32_sync.cpp
void registerProcessShims(ShimRegistry& shims);   // kernel32_process.cpp
void registerTimeShims(ShimRegistry& shims);      // kernel32_time.cpp

// ---- 共用的诊断打印（一行一条，stderr；不受 --trace 影响 —— 缺口感必须是常亮的）----
inline void shimVPrint(const char* module, const char* name, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[shim] %s!%s: ", module, name);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// "这条做不到" ⇒ 打一行然后**停**（参数非法 / 无效句柄 / 该实现却没实现）。
// 用 HandleTable::invalidHandle() 的路径不必再叠一层（它自己打印并停）。
[[noreturn]] inline void shimAbort(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrint(module, name, fmt, ap);
    va_end(ap);
    std::abort();
}

// "这条按 Windows 的真实失败值返回" ⇒ 打一行但不中止（缺 DLL / 名字找不到 / 点名过的偏差）。
inline void shimLog(const char* module, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    shimVPrint(module, name, fmt, ap);
    va_end(ap);
}

}  // namespace lift
