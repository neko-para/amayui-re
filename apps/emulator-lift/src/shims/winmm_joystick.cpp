// emulator-lift · winmm 摇杆（SCOPE §1：大概率永久不做 ⇒ 无设备 no-op）
//
// ★ 用户口径（2026-11-04）：摇杆与菜单同属"最终大概率一定不做"的一类（不是 audio / video 那种
//   "延后但最终要做"），记录在 `SCOPE.md` §1；当前按**无设备**处理，不是硬 trap。
//   判据：`joyGetNumDevs` → 0（系统里没有摇杆）；`joyGetPosEx` → `JOYERR_UNPLUGGED(167)`；
//   `joyGetDevCapsA` → `MMSYSERR_NODRIVER(6)`。
// ★ 每一条都打一行 `[shim]`（SCOPE §3），登记为 `addStub`。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kNoDriver = 6;        // MMSYSERR_NODRIVER
constexpr std::uint32_t kJoyUnplugged = 167;  // JOYERR_UNPLUGGED

void shimJoyGetNumDevs(X86&, ShimCall& call) {
    shimLog("winmm", "joyGetNumDevs", "摇杆永久不做 ⇒ 返回 0（系统里没有摇杆；SCOPE §1）");
    call.result = 0;
}

void shimJoyGetDevCapsA(X86&, ShimCall& call) {
    shimLog("winmm", "joyGetDevCapsA", "摇杆永久不做 ⇒ 返回 MMSYSERR_NODRIVER(6)（SCOPE §1）");
    call.result = kNoDriver;
}

void shimJoyGetPosEx(X86&, ShimCall& call) {
    shimLog("winmm", "joyGetPosEx", "摇杆永久不做 ⇒ 返回 JOYERR_UNPLUGGED(167)（SCOPE §1）");
    call.result = kJoyUnplugged;
}

}  // namespace

void registerWinmmJoystickShims(ShimRegistry& s) {
    s.addStub("winmm", "joyGetNumDevs", Conv::Stdcall, 0, &shimJoyGetNumDevs,
              "SCOPE §1 摇杆永久不做：joyGetNumDevs 返回 0（系统里没有摇杆）");
    s.addStub("winmm", "joyGetDevCapsA", Conv::Stdcall, 3, &shimJoyGetDevCapsA,
              "SCOPE §1 摇杆永久不做：joyGetDevCapsA 返回 MMSYSERR_NODRIVER(6)");
    s.addStub("winmm", "joyGetPosEx", Conv::Stdcall, 2, &shimJoyGetPosEx,
              "SCOPE §1 摇杆永久不做：joyGetPosEx 返回 JOYERR_UNPLUGGED(167)");
}

}  // namespace lift
