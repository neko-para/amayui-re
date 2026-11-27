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
extern "C" std::uint32_t lift_cc_436D00(void*, std::uint8_t*, std::uint32_t, void*);
// ★ 寄存器计数移位（CL）的两条定点差分 case 的入口：只有**链接生成器产物**的两条 target
//   （`lift-ir-gen-*`，CMake 传 `LIFT_IR_CASE_GEN=1`）里才有这三个符号；手写 golden
//   （`tests/ir/lift_*.ll`）没有 ⇒ 声明与调用点都挂在 `LIFT_IR_CASE_GEN` 下。
//   缺 clang 时由 `tests/ir/lift_stub.cpp` 补替身（否则那两条 target 链接断）。
#if defined(LIFT_IR_CASE_GEN)
extern "C" std::uint32_t lift_cc_41880A(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4C6E64(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4E4B98(void*, std::uint8_t*, std::uint32_t, void*);
// ★ 2026-11-15 · S3-b：条件融合（`cmp`/`test` + `jcc`）的六条定点差分 case 的入口
//   （见 §2c）。同样只有链接生成器产物的两条 target 才有这些符号。
extern "C" std::uint32_t lift_cc_47A7F3(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_49EE49(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_404DEA(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4200F7(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4DD199(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4D19D3(void*, std::uint8_t*, std::uint32_t, void*);
// ★ 2026-11-15 · S3-c：`div`/`idiv` 与 flags 的两条定点差分 case 的入口（见 §2d）。
//   同样只有链接生成器产物的两条 target 才有这些符号。
extern "C" std::uint32_t lift_cc_48D500(void*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_419F0D(void*, std::uint8_t*, std::uint32_t, void*);
#endif

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
// ★ 寄存器计数移位（CL）的两条定点差分 case 的入口（见下面 §2b）：
//   0x41880A = `sub_4187B0` 的尾块；0x4C6E64 = `sub_4C6E30` 的位读取器尾段。
constexpr std::uint32_t kEaShiftA = 0x0041880Au;
constexpr std::uint32_t kEaShiftB = 0x004C6E64u;
constexpr std::uint32_t kEaShiftC = 0x004E4B98u;
// ★ 条件融合（S3-b）的六条定点差分 case 的入口（见下面 §2c）：
//   0x47A7F3 cmp+jb · 0x49EE49 cmp+jl · 0x404DEA cmp+jle ·
//   0x4200F7 test+jz(je) · 0x4DD199 test+jp（PF ⇒ 退化）· 0x4D19D3 cmp+je（中间夹非写者）
constexpr std::uint32_t kEaFuseJb = 0x0047A7F3u;
constexpr std::uint32_t kEaFuseJl = 0x0049EE49u;
constexpr std::uint32_t kEaFuseJle = 0x00404DEAu;
constexpr std::uint32_t kEaFuseJz = 0x004200F7u;
constexpr std::uint32_t kEaFuseJp = 0x004DD199u;
constexpr std::uint32_t kEaFuseMid = 0x004D19D3u;
// ★ 2026-11-15 · S3-c：`div`/`idiv` 与 flags 的**两臂逐位 oracle**（见下面 §2d）的入口。
//   A 0x48D500 = `mov eax,[ecx+26E4h] / imul eax,3E8h / xor edx,edx / div [ecx+26F0h] / retn`
//   B 0x419F0D = `idiv [esi+77614h] / pop edi / pop esi / mov [ecx],edx / retn`
constexpr std::uint32_t kEaDivFlags = 0x0048D500u;
constexpr std::uint32_t kEaIdivNeutral = 0x00419F0Du;
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

using SteadyClock = std::chrono::steady_clock;

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
#if defined(LIFT_IR_CASE_GEN)
        // 寄存器计数移位（CL）的三条 case（见下面 §2b）；手写 golden 里没有它们。
        case kEaShiftA: return &lift_cc_41880A;
        case kEaShiftB: return &lift_cc_4C6E64;
        case kEaShiftC: return &lift_cc_4E4B98;
        // 条件融合（S3-b）的六条 case（见下面 §2c）。
        case kEaFuseJb: return &lift_cc_47A7F3;
        case kEaFuseJl: return &lift_cc_49EE49;
        case kEaFuseJle: return &lift_cc_404DEA;
        case kEaFuseJz: return &lift_cc_4200F7;
        case kEaFuseJp: return &lift_cc_4DD199;
        case kEaFuseMid: return &lift_cc_4D19D3;
        // S3-c：`div`/`idiv` 与 flags 的两条 oracle（见 §2d）。
        case kEaDivFlags: return &lift_cc_48D500;
        case kEaIdivNeutral: return &lift_cc_419F0D;
#endif
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
            const auto t0 = SteadyClock::now();
            const std::uint32_t entry_ea = ea;
            ea = f(&regs, mem, ea, &fx.m.cpu());
            const std::uint64_t dt = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - t0).count());
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
    const auto t0 = SteadyClock::now();
    const bool ran = lifted ? runLiftedArm(fx, kEaD00, false, &out.stats, &out.err) : runInterpArm(fx, &out.stats, &out.err);
    const auto t1 = SteadyClock::now();
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
// 2b) 寄存器计数移位（CL）的定点差分 —— 三条**真字节** case
// =====================================================================================
// 判据（红了意味着什么）：生成器把 `shl/shr/sar …, CL` 从 `lift_one` 改成 inline 发射之后，
//   下面四件事里**任何一件**与解释器不同 ⇒ 本段红：
//     ① 计数 == 0 ⇒ flags **一位都不写**（`exec_integer.cpp:71` 的早退）
//     ② 计数 > 31 ⇒ **先** `& 0x1F` 再算（`shiftCount`，`:55-63`）
//     ③ `sar` 的 CF（最后移出的那一位）/ OF（解释器**无条件**写 false，`:102`）
//     ④ `shr` 计数 == 1 时的 OF（只有 c == 1 才替换，`:95`）
//   ★ 为什么必须在这一层：①/②/③/④ 里至少 ①③④ 是"这一条**写不写某一位**"而不是"写成什么"；
//     300 帧的六项不变量（stop / frames / misses / arena / lastHash / insns）看不见死写里错的那一位。
//     上一轮实测的教训（S3 设计 §0 / §3.2④）：判据只能是**解释器**，⛔ 不是 SDM 的 "undefined"。
// ★★ **可观察性边界**（本轮实测，⚠ 决定下面三条 case 各自能证什么）：
//   flags 只有在"这条移位是块内**最后一条写 flags** 的指令"时才可能出现在出口比对里；否则被后面的
//   写者覆盖。全语料扫描（`sar|shr|shl|rol|ror …, cl` 共 640 处）的结论：
//     · `sar/shr …, cl`：**没有一处**的标志位能活到出口、也没有一处被 guest 的 CF/OF 读者看到
//       （前面总会先撞上 add/and/or/xor/test/inc… 里的某个写者）⇒ ③④ 在本二进制里**结构上不可观察**
//       （也意味着那两个位在游戏里是**死写**：写错不会改变行为）。
//     · `shl …, cl`：只有 4 处能活到出口（本文件的 case C 用了其中最好的一条）。
//   ⇒ A/B 两条 case 证的是**结果**（r 与写回），C 证的是**flags**（① ② 与 shl 的 CF/OF/SF）。
//     ③④ 只能靠"照抄解释器那几行"保证（注释里给了行号），⛔ 不要声称差分覆盖了它们。
// ★ 字节来源 = PE 真字节（⛔ 不手编；生成器只认 PE 里的字节）：
//   A. `0x41880A` = `sub_4187B0` 的尾块（110 B / 35 条）：`shr×4 + shl×3`，七个计数分别来自
//      `al+8` / `[ebp-0x1B]` / `cl+0x10` / `[ebp-0x1A]` / `bl+0x18` / `[ebp-0x19]` / **入口 cl**；
//      七个结果最后汇进 `edi` 再写 `[T+0x584]` ⇒ **结果**逐位可比（计数 0/1/31/>31 全摆得出来）。
//   B. `0x4C6E64` = `sub_4C6E30` 的位读取器尾段（36 B / 14 条）：`sar edx, cl`，计数 = `7 - [T+0x44]`
//      ⇒ 0 / 1 / 7 / 33 都能摆出来；`test dl, 1` 让**结果**决定走向 ⇒ 结果偏了连出口 EIP 都不同。
//   C. `0x4E4B98` = `sub_4E4B90` 的尾块（27 B / 10 条）：`shl eax, cl` 是**最后一个写 flags** 的指令
//      （后面只有两条 mov + pop + retn）⇒ 出口的六位**就是**这条 shl 写的 ⇒ ①（计数 0 一位不写）
//      与 ②（32/33 掩码后是 0/1）在 **flags 上**直接可观察。
//   ★ 三条都由 `--roots` 驱动（`tests/ir/roots_golden.txt` 里那三行）；组件由生成器按块闭包算，
//     ⛔ 测试里不铺任何"手写 IR"。
#if defined(LIFT_IR_CASE_GEN)
// 0x41880A..0x418877（到 `retn 4` 为止，110 B；后面 2 字节是 `CC` 对齐填充，不取）
constexpr std::uint8_t kShiftA[] = {
    0x88, 0x4D, 0xDC, 0x88, 0x4D, 0xDE, 0x57, 0x0F, 0xB6, 0xC8, 0x8B, 0x45, 0x08, 0x83, 0xC1, 0x08,
    0x8B, 0xF8, 0xD3, 0xEF, 0x0F, 0xB6, 0x4D, 0xE5, 0x8B, 0xF2, 0x23, 0xFE, 0xD3, 0xE7, 0x0F, 0xB6,
    0x4D, 0xDE, 0x83, 0xC1, 0x10, 0x8B, 0xF0, 0xD3, 0xEE, 0x0F, 0xB6, 0x4D, 0xE6, 0x23, 0xF2, 0xD3,
    0xE6, 0x0F, 0xB6, 0xCB, 0x0B, 0xFE, 0x8B, 0xF0, 0x83, 0xC1, 0x18, 0xD3, 0xEE, 0x0F, 0xB6, 0x4D,
    0xE7, 0x23, 0xF2, 0xD3, 0xE6, 0x0F, 0xB6, 0x4D, 0xDC, 0xD3, 0xE8, 0x0B, 0xFE, 0x89, 0x55, 0xF0,
    0x89, 0x55, 0xE8, 0x23, 0xC2, 0x0B, 0xF8, 0x8B, 0x45, 0xFC, 0x89, 0xB8, 0x84, 0x05, 0x00, 0x00,
    0x5F, 0x5E, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00,
};
static_assert(sizeof(kShiftA) == 110, "0x41880A 的块必须正好 110 B（到 retn 4 为止）");

// 0x4C6E64..0x4C6E87（两个 `retn` 都在这段里，36 B）
constexpr std::uint8_t kShiftB[] = {
    0x8B, 0x46, 0x44, 0x8B, 0x56, 0x48, 0xB9, 0x07, 0x00, 0x00, 0x00, 0x2B, 0xC8, 0xD3, 0xFA, 0x8D,
    0x40, 0x01, 0x89, 0x46, 0x44, 0x5E, 0xF6, 0xC2, 0x01, 0x74, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00,
    0xC3, 0x33, 0xC0, 0xC3,
};
static_assert(sizeof(kShiftB) == 36, "0x4C6E64 的段必须正好 36 B（到两个 retn 为止）");

// 0x4E4B98..0x4E4BB2（`shl eax, cl` 在该块里是**最后一条写 flags** 的指令，27 B）
constexpr std::uint8_t kShiftC[] = {
    0xD3, 0xE0,                                      // shl eax, cl
    0xC7, 0x87, 0x44, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // mov dword [edi+344h], 0
    0x89, 0x87, 0x48, 0x03, 0x00, 0x00,              // mov [edi+348h], eax
    0x8B, 0xC7,                                      // mov eax, edi
    0x5F, 0x5E, 0x5B,                                // pop edi / pop esi / pop ebx
    0x8B, 0xE5, 0x5D, 0xC3,                          // mov esp, ebp / pop ebp / retn
};
static_assert(sizeof(kShiftC) == 27, "0x4E4B98 的段必须正好 27 B（到 retn 为止）");

// 一条 case 的入口状态。`r[]` = EAX ECX EDX EBX ESP EBP ESI EDI —— ESP 由 runner 覆盖，
// EBP / 栈槽由 `frame` 铺（见两个 frame 函数）；`u[]` 是每条 case 自己的帧参数。
struct ShiftEntry {
    std::uint32_t r[8];
    std::uint32_t eflags;
    std::uint32_t u[4];
};

using ShiftFrameFn = void (*)(lift_fixture::Fixture&, std::uint32_t esp0, const ShiftEntry&);

// 一臂的读数：8 GPR + 六位 flags + 出口 EIP + 两个内存窗
//   （数据窗 = [T, T+0x804) —— 两条 case 的写都落在这里；栈窗 = [esp0-0x100, esp0+0x100)）。
struct ShiftArm {
    bool ok = false;
    std::string err;
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t eip = 0;
    // ★ 反 vacuity：lifted 臂必须**真的**调到了 lifted 函数（`lifted_calls == 1`）、
    //   **没有**回落解释器（`handoffs == 0`）。少了这两条，`lookupLifted` 少一个 case 就变成
    //   "两臂都是解释器" ⇒ 差分恒绿（本轮的 MUTATION 实验就是这么发现第一版夹具是空的）。
    std::uint64_t lifted_calls = 0;
    std::uint64_t handoffs = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> stack;
};

// A 的帧：`ebp = esp0 + 0x80`；[ebp-4] = 对象 T（`mov eax,[ebp-4]` 之后写 `[eax+584h]`）、
// [ebp] = 被 `pop ebp` 吃掉的、[ebp+4] = 返回地址（`retn 4` 弹它）、[ebp+8] = 被移位的输入、
// [ebp-0x1B] / [ebp-0x1A] / [ebp-0x19] = 三个**栈上**的计数字节。
//   u[0] = 输入值；u[1..3] = 三个计数字节（低 8 位有效）。
void frameShiftA(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    const std::uint32_t ebp = esp0 + 0x80u;
    fx.m.cpu().r[5] = ebp;  // EBP
    fx.m.mem().wr32(ebp - 4u, kSelf);
    fx.m.mem().wr32(ebp, 0x22222222u);
    fx.m.mem().wr32(ebp + 4u, kSentinel);
    fx.m.mem().wr32(ebp + 8u, en.u[0]);
    fx.m.mem().wr8(ebp - 0x1Bu, static_cast<std::uint8_t>(en.u[1] & 0xFFu));
    fx.m.mem().wr8(ebp - 0x1Au, static_cast<std::uint8_t>(en.u[2] & 0xFFu));
    fx.m.mem().wr8(ebp - 0x19u, static_cast<std::uint8_t>(en.u[3] & 0xFFu));
}

// B 的帧：[esp0] = 被 `pop esi` 吃掉的、[esp0+4] = 返回地址（两个 `retn` 之一弹它）；
// [T+0x44] = 位位置（计数 = 7 - 它）、[T+0x48] = 被 sar 的值。u[0] / u[1] 依次是这两个 dword。
void frameShiftB(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    fx.m.mem().wr32(esp0, 0x33333333u);
    fx.m.mem().wr32(esp0 + 4u, kSentinel);
    fx.m.mem().wr32(kSelf + 0x44u, en.u[0]);
    fx.m.mem().wr32(kSelf + 0x48u, en.u[1]);
}

// C 的帧：`ebp = esp0 + 0x80`；[ebp] = 被 `pop ebp` 吃掉的、[ebp+4] = 返回地址（`retn` 弹它）。
// EDI 由 runner 从 `r[7]` 设成 T（两条 mov 写 [edi+344h] / [edi+348h]）；u[] 不用。
void frameShiftC(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry&) {
    const std::uint32_t ebp = esp0 + 0x80u;
    fx.m.cpu().r[5] = ebp;
    fx.m.mem().wr32(ebp, 0x22222222u);
    fx.m.mem().wr32(ebp + 4u, kSentinel);
}

// 两臂同一起点各跑一遍（每臂一台**全新**的 machine ⇒ 初始内存逐字节相同 ⇒ 差分有意义）。
// ★ `data_base` / `data_bytes`：数据区（= 两臂比较的窗口）的落点与大小。缺省是 `[T, T+0x804)`
//   （§2b/§2c 的 case 都写在那里）；§2d 的两条 `div`/`idiv` case 的位移更大（`[ecx+26F0h]` /
//   `[esi+77614h]`）⇒ 显式铺一块更大的、专门的数据区。
ShiftArm runShiftArm(bool lifted, std::uint32_t ea, const std::uint8_t* code, std::size_t len,
                     const ShiftEntry& en, ShiftFrameFn frame, std::uint32_t data_base = kSelf,
                     std::uint32_t data_bytes = kSelfBytes) {
    ShiftArm out;
    lift_fixture::Fixture fx;
    if (!fx.ok) {
        out.err = fx.err;
        return out;
    }
    if (!fx.m.placeCode(ea, code, len, &fx.err)) {
        out.err = fx.err;
        return out;
    }
    const std::vector<std::uint8_t> zero(data_bytes, 0);
    if (!fx.m.placeData(data_base, zero.data(), zero.size(), &fx.err)) {
        out.err = fx.err;
        return out;
    }
    const std::uint32_t esp0 = fx.m.layout().stack_top - 0x100u;
    lift::X86& cpu = fx.m.cpu();
    for (int i = 0; i < 8; ++i) cpu.r[i] = en.r[i];
    cpu.r[4] = esp0;  // ESP
    frame(fx, esp0, en);
    cpu.eflags = en.eflags;
    cpu.eip = ea;
    Stats st;
    std::string err;
    const bool ran = lifted ? runLiftedArm(fx, ea, false, &st, &err) : runInterpArm(fx, &st, &err);
    if (!ran) {
        out.err = err;
        return out;
    }
    for (int i = 0; i < 8; ++i) out.r[i] = cpu.r[i];
    out.eflags = cpu.eflags;
    out.eip = cpu.eip;
    out.lifted_calls = st.lifted_calls;
    out.handoffs = st.interp_handoffs;
    out.data = bytesAt(fx, data_base, data_bytes);
    out.stack = bytesAt(fx, esp0 - 0x100u, 0x200u);
    out.ok = true;
    return out;
}

// 一个变体：两臂各跑一遍，比 8 GPR + 六位 flags + 出口 EIP + 数据窗 + 栈窗（逐位）。
void runShiftDiffCase(const char* tag, std::uint32_t ea, const std::uint8_t* code, std::size_t len,
                      const ShiftEntry& en, ShiftFrameFn frame, std::uint32_t data_base = kSelf,
                      std::uint32_t data_bytes = kSelfBytes) {
    using namespace lift_test;
    g_current_case = tag;
    ++g_checks;
    const ShiftArm in = runShiftArm(false, ea, code, len, en, frame, data_base, data_bytes);
    const ShiftArm li = runShiftArm(true, ea, code, len, en, frame, data_base, data_bytes);
    if (!in.ok || !li.ok) {
        failf(__FILE__, __LINE__, "[%s] 臂失败：interp=%s lifted=%s", tag, in.ok ? "ok" : in.err.c_str(),
              li.ok ? "ok" : li.err.c_str());
        return;
    }
    static const char* kNames[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    for (int i = 0; i < 8; ++i) checkU32(tag, kNames[i], li.r[i], in.r[i], __LINE__);
    checkU32(tag, "EFLAGS&kDiffFlags", li.eflags & kDiffFlags, in.eflags & kDiffFlags, __LINE__);
    checkU32(tag, "EIP", li.eip, in.eip, __LINE__);
    // ★ 反 vacuity（本轮的 MUTATION 实验逼出来的）：lifted 臂必须真的调到 lifted 函数，
    //   ⛔ 不许"两臂都在解释器里跑"⇒ 差分恒绿。组件是**一个**函数 ⇒ 1 次调用、0 次 handoff。
    checkU32(tag, "lifted_calls", static_cast<std::uint32_t>(li.lifted_calls), 1u, __LINE__);
    checkU32(tag, "interp_handoffs", static_cast<std::uint32_t>(li.handoffs), 0u, __LINE__);
    checkRegion(tag, "data[base,base+n)", li.data, in.data, __LINE__);
    checkRegion(tag, "stack[esp0-0x100,esp0+0x100)", li.stack, in.stack, __LINE__);
}

void runShiftDiffCases() {
    // ---- A. `0x41880A`（shr×4 + shl×3；七个计数全在运行期）----
    //   r[] = {EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI}（ESP/EBP 由 runner/frame 覆盖）
    //   u[] = {被移位的输入, [ebp-0x1B], [ebp-0x1A], [ebp-0x19]}
    struct AV {
        const char* tag;
        ShiftEntry en;
    };
    const AV av[] = {
        // ① 计数 0：入口 cl == 0 ⇒ 最后那条 `shr eax, cl` 的**结果**必须等于输入本身。
        //   ⚠ 这一条**不**证 flags：本块在 `shr eax, cl` 之后还有 `or/and/or` 三个写者
        //     （`test`/`or` 会把六位全写掉）⇒ flags 在出口已不可比；flags 级的判据在 case C。
        {"shift-a/count0", {{0x00000000u, 0x00000000u, 0x000000FFu, 0, 0, 0, 0x51515151u, 0x61616161u},
                            0x202u, {0x80000000u, 8u, 12u, 16u}}},
        // ② 计数 > 31：三处摆 33 / 40 / 33（掩码后 1 / 8 / 1）
        {"shift-a/count-gt-31", {{0x00000000u, 0x00000003u, 0x000000FFu, 0, 0, 0, 0x51515151u, 0x61616161u},
                                 0x202u, {0xDEADBEEFu, 33u, 40u, 33u}}},
        // ④ shr 计数 == 1：OF 必须换成"本来那一位 MSB"（输入 0x80000001 ⇒ 1）
        {"shift-a/shr-count1", {{0x00000000u, 0x00000001u, 0x000000FFu, 0, 0, 0, 0x51515151u, 0x61616161u},
                                0x202u, {0x80000001u, 8u, 12u, 16u}}},
        // 边界：al / bl / cl 都非零、输入为负、入口六位 flags **全 1**（AF 与"不该动的位"也比）
        {"shift-a/edge", {{0x00000007u, 0x0000001Fu, 0x0000FFFFu, 0x00000011u, 0, 0, 0x51515151u, 0x61616161u},
                          0xAD7u, {0xFFFFFFF0u, 31u, 17u, 24u}}},
    };
    for (const AV& v : av) runShiftDiffCase(v.tag, kEaShiftA, kShiftA, sizeof(kShiftA), v.en, frameShiftA);

    // ---- B. `0x4C6E64`（`sar edx, cl`，计数 = 7 - [T+0x44]）----
    struct BV {
        const char* tag;
        ShiftEntry en;
    };
    const BV bv[] = {
        // ③ 计数 0：flags（含 OF=1）必须**原样保留** —— 被"无条件 of=false"写一下就红
        {"shift-b/sar-count0", {{0, 0, 0, 0, 0, 0, kSelf, 0}, 0xAD7u, {7u, 0xFFFFFFF0u, 0, 0}}},
        // ③ 计数 1：结果 = 0xFFFFFFF1 >> 1（算术）= 0xFFFFFFF8 ⇒ `test dl,1` 取"非零"出口
        //   ⚠ `test dl, 1` 会把这个 sar 的六位全写掉 ⇒ 这里只证**结果/走向**，不证 sar 的 CF/OF。
        {"shift-b/sar-count1", {{0, 0, 0, 0, 0, 0, kSelf, 0}, 0xAD7u, {6u, 0xFFFFFFF1u, 0, 0}}},
        // ② 计数 > 31：位位置 -26 ⇒ 计数 33 ⇒ **掩码后 1**（不掩码就是 poison / 错值）
        {"shift-b/sar-count-gt-31", {{0, 0, 0, 0, 0, 0, kSelf, 0}, 0xAD7u, {0xFFFFFFE6u, 0x80000000u, 0, 0}}},
        // 计数 7 + 负数：CF 取符号位、结果 -1 ⇒ `test dl,1` 走"非零"那条出口
        {"shift-b/sar-neg", {{0, 0, 0, 0, 0, 0, kSelf, 0}, 0x202u, {0u, 0x80000001u, 0, 0}}},
    };
    for (const BV& v : bv) runShiftDiffCase(v.tag, kEaShiftB, kShiftB, sizeof(kShiftB), v.en, frameShiftB);

    // ---- C. `0x4E4B98`（`shl eax, cl` 是块内最后一条写 flags 的指令 ⇒ 出口六位就是它写的）----
    //   r[] = {EAX=1, ECX=计数, EDX, EBX, ESP, EBP, ESI, EDI=T}；入口六位 flags **全 1**（0xAD7）
    //   ⇒ 计数 0 时"一位都不写"可观察（写成任何值都会与 0xAD7 不同）。
    struct CV {
        const char* tag;
        std::uint32_t count;  // 直接进 CL
    };
    const CV cv[] = {
        {"shift-c/count0", 0u},      // ① 计数 0 ⇒ 0xAD7 原样
        {"shift-c/count1", 1u},      // 计数 1：r=2 ⇒ CF=0 OF=0 ZF=0 SF=0 PF=0 ⇒ 0x202
        {"shift-c/count31", 31u},    // 计数 31：r=0x80000000、CF=bit1(a)=0 ⇒ SF=1 OF=1 PF=0 ⇒ 0xA86
        {"shift-c/count32", 32u},    // ② 32 & 31 == 0 ⇒ 与计数 0 同（掩码 + 闸门一起才成立）
        {"shift-c/count33", 33u},    // ② 33 & 31 == 1 ⇒ 与计数 1 同（不掩码就是 poison/错值）
    };
    for (const CV& v : cv) {
        ShiftEntry en{};
        en.r[0] = 1u;            // EAX：`shl eax, cl` 的输入
        en.r[1] = v.count;       // ECX（CL）
        en.r[7] = kSelf;         // EDI：两条 mov 的基址
        en.eflags = 0xAD7u;      // 六位全 1 + IF
        runShiftDiffCase(v.tag, kEaShiftC, kShiftC, sizeof(kShiftC), en, frameShiftC);
    }
}
// =====================================================================================
// 2c) 条件融合（S3-b：`cmp`/`test` + `jcc`）的定点差分 —— 六条**真字节** case
// =====================================================================================
// 判据（红了意味着什么）：生成器把 `cmp/test` 后面的 `jcc` 从"读六个 flag 位"改成
//   `icmp` 直读（`plan.md` §2.2 的 16 条件表）之后，下面任何一件事与解释器不同 ⇒ 本段红：
//     ① **无符号**关系（`cmp` + `jb` ⇒ `icmp ult`）                      —— F1
//     ② **有符号**关系（`cmp` + `jl`/`jle` ⇒ `icmp slt/sle`）             —— F2 / F3
//     ③ `test` 的 ZF（`test` + `jz`/`je` ⇒ `icmp eq R,0`）                —— F4
//     ④ **PF 不可融合**（`jp`）⇒ 必须退化回"从 base 读 PF"，仍然逐位正确 —— F5
//     ⑤ 生产者与读者之间夹一条**非写者**（`mov`）仍然要融合              —— F6
//   ★ 每条 case 都是**单函数**调用（`runShiftDiffCase` 已断言 `lifted_calls == 1`、
//     `handoffs == 0`）⇒ 两臂跑同一份真字节，逐位比 8 GPR + 六位 flags + 出口 EIP + 数据/栈窗。
//   ★ MUTATION（交付报告里给红/绿）：把 §2.2 的 `icmp slt` 故意改成 `icmp ult` ⇒ F2/F3 必须红。
// ★ 字节来源 = PE 真字节（⛔ 不手编；生成器只认 PE 里的字节），逐条与 `.lst` 对过：
//   F1 0x47A7F3 `cmp edx,[eax+14h] / jb 0x47A805`（两路都以 `pop ebp / ret 8` 收尾）
//   F2 0x49EE49 `cmp eax,0FFh / jl 0x49EE5B`（两路 `pop ebp / ret 20h`）
//   F3 0x404DEA `cmp [ecx+408h],eax / jle 0x404DF8`（两路 `ret`）
//   F4 0x4200F7 `test esi,esi / je 0x4200FE` —— ⚠ `.lst` 把 opcode 0x74 拼成 `je`；
//      解码器把 `je`/`jz` 都映成 `Cond::E`（`decode.cpp:112`）⇒ 这就是 `test`+`jz` 的那条指令
//   F5 0x4DD199 `test ah,5 / jp 0x4DD1A3` —— **PF 条件 ⇒ 退化路径**
//   F6 0x4D19D3 `cmp [ebp+0Ch],0 / mov eax,[ebp+8] / je 0x4D19E6` —— 中间夹一条非写者
//   ★ 六条都**没有** `call`/`jmp`（只有 `jcc` + `ret`）⇒ 组件 = 1 个入口块 + 2 个分支块，
//     一次 `lift_cc_*` 调用就跑完 ⇒ 不需要 trampoline 接管。
// ★ 本段整段在 §2b 的 `LIFT_IR_CASE_GEN` 区里（块尾那个 `#endif` 覆盖到这里）。
//   EA 常量在文件头（`kEaFuse*`，`lookupLifted` 也要用）。

// 0x47A7F3..0x47A80C（26 B）：cmp edx,[eax+14h] / jb / mov eax,1 / mov [ecx],eax / xor eax,eax
//   / pop ebp / ret 8  ||  xor eax,eax / mov [ecx],eax / pop ebp / ret 8
constexpr std::uint8_t kFuseJb[] = {
    0x3B, 0x50, 0x14, 0x72, 0x0D, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x89, 0x01, 0x33,
    0xC0, 0x5D, 0xC2, 0x08, 0x00, 0x33, 0xC0, 0x89, 0x01, 0x5D, 0xC2, 0x08, 0x00,
};
static_assert(sizeof(kFuseJb) == 26, "0x47A7F3 的段必须正好 26 B（到两路的 ret 8 为止）");

// 0x49EE49..0x49EE5E（22 B）：cmp eax,0FFh / jl / mov eax,0FFh / pop ebp / ret 20h
//   ||（目标）pop ebp / ret 20h
constexpr std::uint8_t kFuseJl[] = {
    0x3D, 0xFF, 0x00, 0x00, 0x00, 0x7C, 0x0B, 0xB8, 0xFF, 0x00, 0x00,
    0x00, 0x5D, 0xC2, 0x20, 0x00, 0x33, 0xC0, 0x5D, 0xC2, 0x20, 0x00,
};
static_assert(sizeof(kFuseJl) == 22, "0x49EE49 的段必须正好 22 B（到两路的 ret 20h 为止）");

// 0x404DEA..0x404DFA（17 B）：cmp [ecx+408h],eax / jle / mov eax,1 / ret || xor eax,eax / ret
constexpr std::uint8_t kFuseJle[] = {
    0x39, 0x81, 0x08, 0x04, 0x00, 0x00, 0x7E, 0x06, 0xB8,
    0x01, 0x00, 0x00, 0x00, 0xC3, 0x33, 0xC0, 0xC3,
};
static_assert(sizeof(kFuseJle) == 17, "0x404DEA 的段必须正好 17 B（到两路的 ret 为止）");

// 0x4200F7..0x420100（10 B）：test esi,esi / je / mov [esi+8],edi / pop edi / pop esi / ret
constexpr std::uint8_t kFuseJz[] = {
    0x85, 0xF6, 0x74, 0x03, 0x89, 0x7E, 0x08, 0x5F, 0x5E, 0xC3,
};
static_assert(sizeof(kFuseJz) == 10, "0x4200F7 的段必须正好 10 B（到 ret 为止）");

// 0x4DD199..0x4DD1A9（17 B）：test ah,5 / jp / or eax,-1 / pop ebp / ret || mov eax,1 / pop ebp / ret
constexpr std::uint8_t kFuseJp[] = {
    0xF6, 0xC4, 0x05, 0x7A, 0x05, 0x83, 0xC8, 0xFF, 0x5D, 0xC3,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0x5D, 0xC3,
};
static_assert(sizeof(kFuseJp) == 17, "0x4DD199 的段必须正好 17 B（到两路的 ret 为止）");

// 0x4D19D3..0x4D19EF（29 B）：cmp [ebp+0Ch],0 / mov eax,[ebp+8] / je
//   / or [eax+4],1 / xor eax,eax / pop ebp / ret 8  ||  and [eax+4],~1 / xor eax,eax / pop ebp / ret 8
constexpr std::uint8_t kFuseMid[] = {
    0x83, 0x7D, 0x0C, 0x00, 0x8B, 0x45, 0x08, 0x74, 0x0A, 0x83, 0x48, 0x04, 0x01, 0x33, 0xC0,
    0x5D, 0xC2, 0x08, 0x00, 0x83, 0x60, 0x04, 0xFE, 0x33, 0xC0, 0x5D, 0xC2, 0x08, 0x00,
};
static_assert(sizeof(kFuseMid) == 29, "0x4D19D3 的段必须正好 29 B（到两路的 ret 8 为止）");

// ---- 六条帧（口径 = 入口栈槽 + 被读的对象；r[] / u[] 由 case 表给）----
// F1：`pop ebp` 吃 [E]、`ret 8` 返回 [E+4]；被比较的 dword 在 [eax+14h]。
void frameFuseJb(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    fx.m.mem().wr32(esp0, 0x22222222u);
    fx.m.mem().wr32(esp0 + 4u, kSentinel);
    fx.m.mem().wr32(kSelf + 0x14u, en.u[1]);
}
// F2：`pop ebp` 吃 [E]、`ret 20h` 返回 [E+4]。
void frameFuseJl(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry&) {
    fx.m.mem().wr32(esp0, 0x22222222u);
    fx.m.mem().wr32(esp0 + 4u, kSentinel);
}
// F3：`ret` 返回 [E]；被比较的 dword 在 [ecx+408h]（= T+0x408，在数据窗里）。
void frameFuseJle(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    fx.m.mem().wr32(esp0, kSentinel);
    fx.m.mem().wr32(kSelf + 0x408u, en.u[0]);
}
// F4：`pop edi` / `pop esi` 吃 [E] / [E+4]、`ret` 返回 [E+8]。
void frameFuseJz(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry&) {
    fx.m.mem().wr32(esp0, 0x11111111u);
    fx.m.mem().wr32(esp0 + 4u, 0x33333333u);
    fx.m.mem().wr32(esp0 + 8u, kSentinel);
}
// F5：`pop ebp` 吃 [E]、`ret` 返回 [E+4]。
void frameFuseJp(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry&) {
    fx.m.mem().wr32(esp0, 0x22222222u);
    fx.m.mem().wr32(esp0 + 4u, kSentinel);
}
// F6：`ebp = esp0 + 0x80`；[ebp+8] = T（被 `or/and [eax+4]` 写）、[ebp+0Ch] = 被比较的 dword；
//     `pop ebp` 吃 [E]、`ret 8` 返回 [E+4]。
void frameFuseMid(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    const std::uint32_t ebp = esp0 + 0x80u;
    fx.m.cpu().r[5] = ebp;  // EBP
    fx.m.mem().wr32(ebp + 8u, kSelf);
    fx.m.mem().wr32(ebp + 0xCu, en.u[0]);
    fx.m.mem().wr32(esp0, 0x22222222u);
    fx.m.mem().wr32(esp0 + 4u, kSentinel);
}

void runFusionDiffCases() {
    struct V {
        const char* tag;
        ShiftEntry en;
    };
    // ---- F1 `cmp edx,[eax+14h]` + `jb`（无符号）----
    //   r[] = {EAX=T, ECX=T, EDX=u[0], ...}；u[1] = [T+0x14]；观测点 = [ecx]（0/1）。
    const V jb[] = {
        {"fuse/cmp-jb/taken", {{kSelf, kSelf, 1u, 0, 0, 0, 0, 0}, 0x202u, {1u, 5u, 0, 0}}},
        {"fuse/cmp-jb/not-taken", {{kSelf, kSelf, 5u, 0, 0, 0, 0, 0}, 0x202u, {5u, 1u, 0, 0}}},
        // 边界：相等 ⇒ **不**跳（`jb` 是严格小于；写成 `jbe` 就红）
        {"fuse/cmp-jb/equal", {{kSelf, kSelf, 3u, 0, 0, 0, 0, 0}, 0x202u, {3u, 3u, 0, 0}}},
        // 无符号 vs 有符号的分界：0xFFFFFFFF > 5（无符号）⇒ 不跳；按有符号 -1 < 5 就会跳
        {"fuse/cmp-jb/unsigned-edge",
         {{kSelf, kSelf, 0xFFFFFFFFu, 0, 0, 0, 0, 0}, 0x202u, {0xFFFFFFFFu, 5u, 0, 0}}},
    };
    for (const V& v : jb) runShiftDiffCase(v.tag, kEaFuseJb, kFuseJb, sizeof(kFuseJb), v.en, frameFuseJb);

    // ---- F2 `cmp eax,0FFh` + `jl`（有符号；EAX 是观测点：跳 ⇒ 保留原值、不跳 ⇒ 0xFF）----
    const V jl[] = {
        // -1 < 255（有符号）⇒ 跳；按无符号 0xFFFFFFFF < 255 不成立 ⇒ MUTATION 会翻
        {"fuse/cmp-jl/neg-taken",
         {{0xFFFFFFFFu, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        // 0xFFFFFF00 = -256 < 255 ⇒ 跳
        {"fuse/cmp-jl/neg256-taken",
         {{0xFFFFFF00u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        // 4096 > 255 ⇒ 不跳 ⇒ EAX = 0xFF
        {"fuse/cmp-jl/pos-not-taken", {{0x1000u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        // 0x7FFFFFFF > 255 ⇒ 不跳
        {"fuse/cmp-jl/max-not-taken",
         {{0x7FFFFFFFu, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
    };
    for (const V& v : jl) runShiftDiffCase(v.tag, kEaFuseJl, kFuseJl, sizeof(kFuseJl), v.en, frameFuseJl);

    // ---- F3 `cmp [ecx+408h],eax` + `jle`（有符号 + ZF；观测点 = EAX 的 1/0）----
    const V jle[] = {
        // 相等 ⇒ ZF=1 ⇒ 跳（`jle` 含 ZF）⇒ EAX=0
        {"fuse/cmp-jle/equal", {{kSelf, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {5u, 5u, 0, 0}}},
        // mem=-1 <= eax=0（有符号）⇒ 跳；按无符号 0xFFFFFFFF <= 0 不成立
        {"fuse/cmp-jle/neg-vs-zero", {{kSelf, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {0xFFFFFFFFu, 0u, 0, 0}}},
        // mem=0x7FFFFFFF <= eax=-1（有符号）不成立 ⇒ 不跳（EAX=1）；按无符号会成立
        {"fuse/cmp-jle/pos-vs-neg", {{kSelf, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {0x7FFFFFFFu, 0xFFFFFFFFu, 0, 0}}},
        // mem=-2 <= eax=-1 ⇒ 跳（两者都负，SF=1/OF=0 ⇒ 有符号比较）
        {"fuse/cmp-jle/both-neg", {{kSelf, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {0xFFFFFFFEu, 0xFFFFFFFFu, 0, 0}}},
    };
    for (const V& v : jle)
        runShiftDiffCase(v.tag, kEaFuseJle, kFuseJle, sizeof(kFuseJle), v.en, frameFuseJle);

    // ---- F4 `test esi,esi` + `jz`（ZF；观测点 = [T+8] 的 dword）----
    //   r[] = {EAX, ECX, EDX, EBX, ESP, EBP, ESI=u[0], EDI=u[1]}
    const V jz[] = {
        {"fuse/test-jz/not-taken",
         {{0, 0, 0, 0, 0, 0, kSelf, 0x5A5A5A5Au}, 0x202u, {kSelf, 0x5A5A5A5Au, 0, 0}}},
        {"fuse/test-jz/taken", {{0, 0, 0, 0, 0, 0, 0u, 0x5A5A5A5Au}, 0x202u, {0u, 0x5A5A5A5Au, 0, 0}}},
        {"fuse/test-jz/not-taken-2",
         {{0, 0, 0, 0, 0, 0, kSelf, 0x00000001u}, 0x202u, {kSelf, 1u, 0, 0}}},
    };
    for (const V& v : jz) runShiftDiffCase(v.tag, kEaFuseJz, kFuseJz, sizeof(kFuseJz), v.en, frameFuseJz);

    // ---- F5 `test ah,5` + `jp`（**PF ⇒ 退化路径**；观测点 = EAX 的 1/0xFFFFFFFF）----
    //   PF = (popcount(ah & 5) 是偶数)。EAX 只通过 AH 影响条件。
    const V jp[] = {
        {"fuse/test-jp/pf-even-ah0", {{0x00000011u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        {"fuse/test-jp/pf-odd-ah1", {{0x00000123u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        {"fuse/test-jp/pf-odd-ah4", {{0x00000400u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
        {"fuse/test-jp/pf-even-ah5", {{0x00000500u, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0, 0, 0, 0}}},
    };
    for (const V& v : jp) runShiftDiffCase(v.tag, kEaFuseJp, kFuseJp, sizeof(kFuseJp), v.en, frameFuseJp);

    // ---- F6 `cmp [ebp+0Ch],0` + `mov eax,[ebp+8]` + `je`（夹非写者；观测点 = [T+4] 的 bit0）----
    const V mid[] = {
        {"fuse/cmp-je/mid/taken", {{0, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0u, 0, 0, 0}}},
        {"fuse/cmp-je/mid/not-taken", {{0, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {7u, 0, 0, 0}}},
        {"fuse/cmp-je/mid/not-taken-neg",
         {{0, 0, 0, 0, 0, 0, 0, 0}, 0x202u, {0xFFFFFFFFu, 0, 0, 0}}},
    };
    for (const V& v : mid)
        runShiftDiffCase(v.tag, kEaFuseMid, kFuseMid, sizeof(kFuseMid), v.en, frameFuseMid);
}

// =====================================================================================
// 2d) `div` / `idiv` 与 flags 的两臂逐位 oracle —— 两条**真字节** case（S3-c）
// =====================================================================================
// 判据（红了意味着什么）：S3-c 把"块尾整字物化"换成"按 need_out 物化"、把写者的掩码按活性收窄
//   之后，下面任何一件事与解释器不同 ⇒ 本段红：
//     ① `div/idiv` 在解释器里**一位 flags 都不写**（`exec_integer.cpp:270-332` 的 `doDiv` 里
//        没有任何 `setXxx`）⇒ 紧邻它前后的写者值必须原样穿过这条屏障。写错 = 把 div 当
//        "六位未定义 ⇒ 清 0"（SDM 口径），而那在 300 帧的六项不变量里**看不见**。
//     ② `lift_one` 屏障的**唯一出口**（`materialize(kAllBits)` → `flushRegs`）必须真的把字写回
//        `%st`：漏了它 ⇒ 屏障之后从 `%st` 回读的是旧值（A 的 `xor edx,edx` 就在屏障前面）。
//     ③ escape（末条 `retn`）⇒ `need_out = ALL` ⇒ 出口六位必须与解释器逐位相同。
//        B 的入口六位**全 1**（0xAD7）且 idiv 一位不写 ⇒ "少物化一位 / 多清一位"都会红。
//   ★ 死位按定义不可观察 ⇒ 这两条 case 是本步**唯一新增风险面**上的守卫（plan §3.2④）。
//   ★ 两条都是**单函数**调用（`runShiftDiffCase` 已断言 `lifted_calls == 1`、`handoffs == 0`）。
//   ★ 为什么必须先钉解释器臂：两臂可能"同样错"（例如夹具把除数铺错就退化成别的路径）⇒
//     A/B 各有一个手算锚（plan §3.2④ 给的数）先钉在解释器臂上。
// ★ 字节来源 = PE 真字节（⛔ 不手编；生成器只认 PE 里的字节），用
//   `pnpm tools disasm-at at --ea <EA> --bytes N` 取并逐字节核过：
//   A 0x48D500（21 B）= mov eax,[ecx+26E4h] / imul eax,3E8h / xor edx,edx / div [ecx+26F0h] / retn
//     手算锚（[ecx+26E4h]=1234、[ecx+26F0h]=7）：EAX=0x2B09D(176285)、EDX=5、
//     EFLAGS&0x8D5=0x44（`xor edx,edx`：ZF=1、PF=1，其余 0；`div` 保持它）。
//   B 0x419F0D（11 B）= idiv [esi+77614h] / pop edi / pop esi / mov [ecx],edx / retn
//     手算锚（EAX=100、EDX=0、[esi+77614h]=7）：EAX=14、EDX=2、[ecx]=2、EFLAGS&0x8D5=0x8D5。
//   ★ B 的位移很大（0x77614）⇒ 它的数据区铺在 0x00080000（0x78000 B），esi / ecx 都指向那里；
//     ⛔ 不把 `kSelf` 的数据区改大去凑它（那会让 §2b/§2c 的窗口口径也一起变）。
constexpr std::uint8_t kDivFlags[] = {
    0x8B, 0x81, 0xE4, 0x26, 0x00, 0x00,              // mov eax, [ecx+26E4h]
    0x69, 0xC0, 0xE8, 0x03, 0x00, 0x00,              // imul eax, 3E8h
    0x33, 0xD2,                                      // xor edx, edx
    0xF7, 0xB1, 0xF0, 0x26, 0x00, 0x00,              // div dword ptr [ecx+26F0h]
    0xC3,                                            // retn
};
static_assert(sizeof(kDivFlags) == 21, "0x48D500 的段必须正好 21 B（到 retn 为止：0x48D500..0x48D514）");

constexpr std::uint8_t kIdivNeutral[] = {
    0xF7, 0xBE, 0x14, 0x76, 0x07, 0x00,              // idiv dword ptr [esi+77614h]
    0x5F, 0x5E,                                      // pop edi / pop esi
    0x89, 0x11,                                      // mov [ecx], edx
    0xC3,                                            // retn
};
static_assert(sizeof(kIdivNeutral) == 11, "0x419F0D 的段必须正好 11 B（到 retn 为止）");

// B 的数据区（见上：0x77614 的位移）
constexpr std::uint32_t kIdivBase = 0x00080000u;
constexpr std::uint32_t kIdivBytes = 0x78000u;
constexpr std::uint32_t kIdivDivisorOff = 0x77614u;

// A 的数据区：`[T+0x26E4]` / `[T+0x26F0]` 都在里面
constexpr std::uint32_t kDivDataBytes = 0x2800u;

// A 的帧：`retn` 弹 [esp]；[ecx+26E4h] = 被乘数、[ecx+26F0h] = 除数。u[0]/u[1] 依次是这两个 dword。
void frameDivFlags(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    fx.m.mem().wr32(esp0, kSentinel);
    fx.m.mem().wr32(kSelf + 0x26E4u, en.u[0]);
    fx.m.mem().wr32(kSelf + 0x26F0u, en.u[1]);
}

// B 的帧：`pop edi` / `pop esi` 吃 [esp] / [esp+4]、`retn` 弹 [esp+8]；
//   [esi+77614h] = 除数（esi = kIdivBase）、`mov [ecx],edx` 写 [ecx]（ecx = kIdivBase+0x10）。
void frameIdivNeutral(lift_fixture::Fixture& fx, std::uint32_t esp0, const ShiftEntry& en) {
    fx.m.mem().wr32(esp0, 0x11111111u);
    fx.m.mem().wr32(esp0 + 4u, 0x22222222u);
    fx.m.mem().wr32(esp0 + 8u, kSentinel);
    fx.m.mem().wr32(kIdivBase + kIdivDivisorOff, en.u[0]);
}

void runDivFlagsDiffCases() {
    using namespace lift_test;
    auto anchor = [](const char* tag, const char* what, std::uint32_t got, std::uint32_t want) {
        ++g_checks;
        if (got != want)
            failf(__FILE__, __LINE__, "[%s] 解释器臂 %s=%08X，手算锚=%08X", tag, what, got, want);
    };
    struct V {
        const char* tag;
        ShiftEntry en;
    };
    // ---- A. `0x48D500`（两条屏障 imul/div + `xor edx,edx` + escape retn）----
    //   r[] = {EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI}（ESP 由 runner 覆盖）
    //   u[] = {被乘数 [T+0x26E4], 除数 [T+0x26F0]}
    const V div[] = {
        {"div-flags/imul-xor-div", {{0, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {1234u, 7u, 0, 0}}},
        {"div-flags/one-by-one", {{0, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {1u, 1u, 0, 0}}},
        // 商 0、余数 = 被除数（除数 = 0xFFFFFFFF）⇒ 两条屏障之间的值仍然原样穿过
        {"div-flags/divisor-max",
         {{0, kSelf, 0, 0, 0, 0, 0, 0}, 0x202u, {1234u, 0xFFFFFFFFu, 0, 0}}},
    };
    // ① 先钉解释器臂的手算锚（plan §3.2④ 的数）
    {
        const ShiftArm in = runShiftArm(false, kEaDivFlags, kDivFlags, sizeof(kDivFlags), div[0].en,
                                        frameDivFlags, kSelf, kDivDataBytes);
        g_current_case = "div-flags/interp-anchor";
        ++g_checks;
        if (!in.ok) {
            failf(__FILE__, __LINE__, "[div-flags/interp-anchor] 解释器臂失败：%s", in.err.c_str());
        } else {
            anchor("div-flags/interp-anchor", "EAX", in.r[0], 0x0002B09Du);
            anchor("div-flags/interp-anchor", "EDX", in.r[2], 5u);
            anchor("div-flags/interp-anchor", "EFLAGS&kDiffFlags", in.eflags & kDiffFlags, 0x44u);
            anchor("div-flags/interp-anchor", "EIP", in.eip, kSentinel);
        }
    }
    for (const V& v : div)
        runShiftDiffCase(v.tag, kEaDivFlags, kDivFlags, sizeof(kDivFlags), v.en, frameDivFlags, kSelf,
                         kDivDataBytes);

    // ---- B. `0x419F0D`（一条屏障 idiv + escape retn；入口六位全 1）----
    //   r[] = {EAX, ECX, EDX, EBX, ESP, EBP, ESI=kIdivBase, EDI}；u[0] = 除数
    const V idiv[] = {
        {"idiv-neutral/100-7", {{100u, kIdivBase + 0x10u, 0, 0, 0, 0, kIdivBase, 0}, 0xAD7u, {7u, 0, 0, 0}}},
        {"idiv-neutral/7-2", {{7u, kIdivBase + 0x10u, 0, 0, 0, 0, kIdivBase, 0}, 0xAD7u, {2u, 0, 0, 0}}},
        // 有符号：edx:eax = -4294967396 ⇒ ÷3 截断到 0 ⇒ 商 -1431655798、余数 -2
        {"idiv-neutral/negative",
         {{0xFFFFFF9Cu, kIdivBase + 0x10u, 0xFFFFFFFFu, 0, 0, 0, kIdivBase, 0}, 0xAD7u, {3u, 0, 0, 0}}},
    };
    {
        const ShiftArm in = runShiftArm(false, kEaIdivNeutral, kIdivNeutral, sizeof(kIdivNeutral),
                                        idiv[0].en, frameIdivNeutral, kIdivBase, kIdivBytes);
        g_current_case = "idiv-neutral/interp-anchor";
        ++g_checks;
        if (!in.ok) {
            failf(__FILE__, __LINE__, "[idiv-neutral/interp-anchor] 解释器臂失败：%s", in.err.c_str());
        } else {
            anchor("idiv-neutral/interp-anchor", "EAX", in.r[0], 14u);
            anchor("idiv-neutral/interp-anchor", "EDX", in.r[2], 2u);
            anchor("idiv-neutral/interp-anchor", "[ecx]", in.data[0x10u] | (in.data[0x11u] << 8) |
                                                      (in.data[0x12u] << 16) |
                                                      (static_cast<std::uint32_t>(in.data[0x13u]) << 24),
                   2u);
            anchor("idiv-neutral/interp-anchor", "EFLAGS&kDiffFlags", in.eflags & kDiffFlags, 0x8D5u);
            anchor("idiv-neutral/interp-anchor", "EIP", in.eip, kSentinel);
        }
    }
    for (const V& v : idiv)
        runShiftDiffCase(v.tag, kEaIdivNeutral, kIdivNeutral, sizeof(kIdivNeutral), v.en,
                         frameIdivNeutral, kIdivBase, kIdivBytes);
}

#endif  // LIFT_IR_CASE_GEN

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
#ifdef LIFT_GEN_TICKS
                // ★ T1c：**生成物**（`lift-gen` 的输出）在块边界发 `@lift_tick` ⇒ lifted 臂也按
                //   解释器的同一口径记账 ⇒ 它必须等于"这条载荷执行过的**全部** guest 指令"：
                //   根函数的闭式 + 冷路径那 3 条 + 建表器的动态条数（后者是 handoff 给解释器跑的，
                //   本来就在解释器的 `cpu.insns` 里）。⇒ 两臂条数**逐条相等**。
                //   ★ 这比原来断言 `== builder`（= 只有 handoff 那部分）**更强**：它同时证明
                //   `lift_tick` 的条数精确（多记一条/少记一条都会红）。
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(li.stats.insns),
                                  static_cast<std::int64_t>(in.stats.insns));
#else
                // 手写 golden（`tests/ir/lift_*.ll`）没有 tick ⇒ lifted 臂只有 handoff 那部分。
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(li.stats.insns), static_cast<std::int64_t>(builder));
#endif
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

    // ---- 2b) 寄存器计数移位（CL）的定点差分 ---------------------------------------------
#ifdef LIFT_IR_CASE_GEN
    // ★ 只有**链接生成器产物**的两条 target 才有这两个入口（手写 golden 里没有）：
    //   `lift-ir-gen-proto` / `lift-ir-gen-shim`。判据见上面 §2b 的标题块。
    runShiftDiffCases();
    std::printf("[lift-ir-proto] shift-diff: 3 条真字节 case（13 变体）跑完（见 §2b 的四条判据）\n");
    // ---- 2c) 条件融合（S3-b：`cmp`/`test` + `jcc`）的定点差分 ---------------------------
    //   判据见 §2c 的标题块：无符号 / 有符号 / ZF / PF 退化 / 中间夹非写者。
    runFusionDiffCases();
    std::printf("[lift-ir-proto] fuse-diff: 6 条真字节 case（22 变体）跑完（见 §2c 的五条判据）\n");
    // ---- 2d) `div`/`idiv` 与 flags 的两臂逐位 oracle（S3-c）--------------------------------
    //   判据见 §2d 的标题块：div/idiv 的 flags 中性 + 屏障唯一出口 + escape ⇒ need_out=ALL。
    runDivFlagsDiffCases();
    std::printf("[lift-ir-proto] div/idiv-diff: 2 条真字节 case（6 变体 + 2 手算锚）跑完（见 §2d）\n");
#endif

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
                    const auto t0 = SteadyClock::now();
                    if (runLiftedArm(fx, kEaD00, false, &st, &err)) {
                        const auto t1 = SteadyClock::now();
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
                    const auto t0 = SteadyClock::now();
                    if (runInterpArm(fx2, &st, &err)) {
                        const auto t1 = SteadyClock::now();
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

    // ★ 同一份测试体被**两条** CTest 复用（`lift-ir-proto` 链手写 golden、`lift-ir-gen-proto`
    //   链 `emulator-lift-gen` 的生成物，见 CMakeLists 的 `lift-ir-gen` 段）⇒ 用例名必须跟着
    //   target 走：`lift-ir-gen-proto` 由 CMake 传 `LIFT_IR_CASE_GEN=1`。⛔ 不复制测试体 /
    //   不建第二份源文件；断言与判据一条没变。
    //   ⚠ 为什么写成 `#if` 两份字面量、而不是一个 `LIFT_IR_CASE_NAME` 宏：`tools/test/
    //     cmake-test-entry.test.mjs` 的静态判据只认**调用点的字面量**
    //   （`finish("…")` / `g_current_case = "…"`）—— 宏在那里看不见，会让这两条 CTest 重新变成
    //   "注册名与用例名对不上"。
#ifdef LIFT_IR_CASE_GEN
    return lift_test::finish("lift-ir-gen-proto");
#else
    return lift_test::finish("lift-ir-proto");
#endif
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
