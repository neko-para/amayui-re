// emulator-lift · Shape A 原型阶段 A：`sub_436D00` 的 `.ll` 与 runtime 链进**同一个** exe
//
// 判据（红了意味着什么）：
//   ① **链路**：`tests/ir/lift_436D00.ll` 由 clang 编成 obj、和 `emulator-lift-runtime` 链进本 exe；
//      trampoline 按 **resume EA** 查表调它。冷路径（[T+0x800]==0）走两次 `call`
//      （两个建表器 0x436BD0 / 0x436C60 **故意留在解释器**）⇒ 混合模式；热路径（flag=1）一次都不回解释器。
//   ② **逐位差分**：同一份真字节，解释器臂 vs lifted 臂，比 8 GPR + `EFLAGS & kDiffFlags`
//      + 三片内存哈希（`[T,T+0x804)` / `[buf,buf+n)` / `[esp-0x100,esp)`）+ 出口 EIP + ESP。
//      n ∈ {0,1,3,31,4096} × 随机 buffer（含 0x00 / 0xFF、地址非对齐），冷/热各 120 组 = 240 组。
//   ③ **外部锚**：n=9、buf="123456789" ⇒ EAX == 0xCBF43926（CRC-32/ISO-HDLC，反射变体）。
//      两臂都要过；先钉解释器臂（否则"两臂同样错"会静默通过）。
//   ④ **没早退**：解释器臂 `cpu.insns` 增量 == 闭式（`n>0 ⇒ 8n+19`、`n==0 ⇒ 14`）**加上**
//      冷路径独有那 3 条（两条 `call` + 置标志）**加上**两个建表器的动态条数
//      （由解释器在同一份真字节上实测一次 = `builders_dynamic`，⚠ 不是静态的 56+55）；
//      lifted 臂的解释器增量必须正好等于建表器那一段（热路径 = 0）；
//      dispatch 计数是常数（冷 5 次解析 + 1 次终止检查；热 1 次解析 + 1 次终止检查），
//      且冷路径真的进过 0x436BD0 / 0x436C60 / 0x436D19。
//   ⑤ **负控**：`--lifted-only` 子进程在冷路径上必须 `_Exit(9)` + stderr 打出非 lifted EA（0x436BD0）；
//      热路径在 lifted-only 下必须照常绿（证明"lifted 能自己跑完"不是靠早退装出来的）。
//
// ★ 载荷（**纯 hermetic**）：370 B 真字节，不加载 PE / 不碰 LFS / 不碰游戏安装 ⇒ `@env pure`。
//   来源：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` 的 PE 原始字节（**不是手编**）：
//     pnpm tools disasm-at at --ea 0x436BD0 --lines 130 --bytes 144
//     pnpm tools disasm-at at --ea 0x436C60 --lines  20 --bytes  50
//     pnpm tools disasm-at at --ea 0x436C92 --lines  70 --bytes 116
//     pnpm tools disasm-at at --ea 0x436D00 --lines  45 --bytes  80
//   并逐条对过 `.lst` 的指令窗口（0x436D00 push ebp … 0x436D4D retn 8；两个建表器的 8/7 个位步）。
// ★ flags 比较集合（`kDiffFlags` = 0x8D5 = CF|PF|AF|ZF|SF|OF，**只此一处**）：
//   出口的 CF 由 `or eax,-1`（n<=0 路径）或循环里的 `xor eax,[esi+edi*4+400h]` 清 0
//   （两条路径上它都在最后一次 `dec edx` **之前**），其余 5 位来自最后一次 `dec edx`
//   （`dec` 不动 CF）。`not` / `pop` / `retn` 都不写 flags ⇒ 6 位在出口**都有定义**。
//   ⚠ 任务书 §2.4 写"出口 CF 来自 `shr`"：`shr` 的 CF 会被紧随其后的 `xor` 覆盖，
//     最终 CF 的来源是那个 `xor`（n<=0 路径是 `or`）。结论（6 位都有定义）不变，掩码不变。
// ★ 防挂死：lifted 臂没有 `max_insns` ⇒ 本文件自己给预算（dispatch ≤ 1e6、解释器总条数 ≤ 1e8、
//   单组墙钟 ≤ 5 s）；超了**测试红**并打诊断（⛔ 不是 CI 超时）。
// ★ 读数（每次边界 ns / 端到端倍数 / dispatch 计数 / 两臂墙钟）只打印、**不进断言**。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "cmdline.h"
#include "fixture.h"
#include "runtime/lift_abi.h"
#include "runtime/machine.h"
#include "runtime/x86.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// `.ll` 导出的唯一入口（缺 clang 时由 `tests/ir/lift_stub.cpp` 提供同名替身 ⇒ 链接永不断）
extern "C" std::uint32_t lift_cc_436D00(::LiftRegs*, std::uint8_t*, std::uint32_t, void*);

