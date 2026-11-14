// emulator-lift · Shape A 原型阶段 A 的 ABI 薄层（实现；只转发，不写语义）
//
// ★ 本文件是 `lift_abi.h` 里那些 `extern "C"` 符号的**唯一**定义点。
// ★ `offsetof` 断言放这里（ABI TU）**并且**在 `tests/lift_abi_probe.cpp` 里再放一份
//   （复审 §5.3 的"两侧都放"）：这里守着 `.ll` 编译期的偏移假设，测试守着"改布局会红"。
#include "runtime/lift_abi.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "runtime/x86.h"

// `.ll` 里硬编码的偏移假设（`tests/ir/lift_436D00.ll` 只用 LiftRegs，不用 X86；
// 下面这组是**搬运函数**的对照面 —— 它们一旦漂了，搬运函数的逐字段实现仍然对，
// 但"LiftRegs 和 X86 头部不是同一布局"这条前提就变了，必须显式看见）。
static_assert(offsetof(lift::X86, r) == 0, "X86.r 必须在 +0");
static_assert(offsetof(lift::X86, eip) == 32, "X86.eip 必须在 +32（LiftRegs 没有这个字段）");
static_assert(offsetof(lift::X86, eflags) == 36, "X86.eflags 必须在 +36（LiftRegs 在 +32）");
static_assert(sizeof(lift::X86::r) == 32, "X86.r 必须是 8 个 dword");

extern "C" void lift_note_write(void* cpu, std::uint32_t ea, std::uint32_t n) {
    if (!cpu) return;  // 与 noteGuestWrite 的 cache==nullptr 同口径（no-op）
    lift::noteGuestWrite(static_cast<lift::X86*>(cpu)->cache, ea, n);
}

extern "C" std::uint32_t lift_shim(std::uint32_t stub_ea, LiftRegs* st, void* cpu_ptr) {
    if (!cpu_ptr || !st) {
        // ⛔ 不静默：这是调用方的编程错误（trampoline 只该把 shim EA + 有效状态递进来）
        std::fprintf(stderr, "[lift_shim] 调用方传了空指针（cpu=%p st=%p）\n", cpu_ptr, static_cast<void*>(st));
        std::fflush(stderr);
        std::_Exit(4);  // 宿主自身 bug（与 hostFault 同一档退出码）
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    // ① 物化：8 GPR + eflags。`invoke` 第一条就 `mem->rd32(cpu.esp())` 取返回地址；
    //    eip 只是诊断面，但口径要与解释器一致 —— dispatcher 命中 shim 块时 cpu.eip 就是这个桩 EA。
    lift::liftRegsToCpu(*st, cpu);
    cpu.eip = stub_ea;
    // ② 桩 EA → id。★ 为什么**不**用 `(stub_ea - 桩页) >> 2`（任务书 §2.1 的字面公式）：
    //    id 是**注册序**（`shims.cpp:169/238`），stub 槽是**导入序 / 发号序**（`shims.cpp:319/340`），
    //    两者在真机上本来就不一定相等；夹具（`tests/fixture.h:24` 的 `kStubBase = 0x7FFE2000`
    //    + `use()` 自己发号）更是明确不等。字面公式在这里会拿到一个错的 id ⇒ 调错 shim。
    //    ⇒ 用**与 dispatcher 同一个来源**：块缓存里 `installShim(ea, id)` 记下的 `shim_id`
    //      （`x86.cpp:190-199`、`machine.cpp:1585` / `audio.cpp:2214` 就是这么取 id 的）。
    //    块缓存缺这一条（例如还没 installShim）时回退注册表反查（`audio.cpp:2198` 的同一入口）。
    std::uint32_t id = 0;
    bool found = false;
    if (cpu.cache) {
        if (const lift::Block* b = cpu.cache->peek(stub_ea); b && b->is_shim) {
            id = b->shim_id;
            found = true;
        }
    }
    if (!found && cpu.shims) {
        id = cpu.shims->idForStub(stub_ea);
        // id 0 是合法登记；只有"反查不到"才该响亮失败
        found = (id != 0) || (cpu.shims->byStubEa(stub_ea) != nullptr);
    }
    if (!found) {
        // 与"未实现的导入"共用**同一个**响亮停机点（不新增静默路径）：running=false +
        // stop_kind/stop_why 已写 ⇒ 下面的 kLiftHalt 会让 trampoline 停下来报。
        const std::uint32_t ret_ea = cpu.mem ? cpu.mem->rd32(cpu.esp()) : 0u;
        lift::trapUnimplemented(cpu, "(lift)", "lift_shim", "桩 EA 不在 shim 注册表 / 块缓存里", ret_ea, stub_ea);
    } else if (!cpu.shims || !cpu.shims->invokeById(cpu, id)) {
        const std::uint32_t ret_ea = cpu.mem ? cpu.mem->rd32(cpu.esp()) : 0u;
        lift::trapUnimplemented(cpu, "(lift)", "lift_shim", "invokeById 找不到这个 id", ret_ea, stub_ea);
    }
    // ③ 整块回读（⛔ 不按"框架只写 eax/esp/eip"精简：QueryPerformanceCounter 自己写 EDX）
    lift::liftCpuToRegs(cpu, *st);
    // ④ running == false ⇒ 停机哨兵（trampoline 负责报 stop_kind / stop_why）
    return cpu.running ? cpu.eip : ::kLiftHalt;
}

namespace lift {

// ★ 逐字段（8 个 GPR + eflags）：**不**用 memcpy / offsetof 当正确性依据 ——
//   正确性只取决于这一对函数本身，与 `X86` 的布局无关。
void liftRegsToCpu(const LiftRegs& st, X86& cpu) {
    cpu.r[0] = st.r[0];
    cpu.r[1] = st.r[1];
    cpu.r[2] = st.r[2];
    cpu.r[3] = st.r[3];
    cpu.r[4] = st.r[4];
    cpu.r[5] = st.r[5];
    cpu.r[6] = st.r[6];
    cpu.r[7] = st.r[7];
    cpu.eflags = st.eflags;
}

void liftCpuToRegs(const X86& cpu, LiftRegs& st) {
    st.r[0] = cpu.r[0];
    st.r[1] = cpu.r[1];
    st.r[2] = cpu.r[2];
    st.r[3] = cpu.r[3];
    st.r[4] = cpu.r[4];
    st.r[5] = cpu.r[5];
    st.r[6] = cpu.r[6];
    st.r[7] = cpu.r[7];
    st.eflags = cpu.eflags;
}

}  // namespace lift
