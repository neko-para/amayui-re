// emulator-lift · 计时族的多媒体定时器（pure：guest 侧自造循环 + 回调，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "计时族"）：`timeSetEvent` / `timeKillEvent` / `timeGetDevCaps` 不再是 stub，
// 而且**回调真的会跑**：
//   ① 一次性定时器：虚拟时钟走过 uDelay 后，dispatcher 在块边界把控制权交给 guest 回调；
//      回调收到的 `uID` 就是 timeSetEvent 的返回值；
//   ② 周期定时器：会**重复**触发（≥2 次）；`timeKillEvent` 之后不再触发；
//   ③ `timeGetDevCaps` 填 TIMECAPS（8 字节），非法参数 ⇒ MMSYSERR_INVALPARAM(11)。
// ★ 这条同时是**协作式定时器**（x86.h 的 GuestTimer / kTimerCallbackReturnEa）的唯一守卫：
//   续延点、恢复 esp/eip、周期续约任何一处写错，这里都会红。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;

constexpr std::uint32_t kTimePeriodic = 0x00000001u;
constexpr std::uint32_t kMmsysErrNoError = 0u;
constexpr std::uint32_t kMmsysErrInvalidParam = 11u;

// 主程序：一段**有界**的忙循环（每次迭代都跨块边界 ⇒ dispatcher 每 2 条指令查一次定时器）
void emitBusyLoop(Asm& a, std::uint32_t iterations) {
    a.u8(0xB9);              // mov ecx, imm32
    a.u32(iterations);
    const std::uint32_t dec_ea = a.currentEa();
    a.u8(0x49);              // dec ecx
    a.u8(0x75);              // jnz rel8
    a.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(dec_ea - (a.currentEa() + 1))));
    a.ret();
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① 一次性定时器：1 次回调，参数 uID 正确
    {
        g_current_case = "time-set-event-oneshot";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t set_event = f.use("winmm", "timeSetEvent");
        const std::uint32_t kill_event = f.use("winmm", "timeKillEvent");

        const std::uint32_t count_ea = f.zeroData(4);
        const std::uint32_t uid_ea = f.zeroData(4);
        const std::uint32_t cb_ea = 0x00408000u;
        {
            // 回调：记 uID（[esp+4]）并计数；`ret`（我们恢复 esp，不依赖 ret 20）
            Asm a(cb_ea);
            a.u8(0x8B); a.u8(0x44); a.u8(0x24); a.u8(0x04);  // mov eax,[esp+4]
            a.u8(0xA3); a.u32(uid_ea);                       // mov [uid],eax
            a.u8(0xFF); a.u8(0x05); a.u32(count_ea);         // inc dword [count]
            a.ret();
            LIFT_CHECK(f.m.placeCode(cb_ea, a.b.data(), a.b.size(), &f.err));
        }
        // 登记：5 ms 后一次性触发
        const std::uint32_t id = f.call(set_event, {5u, 0u, cb_ea, 0x1234u, 0u});
        LIFT_CHECK_NE_U32(id, 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), count_ea, 0u);

        // 忙循环 ≈ 120000 条指令 = 12 ms（> 5 ms）⇒ 恰好一次
        const std::uint32_t main_ea = lift_fixture::kCodeBase + 0x800u;
        {
            Asm a(main_ea);
            emitBusyLoop(a, 60000u);
            LIFT_CHECK(f.m.placeCode(main_ea, a.b.data(), a.b.size(), &f.err));
        }
        f.rearmStack();
        const lift::RunResult r = f.runCode(main_ea);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_MEM_U32(f.m.mem(), count_ea, 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), uid_ea, id);
        // 一次性定时器已经消耗掉 ⇒ 再 kill 它 ⇒ MMSYSERR_INVALPARAM
        LIFT_CHECK_EQ_U32(f.call(kill_event, {id}), kMmsysErrInvalidParam);
        LIFT_CHECK(f.ok);
    }

    // ② 周期定时器：重复触发；kill 之后不再触发
    {
        g_current_case = "time-set-event-periodic-and-kill";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t set_event = f.use("winmm", "timeSetEvent");
        const std::uint32_t kill_event = f.use("winmm", "timeKillEvent");
        const std::uint32_t count_ea = f.zeroData(4);
        const std::uint32_t cb_ea = 0x00408000u;
        {
            Asm a(cb_ea);
            a.u8(0xFF); a.u8(0x05); a.u32(count_ea);  // inc dword [count]
            a.ret();
            LIFT_CHECK(f.m.placeCode(cb_ea, a.b.data(), a.b.size(), &f.err));
        }
        const std::uint32_t id = f.call(set_event, {1u, 0u, cb_ea, 0u, kTimePeriodic});
        LIFT_CHECK_NE_U32(id, 0u);

        const std::uint32_t main_ea = lift_fixture::kCodeBase + 0x800u;
        {
            Asm a(main_ea);
            emitBusyLoop(a, 30000u);  // 60000 条指令 = 6 ms ⇒ 周期 1 ms ⇒ 若干次
            LIFT_CHECK(f.m.placeCode(main_ea, a.b.data(), a.b.size(), &f.err));
        }
        f.rearmStack();
        const lift::RunResult r = f.runCode(main_ea);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        const std::uint32_t fired = f.m.mem().rd32(count_ea);
        LIFT_CHECK(fired >= 2u);   // 周期 ⇒ 至少两次
        LIFT_CHECK(fired <= 10u);  // 有界（不是"每次块边界都触发"）
        LIFT_CHECK_EQ_U32(f.call(kill_event, {id}), kMmsysErrNoError);

        // kill 之后再跑一段：计数不再增长
        const std::uint32_t main2_ea = lift_fixture::kCodeBase + 0xC00u;
        {
            Asm a(main2_ea);
            emitBusyLoop(a, 30000u);
            LIFT_CHECK(f.m.placeCode(main2_ea, a.b.data(), a.b.size(), &f.err));
        }
        f.rearmStack();
        f.runCode(main2_ea);
        LIFT_CHECK_MEM_U32(f.m.mem(), count_ea, fired);
        LIFT_CHECK(f.ok);
    }

    // ③ timeGetDevCaps：填 TIMECAPS；非法参数 ⇒ 11
    {
        g_current_case = "time-get-dev-caps";
        Fixture f;
        lift::registerTimeShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t caps = f.use("winmm", "timeGetDevCaps");
        // 8 字节结构：TIMECAPS { wPeriodMin; wPeriodMax; }
        const std::uint32_t tc = f.zeroData(8);
        LIFT_CHECK_EQ_U32(f.call(caps, {tc, 8u}), kMmsysErrNoError);
        const std::uint32_t min_period = f.m.mem().rd32(tc);
        const std::uint32_t max_period = f.m.mem().rd32(tc + 4u);
        LIFT_CHECK_EQ_U32(min_period, 1u);
        LIFT_CHECK_EQ_U32(max_period, 1000000u);
        LIFT_CHECK_EQ_U32(f.call(caps, {tc, 7u}), kMmsysErrInvalidParam);
        LIFT_CHECK_EQ_U32(f.call(caps, {0u, 8u}), kMmsysErrInvalidParam);
        LIFT_CHECK(f.ok);
    }

    return finish("timer-ext");
}
