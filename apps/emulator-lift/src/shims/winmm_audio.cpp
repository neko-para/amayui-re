// emulator-lift · winmm 音频 / 多媒体 IO（SCOPE §1：延后实现 ⇒ 现在整体 no-op）
//
// ★ 用户口径（2026-11-04）：音频**后续要真做**，但当前按 no-op 实施；不是硬 trap。
//   判据：
//     · MCI（`mciSendCommandA` / `mciSendStringA` / `mciGetErrorStringA`）→ 接受命令、返回成功，
//       `mciSendStringA` 的返回串写成空串；
//     · mixer（`mixerOpen` / `mixerClose` / `mixerSetControlDetails`）→ 成功 + 非空假句柄；
//     · mmio（`mmioOpenA` / `mmioRead` / `mmioClose` / `mmioAscend` / `mmioDescend`）→ 接受调用，
//       `mmioOpenA` 返回非空假句柄，`mmioRead` 返回 0 字节（空流）。
// ★ 每一条都打一行 `[shim]`（SCOPE §3：⛔ 不许静默 no-op），登记为 `addStub`。
// ★ 摇杆（`joy*`）不属于音频，仍留在派生缺口里（M2 输入）。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kFakeMixer = 0x00030001u;
constexpr std::uint32_t kFakeMmio = 0x00040001u;

void shimMciSendCommandA(X86&, ShimCall& call) {
    shimLog("winmm", "mciSendCommandA", "音频 no-op ⇒ 返回成功（SCOPE §1；不打开设备、不播放）");
    call.result = 0;
}

void shimMciSendStringA(X86& cpu, ShimCall& call) {
    // 参数：lpstrCommand / lpstrReturnString / uReturnLength / hwndCallback
    const std::uint32_t ret = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    if (ret && cap) cpu.mem->wr8(ret, 0);
    shimLog("winmm", "mciSendStringA", "音频 no-op ⇒ 返回成功、返回串为空（SCOPE §1）");
    call.result = 0;
}

void shimMciGetErrorStringA(X86&, ShimCall& call) {
    shimLog("winmm", "mciGetErrorStringA", "音频 no-op ⇒ 没有错误串可给（返回 FALSE；SCOPE §1）");
    call.result = 0;
}

void shimMixerOpen(X86& cpu, ShimCall& call) {
    // 参数：phmx / uDeviceID / dwCallback / dwInstance / fdwOpen
    const std::uint32_t out = call.u32(0);
    if (out) cpu.mem->wr32(out, kFakeMixer);
    shimLog("winmm", "mixerOpen", "音频 no-op ⇒ 成功 + 假句柄 0x%08X（SCOPE §1）", kFakeMixer);
    call.result = 0;
}

void shimMixerClose(X86&, ShimCall& call) {
    shimLog("winmm", "mixerClose", "音频 no-op ⇒ 返回成功（SCOPE §1）");
    call.result = 0;
}

void shimMixerSetControlDetails(X86&, ShimCall& call) {
    shimLog("winmm", "mixerSetControlDetails", "音频 no-op ⇒ 接受音量 / 控制设置（SCOPE §1；不作用于宿主）");
    call.result = 0;
}

void shimMmioOpenA(X86&, ShimCall& call) {
    // 参数：szFileName / lpmmioinfo / fdwOpen
    shimLog("winmm", "mmioOpenA", "音频 no-op ⇒ 返回假 HMMIO 0x%08X（空流；SCOPE §1）", kFakeMmio);
    call.result = kFakeMmio;
}

void shimMmioRead(X86&, ShimCall& call) {
    shimLog("winmm", "mmioRead", "音频 no-op ⇒ 返回 0 字节（空流；SCOPE §1）");
    call.result = 0;
}

void shimMmioClose(X86&, ShimCall& call) {
    shimLog("winmm", "mmioClose", "音频 no-op ⇒ 返回成功（SCOPE §1）");
    call.result = 0;
}

void shimMmioAscend(X86&, ShimCall& call) {
    shimLog("winmm", "mmioAscend", "音频 no-op ⇒ 返回成功（SCOPE §1）");
    call.result = 0;
}

void shimMmioDescend(X86&, ShimCall& call) {
    shimLog("winmm", "mmioDescend", "音频 no-op ⇒ 返回成功（SCOPE §1；空流没有 chunk）");
    call.result = 0;
}

}  // namespace

void registerWinmmAudioShims(ShimRegistry& s) {
    s.addStub("winmm", "mciSendCommandA", Conv::Stdcall, 4, &shimMciSendCommandA,
              "SCOPE §1 音频 no-op：mciSendCommandA 接受命令并返回成功（不打开设备、不播放）");
    s.addStub("winmm", "mciSendStringA", Conv::Stdcall, 4, &shimMciSendStringA,
              "SCOPE §1 音频 no-op：mciSendStringA 接受命令并返回成功、返回串为空");
    s.addStub("winmm", "mciGetErrorStringA", Conv::Stdcall, 3, &shimMciGetErrorStringA,
              "SCOPE §1 音频 no-op：mciGetErrorStringA 返回 FALSE（没有错误串）");
    s.addStub("winmm", "mixerOpen", Conv::Stdcall, 5, &shimMixerOpen,
              "SCOPE §1 音频 no-op：mixerOpen 返回成功 + 非空假句柄");
    s.addStub("winmm", "mixerClose", Conv::Stdcall, 1, &shimMixerClose,
              "SCOPE §1 音频 no-op：mixerClose 返回成功");
    s.addStub("winmm", "mixerSetControlDetails", Conv::Stdcall, 3, &shimMixerSetControlDetails,
              "SCOPE §1 音频 no-op：mixerSetControlDetails 接受音量 / 控制设置但不作用于宿主");
    s.addStub("winmm", "mmioOpenA", Conv::Stdcall, 3, &shimMmioOpenA,
              "SCOPE §1 音频 no-op：mmioOpenA 返回非空假 HMMIO（空流）");
    s.addStub("winmm", "mmioRead", Conv::Stdcall, 3, &shimMmioRead,
              "SCOPE §1 音频 no-op：mmioRead 返回 0 字节（空流）");
    s.addStub("winmm", "mmioClose", Conv::Stdcall, 2, &shimMmioClose,
              "SCOPE §1 音频 no-op：mmioClose 返回成功");
    s.addStub("winmm", "mmioAscend", Conv::Stdcall, 3, &shimMmioAscend,
              "SCOPE §1 音频 no-op：mmioAscend 返回成功");
    s.addStub("winmm", "mmioDescend", Conv::Stdcall, 4, &shimMmioDescend,
              "SCOPE §1 音频 no-op：mmioDescend 返回成功（空流没有 chunk）");
}

}  // namespace lift
