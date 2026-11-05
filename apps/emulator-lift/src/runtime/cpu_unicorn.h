// emulator-lift · 实验性 CPU 后端：Unicorn（QEMU TCG）的**切片执行器**
//
// ★ 定位：解释器（exec*.cpp / machine.cpp 的 run 主体）仍是 **reference**；本后端只在
//   `--cpu unicorn` 时被 `Machine::run` 分派到，负责执行 guest 指令本身。
//   ⛔ 这里**不许**重写 shim / COM / 窗口 / D3D9 语义：命中 shim 桩后同步寄存器 → 调
//   `ShimRegistry::invoke`（与解释器同一个函数）→ 同步回 Unicorn，继续下一段。
// ★ 为什么是"切片"而不是在 shim 的 hook 里嵌套 `uc_emu_start`：**不是** Unicorn 不支持嵌套
//   （官方明确支持 ≤64 层，有单测）；而是我们的 `GuestCallStack` 续延 / winmm 定时器语义要求
//   回到切片边界、由同一个宿主循环推进。⛔ 不要把"我们的续延语义"写成"Unicorn 的限制"。
// ★ 只有 `cpu_unicorn.cpp` 一个翻译单元 include `<unicorn/unicorn.h>`（默认构建根本不编译它）。
#pragma once

#include <cstdint>

#include "runtime/machine.h"

namespace lift {

// 本构建里有没有 Unicorn 后端（编译期事实；⛔ 不是运行期探测）。
// ★ 没有的构建里 `Machine::runUnicorn` 仍存在（返回 Unimplemented），这样 `--cpu unicorn`
//   能给出**可读的**错误而不是链接失败。
#if defined(LIFT_WITH_UNICORN)
inline constexpr bool kUnicornCompiled = true;
#else
inline constexpr bool kUnicornCompiled = false;
#endif

// POC 的**实测**能力/限制（2026-11-05，Unicorn 2.1.4 arm64-osx；⛔ 不是"pretend"）：
//   · 内存：按**已提交**的 guest 页 `uc_mem_map_ptr` 指到同一份 GuestMemory（⛔ 不复制 4 GiB，
//     也不映射未提交页 ⇒ 未提交仍可 fault）。连续已提交页合成一段映射。
//     运行期新提交的页由 `UC_HOOK_MEM_UNMAPPED` 补映射；guest `VirtualFree` 掉的页在切片边界
//     被 `uc_mem_unmap`（判据是 `GuestMemory::commitEpoch()`：O(1)，⛔ 不是每个切片数一遍页）。
//   · 寄存器：EAX..EDI / EIP / EFLAGS / x87（FP0..7 / FPCW / FPSW / FPTAG）/ XMM0..7 每个切片边界
//     **双向**同步（`uc_reg_read_batch` / `write_batch`：单寄存器 API 每次 ~2µs，逐条同步会把
//     shim 密集路径拖垮）。
//   · FS/GS **段基址**：Unicorn 2.1.4 在 32 位模式下**不支持**写 FS_BASE / GS_BASE（写是 no-op +
//     deprecation 警告），而 Windows 32 位 guest 的 TEB 只能通过 `fs:` 访问 ⇒ 本后端在 guest 里
//     放一张 GDT（null / flat-SS / FS / GS）、写 GDTR、用选择子（0x13 / 0x1B）装载基址。
//     代价（guest 可见）：SS/FS/GS 的**选择子**不再是 0（解释器里恒为 0），GDTR 指向这张表。
//     ⚠ 装 FS 之前必须先把 SS 重装成 flat 数据段：否则 `load_seg` 重算 hflags 会把 HF_SS32 清掉，
//       之后所有 push/ret 的栈地址都会被截成 16 位（实测踩过）。
//   · 段选择子本身（CS/DS/ES/SS）**不**逐切片同步：解释器在 flat 模型下跑，只有 FS/GS 基址
//     参与地址计算，而基址是同步的。
//   · 回调：`UC_HOOK_CODE` 只装在 **shim 桩页**（每个有 stub_ea 的页一个区间 hook）+
//     一个**计数 hook**（见下）；续延 / 定时器返回两个哨兵地址由 `UC_HOOK_MEM_UNMAPPED` 认出来
//     （它们从不提交，解释器也不在那里执行字节）。⛔ 没有"每条指令全局停点"的 hook。
//   · 计数：计数 hook 恒开 ⇒ `insns` / `--max-insns` / `clock_.tick()` 与解释器**同口径**。
//     两处已知偏差（都是"执行相同、计法不同"，实测逐条对齐过）：
//       ① `rep` 族：QEMU 按 TB 拆迭代（同一 EA 连续重入）⇒ 用 F3/F2 前缀折算成一条；
//       ② `9B D9 /7`（FWAIT + FNSTCW）：QEMU 当两条指令，Zydis/解释器折成一条 `fstcw` ⇒
//          boot 到 ExitProcess 时 unicorn 多 2 条（39,562 vs 39,560）。⛔ 不"抹平"这个差异。
//   · fault：`UC_ERR_READ/WRITE/FETCH_UNMAPPED` / `*_PROT` → `StopReason::MemoryFault`；
//     `UC_ERR_INSN_INVALID` → `DecodeError`；`UC_ERR_EXCEPTION` → `Unimplemented`。
//     都打印 EA + 区域 + `.lst` 文本 + 寄存器，⛔ 不静默继续。
//   · 诊断开关（不改语义）：`LIFT_UC_TRACE_FAULTS=1` 打印每次未映射访存；
//     `LIFT_UC_PROFILE=1` 打印每个切片的阶段耗时。
//   · 性能（同一 Release 二进制，见报告）：紧循环（无 shim）unicorn 168 MIPS / interp 91 MIPS；
//     shim 密集的 boot（658 次 shim / 39.5K 指令）unicorn 反而更慢（每次 shim 停点要一次
//     `uc_emu_start` 往返，≈30µs）—— 这是"切片执行"的**结构性**成本，不是 bug。
//   · ⛔ 未做：多线程 / SEH（原样：Unicorn 里 `UC_ERR_EXCEPTION`）/ 自修改代码的 TB 失效验证 /
//     逐切片写回 `X86::seg[]`。

}  // namespace lift
