// emulator-lift · native override：段级：sub_414AC0 的填池循环
//
// ★ entry 0x415384 / resume 0x41539F；只在 EAX==0（`xor eax,eax` 之后）接手，否则 Declined。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// ---------------------------------------------------------------------------------------
// 段级：`sub_414AC0` 的填池循环（`.lst` 0x415384..0x41539D）
//
// ★ 逐字依据（`pnpm tools disasm-at at --ea 0x415340 --lines 45`）：
//     xor  eax,eax                 ; 0x41537E 循环前
//     test edx,edx / jle 0x41539F  ; 0x415380（count<=0 时整段跳过）
//   loc_415384:
//     mov  ecx,[esi-10h]           ; 目标数组（前面 VirtualAlloc 的结果）
//     mov  edx,[ebx+5EC90h]        ; 填充值
//     mov  [ecx+eax*4],edx
//     mov  ecx,[ebx+5D7E8h]
//     inc  eax
//     lea  edx,[ecx+edi+1]         ; count = [ebx+5D7E8] + edi + 1
//     cmp  eax,edx
//     jl   loc_415384              ; **唯一**回边（全语料只有这里引用 0x415384）
//   loc_41539F:
//     mov  eax,[ebx+5D7ECh]        ; 循环后不读 EFLAGS
// ★ `0x415384` 只被 `sub_414AC0` 的这次循环引用（`Select-String 415384` 只有 1 条 `jl`），
//   而 `sub_414AC0` 只有一个调用方 `_WinMain@16` ⇒ 按 EIP 拦是安全的（用户前提：guest 代码只读）。
// ---------------------------------------------------------------------------------------
BlockOverrideOutcome nativeFillPool414AC0(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns) {
    *replaced_insns = 0;
    // 段入口判据：`xor eax,eax` 之后才是 0；回边（`jl`）进来时 EAX>=1 ⇒ 只尝试一次。
    if (cpu.eax() != 0) return BlockOverrideOutcome::Declined;
    const std::uint32_t esi = cpu.esi();
    const std::uint32_t ebx = cpu.ebx();
    const std::uint32_t edi = cpu.edi();
    if (esi < 0x10u) return BlockOverrideOutcome::Declined;
    if (static_cast<std::uint64_t>(ebx) + 0x5EC94u > GuestMemory::kSpace) return BlockOverrideOutcome::Declined;
    if (!mem.committed(esi - 0x10u, 4u) || !mem.committed(ebx + 0x5D7E8u, 4u) ||
        !mem.committed(ebx + 0x5EC90u, 4u))
        return BlockOverrideOutcome::Declined;
    const std::uint32_t target = mem.rd32(esi - 0x10u);
    const std::uint32_t value = mem.rd32(ebx + 0x5EC90u);
    const std::uint32_t base = mem.rd32(ebx + 0x5D7E8u);
    const std::uint32_t count_u = base + edi + 1u;  // guest 的 `lea edx,[ecx+edi+1]` 是 32 位回绕
    const std::int32_t count = static_cast<std::int32_t>(count_u);
    if (count <= 0) return BlockOverrideOutcome::Declined;  // `test edx,edx; jle 0x41539F` ⇒ 不执行循环
    const std::uint64_t bytes = static_cast<std::uint64_t>(static_cast<std::uint32_t>(count)) * 4u;
    if (target == 0) return BlockOverrideOutcome::Failed;
    if (static_cast<std::uint64_t>(target) + bytes > GuestMemory::kSpace) return BlockOverrideOutcome::Failed;
    if (!mem.committed(target, static_cast<std::size_t>(bytes))) return BlockOverrideOutcome::Failed;
    std::uint8_t* p = mem.host(target);
    for (std::int32_t i = 0; i < count; ++i) {
        std::memcpy(p + 4ull * static_cast<std::uint32_t>(i), &value, 4);
    }
    // 循环尾的 guest 状态（EAX=EDX=count，ECX=[ebx+5D7E8]；EFLAGS = `cmp eax,edx` 相等：
    // ZF=1 / PF=1 / CF=AF=SF=OF=0；0x41539F 之后不读标志，但差分测试会逐位比）。
    cpu.eax() = count_u;
    cpu.edx() = count_u;
    cpu.ecx() = base;
    cpu.eflags = (cpu.eflags & ~(FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)) | FLAG_PF | FLAG_ZF;
    cpu.eip = 0x0041539Fu;
    // 每次迭代 = 8 条指令（mov/mov/mov/mov/inc/lea/cmp/jl，最后那次 jl 不跳也算执行）
    *replaced_insns = 8ull * static_cast<std::uint64_t>(count_u);
    return BlockOverrideOutcome::Replaced;
}

}  // namespace lift
