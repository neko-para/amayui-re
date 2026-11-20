// emulator-lift · guest fault 现场（实现；口径与"为什么"见 `runtime/fault_state.h`）
//
// ★ 本文件在**平台异常 / 信号上下文**里执行（`guestFault` 的钩子）⇒ 只许 fprintf + 读已提交页：
//   ⛔ 不构造 std::string、⛔ 不查 `.lst` / 注册表 / 加锁、⛔ 不改 eip。
#include "runtime/fault_state.h"

#include <cstdio>

#include "runtime/exec.h"     // dumpRegisters / dumpGuestStack（与解释器臂同一份实现）
#include "runtime/memory.h"   // FaultHook / setGuestFaultHook
#include "runtime/x86.h"      // X86（cpu->eip / mem / r）

namespace lift {
namespace {

thread_local GuestFaultSite g_site{};

// 桩页判据：一次无符号减法 + 一次比较（⛔ 不 `byStubEa`：那要碰注册表 / 可能拿锁，fault 上下文不安全）。
bool inStubPage(const GuestFaultSite& site, std::uint32_t ea) {
    if (site.stub_span == 0) return false;
    return (ea - site.stub_page) < site.stub_span;
}

void aotFaultHook(const AccessSite& site) {
    (void)site;
    printGuestFaultSite(g_site, stderr);
}

}  // namespace

void setGuestFaultSite(const GuestFaultSite& site) { g_site = site; }
void clearGuestFaultSite() { g_site = GuestFaultSite{}; }

void printGuestFaultSite(const GuestFaultSite& site, std::FILE* out) {
    if (site.cpu == nullptr) {
        // 没有登记现场 ⇒ **明说**，⛔ 不打一份 0 寄存器冒充实测值。
        std::fprintf(out, "  aot        : (本线程没有登记 guest fault 现场：AOT 执行循环之外 / 音频 feeder 线程)\n");
        return;
    }
    X86& cpu = *site.cpu;
    // ★ 活现场在哪一份状态里（理由见头注）：
    //   · `regs_aliased_cpu`（AOT 主循环）⇒ `%st` 就是 `cpu_.r[0]` ⇒ 块内 / shim / lift_one 都在
    //     `cpu_` 里，**一次搬运都不做**；
    //   · 非别名（音频 feeder）⇒ eip 落在桩页 = fault 在 shim 里（`cpu_` 已同步）；否则用它的
    //     `LiftRegs` 块边界值物化（= 该指令执行前的寄存器，正是"哪条指令 / 哪个基址"要看的）。
    const bool in_shim = inStubPage(site, cpu.eip);
    const bool use_lifted = !in_shim && site.regs != nullptr && !site.regs_aliased_cpu;
    if (use_lifted) liftRegsToCpu(*site.regs, cpu);  // 只搬 8 GPR + eflags（eip 原样）
    const bool live_block = !in_shim && (site.regs_aliased_cpu || site.regs != nullptr);
    std::fprintf(out, "  eip        : %08X%s\n", cpu.eip,
                 live_block ? "  (AOT: 当前块的 EA；lifted 代码不逐条更新 eip)" : "");
    if (in_shim) {
        std::fprintf(out, "  block      : (eip 在 shim 桩页 ⇒ 当前在 shim 调用里，块 EA 未记录)\n");
    } else if (site.block_ea != nullptr) {
        std::fprintf(out, "  block      : lift_cc_%08X  (最近派发块)\n", *site.block_ea);
    } else {
        std::fprintf(out, "  block      : lift_cc_%08X\n", cpu.eip);
    }
    std::fprintf(out, "  regs       : %s\n",
                 site.regs_aliased_cpu
                     ? "cpu（AOT: %st 与 cpu_ 同一份内存 ⇒ 块内即活现场）"
                     : (in_shim ? "cpu (fault 在 shim 调用里；lift_shim 入口已同步)"
                                : (site.regs != nullptr
                                       ? "lifted (LiftRegs；块边界的 8 GPR + eflags —— 若 fault 在 "
                                         "lift_one 里，这是该指令执行前的值)"
                                       : "cpu (没有 LiftRegs 可读)")));
    dumpRegisters(cpu, out);
    dumpGuestStack(cpu, out, 32);
}

void installAotFaultHook() {
    // ★ 同一槽位由两臂共用（`GuestMemory` 是进程级单例）⇒ 这里只装，不"检查 Machine"。
    setGuestFaultHook(&aotFaultHook);
}

}  // namespace lift
