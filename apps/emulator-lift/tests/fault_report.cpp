// emulator-lift · fault 归因（pure：不碰游戏 / LFS）
//
// ★ 盯的是 REQ-01M6JCBYBW8DV0920T28337ACF：fault 诊断必须按**异常地址**归因，
//   而不是按"最近一次 guest 访存"（`g_access` 在宿主 AV 时是陈旧值）。四条判据：
//   ① 纯函数 `inGuestSpace`：guest 空间内 ⇒ true；`0x1` ⇒ false；未预留 ⇒ false（两平台都跑）；
//   ② 宿主 AV（**先做一次 guest 访存把 `g_access` 弄脏**，再解引用 `0x1`）：
//      stderr 有 host fault 标记、**没有** `access     :` 行，退出码 = 4；
//   ③ 反向（同机制）：safe 模式读一个未提交页 ⇒ stderr **有** `access     :` 行、退出码 = 3
//      （证明按地址归因之后 guest 诊断没被弄坏）；
//   ④ win32：`hostPageSize()` 必须是 `GetSystemInfo().dwPageSize`（⛔ 不许硬编码 0x1000）。
// ⚠ ②③ 需要"子进程 + 收回它的 stderr"：POSIX 用 fork + 管道，Windows 用**同一个 exe 再启动一次**
//   （`--child-<mode>` 参数）+ 临时文件（写句柄经 STARTUPINFO 继承给子进程）。
//   ★ 两平台都**真跑**，没有 LIFT_SKIP；理由：fault 诊断以 `_Exit(3|4)` 结束进程 ——
//     在测试进程里直接触发会把测试自己带走，所以"触发 fault"的那一侧必须活在另一个进程里。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "cmdline.h"
#include "runtime/fault_state.h"  // AOT fault 现场（eip / 块 EA / 8 GPR + eflags / 32 字栈）
#include "runtime/input_plan.h"   // fault 文本里的 `input-plan` 摘要（C）
#include "runtime/lift_abi.h"     // LiftRegs
#include "runtime/memory.h"
#include "runtime/shims.h"       // ShimRegistry::invoke（`LIFT_WATCH` 的 shim 名接线）
#include "runtime/x86.h"         // X86（fault 现场的 cpu）

#if defined(_WIN32)
#include <cwchar>  // std::swprintf（check.h 已经带 <windows.h>：NOMINMAX / WIN32_LEAN_AND_MEAN）
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// 常驻：fault handler 靠 memory.cpp 里的 `g_memory`（由 GuestMemory 构造时登记）找 guest 空间。
lift::GuestMemory* g_mem = nullptr;

constexpr std::uint32_t kCommitted = 0x00401000u;    // 本进程唯一提交的 guest 页
constexpr std::uint32_t kUncommitted = 0x20000000u;  // 离它足够远（16 KiB 宿主页也不会误提交）

// 把 `g_access` 弄脏：一次成功的 guest 写（EA = kCommitted，宽度 4，方向 WRITE）
void dirtyGuestAccess() { g_mem->wr32(kCommitted, 0xA5A5A5A5u); }

// 子进程：guest 访存之后制造一次**宿主** AV（0x1 在 guest 空间外）
[[noreturn]] void childFaultHost() {
    dirtyGuestAccess();
    volatile int* const p = reinterpret_cast<volatile int*>(0x1);
    const volatile int v = *p;  // 宿主 AV，异常地址 = 0x1 ⇒ 应判成 host fault（_Exit(4)）
    (void)v;
    std::_Exit(90);  // 没 fault ⇒ 用例红（父进程期望 4，看到 90）
}

// 子进程：safe 模式读未提交页 ⇒ 走 guestFault（有 access 行，_Exit(3)）
[[noreturn]] void childFaultGuest() {
    g_mem->setSafeMode(true);
    (void)g_mem->rd32(kUncommitted);
    std::_Exit(91);  // 没 fault ⇒ 用例红
}

// 直接读 guest 映射的宿主字节（**绕过** rd*/wr* 的 g_access 记账）⇒ 未提交页 = 真平台 fault。
// ★ 为什么不用 `rd32(kUncommitted)`：`rd32` 会先把 `g_access` 改成 `READ 0x20000000`，
//   于是 `access` 行与 `fault ea` 行恰好同值 —— 证明不了"两行不互相冒充"（本条用例的判据）。
[[noreturn]] void rawGuestReadFault(std::uint32_t ea) {
    const volatile std::uint32_t* const p =
        reinterpret_cast<const volatile std::uint32_t*>(g_mem->host(ea));
    const volatile std::uint32_t v = *p;
    (void)v;
    std::_Exit(95);  // 没 fault ⇒ 用例红
}

// 子进程：**不开 safe mode** 的真平台 guest fault（异常地址 = host(0)+0x20000000）。
// 先弄脏 g_access（WRITE kCommitted），再裸读未提交页 ⇒ `access` 行必须仍是**旧值** + 显式后缀。
[[noreturn]] void childFaultPlatform() {
    dirtyGuestAccess();
    rawGuestReadFault(kUncommitted);
}

// ---- `LIFT_FAULT_DUMP`：opt-in 的 fault 内存 dump（BUG-32 的根因定位面）----
constexpr std::uint32_t kDumpPage = 0x00402000u;      // 已提交的 dump 页（DE AD BE EF 模式）
constexpr std::uint32_t kDumpFollow = 0x00402040u;    // 这个 dword = 指向 kDumpPage 的指针
constexpr std::uint32_t kDumpZeroPtr = 0x00402048u;   // 这个 dword = 0
constexpr std::uint32_t kDumpBadPtr = 0x0040204Cu;    // 这个 dword = kUncommitted（未提交）
constexpr std::uint32_t kDumpDeltaSlot = 0x00402050u;  // 这个 dword = kDumpObj（`*-8` 跟随用）
constexpr std::uint32_t kDumpOverflowSlot = 0x00402054u;  // 这个 dword = 4（`*-8` 下溢）
constexpr std::uint32_t kDumpObj = 0x00402080u;        // 假对象 P：P-8 = 活块头（'LIFT' + 0x450）

void setFaultDumpEnv(const char* spec) {
#if defined(_WIN32)
    _putenv_s("LIFT_FAULT_DUMP", spec);
#else
    ::setenv("LIFT_FAULT_DUMP", spec, 1);
#endif
}

