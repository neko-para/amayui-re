// emulator-lift · Shape A 原型阶段 A 的 IR ↔ runtime ABI（**唯一一份** C 链接面）
//
// ★ 为什么需要它：`.ll` 不能 `declare` C++ 修饰名（`shims.h` / `native_overrides.h` 都在
//   `namespace lift` 里）⇒ 契约面必须是 `extern "C"` 的薄层，语义一行不写、只转发
//   （`runtime-contract.md` §0-1 与 §7.0）。
// ★ 字段集合闭合（复审 §5.2）：lifted 代码**只许**碰 8 GPR + EFLAGS。`X86` 里还有
//   `mem` / `shims` / `running` / `stop_kind` / 服务指针 —— 在 `.ll` 里一个下标算错就是
//   **静默**改宿主状态。用 POD 把"可碰的东西"变成**类型**，不靠纪律。
// ★ 搬运函数**逐字段**写（正确性与布局无关）；下面的 `offsetof` 断言只保护"`.ll` 里的偏移假设"。
//   ⛔ 不要把正确性押在 `offsetof` 上：它只钉"当前布局"，钉不住"搬运写对了"。
// ★ lifted 代码的宿主调用只有这几个（每个都只转发，语义一行不写）：
//   · `lift_note_write`（SMC 失效）→ `lift::noteGuestWrite`（`x86.h:46` / `x86.cpp:263`），
//     与解释器 `X86::writeMem` 同一份语义；
//   · `lift_tick` / `lift_one` / `lift_fs_base`（T1c，见下）：计数、单指令执行、段基址；
//   · `lift_shim`（阶段 B）：走进一个导入桩。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "runtime/x86.h"  // X86 的完整类型（`liftRegsView` 的内联定义需要它）

namespace lift {
struct X86;
}

// r[0..7] = EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI（== GprIndex）；eflags 用**真位位置**
struct LiftRegs {
    std::uint32_t r[8];
    std::uint32_t eflags;
};
static_assert(sizeof(LiftRegs) == 36, "LiftRegs 必须 36 B");
static_assert(offsetof(LiftRegs, r) == 0, "LiftRegs.r 必须在 +0（.ll 的偏移假设）");
static_assert(offsetof(LiftRegs, eflags) == 32, "LiftRegs.eflags 必须在 +32（.ll 的偏移假设）");
static_assert(std::is_trivially_copyable_v<LiftRegs>, "LiftRegs 必须可平凡拷贝（跨 ABI）");

// ★ A′（2026-11-20）：AOT 臂把 `%st` **直接指到** `cpu_.r[0]`（`X86` 的前 36 B 与 `LiftRegs`
//   逐字节同布局：r@0 / eflags@32，eip 在 +36 之后）。⇒ 每派发那两次整块搬运（22 条）结构上消失。
//   ★ 为什么返回 `void*` 而不是 `LiftRegs*`：`reinterpret_cast<LiftRegs*>(&cpu)` 之后由 C++ 解引用
//     是**严格别名 UB**（`X86` 不是 standard-layout，没有 common initial sequence）。返回 `void*`
//     ⇒ **不形成任何指向 `X86` 的 `LiftRegs*`**，别名判据退化成两个 `void*` 的比较（无对象访问）。
//   ★ 非别名调用方（生成物夹具 / 音频 feeder）用的是自己的真 `LiftRegs` ⇒ 搬运路径逐字不变。
namespace lift {
inline void* liftRegsView(X86& c) { return static_cast<void*>(&c.r[0]); }
}  // namespace lift

// SMC 失效（转 x86.h 的 noteGuestWrite；`cpu == nullptr` ⇒ no-op，与 cache==nullptr 同口径）
extern "C" void lift_note_write(void* cpu, std::uint32_t ea, std::uint32_t n);

