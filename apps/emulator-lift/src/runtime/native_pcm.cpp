// emulator-lift · native override：段级 · `sub_48D610`（Ogg 解码）的 PCM 转换循环
//
// ★ entry `0x48D767` / resume `0x48D89A`（锚 = `.lst` 指令起点；⛔ 不用 `.c` 行号）。
//   REQ-01M75W6BB3PY3MF42ZTS23KTBS（取证：`.tmp/audio-token/feeder-seg-analysis.md` §4）。
// ★ 段体（单入口单出口；段内两个条件跳转都落在段内，唯一回边 `0x48D894 jl 0x48D767`）：
//     0x48D767  mov  eax, [ebp+var_118]      ; 本通道 f32 源缓冲
//     0x48D76D  fld  dword ptr [eax+edi*4]   ; 每样本一次（G7 的"空闲槽 ≥2"就是给它 + ftol2 的）
//     0x48D770  fmul st, st(1)               ; ST(1) = 32767.0（段外 0x48D738 fld dbl_5291B8）
//     0x48D772  call __ftol2_sse             ; EDX:EAX = 64 位**截断**
//     0x48D777  cmp  eax, 7FFFh / jle        ; 低 32 位**有符号**上界 clamp
//     0x48D86B  cmp  eax, 0FFFF8000h / jge   ; 低 32 位**有符号**下界 clamp
//     0x48D877  mov  dword ptr [ebp+var_108], 1   ; ★ 存 1（不是 `or`；段内不重置 —— 清 0 在 0x48D70E）
//     0x48D881  mov  [ebx], ax               ; ★ 每样本只写 2 字节（步长 2*ch）
//     0x48D884  mov  ecx, [esi+25C4h]        ; ch
//     0x48D88A  inc  edi
//     0x48D88B  lea  ebx, [ebx+ecx*2]
//     0x48D88E  cmp  edi, [esi+26D8h]        ; ns
//     0x48D894  jl   0x48D767
//     0x48D89A  mov  eax, [ebp+var_114]      ; ← resume（段出口；由本 fn 写 EIP）
// ★ 为什么值得替换：`__ftol2_sse` 是本机单句占 VOICE ~10% 的热点，而它的**唯一**大调用方就是
//   这段每样本一次的循环（385 个 call 站点里 Ogg 解码这一类占大头）。段级替换把"每样本一次
//   dispatcher + invoke + 两次 softfloat"收成一次段入口。
//
// ★ 闸门 G1–G10（全部在**任何副作用之前**；G1/G2/G5/G6/G9 都先做 `committed()`）：
//   G1 `[ebp-118h]` / `[ebp-108h]` 已提交（源指针槽 / clip 标志；后者只在 clamp 时写）。
//   G2 `[esi+25C4h]` / `[esi+26D8h]` 已提交（ch / ns）。
//   G3 `1 <= ch <= 8`：`<= 8` 是**保守上限**（段内没有这条判据；`1 <= ch` 来自段外 `0x48D72C jle`）。
//   G4 `edi == 0`（写死）：入口 EDI 只可能是 0（`0x48D753 xor edi,edi`）。若真支持 `edi>0`，
//      `n` 必须是 `ns-edi`、`15*n` 也一起变 ⇒ 不在这里放宽。
//   G5 源跨度 `4*ns` 已提交（`[eax+edi*4]`，edi ∈ [0,ns)）。
//   G6 目标跨度 `2*ch*(n-1)+2` 已提交（**不是** `2*ch*n`：最后一次 `lea` 之后不再写）。
//   G7 `ST(0)` 的 ext80 位型 == `x87FromDouble(32767.0)` ∧ 空闲槽 ≥ 2（段内每样本还有一次 `fld`）。
//   G8 `[0x55F2E8] == 0` 且已提交（= `__ftol2` 走 x87 支而不是 `cvttsd2si`；与 `native_crt.cpp` 同判据）。
//   G9 栈 scratch `[align16(esp_seg-0x28), +0x20)` 已提交：那次 `call __ftol2_sse` 会压返回地址
//      ⇒ `__ftol2` 自己入口的 esp = `esp_seg-4`，它的 `frame = (esp-0x24)&~15`
//      ⇒ 等价于 `(esp_seg-0x28)&~15`。★ 照单里直接用 `esp_seg` 会**偏 4 字节**。
//   G10 "段内无 shim" + `__ftol2_sse` 必须在**函数级表**里：段内唯一 call 是 guest 函数 `0x4E82C0`
//      （不是 shim：shim 桩只落在 stub 页），它被本实现**内联**掉 ⇒ 不产生 call 帧。可机械核对的那一半
//      是 `15*n` 的依赖：每样本的 +1 来自 `__ftol2_sse` 函数级 override 自己 `++cpu_.insns`
//      （`machine.cpp` 的 `++cpu_.insns`）⇒ 两臂都必须启用它，否则解释器臂每样本 ~14+40 条而
//      段级只报 15 ⇒ 静默少走时钟。`find()` 只读不可变字段（`ea` / `count_`），feeder 里读它安全。
//
// ★ **⛔ 不调 `nativeFtol2Sse`**：它有 6 个 per-value decline 出口（`FtolWhy`）⇒ 要么两遍（先探
//   再算）、要么在段内回退一半；而"两遍"在 src/dst 重叠时与"读一条写一条"分叉。这里**一遍、升序、
//   读一条写一条**，算术与解释器**逐指令同源**（`x87MemToReg` → `x87Mul` → `x87ToI64` +
//   `x87RegToMem`，与 `exec_fpu.cpp` / `nativeFtol2Sse` 是同一批桥函数）：
//     · 乘数 = 32767.0（`.lst` `0x5291B8 dq 32767.0`；⛔ 不是 32768）。乘积在 ext80 里**恒精确**
//       （≤ 24+15 位有效位）⇒ `fmul` 不置 PE；但**不**用"`double p = f*32767.0` 再 `x87FromDouble`"
//       那个更短的形式：那句"恒精确"只在控制字精度 ≥ 53 位时成立，而 guest 里有 94 条 `fldcw`
//       （PC=24 时 guest 的 `fmul` 会舍入 ⇒ 两条臂分叉）。`x87Mul` 让控制字 / NaN 载荷 / sNaN
//       语义全由 SoftFloat 给 —— 与解释器同一条路。
//     · `x87ToI64(truncate)` = `__ftol2` 的 `fistp` 那条路（解释器口径）；
//     · **必须**做一次 `fst dword` 等价转换（`x87RegToMem(prod, scratch, 4)`）：它同时给出
//       PE/UE/OE（**PE 只来自 ftol2 的 f32 转换**——省掉这一步状态字必红）与 guest 可见的那
//       4 字节 `[E+18h,E+1Ch)`。
//     · x87 净不变：段外一次 `fld dbl`（ST(0)=32767.0）+ 段内每样本一次 `fld dword` + ftol2 弹回
//       ⇒ 本实现不 push/pop（净效果相同；被用的那个物理槽在入口就是 free ⇒ tag 位也不变）。
// ★ `replaced_insns = 15*n + n_clip_hi + 2*n_clip_lo`（**不是** `15*n + n_clip`；逐条数见头文件）。
// ★ 出口寄存器/EFLAGS 是**死值**（`0x48D89A` 覆盖 EAX、`0x48D8A0` 恢复 EBX、`0x48D753` 覆盖 EDI、
//   `0x48D74A/750` 覆盖 ECX/EDX、`inc eax` 覆盖标志）⇒ 写全是为了与解释器**逐位对齐**（伴随断言），
//   不是产品语义。活出口只有：目标内存 / `[ebp-108h]` / `fpu.status`（+ 栈 scratch）。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/fpu_bridge.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// 乘数（`.lst` 0x48D738 `fld dbl_5291B8` ← `0x5291B8 dq 32767.0`）。
// ★ G7 用**字面量**校验 guest 的 `ST(0)`（那是 guest 自己压进来的值），这一行是"乘数"这个参数本身；
//   两者语义上必须是同一个数，但**分开写**是有意的：变异测试要能只改乘数（G7 仍然通过 ⇒ 值表差分
//   才会红），而不是让 G7 把变异当成"形状不对"而 Declined（那红得没意义）。
// ★ 这是**唯一的乘数**：`x87FromDouble(kPcmScale, scale80)` 之后每样本 `x87Mul(sample80, scale80)`。
constexpr double kPcmScale = 32767.0;

