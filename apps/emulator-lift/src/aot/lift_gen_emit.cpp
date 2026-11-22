// emulator-lift · Shape A / T1a：guest 基本块 → LLVM IR 文本（`lift-gen` 的发射器）
//
// ★ 语义真源 = 解释器臂的 `src/runtime/exec*.cpp` + `x86.cpp`：
//   · 寄存器写回/合并（8 位高半、16 位）照 `X86::writeReg`；
//   · 有效地址照 `X86::eaOf`（disp + base + index*scale，32 位回绕）；
//   · flags 照 `exec.cpp` 的 `setLogicFlags` / `setAddFlags` / `setSubFlags` 与 `exec_integer.cpp`
//     的逐条实现（CF/PF/AF/ZF/SF/OF 的公式**逐位**一致；其它位原样保留）；
//   · `push/pop` 照 `X86::push32/pop32`（**不**走 `writeMem` ⇒ **不**调 `lift_note_write`）；
//   · 唯一的数据写通道照 `X86::writeMem`：`LIFT_AOT_NOTE_WRITE=ON` 时先 `lift_note_write(cpu, ea, n)`
//     再写；**缺省（Release/RelWithDebInfo）不发** —— 见 `GenIrTarget::note_write` 的头注。
//   ⛔ 一处都不"顺手优化"成另一套语义：与解释器逐位一致是 AOT 唯一的正确性判据。
//
// ★ 寄存器状态**留在 `%st`（LiftRegs）里**：`%st` 仍然是**唯一真源**，但**块内**用一份寄存器缓存
//   （E2 / S2：`reg_cache_` / `reg_dirty_`）保活 —— 只在**块出口**与**宿主调用之前**写回 `%st`。
//   · 语义不变：`%st` 只在"宿主可观察点"被读（`lift_one` 入口 / `ret` 回 trampoline / `br` 进下一个
//     IR 块），这些点之前一律 `flushRegs()`；缓存**跨块作废**（每个 `bb_<EA>` 开头清空 —— 否则会用
//     到不支配本块的值 ⇒ 非法 IR）。
//   · `lift_one` 会**整块回读** `LiftRegs`（`lift_abi.cpp:107`）⇒ 调用后缓存**整份失效**。
//   · EFLAGS 走**同一个**缓存槽（下标 8 = `%st+32`），但**六位不再逐条整字物化**（S3）：
//     六位各有一个三态槽（`Slot::FromBase` / `Value` / `Poison`），写者只**交付**（`deliverBit`）
//     该算的位、按位**生产者**只发 mask 里的语句，唯一出口 `materialize(mask)` 把 lazy 位并进
//     base 字。哪些位要算/要物化 = **块图上的 flags 活性**（`computeFlagsLiveness`，S3-c）：
//     `need_out(B) = escape(B) ? ALL : ∪ need_in(succ)`、`need_in(B) = backScan(B, need_out)`，
//     迭代到不动点；块尾只物化 `need_out`，写者的掩码 = `live & W`（死位 = 0 语句 + `killBit`）。
//     设计真源 `.tmp/aot/s3design/plan.md` §1.4–§1.6；语义真源仍是解释器（⛔ 不是 SDM 的 undefined）。
//   · 于是仍然不需要跨块 SSA / phi，控制流直接 `br`；这也是"语义等价、但**不**逐字节等于手写 golden"
//     的原因（golden 是 SSA 形态，本生成器是内存形态）。⛔ 不改 golden 去迁就生成器。
//
// ★ 入口函数形状（父级已裁）：
//   `define i32 @lift_cc_<EA>(ptr %st, ptr %mem, i32 %ea, ptr %cpu)`；
//   `switch i32 %ea` 表外 ⇒ `bad:` = `llvm.trap` + `unreachable`（⛔ 不返回哨兵）。
//   · 目标**不是**入口 EA 的 fallthrough / jcc rel32 / jmp rel32 ⇒ 宿主 `br`；
//   · 目标**是**入口 EA ⇒ `ret i32 <目标 EA>`（交回 trampoline）；
//   · `call` ⇒ 压返回 EA 到 guest 栈，然后 `ret i32 <被调方 EA>`（rel32 是常量；
//     reg/mem 从操作数**先读**再压栈 —— 与 `exec_control.cpp` 的顺序一致）；
//   · `ret`/`retn imm16` ⇒ 弹 EA、`esp += 4 [+ imm]`、`ret i32 <弹出的 EA>`；
//   · `jmp reg/mem` ⇒ 读目标后 `ret i32 <目标>`（运行期才知道，必须回 trampoline）。
//
// ★ 本轮**不 inline** 的指令：**单指令宿主原语** `lift_one(cpu, st, ea)` —— 一条指令一次宿主
//   调用，语义交给既有的解释器（`Decoder::decode` + `execInsn`，与 dispatcher 同一对），
//   生成器**不编第二份语义**。
//   这条默认路径覆盖：x87 全族 / `fs:`·`gs:` / 串操作 / `Div`·`Idiv` / `Rcl`·`Rcr`（要读旧 CF）/
//   立即数计数的 `Rol`·`Ror` / 寄存器计数但**目的操作数是内存**的移位 /
//   `Imul` / `Shld` / `Cmpxchg` / `Cpuid` / `Int`·`Int3`·`Hlt` / `Other` / `Invalid` / MMX / SSE /
//   段寄存器操作数（P2）…… 正确性由解释器保证（D10-P2 / D11 / D12）。
//   ★ 寄存器计数（CL）的 `shl/shr/sar/rol/ror` **不在**这条路径上：2026-11-15 起由
//     `emitShiftReg` 无分支 inline 发射（见那里的注释与 `exec_integer.cpp:65-155`）。
//   ⛔ **控制转移**（`jmp/jcc/call/ret`）不走这条路：那些由生成器自己 inline / 回 trampoline；
//   它们掉进默认路径 ⇒ 仍然 `llvm.trap`（那是生成器内部不一致，⛔ 不许静默）。
//   ★ **c2-lite（2026-11-15）**：`lift_one` 的 flags R/W **按 op 分类**（唯一一份：
//     `liftOneFlagsRWOf`）—— 第 1 类 R=∅W=∅（`mov`/`movs`/`stos`/`lods`/`push`/`pop`/`div`/`idiv`/
//     `cld`/`std`/`nop`…）不再把含它的块拖成 need_in=ALL；第 2 类只碰解释器里逐行给出的子集
//     （`cmpxchg`/`cmps`/`scas`/`setcc`/`cmovcc`/`shld`/`shrd`）；其余（x87 全族 / Packed / Sse /
//     `imul` / 循环移位 / `cpuid` / …）仍是 R=W=ALL 的屏障。语义真源照旧是解释器，⛔ 不是 SDM。
//   ★ 计数（D2）：`lift_tick(cpu, n)` 在**块边界**发一次静态条数、在**任何宿主调用之前**把挂账的
//   先 flush 掉 ⇒ 观测点看到的 `insns` 精确；`rep` 按 1 条算（与解释器同一口径）。
#include "aot/lift_gen_emit.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "runtime/exec.h"     // opIsControl / opIsX87（与解释器同一份判据）
#include "runtime/lift_abi.h"  // kLiftHalt（停机哨兵；`lift_one` 失败 ⇒ ret i32 kLiftHalt）
#include "runtime/x87_layout.h"  // ★ 检查点 2：x87 状态在 %cpu 里的**生成代码契约**（唯一一份常量）

namespace lift {
namespace aot {
namespace {

// ★ 2026-11-15：x87 / SSE / 段基址在 `%cpu` 里的偏移契约（唯一一份：`runtime/x87_layout.h`）。
//   提到 TU 顶部：`eaOf`（fs:/gs:）与 MMX/SSE 的向量读写都要用，而它们的定义在 x87 那一段之前。
namespace x87l = lift::x87layout;

// EFLAGS 位（`regs.h` 的同一组常量；这里只用于生成常量掩码）
constexpr std::uint32_t kCf = 0x00000001u;
constexpr std::uint32_t kPf = 0x00000004u;
constexpr std::uint32_t kAf = 0x00000010u;
constexpr std::uint32_t kZf = 0x00000040u;
constexpr std::uint32_t kSf = 0x00000080u;
constexpr std::uint32_t kDf = 0x00000400u;
constexpr std::uint32_t kOf = 0x00000800u;

// 与 golden 同一张"比较集合"（`kDiffFlags` = 0x8D5）：CF|PF|AF|ZF|SF|OF。
constexpr std::uint32_t kSixFlags = kCf | kPf | kAf | kZf | kSf | kOf;
static_assert(kSixFlags == 0x8D5u, "六位比较集合必须与 tests 的 kDiffFlags 一致");

// =====================================================================================
// S3-a：flags 的**表示**（六位 i1 槽 + base 字）
// =====================================================================================
// ★ 设计真源 = `.tmp/aot/s3design/plan.md` §1.1 / §1.6(c) / §3.1；语义真源照旧是**解释器**
//   （`exec.cpp` / `exec_integer.cpp`），⛔ 不是 Intel 手册（SDM 说"未定义"的那些位，
//   解释器都写成了确定值 —— `div/idiv` 一位不写、逻辑运算 AF=0、`mul size==1` 写 ZF/SF）。
//
// 表示：
//   · **base 字** = `reg_cache_[8]`（`%st+32` 的块内缓存）。它承载六位之外的**一切**
//     （DF / IF / bit1 / bit3 / bit5 / …），是那些位**唯一**的真源；⛔ 不许在别处另存一份 flags 字。
//   · **六个 i1 槽**（位序 = EFLAGS 位位置）各有一个三态：
//       `FromBase` = 该位的当前值就在 base 里（读它 ⇒ 一次 `and`+`icmp`）；
//       `Value`    = 该位有一个还没并进 base 的 i1 SSA 值；`Poison` = 写过但**是死的**。
//   · **唯一出口** = `materialize(mask)`：把 mask 里有 `Value` 的位并进 base 字（结构性 overlay），
//     mask 之外的位一律**原样来自 base**。
//   ⛔ 物化里**不许**强置 bit1=1 / 清 bit3,5,15：解释器从不规范化（`x86.h:330-344` 是逐位读-改-写、
//      `popfd` 只 `or 0x200`），把 bit1 强置成 1 就与解释器**逐位不同**（`popfd` 能把它清成 0）。
enum FlagBit : std::uint8_t { kBitCf = 0, kBitPf = 1, kBitAf = 2, kBitZf = 3, kBitSf = 4, kBitOf = 5 };
constexpr std::uint8_t kAllBits = 0x3Fu;
// 位序与 `kCf..kOf` 一一对应（plan §1.1 的 static_assert 逐字落地）。
constexpr std::uint32_t kBitWeight[6] = {kCf, kPf, kAf, kZf, kSf, kOf};
static_assert((kBitWeight[0] | kBitWeight[1] | kBitWeight[2] | kBitWeight[3] | kBitWeight[4] |
               kBitWeight[5]) == 0x8D5u,
              "六位集合必须与 tests 的 kDiffFlags 一致");

// §1.3 条件 → 需要的位（**唯一一份**；三个消费者共用：惰性读、融合判据、物化掩码）。
// ⛔ 不许在第二个地方再写一遍条件语义（少算一位就是静默错）。
std::uint8_t condBits(Cond cc) {
    switch (cc) {
        case Cond::O: return 1u << kBitOf;
        case Cond::NO: return 1u << kBitOf;
        case Cond::B: return 1u << kBitCf;
        case Cond::AE: return 1u << kBitCf;
        case Cond::E: return 1u << kBitZf;
        case Cond::NE: return 1u << kBitZf;
        case Cond::BE: return (1u << kBitCf) | (1u << kBitZf);
        case Cond::A: return (1u << kBitCf) | (1u << kBitZf);
        case Cond::S: return 1u << kBitSf;
        case Cond::NS: return 1u << kBitSf;
        case Cond::P: return 1u << kBitPf;
        case Cond::NP: return 1u << kBitPf;
        case Cond::L: return (1u << kBitSf) | (1u << kBitOf);
        case Cond::GE: return (1u << kBitSf) | (1u << kBitOf);
        case Cond::LE: return (1u << kBitZf) | (1u << kBitSf) | (1u << kBitOf);
        case Cond::G: return (1u << kBitZf) | (1u << kBitSf) | (1u << kBitOf);
        default: return 0;
    }
}

// =====================================================================================
// S3-c：§1.2 的逐位 R/W 表 + 屏障判定 + 块图上的 flags 活性（plan §1.4 / §1.6）
// =====================================================================================
// ★ 判据唯一性（plan §1.2 末尾那条星号）：`flagsRW()` 与"这条指令能不能 inline"必须**同一份**判据。
//   ⇒ 本 TU 里只有一份：`unsupportedReasonOf`（能不能 inline）+ `flagsWriteMaskOf`（inline 的 W 表）+
//   `liftOneFlagsRWOf`（**不 inline** 的 `lift_one` 站点的 R/W 三类，c2-lite）+
//   `flagsRWOf`（把两条路并成 R/W/屏障）。`FnEmit::unsupportedReason` / `FnEmit::flagsWriteMask` 都转发到这里，
//   ⛔ 不许在第二个地方再写一遍表（少算一位 = 静默错，多算一位 = 白算）。
// ★ 活性单位 = **块首 EA**（plan §1.5 坑③）：重叠块只来自"两个根互相重叠"⇒ 同一段字节会有
//   两个块、两份独立的 need_in/need_out。⛔ 不按字节区间/区域索引。
// ★ `escape`（plan §1.4 的表）：`ret` · 间接 `jmp/call` · 边目标 ∉ 任何块（自动覆盖 shim 桩 EA /
//   三个合成 EA / dropped 派生 leader / unbuildable 入口）⇒ `need_out = kAllBits`。
//   `hlt/int3/int` **不是**额外 escape：它们走 `lift_one(terminating=true)`；`lift_one` 的 flags
//   R/W **按 op 分类**（c2-lite，`liftOneFlagsRWOf`）—— 只有第 3 类（ALL）的站点才让块 need_in = ALL。

// 这条指令能不能 inline（非 nullptr = 走 `lift_one` 单指令宿主原语）。**唯一一份**。
const char* unsupportedReasonOf(const Insn& in) {
    // ★ 2026-11-15（本轮：`lift_one` 调用点 → 0）：`Op::Other` 与 `Invalid` **不再**走
    //   `lift_one` —— 解释器对它们**恒**走"响亮停"（`exec.cpp:209-210` 的 `unimplemented`），
    //   所以发射器只表达同一条停机面（`emitStop` → `@lift_stop`），不解释执行任何指令。
    if (in.op == Op::Invalid) return nullptr;
    if (in.op == Op::Other) return nullptr;
    // ★ 检查点 2（D49）：x87 **不再整族走 `lift_one`** —— 逐个 op 判定，覆盖表见下。
    if (opIsX87(in.op)) {
        switch (in.op) {
            // ---- 仍走 lift_one：只有"环境 / 状态保存区"与 80 位内存格式这两类 ----
            // 理由（逐条）：它们操作的是 **28 字节环境区 / 108 字节保存区 / 10 字节内存格式**，
            // 不属于"寄存器栈 + 算术"；本语料 0 站点（300 帧直方图里没有它们），内联它们的
            // IR 体积与风险都不划算。⛔ 不是"整族图省事"：其余 60 余条 x87 op 全部内联。
            case Op::Fldenv:
            case Op::Fnstenv:
            case Op::Fnsave:
            case Op::Frstor:
                // ★ 2026-11-15（本轮）：**已内联**（`emitX87SaveEnv` / `emitX87RestoreEnv`）。
                break;
            case Op::Fld:
            case Op::Fst:
            case Op::Fstp:
                // ★ 2026-11-15（本轮）：10 字节扩展精度**已内联**（`m80ToDouble` / `doubleToM80`）。
                break;
            case Op::Fild:
            case Op::Fist:
            case Op::Fistp:
            case Op::Fisttp:
                if (in.op0().kind == OpKind::Mem && in.op0().mem.size != 2 &&
                    in.op0().mem.size != 4 && in.op0().mem.size != 8)
                    return "x87 整数内存操作数的宽度不是 16/32/64 ⇒ lift_one";
                break;
            default: break;
        }
        return nullptr;  // 其余 x87：内联发射（超越函数族走 `@lift_x87_sem` helper）
    }
    if (in.op == Op::Packed) {
        // ★ 2026-11-15（本轮：`lift_one` → 0）：MMX / SSE 整数打包**全部内联**。
        //   语义真源 = `exec_fpu.cpp:470-920`（`execPacked` + `pkLoad/pkStore/pkShift/pkBinary`）。
        //   本语料的形态（movd/movq/pand/psrlw/…）都在 `emitPacked` 的覆盖里；若出现覆盖外的
        //   形态 ⇒ `emitPacked` **生成期硬错**（⛔ 不许悄悄退回 lift_one）。
        return nullptr;
    }
    if (in.op == Op::Sse) {
        // ★ 同上：`execSse`（`exec_fpu.cpp:1020-1259`）。本语料只有 movdqa / movsd 两种子语义。
        return nullptr;
    }
    if (in.op == Op::Div || in.op == Op::Idiv) return nullptr;  // 内联（#DE 走 `lift_stop`）
    if (in.op == Op::Hlt) return nullptr;                       // 停机面 → `lift_stop`
    if (in.op == Op::Int3 || in.op == Op::Int) return nullptr;  // 停机面 → `lift_stop`
    if (in.op == Op::Other) return nullptr;                     // 解释器也**恒**停 ⇒ `lift_stop`
    if (in.op == Op::Movs || in.op == Op::Stos || in.op == Op::Lods || in.op == Op::Scas ||
        in.op == Op::Cmps) {
        // ★ 2026-11-15：`movs`/`stos`/`scas`/`cmps` 发**真实 IR 循环**（`emitString`）。
        //   `lods` 本语料 0 站点（`.ll` 按 op 的静态站点里没有它）⇒ 保留 `lift_one` 路径。
        if (in.op == Op::Lods) return "串操作：lods 本语料 0 站点（保留 lift_one 路径）";
        return nullptr;
    }
    if (in.op == Op::Rcl) return "Rcl（要读旧 CF；本语料 0 站点）⇒ lift_one";
    if (in.op == Op::Rcr) return nullptr;  // 内联（`doShift` 的 rcr 分支逐行照抄）
    if (in.op == Op::Rol || in.op == Op::Ror) return nullptr;  // 立即数 + 寄存器计数都已内联
    if (in.op == Op::Shld || in.op == Op::Shrd) return nullptr;  // `doShxd` 逐行照抄
    if (in.op == Op::Imul) return nullptr;                        // `doImul` 逐行照抄
    if (in.op == Op::Cmpxchg) return nullptr;                     // `exec_integer.cpp:414-427`
    if (in.op == Op::Cpuid) return nullptr;                       // 虚构 CPU 常量表（同一份）
    // ★ 2026-11-15：`shl/shr/sar` 的寄存器计数（CL）与内存目的操作数都已内联。
    if (in.op == Op::Shl || in.op == Op::Shr || in.op == Op::Sar) {
        if (in.op1().kind != OpKind::Imm && in.op1().kind != OpKind::Reg)
            return "移位计数的操作数既不是立即数也不是寄存器 ⇒ lift_one";
    }
    if (in.op == Op::Setcc && in.op0().kind == OpKind::Mem) return nullptr;  // 条件写内存已内联
    if (in.op == Op::Lea && in.op1().kind != OpKind::Mem) return "Lea 的第二操作数不是内存";
    for (int i = 0; i < 3; ++i) {
        const Operand& op = in.ops[i];
        if (op.kind == OpKind::Xmm) return "XMM 操作数（只有 Packed/Sse 两族内联）";
        // ★ P2（D10）：段寄存器 / x87 寄存器操作数。`LiftRegs` 里没有 `seg[]` / `st[]`
        //   ⇒ 生成器的 `readRegValue` 会把它**静默读成 0**、`writeRegValue` 会**静默丢写**
        //   （解释器 `x86.cpp:31/55` 是真读真写）。⛔ 不给它编语义：让它走 `lift_one`，
        //   由解释器读/写真的 `cpu.seg[]` / `cpu.fpu`。
        if (op.kind == OpKind::Reg) {
            const RegClass cls = regSpec(op.reg).cls;
            // ★ 2026-11-15：段寄存器操作数**已内联**（`cpu.seg[]` 在 `%cpu` 里，偏移见
            //   `runtime/x87_layout.h` 的 kSeg；读/写在 `readRegValue` / `writeRegValue`，
            //   与 `x86.cpp:31` / `:55` 逐位相同）。
            if (cls == RegClass::X87) return "x87 寄存器操作数（LiftRegs 没有 st[]）⇒ lift_one";
        }
        if (op.kind == OpKind::Mem) {
            // ★ 2026-11-15：`fs:` / `gs:` **已内联**（`eaOf` 读 `%cpu` 的 fs_base / gs_base，
            //   与 `x86.cpp:68-75` 的 `X86::eaOf` 同一条加法）。
            if (op.mem.size != 1 && op.mem.size != 2 && op.mem.size != 4)
                return "64/80/128 位内存操作数（只有 Packed/Sse 会用，见各自的发射器）";
        }
    }
    return nullptr;
}

// §1.2 的 W 表：**这条指令写哪些位**（0 = 不碰 flags；kAllBits = 整字覆写）。
// ★ 只覆盖 **inline** 的 op；`unsupportedReasonOf()` 非空的走 `liftOneFlagsRWOf`（c2-lite），
//   ⛔ 不再"不 inline 的一律 W=ALL"（`mov`/`div` 这类 W=∅ 的收窄就是 c2-lite 的收益面）。
std::uint8_t flagsWriteMaskOf(const Insn& in) {
    // ★ 检查点 2（D49）：x87 的 W 表 —— 只有 `fcomi` 族碰 EFLAGS（写 ZF/PF/CF 并清 OF/SF/AF），
    //   其余 x87 只动 x87 自己的状态字（那不是 EFLAGS）⇒ W = ∅。
    //   ★ `fnstsw` 写的是 **GPR**（AX）或内存，不是 EFLAGS ⇒ 也是 ∅。
    if (opIsX87(in.op)) {
        switch (in.op) {
            case Op::Fcomi:
            case Op::Fcomip:
            case Op::Fucomi:
            case Op::Fucomip: return kAllBits;
            default: return 0;
        }
    }
    switch (in.op) {
        // 第 1 行：一个 `setXxx` 都没有（`Not` / `Mov` / `Lea` / `Push` / `Pop` / …）
        case Op::Nop:
        case Op::Pause:
        case Op::Mov:
        case Op::Movzx:
        case Op::Movsx:
        case Op::Lea:
        case Op::Xchg:
        case Op::Push:
        case Op::Pop:
        case Op::Pushad:
        case Op::Popad:
        case Op::Cdq:
        case Op::Cwd:
        case Op::Cwde:
        case Op::Cbw:
        case Op::Bswap:
        case Op::Leave:
        case Op::Not:
        // 第 2/6 行：`pushfd` 只**读**整字、`lahf` 只读五位 ⇒ 不是写者
        case Op::Pushfd:
        case Op::Lahf:
        // 第 4 行：`cld/std` 只动 DF（六位一位都不改）★ 最容易写错的一行
        case Op::Cld:
        case Op::Std:
        // 读者（第 18/19 行）
        case Op::Jcc:
        case Op::Setcc:
        case Op::Cmovcc:
        // 控制转移（第 20 行）
        case Op::Call:
        case Op::Ret:
        case Op::Jmp:
            return 0;
        // ★ 2026-11-15（本轮：`lift_one` → 0）新增的 inline 族：
        //   · `Div/Idiv`：`exec_integer.cpp:270-332` 的 `doDiv` 一位 flags 都不写；
        //   · 串操作 `movs/stos`：`exec_string.cpp:236-254` 不碰 flags；
        //   · `Cpuid`：`exec_integer.cpp:595-614` 只写 4 个 GPR；
        //   · `Packed`：`exec_fpu.cpp:844-920` 全族不碰 EFLAGS（`emms` 只改 x87 tag）；
        //   · `Sse` 的 movdqa/movsd：`exec_fpu.cpp:1026-1125` 不碰 flags；
        //   · 停机类 `Hlt/Int3/Int/Other/Invalid`：停机面本身不写 flags
        //     （`exec_control.cpp:62-72` / `exec.cpp:209-210`）—— 它们之后没有任何后继。
        case Op::Div:
        case Op::Idiv:
        case Op::Movs:
        case Op::Stos:
        case Op::Cpuid:
        case Op::Packed:
        case Op::Hlt:
        case Op::Int3:
        case Op::Int:
        case Op::Other:
        case Op::Invalid:
            return 0;
        case Op::Sse:
            // 只有 comiss/ucomiss 写 EFLAGS（`exec_fpu.cpp:1214-1254`：写 ZF/PF/CF + 清 OF/SF/AF）；
            // 本语料的两个子语义（movdqa/movsd）一位都不写。
            return (in.sse == SseOp::Comiss || in.sse == SseOp::Ucomiss) ? kAllBits : 0;
        case Op::Scas:
        case Op::Cmps:
            // `exec_string.cpp:255-269` 的 setCmpFlags（= setSubFlags）⇒ 六位全写；
            // `rep` 且计数为 0 时整条不执行 ⇒ 旧值穿过（由 `emitString` 的 select 门表达）。
            return kAllBits;
        case Op::Imul:
            // `exec_integer.cpp:214-268` 的 doImul：只 setCf/setOf（1/2/3 操作数三种形态都一样）
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitOf));
        case Op::Cmpxchg:
            // `exec_integer.cpp:414-427`：setSubFlags（:420）⇒ 六位全写
            return kAllBits;
        case Op::Shld:
        case Op::Shrd: {
            // `exec_integer.cpp:157-183`：计数 0 或 ≥ 位宽 ⇒ 一位不写（:164）；
            // 否则写 CF/OF/ZF/SF/PF（:176-180，⛔ 不写 AF）。立即数计数可在生成期定型。
            const unsigned bits = (in.op0().size == 1 ? 8u : (in.op0().size == 2 ? 16u : 32u));
            if (in.op1().kind == OpKind::Imm) {
                const std::uint8_t c = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
                if (c == 0 || c >= bits) return 0;
            }
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) | (1u << kBitZf) |
                                             (1u << kBitSf) | (1u << kBitOf));
        }
        case Op::Rcr: {
            // `exec_integer.cpp:131-142`：CF 无条件写（c≠0）；OF = (c==1) ? 新值 : 旧值 ⇒ 读-改-写。
            if (in.op1().kind == OpKind::Imm) {
                const std::uint8_t c = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
                if (c == 0) return 0;
                if (c != 1) return static_cast<std::uint8_t>(1u << kBitCf);
            }
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitOf));
        }
        // 第 3 行：整字覆写
        case Op::Popfd:
        // 第 5 行：`sahf` 写五位 + bit1，OF 要读旧值
        case Op::Sahf:
        // 第 7 行：六位全写（逻辑运算连 AF 都写 0）
        case Op::Add:
        case Op::Adc:
        case Op::Sub:
        case Op::Sbb:
        case Op::Cmp:
        case Op::Test:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Neg:
        case Op::Xadd:
            return kAllBits;
        // 第 9 行：`inc/dec` **不写 CF**（`exec_integer.cpp:502-508` 的 saved_cf）
        case Op::Inc:
        case Op::Dec:
            return static_cast<std::uint8_t>(kAllBits & ~(1u << kBitCf));
        // 第 10–13 行：移位。计数 0 ⇒ 一位都不改；`shr imm c>1` 的 OF 是读-改-写（不算 def）。
        case Op::Shl:
        case Op::Shr:
        case Op::Sar: {
            const bool imm = (in.op1().kind == OpKind::Imm);
            if (imm) {
                const std::uint8_t c = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
                if (c == 0) return 0;
                if (in.op == Op::Shr && c != 1)  // OF 保留（不写）
                    return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) |
                                                     (1u << kBitZf) | (1u << kBitSf));
            }
            // 寄存器计数（运行期）：OF 可能被写（c==1）⇒ 保守当写者
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) | (1u << kBitZf) |
                                             (1u << kBitSf) | (1u << kBitOf));
        }
        // 第 16 行：`rol/ror` 只写 CF/OF（AF 五个 op 都不写；ZF/SF/PF 只有 shl/shr/sar 写）
        // ★ 2026-11-15：立即数计数可在生成期定型 —— `c==0` ⇒ 一位都不写（`:71` 的早退）；
        //   `ror` 的 OF 是**读-改-写**（`:116`：只在 c==1 时替换）⇒ c!=1 时不进 W。
        case Op::Rol:
        case Op::Ror: {
            if (in.op1().kind == OpKind::Imm) {
                const std::uint8_t c = static_cast<std::uint8_t>(
                    static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
                if (c == 0) return 0;
                if (in.op == Op::Ror && c != 1) return static_cast<std::uint8_t>(1u << kBitCf);
            }
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitOf));
        }
        // 第 14/15 行：`mul size==1` 写 ZF/SF（解释器显式写）；size 2/4 只写 CF/OF
        case Op::Mul: {
            const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
            if (size == 1)
                return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitOf) |
                                                 (1u << kBitZf) | (1u << kBitSf));
            return static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitOf));
        }
        // 第 16/17 行
        case Op::Bt:
        case Op::Bts:
        case Op::Btr:
        case Op::Btc:
            return 1u << kBitCf;
        case Op::Bsf:
        case Op::Bsr:
            return 1u << kBitZf;
        default:
            // 屏障：`unsupportedReasonOf()` 非空的都走 `lift_one`（整份回读 ⇒ 六位 = 解释器写的值）。
            // 剩下的 inline op 若走到这里，说明 W 表漏了一行 ⇒ 生成期硬错（⛔ 不默认"不碰"）。
            return kAllBits;
    }
}

// §1.2 的 R/W（+ 屏障 / 条件写）。三态与不变式见 plan §1.1 / §1.2a。
struct FlagsRW {
    std::uint8_t read = 0;   // 需要读的位（`barrier` 时 = 解释器**真读**的位，⛔ 不按活性收窄）
    std::uint8_t write = 0;  // 会写的位
    bool barrier = false;    // 走 `lift_one`（宿主调用；发射侧按位保留/重置槽，不再整字 reset）
    // 写是运行期有条件的 ⇒ 旧值必须原样穿过：inline 写者把 read 收紧成 `write & live`，
    // `lift_one` 站点则在宿主调用前把 `write & live` 的旧值物化（见 `backScanBlock` / `emitLiftOne`）。
    bool conditional = false;
};

// =====================================================================================
// c2-lite：`lift_one` 站点的 flags R/W 分类（**唯一一份**；三类）
// =====================================================================================
// 判据一句话：**该 op 在解释器里是否读 / 写这六位**（⛔ 不按 SDM；真源 = `src/runtime/exec*.cpp`）。
//   · 第 1 类 R=∅ W=∅：handler 里根本没有六位访问 ⇒ 这些站点不再把整块 flags 活性拖成 ALL。
//   · 第 2 类：只碰一个**已知子集**（只读 / 只写 / 读写都可能有，但集合由解释器逐行给出）。
//   · 第 3 类（`default`）：一律 R=W=ALL。⛔ 判断不了就归这里，**不猜**。
// 每条的分类依据都在行尾写 `文件:行`（⛔ 不写"SDM 说…"）。
// ★ 为什么 x87 / Packed / Sse 全族只能 ALL（不能一刀切 ∅/∅）：x87 的
//   `Fcomi/Fcomip/Fucomi/Fucomip` **写**六位、`Fcmov*` **读**六位（`exec_fpu.cpp:271-321`），
//   SSE 的 `comiss/ucomiss` 也写六位（`exec_fpu.cpp:1244-1249`）⇒ 同一族里读写者混在一起。
// ★ `Div/Idiv` 的 `#DE` 停机通道不改变"R=∅ W=∅"这条判据：解释器 `doDiv` 只写寄存器/内存、
//   一条 `setXxx` 都没有（`exec_integer.cpp:270-332`），D41 的 M4 变异体专门钉过 div 后的六位。
// ★ 第 2 类的 `conditional`：`cmps/scas` 的 `rep` 计数为 0 时整条不执行（`exec_string.cpp:275-279`），
//   `shld/shrd` 的计数为 0 或 ≥ 位宽时一位都不写（`exec_integer.cpp:164`）⇒ 旧值必须穿过。
FlagsRW liftOneFlagsRWOf(const Insn& in) {
    FlagsRW rw;
    rw.barrier = true;
    switch (in.op) {
        // ---- 第 1 类：R=∅ / W=∅ ----------------------------------------------------
        // 所有 handler 都只在寄存器 / 内存之间搬数据（`wr`/`rd`/`push32`/`pop32`），
        // 没有 `setCf/…`、也没有 `cf()/zf()` 之类的六位读。
        case Op::Nop:
        case Op::Pause:
            return rw;  // `exec_integer.cpp:338-340`：直接 return true
        case Op::Mov:
            return rw;  // `exec_integer.cpp:342-344`：wr(rd())，不碰 flags
        case Op::Movzx:
        case Op::Movsx:
            return rw;  // `exec_integer.cpp:346-355`
        case Op::Lea:
            return rw;  // `exec_integer.cpp:356-360`
        case Op::Xchg:
            return rw;  // `exec_integer.cpp:361-367`
        case Op::Push:
            return rw;  // `exec_integer.cpp:368-370` → `x86.cpp:139-142` push32
        case Op::Pop:
            return rw;  // `exec_integer.cpp:372-374` → `x86.cpp:144-147` pop32
        case Op::Pushad:
        case Op::Popad:
            return rw;  // `exec_integer.cpp:376-396`
        case Op::Cdq:
        case Op::Cwd:
        case Op::Cwde:
        case Op::Cbw:
            return rw;  // `exec_integer.cpp:544-555`
        case Op::Bswap:
            return rw;  // `exec_integer.cpp:556-561`
        case Op::Leave:
            return rw;  // `exec_integer.cpp:660-664`
        case Op::Not:
            return rw;  // `exec_integer.cpp:520-524`（not 不写 flags）
        case Op::Cld:
        case Op::Std:
            return rw;  // `exec_integer.cpp:633-648`：只写 EFLAGS.DF（DF 不在六位里）
        case Op::Div:
        case Op::Idiv:
            // `exec_integer.cpp:270-332`（doDiv）：只有 writeReg / cpuFault，⛔ 无 set*Flags
            return rw;
        case Op::Movs:
        case Op::Stos:
        case Op::Lods:
            // `exec_string.cpp:236-254`；rep 快路径同口径（`exec_string.cpp:84`：EFLAGS 一位不动）
            return rw;
        // ---- 第 2 类：只碰一个已知子集 ---------------------------------------------
        case Op::Setcc:  // `exec_integer.cpp:650-654`：condHolds 读这些位，自己不写
            rw.read = condBits(in.cc);
            return rw;
        case Op::Cmovcc:  // `exec_integer.cpp:656-659`：condHolds 读这些位，只写目的寄存器
            rw.read = condBits(in.cc);
            return rw;
        case Op::Cmpxchg:
            // `exec_integer.cpp:414-427`：`setSubFlags(...)`（:420）⇒ 六位全写；不读 flags
            rw.write = kAllBits;
            return rw;
        case Op::Cmps:
        case Op::Scas:
            // `exec_string.cpp:255-269`：setCmpFlags（:258/:265）⇒ 六位全写；不读 flags。
            // `rep` 前缀（0xF3/0xF2）且 ECX==0 ⇒ 整条不执行、六位一位不动（:275-279）。
            rw.write = kAllBits;
            rw.conditional = (in.rep != 0);
            return rw;
        case Op::Shld:
        case Op::Shrd:
            // `exec_integer.cpp:157-183`（doShxd）：计数 0 或 ≥位宽 ⇒ 一位不写（:164）；
            // 否则写 CF/OF/ZF/SF/PF（:176-180，⛔ 不写 AF）。计数是运行期值（:55-63）⇒ conditional。
            rw.write = static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) | (1u << kBitZf) |
                                                 (1u << kBitSf) | (1u << kBitOf));
            rw.conditional = true;
            return rw;
        // ---- 第 3 类：ALL（保守缺省）------------------------------------------------
        // x87 全族 / Packed / Sse / `Imul` / 循环移位（rol/ror/rcl/rcr 的立即数变体）/
        // `Cpuid` / `Other` / `Hlt` / `Int`/`Int3` / 以及一切没在这里点名的 op。
        default:
            rw.read = kAllBits;
            rw.write = kAllBits;
            return rw;
    }
}

FlagsRW flagsRWOf(const Insn& in) {
    FlagsRW rw;
    if (unsupportedReasonOf(in) != nullptr) return liftOneFlagsRWOf(in);
    rw.write = flagsWriteMaskOf(in);
    switch (in.op) {
        case Op::Pushfd: rw.read = kAllBits; break;                   // 整字读（push32(eflags)）
        case Op::Sahf: rw.read = static_cast<std::uint8_t>(1u << kBitOf); break;  // 只有 OF 要旧值
        case Op::Lahf:                                                // 读五位（不含 OF）
            rw.read = static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) |
                                                (1u << kBitAf) | (1u << kBitZf) |
                                                (1u << kBitSf));
            break;
        case Op::Adc:                                                 // CF 是输入（先读后写）
        case Op::Sbb: rw.read = static_cast<std::uint8_t>(1u << kBitCf); break;
        case Op::Jcc: {
            // §2.4 情形 8：**退化 jcc**（target == fall）在 `emitJcc` 里一位都不读 ⇒ R = 0。
            const bool degenerate = (genBranchTarget(in) == in.ea + in.length);
            rw.read = degenerate ? 0 : condBits(in.cc);
            break;
        }
        case Op::Setcc:
        case Op::Cmovcc: rw.read = condBits(in.cc); break;
        // ★ 检查点 2（D49）：x87 的 `fcmovcc` 族读 EFLAGS（`condHolds` 同一条判据，
        //   `exec_fpu.cpp` 的 `fcmov()`）。⚠ 它们的条件在 `in.op` 里、`in.cc` 是 None ⇒
        //   必须逐个 op 映射（⛔ 不能 `condBits(in.cc)`）。
        case Op::Fcmovb:
        case Op::Fcmovnb: rw.read = static_cast<std::uint8_t>(1u << kBitCf); break;
        case Op::Fcmove:
        case Op::Fcmovne: rw.read = static_cast<std::uint8_t>(1u << kBitZf); break;
        case Op::Fcmovbe:
        case Op::Fcmovnbe:
            rw.read = static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitZf));
            break;
        case Op::Fcmovu:
        case Op::Fcmovnu: rw.read = static_cast<std::uint8_t>(1u << kBitPf); break;
        case Op::Shl:
        case Op::Shr:
        case Op::Sar:
        case Op::Rol:
        case Op::Ror:
        case Op::Rcr:
        case Op::Shld:
        case Op::Shrd:
            // 寄存器计数移位：整条指令的运行期条件写（计数 0 ⇒ 一位都不写）⇒ 旧值必须保留。
            // `emitShiftReg` 用 `select(c_nz, 新值, 旧值)` 表达它 ⇒ 只有**活的位**才要读旧值。
            if (in.op1().kind == OpKind::Reg) {
                rw.read = rw.write;
                rw.conditional = true;
            }
            break;
        case Op::Scas:
        case Op::Cmps:
            // `rep` 前缀且 ECX==0 ⇒ 整条不执行、六位一位不动（`exec_string.cpp:279`）。
            // 计数是运行期值 ⇒ 条件写；`emitString` 用 `select(executed, 新, 旧)` 表达。
            rw.conditional = true;
            break;
        default: break;
    }
    // ★ 2026-11-15：`rcr` **读旧 CF**（它是输入，`exec_integer.cpp:131-142`）。立即数计数 0 ⇒
    //   整条不执行（`:71` 的早退）⇒ 那一位不必读。
    if (in.op == Op::Rcr) {
        const bool skip = (in.op1().kind == OpKind::Imm &&
                           (static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu) == 0);
        if (!skip) rw.read = static_cast<std::uint8_t>(rw.read | (1u << kBitCf));
    }
    return rw;
}

// §2.1 的可融合判据（**纯判据、不发 IR**；发射侧是 `directCond`，两处同一份条件语义）。
bool fusableCond(Op op, Cond cc) {
    if (op != Op::Cmp && op != Op::Test) return false;
    const std::uint8_t need = condBits(cc);
    if (need == 0) return false;
    if ((need & (1u << kBitPf)) != 0) return false;  // PF 不是一条 icmp
    if (op == Op::Cmp && (cc == Cond::O || cc == Cond::NO)) return false;  // cmp 的 OF 不可直读
    return true;
}

// 一个块的 flags 活性（plan §1.6(a) 的 BlockFlags）。⛔ 不进头文件：只有本 TU 的两个消费者
// （活性计算 / FnEmit）用它。
struct BlockFlags {
    std::uint8_t need_in = 0;
    std::uint8_t need_out = 0;
    bool escape = false;
    std::vector<std::uint8_t> wr_mask;   // 与 insns 同长：这条写者该算哪些位
    std::vector<std::uint8_t> mat_mask;  // 与 insns 同长：lift_one 调用前必须先物化的位（c2-lite）
    std::vector<int> fuse_from;          // 与 insns 同长：-1 = 不融合；>=0 = 融合的生产者下标
};

struct BlockScan {
    std::uint8_t live_in = 0;
    std::vector<std::uint8_t> wr_mask;
    // ★ c2-lite：`lift_one` 站点在宿主调用**之前**必须先并进 base 的位
    //   = 解释器真读的位 ∪ 条件写可能原样穿过的活位。inline 站点恒 0（不用）。
    std::vector<std::uint8_t> mat_mask;
    std::vector<int> fuse_from;
};

// **一次后向扫描**同时产出 `wr_mask[]` / `mat_mask[]` / `fuse_from[]`（plan §1.4 + §2.1）。
// ★ 融合候选先做一遍前向游标（它只依赖 W/op/cc，**与活性无关**），后向扫描再把它当成 R=0 的读者。
BlockScan backScanBlock(const GenBlock& b, std::uint8_t need_out) {
    const std::size_t n = b.insns.size();
    BlockScan s;
    s.wr_mask.assign(n, 0);
    s.mat_mask.assign(n, 0);
    s.fuse_from.assign(n, -1);
    // ---- ① 融合候选（前向：最近一次写这些位的指令必须是 cmp/test 且可直读）----
    int owner[6] = {-1, -1, -1, -1, -1, -1};
    for (std::size_t i = 0; i < n; ++i) {
        const Insn& in = b.insns[i];
        const FlagsRW rw = flagsRWOf(in);
        const bool reader = (in.op == Op::Jcc || in.op == Op::Setcc || in.op == Op::Cmovcc);
        const bool degenerate = (in.op == Op::Jcc && genBranchTarget(in) == in.ea + in.length);
        if (!rw.barrier && reader && !degenerate) {
            const std::uint8_t need = condBits(in.cc);
            int j = -1;
            bool ok = (need != 0);
            for (int bit = 0; ok && bit < 6; ++bit) {
                if ((need & (1u << bit)) == 0) continue;
                if (owner[bit] < 0) ok = false;
                else if (j < 0) j = owner[bit];
                else if (j != owner[bit]) ok = false;
            }
            if (ok && j >= 0 && fusableCond(b.insns[static_cast<std::size_t>(j)].op, in.cc))
                s.fuse_from[i] = j;
        }
        for (int bit = 0; bit < 6; ++bit)
            if ((rw.write & (1u << bit)) != 0) owner[bit] = static_cast<int>(i);
    }
    // ---- ② 后向扫描（live = 块尾的 need_out；R 在 W 之后并：RMW 先读后写）----
    std::uint8_t live = need_out;
    for (std::size_t i = n; i-- > 0;) {
        const Insn& in = b.insns[i];
        const FlagsRW rw = flagsRWOf(in);
        std::uint8_t r = rw.read;
        if (s.fuse_from[i] >= 0) r = 0;  // 融合读者**不**从 flags 槽读任何位（§2.5）
        std::uint8_t keep = 0;
        if (rw.barrier) {
            // ★ c2-lite：`lift_one` 不是"整字屏障"了 —— `read` 是**解释器真读**的位（⛔ 不按活性
            //   收紧：那些位可能决定非 flags 结果，例如 `setcc [mem]` 的条件、`rcr` 的 CF 输入）。
            //   条件写（`cmps/scas` 的 rep 计数 0、`shld/shrd` 的计数 0）⇒ 写可能不发生，
            //   于是**调用之后还活着**的那些 W 位必须原样穿过 ⇒ 它们的旧值要在调用前物化。
            //   其余 W 位（无条件写）调用后一定被解释器覆盖，不必物化；~W 的活位留在槽里即可
            //   （调用后只重置 W 位的槽，见 `emitLiftOne`）。
            if (rw.conditional) keep = static_cast<std::uint8_t>(live & rw.write);
            s.wr_mask[i] = static_cast<std::uint8_t>(r | keep);
            s.mat_mask[i] = static_cast<std::uint8_t>(r | keep);
        } else {
            // 条件写的 inline 写者（`shl …, cl` 等）：只读"活着的旧值"（emitShiftReg 用它 select）。
            if (rw.conditional) r = static_cast<std::uint8_t>(r & live);
            s.wr_mask[i] = static_cast<std::uint8_t>(live & rw.write);
        }
        live = static_cast<std::uint8_t>((live & ~rw.write) | r | keep);
    }
    s.live_in = live;
    return s;
}

// 块图上的 flags 活性（plan §1.4）：`needOut(B) = escape(B) ? ALL : ∪ needIn(succ)`、
// `needIn(B) = backScan(B, needOut(B)).live_in`，迭代到不动点（单调增 ⇒ 必收敛）。
bool computeFlagsLiveness(const std::vector<GenComponent>& comps,
                          const std::set<std::uint32_t>& entries,
                          std::map<std::uint32_t, BlockFlags>* out, GenEmitStats* stats,
                          std::string* err) {
    std::map<std::uint32_t, const GenBlock*> block_of;
    std::map<std::uint32_t, const GenComponent*> comp_of;
    for (const GenComponent& c : comps) {
        for (const auto& kv : c.blocks) {
            block_of[kv.first] = &kv.second;
            comp_of[kv.first] = &c;
        }
    }
    out->clear();
    std::map<std::uint32_t, std::vector<std::uint32_t>> succs;
    std::map<std::uint32_t, std::vector<std::uint32_t>> preds;
    // ---- ① escape 分类 + 已解析的静态后继（唯一来源：`genBlockEdges` + 坑①的补边）----
    for (const auto& kv : block_of) {
        const std::uint32_t ea = kv.first;
        const GenBlock& b = *kv.second;
        const GenComponent& comp = *comp_of.at(ea);
        BlockFlags f;
        std::vector<std::uint32_t> ss;
        bool esc = false;
        auto resolve = [&](std::uint32_t t, bool dynamic) {
            if (dynamic) {
                esc = true;  // 间接 jmp/call：目标运行期才知道
                ++stats->flags_escape_indirect;
                return;
            }
            const bool local = comp.blocks.count(t) != 0;
            const auto it = block_of.find(t);
            // 跨分量的边：目标必须是**入口 EA**（trampoline 才派发得到）⇒ 用它的 need_in。
            // 目标 ∉ 任何块（shim 桩 / 三个合成 EA / dropped 派生 leader / unbuildable 入口）
            // 或"有块但不是入口"（派发不出去 = 既有缺陷面）⇒ escape，并逐条计数（⛔ 不静默）。
            if (local || (it != block_of.end() && entries.count(t) != 0)) {
                ss.push_back(t);
                return;
            }
            esc = true;
            ++stats->flags_escape_no_block;
            ++stats->flags_edge_no_block;
        };
        if (!b.insns.empty()) {
            const Insn& last = b.insns.back();
            if (b.terminated && last.op == Op::Ret) {
                // `ret`：目标运行期才知道（保守版 = plan §1.4 的默认实现；S3-d 才收紧）。
                esc = true;
                ++stats->flags_escape_ret;
            } else {
                // ★ 坑①：`call` 有**两条** flags 后继 —— `genBlockEdges` 只给 resume（因为分区把
                //   call-resume 当软根）⇒ 被调方在这里自己补。⛔ 不改 `genBlockEdges`（分区依赖它）。
                //   ★ 补边时必须**自己**判"目标能不能静态知道"：`genBlockEdges` 对 `call` 只发 resume，
                //     **不发** `dynamic` 边（`dynamic` 只来自 `jmp reg/mem` 与 `ret`）⇒ `call reg/mem`
                //     若照抄"没有 dynamic 边"就会漏掉 escape（被调方的 need_in 未知 ⇒ 必须 ALL）。
                if (b.terminated && last.op == Op::Call) {
                    if (last.op0().kind == OpKind::Imm) resolve(genBranchTarget(last), /*dynamic=*/false);
                    else resolve(0, /*dynamic=*/true);  // 目标运行期才知道（reg/mem）⇒ escape
                }
                for (const GenEdge& e : genBlockEdges(b)) resolve(e.ea, e.dynamic);
            }
        }
        f.escape = esc;
        f.need_out = esc ? kAllBits : 0;
        succs[ea] = std::move(ss);
        for (std::uint32_t t : succs[ea]) preds[t].push_back(ea);
        (*out)[ea] = std::move(f);
    }
    // ---- ② 不动点（worklist；初值 ⊥ = 0，escape 固定 ALL，单调增 ⇒ 有限步到顶）----
    std::map<std::uint32_t, std::uint8_t> need_in;
    for (const auto& kv : *out) need_in[kv.first] = 0;
    std::vector<std::uint32_t> work;
    work.reserve(out->size());
    for (const auto& kv : *out) work.push_back(kv.first);
    std::size_t cursor = 0;
    std::size_t steps = 0;
    // 护栏（6 位 × 每块最多 6 次变化 ⇒ 16×(块+边)+64K 绰绰有余）。撞到 = 生成器内部不一致。
    std::size_t edge_count = 0;
    for (const auto& kv : succs) edge_count += kv.second.size();
    const std::size_t kMaxSteps = 16u * (out->size() + edge_count) + 65536u;
    while (cursor < work.size()) {
        if (++steps > kMaxSteps) {
            *err = "flags 活性不动点不收敛（worklist 超过护栏）";
            return false;
        }
        const std::uint32_t ea = work[cursor++];
        BlockFlags& f = (*out)[ea];
        std::uint8_t no = f.escape ? kAllBits : 0;
        for (const std::uint32_t t : succs[ea]) no = static_cast<std::uint8_t>(no | need_in.at(t));
        f.need_out = no;
        const BlockScan sc = backScanBlock(*block_of.at(ea), no);
        if (sc.live_in != need_in[ea]) {
            need_in[ea] = sc.live_in;
            for (const std::uint32_t p : preds[ea]) work.push_back(p);
        }
    }
    // ---- ③ 定型：按最终的 need_out 再扫一遍，存 wr_mask / mat_mask / fuse_from；并记生成期读数 ----
    for (auto& kv : *out) {
        BlockFlags& f = kv.second;
        const BlockScan sc = backScanBlock(*block_of.at(kv.first), f.need_out);
        f.need_in = need_in.at(kv.first);
        f.wr_mask = std::move(sc.wr_mask);
        f.mat_mask = std::move(sc.mat_mask);
        f.fuse_from = std::move(sc.fuse_from);
        ++stats->flags_blocks;
        if (f.escape) ++stats->flags_escape_blocks;
        if (f.need_in == kAllBits) ++stats->flags_need_in_all;
        if (f.need_out == kAllBits) ++stats->flags_need_out_all;
        bool any_write = false;
        for (const std::uint8_t m : f.wr_mask)
            if (m != 0) any_write = true;
        if (f.need_out == 0 && !any_write) ++stats->flags_zero_blocks;
        for (int bit = 0; bit < 6; ++bit)
            if ((f.need_out & (1u << bit)) != 0) ++stats->flags_tail_bits[bit];
    }
    return true;
}

std::uint32_t maskOfSize(unsigned size) {
    return size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}
unsigned bitsOfSize(unsigned size) { return size == 1 ? 8u : (size == 2 ? 16u : 32u); }
std::uint32_t signOfSize(unsigned size) {
    return size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
}

std::string i32c(std::uint32_t v) { return std::to_string(static_cast<std::int32_t>(v)); }
std::string i64c(std::uint64_t v) { return std::to_string(static_cast<std::int64_t>(v)); }
std::string hexEa(std::uint32_t ea) {
    char b[16];
    std::snprintf(b, sizeof(b), "%X", ea);
    return b;
}

// =====================================================================================
// 一个入口函数的发射器
// =====================================================================================
class FnEmit {
public:
    FnEmit(const GenComponent& comp, const std::set<std::uint32_t>& entries,
           const std::set<std::uint32_t>& dropped_blocks,
           const std::map<std::uint32_t, BlockFlags>& flags, const GenStructure& structure,
           const std::map<std::uint32_t, std::uint32_t>& entry_owner, GenEmitStats* stats,
           std::vector<GenUnsupported>* unsupported, const GenIrTarget& ir_target)
        : comp_(comp), entries_(entries), dropped_(dropped_blocks), flags_(flags),
          structure_(structure), owner_(entry_owner), stats_(stats), unsupported_(unsupported),
          ir_target_(ir_target) {}

    std::string run();
    // ★ S3：生成期硬错（不是警告）。`emitModule` 见到 true ⇒ 返回 false ⇒ `lift-gen` 退出码 1。
    bool failed() const { return failed_; }
    const std::string& failWhy() const { return fail_why_; }

private:
    // ---- 基础 ----
    std::string tmp() { return "%t" + std::to_string(tmp_++); }
    void add(const std::string& s) { body_ += "  " + s + "\n"; }
    void put(const std::string& s) { body_ += s; }

    std::string bin(const char* op, const char* ty, const std::string& a, const std::string& b) {
        std::string r = tmp();
        add(r + " = " + op + " " + ty + " " + a + ", " + b);
        return r;
    }
    std::string bin1(const char* op, const std::string& a, const std::string& b) {
        std::string r = tmp();
        add(r + " = " + op + " i1 " + a + ", " + b);
        return r;
    }
    std::string not1(const std::string& a) { return bin1("xor", a, "true"); }
    std::string and1(const std::string& a, const std::string& b) { return bin1("and", a, b); }
    std::string or1(const std::string& a, const std::string& b) { return bin1("or", a, b); }
    std::string select1(const std::string& c, const char* ty, const std::string& a,
                        const std::string& b) {
        std::string r = tmp();
        add(r + " = select i1 " + c + ", " + ty + " " + a + ", " + ty + " " + b);
        return r;
    }
    // ★ A2：`and i32 %x, -1` 是**恒等** ⇒ 零语句（`maskVal` 已经对 size==4 早退；
    //   这里兜住 `emitShift` / `emitShiftReg` 直接拿 `maskOfSize(4)` 的那些站点）。
    //   语义零风险：`x & 0xFFFFFFFF == x`（i32 位精确）。
    std::string andv(const std::string& a, std::uint32_t mask) {
        if (mask == 0xFFFFFFFFu) return a;
        return bin("and", "i32", a, i32c(mask));
    }
    std::string maskVal(const std::string& v, unsigned size) {
        return size >= 4 ? v : andv(v, maskOfSize(size));
    }
    std::string orList(std::initializer_list<std::string> xs) {
        std::string acc;
        for (const std::string& x : xs) acc = acc.empty() ? x : bin("or", "i32", acc, x);
        return acc;
    }

    // ---- 寄存器 / 内存 ----
    std::string loadReg(int index);
    // ★ A3：`%st + 4*index` 的 GEP —— **按需**发射、同一块内同槽复用（见 `FnEmit::stGep`）。
    std::string stGep(int index);
    void storeReg(int index, const std::string& v);
    std::string readRegValue(Reg reg);
    void writeRegValue(Reg reg, const std::string& v);
    std::string eaOf(const MemOperand& m);
    std::string loadMemValue(const MemOperand& m);
    void storeMemValue(const MemOperand& m, const std::string& v);
    // ★ 按**显式 EA**（而不是 `MemOperand`）读 / 写一个 guest 值。为什么需要第二组：
    //   `bt/bts/btr/btc` 的内存形要在**运行时**按位偏移调整地址
    //   （SDM：`EA + (BitOffset DIV 位宽) * 字节数`）—— 那个 delta 是 SSA 值，
    //   塞不进 `MemOperand::disp`（编译期常量）。`loadMemValue/storeMemValue` 本身就是这一组
    //   的薄包装 ⇒ 读写语义（align 1 / zext / `lift_note_write`）只有一份。
    std::string loadMemAt(const std::string& ea, unsigned size);
    void storeMemAt(const std::string& ea, unsigned size, const std::string& v);
    std::string loadAt(const std::string& ea);
    std::string memPtrFor(const std::string& ea);
    std::string readValue(const Operand& op);
    void writeDest(const Operand& op, const std::string& v);

    // ---- 栈（照 X86::push32 / pop32；⛔ 不调 lift_note_write）----
    void pushValue(const std::string& v);
    std::string popValue();

    // ---- flags（S3-a：六位 i1 槽 + base 字；唯一出口 materialize）----
    // 读：`readBit`（FromBase ⇒ 从 base 位取；Value ⇒ 直接给 SSA）；写：`deliverBit` / `killBit`；
    // 出口：`materialize(mask)`（把 mask 里的 lazy 位并进 base 字并 store 到缓存槽 8）。
    std::string loadEflags();
    void storeEflags(const std::string& v);
    std::string readBit(int b);
    void deliverBit(int b, std::string v);
    void killBit(int b);
    void beginFlagWrite(std::uint8_t w, std::uint8_t mask);
    std::uint8_t valueMask() const;
    void materialize(std::uint8_t mask, bool allow_poison = false);
    void resetFlagsSlots();
    std::string flagCond(const std::string& eflags, std::uint32_t bit);
    std::string parity(const std::string& v);
    // §1.7 的**按位生产者**（只算 mask 里的位；mask==0 ⇒ 一条语句都不发）
    void deliverLogicBits(std::uint8_t mask, const std::string& result, unsigned size);
    void deliverAddBits(std::uint8_t mask, const std::string& a, const std::string& b,
                        const std::string& result, unsigned size, bool with_carry);
    void deliverSubBits(std::uint8_t mask, const std::string& a, const std::string& b,
                        const std::string& result, unsigned size, bool with_borrow);
    // §1.3 + §1.2 的唯一读法（不融合时）：按条件码只读它需要的位
    std::string condValue(Cond cc);
    // §1.2 的 W 表（这条指令写哪些位；0 = 不碰 flags；kAllBits = 屏障/整字覆写）
    std::uint8_t flagsWriteMask(const Insn& in) const;
    // ★ S3-c：这条指令该**真的算**哪些位（= `BlockFlags::wr_mask[i]`；块内第 i 条）。
    //   `mask ⊆ W`；`W \ mask` 的位是**死位** ⇒ `beginFlagWrite` 会 `killBit`（⛔ 不许留旧 Value）。
    std::uint8_t wrMaskAt(int index) const;
    // ★ c2-lite：`lift_one` 站点宿主调用前必须物化的位（= `BlockFlags::mat_mask[i]`；inline 恒 0）。
    std::uint8_t matMaskAt(int index) const;
    // S3-b：发射后记账（每位最后写者 + 最近一条 cmp/test 的 SSA 操作数）。
    // `write_mask` = 这条指令**逻辑上的 W**（`lift_one` 用 `liftOneFlagsRWOf` 的收窄结果，⛔ 不再一律 ALL）。
    void recordFlagsWrites(const Insn& in, int index, std::uint8_t write_mask);
    // S3-b：条件值（能融合 ⇒ 直接 icmp/常量；否则退化到逐位惰性读）。
    // `kind` 只用于计数分桶：0 = jcc、1 = setcc、2 = cmovcc。
    std::string condForReader(Cond cc, int kind);
    // §2.2 的 16 种条件 → 直接 IR（空串 = 不可融合 ⇒ 调用方退化）
    std::string directCond(Op op, Cond cc, const std::string& a, const std::string& b,
                           const std::string& r, unsigned size);
    // 按 size 的**符号扩展**（size==4 ⇒ 零语句；否则 shl+ashr 两条）
    std::string signExt(const std::string& v, unsigned size);
    // 生成期硬错（⛔ 不是警告）：emitModule 返回 false ⇒ `lift-gen` 退出码 1（构建红）
    void fail(const std::string& why);

    // ---- 控制流 ----
    std::string labelFor(std::uint32_t ea);
    void emitEdge(std::uint32_t ea);
    // ---- S7（D53）：一对一结构翻译 ----
    // ★ 判据（能否结构化 / 传递闭包）在 `aot/lift_struct.*`，这里**只消费结论**：
    //   · `selfStructured()`：本单元根可一对一转译（它的 `ret` 返回给宿主调用者）；
    //   · `structuredRoot(r)`：某个 callee 单元可结构化的前提；
    //   · `mustDispatchAt(ea)`：宿主语义钩子 EA（skip-logo / recovery）**必须**留在派发边界
    //     （见 `aot/aot_hook_ea.h`：S7 之后控制流不再逐函数回派发器 ⇒ 钩子会静默失效）。
    bool selfStructured() const { return structure_.structured_roots.count(comp_.root) != 0; }
    bool structuredRoot(std::uint32_t root) const {
        return structure_.structured_roots.count(root) != 0;
    }
    bool inShimPage(std::uint32_t ea) const {
        return ea >= structure_.shim_begin && ea < structure_.shim_end;
    }
    bool mustDispatchAt(std::uint32_t ea) const { return structure_.hook_eas.count(ea) != 0; }
    std::uint32_t ownerOf(std::uint32_t ea) const;
    // 宿主 `call` / `lift_dyn` 之后的**唯一** continuation 判据：
    //   `%r == 本次 call 的 resume EA` ⇔ 目标正常返回（它 `ret` 弹的就是我们压的那一格）
    //   ⇒ 在本宿主帧里 `br` 到 resume 块继续；否则它把控制权交给了别处 ⇒ 原样上抛 `ret i32 %r`
    //   （单调回退：goal 语义仍是"下一个 guest EA"，与今天的 trampoline 口径逐位相同）。
    void emitCallReturnCheck(const std::string& r, std::uint32_t resume);
    // `tail call` 到目标单元 + `ret i32 %r`（跨函数 jmp 的落点；LLVM 会把它变成 jmp）。
    void emitTailCallTo(std::uint32_t owner_root, std::uint32_t target);

    // ---- 指令计数（D2）/ 单指令宿主原语（T1c）----
    // flushTick：把**已经执行完**的条数一次 `lift_tick` 发出去（块边界 + 任何宿主调用之前）。
    void flushTick();
    // ---- 块内寄存器缓存（E2 / S2）----
    // 口径：`%st` 仍是**唯一真源**，缓存只在**块内**有效。
    //   · flushRegs()：把脏槽写回 `%st`（`reg_dirty_` 过滤 ⇒ 没写过就不发 store）；**不**清缓存
    //     （写回后缓存里的值仍等于 `%st` 里的值 ⇒ 后续 load 仍可命中）。
    //   · invalidateRegs()：整份作废（`lift_one` 之后 / 每个块开头 —— 前者的 `%st` 被整块回读，
    //     后者是"块开头的状态来自调用方"）。
    //   · syncBeforeHostCall()：`flushTick()` + `flushRegs()` —— **任何**宿主调用之前调它。
    void flushRegs();
    void invalidateRegs();
    void syncBeforeHostCall();
    void emitLiftOne(const Insn& in, bool terminating);

    // ---- 指令 ----
    void emitBlock(const GenBlock& block);
    void emitInsn(const Insn& in);
    void emitAlu(const Insn& in);
    void emitIncDec(const Insn& in);
    void emitShift(const Insn& in);
    void emitShiftReg(const Insn& in);
    void emitMul(const Insn& in);
    void emitBitTest(const Insn& in);
    void emitBitScan(const Insn& in);
    // ---- 2026-11-15（本轮：`lift_one` 调用点 → 0）----
    // 每条都照抄解释器的 handler（`文件:行` 写在定义处）；⛔ 不发明第二份语义。
    void emitStop(const Insn& in, std::uint32_t kind);   // Hlt/Int3/Int/Other/Invalid → `lift_stop`
    void emitImul(const Insn& in);                       // `exec_integer.cpp:214-268`
    void emitDivIdiv(const Insn& in);                    // `exec_integer.cpp:270-332`（#DE）
    void emitString(const Insn& in);                     // `exec_string.cpp:219-304`
    void emitShldShrd(const Insn& in);                   // `exec_integer.cpp:157-183`
    void emitCmpxchg(const Insn& in);                    // `exec_integer.cpp:414-427`
    void emitCpuid(const Insn& in);                      // `exec_integer.cpp:595-614`
    void emitPacked(const Insn& in);                     // `exec_fpu.cpp:470-920`
    void emitSse(const Insn& in);                        // `exec_fpu.cpp:1020-1259`
    // MMX/SSE 的向量读写（与 `pkLoad`/`pkStore` 同一份形态判据）：统一载成 `<16 x i8>`。
    //   · `mm`（`RegClass::X87`）⇒ 固定物理槽的低 8 字节（⛔ 不过 `phys()`，见 `exec_fpu.cpp:518-522`）
    //   · `xmm` ⇒ `%cpu + kXmm + 16*idx`
    //   · `m32/m64/m128` ⇒ guest 内存（align 1）
    std::string vecLoad(const Operand& op, int* bytes);
    void vecStore(const Operand& op, const std::string& v, int bytes);
    std::string cpuBytePtr(std::uint64_t off);
    // 通用：把 `<16 x i8>` 按 lane 类型 bitcast / 还原
    std::string bc(const std::string& v, const char* ty);
    // x87 的 10 字节扩展格式（`fpu_bridge.cpp:27-50` 的 m80ToDouble）与其反向（`:52-...`）
    std::string m80ToDouble(const std::string& p);
    void doubleToM80(const std::string& v, const std::string& p);
    // 环境 / 保存区（`exec_fpu.cpp:337-379`：doSaveEnv / doLoadEnv / doFnsave / doFrstor / doFninit）
    void emitX87SaveEnv(const MemOperand& m, bool with_regs);
    void emitX87RestoreEnv(const MemOperand& m, bool with_regs);

    // ---- x87（D49：状态在 `%cpu` 里；偏移契约见 `runtime/x87_layout.h`）----
    // ★ 语义真源 = `exec_fpu.cpp` 的 `execFpu`（解释器臂）。这里逐条照抄它的**状态机**：
    //   8 个 double 槽 / TOP / tag word / 状态字 C0..C3 / 控制字（PC 无效，RC 只给 fist*/frndint）。
    //   ⛔ 算术用普通 LLVM 浮点指令（`fadd double` / `fmul double` / `fptosi`…），⛔ 不调宿主 helper。
    //   ★ 只有"超越函数 + fprem/fscale/fxtract/fxam"走宿主 helper `@lift_x87_sem(ptr, i32)`：
    //     它们在本语料合计 <100 次/300 帧，且实现在 `exec_fpu.cpp` 里**同一份**（⛔ 不重写公式）。
    std::string cpuGep(std::uint64_t off);
    std::string x87LoadU16(std::uint64_t off);
    void x87StoreU16(std::uint64_t off, const std::string& v);
    std::string x87LoadTop();
    void x87StoreTop(const std::string& v);
    std::string x87Phys(int logical);
    std::string x87SlotPtr(int logical);
    std::string x87LoadSt(int logical);
    void x87StoreSt(int logical, const std::string& v);
    std::string x87TagBitsAt(int logical);  // i16：(tag >> (2·phys)) & 3
    void x87WriteTagSlot(int logical, int known_zero);  // known_zero: -1 未知 / 0 valid / 1 zero
    void x87SetTagEmpty(int logical);
    void x87SetCC(const std::string& c0, const std::string& c1, const std::string& c2,
                  const std::string& c3);
    // status |= （异常位 | ES）—— ES（0x0080）= 状态字里任一异常位非零（任务 0 / D55）
    void x87OrStatus(const std::string& bits, const std::string& why = std::string());
    // ★ 任务 0：上溢 / 下溢的**同一份判据**（与 `fpu_bridge.cpp` 的
    //   `x87RaiseOverflowUnderflow` 逐条对应）：
    //     OE ⇐ `r` 是 ±inf 且 `a`、`b` 都有限（⇒ 运算上溢，不是 inf 传播 / 除零）
    //     UE ⇐ `r` 是次正规（`|r| < DBL_MIN && r != 0`）
    //   `not_zero_den` 非空时（`fdiv`/`fdivr`）⇒ OE 还要 `not_zero_den` 为真（除零是 ZE）。
    //   PE **恒 0**（⛔ 不发射任何 PE 判据；见 `fpu_bridge.h` 文件头 §5）。
    void x87RaiseFloatFlags(const std::string& a, const std::string& b, const std::string& r,
                            const std::string& not_zero_den = std::string());
    void x87SyncTop();                          // 状态字的 TOP 镜像（解释器 syncStatus 同一条）
    void x87Push(const std::string& val, int known_zero);
    void x87Pop();
    void x87StoreConst(double v);  // 已知零性 ⇒ tag 位常量（省一条 fcmp）
    std::string x87LoadMemF64(const MemOperand& m);
    void x87StoreMemF64(const MemOperand& m, const std::string& v);
    std::string x87LoadMemF32(const MemOperand& m);
    void x87StoreMemF32(const MemOperand& m, const std::string& v);
    std::string x87ToInt(const std::string& v, unsigned size);  // 按 RC 四档 + 越界钳制
    void x87FistStore(const Insn& in);
    void emitX87(const Insn& in);
    void emitX87Arith(const Insn& in);
    void emitX87Compare(const Insn& in, bool pop_after, bool quiet);
    void emitX87CmpEflags(const Insn& in, bool pop_after, bool quiet);
    void emitX87Fcmov(const Insn& in);
    void emitX87HelperCall(const Insn& in);
    std::string x87CondValue(const Insn& in);

    void emitTerminator(const Insn& in);
    void emitCall(const Insn& in);
    void emitJcc(const Insn& in);
    void emitRet(const Insn& in);
    void emitTrap(const Insn& in, const char* reason);
    const char* unsupportedReason(const Insn& in) const;

    const GenComponent& comp_;
    const std::set<std::uint32_t>& entries_;
    const std::set<std::uint32_t>& dropped_;  // 被丢弃的派生 leader（没有块 ⇒ 边发 `ret i32`）
    const std::map<std::uint32_t, BlockFlags>& flags_;  // ★ S3-c：块首 EA → 活性（全部块，全局一份）
    const GenStructure& structure_;                      // ★ S7：结构翻译的判据结论（全局一份）
    const std::map<std::uint32_t, std::uint32_t>& owner_;  // ★ S7：入口 EA → 单元根
    GenEmitStats* stats_;
    std::vector<GenUnsupported>* unsupported_;
    // ★ 本单：`define` 行的 `#0 {` 与 `emitPrologue` 的 `attributes #0 = {...}` 是**同一份**取值
    //   （都由 `lift-gen --ir-target-*` 传进来）。`empty()` ⇒ 定义行与今天逐字节相同（⛔ 不发 `#0`）。
    const GenIrTarget& ir_target_;
    std::string body_;
    int tmp_ = 0;
    std::set<std::uint32_t> exit_ea_;  // 需要单独 `ret` 块的入口 EA（jcc 的两个出口）
    // ★ 块内寄存器缓存（E2 / S2）：下标 = `LiftRegs` 的 dword 下标（0..7 = GPR，8 = EFLAGS@+32）。
    //   非空 = 该槽的**当前值**（一个 i32 SSA 值名，不一定已经在 `%st` 里）；`reg_dirty_` = 还没写回。
    //   ★ EFLAGS 与 GPR 走**同一套**槽（索引 8）—— 这正是"只搬一次，而不是每条指令整份搬"的落点。
    std::string reg_cache_[9];
    bool reg_dirty_[9] = {false, false, false, false, false, false, false, false, false};
    // ★ A3：`%st + 4*i` 的 GEP 值（**块内**缓存；空 = 还没发过）。
    //   为什么块内有效就够：GEP 是纯计算（不读不写内存），唯一要求是**支配**使用点 ——
    //   "首次使用时发射"保证同一块里后面的使用都在它之后；块入口 / `lift_one` 之后由
    //   `invalidateRegs()` 清空（⛔ 不复用别的块里定义的值，那会是不支配的 SSA 使用）。
    std::string st_gep_[9];
    // ★ S3-a：六个 flags 位的三态槽（下标 = EFLAGS 位位置：0=CF 1=PF 2=AF 3=ZF 4=SF 5=OF）。
    enum class Slot : std::uint8_t { FromBase, Value, Poison };
    Slot slot_[6] = {Slot::FromBase, Slot::FromBase, Slot::FromBase,
                     Slot::FromBase, Slot::FromBase, Slot::FromBase};
    std::string slot_val_[6];  // 只有 `Slot::Value` 有意义
    // ★ S3-b / S3-c：融合账。`bit_owner_[b]` = 本块内最后一条写过该位的指令下标（-1 = 本块内没有写者）；
    //   `cmp_recs_[i]` = 第 i 条指令若是 cmp/test 时**当刻**的 SSA 操作数（⛔ 不用 `const Insn*` 重算：
    //   寄存器可能已经被后面改过，融合要的是**生产者那一刻**的值）。
    //   ★ 融合判据由后向扫描给出（`fuse_from[i]` = 生产者下标），前向发射处只做**一致性校验**
    //   （`fuse_from` 说能融 ⇒ `bit_owner_`/`cmp_recs_` 必须对得上，否则生成期硬错）。
    int bit_owner_[6] = {-1, -1, -1, -1, -1, -1};
    struct CmpRec {
        bool valid = false;
        int index = -1;
        Op op = Op::Invalid;
        std::string a, b, r;
        unsigned size = 4;
    };
    std::vector<CmpRec> cmp_recs_;  // 与 block.insns 同长（每块重新赋值）
    // ★ S3-c：当前块的活性（块尾要物化哪些位 / 每条写者的掩码 / 融合候选）。
    const BlockFlags* fb_ = nullptr;
    std::uint8_t need_out_ = 0;
    int cur_index_ = -1;  // 当前发射的指令在本块 `insns` 里的下标（-1 = 不在块内）
    // `emitAlu` 在发射 cmp/test 时把操作数记在这里，`recordFlagsWrites` 再挂上指令下标。
    struct CmpOps {
        Op op = Op::Invalid;
        std::string a, b, r;
        unsigned size = 4;
    };
    CmpOps cmp_ops_;
    // ★ 2026-11-15：条件写者的 `deliverBit` 门（非空 ⇒ `deliverBit(b, v)` 变成
    //   `deliverBit(b, select(guard, v, 旧值))`）。用在"整条指令可能一条都不执行"的站点
    //   （`rep scas/cmps` 的 ECX==0、`shld/shrd`/`rcr` 的计数 0）。空 = 不设门（绝大多数字段）。
    std::string deliver_guard_;
    // 生成期硬错（⛔ 不许静默）：`emitModule` 检查它并返回 false。
    bool failed_ = false;
    std::string fail_why_;
    std::uint32_t cur_ea_ = 0;  // 当前发射的 guest 指令 EA（只用于错误信息）
    // ★ 挂账的**已执行**条数（D2）：每发射完一条 guest 指令 +1，在块边界 / 宿主调用之前 flush。
    //   `lift_one` 的那一条只在**成功**路径上算（失败 ⇒ 停机，与解释器"失败的这条不算执行过"同口径）。
    int pending_ticks_ = 0;
    // ★ `lift_one` 的两个出口：`lo_ok_<n>` 在块内继续，`lo_halt_<n>` 收集起来在**函数尾**统一发
    //   （不能就地发：`ret` 之后的指令会被 LLVM 当成不可达块的新指令）。序号 = 发射顺序 ⇒ 确定性。
    // ★ 2026-11-15：停机出口（label, EA, `@lift_stop` 的 kind；kind = 0xFFFFFFFF ⇒ 只发 `ret kLiftHalt`）。
    struct HaltSite {
        std::string label;
        std::uint32_t ea;
        std::uint32_t kind;
    };
    std::vector<HaltSite> halt_labels_;
    int label_id_ = 0;
    // ★ `emitTrap` 之后**必须**停止发射本块（P5：`unreachable` 后面再发指令 = 非法 IR）。
    //   机械判据（而不是"今天到不了"的隐式不变量）。
    bool trapped_ = false;
};

// ---------------------------------------------------------------------------------------
// 基础：寄存器 / 内存
// ---------------------------------------------------------------------------------------
// ★ E2 / S2：这两个函数**走块内缓存**（语义与"每条指令直接读写 `%st`"逐位相同，见文件头注）。
std::string FnEmit::stGep(int index) {
    std::string& g = st_gep_[index];
    if (g.empty()) {
        g = tmp();
        add(g + " = getelementptr i8, ptr %st, i64 " + std::to_string(4 * index));
    }
    return g;
}

std::string FnEmit::loadReg(int index) {
    if (!reg_cache_[index].empty()) return reg_cache_[index];  // 块内已有当前值 ⇒ 零 IR
    const std::string g = stGep(index);  // ★ A3：同块同槽只发一条 GEP（原来每次访问都重发）
    std::string v = tmp();
    add(v + " = load i32, ptr " + g + ", align 4");
    reg_cache_[index] = v;
    reg_dirty_[index] = false;  // 刚从 `%st` 读出来 ⇒ 干净
    return v;
}

void FnEmit::storeReg(int index, const std::string& v) {
    reg_cache_[index] = v;
    reg_dirty_[index] = true;
}

// 把脏槽写回 `%st`。⛔ 只在"宿主可观察点"之前调：块出口（`ret` / `br`）与任何宿主调用之前。
void FnEmit::flushRegs() {
    for (int i = 0; i < 9; ++i) {
        if (!reg_dirty_[i]) continue;
        const std::string g = stGep(i);  // ★ A3：与 `loadReg` 共用同一条 GEP（原来这里重发一条）
        add("store i32 " + reg_cache_[i] + ", ptr " + g + ", align 4");
        reg_dirty_[i] = false;
    }
}

void FnEmit::invalidateRegs() {
    for (int i = 0; i < 9; ++i) {
        reg_cache_[i].clear();
        reg_dirty_[i] = false;
        st_gep_[i].clear();  // ★ A3：GEP 缓存与寄存器缓存同寿命（块内）
    }
}

void FnEmit::syncBeforeHostCall() {
    flushTick();
    flushRegs();
}

std::string FnEmit::readRegValue(Reg reg) {
    const RegSpec spec = regSpec(reg);
    // ★ 2026-11-15：段寄存器**在 `%cpu` 里**（`X86::seg[6]`，u16；偏移契约 = `x87l::kSeg`）。
    //   语义真源 `x86.cpp:31`：`case RegClass::Seg: return seg[spec.index];`（零扩展到 i32）。
    //   ⛔ 不走 `loadReg`（那是 `%st` 的 8 GPR + eflags，里面没有 seg）。
    if (spec.cls == RegClass::Seg) {
        const std::string h = tmp();
        add(h + " = load i16, ptr " + cpuGep(x87l::kSeg + x87l::kSegStride * spec.index) +
            ", align 2");
        const std::string z = tmp();
        add(z + " = zext i16 " + h + " to i32");
        return z;
    }
    const std::string full = loadReg(spec.index);
    switch (spec.cls) {
        case RegClass::Gpr32: return full;
        case RegClass::Gpr16: return andv(full, 0xFFFFu);
        case RegClass::Gpr8: {
            if (spec.byte_off == 0) return andv(full, 0xFFu);
            std::string hi = bin("lshr", "i32", full, "8");
            return andv(hi, 0xFFu);
        }
        default: return "0";
    }
}

// 照 `X86::writeReg`：32 位整写；16 位保留高 16；8 位低/高半各自只改自己的字节。
void FnEmit::writeRegValue(Reg reg, const std::string& v) {
    const RegSpec spec = regSpec(reg);
    // ★ 2026-11-15：段寄存器写（`x86.cpp:55`：`seg[index] = value & 0xFFFF`）。
    if (spec.cls == RegClass::Seg) {
        const std::string t = tmp();
        add(t + " = trunc i32 " + v + " to i16");
        add("store i16 " + t + ", ptr " + cpuGep(x87l::kSeg + x87l::kSegStride * spec.index) +
            ", align 2");
        return;
    }
    switch (spec.cls) {
        case RegClass::Gpr32: storeReg(spec.index, v); break;
        case RegClass::Gpr16: {
            const std::string cur = loadReg(spec.index);
            storeReg(spec.index, bin("or", "i32", andv(cur, 0xFFFF0000u), andv(v, 0xFFFFu)));
            break;
        }
        case RegClass::Gpr8: {
            const unsigned shift = 8u * spec.byte_off;
            const std::string cur = loadReg(spec.index);
            std::string cleared = andv(cur, ~(0xFFu << shift));
            std::string part = andv(v, 0xFFu);
            if (shift != 0) part = bin("shl", "i32", part, i32c(shift));
            storeReg(spec.index, bin("or", "i32", cleared, part));
            break;
        }
        default: break;
    }
}

// 照 `X86::eaOf`：disp 起头，+ base，+ index*scale（32 位回绕）。
// ★ A2：**不带零项**（`add i32 0, %x` / `add i32 %x, 0` 是恒等 ⇒ 零语句）。
//   求值顺序与旧式 `((disp + base) + index*scale)` 完全一致，只是把 disp==0 那一项省掉；
//   全空（disp==0 且无 base/index）仍然是常量 `0`。⚠ 32 位回绕语义不变（加法仍逐条发）。
std::string FnEmit::eaOf(const MemOperand& m) {
    std::string ea;
    if (m.disp != 0) ea = i32c(m.disp);
    if (m.base != Reg::None) {
        const std::string b = loadReg(regSpec(m.base).index);
        ea = ea.empty() ? b : bin("add", "i32", ea, b);
    }
    if (m.index != Reg::None) {
        std::string ix = loadReg(regSpec(m.index).index);
        if (m.scale != 1) ix = bin("mul", "i32", ix, i32c(m.scale));
        ea = ea.empty() ? ix : bin("add", "i32", ea, ix);
    }
    // ★ 2026-11-15：`fs:` / `gs:` 覆盖 —— 语义真源是 `x86.cpp:68-75` 的 `X86::eaOf`：
    //   `if (m.seg_fs && fs_base) ea += fs_base;`（加 0 是恒等 ⇒ 这里无条件加，逐位相同）。
    //   基址来自 `%cpu`（偏移契约在 `runtime/x87_layout.h`），⛔ 不是宿主段寄存器。
    if (m.seg_fs || m.seg_gs) {
        const std::string b = tmp();
        add(b + " = load i32, ptr " + cpuGep(m.seg_fs ? x87l::kFsBase : x87l::kGsBase) +
            ", align 4");
        ea = ea.empty() ? b : bin("add", "i32", ea, b);
    }
    return ea.empty() ? std::string("0") : ea;
}

std::string FnEmit::loadMemAt(const std::string& ea, unsigned size) {
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    const std::string v = tmp();
    // ★ 一律 align 1：guest 地址不保证对齐（夹具里 kBuf 就是故意非对齐的）；
    //   `align 4` 在 LLVM 里是"我保证对齐"的承诺，违约是 UB。语义优先。
    switch (size) {
        case 1: {
            add(v + " = load i8, ptr " + p + ", align 1");
            const std::string z = tmp();
            add(z + " = zext i8 " + v + " to i32");
            return z;
        }
        case 2: {
            add(v + " = load i16, ptr " + p + ", align 1");
            const std::string z = tmp();
            add(z + " = zext i16 " + v + " to i32");
            return z;
        }
        default: add(v + " = load i32, ptr " + p + ", align 1"); return v;
    }
}

std::string FnEmit::loadMemValue(const MemOperand& m) { return loadMemAt(eaOf(m), m.size); }

// 照 `X86::writeMem`：先 `lift_note_write`（SMC 失效）再写。`push/pop` 不走这里。
// ★ `ir_target_.note_write`（CMake `LIFT_AOT_NOTE_WRITE`，缺省 OFF）：
//   OFF ⇒ 只发 store（纯 guest store **不是**宿主调用 ⇒ ⛔ 不发 `syncBeforeHostCall()`，也就
//   不需要为它 `flushTick/flushRegs`）；ON ⇒ 与今天逐字节一致（SMC 记账 + 调用前的同步）。
void FnEmit::storeMemAt(const std::string& ea, unsigned size, const std::string& v) {
    if (ir_target_.note_write) {
        // ★ 宿主调用之前先把挂账的条数发出去 + 把块内寄存器缓存写回 `%st`（E2 / S2）：
        //   `lift_note_write` 也是宿主调用（口径：**任何**宿主调用之前 `%st` 都是同步的）。
        //   本条自己**还没**执行完 ⇒ 不在这批 tick 里（pending 只含之前的指令）。
        //   ★ 顺序：先 tick 再 flush —— 两者都在调用之前，互不依赖；`flushRegs()` **不**清缓存，
        //     所以 `ea` 在调用方算得再早也不受影响（flush 只写回 `%st`，不改 SSA 值）。
        syncBeforeHostCall();
        add("call void @lift_note_write(ptr %cpu, i32 " + ea + ", i32 " + std::to_string(size) + ")");
        stats_->used_note_write = true;
    }
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    switch (size) {
        case 1: {
            const std::string t = tmp();
            add(t + " = trunc i32 " + v + " to i8");
            add("store i8 " + t + ", ptr " + p + ", align 1");
            break;
        }
        case 2: {
            const std::string t = tmp();
            add(t + " = trunc i32 " + v + " to i16");
            add("store i16 " + t + ", ptr " + p + ", align 1");
            break;
        }
        default: add("store i32 " + v + ", ptr " + p + ", align 1"); break;
    }
}

void FnEmit::storeMemValue(const MemOperand& m, const std::string& v) {
    storeMemAt(eaOf(m), m.size, v);
}

// 从显式 EA 读一个 i32（`ret` 的返回地址 / `leave` 的保存 EBP 用）。
std::string FnEmit::loadAt(const std::string& ea) {
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    const std::string v = tmp();
    add(v + " = load i32, ptr " + p + ", align 1");
    return v;
}

// 照 `rd(cpu, op) = readOp(op) & maskOf(op.size)`。
std::string FnEmit::readValue(const Operand& op) {
    const unsigned size = op.size != 0 ? op.size : 4u;
    switch (op.kind) {
        case OpKind::Reg: return maskVal(readRegValue(op.reg), size);
        case OpKind::Imm: return i32c(static_cast<std::uint32_t>(op.imm) & maskOfSize(size));
        case OpKind::Mem: return maskVal(loadMemValue(op.mem), size);
        default: return "0";
    }
}

// 照 `wr(cpu, op, v)`：先按 op.size 截断，再按 Reg（合并）/ Mem（writeMem）写。
void FnEmit::writeDest(const Operand& op, const std::string& v) {
    const unsigned size = op.size != 0 ? op.size : 4u;
    const std::string x = maskVal(v, size);
    switch (op.kind) {
        case OpKind::Reg: writeRegValue(op.reg, x); break;
        case OpKind::Mem: storeMemValue(op.mem, x); break;
        default: break;
    }
}

void FnEmit::pushValue(const std::string& v) {
    const std::string esp = loadReg(4);
    const std::string esp2 = bin("sub", "i32", esp, "4");
    storeReg(4, esp2);
    const std::string p_ea = memPtrFor(esp2);
    add("store i32 " + v + ", ptr " + p_ea + ", align 1");
}

// `pushValue` 的地址计算抽出来（避免两条语句里各写一遍 zext/gep 的样板）。
std::string FnEmit::memPtrFor(const std::string& ea) {
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    return p;
}

std::string FnEmit::popValue() {
    const std::string esp = loadReg(4);
    const std::string v = loadAt(esp);
    const std::string esp2 = bin("add", "i32", esp, "4");
    storeReg(4, esp2);
    return v;
}

// ---------------------------------------------------------------------------------------
// flags（S3-a：六位 i1 槽 + base 字；逐位照 exec.cpp / exec_integer.cpp）
// ---------------------------------------------------------------------------------------
std::string FnEmit::loadEflags() { return loadReg(8); }
void FnEmit::storeEflags(const std::string& v) { storeReg(8, v); }

std::string FnEmit::flagCond(const std::string& eflags, std::uint32_t bit) {
    return bin("icmp ne", "i32", andv(eflags, bit), "0");
}

std::string FnEmit::parity(const std::string& v) {
    const std::string low = andv(v, 0xFFu);
    const std::string pc = tmp();
    add(pc + " = call i32 @llvm.ctpop.i32(i32 " + low + ")");
    stats_->used_ctpop = true;
    const std::string odd = andv(pc, 1u);
    return bin("icmp eq", "i32", odd, "0");
}

void FnEmit::fail(const std::string& why) {
    if (failed_) return;
    failed_ = true;
    char b[24];
    std::snprintf(b, sizeof(b), "0x%08X", cur_ea_);
    fail_why_ = why + " @ " + b;
}

std::uint8_t FnEmit::valueMask() const {
    std::uint8_t v = 0;
    for (int b = 0; b < 6; ++b)
        if (slot_[b] == Slot::Value) v = static_cast<std::uint8_t>(v | (1u << b));
    return v;
}

// 读一位：Value ⇒ 直接给 SSA；FromBase ⇒ 从 base 的位取（今天的 `flagCond`）；
// Poison ⇒ **生成期硬错**（"写了但没人读"与"有人读"同时成立 = 活性自相矛盾）。
std::string FnEmit::readBit(int b) {
    if (slot_[b] == Slot::Value) return slot_val_[b];
    if (slot_[b] == Slot::FromBase)
        return bin("icmp ne", "i32", andv(loadEflags(), kBitWeight[b]), "0");
    fail("读了 Poison 位（flags 活性自相矛盾）");
    return "false";
}

void FnEmit::deliverBit(int b, std::string v) {
    // ★ 2026-11-15：条件写门（见 `deliver_guard_` 的声明）。先读旧值 —— deliver 会覆盖槽。
    if (!deliver_guard_.empty()) v = select1(deliver_guard_, "i1", v, readBit(b));
    slot_[b] = Slot::Value;
    slot_val_[b] = std::move(v);
    ++stats_->flags_deliver[b];
}

// ★ 写者死位必须 kill：否则槽里留着的旧 `Value` 会在下一次 `materialize` 里**复活**（静默错）。
void FnEmit::killBit(int b) {
    slot_[b] = Slot::Poison;
    slot_val_[b].clear();
    ++stats_->flags_kill;
}

// 统一三段式的第一段（`plan.md` §1.6(d)）：W 里**不**在 mask 里的位 = 死位 ⇒ kill。
// S3-a/b 里 mask == W（全量物化）⇒ 这一段恒为空；S3-c 收紧掩码后它才起作用。
void FnEmit::beginFlagWrite(std::uint8_t w, std::uint8_t mask) {
    for (int b = 0; b < 6; ++b)
        if ((w & (1u << b)) != 0 && (mask & (1u << b)) == 0) killBit(b);
}

void FnEmit::resetFlagsSlots() {
    for (int b = 0; b < 6; ++b) {
        slot_[b] = Slot::FromBase;
        slot_val_[b].clear();
    }
}

// ★ 唯一出口：把 mask 里有 lazy 值的位并进 base 字（**结构性 overlay**），其余位原样来自 base。
//   `mask == 0` ⇒ 零 IR；mask 里的位都没有 lazy 值（全 FromBase）⇒ 也零 IR
//   （那正是"这些位已经在 base 里"的等价说法 —— S3-a/b 的块尾物化因此不发任何指令）。
//   ⛔ 不 flush：真正落盘由既有的 `flushRegs()` 负责（`materialize` 只改缓存 + 记脏）。
void FnEmit::materialize(std::uint8_t mask, bool allow_poison) {
    if (mask == 0) return;
    ++stats_->flags_materialize_calls;
    const std::uint8_t v = static_cast<std::uint8_t>(mask & valueMask());
    for (int b = 0; b < 6; ++b) {
        if ((mask & (1u << b)) != 0 && slot_[b] == Slot::Poison && !allow_poison) {
            fail("要向 base 物化一个 Poison 位（块尾/读者处 flags 活性自相矛盾）");
            return;
        }
    }
    if (v == 0) return;  // 这些位已经都在 base 里 ⇒ 零 IR
    ++stats_->flags_materialize_stores;
    std::uint32_t clear = 0;
    for (int b = 0; b < 6; ++b)
        if ((v & (1u << b)) != 0) clear |= kBitWeight[b];
    std::string word = andv(loadEflags(), ~clear);  // 只清"真的有 lazy 值"的位
    for (int b = 0; b < 6; ++b)
        if ((v & (1u << b)) != 0)
            word = bin("or", "i32", word, select1(slot_val_[b], "i32", i32c(kBitWeight[b]), "0"));
    storeEflags(word);
    for (int b = 0; b < 6; ++b)
        if ((mask & (1u << b)) != 0) {
            slot_[b] = Slot::FromBase;  // 值已经并进 base
            slot_val_[b].clear();
        }
}

// §1.7 的按位生产者：**每一条只算 mask 里的位**（mask==0 ⇒ 一条语句都不发）。
// 记号照 plan：`rm = maskVal(result,size)`、`sg = signOfSize(size)`。
void FnEmit::deliverLogicBits(std::uint8_t mask, const std::string& result, unsigned size) {
    if (mask == 0) return;  // 死写 = 零语句（S3-c）
    const std::string r = maskVal(result, size);
    if (mask & (1u << kBitZf)) deliverBit(kBitZf, bin("icmp eq", "i32", r, "0"));
    if (mask & (1u << kBitSf))
        deliverBit(kBitSf, bin("icmp ne", "i32", andv(r, signOfSize(size)), "0"));
    if (mask & (1u << kBitPf)) deliverBit(kBitPf, parity(r));
    // 逻辑运算：CF/OF/AF 解释器都写 0（`exec.cpp:40` 的 setLogicFlags 最后一行也写 AF）⇒ **常量 false**。
    if (mask & (1u << kBitCf)) deliverBit(kBitCf, "false");
    if (mask & (1u << kBitOf)) deliverBit(kBitOf, "false");
    if (mask & (1u << kBitAf)) deliverBit(kBitAf, "false");
}

void FnEmit::deliverAddBits(std::uint8_t mask, const std::string& a, const std::string& b,
                            const std::string& result, unsigned size, bool with_carry) {
    if (mask == 0) return;  // 死写 = 零语句（S3-c）
    const std::uint32_t smask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    // 操作数已经按宽度截断（见 readValue）⇒ maskVal 在 size==4 时是**零语句**（⛔ 不发 and -1）
    const std::string r = maskVal(result, size);
    const std::string av = maskVal(a, size);
    const std::string bv = maskVal(b, size);
    if (mask & (1u << kBitCf)) {
        if (!with_carry) {
            // CF = (a+b) 溢出宽度 ⇔ r < a（无符号回绕）
            deliverBit(kBitCf, bin("icmp ult", "i32", r, av));
        } else {
            // carry-in 只占 0.0003%（`adc`）：走 i64 的 full > mask 老路，与解释器逐位一致。
            const std::string a64 = tmp();
            add(a64 + " = zext i32 " + av + " to i64");
            const std::string b64 = tmp();
            add(b64 + " = zext i32 " + bv + " to i64");
            std::string full = bin("add", "i64", a64, b64);
            const std::string carry = select1(readBit(kBitCf), "i64", "1", "0");
            full = bin("add", "i64", full, carry);
            deliverBit(kBitCf, bin("icmp ugt", "i64", full, i64c(static_cast<std::uint64_t>(smask))));
        }
    }
    if (mask & (1u << kBitZf)) deliverBit(kBitZf, bin("icmp eq", "i32", r, "0"));
    if (mask & (1u << kBitSf))
        deliverBit(kBitSf, bin("icmp ne", "i32", andv(r, sign), "0"));
    if (mask & (1u << kBitPf)) deliverBit(kBitPf, parity(r));
    if (mask & (1u << kBitAf)) {
        const std::string ax = bin("xor", "i32", av, bv);
        deliverBit(kBitAf, bin("icmp ne", "i32", andv(bin("xor", "i32", ax, r), kAf), "0"));
    }
    if (mask & (1u << kBitOf)) {
        const std::string x1 = bin("xor", "i32", av, r);
        const std::string x2 = bin("xor", "i32", bv, r);
        deliverBit(kBitOf, bin("icmp ne", "i32", andv(bin("and", "i32", x1, x2), sign), "0"));
    }
}

// setSubFlags：CF = av < bv + borrow（无符号）；AF = (av^bv^r)&0x10；OF = (a^b)&(a^r) 的符号位。
void FnEmit::deliverSubBits(std::uint8_t mask, const std::string& a, const std::string& b,
                            const std::string& result, unsigned size, bool with_borrow) {
    if (mask == 0) return;  // 死写 = 零语句（S3-c）
    const std::uint32_t smask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    // 操作数已经按宽度截断（见 readValue）⇒ maskVal 在 size==4 时是**零语句**（⛔ 不发 and -1）
    const std::string av = maskVal(a, size);
    const std::string bv = maskVal(b, size);
    const std::string r = maskVal(result, size);
    if (mask & (1u << kBitCf)) {
        if (!with_borrow) {
            deliverBit(kBitCf, bin("icmp ult", "i32", av, bv));
        } else {
            const std::string av64 = tmp();
            add(av64 + " = zext i32 " + av + " to i64");
            const std::string bv64 = tmp();
            add(bv64 + " = zext i32 " + bv + " to i64");
            const std::string borrow = select1(readBit(kBitCf), "i64", "1", "0");
            const std::string bp = bin("add", "i64", bv64, borrow);
            deliverBit(kBitCf, bin("icmp ult", "i64", av64, bp));
        }
    }
    if (mask & (1u << kBitZf)) deliverBit(kBitZf, bin("icmp eq", "i32", r, "0"));
    if (mask & (1u << kBitSf))
        deliverBit(kBitSf, bin("icmp ne", "i32", andv(r, sign), "0"));
    if (mask & (1u << kBitPf)) deliverBit(kBitPf, parity(r));
    if (mask & (1u << kBitAf)) {
        const std::string x = bin("xor", "i32", bin("xor", "i32", av, bv), r);
        deliverBit(kBitAf, bin("icmp ne", "i32", andv(x, kAf), "0"));
    }
    if (mask & (1u << kBitOf)) {
        const std::string x1 = bin("xor", "i32", av, bv);
        const std::string x2 = bin("xor", "i32", av, r);
        deliverBit(kBitOf, bin("icmp ne", "i32", andv(bin("and", "i32", x1, x2), sign), "0"));
    }
}

// 不融合时的读法：按 §1.3 只读**它需要的位**（⛔ 不再无条件读五位）。
std::string FnEmit::condValue(Cond cc) {
    switch (cc) {
        case Cond::O: return readBit(kBitOf);
        case Cond::NO: return not1(readBit(kBitOf));
        case Cond::B: return readBit(kBitCf);
        case Cond::AE: return not1(readBit(kBitCf));
        case Cond::E: return readBit(kBitZf);
        case Cond::NE: return not1(readBit(kBitZf));
        case Cond::BE: return or1(readBit(kBitCf), readBit(kBitZf));
        case Cond::A: return not1(or1(readBit(kBitCf), readBit(kBitZf)));
        case Cond::S: return readBit(kBitSf);
        case Cond::NS: return not1(readBit(kBitSf));
        case Cond::P: return readBit(kBitPf);
        case Cond::NP: return not1(readBit(kBitPf));
        case Cond::L: return bin1("xor", readBit(kBitSf), readBit(kBitOf));
        case Cond::GE: return not1(bin1("xor", readBit(kBitSf), readBit(kBitOf)));
        case Cond::LE: return or1(readBit(kBitZf), bin1("xor", readBit(kBitSf), readBit(kBitOf)));
        case Cond::G:
            return and1(not1(readBit(kBitZf)),
                        not1(bin1("xor", readBit(kBitSf), readBit(kBitOf))));
        default: return "false";
    }
}

// 按 size 的符号扩展：size==4 ⇒ 零语句（A/B 已经是 i32）；否则 shl+ashr（隐含按 size 掩码）。
std::string FnEmit::signExt(const std::string& v, unsigned size) {
    if (size >= 4) return v;
    const unsigned sh = 32u - bitsOfSize(size);
    const std::string l = bin("shl", "i32", v, i32c(sh));
    return bin("ashr", "i32", l, i32c(sh));
}

// §2.2 的 16 种条件 → 直接 IR。返回**空串** = 不可融合（`o/no` 配 `cmp`、以及 `p/np`）。
// ★ 整张表由 §1.3 的 `condBits` 驱动（每个条件只允许出现在这张表里一次）。
std::string FnEmit::directCond(Op op, Cond cc, const std::string& a, const std::string& b,
                               const std::string& r, unsigned size) {
    const bool is_test = (op == Op::Test);
    switch (cc) {
        // `o/no`：`cmp` 的 OF 不是一条 icmp ⇒ 不融合；`test` 的 OF 恒 0（逻辑运算）⇒ 常量。
        case Cond::O: return is_test ? std::string("false") : std::string();
        case Cond::NO: return is_test ? std::string("true") : std::string();
        // `b/ae`：`test` 的 CF 恒 0 ⇒ 常量（"test; jae 恒真"这类惯用法在这里变零语句）。
        case Cond::B: return is_test ? std::string("false") : bin("icmp ult", "i32", a, b);
        case Cond::AE: return is_test ? std::string("true") : bin("icmp uge", "i32", a, b);
        case Cond::E: return is_test ? bin("icmp eq", "i32", r, "0") : bin("icmp eq", "i32", a, b);
        case Cond::NE: return is_test ? bin("icmp ne", "i32", r, "0") : bin("icmp ne", "i32", a, b);
        case Cond::BE: return is_test ? bin("icmp eq", "i32", r, "0") : bin("icmp ule", "i32", a, b);
        case Cond::A: return is_test ? bin("icmp ne", "i32", r, "0") : bin("icmp ugt", "i32", a, b);
        case Cond::S: return bin("icmp slt", "i32", signExt(r, size), "0");
        case Cond::NS: return bin("icmp sge", "i32", signExt(r, size), "0");
        case Cond::P: return std::string();  // PF 不是一条 icmp ⇒ 退化
        case Cond::NP: return std::string();
        case Cond::L:
            return is_test ? bin("icmp slt", "i32", signExt(r, size), "0")
                           : bin("icmp slt", "i32", signExt(a, size), signExt(b, size));
        case Cond::GE:
            return is_test ? bin("icmp sge", "i32", signExt(r, size), "0")
                           : bin("icmp sge", "i32", signExt(a, size), signExt(b, size));
        case Cond::LE:
            return is_test ? bin("icmp sle", "i32", signExt(r, size), "0")
                           : bin("icmp sle", "i32", signExt(a, size), signExt(b, size));
        case Cond::G:
            return is_test ? bin("icmp sgt", "i32", signExt(r, size), "0")
                           : bin("icmp sgt", "i32", signExt(a, size), signExt(b, size));
        default: return std::string();
    }
}

// S3-b：能融合就发一条（最多三条）直接 IR；否则退化成逐位惰性读（语义不变）。
// ★ S3-c：判断"能不能融合"的唯一来源是后向扫描给出的 `fuse_from[cur_index_]`（plan §2.1）；
//   这里只做**前后两遍一致性校验**：`fuse_from` 说能融 ⇒ 需要的每一位的"最近写者"必须就是它、
//   且那条 cmp/test 的当刻操作数在账上。对不上 ⇒ 生成期硬错（⛔ 不许静默退化成读一个 Poison 位）。
//   反方向（后向说不能融、前向其实能）**不**报错：那只是少省几条语句，语义仍然正确 —— 记在
//   `flags_fuse_missed` 读数里（应恒 0；非 0 说明两份判据漂了）。
std::string FnEmit::condForReader(Cond cc, int kind) {
    const std::uint8_t need = condBits(cc);
    const int idx = cur_index_;
    const int fused_from =
        (fb_ != nullptr && idx >= 0 && static_cast<std::size_t>(idx) < fb_->fuse_from.size())
            ? fb_->fuse_from[static_cast<std::size_t>(idx)]
            : -1;
    if (fused_from >= 0) {
        bool match = static_cast<std::size_t>(fused_from) < cmp_recs_.size() &&
                     cmp_recs_[static_cast<std::size_t>(fused_from)].valid &&
                     cmp_recs_[static_cast<std::size_t>(fused_from)].index == fused_from;
        for (int b = 0; match && b < 6; ++b) {
            if ((need & (1u << b)) == 0) continue;
            if (bit_owner_[b] != fused_from) match = false;
        }
        if (!match) {
            fail("融合账前后两遍不一致（fuse_from 与最近写者 / cmp 记录对不上）");
            return "false";
        }
        const CmpRec& rec = cmp_recs_[static_cast<std::size_t>(fused_from)];
        const std::string c = directCond(rec.op, cc, rec.a, rec.b, rec.r, rec.size);
        if (c.empty()) {
            fail("融合账前后两遍不一致（被融合的生产者却给不出直接 IR）");
            return "false";
        }
        ++stats_->flags_fused;
        if (kind == 0) ++stats_->flags_fused_jcc;
        else if (kind == 1) ++stats_->flags_fused_setcc;
        else ++stats_->flags_fused_cmovcc;
        return c;
    }
    // ---- 退化：分桶（读数，不是判据）----
    if ((need & (1u << kBitPf)) != 0) {
        ++stats_->flags_fallback_pf;
    } else {
        int owner = -2;  // -2 = 还没见到任何需要的位
        bool cross = false, multi = false;
        for (int b = 0; b < 6; ++b) {
            if ((need & (1u << b)) == 0) continue;
            const int o = bit_owner_[b];
            if (o < 0) { cross = true; break; }
            if (owner == -2) owner = o;
            else if (owner != o) { multi = true; break; }
        }
        if (cross) ++stats_->flags_fallback_cross_block;
        else if (multi) ++stats_->flags_fallback_multi;
        else {
            const bool owner_is_cmp =
                owner >= 0 && static_cast<std::size_t>(owner) < cmp_recs_.size() &&
                cmp_recs_[static_cast<std::size_t>(owner)].valid &&
                cmp_recs_[static_cast<std::size_t>(owner)].index == owner;
            if (owner_is_cmp) {
                // 是 cmp/test 却没融：只在 o/no（cmp）与 p/np 上该发生（后者已在上面的桶里）。
                ++stats_->flags_fallback_not_direct;
                ++stats_->flags_fuse_missed;
            } else {
                ++stats_->flags_fallback_other_op;
            }
        }
    }
    return condValue(cc);
}

// §1.2 的 W 表（**唯一一份**在文件上半部分：`flagsWriteMaskOf`）。这里只转发。
// ★ 只用于 **inline** 的 op；不 inline 的（`lift_one`）走 `liftOneFlagsRWOf`（c2-lite），⛔ 不再一律 W=ALL。
std::uint8_t FnEmit::flagsWriteMask(const Insn& in) const { return flagsWriteMaskOf(in); }

// ★ S3-c：这条写者该**真的算**哪些位。`mask ⊆ W`；`W \ mask` 的位是死位 ⇒ `beginFlagWrite`
//   把它们 `killBit`（⛔ 漏了它会留下旧 `Value`，下一次物化就把它复活 = 静默错）。
//   表格缺失（块没进活性表）⇒ 保守回退到 W（并且 `emitBlock` 已经硬错过 —— 不会走到这里）。
std::uint8_t FnEmit::wrMaskAt(int index) const {
    if (fb_ != nullptr && index >= 0 && static_cast<std::size_t>(index) < fb_->wr_mask.size())
        return fb_->wr_mask[static_cast<std::size_t>(index)];
    return kAllBits;
}

// ★ c2-lite：`lift_one` 宿主调用前要先把哪些位并进 base。表缺失（不该发生）⇒ 保守回退到 ALL，
//   让 `materialize` 的 Poison 自检把生成期不一致**响亮**打出来（⛔ 不静默少物化）。
std::uint8_t FnEmit::matMaskAt(int index) const {
    if (fb_ != nullptr && index >= 0 && static_cast<std::size_t>(index) < fb_->mat_mask.size())
        return fb_->mat_mask[static_cast<std::size_t>(index)];
    return kAllBits;
}


// S3-b / S3-c：发射后的账。`write_mask` = 这条指令**逻辑上的 W**（不按活性收窄）——
// 融合判据（`fuse_from`）与"最近写者"两遍必须用同一份判据；收窄只影响"算/算不算"。
// `lift_one` 站点传 `liftOneFlagsRWOf(in).write`（c2-lite：`mov`/`div` 这类 W=∅ 的**不覆盖**任何位，
// 于是它前面的 cmp 仍然可以把条件融合给后面的 jcc；⛔ 旧实现一律 ALL 会把这条融合掐死）。
// `cmp_recs_[index]` 记下 cmp/test **当刻**的 SSA 操作数（生产者的值），读者按 `fuse_from` 取用。
void FnEmit::recordFlagsWrites(const Insn& in, int index, std::uint8_t write_mask) {
    for (int b = 0; b < 6; ++b)
        if ((write_mask & (1u << b)) != 0) bit_owner_[b] = index;
    if (in.op == Op::Cmp || in.op == Op::Test) {
        CmpRec r;
        r.valid = true;
        r.index = index;
        r.op = in.op;
        r.a = cmp_ops_.a;
        r.b = cmp_ops_.b;
        r.r = cmp_ops_.r;
        r.size = cmp_ops_.size;
        if (index >= 0 && static_cast<std::size_t>(index) < cmp_recs_.size())
            cmp_recs_[static_cast<std::size_t>(index)] = std::move(r);
    }
}


// ---------------------------------------------------------------------------------------
// 控制流
// ---------------------------------------------------------------------------------------
std::string FnEmit::labelFor(std::uint32_t ea) {
    // 入口 EA / 被丢弃的派生 leader（没有块）/ **宿主语义钩子 EA** ⇒ 统一的 `bb_exit_<EA>` 出口块
    //   （`ret i32 <EA>`）。最后那一类见 `aot/aot_hook_ea.h`：S7 之后控制流不再逐函数回派发器，
    //   钩子 EA 若不强制过一次派发就再也见不到 trampoline ⇒ 钩子静默失效。
    if (entries_.count(ea) != 0 || dropped_.count(ea) != 0 || mustDispatchAt(ea)) {
        exit_ea_.insert(ea);
        return "%bb_exit_" + hexEa(ea);
    }
    return "%bb_" + hexEa(ea);
}

void FnEmit::emitEdge(std::uint32_t ea) {
    // ★ 块出口（E2 / S2）：`ret`/`br` 之后 `%st` 就是下一个执行者的输入 ⇒ 先写回缓存。
    flushRegs();
    if (entries_.count(ea) != 0 || dropped_.count(ea) != 0 || mustDispatchAt(ea)) {
        // 目标是入口 EA ⇒ 交回 trampoline（状态已经在 %st 里）。
        // 目标是被丢弃的派生 leader ⇒ 同样交回 trampoline（它没有块；解释器会自己解那段字节）。
        // 目标是宿主钩子 EA ⇒ 必须经过一次派发（trampoline 在那里跑钩子）。
        add("ret i32 " + i32c(ea));
        return;
    }
    add("br label %bb_" + hexEa(ea));
}

std::uint32_t FnEmit::ownerOf(std::uint32_t ea) const {
    const auto it = owner_.find(ea);
    return it == owner_.end() ? 0u : it->second;
}

// 宿主 `call` / `lift_dyn` 之后的 continuation（D53 的唯一判据，见类内声明的长注释）。
void FnEmit::emitCallReturnCheck(const std::string& r, std::uint32_t resume) {
    // 被调方在退出前把 GPR / EFLAGS 写回了 `%st`（`ret` 出口是 flags 的 escape 块 ⇒ ALL 位物化）
    // ⇒ 本块的寄存器缓存整份作废；flags 槽回到 FromBase（下一块按自己的 need_in 取）。
    invalidateRegs();
    for (int b = 0; b < 6; ++b) {
        slot_[b] = Slot::FromBase;
        slot_val_[b].clear();
    }
    const std::string ok = tmp();
    add(ok + " = icmp eq i32 " + r + ", " + i32c(resume));
    const std::string id = std::to_string(label_id_++);
    add("br i1 " + ok + ", label %s7_ok_" + id + ", label %s7_esc_" + id);
    put("s7_esc_" + id + ":\n");
    add("ret i32 " + r);  // 目标把控制权交给了别处（`lift_dyn` 尾调用 / 派发出口 / 停机）⇒ 上抛
    put("s7_ok_" + id + ":\n");
    if (comp_.blocks.count(resume) != 0 && !mustDispatchAt(resume)) {
        // resume 块就在**本单元**里 ⇒ 直接进块（今天的 trampoline 会派发到同一个块）。
        add("br label %bb_" + hexEa(resume));
    } else {
        // resume EA 在别的单元 / 或被丢弃 / 是钩子点 ⇒ 交回派发器（一次派发；语义与今天相同）。
        add("ret i32 " + i32c(resume));
    }
}

void FnEmit::emitTailCallTo(std::uint32_t owner_root, std::uint32_t target) {
    // ★ `tail` 关键字是**要求**而不是提示：`ret i32 %r` 紧跟其后 ⇒ LLVM 必须把它落成 jmp
    //   （否则宿主栈会随跨函数 `jmp` 增长）。真正的保证由构建期 objdump 守卫复核。
    const std::string r = tmp();
    add(r + " = tail call i32 @" + genFunctionName(owner_root) +
        "(ptr %st, ptr %mem, i32 " + i32c(target) + ", ptr %cpu)");
    add("ret i32 " + r);
    ++stats_->s7_tail_calls;
}

void FnEmit::emitCall(const Insn& in) {
    const std::uint32_t ret = in.ea + in.length;
    const Operand& op = in.op0();
    // ★ 先读目标（`call [esp+x]` 依赖压栈前的 ESP；与 exec_control.cpp 同序），再压栈。
    std::string dyn;
    if (op.kind != OpKind::Imm) dyn = readValue(op);
    pushValue(i32c(ret));
    flushRegs();  // 块出口（E2/S2）：`pushValue` 刚改过 ESP ⇒ 必须在宿主调用 / `ret` 之前写回 `%st`
    // ---- 间接 `call reg/mem`：查派发表 + 尾调用进目标（⛔ 不回宿主主循环）----
    if (op.kind != OpKind::Imm) {
        if (!structure_.enabled) {
            // `--no-s7`：回到 D55 之前的形状（目标 EA 交回 trampoline）
            add("ret i32 " + dyn);
            ++stats_->s7_call_dispatch;
            return;
        }
        const std::string r = tmp();
        add(r + " = tail call i32 @lift_dyn(ptr %cpu, ptr %st, i32 " + dyn + ")");
        stats_->used_lift_dyn = true;
        ++stats_->s7_call_lift_dyn;
        ++stats_->s7_tail_calls;
        // `lift_dyn` 的 `return fn(st, mem, ea, cpu)` 是尾调用 ⇒ 目标的宿主返回地址**就是我们**
        // ⇒ 目标正常 `ret` 时 `%r` 就是 resume EA（判据与宿主 call 完全相同）。
        emitCallReturnCheck(r, ret);
        return;
    }
    const std::uint32_t target = genBranchTarget(in);
    // ---- 静态目标 ----
    // 宿主 `call` 的条件（D53 + 传递闭包）：caller 可结构化、目标不是 shim 桩、不是钩子点、
    // 且目标的单元根也可结构化。⚠ 这些条件对"结构化 caller"是**由判据保证**的（`analyzeStructure`
    // 已经要求所有静态 callee 都可结构化）；写成显式判断是为了让发射器的形状**自证**。
    const std::uint32_t owner = ownerOf(target);
    if (selfStructured() && !inShimPage(target) && !mustDispatchAt(target) && owner != 0 &&
        structuredRoot(owner)) {
        const std::string r = tmp();
        add(r + " = call i32 @" + genFunctionName(owner) + "(ptr %st, ptr %mem, i32 " +
            i32c(target) + ", ptr %cpu)");
        ++stats_->s7_call_host;
        emitCallReturnCheck(r, ret);
        return;
    }
    // ---- 其余：保持今天的语义（`ret i32 <目标>` 交回 trampoline）----
    if (inShimPage(target)) ++stats_->s7_call_shim;
    else ++stats_->s7_call_dispatch;
    add("ret i32 " + i32c(target));
}

void FnEmit::emitJcc(const Insn& in) {
    const std::uint32_t target = genBranchTarget(in);
    const std::uint32_t fall = in.ea + in.length;
    if (target == fall) {
        // 退化 jcc（§2.4 情形 8）：不读 flags，走既有的 emitEdge 分支。
        emitEdge(target);
        return;
    }
    // ★ S3-b：能融合 ⇒ 一条 icmp（不产生任何 flag 位）；否则逐位惰性读。
    const std::string cond = condForReader(in.cc, /*kind=*/0);
    flushRegs();  // 块出口（E2/S2）：两个后继块都以 `%st` 为输入
    add("br i1 " + cond + ", label " + labelFor(target) + ", label " + labelFor(fall));
}

void FnEmit::emitRet(const Insn& in) {
    // ★ S7：形状**一个字都没改** —— `ret i32 <弹出的 guest EA>` 在 LLVM 里就是**宿主 `ret`**。
    //   区别只在"谁调用了这个函数"：可结构化单元用宿主 `call` 进来（caller 的 continuation 由
    //   `emitCallReturnCheck` 按 `%r == resume` 判），非结构化单元仍由 trampoline 派发进来。
    //   ⇒ "宿主 `ret` 与 guest 栈上那个返回 EA 一一对应"这件事由 call 站点的判据**当场核对**，
    //   ⛔ 不靠静态假设（任何对不上都退化成"原样上抛给派发器"，语义等价）。
    if (selfStructured()) ++stats_->s7_ret_host;
    else ++stats_->s7_ret_dispatch;
    const std::string ret = popValue();
    if (in.op0().kind == OpKind::Imm) {
        const std::uint32_t imm = static_cast<std::uint32_t>(in.op0().imm) & 0xFFFFu;
        if (imm != 0) storeReg(4, bin("add", "i32", loadReg(4), i32c(imm)));
    }
    flushRegs();  // 块出口（E2/S2）：弹出的返回地址 + `esp += 4 [+ imm]` 都要先写回 `%st`
    add("ret i32 " + ret);
}

void FnEmit::emitTerminator(const Insn& in) {
    switch (in.op) {
        case Op::Call: emitCall(in); break;
        case Op::Jcc: emitJcc(in); break;
        case Op::Ret: emitRet(in); break;
        case Op::Jmp: {
            const Operand& op = in.op0();
            if (op.kind != OpKind::Imm) {
                // 间接 `jmp`：目标运行期才知道 ⇒ `lift_dyn` 尾调用（⛔ 不回宿主主循环）。
                const std::string target = readValue(op);
                if (!structure_.enabled) {
                    // `--no-s7`：回到 D55 之前的形状（目标 EA 交回 trampoline）
                    add("ret i32 " + target);
                    ++stats_->s7_jmp_dispatch;
                    break;
                }
                const std::string r = tmp();
                add(r + " = tail call i32 @lift_dyn(ptr %cpu, ptr %st, i32 " + target + ")");
                add("ret i32 " + r);
                stats_->used_lift_dyn = true;
                ++stats_->s7_jmp_lift_dyn;
                ++stats_->s7_tail_calls;
                break;
            }
            const std::uint32_t target = genBranchTarget(in);
            if (comp_.blocks.count(target) != 0 && !mustDispatchAt(target)) {
                // 单元内 ⇒ `br`（今天的形状；块入口标签唯一）
                flushRegs();
                add("br label %bb_" + hexEa(target));
                break;
            }
            if (mustDispatchAt(target)) {
                // 钩子 EA：必须经过一次派发（trampoline 在那里跑钩子后再派发回来）
                flushRegs();
                add("ret i32 " + i32c(target));
                ++stats_->s7_jmp_dispatch;
                break;
            }
            const std::uint32_t owner = ownerOf(target);
            if (selfStructured() && owner != 0 && structuredRoot(owner)) {
                // 跨函数 `jmp` ⇒ **尾巴调用**（不新增 guest 栈帧，也不多一次派发）
                emitTailCallTo(owner, target);
                ++stats_->s7_jmp_tail;
                break;
            }
            // 目标运行期才知道 / 不可结构化 ⇒ `ret i32 <目标>` 也是块出口（E2/S2：先写回缓存）。
            flushRegs();
            add("ret i32 " + i32c(target));
            ++stats_->s7_jmp_dispatch;
            break;
        }
        default: emitTrap(in, "控制流指令本轮不支持");
    }
}

void FnEmit::emitTrap(const Insn& in, const char* reason) {
    add("; unsupported @ " + hexEa(in.ea) + " (" + (in.mnemonic ? in.mnemonic : "?") + ") — " + reason);
    add("call void @llvm.trap()");
    add("unreachable");
    ++stats_->traps_unsupported;
    unsupported_->push_back(GenUnsupported{in.ea, opName(in.op), reason});
    trapped_ = true;  // ⛔ 之后不许再发指令（P5：`unreachable` 后面跟指令 = 非法 IR）
}

// ---------------------------------------------------------------------------------------
// 指令计数（D2）与单指令宿主原语（T1c）
// ---------------------------------------------------------------------------------------
void FnEmit::flushTick() {
    if (pending_ticks_ <= 0) return;
    add("call void @lift_tick(ptr %cpu, i32 " + std::to_string(pending_ticks_) + ")");
    stats_->used_tick = true;
    pending_ticks_ = 0;
}

// 一条**不 inline** 的指令 ⇒ 一次 `lift_one`，成功继续、失败回 trampoline 报停机。
//   形状（父级裁决）：
//     %ok = call i1 @lift_one(ptr %cpu, ptr %st, i32 <EA>)
//     br i1 %ok, label %lo_ok_N, label %lo_halt_N
//   lo_ok_N:   …后续指令…
//   lo_halt_N: ret i32 kLiftHalt      ← 收集到函数尾（见 run()）
void FnEmit::emitLiftOne(const Insn& in, bool terminating) {
    // ★ 宿主调用前 flush（D2 + E2/S2）：`lift_one` 执行之后才算这一条，所以先只结算之前的。
    //   寄存器缓存必须**先写回 `%st`**：`lift_one` 第一步就是 `liftRegsToCpu(*st, cpu)`（读 `%st`）。
    // ★ c2-lite：不再是"整字屏障" —— 调用前只把 `mat_mask`（= 解释器真读的位 ∪ 条件写可能穿过的
    //   活位）并进 base。⛔ 不能照抄 `materialize(kAllBits)`：收窄后 `W` 之外的位可能是 Poison
    //   （死写），那会撞 materialize 的自检；W=∅ 的 op 也不该为不读的位付物化。
    //   ⚠ 顺序：materialize 在 `syncBeforeHostCall()`（= flushTick + flushRegs）**之前**。
    const FlagsRW rw = flagsRWOf(in);
    materialize(matMaskAt(cur_index_));
    syncBeforeHostCall();
    const std::string id = std::to_string(label_id_++);
    const std::string ok_label = "lo_ok_" + id;
    const std::string halt_label = "lo_halt_" + id;
    add("; lift_one @ " + hexEa(in.ea) + " (" + (in.mnemonic ? in.mnemonic : "?") + " / " + opName(in.op) +
        ") — 单指令宿主原语：语义在解释器里，⛔ 这里不编");
    const std::string ok = tmp();
    add(ok + " = call i1 @lift_one(ptr %cpu, ptr %st, i32 " + i32c(in.ea) + ")");
    stats_->used_lift_one = true;
    ++stats_->lift_one_sites;
    ++stats_->lift_one_by_op[opName(in.op)];
    // ★ E2 / S2：`lift_one` 的第三步是**整块回读** `liftCpuToRegs(cpu, *st)`（`lift_abi.cpp:139`，
    //   `lift_abi.h:130` 明写"不按'这条只写 eax'精简"）⇒ 缓存里的值全部作废，必须重读 `%st`。
    invalidateRegs();
    // ★ c2-lite：回读之后，只有解释器**写过**的位（`rw.write`）是 `%st` 里的权威值 ⇒ 槽置 FromBase。
    //   ⛔ 不能整份 reset：`mov`/`div` 这类 W=∅ 的 op 之后，块内还没物化的 lazy 值必须**原样保留**
    //   （S3-c 的 div case 就是"`xor` 的值穿过 div"：整份 reset 会把它丢掉 ⇒ 出口六位静默变旧值）。
    for (int b = 0; b < 6; ++b) {
        if ((rw.write & (1u << b)) != 0) {
            slot_[b] = Slot::FromBase;  // 解释器写过的位：%st 回读即权威
            slot_val_[b].clear();
        }
    }
    add("br i1 " + ok + ", label %" + ok_label + ", label %" + halt_label);
    put(ok_label + ":\n");
    pending_ticks_ = 1;  // 这一条已经执行（成功路径）；失败路径直接停机 ⇒ 不计数
    halt_labels_.push_back(HaltSite{halt_label, in.ea, 0xFFFFFFFFu});  // `lift_one` 的失败路径：只发 ret
    if (terminating) {
        // `Hlt/Int3/Int` 在解释器里**恒**返回 false（`exec_control.cpp:62-72`：置停 / trapInt）
        // ⇒ 成功路径不可达。真到这里说明解释器口径变了 ⇒ 响亮 trap（⛔ 不静默落空）。
        add("; ⚠ 停机类指令（hlt/int3/int）lift_one 返回 true = 解释器口径变了（生成器内部不一致）");
        add("call void @llvm.trap()");
        add("unreachable");
        trapped_ = true;
    }
}

// ---------------------------------------------------------------------------------------
// 本轮不支持的判定（父级裁定 + 本单的 (L) 缺口，都是**响亮**路）
// ---------------------------------------------------------------------------------------
// 这条指令能不能 inline（**唯一一份**在文件上半部分：`unsupportedReasonOf`）。这里只转发。
const char* FnEmit::unsupportedReason(const Insn& in) const { return unsupportedReasonOf(in); }

// ---------------------------------------------------------------------------------------
// 单条指令
// ---------------------------------------------------------------------------------------
void FnEmit::emitAlu(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::string a = readValue(in.op0());
    const std::string b = readValue(in.op1());
    // ★ S3-a 的统一三段式：`w` = 这条写哪些位、`mask` = 其中**要真的算出来**的位（S3-c：按活性收窄）。
    //   `w \ mask` 的位是死位 ⇒ `beginFlagWrite` 把它们 kill（⛔ 不许留旧 Value）。
    //   `mask == 0` ⇒ 生产者一条语句都不发（"死写 = 0 语句"就是这里）。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t mask = wrMaskAt(cur_index_);
    switch (in.op) {
        case Op::Add: {
            const std::string r = bin("add", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverAddBits(mask, a, b, r, size, /*with_carry=*/false);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Adc: {
            // CF 是**输入**（先读后写）：必须在 beginFlagWrite 之前读它。
            const std::string cin = select1(readBit(kBitCf), "i32", "1", "0");
            const std::string r = bin("add", "i32", bin("add", "i32", a, b), cin);
            beginFlagWrite(w, mask);
            deliverAddBits(mask, a, b, r, size, /*with_carry=*/true);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Sub: {
            const std::string r = bin("sub", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverSubBits(mask, a, b, r, size, /*with_borrow=*/false);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Sbb: {
            const std::string borrow = select1(readBit(kBitCf), "i32", "1", "0");
            const std::string r = bin("sub", "i32", bin("sub", "i32", a, b), borrow);
            beginFlagWrite(w, mask);
            deliverSubBits(mask, a, b, r, size, /*with_borrow=*/true);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Cmp: {
            const std::string r = bin("sub", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverSubBits(mask, a, b, r, size, /*with_borrow=*/false);
            // ★ S3-b：记下**当刻**的操作数（后续写寄存器不影响它；任何六位写者会作废这条记录）。
            cmp_ops_ = CmpOps{in.op, a, b, r, size};
            return;
        }
        case Op::Test: {
            const std::string r = bin("and", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverLogicBits(mask, r, size);
            cmp_ops_ = CmpOps{in.op, a, b, r, size};
            return;
        }
        case Op::And: {
            const std::string r = bin("and", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverLogicBits(mask, r, size);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Or: {
            const std::string r = bin("or", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverLogicBits(mask, r, size);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Xor: {
            const std::string r = bin("xor", "i32", a, b);
            beginFlagWrite(w, mask);
            deliverLogicBits(mask, r, size);
            writeDest(in.op0(), r);
            return;
        }
        default: emitTrap(in, "算逻族未实现的 op");
    }
}

void FnEmit::emitIncDec(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::string a = readValue(in.op0());
    const std::string r = in.op == Op::Inc ? bin("add", "i32", a, "1") : bin("sub", "i32", a, "1");
    // ★ 第 9 行：`inc/dec` **不写 CF**（`exec_integer.cpp:502-508` 的 saved_cf）。
    //   ⇒ CF 既不出现在 W 里、也不 kill：它必须保持原来的 FromBase/Value（这里 = base）。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t mask = wrMaskAt(cur_index_);
    beginFlagWrite(w, mask);
    if (in.op == Op::Inc) deliverAddBits(mask, a, "1", r, size, /*with_carry=*/false);
    else deliverSubBits(mask, a, "1", r, size, /*with_borrow=*/false);
    writeDest(in.op0(), r);
}

// 移位：**立即数计数**（编译期展开，本函数）与**寄存器计数**（运行期，`emitShiftReg`）两条路。
// 语义逐行照 `doShift`（`exec_integer.cpp:65-155`）。
void FnEmit::emitShift(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    if (in.op1().kind != OpKind::Imm) {
        emitShiftReg(in);
        return;
    }
    const std::uint8_t c = static_cast<std::uint8_t>(static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
    if (c == 0) return;  // 计数 0：什么都不改（含 flags）
    const std::string a = readValue(in.op0());
    // ★ S3-c：按活性只算**要交付**的位（`fmask ⊆ W`）；`W \ fmask` 的位是死位 ⇒ `beginFlagWrite` kill。
    //   `of` 的公式里有两条要用 `cf`（Shl / Rol）⇒ 那种情形即使不交付 CF 也要把它的值算出来。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t fmask = wrMaskAt(cur_index_);
    const bool need_cf = (fmask & (1u << kBitCf)) != 0;
    const bool need_of = (fmask & (1u << kBitOf)) != 0;

    std::string r;
    std::string cf;      // 空 = 常量 false
    std::string of;      // 空 = **保留** OF（Shr c>1）；非空 = 替换为这个 i1
    switch (in.op) {
        case Op::Shl:
            if (c < bits) {
                r = andv(bin("shl", "i32", a, i32c(c)), mask);
                if (need_cf || need_of)
                    cf = bin("icmp ne", "i32",
                             andv(bin("lshr", "i32", a, i32c(bits - c)), 1u), "0");
            } else {
                r = "0";
                if (c == bits && (need_cf || need_of))
                    cf = bin("icmp ne", "i32", andv(a, 1u), "0");
            }
            // OF = MSB(result) != CF（`doShift:85` 无条件替换）
            if (need_of) {
                const std::string msb = bin("icmp ne", "i32", andv(r, sign), "0");
                if (cf.empty()) of = msb;
                else of = bin1("xor", msb, cf);
            }
            break;
        case Op::Shr:
            if (c < bits) {
                r = andv(bin("lshr", "i32", a, i32c(c)), mask);
                if (need_cf)
                    cf = bin("icmp ne", "i32",
                             andv(bin("lshr", "i32", a, i32c(c - 1)), 1u), "0");
            } else {
                r = "0";
                if (c == bits && need_cf) cf = bin("icmp ne", "i32", andv(a, sign), "0");
            }
            // ★ `shr imm c>1`：OF 是**读-改-写**（`:95`）⇒ 不在 W 里，`of` 留空、不交付
            //   ⇒ 物化不会碰它，base 的旧 OF 原样保留。
            if (c == 1 && need_of) of = bin("icmp ne", "i32", andv(a, sign), "0");
            break;
        case Op::Sar: {
            std::string sa = a;
            if (size != 4) {
                const std::string shifted = bin("shl", "i32", a, i32c(32u - bits));
                sa = bin("ashr", "i32", shifted, i32c(32u - bits));
            }
            const unsigned sh = std::min<unsigned>(c, bits);
            if (need_cf)
                cf = bin("icmp ne", "i32", andv(bin("ashr", "i32", sa, i32c(sh - 1)), 1u), "0");
            r = andv(bin("ashr", "i32", sa, i32c(sh)), mask);
            if (need_of) of = "false";  // doShift 对 Sar 无条件 of = false
            break;
        }
        case Op::Rol: {
            // `exec_integer.cpp:105-111`：k = c % bits；r = (a<<k)|(a>>((bits-k)%bits))；
            //   CF = r&1；OF = MSB(r) != CF（**无条件**替换 OF）。
            const std::uint8_t k = static_cast<std::uint8_t>(c % bits);
            const std::uint8_t l = static_cast<std::uint8_t>((bits - k) % bits);
            r = andv(bin("or", "i32", bin("shl", "i32", a, i32c(k)),
                        bin("lshr", "i32", a, i32c(l))),
                     mask);
            if (need_cf || need_of) cf = bin("icmp ne", "i32", andv(r, 1u), "0");
            if (need_of) of = bin1("xor", bin("icmp ne", "i32", andv(r, sign), "0"), cf);
            break;
        }
        case Op::Ror: {
            // `exec_integer.cpp:112-118`：CF = MSB(r)；OF 是**读-改-写**（只在 c==1 时替换，
            //   `:116`）—— c!=1 时 W 里没有 OF（`flagsWriteMaskOf` 已收窄），这里也不交付。
            const std::uint8_t k = static_cast<std::uint8_t>(c % bits);
            const std::uint8_t l = static_cast<std::uint8_t>((bits - k) % bits);
            r = andv(bin("or", "i32", bin("lshr", "i32", a, i32c(k)),
                        bin("shl", "i32", a, i32c(l))),
                     mask);
            if (need_cf || need_of) cf = bin("icmp ne", "i32", andv(r, sign), "0");
            if (need_of && c == 1)
                of = bin1("xor", bin("icmp ne", "i32", andv(a, sign), "0"), cf);
            break;
        }
        case Op::Rcr: {
            // `exec_integer.cpp:131-142`：k = c % (bits+1)；把 (CF,a) 当成 bits+1 位的数右旋 k 位。
            const unsigned span = bits + 1;
            const std::uint8_t k = static_cast<std::uint8_t>(c % span);
            const std::string cfw = select1(readBit(kBitCf), "i64", i64c(1ull << bits), "0");
            const std::string a64 = tmp();
            add(a64 + " = zext i32 " + a + " to i64");
            const std::string wv = bin("or", "i64", cfw, a64);
            const std::string r64 = bin("or", "i64", bin("lshr", "i64", wv, i64c(k)),
                                        bin("shl", "i64", wv, i64c(span - k)));
            const std::string rsp = bin("and", "i64", r64, i64c((1ull << span) - 1ull));
            const std::string r32 = tmp();
            add(r32 + " = trunc i64 " + rsp + " to i32");
            r = andv(r32, mask);
            const std::string cfb = tmp();
            add(cfb + " = lshr i64 " + rsp + ", " + std::to_string(bits));
            cf = bin("icmp ne", "i64", bin("and", "i64", cfb, "1"), "0");
            if (need_of && c == 1)
                of = bin1("xor", bin("icmp ne", "i32", andv(a, sign), "0"),
                          bin("icmp ne", "i32", andv(r, sign), "0"));
            break;
        }
        default: emitTrap(in, "移位族未实现的 op"); return;
    }

    // ---- 写者（S3-a 三段式）：只交付 `fmask` 里那几位；`shr imm c>1` 的 OF 不写 ⇒ 保持 base 的旧值 ----
    beginFlagWrite(w, fmask);
    if ((fmask & (1u << kBitZf)) != 0) deliverBit(kBitZf, bin("icmp eq", "i32", r, "0"));
    if ((fmask & (1u << kBitSf)) != 0)
        deliverBit(kBitSf, bin("icmp ne", "i32", andv(r, sign), "0"));
    if ((fmask & (1u << kBitPf)) != 0) deliverBit(kBitPf, parity(r));
    if ((fmask & (1u << kBitCf)) != 0) deliverBit(kBitCf, cf.empty() ? std::string("false") : cf);
    if ((fmask & (1u << kBitOf)) != 0) deliverBit(kBitOf, of);  // 常量 `false`（sar）⇒ 零语句
    writeDest(in.op0(), r);
}

// ---------------------------------------------------------------------------------------
// 寄存器计数移位 / 循环移位（`shl/shr/sar/rol/ror r/m, CL`）
// ---------------------------------------------------------------------------------------
// ★ 语义真源 = 解释器 `doShift`（`src/runtime/exec_integer.cpp:65-155`），⛔ 不是 SDM：
//   · 计数 = `readReg(op1) & 0xFF` 再 `& 0x1F`（`shiftCount`，`:55-63`）—— 8/16/32 位操作数**一律 5 位**，
//     所以"计数 > 31"永远是**先掩码**再看（8/16 位操作数也可能掩码后 ≥ 宽度）；
//   · 计数 == 0 ⇒ 整条指令**什么都不改**（`:71` 的早退）：既不写 EFLAGS、也不写目的操作数；
//   · 只有 `shl/shr/sar` 写 ZF/SF/PF（`:148-152`）；`rol/ror` **只**写 CF/OF；AF 五个 op 都不写；
//   · `shl`/`rol` **无条件**替换 OF（`:85`/`:109`）；`sar` 无条件 `of = false`（`:102`）；
//     `shr`/`ror` **只在计数 == 1** 时替换 OF，计数 > 1 时 OF 原样保留（`:95`/`:116`）；
//   · 计数 ≥ 宽度时的 CF：`shl` 只有 c == bits 才是原 LSB、`shr` 只有 c == bits 才是原 MSB，其余为 0
//     （`:78-94`）；`sar` 的 CF 取"最后移出的那一位"（`:100`，宽度被 `min(c,bits)` 夹住）。
// ★ 为什么**无分支**（IR 里没有 `br`）：
//   · 计数是运行期值 ⇒ 用 `select` 表达"计数 0 / 计数 ≥ 宽度 / 计数 == 1"这三种运行期条件；
//     于是本函数**不必**拆基本块 ⇒ 不碰 `trapped_` / `label_id_` / `halt_labels_` 那套记账。
//   · ⚠ LLVM 里 `i32` 的位移量 ≥ 32 或 < 0 是 **poison**：所有运行期位移量进 `shl/lshr/ashr` **之前**
//     一律 `& 31`（`(bits-c)`、`(c-1)`、`(sh-1)`、`(bits-k)` 在各自条件不成立时本来是负数或正好 == 32）
//     ⇒ 生成物里连 poison 都不存在，不依赖"select 只传播被选中那一支"这条规则。
//   · 计数 == 0 时五个 op 都满足 `r == a`（恒等式：`a<<0`、`a>>0`、`asr(a,0)&mask`、
//     `rol/ror` 的 k==0 ⇒ `a|a`）⇒ 目的操作数**不必**再 select 一次（写回同值 ⇒ 观察不到），
//     只有 EFLAGS 需要 `select(计数 != 0, 新值, 旧值)`。
//   · ⛔ 目的操作数是**内存**时这里根本不会被调用（`unsupportedReason` 让它走 `lift_one`）：
//     计数 0 时解释器不写内存 ⇒ 生成器不能无条件发 `lift_note_write`。
void FnEmit::emitShiftReg(const Insn& in) {
    const bool is_rot = (in.op == Op::Rol || in.op == Op::Ror);
    if (in.op1().kind != OpKind::Reg) {
        emitTrap(in, "移位计数既不是立即数也不是寄存器");
        return;
    }
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);

    const std::string a = readValue(in.op0());
    // 照 `shiftCount`：`readReg(op1) & 0xFF` 再 `& 0x1F`。`readValue` 已按操作数宽度截断
    // （CL 是 8 位 ⇒ 已经是 `& 0xFF`）⇒ 这里只补 `& 0x1F`。
    const std::string c = andv(readValue(in.op1()), 0x1Fu);
    const std::string c_nz = bin("icmp ne", "i32", c, "0");
    // ★ S3-c：按活性收窄（`fmask ⊆ W`）。计数 0 的"整条不写"仍然用 `select(c_nz, 新, 旧)` 表达，
    //   但**死位**根本不该算：`W \ fmask` 由 `beginFlagWrite` kill，连旧值都不读（§2.5 的口径）。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t fmask = wrMaskAt(cur_index_);
    const bool need_cf = (fmask & (1u << kBitCf)) != 0;
    const bool need_of = (fmask & (1u << kBitOf)) != 0;
    const std::string c_lt = bin("icmp ult", "i32", c, i32c(bits));   // c < bits
    const std::string c_eq_bits = bin("icmp eq", "i32", c, i32c(bits));
    std::string msb_a;  // 惰性（只有 Shr/Ror 的 OF 用它）
    auto msbOfA = [&]() -> const std::string& {
        if (msb_a.empty()) msb_a = bin("icmp ne", "i32", andv(a, sign), "0");
        return msb_a;
    };

    std::string r;    // 结果（已按宽度截断）
    std::string cf;   // i1；空 = 常量 false
    std::string of;   // i1；空 = 常量 false（`sar`）；`shr`/`ror` 里 = **c == 1 时的新值**
                      //   （c == 0 / c > 1 保留旧值 —— 由组装段的 `select(c_eq_one, 新, 旧)` 表达）
    switch (in.op) {
        case Op::Shl: {
            // 计数 < 宽度：r = a << c、CF = a 的第 (bits-c) 位；否则 r = 0、CF 只在 c == bits 时是 a&1
            r = select1(c_lt, "i32", andv(bin("shl", "i32", a, c), mask), "0");
            if (need_cf || need_of) {  // `of` 的公式要用 cf 的值
                const std::string sh = andv(bin("sub", "i32", i32c(bits), c), 31u);  // (bits - c) & 31
                const std::string cf_lt =
                    bin("icmp ne", "i32", andv(bin("lshr", "i32", a, sh), 1u), "0");
                const std::string cf_ge =
                    select1(c_eq_bits, "i1", bin("icmp ne", "i32", andv(a, 1u), "0"), "false");
                cf = select1(c_lt, "i1", cf_lt, cf_ge);
            }
            // OF = MSB(r) != CF（`:88` 无条件替换）
            if (need_of) of = bin1("xor", bin("icmp ne", "i32", andv(r, sign), "0"), cf);
            break;
        }
        case Op::Shr: {
            r = select1(c_lt, "i32", andv(bin("lshr", "i32", a, c), mask), "0");
            if (need_cf) {
                const std::string s1 = andv(bin("sub", "i32", c, "1"), 31u);  // (c - 1) & 31
                const std::string cf_lt =
                    bin("icmp ne", "i32", andv(bin("lshr", "i32", a, s1), 1u), "0");
                const std::string cf_ge = select1(c_eq_bits, "i1", msbOfA(), "false");
                cf = select1(c_lt, "i1", cf_lt, cf_ge);
            }
            // ★ 修（正确性）：OF 只在计数 == 1 时被替换（`exec_integer.cpp:95` 的
            //   `of = c == 1 ? msbOf(a, size) : of`）。c > 1 与 c == 0 都**保留旧值** ——
            //   旧实现 `and(c_eq_one, msb(a))` 在 c > 1 时给 false（与解释器不同）。
            //   ⇒ 这里只算"c == 1 时的新值"，保留旧值的 select 在下面的组装段（`of_when1`）。
            if (need_of) of = msbOfA();
            break;
        }
        case Op::Sar: {
            std::string sa = a;
            if (size != 4) {
                const std::string sh = bin("shl", "i32", a, i32c(32u - bits));
                sa = bin("ashr", "i32", sh, i32c(32u - bits));
            }
            // `doShift` 里 `sh = (c > bits) ? bits : c` == min(c, bits)；`sh - 1` 再 & 31
            // （计数 0 时它是 31：值不会被选中，但必须**已定义**）。
            const std::string sh = select1(c_lt, "i32", c, i32c(bits));
            if (need_cf) {
                const std::string sh1 = andv(bin("sub", "i32", sh, "1"), 31u);
                cf = bin("icmp ne", "i32", andv(bin("ashr", "i32", sa, sh1), 1u), "0");
            }
            r = andv(bin("ashr", "i32", sa, sh), mask);
            // `of` 留空 = **不放进 six**，但掩码里含 kOf ⇒ OF 被清 0（`:102` 的 `of = false`）
            break;
        }
        case Op::Rol:
        case Op::Ror: {
            // k = c % bits == c & (bits-1)（bits 是 2 的幂）；l = (bits - k) % bits == (bits-k) & (bits-1)
            const std::string k = andv(c, bits - 1u);
            const std::string l = andv(bin("sub", "i32", i32c(bits), k), bits - 1u);
            const std::string rot = (in.op == Op::Rol)
                                        ? orList({bin("shl", "i32", a, k), bin("lshr", "i32", a, l)})
                                        : orList({bin("lshr", "i32", a, k), bin("shl", "i32", a, l)});
            r = andv(rot, mask);
            std::string msb_r;
            if (need_cf || need_of) msb_r = bin("icmp ne", "i32", andv(r, sign), "0");
            if (in.op == Op::Rol) {
                if (need_cf || need_of) cf = bin("icmp ne", "i32", andv(r, 1u), "0");
                if (need_of) of = bin1("xor", msb_r, cf);  // 无条件替换 OF（`:109`）
            } else {
                if (need_cf) cf = msb_r;                   // `cf = msbOf(r)`（`:115`）
                // ★ 修（正确性）：只在计数 == 1 时被替换（`exec_integer.cpp:116` 的
                //   `of = (c == 1) ? (msbOf(a) != msbOf(r)) : of`）。c > 1 与 c == 0 都**保留旧值** ——
                //   旧实现 `and(c_eq_one, msb(a) ^ msb(r))` 在 c > 1 时给 false（与解释器不同）。
                if (need_of) of = bin1("xor", msbOfA(), msb_r);
            }
            break;
        }
        case Op::Rcr: {
            // `exec_integer.cpp:131-142`：k = c % (bits+1)（运行期）；(CF,a) 当 bits+1 位数右旋 k。
            //   ★ CF 是**输入**（`flagsRWOf` 已把 CF 并进 R），OF 只在 c==1 时替换（`:140`）。
            const unsigned span = bits + 1;
            const std::string k32 = bin("urem", "i32", c, i32c(span));
            const std::string cfw = select1(readBit(kBitCf), "i32", i32c(1u << bits), "0");
            const std::string wv = bin("or", "i32", cfw, a);  // bits+1 位值（i32 够）
            const std::string k64 = tmp();
            add(k64 + " = zext i32 " + k32 + " to i64");
            const std::string s64 = tmp();
            add(s64 + " = sub i64 " + std::to_string(span) + ", " + k64);
            const std::string w64 = tmp();
            add(w64 + " = zext i32 " + wv + " to i64");
            const std::string r64 = bin("or", "i64", bin("lshr", "i64", w64, k64),
                                        bin("shl", "i64", w64, s64));
            const std::string rsp = bin("and", "i64", r64, i64c((1ull << span) - 1ull));
            const std::string r32 = tmp();
            add(r32 + " = trunc i64 " + rsp + " to i32");
            r = andv(r32, mask);
            const std::string cfb = tmp();
            add(cfb + " = lshr i64 " + rsp + ", " + std::to_string(bits));
            cf = bin("icmp ne", "i64", bin("and", "i64", cfb, "1"), "0");
            if (need_of)
                of = bin1("xor", msbOfA(), bin("icmp ne", "i32", andv(r, sign), "0"));
            break;
        }
        default: emitTrap(in, "移位族未实现的 op"); return;
    }
    // `c_nz == false` ⇒ 整条指令**一位都不写**（`:71` 的早退）。槽是 i1 ⇒ 用
    // `select(c_nz, 新值, 旧值)` 表达"这一条到底写没写这一位"（旧值 = base/前一个生产者的那一位）。
    const bool of_always = (in.op == Op::Shl || in.op == Op::Rol);   // of 表达式 + 无条件写
    const bool of_never = (in.op == Op::Sar);                       // 常量 0 + 无条件写
    const bool of_when1 =
        (in.op == Op::Shr || in.op == Op::Ror || in.op == Op::Rcr);  // 只在计数 == 1 时替换
    beginFlagWrite(w, fmask);
    auto sel = [&](FlagBit b, const std::string& newbit) {
        const std::string old = readBit(b);  // ★ 先读旧值（deliverBit 会覆盖槽）
        deliverBit(b, select1(c_nz, "i1", newbit, old));
    };
    if (!is_rot) {
        if ((fmask & (1u << kBitZf)) != 0) sel(kBitZf, bin("icmp eq", "i32", r, "0"));
        if ((fmask & (1u << kBitSf)) != 0)
            sel(kBitSf, bin("icmp ne", "i32", andv(r, sign), "0"));
        if ((fmask & (1u << kBitPf)) != 0) sel(kBitPf, parity(r));
    }
    if ((fmask & (1u << kBitCf)) != 0) sel(kBitCf, cf.empty() ? std::string("false") : cf);
    if ((fmask & (1u << kBitOf)) != 0) {
        if (of_always || of_never) {
            sel(kBitOf, of.empty() ? std::string("false") : of);
        } else if (of_when1) {
            // ★ 修（正确性）：`shr/ror r/m, cl` 的 OF 是**读-改-写**，不是"只在 c==1 写、其余写 false"。
            //   解释器 `exec_integer.cpp:95`（shr）/ `:116`（ror）都写成
            //       `of = (c == 1) ? 新值 : of;`
            //   ⇒ **c == 0 ⇒ 旧值**、**c == 1 ⇒ 新值**、**c > 1 ⇒ 旧值**（逐位照抄，⛔ 不是 SDM 的
            //   "undefined"）。旧实现 `select(c_nz, and(c_eq_one, 新值), 旧值)` 在 c > 1 时给 `false`，
            //   与解释器不同；这里换成 `select(c_eq_one, 新值, 旧值)`（c == 0 也自然落旧值）。
            //   ⚠ 覆盖限制（如实说明）：**按解释器 `:95/:116` 修，本语料无差分覆盖，理由：无读者**
            //     —— 全语料 `sar/shr …, cl` 的 CF/OF 没有任何 guest 读者（`tests/lift_ir_proto.cpp`
            //     §2b 的"可观察性边界"实测：640 处 `…, cl` 里没有一处这两位的值能活到出口），
            //     所以 ⛔ 不为此造假 case；本修靠"逐行照抄解释器"保证。
            //   `readBit` 必须在 `deliverBit` 之前（先取旧值，deliver 会覆盖槽）。
            const std::string old = readBit(kBitOf);
            const std::string c_eq_one = bin("icmp eq", "i32", c, "1");
            deliverBit(kBitOf, select1(c_eq_one, "i1", of, old));
        }
    }
    // 计数 == 0 时 `r == a` ⇒ 写回同值（地址/宽度合并规则照 `writeDest`/`writeRegValue`）。
    // ★ 目的操作数是**内存**时，计数 0 解释器**根本不写**（`:71` 的早退）⇒ 必须条件写
    //   （分支，⛔ 不能无条件 `lift_note_write` + store）。
    if (in.op0().kind == OpKind::Mem) {
        const std::string id = std::to_string(label_id_++);
        const std::string yes = "sh_yes_" + id, skip = "sh_skip_" + id;
        flushRegs();  // ★ 分支之前：脏寄存器先写回 `%st`（块内缓存跨分支不成立）
        add("br i1 " + c_nz + ", label %" + yes + ", label %" + skip);
        put(yes + ":\n");
        storeMemValue(in.op0().mem, r);
        add("br label %" + skip);
        put(skip + ":\n");
        invalidateRegs();  // ★ 合并点之后（同上）
    } else {
        writeDest(in.op0(), r);
    }
}

// `mul r/m`：照 `doMul`（`exec_integer.cpp:188-215`）。
// ★ S3-a：`W` 按第 14/15 行 —— `size==1` 写 {CF,OF,ZF,SF}（解释器**显式**写 ZF/SF），
//   `size==2/4` 只写 {CF,OF} ⇒ AF/PF 一位都不碰（保持 base 的旧值）。
void FnEmit::emitMul(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::uint32_t mask = maskOfSize(size);
    const std::string src = readValue(in.op0());
    const std::string eax = loadReg(0);
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    if (size == 4) {
        const std::string a64 = tmp();
        add(a64 + " = zext i32 " + eax + " to i64");
        const std::string b64 = tmp();
        add(b64 + " = zext i32 " + src + " to i64");
        const std::string prod = bin("mul", "i64", a64, b64);
        const std::string lo = tmp();
        add(lo + " = trunc i64 " + prod + " to i32");
        const std::string hi64 = bin("lshr", "i64", prod, "32");
        const std::string hi = tmp();
        add(hi + " = trunc i64 " + hi64 + " to i32");
        storeReg(0, lo);
        storeReg(2, hi);
        beginFlagWrite(w, m);
        if ((m & ((1u << kBitCf) | (1u << kBitOf))) != 0) {
            const std::string nz = bin("icmp ne", "i32", hi, "0");
            if ((m & (1u << kBitCf)) != 0) deliverBit(kBitCf, nz);
            if ((m & (1u << kBitOf)) != 0) deliverBit(kBitOf, nz);
        }
        return;
    }
    const std::string al = andv(eax, mask);
    const std::string bv = andv(src, mask);
    const std::string prod = bin("mul", "i32", al, bv);
    const std::string hi = andv(bin("lshr", "i32", prod, i32c(bitsOfSize(size))),
                                size == 2 ? 0xFFFFu : 0xFFu);
    if (size == 2) {
        writeRegValue(Reg::AX, andv(prod, 0xFFFFu));
        writeRegValue(Reg::DX, hi);
    } else {
        writeRegValue(Reg::AX, andv(prod, 0xFFFFu));
    }
    beginFlagWrite(w, m);
    if ((m & ((1u << kBitCf) | (1u << kBitOf))) != 0) {
        const std::string nz = bin("icmp ne", "i32", hi, "0");
        if ((m & (1u << kBitCf)) != 0) deliverBit(kBitCf, nz);
        if ((m & (1u << kBitOf)) != 0) deliverBit(kBitOf, nz);
    }
    if (size == 1) {
        // ★ 照抄 `doMul` 的 size==1 分支：ZF = (r & 0xFFFF)==0、SF = msbOf(r, 2)（bit15）。
        if ((m & (1u << kBitZf)) != 0)
            deliverBit(kBitZf, bin("icmp eq", "i32", andv(prod, 0xFFFFu), "0"));
        if ((m & (1u << kBitSf)) != 0)
            deliverBit(kBitSf, bin("icmp ne", "i32", andv(prod, 0x8000u), "0"));
    }
}

// `bt/bts/btr/btc`：只动 CF（+ 条件写目的操作数）。
// ★ SDM：**寄存器形**才把位偏移截到操作数宽度的低位；**内存形**的位偏移是**地址**偏移 ——
//   访问 `EA + (BitOffset DIV 位宽) * 字节数`，只有余数选位。语义真源 = 解释器
//   `exec_integer.cpp` 的 `Op::Bt/Bts/Btr/Btc`（⛔ 两臂不许各写一份判据）。
void FnEmit::emitBitTest(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::string idx = readValue(in.op1());
    const std::string bit = andv(idx, bits - 1u);
    // ★ 只有内存形才调整地址。`sdiv`（有符号截断除法）而不是 `udiv`：偏移是**有符号**的
    //   （`bt eax, -1` 合法）；余数由 `& (bits-1)` 取，与解释器的 `off & (bits-1)` 同形。
    const bool is_mem = in.op0().kind == OpKind::Mem;
    std::string mem_ea;
    if (is_mem) {
        const std::string slot = bin("sdiv", "i32", idx, i32c(bits));
        const std::string delta = bin("mul", "i32", slot, i32c(size));
        mem_ea = bin("add", "i32", eaOf(in.op0().mem), delta);
    }
    const std::string v = is_mem ? maskVal(loadMemAt(mem_ea, size), size) : readValue(in.op0());
    // ★ 第 16 行：`bt/bts/btr/btc` **只写 CF**（其余五位一位都不碰 ⇒ 保持 base 的旧值）。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    beginFlagWrite(w, m);
    if ((m & (1u << kBitCf)) != 0)
        deliverBit(kBitCf, bin("icmp ne", "i32", andv(bin("lshr", "i32", v, bit), 1u), "0"));
    if (in.op == Op::Bt) return;
    const std::string one = bin("shl", "i32", "1", bit);
    std::string nv;
    switch (in.op) {
        case Op::Bts: nv = bin("or", "i32", v, one); break;
        case Op::Btr: nv = bin("and", "i32", v, bin("xor", "i32", one, i32c(0xFFFFFFFFu))); break;
        default: nv = bin("xor", "i32", v, one); break;
    }
    if (is_mem) storeMemAt(mem_ea, size, nv);
    else writeDest(in.op0(), nv);
}

// `bsf/bsr`：只动 ZF；v==0 时不写目的寄存器（用 select 写回原值 ⇒ 观察不到差别）。
void FnEmit::emitBitScan(const Insn& in) {
    const unsigned size = in.op1().size != 0 ? in.op1().size : 4u;
    const std::string v = readValue(in.op1());
    // ★ 第 17 行：`bsf/bsr` **只写 ZF**。
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    // `zf` 同时是**结果**要用的（`nz = !zf` 决定写不写目的寄存器）⇒ 无论如何都要算。
    const std::string zf = bin("icmp eq", "i32", v, "0");
    beginFlagWrite(w, m);
    if ((m & (1u << kBitZf)) != 0) deliverBit(kBitZf, zf);
    const std::string nz = not1(zf);
    std::string idx;
    if (in.op == Op::Bsf) {
        idx = tmp();
        add(idx + " = call i32 @llvm.cttz.i32(i32 " + v + ", i1 false)");
        stats_->used_cttz = true;
    } else {
        const std::string lz = tmp();
        add(lz + " = call i32 @llvm.ctlz.i32(i32 " + v + ", i1 false)");
        stats_->used_ctlz = true;
        idx = bin("sub", "i32", "31", lz);
    }
    const std::string cur = readRegValue(in.op0().reg);
    writeDest(in.op0(), select1(nz, "i32", idx, cur));
}

// =====================================================================================
// 2026-11-15 · 本轮：把 `lift_one` 的调用点降到 0（剩下的都是**真翻译**）
// =====================================================================================
// ★ 每条都照抄解释器的 handler（行号写在定义处），⛔ 不发明第二份语义。
// ★ 停机面（`Hlt/Int3/Int/Other/Invalid` 与 `Div/Idiv` 的 `#DE`）发 `@lift_stop`：
//   它不是"执行一条 guest 指令"，而是把解释器在同一个停机点上的两步
//   （`running=false` + `stop_why=<同一句>`）表达出来（见 `lift_abi.h` 的 `LiftStopKind`）。
void FnEmit::emitStop(const Insn& in, std::uint32_t kind) {
    // 宿主调用之前同步 `%st`（`lift_stop` 会打印寄存器现场）。
    syncBeforeHostCall();
    add("; lift_stop @ " + hexEa(in.ea) + " (" + (in.mnemonic ? in.mnemonic : "?") + " / " +
        opName(in.op) + ") — 解释器在同一条停机面上（⛔ 不是解释执行一条指令）");
    add("call void @lift_stop(ptr %cpu, i32 " + i32c(in.ea) + ", i32 " + std::to_string(kind) + ")");
    add("ret i32 " + i32c(::kLiftHalt));
    stats_->used_lift_stop = true;
    ++stats_->lift_stop_sites;
    trapped_ = true;  // `ret` 之后不许再发指令（否则是非法 IR）
}

// `imul`：逐行照 `exec_integer.cpp:214-268`（`doImul`）。
//   · 1 操作数（`imul r/m`）：被乘数 = EAX/AX/AL，结果写 EDX:EAX / DX:AX / AX；
//   · 2 操作数（`0F AF`）：r = r * r/m；
//   · 3 操作数（`6B`/`69`）：r = **r/m** * imm（不是 r * imm —— `:244-248`）。
//   CF/OF = **有符号结果放不进目的宽度**（`sext(trunc(prod)) != prod`）。
void FnEmit::emitImul(const Insn& in) {
    ++stats_->imul_sites;
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::uint32_t mask = maskOfSize(size);
    const bool has_dst = in.op1().kind != OpKind::None;
    const bool has_imm = in.op2().kind != OpKind::None;
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    auto sext64 = [&](const std::string& v) {
        const std::string r = tmp();
        add(r + " = sext i32 " + v + " to i64");
        return r;
    };
    auto ovfOf = [&](const std::string& prod) {
        const std::string lo = tmp();
        add(lo + " = trunc i64 " + prod + " to i" + std::to_string(bits));
        const std::string back = tmp();
        add(back + " = sext i" + std::to_string(bits) + " " + lo + " to i64");
        return bin("icmp ne", "i64", back, prod);
    };
    if (!has_dst) {
        // 1 操作数形态：与 `mul` 同形，但按**有符号**算（`:217-242`）
        const std::string src = readValue(in.op0());
        const std::string eax = loadReg(0);
        const std::string prod =
            bin("mul", "i64", sext64(signExt(andv(eax, mask), size)), sext64(signExt(src, size)));
        const std::string t32 = tmp();
        add(t32 + " = trunc i64 " + prod + " to i32");
        if (size == 4) {
            const std::string hi = tmp();
            add(hi + " = trunc i64 " + bin("lshr", "i64", prod, "32") + " to i32");
            storeReg(0, t32);
            storeReg(2, hi);
        } else if (size == 2) {
            writeRegValue(Reg::AX, andv(t32, 0xFFFFu));
            writeRegValue(Reg::DX, andv(bin("lshr", "i32", t32, "16"), 0xFFFFu));
        } else {
            writeRegValue(Reg::AX, andv(t32, 0xFFFFu));
        }
        beginFlagWrite(w, m);
        if ((m & ((1u << kBitCf) | (1u << kBitOf))) != 0) {
            const std::string ovf = ovfOf(prod);
            if ((m & (1u << kBitCf)) != 0) deliverBit(kBitCf, ovf);
            if ((m & (1u << kBitOf)) != 0) deliverBit(kBitOf, ovf);
        }
        return;
    }
    // 2 / 3 操作数形态
    const Operand& mul_op = has_imm ? in.op1() : in.op0();
    const std::string a = sext64(signExt(readValue(mul_op), size));
    std::string b32;
    if (has_imm) {
        // `:253-257`：立即数按 **32 位有符号**取；只有 size==1 再截成 int8。
        b32 = i32c(static_cast<std::uint32_t>(in.op2().imm));
        if (size == 1) b32 = signExt(b32, 1);
    } else {
        b32 = signExt(readValue(in.op1()), size);
    }
    const std::string prod = bin("mul", "i64", a, sext64(b32));
    const std::string t32 = tmp();
    add(t32 + " = trunc i64 " + prod + " to i32");
    beginFlagWrite(w, m);
    if ((m & ((1u << kBitCf) | (1u << kBitOf))) != 0) {
        const std::string ovf = ovfOf(prod);
        if ((m & (1u << kBitCf)) != 0) deliverBit(kBitCf, ovf);
        if ((m & (1u << kBitOf)) != 0) deliverBit(kBitOf, ovf);
    }
    writeDest(in.op0(), t32);
}

// `div` / `idiv`：逐行照 `exec_integer.cpp:270-332`（`doDiv`）。
//   ★ 三条 `#DE` 判据与解释器**同一条**：除零、`idiv` 的 `MIN/-1`、商放不进目的宽度。
//     命中 ⇒ `lift_stop`（`cpuFault` 的同一停机面），商/余数一个字节都不写。
//   ★ 除法**必须在守卫之后**的块里发（`udiv/sdiv` 除以 0 是 UB ⇒ LLVM 可以合法利用）。
void FnEmit::emitDivIdiv(const Insn& in) {
    ++stats_->div_sites;
    const bool sign = (in.op == Op::Idiv);
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::uint32_t mask = maskOfSize(size);
    const std::string src = readValue(in.op0());   // 已按 size 截断
    const std::string eax = loadReg(0);
    const std::string edx = loadReg(2);
    const std::string id = std::to_string(label_id_++);
    // ★ 两个停机出口（除零/special 与商溢出）各发一条 `lift_stop`（`exec_integer.cpp` 的 `cpuFault` 同一条面）
    const std::string halt_zero = "lo_halt0_" + id;
    const std::string halt_ovf = "lo_halt1_" + id;
    const std::string ok = "dv_ok_" + id;
    const std::string done = "dv_done_" + id;

    // ---- 被除数 / 除数 / 三条 #DE 判据（全部是纯计算，不发除法）----
    std::string zero_cond;     // i1：除数为 0（⇒ cpuFault "除零（#DE）"）
    std::string special_cond;  // i1：`idiv` 的 MIN/-1（⇒ cpuFault "idiv 商溢出（#DE）"），其余恒 false
    std::string dividend;  // 除法用的带宽度的值
    std::string divisor;
    const char* op_ty = nullptr;
    if (size == 1) {
        op_ty = "i32";
        const std::string d = andv(eax, 0xFFFFu);
        const std::string v = andv(src, 0xFFu);
        if (sign) {
            dividend = signExt(d, 2);   // int16 → i32
            divisor = signExt(v, 1);    // int8 → i32
        } else {
            dividend = d;
            divisor = v;
        }
        const std::string zero = bin("icmp eq", "i32", divisor, "0");
        if (sign) {
            const std::string lo = bin("icmp slt", "i32", dividend, "-32768");
            const std::string hi = bin("icmp sgt", "i32", dividend, "32767");
            zero_cond = zero;  // size==1 的 idiv 不会溢出 i32 ⇒ 没有 special
            special_cond = "false";
            (void)lo;
            (void)hi;
        } else {
            zero_cond = zero;
            special_cond = "false";
        }
    } else if (size == 2) {
        op_ty = "i32";
        const std::string d = bin("or", "i32", bin("shl", "i32", andv(edx, 0xFFFFu), "16"),
                                  andv(eax, 0xFFFFu));
        const std::string v = andv(src, 0xFFFFu);
        dividend = d;
        divisor = sign ? signExt(v, 2) : v;
        const std::string zero = bin("icmp eq", "i32", divisor, "0");
        if (sign) {
            // `:290`：INT32_MIN / -1 必须在**除法之前**判（C++ 里那是 UB）
            const std::string sp1 = bin("icmp eq", "i32", dividend, i32c(0x80000000u));
            const std::string sp2 = bin("icmp eq", "i32", divisor, "-1");
            zero_cond = zero;
            special_cond = and1(sp1, sp2);
        } else {
            zero_cond = zero;
            special_cond = "false";
        }
    } else {
        op_ty = "i64";
        const std::string e64 = tmp();
        add(e64 + " = zext i32 " + edx + " to i64");
        const std::string hi = bin("shl", "i64", e64, "32");
        const std::string lo = tmp();
        add(lo + " = zext i32 " + eax + " to i64");
        dividend = bin("or", "i64", hi, lo);
        const std::string z = tmp();
        add(z + " = zext i32 " + src + " to i64");
        if (sign) {
            const std::string s = tmp();
            add(s + " = sext i32 " + src + " to i64");
            divisor = s;
            const std::string zero = bin("icmp eq", "i64", divisor, "0");
            const std::string sp1 = bin("icmp eq", "i64", dividend,
                                        "-9223372036854775808");
            const std::string sp2 = bin("icmp eq", "i64", divisor, "-1");
            zero_cond = zero;
            special_cond = and1(sp1, sp2);
        } else {
            divisor = z;
            zero_cond = bin("icmp eq", "i64", divisor, "0");
            special_cond = "false";
        }
    }
    flushRegs();  // ★ 同上
    const std::string halt_special = "lo_halt2_" + id;
    const std::string sp = "dv_sp_" + id;
    add("br i1 " + zero_cond + ", label %" + halt_zero + ", label %" + sp);
    put(sp + ":\n");
    add("br i1 " + special_cond + ", label %" + halt_special + ", label %" + ok);
    put(ok + ":\n");
            invalidateRegs();  // ★ 合并点之后（同上）
    // ---- 除法（此块里除数 != 0、且不是 idiv 的 MIN/-1）----
    std::string q;
    std::string rem;
    if (size == 4) {
        q = bin(sign ? "sdiv" : "udiv", "i64", dividend, divisor);
        rem = bin(sign ? "srem" : "urem", "i64", dividend, divisor);
    } else {
        q = bin(sign ? "sdiv" : "udiv", "i32", dividend, divisor);
        rem = bin(sign ? "srem" : "urem", "i32", dividend, divisor);
    }
    // ---- 商的宽度判据（`:281/293/303/312/319/326`）----
    std::string ovf;
    if (size == 1) {
        ovf = sign ? or1(bin("icmp slt", "i32", q, "-128"), bin("icmp sgt", "i32", q, "127"))
                   : bin("icmp ugt", "i32", q, "255");
    } else if (size == 2) {
        ovf = sign ? or1(bin("icmp slt", "i32", q, "-32768"), bin("icmp sgt", "i32", q, "32767"))
                   : bin("icmp ugt", "i32", q, "65535");
    } else {
        ovf = sign ? or1(bin("icmp slt", "i64", q, "-2147483648"),
                         bin("icmp sgt", "i64", q, "2147483647"))
                   : bin("icmp ugt", "i64", q, "4294967295");
    }
    add("br i1 " + ovf + ", label %" + halt_ovf + ", label %" + done);
    put(done + ":\n");
    // ---- 写回（`:282-328` 逐支照抄）----
    const std::string q32 = (size == 4) ? [&] {
        std::string t = tmp();
        add(t + " = trunc i64 " + q + " to i32");
        return t;
    }() : q;
    const std::string r32 = (size == 4) ? [&] {
        std::string t = tmp();
        add(t + " = trunc i64 " + rem + " to i32");
        return t;
    }() : rem;
    if (size == 1) {
        writeRegValue(Reg::AL, andv(q32, 0xFFu));
        writeRegValue(Reg::AH, andv(r32, 0xFFu));
    } else if (size == 2) {
        writeRegValue(Reg::AX, andv(q32, 0xFFFFu));
        writeRegValue(Reg::DX, andv(r32, 0xFFFFu));
    } else {
        storeReg(0, q32);
        storeReg(2, r32);
    }
    (void)mask;
    halt_labels_.push_back(HaltSite{halt_zero, in.ea, static_cast<std::uint32_t>(::kLiftStopDivZero)});
    halt_labels_.push_back(HaltSite{halt_special, in.ea, static_cast<std::uint32_t>(::kLiftStopIdivOverflow)});
    halt_labels_.push_back(HaltSite{halt_ovf, in.ea, sign ? static_cast<std::uint32_t>(::kLiftStopIdivOverflow) : static_cast<std::uint32_t>(::kLiftStopDivOverflow)});
}

// 串操作：逐行照 `exec_string.cpp:219-304`（`execString`）。
//   · `movs`/`stos`（**带 `rep`**）：先在**解码那一层**认出"这是 `rep movs*` / `rep stos*`"
//     （`in.rep != 0 && (in.op == Movs || in.op == Stos)`；`F3 A4/A5/AA/AB` 解码后就是这两个 op）
//     ⇒ 发**一次** `@lift_rep_bulk(ptr %cpu, ptr %st, i32 <EA>, i32 <kind>, i32 <size>)`：
//     `true` = 闸门放行、整段已搬完（跳过逐元素 IR）；`false` = 闸门拒绝 ⇒ 落回下面那对
//     **真实 IR 循环**（DF 正向 / 反向各一份 ⇒ 正向那份本来指望 `LoopIdiomRecognize` 认成 memcpy）。
//     ★ 判据不在发射器里：闸门**唯一真源** = `runtime/rep_bulk.h` 的 `repBulkPlan`，执行 = 同一个
//       `repBulkExecute`（`exec_string.cpp`）⇒ 解释器臂与 AOT 臂共用一条判定 + 一份实现 + 一份计数。
//       ⛔ 这里不许出现第二条"跨 4 GiB / committed / 方向×重叠"的判断（那是第二套闸门）。
//   · `scas`/`cmps`：单循环 + 运行期步长（每元素写 EFLAGS，且 `repe/repne` 会提前结束）。
//   ★ DF 的**唯一来源**是 `%st` 的 eflags 基字（`exec_string.cpp` 的 `advance` 读 `cpu.df()`）。
//   ★ SMC 记账：`movs`/`stos` 的**逐元素臂**在循环之前发一次 `lift_note_write(lo, n)`（与解释器自己的
//     `rep_bulk` 快路径同一口径 —— 它也是一次 `noteGuestWrite(lo, n)`，而
//     `BlockCache::noteWrite` 内部按页循环 ⇒ 页集合与逐元素调用相同；`exec_string.cpp:81-138`）。
//     ★ 这一处与 `storeMemAt` 走**同一道门**：`LIFT_AOT_NOTE_WRITE` 缺省 OFF ⇒ 一条都不发。
//     ★ 快路径那支不发 `lift_note_write`：`repBulkExecute` 自己在**写之前**做一次
//       `noteGuestWrite(lo, n)`（同一段、同一页集合）。AOT 臂 `cpu.cache == nullptr` ⇒ 两条都是 no-op。
//   ★ 非 rep 的单元素形态走 `writeElem` 的**逐元素**钩子（`exec_string.cpp:41-49`）。
void FnEmit::emitString(const Insn& in) {
    ++stats_->string_sites;
    const unsigned size = in.opsz != 0 ? in.opsz : 4u;
    const std::uint32_t mask = maskOfSize(size);
    const std::string ity = size == 1 ? "i8" : (size == 2 ? "i16" : "i32");
    const bool rep = (in.rep != 0);
    const bool repne = (in.rep == 0xF2);
    const std::string id = std::to_string(label_id_++);
    const std::string esi0 = loadReg(6);
    const std::string edi0 = loadReg(7);

    auto loadElem = [&](const std::string& ea) {
        const std::string v = tmp();
        add(v + " = load " + ity + ", ptr " + memPtrFor(ea) + ", align 1");
        return v;
    };
    auto noteWriteAt = [&](const std::string& ea, const std::string& nbytes) {
        // ★ 与 `storeMemAt` 同一道门（`LIFT_AOT_NOTE_WRITE`，缺省 OFF ⇒ 这里什么都不发）。
        if (!ir_target_.note_write) return;
        syncBeforeHostCall();
        add("call void @lift_note_write(ptr %cpu, i32 " + ea + ", i32 " + nbytes + ")");
        stats_->used_note_write = true;
    };
    auto eaxMasked = [&]() { return andv(loadReg(0), mask); };
    auto stosValue = [&]() {
        // ★ size==4 时 EAX 本身就是元素值（⛔ 不要发 `trunc i32 to i32`：非法 IR，实测 clang 红）
        if (size >= 4) return eaxMasked();
        const std::string v = tmp();
        add(v + " = trunc i32 " + eaxMasked() + " to " + ity);
        return v;
    };

    // ---------- 无 rep：单元素（`exec_string.cpp:275-278`）----------
    if (!rep) {
        const std::string dfb = bin("icmp ne", "i32", andv(loadEflags(), kDf), "0");
        const std::string step = select1(dfb, "i32", i32c(0u - size), i32c(size));
        if (in.op == Op::Movs || in.op == Op::Stos) {
            const std::string v = (in.op == Op::Movs) ? loadElem(esi0) : stosValue();
            noteWriteAt(edi0, std::to_string(size));  // ★ 先记账再写（与 `writeElem` 同序）
            add("store " + ity + " " + v + ", ptr " + memPtrFor(edi0) + ", align 1");
            if (in.op == Op::Movs) storeReg(6, bin("add", "i32", esi0, step));
            storeReg(7, bin("add", "i32", edi0, step));
            return;
        }
        if (in.op == Op::Scas) {
            const std::string b16 = loadElem(edi0);
            const std::string b = tmp();
            add(b + (size >= 4 ? std::string(" = or i32 ") + b16 + ", 0" : std::string(" = zext ") + ity + " " + b16 + " to i32"));
            const std::string a = eaxMasked();
            const std::string r = bin("sub", "i32", a, b);
            beginFlagWrite(flagsWriteMask(in), wrMaskAt(cur_index_));
            deliverSubBits(wrMaskAt(cur_index_), a, b, r, size, /*with_borrow=*/false);
            storeReg(7, bin("add", "i32", edi0, step));
            return;
        }
        // Cmps
        const std::string a16 = loadElem(esi0);
        const std::string b16 = loadElem(edi0);
        const std::string a = tmp();
        add(a + (size >= 4 ? std::string(" = or i32 ") + a16 + ", 0" : std::string(" = zext ") + ity + " " + a16 + " to i32"));
        const std::string b = tmp();
        add(b + (size >= 4 ? std::string(" = or i32 ") + b16 + ", 0" : std::string(" = zext ") + ity + " " + b16 + " to i32"));
        const std::string r = bin("sub", "i32", a, b);
        beginFlagWrite(flagsWriteMask(in), wrMaskAt(cur_index_));
        deliverSubBits(wrMaskAt(cur_index_), a, b, r, size, /*with_borrow=*/false);
        storeReg(6, bin("add", "i32", esi0, step));
        storeReg(7, bin("add", "i32", edi0, step));
        return;
    }

    // ---------- rep ----------
    const std::string ecx0 = loadReg(1);
    const std::string lim = tmp();
    add(lim + " = zext i32 " + ecx0 + " to i64");
    const std::string n = bin("mul", "i32", ecx0, i32c(size));
    const std::string dfb = bin("icmp ne", "i32", andv(loadEflags(), kDf), "0");

    if (in.op == Op::Movs || in.op == Op::Stos) {
        // ===================== ① 闸门快路径探针 =====================
        // 判据全在 runtime：`@lift_rep_bulk` → `lift::repBulkTry` → `repBulkPlan` + `repBulkExecute`。
        // 发射器只发**一次**调用，并把"放行了吗"当分支条件；拒绝 ⇒ 走下面那段逐元素 IR。
        const std::string handled = tmp();
        const std::string slow = "st_slow_" + id, done = "st_done_" + id;
        syncBeforeHostCall();  // ★ 宿主调用前：挂账 tick 结算 + 脏寄存器物化（`%st` 才是最新）
        add(handled + " = call i1 @lift_rep_bulk(ptr %cpu, ptr %st, i32 " + i32c(in.ea) + ", i32 " +
            (in.op == Op::Stos ? "1" : "0") + ", i32 " + std::to_string(size) + ")");
        stats_->used_rep_bulk = true;
        ++stats_->rep_bulk_sites;
        add("br i1 " + handled + ", label %" + done + ", label %" + slow);
        put(slow + ":\n");
        // ★ 拒绝路径**重新读**寄存器：宿主调用可能改了 8 GPR（放行时 ECX/ESI/EDI 都变了）⇒ 缓存作废。
        //   ⚠ `esi0`/`edi0`/`ecx0`/`n`/`dfb` 是**调用之前**算出来的 SSA 值 —— 它们在 `slow` 与 `done`
        //     两个块里都支配（`done` 的写回要用），而拒绝路径不写 guest 状态（`rep_bulk.h` 的契约）。
        invalidateRegs();
        // ---- ② 逐元素臂（闸门拒绝时唯一的路；与改动前逐字节相同）----
        // SMC：整段一次（解释器 `rep_bulk` 快路径同一口径；见函数头注）
        const std::string dn = bin("sub", "i32", edi0, n);
        const std::string lo = select1(dfb, "i32", bin("add", "i32", dn, i32c(size)), edi0);
        noteWriteAt(lo, n);
        // ★ 循环不变量：`stos` 的值必须**提到两个循环之前**（在循环体里 loadReg 会在第一个体块里
        //   定义、第二个体块里复用 ⇒ 不支配；实测 clang：Instruction does not dominate all uses）
        const std::string stos_v = (in.op == Op::Stos) ? stosValue() : std::string();
        const std::string pf = "st_pf_" + id, hf = "st_hf_" + id, bf = "st_bf_" + id;
        const std::string xf = "st_xf_" + id, pb = "st_pb_" + id, hb = "st_hb_" + id;
        const std::string bb = "st_bb_" + id, xb = "st_xb_" + id, after = "st_after_" + id;
        flushRegs();  // ★ 分支之前：把脏寄存器写回 `%st`（块内缓存跨分支不成立）
        add("br i1 " + dfb + ", label %" + pb + ", label %" + pf);
        // ---- 正向循环（步长 = +size 常量 ⇒ `LoopIdiomRecognize` 可识别）----
        put(pf + ":\n");
        add("br label %" + hf);
        put(hf + ":\n");
        const std::string i_f = tmp();
        const std::string i_fn = i_f + "_n";
        const std::string d_f = tmp();
        add(i_f + " = phi i64 [ 0, %" + pf + " ], [ " + i_fn + ", %" + bf + " ]");
        add(d_f + " = icmp uge i64 " + i_f + ", " + lim);
        add("br i1 " + d_f + ", label %" + xf + ", label %" + bf);
        put(bf + ":\n");
        {
            const std::string off = bin("mul", "i64", i_f, std::to_string(size));
            const std::string ib = tmp();
            add(ib + " = trunc i64 " + off + " to i32");
            const std::string de = bin("add", "i32", edi0, ib);
            const std::string v =
                (in.op == Op::Movs) ? loadElem(bin("add", "i32", esi0, ib)) : stos_v;
            add("store " + ity + " " + v + ", ptr " + memPtrFor(de) + ", align 1");
            add(i_fn + " = add i64 " + i_f + ", 1");
            add("br label %" + hf);
        }
        put(xf + ":\n");
        add("br label %" + after);
        // ---- 反向循环（`DF=1`：地址递减）----
        put(pb + ":\n");
        add("br label %" + hb);
        put(hb + ":\n");
        const std::string i_b = tmp();
        const std::string i_bn = i_b + "_n";
        const std::string d_b = tmp();
        add(i_b + " = phi i64 [ 0, %" + pb + " ], [ " + i_bn + ", %" + bb + " ]");
        add(d_b + " = icmp uge i64 " + i_b + ", " + lim);
        add("br i1 " + d_b + ", label %" + xb + ", label %" + bb);
        put(bb + ":\n");
        {
            const std::string off = bin("mul", "i64", i_b, std::to_string(size));
            const std::string ib = tmp();
            add(ib + " = trunc i64 " + off + " to i32");
            const std::string de = bin("sub", "i32", edi0, ib);
            const std::string v =
                (in.op == Op::Movs) ? loadElem(bin("sub", "i32", esi0, ib)) : stos_v;
            add("store " + ity + " " + v + ", ptr " + memPtrFor(de) + ", align 1");
            add(i_bn + " = add i64 " + i_b + ", 1");
            add("br label %" + hb);
        }
        put(xb + ":\n");
        add("br label %" + after);
        put(after + ":\n");
        invalidateRegs();  // ★ 合并点之后：分支/循环里 load 过的缓存值不许再用
        // 收尾：ECX = 0；ESI/EDI 各走 `n`（32 位回绕）。两个方向都用 `dfb` 选 ⇒ 无 phi 合并。
        storeReg(1, "0");
        if (in.op == Op::Movs)
            storeReg(6, select1(dfb, "i32", bin("sub", "i32", esi0, n), bin("add", "i32", esi0, n)));
        storeReg(7, select1(dfb, "i32", bin("sub", "i32", edi0, n), bin("add", "i32", edi0, n)));
        // ★ 慢路径在**自己的块里**把写回落盘：`done` 是合并点，那里必须让缓存为空（快路径没有这些值）。
        flushRegs();
        add("br label %" + done);
        // ===================== ③ 合并点（两条路都到这里；寄存器已在 `%st` 里）=====================
        put(done + ":\n");
        invalidateRegs();
        return;
    }

    // ---------- scas / cmps：单循环 + 运行期步长 + `repe/repne` 提前结束 ----------
    // 判据（`exec_string.cpp:293-302`）等价于：`i < ecx0 && (i == 0 || (repne ? zf : !zf))`，
    // `i` = 已完成的元素数；`i == 0` 那一支由 `inz` 门表达（初值 zf=false 不会被选中）。
    const std::string pre = "st_pre_s" + id, head = "st_head_s" + id;
    const std::string body = "st_body_s" + id, exit = "st_exit_s" + id;
    const std::string step = select1(dfb, "i32", i32c(0u - size), i32c(size));
    const std::string i_v = tmp();
    const std::string a_v = tmp();
    const std::string b_v = tmp();
    const std::string z_v = tmp();
    const std::string i_n = i_v + "_n", a_n = a_v + "_n", b_n = b_v + "_n", z_n = z_v + "_n";
    flushRegs();  // ★ 循环之前（同上）
    add("br label %" + pre);
    put(pre + ":\n");
    add("br label %" + head);
    put(head + ":\n");
    add(i_v + " = phi i64 [ 0, %" + pre + " ], [ " + i_n + ", %" + body + " ]");
    add(a_v + " = phi i32 [ 0, %" + pre + " ], [ " + a_n + ", %" + body + " ]");
    add(b_v + " = phi i32 [ 0, %" + pre + " ], [ " + b_n + ", %" + body + " ]");
    add(z_v + " = phi i1 [ false, %" + pre + " ], [ " + z_n + ", %" + body + " ]");
    const std::string done = tmp();
    add(done + " = icmp uge i64 " + i_v + ", " + lim);
    const std::string inz = tmp();
    add(inz + " = icmp ne i64 " + i_v + ", 0");
    const std::string brk = tmp();
    add(brk + " = and i1 " + inz + ", " + (repne ? z_v : not1(z_v)));
    const std::string stp = tmp();
    add(stp + " = or i1 " + done + ", " + brk);
    add("br i1 " + stp + ", label %" + exit + ", label %" + body);
    put(body + ":\n");
    {
        if (in.op == Op::Scas) {
            add(a_n + " = and i32 " + loadReg(0) + ", " + i32c(mask));
            const std::string be = loadElem(edi0);
            add(b_n + (size >= 4 ? std::string(" = or i32 ") + be + ", 0" : std::string(" = zext ") + ity + " " + be + " to i32"));
        } else {
            const std::string ae = loadElem(esi0);
            const std::string be = loadElem(edi0);
            add(a_n + (size >= 4 ? std::string(" = or i32 ") + ae + ", 0" : std::string(" = zext ") + ity + " " + ae + " to i32"));
            add(b_n + (size >= 4 ? std::string(" = or i32 ") + be + ", 0" : std::string(" = zext ") + ity + " " + be + " to i32"));
        }
        const std::string rr = tmp();
        add(rr + " = sub i32 " + a_n + ", " + b_n);
        add(z_n + " = icmp eq i32 " + rr + ", 0");
        add(i_n + " = add i64 " + i_v + ", 1");
        add("br label %" + head);
    }
    put(exit + ":\n");
    invalidateRegs();  // ★ 合并点之后（同上）
    const std::string i32v = tmp();
    add(i32v + " = trunc i64 " + i_v + " to i32");
    const std::string off = bin("mul", "i32", i32v, step);
    storeReg(1, bin("sub", "i32", ecx0, i32v));
    if (in.op == Op::Cmps) storeReg(6, bin("add", "i32", esi0, off));
    storeReg(7, bin("add", "i32", edi0, off));
    // flags：`executed` 为假 ⇒ 六位一位不动（旧值穿过），与 `:279` 的早退同口径。
    const std::string ex = tmp();
    add(ex + " = icmp ne i64 " + i_v + ", 0");
    const std::string rr = bin("sub", "i32", a_v, b_v);
    beginFlagWrite(flagsWriteMask(in), wrMaskAt(cur_index_));
    deliver_guard_ = ex;
    deliverSubBits(wrMaskAt(cur_index_), a_v, b_v, rr, size, /*with_borrow=*/false);
    deliver_guard_.clear();
}

// `shld` / `shrd`：逐行照 `exec_integer.cpp:157-183`（`doShxd`）。
//   ★ 计数 0 或 ≥ 位宽 ⇒ **整条什么都不做**（`:164`）—— 目的操作数不写、六位一位不写。
//   ★ `shld` 的 `wide = (dst<<bits)|src`、`shrd` 的 `wide = (src<<bits)|dst`（`:167-175`）。
void FnEmit::emitShldShrd(const Insn& in) {
    ++stats_->shxd_sites;
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    const std::string dstv = readValue(in.op0());
    const std::string srcv = readValue(in.op1());
    const bool imm_form = (in.op1().kind == OpKind::Imm);
    std::string cond;  // 空 = 无条件（imm 且 0<c<bits）
    if (imm_form) {
        const std::uint8_t c0 = static_cast<std::uint8_t>(
            static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
        if (c0 == 0 || c0 >= bits) return;  // `:164` 的早退：一条语句都不发
    } else {
        const std::string c0 = andv(readValue(in.op1()), 0x1Fu);
        const std::string nz = bin("icmp ne", "i32", c0, "0");
        const std::string lt = bin("icmp ult", "i32", c0, i32c(bits));
        cond = and1(nz, lt);
    }
    // 计数（i64；imm 是常量、reg 是运行期值。poison 已用 `& 31` 挡掉）
    std::string c64;
    if (imm_form) {
        c64 = i64c(static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
    } else {
        const std::string c0 = andv(readValue(in.op1()), 0x1Fu);
        c64 = tmp();
        add(c64 + " = zext i32 " + c0 + " to i64");
    }
    const std::string c32 = imm_form ? i32c(static_cast<std::uint32_t>(in.op1().imm) & 0x1F)
                                     : andv(readValue(in.op1()), 0x1Fu);
    // ---- wide / 结果 ----
    const std::string d64 = tmp();
    add(d64 + " = zext i32 " + dstv + " to i64");
    const std::string s64 = tmp();
    add(s64 + " = zext i32 " + srcv + " to i64");
    const std::string wide =
        (in.op == Op::Shld)
            ? bin("or", "i64", bin("shl", "i64", d64, i64c(bits)), s64)
            : bin("or", "i64", bin("shl", "i64", s64, i64c(bits)), d64);
    std::string r64;
    if (in.op == Op::Shld)
        r64 = bin("lshr", "i64", bin("shl", "i64", wide, c64), i64c(bits));
    else
        r64 = bin("lshr", "i64", wide, c64);
    const std::string r32 = tmp();
    add(r32 + " = trunc i64 " + r64 + " to i32");
    const std::string rm = andv(r32, mask);
    // ---- CF / OF ----
    std::string cf;
    if (in.op == Op::Shld) {
        const std::string sh = andv(bin("sub", "i32", i32c(bits), c32), 31u);
        cf = bin("icmp ne", "i32", andv(bin("lshr", "i32", dstv, sh), 1u), "0");
    } else {
        const std::string sh = andv(bin("sub", "i32", c32, "1"), 31u);
        cf = bin("icmp ne", "i32", andv(bin("lshr", "i32", dstv, sh), 1u), "0");
    }
    // ---- 目的操作数（imm ⇒ 无条件；reg ⇒ 条件写。内存目的操作数用分支，⛔ 不无条件 store）----
    if (cond.empty()) {
        writeDest(in.op0(), rm);
    } else if (in.op0().kind == OpKind::Mem) {
        const std::string id = std::to_string(label_id_++);
        const std::string yes = "sx_yes_" + id, skip = "sx_skip_" + id;
        flushRegs();  // ★ 分支之前：脏寄存器先写回 `%st`（块内缓存跨分支不成立）
        add("br i1 " + cond + ", label %" + yes + ", label %" + skip);
        put(yes + ":\n");
        storeMemValue(in.op0().mem, rm);
        add("br label %" + skip);
        put(skip + ":\n");
        invalidateRegs();  // ★ 合并点之后（同上）
    } else {
        writeDest(in.op0(), select1(cond, "i32", rm, dstv));
    }
    // ---- flags（CF/OF/ZF/SF/PF；⛔ 不写 AF）----
    beginFlagWrite(w, m);
    deliver_guard_ = cond;  // 空 ⇒ 无条件
    if ((m & (1u << kBitCf)) != 0) deliverBit(kBitCf, cf);
    if ((m & (1u << kBitOf)) != 0)
        deliverBit(kBitOf, bin1("xor", bin("icmp ne", "i32", andv(dstv, sign), "0"),
                                bin("icmp ne", "i32", andv(rm, sign), "0")));
    if ((m & (1u << kBitZf)) != 0) deliverBit(kBitZf, bin("icmp eq", "i32", rm, "0"));
    if ((m & (1u << kBitSf)) != 0)
        deliverBit(kBitSf, bin("icmp ne", "i32", andv(rm, sign), "0"));
    if ((m & (1u << kBitPf)) != 0) deliverBit(kBitPf, parity(rm));
    deliver_guard_.clear();
}

// `cmpxchg`：逐行照 `exec_integer.cpp:414-427`。
//   · flags = `setSubFlags(ACC, DST)`（:420）⇒ 六位全写；
//   · 相等 ⇒ DST := SRC；不等 ⇒ EAX 的低 `size` 字节 := DST。
void FnEmit::emitCmpxchg(const Insn& in) {
    ++stats_->cmpxchg_sites;
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::uint32_t mask = maskOfSize(size);
    const std::string dstv = readValue(in.op0());
    const std::string eax = loadReg(0);
    const std::string acc = andv(eax, mask);
    const std::string src = readValue(in.op1());
    const std::string r = bin("sub", "i32", acc, dstv);
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    beginFlagWrite(w, m);
    deliverSubBits(m, acc, dstv, r, size, /*with_borrow=*/false);
    const std::string eq = bin("icmp eq", "i32", acc, dstv);
    const std::string neweax = bin("or", "i32", andv(eax, ~mask), andv(dstv, mask));
    storeReg(0, select1(eq, "i32", eax, neweax));
    if (in.op0().kind == OpKind::Mem) {
        // 条件写内存：用分支（⛔ 不无条件 store —— 解释器只在相等时写）
        const std::string id = std::to_string(label_id_++);
        const std::string yes = "cx_yes_" + id, skip = "cx_skip_" + id;
        flushRegs();  // ★ 分支之前：脏寄存器先写回 `%st`（块内缓存跨分支不成立）
        add("br i1 " + eq + ", label %" + yes + ", label %" + skip);
        put(yes + ":\n");
        storeMemValue(in.op0().mem, src);
        add("br label %" + skip);
        put(skip + ":\n");
        invalidateRegs();  // ★ 合并点之后（同上）
    } else {
        writeDest(in.op0(), select1(eq, "i32", src, dstv));
    }
}

// `cpuid`：逐行照 `exec_integer.cpp:595-614`（虚构 CPU 的固定常量表）。
void FnEmit::emitCpuid(const Insn& in) {
    (void)in;
    const std::string leaf = loadReg(0);
    const std::string l0 = bin("icmp eq", "i32", leaf, "0");
    const std::string l1 = bin("icmp eq", "i32", leaf, "1");
    const std::string l2 = bin("icmp eq", "i32", leaf, i32c(0x80000000u));
    auto pick = [&](const char* a, const char* b, const char* c, const char* d) {
        const std::string s1 = select1(l2, "i32", c, d);
        const std::string s2 = select1(l1, "i32", b, s1);
        return select1(l0, "i32", a, s2);
    };
    storeReg(0, pick("1", "1778", i32c(0x80000004u).c_str(), "0"));
    storeReg(3, pick(i32c(0x756E6547u).c_str(), "2048", "0", "0"));
    storeReg(1, pick(i32c(0x6C65746Eu).c_str(), "0", "0", "0"));
    storeReg(2, pick(i32c(0x49656E69u).c_str(), "1", "0", "0"));
}

// ---------------------------------------------------------------------------------------
// MMX / SSE 的向量读写（`exec_fpu.cpp:511-575` 的 pkLoad / pkStore 同一份形态判据）
// ---------------------------------------------------------------------------------------
// ★ 统一载成 `<16 x i8>`：mm = 8 字节（高 8 字节**补 0**，与 `pkLoad` 的 `memset(v.b,0,…)` 同）、
//   xmm = 16 字节、内存 = 4/8/16 字节（同样补 0 到 16）、GPR（只有 movd 的源）= 低 4 字节。
// ★ 内存一律 `align 1`（guest 不保证对齐）。
std::string FnEmit::cpuBytePtr(std::uint64_t off) { return cpuGep(off); }

std::string FnEmit::bc(const std::string& v, const char* ty) {
    const std::string r = tmp();
    add(r + " = bitcast <16 x i8> " + v + " to " + ty);
    return r;
}

// `<n x i8>` → `<16 x i8>`（高字节补 0；n ∈ {4,8}）
static std::string widenMaskStr(int n) {
    std::string s = "<16 x i32> <";
    for (int i = 0; i < 16; ++i) {
        if (i) s += ", ";
        s += "i32 " + std::to_string(i < n ? i : n);
    }
    return s + ">";
}

// 取 `<16 x i8>` 的低 `n` 个字节（n < 16）
static std::string lowMaskStr(int n) {
    std::string s = "<" + std::to_string(n) + " x i32> <";
    for (int i = 0; i < n; ++i) {
        if (i) s += ", ";
        s += "i32 " + std::to_string(i);
    }
    return s + ">";
}

std::string FnEmit::vecLoad(const Operand& op, int* bytes) {
    if (op.kind == OpKind::Xmm) {
        if (op.vec >= 8) {
            fail("xmm 下标越界");
            return "zeroinitializer";
        }
        const std::string v = tmp();
        add(v + " = load <16 x i8>, ptr " +
                cpuGep(x87l::kXmm + x87l::kXmmStride * static_cast<std::uint64_t>(op.vec)) +
                ", align 1");
        *bytes = 16;
        return v;
    }
    if (op.kind == OpKind::Reg) {
        const RegSpec sp = regSpec(op.reg);
        if (sp.cls == RegClass::X87) {
            // ★ MMX = **固定物理槽**的低 8 字节（⛔ 不过 `phys()`，理由见 `exec_fpu.cpp:518-522`）
            const std::string h = tmp();
            add(h + " = load <8 x i8>, ptr " +
                cpuGep(x87l::kRegs + 8ull * static_cast<std::uint64_t>(sp.index)) + ", align 1");
            *bytes = 8;
            const std::string v = tmp();
            add(v + " = shufflevector <8 x i8> " + h + ", <8 x i8> zeroinitializer, " +
                widenMaskStr(8));
            return v;
        }
        const std::string g = readRegValue(op.reg);
        const std::string h = tmp();
        add(h + " = bitcast i32 " + g + " to <4 x i8>");
        *bytes = 4;
        const std::string v = tmp();
        add(v + " = shufflevector <4 x i8> " + h + ", <4 x i8> zeroinitializer, " +
            widenMaskStr(4));
        return v;
    }
    if (op.kind == OpKind::Mem) {
        const int nb = op.mem.size;
        if (nb != 4 && nb != 8 && nb != 16) {
            fail("MMX/SSE 的内存操作数宽度不是 4/8/16");
            return "zeroinitializer";
        }
        const std::string p = memPtrFor(eaOf(op.mem));
        const std::string h = tmp();
        add(h + " = load <" + std::to_string(nb) + " x i8>, ptr " + p + ", align 1");
        *bytes = nb;
        if (nb >= 16) return h;
        const std::string v = tmp();
        add(v + " = shufflevector <" + std::to_string(nb) + " x i8> " + h + ", <" +
            std::to_string(nb) + " x i8> zeroinitializer, " + widenMaskStr(nb));
        return v;
    }
    fail("MMX/SSE 的操作数形态不支持");
    return "zeroinitializer";
}

void FnEmit::vecStore(const Operand& op, const std::string& v, int bytes) {
    if (op.kind == OpKind::Xmm) {
        if (op.vec >= 8) {
            fail("xmm 下标越界");
            return;
        }
        add("store <16 x i8> " + v + ", ptr " +
            cpuGep(x87l::kXmm + x87l::kXmmStride * static_cast<std::uint64_t>(op.vec)) +
            ", align 1");
        return;
    }
    if (op.kind == OpKind::Reg && regSpec(op.reg).cls == RegClass::X87) {
        const unsigned idx = static_cast<unsigned>(regSpec(op.reg).index);
        const std::string h = tmp();
        add(h + " = shufflevector <16 x i8> " + v + ", <16 x i8> zeroinitializer, " +
            lowMaskStr(8));
        add("store <8 x i8> " + h + ", ptr " + cpuGep(x87l::kRegs + 8ull * idx) + ", align 1");
        return;
    }
    if (op.kind == OpKind::Mem) {
        // ★ 与 `pkStore` 同口径：**不**发 `lift_note_write`（`exec_fpu.cpp:568-572` 直接 wr8）
        const std::string p = memPtrFor(eaOf(op.mem));
        if (bytes >= 16) {
            add("store <16 x i8> " + v + ", ptr " + p + ", align 1");
            return;
        }
        const std::string h = tmp();
        add(h + " = shufflevector <16 x i8> " + v + ", <16 x i8> zeroinitializer, " +
            lowMaskStr(bytes));
        add("store <" + std::to_string(bytes) + " x i8> " + h + ", ptr " + p + ", align 1");
        return;
    }
    fail("MMX/SSE 的目标操作数形态不支持");
}

// ---------------------------------------------------------------------------------------
// MMX / SSE 整数打包（`Op::Packed`）：逐条照 `exec_fpu.cpp:844-920` 的 `execPacked`
// ---------------------------------------------------------------------------------------
void FnEmit::emitPacked(const Insn& in) {
    ++stats_->packed_sites;
    const Operand& dst = in.op0();
    const Operand& src = in.op1();
    // ---- EMMS（`:848-851`）：tag word = 全 empty，TOP 不动 ----
    if (in.pk == PkOp::Ems) {
        add("store i16 -1, ptr " + cpuGep(x87l::kTag) + ", align 2");
        return;
    }
    auto vecTy = [](const char* base, int n) {
        return std::string("<") + std::to_string(n) + " x " + base + ">";
    };
    // ★ LLVM 的**二元运算**只写一次类型（`add <8 x i16> %a, %b`）⇒ 常量操作数要**不带类型前缀**
    //   （`<i16 0, i16 0, …>`）；`select` / `call` 的每个操作数各自带类型。
    //   ⇒ `splat()` 给"值"（不带类型），`tv()` 给"类型 + 值"。两者一起用，⛔ 不要混。
    auto splat = [&](const char* base, int n, const std::string& val) {
        std::string s = "<";
        for (int i = 0; i < n; ++i) {
            if (i) s += ", ";
            s += std::string(base) + " " + val;
        }
        return s + ">";
    };
    auto tv = [&](const char* base, int n, const std::string& x) {
        return vecTy(base, n) + " " + x;
    };
    auto asLanes = [&](const std::string& v, const char* base, int n) {
        // ⛔ `bitcast <16 x i8> ... to <16 x i8>` 是非法 IR（实测 clang：invalid cast opcode）
        if (std::string(base) == "i8" && n == 16) return v;
        const std::string r = tmp();
        add(r + " = bitcast <16 x i8> " + v + " to " + vecTy(base, n));
        return r;
    };
    auto toVec = [&](const std::string& v, const char* base, int n) {
        if (std::string(base) == "i8" && n == 16) return v;  // 已经是 <16 x i8>
        const std::string r = tmp();
        add(r + " = bitcast " + vecTy(base, n) + " " + v + " to <16 x i8>");
        return r;
    };
    auto vop = [&](const char* opn, const char* base, int n, const std::string& a,
                   const std::string& b) {
        const std::string r = tmp();
        add(r + " = " + opn + " " + vecTy(base, n) + " " + a + ", " + b);
        return r;
    };
    auto ext = [&](const std::string& v, const char* from, int nf, const char* to, int nt,
                   bool sign) {
        const std::string r = tmp();
        add(r + " = " + (sign ? "sext" : "zext") + " " + vecTy(from, nf) + " " + v + " to " +
            vecTy(to, nt));
        return r;
    };
    auto truncv = [&](const std::string& v, const char* from, int nf, const char* to, int nt) {
        const std::string r = tmp();
        add(r + " = trunc " + vecTy(from, nf) + " " + v + " to " + vecTy(to, nt));
        return r;
    };
    auto icmpv = [&](const char* pred, const char* base, int n, const std::string& a,
                     const std::string& b) {
        const std::string r = tmp();
        add(r + " = icmp " + std::string(pred) + " " + vecTy(base, n) + " " + a + ", " + b);
        return r;
    };
    // 操作数 → "类型 + 值"（`splat()` 已经带类型 ⇒ 原样用；SSA 值 ⇒ 补类型）
    auto selv = [&](const std::string& c, const char* base, int n, const std::string& t,
                    const std::string& f) {
        const std::string r = tmp();
        add(r + " = select " + vecTy("i1", n) + " " + c + ", " + tv(base, n, t) + ", " +
            tv(base, n, f));
        return r;
    };
    // 标量 i1 条件 + 向量操作数（LLVM 允许；shift 的 `over` 是标量比较）
    auto selvS = [&](const std::string& c, const char* base, int n, const std::string& t,
                     const std::string& f) {
        const std::string r = tmp();
        add(r + " = select i1 " + c + ", " + tv(base, n, t) + ", " + tv(base, n, f));
        return r;
    };
    auto shuffle = [&](const std::string& a, const std::string& b, const char* base, int n,
                       const std::vector<int>& idx) {
        std::string mk = "<" + std::to_string(idx.size()) + " x i32> <";
        for (std::size_t i = 0; i < idx.size(); ++i) {
            if (i) mk += ", ";
            mk += "i32 " + std::to_string(idx[i]);
        }
        mk += ">";
        const std::string r = tmp();
        add(r + " = shufflevector " + vecTy(base, n) + " " + a + ", " + vecTy(base, n) + " " + b +
            ", " + mk);
        return r;
    };
    auto clampTo = [&](bool sgn, const char* base, int n, const std::string& v,
                       const std::string& lo, const std::string& hi) {
        // ★ 顺序很重要：无符号形态要**先**把负数夹到 0（`pkClamp` 的 v<lo 分支），再夹上界；
        //   反过来用无符号比较会把 -1 当成 65535 夹成 255（实测：`packuswb` 的差分当场红）。
        std::string r0 = v;
        if (!sgn) {
            const std::string c0 = icmpv("slt", base, n, v, splat(base, n, "0"));
            r0 = selv(c0, base, n, splat(base, n, "0"), v);
        }
        const std::string c1 = icmpv(sgn ? "sgt" : "ugt", base, n, r0, splat(base, n, hi));
        const std::string r1 = selv(c1, base, n, splat(base, n, hi), r0);
        if (!sgn) return r1;
        const std::string c2 = icmpv("slt", base, n, r1, splat(base, n, "0"));
        const std::string c3 = icmpv("slt", base, n, r1, splat(base, n, lo));
        return selv(c3, base, n, splat(base, n, lo), r1);
    };
    auto sat = [&](const char* opn, const char* base, int n, const std::string& a,
                   const std::string& b) {
        const std::string r = tmp();
        add(r + " = call " + vecTy(base, n) + " @llvm." + opn + ".v" + std::to_string(n) +
            base + "(" + vecTy(base, n) + " " + a + ", " + vecTy(base, n) + " " + b + ")");
        return r;
    };

    // ---- movd / movq（`:852-878`）----
    if (in.pk == PkOp::Movd || in.pk == PkOp::Movq) {
        const bool q = (in.pk == PkOp::Movq);
        const int width = q ? 8 : 4;
        int nb = 0;
        const std::string v = vecLoad(src, &nb);
        (void)nb;
        std::string z = v;
        if (width < 16) {
            std::vector<int> mk;
            for (int i = 0; i < 16; ++i) mk.push_back(i < width ? i : 16);
            z = shuffle(v, "zeroinitializer", "i8", 16, mk);
        }
        if (dst.kind == OpKind::Xmm ||
            (dst.kind == OpKind::Reg && regSpec(dst.reg).cls == RegClass::X87)) {
            vecStore(dst, z, 16);
            return;
        }
        if (dst.kind == OpKind::Reg) {
            if (q) {
                fail("32 位 guest 里没有 movq 到通用寄存器");
                return;
            }
            const std::string l = tmp();
            add(l + " = bitcast <16 x i8> " + z + " to <4 x i32>");
            const std::string e = tmp();
            add(e + " = extractelement <4 x i32> " + l + ", i32 0");
            writeRegValue(dst.reg, e);
            return;
        }
        if (dst.kind == OpKind::Mem) {
            vecStore(dst, z, width);
            return;
        }
        fail("movd/movq 的目标操作数形态不支持");
        return;
    }

    if (!(dst.kind == OpKind::Xmm ||
          (dst.kind == OpKind::Reg && regSpec(dst.reg).cls == RegClass::X87))) {
        fail("MMX/SSE 的打包指令要求目标是 mm / xmm 寄存器");
        return;
    }
    int an = 0;
    const std::string a = vecLoad(dst, &an);
    const int bytes = (an == 16) ? 16 : 8;

    // ---- 移位族（`:888-908`）----
    if (in.pk >= PkOp::Psllw && in.pk <= PkOp::Psrad) {
        const bool arith = (in.pk == PkOp::Psraw || in.pk == PkOp::Psrad);
        const bool left = (in.pk == PkOp::Psllw || in.pk == PkOp::Pslld || in.pk == PkOp::Psllq);
        const bool qw = (in.pk == PkOp::Psllq || in.pk == PkOp::Psrlq);
        const bool dw = (in.pk == PkOp::Pslld || in.pk == PkOp::Psrld || in.pk == PkOp::Psrad);
        const char* base = qw ? "i64" : (dw ? "i32" : "i16");
        const int n = qw ? 2 : (dw ? 4 : 8);
        const unsigned maxsh = qw ? 63u : (dw ? 31u : 15u);
        std::string c32;  // i32 计数
        if (in.op1().kind == OpKind::Imm) {
            c32 = i32c(static_cast<std::uint32_t>(in.op1().imm) & 0xFFu);
        } else {
            int bn = 0;
            const std::string bv = vecLoad(src, &bn);
            const std::string b64 = asLanes(bv, "i64", 2);
            const std::string lo64 = tmp();
            add(lo64 + " = extractelement <2 x i64> " + b64 + ", i32 0");
            const std::string big = tmp();
            add(big + " = icmp ugt i64 " + lo64 + ", 4294967295");
            const std::string t = tmp();
            add(t + " = trunc i64 " + lo64 + " to i32");
            c32 = select1(big, "i32", "-1", t);
        }
        const std::string over = bin("icmp ugt", "i32", c32, i32c(maxsh));
        const std::string sh32 = select1(over, "i32", i32c(maxsh), c32);
        // 位移量：按 lane 类型的 splat（i16/i32/i64 —— LLVM 要求向量位移量是同型向量）
        std::string shexpr;
        if (qw) {
            const std::string z = tmp();
            add(z + " = zext i32 " + sh32 + " to i64");
            const std::string o = tmp();
            add(o + " = insertelement <2 x i64> poison, i64 " + z + ", i32 0");
            shexpr = tmp();
            add(shexpr + " = shufflevector <2 x i64> " + o + ", <2 x i64> poison, " +
                "<2 x i32> <i32 0, i32 0>");
        } else if (dw) {
            const std::string o = tmp();
            add(o + " = insertelement <4 x i32> poison, i32 " + sh32 + ", i32 0");
            shexpr = tmp();
            add(shexpr + " = shufflevector <4 x i32> " + o + ", <4 x i32> poison, " +
                "<4 x i32> <i32 0, i32 0, i32 0, i32 0>");
        } else {
            const std::string t16 = tmp();
            add(t16 + " = trunc i32 " + sh32 + " to i16");
            const std::string o = tmp();
            add(o + " = insertelement <8 x i16> poison, i16 " + t16 + ", i32 0");
            shexpr = tmp();
            add(shexpr + " = shufflevector <8 x i16> " + o + ", <8 x i16> poison, " +
                "<8 x i32> <i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0, i32 0>");
        }
        const std::string av = asLanes(a, base, n);
        std::string r;
        if (arith) {
            // 算术右移：计数 > 位宽-1 ⇒ 整 lane 填符号位 = ashr(x, max)（`:655-663`）
            const std::string ra = tmp();
            add(ra + " = ashr " + vecTy(base, n) + " " + av + ", " + shexpr);
            r = ra;
        } else if (left) {
            const std::string sl = tmp();
            add(sl + " = shl " + vecTy(base, n) + " " + av + ", " + shexpr);
            r = selvS(over, base, n, splat(base, n, "0"), sl);
        } else {
            const std::string sl = tmp();
            add(sl + " = lshr " + vecTy(base, n) + " " + av + ", " + shexpr);
            r = selvS(over, base, n, splat(base, n, "0"), sl);
        }
        vecStore(dst, toVec(r, base, n), bytes);
        return;
    }

    // ---- 二元族：源宽度必须是 64 / 128（`punpckl*` 允许 m32 —— `:914`）----
    int bn = 0;
    const std::string b = vecLoad(src, &bn);
    {
        const bool low_unpack = (in.pk == PkOp::Punpcklbw || in.pk == PkOp::Punpcklwd ||
                                 in.pk == PkOp::Punpckldq);
        if (bn != 8 && bn != 16 && !(low_unpack && bn == 4)) {
            fail("MMX/SSE 的源宽度必须是 64 / 128 位（punpckl* 允许 m32）");
            return;
        }
    }
    // punpck：按 `pkUnpack`（`:577-589`）的 lane 置换
    auto punpck = [&](int elem, bool high) -> std::string {
        const char* base = elem == 1 ? "i8" : (elem == 2 ? "i16" : (elem == 4 ? "i32" : "i64"));
        const int n = 16 / elem;
        const int half = bytes / 2;
        const int off = (high ? half : 0) / elem;
        const int elems = half / elem;
        std::vector<int> mk(static_cast<std::size_t>(n), 0);
        for (int j = 0; j < 2 * elems; ++j) {
            const int src_l = off + j / 2;
            mk[static_cast<std::size_t>(j)] = (j % 2 == 0) ? src_l : (n + src_l);
        }
        return shuffle(asLanes(a, base, n), asLanes(b, base, n), base, n, mk);
    };
    // pack：按 `pkPack`（`:591-618`）的饱和 + 拼接
    auto packBytes = [&](bool sgn) -> std::string {
        const std::string av16 = asLanes(a, "i16", 8);
        const std::string bv16 = asLanes(b, "i16", 8);
        // ★ `packsswb` 钳到 [-128,127]；`packuswb` 钳到 **[0,255]**（`exec_fpu.cpp:596-607`）
        const std::string lo = sgn ? "-128" : "0";
        const std::string hi = sgn ? "127" : "255";
        const std::string pa = clampTo(sgn, "i16", 8, av16, lo, hi);
        const std::string pb = clampTo(sgn, "i16", 8, bv16, lo, hi);
        const std::string ta = truncv(pa, "i16", 8, "i8", 8);
        const std::string tb = truncv(pb, "i16", 8, "i8", 8);
        const int nn = bytes / 2;
        std::vector<int> mk(16, 0);
        for (int j = 0; j < 2 * nn; ++j) mk[static_cast<std::size_t>(j)] = (j < nn) ? j : (8 + j - nn);
        return shuffle(ta, tb, "i8", 8, mk);
    };
    auto packWords = [&]() -> std::string {
        const std::string av32 = asLanes(a, "i32", 4);
        const std::string bv32 = asLanes(b, "i32", 4);
        const std::string pa = clampTo(true, "i32", 4, av32, "-32768", "32767");
        const std::string pb = clampTo(true, "i32", 4, bv32, "-32768", "32767");
        const std::string ta = truncv(pa, "i32", 4, "i16", 4);
        const std::string tb = truncv(pb, "i32", 4, "i16", 4);
        const int nn = bytes / 4;
        std::vector<int> mk(8, 0);
        for (int j = 0; j < 2 * nn; ++j) mk[static_cast<std::size_t>(j)] = (j < nn) ? j : (4 + j - nn);
        return toVec(shuffle(ta, tb, "i16", 4, mk), "i16", 8);
    };

    std::string res;  // <16 x i8>
    switch (in.pk) {
        case PkOp::Pand: res = vop("and", "i8", 16, a, b); break;
        case PkOp::Pandn: {
            const std::string na = vop("xor", "i8", 16, a, splat("i8", 16, "-1"));
            res = vop("and", "i8", 16, na, b);
            break;
        }
        case PkOp::Por: res = vop("or", "i8", 16, a, b); break;
        case PkOp::Pxor: res = vop("xor", "i8", 16, a, b); break;
        case PkOp::PaddB: res = vop("add", "i8", 16, a, b); break;
        case PkOp::PaddW:
            res = toVec(vop("add", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)), "i16", 8);
            break;
        case PkOp::PaddD:
            res = toVec(vop("add", "i32", 4, asLanes(a, "i32", 4), asLanes(b, "i32", 4)), "i32", 4);
            break;
        case PkOp::PaddQ:
            res = toVec(vop("add", "i64", 2, asLanes(a, "i64", 2), asLanes(b, "i64", 2)), "i64", 2);
            break;
        case PkOp::PsubB: res = vop("sub", "i8", 16, a, b); break;
        case PkOp::PsubW:
            res = toVec(vop("sub", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)), "i16", 8);
            break;
        case PkOp::PsubD:
            res = toVec(vop("sub", "i32", 4, asLanes(a, "i32", 4), asLanes(b, "i32", 4)), "i32", 4);
            break;
        case PkOp::PsubQ:
            res = toVec(vop("sub", "i64", 2, asLanes(a, "i64", 2), asLanes(b, "i64", 2)), "i64", 2);
            break;
        case PkOp::PaddSb: res = sat("sadd.sat", "i8", 16, a, b); break;
        case PkOp::PaddSw:
            res = toVec(sat("sadd.sat", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)),
                        "i16", 8);
            break;
        case PkOp::PaddUsb: res = sat("uadd.sat", "i8", 16, a, b); break;
        case PkOp::PaddUsw:
            res = toVec(sat("uadd.sat", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)),
                        "i16", 8);
            break;
        case PkOp::PsubSb: res = sat("ssub.sat", "i8", 16, a, b); break;
        case PkOp::PsubSw:
            res = toVec(sat("ssub.sat", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)),
                        "i16", 8);
            break;
        case PkOp::PsubUsb: res = sat("usub.sat", "i8", 16, a, b); break;
        case PkOp::PsubUsw:
            res = toVec(sat("usub.sat", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)),
                        "i16", 8);
            break;
        case PkOp::PcmpeqB: {
            const std::string c = icmpv("eq", "i8", 16, a, b);
            const std::string s = tmp();
            add(s + " = sext <16 x i1> " + c + " to <16 x i8>");
            res = s;
            break;
        }
        case PkOp::PcmpeqW: {
            const std::string c = icmpv("eq", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8));
            const std::string s = tmp();
            add(s + " = sext <8 x i1> " + c + " to <8 x i16>");
            res = toVec(s, "i16", 8);
            break;
        }
        case PkOp::PcmpeqD: {
            const std::string c = icmpv("eq", "i32", 4, asLanes(a, "i32", 4), asLanes(b, "i32", 4));
            const std::string s = tmp();
            add(s + " = sext <4 x i1> " + c + " to <4 x i32>");
            res = toVec(s, "i32", 4);
            break;
        }
        case PkOp::PcmpgtB: {
            const std::string c = icmpv("sgt", "i8", 16, a, b);
            const std::string s = tmp();
            add(s + " = sext <16 x i1> " + c + " to <16 x i8>");
            res = s;
            break;
        }
        case PkOp::PcmpgtW: {
            const std::string c = icmpv("sgt", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8));
            const std::string s = tmp();
            add(s + " = sext <8 x i1> " + c + " to <8 x i16>");
            res = toVec(s, "i16", 8);
            break;
        }
        case PkOp::PcmpgtD: {
            const std::string c = icmpv("sgt", "i32", 4, asLanes(a, "i32", 4), asLanes(b, "i32", 4));
            const std::string s = tmp();
            add(s + " = sext <4 x i1> " + c + " to <4 x i32>");
            res = toVec(s, "i32", 4);
            break;
        }
        case PkOp::Punpcklbw: res = punpck(1, false); break;
        case PkOp::Punpcklwd: res = toVec(punpck(2, false), "i16", 8); break;
        case PkOp::Punpckldq: res = toVec(punpck(4, false), "i32", 4); break;
        case PkOp::Punpcklqdq: res = toVec(punpck(8, false), "i64", 2); break;
        case PkOp::Punpckhbw: res = punpck(1, true); break;
        case PkOp::Punpckhwd: res = toVec(punpck(2, true), "i16", 8); break;
        case PkOp::Punpckhdq: res = toVec(punpck(4, true), "i32", 4); break;
        case PkOp::Punpckhqdq: res = toVec(punpck(8, true), "i64", 2); break;
        case PkOp::Packsswb: res = packBytes(true); break;
        case PkOp::Packuswb: res = packBytes(false); break;
        case PkOp::Packssdw: res = packWords(); break;
        case PkOp::Pmullw:
            res = toVec(vop("mul", "i16", 8, asLanes(a, "i16", 8), asLanes(b, "i16", 8)), "i16", 8);
            break;
        case PkOp::Pmulhw: {
            const std::string x = ext(asLanes(a, "i16", 8), "i16", 8, "i32", 8, true);
            const std::string y = ext(asLanes(b, "i16", 8), "i16", 8, "i32", 8, true);
            const std::string p = vop("mul", "i32", 8, x, y);
            const std::string sh = tmp();
            add(sh + " = lshr <8 x i32> " + p + ", " + splat("i32", 8, "16"));
            res = toVec(truncv(sh, "i32", 8, "i16", 8), "i16", 8);
            break;
        }
        case PkOp::Pmulhuw: {
            const std::string x = ext(asLanes(a, "i16", 8), "i16", 8, "i32", 8, false);
            const std::string y = ext(asLanes(b, "i16", 8), "i16", 8, "i32", 8, false);
            const std::string p = vop("mul", "i32", 8, x, y);
            const std::string sh = tmp();
            add(sh + " = lshr <8 x i32> " + p + ", " + splat("i32", 8, "16"));
            res = toVec(truncv(sh, "i32", 8, "i16", 8), "i16", 8);
            break;
        }
        default: break;
    }
    if (res.empty()) {
        fail(std::string("MMX/SSE 打包族没有内联实现：pk=") +
             std::to_string(static_cast<int>(in.pk)));
        return;
    }
    vecStore(dst, res, bytes);
}

// `Op::Sse`：本语料只有 movdqa / movsd（`exec_fpu.cpp:1020-1259`）
void FnEmit::emitSse(const Insn& in) {
    ++stats_->sse_sites;
    const Operand& dst = in.op0();
    const Operand& src = in.op1();
    if (in.sse == SseOp::Movdqa || in.sse == SseOp::Movaps || in.sse == SseOp::Movups ||
        in.sse == SseOp::Movdqu) {
        const bool aligned = (in.sse == SseOp::Movdqa || in.sse == SseOp::Movaps);
        const Operand& mop = (dst.kind == OpKind::Mem) ? dst : src;
        if (aligned && mop.kind == OpKind::Mem) {
            // `exec_fpu.cpp:1010-1016`：m128 未按 16 字节对齐 ⇒ **响亮停**（⛔ 不按 MOVUPS 处理）
            const std::string ea = eaOf(mop.mem);
            const std::string bad = bin("icmp ne", "i32", andv(ea, 15u), "0");
            const std::string id = std::to_string(label_id_++);
            const std::string ok = "sse_ok_" + id;
            flushRegs();  // ★ 同上
            add("br i1 " + bad + ", label %sse_bad_" + id + ", label %" + ok);
            put("sse_bad_" + id + ":\n");
            add("call void @lift_stop(ptr %cpu, i32 " + i32c(in.ea) + ", i32 " +
                std::to_string(::kLiftStopSseAlign) + ")");
            stats_->used_lift_stop = true;
            add("ret i32 " + i32c(::kLiftHalt));
            put(ok + ":\n");
            invalidateRegs();  // ★ 合并点之后（同上）
        }
        if (dst.kind == OpKind::Xmm) {
            int nb = 0;
            const std::string v = vecLoad(src, &nb);
            if (nb != 16) {
                fail("SSE 搬移的源必须是 xmm / m128");
                return;
            }
            vecStore(dst, v, 16);
            return;
        }
        if (dst.kind == OpKind::Mem && dst.mem.size == 16 && src.kind == OpKind::Xmm) {
            int nb = 0;
            const std::string v = vecLoad(src, &nb);
            vecStore(dst, v, 16);
            return;
        }
        fail("SSE 搬移的操作数形态不支持");
        return;
    }
    if (in.sse == SseOp::Movsd) {
        // `exec_fpu.cpp:1092-1125`：reg-reg 只写低 64 位（高 64 位原样）；m64 源 ⇒ 高 64 位清 0；
        //   m64 目标 ⇒ 只写 8 字节。⛔ 不碰 EFLAGS、⛔ 不发 `lift_note_write`（解释器也没发）。
        if (dst.kind == OpKind::Xmm) {
            if (dst.vec >= 8) {
                fail("xmm 下标越界");
                return;
            }
            const std::string dp =
                cpuGep(x87l::kXmm + x87l::kXmmStride * static_cast<std::uint64_t>(dst.vec));
            if (src.kind == OpKind::Xmm) {
                if (src.vec >= 8) {
                    fail("xmm 下标越界");
                    return;
                }
                const std::string v = tmp();
                add(v + " = load i64, ptr " +
                    cpuGep(x87l::kXmm + x87l::kXmmStride * static_cast<std::uint64_t>(src.vec)) +
                    ", align 1");
                add("store i64 " + v + ", ptr " + dp + ", align 1");
                return;
            }
            if (src.kind == OpKind::Mem && src.mem.size == 8) {
                const std::string v = tmp();
                add(v + " = load i64, ptr " + memPtrFor(eaOf(src.mem)) + ", align 1");
                add("store i64 " + v + ", ptr " + dp + ", align 1");
                add("store i64 0, ptr " +
                    cpuGep(x87l::kXmm + 8u +
                           x87l::kXmmStride * static_cast<std::uint64_t>(dst.vec)) +
                    ", align 1");
                return;
            }
            fail("movsd 的源操作数形态不支持");
            return;
        }
        if (dst.kind == OpKind::Mem && dst.mem.size == 8 && src.kind == OpKind::Xmm) {
            if (src.vec >= 8) {
                fail("xmm 下标越界");
                return;
            }
            const std::string v = tmp();
            add(v + " = load i64, ptr " +
                cpuGep(x87l::kXmm + x87l::kXmmStride * static_cast<std::uint64_t>(src.vec)) +
                ", align 1");
            add("store i64 " + v + ", ptr " + memPtrFor(eaOf(dst.mem)) + ", align 1");
            return;
        }
        fail("movsd 的目标操作数形态不支持");
        return;
    }
    fail(std::string("SSE 族没有内联实现：sse=") + std::to_string(static_cast<int>(in.sse)));
}

void FnEmit::emitInsn(const Insn& in) {
    // ★ 检查点 2（D49）：x87 整族在这里分派（是否真的内联由 `unsupportedReasonOf` 先判过）。
    if (opIsX87(in.op)) {
        emitX87(in);
        return;
    }
    switch (in.op) {
        // ★ 2026-11-15（本轮：`lift_one` 调用点 → 0）：停机类与新增的 inline 族
        case Op::Hlt: emitStop(in, ::kLiftStopHlt); return;
        case Op::Int3: emitStop(in, ::kLiftStopInt3); return;
        case Op::Int: emitStop(in, ::kLiftStopInt); return;
        case Op::Other: emitStop(in, ::kLiftStopOther); return;
        case Op::Invalid: emitStop(in, ::kLiftStopInvalid); return;
        case Op::Imul: emitImul(in); return;
        case Op::Div:
        case Op::Idiv: emitDivIdiv(in); return;
        case Op::Movs:
        case Op::Stos:
        case Op::Scas:
        case Op::Cmps: emitString(in); return;
        case Op::Shld:
        case Op::Shrd: emitShldShrd(in); return;
        case Op::Cmpxchg: emitCmpxchg(in); return;
        case Op::Cpuid: emitCpuid(in); return;
        case Op::Packed: emitPacked(in); return;
        case Op::Sse: emitSse(in); return;
        case Op::Nop:
        case Op::Pause: return;
        case Op::Mov: writeDest(in.op0(), readValue(in.op1())); return;
        case Op::Movzx: writeDest(in.op0(), readValue(in.op1())); return;
        case Op::Movsx: {
            const unsigned size = in.op1().size != 0 ? in.op1().size : 4u;
            std::string v = readValue(in.op1());
            if (size == 1) {
                const std::string t = tmp();
                add(t + " = trunc i32 " + v + " to i8");
                const std::string s = tmp();
                add(s + " = sext i8 " + t + " to i32");
                v = s;
            } else if (size == 2) {
                const std::string t = tmp();
                add(t + " = trunc i32 " + v + " to i16");
                const std::string s = tmp();
                add(s + " = sext i16 " + t + " to i32");
                v = s;
            }
            writeDest(in.op0(), v);
            return;
        }
        case Op::Lea: writeDest(in.op0(), eaOf(in.op1().mem)); return;
        case Op::Xchg: {
            const std::string a = readValue(in.op0());
            const std::string b = readValue(in.op1());
            writeDest(in.op0(), b);
            writeDest(in.op1(), a);
            return;
        }
        case Op::Push: pushValue(readValue(in.op0())); return;
        case Op::Pop: {
            const std::string v = popValue();
            writeDest(in.op0(), v);
            return;
        }
        case Op::Pushad: {
            const std::string esp0 = loadReg(4);
            for (int i = 0; i < 8; ++i) pushValue(i == 4 ? esp0 : loadReg(i));
            return;
        }
        case Op::Popad: {
            for (int i = 7; i >= 0; --i) {
                const std::string v = popValue();
                if (i != 4) storeReg(i, v);
            }
            return;
        }
        case Op::Pushfd:
            // ★ 第 2 行：整字**读** ⇒ 先把六位并进 base（S3-a 里恒为 no-op，S3-c 起才必需）。
            materialize(kAllBits);
            pushValue(loadEflags());
            return;
        case Op::Popfd: {
            const std::string v = popValue();
            // X86::popFlags：eflags = pop32() | FLAG_IF ⇒ **整字覆写**（DF 也一起变）。
            storeEflags(bin("or", "i32", v, i32c(0x200u)));
            resetFlagsSlots();  // 六位现在全部来自刚写进去的 base 字
            return;
        }
        case Op::Xadd: {
            const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
            const std::string a = readValue(in.op0());
            const std::string b = readValue(in.op1());
            const std::string r = bin("add", "i32", a, b);
            const std::uint8_t w = flagsWriteMask(in);  // 第 7 行：W = ALL
            const std::uint8_t m = wrMaskAt(cur_index_);
            beginFlagWrite(w, m);
            deliverAddBits(m, a, b, r, size, /*with_carry=*/false);
            writeDest(in.op0(), r);
            writeDest(in.op1(), a);
            return;
        }
        case Op::Add:
        case Op::Or:
        case Op::And:
        case Op::Sub:
        case Op::Xor:
        case Op::Cmp:
        case Op::Test:
        case Op::Adc:
        case Op::Sbb: emitAlu(in); return;
        case Op::Inc:
        case Op::Dec: emitIncDec(in); return;
        case Op::Neg: {
            const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
            const std::string a = readValue(in.op0());
            const std::string r = bin("sub", "i32", "0", a);
            const std::uint8_t w = flagsWriteMask(in);  // 第 7 行：W = ALL
            const std::uint8_t m = wrMaskAt(cur_index_);
            beginFlagWrite(w, m);
            deliverSubBits(m, "0", a, r, size, /*with_borrow=*/false);
            writeDest(in.op0(), r);
            return;
        }
        case Op::Not: {
            writeDest(in.op0(), bin("xor", "i32", readValue(in.op0()), i32c(0xFFFFFFFFu)));
            return;
        }
        case Op::Shl:
        case Op::Shr:
        case Op::Sar: emitShift(in); return;
        // ★ 2026-11-15：循环移位（`rol/ror/rcr`）的**立即数**计数与寄存器计数都内联
        //   （`emitShift` 内部按 `op1.kind` 分流；`emitShiftReg` 负责运行期无条件写）。
        case Op::Rol:
        case Op::Ror:
        case Op::Rcr: emitShift(in); return;
        case Op::Mul: emitMul(in); return;
        case Op::Cdq: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x80000000u), "0");
            storeReg(2, select1(neg, "i32", "-1", "0"));
            return;
        }
        case Op::Cwd: {
            const std::string ax = readRegValue(Reg::AX);
            const std::string neg = bin("icmp ne", "i32", andv(ax, 0x8000u), "0");
            writeRegValue(Reg::DX, select1(neg, "i32", i32c(0xFFFFu), "0"));
            return;
        }
        case Op::Cwde: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x8000u), "0");
            const std::string hi = select1(neg, "i32", i32c(0xFFFF0000u), "0");
            storeReg(0, bin("or", "i32", hi, andv(eax, 0xFFFFu)));
            return;
        }
        case Op::Cbw: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x80u), "0");
            const std::string hi = select1(neg, "i32", i32c(0xFF00u), "0");
            writeRegValue(Reg::AX, bin("or", "i32", hi, andv(eax, 0xFFu)));
            return;
        }
        case Op::Bswap: {
            const Reg reg = in.op0().reg;
            const std::string v = loadReg(regSpec(reg).index);
            const std::string b0 = bin("shl", "i32", andv(v, 0xFFu), "24");
            const std::string b1 = bin("shl", "i32", andv(v, 0xFF00u), "8");
            const std::string b2 = bin("lshr", "i32", andv(v, 0xFF0000u), "8");
            const std::string b3 = bin("lshr", "i32", andv(v, 0xFF000000u), "24");
            writeRegValue(reg, orList({b0, b1, b2, b3}));
            return;
        }
        case Op::Bt:
        case Op::Bts:
        case Op::Btr:
        case Op::Btc: emitBitTest(in); return;
        case Op::Bsf:
        case Op::Bsr: emitBitScan(in); return;
        case Op::Sahf: {
            const std::string ah = andv(bin("lshr", "i32", loadReg(0), "8"), 0xFFu);
            // ★ 第 5 行：`sahf` 写 CF/PF/AF/ZF/SF（+ bit1=1），**只有 OF 要读旧值** ⇒ 先物化它。
            //   整字公式 `(eflags & ~0xD5) | (ah & 0xD5) | 2` 是**唯一允许**的整字覆写之一
            //   （另一个是 `popfd`）；⛔ 不许在别处手搓 flag 字。
            materialize(static_cast<std::uint8_t>(1u << kBitOf));
            const std::string eflags = loadEflags();
            storeEflags(bin("or", "i32",
                            bin("or", "i32", andv(eflags, ~0xD5u), andv(ah, 0xD5u)),
                            "2"));
            resetFlagsSlots();  // base 里的六位现在是权威值
            return;
        }
        case Op::Lahf: {
            // ★ 第 6 行：`lahf` 读 CF/PF/AF/ZF/SF（**不含 OF**）⇒ 先把这五位并进 base。
            materialize(static_cast<std::uint8_t>((1u << kBitCf) | (1u << kBitPf) |
                                                  (1u << kBitAf) | (1u << kBitZf) |
                                                  (1u << kBitSf)));
            const std::string v = bin("or", "i32", andv(loadEflags(), 0xD5u), "2");
            writeRegValue(Reg::AH, v);
            return;
        }
        // ★ 第 4 行：`cld/std` **只改 base 的 DF**，六位一位都不改 ⇒ 不抹 lazy 槽
        //   （⛔ 写成"整字覆盖"会把 lazy 槽抹掉 = 静默错）。所以这里直接 `storeReg(8, …)`，
        //   不走 `storeEflags`（那是整字覆写的名字）。
        case Op::Cld: storeReg(8, andv(loadEflags(), ~kDf)); return;
        case Op::Std: storeReg(8, bin("or", "i32", loadEflags(), i32c(kDf))); return;
        case Op::Setcc: {
            // ★ S3-b：`setcc` ⇒ `zext i1 cond to i32`（比 `select ... 1, 0` 少一条；clang 出 `cset`）。
            const std::string cond = condForReader(in.cc, /*kind=*/1);
            const std::string z = tmp();
            add(z + " = zext i1 " + cond + " to i32");
            writeDest(in.op0(), z);
            return;
        }
        case Op::Cmovcc: {
            const std::string cond = condForReader(in.cc, /*kind=*/2);
            const std::string cur = readRegValue(in.op0().reg);
            const std::string src = readValue(in.op1());
            writeDest(in.op0(), select1(cond, "i32", src, cur));
            return;
        }
        case Op::Leave: {
            const std::string ebp = loadReg(5);
            const std::string saved = loadAt(ebp);
            storeReg(5, saved);
            storeReg(4, bin("add", "i32", ebp, "4"));
            return;
        }
        default: emitTrap(in, "T1a 未实现的 op");
    }
}

// =====================================================================================
// x87（检查点 2 / D49）：把 x87 抬进 IR
// =====================================================================================
// ★ 语义真源 = `exec_fpu.cpp` 的 `execFpu`（解释器臂）—— 下面每一条都照抄它的**状态机**：
//   8 个 double 槽 / TOP / tag word / 状态字（C0..C3 + IE/ZE）/ 控制字（PC 无效、RC 只给 fist*/frndint）。
// ★ **算术是普通 LLVM 浮点指令**（`fadd double` / `fmul double` / `fptosi`…）—— 这是 D49 的
//   全部目的：x87 不再是"最难忠实翻译的一族"。⛔ 不调宿主 helper 做四则。
// ★ 状态全部在 `%cpu`（X86）里，偏移见 `runtime/x87_layout.h`（唯一一份常量，`lift_abi.cpp`
//   用 static_assert 钉住）。⛔ 不动 `LiftRegs` 的 ABI（它只有 8 GPR + eflags）。
// ★ 只有"超越函数 + fprem/fscale/fxtract/fxam"走宿主 helper `@lift_x87_sem(ptr %cpu, i32 <Op>)`：
//   本语料合计 <100 次/300 帧；helper 的实现就是解释器的 `execFpu`（同一份语义，⛔ 不重写公式）。
// ★ `fldenv`/`fnstenv`/`fnsave`/`frstor` 与 `fld/fst(p) tbyte`（10 字节内存格式）仍走 `lift_one`：
//   见 `unsupportedReasonOf` 里的逐条理由。
namespace x87l = lift::x87layout;

// `ST(i)` 的下标（与 `exec_fpu.cpp` 的 `stIndex` 同一份判据：非 Reg ⇒ 缺省值）。
int x87StIndex(const Operand& op, int def) {
    return (op.kind == OpKind::Reg) ? static_cast<int>(regSpec(op.reg).index) : def;
}

std::string FnEmit::cpuGep(std::uint64_t off) {
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %cpu, i64 " + std::to_string(static_cast<long long>(off)));
    return p;
}

std::string FnEmit::x87LoadU16(std::uint64_t off) {
    std::string v = tmp();
    add(v + " = load i16, ptr " + cpuGep(off) + ", align 2");
    return v;
}

void FnEmit::x87StoreU16(std::uint64_t off, const std::string& v) {
    add("store i16 " + v + ", ptr " + cpuGep(off) + ", align 2");
}

std::string FnEmit::x87LoadTop() {
    std::string v = tmp();
    add(v + " = load i8, ptr " + cpuGep(x87l::kTop) + ", align 1");
    std::string z = tmp();
    add(z + " = zext i8 " + v + " to i32");
    return z;
}

void FnEmit::x87StoreTop(const std::string& v) {
    std::string t = tmp();
    add(t + " = trunc i32 " + v + " to i8");
    add("store i8 " + t + ", ptr " + cpuGep(x87l::kTop) + ", align 1");
}

// phys = (top + i) & 7（**每次现取 top**：x87 的 top 是内存里的状态，块内不做缓存 ——
//   缓存会引入"哪条路径改了 top"的支配关系问题，本轮的判据优先于几十条 IR 的节省）。
std::string FnEmit::x87Phys(int logical) {
    const std::string top = x87LoadTop();
    std::string s = top;
    if (logical != 0) s = bin("add", "i32", top, std::to_string(logical));
    return bin("and", "i32", s, "7");
}

std::string FnEmit::x87SlotPtr(int logical) {
    const std::string phys = x87Phys(logical);
    const std::string bytes = bin("shl", "i32", phys, "3");
    std::string off = tmp();
    add(off + " = zext i32 " + bytes + " to i64");
    std::string boff = tmp();
    add(boff + " = add i64 " + off + ", " + std::to_string(static_cast<long long>(x87l::kRegs)));
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %cpu, i64 " + boff);
    return p;
}

std::string FnEmit::x87LoadSt(int logical) {
    std::string v = tmp();
    add(v + " = load double, ptr " + x87SlotPtr(logical) + ", align 8");
    return v;
}

void FnEmit::x87StoreSt(int logical, const std::string& v) {
    add("store double " + v + ", ptr " + x87SlotPtr(logical) + ", align 8");
}

// i16：(tag >> (2·phys(logical))) & 3（= 该槽的 tag 位；3 = empty）
std::string FnEmit::x87TagBitsAt(int logical) {
    const std::string tag = x87LoadU16(x87l::kTag);
    const std::string sh = bin("mul", "i32", x87Phys(logical), "2");
    std::string sh16 = tmp();
    add(sh16 + " = trunc i32 " + sh + " to i16");
    std::string lsr = tmp();
    add(lsr + " = lshr i16 " + tag + ", " + sh16);
    std::string low = tmp();
    add(low + " = and i16 " + lsr + ", 3");
    return low;
}

// tag[phys] := (val == 0.0) ? 01 (zero) : 00 (valid)
//   `known_zero`：1 = 生成期已知是零、0 = 已知非零、-1 = 未知（现算一条 fcmp）。
void FnEmit::x87WriteTagSlot(int logical, int known_zero) {
    const std::string phys = x87Phys(logical);
    const std::string sh = bin("mul", "i32", phys, "2");
    std::string sh16 = tmp();
    add(sh16 + " = trunc i32 " + sh + " to i16");
    std::string bit;
    if (known_zero == 1) {
        bit = "1";
    } else if (known_zero == 0) {
        bit = "0";
    } else {
        const std::string v = x87LoadSt(logical);
        const std::string z = tmp();
        add(z + " = fcmp oeq double " + v + ", 0.0");
        bit = tmp();
        add(bit + " = select i1 " + z + ", i16 1, i16 0");
    }
    const std::string three = tmp();
    add(three + " = shl i16 3, " + sh16);
    const std::string clr = tmp();
    add(clr + " = xor i16 " + three + ", -1");
    const std::string tag = x87LoadU16(x87l::kTag);
    const std::string cleared = tmp();
    add(cleared + " = and i16 " + tag + ", " + clr);
    if (bit == "0") {
        x87StoreU16(x87l::kTag, cleared);
        return;
    }
    const std::string shifted = tmp();
    add(shifted + " = shl i16 " + bit + ", " + sh16);
    const std::string nv = tmp();
    add(nv + " = or i16 " + cleared + ", " + shifted);
    x87StoreU16(x87l::kTag, nv);
}

// tag[phys] := 11（empty）；`ffree` 用
void FnEmit::x87SetTagEmpty(int logical) {
    const std::string phys = x87Phys(logical);
    const std::string sh = bin("mul", "i32", phys, "2");
    std::string sh16 = tmp();
    add(sh16 + " = trunc i32 " + sh + " to i16");
    const std::string three = tmp();
    add(three + " = shl i16 3, " + sh16);
    const std::string tag = x87LoadU16(x87l::kTag);
    const std::string nv = tmp();
    add(nv + " = or i16 " + tag + ", " + three);
    x87StoreU16(x87l::kTag, nv);
}

// 状态字的 C0..C3（其余位原样）—— 照 `exec_fpu.cpp` 的 `setCC`。
// 每个参数是 `"true"` / `"false"` / 一个 i1 SSA 值名。
void FnEmit::x87SetCC(const std::string& c0, const std::string& c1, const std::string& c2,
                      const std::string& c3) {
    // 位值与位移：C0=0x0100 C1=0x0200 C2=0x0400 C3=0x4000
    struct Term {
        const std::string* v;
        int bit;
    };
    const Term terms[4] = {{&c0, 0x0100}, {&c1, 0x0200}, {&c2, 0x0400}, {&c3, 0x4000}};
    std::string acc;
    for (const Term& t : terms) {
        std::string piece;
        if (*t.v == "false") continue;
        if (*t.v == "true") {
            piece = std::to_string(t.bit);
            if (acc.empty()) { acc = piece; continue; }
            acc = bin("or", "i16", acc, piece);
            continue;
        }
        piece = tmp();
        add(piece + " = select i1 " + *t.v + ", i16 " + std::to_string(t.bit) + ", i16 0");
        acc = acc.empty() ? piece : bin("or", "i16", acc, piece);
    }
    const std::string st = x87LoadU16(x87l::kStatus);
    const std::string cleared = tmp();
    add(cleared + " = and i16 " + st + ", -18177");  // ~0x4700
    const std::string nv = acc.empty() ? cleared : bin("or", "i16", cleared, acc);
    x87StoreU16(x87l::kStatus, nv);
}

// status |= <bits>（i16 常量串）+ **ES**（任务 0 / D55）。
// ★ ES（0x0080）= 状态字里任一异常位（0x003F：IE/DE/ZE/OE/UE/PE）非零 —— 与解释器
//   `syncStatus` 的最后一步**同一条**判据（D49 之前的实现就是这样；真机的栈上溢 = IE + ES）。
//   ⇒ 所有把异常位并进状态字的站点都必须走这里，⛔ 不许在图里直接 `or i16 st, 1`：
//   那样生成代码的 ES 恒 0，而解释器的 ES 会被置 ⇒ 差分当场红（两臂逐位比较状态字）。
void FnEmit::x87OrStatus(const std::string& bits, const std::string& why) {
    // `bits` 是生成期常量（十进制串）；异常位 ⇒ 同时置 ES。
    std::uint16_t v = 0;
    try {
        v = static_cast<std::uint16_t>(std::stoul(bits));
    } catch (...) {
        fail("x87OrStatus 的 bits 不是数字常量：" + bits);
        return;
    }
    if ((v & 0x003Fu) != 0) v = static_cast<std::uint16_t>(v | 0x0080u);
    (void)why;
    const std::string st = x87LoadU16(x87l::kStatus);
    const std::string nv = tmp();
    add(nv + " = or i16 " + st + ", " + std::to_string(v));
    x87StoreU16(x87l::kStatus, nv);
}

// ★ 任务 0：上溢 / 下溢（判据与 `fpu_bridge.cpp` 的 `x87RaiseOverflowUnderflow` **同一份**）。
//   全部是 `double` 的位型判断（⛔ 不调宿主 helper、⛔ 不读 MXCSR）：
//     abs_r == 0x7FF0000000000000            ⇒ r 是 ±inf
//     abs_a/b >= 0x7FF0000000000000          ⇒ a/b 不是有限值（inf 或 NaN）
//     abs_r <  0x0010000000000000 && abs_r!=0 ⇒ r 是次正规
//   ⚠ 这里用整型位比较而不是 `fcmp`：NaN 的 abs 位型 > inf 的位型，所以 ">= 0x7FF0…" 一次
//   就把 inf 与 NaN 都归到"不有限"，与 `std::isfinite` 的用法一致（NaN 操作数时 r 是 NaN，
//   `abs_r == inf` 为假 ⇒ 不会误置 OE）。
void FnEmit::x87RaiseFloatFlags(const std::string& a, const std::string& b, const std::string& r,
                                const std::string& not_zero_den) {
    auto absBits = [&](const std::string& d) {
        const std::string bits = tmp();
        add(bits + " = bitcast double " + d + " to i64");
        const std::string v = tmp();
        add(v + " = and i64 " + bits + ", 9223372036854775807");
        return v;
    };
    const std::string abs_r = absBits(r);
    const std::string inf_r = tmp();
    add(inf_r + " = icmp eq i64 " + abs_r + ", 9218868437227405312");  // 0x7FF0000000000000
    const std::string abs_a = absBits(a);
    const std::string nonfinite_a = tmp();
    add(nonfinite_a + " = icmp uge i64 " + abs_a + ", 9218868437227405312");
    const std::string abs_b = absBits(b);
    const std::string nonfinite_b = tmp();
    add(nonfinite_b + " = icmp uge i64 " + abs_b + ", 9218868437227405312");
    const std::string both_finite = tmp();
    add(both_finite + " = and i1 " + not1(nonfinite_a) + ", " + not1(nonfinite_b));
    std::string oe = tmp();
    add(oe + " = and i1 " + inf_r + ", " + both_finite);
    if (!not_zero_den.empty()) oe = and1(oe, not_zero_den);
    const std::string subnormal = tmp();
    add(subnormal + " = icmp ult i64 " + abs_r + ", 4503599627370496");  // 0x0010000000000000
    const std::string nonzero = tmp();
    add(nonzero + " = icmp ne i64 " + abs_r + ", 0");
    const std::string ue = and1(subnormal, nonzero);
    // 状态字位：OE = 0x0008、UE = 0x0010；都带 ES（0x0080）⇒ 0x88 / 0x90（见 `x87OrStatus`）。
    const std::string oe_m = tmp();
    add(oe_m + " = select i1 " + oe + ", i16 136, i16 0");
    const std::string ue_m = tmp();
    add(ue_m + " = select i1 " + ue + ", i16 144, i16 0");
    const std::string mask = bin("or", "i16", oe_m, ue_m);
    const std::string st = x87LoadU16(x87l::kStatus);
    const std::string nv = tmp();
    add(nv + " = or i16 " + st + ", " + mask);
    x87StoreU16(x87l::kStatus, nv);
}

// 状态字的 TOP 镜像（`exec_fpu.cpp` 的 `syncStatus` 的最后一步；每次 top 变化都要做）。
void FnEmit::x87SyncTop() {
    const std::string top = x87LoadTop();
    const std::string st = x87LoadU16(x87l::kStatus);
    const std::string cleared = tmp();
    add(cleared + " = and i16 " + st + ", -14337");  // ~0x3800
    std::string t16 = tmp();
    add(t16 + " = trunc i32 " + top + " to i16");
    const std::string sh = tmp();
    add(sh + " = shl i16 " + t16 + ", 11");
    const std::string nv = tmp();
    add(nv + " = or i16 " + cleared + ", " + sh);
    x87StoreU16(x87l::kStatus, nv);
}

// 压栈：照 `exec_fpu.cpp` 的 `push()`（含栈上溢 = C1=1 + IE + 退回原状）。
void FnEmit::x87Push(const std::string& val, int known_zero) {
    const std::string top = x87LoadTop();
    const std::string next = bin("and", "i32", bin("sub", "i32", top, "1"), "7");
    // 判据 = `next` 槽的 tag 位是不是 11（empty）
    const std::string tag = x87LoadU16(x87l::kTag);
    const std::string sh = bin("mul", "i32", next, "2");
    std::string sh16 = tmp();
    add(sh16 + " = trunc i32 " + sh + " to i16");
    const std::string lsr = tmp();
    add(lsr + " = lshr i16 " + tag + ", " + sh16);
    const std::string low = tmp();
    add(low + " = and i16 " + lsr + ", 3");
    const std::string is_empty = tmp();
    add(is_empty + " = icmp eq i16 " + low + ", 3");
    const std::string id = std::to_string(label_id_++);
    add("br i1 " + is_empty + ", label %x87p_ok_" + id + ", label %x87p_ovf_" + id);
    // ---- 上溢臂（冷）：C1=1、IE、TOP 镜像、不改栈 ----
    put("x87p_ovf_" + id + ":\n");
    x87SetCC("false", "true", "false", "false");
    x87OrStatus("1", "栈上溢：IE（0x0001）+ ES（0x0080）");  // ⛔ 不是 bridge 的 16
    x87SyncTop();
    add("br label %x87p_cont_" + id);
    // ---- 正常臂：top=next、写槽、tag=valid/zero、TOP 镜像 ----
    put("x87p_ok_" + id + ":\n");
    x87StoreTop(next);
    {
        const std::string bytes = bin("shl", "i32", next, "3");
        std::string off = tmp();
        add(off + " = zext i32 " + bytes + " to i64");
        std::string boff = tmp();
        add(boff + " = add i64 " + off + ", " + std::to_string(static_cast<long long>(x87l::kRegs)));
        std::string p = tmp();
        add(p + " = getelementptr i8, ptr %cpu, i64 " + boff);
        add("store double " + val + ", ptr " + p + ", align 8");
    }
    // tag 位：用 `next`（此刻 top 已经是 next ⇒ `x87WriteTagSlot(0, …)` 等价；这里直接按 next 算，
    //   省一次 load —— 但为了与 `x87WriteTagSlot` 的唯一实现保持一致，仍走它（top 已就位）。
    x87WriteTagSlot(0, known_zero);
    x87SyncTop();
    add("br label %x87p_cont_" + id);
    put("x87p_cont_" + id + ":\n");
}

// 弹栈：照 `exec_fpu.cpp` 的 `pop()`（空栈不检查 —— 解释器也不检查，两臂一致）。
void FnEmit::x87Pop() {
    const std::string top = x87LoadTop();
    const std::string sh = bin("mul", "i32", top, "2");
    std::string sh16 = tmp();
    add(sh16 + " = trunc i32 " + sh + " to i16");
    const std::string three = tmp();
    add(three + " = shl i16 3, " + sh16);
    const std::string tag = x87LoadU16(x87l::kTag);
    const std::string nv = tmp();
    add(nv + " = or i16 " + tag + ", " + three);
    x87StoreU16(x87l::kTag, nv);
    const std::string next = bin("and", "i32", bin("add", "i32", top, "1"), "7");
    x87StoreTop(next);
    x87SyncTop();
}

// 已知常数的压栈（tag 位在生成期就定了 ⇒ 省掉 fcmp）
void FnEmit::x87StoreConst(double v) {
    // LLVM IR 的十六进制浮点常量：直接给**位型**（⛔ 不用十进制字面量，免得二次舍入）
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(bits));
    x87Push(buf, v == 0.0 ? 1 : 0);
}

std::string FnEmit::x87LoadMemF64(const MemOperand& m) {
    const std::string ea = eaOf(m);
    std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    std::string v = tmp();
    add(v + " = load double, ptr " + p + ", align 1");
    return v;
}

void FnEmit::x87StoreMemF64(const MemOperand& m, const std::string& v) {
    const std::string ea = eaOf(m);
    std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    add("store double " + v + ", ptr " + p + ", align 1");
}

std::string FnEmit::x87LoadMemF32(const MemOperand& m) {
    const std::string ea = eaOf(m);
    std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    std::string f = tmp();
    add(f + " = load float, ptr " + p + ", align 1");
    std::string d = tmp();
    add(d + " = fpext float " + f + " to double");  // m32 → double 是**精确**的
    return d;
}

void FnEmit::x87StoreMemF32(const MemOperand& m, const std::string& v) {
    const std::string ea = eaOf(m);
    std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    std::string f = tmp();
    add(f + " = fptrunc double " + v + " to float");  // 就近偶数（x87 的 fst m32 同样按 RC；本模型固定就近）
    add("store float " + f + ", ptr " + p + ", align 1");
}

// double → 整数：照 `fpu_bridge.cpp` 的 `toI32` / `toI64`
//   · 按控制字的 RC 四档取整（`llvm.roundeven` / `floor` / `ceil` / `trunc`）
//   · 越界 / NaN ⇒ 置 IE + 整数不定值（⛔ 不做裸 `fptosi`：越界是 poison）
// `size` = 2 / 4 → i32 路径（与解释器一致：m16 也走 i32 再截低 16 位）；8 → i64
std::string FnEmit::x87ToInt(const std::string& v, unsigned size) {
    const bool wide = (size == 8);
    const std::string cw = x87LoadU16(x87l::kControl);
    const std::string rc = tmp();
    add(rc + " = and i16 " + cw + ", 3072");  // RC 位 = bit10-11
    const std::string is_near = tmp();
    add(is_near + " = icmp eq i16 " + rc + ", 0");
    const std::string is_down = tmp();
    add(is_down + " = icmp eq i16 " + rc + ", 1024");
    const std::string is_up = tmp();
    add(is_up + " = icmp eq i16 " + rc + ", 2048");
    const std::string rn = tmp();
    // ★ `llvm.rint` = "按**当前舍入模式**取整"（宿主 MXCSR 是就近偶数，且我们从不改它）⇒ 就是
    //   x87 的 RC=最近偶数那一档，而且**不引 libm 调用**（`llvm.roundeven` 在 MSVC 目标上会落到
    //   链接期找不到的 `roundeven` —— 实测 lld-link: undefined symbol）。
    add(rn + " = call double @llvm.rint.f64(double " + v + ")");
    const std::string rd = tmp();
    add(rd + " = call double @llvm.floor.f64(double " + v + ")");
    const std::string ru = tmp();
    add(ru + " = call double @llvm.ceil.f64(double " + v + ")");
    const std::string rz = tmp();
    add(rz + " = call double @llvm.trunc.f64(double " + v + ")");
    const std::string s1 = select1(is_near, "double", rn, rz);
    const std::string s2 = select1(is_down, "double", rd, s1);
    const std::string r = select1(is_up, "double", ru, s2);
    // 区间判据：`r >= lo && r < hi`（NaN 两项都是 false ⇒ 落"越界"，与解释器同）
    const char* lo = wide ? "-9223372036854775808.0" : "-2147483648.0";
    const char* hi = wide ? "9223372036854775808.0" : "2147483648.0";
    const std::string ge = tmp();
    add(ge + " = fcmp oge double " + r + ", " + lo);
    const std::string lt = tmp();
    add(lt + " = fcmp olt double " + r + ", " + hi);
    const std::string in = tmp();
    add(in + " = and i1 " + ge + ", " + lt);
    const std::string safe = select1(in, "double", r, "0.0");
    const std::string conv = tmp();
    add(conv + " = fptosi double " + safe + " to " + (wide ? "i64" : "i32"));
    const std::string bad = not1(in);
    const std::string indef = wide ? "-9223372036854775808" : "-2147483648";
    const std::string res = select1(bad, wide ? "i64" : "i32", indef, conv);
    const std::string ie = tmp();
    // 0x0001 | 0x0080：IE + ES（任务 0；ES = 任一异常位，见 `x87OrStatus`）
    add(ie + " = select i1 " + bad + ", i16 129, i16 0");
    const std::string st = x87LoadU16(x87l::kStatus);
    const std::string nv = tmp();
    add(nv + " = or i16 " + st + ", " + ie);
    x87StoreU16(x87l::kStatus, nv);
    return res;
}

// `fist` / `fistp`：按**当前 RC** 取整写内存（`fisttp` 的"恒朝零"在 `emitX87` 里单独一条路径）
void FnEmit::x87FistStore(const Insn& in) {
    const std::string v = x87LoadSt(0);
    const unsigned size = in.op0().mem.size;
    const std::string iv = x87ToInt(v, size);
    // 写内存（只写 m.size 个字节，小端）
    const std::string ea = eaOf(in.op0().mem);
    std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    if (size == 8) {
        add("store i64 " + iv + ", ptr " + p + ", align 1");
    } else if (size == 4) {
        add("store i32 " + iv + ", ptr " + p + ", align 1");
    } else {
        const std::string t = tmp();
        add(t + " = trunc i32 " + iv + " to i16");
        add("store i16 " + t + ", ptr " + p + ", align 1");
    }
}

// 比较的 C0/C2/C3（无序 = C3=C2=C0=1；相等 ⇒ C3；小于 ⇒ C0；大于 ⇒ 全 0）
void FnEmit::emitX87Compare(const Insn& in, bool pop_after, bool quiet) {
    std::string b;
    if (in.op1().kind == OpKind::Reg) {
        b = x87LoadSt(x87StIndex(in.op1(), 1));
    } else if (in.op0().kind == OpKind::Reg) {
        b = x87LoadSt(x87StIndex(in.op0(), 1));
    } else if (in.op0().kind == OpKind::Mem) {
        b = (in.op0().mem.size == 4) ? x87LoadMemF32(in.op0().mem) : x87LoadMemF64(in.op0().mem);
    } else {
        b = x87LoadSt(1);  // 隐式源 = ST(1)
    }
    const std::string a = x87LoadSt(0);
    const std::string uno = tmp();
    add(uno + " = fcmp uno double " + a + ", " + b);
    const std::string eq = tmp();
    add(eq + " = fcmp oeq double " + a + ", " + b);
    const std::string lt = tmp();
    add(lt + " = fcmp olt double " + a + ", " + b);
    const std::string c0 = or1(uno, lt);
    const std::string c3 = or1(uno, eq);
    x87SetCC(c0, "false", uno, c3);
    // IE：`fcom*`（quiet=false）任何 NaN 都置；`fucom*` 只在 **SNaN** 置（与 `x87Compare` 同）
    if (!quiet) {
        const std::string ie = tmp();
        add(ie + " = select i1 " + uno + ", i16 129, i16 0");
        const std::string st = x87LoadU16(x87l::kStatus);
        const std::string nv = tmp();
        add(nv + " = or i16 " + st + ", " + ie);
        x87StoreU16(x87l::kStatus, nv);
    } else {
        // SNaN 判据（quiet 位 = double 尾数的 bit51）：只对 signaling NaN 置 IE
        auto snanOf = [&](const std::string& d) {
            const std::string bitsv = tmp();
            add(bitsv + " = bitcast double " + d + " to i64");
            const std::string absb = tmp();
            add(absb + " = and i64 " + bitsv + ", 9223372036854775807");
            const std::string isnan = tmp();
            add(isnan + " = icmp ugt i64 " + absb + ", 9218868437227405312");  // > 0x7FF0…
            const std::string q = tmp();
            add(q + " = and i64 " + bitsv + ", 2251799813685248");  // 0x0008_0000_0000_0000
            const std::string isq = tmp();
            add(isq + " = icmp ne i64 " + q + ", 0");
            const std::string sn = tmp();
            add(sn + " = and i1 " + isnan + ", " + not1(isq));
            return sn;
        };
        const std::string sn = or1(snanOf(a), snanOf(b));
        const std::string ie = tmp();
        add(ie + " = select i1 " + sn + ", i16 129, i16 0");
        const std::string st = x87LoadU16(x87l::kStatus);
        const std::string nv = tmp();
        add(nv + " = or i16 " + st + ", " + ie);
        x87StoreU16(x87l::kStatus, nv);
    }
    if (pop_after) x87Pop();
}

// `fcomi` / `fcomip` / `fucomi` / `fucomip`：结果进 **EFLAGS**
void FnEmit::emitX87CmpEflags(const Insn& in, bool pop_after, bool quiet) {
    const int i = (in.op1().kind == OpKind::Reg) ? x87StIndex(in.op1(), 0) : x87StIndex(in.op0(), 1);
    const std::string b = x87LoadSt(i);
    const std::string a = x87LoadSt(0);
    const std::string uno = tmp();
    add(uno + " = fcmp uno double " + a + ", " + b);
    const std::string eq = tmp();
    add(eq + " = fcmp oeq double " + a + ", " + b);
    const std::string lt = tmp();
    add(lt + " = fcmp olt double " + a + ", " + b);
    const std::string zf = or1(uno, eq);
    const std::string cf = or1(uno, lt);
    // `exec_fpu.cpp`：OF/SF/AF 清 0，ZF/PF/CF 按比较结果
    const std::uint8_t w = flagsWriteMask(in);
    const std::uint8_t m = wrMaskAt(cur_index_);
    beginFlagWrite(w, m);
    if (m & (1u << kBitZf)) deliverBit(kBitZf, zf);
    if (m & (1u << kBitPf)) deliverBit(kBitPf, uno);
    if (m & (1u << kBitCf)) deliverBit(kBitCf, cf);
    if (m & (1u << kBitOf)) deliverBit(kBitOf, "false");
    if (m & (1u << kBitSf)) deliverBit(kBitSf, "false");
    if (m & (1u << kBitAf)) deliverBit(kBitAf, "false");
    // IE：只有 fcom* 对 QNaN 也置；fucom* 只对 SNaN（这里与 `emitX87Compare` 同口径）
    (void)quiet;
    if (!quiet) {
        const std::string ie = tmp();
        add(ie + " = select i1 " + uno + ", i16 129, i16 0");
        const std::string st = x87LoadU16(x87l::kStatus);
        const std::string nv = tmp();
        add(nv + " = or i16 " + st + ", " + ie);
        x87StoreU16(x87l::kStatus, nv);
    }
    if (pop_after) x87Pop();
}

std::string FnEmit::x87CondValue(const Insn& in) {
    switch (in.op) {
        case Op::Fcmovb: return readBit(kBitCf);
        case Op::Fcmove: return readBit(kBitZf);
        case Op::Fcmovbe: return or1(readBit(kBitCf), readBit(kBitZf));
        case Op::Fcmovu: return readBit(kBitPf);
        case Op::Fcmovnb: return not1(readBit(kBitCf));
        case Op::Fcmovne: return not1(readBit(kBitZf));
        case Op::Fcmovnbe: return and1(not1(readBit(kBitCf)), not1(readBit(kBitZf)));
        case Op::Fcmovnu: return not1(readBit(kBitPf));
        default: return "false";
    }
}

void FnEmit::emitX87Fcmov(const Insn& in) {
    const std::string cond = x87CondValue(in);
    const int i = (in.op1().kind == OpKind::Reg) ? x87StIndex(in.op1(), 1) : x87StIndex(in.op0(), 1);
    // 照 `exec_fpu.cpp`：**条件成立才写**（不成立时连 tag 都不动）⇒ 用分支，不用 select。
    const std::string id = std::to_string(label_id_++);
    add("br i1 " + cond + ", label %x87f_take_" + id + ", label %x87f_cont_" + id);
    put("x87f_take_" + id + ":\n");
    x87StoreSt(0, x87LoadSt(i));
    x87WriteTagSlot(0, -1);
    add("br label %x87f_cont_" + id);
    put("x87f_cont_" + id + ":\n");
}

// 超越函数族：`@lift_x87_sem(ptr %cpu, i32 <Op>)` —— 实现在 `exec_fpu.cpp`（解释器同一份）
void FnEmit::emitX87HelperCall(const Insn& in) {
    syncBeforeHostCall();  // 宿主调用前的既有纪律（tick + 寄存器写回）
    add("call void @lift_x87_sem(ptr %cpu, i32 " + std::to_string(static_cast<int>(in.op)) + ")");
    stats_->used_x87_sem = true;
    ++stats_->x87_helper_sites;
}

// 二元算术（非 p 形态）：照 `exec_fpu.cpp` 的 `arith()`
void FnEmit::emitX87Arith(const Insn& in) {
    std::string a;
    std::string b;
    int dst = 0;
    if (in.op0().kind == OpKind::Mem) {
        b = (in.op0().mem.size == 4) ? x87LoadMemF32(in.op0().mem) : x87LoadMemF64(in.op0().mem);
        a = x87LoadSt(0);
        dst = 0;
    } else {
        dst = static_cast<int>(regSpec(in.op0().reg).index);
        a = x87LoadSt(dst);
        if (in.op1().kind == OpKind::Reg) {
            b = x87LoadSt(static_cast<int>(regSpec(in.op1().reg).index));
        } else if (in.op1().kind == OpKind::Mem) {
            b = (in.op1().mem.size == 4) ? x87LoadMemF32(in.op1().mem) : x87LoadMemF64(in.op1().mem);
        } else {
            b = x87LoadSt(0);
        }
    }
    std::string r;
    switch (in.op) {
        case Op::Fadd: r = bin("fadd", "double", a, b); break;
        case Op::Fsub: r = bin("fsub", "double", a, b); break;
        case Op::Fsubr: r = bin("fsub", "double", b, a); break;
        case Op::Fmul: r = bin("fmul", "double", a, b); break;
        case Op::Fdiv: r = bin("fdiv", "double", a, b); break;
        default: r = bin("fdiv", "double", b, a); break;  // Fdivr
    }
    if (in.op == Op::Fdiv || in.op == Op::Fdivr) {
        // 照 `x87Div`：**除数为 0** 时 —— 分子是 0 或 NaN ⇒ IE；否则 ⇒ ZE。
        const std::string den = (in.op == Op::Fdiv) ? b : a;
        const std::string num = (in.op == Op::Fdiv) ? a : b;
        const std::string dz = tmp();
        add(dz + " = fcmp oeq double " + den + ", 0.0");
        const std::string num_zero = tmp();
        add(num_zero + " = fcmp oeq double " + num + ", 0.0");
        const std::string num_nan = tmp();
        add(num_nan + " = fcmp uno double " + num + ", " + num);
        const std::string bad = or1(num_zero, num_nan);
        const std::string ie_cond = and1(dz, bad);
        const std::string ze_cond = and1(dz, not1(bad));
        const std::string ie = tmp();
        // 0x0001 | 0x0080：IE + ES（任务 0）
        add(ie + " = select i1 " + ie_cond + ", i16 129, i16 0");
        const std::string ze = tmp();
        // 0x0004 | 0x0080：ZE + ES（任务 0）
        add(ze + " = select i1 " + ze_cond + ", i16 132, i16 0");
        const std::string st = x87LoadU16(x87l::kStatus);
        const std::string with_ie = tmp();
        add(with_ie + " = or i16 " + st + ", " + ie);
        const std::string with_ze = tmp();
        add(with_ze + " = or i16 " + with_ie + ", " + ze);
        x87StoreU16(x87l::kStatus, with_ze);
        // ★ 任务 0：OE / UE 与解释器**同一份判据**（`b == 0` 的 inf 是 ZE，⛔ 不是上溢）。
        //   ⚠ 这里**不 return**：结果照旧要 `x87StoreSt` / `x87WriteTagSlot` 落盘。
        x87RaiseFloatFlags(a, b, r, not1(dz));
    } else {
        x87RaiseFloatFlags(a, b, r);
    }
    x87StoreSt(dst, r);
    x87WriteTagSlot(dst, -1);
}

// ---------------------------------------------------------------------------------------
// x87 的 10 字节扩展精度内存格式（`fpu_bridge.cpp:27-50` / `:52-...`）
// ---------------------------------------------------------------------------------------
// ★ 为什么字面量全是十进制：`add()` 直接把字符串拼进 IR，i64 的十六进制字面量在 LLVM 文本里
//   不认 `0x` 前缀；这里用常量表把每个魔数写成十进制并标注原值（`⛔ 不许改值`）。
std::string FnEmit::m80ToDouble(const std::string& p) {
    const std::string sig = tmp();
    add(sig + " = load i64, ptr " + p + ", align 1");
    const std::string p8 = tmp();
    add(p8 + " = getelementptr i8, ptr " + p + ", i64 8");
    const std::string se = tmp();
    add(se + " = load i16, ptr " + p8 + ", align 1");
    const std::string negb = tmp();
    add(negb + " = and i16 " + se + ", -32768");  // 0x8000
    const std::string neg = tmp();
    add(neg + " = icmp ne i16 " + negb + ", 0");
    const std::string e16 = tmp();
    add(e16 + " = and i16 " + se + ", 32767");  // 0x7FFF
    const std::string e32 = tmp();
    add(e32 + " = zext i16 " + e16 + " to i32");
    const std::string sgn64 = select1(neg, "i64", "-9223372036854775808", "0");  // 1<<63
    // ---- NaN / Inf（`e == 0x7FFF`）----
    const std::string e_all = tmp();
    add(e_all + " = icmp eq i32 " + e32 + ", 32767");
    const std::string sh1 = tmp();
    add(sh1 + " = shl i64 " + sig + ", 1");
    const std::string is_inf = tmp();
    add(is_inf + " = icmp eq i64 " + sh1 + ", 0");
    const std::string fs = tmp();
    add(fs + " = lshr i64 " + sig + ", 11");
    const std::string fm = tmp();
    add(fm + " = and i64 " + fs + ", 4503599627370495");  // 0xFFFFFFFFFFFFF
    const std::string fz = tmp();
    add(fz + " = icmp eq i64 " + fm + ", 0");
    const std::string fr = select1(fz, "i64", "2251799813685248", fm);  // 0x8000000000000
    const std::string nb0 = tmp();
    add(nb0 + " = or i64 " + fr + ", 9218868437227405312");  // 0x7FF0000000000000
    const std::string nanb = tmp();
    add(nanb + " = or i64 " + nb0 + ", " + sgn64);
    const std::string infb = tmp();
    add(infb + " = or i64 9218868437227405312, " + sgn64);
    const std::string spb = select1(is_inf, "i64", infb, nanb);
    // ---- 零（`e == 0 && sig == 0`）----
    const std::string zs = tmp();
    add(zs + " = icmp eq i64 " + sig + ", 0");
    const std::string ze = tmp();
    add(ze + " = icmp eq i32 " + e32 + ", 0");
    const std::string is_zero = and1(zs, ze);
    // ---- 有限（`biased = (e==0) ? 1 : e`；`v = sig · 2^(biased-16383-63)`）----
    const std::string biased = select1(ze, "i32", "1", e32);
    const std::string m = tmp();
    add(m + " = uitofp i64 " + sig + " to double");
    const std::string ex = bin("sub", "i32", biased, "16446");  // 16383 + 63
    const std::string lv = tmp();
    add(lv + " = call double @llvm.ldexp.f64(double " + m + ", i32 " + ex + ")");
    stats_->used_ldexp = true;
    const std::string nlv = tmp();
    add(nlv + " = fneg double " + lv);
    const std::string fin = select1(neg, "double", nlv, lv);
    const std::string zv = tmp();
    add(zv + " = bitcast i64 " + sgn64 + " to double");
    const std::string sv = tmp();
    add(sv + " = bitcast i64 " + spb + " to double");
    const std::string nz = select1(is_zero, "double", zv, fin);
    return select1(e_all, "double", sv, nz);
}

void FnEmit::doubleToM80(const std::string& v, const std::string& p) {
    const std::string bits = tmp();
    add(bits + " = bitcast double " + v + " to i64");
    const std::string nb = tmp();
    add(nb + " = lshr i64 " + bits + ", 63");
    const std::string neg = tmp();
    add(neg + " = icmp ne i64 " + nb + ", 0");
    const std::string ex = tmp();
    add(ex + " = lshr i64 " + bits + ", 52");
    const std::string e64 = tmp();
    add(e64 + " = and i64 " + ex + ", 2047");  // 0x7FF
    const std::string e32 = tmp();
    add(e32 + " = trunc i64 " + e64 + " to i32");
    const std::string frac = tmp();
    add(frac + " = and i64 " + bits + ", 4503599627370495");  // 0xFFFFFFFFFFFFF
    // 正规：signif = 1<<63 | frac<<11；se = e + 15360（= 16383 - 1023）
    const std::string fs = tmp();
    add(fs + " = shl i64 " + frac + ", 11");
    const std::string msb = tmp();
    add(msb + " = or i64 9223372036854775808, " + fs);
    const std::string se_n = bin("add", "i32", e32, "15360");
    // 次正规（e==0 && frac!=0）：先规格化（`fpu_bridge.cpp:67-75` 的 while）
    //   sh = clz64(frac) - 11 = 52 - floor(log2(frac))；signif = 1<<63 | ((frac<<sh) & mask)<<11
    //   se = 16383 - 1022 - sh = 15361 - sh
    const std::string lz = tmp();
    add(lz + " = call i64 @llvm.ctlz.i64(i64 " + frac + ", i1 false)");
    stats_->used_ctlz64 = true;
    const std::string sh = tmp();
    add(sh + " = sub i64 " + lz + ", 11");
    const std::string sh32 = tmp();
    add(sh32 + " = trunc i64 " + sh + " to i32");
    const std::string fsh = tmp();
    add(fsh + " = shl i64 " + frac + ", " + sh);
    const std::string fshm = tmp();
    add(fshm + " = and i64 " + fsh + ", 4503599627370495");
    const std::string fsh11 = tmp();
    add(fsh11 + " = shl i64 " + fshm + ", 11");
    const std::string ms = tmp();
    add(ms + " = or i64 9223372036854775808, " + fsh11);
    const std::string se_s = bin("sub", "i32", "15361", sh32);
    // Inf / NaN：se = 0x7FFF；signif = 1<<63 | (frac==0 ? 0 : frac<<11)
    const std::string fz = tmp();
    add(fz + " = icmp eq i64 " + frac + ", 0");
    const std::string fsp = select1(fz, "i64", "0", fs);
    const std::string msp = tmp();
    add(msp + " = or i64 9223372036854775808, " + fsp);
    // ---- 组装 ----
    const std::string is_special = bin("icmp eq", "i32", e32, "2047");
    const std::string is_sub = and1(bin("icmp eq", "i32", e32, "0"), not1(fz));
    const std::string is_zero = and1(bin("icmp eq", "i32", e32, "0"), fz);
    const std::string sig1 = select1(is_special, "i64", msp, msb);
    const std::string sig2 = select1(is_zero, "i64", "0", sig1);
    const std::string sig3 = select1(is_sub, "i64", ms, sig2);
    const std::string se1 = select1(is_special, "i32", "32767", se_n);
    const std::string se2 = select1(is_sub, "i32", se_s, se1);
    const std::string se3 = select1(is_zero, "i32", "0", se2);
    const std::string negb = select1(neg, "i32", "-32768", "0");  // 0x8000
    const std::string sef = bin("or", "i32", se3, negb);
    add("store i64 " + sig3 + ", ptr " + p + ", align 1");
    const std::string p8 = tmp();
    add(p8 + " = getelementptr i8, ptr " + p + ", i64 8");
    const std::string se16 = tmp();
    add(se16 + " = trunc i32 " + sef + " to i16");
    add("store i16 " + se16 + ", ptr " + p8 + ", align 1");
}

// 保存环境 / 保存区（`exec_fpu.cpp:337-367`：doSaveEnv + doFnsave + doFninit）
void FnEmit::emitX87SaveEnv(const MemOperand& m, bool with_regs) {
    x87SyncTop();  // `syncStatus`：状态字的 TOP 镜像（`exec_fpu.cpp:1425` 先 sync 再 save）
    const std::string p = memPtrFor(eaOf(m));
    auto store_u16_as_u32 = [&](std::uint64_t off, const std::string& v16) {
        const std::string z = tmp();
        add(z + " = zext i16 " + v16 + " to i32");
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr " + p + ", i64 " + std::to_string((long long)off));
        add("store i32 " + z + ", ptr " + q + ", align 1");
    };
    store_u16_as_u32(0, x87LoadU16(x87l::kControl));
    store_u16_as_u32(4, x87LoadU16(x87l::kStatus));
    store_u16_as_u32(8, x87LoadU16(x87l::kTag));
    for (int i = 12; i <= 24; i += 4) {  // FIP / FCS+FOP / FDP / FDS：解释器一律写 0
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr " + p + ", i64 " + std::to_string(i));
        add("store i32 0, ptr " + q + ", align 1");
    }
    if (!with_regs) return;
    for (int i = 0; i < 8; ++i) {
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr " + p + ", i64 " + std::to_string(28 + 10 * i));
        doubleToM80(x87LoadSt(i), q);
    }
    // `doFnsave` 之后 FPU 处于 init 状态（`exec_fpu.cpp:366` → `doFninit`：`X87State::reset()`）
    x87StoreU16(x87l::kControl, "956");  // 0x037F
    x87StoreU16(x87l::kStatus, "0");
    x87StoreTop("0");
    x87StoreU16(x87l::kTag, "-1");  // 0xFFFF
    for (int i = 0; i < 8; ++i) {
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr %cpu, i64 " +
            std::to_string(static_cast<long long>(x87l::kRegs + 8u * static_cast<unsigned>(i))));
        add("store double 0.0, ptr " + q + ", align 8");
    }
}

// 恢复环境 / 保存区（`exec_fpu.cpp:347-353` 的 doLoadEnv + `:369-379` 的 doFrstor）
void FnEmit::emitX87RestoreEnv(const MemOperand& m, bool with_regs) {
    const std::string p = memPtrFor(eaOf(m));
    auto load_u16 = [&](std::uint64_t off) {
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr " + p + ", i64 " + std::to_string((long long)off));
        const std::string v = tmp();
        add(v + " = load i16, ptr " + q + ", align 1");
        return v;
    };
    x87StoreU16(x87l::kControl, load_u16(0));
    const std::string st = load_u16(4);
    x87StoreU16(x87l::kStatus, st);
    x87StoreU16(x87l::kTag, load_u16(8));
    const std::string sh = tmp();
    add(sh + " = lshr i16 " + st + ", 11");
    const std::string t7 = tmp();
    add(t7 + " = and i16 " + sh + ", 7");
    const std::string top = tmp();
    add(top + " = zext i16 " + t7 + " to i32");
    x87StoreTop(top);  // ★ 必须在读寄存器**之前**（`phys(i)` 依赖 top）
    if (!with_regs) return;
    for (int i = 0; i < 8; ++i) {
        const std::string q = tmp();
        add(q + " = getelementptr i8, ptr " + p + ", i64 " + std::to_string(28 + 10 * i));
        x87StoreSt(i, m80ToDouble(q));
        // ★ `doFrstor`（`exec_fpu.cpp:369-379`）**不写 tag**：tag 来自环境区（上面已 store）。
        //   ⛔ 不要在这里 `x87WriteTagSlot` —— 那会把环境里的 tag 覆盖成 valid。
    }
    x87StoreTop(top);  // `exec_fpu.cpp:378` 再算一次（值相同，照抄）
}

// `fld` / `fst(p)` 的 10 字节扩展精度内存形态（`exec_fpu.cpp:1266-1304` + `x87LoadMem(..,10)`）
void FnEmit::emitX87(const Insn& in) {
    ++stats_->x87_inline_sites;
    switch (in.op) {
        // ---- 压栈族 ----
        case Op::Fld: {
            if (in.op0().kind == OpKind::Reg) {
                const int i = x87StIndex(in.op0(), 0);
                x87Push(x87LoadSt(i), -1);
            } else if (in.op0().mem.size == 4) {
                x87Push(x87LoadMemF32(in.op0().mem), -1);
            } else if (in.op0().mem.size == 10) {
                // ★ 2026-11-15：10 字节扩展精度（`x87LoadMem(..,10)` → `m80ToDouble`）
                const std::string ea = eaOf(in.op0().mem);
                const std::string e64 = tmp();
                add(e64 + " = zext i32 " + ea + " to i64");
                const std::string p = tmp();
                add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
                x87Push(m80ToDouble(p), -1);
            } else {
                x87Push(x87LoadMemF64(in.op0().mem), -1);
            }
            return;
        }
        case Op::Fild: {
            const MemOperand& m = in.op0().mem;
            const std::string ea = eaOf(m);
            std::string e64 = tmp();
            add(e64 + " = zext i32 " + ea + " to i64");
            std::string p = tmp();
            add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
            std::string v;
            if (m.size == 2) {
                const std::string l = tmp();
                add(l + " = load i16, ptr " + p + ", align 1");
                const std::string s = tmp();
                add(s + " = sext i16 " + l + " to i32");
                v = tmp();
                add(v + " = sitofp i32 " + s + " to double");
            } else if (m.size == 4) {
                const std::string l = tmp();
                add(l + " = load i32, ptr " + p + ", align 1");
                v = tmp();
                add(v + " = sitofp i32 " + l + " to double");
            } else {
                const std::string l = tmp();
                add(l + " = load i64, ptr " + p + ", align 1");
                v = tmp();
                add(v + " = sitofp i64 " + l + " to double");
            }
            x87Push(v, -1);
            return;
        }
        case Op::Fldz: x87StoreConst(0.0); return;
        case Op::Fld1: x87StoreConst(1.0); return;
        case Op::Fldpi: x87StoreConst(3.14159265358979323846); return;
        case Op::Fldln2: x87StoreConst(0.69314718055994530942); return;
        case Op::Fldlg2: x87StoreConst(0.30102999566398119521); return;
        case Op::Fldl2e: x87StoreConst(1.44269504088896340736); return;
        case Op::Fldl2t: x87StoreConst(3.32192809488736234787); return;
        // ---- 落盘族 ----
        case Op::Fst:
        case Op::Fstp: {
            const std::string v = x87LoadSt(0);
            if (in.op0().kind == OpKind::Reg) {
                const int i = x87StIndex(in.op0(), 0);
                if (i != 0) {
                    x87StoreSt(i, v);
                    x87WriteTagSlot(i, -1);
                }
            } else if (in.op0().mem.size == 4) {
                x87StoreMemF32(in.op0().mem, v);
            } else if (in.op0().mem.size == 10) {
                // ★ 2026-11-15：10 字节扩展精度（`x87StoreMem(..,10)` → `doubleToM80`）
                const std::string ea = eaOf(in.op0().mem);
                const std::string e64 = tmp();
                add(e64 + " = zext i32 " + ea + " to i64");
                const std::string p = tmp();
                add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
                doubleToM80(v, p);
            } else {
                x87StoreMemF64(in.op0().mem, v);
            }
            if (in.op == Op::Fstp) x87Pop();
            return;
        }
        case Op::Fist:
        case Op::Fistp:
        case Op::Fisttp: {
            if (in.op == Op::Fisttp) {
                // `fisttp` 恒朝零：临时把 RC 写成 3 是**不对**的（那是状态字的一部分）
                // ⇒ 用 `llvm.trunc` 单独一条路径（与 `x87ToI32Trunc` 同语义）。
                const std::string v = x87LoadSt(0);
                const unsigned size = in.op0().mem.size;
                const bool wide = (size == 8);
                const std::string t0 = tmp();
                add(t0 + " = call double @llvm.trunc.f64(double " + v + ")");
                const char* lo = wide ? "-9223372036854775808.0" : "-2147483648.0";
                const char* hi = wide ? "9223372036854775808.0" : "2147483648.0";
                const std::string ge = tmp();
                add(ge + " = fcmp oge double " + t0 + ", " + lo);
                const std::string lt = tmp();
                add(lt + " = fcmp olt double " + t0 + ", " + hi);
                const std::string inr = tmp();
                add(inr + " = and i1 " + ge + ", " + lt);
                const std::string safe = select1(inr, "double", t0, "0.0");
                const std::string conv = tmp();
                add(conv + " = fptosi double " + safe + " to " + (wide ? "i64" : "i32"));
                const std::string indef = wide ? "-9223372036854775808" : "-2147483648";
                const std::string iv = select1(not1(inr), wide ? "i64" : "i32", indef, conv);
                const std::string ie = tmp();
                add(ie + " = select i1 " + not1(inr) + ", i16 129, i16 0");
                const std::string st = x87LoadU16(x87l::kStatus);
                const std::string nv = tmp();
                add(nv + " = or i16 " + st + ", " + ie);
                x87StoreU16(x87l::kStatus, nv);
                // 写内存
                const std::string ea = eaOf(in.op0().mem);
                std::string e64 = tmp();
                add(e64 + " = zext i32 " + ea + " to i64");
                std::string p = tmp();
                add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
                if (wide) {
                    add("store i64 " + iv + ", ptr " + p + ", align 1");
                } else if (size == 4) {
                    add("store i32 " + iv + ", ptr " + p + ", align 1");
                } else {
                    const std::string tr = tmp();
                    add(tr + " = trunc i32 " + iv + " to i16");
                    add("store i16 " + tr + ", ptr " + p + ", align 1");
                }
                x87Pop();
                return;
            }
            x87FistStore(in);
            if (in.op == Op::Fistp) x87Pop();
            return;
        }
        // ---- 四则 ----
        case Op::Fadd:
        case Op::Fsub:
        case Op::Fsubr:
        case Op::Fmul:
        case Op::Fdiv:
        case Op::Fdivr:
            emitX87Arith(in);
            return;
        case Op::Faddp:
        case Op::Fsubp:
        case Op::Fsubrp:
        case Op::Fmulp:
        case Op::Fdivp:
        case Op::Fdivrp: {
            // `faddp st(i), st` ⇒ ST(i) = ST(i) op ST0，然后 pop（照 `exec_fpu.cpp` 的重写）
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 1;
            const std::string a = x87LoadSt(i);
            const std::string b = x87LoadSt(0);
            std::string r;
            switch (in.op) {
                case Op::Faddp: r = bin("fadd", "double", a, b); break;
                case Op::Fsubp: r = bin("fsub", "double", a, b); break;
                case Op::Fsubrp: r = bin("fsub", "double", b, a); break;
                case Op::Fmulp: r = bin("fmul", "double", a, b); break;
                case Op::Fdivp: r = bin("fdiv", "double", a, b); break;
                default: r = bin("fdiv", "double", b, a); break;
            }
            // ★ 任务 0：OE / UE（解释器走 `x87Div`/`x87Add`… 的同一份判据）
            x87RaiseFloatFlags(a, b, r);
            x87StoreSt(i, r);
            x87WriteTagSlot(i, -1);
            x87Pop();
            return;
        }
        case Op::Fiadd:
        case Op::Fisub:
        case Op::Fisubr:
        case Op::Fimul:
        case Op::Fidiv:
        case Op::Fidivr: {
            const MemOperand& m = in.op0().mem;
            const std::string ea = eaOf(m);
            std::string e64 = tmp();
            add(e64 + " = zext i32 " + ea + " to i64");
            std::string p = tmp();
            add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
            std::string b;
            if (m.size == 2) {
                const std::string l = tmp();
                add(l + " = load i16, ptr " + p + ", align 1");
                const std::string s = tmp();
                add(s + " = sext i16 " + l + " to i32");
                b = tmp();
                add(b + " = sitofp i32 " + s + " to double");
            } else if (m.size == 4) {
                const std::string l = tmp();
                add(l + " = load i32, ptr " + p + ", align 1");
                b = tmp();
                add(b + " = sitofp i32 " + l + " to double");
            } else {
                const std::string l = tmp();
                add(l + " = load i64, ptr " + p + ", align 1");
                b = tmp();
                add(b + " = sitofp i64 " + l + " to double");
            }
            const std::string a = x87LoadSt(0);
            std::string r;
            switch (in.op) {
                case Op::Fiadd: r = bin("fadd", "double", a, b); break;
                case Op::Fisub: r = bin("fsub", "double", a, b); break;
                case Op::Fisubr: r = bin("fsub", "double", b, a); break;
                case Op::Fimul: r = bin("fmul", "double", a, b); break;
                case Op::Fidiv: r = bin("fdiv", "double", a, b); break;
                default: r = bin("fdiv", "double", b, a); break;
            }
            // ★ 任务 0：OE / UE（与解释器 `integerArith` → `x87Div`/`x87Add`… 同一份判据）
            x87RaiseFloatFlags(a, b, r);
            x87StoreSt(0, r);
            x87WriteTagSlot(0, -1);
            return;
        }
        // ---- 比较 ----
        case Op::Fcom: emitX87Compare(in, false, false); return;
        case Op::Fcomp: emitX87Compare(in, true, false); return;
        case Op::Fucom: emitX87Compare(in, false, true); return;
        case Op::Fucomp: emitX87Compare(in, true, true); return;
        case Op::Fcompp:
            emitX87Compare(in, false, false);
            x87Pop();
            x87Pop();
            return;
        case Op::Fucompp:
            emitX87Compare(in, false, true);
            x87Pop();
            x87Pop();
            return;
        case Op::Ftst: {
            const std::string a = x87LoadSt(0);
            const std::string uno = tmp();
            add(uno + " = fcmp uno double " + a + ", 0.0");
            const std::string eq = tmp();
            add(eq + " = fcmp oeq double " + a + ", 0.0");
            const std::string lt = tmp();
            add(lt + " = fcmp olt double " + a + ", 0.0");
            x87SetCC(or1(uno, lt), "false", uno, or1(uno, eq));
            const std::string ie = tmp();
            add(ie + " = select i1 " + uno + ", i16 1, i16 0");
            const std::string st = x87LoadU16(x87l::kStatus);
            const std::string nv = tmp();
            add(nv + " = or i16 " + st + ", " + ie);
            x87StoreU16(x87l::kStatus, nv);
            return;
        }
        case Op::Fcomi: emitX87CmpEflags(in, false, false); return;
        case Op::Fcomip: emitX87CmpEflags(in, true, false); return;
        case Op::Fucomi: emitX87CmpEflags(in, false, true); return;
        case Op::Fucomip: emitX87CmpEflags(in, true, true); return;
        case Op::Fcmovb:
        case Op::Fcmove:
        case Op::Fcmovbe:
        case Op::Fcmovu:
        case Op::Fcmovnb:
        case Op::Fcmovne:
        case Op::Fcmovnbe:
        case Op::Fcmovnu:
            emitX87Fcmov(in);
            return;
        // ---- 控制 / 状态字 ----
        case Op::Fnstsw: {
            x87SyncTop();  // = `syncStatus` 的 TOP 部分（挂账异常位在这一层不存在）
            const std::string sw = x87LoadU16(x87l::kStatus);
            const std::string z = tmp();
            add(z + " = zext i16 " + sw + " to i32");
            if (in.op0().kind == OpKind::Reg) {
                writeRegValue(in.op0().reg, z);
            } else if (in.op0().kind == OpKind::Mem) {
                const std::string ea = eaOf(in.op0().mem);
                std::string e64 = tmp();
                add(e64 + " = zext i32 " + ea + " to i64");
                std::string p = tmp();
                add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
                add("store i16 " + sw + ", ptr " + p + ", align 1");
            } else {
                writeRegValue(Reg::AX, z);
            }
            return;
        }
        case Op::Fldcw: {
            const std::string ea = eaOf(in.op0().mem);
            std::string e64 = tmp();
            add(e64 + " = zext i32 " + ea + " to i64");
            std::string p = tmp();
            add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
            const std::string cw = tmp();
            add(cw + " = load i16, ptr " + p + ", align 1");
            x87StoreU16(x87l::kControl, cw);
            return;
        }
        case Op::Fnstcw: {
            const std::string cw = x87LoadU16(x87l::kControl);
            const std::string ea = eaOf(in.op0().mem);
            std::string e64 = tmp();
            add(e64 + " = zext i32 " + ea + " to i64");
            std::string p = tmp();
            add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
            add("store i16 " + cw + ", ptr " + p + ", align 1");
            return;
        }
        // ---- 栈操作 ----
        case Op::Fxch: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 1;
            const std::string v0 = x87LoadSt(0);
            const std::string v1 = x87LoadSt(i);
            x87StoreSt(0, v1);
            x87StoreSt(i, v0);
            return;
        }
        case Op::Ffree: {
            const int i = (in.op0().kind == OpKind::Reg) ? static_cast<int>(regSpec(in.op0().reg).index) : 0;
            x87SetTagEmpty(i);
            return;
        }
        case Op::Fdecstp:
        case Op::Fincstp: {
            const std::string top = x87LoadTop();
            // `fdecstp` = (top + 7) & 7；`fincstp` = (top + 1) & 7（与 `exec_fpu.cpp` 同一条）
            const char* d = (in.op == Op::Fdecstp) ? "7" : "1";
            x87StoreTop(bin("add", "i32", top, d));
            x87SyncTop();
            return;
        }
        // ---- 状态字 / 控制字 ----
        case Op::Fninit: {
            // 照 `X87State::reset()`：8 个槽清零、控制字回 0x037F、状态字 0、top 0、tag 全 empty。
            // ⚠ 解释器的 `reset()` 会 `memset(regs)` ⇒ 这里也必须清（否则 `fninit; fstp` 的载荷不同）。
            for (int k = 0; k < 8; ++k) {
                const std::string p = tmp();
                add(p + " = getelementptr i8, ptr %cpu, i64 " +
                    std::to_string(static_cast<long long>(x87l::kRegs + 8ull * static_cast<unsigned>(k))));
                add("store double 0.0, ptr " + p + ", align 8");
            }
            x87StoreU16(x87l::kControl, "959");  // 0x037F
            x87StoreU16(x87l::kStatus, "0");
            x87StoreTop("0");
            x87StoreU16(x87l::kTag, "-1");  // 0xFFFF
            return;
        }
        case Op::Fclex: {
            // 清 IE/DE/ZE/OE/UE/PE/SF/ES = 低 8 位（与 `exec_fpu.cpp` 同一条掩码）
            const std::string st = x87LoadU16(x87l::kStatus);
            const std::string nv = tmp();
            add(nv + " = and i16 " + st + ", -256");  // ~0x00FF
            x87StoreU16(x87l::kStatus, nv);
            return;
        }
        // ---- 一元 ----
        case Op::Fchs: {
            const std::string v = x87LoadSt(0);
            const std::string r = tmp();
            add(r + " = fneg double " + v);
            x87StoreSt(0, r);
            x87WriteTagSlot(0, -1);
            return;
        }
        case Op::Fabs: {
            const std::string v = x87LoadSt(0);
            const std::string r = tmp();
            add(r + " = call double @llvm.fabs.f64(double " + v + ")");
            x87StoreSt(0, r);
            x87WriteTagSlot(0, -1);
            return;
        }
        case Op::Fsqrt: {
            const std::string v = x87LoadSt(0);
            const std::string neg = tmp();
            add(neg + " = fcmp olt double " + v + ", 0.0");
            const std::string r = tmp();
            add(r + " = call double @llvm.sqrt.f64(double " + v + ")");
            const std::string ie = tmp();
            add(ie + " = select i1 " + neg + ", i16 129, i16 0");
            const std::string st = x87LoadU16(x87l::kStatus);
            const std::string nv = tmp();
            add(nv + " = or i16 " + st + ", " + ie);
            x87StoreU16(x87l::kStatus, nv);
            x87StoreSt(0, r);
            x87WriteTagSlot(0, -1);
            return;
        }
        case Op::Frndint: {
            const std::string v = x87LoadSt(0);
            const std::string r = x87ToInt(v, 8);  // 复用 RC 四档（返回 i64）
            const std::string back = tmp();
            add(back + " = sitofp i64 " + r + " to double");
            x87StoreSt(0, back);
            x87WriteTagSlot(0, -1);
            return;
        }
        case Op::Fwait:
        case Op::Fnop: return;
        // ---- 超越函数族：宿主 helper（解释器的同一份实现）----
        case Op::Fsin:
        case Op::Fcos:
        case Op::Fsincos:
        case Op::Fptan:
        case Op::Fpatan:
        case Op::Fyl2x:
        case Op::Fyl2xp1:
        case Op::F2xm1:
        case Op::Fscale:
        case Op::Fprem:
        case Op::Fprem1:
        case Op::Fxtract:
        case Op::Fxam: emitX87HelperCall(in); return;
        // ---- 环境 / 保存区（2026-11-15：`lift_one` → 0，改成内联的 28B/108B 读写）----
        //   `exec_fpu.cpp:1412-1433` 的四个 case 逐条照抄：`fnsave` 之后 FPU 处于 init 状态。
        case Op::Fldenv:
            if (in.op0().kind != OpKind::Mem) { emitTrap(in, "fldenv 只支持内存操作数"); return; }
            emitX87RestoreEnv(in.op0().mem, /*with_regs=*/false);
            return;
        case Op::Fnstenv:
            if (in.op0().kind != OpKind::Mem) { emitTrap(in, "fnstenv 只支持内存操作数"); return; }
            emitX87SaveEnv(in.op0().mem, /*with_regs=*/false);
            return;
        case Op::Fsave:
        case Op::Fnsave:
            if (in.op0().kind != OpKind::Mem) { emitTrap(in, "fnsave 只支持内存操作数"); return; }
            emitX87SaveEnv(in.op0().mem, /*with_regs=*/true);
            return;
        case Op::Frstor:
            if (in.op0().kind != OpKind::Mem) { emitTrap(in, "frstor 只支持内存操作数"); return; }
            emitX87RestoreEnv(in.op0().mem, /*with_regs=*/true);
            return;
        default:
            // `unsupportedReasonOf` 应当把这些挡在 `lift_one` 路上（生成器内部不一致 ⇒ 响亮 trap）
            emitTrap(in, "x87 族：覆盖判定漏了这条（生成器内部不一致）");
            return;
    }
}

void FnEmit::emitBlock(const GenBlock& block) {
    // ★ G3 修的一个**既有**缺陷：`trapped_` 的语义是"**本块**里已经发过 `unreachable`，后面不许再发
    //   指令"（P5：`unreachable` 之后跟指令 = 非法 IR）。但它原来是**跨块**留着的 ⇒ 只要某个块以
    //   停机类指令（`hlt/int3/int` 的 `lift_one(terminating=true)`）收尾，同一函数里**后面每个块**
    //   都会在 `if (trapped_) return;` 处提前返回 ⇒ 块没有终结指令 ⇒ clang 报
    //   `expected instruction opcode`（G3 实测：`bb_50C5C6` 是 `int`，紧随其后的 `bb_50C694` 就断了）。
    //   每个基本块是独立的 IR 块 ⇒ 这里按块复位。
    trapped_ = false;
    // ★ E2 / S2：块入口的寄存器状态**来自调用方**（`%st`），缓存必须空起步 ——
    //   跨块复用一个"只在本块里定义过"的 SSA 值会用到不支配本块的值（非法 IR，且语义错）。
    invalidateRegs();
    // ★ S3-c：块首按 `need_in` 逐位决定 `FromBase` / `Poison`（plan §1.6(b)）。
    //   `FromBase` = 这位在块首就正确（前面某条边把它物化过）；`Poison` = 块内没人读它
    //   ⇒ 写它的时候也不算（死写 = 0 语句）。⛔ 顺序：`need_in` 必须在块首就位。
    const auto fit = flags_.find(block.ea);
    if (fit == flags_.end()) {
        // 活性表必须覆盖**每一个**块（`computeFlagsLiveness` 的来源与 `components` 同源）。
        fail("flags 活性表里没有这个块（生成器内部不一致）");
        return;
    }
    fb_ = &fit->second;
    need_out_ = fit->second.need_out;
    for (int b = 0; b < 6; ++b) {
        slot_[b] = (fit->second.need_in & (1u << b)) != 0 ? Slot::FromBase : Slot::Poison;
        slot_val_[b].clear();
    }
    // ★ S3-b：融合账也整份复位 —— 跨块的生产者**不能**当本块的"最近写者"。
    for (int b = 0; b < 6; ++b) bit_owner_[b] = -1;
    cmp_recs_.assign(block.insns.size(), CmpRec{});
    cur_index_ = -1;
    put("bb_" + hexEa(block.ea) + ":\n");
    for (std::size_t i = 0; i < block.insns.size(); ++i) {
        const int index = static_cast<int>(i);
        const Insn& in = block.insns[i];
        cur_ea_ = in.ea;
        cur_index_ = index;
        const bool last = (i + 1 == block.insns.size());
        if (last && block.terminated) {
            // ---- 控制流收尾 ----
            // ★ 控制转移（`jmp/jcc/call/ret`）由生成器自己 inline / 回 trampoline。
            //   停机类（`hlt/int3/int` 与 `Other`/`Invalid`）**不是**控制转移 ⇒ 交给 `emitStop`
            //   （`@lift_stop` + `ret i32 kLiftHalt`，与解释器的停机面**逐字**相同）。
            if (in.op == Op::Hlt || in.op == Op::Int3 || in.op == Op::Int ||
                in.op == Op::Other || in.op == Op::Invalid) {
                emitStop(in, in.op == Op::Hlt       ? ::kLiftStopHlt
                             : in.op == Op::Int3    ? ::kLiftStopInt3
                             : in.op == Op::Int      ? ::kLiftStopInt
                             : in.op == Op::Other    ? ::kLiftStopOther
                                                     : ::kLiftStopInvalid);
                recordFlagsWrites(in, index, 0);
                return;
            }
            if (const char* reason = unsupportedReason(in)) {
                // 控制转移走不到自己的 inline 路径 ⇒ 生成器内部不一致 ⇒ 仍然**响亮 trap**
                // （父级裁决：⛔ 不许让控制流借 `lift_one` 绕过去 —— 那会丢掉 eip 的变化）。
                flushTick();
                emitTrap(in, reason);
                return;
            }
            ++pending_ticks_;  // 控制转移指令本身也是一条已执行指令
            // 块边界 ⇒ 挂账的静态条数一次发出去 + 寄存器缓存写回 `%st`（E2 / S2）。
            //   `emitTerminator` 自己还会再 flush 一次（比如 `call` 的 push、`ret` 的 esp 调整）。
            // ★ S3-c：退出前把**后继真的要读**的位并进 base（`need_out`；escape 块 = ALL）。
            //   ⚠ 必须在 `flushRegs()` 之前（`materialize` 只改缓存 + 记脏）。
            materialize(need_out_);
            syncBeforeHostCall();
            emitTerminator(in);
            return;
        }
        if (const char* reason = unsupportedReason(in)) {
            // ★ T1c 的**默认路径**：不 inline 的 op ⇒ 单指令宿主原语（语义只有解释器那一份）。
            (void)reason;
            emitLiftOne(in, /*terminating=*/false);
            // ★ c2-lite：按 op 的 W 记账（`mov`/`div` 这类 W=∅ 的站点**不**作废旧写者账；
            //   于是它前面的 cmp 仍可把条件融合给后面的 jcc）。⛔ 不再一律 ALL。
            recordFlagsWrites(in, index, flagsRWOf(in).write);
            if (trapped_) return;
            continue;
        }
        emitInsn(in);
        // ★ S3-b：逐位记账（`W` 表 + 最近一条 `cmp/test` 的 SSA 操作数）。
        recordFlagsWrites(in, index, flagsWriteMask(in));
        ++pending_ticks_;
        if (trapped_) return;  // `emitInsn` 内部的 trap（生成器不一致）⇒ 该块就地收尾
    }
    // 没以控制流收尾 ⇒ 落空到下一个 leader（它一定在 blocks 里）。
    const Insn& last = block.insns.back();
    materialize(need_out_);  // 块出口：只物化后继要读的位（S3-c；escape 块才是 ALL）
    flushTick();  // 块边界：把本块挂账的静态条数一次发出去（寄存器写回由 `emitEdge` 负责）
    emitEdge(last.ea + last.length);
}

std::string FnEmit::run() {
    std::vector<std::uint32_t> entries = comp_.entries;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    // ★ 本单：`ir_target_` 非空 ⇒ 定义行挂 `#0 {`（属性组在 `emitPrologue` 里发；两组值同源）；
    //   空 ⇒ 与今天**逐字节相同**（既有夹具 / `--check` / golden 走的就是这条路）。
    put("define i32 @" + genFunctionName(comp_.root) + "(ptr %st, ptr %mem, i32 %ea, ptr %cpu)" +
        (ir_target_.empty() ? "" : " #0") + " {\n");
    put("entry:\n");
    // ★ A3：这里**不再**预先发 9 条 `%p_sN = getelementptr i8, ptr %st, i64 K`。
    //   那些 GEP 在旧实现里**一次都没被用过**（`loadReg` / `flushRegs` 各自另发），
    //   每函数 9 行纯死语句。现在 GEP 由 `stGep(i)` 在**首次使用处**按需发射、块内复用。
    put("  switch i32 %ea, label %bad [\n");
    for (std::uint32_t ea : entries) {
        put("    i32 " + i32c(ea) + ", label %bb_" + hexEa(ea) + "\n");
    }
    put("  ]\n\n");

    put("bad:\n");
    add("call void @llvm.trap()");
    add("unreachable");
    ++stats_->traps_bad;

    for (const auto& kv : comp_.blocks) {
        put("\n");
        emitBlock(kv.second);
    }
    for (std::uint32_t ea : exit_ea_) {
        put("\nbb_exit_" + hexEa(ea) + ":\n");
        add("ret i32 " + i32c(ea));
    }
    // ★ `lift_one` 的停机出口：`lift_one` 返回 false ⇒ `ret i32 kLiftHalt`
    //   （trampoline 见到它就按 `cpu.stop_kind` / `stop_why` 报停机，⛔ 不许当成"跑完了"）。
    //   统一在函数尾发：块中间发 `ret` 会让后面的指令变成新块的第一条。
    for (const HaltSite& hs : halt_labels_) {
        put("\n" + hs.label + ":\n");
        if (hs.kind != 0xFFFFFFFFu) {
            // ★ `#DE` 等停机面：先 `lift_stop`（置 running/stop_why），再回 trampoline
            add("call void @lift_stop(ptr %cpu, i32 " + i32c(hs.ea) + ", i32 " + std::to_string(hs.kind) + ")");
            stats_->used_lift_stop = true;
            ++stats_->lift_stop_sites;
        }
        add("ret i32 " + i32c(::kLiftHalt));
    }
    put("}\n");
    return body_;
}

}  // namespace

// =====================================================================================
// 块的后继边（分区与发射共用；唯一一份）
// =====================================================================================
std::string genFunctionName(std::uint32_t root) { return "lift_cc_" + hexEa(root); }

std::uint32_t genBranchTarget(const lift::Insn& insn) {
    if (insn.op0().kind != OpKind::Imm) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(insn.ea) + insn.length +
                                      static_cast<std::int64_t>(
                                          static_cast<std::int32_t>(static_cast<std::uint32_t>(
                                              insn.op0().imm))));
}

std::vector<GenEdge> genBlockEdges(const GenBlock& block) {
    std::vector<GenEdge> out;
    if (block.insns.empty()) return out;
    const lift::Insn& last = block.insns.back();
    if (!block.terminated) {
        out.push_back(GenEdge{last.ea + last.length, false, false});
        return out;
    }
    switch (last.op) {
        case Op::Call:
            out.push_back(GenEdge{last.ea + last.length, true, false});
            break;
        case Op::Jmp:
            if (last.op0().kind == OpKind::Imm) out.push_back(GenEdge{genBranchTarget(last), false, false});
            else out.push_back(GenEdge{0, false, true});
            break;
        case Op::Jcc:
            out.push_back(GenEdge{genBranchTarget(last), false, false});
            out.push_back(GenEdge{last.ea + last.length, false, false});
            break;
        case Op::Ret: out.push_back(GenEdge{0, false, true}); break;
        default: break;  // Hlt/Int3/Int：没有静态后继（本轮也只会生成 trap）
    }
    return out;
}

namespace {

// ★ 所有 body 发射的**唯一**入口（`emitModule` 与 `emitModuleSplit` 都只经这里）。
//   ★★ 红线（design.md §3.2-3）：`entry_owner` 与 `block_flags` 都在这里按 **全集** 算一次。
//   拆分的每一份**复用**这一份结论；按份重算会让跨份静态 `call` 静默退化成 `ret i32`
//   （`ownerOf` 查不到目标 ⇒ 走派发），并改变 `call` 边的 flags 物化 —— 语义还"等价"，
//   但产物形状与 `s7_*` 读数全变，而且**没有任何东西会喊**。
bool emitBodies(const std::vector<GenComponent>& components,
                const std::set<std::uint32_t>& entry_set,
                const std::set<std::uint32_t>& dropped_blocks, const GenStructure& structure,
                GenEmitStats* stats, std::vector<GenUnsupported>* unsupported,
                std::vector<std::string>* chunks, const GenIrTarget& ir_target) {
    // ★ S7：入口 EA → 单元根（调用目标的归属）。**用 components 自己的 entries**（含 D21 跳表加进来的），
    //   与派发表同一份来源 ⇒ 不会出现"表里有、这里没有"。
    std::map<std::uint32_t, std::uint32_t> entry_owner;
    for (const GenComponent& c : components) {
        for (const std::uint32_t ea : c.entries) entry_owner.emplace(ea, c.root);
    }
    // ★ S7：钩子 EA 必须在**派发边界**上（否则 `ret i32 <EA>` 会在 trampoline 里响亮停）。
    //   `lift-gen` 已经按"这一份模块的入口集"过滤过（小根集模块里可能没有钩子点）；
    //   这里再复核一次，防止将来有人绕过那条过滤 ⇒ 生成期硬错（⛔ 不静默降级成"钩子失效"）。
    for (const std::uint32_t hook : structure.hook_eas) {
        if (entry_owner.count(hook) == 0) {
            std::fprintf(stderr,
                         "emulator-lift-gen: host hook EA %08X is not in the entry set (S7 cannot stay on a dispatch boundary)\n",
                         hook);
            return false;
        }
    }
    // ★ S3-c：**任何** FnEmit 构造之前跑一次块图上的 flags 活性（plan §1.6(b) 的第 1 步）。
    //   失败 = 生成器内部不一致（不动点不收敛）⇒ 返回 false ⇒ `lift-gen` 退出码 1（构建红）。
    std::map<std::uint32_t, BlockFlags> block_flags;
    {
        std::string liveness_err;
        if (!computeFlagsLiveness(components, entry_set, &block_flags, stats, &liveness_err)) {
            std::fprintf(stderr, "emulator-lift-gen: flags liveness computation failed: %s\n", liveness_err.c_str());
            return false;
        }
    }
    for (const GenComponent& comp : components) {
        FnEmit emit(comp, entry_set, dropped_blocks, block_flags, structure, entry_owner, stats,
                    unsupported, ir_target);
        std::string one = emit.run();
        if (emit.failed()) {
            // ★ S3-a：生成期硬错（"读了/要物化一个 Poison 位"= flags 活性自相矛盾）。
            //   ⛔ 不降级成警告：产物一旦带着错的 flags 状态出去，就是**静默**错。
            std::fprintf(stderr, "emulator-lift-gen: hard error during flags emission: %s (component root 0x%08X)\n",
                         emit.failWhy().c_str(), comp.root);
            return false;
        }
        one += "\n";
        chunks->push_back(std::move(one));
    }
    return true;
}

// 模块头 + `declare` 清单（**全局读数**；拆分时每份重复这一段 ⇒ 每份都是独立可编译模块）。
// ★ `stats` 是**全部** body 发射之后的累计值 ⇒ 头部读数与单份模块完全一致（同输入同字节）。
std::string emitPrologue(const GenClosureStats& closure, const GenEmitStats* stats,
                         const GenIrTarget& ir_target) {
    std::string text;
    text += "; emulator-lift · Shape A / T1c —— 由 `emulator-lift-gen` 生成，⛔ 不手改、不 commit。\n";
    text += "; 契约：%st = void*（AOT 臂 = cpu_.r[0] 的别名；夹具 = 真 LiftRegs；r[0..7] + eflags@+32）；%mem = guest EA 0 的宿主基址；\n";
    text += ";       %ea = resume EA；%cpu = X86*；返回值 = 下一个 guest EA。\n";
    text += "; 不 inline 的指令 = 一次 @lift_one（语义在解释器里）；\n";
    text += "; 指令计数 = 块边界 / 宿主调用前的 @lift_tick（D2 / D2b：只记条数，不推进虚拟时间）。\n";
    // ★ S7（D53）：**覆盖率读数**（按 `.ll` 里实际发射的站点数；⛔ 不是估值）。
    //   判据 = 单元根"可一对一结构翻译"（`aot/lift_struct.*` 的 fixpoint），
    //   没转换的站点**保持今天的语义**（`ret i32` 回 trampoline）⇒ 部分完成是安全的。
    text += "; S7 结构翻译（D53）：ret 站点 宿主ret=" + std::to_string(stats->s7_ret_host);
    text += " 回派发器=" + std::to_string(stats->s7_ret_dispatch);
    text += "；call 站点 宿主call=" + std::to_string(stats->s7_call_host);
    text += " lift_dyn=" + std::to_string(stats->s7_call_lift_dyn);
    text += " shim派发=" + std::to_string(stats->s7_call_shim);
    text += " 其余派发=" + std::to_string(stats->s7_call_dispatch);
    text += "；跨函数 jmp 尾巴调用=" + std::to_string(stats->s7_jmp_tail);
    text += " lift_dyn=" + std::to_string(stats->s7_jmp_lift_dyn);
    text += " 派发=" + std::to_string(stats->s7_jmp_dispatch);
    text += "；tail call 站点合计=" + std::to_string(stats->s7_tail_calls) + "\n";
    std::fprintf(stderr,
                 "lift-gen: [S7 结构翻译] ret 站点 宿主ret=%llu 回派发器=%llu · call 站点 宿主call=%llu "
                 "lift_dyn=%llu shim派发=%llu 其余派发=%llu · 跨函数 jmp 尾巴调用=%llu lift_dyn=%llu "
                 "派发=%llu · tail call 合计=%llu\n",
                 static_cast<unsigned long long>(stats->s7_ret_host),
                 static_cast<unsigned long long>(stats->s7_ret_dispatch),
                 static_cast<unsigned long long>(stats->s7_call_host),
                 static_cast<unsigned long long>(stats->s7_call_lift_dyn),
                 static_cast<unsigned long long>(stats->s7_call_shim),
                 static_cast<unsigned long long>(stats->s7_call_dispatch),
                 static_cast<unsigned long long>(stats->s7_jmp_tail),
                 static_cast<unsigned long long>(stats->s7_jmp_lift_dyn),
                 static_cast<unsigned long long>(stats->s7_jmp_dispatch),
                 static_cast<unsigned long long>(stats->s7_tail_calls));
    // ★ D14：静态调用闭包的读数写进产物头部 —— 拿到一份 `.ll` 就能看出它收了多少软根、
    //   丢了多少（⛔ 不让"生成了什么"只存在于 stderr 里）。数字全部由输入确定 ⇒ 同输入同字节。
    text += "; 静态调用闭包（D14；--no-close-calls 关）= ";
    text += closure.close_calls ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.soft_outside);
    text += " 解不出=" + std::to_string(closure.soft_undecodable);
    text += " 重叠=" + std::to_string(closure.soft_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.shim_targets) + "\n";
    // ★ D15：第二类软根（写内存 mov 的 imm32）的读数**独立一行**，三档丢弃同样各一个数
    //   （⛔ 不与 D14 那行合并：两类软根的账要能分别读）。
    text += "; 代码指针立即数（D15；--no-close-stores 关）= ";
    text += closure.close_stores ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.store_soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.store_outside);
    text += " 解不出=" + std::to_string(closure.store_undecodable);
    text += " 重叠=" + std::to_string(closure.store_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.store_shim_targets);
    text += "；写内存 mov 的 imm32 站点=" + std::to_string(closure.store_imm_sites);
    text += "（imm 落在可执行节内=" + std::to_string(closure.store_imm_in_exec) + "）\n";
    // ★ D17：第三类软根（非可执行节里静态初始化的代码指针 = `.data` 指针表）的读数**独立一行**，
    //   三档丢弃同样各一个数（⛔ 不与 D14/D15 那两行合并：三类的账要能分别读）。
    text += "; 静态代码指针（D17；--no-close-data 关）= ";
    text += closure.close_data ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.data_soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.data_outside);
    text += " 解不出=" + std::to_string(closure.data_undecodable);
    text += " 重叠=" + std::to_string(closure.data_overlap);
    text += "（其中收之前就挡下=" + std::to_string(closure.data_overlap_pre) + "）";
    text += "；shim 目标跳过=" + std::to_string(closure.data_shim_targets);
    text += "；非可执行节的 4 字节对齐 dword 站点=" + std::to_string(closure.data_ptr_sites);
    text += "（值落在可执行节内=" + std::to_string(closure.data_ptr_in_exec);
    text += "，不同候选=" + std::to_string(closure.data_ptr_distinct);
    text += "，已在入口集/已判过=" + std::to_string(closure.data_ptr_skipped) + "）\n";
    // ★ G3：分区收敛的护栏读数（⛔ 不是软根）：入口 EA 里**没有块**、因而不可能成为 unit 根的那些
    //   （落在别人块的字节区间内 / 解不出）⇒ 退出入口集，指向它们的边发 `ret i32`。
    //   这一行是为了"⛔ 不静默"：不写它的话，覆盖面缩小这件事在产物里看不见。
    text += "; 入口集收敛护栏（G3）：没有块的入口 EA=" + std::to_string(closure.unbuildable_entries);
    text += "（⇒ 退出入口集、指向它的边发 ret i32）\n";
    // ★ D21（第四类）：可执行节里的跳表 dword = 已建块首 EA、但不在入口集 ⇒ **只加 entries**
    //   （switch case + 派发表行），⛔ 0 新块 / 0 新指令。这一行让"加了几条"在产物里看得见。
    text += "; 跳表入口（D21）：added=" + std::to_string(closure.jump_table_added);
    text += "（只加 entries；⛔ 0 新块 / 0 新指令）";
    text += "；可执行节 4 字节对齐 dword 站点=" + std::to_string(closure.jump_table_sites);
    text += "（值=已建块首=" + std::to_string(closure.jump_table_hits);
    text += "，不同目标=" + std::to_string(closure.jump_table_distinct) + "）";
    text += "；已在入口集=" + std::to_string(closure.jump_table_skipped);
    text += "；孤儿提升=" + std::to_string(closure.jump_table_unclaimed) + "\n";
    // ★ D26（第五类）：可执行节里"值指向**尚未解码**的代码"的 dword ⇒ **新增软根 = 新增块**
    //   （与上一条 D21 相反：D21 只加 entry、0 新块）。候选面五档相加 = `不同候选`。
    text += "; 可执行节跳表目标（D26；--no-close-jmptab 关）= ";
    text += closure.close_jmptab ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.jumptab_soft_roots);
    text += "（新增 unit 根 = **会新增块**；最终仍在入口集=" + std::to_string(closure.jumptab_live_roots) + "）";
    text += "；丢弃 越界=" + std::to_string(closure.jumptab_outside);
    text += " 解不出=" + std::to_string(closure.jumptab_undecodable);
    text += " 重叠=" + std::to_string(closure.jumptab_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.jumptab_shim_targets);
    text += "；不动点轮数=" + std::to_string(closure.jumptab_rounds);
    text += "；候选面：站点=" + std::to_string(closure.jumptab_sites);
    text += "（值在可执行节内=" + std::to_string(closure.jumptab_in_exec);
    text += "，不同候选=" + std::to_string(closure.jumptab_distinct);
    text += "，已在入口集=" + std::to_string(closure.jumptab_in_entries);
    text += "，已是块首=" + std::to_string(closure.jumptab_block_starts);
    text += "，可解码=" + std::to_string(closure.jumptab_decodable);
    text += "，解不出=" + std::to_string(closure.jumptab_undecodable_pre);
    text += "，被重叠挡下=" + std::to_string(closure.jumptab_overlap_pre);
    text += "，shim 桩=" + std::to_string(closure.jumptab_shim) + "）\n";
    // ★ S3-a / S3-b：flags 的表示与融合读数（**全部确定性计数**，⛔ 不是运行期埋点）。
    //   口径：`物化调用` = `materialize(mask != 0)` 的次数；`真的发 IR` = 其中有 lazy 位的
    //   （`mask == 0` 或"六位全 FromBase"⇒ 零 IR，那正是"没有活位"的等价说法）。
    //   `deliver 按位` = 逐位生产者真的算出来的次数；`kill` = 死位（S3-a/b 应恒 0）。
    text += "; EFLAGS 表示（S3-a：六位 i1 槽 + base 字；唯一出口 materialize）：物化调用=";
    text += std::to_string(stats->flags_materialize_calls);
    text += "（真的发 IR=" + std::to_string(stats->flags_materialize_stores) + "）";
    text += "；deliver 按位 cf=" + std::to_string(stats->flags_deliver[0]);
    text += " pf=" + std::to_string(stats->flags_deliver[1]);
    text += " af=" + std::to_string(stats->flags_deliver[2]);
    text += " zf=" + std::to_string(stats->flags_deliver[3]);
    text += " sf=" + std::to_string(stats->flags_deliver[4]);
    text += " of=" + std::to_string(stats->flags_deliver[5]);
    text += "；kill（死位）=" + std::to_string(stats->flags_kill) + "\n";
    // ★ 融合（S3-b：只做 cmp/test；`o/no` 与 `p/np` 退化）。融合读者 = 本步的**收益指标**。
    text += "; 条件融合（S3-b：只做 cmp/test）：融合读者=" + std::to_string(stats->flags_fused);
    text += "（jcc=" + std::to_string(stats->flags_fused_jcc);
    text += " setcc=" + std::to_string(stats->flags_fused_setcc);
    text += " cmovcc=" + std::to_string(stats->flags_fused_cmovcc) + "）";
    text += "；回退：P/NP=" + std::to_string(stats->flags_fallback_pf);
    text += " 多生产者=" + std::to_string(stats->flags_fallback_multi);
    text += " 非cmp/test=" + std::to_string(stats->flags_fallback_other_op);
    text += " 跨块=" + std::to_string(stats->flags_fallback_cross_block);
    text += " 无直读式=" + std::to_string(stats->flags_fallback_not_direct) + "\n";
    // ★ S3-c：块图上的 flags 活性读数（plan §1.8 的"看门处"）。全部确定性计数，⛔ 不是运行期埋点。
    //   判据：`零 flags 块` 应 ≈ 50%（e3flags 3,322/6,549）；`物化站点数` 应从"每条写者一次"降到
    //   "每块尾一次（且只物化活位）"。⚠ c2-lite **之前** `lift_one` 一律 R=W=ALL，含它的块
    //   need_in=ALL ⇒ 实测物化率高于 e3flags 的 0.081；c2-lite 之后差额只剩第 3 类（ALL）的站点。
    text += "; EFLAGS 活性（S3-c：块图上按 need_in/need_out 物化；escape = ALL；lift_one 按 op 的 R/W）：块=";
    text += std::to_string(stats->flags_blocks);
    text += "；零 flags 块（need_out=∅ 且无活写）=" + std::to_string(stats->flags_zero_blocks);
    text += "；need_in=ALL 的块=" + std::to_string(stats->flags_need_in_all);
    text += "；need_out=ALL 的块=" + std::to_string(stats->flags_need_out_all);
    text += "；escape=ALL 的块=" + std::to_string(stats->flags_escape_blocks);
    text += "（ret=" + std::to_string(stats->flags_escape_ret);
    text += " 间接jmp/call=" + std::to_string(stats->flags_escape_indirect);
    text += " 无块/shim/合成/dropped=" + std::to_string(stats->flags_escape_no_block) + "）";
    text += "；静态边落空=" + std::to_string(stats->flags_edge_no_block) + "\n";
    text += ";   块尾物化位数按位：cf=" + std::to_string(stats->flags_tail_bits[0]);
    text += " pf=" + std::to_string(stats->flags_tail_bits[1]);
    text += " af=" + std::to_string(stats->flags_tail_bits[2]);
    text += " zf=" + std::to_string(stats->flags_tail_bits[3]);
    text += " sf=" + std::to_string(stats->flags_tail_bits[4]);
    text += " of=" + std::to_string(stats->flags_tail_bits[5]);
    text += "（合计=";
    {
        std::uint64_t total = 0;
        for (int b = 0; b < 6; ++b) total += stats->flags_tail_bits[b];
        text += std::to_string(total);
    }
    text += "）；屏障（lift_one）站点=" + std::to_string(stats->lift_one_sites);
    text += "；融合漏判（应恒 0）=" + std::to_string(stats->flags_fuse_missed) + "\n";
    // 同一条读数也打到 stderr（要求：⛔ 不静默；`.ll` 头部之外也要能看见）。
    std::fprintf(stderr,
                 "lift-gen: [EFLAGS 活性 S3-c] 块=%llu 零 flags 块=%llu need_in=ALL=%llu "
                 "need_out=ALL=%llu escape=%llu（ret=%llu 间接=%llu 无块=%llu）静态边落空=%llu · "
                 "物化站点=%llu（真的发 IR=%llu）· 块尾位数 cf/pf/af/zf/sf/of=%llu/%llu/%llu/%llu/%llu/%llu · "
                 "屏障站点=%llu · 融合读者=%llu\n",
                 static_cast<unsigned long long>(stats->flags_blocks),
                 static_cast<unsigned long long>(stats->flags_zero_blocks),
                 static_cast<unsigned long long>(stats->flags_need_in_all),
                 static_cast<unsigned long long>(stats->flags_need_out_all),
                 static_cast<unsigned long long>(stats->flags_escape_blocks),
                 static_cast<unsigned long long>(stats->flags_escape_ret),
                 static_cast<unsigned long long>(stats->flags_escape_indirect),
                 static_cast<unsigned long long>(stats->flags_escape_no_block),
                 static_cast<unsigned long long>(stats->flags_edge_no_block),
                 static_cast<unsigned long long>(stats->flags_materialize_calls),
                 static_cast<unsigned long long>(stats->flags_materialize_stores),
                 static_cast<unsigned long long>(stats->flags_tail_bits[0]),
                 static_cast<unsigned long long>(stats->flags_tail_bits[1]),
                 static_cast<unsigned long long>(stats->flags_tail_bits[2]),
                 static_cast<unsigned long long>(stats->flags_tail_bits[3]),
                 static_cast<unsigned long long>(stats->flags_tail_bits[4]),
                 static_cast<unsigned long long>(stats->flags_tail_bits[5]),
                 static_cast<unsigned long long>(stats->lift_one_sites),
                 static_cast<unsigned long long>(stats->flags_fused));
    text += "\n";
    // ★ 本单：**函数属性组**（`#0`）—— 注释头之后、`declare` 之前，**恰好一行**。
    //   · 值**原样**拼进去（不转义）：`--ir-target-*` 从 configure 期的 clang 探针来，
    //     与运行库 TU 的函数属性串**逐字相同**（mac 期望读数见 `.tmp/codegen/attrs-min.txt`）。
    //   · ⛔ 只在 `ir_target` 两个值都非空时发：任一为空 ⇒ 产物与今天**逐字节相同**
    //     （既有 C++ 测试 / `--check` / golden 夹具都直接调这些 API，不能因为"顺手对齐"变字节）。
    //   · 为什么这行是 LTO 内联的**唯一**开关（不是 codegen 开关）：LLVM 的
    //     `areInlineCompatible()` 比函数属性，一侧缺 ⇒ callee 的 feature 不是 caller 的子集 ⇒
    //     `lift_tick` / `lift_note_write` 一条都内联不进 AOT（`.tmp/lto/report.md` §5.2/§5.3）。
    if (!ir_target.empty()) {
        text += "attributes #0 = { \"target-cpu\"=\"" + ir_target.cpu + "\" \"target-features\"=\"" +
                ir_target.features + "\" }\n";
    }
    // ★ E2 / S1：给两个**逐指令**宿主原语加内存属性 —— 这是"clang 愿不愿把 `%st` 保活"的许可实验。
    //   `argmemonly`：`lift_tick` 只碰 `cpu->insns`、`lift_note_write` 只碰 `cpu->cache`
    //   （`aot_host.cpp:60` 把 cache 置 nullptr ⇒ AOT 里它连那一层都不碰）⇒ 两者**只访问指针参数
    //   所指的内存**，属性成立（⚠ 证明来自它们的实现 + AOT 的装配，⛔ 不是"看起来像"）。
    //   `willreturn`：两者都不循环（只有空指针 ⇒ `_Exit`，那是调用方编程错误路径）。
    //   `nounwind`：两者都不抛（`lift_tick` 只在错误路径 `fprintf` + `_Exit`）。
    //   ⛔ **`lift_one` 不加 `argmemonly`**：它经由 `cpu->mem` 读写 guest 内存 —— 那是"参数里**存的**
    //      指针"指向的内存（两层），不是"参数所指的内存"；而 `noalias`（plan §2.1.1 的第 4 条）
    //      一上，LLVM 就能证明 guest 内存与 `%st`/`%cpu` 不相交 ⇒ 带 `argmemonly` 的 `lift_one`
    //      会被当成"不碰 guest 内存"⇒ **静默重排/删 store**。今天没加 `noalias` 才看不出问题
    //      （见 §S1 的探针：`argmemonly` 单独上时 clang 连一条 `%st` 的 load 都消不掉）。
    //   `willreturn` 对 `lift_one` 也不成立（`execInsn` 会 `_Exit` / 报停机）。
    if (stats->used_note_write)
        text += "declare void @lift_note_write(ptr, i32, i32) argmemonly nounwind willreturn\n";
    // ★ 2026-11-22：`rep movs*` / `rep stos*` 的整段快路径（闸门 = `runtime/rep_bulk.h`）。
    //   返回 `i1`：`true` = 闸门放行、整段已搬完（生成代码跳过逐元素 IR）；`false` = 拒绝 ⇒ 落回循环。
    //   ⚠ 声明**无属性**（⛔ `argmemonly` / `readnone` / `nounwind`）：它经由 `cpu->mem` 读写 guest 内存
    //   （"参数里**存的**指针"那一层），与 `lift_one` / `lift_x87_sem` 同一条理由；它还会推进
    //   ECX/ESI/EDI 并把 `rep_bulk_hits/declines` 记进 `cpu`。给属性 = 允许 LLVM 重排/删掉访存。
    if (stats->used_rep_bulk) text += "declare i1 @lift_rep_bulk(ptr, ptr, i32, i32, i32)\n";
    if (stats->used_tick) text += "declare void @lift_tick(ptr, i32) argmemonly nounwind willreturn\n";
    if (stats->used_lift_one) text += "declare i1 @lift_one(ptr, ptr, i32)\n";
    // ★ 2026-11-15（本轮：`lift_one` 调用点 → 0）：停机原语。**不加属性**（它写 `%cpu` 的
    //   `running` / `stop_why`，而且它是"停机面"本身；`readnone`/`argmemonly` 都是假前提）。
    if (stats->used_lift_stop) text += "declare void @lift_stop(ptr, i32, i32)\n";
    // ★ MMX/SSE 的饱和加减（`llvm.sadd.sat` / `uadd.sat` / `ssub.sat` / `usub.sat`）：
    //   语义与解释器 `pkClamp`（`exec_fpu.cpp:502-504`）的三个分支逐位相同；有 Packed 站点就全声明。
    if (stats->packed_sites != 0) {
        text += "declare <16 x i8> @llvm.sadd.sat.v16i8(<16 x i8>, <16 x i8>)\n";
        text += "declare <8 x i16> @llvm.sadd.sat.v8i16(<8 x i16>, <8 x i16>)\n";
        text += "declare <16 x i8> @llvm.uadd.sat.v16i8(<16 x i8>, <16 x i8>)\n";
        text += "declare <8 x i16> @llvm.uadd.sat.v8i16(<8 x i16>, <8 x i16>)\n";
        text += "declare <16 x i8> @llvm.ssub.sat.v16i8(<16 x i8>, <16 x i8>)\n";
        text += "declare <8 x i16> @llvm.ssub.sat.v8i16(<8 x i16>, <8 x i16>)\n";
        text += "declare <16 x i8> @llvm.usub.sat.v16i8(<16 x i8>, <16 x i8>)\n";
        text += "declare <8 x i16> @llvm.usub.sat.v8i16(<8 x i16>, <8 x i16>)\n";
    }
    // ★ S7（D53）：`lift_dyn(cpu, st, ea)` = 查派发表 + 尾调用进目标（实现见 `runtime/lift_abi.cpp`）。
    //   ⚠ 声明**无属性**（⛔ `argmemonly` / `noalias` / `readnone`）：它经由 `cpu->mem` 读写
    //   guest 内存（"参数里**存的**指针"那一层），与 `lift_one` 同一条理由 —— 一上属性就是
    //   静默数据损坏的许可。
    if (stats->used_lift_dyn) text += "declare i32 @lift_dyn(ptr, ptr, i32)\n";
    // ★ 检查点 2（D49）：x87 的宿主 helper 与它用到的 LLVM 内建。
    //   `@lift_x87_sem(ptr %cpu, i32 <Op>)` 的实现 = 解释器的 `execFpu`（`exec_fpu.cpp`，同一份语义）
    //   ⇒ 语义只有一处；这里只声明。⚠ **不加任何属性**：它读写 `%cpu` 指向的状态（两层指针），
    //   与 `lift_one` 同一条理由（`argmemonly` / `readnone` 会变成可被合法利用的假前提）。
    if (stats->x87_helper_sites != 0) text += "declare void @lift_x87_sem(ptr, i32)\n";
    if (stats->x87_inline_sites != 0) {
        // 取整 / 开方 / 取绝对值：**只声明用得到的**会给发射器加一堆记账 ⇒ 有 x87 内联站点就全声明
        // （未使用的 `declare` 是合法的，且 clang 不会为它生成任何代码）。
        text += "declare double @llvm.rint.f64(double)\n";
        text += "declare double @llvm.floor.f64(double)\n";
        text += "declare double @llvm.ceil.f64(double)\n";
        text += "declare double @llvm.trunc.f64(double)\n";
        text += "declare double @llvm.sqrt.f64(double)\n";
        text += "declare double @llvm.fabs.f64(double)\n";
    }
    text += "declare void @llvm.trap()\n";
    if (stats->used_ldexp) text += "declare double @llvm.ldexp.f64(double, i32)\n";
    if (stats->used_ctlz64) text += "declare i64 @llvm.ctlz.i64(i64, i1)\n";
    if (stats->used_ctpop) text += "declare i32 @llvm.ctpop.i32(i32)\n";
    if (stats->used_cttz) text += "declare i32 @llvm.cttz.i32(i32, i1)\n";
    if (stats->used_ctlz) text += "declare i32 @llvm.ctlz.i32(i32, i1)\n";
    text += "\n";
    return text;
}

}  // namespace

bool emitModule(const std::vector<GenComponent>& components,
                const std::set<std::uint32_t>& entry_set,
                const std::set<std::uint32_t>& dropped_blocks, const GenClosureStats& closure,
                const GenStructure& structure, std::string* out, GenEmitStats* stats,
                std::vector<GenUnsupported>* unsupported, const GenIrTarget& ir_target) {
    if (out == nullptr || stats == nullptr || unsupported == nullptr) return false;
    std::vector<std::string> chunks;
    if (!emitBodies(components, entry_set, dropped_blocks, structure, stats, unsupported, &chunks,
                    ir_target))
        return false;
    std::string text = emitPrologue(closure, stats, ir_target);
    for (const std::string& c : chunks) text += c;
    *out = std::move(text);
    return true;
}

bool emitModuleSplit(const std::vector<GenComponent>& components,
                     const std::set<std::uint32_t>& entry_set,
                     const std::set<std::uint32_t>& dropped_blocks, const GenClosureStats& closure,
                     const GenStructure& structure, std::size_t split, GenEmitStats* stats,
                     std::vector<GenUnsupported>* unsupported, std::vector<std::string>* parts,
                     std::vector<std::vector<std::uint32_t>>* part_roots,
                     const GenIrTarget& ir_target) {
    if (stats == nullptr || unsupported == nullptr || parts == nullptr || part_roots == nullptr)
        return false;
    if (split == 0) split = 1;
    if (components.empty()) {
        parts->assign(split, std::string());
        part_roots->assign(split, std::vector<std::uint32_t>());
        return true;
    }
    if (split > components.size()) split = components.size();
    // ★ 全局分析一次（entry_owner / block_flags / body 全部在这里；⛔ 不按份重算）。
    std::vector<std::string> chunks;
    if (!emitBodies(components, entry_set, dropped_blocks, structure, stats, unsupported, &chunks,
                    ir_target))
        return false;
    // ★ 分桶（确定性纯函数）：分量根 EA 升序、贪心填到 `Σbody/split`。
    //   `components` 本身已按根升序 ⇒ 用下标顺序即可；这里仍显式排序，防上游顺序变动改变产物。
    std::vector<std::size_t> order(components.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return components[a].root < components[b].root;
    });
    std::uint64_t total = 0;
    for (const std::string& c : chunks) total += static_cast<std::uint64_t>(c.size());
    const std::uint64_t target = (total + split - 1) / split;
    std::vector<std::vector<std::size_t>> buckets(split);
    std::size_t bi = 0;
    std::uint64_t acc = 0;
    for (const std::size_t idx : order) {
        const std::uint64_t sz = static_cast<std::uint64_t>(chunks[idx].size());
        if (!buckets[bi].empty() && acc + sz > target && bi + 1 < split) {
            ++bi;
            acc = 0;
        }
        buckets[bi].push_back(idx);
        acc += sz;
    }
    const std::string prologue = emitPrologue(closure, stats, ir_target);
    parts->clear();
    part_roots->clear();
    for (std::size_t b = 0; b < split; ++b) {
        std::vector<std::size_t> idxs = buckets[b];
        // 桶内按**原始分量顺序**拼接（与单份文件的字节顺序一致；`std::sort` 稳定化不需要）。
        std::sort(idxs.begin(), idxs.end());
        std::string body;
        std::set<std::string> defined;
        for (const std::size_t idx : idxs) {
            body += chunks[idx];
            defined.insert(genFunctionName(components[idx].root));
        }
        // ★ 跨份静态 `call` / `tail call` 的目标在本份里**只有声明**（定义在别的份里）：LLVM 要求
        //   使用前可见 ⇒ 每份为自己的"外部 callee"补 `declare i32 @lift_cc_<root>(ptr, ptr, i32, ptr)`。
        //   签名与定义**逐字相同**（`FnEmit::run` 的 `define` 那一行）；名字集合 = body 里出现过的
        //   `@lift_cc_*` 减掉本份自己的定义（`std::set` ⇒ 升序 ⇒ 确定性）。
        //   ⚠ 单份（`split == 1`）时差集恒空 ⇒ 产物与今天的 `.ll` **逐字节相同**。
        std::set<std::string> referenced;
        for (std::size_t pos = body.find("@lift_cc_"); pos != std::string::npos;
             pos = body.find("@lift_cc_", pos + 1)) {
            std::size_t end = pos + 9;  // skip "@lift_cc_"（9 字符）
            while (end < body.size() &&
                   std::isxdigit(static_cast<unsigned char>(body[end])) != 0) {
                ++end;
            }
            referenced.insert(body.substr(pos + 1, end - pos - 1));  // 去掉 '@'
        }
        std::string extra;
        for (const std::string& name : referenced) {
            if (defined.count(name) != 0) continue;
            extra += "declare i32 @" + name + "(ptr, ptr, i32, ptr)\n";
        }
        std::string text = prologue;
        if (!extra.empty()) {
            // 插在 prologue 末尾那个空行**之前**（declare 与 body 之间仍隔一空行）。
            text = prologue.substr(0, prologue.size() - 1) + extra + "\n";
        }
        std::vector<std::uint32_t> roots;
        roots.reserve(idxs.size());
        for (const std::size_t idx : idxs) roots.push_back(components[idx].root);
        text += body;
        parts->push_back(std::move(text));
        part_roots->push_back(std::move(roots));
    }
    return true;
}

}  // namespace aot
}  // namespace lift
