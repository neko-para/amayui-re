// emulator-lift · Shape A 原型阶段 A 的 ABI 薄层（实现；只转发，不写语义）
//
// ★ 本文件是 `lift_abi.h` 里那些 `extern "C"` 符号的**唯一**定义点。
// ★ `offsetof` 断言放这里（ABI TU）**并且**在 `tests/lift_abi_probe.cpp` 里再放一份
//   （复审 §5.3 的"两侧都放"）：这里守着 `.ll` 编译期的偏移假设，测试守着"改布局会红"。
#include "runtime/lift_abi.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime/decode.h"
#include "runtime/decode_cache.h"
#include "runtime/exec.h"
#include "runtime/host_services.h"
#include "runtime/rep_bulk.h"
#include "runtime/shims.h"
#include "runtime/x86.h"
#include "runtime/x87_layout.h"

namespace lift {
namespace {
// ★ `lift_one` 的解码缓存：**每线程一份**（缓存不是线程安全的；`lift_one` 今天只在 AOT 主线程上
//   被 lifted 代码调用 —— 音频 feeder 走的是解释器块缓存，不经过这里）。见 `decode_cache.h` 头注。
thread_local DecodeCache g_liftOneDecodeCache;

// ★ A′ 的**唯一**别名判据（`lift_shim*` 与 `lift_one` 共用）：`%st` 是不是就是 `cpu_.r[0]`。
//   true ⇒ 两处整块搬运是自拷贝，**跳过**；false ⇒ 调用方是生成物夹具 / 音频 feeder（真
//   `LiftRegs`）⇒ 搬运路径逐字不变。判据只是两个 `void*` 的比较（⛔ 不解引用、⛔ 不产生
//   指向 `X86` 的 `LiftRegs*`）。
bool regsAliasCpu(const void* st, X86& cpu) { return st == liftRegsView(cpu); }
}  // namespace
// 可写入口只给 `lift_one`（下面）；对外只有头文件里的只读 `liftOneDecodeCache()`（读数用）。
DecodeCache& liftOneDecodeCacheMutable() { return g_liftOneDecodeCache; }
const DecodeCache& liftOneDecodeCache() { return g_liftOneDecodeCache; }
}  // namespace lift

// ---------------------------------------------------------------------------------------
// 采样式内部 profile 的**唯一接缝**（AOT 的 `--profile`；契约见 `lift_abi.h`）
// ★ 默认 `nullptr`：`lift_one` 里只有一次指针判空（关档零成本）。
//   ★ **每线程一份**（`thread_local`）：`lift_one` 可能在两条宿主线程上被调（AOT 主线程 +
//     音频 feeder 的 IR 执行器）。feeder 的副本恒空 ⇒ 不采样、不写主线程的 `AotProfile`。
//   ★ 解释器臂（`emulator-lift`）永远不设它；这里的计数器也只在钩子非空时才被推进。
// ---------------------------------------------------------------------------------------
namespace lift {
thread_local LiftOneProfileHook liftOneProfileHook = nullptr;
namespace {
thread_local std::uint64_t g_liftOneProfileCount = 0;
}
bool liftOneProfileSampleDue() {
    // 只在 `liftOneProfileHook != nullptr` 时被调用（调用点用 `&&` 短路）。
    if (++g_liftOneProfileCount < kLiftOneProfileStride) return false;
    g_liftOneProfileCount = 0;
    return true;
}
std::uint64_t liftOneProfileNowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
}  // namespace lift