// 子进程：自己装 `LIFT_FAULT_DUMP`（env → 解析 → 安装这条启动期链路），再制造真平台 fault。
// ★ spec 的 8 段覆盖：已提交 / 未提交（整段拒绝） / `*` 跟随命中 / 指针=0 / 指针→未提交 /
//   失败段**之后**的已提交段（"一段失败不影响后段"） / `*-8` 下溢 / `*-8` 打块头（增量）。
[[noreturn]] void childFaultDump() {
    if (!g_mem->commit(kDumpPage, lift::GuestMemory::kPage, lift::RegionKind::Heap, "fault-report-dump")) {
        std::fprintf(stderr, "提交 dump 页失败：%s\n", g_mem->lastError());
        std::_Exit(96);
    }
    const std::uint8_t pat[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    for (std::uint32_t i = 0; i < 32; ++i) g_mem->wr8(kDumpPage + i, pat[i % 4u]);
    g_mem->wr32(kDumpFollow, kDumpPage);
    g_mem->wr32(kDumpZeroPtr, 0);
    g_mem->wr32(kDumpBadPtr, kUncommitted);
    // 增量：`*-8` 读块头。P-8 = magic 'LIFT'（小端字节序 54 46 49 4C）+ 请求字节数 0x450。
    g_mem->wr32(kDumpDeltaSlot, kDumpObj);
    g_mem->wr32(kDumpObj - 8u, 0x4C494654u);  // magic 'LIFT'
    g_mem->wr32(kDumpObj - 4u, 0x450u);       // 请求字节数（活块）
    g_mem->wr32(kDumpObj, 0x00528BE8u);       // 假 vtable（对象首 dword）
    g_mem->wr32(kDumpOverflowSlot, 4u);       // ptr=4 + delta=-8 ⇒ 32 位下溢 ⇒ ✗ 且继续
    setFaultDumpEnv(
        "0x00402000:16,0x20000000:8,0x00402040:*16,0x00402048:*16,0x0040204C:*16,0x00402010:16,"
        "0x00402054:*-8:16,0x00402050:*-8:16");
    char derr[256] = {};
    if (!lift::initFaultDumpFromEnv(derr, sizeof(derr))) {
        std::fprintf(stderr, "LIFT_FAULT_DUMP 解析失败：%s\n", derr);
        std::_Exit(97);
    }
    dirtyGuestAccess();
    rawGuestReadFault(kUncommitted);
}

// =====================================================================================
// AOT 臂的 guest fault 现场（`REQ-01M7PG086DG4BBPA40VCVZTP4X` A）+ fault 文本里的
// `input-plan` 摘要（同单 C）。
//
// ★ 为什么在子进程里：`guestFault` 以 `_Exit(3)` 结束进程（诊断的判据之一就是退出码）。
// ★ 为什么要合成现场：AOT 没有 `Machine` ⇒ 解释器的 `liftFaultHook` 不成立；本用例直接驱动
//   `fault_state.h`（AOT 的钩子）与 `InputPlan::printFaultSummary`（C 的唯一出口），不跑游戏、
//   不读 LFS 资产 ⇒ pure 档。
// ★ 修前红：字段不存在时（`eip` / `lift_cc_` / `eax=` / 第 32 个栈字 / `input-plan: steps=`）
//   断言当场红 —— 这批断言就是"缺字段"的机械判据。
// =====================================================================================
constexpr std::uint32_t kAotStack = 0x10000000u;   // 合成 guest 栈（32 字）
constexpr std::uint32_t kAotBlockEa = 0x004B06D0u;  // 合成"当前派发块" EA

void faultPlanText(void* ctx, std::FILE* out) {
    auto* plan = static_cast<lift::InputPlan*>(ctx);
    if (plan != nullptr) plan->printFaultSummary(out);
}

[[noreturn]] void childFaultAot() {
    if (!g_mem->commit(kAotStack, lift::GuestMemory::kPage, lift::RegionKind::Stack, "fault-report-stack")) {
        std::fprintf(stderr, "提交合成 guest 栈页失败：%s\n", g_mem->lastError());
        std::_Exit(92);
    }
    for (std::uint32_t i = 0; i < 32; ++i) g_mem->wr32(kAotStack + i * 4u, 0xA0000000u + i);
    // 合成 cpu / LiftRegs：每个寄存器一个可辨认的值（断言里逐字出现）。
    lift::X86 cpu{};
    cpu.mem = g_mem;
    cpu.eip = kAotBlockEa;  // lifted 代码不逐条更新 eip ⇒ 它 = 当前块 EA
    LiftRegs regs{};
    regs.r[0] = 0x11111111u;  // eax
    regs.r[1] = 0x22222222u;  // ecx
    regs.r[2] = 0x33333333u;  // edx
    regs.r[3] = 0x44444444u;  // ebx
    regs.r[4] = kAotStack;    // esp
    regs.r[5] = 0u;           // ebp（0 ⇒ ebp 链只打表头，不编造帧）
    regs.r[6] = 0x66666666u;  // esi
    regs.r[7] = 0x77777777u;  // edi
    regs.eflags = 0x246u;
    std::uint32_t block_ea = kAotBlockEa;
    lift::GuestFaultSite site;
    site.cpu = &cpu;
    site.regs = &regs;
    site.block_ea = &block_ea;
    site.stub_page = 0x7FFE0000u;
    site.stub_span = 0x1000u;  // eip 不在桩页 ⇒ "块内直接访存"那条腿
    lift::setGuestFaultSite(site);
    lift::installAotFaultHook();  // = AotHost::init 装的那一个
    // C：fault 文本里的 `input-plan` 摘要（同一份 InputPlan / RecipeOutcome）。
    lift::InputPlan plan;
    std::string perr;
    if (!plan.loadText("await script NOSUCHSCRIPT\nclick 1 2\n", "mem", &perr)) {
        std::fprintf(stderr, "合成配方被拒：%s\n", perr.c_str());
        std::_Exit(93);
    }
    lift::setGuestFaultTextHook(&plan, &faultPlanText);
    g_mem->setSafeMode(true);
    (void)g_mem->rd32(kUncommitted);  // ⇒ guestFault ⇒ 两个钩子 ⇒ _Exit(3)
    std::_Exit(94);
}

// =====================================================================================
// `LIFT_WATCH`：opt-in 的 guest / shim **写观察**（BUG-32 的"谁在 P-8 写了 16 字节"）
// ★ 三层判据：① 解析器（进程内）；② 走 `lift_note_write`（AOT 臂的**唯一** guest store 观察点）
//   证明 follow 的时序 —— 命中时只 pending，**下一次写的最开头**才读 P，然后用**更新后**的观察集
//   判本次写；③ shim 侧写入（`wr32`）也要命中，且不在观察集的写**一行都不多**。
// =====================================================================================
constexpr std::uint32_t kWatchSlot = 0x1005AB58u;    // BUG-32 的槽 EA（真值；槽里放合成指针 P）
constexpr std::uint32_t kWatchObj = 0x1005C008u;     // 合成对象 P ⇒ [P-8,P+8) = [0x1005C000,0x1005C010)
constexpr std::uint32_t kWatchBlockA = 0x004A5115u;  // 写槽的块 EA（`mov ecx,[esi+ebx*4+0A5D8h]`）
constexpr std::uint32_t kWatchBlockB = 0x004A5125u;  // 写 P-8 的块 EA（fault 候选 `mov eax,[edx+20h]`）
constexpr std::uint32_t kWatchShimEa = 0x00405000u;  // shim 侧的观察段（已提交页）

void setFaultWatchEnv(const char* spec) {
#if defined(_WIN32)
    _putenv_s("LIFT_WATCH", spec);
#else
    ::setenv("LIFT_WATCH", spec, 1);
#endif
}

// 子进程：guest store 走 `lift_note_write`，证明 follow 的**时序**：
// ① 命中 follow 段时只挂 pending（那一刻槽里还是旧值）；② 槽被 store 之后，下一次写的最开头才读 P；
// ③ 把 [P-8,P+8) 加进观察集，并用**更新后的**观察集判本次写（⇒ 点名写 P-8 的块 EA）。
[[noreturn]] void childWatchFollow() {
    setFaultWatchEnv("0x1005AB58:4>follow");
    char werr[256] = {};
    if (!lift::initFaultWatchFromEnv(werr, sizeof(werr))) {
        std::fprintf(stderr, "LIFT_WATCH 解析失败：%s\n", werr);
        std::_Exit(98);
    }
    if (!g_mem->commit(kWatchSlot, 16, lift::RegionKind::Heap, "fault-report-watch-slot") ||
        !g_mem->commit(kWatchObj, lift::GuestMemory::kPage, lift::RegionKind::Heap,
                       "fault-report-watch-obj")) {
        std::fprintf(stderr, "提交 watch 页失败：%s\n", g_mem->lastError());
        std::_Exit(96);
    }
    lift::X86 cpu{};
    cpu.mem = g_mem;
    cpu.cache = nullptr;  // SMC 记账 no-op ⇒ 本用例只看 watch 接线（不建 BlockCache）
    cpu.eip = kWatchBlockA;
    lift_note_write(&cpu, kWatchSlot, 4);  // ① 命中 follow 段（store 还没发生）
    const std::uint32_t obj = kWatchObj;
    // ② store 完成：**绕过** wr*（否则 shim 观察会冒充"下一次写"，正好吃掉本条判据）
    std::memcpy(g_mem->host(kWatchSlot), &obj, 4);
    cpu.eip = kWatchBlockB;
    lift_note_write(&cpu, kWatchObj - 8u, 16);  // ③ 开头补读 P ⇒ arm ⇒ 再判本次写
    std::_Exit(0);
}

// 被 `ShimRegistry::invoke` 调用的探针：写一次 guest 内存（⇒ `watchShimWrite` 命中）。
void watchProbeShim(lift::X86& cpu, lift::ShimCall&) { cpu.mem->wr32(kWatchShimEa + 12u, 0xCAFEBABEu); }
constexpr std::uint32_t kWatchShimStack = 0x00406000u;  // invoke 从 [esp] 读返回地址

// 子进程：shim 侧的写（`wr32` 内联 helper）也要命中；命中行带**当前 shim 名**；不在观察集的写零行。
[[noreturn]] void childWatchShim() {
    setFaultWatchEnv("0x00405000:16");
    char werr[256] = {};
    if (!lift::initFaultWatchFromEnv(werr, sizeof(werr))) {
        std::fprintf(stderr, "LIFT_WATCH 解析失败：%s\n", werr);
        std::_Exit(98);
    }
    if (!g_mem->commit(kWatchShimEa, 16, lift::RegionKind::Heap, "fault-report-watch-shim") ||
        !g_mem->commit(kWatchShimStack, lift::GuestMemory::kPage, lift::RegionKind::Stack,
                       "fault-report-watch-shim-stack")) {
        std::fprintf(stderr, "提交 watch 页失败：%s\n", g_mem->lastError());
        std::_Exit(96);
    }
    g_mem->wr32(kWatchShimEa, 0x12345678u);  // 未设名字 ⇒ `shim=?`（⛔ 不编造名字）
    lift::setWatchShimName("test-shim");
    g_mem->wr32(kWatchShimEa + 4u, 0x9ABCDEF0u);  // ⇒ `shim=test-shim`
    lift::setWatchShimName(nullptr);
    g_mem->wr32(kWatchShimEa + 8u, 0x0BADF00Du);  // 名字清掉 ⇒ 回到 `shim=?`
    // ★ 走**真实** `ShimRegistry::invoke`：名字由 invoke 从 `def->name` 设（测试**不**自己设）⇒
    //   钉住 `shims.cpp` 的接线（去掉那行 ⇒ `shim=?` 而不是 `registered-shim` ⇒ 本用例红）。
    lift::ShimRegistry shims;
    lift::ShimDef def;
    def.module = "test";
    def.name = "registered-shim";
    def.conv = lift::Conv::Stdcall;
    def.argc = 0;
    def.fn = &watchProbeShim;
    g_mem->wr32(kWatchShimStack, 0xFFFFFFFFu);  // invoke 从 [esp] 读返回地址（不真的跳回去）
    lift::X86 cpu{};
    cpu.mem = g_mem;
    cpu.esp() = kWatchShimStack;
    shims.invoke(cpu, def);  // ⇒ 探针写 +12 ⇒ `shim=registered-shim`
    g_mem->wr32(kCommitted, 0x0BADF00Du);  // 不在观察集 ⇒ 不许多一行
    std::_Exit(0);
}

// 子进程：**不设** `LIFT_WATCH`（显式清掉 —— 开发机的 shell 可能设过）⇒ 一条 `watch      :` 都不打。
[[noreturn]] void childWatchOff() {
    setFaultWatchEnv("");
    char werr[256] = {};
    if (!lift::initFaultWatchFromEnv(werr, sizeof(werr))) {
        std::fprintf(stderr, "LIFT_WATCH 解析失败：%s\n", werr);
        std::_Exit(98);
    }
    lift::X86 cpu{};
    cpu.mem = g_mem;
    cpu.cache = nullptr;
    cpu.eip = kWatchBlockA;
    lift_note_write(&cpu, kWatchSlot, 4);           // guest store（观察点被关档门挡住）
    g_mem->wr32(kCommitted, 0x12345678u);           // shim 侧 wr*
    g_mem->writeBytes(kCommitted + 4u, "abcd", 4);  // shim 侧 bulk 写
    std::_Exit(0);
}

struct ChildRun {
    int exit_code = -1;
    bool exited = false;
    std::string stderr_text;
};

// 子进程 stderr 的字节 → `stderr_text`。
// ★ Windows 上子进程的 stderr 是 CRT **文本模式**（继承的文件句柄也一样）⇒ 每个 `\n` 被翻成
//   `\r\n`，而断言按 POSIX 的 `\n` 写。`\r` 只可能来自这次翻译 ⇒ 统一剥掉。
//   ⛔ 不改产品输出、⛔ 不改断言（父级裁决 2026-11-18：只改测试的捕获侧）。
void appendChildStderr(ChildRun& r, const char* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] != '\r') r.stderr_text.push_back(data[i]);
    }
}

