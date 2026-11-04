// emulator-lift · version.dll 版本资源查询（**按 Windows 的"没有版本资源"返回失败**）
//
// ★ 为什么是 stub 而不是实现：本线不解析 PE 的 `.rsrc` 版本资源（那是 M2 的资源族）；
//   而 Windows 对"文件没有版本资源"的真实回答就是 **FALSE + ERROR_RESOURCE_TYPE_NOT_FOUND(1813)**。
//   ⇒ 返回值是**真实语义**，不是"假装成功"；SCOPE §3 的三条（合理值 / 不阻塞 / 一行日志 / 清单登记）都满足。
// ★ 它挡在 `_WinMain` 的启动路径上（实测 0x0048E663 调用 `GetFileVersionInfoSizeA`）：
//   M1 不做真版本资源，但也不能让它把启动打断 —— 用真实失败码让 guest 走"没有版本信息"的分支。
// ★ 未来若真做资源族：把这里换成解析 `.rsrc` 的 VS_VERSIONINFO，并把 overrides 的 kind 改成 replace。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrResourceTypeNotFound = 1813u;

void versionNoResource(X86& cpu, ShimCall& call, const char* api) {
    cpu.shim.last_error = kErrResourceTypeNotFound;
    shimLog("version", api, "本线不解析 PE 版本资源 ⇒ 按 Windows 的「没有版本资源」返回 FALSE + 错误 1813");
    call.result = 0;
}

}  // namespace

void registerVersionShims(ShimRegistry& s) {
    const char* kReason =
        "version.dll 版本资源查询：本线不解析 PE 的 .rsrc 版本资源 ⇒ 按 Windows 的\"没有版本资源\""
        "返回 FALSE + ERROR_RESOURCE_TYPE_NOT_FOUND(1813)（不是假装成功）";
    s.addStub("version", "GetFileVersionInfoSizeA", Conv::Stdcall, 2,
              [](X86& cpu, ShimCall& call) { versionNoResource(cpu, call, "GetFileVersionInfoSizeA"); }, kReason);
    s.addStub("version", "GetFileVersionInfoA", Conv::Stdcall, 4,
              [](X86& cpu, ShimCall& call) { versionNoResource(cpu, call, "GetFileVersionInfoA"); }, kReason);
    s.addStub("version", "VerQueryValueA", Conv::Stdcall, 4,
              [](X86& cpu, ShimCall& call) { versionNoResource(cpu, call, "VerQueryValueA"); }, kReason);
}

}  // namespace lift
