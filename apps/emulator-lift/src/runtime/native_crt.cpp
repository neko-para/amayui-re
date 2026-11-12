// emulator-lift · native override：CRT 叶子（memcpy / memset / sign-memcmp / strlen / __ftol2_sse）
//
// ★ 只经 `GuestMemory` 访存、guest 指针保持 32 位；不重叠的 `_memcpy` 才接手，重叠时回退解释器。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/fpu_bridge.h"
// ★ 两条方向循环（`copyForwardBytes` / `copyBackwardBytes`）与分块下界搬到了共享头：
//   `rep movs*` 的宿主快路径要用**同一份**（等价性论证只有一条，见该文件头）。
//   ⛔ 语义一个字没变（纯搬运）；本函数体的分支与判据仍在本文件里。
#include "runtime/guest_copy.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// ---------------------------------------------------------------------------------------
// CRT 叶子实现（全部只经 GuestMemory 读写 guest；guest 指针保持 32 位）
// ---------------------------------------------------------------------------------------

bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t dst = cpu.arg(0);
    const std::uint32_t src = cpu.arg(1);
    const std::uint32_t n = cpu.arg(2);
    *result = dst;
    if (n == 0) return true;
    auto decline = [&cpu](MemcpyWhy why) {
        if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        return false;
    };
    // ★ 仍然回退（本期不动）：跨 4 GiB 的跨度 —— 宿主平坦映射只有 4 GiB，搬它就是读出界；
    //   guest 在那里的行为是 fault，交给解释器表态。
    if (static_cast<std::uint64_t>(dst) + n > GuestMemory::kSpace ||
        static_cast<std::uint64_t>(src) + n > GuestMemory::kSpace)
        return decline(MemcpyWhy::CrossBoundary);
    // ★ 仍然回退（本期不动）：safe mode 下任一跨度不在已提交页 —— guest 会 fault，
    //   替它搬等于把 fault 掩盖掉。非 safe mode 下这条**照旧不查**（与既有实现一致）。
    if (mem.safeMode() && (!mem.committed(dst, n) || !mem.committed(src, n))) return decline(MemcpyWhy::Uncommitted);

    // —— 不重叠：原路径不变（`std::memcpy`，重叠 UB 的前提不成立）——
    const std::uint64_t d64 = dst;
    const std::uint64_t s64 = src;
    const std::uint64_t n64 = n;
    const bool overlap = d64 < s64 + n64 && s64 < d64 + n64;
    if (!overlap) {
        std::memcpy(mem.host(dst), mem.host(src), n);
        return true;
    }
    // —— 重叠：按 guest 的两支分流（⛔ 不许合成一支，也 ⛔ 不许用 `std::memmove`）——
    //   方向判据来自 `0x4E74C4`（`dst <= src` ⇒ 前向）与 `0x4E74CA jb 0x4E7670`（`dst > src` ⇒ 后向）；
    //   两者**不含** `dst == src`（那支走 0x4E74F4 的前向逐字节自拷贝 ⇒ 内存不变，直接返回）。
    if (dst == src) return true;
    std::uint8_t* const d = mem.host(dst);
    const std::uint8_t* const s = mem.host(src);
    if (dst < src) {
        copyForwardBytes(d, s, n);
    } else {
        copyBackwardBytes(d, s, n);
    }
    return true;
}

bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t dst = cpu.arg(0);
    const std::uint32_t value = cpu.arg(1) & 0xFFu;
    const std::uint32_t n = cpu.arg(2);
    *result = dst;
    if (n == 0) return true;
    if (static_cast<std::uint64_t>(dst) + n > GuestMemory::kSpace) return false;
    if (mem.safeMode() && !mem.committed(dst, n)) return false;
    std::memset(mem.host(dst), static_cast<int>(value), n);
    return true;
}

bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t a = cpu.arg(0);
    const std::uint32_t b = cpu.arg(1);
    const std::uint32_t n = cpu.arg(2);
    *result = 0;
    if (n == 0) return true;
    if (static_cast<std::uint64_t>(a) + n > GuestMemory::kSpace ||
        static_cast<std::uint64_t>(b) + n > GuestMemory::kSpace)
        return false;
    if (mem.safeMode() && (!mem.committed(a, n) || !mem.committed(b, n))) return false;
    const std::uint8_t* pa = mem.host(a);
    const std::uint8_t* pb = mem.host(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            // ★ 与 `.lst` 的 `sar eax,1Fh; or eax,1` 一致：只返回符号（-1 / +1），不是字节差。
            *result = (pa[i] < pb[i]) ? 0xFFFFFFFFu : 1u;
            return true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// `__ftol2_sse`（0x4E82C0）—— MSVC 编译器 support routine：x87 `st(0)` → `EDX:EAX` 的 64 位截断
// ---------------------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kSse2FlagEa = 0x0055F2E8u;  // CRT 的 x87→SSE 选路开关（唯一写者 0x4F1117）
constexpr std::int64_t kI64Max = 0x7FFFFFFFFFFFFFFFll;

}  // namespace

// ★ 为什么挂 `0x4E82C0` 而不是 `0x4E82F6`（⛔ 挂错就 hits=0 却全绿）：`0x4E82C0` 是公开入口
//   （`.lst` 381874 的 COLLAPSED 行，28 B），内容是
//     `0x4E82C0 cmp ds:dword_55F2E8,0` / `0x4E82E3 jz 0x4E82F6`，
//   另一支是 `0x4E82C9..0x4E82DB` 的 `fstp qword [esp]; cvttsd2si eax,[esp]`（只写 EAX，
//   越界语义与本单的两条 x87 路径都**不同**）。本线 `shimIsProcessorFeaturePresent` 恒 0
//   ⇒ guest 永远走 `0x4E82F6 = __ftol2`（117 B）。`0x4E82F6` 只是**函数内**的 `jz` 目标
//   ⇒ 385 个 `call __ftol2_sse` 一个都落不到它上面。
//
// ★ 语义（由 `0x4E82F6..0x4E836A` 的**字节**推出；`.c` 里没有这个函数）：
//   ① `0x4E82FF fld st(0)` / `0x4E8301 fst dword [E+18h]` / `0x4E8305 fistp qword [E+10h]` /
//      `0x4E8309 fild qword [E+10h]`，其中 `E = align16(入口 esp − 0x24)`
//      （`push ebp` + `sub esp,20h` + `and esp,-10h`）；
//   ② `0x4E8319 DE E9 fsubp st(1),st` 之后 `0x4E831F` / `0x4E833D` 的 `D9 1C 24 fstp dword [esp]`
//      只借 f32 的**符号位**做 `add 7FFFFFFFh` + `sbb/adc 0` 的 ±1 修正环 ⇒ 把"就近取整"
//      拉回**向零截断**（四种 RC 都成立 ⇒ 最终值对 RC 无关）；
//   ③ 两条出口（`0x4E8361/0x4E8365` 的两次 `fstp` 与 `0x4E833B`/`0x4E8353` 跳到 `0x4E8369`）
//      **净效果都是消费 1 层**（`fld`+1 / `fistp`−1 / `fild`+1 / `fsubp`−1 / `fstp`−1）；
//   ④ 返回 = 64 位截断值：低 32 位进 EAX、高 32 位进 EDX（**不回绕也不 clamp**）。
//      `|x| ≥ 2^63` / NaN / ±Inf ⇒ `n = INT64_MIN` ⇒ `EAX=0, EDX=0x80000000` + IE。
//
// ★ 实现 = "一次 softfloat 截断 + 一次 f32 转换"，⛔ 不是"照 33 条逐条重放"（已裁决）。
//   硬约束只有两条：值必须是 softfloat 的 **trunc**（⛔ 不是宿主 `(int64)(double)`：`|x| ≥ 2^53`
//   差一位、越界会给 `0x80000000` 而不是 `0/0x80000000`），以及复刻 `0x4E8301 fst dword` 的
//   **副作用**（那个 f32 既给出 PE/UE/OE，又给出 guest 可见的 4 字节）。
//
// ★ 写集合：guest 那 20 B scratch 里我们只写 4 B。guest 写 `[E+18h,E+1Ch)` = `f32(x)`（两条路
//   都有）、`[E+10h,E+18h)` = 就近取整的 int64、非零支再写 `[E,E+4)` = `f32(x − n)`。
//   native 只写 `f32(x)`；另两段是 **caller 栈帧以下的死 scratch**，其值分别是"按 RC 就近的 n"
//   与"余数"，复刻它们就退化成逐条重放 ⇒ 登记为**已知偏差**：`tests/native_override_assets.cpp`
//   的 `ftol` 只逐字节比 `[E+18h,E+1Ch)`，并断言另两段在 native 臂里仍是预填哨兵。
//   被消费掉的物理槽里残留的字节也不同（guest 留 `x−n`、native 留 `x`）—— tag 已置 empty
//   ⇒ 不是 guest 可观察状态（登记，不造假）。
//
// ★ 状态字：⛔ **不要**抄 `native_affine.cpp` 的 `syncX87Status`（它末尾 `s & ~0x4700` 清 C0..C3）。
//   这一段 guest 里**没有任何指令改 C0..C3**（`fld`/`fst`/`fistp`/`fild`/`fsubp` 都不改）⇒ CC 原样保留。
//   这里按 `exec_fpu.cpp` 的 `syncStatus`（不清 CC）只取走一次 + OR。
//   ★ oracle = **解释器**（不是真机/SDM）：解释器的 `fistp` 走 `exact=false` ⇒ **不置 PE**，
//     `fld`/`fild` 也不置 C1 ⇒ 按 SDM 去"补"PE/C1 反而会把差分弄红（已知偏差，登记在这里）。
bool nativeFtol2Sse(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    auto decline = [&](FtolWhy why) {
        if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        return false;
    };
    // ── ① 闸门（全部在**任何**副作用之前：guest 写 / x87 栈 / 宿主 softfloat 全局）──────────
    // G-1 `[0x55F2E8] == 0`：非 0 ⇒ 真机走 `0x4E82D5 cvttsd2si`（只写 EAX、越界语义不同）⇒ decline。
    //   ★ 判据必须带 `committed()`：`rd32` 只在 safe mode 检查提交，非 safe 时读未提交页会
    //     静默给 0 ⇒ 少了这一条，闸门在裸机器上会"放行"。
    if (!mem.committed(kSse2FlagEa, 4u)) return decline(FtolWhy::FlagUnreadable);
    if (mem.rd32(kSse2FlagEa) != 0u) return decline(FtolWhy::Sse2Path);
    // G-2 st(0) 必须非空。★ 判据是**物理槽**：`X87State::empty(i)` 吃的是物理槽号
    //   ⇒ 必须 `empty(phys(0))`；写成 `empty(0)` 在 top≠0 时会查错槽（复审 H2 的硬错）。
    if (cpu.fpu.empty(cpu.fpu.phys(0))) return decline(FtolWhy::StackEmpty);
    // G-3 空闲槽 ≥ 2：guest 峰值 = 入口 + 1 层（`fld` 与 `fild` 各 +1，中间 `fistp` 已 −1），
    //   取 2 是**保守值**（不做窄档）。★ 方向别写反：`used = 8 − free` ⇒ 判据是 `8 − used ≥ 2`。
    {
        unsigned used = 0;
        for (int i = 0; i < 8; ++i)
            if (!cpu.fpu.empty(i)) ++used;
        if (8u - used < 2u) return decline(FtolWhy::StackFull);
    }
    // G-4 栈 scratch 已提交：整段 `[E,E+20h)`（= guest 会写到的全部）都落在入口 esp 之下。
    const std::uint32_t esp = cpu.esp();
    const std::uint32_t frame = (esp - 0x24u) & ~15u;
    if (static_cast<std::uint64_t>(frame) + 0x20u > GuestMemory::kSpace)
        return decline(FtolWhy::ScratchUnreadable);
    if (!mem.committed(frame, 0x20u)) return decline(FtolWhy::ScratchUnreadable);
    Ext80 x;
    std::memcpy(x, cpu.fpu.regs[cpu.fpu.phys(0)], 16);
    const std::int64_t n = x87ToI64(x, /*truncate=*/true);
    // G-5 越界边界上的"就近 vs 截断"分叉：guest 的 `fistp` 按**当前 RC 就近**取整，只有**取整值**
    //   放不进 int64 才落不定值 + IE；`truncate=true` 的 invalid 判据是"**截断值**放不进"。
    //   两者只在 `x ∈ (2^63−1, 2^63)` 上不同，而该区间内唯一可表示的 ext80 值是 `2^63−0.5`
    //   （区间内 ulp = 0.5）：guest 就近成 2^63 ⇒ `0/0x80000000` + IE；trunc 给 `0xFFFFFFFF/0x7FFFFFFF`。
    //   ⇒ 那一点交解释器（⛔ 不在这里自己算"就近"：那会把 RC 重新拖进来）。
    //   ★ 放在 applyControlWord **之前**：这条判据只依赖 trunc 与一个常量比较，都与 RC 无关
    //     ⇒ 回退路径上宿主 softfloat 全局一位都不动（准入门 ②）。
    if (n == kI64Max) {
        Ext80 limit;
        x87FromI64(kI64Max, limit);
        if (x87Compare(x, limit) > 0) return decline(FtolWhy::TruncRoundDiverge);
    }

    // ── ② 从这里开始不再回退 ──────────────────────────────────────────────────────────────
    x87ApplyControlWord(cpu.fpu.control);  // 解释器每条 FP 指令之前都做同一件事（native_affine 同形）
    std::uint8_t scratch[4];
    x87RegToMem(x, scratch, 4);  // = `0x4E8301 fst dword [esp+18h]`：f32 的 4 字节 + PE/UE/OE
    std::memcpy(mem.host(frame + 0x18u), scratch, 4);
    // ── ③ 弹栈 + 状态字 ────────────────────────────────────────────────────────────────
    cpu.fpu.setEmpty(cpu.fpu.phys(0));
    cpu.fpu.top = static_cast<std::uint8_t>((cpu.fpu.top + 1) & 7u);
    {
        const std::uint8_t f = x87TakeExceptionFlags();  // 取走 + 清空（= guest 的"读状态字"）
        std::uint16_t s = cpu.fpu.status;
        if (f & 1u) s |= 0x0020u;   // PE（inexact）
        if (f & 2u) s |= 0x0010u;   // UE（underflow）
        if (f & 4u) s |= 0x0008u;   // OE（overflow）
        if (f & 8u) s |= 0x0004u;   // ZE（divide-by-zero）
        if (f & 16u) s |= 0x0001u;  // IE（invalid）
        if (s & 0x003Fu) s |= 0x0080u;  // ES（解释器同一条简化：有粘性位就汇总）
        s = static_cast<std::uint16_t>((s & ~0x3800u) | ((cpu.fpu.top & 7u) << 11));  // TOP
        cpu.fpu.status = s;  // ⛔ 不动 C0..C3（见上）
    }
    // ── ④ 返回：`EDX:EAX` = 64 位截断值（本项是唯一需要写 EDX 的 override；见头文件的契约段）──
    //   `invoke()` 在 fn 返回后才写 EAX ⇒ EDX 必须在**所有闸门之后**写（回退前不留 guest 副作用）。
    cpu.edx() = static_cast<std::uint32_t>(static_cast<std::uint64_t>(n) >> 32);
    *result = static_cast<std::uint32_t>(static_cast<std::uint64_t>(n));
    return true;
}

bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t p = cpu.arg(0);
    // ★ 不直接用宿主 `strlen`：非 safe mode 下它可能越过 4 GiB 预留（宿主 OOB，fault 会归错）。
    //   逐字节走 `rd8`（safe mode 的提交检查仍在），并用 64 位地址挡住边界。
    std::uint64_t addr = p;
    while (addr < GuestMemory::kSpace) {
        if (mem.rd8(static_cast<std::uint32_t>(addr)) == 0) {
            *result = static_cast<std::uint32_t>(addr - p);
            return true;
        }
        ++addr;
    }
    return false;
}

}  // namespace lift