// ★ `rep movs*` / `rep stos*` 的**整段快路径**（AOT 臂的入口；闸门 = `runtime/rep_bulk.h` 的
//   `repBulkPlan`，**唯一真源**，这里一个字都不重写）。
//   ① 生成代码在 `rep` 站点发**一次**调用，`kind ∈ {movs, stos}`、`size` = 元素宽度（1/2/4）；
//   ② `true`  = 闸门放行 ⇒ ECX / ESI / EDI / guest 内存都已写好（EFLAGS 未动）⇒ **跳过逐元素 IR**；
//      `false` = 闸门拒绝 ⇒ 生成代码照旧跑逐元素 IR（那条路与改动前逐字节相同）。
//   ③ `st` 与 `lift_one` 同口径：AOT 臂 = `liftRegsView(cpu)`（别名 ⇒ 搬运跳过）；夹具 / feeder 是
//      真 `LiftRegs` ⇒ 调用前后整块物化 / 回读（否则会在过期寄存器上判、再把结果丢掉）。
//   ④ **不**改 `insns`（计数只由 `lift_tick` 负责；`rep` 在两条臂上都算**一条**，见 `lift_tick` 注）。
//   ⑤ 声明**无属性**（⛔ `argmemonly` / `readnone`）：它经由 `cpu->mem` 读写 guest 内存（两层指针），
//      与 `lift_one` / `lift_x87_sem` 同一条理由。
//   ⚠ 拒绝路径不写 guest 状态（`rep_bulk.h` 的 `repBulkTry` 契约）⇒ 生成代码在合并点之后读到的是
//      调用前的寄存器。但发射器仍选择"慢路径重新 `loadReg`"（见 `lift_gen_emit.cpp` 的 `emitString`）。
extern "C" bool lift_rep_bulk(void* cpu, void* st, std::uint32_t ea, std::uint32_t kind,
                              std::uint32_t size);
// `kind` 的闭集合（⛔ 不要用 `Op` 的枚举值：那是 runtime 内部编号，改了就会静默对不上）。
inline constexpr std::uint32_t kLiftRepMovs = 0;
inline constexpr std::uint32_t kLiftRepStos = 1;

// =====================================================================================
// ★ 构建期**库内建函数**（v1：静态 CRT 的 `_memcpy` @ 0x004E74B0；`aot/intrinsics.txt`）
//
// 形状 = 单元入口的**带闸门快路径 + 回退到原 lifted 函数体**（与 `lift_rep_bulk` 同一类，
// ⛔ 不是运行期 override）：
//   ① 生成代码在 `_memcpy` 的单元根入口把 `dst/[esp+4]`、`src/[esp+8]`、`n/[esp+12]` 读出来，
//      发**一次** `@lift_intrinsic_memcpy_gate(cpu, st, dst, src, n)`；
//   ② `true`  ⇒ 发 `@lift_intrinsic_memcpy_do(cpu, dst, src, n)`（宿主 `memmove`），
//      然后由 **IR** 写 `EAX = dst`、`resume = mem32[esp]`、`esp += 4`，`ret i32 %resume`；
//      `false` ⇒ `br` 到原函数的入口块（逐字节不变的原 lifted 函数体）。
//   ③ 闸门**只读**（不写任何 guest 状态）；`--no-intrinsics` 只让它返回 false（A/B 与诊断）。
//   ④ ⛔ ECX / EDX 的残留值**不拟合**：cdecl 调用者不依赖被调方留下的它们（显式裁决，见
//      `runtime/lift_intrinsics.h` 的头注）。`EBX/ESI/EDI/EBP` 不动；`ESP` 只 `+4`；`EFLAGS` 不碰。
// =====================================================================================
extern "C" bool lift_intrinsic_memcpy_gate(void* cpu, void* st, std::uint32_t dst,
                                           std::uint32_t src, std::uint32_t n);
extern "C" void lift_intrinsic_memcpy_do(void* cpu, std::uint32_t dst, std::uint32_t src,
                                         std::uint32_t n);

// =====================================================================================
// T1c 的三个宿主原语（父级裁决 D2 / D2b / D10-P2 / D11）
// ★ 共同口径：`.ll` 里**没有任何解释器类型**，只有一次 C 调用；语义一行不写、只转发。
// =====================================================================================