// `.ll` 里硬编码的偏移假设（`tests/ir/lift_436D00.ll` 只用 LiftRegs，不用 X86；
// 下面这组是**搬运函数**的对照面 —— 它们一旦漂了，搬运函数的逐字段实现仍然对，
// 但"`LiftRegs` 和 `X86` 头部逐字节同布局"这条前提就变了，必须显式看见）。
// ★ A′（2026-11-20）：`eip` 与 `eflags` 互换 ⇒ `r[8]@0..31 / eflags@32 / eip@36`；`seg@40` 不变。
static_assert(offsetof(lift::X86, r) == 0, "X86.r 必须在 +0");
static_assert(offsetof(lift::X86, eflags) == 32, "X86.eflags 必须在 +32（LiftRegs 的 eflags）");
static_assert(offsetof(lift::X86, eip) == 36, "X86.eip 必须在 +36（紧跟在 LiftRegs 之后）");
static_assert(sizeof(lift::X86::r) == 32, "X86.r 必须是 8 个 dword");
// ★ 防重叠 / 别名契约：`LiftRegs`（r + eflags = 36 B）必须**恰好**覆盖到 `eip` 为止，`eip` 再占
//   4 B 到 `seg`。设计单写的单条 `offsetof(r) + sizeof(LiftRegs) == offsetof(seg)` 少算了 `eip`
//   的 4 B（0+36=36 != 40）⇒ 拆成两条等价判据（既钉"无重叠"，也钉"中间没有填充"）。
static_assert(offsetof(lift::X86, r) + sizeof(LiftRegs) == offsetof(lift::X86, eip),
              "LiftRegs 必须恰好覆盖到 eip（AOT 别名 `%st` 的前 36 B）");
static_assert(offsetof(lift::X86, r) + sizeof(LiftRegs) + sizeof(lift::X86::eip) ==
                  offsetof(lift::X86, seg),
              "eip 之后必须紧跟 seg（无填充 ⇒ `%st` 别名 `cpu_` 的布局前提）");

// ★ D49 / 检查点 2：**x87 状态偏移是生成代码契约**（唯一一份常量在 `runtime/x87_layout.h`）。
//   发射器把 x87 状态读写发成 `getelementptr i8, ptr %cpu, i64 <kXxx>` ⇒ 这里的每个常量都必须
//   等于真实的 `offsetof(X86, fpu) + offsetof(X87State, …)`。⚠ 这组断言**就是**那份契约的判据：
//   改动 `x86.h` 的 `X87State` 而不改 `x87_layout.h` ⇒ 这里编译期红（不是运行期静默错字段）。
static_assert(lift::x87layout::kRegs ==
                  offsetof(lift::X86, fpu) + offsetof(lift::X87State, regs),
              "x87 槽起点漂了：同步 runtime/x87_layout.h 的 kRegs");
static_assert(lift::x87layout::kControl ==
                  offsetof(lift::X86, fpu) + offsetof(lift::X87State, control),
              "x87 控制字偏移漂了：同步 runtime/x87_layout.h 的 kControl");
static_assert(lift::x87layout::kStatus ==
                  offsetof(lift::X86, fpu) + offsetof(lift::X87State, status),
              "x87 状态字偏移漂了：同步 runtime/x87_layout.h 的 kStatus");
static_assert(lift::x87layout::kTop == offsetof(lift::X86, fpu) + offsetof(lift::X87State, top),
              "x87 TOP 偏移漂了：同步 runtime/x87_layout.h 的 kTop");
static_assert(lift::x87layout::kTag == offsetof(lift::X86, fpu) + offsetof(lift::X87State, tag),
              "x87 tag 偏移漂了：同步 runtime/x87_layout.h 的 kTag");
static_assert(lift::x87layout::kSlotSize == sizeof(double), "x87 槽宽度必须是 sizeof(double)");
static_assert(sizeof(lift::X87State::regs) == 8 * sizeof(double), "x87 槽必须是 8 个 double");

// ★ 2026-11-15（本轮：`lift_one` → 0）：`fs:`/`gs:` 与 SSE 状态也进了同一份**生成代码契约**。
//   理由与上面那组逐字相同：发射器只拿到 `ptr %cpu` ⇒ 偏移必须是唯一一份常量 + 编译期断言。
static_assert(lift::x87layout::kFsBase == offsetof(lift::X86, fs_base),
              "fs_base 偏移漂了：同步 runtime/x87_layout.h 的 kFsBase");
static_assert(lift::x87layout::kGsBase == offsetof(lift::X86, gs_base),
              "gs_base 偏移漂了：同步 runtime/x87_layout.h 的 kGsBase");
static_assert(lift::x87layout::kXmm == offsetof(lift::X86, xmm),
              "xmm 偏移漂了：同步 runtime/x87_layout.h 的 kXmm");
static_assert(lift::x87layout::kXmmStride == sizeof(lift::X86::xmm[0]),
              "xmm 槽宽度漂了：同步 runtime/x87_layout.h 的 kXmmStride");