#if defined(_WIN32)

// 子进程 = **同一个 exe** 加 `--child-<mode>` 再启动一次（Windows 没有 fork）。
// ★ 子进程的 stderr 写进临时文件（`CreateProcessW` 把该写句柄继承过去），父进程等它退出后读回来 ——
//   这与 POSIX 那条管道拿到的是**同一种证据**（子进程真的打了什么 + 真实退出码）。
ChildRun runChild(const wchar_t* mode) {
    ChildRun r;
    wchar_t exe[MAX_PATH] = {};
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        lift_test::failf(__FILE__, __LINE__, "GetModuleFileNameW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }

    wchar_t tmp_dir[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tmp_dir) == 0) {
        lift_test::failf(__FILE__, __LINE__, "GetTempPathW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }
    wchar_t tmp_file[MAX_PATH] = {};
    if (::GetTempFileNameW(tmp_dir, L"lfr", 0, tmp_file) == 0) {
        lift_test::failf(__FILE__, __LINE__, "GetTempFileNameW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }

    // 写句柄要可继承；父进程在 CreateProcess 之后立刻关掉自己那份（否则子进程退出后文件仍被占着）。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE out = ::CreateFileW(tmp_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        lift_test::failf(__FILE__, __LINE__, "CreateFileW(stderr 临时文件) 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        ::DeleteFileW(tmp_file);
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;  // 子进程正常路径不写 stdout；一并收进来不影响判据
    si.hStdError = out;

    // ★ 命令行拼装与其余夹具同一份实现（tests/cmdline.h）：exe 路径按 MSVC 规则加引号
    //   （本 exe 在构建树里，文件名不含引号；统一走一份实现，免得哪天路径变成以 `\` 结尾）。
    const std::wstring cmd_w = lift_test::quoteArg(exe) + L" --child-" + mode;
    std::vector<wchar_t> cmd(cmd_w.begin(), cmd_w.end());
    cmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(out);  // 子进程已继承一份
    if (!ok) {
        lift_test::failf(__FILE__, __LINE__, "CreateProcessW 失败（err=%lu，cmd=%ls）",
                         static_cast<unsigned long>(::GetLastError()), cmd_w.c_str());
        ::DeleteFileW(tmp_file);
        return r;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    r.exited = ::GetExitCodeProcess(pi.hProcess, &code) != 0;
    r.exit_code = r.exited ? static_cast<int>(code) : -1;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    const HANDLE in = ::CreateFileW(tmp_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        char buf[4096];
        DWORD n = 0;
        while (::ReadFile(in, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr) != 0 && n > 0) {
            appendChildStderr(r, buf, static_cast<std::size_t>(n));
        }
        ::CloseHandle(in);
    }
    ::DeleteFileW(tmp_file);
    // 子进程的 stderr 原样留一份在测试输出里（红了才有用；绿了只是背景）
    std::printf("[child stderr]\n%s[/child stderr]\n", r.stderr_text.c_str());
    return r;
}

#else  // POSIX

// fork 一个子进程跑 fn()，把它写到 fd 2 的东西全部收回来（先读干净再 waitpid，避免管道写满互锁）。
ChildRun runChild(void (*fn)()) {
    ChildRun r;
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        lift_test::failf(__FILE__, __LINE__, "pipe() 失败");
        return r;
    }
    // ★ fork 前先把本进程的 stdio 缓冲清空：子进程里的 fault 诊断会 `fflush(nullptr)`，
    //   不清就会把父进程**继承过去的缓冲**再写一遍（同一条 [child stderr] 打印两次）。
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        lift_test::failf(__FILE__, __LINE__, "fork() 失败");
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }
    if (pid == 0) {
        ::close(fds[0]);
        if (::dup2(fds[1], STDERR_FILENO) < 0) std::_Exit(93);
        ::close(fds[1]);
        fn();
        std::_Exit(92);  // fn 是 [[noreturn]]，不该走到这里
    }
    ::close(fds[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) appendChildStderr(r, buf, static_cast<std::size_t>(n));
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        lift_test::failf(__FILE__, __LINE__, "waitpid() 失败");
        return r;
    }
    r.exited = WIFEXITED(status) != 0;
    r.exit_code = r.exited ? WEXITSTATUS(status) : -1;
    if (!r.exited) lift_test::failf(__FILE__, __LINE__, "子进程被信号杀死（signal %d）", WTERMSIG(status));
    // 子进程的 stderr 原样留一份在测试输出里（红了才有用；绿了只是背景）
    std::printf("[child stderr]\n%s[/child stderr]\n", r.stderr_text.c_str());
    return r;
}

#endif  // _WIN32

// ④ A + C 的判据（两个平台的子进程跑完都走这一份；⛔ 不各写一套）。
void checkAotFault(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 3);  // guestFault 的退出码口径不变
    LIFT_CHECK(r.stderr_text.find("=== guest memory fault") != std::string::npos);
    // ---- A：eip + 当前块 EA（`lift_cc_<EA>`）+ 8 GPR + eflags ----
    LIFT_CHECK(r.stderr_text.find("eip        : 004B06D0") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("lift_cc_004B06D0") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("eax=11111111 ecx=22222222 edx=33333333 ebx=44444444") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("esp=10000000 ebp=00000000 esi=66666666 edi=77777777") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("eflags=00000246") != std::string::npos);
    // 32 字栈：第 1 个与第 32 个都要在（少打一个字就红 ⇒ 钉住"32 字"这个口径）
    LIFT_CHECK(r.stderr_text.find("A0000000") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("A000001F") != std::string::npos);
    // ---- C：fault 文本里的 `input-plan` 摘要（与正常收口同一份数字 + RecipeOutcome）----
    LIFT_CHECK(r.stderr_text.find("input-plan: steps=2 fired=0 pending=2") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("outcome=running") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("plan-step  : mem:1") != std::string::npos);
}

// ⑤ 真平台 guest fault 的判据（两个平台的子进程跑完都走这一份）。
// ★ 一条用例同时钉两件事：① `fault ea` 的**数值**（host − base）算对；② `access` 行不会冒充本次
//   fault —— 它仍是上一条 shim 侧写（WRITE 0x00401000），只是行尾多了显式后缀。
void checkPlatformFault(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 3);
    LIFT_CHECK(r.stderr_text.find("=== guest memory fault") != std::string::npos);
    // ① 本次 fault 的 guest EA（host 0x…20000020 − base 0x…20000000 = 0x20000000）
    LIFT_CHECK(r.stderr_text.find("fault ea   : 0x20000000") != std::string::npos);
    // 换算依据必须在同一行（人复核用；⛔ 不是"只给结论"）
    LIFT_CHECK(r.stderr_text.find(" - base 0x") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("region=(未登记") != std::string::npos);
#if defined(_WIN32)
    LIFT_CHECK(r.stderr_text.find("dir=READ") != std::string::npos);  // ExceptionInformation[0] == 0
#else
    LIFT_CHECK(r.stderr_text.find("dir=unknown") != std::string::npos);  // 信号上下文没有方向
#endif
    // ② 两行不互相冒充：access = 最近一次 shim 侧访存（这次是 WRITE 0x00401000），带后缀
    LIFT_CHECK(r.stderr_text.find("access     : WRITE 0x00401000") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("← 最近一次 guest 访存（shim 侧记录）；不是本次 fault（本次见 fault ea）") !=
               std::string::npos);
}

// ⑥ fault 时的 opt-in dump（`LIFT_FAULT_DUMP`）：已提交 / 未提交 / 跟随一层 / 指针=0 /
//   指针→未提交 / 失败段之后的段仍继续。
void checkDumpAtFault(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 3);
    LIFT_CHECK(r.stderr_text.find("fault ea   : 0x20000000") != std::string::npos);
    // [0] 已提交段：DE AD BE EF 模式（hexdump 逐字节）
    LIFT_CHECK(r.stderr_text.find("  dump       : [0] 0x00402000 + 16") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("00402000: DE AD BE EF DE AD BE EF DE AD BE EF DE AD BE EF") !=
               std::string::npos);
    // [1] 未提交整段 ⇒ 一行 ✗，不尝试读
    LIFT_CHECK(r.stderr_text.find("  dump       : [1] 0x20000000 + 8  ✗ 未提交（整段拒绝读）") !=
               std::string::npos);
    // [2] `*` 跟随一层：先读 0x00402040 处的 dword（= kDumpPage）再 dump
    LIFT_CHECK(r.stderr_text.find("  dump       : [2] 0x00402040:*16  → ptr=0x00402000") !=
               std::string::npos);
    // [3] 指针为 0 ⇒ 不跟随
    LIFT_CHECK(r.stderr_text.find("  dump       : [3] 0x00402048:*16  ✗ 指针为 0（不跟随）") !=
               std::string::npos);
    // [4] 指针指向未提交页 ⇒ 不跟随（且把指针值打出来）
    LIFT_CHECK(r.stderr_text.find(
                   "  dump       : [4] 0x0040204C:*16  ✗ 指针 0x20000000 指向未提交页（不跟随）") !=
               std::string::npos);
    // [5] 失败段之后仍继续：最后一段照常 dump（"一段失败不影响后段"）
    LIFT_CHECK(r.stderr_text.find("  dump       : [5] 0x00402010 + 16") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("00402010: DE AD BE EF") != std::string::npos);
    // [6] 增量：ptr=4 + delta=-8 ⇒ 32 位下溢 ⇒ ✗（不崩、不静默）
    LIFT_CHECK(r.stderr_text.find("  dump       : [6] 0x00402054:*-8:16  ✗ ptr+delta 越界（不跟随）") !=
               std::string::npos);
    // [7] 增量：`*-8` 打块头（活块 = magic 'LIFT' + 请求字节数 0x450），且 [6] 失败后仍继续
    LIFT_CHECK(r.stderr_text.find("  dump       : [7] 0x00402050:*-8:16  → ptr=0x00402080 delta=-8") !=
               std::string::npos);
    LIFT_CHECK(r.stderr_text.find("00402078: 54 46 49 4C 50 04 00 00") != std::string::npos);
}