// ① 指令计数（`cpu->insns += n`）。
//   ★ 为什么需要它：解释器是**逐条** `++cpu_.insns`（`machine.cpp:1727`），而一次 `LiftFn` 调用
//     执行**动态条数**的 guest 指令 ⇒ `.ll` 自己数出来的只是"静态条数"，且 `LiftRegs`（36 B）
//     不可扩。生成器在**块边界**发一次静态条数、在**任何宿主调用之前**把挂账的先 flush 掉，
//     于是任何观测点（一次 shim 调用 = guest 读时钟）看到的 `insns` 都是精确的。
//   ★ D2b：时钟 = **wall clock** ⇒ 这里**只**记条数，⛔ 不推进虚拟时间（那部分是恒等操作）。
//   ★ `rep` 类按**1 条**算（与解释器同一口径：`execString` 在一条 `Insn` 里循环元素，
//     `machine.cpp` 的 `++cpu_.insns` 一条一次）。
//   `cpu` **必须非空**；Debug 档 `assert` 点名，Release/NDEBUG 下零代码零分支。
//   ★ 2026-11-17 用户裁决：旧的"`cpu == nullptr` ⇒ 响亮退出"分支已删 —— 唯一调用方是生成物
//     （恒传 `&AotRunner::cpu_`），`runtime/` 与 `tests/` 零调用，也没有测试断言过 null 行为。
//     ⛔ 不要把这个判空加回来：它在 10 万级 tick 站点上是一条热路径 `cbz`。
extern "C" void lift_tick(void* cpu, std::uint32_t n);

// ② 单指令宿主原语：把**这一条** guest 指令交给既有执行器（`Decoder` + `execInsn`）执行一次。
//   § 为什么要它（D10 P1/P2 / D11 / D12）：生成器只 inline 实现了 ~30 个 op；剩下的（x87 1022 /
//     `fs:` 475 / 串操作 85 / Div 53 / 循环移位 58 / Imul 169 …）手写第二份语义是**静默偏差**的
//     高发区（P1 的移位 PF 就是同类事故的现成例子）。⇒ 一条指令一次宿主调用，**语义只有一份**。
//   § 为什么有 `st` 这个参数（与 `lift_shim` 的 `%st` 同一条理由，**不能省**）：
//     lifted 代码把 guest 状态放在 `%st` 里（AOT 臂 = `cpu_.r[0]` 的别名；夹具 / feeder = 真
//     `LiftRegs`），而 `execInsn` 吃的是 `X86`。只给 `cpu` 就会**在过期的寄存器上执行、再把结果
//     丢掉**（静默错）⇒ 这里做与 `lift_shim` 完全一样的三件事：① `st → cpu`；② 执行；③ `cpu → st`。
//     ★ A′ 之后 `st` 是 `void*`；别名时那两步是自拷贝 ⇒ 跳过（判据见 `liftRegsView`）。
//   § 边界（本轮**必须**遵守）：
//     · ⛔ 只用于**非控制转移**指令（`jmp/jcc/call/ret` 由生成器自己 inline / 回 trampoline）。
//       生成器显式判定；控制转移掉进这里是生成器 bug（那边仍然是 `llvm.trap`）。
//     · 执行前 `saved = cpu.eip; cpu.eip = ea;`，执行后 `cpu.eip = saved` ⇒ "单条执行"不会打乱
//       生成器的块内推进（`execInsn` 对控制转移会改 eip —— 那正是要挡在外面的东西）。
//     · **不**改 `insns`（计数只由 `lift_tick` 负责，⛔ 不许双重计数）。
//     · `execInsn` 返回 false（`Hlt/Int3/Int/Other/Invalid` 与 `Div/Idiv` 的 #DE … 它自己拒绝的）
//       ⇒ 它已经把 `running=false` / `stop_why` 置好，这里再补一行点名 EA + op 的诊断并返回
//       false（生成器收到 false ⇒ `ret i32 kLiftHalt`，trampoline 报 stop_kind/stop_why）。
//     · 宿主没装配（`hostOf(cpu) == nullptr` 或 `cpu.mem == nullptr`）⇒ **响亮停**并返回 false
//       （⛔ 不许静默返回 true —— 那会把"没执行"变成"执行成功"）。
extern "C" bool lift_one(void* cpu, void* st, std::uint32_t ea);

