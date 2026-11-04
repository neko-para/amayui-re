// emulator-lift · 视频 / 媒体基础（SCOPE §1：延后实现 ⇒ 现在整体 no-op）
//
// ★ 用户口径（2026-11-04）：视频与音频一样**后续要真做**，当前按 no-op 实施；不是硬 trap。
//   判据：
//     · `MFStartup` / `MFShutdown` / `MFInvokeCallback` → 成功（S_OK）；
//     · 所有 `MFCreate*` / `MFGetService` / `DXVA2CreateDirect3DDeviceManager9` → `E_FAIL`，
//       并把输出指针置 0（⛔ 不返回假 COM 对象：guest 会按 vtable 调用，必崩）。
// ★ 为什么"失败"也是 no-op：这些 API 的产物是 COM 对象；没有真实现时，返回**假对象**比返回
//   `E_FAIL` 危险得多。调用方拿到 E_FAIL 会走"没有视频"的降级路径，符合 SCOPE §1 的"影片 skip"。
// ★ 每一条都打一行 `[shim]`（SCOPE §3），登记为 `addStub`。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kEfail = 0x80004005u;

// 统一的"没有可返回的对象"路径：输出指针（若给）置 0，返回 E_FAIL。
void failWithNull(X86& cpu, ShimCall& call, int out_index, const char* module, const char* api) {
    const std::uint32_t out = call.u32(out_index);
    if (out) cpu.mem->wr32(out, 0);
    shimLog(module, api, "视频 / 媒体延期实现 ⇒ 返回 E_FAIL 并置空输出（SCOPE §1；⛔ 不返回假 COM 对象）");
    call.result = kEfail;
}

void shimMFStartup(X86&, ShimCall& call) {
    shimLog("mfplat", "MFStartup", "视频 / 媒体延期实现 ⇒ 返回 S_OK（SCOPE §1；不初始化真实媒体栈）");
    call.result = 0;
}

void shimMFShutdown(X86&, ShimCall& call) {
    shimLog("mfplat", "MFShutdown", "视频 / 媒体延期实现 ⇒ 返回 S_OK（SCOPE §1）");
    call.result = 0;
}

void shimMFInvokeCallback(X86&, ShimCall& call) {
    shimLog("mfplat", "MFInvokeCallback", "视频 / 媒体延期实现 ⇒ 返回 S_OK（SCOPE §1；没有异步结果）");
    call.result = 0;
}

void shimMFCreateAttributes(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 0, "mfplat", "MFCreateAttributes");
}

void shimMFCreateMediaType(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 0, "mfplat", "MFCreateMediaType");
}

void shimMFCreateAsyncResult(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 3, "mfplat", "MFCreateAsyncResult");
}

void shimMFCreateAudioRendererActivate(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 0, "mf", "MFCreateAudioRendererActivate");
}

void shimMFCreateMediaSession(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 1, "mf", "MFCreateMediaSession");
}

void shimMFCreateSourceResolver(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 0, "mf", "MFCreateSourceResolver");
}

void shimMFCreateTopology(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 0, "mf", "MFCreateTopology");
}

void shimMFCreateTopologyNode(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 1, "mf", "MFCreateTopologyNode");
}

void shimMFCreateVideoRendererActivate(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 1, "mf", "MFCreateVideoRendererActivate");
}

void shimMFGetService(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 3, "mf", "MFGetService");
}

void shimMFCreateVideoSampleFromSurface(X86& cpu, ShimCall& call) {
    failWithNull(cpu, call, 1, "evr", "MFCreateVideoSampleFromSurface");
}

void shimDXVA2CreateDirect3DDeviceManager9(X86& cpu, ShimCall& call) {
    const std::uint32_t token = call.u32(0);
    if (token) cpu.mem->wr32(token, 0);
    failWithNull(cpu, call, 1, "dxva2", "DXVA2CreateDirect3DDeviceManager9");
}

}  // namespace

void registerVideoShims(ShimRegistry& s) {
    // MFPlat：初始化 / 异步骨架
    s.addStub("mfplat", "MFStartup", Conv::Stdcall, 2, &shimMFStartup,
              "SCOPE §1 视频 / 媒体延期：MFStartup 返回 S_OK（不初始化真实媒体栈）");
    s.addStub("mfplat", "MFShutdown", Conv::Stdcall, 0, &shimMFShutdown,
              "SCOPE §1 视频 / 媒体延期：MFShutdown 返回 S_OK");
    s.addStub("mfplat", "MFInvokeCallback", Conv::Stdcall, 1, &shimMFInvokeCallback,
              "SCOPE §1 视频 / 媒体延期：MFInvokeCallback 返回 S_OK（没有异步结果）");
    s.addStub("mfplat", "MFCreateAttributes", Conv::Stdcall, 2, &shimMFCreateAttributes,
              "SCOPE §1 视频 / 媒体延期：MFCreateAttributes 返回 E_FAIL + 空输出（⛔ 不返回假 COM 对象）");
    s.addStub("mfplat", "MFCreateMediaType", Conv::Stdcall, 1, &shimMFCreateMediaType,
              "SCOPE §1 视频 / 媒体延期：MFCreateMediaType 返回 E_FAIL + 空输出");
    s.addStub("mfplat", "MFCreateAsyncResult", Conv::Stdcall, 4, &shimMFCreateAsyncResult,
              "SCOPE §1 视频 / 媒体延期：MFCreateAsyncResult 返回 E_FAIL + 空输出");

    // MF：会话 / 拓扑 / 渲染器
    s.addStub("mf", "MFCreateAudioRendererActivate", Conv::Stdcall, 1, &shimMFCreateAudioRendererActivate,
              "SCOPE §1 音频 / 视频延期：MFCreateAudioRendererActivate 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFCreateMediaSession", Conv::Stdcall, 2, &shimMFCreateMediaSession,
              "SCOPE §1 视频延期：MFCreateMediaSession 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFCreateSourceResolver", Conv::Stdcall, 1, &shimMFCreateSourceResolver,
              "SCOPE §1 视频延期：MFCreateSourceResolver 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFCreateTopology", Conv::Stdcall, 1, &shimMFCreateTopology,
              "SCOPE §1 视频延期：MFCreateTopology 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFCreateTopologyNode", Conv::Stdcall, 2, &shimMFCreateTopologyNode,
              "SCOPE §1 视频延期：MFCreateTopologyNode 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFCreateVideoRendererActivate", Conv::Stdcall, 2, &shimMFCreateVideoRendererActivate,
              "SCOPE §1 视频延期：MFCreateVideoRendererActivate 返回 E_FAIL + 空输出");
    s.addStub("mf", "MFGetService", Conv::Stdcall, 4, &shimMFGetService,
              "SCOPE §1 视频延期：MFGetService 返回 E_FAIL + 空输出");

    // EVR / DXVA2
    s.addStub("evr", "MFCreateVideoSampleFromSurface", Conv::Stdcall, 2, &shimMFCreateVideoSampleFromSurface,
              "SCOPE §1 视频延期：MFCreateVideoSampleFromSurface 返回 E_FAIL + 空输出");
    s.addStub("dxva2", "DXVA2CreateDirect3DDeviceManager9", Conv::Stdcall, 2, &shimDXVA2CreateDirect3DDeviceManager9,
              "SCOPE §1 视频延期：DXVA2CreateDirect3DDeviceManager9 返回 E_FAIL + 空输出");
}

}  // namespace lift
