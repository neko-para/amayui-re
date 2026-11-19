// emulator-lift · **读数**：五类 guest 指令形状的「每条指令墙钟（ns/条）」
//
// ★ 这不是合同、是**测量**（AGENTS.md §10：读数进文档，不进 test 的判据面）。
//   本文件里唯一会红的东西是**测量本身的前提**：
//     ① 每一遍必须真的执行了我们以为的那么多条指令（`cpu.insns` 实测 == 请求值）；
//     ② 每种形状的「每轮条数」必须与注释一致（除不尽 = 形状写错了，读数无意义）；
//     ③ 每轮确实停在正常停因（`MaxInsns`）而不是解码失败 / 未实现；
//     ④ 块计时闸门（`REQ-01M7CAJHYHWCVF3XQ7NM3X428Y`）的四个**关系**：无消费者时 `b.ns` 合计 == 0
//        （闸门真的关掉了累加）、`--profile` 时 > 0（消费者仍拿得到计时）、只开
//        `LIFT_FRAME_TRACE` 时仍 == 0（trace 的 `guest=` 是 `--profile` 的显示面，不是独立判据）、
//        三臂 `insns` 相等。
//   ⛔ 这里**不断言任何 ns 数值** —— 那是某台机器某次运行的读数，不是仓库的不变量。
//
// ★ 为什么要它（父级问题）：一段真实转场（FELLOW 进入）实测 2.315 亿条 guest 指令 / 6.74 s
//   （无 `--profile` 臂），窗口 profile 的 `interp` 桶 = 5.878 s ⇒ ≈25 ns/条。好的解释器是 3–10 ns/条。
//   要回答「25 ns 里派发 / 访存 / 调用 / x87 / shim 各占多少」，需要一个**不碰热路径**、
//   可移植（Windows x64 + macOS arm64：只用 `std::chrono`，无 arch 专属指令 / 汇编）的读数面。
//
// 形状（每轮 = 一次循环体；`条/轮` 与 `块/轮` 见下面的 `Arm` 表，⛔ 不许只写在报告里）：
//   alu-loop     4 条 / 1 块：`mov eax,eax` `mov edx,edx` `dec ecx` `jnz`（无内存、无调用）
//   alu-wide    10 条 / 1 块：8× `mov reg,reg` + dec + jnz
//                —— **附加诊断**：与 alu-loop 两点拟合 `t_轮 = b + c·k`，把"每块税 b"与
//                   "每条边际成本 c"分开（单一形状的 ns/条 里两者是混着的）；靠 `LIFT_INSN_COST_EXTRA`
//                   打开（默认关 = 默认门禁只跑五个必需形状）
//   mem-loop     4 条 / 1 块：`mov eax,[ebx]` `mov [ebx],eax` `dec ecx` `jnz`（同一页已 committed）
//   call-loop    5 条 / 3 块：`call leaf` + leaf(`mov eax,eax` `ret`) + dec + jnz
//   x87-loop     5 条 / 1 块：入口 `fld1` 一次，之后每轮 `fld1` `fadd st0,st1` `fstp st0` dec jnz
//                —— 值恒为 1.0 / 2.0、栈深恒 1 ⇒ 1e7 条也不会溢出 / 不会长栈
//   shim-tick   10 条 / 2 块 + **1 次 shim**：8× `mov reg,reg`（填充）`call kernel32!GetTickCount`
//                `dec ecx` `jnz`
//                ★ 为什么有 8 条填充：任务要求「每遍 ≥1e7 条」。3 条/轮那版会把 1e7 条变成 330 万次
//                  shim 调用（实测 ~1.8 s/遍 ⇒ 3 遍 5.4 s，默认门禁太贵）；填充把每遍的 shim 调用
//                  降到 100 万次，而 shim 的成本由**残差**（实测 ns/轮 − 模型）单独算出来，不丢。
//                ★ shim 本身**不计** `cpu.insns`（`Machine::run` 的 shim 分支没有 `++cpu.insns`）
//                  ⇒ ns/轮 = 「一次 shim 调用 + 10 条指令的派发」；ns/条 被 10 条摊薄，读报告看 ns/轮
//   shim-lasterr 同形，但调 `kernel32!GetLastError`（函数体只有 `call.result = cpu.shim.last_error`）
//                —— **附加诊断**（`LIFT_INSN_COST_EXTRA`）：shim 表里最轻的一条 ⇒ 它给出
//                   「shim 派发固定开销」的**下界**（几乎不含 API 自己的工作）
//
// ★ `LIFT_INSN_COST_EXTRA=1`（默认关）额外跑：`alu-wide`（两点拟合）+ `shim-lasterr` + 末尾的
//   `profiled` 诊断（同一个 alu 循环带 / 不带 `profile_begin/end` 各一遍，差值 = **仪器自身的税**：
//   窗口臂的块首/块尾 2 次 + 迭代守卫 2 次 `steady_clock::now()` + 记账；闸门关时对照臂那 2 次块时钟
//   已经不取）与 `gate-probe`（闸门关 vs "armed 但窗口从未开"，同一进程紧邻两臂 ⇒ 差值 ≈ 每块
//   4 次 now()，即本单关掉的 2 次 ≈ 差值一半）。
//   默认关的理由是**默认门禁的耗时**：五形状 × 3 遍
//   × 1e7 条在本机满载时已 ~11 s；两个附加臂各再 +2~5 s，而它们是"解释读数用"的诊断，不是读数本身。
//   ⛔ 关掉它们不会让本用例变成"什么都不量"：五个必需形状照跑、照打表。
//
// ★ 口径（读报告必须一起看，否则数字会被读错）：
//   ① 每遍 N = 10,000,000 条（≥ 1e7），用 `RunOptions.max_insns` 停在精确条数上；实测 `r.insns`
//      必须等于请求值（那一条会红）。报告里的 N 一律用**实测** `cpu.insns`，不写理论值。
//   ② 3 遍**轮转**（先所有形状各跑 1 遍，再第 2 遍…）：慢漂移（频率 / 热 / 调度）对各形状共模，
//      比"一个形状连跑 3 遍再下一个"更能让形状之间可比。每条报墙角，形状报中位 + 极差。
//   ③ 每遍之前复位起点：`eip` / `ecx`（+ mem 的 `ebx`、x87 的 FPU 栈）。块缓存从第 2 遍起是**热的**
//      （第 1 遍含一次解码）⇒ 中位反映**稳态**；本文件的形状解码成本 ~0（同一个块被反复执行）。
//      ⚠ 这不等于真实负载：真实 guest 代码的代码足迹大得多（i-cache / 解码缓存 / 分支预测都不在
//        本文件的"同一个 7~32 字节循环"里）⇒ 报告里必须把这条限制写出来。
//   ④ 时间只用 `std::chrono::steady_clock`（可移植；⛔ 无 `rdtsc` / 无汇编）。
//   ⑤ 末尾的**校准**（依赖 add 链 / `steady_clock::now()` / 未竞争 `mutex` lock+unlock）只用来
//      给读数**标定机器状态**：绝对 ns 会随频率 / 抢占漂；校准值偏大 ⇒ 这台机器当时慢
//      （例：`dispatch_tax.cpp` 的记录里 lock+unlock 是 4.8~10 ns）。⛔ 校准值也不进判据。
//
// ★ 档位：顶层无条件注册 ⇒ **pure 档**（`AGENTS.md` §5：判据 = 注册位置）。它只用 `initBare()`
//   自造机器码 + 真 `Machine`，不碰游戏 / LFS / GPU / 音频设备。
// ★ `std::getenv` 在 MSVC / clang-cl 里被标成 deprecated（本仓其它 TU 用 `_CRT_SECURE_NO_WARNINGS`
//   关它）。这是**可移植**用法，只在**这个文件**里显式关掉，不去动 target 的编译口径。
#ifdef _MSC_VER
// 命令行（CMake 的 `_CRT_SECURE_NO_WARNINGS=1`）可能已经定义过；重复 `#define` 在
// clang-cl 的 `/W4 /WX` 下是 `-Wmacro-redefined` ⇒ 让出一条路给"已经定义"的构建。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x100u;  // 每个形状一块自己的循环（同一页内，够放）
constexpr std::uint32_t kDataEa = 0x00480000u;  // mem-loop 的 committed 数据页
constexpr std::uint32_t kStubPage = 0x7FF80000u;  // bare 机器没有 PE ⇒ 显式给一页桩页
constexpr std::uint32_t kStubSlots = 64u;
// ★ 10,000,000 同时被 4 / 5 / 10 整除 ⇒ 各形状都能停在**整轮边界**上（不留半轮）；
//   也正好是任务要求的下限（≥1e7），再大只会让默认门禁更贵。
constexpr std::uint64_t kInsnsPerPass = 10000000ull;
constexpr int kPasses = 3;
// 附加诊断的开关（见文件头）；默认关 ⇒ 默认门禁只跑五个必需形状。
constexpr const char* kExtraEnv = "LIFT_INSN_COST_EXTRA";
// `profiled` 诊断臂的时间线：一进 run 就开窗；收窗留 ~1% 余量（见 `runAluControl`）。
constexpr const char* kInstrumentedScript =
    "at insn 0\nprofile_begin alu-instrumented\nat insn 9900000\nprofile_end alu-instrumented\n";