// ★ **采样式内部 profile 的唯一接缝**（AOT 的 `--profile`）：`lift_one` 是**生成物直接 call**
//   的（`lift_gen_emit.cpp` 的 `call i1 @lift_one`），宿主一次都看不见 ⇒ 单次耗时的采样只能在这里。
//   ★ 三条硬约束（父级裁决 (A)）：
//     ① **关档零成本**：热路径上只有**一次指针判空**（`&&` 短路 ⇒ 计数器 / 取时都不执行）；
//     ② 默认 `nullptr`，解释器臂（`emulator-lift`）**永不设置它** ⇒ 解释器行为逐字不变；
//     ③ **不改 `lift_one` 的语义**：只在它那四段之间插取时（+ 出口一次），⛔ 不提前、不延后、
//        不改它读写什么（它是 S3 设计里的"整字屏障"）；取时本身只在**采样到的**那一次执行。
//   ★ 采样率 = 每 `kLiftOneProfileStride` 次调用一次；计数器在 `lift_abi.cpp`（宿主不去数）。
namespace lift {
// ★ 一次采样的**四段归因**（只给 `--profile` 的读数用；⛔ 不进任何判据、⛔ 不改语义）：
//   [0] A 换入 = `liftRegsToCpu` + `saved_eip` / `cpu.eip = ea`
//   [1] B 解码 = `Insn` / `std::string why` 构造 + `Decoder::decode`（含解码缓存查找）
//   [2] C 执行 = `noteInsn` + `opName`（采样时才取）+ `execInsn`
//   [3] D 回读 = `liftCpuToRegs` + `cpu.eip` 复原
//   ★ 四段之和 == `ns`（每段都是相邻两次取时之差）⇒ 一段没算漏、也没重复算。
//   ⚠ 每段都含一次 `liftOneProfileNowNs()`（实测 ~20 ns/次，见 `freq: 取时开销`）⇒
//     短段的读数有那个量级的地板，⛔ 不要把 5 ns 的差当成结论。
struct LiftOneProfileSample {
    std::uint32_t ea = 0;
    std::uint64_t ns = 0;          // 整条 `lift_one`（= 四段之和）
    const char* op = "?";          // `opName(in.op)`；解码失败时是 "?"
    std::uint64_t phase_ns[4] = {};  // A / B / C / D
};
using LiftOneProfileHook = void (*)(const LiftOneProfileSample&);
// ★ `thread_local`：`lift_one` 可能在**两条宿主线程**上被调（AOT 主线程 + 音频 feeder 的 IR
//   执行器）。采样钩子与它的计数器都必须**每线程一份**：
//   · 主线程（`AotHost::beginProfile` 设置）照常采样，读数只反映主线程的 lifted 代码；
//   · feeder 线程的副本恒为 nullptr ⇒ 不采样、不写主线程的 `AotProfile`（否则是数据竞争）。
//   ⛔ 不要改回全局：那会让"窗口 profile 与音频同开"变成 UB 级隐患。
extern thread_local LiftOneProfileHook liftOneProfileHook;
inline constexpr std::uint64_t kLiftOneProfileStride = 1024;
// 只在钩子非空时被调用（关档不执行）：这一次调用是否该采样 / 采样用的单调时钟（ns）。
// ★ 计数器也每线程一份（见上）：feeder 的 `lift_one` 调用不推进主线程的采样相位。
bool liftOneProfileSampleDue();
std::uint64_t liftOneProfileNowNs();
}  // namespace lift