// ⑦ 缺省档 / safe-mode 的"不多打"反向断言（find == npos 才是判据 —— 少一行不是"看起来像没打"）。
void checkDefaultFaultText(const ChildRun& safe, const ChildRun& platform) {
    // safe-mode（没有平台地址 ⇒ 结构性不打 `fault ea`）
    LIFT_CHECK(safe.exited);
    LIFT_CHECK_EQ_I64(safe.exit_code, 3);
    LIFT_CHECK(safe.stderr_text.find("fault ea") == std::string::npos);
    LIFT_CHECK(safe.stderr_text.find("dump       :") == std::string::npos);
    // `access` 行**整行（含换行）逐字**：safe-mode 没有任何后缀
    LIFT_CHECK(safe.stderr_text.find("access     : READ 0x20000000  size=4\n") != std::string::npos);
    // 平台 fault（不设 env）：旧三行照旧在（"只新增行、不改旧行"），但 dump 一条都不多
    LIFT_CHECK(platform.exited);
    LIFT_CHECK_EQ_I64(platform.exit_code, 3);
    LIFT_CHECK(platform.stderr_text.find("why        :") != std::string::npos);
    LIFT_CHECK(platform.stderr_text.find("access     :") != std::string::npos);
    LIFT_CHECK(platform.stderr_text.find("region(ea) :") != std::string::npos);
    LIFT_CHECK(platform.stderr_text.find("dump       :") == std::string::npos);
}

