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

#include <cstddef>
#include <cstdint>
#include <type_traits>

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

// SMC 失效（转 x86.h 的 noteGuestWrite；`cpu == nullptr` ⇒ no-op，与 cache==nullptr 同口径）
extern "C" void lift_note_write(void* cpu, std::uint32_t ea, std::uint32_t n);

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
//   `cpu == nullptr` ⇒ 调用方编程错误 ⇒ 响亮退出（⛔ 不静默 no-op）。
extern "C" void lift_tick(void* cpu, std::uint32_t n);

// ② 单指令宿主原语：把**这一条** guest 指令交给既有执行器（`Decoder` + `execInsn`）执行一次。
//   § 为什么要它（D10 P1/P2 / D11 / D12）：生成器只 inline 实现了 ~30 个 op；剩下的（x87 1022 /
//     `fs:` 475 / 串操作 85 / Div 53 / 循环移位 58 / Imul 169 …）手写第二份语义是**静默偏差**的
//     高发区（P1 的移位 PF 就是同类事故的现成例子）。⇒ 一条指令一次宿主调用，**语义只有一份**。
//   § 为什么有 `st` 这个参数（与 `lift_shim` 的 `LiftRegs*` 同一条理由，**不能省**）：
//     lifted 代码把 guest 状态放在 `LiftRegs`（`%st`）里，而 `execInsn` 吃的是 `X86`。
//     只给 `cpu` 就会**在过期的寄存器上执行、再把结果丢掉**（静默错）⇒ 这里做与 `lift_shim`
//     完全一样的三件事：① `liftRegsToCpu(*st, cpu)`；② 执行；③ `liftCpuToRegs(cpu, *st)`。
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
extern "C" bool lift_one(void* cpu, LiftRegs* st, std::uint32_t ea);

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

// 走进一个 shim（阶段 B 的 ④）。语义**全部**在 `lift_abi.cpp`，这里只钉契约：
//   ① `liftRegsToCpu(*st, cpu)`（8 GPR + eflags 物化；`cpu.eip = stub_ea`，与解释器 dispatcher
//      命中 shim 块时同一口径）—— 因为 `invoke` 第一条就读 `[esp]` 取返回地址、`ShimCall::u32`
//      全部基于 ESP；
//   ② 用**桩 EA 反查 id**（块缓存里的 `installShim` 记录，与 dispatcher 同一个来源；缺块时回退
//      `ShimRegistry::idForStub`），再 `ShimRegistry::invokeById(id, cpu)` —— ⛔ 不要自己新建
//      `ShimCall`：invoke 自己读 `[esp]` 当返回地址、弹参、置 eip；
//   ③ `liftCpuToRegs(cpu, *st)`（**整块回读** —— `QueryPerformanceCounter` 会自己写 EDX）；
//   ④ 返回 `cpu.running ? cpu.eip : kLiftHalt`。
// 失败面：桩 EA 不在注册表 / id 失效 ⇒ 走 `trapUnimplemented`（响亮停机）⇒ 返回 `kLiftHalt`。
extern "C" std::uint32_t lift_shim(std::uint32_t stub_ea, LiftRegs* st, void* cpu);

namespace lift {
// 入口 = **resume EA**（复审裁决 1a）：`sub_436D00` 的两条 `call` 返回点 0x436D14 / 0x436D19
// 都在函数中段 ⇒ 一个 `.ll` 导出**一个**函数，函数体开头按 `%ea` 分派到对应入口块。
//   ^st     LiftRegs*（原地复用，无拷贝）
//   ^mem    guest EA 0 的宿主基址（`GuestMemory::host(0)`；4 GiB 预留后不搬迁）
//   ^resume 下一个 guest EA（本函数 = 0x436D00 / 0x436D14 / 0x436D19）
//   ^cpu    只给 lift_note_write 用
//   返回    下一个 guest EA；0xFFFFFFFF = 回 trampoline 的哨兵
using LiftFn = std::uint32_t (*)(LiftRegs*, std::uint8_t*, std::uint32_t, void*);

void liftRegsToCpu(const LiftRegs&, X86&);  // 逐字段 8 行 + eflags，⛔ 不用 memcpy/offsetof 当正确性依据
void liftCpuToRegs(const X86&, LiftRegs&);  // 同上
}  // namespace lift