namespace {

// =====================================================================================
// 真字节（370 B）
// =====================================================================================
// 0x436BD0..0x436C5F（144 B）· 0x436C60..0x436CF1（146 B）· 0x436D00..0x436D4F（80 B）
constexpr std::uint8_t kBytes[] = {
    // ---- 0x436BD0：建 +0x000 表（非反射，多项式 4C11DB7h），56 条 / 144 B ----
    0x33, 0xD2, 0x8B, 0xC2, 0xC1, 0xE0, 0x18, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D,
    0xC1, 0x04, 0xEB, 0x02, 0x03, 0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1,
    0x04, 0xEB, 0x02, 0x03, 0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04,
    0xEB, 0x02, 0x03, 0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04, 0xEB,
    0x02, 0x03, 0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04, 0xEB, 0x02,
    0x03, 0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04, 0xEB, 0x02, 0x03,
    0xC0, 0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04, 0xEB, 0x02, 0x03, 0xC0,
    0x85, 0xC0, 0x79, 0x09, 0x03, 0xC0, 0x35, 0xB7, 0x1D, 0xC1, 0x04, 0xEB, 0x02, 0x03, 0xC0, 0x89,
    0x04, 0x91, 0x42, 0x81, 0xFA, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0x86, 0x73, 0xFF, 0xFF, 0xFF, 0xC3,
    // ---- 0x436C60：建 +0x400 表（反射，多项式 0EDB88320h），55 条 / 146 B ----
    0x33, 0xD2, 0x81, 0xC1, 0x00, 0x04, 0x00, 0x00, 0x8B, 0xC2, 0xD1, 0xE8, 0xF6, 0xC2, 0x01, 0x74,
    0x05, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8,
    0xED, 0xEB, 0x02, 0xD1, 0xE8, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED,
    0xEB, 0x02, 0xD1, 0xE8, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xEB,
    0x02, 0xD1, 0xE8, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xEB, 0x02,
    0xD1, 0xE8, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xEB, 0x02, 0xD1,
    0xE8, 0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xEB, 0x02, 0xD1, 0xE8,
    0xA8, 0x01, 0x74, 0x09, 0xD1, 0xE8, 0x35, 0x20, 0x83, 0xB8, 0xED, 0xEB, 0x02, 0xD1, 0xE8, 0x89,
    0x01, 0x42, 0x83, 0xC1, 0x04, 0x81, 0xFA, 0xFF, 0x00, 0x00, 0x00, 0x0F, 0x86, 0x77, 0xFF, 0xFF,
    0xFF, 0xC3,
    // ---- 0x436D00：根（反射变体，30 条 / 80 B）----
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x00, 0x08, 0x00, 0x00, 0x00, 0x75, 0x14, 0xE8,
    0xBC, 0xFE, 0xFF, 0xFF, 0xE8, 0x47, 0xFF, 0xFF, 0xFF, 0xC7, 0x86, 0x00, 0x08, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x8B, 0x55, 0x08, 0x83, 0xC8, 0xFF, 0x4A, 0x78, 0x1D, 0x8B, 0x4D, 0x0C, 0x53,
    0x57, 0x0F, 0xB6, 0x39, 0x0F, 0xB6, 0xD8, 0x33, 0xFB, 0xC1, 0xE8, 0x08, 0x33, 0x84, 0xBE, 0x00,
    0x04, 0x00, 0x00, 0x41, 0x4A, 0x79, 0xEA, 0x5F, 0x5B, 0xF7, 0xD0, 0x5E, 0x5D, 0xC2, 0x08, 0x00,
};

constexpr std::uint32_t kEaBD0 = 0x00436BD0u;
constexpr std::uint32_t kEaC60 = 0x00436C60u;
constexpr std::uint32_t kEaD00 = 0x00436D00u;
constexpr std::uint32_t kEaD14 = 0x00436D14u;
constexpr std::uint32_t kEaD19 = 0x00436D19u;
constexpr std::size_t kLenBD0 = 0x90u;
constexpr std::size_t kLenC60 = 0x92u;
constexpr std::size_t kLenD00 = 0x50u;
constexpr std::size_t kOffBD0 = 0;
constexpr std::size_t kOffC60 = kOffBD0 + kLenBD0;
constexpr std::size_t kOffD00 = kOffC60 + kLenC60;
static_assert(sizeof(kBytes) == kLenBD0 + kLenC60 + kLenD00, "370 B：三段真字节的总长");

// =====================================================================================
// 夹具常量
// =====================================================================================
constexpr std::uint32_t kSelf = 0x00100000u;      // 表对象 T（两张 256×dword 表 + 标志）
constexpr std::uint32_t kSelfBytes = 0x804u;      // [T, T+0x800) 两张表 + [T+0x800] 标志
constexpr std::uint32_t kFlagOffset = 0x800u;
constexpr std::uint32_t kBuf = 0x00110001u;       // ★ 故意非对齐
constexpr std::uint32_t kBufPage = 0x00110000u;
constexpr std::uint32_t kBufCommit = 0x2000u;
constexpr std::uint32_t kBuilderStub = 0x00402000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;

// ★ flags 比较集合的**唯一**一处定义（两侧共用）。见文件头：出口 6 位都有定义。
constexpr std::uint32_t kDiffFlags = 0x8D5u;  // CF|PF|AF|ZF|SF|OF

constexpr std::uint64_t kDispatchBudget = 1000000u;      // 单组 dispatch 上限
constexpr std::uint64_t kInterpBudget = 100000000u;      // 单组解释器条数上限
constexpr double kGroupWallLimitSec = 5.0;               // 单组墙钟上限
constexpr std::uint64_t kBuildersStatic = 56u + 55u;  // `.lst` 的静态条数（只作读数对照，⛔ 不是闭式）

using Clock = std::chrono::steady_clock;

std::uint64_t fnv1a(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::vector<std::uint8_t> bytesAt(lift_fixture::Fixture& fx, std::uint32_t ea, std::size_t n) {
    const std::uint8_t* p = fx.m.mem().host(ea);
    return std::vector<std::uint8_t>(p, p + n);
}

// =====================================================================================
// 一臂的读数 / 快照
// =====================================================================================
struct Stats {
    std::uint64_t dispatches = 0;       // 非哨兵 EA 的解析次数（lifted 调用 + 解释器移交）
    // ★ 复审 P3-1：这里现在是**真的**在 `while` 条件被求值的位置 +1（见 `runLiftedArm`），
    //   ⛔ 不再是 `dispatches + 1` 的派生量。它的口径 = "循环入口被求值了几次"（含终止那一次），
    //   与 dispatches 是**同一件事的两种读法**（值必然差 1）—— 报告里不再把它当第二套独立证据。
    std::uint64_t boundary_checks = 0;
    std::uint64_t lifted_calls = 0;
    std::uint64_t interp_handoffs = 0;
    std::uint64_t lifted_hits_d00 = 0, lifted_hits_d14 = 0, lifted_hits_d19 = 0;
    std::uint64_t interp_hits_bd0 = 0, interp_hits_c60 = 0;
    bool saw_d19 = false;
    std::uint64_t insns = 0;       // 本臂 `Machine::run` 的指令增量（lifted 臂 = 解释器那一部分）
    std::uint64_t lifted_ns = 0;   // 只累计 `f(...)` 的墙钟（每次 trampoline 边界）
    std::uint64_t lifted_ns_ea[3] = {};   // 按入口分：0=0x436D00 1=0x436D14 2=0x436D19
    std::uint64_t lifted_cnt_ea[3] = {};
    std::uint64_t wall_ns = 0;     // 整臂墙钟
};

struct Snap {
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t eip = 0;
    std::uint32_t flag = 0;
    std::uint32_t entry_esp = 0;  // 用来显式钉 `retn 8`：出口 ESP == 入口 + 4 + 8
    std::vector<std::uint8_t> t, buf, stack;
    std::uint64_t h[3] = {};  // [T,T+0x804) / [buf,buf+n) / [esp-0x100,esp)
};

struct ArmResult {
    Snap snap;
    Stats stats;
    bool ok = false;
    std::string err;
};

void checkU32(const char* tag, const char* what, std::uint32_t lifted, std::uint32_t interp, int line) {
    ++lift_test::g_checks;
    if (lifted != interp)
        lift_test::failf(__FILE__, line, "[%s] %s: lifted=0x%08X interp=0x%08X", tag, what, lifted, interp);
}

void checkU64(const char* tag, const char* what, std::uint64_t lifted, std::uint64_t interp, int line) {
    ++lift_test::g_checks;
    if (lifted != interp)
        lift_test::failf(__FILE__, line, "[%s] %s: lifted=%llu interp=%llu", tag, what,
                         static_cast<unsigned long long>(lifted), static_cast<unsigned long long>(interp));
}

void checkRegion(const char* tag, const char* what, const std::vector<std::uint8_t>& lifted,
                 const std::vector<std::uint8_t>& interp, int line) {
    ++lift_test::g_checks;
    if (lifted == interp) return;
    std::size_t i = 0;
    while (i < lifted.size() && i < interp.size() && lifted[i] == interp[i]) ++i;
    lift_test::failf(__FILE__, line,
                     "[%s] region %s: first diff @+0x%zX lifted=0x%02X interp=0x%02X (sizes %zu/%zu)", tag, what,
                     i, i < lifted.size() ? lifted[i] : 0u, i < interp.size() ? interp[i] : 0u, lifted.size(),
                     interp.size());
}

// =====================================================================================
// 夹具：放真字节 + 对象 T + buffer + 入口帧
// =====================================================================================
bool placePayload(lift_fixture::Fixture& fx) {
    if (!fx.m.placeCode(kEaBD0, kBytes + kOffBD0, kLenBD0, &fx.err)) return false;
    if (!fx.m.placeCode(kEaC60, kBytes + kOffC60, kLenC60, &fx.err)) return false;
    if (!fx.m.placeCode(kEaD00, kBytes + kOffD00, kLenD00, &fx.err)) return false;
    return true;
}

// `hot=true` ⇒ 先用解释器跑一遍**guest 自己的**两个建表器再置标志（与 `0x436D0F/0x436D14/0x436D19`
// 同一份代码、同一个顺序）。★ 本函数**不**给 lifted 臂任何"预建表"的捷径：冷/热两条路径都是
// 两臂各自从同一份初始内存出发（复审 §4(d) 的 vacuity 就是靠"冷臂开局 flag==0"堵的）。
bool setupFixture(lift_fixture::Fixture& fx, std::uint32_t n, const std::vector<std::uint8_t>& buf, bool hot,
                  std::uint32_t* entry_esp) {
    if (!fx.ok) return false;
    if (!placePayload(fx)) return false;
    const std::vector<std::uint8_t> zero(kSelfBytes, 0);
    if (!fx.m.placeData(kSelf, zero.data(), zero.size(), &fx.err)) return false;
    const std::vector<std::uint8_t> buf_commit(kBufCommit, 0);
    if (!fx.m.placeData(kBufPage, buf_commit.data(), buf_commit.size(), &fx.err)) return false;
    if (n != 0) fx.m.mem().writeBytes(kBuf, buf.data(), n);
    if (hot) {
        lift_fixture::Asm b(kBuilderStub);
        b.u8(0xB9);  // mov ecx, T
        b.u32(kSelf);
        b.callRel(kEaBD0);
        b.u8(0xB9);
        b.u32(kSelf);
        b.callRel(kEaC60);
        b.ret();
        if (!fx.m.placeCode(kBuilderStub, b.b.data(), b.b.size(), &fx.err)) return false;
        fx.rearmStack();
        fx.runCode(kBuilderStub);
        if (!fx.ok) return false;
        fx.m.mem().wr32(kSelf + kFlagOffset, 1u);
    }
    // 入口帧：esp = E，[E] = 哨兵（retn 8 弹的返回地址），[E+4] = n，[E+8] = buf
    const std::uint32_t e = fx.m.layout().stack_top - 0x100u;
    fx.m.cpu().esp() = e;
    fx.m.mem().wr32(e, kSentinel);
    fx.m.mem().wr32(e + 4u, n);
    fx.m.mem().wr32(e + 8u, kBuf);
    *entry_esp = e;
    return true;
}

void setEntryState(lift_fixture::Fixture& fx, std::uint32_t entry_esp) {
    lift::X86& cpu = fx.m.cpu();
    cpu.eax() = 0;
    cpu.ecx() = kSelf;  // this（thiscall）
    cpu.edx() = 0;
    cpu.ebx() = 0x11111111u;
    cpu.esp() = entry_esp;
    cpu.ebp() = 0x22222222u;
    cpu.esi() = 0x33333333u;
    cpu.edi() = 0x44444444u;
    cpu.eflags = 0x202u;
    cpu.eip = kEaD00;
}

Snap takeSnap(lift_fixture::Fixture& fx, std::uint32_t entry_esp, std::uint32_t n) {
    Snap s;
    for (int i = 0; i < 8; ++i) s.r[i] = fx.m.cpu().r[i];
    s.eflags = fx.m.cpu().eflags;
    s.eip = fx.m.cpu().eip;
    s.flag = fx.m.mem().rd32(kSelf + kFlagOffset);
    s.entry_esp = entry_esp;
    s.t = bytesAt(fx, kSelf, kSelfBytes);
    s.buf = bytesAt(fx, kBuf, n);
    s.stack = bytesAt(fx, entry_esp - 0x100u, 0x100u);
    s.h[0] = fnv1a(s.t.data(), s.t.size());
    s.h[1] = fnv1a(s.buf.data(), s.buf.size());
    s.h[2] = fnv1a(s.stack.data(), s.stack.size());
    return s;
}

// 解释器臂：从函数入口跑到合成返回哨兵（`until_ea = 0xFFFFFFFF`）
bool runInterpArm(lift_fixture::Fixture& fx, Stats* st, std::string* err) {
    const std::uint64_t before = fx.m.cpu().insns;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = before + kInterpBudget;
    const lift::RunResult r = fx.m.run(o);
    st->insns = fx.m.cpu().insns - before;
    if (r.reason != lift::StopReason::UntilEa) {
        char b[160];
        std::snprintf(b, sizeof(b), "解释器臂意外停机：%s — %s", lift::stopReasonName(r.reason), r.detail.c_str());
        *err = b;
        return false;
    }
    return true;
}

[[noreturn]] void hardFailLiftedOnly(std::uint32_t ea) {
    std::fprintf(stderr,
                 "lifted-only: EA 0x%08X 不在 lifted 表里 —— 解释器会回到执行路径上（负控必须红）\n", ea);
    std::fflush(stderr);
    std::_Exit(9);
}

lift::LiftFn lookupLifted(std::uint32_t ea) {
    switch (ea) {
        case kEaD00:
        case kEaD14:
        case kEaD19: return &lift_cc_436D00;  // 一个函数体，按 %ea 分派到 3 个入口块
        default: return nullptr;
    }
}

// lifted 臂：trampoline（本单的"混合模式"就是这里的 else 分支）
bool runLiftedArm(lift_fixture::Fixture& fx, std::uint32_t start_ea, bool lifted_only, Stats* st,
                  std::string* err) {
    LiftRegs regs;
    lift::liftCpuToRegs(fx.m.cpu(), regs);
    std::uint8_t* mem = fx.m.mem().host(0);
    std::uint32_t ea = start_ea;
    const std::uint64_t insns0 = fx.m.cpu().insns;
    while (true) {
        // ★ P3-1 修法：计数发生在**条件真正被求值**的地方（终止哨兵那一轮也计），
        //   ⛔ 不是循环体里 +1 再在出口用 `dispatches + 1` 覆盖。
        ++st->boundary_checks;
        if (ea == kSentinel) break;
        if (st->dispatches >= kDispatchBudget) {
            *err = "dispatch 预算耗尽（lifted 侧可能死循环）";
            return false;
        }
        ++st->dispatches;
        lift::LiftFn f = lookupLifted(ea);
        if (f) {
            ++st->lifted_calls;
            st->lifted_hits_d00 += (ea == kEaD00) ? 1u : 0u;
            st->lifted_hits_d14 += (ea == kEaD14) ? 1u : 0u;
            st->lifted_hits_d19 += (ea == kEaD19) ? 1u : 0u;
            if (ea == kEaD19) st->saw_d19 = true;
            const auto t0 = Clock::now();
            const std::uint32_t entry_ea = ea;
            ea = f(&regs, mem, ea, &fx.m.cpu());
            const std::uint64_t dt = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
            st->lifted_ns += dt;
            const int slot = (entry_ea == kEaD00) ? 0 : (entry_ea == kEaD14) ? 1 : 2;
            st->lifted_ns_ea[slot] += dt;
            st->lifted_cnt_ea[slot] += 1u;
        } else {
            ++st->interp_handoffs;
            st->interp_hits_bd0 += (ea == kEaBD0) ? 1u : 0u;
            st->interp_hits_c60 += (ea == kEaC60) ? 1u : 0u;
            if (lifted_only) hardFailLiftedOnly(ea);
            // 混合模式：解释器从 ea 跑到 [esp] 那个返回地址
            lift::liftRegsToCpu(regs, fx.m.cpu());
            fx.m.cpu().eip = ea;
            lift::RunOptions o;
            o.until_ea = fx.m.mem().rd32(regs.r[4]);
            o.max_insns = fx.m.cpu().insns + kInterpBudget;
            const lift::RunResult r = fx.m.run(o);
            if (r.reason != lift::StopReason::UntilEa) {
                char b[192];
                std::snprintf(b, sizeof(b), "混合模式解释器在 0x%08X 意外停机：%s — %s", ea,
                              lift::stopReasonName(r.reason), r.detail.c_str());
                *err = b;
                return false;
            }
            lift::liftCpuToRegs(fx.m.cpu(), regs);
            ea = fx.m.cpu().eip;
        }
    }
    st->insns = fx.m.cpu().insns - insns0;
    lift::liftRegsToCpu(regs, fx.m.cpu());
    fx.m.cpu().eip = kSentinel;
    return true;
}

ArmResult runArm(bool lifted, std::uint32_t n, const std::vector<std::uint8_t>& buf, bool hot) {
    ArmResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) {
        out.err = fx.err;
        return out;
    }
    std::uint32_t e = 0;
    if (!setupFixture(fx, n, buf, hot, &e)) {
        out.err = fx.err.empty() ? "setup 失败" : fx.err;
        return out;
    }
    // 进入时断言（复审 §4(d)）：冷路径必须真的从"表还没建"开局
    const std::uint32_t flag_in = fx.m.mem().rd32(kSelf + kFlagOffset);
    ++lift_test::g_checks;
    if (flag_in != (hot ? 1u : 0u))
        lift_test::failf(__FILE__, __LINE__, "[%s] 入口 [T+0x800]=%u，期望 %u（冷/热路径的初始条件）",
                         lift_test::g_current_case, flag_in, hot ? 1u : 0u);
    setEntryState(fx, e);
    const auto t0 = Clock::now();
    const bool ran = lifted ? runLiftedArm(fx, kEaD00, false, &out.stats, &out.err) : runInterpArm(fx, &out.stats, &out.err);
    const auto t1 = Clock::now();
    out.stats.wall_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (out.stats.wall_ns > static_cast<std::uint64_t>(kGroupWallLimitSec * 1e9)) {
        char b[128];
        std::snprintf(b, sizeof(b), "单组墙钟超 %.1f s（%.3f s）", kGroupWallLimitSec,
                      static_cast<double>(out.stats.wall_ns) / 1e9);
        out.err = b;
        return out;
    }
    out.snap = takeSnap(fx, e, n);
    out.ok = ran;
    return out;
}

// 两臂逐位比较；返回 true = 全同
bool compareArms(const ArmResult& interp, const ArmResult& lifted, const char* tag) {
    static const char* kNames[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    bool same = true;
    const int before_failures = lift_test::g_failures;
    for (int i = 0; i < 8; ++i) {
        checkU32(tag, kNames[i], lifted.snap.r[i], interp.snap.r[i], __LINE__);
    }
    checkU32(tag, "EFLAGS&kDiffFlags", lifted.snap.eflags & kDiffFlags, interp.snap.eflags & kDiffFlags, __LINE__);
    checkU32(tag, "EIP", lifted.snap.eip, interp.snap.eip, __LINE__);
    checkU32(tag, "flag[T+0x800]", lifted.snap.flag, interp.snap.flag, __LINE__);
    checkU64(tag, "hash[T,T+0x804)", lifted.snap.h[0], interp.snap.h[0], __LINE__);
    checkU64(tag, "hash[buf,buf+n)", lifted.snap.h[1], interp.snap.h[1], __LINE__);
    checkU64(tag, "hash[esp-0x100,esp)", lifted.snap.h[2], interp.snap.h[2], __LINE__);
    checkRegion(tag, "[T,T+0x804)", lifted.snap.t, interp.snap.t, __LINE__);
    checkRegion(tag, "[buf,buf+n)", lifted.snap.buf, interp.snap.buf, __LINE__);
    checkRegion(tag, "[esp-0x100,esp)", lifted.snap.stack, interp.snap.stack, __LINE__);
    // `retn 8` 的显式口径：出口 ESP == 入口 ESP + 4（弹返回地址）+ 8（imm16）= +12。
    // ★ 任务书写"入口 esp - 4 - 8"：x86 的 retn 是**加**（栈向上弹），符号按 SDM 为准。
    //   这条只钉"弹了 4 又加了 8"；栈区哈希与 ESP 的两臂比较同时也在守同一件事。
    checkU32(tag, "ESP==entry+4+8(lifted)", lifted.snap.r[4], lifted.snap.entry_esp + 12u, __LINE__);
    checkU32(tag, "ESP==entry+4+8(interp)", interp.snap.r[4], interp.snap.entry_esp + 12u, __LINE__);
    same = (lift_test::g_failures == before_failures);
    return same;
}

std::uint32_t xorShift(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

std::vector<std::uint8_t> makeBuf(std::uint32_t n, int rep, bool hot) {
    std::vector<std::uint8_t> buf(n);
    std::uint32_t seed = 0x9E3779B9u ^ static_cast<std::uint32_t>(rep * 2654435761u) ^ (hot ? 0x51ED270Bu : 0u);
    seed ^= n * 2246822519u + 1u;
    if (seed == 0) seed = 0xA5A5A5A5u;
    for (std::uint32_t i = 0; i < n; ++i) buf[i] = static_cast<std::uint8_t>(xorShift(seed) >> 24);
    // 显式覆盖两端：0x00 / 0xFF 必须真的出现过（n==1 时按 rep 交替）
    if (n >= 1) buf[0] = (n == 1 && (rep & 1) != 0) ? 0xFF : 0x00;
    if (n >= 2) buf[n - 1] = 0xFF;
    if (n >= 3) buf[n / 2] = 0x00;
    return buf;
}

// 一个建表器在解释器里跑一遍的**动态**条数（从它的入口跑到 retn 的哨兵）。
// ⛔ 不是静态条数：`.lst` 的 56/55 是**静态**的，动态是 256 次外层循环 × 每个位步 3~5 条。
std::uint64_t measureOneBuilderInsns(std::uint32_t ea) {
    lift_fixture::Fixture fx;
    if (!fx.ok || !placePayload(fx)) return 0;
    const std::vector<std::uint8_t> zero(kSelfBytes, 0);
    if (!fx.m.placeData(kSelf, zero.data(), zero.size(), &fx.err)) return 0;
    fx.rearmStack();
    fx.m.cpu().ecx() = kSelf;
    fx.m.cpu().eip = ea;
    const std::uint64_t before = fx.m.cpu().insns;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = before + kInterpBudget;
    const lift::RunResult r = fx.m.run(o);
    if (r.reason != lift::StopReason::UntilEa) return 0;
    return fx.m.cpu().insns - before;
}

// 两个建表器的动态条数之和。它只用来做**加法口径**：根函数的闭式 `8n+19` / `14`
// 是解析给出来的；冷路径在它之上再加 ① 只在冷路径执行的那 3 条（两条 call + 置标志）
// 与 ② 两个建表器的动态条数。lifted 臂的解释器增量必须**正好**只有 ②。
std::uint64_t measureBuildersInsns() {
    const std::uint64_t bd0 = measureOneBuilderInsns(kEaBD0);
    const std::uint64_t c60 = measureOneBuilderInsns(kEaC60);
    return bd0 + c60;
}

// =====================================================================================
// 子进程（负控）：只在本进程之外验 `_Exit(9)`
// =====================================================================================
#if defined(_WIN32)
struct ChildRun {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
    bool timed_out = false;
    std::string out;
    std::string err;
};

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

ChildRun spawnSelf(const std::filesystem::path& exe, const std::wstring& args, const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    ChildRun r;
    const fs::path o = dir / "child.out";
    const fs::path e = dir / "child.err";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h_out = ::CreateFileW(o.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE h_err = ::CreateFileW(e.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_out == INVALID_HANDLE_VALUE || h_err == INVALID_HANDLE_VALUE) {
        if (h_out != INVALID_HANDLE_VALUE) ::CloseHandle(h_out);
        if (h_err != INVALID_HANDLE_VALUE) ::CloseHandle(h_err);
        return r;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out;
    si.hStdError = h_err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = lift_test::quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     dir.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000u);
    if (waited != WAIT_OBJECT_0) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
    }
    ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    r.out = readAll(o);
    r.err = readAll(e);
    return r;
}
#endif  // _WIN32

// =====================================================================================
// 负控子进程模式：冷路径 + lifted-only ⇒ 必须 `_Exit(9)`
// =====================================================================================
int runLiftedOnlyChild() {
    lift_fixture::Fixture fx;
    if (!fx.ok) {
        std::fprintf(stderr, "lifted-only child: fixture 失败：%s\n", fx.err.c_str());
        return 1;
    }
    const std::vector<std::uint8_t> buf = makeBuf(31u, 0, false);
    std::uint32_t e = 0;
    if (!setupFixture(fx, 31u, buf, false, &e)) {
        std::fprintf(stderr, "lifted-only child: setup 失败：%s\n", fx.err.c_str());
        return 1;
    }
    setEntryState(fx, e);
    Stats st;
    std::string err;
    if (!runLiftedArm(fx, kEaD00, true, &st, &err)) {
        std::fprintf(stderr, "lifted-only child: %s\n", err.c_str());
        return 1;
    }
    // 走到这里说明"要回解释器时没有响亮失败"—— 那正是负控要抓的静默
    std::fprintf(stderr, "FAIL: lifted-only 下冷路径居然跑完了（没有回落解释器？）\n");
    return 1;
}

// =====================================================================================
// main
// =====================================================================================
int runMain(const std::filesystem::path& self_exe) {
    using namespace lift_test;

    // ---- 0) 版本 / 环境（读数，不进断言）------------------------------------------------
    const char* build =
#ifdef LIFT_IR_BUILD_TYPE
        LIFT_IR_BUILD_TYPE;
#else
        "(unknown)";
#endif
    std::printf("[lift-ir-proto] build=%s", build);
#ifdef _WIN32
    if (const char* cpu = std::getenv("PROCESSOR_IDENTIFIER")) std::printf(" CPU=%s", cpu);
#endif
    std::printf("\n");

    // ---- 1) 差分：冷/热 × n ∈ {0,1,3,31,4096} × 24 个随机 buffer = 240 组 -------------
    // 两个建表器的动态条数（冷路径上解释器要真的跑完它们；见 measureBuildersInsns）
    const std::uint64_t builders_dynamic = measureBuildersInsns();
    LIFT_CHECK(builders_dynamic > 0u);
    std::printf("[lift-ir-proto] builders(dynamic insns)=%llu（静态 %llu；动态 = 256 次外层循环）\n",
                static_cast<unsigned long long>(builders_dynamic), static_cast<unsigned long long>(kBuildersStatic));
    const std::uint32_t kNList[] = {0u, 1u, 3u, 31u, 4096u};
    constexpr int kReps = 24;
    std::uint64_t groups = 0, mismatches = 0;
    std::uint64_t cold_groups = 0, hot_groups = 0;
    for (int path = 0; path < 2; ++path) {
        const bool hot = path == 1;
        for (int ni = 0; ni < 5; ++ni) {
            const std::uint32_t n = kNList[ni];
            for (int rep = 0; rep < kReps; ++rep) {
                const std::vector<std::uint8_t> buf = makeBuf(n, rep, hot);
                const std::string name = std::string(hot ? "hot" : "cold") + "-n" + std::to_string(n) + "-r" +
                                         std::to_string(rep);
                g_current_case = name.c_str();
                ArmResult in = runArm(false, n, buf, hot);
                ArmResult li = runArm(true, n, buf, hot);
                ++g_checks;
                if (!in.ok || !li.ok) {
                    failf(__FILE__, __LINE__, "[%s] 臂失败：interp=%s lifted=%s", name.c_str(),
                          in.ok ? "ok" : in.err.c_str(), li.ok ? "ok" : li.err.c_str());
                    continue;
                }
                ++groups;
                if (hot) ++hot_groups; else ++cold_groups;

                // ② 逐位差分
                if (!compareArms(in, li, name.c_str())) ++mismatches;

                // ④ 没早退：闭式 + dispatch 计数
                //   根函数的闭式是**解析**的（8n+19 / n==0 ⇒ 14）；冷路径在它之上再加
                //   ① 只在冷路径执行的 3 条（两条 call + `mov [esi+800h],1`）
                //   ② 两个建表器的**动态**条数（解释器实测，见 measureBuildersInsns）。
                //   lifted 臂的解释器增量必须**正好**只有 ②。
                const std::uint64_t closure = (n == 0) ? 14u : 8ull * n + 19u;
                const std::uint64_t cold_extra = hot ? 0u : 3u;
                const std::uint64_t builder = hot ? 0u : builders_dynamic;
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(in.stats.insns),
                                  static_cast<std::int64_t>(closure + cold_extra + builder));
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(li.stats.insns), static_cast<std::int64_t>(builder));
                if (hot) {
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.dispatches), 1u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.boundary_checks), 2u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.lifted_calls), 1u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_handoffs), 0u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_hits_bd0), 0u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_hits_c60), 0u);
                    LIFT_CHECK(!li.stats.saw_d19);
                } else {
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.dispatches), 5u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.boundary_checks), 6u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.lifted_calls), 3u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_handoffs), 2u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_hits_bd0), 1u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.interp_hits_c60), 1u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.lifted_hits_d19), 1u);
                    LIFT_CHECK(li.stats.saw_d19);
                }
            }
        }
    }
    std::printf("[lift-ir-proto] diff: groups=%llu mismatches=%llu (cold=%llu hot=%llu, kDiffFlags=0x%03X)\n",
                static_cast<unsigned long long>(groups), static_cast<unsigned long long>(mismatches),
                static_cast<unsigned long long>(cold_groups), static_cast<unsigned long long>(hot_groups),
                kDiffFlags);
    LIFT_CHECK(groups >= 200u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(mismatches), 0u);

    // ---- 2) 外部锚：n=9、buf="123456789" ⇒ 0xCBF43926 -----------------------------------
    {
        g_current_case = "oracle/crc32-iso-hdlc";
        const std::vector<std::uint8_t> text = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        ArmResult in = runArm(false, 9u, text, true);
        ArmResult li = runArm(true, 9u, text, true);
        LIFT_CHECK(in.ok && li.ok);
        if (in.ok && li.ok) {
            LIFT_CHECK_EQ_U32(in.snap.r[0], 0xCBF43926u);  // 先钉解释器臂
            LIFT_CHECK_EQ_U32(li.snap.r[0], 0xCBF43926u);
            compareArms(in, li, "oracle");
            std::printf("[lift-ir-proto] oracle: \"123456789\" ⇒ EAX=0x%08X/0x%08X（期望 0xCBF43926）\n",
                        in.snap.r[0], li.snap.r[0]);
        }
    }

    // ---- 3) 负控（in-process）：热路径在 lifted-only 下必须绿 ------------------------------
    {
        g_current_case = "negctl/hot-lifted-only";
        const std::vector<std::uint8_t> buf = makeBuf(31u, 7, true);
        ArmResult in = runArm(false, 31u, buf, true);
        lift_fixture::Fixture fx;
        std::uint32_t e = 0;
        std::string err;
        bool ok = false;
        if (in.ok && fx.ok && setupFixture(fx, 31u, buf, true, &e)) {
            setEntryState(fx, e);
            Stats st;
            ok = runLiftedArm(fx, kEaD00, true, &st, &err);
            LIFT_CHECK(ok);
            if (ok) {
                LIFT_CHECK_EQ_U32(fx.m.cpu().eax(), in.snap.r[0]);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(st.interp_handoffs), 0u);
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(st.dispatches), 1u);
            }
        } else {
            LIFT_CHECK(false);
        }
    }

    // ---- 4) 负控（子进程）：冷路径 + lifted-only ⇒ exit 9 + stderr 打出 0x436BD0 ----------
