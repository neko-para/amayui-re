// emulator-lift · 串操作语义（movs / stos / lods / scas / cmps + rep）
//
// ★ 方向由 DF 决定。`cld`(FC) / `std`(FD) 是**独立助记符**（`Op::Cld` / `Op::Std`；2026-11-10 补：
//   `.lst` 里引擎自己写的代码确实没有它们，但 **CRT 的折叠段**有 —— `___CxxFrameHandler3`
//   的序言 `0x4E709D` 就是 `cld`，room-exit 的 C++ EH 一进 handler 就撞上）。此外
//   `popfd` / `sahf` 也能改 DF。这里按 DF 正负推进（`advance`）。
// ★ 元素宽度取解码器的 `opsz`（`rep movsd` ⇒ 4）。`rep` = 0xF3，`repne` = 0xF2。
// ★ `rep movsd` / `rep stosd` 是 S3 点名要做的两条（语料里有 300 / 18 处）。
//
// ★★ `rep movs*` / `rep stos*` 的**宿主侧快路径**（REQ-01M75XK0XRTSWCZ4G8A5W2GW8）
//   整条 `rep` 本来就是"一条指令内的宿主 `while`"（下面 `while (cpu.ecx() != 0)`），但**每个元素**
//   要付 45–75 条宿主指令的固定税：`writeElem` 每元素一次 `noteGuestWrite`（跨 TU，无 LTO）、
//   两次 `g_access` 记账、两次 DF 判、ESI/EDI/ECX 读改写、size 分发。快路径把这笔税压成
//   **O(1) / 整条**（不重叠时一次 `std::memcpy`；重叠时走 `guest_copy.h` 的两条方向循环）。
//   ★ 闸门**一条都不许放宽**（判据在 `runtime/rep_bulk.h` 的 `repBulkPlan`，那里是唯一真源）：
//     `ECX × 元素宽度` 用 64 位算且不跨 4 GiB · **写之前**判 safe-mode committed ·
//     `ECX == 0` 不碰内存 · **EFLAGS 一位不动** · **方向 × 重叠**：只有"写落后于读"
//     （`DF=0 && dst<=src` 或 `DF=1 && dst>=src`）能按块搬，反向涂抹退回逐元素臂。
//   ★ 换臂开关：`X86::rep_bulk`（宿主字段，默认 true）⇒ 同一进程里两臂可切（差分夹具要这个）。
//   ★ 优先级不变：override（`machine.cpp:1320`，块入口）> 本快路径 > 逐元素臂。
#include <cstdio>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/guest_copy.h"
#include "runtime/rep_bulk.h"
#include "runtime/x86.h"


