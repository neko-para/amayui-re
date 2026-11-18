// emulator-lift · guest fault 的**现场**（AOT 臂唯一的寄存器 / 栈来源；解释器臂走 `liftFaultHook`）
//
// ★ 为什么需要它（`REQ-01M7PG086DG4BBPA40VCVZTP4X` / `defects.md#BUG-32`）：
//   `runtime/memory.cpp` 的 `guestFault` 只打 why / access / region 三行 + 一个**可选**的 `g_hook`。
//   解释器臂的 `liftFaultHook`（`machine.cpp`）在 `Machine::init` 里注册，它读 `Machine::cpu()`；
//   **AOT 臂故意没有 `Machine`** ⇒ `g_hook == nullptr` ⇒ fault 现场只剩那 3 行（没有 eip / 寄存器 / 栈）。
//   本文件把"现场"抽成一份**不依赖 `Machine`** 的数据 + 一个打印函数：AOT 的 `AotHost::run` 在
//   入口登记一次（**不是**每块 / 每帧），`guestFault` 通过 `installAotFaultHook()` 注册的钩子读它。
//
// ★ 口径（三条，⛔ 改之前先读）：
//   ① **零周期 / 零热路径成本**：登记只在 `run()` 入口写一次（几个字段），执行循环里一个字节都不加；
//   ② **只读**：打印函数只读 cpu / regs / guest 内存，⛔ 不改 guest 可见状态、⛔ 不改 eip；
//      唯一写的是把 `LiftRegs`（lifted 侧真源）搬进 `cpu` 的 8 GPR + eflags —— 那是 fault 之后
//      的一次性物化（进程随即 `_Exit(3)`），目的只是让打印与解释器臂同形；
//   ③ **零分配**：fault 在平台异常 / 信号上下文里被调用（`memory.h` 的 `FaultHook` 注释）⇒
//      只许 `fprintf` / 读已提交的 guest 页；⛔ 不构造 `std::string`、⛔ 不碰 `.lst`。
//
// ★ 寄存器真源怎么选（这是 AOT 唯一的坑）：
//   lifted 代码把 guest 状态放在 `LiftRegs`（`%st`）里，`cpu_` 只在**块边界 / shim 调用**才同步。
//   所以 fault 可能发生在这三种现场，判据**不需要热路径记账**（用 eip 落在哪个区区分）：
//   · 块内的直接访存（SAVE04 的 WRITE 就是这一类）⇒ eip == 当前块 EA，`LiftRegs` 是活现场；
//   · `lift_shim` 里（eip == shim 桩 EA）⇒ `cpu_` 是活现场（`lift_shim` 入口已经 `liftRegsToCpu`）；
//   · `lift_one` 里（eip == 那一条指令的 EA）⇒ `cpu_` 是活现场；我们用 `LiftRegs` 的块边界值
//     （= 该指令**执行前**的寄存器，正是"哪条指令 / 哪个基址"要看的），并在行里显式标注来源。
//   ⇒ 输出里有一行 `regs       :` 说明这次用的是哪一份，⛔ 不假装两者等价。
#pragma once

#include <cstdint>
#include <cstdio>

#include "runtime/lift_abi.h"  // LiftRegs（AOT 的寄存器真源；纯 POD）

namespace lift {

struct X86;

// 一次 guest fault 的现场（每线程一份；由 `AotHost::run` 在入口登记、收口清除）。
//   · `cpu`      = `AotHost::cpu_`（mem / eip / 宿主侧寄存器）；
//   · `regs`     = `AotHost::regs_`（lifted 侧真源；块边界同步）；
//   · `block_ea` = `AotHost::run` 里的 `ea` 局部变量地址（= 当前派发块 EA，⛔ 不逐条更新）；
//   · `stub_page` / `stub_span` = 桩页范围（判"此刻在 shim 里"只用一次无符号减法，⛔ 不查表、不拿锁）。
struct GuestFaultSite {
    X86* cpu = nullptr;
    const LiftRegs* regs = nullptr;
    const std::uint32_t* block_ea = nullptr;
    std::uint32_t stub_page = 0;
    std::uint32_t stub_span = 0;
};

// 当前线程的现场（登记 / 清除都只写一个 thread_local POD）。
void setGuestFaultSite(const GuestFaultSite& site);
void clearGuestFaultSite();

// 打印现场（唯一实现；`fault-report` CTest 直接驱动它）：eip / block EA / 寄存器 / 32 字栈。
// ★ `site.cpu == nullptr` ⇒ 打一行"没有登记现场"（⛔ 不伪造 0 寄存器）。
void printGuestFaultSite(const GuestFaultSite& site, std::FILE* out);

// 把本模块装成 `GuestMemory` 的 fault 钩子（`setGuestFaultHook`）。幂等；AOT 的 `init()` 调用一次。
// ★ 与解释器臂的 `liftFaultHook` 是**同一个槽位**（两臂不同时存在）⇒ 后装者赢是设计内的。
void installAotFaultHook();

}  // namespace lift
