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
// ★ `lift_note_write` 是 lifted 代码**唯一**的宿主调用（SMC 失效）；它转发到
//   `lift::noteGuestWrite`（`x86.h:46` / `x86.cpp:263`），与解释器 `X86::writeMem` 同一份语义。
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
