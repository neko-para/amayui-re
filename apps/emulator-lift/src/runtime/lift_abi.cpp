// emulator-lift · Shape A 原型阶段 A 的 ABI 薄层（实现；只转发，不写语义）
//
// ★ 本文件是 `lift_abi.h` 里那些 `extern "C"` 符号的**唯一**定义点。
// ★ `offsetof` 断言放这里（ABI TU）**并且**在 `tests/lift_abi_probe.cpp` 里再放一份
//   （复审 §5.3 的"两侧都放"）：这里守着 `.ll` 编译期的偏移假设，测试守着"改布局会红"。
#include "runtime/lift_abi.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime/decode.h"
#include "runtime/exec.h"
#include "runtime/host_services.h"
#include "runtime/shims.h"
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

// ---------------------------------------------------------------------------------------
// ① lift_tick —— 只记条数（D2b：时钟 = wall clock ⇒ 不推进虚拟时间）
// ---------------------------------------------------------------------------------------
extern "C" void lift_tick(void* cpu_ptr, std::uint32_t n) {
    if (!cpu_ptr) {
        // 与 `lift_shim` 的空指针同一档：调用方（生成物 / trampoline）的编程错误，⛔ 不静默。
        std::fprintf(stderr, "[lift_tick] 调用方传了空 cpu（n=%u）\n", n);
        std::fflush(stderr);
        std::_Exit(4);
    }
    static_cast<lift::X86*>(cpu_ptr)->insns += n;
}

// ---------------------------------------------------------------------------------------
// ② lift_one —— 单指令执行（语义全部在 Decoder + execInsn，这里只做搬运与 eip 保护）
// ---------------------------------------------------------------------------------------
extern "C" bool lift_one(void* cpu_ptr, LiftRegs* st, std::uint32_t ea) {
    if (!cpu_ptr || !st) {
        std::fprintf(stderr, "[lift_one] 调用方传了空指针（cpu=%p st=%p ea=%08X）\n", cpu_ptr,
                     static_cast<void*>(st), ea);
        std::fflush(stderr);
        std::_Exit(4);
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    // ★ 宿主必须先装配好"解码 + 执行"这两份服务：`HostServices`（D4）提供 `decoder()`，
    //   `cpu.mem` 提供 guest 字节。缺任何一份都**响亮停**（⛔ 不返回 true 假装执行过）。
    lift::HostServices* host = lift::hostOf(cpu);
    if (host == nullptr || cpu.mem == nullptr) {
        std::fprintf(stderr,
                     "[lift_one] 宿主没装配解码器 / guest 内存（cpu.host=%p cpu.mem=%p）@ %08X ⇒ 响亮停\n",
                     static_cast<void*>(host), static_cast<void*>(cpu.mem), ea);
        std::fflush(stderr);
        cpu.running = false;
        cpu.stop_why = "lift_one: 宿主没有 decoder / guest 内存（hostOf 为 null）";
        return false;
    }
    lift::Decoder& decoder = host->decoder();

    // ① 状态进 X86（与 `lift_shim` 的第 ① 步同口径；lifted 侧的状态在 `*st` 里）。
    lift::liftRegsToCpu(*st, cpu);
    // ② eip：**单条执行**不许打乱生成器的块内推进 ⇒ 执行前设成这一条的 EA、执行后复原。
    const std::uint32_t saved_eip = cpu.eip;
    cpu.eip = ea;

    lift::Insn in;
    std::string why;
    bool ok = false;
    if (!decoder.decode(*cpu.mem, ea, &in, &why)) {
        // 生成器是从同一份 PE 解出来的，所以走到这里说明"生成物与字节真源不一致"——
        // 仍然只是**响亮停**（不是宿主 bug）：真机到此也会取指失败。
        std::fprintf(stderr, "[lift_one] 解码失败 @ %08X：%s ⇒ 响亮停\n", ea, why.c_str());
        std::fflush(stderr);
        cpu.running = false;
        cpu.stop_why = "lift_one: 解码失败（生成物与 PE 字节不一致）";
    } else {
        // 诊断面与解释器 dispatcher 同一份（`machine.cpp:1715`）：停机现场能看见"从哪来"。
        cpu.noteInsn(in.ea, in.mnemonic);
        ok = lift::execInsn(cpu, in);
        if (!ok) {
            // ★ 五类 + `#DE`：`execInsn` 自己已经把 `running=false` / `stop_why` 置好
            //   （`exec_control.cpp` 的 Hlt/Int3/Int、`exec_integer.cpp` 的 `cpuFault`、
            //   `exec.cpp` 的 `unimplemented`）。这里只补一行**点名 EA + op**，并把"它没置停"
            //   这种内部不一致也收回到停机（⛔ 不许让调用方以为还能继续跑）。
            if (cpu.running) {
                cpu.running = false;
                cpu.stop_why = "lift_one: execInsn 返回 false 但没有置停";
            }
            std::fprintf(stderr, "[lift_one] 单指令停机 @ %08X (%s, op=%s) ⇒ %s\n", ea,
                         in.mnemonic != nullptr ? in.mnemonic : "?", lift::opName(in.op),
                         cpu.stop_why != nullptr ? cpu.stop_why : "(未登记理由)");
            std::fflush(stderr);
        }
    }
    cpu.eip = saved_eip;
    // ③ 整块回读（⛔ 不按"这条只写 eax"精简：execInsn 可能改任何 GPR / EFLAGS）。
    lift::liftCpuToRegs(cpu, *st);
    return ok;
}

// ---------------------------------------------------------------------------------------
// ③ 段基址（`fs:` / `gs:` 的 EA 计算；本轮生成器走 `lift_one`，不用这两条 —— 见头注）
// ---------------------------------------------------------------------------------------
extern "C" std::uint32_t lift_fs_base(void* cpu_ptr) {
    if (!cpu_ptr) {
        std::fprintf(stderr, "[lift_fs_base] 调用方传了空 cpu\n");
        std::fflush(stderr);
        std::_Exit(4);
    }
    return static_cast<lift::X86*>(cpu_ptr)->fs_base;
}

extern "C" std::uint32_t lift_gs_base(void* cpu_ptr) {
    if (!cpu_ptr) {
        std::fprintf(stderr, "[lift_gs_base] 调用方传了空 cpu\n");
        std::fflush(stderr);
        std::_Exit(4);
    }
    return static_cast<lift::X86*>(cpu_ptr)->gs_base;
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