// ①' 解析器（进程内；两个平台同一份判据）：合法 / 空串 / 缺冒号 / 坏 hex / 长度边界 /
//   段数 / 总字节 / EA+LEN 越 4 GiB / spec 本身超长。
void checkFaultDumpParser() {
    lift::FaultDumpProgram p{};
    char err[256] = {};
    // 合法：`*` 跟随 + 普通段
    LIFT_CHECK(lift::parseFaultDumpSpec("0x1005AB58:*64,0x10050000:32", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 2u);
    LIFT_CHECK_EQ_U32(p.ranges[0].ea, 0x1005AB58u);
    LIFT_CHECK_EQ_U32(p.ranges[0].len, 64u);
    LIFT_CHECK(p.ranges[0].follow);
    LIFT_CHECK_EQ_U32(p.ranges[1].ea, 0x10050000u);
    LIFT_CHECK_EQ_U32(p.ranges[1].len, 32u);
    LIFT_CHECK(!p.ranges[1].follow);
    // `EA:4*LEN` 是 `EA:*LEN` 的别名（EA 的 0x 可省）
    LIFT_CHECK(lift::parseFaultDumpSpec("10000000:4*16", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 1u);
    LIFT_CHECK_EQ_U32(p.ranges[0].ea, 0x10000000u);
    LIFT_CHECK_EQ_U32(p.ranges[0].len, 16u);
    LIFT_CHECK(p.ranges[0].follow);
    LIFT_CHECK_EQ_I64(p.ranges[0].delta, 0);
    // 增量：`*<delta>:<LEN>`（十进制带符号）；`*LEN` / `4*LEN` 的 delta 恒 0
    LIFT_CHECK(lift::parseFaultDumpSpec("0x1000:*-8:16", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 1u);
    LIFT_CHECK_EQ_U32(p.ranges[0].ea, 0x1000u);
    LIFT_CHECK_EQ_U32(p.ranges[0].len, 16u);
    LIFT_CHECK(p.ranges[0].follow);
    LIFT_CHECK_EQ_I64(p.ranges[0].delta, -8);
    LIFT_CHECK(lift::parseFaultDumpSpec("0x1000:*+0:8", &p, err, sizeof(err)));
    LIFT_CHECK(p.ranges[0].follow);
    LIFT_CHECK_EQ_I64(p.ranges[0].delta, 0);
    LIFT_CHECK_EQ_U32(p.ranges[0].len, 8u);
    LIFT_CHECK(lift::parseFaultDumpSpec("0x1000:*64", &p, err, sizeof(err)));
    LIFT_CHECK(p.ranges[0].follow);
    LIFT_CHECK_EQ_I64(p.ranges[0].delta, 0);
    LIFT_CHECK_EQ_U32(p.ranges[0].len, 64u);
    // 非法：`4*-8:64`（别名只许 delta=0） / delta 越 ±1 MiB / `*-8:0` / `*--8:8`
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1000:4*-8:64", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1000:*-1048577:8", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1000:*1048577:8", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1000:*-8:0", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1000:*--8:8", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    // 空串 = 不 dump（shell 里 `LIFT_FAULT_DUMP=` 常见，不当错误）
    LIFT_CHECK(lift::parseFaultDumpSpec("", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 0u);
    // 缺冒号
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1005AB58", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    // 坏 hex
    LIFT_CHECK(!lift::parseFaultDumpSpec("0xZZ:4", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(std::strstr(err, "不是合法数字") != nullptr);
    // 长度 0 / 257
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1005AB58:0", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "256") != nullptr);
    LIFT_CHECK(!lift::parseFaultDumpSpec("0x1005AB58:257", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "256") != nullptr);
    // 9 段（上限 8）
    std::string nine;
    for (int i = 0; i < 9; ++i) {
        if (i != 0) nine += ',';
        nine += "0x10000000:1";
    }
    LIFT_CHECK(!lift::parseFaultDumpSpec(nine.c_str(), &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 9 段") != nullptr);
    // 总 1025 B（4×256 + 1）
    std::string big;
    for (int i = 0; i < 4; ++i) {
        if (i != 0) big += ',';
        big += "0x10000000:256";
    }
    big += ",0x10000000:1";
    LIFT_CHECK(!lift::parseFaultDumpSpec(big.c_str(), &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "1024") != nullptr);
    // EA+LEN 越过 4 GiB
    LIFT_CHECK(!lift::parseFaultDumpSpec("0xFFFFFFF0:32", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "4 GiB") != nullptr);
    // spec 本身超过 1024 字符
    const std::string long_spec(1025, '0');
    LIFT_CHECK(!lift::parseFaultDumpSpec(long_spec.c_str(), &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "1024") != nullptr);
}

// ①'' `LIFT_WATCH` 的解析器（进程内；两个平台同一份判据）：合法 / 空串 / 缺冒号 / 坏 hex /
//   LEN 边界 / follow 只许 LEN=4 / 段数 / spec 本身超长。
void checkFaultWatchParser() {
    lift::FaultWatchProgram p{};
    char err[256] = {};
    // 合法：follow 段（BUG-32 的槽）+ 普通段
    LIFT_CHECK(lift::parseFaultWatchSpec("0x1005AB58:4>follow", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 1u);
    LIFT_CHECK_EQ_U32(p.r[0].ea, 0x1005AB58u);
    LIFT_CHECK_EQ_U32(p.r[0].len, 4u);
    LIFT_CHECK(p.r[0].follow);
    LIFT_CHECK(p.r[0].armed);
    LIFT_CHECK(lift::parseFaultWatchSpec("0x1000:16", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 1u);
    LIFT_CHECK_EQ_U32(p.r[0].len, 16u);
    LIFT_CHECK(!p.r[0].follow);
    LIFT_CHECK(lift::parseFaultWatchSpec("10000000:1,0x20000000:8", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 2u);
    LIFT_CHECK_EQ_U32(p.r[0].ea, 0x10000000u);
    LIFT_CHECK_EQ_U32(p.r[1].ea, 0x20000000u);
    LIFT_CHECK_EQ_U32(p.r[1].len, 8u);
    // 空串 = 关闭（shell 里 `LIFT_WATCH=` 常见，不当错误）
    LIFT_CHECK(lift::parseFaultWatchSpec("", &p, err, sizeof(err)));
    LIFT_CHECK_EQ_U32(p.count, 0u);
    // 缺冒号
    LIFT_CHECK(!lift::parseFaultWatchSpec("0x1005AB58", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    // 坏 hex
    LIFT_CHECK(!lift::parseFaultWatchSpec("0xZZ:4", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    LIFT_CHECK(std::strstr(err, "不是合法数字") != nullptr);
    // LEN 0 / 17（上限 16）
    LIFT_CHECK(!lift::parseFaultWatchSpec("0x1000:0", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "1..16") != nullptr);
    LIFT_CHECK(!lift::parseFaultWatchSpec("0x1000:17", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "1..16") != nullptr);
    // follow 只许 LEN=4（8 字节的槽没有"下一次写读 4 字节当指针"的语义）
    LIFT_CHECK(!lift::parseFaultWatchSpec("0x1000:8>follow", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "follow") != nullptr);
    LIFT_CHECK(std::strstr(err, "4") != nullptr);
    // `>follow` 拼错也算坏（⛔ 不静默当普通段）
    LIFT_CHECK(!lift::parseFaultWatchSpec("0x1000:4>follo", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 1 段") != nullptr);
    // 9 段（上限 8）
    std::string nine;
    for (int i = 0; i < 9; ++i) {
        if (i != 0) nine += ',';
        nine += "0x10000000:1";
    }
    LIFT_CHECK(!lift::parseFaultWatchSpec(nine.c_str(), &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "第 9 段") != nullptr);
    // EA+LEN 越过 4 GiB
    LIFT_CHECK(!lift::parseFaultWatchSpec("0xFFFFFFF8:16", &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "4 GiB") != nullptr);
    // spec 本身超过 1024 字符
    const std::string long_spec(1025, '0');
    LIFT_CHECK(!lift::parseFaultWatchSpec(long_spec.c_str(), &p, err, sizeof(err)));
    LIFT_CHECK(std::strstr(err, "1024") != nullptr);
}

// ⑧ follow 的时序（走 `lift_note_write` = AOT 臂的真实接线）。
void checkWatchFollow(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 0);
    // ① 命中 follow 段 ⇒ 只挂 pending（store 还没发生，⛔ 此刻读槽会读到旧值）
    LIFT_CHECK(r.stderr_text.find("watch      : follow pending 0x1005AB58") != std::string::npos);
    // ② 槽写本身也被点名（它命中的就是这个段；块 EA 来自 `cpu->eip`）
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x1005AB58 n=4 block=lift_cc_004A5115") !=
               std::string::npos);
    // ③ 下一次写的最开头补读 P（store 已完成）⇒ ptr= 与 arm 区间
    LIFT_CHECK(r.stderr_text.find("watch      : follow 0x1005AB58 → ptr=0x1005C008") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("; arm [0x1005C000,0x1005C010)") != std::string::npos);
    // ④ 用**更新后**的观察集判本次写 ⇒ 16 字节块头写被点名（这就是"谁在 P-8 写了 16 字节"）
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x1005C000 n=16 block=lift_cc_004A5125") !=
               std::string::npos);
    // ⑤ 反向：P-8 这次写**不是** follow 段（⛔ 不许把新 arm 的段又当 follow）
    LIFT_CHECK(r.stderr_text.find("follow pending 0x1005C000") == std::string::npos);
}

// ⑨ shim 侧写入（`wr32` 内联 helper）：命中打 `... shim`，不在观察集的写零输出。
void checkWatchShim(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 0);
    // 旧断言保：未设名字的命中行是它的前缀
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x00405000 n=4 shim") != std::string::npos);
    // 未设名字 ⇒ `?`（⛔ 不编造名字）/ 设了名字 ⇒ 名字 / 清掉 ⇒ 回到 `?`
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x00405000 n=4 shim=?") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x00405004 n=4 shim=test-shim") != std::string::npos);
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x00405008 n=4 shim=?") != std::string::npos);
    // 真实 `ShimRegistry::invoke`：名字来自 `def->name`（⛔ 不是测试自己设的）⇒ 钉住 shims.cpp 的接线
    LIFT_CHECK(r.stderr_text.find("watch      : WRITE 0x0040500C n=4 shim=registered-shim") !=
               std::string::npos);
    // 不在观察集的写零行（反向断言）
    LIFT_CHECK(r.stderr_text.find("WRITE 0x00401000") == std::string::npos);
}

// ⑩ 缺省档（不设 `LIFT_WATCH`）⇒ 一条 `watch      :` 都不打（反向断言：find == npos）。
void checkWatchOff(const ChildRun& r) {
    LIFT_CHECK(r.exited);
    LIFT_CHECK_EQ_I64(r.exit_code, 0);
    LIFT_CHECK(r.stderr_text.find("watch      :") == std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    const char* const mode = (argc > 1 && argv[1] != nullptr) ? argv[1] : "";

    lift::GuestMemory mem;
    g_mem = &mem;
    if (!mem.reserved()) {
        std::fprintf(stderr, "GuestMemory 预留失败：%s\n", mem.lastError());
        return 1;
    }
    if (!mem.commit(kCommitted, lift::GuestMemory::kPage, lift::RegionKind::Heap, "fault-report")) {
        std::fprintf(stderr, "提交 guest 页失败：%s\n", mem.lastError());
        return 1;
    }

    // ★ 子进程模式：真正触发 fault 的那一步必须活在**另一个进程**里（诊断会 `_Exit`）。
    if (std::strcmp(mode, "--child-host-av") == 0) childFaultHost();
    if (std::strcmp(mode, "--child-guest-fault") == 0) childFaultGuest();
    if (std::strcmp(mode, "--child-aot-fault") == 0) childFaultAot();
    if (std::strcmp(mode, "--child-platform-fault") == 0) childFaultPlatform();
    if (std::strcmp(mode, "--child-fault-dump") == 0) childFaultDump();
    if (std::strcmp(mode, "--child-watch-follow") == 0) childWatchFollow();
    if (std::strcmp(mode, "--child-watch-shim") == 0) childWatchShim();
    if (std::strcmp(mode, "--child-watch-off") == 0) childWatchOff();

    // ① 纯函数（两平台都跑）
    g_current_case = "in-guest-space-pure-function";
    const void* const guest_base = mem.host(0);
    const std::uintptr_t host_base = reinterpret_cast<std::uintptr_t>(guest_base);
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base));                                   // 空间起点
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base + 0x00401000u));                     // 中段
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base + lift::GuestMemory::kSpace - 1));   // 最后一个字节
    LIFT_CHECK(!lift::inGuestSpace(guest_base, host_base + lift::GuestMemory::kSpace));      // 右开区间
    LIFT_CHECK(!lift::inGuestSpace(guest_base, 0x1u));                                        // 宿主低地址 ⇒ host
    LIFT_CHECK(!lift::inGuestSpace(nullptr, host_base));                                      // 未预留 ⇒ 不归因 guest

    // ①' `LIFT_FAULT_DUMP` 的解析器（进程内；两个平台同一份判据）
    g_current_case = "fault-dump-spec-parser";
    checkFaultDumpParser();

    // ①'' `LIFT_WATCH` 的解析器（进程内；两个平台同一份判据）
    g_current_case = "fault-watch-spec-parser";
    checkFaultWatchParser();

#if defined(_WIN32)
    // ④ 宿主页大小以系统为准（⛔ 不许硬编码 0x1000）。
    //   ★ 本机 dwPageSize 恰好也是 0x1000 ⇒ "改回常量 0x1000"在这台机器上**测不出来**
    //     （这正是本单的已知边界）；这条守卫抓的是页面更大的宿主（16 KiB / 64 KiB）与用错来源
    //     （例如误用 dwAllocationGranularity = 64 KiB）。
    g_current_case = "host-page-size-from-system";
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    LIFT_CHECK_EQ_U32(lift::hostPageSize(), static_cast<std::uint32_t>(si.dwPageSize));

    // ② 宿主 AV：先弄脏 g_access，再解引用 0x1 ⇒ 必须是 host fault（不许报成 guest 访存）
    g_current_case = "host-av-attributed-to-host";
    const ChildRun host = runChild(L"host-av");
    LIFT_CHECK(host.exited);
    LIFT_CHECK_EQ_I64(host.exit_code, 4);  // 宿主 fault 的退出码（≠ guest 的 3）
    LIFT_CHECK(host.stderr_text.find("=== host fault") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("0x1") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("=== guest memory fault") == std::string::npos);
    LIFT_CHECK(host.stderr_text.find("access     :") == std::string::npos);  // ★ 陈旧 g_access 不许冒充本次 fault
    // 附注：那条"最近 guest 访存"确实被弄脏过（否则本用例证明不了"陈旧值不会冒充"）
    LIFT_CHECK(host.stderr_text.find("0x00401000") != std::string::npos);

    // ③ 反向：guest fault 仍是 guest fault（EA / 宽度 / 方向 + 退出码 3 都不回退）
    g_current_case = "guest-fault-still-guest";
    const ChildRun guest = runChild(L"guest-fault");
    LIFT_CHECK(guest.exited);
    LIFT_CHECK_EQ_I64(guest.exit_code, 3);
    LIFT_CHECK(guest.stderr_text.find("=== guest memory fault") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("access     :") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("READ 0x20000000  size=4") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("=== host fault") == std::string::npos);

    // ④ AOT 臂的 guest fault 现场（A）+ fault 文本里的 `input-plan` 摘要（C）：
    //   解释器臂那条 `liftFaultHook` 在 AOT 上不成立 ⇒ 这一条是 AOT 唯一的现场面。
    g_current_case = "aot-fault-site-and-plan-summary";
    const ChildRun aot = runChild(L"aot-fault");
    checkAotFault(aot);

    // ⑤ 真平台 guest fault：`fault ea` 行（host − base）+ `access` 行的"不是本次 fault"后缀
    //   ★ 修前红：本条断言 `fault ea   : 0x20000000` 在实现缺失时**运行期**红（不是链接红）。
    g_current_case = "platform-guest-fault-ea";
    const ChildRun plat = runChild(L"platform-fault");
    checkPlatformFault(plat);

    // ⑥ fault 时的 opt-in dump（env → 解析 → 安装 → dump；6 段覆盖成功/失败/继续）
    g_current_case = "fault-dump-at-fault";
    const ChildRun dump = runChild(L"fault-dump");
    checkDumpAtFault(dump);

    // ⑦ 缺省档 / safe-mode 的"不多打"（反向断言：find == npos）
    g_current_case = "default-fault-text-unchanged";
    const ChildRun safe = runChild(L"guest-fault");
    const ChildRun plat_default = runChild(L"platform-fault");
    checkDefaultFaultText(safe, plat_default);

    // ⑧ `LIFT_WATCH` follow 的时序（走 `lift_note_write`：槽写 → 下一次写补读 P → arm → 点名本次写）
    g_current_case = "fault-watch-follow";
    const ChildRun watch_follow = runChild(L"watch-follow");
    checkWatchFollow(watch_follow);

    // ⑨ shim 侧写入（`wr32`）+ 不在观察集的写零输出
    g_current_case = "fault-watch-shim";
    const ChildRun watch_shim = runChild(L"watch-shim");
    checkWatchShim(watch_shim);

    // ⑩ 缺省档（不设 env）⇒ 一条 `watch      :` 都不打
    g_current_case = "fault-watch-off";
    const ChildRun watch_off = runChild(L"watch-off");
    checkWatchOff(watch_off);

    return finish("fault-report");
#else
    // ② 宿主 AV：先弄脏 g_access，再解引用 0x1 ⇒ 必须是 host fault（不许报成 guest 访存）
    g_current_case = "host-av-attributed-to-host";
    const ChildRun host = runChild(&childFaultHost);
    LIFT_CHECK(host.exited);
    LIFT_CHECK_EQ_I64(host.exit_code, 4);  // 宿主 fault 的退出码（≠ guest 的 3）
    LIFT_CHECK(host.stderr_text.find("=== host fault") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("0x1") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("=== guest memory fault") == std::string::npos);
    LIFT_CHECK(host.stderr_text.find("access     :") == std::string::npos);  // ★ 陈旧 g_access 不许冒充本次 fault
    // 附注：那条"最近 guest 访存"确实被弄脏过（否则本用例证明不了"陈旧值不会冒充"）
    LIFT_CHECK(host.stderr_text.find("0x00401000") != std::string::npos);

    // ③ 反向：guest fault 仍是 guest fault（EA / 宽度 / 方向 + 退出码 3 都不回退）
    g_current_case = "guest-fault-still-guest";
    const ChildRun guest = runChild(&childFaultGuest);
    LIFT_CHECK(guest.exited);
    LIFT_CHECK_EQ_I64(guest.exit_code, 3);
    LIFT_CHECK(guest.stderr_text.find("=== guest memory fault") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("access     :") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("READ 0x20000000  size=4") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("=== host fault") == std::string::npos);

    // ④ AOT 臂的 guest fault 现场（A）+ fault 文本里的 `input-plan` 摘要（C）
    g_current_case = "aot-fault-site-and-plan-summary";
    const ChildRun aot = runChild(&childFaultAot);
    checkAotFault(aot);

    // ⑤ 真平台 guest fault：`fault ea` 行（host − base）+ `access` 行的"不是本次 fault"后缀
    g_current_case = "platform-guest-fault-ea";
    const ChildRun plat = runChild(&childFaultPlatform);
    checkPlatformFault(plat);

    // ⑥ fault 时的 opt-in dump（env → 解析 → 安装 → dump；6 段覆盖成功/失败/继续）
    g_current_case = "fault-dump-at-fault";
    const ChildRun dump = runChild(&childFaultDump);
    checkDumpAtFault(dump);

    // ⑦ 缺省档 / safe-mode 的"不多打"（反向断言：find == npos）
    g_current_case = "default-fault-text-unchanged";
    const ChildRun safe = runChild(&childFaultGuest);
    const ChildRun plat_default = runChild(&childFaultPlatform);
    checkDefaultFaultText(safe, plat_default);

    // ⑧ `LIFT_WATCH` follow 的时序（走 `lift_note_write`：槽写 → 下一次写补读 P → arm → 点名本次写）
    g_current_case = "fault-watch-follow";
    const ChildRun watch_follow = runChild(&childWatchFollow);
    checkWatchFollow(watch_follow);

    // ⑨ shim 侧写入（`wr32`）+ 不在观察集的写零输出
    g_current_case = "fault-watch-shim";
    const ChildRun watch_shim = runChild(&childWatchShim);
    checkWatchShim(watch_shim);

    // ⑩ 缺省档（不设 env）⇒ 一条 `watch      :` 都不打
    g_current_case = "fault-watch-off";
    const ChildRun watch_off = runChild(&childWatchOff);
    checkWatchOff(watch_off);

    return finish("fault-report");
#endif
}