// `gate-probe` 诊断臂（附加诊断）：脚本里**有** `profile_*` 动作（⇒ `run()` 会 arm profiler），
// 但两个触发点都远在本次 run 的 N 之外 ⇒ 窗口**一次也不开**（armed 但 `!active`）。于是块循环里
// 只剩「闸门自己的 2 次块时钟 + `IterScope` 的 2 次迭代时钟」，没有窗口记账 ⇒ 它和默认档
// （块循环 0 次时钟）的差值 ≈ 每块 **4 次** `steady_clock::now()` ⇒ 本单关掉的那 2 次 ≈ 差值一半。
constexpr const char* kGateProbeScript =
    "at insn 20000000\nprofile_begin gate-probe\nat insn 30000000\nprofile_end gate-probe\n";

// ---- 形状编号（顺序 = 打印顺序 = 每遍的轮转顺序）----
enum ShapeId : int {
    kAlu = 0,
    kAluWide,
    kMem,
    kCall,
    kX87,
    kShimTick,
    kShimLastErr,
    kShapeCount
};

struct Arm {
    const char* name;
    const char* note;  // 每轮的构成（打印用）
    std::uint32_t ea;
    std::uint32_t prologue_insns;   // 起点到循环头之间只执行一次的条数（x87 的 `fld1`）
    std::uint64_t insns_per_iter;   // 每轮**计数**指令数（shim 调用本身不计）
    std::uint64_t blocks_per_iter;  // 每轮进几次 dispatcher 迭代（块边界）
    bool reset_x87;
    bool extra_only;  // true = 只有 `LIFT_INSN_COST_EXTRA` 打开时才跑
};

struct PassReading {
    std::uint64_t insns = 0;
    double wall_ms = 0.0;
    double ns_per_insn = 0.0;
    double ns_per_iter = 0.0;
};

// ---------------------------------------------------------------------------------------
// 机器码构造（只有我们自己写的字节；与 PE / `.lst` 无关 ⇒ pure）
// ---------------------------------------------------------------------------------------
void pushLe32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

/** `jnz rel8` 跳回**循环头**（`loop_off` = 循环头在 body 里的偏移）。`jnz_at` = 这条 jnz 的偏移。 */
void pushJnzTo(std::vector<std::uint8_t>& b, std::size_t jnz_at, std::uint32_t loop_off) {
    const std::uint32_t next = static_cast<std::uint32_t>(jnz_at + 2u);
    b.push_back(0x75);  // jnz rel8
    b.push_back(static_cast<std::uint8_t>((loop_off - next) & 0xFFu));
}