#if defined(_WIN32)
    {
        g_current_case = "negctl/cold-lifted-only-child";
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / ("lift-ir-proto-" + std::to_string(::GetCurrentProcessId()));
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        if (!ec) {
            const ChildRun child = spawnSelf(self_exe, L"--lifted-only", dir);
            LIFT_CHECK(child.started);
            LIFT_CHECK(!child.timed_out);
            LIFT_CHECK_EQ_U32(child.exit_code, 9u);
            LIFT_CHECK(child.err.find("436BD0") != std::string::npos);
            std::printf("[lift-ir-proto] negctl: child exit=%u stderr=%s", static_cast<unsigned>(child.exit_code),
                        child.err.empty() ? "(空)" : child.err.c_str());
            LIFT_CHECK(child.err.find("lifted-only") != std::string::npos);
        } else {
            LIFT_CHECK(false);
        }
        fs::remove_all(dir, ec);
    }
#else
    std::printf("[lift-ir-proto] negctl 子进程：非 Windows 无 CreateProcessW ⇒ 跳过（in-process 判据仍跑）\n");
#endif

    // ---- 5) 读数（不进断言）：每次 trampoline 边界 ns / 端到端倍数 / dispatch / 墙钟 ------
    {
        auto median = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        // 冷：每次都要重新起一台冷机器（第一遍就会把 flag 置 1）
        std::vector<double> cold_ns_ea[3];
        for (int rep = 0; rep < 3; ++rep) {
            const std::vector<std::uint8_t> buf = makeBuf(4096u, rep, false);
            ArmResult r = runArm(true, 4096u, buf, false);
            if (!r.ok) continue;
            for (int k = 0; k < 3; ++k) {
                if (r.stats.lifted_cnt_ea[k] != 0)
                    cold_ns_ea[k].push_back(static_cast<double>(r.stats.lifted_ns_ea[k]) /
                                            static_cast<double>(r.stats.lifted_cnt_ea[k]));
            }
        }
        // 热：同一台机器跑 3 遍（每遍重置入口帧/寄存器）
        std::vector<double> hot_ns_ea[3];
        std::vector<double> e2e_interp_ns, e2e_lifted_ns;
        {
            const std::vector<std::uint8_t> buf = makeBuf(4096u, 99, true);
            lift_fixture::Fixture fx;
            std::uint32_t e = 0;
            if (fx.ok && setupFixture(fx, 4096u, buf, true, &e)) {
                for (int rep = 0; rep < 3; ++rep) {
                    setEntryState(fx, e);
                    fx.m.mem().wr32(e, kSentinel);
                    fx.m.mem().wr32(e + 4u, 4096u);
                    fx.m.mem().wr32(e + 8u, kBuf);
                    Stats st;
                    std::string err;
                    const auto t0 = Clock::now();
                    if (runLiftedArm(fx, kEaD00, false, &st, &err)) {
                        const auto t1 = Clock::now();
                        e2e_lifted_ns.push_back(
                            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                        for (int k = 0; k < 3; ++k) {
                            if (st.lifted_cnt_ea[k] != 0)
                                hot_ns_ea[k].push_back(static_cast<double>(st.lifted_ns_ea[k]) /
                                                       static_cast<double>(st.lifted_cnt_ea[k]));
                        }
                    }
                }
            }
            lift_fixture::Fixture fx2;
            std::uint32_t e2 = 0;
            if (fx2.ok && setupFixture(fx2, 4096u, buf, true, &e2)) {
                for (int rep = 0; rep < 3; ++rep) {
                    setEntryState(fx2, e2);
                    fx2.m.mem().wr32(e2, kSentinel);
                    fx2.m.mem().wr32(e2 + 4u, 4096u);
                    fx2.m.mem().wr32(e2 + 8u, kBuf);
                    Stats st;
                    std::string err;
                    const auto t0 = Clock::now();
                    if (runInterpArm(fx2, &st, &err)) {
                        const auto t1 = Clock::now();
                        e2e_interp_ns.push_back(
                            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                    }
                }
            }
        }
        const double e2e_l = median(e2e_lifted_ns);
        const double e2e_i = median(e2e_interp_ns);
        const double ratio = (e2e_l > 0.0) ? e2e_i / e2e_l : 0.0;
        std::printf("[lift-ir-proto] ns/trampoline-boundary（3 遍中位，只算 f(...)；cold 按入口分开）:\n");
        std::printf("  cold: 0x436D00=%.1f 0x436D14=%.1f 0x436D19=%.1f\n", median(cold_ns_ea[0]),
                    median(cold_ns_ea[1]), median(cold_ns_ea[2]));
        std::printf("  hot : 0x436D00=%.1f（热路径只有这一个边界）\n", median(hot_ns_ea[0]));
        std::printf("[lift-ir-proto] e2e n=4096 hot: interp=%.0f ns lifted=%.0f ns ratio=%.2fx（3 遍中位）\n", e2e_i,
                    e2e_l, ratio);
        std::printf("[lift-ir-proto] wall n=4096 hot: interp=%.1f ms lifted=%.1f ms\n", e2e_i / 1e6, e2e_l / 1e6);
        std::printf("[lift-ir-proto] dispatch/group: cold=5(+1 终止检查) hot=1(+1 终止检查)；lifted 臂解释器条数："
                    "cold=%llu hot=0\n",
                    static_cast<unsigned long long>(builders_dynamic));
    }

    return lift_test::finish("lift-ir-proto");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    // 子进程模式：只做"冷路径 + lifted-only 必须 _Exit(9)"这一件事
    if (argc > 1 && std::strcmp(argv[1], "--lifted-only") == 0) return runLiftedOnlyChild();
#ifdef LIFT_IR_CLANG
    // 缺 clang 时 `.ll` 编不出来，本 exe 里链的是 `tests/ir/lift_stub.cpp`（返回哨兵）⇒ 运行期跳过。
    // ⚠ configure / 链接**不会**因此红：替身 TU 存在的唯一理由就是这条（复审 §3.4）。
    const std::filesystem::path self = (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path(argv[0]);
    return runMain(self);
#else
    (void)argc;
    (void)argv;
    LIFT_SKIP("缺 clang（未定义 LIFT_IR_CLANG）：`tests/ir/lift_436D00.ll` 未编，链的是替身 TU");
#endif
}