static_assert(lift::x87layout::kMxcsr == offsetof(lift::X86, mxcsr),
              "mxcsr 偏移漂了：同步 runtime/x87_layout.h 的 kMxcsr");
static_assert(lift::x87layout::kSeg == offsetof(lift::X86, seg),
              "seg 偏移漂了：同步 runtime/x87_layout.h 的 kSeg");
static_assert(lift::x87layout::kSegStride == sizeof(lift::X86::seg[0]),
              "seg 槽宽度漂了：同步 runtime/x87_layout.h 的 kSegStride");

extern "C" void lift_note_write(void* cpu, std::uint32_t ea, std::uint32_t n) {
    if (!cpu) return;  // 与 noteGuestWrite 的 cache==nullptr 同口径（no-op）
    // ★ opt-in 的 guest **写观察**（`LIFT_WATCH`；BUG-32 的"谁在 P-8 写了那 16 字节"）。
    //   热路径只有一次 `watchOn()`（relaxed load + 分支）；缺省关闭 ⇒ 一个字节的输出都不多。
    //   必须在 store **之前**（生成代码在 store 前 call 本函数）⇒ follow 的 pending 靠
    //   "下一次写在 store 之后开头补读"才拿得到新值（契约见 `runtime/memory.h`）。
    //   `cpu->eip` = 当前块 EA（lifted 代码不逐条更新 eip）⇒ 打出来就是 `lift_cc_<块EA>`。
    if (lift::watchOn()) lift::watchGuestWrite(static_cast<lift::X86*>(cpu)->eip, ea, n);
    lift::noteGuestWrite(static_cast<lift::X86*>(cpu)->cache, ea, n);
}