/** 8 条 `mov eax,eax` / `mov edx,edx` 交替（shim 族的填充；也用于 alu-wide 的 8 条）。 */
void pushRegMovs(std::vector<std::uint8_t>& b, int movs) {
    for (int i = 0; i < movs; ++i) {
        b.push_back(0x89);                                                  // mov r/m32, r32
        b.push_back(static_cast<std::uint8_t>((i % 2 == 0) ? 0xC0 : 0xD2));  // eax,eax / edx,edx
    }
}

/** alu 族：`movs` 条 mov + `dec ecx` + `jnz` 回起点。 */
std::vector<std::uint8_t> regMovLoop(int movs) {
    std::vector<std::uint8_t> b;
    pushRegMovs(b, movs);
    b.push_back(0x49);  // dec ecx
    pushJnzTo(b, b.size(), 0u);
    return b;
}

/** mem 族：`mov eax,[ebx]` `mov [ebx],eax` `dec ecx` `jnz` 回起点（ebx 指向已 committed 页）。 */
std::vector<std::uint8_t> memLoop() {
    std::vector<std::uint8_t> b;
    b.push_back(0x8B);  // mov r32, r/m32
    b.push_back(0x03);  // eax, [ebx]
    b.push_back(0x89);  // mov r/m32, r32
    b.push_back(0x03);  // [ebx], eax
    b.push_back(0x49);  // dec ecx
    pushJnzTo(b, b.size(), 0u);
    return b;
}

/**
 * call 族：`call leaf` `dec ecx` `jnz` 回起点，紧跟 leaf: `mov eax,eax` `ret`。
 * 布局：0: E8 rel32(→8) / 5: 49 / 6: 75 F8 / 8: 89 C0 / 10: C3 ⇒ 每轮 5 条、3 块。
 */
std::vector<std::uint8_t> callLoop() {
    std::vector<std::uint8_t> b;
    constexpr std::uint32_t kLeafOff = 8;
    b.push_back(0xE8);                       // call rel32
    pushLe32(b, kLeafOff - 5u);              // rel32 = leaf − (本条 call 的下一条)
    b.push_back(0x49);                       // dec ecx
    pushJnzTo(b, b.size(), 0u);              // 回 offset 0；此处的"下一条"= 8 = leaf 起点
    b.push_back(0x89);
    b.push_back(0xC0);  // leaf: mov eax,eax
    b.push_back(0xC3);  // leaf: ret
    return b;
}

/**
 * x87 族：入口 `fld1`（只执行一次、让 ST(1) 有定义），循环体：
 *   `fld1` `fadd st0,st1` `fstp st0` `dec ecx` `jnz` 回循环头。
 * 栈深恒 1、值恒为 1.0 / 2.0（不会溢出 / 不会长栈）⇒ 可以跑 1e7 条。
 * 布局：0: D9 E8(入口) / 2: D9 E8 / 4: D8 C1 / 6: DD D8 / 8: 49 / 9: 75 F5
 */
std::vector<std::uint8_t> x87Loop() {
    std::vector<std::uint8_t> b;
    b.push_back(0xD9);
    b.push_back(0xE8);  // 入口 fld1（prologue）
    b.push_back(0xD9);
    b.push_back(0xE8);  // fld1
    b.push_back(0xD8);
    b.push_back(0xC1);  // fadd st(0), st(1)
    b.push_back(0xDD);
    b.push_back(0xD8);  // fstp st(0)
    b.push_back(0x49);  // dec ecx
    pushJnzTo(b, b.size(), 2u);  // 回到循环头 offset 2
    return b;
}

/**
 * shim 族：8× `mov reg,reg`（填充，见文件头）+ `call stub` + `dec ecx` + `jnz` 回起点。
 * 每轮 10 条**计数**指令 + 1 次 shim 调用（shim 不计 `cpu.insns`）。
 */
std::vector<std::uint8_t> shimLoop(std::uint32_t ea, std::uint32_t stub_ea) {
    std::vector<std::uint8_t> b;
    pushRegMovs(b, 8);
    const std::uint32_t call_off = static_cast<std::uint32_t>(b.size());
    b.push_back(0xE8);                                            // call rel32
    pushLe32(b, stub_ea - (ea + call_off + 5u));                   // rel32 = stub − 下一条
    b.push_back(0x49);                                            // dec ecx
    pushJnzTo(b, b.size(), 0u);
    return b;
}

// ---------------------------------------------------------------------------------------
// 公共小工具
// ---------------------------------------------------------------------------------------
double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    return (v.size() % 2 != 0) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
}

std::uint32_t codeEaOf(int shape) {
    return kCodeEa + static_cast<std::uint32_t>(shape) * kCodeStride;
}

/**
 * 在 bare 机器上给一个**真实已实现**的 shim 发一个桩地址并装块。
 * ★ 不手写 shim 实现、也不用私有的内部符号：先用族注册函数登记（`registerTimeShims` /
 *   `registerErrorTlsShims`，都是 `shims/register.h` 的公开面），再从注册表里把**它自己的**
 *   函数指针取出来交给 `addRuntime` 换一个桩地址 —— 于是测的是真正的生产实现。
 */
std::uint32_t prepareShim(lift::Machine& m, const char* module, const char* name) {
    const lift::ShimDef* base = m.shims().find(module, name);
    if (base == nullptr || base->fn == nullptr) return 0;
    lift::ShimDef* def = m.shims().addRuntime(module, name, base->conv, base->argc, base->fn);
    if (def == nullptr || def->stub_ea == 0) return 0;
    m.cache().installShim(def->stub_ea, def->id);
    return def->stub_ea;
}

