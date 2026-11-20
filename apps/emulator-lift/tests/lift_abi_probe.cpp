// emulator-lift · Shape A 原型阶段 A 的 ABI 探针（pure 档，最外层 add_test；**不碰 clang / `.ll`**）
//
// 判据（红了意味着什么）：
//   ① **布局**：`LiftRegs` 是 `.ll` 硬编码的那一份（sizeof == 36、r 在 +0、eflags 在 +32）；
//      `X86` 的 r/eflags/eip 在 +0/+32/+36、r 是 32 B。★ A′ 之后前 36 B **逐字节同布局**
//      （`LiftRegs{r,eflags}` 恰好覆盖到 `eip`）—— 这是 AOT 把 `%st` 指到 `cpu_.r[0]` 的
//      结构前提；搬运函数仍然逐字段写（正确性不押在布局上）。
//   ② **搬运**：先把 X86 置 8 个互不相同的值 + eflags，`liftCpuToRegs`；
//      再把 X86 清零、`liftRegsToCpu` —— 8 个寄存器与 eflags 全部按字段回得来。
//      （⛔ 不用 memcpy/offsetof 当正确性依据：这条用例只认逐字段对应。）
//   ③ **SMC 钩子真的接上了**：`lift_note_write` 写**已解码的代码页** ⇒ 下一次
//      `BlockCache::get()` 重新解码（`misses()` +1）；写**数据页** ⇒ `misses()` 不动。
//      这条堵的是"ABI 函数存在但没转发到 noteGuestWrite"（那会让 lifted 的写永不失效块缓存）。
//
// ★ 为什么它是独立的 CTest：ABI 是 Shape A 里唯一"编译期布局 + 运行期转发"同时成立的东西，
//   而 `.ll` 用例可能因缺 clang 而 SKIP(77) —— ABI 这条必须**每次都跑**（顶层无条件注册 = pure）。
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "check.h"
#include "fixture.h"
#include "runtime/lift_abi.h"
#include "runtime/machine.h"
#include "runtime/x86.h"

namespace {

// 8 个寄存器 + eflags 各取一个**互不相同**的位型：任何一处串字段都会让至少一个比较落空。
constexpr std::uint32_t kRegSeed[8] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
                                       0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
constexpr std::uint32_t kFlagsSeed = 0x00000A5Au;  // 含 CF/PF/AF/ZF/SF/OF 的混合位型（只作位型用）

}  // namespace

int main() {
    using namespace lift_test;

    g_current_case = "layout";
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sizeof(::LiftRegs)), 36u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(::LiftRegs, r)), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(::LiftRegs, eflags)), 32u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(lift::X86, r)), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(lift::X86, eflags)), 32u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(lift::X86, eip)), 36u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sizeof(lift::X86::r)), 32u);
    // ★ A′ 防重叠：`LiftRegs`（r + eflags = 36 B）恰好覆盖到 `eip`，`eip` 再 4 B 到 `seg`。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(lift::X86, r) + sizeof(::LiftRegs)),
                      static_cast<std::uint32_t>(offsetof(lift::X86, eip)));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(offsetof(lift::X86, r) + sizeof(::LiftRegs) + 4u),
                      static_cast<std::uint32_t>(offsetof(lift::X86, seg)));

    g_current_case = "regs-roundtrip";
    {
        lift::X86 cpu;
        for (int i = 0; i < 8; ++i) cpu.r[i] = kRegSeed[i];
        cpu.eflags = kFlagsSeed;
        ::LiftRegs st;
        lift::liftCpuToRegs(cpu, st);
        for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(st.r[i], kRegSeed[i]);
        LIFT_CHECK_EQ_U32(st.eflags, kFlagsSeed);
        // 反向：清零后再搬回来
        lift::X86 cpu2;
        for (int i = 0; i < 8; ++i) cpu2.r[i] = 0;
        cpu2.eflags = 0;
        lift::liftRegsToCpu(st, cpu2);
        for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(cpu2.r[i], kRegSeed[i]);
        LIFT_CHECK_EQ_U32(cpu2.eflags, kFlagsSeed);
    }

    g_current_case = "note-write-smc";
    {
        lift_fixture::Fixture fx;
        LIFT_CHECK(fx.ok);
        if (fx.ok) {
            // `nop; ret`：跑一遍让块缓存里真的有这个 EA 的解码块（代码页被标记）
            const std::uint8_t code[] = {0x90, 0xC3};
            const std::uint32_t code_ea = lift_fixture::kCodeBase;
            LIFT_CHECK(fx.m.placeCode(code_ea, code, sizeof(code), &fx.err));
            fx.rearmStack();
            fx.m.cpu().eip = code_ea;
            lift::RunOptions o;
            o.until_ea = lift_fixture::kSentinel;
            o.max_insns = 100;
            const lift::RunResult r = fx.m.run(o);
            LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
            LIFT_CHECK(fx.m.cache().peek(code_ea) != nullptr);
            const std::uint32_t misses0 = static_cast<std::uint32_t>(fx.m.cache().misses());

            // 写**数据页**（这个 EA 从来没被解码成块）⇒ 不失效、下一次 get 是命中
            ::lift_note_write(&fx.m.cpu(), 0x00410000u, 4);
            LIFT_CHECK(fx.m.cache().get(code_ea) != nullptr);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fx.m.cache().misses()), misses0);

            // 写**已解码的代码页** ⇒ 失效、下一次 get 重新解码（misses +1）
            ::lift_note_write(&fx.m.cpu(), code_ea, 1);
            LIFT_CHECK(fx.m.cache().get(code_ea) != nullptr);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fx.m.cache().misses()), misses0 + 1u);
        }
    }

    return lift_test::finish("lift-abi-probe");
}
