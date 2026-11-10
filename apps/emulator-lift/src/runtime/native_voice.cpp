// emulator-lift · `sub_4BBAB0`（`Voice::Tick(now)`）的 stateless 快路径
//
// ★ 需求单：REQ-01M6XVZF3VDC5NSBJ20J2451VJ（"每帧固定税"）。裁决 = **A 案**：函数级
//   `NativeOverride` + **stateless 判定**（纯查询才短路），⛔ 不重写 `sub_4BB840`、⛔ 不重写整个 Tick。
// ★ 事实来自 `.lst` 0x4BBAB0..0x4BBC24（EA 是锚；本文件不复述行号）：
//   - `ecx = this`（= T_Engine+0x148A0）、1 个栈参 = `now`(ms)、`retn 4`；
//   - 三个槽 = `this+0x424 + 4*i`（i = 0..2 ↔ 声道 12/13/14）；
//   - 每个槽先看 `[slot+0x30] & 3`：== 3 ⇒ 进内层扫描（4 次 `sub_4B6130` 查询 + 可能写）；
//     否则看 `[slot]`：== 0 ⇒ **直接跳到循环尾**（该槽这一轮一条写都没有）；
//   - `[slot] != 0`（已启动）⇒ 再看 `[slot+0xC]`（last tick）与 `[slot+0x18]`（周期）：
//     last == 0 ⇒ 写 `last = now`；未到期 ⇒ 只读；到期 ⇒ 清 `[slot]`/标志并调 `sub_4BB840`。
//   ⇒ **唯一零写形态** = 三个槽都满足 `(slot[0x30] & 3) != 3` 且 `slot[0] == 0`。
// ★ 为什么不把"已启动但未到期"也短路（它在语义上也是纯查询）：
//   短路要替换掉**固定条数**的指令（见 `kVoiceTickPureInsns`），而那条路径每槽要多读 2 个 dword、
//   多 7 条指令，且随"有几个槽在播"变化 ⇒ k 不再是定值、时钟补偿就会改掉 `now` 的输入序列。
//   ⇒ 只认"三槽全惰性"这一种，其余**一律回退解释器**（正确性优先；稳态空转正是这一种）。
// ★ 顺序 / 次数：函数本身没有游标 / 计数器 / 全局（`.lst` 里 `[ebp+ArgList]` 是本地循环变量），
//   所以"三槽都惰性"时扫描顺序无关；但**有写时**跨槽顺序会影响后处理槽的内层扫描 ⇒ 有写就回退。
#include "runtime/native_overrides.h"

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// `this` 一侧的字段（EA 只做锚：偏移来自 `.lst` 的寻址，不是猜的）。
constexpr std::uint32_t kDeviceOffset = 0x408;  // `[this+0x408]` = 设备 / 管理器指针
constexpr std::uint32_t kSlotBase = 0x424;     // 第 0 个槽（槽 i 在 +4*i）
constexpr std::uint32_t kSlotStride = 4;
constexpr std::uint32_t kSlotCount = 3;
constexpr std::uint32_t kSlotActive = 0x00;    // 非 0 ⇒ 该槽已启动
constexpr std::uint32_t kSlotLastTick = 0x0C;  // 上一次 tick 的 ms（0 ⇒ 首 tick 会写它）
constexpr std::uint32_t kSlotPeriod = 0x18;    // 周期 ms
constexpr std::uint32_t kSlotMode = 0x30;      // 低 2 位：3 ⇒ 内层扫描
// 一次 `committed()` 覆盖 this+0x408 .. this+0x460（设备指针 + 三个槽的全部字段）。
constexpr std::uint32_t kSpan = (kSlotBase + (kSlotCount - 1) * kSlotStride + kSlotMode + 4) - kDeviceOffset;
// `this` 大到让 `this + kSpan` 回绕 ⇒ 不判（交给解释器，它会按 32 位回绕读到同一个地方）。
constexpr std::uint32_t kSelfLimit = static_cast<std::uint32_t>(0x100000000ull - kSpan);