/** 跑一遍：复位起点 → 用 `max_insns` 精确停 → 量墙钟 → 用**实测**条数算 ns/条 与 ns/轮。 */
bool runPass(lift::Machine& m, const Arm& a, PassReading* out) {
    using namespace lift_test;
    if (kInsnsPerPass % a.insns_per_iter != 0) {
        std::fprintf(stderr, "insn-cost: %s 的每轮条数 %llu 不整除 N=%llu ⇒ 形状算错了\n", a.name,
                     static_cast<unsigned long long>(a.insns_per_iter),
                     static_cast<unsigned long long>(kInsnsPerPass));
        LIFT_CHECK(false);
        return false;
    }
    if (a.reset_x87) m.cpu().fpu.reset();
    m.cpu().eip = a.ea;
    m.cpu().writeReg(lift::Reg::ECX, static_cast<std::uint32_t>(kInsnsPerPass / a.insns_per_iter));
    m.cpu().writeReg(lift::Reg::EBX, kDataEa);  // mem 族用；其余形状不看它（写它无害）

    const std::uint64_t base = m.cpu().insns;
    const std::uint64_t want = base + a.prologue_insns + kInsnsPerPass;
    lift::RunOptions opt;
    opt.max_insns = want;
    const auto t0 = Clock::now();
    const lift::RunResult r = m.run(opt);
    const auto t1 = Clock::now();
    const double wall_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

    // ★ 前提判据（⛔ 不是性能阈值）：停因与条数必须就是我们请求的那一份。
    LIFT_CHECK(r.reason == lift::StopReason::MaxInsns);
    LIFT_CHECK(r.insns == want);
    if (r.reason != lift::StopReason::MaxInsns || r.insns != want) {
        std::fprintf(stderr, "insn-cost: %s 没有精确跑完（reason=%s insns=%llu 期望 %llu）\n", a.name,
                     lift::stopReasonName(r.reason), static_cast<unsigned long long>(r.insns),
                     static_cast<unsigned long long>(want));
        return false;
    }
    const std::uint64_t insns = r.insns - base;
    const std::uint64_t iters = (insns - a.prologue_insns) / a.insns_per_iter;
    if (iters == 0) {
        LIFT_CHECK(false);
        return false;
    }
    out->insns = insns;
    out->wall_ms = wall_ns / 1e6;
    out->ns_per_insn = wall_ns / static_cast<double>(insns);
    out->ns_per_iter = wall_ns / static_cast<double>(iters);
    return true;
}

/**
 * 同一份 alu 循环的一次对照跑（`profiled` / `gate-probe` 诊断臂用）。
 * `script == nullptr` ⇒ 不带时间线（默认档）；否则加载它（`profile_*` 动作会让 `run()` arm profiler）。
 */
bool runAluControl(std::uint32_t ea, const char* script, double* wall_ms, std::uint64_t* insns) {
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err) || !m.prepareStack(&err)) {
        std::fprintf(stderr, "insn-cost/profiled: 裸 Machine 初始化失败：%s\n", err.c_str());
        LIFT_CHECK(false);
        return false;
    }
    const std::vector<std::uint8_t> code = regMovLoop(2);
    if (!m.placeCode(ea, code.data(), code.size(), &err)) {
        std::fprintf(stderr, "insn-cost/profiled: placeCode 失败：%s\n", err.c_str());
        LIFT_CHECK(false);
        return false;
    }
    // ★ 触发值按 **run 起算**（`InputScript` 把相对触发平移到 origin）：`at insn 0` 一进 run 就开窗；
    //   收窗留 ~1% 余量，好让 `profile_end` 在 run 因 `max_insns` 停之前被消费（否则窗口只能靠
    //   "未闭合 ⇒ 自动收口"打印，读数没问题但会多一条告警）。
    if (script != nullptr) {
        if (!m.loadInputScriptText(script, "insn-cost/profiled", &err)) {
            std::fprintf(stderr, "insn-cost/profiled: 时间线解析失败：%s\n", err.c_str());
            LIFT_CHECK(false);
            return false;
        }
    }
    m.cpu().eip = ea;
    m.cpu().writeReg(lift::Reg::ECX, static_cast<std::uint32_t>(kInsnsPerPass / 4u));
    const std::uint64_t base = m.cpu().insns;
    lift::RunOptions opt;
    opt.max_insns = base + kInsnsPerPass;
    const auto t0 = Clock::now();
    const lift::RunResult r = m.run(opt);
    const auto t1 = Clock::now();
    LIFT_CHECK(r.reason == lift::StopReason::MaxInsns);
    LIFT_CHECK(r.insns == base + kInsnsPerPass);
    *wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    *insns = r.insns - base;
    return r.reason == lift::StopReason::MaxInsns && r.insns == base + kInsnsPerPass;
}

/** 块计时闸门守卫臂的条数：1e6 的 4 倍 ⇒ 整轮（alu 循环 4 条/轮），够采样又够快。 */
constexpr std::uint64_t kGateInsns = 4000000ull;

/**
 * 块计时闸门的守卫臂（`REQ-01M7CAJHYHWCVF3XQ7NM3X428Y`）：裸机器上跑一遍 alu 循环，返回实测条数
 * 与**所有 guest 块的 `b.ns` 合计**。`profile=false` = 关掉全部消费者（无 `--profile`、无窗口
 * profiler；逐帧 trace 的 `guest=` 读的就是同一份 `b.ns`，⛔ 不是独立判据）。
 */
bool runGateArm(std::uint32_t ea, bool profile, std::uint64_t* insns, std::uint64_t* block_ns) {
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err) || !m.prepareStack(&err)) {
        std::fprintf(stderr, "insn-cost/block-clock-gate: 裸 Machine 初始化失败：%s\n", err.c_str());
        LIFT_CHECK(false);
        return false;
    }
    const std::vector<std::uint8_t> code = regMovLoop(2);
    if (!m.placeCode(ea, code.data(), code.size(), &err)) {
        std::fprintf(stderr, "insn-cost/block-clock-gate: placeCode 失败：%s\n", err.c_str());
        LIFT_CHECK(false);
        return false;
    }
    m.cpu().eip = ea;
    m.cpu().writeReg(lift::Reg::ECX, static_cast<std::uint32_t>(kGateInsns / 4u));
    const std::uint64_t base = m.cpu().insns;
    lift::RunOptions opt;
    opt.profile = profile;
    opt.max_insns = base + kGateInsns;
    const lift::RunResult r = m.run(opt);
    LIFT_CHECK(r.reason == lift::StopReason::MaxInsns);
    LIFT_CHECK(r.insns == base + kGateInsns);
    *insns = r.insns - base;
    std::uint64_t sum = 0;
    for (const auto& kv : m.cache().guestBlocks()) sum += kv.second->ns;
    *block_ns = sum;
    return r.reason == lift::StopReason::MaxInsns && r.insns == base + kGateInsns;
}

