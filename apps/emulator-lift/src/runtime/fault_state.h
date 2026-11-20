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
// ★ 寄存器真源（A′ 之后，2026-11-20）：
//   AOT 主循环的 `%st` **直接指向 `cpu_.r[0]`**（`lift_abi.h` 的 `liftRegsView`）⇒ `cpu_` 的内存
//   **就是** lifted 寄存器现场：块内直接访存 / `lift_shim` / `lift_one` 三种现场都读 `cpu_`，
//   打印时**零搬运**。登记时 `regs = nullptr` + `regs_aliased_cpu = true` 把这件事显式化。
//   音频 feeder 跑在自己的线程 / 自己的 `X86` 副本上，`%st` 是它私有的真 `LiftRegs`
//   ⇒ `regs != nullptr`、`regs_aliased_cpu = false`，块内 fault 仍用块边界物化（旧口径不变）。
//   ⇒ 输出里有一行 `regs       :` 说明这次用的是哪一份，⛔ 不假装两者等价。
#pragma once

#include <cstdint>
#include <cstdio>

#include "runtime/lift_abi.h"  // LiftRegs（AOT 的寄存器真源；纯 POD）

namespace lift {

struct X86;

// 一次 guest fault 的现场（每线程一份；由 `AotHost::run` 在入口登记、收口清除）。
//   · `cpu`      = `AotHost::cpu_`（mem / eip / **活的 8 GPR + eflags**）；
//   · `regs`     = 非别名调用方（音频 feeder）的真 `LiftRegs`；AOT 主循环恒 `nullptr`；
//   · `regs_aliased_cpu` = true ⇒ `%st` 与 `cpu_` 是同一份内存（AOT 主循环）⇒ ⛔ 不要搬 `regs`；
//   · `block_ea` = `AotHost::run` 里的 `ea` 局部变量地址（= 当前派发块 EA，⛔ 不逐条更新）；
//   · `stub_page` / `stub_span` = 桩页范围（判"此刻在 shim 里"只用一次无符号减法，⛔ 不查表、不拿锁）。
struct GuestFaultSite {
    X86* cpu = nullptr;
    const LiftRegs* regs = nullptr;
    bool regs_aliased_cpu = false;
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