// ★ `rep movs*` / `rep stos*` 的整段快路径（AOT 臂）：判 + 执行 + 计数 = `lift::repBulkTry`。
//   ⛔ 这里**不许**出现第二套闸门（`CrossBoundary` / `Uncommitted` / 方向×重叠 全在 `repBulkPlan`）；
//   本函数只做三件搬运之外的事：拼一个 `Insn`（闸门要读 `rep` / `op`）、别名判据、回读。
//   ★ `Insn` 只填闸门真读的四个字段（`ea` / `rep` / `op` / `opsz`）—— 其余字段是解码产物，
//     快路径一个都不读（`repBulkPlan` 只读 `in.rep` / `in.op`）。填 `rep = 0xF3` 与 `0xF2` 等价，见闸门 ⓪。
extern "C" bool lift_rep_bulk(void* cpu_ptr, void* st, std::uint32_t ea, std::uint32_t kind,
                              std::uint32_t size) {
    if (cpu_ptr == nullptr || st == nullptr) {
        // 与 `lift_one` 同口径：唯一调用方是生成物，空指针 = 生成器 / 夹具的编程错误 ⇒ 响亮退出。
        std::fprintf(stderr, "[lift_rep_bulk] 调用方传了空指针（cpu=%p st=%p ea=%08X）\n", cpu_ptr, st, ea);
        std::fflush(stderr);
        std::_Exit(4);
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    const bool alias = lift::regsAliasCpu(st, cpu);
    if (!alias) lift::liftRegsToCpu(*static_cast<const LiftRegs*>(st), cpu);
    lift::Insn in;
    in.ea = ea;
    in.opsz = static_cast<std::uint8_t>(size);
    in.rep = 0xF3;  // `F3` / `F2` 对 movs/stos 是同一条循环（`exec_string.cpp` 的闸门 ⓪）
    in.op = (kind == kLiftRepStos) ? lift::Op::Stos : lift::Op::Movs;
    const bool done = lift::repBulkTry(cpu, in, static_cast<std::uint8_t>(size));
    if (!alias) lift::liftCpuToRegs(cpu, *static_cast<LiftRegs*>(st));
    return done;
}

// ---------------------------------------------------------------------------------------
// ① lift_tick —— 只记条数（D2b：时钟 = wall clock ⇒ 不推进虚拟时间）
// ---------------------------------------------------------------------------------------
extern "C" void lift_tick(void* cpu_ptr, std::uint32_t n) {
    // ★ 2026-11-17：**移除**空指针分支（用户裁决），改 Debug 档 `assert`。
    //   · 为什么可以删：唯一调用方是**生成物**（`lift_gen_emit.cpp` 发的 `call void @lift_tick`），
    //     trampoline 恒传 `&AotRunner::cpu_`；`runtime/` 与 `tests/` 零调用，也没有任何测试
    //     断言过 `cpu == nullptr` 时的行为（旧口径因此没有消费者）。
    //   · 为什么用 `assert` 而不是静默：Release/NDEBUG 下它**零代码、零分支**；Debug 档仍会在
    //     解引用之前**点名** `cpu_ptr == nullptr`（而不是把编程错误变成一个没有名字的段错误）。
    //   · 为什么值得：这一条 `cbz` 落在 **10 万级** tick 站点上（IR 实测 104,949 个；
    //     历史 native 口径 101,765 —— 每个块边界 / 每次宿主调用前一次），是发射面最大的
    //     单一固定成本之一。
    //   ⛔ 不要顺手把 `lift_one` / `lift_shim` 的判空也删掉：它们不是 10 万站点级的热路径。
    assert(cpu_ptr != nullptr);
    static_cast<lift::X86*>(cpu_ptr)->insns += n;
}

// ---------------------------------------------------------------------------------------
// ② lift_one —— 单指令执行（语义全部在 Decoder + execInsn，这里只做搬运与 eip 保护）
// ---------------------------------------------------------------------------------------
extern "C" bool lift_one(void* cpu_ptr, void* st, std::uint32_t ea) {
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

    // ★ 采样式内部 profile（AOT `--profile`）的**入口**：关档成本 = 一次指针判空（`&&` 短路）。
    //   ⛔ 不在这里做任何语义：只把 t0 留在手上，出口处按"采样到了吗"决定要不要报。
    const lift::LiftOneProfileHook prof_hook = lift::liftOneProfileHook;
    const bool prof_sample = prof_hook != nullptr && lift::liftOneProfileSampleDue();
    // ★ 四桶归因（A 换入 / B 解码 / C 执行 / D 回读）：**只在采样到的这一次**取时（见 `profMark`），
    //   关档时这两个变量都不被读、`profMark` 的循环体是 `if (!prof_sample) return;` ⇒ 一次可预测分支。
    std::uint64_t prof_entry_ns = 0;
    std::uint64_t prof_mark_ns = 0;
    std::uint64_t prof_phase_ns[4] = {0, 0, 0, 0};
    const char* prof_op = "?";
    if (prof_sample) {
        prof_entry_ns = lift::liftOneProfileNowNs();
        prof_mark_ns = prof_entry_ns;
    }
    // 收一段的边界：把"上一段到现在"记进 `prof_phase_ns[i]`。⛔ 不参与语义（只在采样时执行）。
    const auto profMark = [&](int i) {
        if (!prof_sample) return;
        const std::uint64_t t = lift::liftOneProfileNowNs();
        prof_phase_ns[i] = t - prof_mark_ns;
        prof_mark_ns = t;
    };

    // ① 状态进 X86（与 `lift_shim` 的第 ① 步同口径；lifted 侧的状态在 `*st` 里）。
    //   ★ A′：AOT 臂 `st` 就是 `cpu.r[0]` 的别名 ⇒ 这步是自拷贝，**跳过**（非别名路径逐字不变）。
    if (!lift::regsAliasCpu(st, cpu)) lift::liftRegsToCpu(*static_cast<const LiftRegs*>(st), cpu);
    // ② eip：**单条执行**不许打乱生成器的块内推进 ⇒ 执行前设成这一条的 EA、执行后复原。
    const std::uint32_t saved_eip = cpu.eip;
    cpu.eip = ea;
    profMark(0);  // A 换入

    lift::Insn in;
    std::string why;
    bool ok = false;
    // ★ 解码走 **EA → Insn 缓存**（自带逐字节校验；见 `decode_cache.h`）：语义与
    //   `decoder.decode(*cpu.mem, ea, &in, &why)` **逐字相同**（同 EA 同字节 ⇒ 同一 Insn），
    //   差别只在"要不要再解一次"。实测解码占 `lift_one` 的 79% ⇒ 这是这个入口的主开销。
    if (!lift::decodeCached(lift::liftOneDecodeCacheMutable(), decoder, *cpu.mem, ea, &in, &why)) {
        profMark(1);  // B 解码（失败路径也要收口，否则后面几桶会跨到失败分支上）
        // 生成器是从同一份 PE 解出来的，所以走到这里说明"生成物与字节真源不一致"——
        // 仍然只是**响亮停**（不是宿主 bug）：真机到此也会取指失败。
        std::fprintf(stderr, "[lift_one] 解码失败 @ %08X：%s ⇒ 响亮停\n", ea, why.c_str());
        std::fflush(stderr);
        cpu.running = false;
        cpu.stop_why = "lift_one: 解码失败（生成物与 PE 字节不一致）";
        profMark(2);  // C 执行（这条路径上是"响亮停的那两行"；⛔ 读数不参与判据）
    } else {
        profMark(1);  // B 解码（成功路径）
        // 诊断面与解释器 dispatcher 同一份（`machine.cpp:1715`）：停机现场能看见"从哪来"。
        cpu.noteInsn(in.ea, in.mnemonic);
        // ★ `opName` 只给采样钩子用 ⇒ **只在采样时取**（关档时是一次 `if`；以前是每条指令都调一次
        //   跨 TU 的大 switch —— 那是纯浪费，改它不改任何语义）。
        if (prof_sample) prof_op = lift::opName(in.op);
        ok = lift::execInsn(cpu, in);
        profMark(2);  // C 执行（含 `execInsn` 失败路径的收口）
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
    //   ★ A′：别名时是自拷贝 ⇒ 跳过（`execInsn` 可能改内存，块内的整份失效照旧在生成器里做）。
    if (!lift::regsAliasCpu(st, cpu)) lift::liftCpuToRegs(cpu, *static_cast<LiftRegs*>(st));
    profMark(3);  // D 回读
    // ★ 采样式 profile 的**出口**：只把 (EA, 总 ns, op, 四段 ns) 交给宿主。
    //   ⛔ 不改返回值、不改任何状态；总 ns = 四段之和（同一次取时链）。
    if (prof_sample) {
        lift::LiftOneProfileSample sample;
        sample.ea = ea;
        sample.ns = prof_mark_ns - prof_entry_ns;
        sample.op = prof_op;
        for (int i = 0; i < 4; ++i) sample.phase_ns[i] = prof_phase_ns[i];
        prof_hook(sample);
    }
    return ok;
}

// ---------------------------------------------------------------------------------------
// ②b lift_stop —— 生成代码的**停机原语**（本轮：`lift_one` 调用点 → 0）
// ---------------------------------------------------------------------------------------
// ★ 它**不执行**任何 guest 指令：只把解释器在同一个停机点上做的两件事照做 ——
//   `cpu.running = false` + `cpu.stop_why = <同一句字符串>`。语义真源逐条写在
//   `lift_abi.h` 的 `LiftStopKind` 旁（`exec_control.cpp:62-72` / `exec.cpp:209-210` /
//   `exec.cpp:339-371` 的 `unimplemented` / `exec_integer.cpp:273-326` 的 `cpuFault`）。
//   ⇒ AOT 的 trampoline 收到随后的 `kLiftHalt` 时走的是**同一条** `hostStopReason` 判据。
// ★ 为什么不再走 `lift_one`：那条路径在 `.ll` 里是 `call i1 @lift_one`（一次解码 +
//   `execInsn` 的完整往返），而这五类**恒**返回 false —— 它换来的是"生成物里没有解释器"
//   这条判据的破产，而不是任何语义。⛔ 不是"保留解释器"，是"把停机面表达出来"。
extern "C" void lift_stop(void* cpu_ptr, std::uint32_t ea, std::uint32_t kind) {
    if (!cpu_ptr) {
        std::fprintf(stderr, "[lift_stop] 调用方传了空 cpu（ea=%08X kind=%u）\n", ea, kind);
        std::fflush(stderr);
        std::_Exit(4);
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    const char* why = "";
    switch (kind) {
        case kLiftStopHlt: why = "hlt"; break;
        case kLiftStopInt3:
            why = "int 3（调试陷阱；M1 不做 SEH / 调试器，plan §9 R1）";
            break;
        case kLiftStopInt: why = "软件中断（M1 不建模中断表）"; break;
        case kLiftStopOther: why = "助记符认识但 M1a 没有实现这条语义"; break;
        case kLiftStopInvalid: why = "解码得到 Invalid"; break;
        case kLiftStopDivZero: why = "除零（#DE）"; break;
        case kLiftStopDivOverflow: why = "div 商溢出（#DE）"; break;
        case kLiftStopIdivOverflow: why = "idiv 商溢出（#DE）"; break;
        case kLiftStopSseAlign:
            why = "MOVAPS/MOVDQA 的 m128 操作数未按 16 字节对齐（SDM：对齐违例 #GP）";
            break;
        default:
            std::fprintf(stderr, "[lift_stop] 未知 kind=%u @ %08X ⇒ 宿主自身 bug\n", kind, ea);
            std::fflush(stderr);
            std::_Exit(4);
    }
    // 诊断面（⛔ 不进任何判据）：与解释器的 `unimplemented` / `trapInt` / `cpuFault` 同一档信息量。
    std::fprintf(stderr, "\n=== guest stop (lifted) ===\n  why : %s\n  ea  : %08X\n", why, ea);
    lift::dumpAtEa(cpu, ea, stderr);
    lift::dumpRegisters(cpu, stderr);
    std::fflush(stderr);
    cpu.running = false;
    cpu.stop_why = why;
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

// ---------------------------------------------------------------------------------------
// ④ lift_x87_sem —— 生成代码的 x87 宿主 helper（检查点 2 / D49）
// ---------------------------------------------------------------------------------------
// ★ 发射器把**超越函数族**（fsin/fcos/fsincos/fptan/fpatan/fyl2x/fyl2xp1/f2xm1）与
//   fprem/fprem1/fscale/fxtract/fxam 发成 `call void @lift_x87_sem(ptr %cpu, i32 <Op>)`。
//   理由：它们在本语料合计 <100 次/300 帧，而语义是"libm + 一堆边界"，内联不划算。
// ★ **语义只有一处**：这里转给 `execX87NoOperand`（`exec_fpu.cpp`）⇒ 就是解释器那条路径。
//   ⛔ 不许在这里另写一份公式（那会造出第二个模型）。
// ★ 无属性声明（`.ll` 里是 `declare void @lift_x87_sem(ptr, i32)`）：它读写 `%cpu` 指向的
//   状态对象（两层指针），与 `lift_one` 同一条理由 —— `argmemonly` / `readnone` 会变成
//   "可以被 LLVM 合法利用的假前提"。
extern "C" void lift_x87_sem(void* cpu_ptr, std::uint32_t op) {
    if (!cpu_ptr) {
        std::fprintf(stderr, "[lift_x87_sem] 调用方传了空 cpu（op=%u）\n", op);
        std::fflush(stderr);
        std::_Exit(4);
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    if (!lift::execX87NoOperand(cpu, static_cast<lift::Op>(op))) {
        // `execX87NoOperand` 只在"op 需要操作数"这类生成器内部不一致时失败 ⇒ 当作停机上报。
        std::fprintf(stderr, "[lift_x87_sem] op=%u 的单指令语义失败 @ eip=%08X ⇒ %s\n", op, cpu.eip,
                     cpu.stop_why != nullptr ? cpu.stop_why : "(未登记理由)");
        std::fflush(stderr);
        if (cpu.running) {
            cpu.running = false;
            cpu.stop_why = "lift_x87_sem: execX87NoOperand 返回 false 但没有置停";
        }
    }
}

namespace {

// ★ `lift_shim` / `lift_shim_id` 的**同一份**实现（见 lift_abi.h 的两个入口契约）。
//   `id_plus_one == 0` ⇒ 不知道 id ⇒ 走"块缓存 peek → 注册表反查"的老路（解释器臂逐字不变）；
//   `id_plus_one != 0` ⇒ AOT 的派发循环已经把 id 解析好了 ⇒ **零反查**（不拿 shims 锁、不扫 defs_）。
std::uint32_t liftShimCommon(std::uint32_t id_plus_one, std::uint32_t stub_ea, void* st, void* cpu_ptr) {
    if (!cpu_ptr || !st) {
        // ⛔ 不静默：这是调用方的编程错误（trampoline 只该把 shim EA + 有效状态递进来）
        std::fprintf(stderr, "[lift_shim] 调用方传了空指针（cpu=%p st=%p）\n", cpu_ptr, static_cast<void*>(st));
        std::fflush(stderr);
        std::_Exit(4);  // 宿主自身 bug（与 hostFault 同一档退出码）
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    // ① 物化：8 GPR + eflags。`invoke` 第一条就 `mem->rd32(cpu.esp())` 取返回地址；
    //    eip 只是诊断面，但口径要与解释器一致 —— dispatcher 命中 shim 块时 cpu.eip 就是这个桩 EA。
    //    ★ A′：AOT 臂 `st == liftRegsView(cpu)` ⇒ 自拷贝，跳过（非别名路径逐字不变）。
    if (!lift::regsAliasCpu(st, cpu)) lift::liftRegsToCpu(*static_cast<const LiftRegs*>(st), cpu);
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
    if (id_plus_one != 0) {
        // ★ AOT 快路径：id 已经由调用方解析（`lift_shim_id`），这里**不再**反查。
        id = id_plus_one - 1u;
        found = true;
    } else if (cpu.cache) {
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
    //    ★ A′：别名时是自拷贝，跳过。
    if (!lift::regsAliasCpu(st, cpu)) lift::liftCpuToRegs(cpu, *static_cast<LiftRegs*>(st));
    // ④ running == false ⇒ 停机哨兵（trampoline 负责报 stop_kind / stop_why）
    return cpu.running ? cpu.eip : ::kLiftHalt;
}

}  // namespace

extern "C" std::uint32_t lift_shim(std::uint32_t stub_ea, void* st, void* cpu_ptr) {
    return liftShimCommon(0u, stub_ea, st, cpu_ptr);
}

extern "C" std::uint32_t lift_shim_id(std::uint32_t id_plus_one, std::uint32_t stub_ea, void* st,
                                      void* cpu_ptr) {
    return liftShimCommon(id_plus_one, stub_ea, st, cpu_ptr);
}

// ---------------------------------------------------------------------------------------
// ⑤ lift_dyn —— S7 的动态入口（间接 jmp / 间接 call）：查派发表 + **尾调用**进目标
// ---------------------------------------------------------------------------------------
// ★ 为什么需要它：D53 的目标形态里"目标运行期才知道"的站点（`jmp reg/mem` / `call reg|mem`）
//   不再回宿主主循环，而是**尾巴跳**进派发表查到的那个生成函数。`lift_dyn` 就是那一跳的宿主
//   侧落点：它做一次 `EA → LiftFn` 查表，然后 `return fn(st, mem, ea, cpu)` —— **尾调用**。
//   ★ 最后一步用 `[[clang::musttail]]`：不是"希望编译器优化"，而是**编译期要求**。理由有两条：
//     ① 每个站点若多一个宿主帧，深递归（间接调用链）会先把宿主栈吃掉；
//        `tail call` 站点数就会与实际栈行为不符；
//     ② 间接 `call` 的 continuation 判据（`%r == resume`）要求目标的 `ret` 直接回到**调用者**。
//   若某天 musttail 不成立（换了编译器 / 签名漂了），构建当场红 —— 正是我们要的。
// ★ 查表失败（EA 不在派发表 / 没装查找）：**响亮停**（`running=false` + `stop_why`）并返回
//   `kLiftHalt`，与 trampoline 的"EA 不在表里"同一档（⛔ 不静默当成"跑完了"）。
// ★ 语义边界：这里**一行 guest 语义都不写** —— 目标函数的语义在生成代码里，查表只是路由。
namespace lift {
LiftDynLookup liftDynLookup = nullptr;
}  // namespace lift

// ★ 窗口 profile 的计数（缺省关 ⇒ `lift_dyn` 里只多一次 relaxed load）。
//   ⚠ 定义在**全局**作用域：声明在 `lift_abi.h` 的 `extern "C" lift_dyn` 旁边（同名一处）。
//   ★ 原子：`lift_dyn` 可能在主线程与音频 feeder 的 IR 执行器上**并发**执行，而
//     `liftDynCountOn` 由主线程的窗口 profile 在运行中开/关（见 lift_abi.h 的注记）。
std::atomic<bool> liftDynCountOn{false};
std::atomic<std::uint64_t> liftDynCalls{0};

extern "C" std::uint32_t lift_dyn(void* cpu_ptr, void* st, std::uint32_t ea) {
    if (!cpu_ptr || !st) {
        std::fprintf(stderr, "[lift_dyn] 调用方传了空指针（cpu=%p st=%p ea=%08X）\n", cpu_ptr,
                     static_cast<void*>(st), ea);
        std::fflush(stderr);
        std::_Exit(4);  // 宿主自身 bug（与 lift_shim / lift_one 同一档退出码）
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    // ★ 窗口 profile：只计数（闸门是 atomic<bool> ⇒ 缺省档一次 relaxed load）。位置在查表**之前**
    //   —— 查表失败（EA 不在表里 ⇒ 原样返回给 trampoline 裁决）那次也算一次动态入口。
    if (liftDynCountOn.load(std::memory_order_relaxed)) {
        liftDynCalls.fetch_add(1, std::memory_order_relaxed);
    }
    const lift::LiftDynLookup lookup = lift::liftDynLookup;
    if (lookup == nullptr) {
        std::fprintf(stderr,
                     "[lift_dyn] 派发表查找没安装（ea=%08X）⇒ 响亮停（执行臂忘了设 liftDynLookup）\n", ea);
        std::fflush(stderr);
        if (cpu.running) {
            cpu.running = false;
            cpu.stop_why = "lift_dyn: 派发表查找没安装（执行臂忘了设 liftDynLookup）";
        }
        return ::kLiftHalt;
    }
    const lift::LiftFn fn = lookup(ea);
    if (fn == nullptr) {
        // ★ **原样返回目标 EA**（= "这个 EA 不是生成函数的入口，请由 trampoline 裁决"）。
        //   为什么这是对的行为：间接跳转的最大一类目标是 **shim 桩 EA**（Win32 调用经
        //   `poly` 的 `j_XXX: jmp ds:__imp_XXX` ⇒ IAT 槽里是桩 EA）—— 桩由 trampoline 的 shim 分支
        //   执行，语义（`[esp]` = 返回地址、shim 自己弹参、置 eip）全在 `lift_shim` 里。
        //   ⇒ 这里**不执行** shim，也不写停：把 EA 交回去，与 S7 之前的 `ret i32 <目标>` 同一形状。
        //   ⛔ 这不是"回到派发主循环"的块出口：生成代码的下一条仍是 `ret i32 %r`（guest `ret`
        //      同形状），只是这一个 EA 由宿主裁决。真正不认识的 EA 会在 trampoline 里响亮停
        //      （那里会打印 EA 并区分"栈哨兵 / 不在表里"）。
        return ea;
    }
    if (cpu.mem == nullptr) {
        std::fprintf(stderr, "[lift_dyn] cpu.mem == nullptr（ea=%08X）⇒ 响亮停\n", ea);
        std::fflush(stderr);
        if (cpu.running) {
            cpu.running = false;
            cpu.stop_why = "lift_dyn: 宿主没有装配 guest 内存";
        }
        return ::kLiftHalt;
    }
#if defined(__clang__)
    // ★ 尾调用：形状上写成 `return fn(...)`（同返回类型 + 立即 return ⇒ 后端可做 sibling call）。
    //   ⚠ 为什么**不能**用 `[[clang::musttail]]`：它要求调用者与被调者的签名"兼容"，
    //   而 `lift_dyn` 是 3 参（cpu/st/ea）、`LiftFn` 是 4 参 ⇒ clang 直接拒绝
    //   （实测："target function has different number of parameters (expected 3 but has 4)"）。
    //   也**不能**改 `lift_dyn` 的签名去凑：那是 `.ll` 里的 ABI（任务书钉死 3 参）。
    //   ⇒ TCO 由**构建期/收工前的 objdump 复核**保证（`llvm-objdump -d` 里 `lift_dyn` 的
    //   结尾必须是 `jmp`，不是 `call` + `ret`）；读数与判据见 S7 的报告 / defect 台账。
    return fn(st, cpu.mem->host(0), ea, cpu_ptr);
#else
    return fn(st, cpu.mem->host(0), ea, cpu_ptr);
#endif
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