/**
 * 设置 / 清除一个「存在即开」的开关型环境变量（守卫臂 iii 用）。
 * ★ Windows 的 `_putenv_s(name, "")` 与 POSIX 的 `unsetenv` 都真的是**移除**（`getenv` 变回 nullptr）
 *   ⇒ 不会把"关了"留成"还在但为空"的假状态。
 */
bool setEnvFlag(const char* name, bool on) {
#ifdef _WIN32
    return _putenv_s(name, on ? "1" : "") == 0;
#else
    return on ? ::setenv(name, "1", 1) == 0 : ::unsetenv(name) == 0;
#endif
}

// ★ 校准用的**不透明**迭代数 / 乘数：写成 `volatile` 全局再读进本地，编译器就折不出闭式。
//   实测教训（两次都踩了）：`acc = acc + 1` 配**任何**迭代数都会被 indvars/SCEV 折成 `acc + n`
//   （读数是 0.000 ns —— 整条链没了）。换成"乘数是运行期值"的乘加链，SCEV 给不出闭式 ⇒ 链留下，
//   而 64 位 `imul` 在 Zen3 上是 **3 周期延迟** ⇒ ns/次 × 3 就是有效周期的倒数。
volatile std::uint64_t g_cal_iters = 20000000ull;
volatile std::uint64_t g_cal_mul = 3ull;

/** 校准：`n` 次**串行依赖** `acc = acc*mul + 1` 的每次墙钟 ns（imul 3 周期 ⇒ 有效 GHz ≈ 3/ns）。 */
double measureDependentMulNs(std::uint64_t* used_n) {
    const std::uint64_t n = g_cal_iters;  // 循环外读一次：n 对优化器不透明
    const std::uint64_t mul = g_cal_mul;  // 同上：乘数不是常数 ⇒ 折不出闭式
    std::uint64_t acc = 1;
    const auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < n; ++i) acc = acc * mul + 1u;
    const auto t1 = Clock::now();
    // 防优化：把结果按位异或进一个 volatile（只写一次，不在循环里）。
    static volatile std::uint64_t sink = 0;
    sink = sink ^ acc;
    *used_n = n;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n);
}

/** 校准：`n` 次 `steady_clock::now()` 的每次墙钟 ns（样本间取最小 ⇒ 去掉被抢占的尾巴）。 */
double measureClockNs(std::uint64_t n) {
    double best = 1e18;
    for (int s = 0; s < 3; ++s) {
        std::uint64_t acc = 0;
        const auto t0 = Clock::now();
        for (std::uint64_t i = 0; i < n; ++i) {
            acc += static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
        }
        const auto t1 = Clock::now();
        static volatile std::uint64_t sink = 0;
        sink = sink ^ acc;
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() /
                                 static_cast<double>(n));
    }
    return best;
}

/** 校准：`n` 次**未竞争** `std::mutex` lock+unlock 的每次墙钟 ns（样本间取最小）。 */
double measureLockUnlockNs(std::uint64_t n) {
    double best = 1e18;
    for (int s = 0; s < 3; ++s) {
        std::mutex mu;
        const auto t0 = Clock::now();
        for (std::uint64_t i = 0; i < n; ++i) {
            mu.lock();
            mu.unlock();
        }
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() /
                                 static_cast<double>(n));
    }
    return best;
}

}  // namespace