// `[0x55F2E8]`：CRT 的 x87→SSE 选路开关（唯一写者 `0x4F1117`）。非 0 ⇒ 真机走 `cvttsd2si` ⇒ 交解释器。
constexpr std::uint32_t kSse2FlagEa = 0x0055F2E8u;

// `__ftol2_sse` 的入口 EA（G10 的依赖判据：`15*n` 里的 +1 来自它的函数级 override）。
constexpr std::uint32_t kFtol2Ea = 0x004E82C0u;

// `sub_48D610` 的段出口（Replaced 之后 EIP 落这里）。
constexpr std::uint32_t kResumeEa = 0x0048D89Au;

}  // namespace

BlockOverrideOutcome nativePcm48D767(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns) {
    *replaced_insns = 0;

    // ── ① 闸门（全部在任何副作用之前）────────────────────────────────────────────────────
    // G1 `[ebp-118h]`（源指针槽）/ `[ebp-108h]`（clip 标志）。
    const std::uint32_t ebp = cpu.ebp();
    if (ebp < 0x118u) return BlockOverrideOutcome::Declined;
    if (!mem.committed(ebp - 0x118u, 4u)) return BlockOverrideOutcome::Declined;
    if (!mem.committed(ebp - 0x108u, 4u)) return BlockOverrideOutcome::Declined;

    // G2 `[esi+25C4h]`（ch）/ `[esi+26D8h]`（ns）。
    const std::uint32_t esi = cpu.esi();
    if (static_cast<std::uint64_t>(esi) + 0x26DCu > GuestMemory::kSpace) return BlockOverrideOutcome::Declined;
    if (!mem.committed(esi + 0x25C4u, 4u) || !mem.committed(esi + 0x26D8u, 4u))
        return BlockOverrideOutcome::Declined;

    // G3 `1 <= ch <= 8`。
    const std::uint32_t ch = mem.rd32(esi + 0x25C4u);
    if (ch < 1u || ch > 8u) return BlockOverrideOutcome::Declined;

    // G4 `edi == 0`（写死；见头注）。
    if (cpu.edi() != 0u) return BlockOverrideOutcome::Declined;

    // G5 ns（`0x48D75B cmp [esi+26D8h],edi / jle` 保证进来时 > 0）+ 源跨度 `4*ns`。
    const std::int32_t ns = static_cast<std::int32_t>(mem.rd32(esi + 0x26D8u));
    if (ns <= 0) return BlockOverrideOutcome::Declined;
    const std::uint32_t src = mem.rd32(ebp - 0x118u);
    const std::uint64_t src_bytes = 4ull * static_cast<std::uint32_t>(ns);
    if (static_cast<std::uint64_t>(src) + src_bytes > GuestMemory::kSpace) return BlockOverrideOutcome::Declined;
    if (!mem.committed(src, static_cast<std::size_t>(src_bytes))) return BlockOverrideOutcome::Declined;

    // G6 目标跨度 `2*ch*(ns-1)+2`（不是 `2*ch*ns`）。
    const std::uint32_t dst = cpu.ebx();
    const std::uint64_t dst_bytes =
        2ull * ch * (static_cast<std::uint64_t>(ns) - 1ull) + 2ull;
    if (static_cast<std::uint64_t>(dst) + dst_bytes > GuestMemory::kSpace)
        return BlockOverrideOutcome::Declined;
    if (!mem.committed(dst, static_cast<std::size_t>(dst_bytes))) return BlockOverrideOutcome::Declined;

    // G7 `ST(0)` 的 ext80 位型 == 32767.0 ∧ 空闲槽 ≥ 2。
    //   ★ 判据用**物理槽**（`empty(phys(0))`，⛔ 不是 `empty(0)`）。
    //   ★ `x87FromDouble(32767.0)` 是**精确**转换（无舍入 ⇒ 不置任何异常位），所以它放在闸门里
    //     不会给 `fpu.status` 留副作用。
    if (cpu.fpu.empty(cpu.fpu.phys(0))) return BlockOverrideOutcome::Declined;
    {
        Ext80 want;
        x87FromDouble(32767.0, want);
        if (std::memcmp(cpu.fpu.regs[cpu.fpu.phys(0)], want, 10) != 0) return BlockOverrideOutcome::Declined;
        unsigned used = 0;
        for (int i = 0; i < 8; ++i)
            if (!cpu.fpu.empty(i)) ++used;
        if (8u - used < 2u) return BlockOverrideOutcome::Declined;
    }

    // G8 `[0x55F2E8] == 0`（非 0 ⇒ 真机走 `cvttsd2si` ⇒ 只写 EAX、越界语义不同）。
    if (!mem.committed(kSse2FlagEa, 4u)) return BlockOverrideOutcome::Declined;
    if (mem.rd32(kSse2FlagEa) != 0u) return BlockOverrideOutcome::Declined;

    // G9 `__ftol2` 的栈 scratch：`(esp_seg-0x28)&~15 .. +0x20`。
    const std::uint32_t frame = (cpu.esp() - 0x28u) & ~15u;
    if (static_cast<std::uint64_t>(frame) + 0x20u > GuestMemory::kSpace) return BlockOverrideOutcome::Declined;
    if (!mem.committed(frame, 0x20u)) return BlockOverrideOutcome::Declined;

    // G10 段内无 shim + `__ftol2_sse` 在函数级表里（见头注）。`find()` 只读不可变字段 ⇒ 线程安全。
    if (cpu.native_overrides == nullptr || cpu.native_overrides->find(kFtol2Ea) == nullptr)
        return BlockOverrideOutcome::Declined;

    // ── ② 从这里开始不再回退 ────────────────────────────────────────────────────────────
    // 解释器在每条 FP 指令之前都做同一件事（`exec_fpu.cpp`）；段内不改控制字 ⇒ 做一次就够。
    // ★ 算术**逐指令复刻解释器**（`fld` → `fmul` → `__ftol2`）：`x87MemToReg` / `x87Mul` /
    //   `x87ToI64` / `x87RegToMem` 与 `exec_fpu.cpp` + `nativeFtol2Sse` 是**同一批**桥函数。
    //   为什么不用"`double p = f*32767.0` 再 `x87FromDouble`"那个更短的形式：乘积虽然恒精确
    //   （≤ 24+15 位有效位），但那句"恒精确"只在**控制字精度 ≥ 53 位**时成立；guest 里有 94 条
    //   `fldcw`（`0x4046B2` / `0x408206` / `0x44A6DB` …），PC=24（单精度）时 guest 的 `fmul` 会
    //   把乘积舍入到 24 位 ⇒ 两条臂分叉。走 `x87Mul` 后控制字 / NaN 载荷 / sNaN 语义全部由
    //   SoftFloat 给，和解释器**同一条路**（乘数常量见 `kPcmScale`）。
    x87ApplyControlWord(cpu.fpu.control);
    Ext80 scale80;                    // = 段外 `0x48D738 fld dbl_5291B8`（G7 已核 guest 的 ST(0) 就是它）
    x87FromDouble(kPcmScale, scale80);

    const std::uint8_t* s = mem.host(src);
    std::uint8_t* d = mem.host(dst);
    // `__ftol2` 的 `fst dword [esp+18h]` 落点（G9 已核）：guest 可见的 4 字节 scratch。
    std::uint8_t* const ftol_scratch = mem.host(frame + 0x18u);

    std::uint64_t n_clip_hi = 0;
    std::uint64_t n_clip_lo = 0;
    std::uint32_t last_eax = 0;  // 末个 clamp 之后留在 EAX 的值（死值，伴随断言用）
    std::uint32_t last_edx = 0;  // 末个 ftol2 的高 32 位（死值）

    for (std::int32_t i = 0; i < ns; ++i) {
        // `0x48D76D fld dword ptr [eax+edi*4]` + `0x48D770 fmul st, st(1)`。
        Ext80 sample80;
        x87MemToReg(s + 4ull * static_cast<std::uint32_t>(i), sample80, 4);
        Ext80 prod;
        x87Mul(sample80, scale80, prod);
        // `call __ftol2_sse`：EDX:EAX = 64 位截断值（解释器口径；见 nativeFtol2Sse）。
        const std::int64_t v = x87ToI64(prod, /*truncate=*/true);
        // `0x4E8301 fst dword [esp+18h]` 的等价：PE/UE/OE + guest 可见的 4 字节。
        x87RegToMem(prod, ftol_scratch, 4);
        last_edx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32);
        // 低 32 位**有符号**比较（两条独立判据：0x48D777 `jle` / 0x48D86B `jge`）。
        std::int32_t ax = static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
        bool clipped = false;
        if (ax > 0x7FFF) {
            ax = 0x7FFF;
            ++n_clip_hi;
            clipped = true;
        } else if (ax < -32768) {
            ax = -32768;
            ++n_clip_lo;
            clipped = true;
        }
        // `0x48D877 mov dword ptr [ebp-108h],1`（**不是** `or`；段内不重置）。
        // ★ 单一写点（两条 clip 分支都在这里写 1，语义与 guest 等价）：`[ebp-108h]` 是**栈帧**里的槽，
        //   而源/目标是 stream 对象里的缓冲 ⇒ 三者不别名，所以"循环内立即写"与"该样本之后写"不可区分。
        //   合并写点是为了让"不写这个标志"这个变异只有一处可下手（`tools/mutate-check.mjs` 的 M3）。
        if (clipped) mem.wr32(ebp - 0x108u, 1u);
        // `0x48D881 mov [ebx],ax`：每样本**只写 2 字节**（小端）。
        const std::uint16_t out16 = static_cast<std::uint16_t>(static_cast<std::uint32_t>(ax));
        std::memcpy(d + 2ull * ch * static_cast<std::uint32_t>(i), &out16, 2);
        last_eax = static_cast<std::uint32_t>(ax);
    }

    // ── ③ 状态字：PE/UE/OE 来自 ftol2 的 `fst dword`（解释器的 `fistp` 不置 PE）。
    //   ⛔ 不清 C0..C3（段内没有指令改它们）—— 与 `nativeFtol2Sse` 同形（⛔ 不要抄 native_affine 的
    //   `syncX87Status`，它末尾会清 CC）。
    {
        const std::uint8_t f = x87TakeExceptionFlags();  // 取走 + 清空（= guest 的"读状态字"）
        std::uint16_t st = cpu.fpu.status;
        if (f & 1u) st |= 0x0020u;   // PE（inexact）
        if (f & 2u) st |= 0x0010u;   // UE（underflow）
        if (f & 4u) st |= 0x0008u;   // OE（overflow）
        if (f & 8u) st |= 0x0004u;   // ZE（divide-by-zero）
        if (f & 16u) st |= 0x0001u;  // IE（invalid）
        if (st & 0x003Fu) st |= 0x0080u;  // ES（解释器同一条简化：有粘性位就汇总）
        st = static_cast<std::uint16_t>((st & ~0x3800u) | ((cpu.fpu.top & 7u) << 11));  // TOP
        cpu.fpu.status = st;
    }

    // ── ④ 出口：EIP = 段出口；寄存器 / EFLAGS 是死值，但必须与解释器逐位相同（伴随断言）。
    //   EBX：每次迭代 `lea ebx,[ebx+ecx*2]` ⇒ +2*ch*ns（最后一次 lea 之后 `jl` 不跳）。
    //   EDI = ns（`cmp edi,[esi+26D8h]` 相等）；ECX = ch（`0x48D884`）。
    //   EFLAGS = `cmp edi,[esi+26D8h]` 相等：ZF=PF=1、CF=AF=SF=OF=0。
    cpu.eax() = last_eax;
    cpu.edx() = last_edx;
    cpu.ecx() = ch;
    cpu.edi() = static_cast<std::uint32_t>(ns);
    cpu.ebx() = dst + 2u * ch * static_cast<std::uint32_t>(ns);
    cpu.eflags = (cpu.eflags & ~(FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)) | FLAG_PF | FLAG_ZF;
    cpu.eip = kResumeEa;

    // `15*n + 1*n_clip_hi + 2*n_clip_lo`（见头注；`15*n` 依赖 `__ftol2_sse` 的函数级 override，G10）。
    *replaced_insns = 15ull * static_cast<std::uint64_t>(ns) + n_clip_hi + 2ull * n_clip_lo;
    return BlockOverrideOutcome::Replaced;
}

}  // namespace lift