namespace lift {
namespace {

std::uint32_t readElem(X86& cpu, std::uint32_t ea, std::uint8_t size) {
    switch (size) {
        case 1: return cpu.mem->rd8(ea);
        case 2: return cpu.mem->rd16(ea);
        default: return cpu.mem->rd32(ea);
    }
}

void writeElem(X86& cpu, std::uint32_t ea, std::uint8_t size, std::uint32_t v) {
    // 串操作也可能写到代码页（`rep movs` 到 .text）⇒ 与 `writeMem` 同一条 SMC 钩子。
    noteGuestWrite(cpu.cache, ea, size);
    switch (size) {
        case 1: cpu.mem->wr8(ea, static_cast<std::uint8_t>(v & 0xFFu)); break;
        case 2: cpu.mem->wr16(ea, static_cast<std::uint16_t>(v & 0xFFFFu)); break;
        default: cpu.mem->wr32(ea, v); break;
    }
}

bool par(std::uint32_t v) {
    std::uint8_t x = static_cast<std::uint8_t>(v & 0xFFu);
    x ^= static_cast<std::uint8_t>(x >> 4);
    x &= 0x0Fu;
    return ((0x6996u >> x) & 1u) != 0;
}

void setCmpFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint8_t size) {
    setSubFlags(cpu, a, b, a - b, size, false);
    cpu.setPf(par(a - b));
}

// 一条串操作**实际访问**的字节区间：`[lo, hi)`（长度恰好 `n`）。
//   · DF=0：第一个元素就在起点 ⇒ `[start, start+n)`；
//   · DF=1：第一个元素**也在起点**，之后往下走 ⇒ `[start-n+size, start+size)`
//     （★ 注意不是 `[start-n, start)`：末元素在 `start-(count-1)*size`，它的**高**端是 `start+size`）。
// ★ 为什么必须有这一步：4 B 分块与 `std::memcpy` 都要"块的低端地址 + 长度"，而 `ESI/EDI` 在 DF=1
//   时指的是**高端**。少了这次换算就会搬到 `[start, start+n)` —— 与 guest 的 `[start-n+size, …)`
//   整段错位（实测：DF=1 的差分当场红，首个差异落在目标区中段）。
// 返回 false = 这个区间在 32 位地址空间里会绕回 / 越过 4 GiB（宿主平坦映射表达不了）⇒ 不许快路径。
bool bulkBlock(std::uint32_t start, std::uint64_t n, std::uint8_t size, bool df, std::uint64_t* lo,
               std::uint64_t* hi) {
    const std::uint64_t high = static_cast<std::uint64_t>(start) + size;  // 首个元素的高端（排他）
    if (high > static_cast<std::uint64_t>(GuestMemory::kSpace)) return false;
    if (df) {
        if (n > high) return false;  // 会绕到 0 以下
        *lo = high - n;
        *hi = high;
    } else {
        if (static_cast<std::uint64_t>(start) + n > static_cast<std::uint64_t>(GuestMemory::kSpace)) return false;
        *lo = start;
        *hi = static_cast<std::uint64_t>(start) + n;
    }
    return true;
}

// `rep movs*` / `rep stos*` 的**一次搬完**（调用前 `repBulkPlan` 必须已经返回 `Ok`：闸门全在那里）。
// ★ 这里的每个动作都能在逐元素臂里找到对应物，而且顺序与元素臂一致：
//   ① SMC 记账（在写之前）② 搬字节 ③ 推进 ESI/EDI ④ 清 ECX ⑤ 诊断面 `g_access`。
//   ⛔ EFLAGS 一位不动（`movs`/`stos` 不改标志）；DF 只在两处读：方向选择、推进方向。
// ★ SMC 口径（"记账能不能从每元素提到每段一次"）：**等价**，因为
//   `BlockCache::noteWrite`（`x86.cpp:247–261`）内部本来就是**按页**循环：
//   `p0 = ea/4096 … p1 = (ea+n-1)/4096`，命中任一已标记代码页就置 `stale_`（消费点是下一轮
//   `get()`，`x86.cpp:255` 的注）。逐元素调用 n/size 次得到的页集合 =
//   ∪ᵢ pages(elem_i) = pages(整段 [lo, hi))（元素区间连续、首尾相接），与**一次**
//   `noteWrite(lo, n)` 的循环范围逐页相同；早退条件（`n == 0` / 首字节起点 ≥ 4 GiB）在本快路径的
//   闸门下都不会触发（`n > 0` 且 `hi <= 4 GiB`）⇒ 记账户径**逐页等价**，不是"变粗"。
//   净效果还少 n/size−1 次跨 TU 调用（这就是那笔 15–22 条/dword 的税）。
void repBulkExecute(X86& cpu, const Insn& in, std::uint8_t size, const RepBulkPlan& plan) {
    const std::uint32_t dst = cpu.edi();
    const std::uint32_t src = (in.op == Op::Movs) ? cpu.esi() : 0u;
    const std::uint32_t n = static_cast<std::uint32_t>(plan.bytes);  // 闸门已保证 < 4 GiB
    const bool df = cpu.df();                                       // ★ 全程只读一次 DF
    const std::uint32_t dst_lo = static_cast<std::uint32_t>(plan.dst_lo);
    const std::uint32_t src_lo = static_cast<std::uint32_t>(plan.src_lo);

    noteGuestWrite(cpu.cache, dst_lo, n);  // ① 写之前记账（覆盖整段；见上）
    std::uint8_t* const d = cpu.mem->host(dst_lo);
    if (in.op == Op::Movs) {
        const std::uint8_t* const s = cpu.mem->host(src_lo);
        if (!plan.overlap) {
            std::memcpy(d, s, n);  // 两段不交：方向无关（DF 只影响寄存器收尾）
        } else if (dst_lo != src_lo) {  // 低端相等 ⇒ 两段完全重合 ⇒ 内存不变
            // 闸门保证只剩"写落后于读"的两支：`DF=0 ⇒ dst_lo<src_lo` 前向、`DF=1 ⇒ dst_lo>src_lo` 后向
            if (df) {
                copyBackwardBytes(d, s, n);
            } else {
                copyForwardBytes(d, s, n);
            }
        }
    } else {
        // `stos*`：图样重复（1 字节 → `memset`；2/4 字节 → 先按元素写满前 256 B，再倍增 —— 见
        // `guest_copy.h` 的 `fillPatternBytes`；⛔ 不是"每个元素一次宿主 store"）
        const std::uint32_t mask = (size == 1) ? 0xFFu : ((size == 2) ? 0xFFFFu : 0xFFFFFFFFu);
        const std::uint32_t v = cpu.eax() & mask;
        std::uint8_t pat[4] = {};
        for (std::uint8_t i = 0; i < size; ++i) pat[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
        fillPatternBytes(d, pat, size, n);
    }
    // ③ 推进：逐元素臂的净效果就是 ESI/EDI 各 ±n（32 位回绕），`stos` 只动 EDI。
    //    ④ 清 ECX（rep 跑完一定是 0；`rep` 对 movs/stos 没有提前终止）。
    if (df) {
        cpu.edi() -= n;
        if (in.op == Op::Movs) cpu.esi() -= n;
    } else {
        cpu.edi() += n;
        if (in.op == Op::Movs) cpu.esi() += n;
    }
    cpu.ecx() = 0;
    // ⑤ 诊断面与逐元素臂对齐：元素臂最后一次**写**留下的就是这个值（读的诊断值会被随后的写覆盖）。
    //    DF=0 的最后一次写在高端（`dst+n-size`），DF=1 的在低端（= `dst_lo`）。
    const std::uint32_t last_write = df ? dst_lo : static_cast<std::uint32_t>(dst + n - size);
    g_access = {last_write, size, 1};
}

}  // namespace

// ---- 快路径的闸门（判据的唯一真源；`runtime/rep_bulk.h` 声明，测试也直接调它）----
RepBulkPlan repBulkPlan(const X86& cpu, const Insn& in, std::uint8_t size) {
    RepBulkPlan plan;
    // ⓪ 形态 + 开关。`rep`(0xF3) 与 `repne`(0xF2) 对 `movs`/`stos` 是**同一条循环**：提前终止
    //    只对 `cmps`/`scas` 有意义（见下面 `cmp_like`）⇒ 两种前缀在快路径里等价。
    //    `lods` / `scas` / `cmps` 不做（`lods` 只读一个元素、后两条每元素写 EFLAGS）。
    if (!cpu.rep_bulk) return plan;
    if (in.rep == 0) return plan;
    if (in.op != Op::Movs && in.op != Op::Stos) return plan;
    if (size != 1 && size != 2 && size != 4) return plan;  // 解码器只产 1/2/4（`decode.cpp` 的归一化）

    // ① `ECX × 元素宽度` 用 **64 位**算；`ECX == 0` ⇒ 一个字节都不许碰（没有"零长度快路径"）。
    const std::uint64_t n = static_cast<std::uint64_t>(cpu.ecx()) * size;
    plan.bytes = n;
    if (n == 0) return plan;  // why 保持 NotApplicable
    // ★ `n` 自己也得 < 4 GiB：`start + n <= 4 GiB` 这条**挡不住** `start == 0, n == 4 GiB`
    //   （区间 `[0, 4 GiB)` 合法），而执行体的块长是 32 位（`uint32_t n = plan.bytes`）——
    //   截断成 0 会变成"什么都不搬、ECX 却清零"。这里显式拦掉。
    if (n >= GuestMemory::kSpace) {
        plan.why = RepBulkWhy::CrossBoundary;
        return plan;
    }

    // ② 两条实际访问区间都不许绕回 / 越 4 GiB。★ 32 位算 `ECX × 宽度` 会把
    //    `ECX=0x40000001, size=4` 得成 4（闸门完全失效）—— 变异条目钉住这条。
    const bool df = cpu.df();
    std::uint64_t dlo = 0;
    std::uint64_t dhi = 0;
    if (!bulkBlock(cpu.edi(), n, size, df, &dlo, &dhi)) {
        plan.why = RepBulkWhy::CrossBoundary;
        return plan;
    }
    std::uint64_t slo = dlo;
    std::uint64_t shi = dhi;
    if (in.op == Op::Movs && !bulkBlock(cpu.esi(), n, size, df, &slo, &shi)) {
        plan.why = RepBulkWhy::CrossBoundary;
        return plan;
    }
    plan.dst_lo = dlo;
    plan.src_lo = slo;

    // ③ safe mode 的 committed 预判 —— **在写之前**。未提交 ⇒ guest 会 fault，替它搬等于把
    //    fault 掩盖成"部分拷贝"。⚠ 与 `nativeMemcpy` 同口径：非 safe mode 下不查。
    if (cpu.mem != nullptr && cpu.mem->safeMode()) {
        if (!cpu.mem->committed(static_cast<std::uint32_t>(dlo), static_cast<std::size_t>(n))) {
            plan.why = RepBulkWhy::Uncommitted;
            return plan;
        }
        if (in.op == Op::Movs &&
            !cpu.mem->committed(static_cast<std::uint32_t>(slo), static_cast<std::size_t>(n))) {
            plan.why = RepBulkWhy::Uncommitted;
            return plan;
        }
    }

    // ④ ★ 方向 × 重叠：4 B 分块只在"**写落后于读**"时与逐字节等价（`guest_copy.h` 文件头）。
    //    允许：`DF=0 && dst_lo<=src_lo`、`DF=1 && dst_lo>=src_lo`；另两种（`DF=0 && dst_lo>src_lo`、
    //    `DF=1 && dst_lo<src_lo`）**重叠**时必须退回逐元素臂 —— 那是"逆向涂抹"，分块与逐字节
    //    在这里**结果不同**（位移 1..size-1 字节时必错）。低端相等（两段完全重合）与不重叠都不受
    //    方向限制（前者内存不变，后者顺序不影响结果）。
    //    ★ guest 自己的 `_memcpy` 不会产生那两种组合（`0x4E74C4 dst<=src ⇒ 前向`、
    //    `0x4E74CA jb ⇒ 后向`），但闸门必须在：别处一条 `std; rep movsd` 往低地址搬就会静默变结果。
    if (in.op == Op::Movs) {
        plan.overlap = dlo < shi && slo < dhi;
        if (plan.overlap && dlo != slo) {
            const bool reverse = df ? (dlo < slo) : (dlo > slo);
            if (reverse) {
                plan.why = RepBulkWhy::ReverseOverlap;
                plan.overlap = false;
                return plan;
            }
        }
    }
    plan.why = RepBulkWhy::Ok;
    return plan;
}

bool execString(X86& cpu, const Insn& in) {
    const std::uint8_t size = in.opsz ? in.opsz : 4;
    const std::uint32_t step = size;
    const bool rep = in.rep == 0xF3;
    const bool repne = in.rep == 0xF2;

    auto advance = [&](std::uint32_t& reg) {
        if (cpu.df()) {
            reg -= step;
        } else {
            reg += step;
        }
    };

    // 一次元素操作；返回是否需要"提前结束 rep"（scas / cmps 的 repe / repne）
    auto once = [&]() -> bool {
        switch (in.op) {
            case Op::Movs: {
                const std::uint32_t v = readElem(cpu, cpu.esi(), size);
                writeElem(cpu, cpu.edi(), size, v);
                advance(cpu.esi());
                advance(cpu.edi());
                return false;
            }
            case Op::Stos: {
                const std::uint32_t v = cpu.eax() & (size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                writeElem(cpu, cpu.edi(), size, v);
                advance(cpu.edi());
                return false;
            }
            case Op::Lods: {
                const std::uint32_t v = readElem(cpu, cpu.esi(), size);
                cpu.writeReg(size == 1 ? Reg::AL : (size == 2 ? Reg::AX : Reg::EAX), v);
                advance(cpu.esi());
                return false;
            }
            case Op::Scas: {
                const std::uint32_t a = cpu.eax() & (size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                const std::uint32_t b = readElem(cpu, cpu.edi(), size);
                setCmpFlags(cpu, a, b, size);
                advance(cpu.edi());
                return true;
            }
            case Op::Cmps: {
                const std::uint32_t a = readElem(cpu, cpu.esi(), size);
                const std::uint32_t b = readElem(cpu, cpu.edi(), size);
                setCmpFlags(cpu, a, b, size);
                advance(cpu.esi());
                advance(cpu.edi());
                return true;
            }
            default:
                return false;
        }
    };

    if (!rep && !repne) {
        once();
        return true;
    }
    if (cpu.ecx() == 0) return true;  // rep 且计数 0 ⇒ 一条都不执行（⛔ 也不许判 committed）

    // ★ REP 快路径（见文件头）：闸门全在 `repBulkPlan` 里；`Ok` 才一次搬完，否则照旧走逐元素臂。
    //   ⛔ 这里的顺序不许动：本分支在 `execInsn` 之后（override 已在块入口吃过流量），
    //   而快路径**失败就退回**下面的 `while`（不许有第三条路）。
    const RepBulkPlan bulk = repBulkPlan(cpu, in, size);
    if (bulk.why == RepBulkWhy::Ok) {
        ++cpu.rep_bulk_hits;
        repBulkExecute(cpu, in, size, bulk);
        return true;
    }
    // 诊断计数：只有"本来够格、被某条闸门拦下"才算退回（`NotApplicable` 的形态差异不计）。
    if (bulk.why != RepBulkWhy::NotApplicable) ++cpu.rep_bulk_declines;

    while (cpu.ecx() != 0) {
        once();
        cpu.ecx() -= 1;
        if (cpu.ecx() == 0) break;
        const bool cmp_like = (in.op == Op::Scas || in.op == Op::Cmps);
        if (cmp_like) {
            if (rep && !cpu.zf()) break;      // repe：不相等就停
            if (repne && cpu.zf()) break;     // repne：相等就停
        }
    }
    return true;
}

}  // namespace lift