// ②b 停机原语（2026-11-15，本轮：`lift_one` 调用点 → 0）。
//   § 为什么需要它：`Hlt` / `Int3` / `Int` / `Op::Other`（助记符认识但没实现）在解释器里
//     **恒**走"响亮停"（`exec_control.cpp:62-72` 的 `hlt`/`trapInt`、`exec.cpp:339-371` 的
//     `unimplemented`）⇒ 它们不需要"解释执行一条指令"，只需要**把同一条停机面表达出来**。
//     以前它们走 `lift_one`（一次宿主调用 + 一次解码 + `execInsn` → 立刻 false），
//     生成的 `.ll` 里因此留下 `call i1 @lift_one` —— 而这条路径的语义**不是**"翻译"。
//   § 停机面**逐字**照解释器（唯一判据）：`running = false` + `stop_why = <同一句字符串>`
//     （`cpuFault` / `trapInt` / `unimplemented` 都是这两步）。⛔ 不写 `stop_kind`
//     （解释器也不写 ⇒ AOT 的 `hostStopReason` 归到 `Stopped`，与今天**逐位相同**）。
//   § 打印：只打一行 `=== guest stop ===` + why + EA（诊断面；⛔ 不进任何判据）。
//   § 返回：`void`。发射器随后发 `ret i32 kLiftHalt`（trampoline 的停机口径不变）。
//   `kind` ∈ `LiftStopKind`（闭集合，与解释器的停机点一一对应，见 `.cpp`）。
enum LiftStopKind : std::uint32_t {
    kLiftStopHlt = 0,        // `exec_control.cpp:63-65`：stop_why = "hlt"
    kLiftStopInt3 = 1,       // `exec_control.cpp:67-69` → trapInt
    kLiftStopInt = 2,        // `exec_control.cpp:70-72` → trapInt
    kLiftStopOther = 3,      // `exec.cpp:210` → unimplemented（助记符认识但没实现）
    kLiftStopInvalid = 4,    // `exec.cpp:209` → unimplemented（解码得到 Invalid）
    kLiftStopDivZero = 5,    // `exec_integer.cpp:273/278` → cpuFault("除零（#DE）")
    kLiftStopDivOverflow = 6,    // `exec_integer.cpp:312/319/326` → cpuFault("div 商溢出（#DE）")
    kLiftStopIdivOverflow = 7,   // `exec_integer.cpp:281/290/293/300/303` → cpuFault("idiv 商溢出（#DE）")
    // MOVAPS/MOVDQA 的 m128 未按 16 字节对齐（`exec_fpu.cpp:1010-1016` → unimplemented）
    kLiftStopSseAlign = 8,
};
extern "C" void lift_stop(void* cpu, std::uint32_t ea, std::uint32_t kind);

// ③ 段基址（`fs:` / `gs:` 覆盖的 EA 计算要用）。解释器臂的真源是 `X86::fs_base` / `gs_base`
//   （`x86.cpp` 的 `eaOf`：`ea = base[index] + disp + …`）。
//   ★ 本轮生成器**不**用它：`fs:`/`gs:` 操作数一律走 `lift_one`（D10-P2 / D12：不 inline 化）。
//     它是给"以后把 `fs:` inline 成一条宿主调用"留的接缝，语义与 `lift_one` 完全同源。
//   ★ 与 `lift_one` 同口径：`cpu == nullptr` ⇒ 响亮退出（⛔ 不返回 0 冒充"段基址是 0"）。
extern "C" std::uint32_t lift_fs_base(void* cpu);
extern "C" std::uint32_t lift_gs_base(void* cpu);

// ★ **停机哨兵**（阶段 B）：`lift_shim` 返回它 = 宿主 shim 报了停机（`cpu.running == false`，
//   例如未实现导入 / 条件 trap）。trampoline 见到它**必须停**并把 `cpu.stop_kind` / `stop_why`
//   报出来，⛔ 不许静默当成"跑完了"（那会把一次响亮停机变成一次假绿）。
//   与"回到 trampoline 的哨兵" 0xFFFFFFFF 区分开：那个是**正常**结局（guest 自己 ret 到栈哨兵）。
inline constexpr std::uint32_t kLiftHalt = 0xFFFFFFFEu;

