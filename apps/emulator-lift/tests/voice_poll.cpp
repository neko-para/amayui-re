// emulator-lift · `sub_4BBAB0`（`Voice::Tick(now)`）stateless 快路径的 pure 夹具
//
// 需求单 REQ-01M6XVZF3VDC5NSBJ20J2451VJ（"每帧固定税"）的 **A 案**判据。红了意味着什么：
//   ① 三槽全惰性 ⇒ override **接管**：EAX=0、**槽区逐字节不变**（零写）、解释器**没跑**（替身未写哨兵）。
//   ② 任一槽"已启动"（未到期 / 首 tick / 到期）⇒ **回退**：解释器真的跑了（替身写了哨兵），
//      而 override 自己**一个字节都没写**。
//   ④ 无设备（`[this+0x408] == 0`）⇒ 回退（即便三槽全惰性）。
//   ⑧ 回退时 override **不改写 guest 状态**（首 tick 的 `last = now` 不许被它写掉）。
//   ⑩ 读不到（this == 0 / this 指向未提交页）⇒ 回退，且不写 `*result`、不越界读。
//   ⑪ 稳态：连续多次调用返回值序列逐次相同（都 0），槽区逐字节不变。
//
// ★ 已删的判据（`AGENTS.md` §10：我方政策 / 我方计数器 / 我方文案，外部不可观测）：
//   ③/⑤ 内层扫描与"只看低 2 位"、⑥ 时钟补偿量（`kVoiceTickPureInsns`=60）、
//   ⑨ `declines[]` 分桶口径（值 = 桶下标）、⑫ 注册表形态（名字 / ABI / arg_bytes / 非 debug）。
//   统计口径的**行为面**仍由 ②/⑧/⑦·④ 的 `declines[...]` 顺带覆盖（每桶各一处）。
// ★ 本夹具**不**替用真机语义取证：替身函数只用来让"回退"可观测；被断言的是**判据**
//   （接管 / 回退 / 零写）。
// ★ 真机 A/B（墙钟 + 终态 hash）不在本档：那需要游戏安装（assets 档），本单不要求读数。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "runtime/native_overrides.h"

namespace {

// 被试 EA 与真机一致；替身函数放在**同一个 EA**，这样"回退 ⇒ 解释器执行它"是可观测的。
constexpr std::uint32_t kCalleeEa = 0x004BBAB0u;
constexpr std::uint32_t kSlotBase = 0x424;
constexpr std::uint32_t kSlotStride = 4;
constexpr std::uint32_t kDeviceOffset = 0x408;
constexpr std::uint32_t kSlotActive = 0x00;
constexpr std::uint32_t kSlotLastTick = 0x0C;
constexpr std::uint32_t kSlotPeriod = 0x18;
constexpr std::uint32_t kSlotMode = 0x30;
constexpr std::size_t kSlotRegion = 0x500;  // this .. this+0x500（含设备指针与三槽全部字段）

// `VoicePollWhy` 的桶下标在夹具里也写成常量：⛔ 不从被测实现里"借"枚举去比对（那会自我一致）。
constexpr std::size_t kWhyNoDevice = 1;
constexpr std::size_t kWhyInnerScan = 3;
constexpr std::size_t kWhyFirstTick = 4;
constexpr std::size_t kWhyNotDue = 5;
constexpr std::size_t kWhyDue = 6;

struct DirectResult {
    bool took = false;
    std::uint32_t result = 0xDEADBEEFu;
    std::uint64_t clock_delta = 0;
    std::uint64_t insn_delta = 0;
};

struct RunDelta {
    std::uint32_t eax = 0;
    std::uint64_t clock_delta = 0;
    std::uint64_t insn_delta = 0;
};

struct Harness {
    lift_fixture::Fixture fx;
    std::uint32_t self = 0;    // ECX（= T_Engine+0x148A0 的替身）
    std::uint32_t dev = 0;     // `[this+0x408]` 的设备指针（非 0 就够：快路径不读它的内容）
    std::uint32_t marker = 0;  // 替身函数写的哨兵（"解释器跑过"的机械判据）