int main() {
    using namespace lift_test;
    g_current_case = "insn-cost/setup";

    const bool extra = std::getenv(kExtraEnv) != nullptr;
    lift::Machine m;
    std::string err;
    if (!m.initBare(&err)) {
        std::fprintf(stderr, "insn-cost: initBare 失败：%s\n", err.c_str());
        return 1;
    }
    if (!m.prepareStack(&err)) {
        std::fprintf(stderr, "insn-cost: prepareStack 失败：%s\n", err.c_str());
        return 1;
    }
    // ★ 裸机器没有 PE 导入表 ⇒ 桩页与族的注册都得显式做（与 tests/dispatch.cpp 同一套做法）。
    m.shims().configureStubPage(kStubPage, kStubSlots);
    lift::registerTimeShims(m.shims());
    lift::registerErrorTlsShims(m.shims());
    const std::uint32_t stub_tick = prepareShim(m, "kernel32", "GetTickCount");
    const std::uint32_t stub_lasterr = prepareShim(m, "kernel32", "GetLastError");
    LIFT_CHECK(stub_tick != 0);
    LIFT_CHECK(stub_lasterr != 0);
    if (stub_tick == 0 || stub_lasterr == 0) {
        std::fprintf(stderr, "insn-cost: shim 桩地址没发出来（tick=%08X lasterr=%08X）\n", stub_tick,
                     stub_lasterr);
        return finish("insn-cost");
    }

    // ---- 放代码 / 数据（每个形状一个独立循环；EA 见 `codeEaOf`）----
    const std::vector<std::uint8_t> code_alu = regMovLoop(2);
    const std::vector<std::uint8_t> code_wide = regMovLoop(8);
    const std::vector<std::uint8_t> code_mem = memLoop();
    const std::vector<std::uint8_t> code_call = callLoop();
    const std::vector<std::uint8_t> code_x87 = x87Loop();
    const std::vector<std::uint8_t> code_tick = shimLoop(codeEaOf(kShimTick), stub_tick);
    const std::vector<std::uint8_t> code_lasterr = shimLoop(codeEaOf(kShimLastErr), stub_lasterr);
    const std::uint8_t data[16] = {0};
    bool placed = m.placeData(kDataEa, data, sizeof(data), &err);
    placed = m.placeCode(codeEaOf(kAlu), code_alu.data(), code_alu.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kAluWide), code_wide.data(), code_wide.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kMem), code_mem.data(), code_mem.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kCall), code_call.data(), code_call.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kX87), code_x87.data(), code_x87.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kShimTick), code_tick.data(), code_tick.size(), &err) && placed;
    placed = m.placeCode(codeEaOf(kShimLastErr), code_lasterr.data(), code_lasterr.size(), &err) && placed;
    LIFT_CHECK(placed);
    if (!placed) {
        std::fprintf(stderr, "insn-cost: 放代码 / 数据失败：%s\n", err.c_str());
        return finish("insn-cost");
    }

    const Arm arms[kShapeCount] = {
        {"alu-loop", "2×mov reg,reg + dec + jnz（1 块）", codeEaOf(kAlu), 0u, 4u, 1u, false, false},
        {"alu-wide", "8×mov reg,reg + dec + jnz（1 块；附加诊断）", codeEaOf(kAluWide), 0u, 10u, 1u, false,
         true},
        {"mem-loop", "mov eax,[ebx] + mov [ebx],eax + dec + jnz（1 块）", codeEaOf(kMem), 0u, 4u, 1u, false,
         false},
        {"call-loop", "call leaf(mov,ret) + dec + jnz（3 块）", codeEaOf(kCall), 0u, 5u, 3u, false, false},
        {"x87-loop", "fld1 + fadd st0,st1 + fstp st0 + dec + jnz（1 块）", codeEaOf(kX87), 1u, 5u, 1u, true,
         false},
        {"shim-tick", "8×mov reg,reg + call GetTickCount + dec + jnz（2 块 + 1 次 shim）",
         codeEaOf(kShimTick), 0u, 10u, 2u, false, false},
        {"shim-lasterr", "8×mov reg,reg + call GetLastError + dec + jnz（2 块 + 1 次 shim；附加诊断）",
         codeEaOf(kShimLastErr), 0u, 10u, 2u, false, true},
    };
    bool enabled[kShapeCount] = {};
    for (int s = 0; s < kShapeCount; ++s) enabled[s] = !arms[s].extra_only || extra;

    // ---- 3 遍轮转 ----
    g_current_case = "insn-cost/passes";
    std::vector<double> ns_per_insn[kShapeCount];
    std::vector<double> ns_per_iter[kShapeCount];
    std::vector<double> wall_ms[kShapeCount];
    std::uint64_t insns_seen[kShapeCount] = {};
    for (int round = 0; round < kPasses; ++round) {
        // ★ **每轮把形状顺序转一格**：实测这台机器上同一轮里"越靠后测越慢"是系统性的
        //   （笔记本降频 / 后台负载，实测 pass1 常比 pass3 快 10~20%）⇒ 固定顺序会把那个偏差
        //   整个记到"排在后面的形状"头上。转一格后每个形状在 3 轮里落到不同位置，中位对位置偏差
        //   不再单向偏。⛔ 报告里必须写清这条（它也是极差偏大的来源）。
        for (int k = 0; k < kShapeCount; ++k) {
            const int s = (k + round) % kShapeCount;
            if (!enabled[s]) continue;
            PassReading pr;
            if (!runPass(m, arms[s], &pr)) return finish("insn-cost");
            ns_per_insn[s].push_back(pr.ns_per_insn);
            ns_per_iter[s].push_back(pr.ns_per_iter);
            wall_ms[s].push_back(pr.wall_ms);
            insns_seen[s] = pr.insns;  // 每遍都相同（前提判据已断言）
        }
    }

    std::printf("\ninsn-cost: 口径 = bare Machine（自造字节）/ %d 遍轮转 / 每遍 N=%llu 条"
                "（`max_insns` 精确停，实测 `cpu.insns`）/ 块缓存热 / 无窗口 profiler /"
                " 附加诊断=%s（%s）\n",
                kPasses, static_cast<unsigned long long>(kInsnsPerPass), extra ? "开" : "关", kExtraEnv);
    std::printf("%-12s %9s %9s %9s %9s | %8s %-16s %9s %6s %7s  %s\n", "shape", "insns", "pass1_ms",
                "pass2_ms", "pass3_ms", "ns/insn", "极差[min,max]", "ns/轮", "条/轮", "块/轮", "每轮构成");
    for (int s = 0; s < kShapeCount; ++s) {
        if (!enabled[s]) continue;
        std::vector<double> sorted = ns_per_insn[s];
        std::sort(sorted.begin(), sorted.end());
        std::printf("%-12s %9llu %9.2f %9.2f %9.2f | %8.2f [%5.2f,%5.2f] %9.2f %6llu %7llu  %s\n",
                    arms[s].name, static_cast<unsigned long long>(insns_seen[s]), wall_ms[s][0],
                    wall_ms[s][1], wall_ms[s][2], medianOf(ns_per_insn[s]), sorted.front(), sorted.back(),
                    medianOf(ns_per_iter[s]), static_cast<unsigned long long>(arms[s].insns_per_iter),
                    static_cast<unsigned long long>(arms[s].blocks_per_iter), arms[s].note);
    }

    // ---- 两点拟合：t_轮 = b + c·k（alu 4 条/块、alu-wide 10 条/块）----
    //   ★ 这是**读数模型**，不是断言：b = 每块固定税、c = 每条边际成本。若两个形状的 (b, c) 拟合
    //     给不出正数，说明读数被噪声吃了 ⇒ 打印出来（⛔ 不编故事）。
    //   ★ 没有 alu-wide（附加诊断关着）时**不拟合**：缺一个点就用 0 去拟合会产出负的 c（实测过），
    //     那比不拟合更糟。
    double c_fit = 0.0;
    double b_fit = 0.0;
    const bool have_fit = enabled[kAluWide];
    if (have_fit) {
        const double t_alu = medianOf(ns_per_iter[kAlu]);
        const double t_wide = medianOf(ns_per_iter[kAluWide]);
        c_fit = (t_wide - t_alu) / 6.0;  // (10 − 4) 条
        b_fit = t_alu - c_fit * 4.0;
        std::printf("\ninsn-cost: 两点拟合（alu-loop 4 条/块 vs alu-wide 10 条/块）："
                    "t_轮 = b + c×k ⇒ b=%.2f ns/块  c=%.2f ns/条\n",
                    b_fit, c_fit);
    } else {
        std::printf("\ninsn-cost: 没跑 alu-wide（附加诊断关）⇒ 不做 b/c 两点拟合；"
                    "要 b/c 就设 %s=1\n",
                    kExtraEnv);
    }

    // ---- 残差：每种形状实测 ns/轮 − 模型（b×块 + c×条）----
    //   ★ 残差 = 该形状**特有**的那部分（访存 / call+ret / x87 的三条 / shim 的一次调用）。
    //     它不能把读写 / 分支预测 / i-cache 分开 —— 报告里必须说明这一点。
    if (have_fit) {
        std::printf("insn-cost: 相对上面模型的残差（形状特有部分；⛔ 不是「归因」，只是差额）\n");
        for (int s = 0; s < kShapeCount; ++s) {
            if (!enabled[s]) continue;
            const double measured = medianOf(ns_per_iter[s]);
            const double predicted = b_fit * static_cast<double>(arms[s].blocks_per_iter) +
                                     c_fit * static_cast<double>(arms[s].insns_per_iter);
            const double residual = measured - predicted;
            const char* unit = "";
            double per_unit = residual;
            if (s == kMem) {
                unit = "次访存";
                per_unit = residual / 2.0;
            } else if (s == kCall) {
                unit = "call+ret";
                per_unit = residual;
            } else if (s == kX87) {
                unit = "条 x87";
                per_unit = residual / 3.0;
            } else if (s == kShimTick || s == kShimLastErr) {
                unit = "次 shim 调用";
                per_unit = residual;
            }
            if (unit[0] == '\0') {
                std::printf("  %-12s 模型 %8.2f  实测 %8.2f  残差 %8.2f ns/轮\n", arms[s].name, predicted,
                            measured, residual);
            } else {
                std::printf("  %-12s 模型 %8.2f  实测 %8.2f  残差 %8.2f ns/轮  ⇒ %7.2f ns/%s\n",
                            arms[s].name, predicted, measured, residual, per_unit, unit);
            }
        }
    }

    // ---- 环境校准（只用来给上面那批读数标定机器状态；⛔ 不进判据）----
    g_current_case = "insn-cost/calibration";
    {
        constexpr std::uint64_t kClockN = 200000ull;
        constexpr std::uint64_t kLockN = 200000ull;
        std::uint64_t add_n = 0;
        const double add_ns = measureDependentMulNs(&add_n);
        const double clock_ns = measureClockNs(kClockN);
        const double lock_ns = measureLockUnlockNs(kLockN);
        LIFT_CHECK(add_ns > 0.0);  // 前提：真的量到了（⛔ 不断言数值大小）
        LIFT_CHECK(clock_ns > 0.0);
        LIFT_CHECK(lock_ns > 0.0);
        std::printf("\ninsn-cost/calibration（标定机器状态，⛔ 不是判据；%llu 次依赖 imul / %llu 次 now / "
                    "%llu 次 lock+unlock）\n"
                    "  串行依赖 imul: %.3f ns/次 ⇒ 有效 ≈ %.2f GHz（按 imul 3 周期/次；标称 3.2 GHz）\n"
                    "  steady_clock : %.2f ns/次（dispatch_tax 的历史记录 4.8~10 ns）\n"
                    "  mutex lock+unlock: %.2f ns/次（同上）\n",
                    static_cast<unsigned long long>(add_n), static_cast<unsigned long long>(kClockN),
                    static_cast<unsigned long long>(kLockN), add_ns, add_ns > 0.0 ? 3.0 / add_ns : 0.0,
                    clock_ns, lock_ns);
    }

    // ---- 守卫：块计时闸门（REQ-01M7CAJHYHWCVF3XQ7NM3X428Y）----
    //   ★ 本单的性能收益（默认档不再取两次时钟）**只能靠读数看**（同一命令的墙钟 / 两点拟合的 b），
    //     它不是断言面。这里守的是这次改动**引入的那个静默风险**：闸门与 `b->ns` 的累加条件一旦
    //     分叉，某个消费者就会拿到恒 0 的 `b->ns`（表里全是 0.000ms，不报错）。判据是**关系**：
    //     ① 关掉全部消费者 ⇒ 一轮跑完所有 guest 块的 `b.ns` 合计 **== 0**
    //        （累加被顺手改成无条件 ⇒ 当场红）；
    //     ② 打开 `--profile` ⇒ 同一段代码 `b.ns` 合计 **> 0**（闸门把累加也一起关掉 / 漏消费者 ⇒ 红）；
    //     ③ `LIFT_FRAME_TRACE=1` 单独开（没有 `--profile`）时 `b.ns` 合计**仍 == 0**
    //        —— 逐帧 trace 的 `guest=` 是 `--profile` 那一份块计时的**显示面**（README §4.3.1；
    //        改动前实测那次 run 的每一帧都是 `guest=0.0ms`）⇒ 它**不是**独立判据，别把它加进闸门
    //        （加进去就会白付两次时钟，且悄悄改掉这个口径）；
    //     ④ 三臂实测 `insns` **相等且等于请求值**（闸门不改执行语义）。
    //   ⛔ 不断言任何 ns 数值（读数进报告 / 文档，不进 test）。
    g_current_case = "insn-cost/block-clock-gate";
    {
        std::uint64_t off_insns = 0;
        std::uint64_t off_ns = 0;
        std::uint64_t on_insns = 0;
        std::uint64_t on_ns = 0;
        std::uint64_t tr_insns = 0;
        std::uint64_t tr_ns = 0;
        if (!runGateArm(codeEaOf(kAlu), false, &off_insns, &off_ns)) return finish("insn-cost");
        if (!runGateArm(codeEaOf(kAlu), true, &on_insns, &on_ns)) return finish("insn-cost");
        LIFT_CHECK(setEnvFlag("LIFT_FRAME_TRACE", true));
        const bool tr_ok = runGateArm(codeEaOf(kAlu), false, &tr_insns, &tr_ns);
        LIFT_CHECK(setEnvFlag("LIFT_FRAME_TRACE", false));
        if (!tr_ok) return finish("insn-cost");
        LIFT_CHECK(off_ns == 0);
        LIFT_CHECK(on_ns > 0);
        LIFT_CHECK(tr_ns == 0);
        LIFT_CHECK(off_insns == on_insns);
        LIFT_CHECK(off_insns == tr_insns);
        std::printf("\ninsn-cost/block-clock-gate: 无消费者 b.ns=%llu（判据 ==0）/ --profile b.ns=%llu"
                    "（判据 >0）/ LIFT_FRAME_TRACE b.ns=%llu（判据 ==0）/ insns %llu == %llu == %llu\n",
                    static_cast<unsigned long long>(off_ns), static_cast<unsigned long long>(on_ns),
                    static_cast<unsigned long long>(tr_ns), static_cast<unsigned long long>(off_insns),
                    static_cast<unsigned long long>(on_insns), static_cast<unsigned long long>(tr_insns));
    }

    // ---- 附加诊断：窗口 profiler 自身的税（同一个 alu 循环，带 / 不带 `profile_begin/end`）----
    if (extra) {
        g_current_case = "insn-cost/profiled";
        double ctrl_ms = 0.0;
        double inst_ms = 0.0;
        std::uint64_t ctrl_insns = 0;
        std::uint64_t inst_insns = 0;
        if (!runAluControl(codeEaOf(kAlu), nullptr, &ctrl_ms, &ctrl_insns)) return finish("insn-cost");
        if (!runAluControl(codeEaOf(kAlu), kInstrumentedScript, &inst_ms, &inst_insns))
            return finish("insn-cost");
        const double ctrl_ns = ctrl_ms * 1e6 / static_cast<double>(ctrl_insns);
        const double inst_ns = inst_ms * 1e6 / static_cast<double>(inst_insns);
        std::printf("\ninsn-cost/profiled: 同一个 alu 循环、同一 N=%llu\n"
                    "  无仪器            : %.2f ms ⇒ %.2f ns/条\n"
                    "  窗口 profiler armed: %.2f ms ⇒ %.2f ns/条（窗口报告见下面 `window` / `buckets` 行）\n"
                    "  仪器税            : +%.2f ns/条（= 每块 +%.2f ns；块首/块尾 2 次 + 迭代守卫 2 次 "
                    "steady_clock::now() + 记账；⛔ 闸门关时对照臂这 2 次块时钟已经不取）\n",
                    static_cast<unsigned long long>(ctrl_insns), ctrl_ms, ctrl_ns, inst_ms, inst_ns,
                    inst_ns - ctrl_ns, (inst_ns - ctrl_ns) * 4.0);

        // ---- 闸门探针：同一进程里量「块循环取不取那两次时钟」的代价（`REQ-01M7CAJHYHWCVF3XQ7NM3X428Y`）
        //   ★ 为什么要它：跨 run 的墙钟 / 两点拟合的 b 都被机器负载拖着走（本机 `steady_clock`
        //     实测 27–30 ns/次，`dispatch_tax` 的历史记录是 4.8–10 ⇒ 这台机器当时慢了 3–6 倍）。
        //     这里两条臂**同一进程、紧邻着跑、同一 N**，差值只有"时钟 + 记账"两项。
        //   ★ 闸门开的那一臂用 `armed` 但窗口**从未开**（触发点在 N 之外）⇒ 没有窗口记账，只多
        //     闸门的 2 次块时钟 + IterScope 的 2 次迭代时钟 ⇒ 差值 ≈ 每块 4 次 now()。
        //   ⛔ 只打印读数，不断言 ns 值；唯一的断言是两臂 insns 相同（读数前提）。
        g_current_case = "insn-cost/gate-probe";
        double probe_ms = 0.0;
        std::uint64_t probe_insns = 0;
        if (!runAluControl(codeEaOf(kAlu), kGateProbeScript, &probe_ms, &probe_insns))
            return finish("insn-cost");
        LIFT_CHECK(probe_insns == ctrl_insns);
        const double probe_ns = probe_ms * 1e6 / static_cast<double>(probe_insns);
        std::printf("\ninsn-cost/gate-probe: 同一个 alu 循环、同一 N、同一进程（4 条/轮、1 块/轮）\n"
                    "  闸门关（默认档：块时钟 0 次）        : %.2f ms ⇒ %.2f ns/条（%.2f ns/轮）\n"
                    "  闸门开（armed 但窗口从未开：块 2 + 迭代 2）: %.2f ms ⇒ %.2f ns/条（%.2f ns/轮）\n"
                    "  ⇒ 每块 4 次 steady_clock::now() ≈ %.2f ns ⇒ 本闸门关掉的那 2 次 ≈ %.2f ns/块"
                    "（≈ %.2f ns/条 @ 4 条/块）\n",
                    ctrl_ms, ctrl_ns, ctrl_ns * 4.0, probe_ms, probe_ns, probe_ns * 4.0,
                    (probe_ns - ctrl_ns) * 4.0, (probe_ns - ctrl_ns) * 2.0, (probe_ns - ctrl_ns) / 2.0);
    }

    return finish("insn-cost");
}