// =====================================================================================
// ★ S7（D53）：**动态入口** `lift_dyn(cpu, st, ea)` —— 查派发表 + **尾调用**进目标，
//   ⛔ 不回宿主主循环。
//   生成代码只在"目标运行期才知道"的站点发它（间接 `jmp` / 间接 `call reg|mem`）：
//     · 间接 `jmp`：`%r = tail call i32 @lift_dyn(...)` + `ret i32 %r` ⇒ 真尾调用（LLVM 落成 jmp）；
//     · 间接 `call`：`%r = call i32 @lift_dyn(...)`（★ **普通 call**，⛔ 不是 `tail`：`%r` 要被
//       `emitCallReturnCheck` 的 `icmp` 使用 ⇒ 该点不在尾位置），`%r == resume EA` 才在本宿主帧里
//       继续（见 `lift_gen_emit.cpp` 的 `emitCallReturnCheck`：guest `ret` 弹的就是我们压的那一格
//       ⇒ 判据就是"返回 EA == resume"）。**进入目标**的那一跳仍由本函数内部的 `return fn(...)` 完成，
//       那一处才是真尾调用（见 .cpp 的 musttail 注记）。
//   ★ 返回 = 目标的返回值（= 下一个 guest EA）；目标不在派发表 ⇒ 响亮停并返回 `kLiftHalt`。
//   ★ 声明**无属性**（⛔ `argmemonly` / `noalias` / `readnone`）：它经由 `cpu->mem` 读写 guest
//     内存（"参数里**存的**指针"那一层），与 `lift_one` 同一条理由。
//   ★ 实现里最后一步必须是**尾调用**（`[[clang::musttail]]`，见 .cpp）：否则每个 `lift_dyn`
//     站点都多一个宿主栈帧 ⇒ 深递归会爆栈，"tail call 站点"这个读数也会失真。
extern "C" std::uint32_t lift_dyn(void* cpu, void* st, std::uint32_t ea);

// ★ 窗口 profile 的 `lift_dyn` 计数（AGENTS §12.8 第 3 条：缺省零成本 ⇒ 闸门是一个 bool）。
//   ★ 为什么只报**次数**、不报时间：`lift_dyn` 是**尾调用**（`return fn(...)`）⇒ 它的区间
//     含目标函数体，与 `lift` 桶重复 ⇒ 报时间会双计。计数则是**精确**的（每次调用一次 `++`）。
//   ★ 为什么今天必须有它：S7 之前 `lift_dyn` 是"完全无读数"的（不计 dispatch、不进 liftone 采样）
//     ⇒ 间接跳转那一大批派发在任何表里都没有一行。
//   ★ 为什么是 `std::atomic`：`lift_dyn` 可能在**两条宿主线程**上执行（主线程 + 音频 feeder 的
//     IR 执行器）；而 `liftDynCountOn` 由主线程的窗口 profile 在**运行中**开/关
//     （`AotHost::beginWindow/endWindow`）⇒ 读写都必须原子（relaxed：它只是计数，不是同步点）。
//     ⛔ 不要改回裸 `bool` / `uint64_t`：窗口 profile 与音频同开是既有用法（`sn0000.plan`）。
extern std::atomic<bool> liftDynCountOn;
extern std::atomic<std::uint64_t> liftDynCalls;

// 走进一个 shim（阶段 B 的 ④）。语义**全部**在 `lift_abi.cpp`，这里只钉契约：
//   ① `liftRegsToCpu(*st, cpu)`（8 GPR + eflags 物化；`cpu.eip = stub_ea`，与解释器 dispatcher
//      命中 shim 块时同一口径）—— 因为 `invoke` 第一条就读 `[esp]` 取返回地址、`ShimCall::u32`
//      全部基于 ESP；★ A′ 之后 `st == liftRegsView(cpu)`（AOT 臂）时这一步**跳过**（自拷贝）；
//   ② 用**桩 EA 反查 id**（块缓存里的 `installShim` 记录，与 dispatcher 同一个来源；缺块时回退
//      `ShimRegistry::idForStub`），再 `ShimRegistry::invokeById(id, cpu)` —— ⛔ 不要自己新建
//      `ShimCall`：invoke 自己读 `[esp]` 当返回地址、弹参、置 eip；
//   ③ `liftCpuToRegs(cpu, *st)`（**整块回读** —— `QueryPerformanceCounter` 会自己写 EDX）；
//   ④ 返回 `cpu.running ? cpu.eip : kLiftHalt`。
// 失败面：桩 EA 不在注册表 / id 失效 ⇒ 走 `trapUnimplemented`（响亮停机）⇒ 返回 `kLiftHalt`。
extern "C" std::uint32_t lift_shim(std::uint32_t stub_ea, void* st, void* cpu);