    bool build() {
        if (!fx.ok) return false;
        dev = fx.u32data(0xDEAD0001u);
        marker = fx.u32data(0u);
        self = fx.zeroData(kSlotRegion);  // 槽区**全 0**（= 惰性）
        if (!fx.ok) return false;
        fx.m.mem().wr32(self + kDeviceOffset, dev);
        return true;
    }

    std::uint32_t slot(unsigned i) const { return self + kSlotBase + i * kSlotStride; }

    void setSlot(unsigned i, std::uint32_t active, std::uint32_t last, std::uint32_t period, std::uint32_t mode) {
        lift::GuestMemory& mem = fx.m.mem();
        mem.wr32(slot(i) + kSlotActive, active);
        mem.wr32(slot(i) + kSlotLastTick, last);
        mem.wr32(slot(i) + kSlotPeriod, period);
        mem.wr32(slot(i) + kSlotMode, mode);
    }
    void resetSlots() {
        for (unsigned i = 0; i < 3; ++i) setSlot(i, 0u, 0u, 0u, 0u);
    }

    // 槽区快照（0x500 字节）：用来断言"零写"（回退分支与快路径都不许改 guest）
    std::vector<std::uint8_t> snapshot() {
        std::vector<std::uint8_t> v(kSlotRegion);
        fx.m.mem().readBytes(self, v.data(), v.size());
        return v;
    }

    // 直接调 native 实现（不经 dispatcher）：用来断言"回退 / 接管"与补偿量 k-1。
    DirectResult callDirect(std::uint32_t now) {
        lift::X86& cpu = fx.m.cpu();
        lift::GuestMemory& mem = fx.m.mem();
        cpu.esp() = fx.m.layout().stack_top - 0x40u;
        mem.wr32(cpu.esp(), lift_fixture::kSentinel);  // [esp]     = 返回地址
        mem.wr32(cpu.esp() + 4u, now);                 // [esp + 4] = now（唯一栈参）
        cpu.ecx() = self;
        DirectResult d;
        std::uint32_t r = 0xDEADBEEFu;
        const std::uint64_t c0 = fx.m.clock().now100ns();
        const std::uint64_t i0 = cpu.insns;
        d.took = lift::nativeVoiceTick4BBAB0(cpu, mem, &r);
        d.result = r;
        d.clock_delta = fx.m.clock().now100ns() - c0;
        d.insn_delta = cpu.insns - i0;
        return d;
    }

    // 走 `NativeOverrideTable::invoke`（= dispatcher 的同一条路）：验证台账分桶。
    bool invokeTable(lift::NativeOverrideTable& table, std::uint32_t now) {
        lift::X86& cpu = fx.m.cpu();
        lift::GuestMemory& mem = fx.m.mem();
        cpu.esp() = fx.m.layout().stack_top - 0x40u;
        mem.wr32(cpu.esp(), lift_fixture::kSentinel);
        mem.wr32(cpu.esp() + 4u, now);
        cpu.ecx() = self;
        lift::NativeOverride* ov = table.find(kCalleeEa);
        if (ov == nullptr) return false;
        const char* why = nullptr;
        return table.invoke(*ov, cpu, mem, &why);
    }

    bool registerOverride() {
        return fx.m.overrides().add(kCalleeEa, "sub_4BBAB0", lift::GuestConv::Thiscall, 4,
                                    lift::nativeVoiceTick4BBAB0, &fx.err);
    }
    const lift::NativeOverride* entry() const { return fx.m.overrides().at(0); }

    // 替身：`mov [marker],1; mov eax,1; retn 4`（3 条指令）—— 回退时才被解释器执行。
    bool placeMarkerStub() {
        lift_fixture::Asm a(kCalleeEa);
        a.movMemImm(marker, 1u);
        a.movEaxImm(1u);
        a.retImm16(4u);
        return fx.m.placeCode(kCalleeEa, a.b.data(), a.b.size(), &fx.err);
    }
    // 替身：**恰好 60 条指令**的惰性函数（57 nop + 写 marker + `mov eax,0` + `retn 4`）。
    bool placeSixtyInsnStub() {
        lift_fixture::Asm a(kCalleeEa);
        for (int i = 0; i < 57; ++i) a.u8(0x90);  // nop
        a.movMemImm(marker, 1u);
        a.movEaxImm(0u);
        a.retImm16(4u);
        return fx.m.placeCode(kCalleeEa, a.b.data(), a.b.size(), &fx.err);
    }