VoicePollDecision decline(VoicePollWhy why, unsigned slot) {
    VoicePollDecision d;
    d.pure = false;
    d.why = why;
    d.slot = slot;
    return d;
}

}  // namespace

VoicePollDecision voiceTickIsPureQuery(const GuestMemory& mem, std::uint32_t self, std::uint32_t now_ms) {
    if (self == 0u || self >= kSelfLimit) return decline(VoicePollWhy::BadThis, 0);
    if (!mem.committed(self + kDeviceOffset, kSpan)) return decline(VoicePollWhy::BadThis, 0);
    // ★ 无设备 ⇒ 回退（保守门；纯查询支本身不读它，所以这条**只可能**让结果更保守）。
    if (mem.rd32(self + kDeviceOffset) == 0u) return decline(VoicePollWhy::NoDevice, 0);
    for (unsigned i = 0; i < kSlotCount; ++i) {
        const std::uint32_t slot = self + kSlotBase + i * kSlotStride;
        // ① `(slot[0x30] & 3) == 3` ⇒ 内层扫描：有查询、也可能写 ⇒ 不判。
        if ((mem.rd32(slot + kSlotMode) & 3u) == 3u) return decline(VoicePollWhy::InnerScan, i);
        // ② `slot[0] == 0` ⇒ 解释器在这一槽只有读 ⇒ 惰性。
        const std::uint32_t active = mem.rd32(slot + kSlotActive);
        if (active == 0u) continue;
        // ③ 已启动：首 tick 会写 `last = now` ⇒ 不判。
        const std::uint32_t last = mem.rd32(slot + kSlotLastTick);
        if (last == 0u) return decline(VoicePollWhy::FirstTick, i);
        // ④ 未到期 ⇒ 语义上纯、但指令数与 k 不符 ⇒ 仍然回退（见文件头）。
        //   `cmp ecx, [esi+18h]` / `jl` 是**有符号**比较（32 位差会回绕）⇒ 这里逐字照抄。
        const std::int32_t elapsed = static_cast<std::int32_t>(now_ms - last);
        const std::int32_t period = static_cast<std::int32_t>(mem.rd32(slot + kSlotPeriod));
        if (elapsed < period) return decline(VoicePollWhy::NotDue, i);
        return decline(VoicePollWhy::Due, i);
    }
    VoicePollDecision d;
    d.pure = true;
    d.why = VoicePollWhy::Pure;
    d.slot = kSlotCount;
    return d;
}

bool nativeVoiceTick4BBAB0(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t now_ms = mem.rd32(cpu.esp() + 4u);  // 唯一栈参（invoke 已校验过它在已提交页）
    const VoicePollDecision d = voiceTickIsPureQuery(mem, cpu.ecx(), now_ms);
    if (!d.pure) {
        if (cpu.current_override != nullptr) {
            ++cpu.current_override->declines[static_cast<std::size_t>(d.why)];
        }
        return false;  // ⇒ dispatcher 回退解释器（原字节照跑）
    }
    // ★ 时钟补偿（**判据**）：解释器跑完这条路径恰好 `kVoiceTickPureInsns` 条指令；dispatcher 在
    //   override 命中后自己会 `++cpu_.insns; clock_.tick()` ⇒ 这里只补 k-1（⛔ 不补 / 补多都会改掉
    //   下一次 `timeGetTime` 看到的 `now`）。insns 与时钟一起补，两者才与解释器一致。
    constexpr std::uint64_t kExtra = static_cast<std::uint64_t>(kVoiceTickPureInsns) - 1ull;
    if (cpu.clock != nullptr) cpu.clock->tick(kExtra);
    cpu.insns += kExtra;
    if (cpu.current_override != nullptr) ++cpu.current_override->fast_hits;
    *result = 0;  // EAX = 0：没有启动任何槽（解释器同路径的返回值）
    return true;
}

}  // namespace lift