// ★ **AOT 专用入口**（S3 性能修，2026-11-15）：调用方（AOT 的派发循环）**已经把桩 EA 解析成了
//   `ShimRegistry::id`**（槽位索引表），⇒ 这里不再反查（不 `cache->peek`、不 `idForStub`、
//   不再拿 shims 锁）。语义与 `lift_shim` **逐字相同**的唯一差别就是"id 从哪来"：
//   · `id_plus_one != 0` ⇒ id = `id_plus_one - 1`，**直接**当已解析的登记项用；
//   · `id_plus_one == 0` ⇒ 不知道 ⇒ 走 `lift_shim` 的**同一条**反查路径（块缓存 → 注册表）。
//     （0 是合法 id ⇒ 用 +1 编码"有 id"，⛔ 不要把 0 当"没有 id"。）
//   ★ 为什么必须由调用方递 id（⛔ 不要改成"槽号 = id"）：id 是**注册序**，桩槽是**导入序 /
//     发号序**（`shims.cpp` 的 bind/addRuntime），两者本来就不一定相等（夹具更明确不等）
//     ⇒ `(stub_ea - 桩页) >> 2` 不是 id。判据：`lift-shim-stub-index`（`tests/shim_stub_index.cpp`）。
//   ★ 解释器臂不调这个入口（它的 dispatcher 有块缓存，走 `peek` 那条），行为不变。
extern "C" std::uint32_t lift_shim_id(std::uint32_t id_plus_one, std::uint32_t stub_ea, void* st,
                                      void* cpu);

namespace lift {
// 入口 = **resume EA**（复审裁决 1a）：`sub_436D00` 的两条 `call` 返回点 0x436D14 / 0x436D19
// 都在函数中段 ⇒ 一个 `.ll` 导出**一个**函数，函数体开头按 `%ea` 分派到对应入口块。
//   ^st     void*（AOT 臂 = `liftRegsView(cpu_)`，即 `cpu_.r[0]` 的别名；夹具 / feeder = 真
//           `LiftRegs`。**不产生指向 `X86` 的 `LiftRegs*`** ⇒ 无严格别名 UB）
//   ^mem    guest EA 0 的宿主基址（`GuestMemory::host(0)`；4 GiB 预留后不搬迁）
//   ^resume 下一个 guest EA（本函数 = 0x436D00 / 0x436D14 / 0x436D19）
//   ^cpu    只给 lift_note_write 用
//   返回    下一个 guest EA；0xFFFFFFFF = 回 trampoline 的哨兵
using LiftFn = std::uint32_t (*)(void*, std::uint8_t*, std::uint32_t, void*);

// ★ S7：`lift_dyn` 的派发表查找**唯一安装点**。运行期由执行臂 / 生成物夹具设成
//   `lift::aot::lookup`（见 `aot_dispatch.h`）。
//   ★ 为什么用函数指针而不是让 `lift_abi.cpp` 直接调 `aot::lookup`：`lift_abi.cpp` 属于
//     `emulator-lift-runtime`（**所有**测试 exe 都链它），而派发表在 `aot_dispatch.cpp`
//     （只有 AOT 臂 / 生成物夹具带）⇒ 直接依赖会给 ~100 个测试 exe 各加一个 undefined symbol。
//   ★ 缺省 `nullptr` ⇒ `lift_dyn` 走**响亮停**（⛔ 不许静默当成"跑完了"）。
//   ★ 解释器臂（`emulator-lift`）不装、也不需要：生成代码才用得到它。
using LiftDynLookup = LiftFn (*)(std::uint32_t ea);
extern LiftDynLookup liftDynLookup;

void liftRegsToCpu(const LiftRegs&, X86&);  // 逐字段 8 行 + eflags，⛔ 不用 memcpy/offsetof 当正确性依据
void liftCpuToRegs(const X86&, LiftRegs&);  // 同上
}  // namespace lift
