// emulator-lift · AOT 臂的音频 feeder guest 执行器（实现；契约见 aot_feeder.h）
//
// ★ 本文件**一行 guest 语义都不写**：只做"EA → 派发表 / 桩 EA → lift_shim_id"的路由、返回哨兵的
//   识别、以及错误面的表达。语义全在生成物（IR）与 shim 里。
#include "aot/aot_feeder.h"

#include <cstdio>

#include "aot/aot_dispatch.h"
#include "runtime/fault_state.h"  // guest fault 现场（这条线程的登记 / 清除）
#include "runtime/lift_abi.h"
#include "runtime/memory.h"
#include "runtime/shims.h"
#include "runtime/x86.h"

namespace lift {
namespace aot {

bool runAotFeederGuest(X86& cpu, GuestMemory& mem, const ShimRegistry& shims, std::uint32_t entry_ea,
                       std::uint32_t return_ea, std::string* err) {
    // 引擎（`AudioEngine::feederEvent`）已经把 `audio_cpu_` 的 esp / ecx / eip / eflags / x87 / xmm
    // 铺成 fill 的入口现场 ⇒ 这里只把寄存器搬进 `LiftRegs`（之后它才是唯一真源）。
    LiftRegs st;
    liftCpuToRegs(cpu, st);
    // 桩页判据与主循环同一份来源（`ShimRegistry` 自己的 page/slots；⛔ 不取 layout）。
    const std::uint32_t stub_page = shims.stubPage();
    const std::uint32_t stub_span = shims.stubSlots() * 4u;
    std::uint32_t ea = entry_ea;
    // ★ guest fault 现场（`runtime/fault_state.h`）：feeder 跑在自己的宿主线程上 ⇒ 主线程那份
    //   thread_local 现场在这里是空的。**入口登记一次**（不是每块）⇒ 这条线程上的 guest fault
    //   也能打出 eip / 块 EA / 寄存器 / 栈，而不是只有 3 行。
    GuestFaultSite fault_site;
    fault_site.cpu = &cpu;
    fault_site.regs = &st;
    fault_site.block_ea = &ea;
    fault_site.stub_page = stub_page;
    fault_site.stub_span = stub_span;
    setGuestFaultSite(fault_site);
    bool ok = true;
    for (;;) {
        // ★ 返回哨兵：**必须在查表之前**（主循环不认识它；见 aot_feeder.h 的注释）。
        if (ea == return_ea) break;
        if (!cpu.running) {
            if (err) *err = cpu.stop_why != nullptr ? cpu.stop_why : "guest 主动停止";
            ok = false;
            break;
        }
        cpu.eip = ea;  // 诊断面 / `lift_shim_id` 的 `cpu.eip = stub_ea` 口径
        std::uint32_t next = 0;
        const std::uint32_t stub_off = ea - stub_page;  // 无符号：ea < page ⇒ 巨大值 ⇒ 页外
        if (stub_off < stub_span) {
            // 运行期新增的 COM/DS vtable thunk 也在桩页里 ⇒ 每次都要查（`byStubEa` 是 O(1) 无锁快路径）。
            const ShimDef* def = shims.byStubEa(ea);
            if (def == nullptr) {
                // 页内、但注册表里没有 ⇒ 与主循环的"桩 EA 不在注册表"同一档响亮失败。
                cpu.running = false;
                cpu.stop_why = "音频 feeder：桩 EA 不在 shim 注册表里";
                if (err) *err = cpu.stop_why;
                ok = false;
                break;
            }
            // ★ 用**已解析的 id**：`lift_shim_id` 在 `id_plus_one != 0` 时**不碰** `cpu.cache`
            //   （feeder 的 cache 是 nullptr，`BlockCache` 也不是线程安全的）。
            next = lift_shim_id(def->id + 1u, ea, &st, &cpu);
        } else {
            // ★ `lookupUncounted`：与 `lookup` 同一条哈希探测，只是不记 trampoline 的派发账
            //   （`DispatchStats` 是 thread_local，这里也不会污染主线程的读数）。
            const LiftFn fn = lookupUncounted(ea);
            if (fn == nullptr) {
                if (err) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "音频 feeder：EA %08X 既不在派发表里，也不是 shim 桩 EA", ea);
                    *err = buf;
                }
                cpu.running = false;
                cpu.stop_why = "音频 feeder：EA 不在派发表里";
                ok = false;
                break;
            }
            next = fn(&st, mem.host(0), ea, &cpu);
        }
        if (next == ::kLiftHalt) {
            // 生成物的停机出口（`lift_one` 的 false / `lift_stop`）。`cpu.running` / `stop_why` 已由
            // 那条路径写好 ⇒ 原样交出去（与主循环同一条口径）。
            if (err) *err = cpu.stop_why != nullptr ? cpu.stop_why : "lifted 停机出口";
            ok = false;
            break;
        }
        if (!cpu.running) {
            if (err) *err = cpu.stop_why != nullptr ? cpu.stop_why : "guest 主动停止";
            ok = false;
            break;
        }
        ea = next;
    }
    // 把 `LiftRegs` 物化回 `X86`：失败面（诊断 / `abortFill` 的上下文恢复）要看寄存器现场。
    liftRegsToCpu(st, cpu);
    clearGuestFaultSite();  // 这把线程的现场在本次 feeder 调用结束后就失效了
    return ok;
}

}  // namespace aot
}  // namespace lift