    // 调一次 `sub_4BBAB0`：`mov ecx,self; push now; call 0x4BBAB0; ret`（哨兵）。
    std::uint32_t placeCaller(std::uint32_t now) {
        const std::uint32_t code_ea = fx.code_cursor;
        fx.code_cursor += 0x100u;
        lift_fixture::Asm a(code_ea);
        a.u8(0xB9);  // mov ecx, imm32
        a.u32(self);
        a.pushImm(now);
        a.callRel(kCalleeEa);
        a.ret();
        if (!fx.m.placeCode(code_ea, a.b.data(), a.b.size(), &fx.err)) {
            fx.ok = false;
            return 0;
        }
        fx.rearmStack();
        return code_ea;
    }

    RunDelta runOnce(std::uint32_t now) {
        const std::uint32_t code_ea = placeCaller(now);
        const std::uint64_t c0 = fx.m.clock().now100ns();
        const std::uint64_t i0 = fx.m.cpu().insns;
        fx.runCode(code_ea);
        RunDelta d;
        d.eax = fx.m.cpu().eax();
        d.clock_delta = fx.m.clock().now100ns() - c0;
        d.insn_delta = fx.m.cpu().insns - i0;
        return d;
    }
};

}  // namespace

int main() {
    // ---- ① 三槽全惰性 ⇒ 接管：EAX=0 / 零写 / 解释器没跑 / 台账 ----
    {
        Harness h;
        LIFT_CHECK(h.build());
        LIFT_CHECK(h.placeMarkerStub());
        LIFT_CHECK(h.registerOverride());

        // 直接调用：零写（槽区逐字节不变）+ 返回 0
        const std::vector<std::uint8_t> before = h.snapshot();
        const DirectResult d = h.callDirect(1000u);
        LIFT_CHECK(d.took);
        LIFT_CHECK_EQ_U32(d.result, 0u);
        LIFT_CHECK(h.snapshot() == before);

        // 走真 dispatcher：替身**没**跑（marker 仍 0）⇒ 那个调用被 override 吃掉了
        const RunDelta r = h.runOnce(1000u);
        LIFT_CHECK_EQ_U32(r.eax, 0u);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.marker, 0u);
        LIFT_CHECK(h.snapshot() == before);
        // callee-saved（EBX/ESI/EDI/EBP）必须原样：`retn 4` 之后调用方只应看到 EAX=0
        // （ECX/EDX/EFLAGS 是 call-clobbered，机制上不保证 —— 见 native_overrides.h 的 NativeFn 契约）。
        {
            lift::X86& cpu = h.fx.m.cpu();
            cpu.ebx() = 0x11111111u;
            cpu.esi() = 0x22222222u;
            cpu.edi() = 0x33333333u;
            cpu.ebp() = 0x44444444u;
            const RunDelta r2 = h.runOnce(1001u);
            LIFT_CHECK_EQ_U32(r2.eax, 0u);
            LIFT_CHECK_EQ_U32(cpu.ebx(), 0x11111111u);
            LIFT_CHECK_EQ_U32(cpu.esi(), 0x22222222u);
            LIFT_CHECK_EQ_U32(cpu.edi(), 0x33333333u);
            LIFT_CHECK_EQ_U32(cpu.ebp(), 0x44444444u);
        }
        LIFT_CHECK(h.entry() != nullptr);
        if (const lift::NativeOverride* e = h.entry()) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->hits), 2u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->fast_hits), 2u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->fallbacks), 0u);
        }
    }

    // ---- ⑪ 稳态：连续三次（now 递增）⇒ 返回值序列逐次相同，槽区逐字节不变 ----
    {
        Harness h;
        LIFT_CHECK(h.build());
        LIFT_CHECK(h.placeMarkerStub());
        LIFT_CHECK(h.registerOverride());
        const std::vector<std::uint8_t> before = h.snapshot();
        for (unsigned k = 0; k < 3; ++k) {
            const RunDelta r = h.runOnce(2000u + 17u * k);
            LIFT_CHECK_EQ_U32(r.eax, 0u);  // 逐次相同：都是 0（解释器同路径也返回 0）
            LIFT_CHECK(h.snapshot() == before);
        }
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.marker, 0u);
        if (const lift::NativeOverride* e = h.entry()) LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->hits), 3u);
    }

    // ---- ② 未到期（已启动、last != 0、elapsed < period）⇒ 回退，且 override 不写 guest ----
    {
        Harness h;
        LIFT_CHECK(h.build());
        LIFT_CHECK(h.placeMarkerStub());
        LIFT_CHECK(h.registerOverride());
        h.setSlot(0, 1u, 1000u, 60u, 0u);
        const std::vector<std::uint8_t> before = h.snapshot();
        const DirectResult d = h.callDirect(1030u);  // elapsed = 30 < 60
        LIFT_CHECK(!d.took);
        LIFT_CHECK_EQ_U32(d.result, 0xDEADBEEFu);  // 没写 *result
        LIFT_CHECK(h.snapshot() == before);        // 回退分支零写

        const RunDelta r = h.runOnce(1030u);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.marker, 1u);  // 解释器真的跑了（替身写了哨兵）
        LIFT_CHECK_EQ_U32(r.eax, 1u);                    // 替身的返回值 ⇒ 不是 override 的 0
        if (const lift::NativeOverride* e = h.entry()) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->fast_hits), 0u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->declines[kWhyNotDue]), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->fallbacks), 1u);
        }
    }

    // ---- ⑧ 首 tick（已启动且 last == 0）⇒ 回退，且 override **没有**写 `last = now` ----
    {
        Harness h;
        LIFT_CHECK(h.build());
        LIFT_CHECK(h.placeMarkerStub());
        LIFT_CHECK(h.registerOverride());
        h.setSlot(1, 1u, 0u, 60u, 0u);
        const std::vector<std::uint8_t> before = h.snapshot();
        const DirectResult d = h.callDirect(1060u);
        LIFT_CHECK(!d.took);
        LIFT_CHECK(h.snapshot() == before);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.slot(1) + kSlotLastTick, 0u);  // ⛔ 没被写成 now

        // 走 dispatcher：解释器真的跑了（哨兵），分桶记在 FirstTick 上
        const RunDelta r = h.runOnce(1060u);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.marker, 1u);
        LIFT_CHECK_EQ_U32(r.eax, 1u);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.slot(1) + kSlotLastTick, 0u);
        if (const lift::NativeOverride* e = h.entry()) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->declines[kWhyFirstTick]), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->fast_hits), 0u);
        }
    }

    // ---- ⑦/④ 无设备（`[this+0x408] == 0`）⇒ 回退，即便三槽全惰性 ----
    {
        Harness h;
        LIFT_CHECK(h.build());
        LIFT_CHECK(h.placeMarkerStub());
        LIFT_CHECK(h.registerOverride());
        h.fx.m.mem().wr32(h.self + kDeviceOffset, 0u);
        const std::vector<std::uint8_t> before = h.snapshot();
        const DirectResult d = h.callDirect(1000u);
        LIFT_CHECK(!d.took);
        LIFT_CHECK(h.snapshot() == before);
        const RunDelta r = h.runOnce(1000u);
        LIFT_CHECK_MEM_U32(h.fx.m.mem(), h.marker, 1u);
        LIFT_CHECK_EQ_U32(r.eax, 1u);
        if (const lift::NativeOverride* e = h.entry()) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e->declines[kWhyNoDevice]), 1u);
        }
    }

    // ---- ⑩ 读不到：this == 0 / this 指向未提交页 ⇒ 回退（不写 *result、不越界读）----
    {
        Harness h;
        LIFT_CHECK(h.build());
        const std::uint32_t saved = h.self;

        h.self = 0u;
        DirectResult d = h.callDirect(1000u);
        LIFT_CHECK(!d.took);
        LIFT_CHECK_EQ_U32(d.result, 0xDEADBEEFu);

        h.self = 0x00001000u;  // 未提交（同 tests/native_override.cpp 的"未提交栈帧"用例）
        LIFT_CHECK(!h.fx.m.mem().committed(h.self, 4));
        d = h.callDirect(1000u);
        LIFT_CHECK(!d.took);
        LIFT_CHECK_EQ_U32(d.result, 0xDEADBEEFu);

        h.self = saved;
        LIFT_CHECK(h.callDirect(1000u).took);  // 复原之后仍然接管
    }

    return lift_test::finish("lift-voice-poll");
}
