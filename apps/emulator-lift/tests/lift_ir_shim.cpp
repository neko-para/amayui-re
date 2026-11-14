// emulator-lift · Shape A 原型阶段 B：**shim 的利用方式**用真实 guest 代码验掉
//
// 载荷：`sub_4D4B40`（0x4D4B40..0x4D4B6C，14 条 / 45 B）。它是真实引擎代码，一条 `retn 4` 的
// __thiscall，函数体里**同时**有：
//   ③ `call eax` —— 动态目标 = vtable 槽值 `[[E]+0xB4]`（本夹具钉成一条 **lifted EA** 0x436D00）
//   ④ `call dword ptr [0051D450]` —— 真 IAT 槽（本夹具写成一个 **shim 桩 EA**）
// ⇒ ③ 走"EA → lifted 表"，④ 走"IAT 槽 → 桩 EA → lift_shim"，两条路**不同**（否则 ③ 等于没测）。
//
// 判据（红了意味着什么）：
//   ① **差分 0 不一致**：≥64 组（随机 V 的其它槽 / `[esp+4]` / 栈帧位置 / E 的地址；
//      `[E+0x150]=n` **只许取 0** —— 它是 CRC 的字节数，非 0 会改执行路径；
//      `[E+0x154]` **固定 0xA5A51234** —— 它是 ③ 的 buf 实参，本单要在被调方入口**断言**它）。
//      比 8 GPR + ESP + `[E,E+0x804)` + `[V,V+0x100)` + `[esp-0x100,esp+0x40)`。
//      ⚠ 出口 EIP **不比也不断言**：lifted 臂的 `cpu.eip` 是 trampoline 收口时**赋**的
//      （`runLiftedArm` 末尾 = 0xFFFFFFFF），拿它当证据是恒真（阶段 B 复审判 P2-1 顺带）。
//      真正的证据是"循环只因 `ea == kSentinel` 才 break" + 解释器臂 `StopReason::UntilEa`。
//      flags：本函数自己的 14 条**没有一条写 flags**；但被调方 0x436D00 写，`call` 不恢复 flags
//      ⇒ 出口 flags 是**被调方留下的**。"两臂 eflags 相同"只证明 flags 穿过本函数没被改
//      ⇒ **恒等、不是证据**（断言里注明；不把它列进证伪清单）。
//   ② **控制流序列**：
//      · lifted 臂 = trampoline **自己记的 dispatch 序列**（含 shim 桩 EA、含终止哨兵）；
//      · 解释器臂 = 从 `cpu.last_eip` 环形缓冲（`kLastInsns = 64`）里**真的拼出来**的轨迹：
//        断言窗口未被截断（`last_count == insns`）、入口/`0x436D00`/`0x4D4B5D`/`0x4D4B63`
//        **各恰好 1 次**（`std::count`，不是只 `find` 首次下标）、顺序递增；桩 EA 由探针
//        `call.def->stub_ea` 给出（shim 块没有指令 ⇒ 不在环形缓冲里）；末项 = `cpu.eip`
//        （`until_ea` 命中 ⇒ 0xFFFFFFFF，是观测不是合成）。
//        ⚠ 解释器臂的序列来自**第二次跑**（独立 Fixture）——与差分那一次不是同一次运行；
//        两臂不共享 Machine 是设计复审的要求，这点显式写在这里。
//   ③ **shim 独立性**：探针把 `timeGetTime` 换成固定返回 0x5EED1234 的实现 ⇒ `[E+0xFC] == 0x5EED1234`
//      且探针每臂恰好被调 1 次；探针**顺手写 EDX = 0xABCD0001** ⇒ 两臂 EDX 都必须是它
//      （守 `lift_shim` 的**整块回读**：只回读 EAX+ESP 会让 lifted 臂 EDX 分叉）。
//      **负控**：换成"不写 call.result"的探针 ⇒ `[E+0xFC]` 必须变（证明这条断言不是恒真）。
//      ⛔ 不用真 `timeGetTime`：它的值是虚拟时钟 = 累计指令数/10000，两臂指令数不同 ⇒ 比
//      `[E+0xFC]` 就退化成"比记账"。
//   ③b **③ 的实参**：`[E+0x150]=0` / `[E+0x154]=0xA5A51234`，在两臂的**被调方入口**
//      （`0x436D00`，执行前）断言 `[esp] = 0x4D4B5D`、`[esp+4] = 0`、`[esp+8] = 0xA5A51234`
//      （解释器臂用 `until_ea = 0x436D00` 停；lifted 臂直接调 0x4D4B40 入口、看它写回的 ESP）。
//      出口栈上**看不到** buf 槽（被 0x4D4B5D 的返回地址覆盖）⇒ 必须在入口断言。
//   ④ **`--lifted-only`（in-process）**：本载荷 100% lifted ⇒ 必须在 lifted-only 下通过；
//      若哪一步要回解释器，`hardFailLiftedOnly` 会 `_Exit(9)`（与阶段 A 的负控同一个哨兵）。
//   ⑤ **写集合 ⊆ 声明区域**（阶段 A 复审 P3-2）：跑前/跑后的页快照，断言两臂**写过的地址**都落在
//      `{[esp-0x40,esp+0x40) ∪ [E,E+0x804) ∪ [0x51D450,+4)}` 内；并且断言"真的观测到了写"
//      （否则这条断言是空的）。大区域（bare 初始化提交的 128 MiB surface arena）用页哈希，不比逐字节。
//   ⑥ **没早退**：解释器臂 `cpu.insns` 增量 == 闭式 28（4D4B40 的 14 条 + 0x436D00 的 14 条；
//      n=0 走热路径的零迭代分支）；lifted 臂的解释器增量 == 0、handoff == 0。
//
// 读数（只打印，⛔ 不进断言）：
//   · 每次 trampoline 边界 ns（热路径 CRC n=4096：经 trampoline vs 直接调 `lift_cc_436D00`）；
//   · `lift_shim` 往返 ns（vs 直接 `invokeById`）；
//   · 端到端：`sub_4D4B40` 全链 lifted vs 解释器（本载荷没有循环 ⇒ 加速比会很小，这是**预期的**）；
//   · 阶段 A 的 n=4096 端到端倍数复跑一次。
//
// ★ 载荷真源（**不是手编**）：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`
//     pnpm tools disasm-at at --ea 0x4D4B40 --lines 20 --bytes 50
//     pnpm tools disasm-at at --ea 0x436D00 --lines 45 --bytes 80
//   ⚠ 0x436D00 的 80 B 与 `tests/lift_ir_proto.cpp` 的 `kBytes[kOffD00..]` 是**同一份 PE 真字节**
//     （阶段 A 已逐字节核过）；这是本阶段允许的重复，生成器阶段会消除它。
// ★ 缺 clang：`tests/ir/lift_stub.cpp` 提供 `lift_cc_436D00` 替身，本 TU 在 `#ifndef LIFT_IR_CLANG`
//   下提供 `lift_cc_4D4B40` 替身 ⇒ configure / 链接永不断；测试体第一件事是 `LIFT_SKIP(77)`。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/image.h"
#include "runtime/lift_abi.h"
#include "runtime/machine.h"
#include "runtime/x86.h"
#include "shims/register.h"

// `.ll` 导出的两个入口（缺 clang 时由替身 TU / 本文件的替身定义提供 ⇒ 链接永不断）
extern "C" std::uint32_t lift_cc_436D00(::LiftRegs*, std::uint8_t*, std::uint32_t, void*);
extern "C" std::uint32_t lift_cc_4D4B40(::LiftRegs*, std::uint8_t*, std::uint32_t, void*);

#ifndef LIFT_IR_CLANG
// 替身（没有 clang 时 `.ll` 编不出来；测试体已 SKIP 77，⛔ 不会拿它当 lifted 代码跑）
extern "C" std::uint32_t lift_cc_4D4B40(::LiftRegs*, std::uint8_t*, std::uint32_t, void*) {
    return 0xFFFFFFFFu;
}
#endif

namespace {

using Clock = std::chrono::steady_clock;

// =====================================================================================
// 常量 / 真字节
// =====================================================================================
constexpr std::uint32_t kEa4D4B40 = 0x004D4B40u;
constexpr std::uint32_t kEa4D4B5D = 0x004D4B5Du;
constexpr std::uint32_t kEa4D4B63 = 0x004D4B63u;
constexpr std::uint32_t kEaCrc = 0x00436D00u;  // ③ 的槽值：一条 lifted EA（⛔ 不是 shim 桩）
constexpr std::uint32_t kIatEa = 0x0051D450u;  // ④ 的真 IAT 槽（`call dword ptr [0051D450]`）
constexpr std::uint32_t kIatPage = 0x0051D000u;
constexpr std::uint32_t kEBase = 0x00200000u;  // 对象 E（随机化只在这些页里选）
constexpr std::uint32_t kVBase = 0x00280000u;  // vtable V
constexpr std::uint32_t kECommit = 0x3000u;    // [E,E+0x804) + 4096 B 的 CRC 输入缓冲（E+0x1000）
constexpr std::uint32_t kBufOff = 0x1000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;  // 回 trampoline 的哨兵（guest ret 到栈哨兵）
constexpr std::uint32_t kHalt = ::kLiftHalt;      // shim 报停机（running == false）
constexpr std::uint32_t kProbeValue = 0x5EED1234u;
// ★ 探针**顺手写 EDX**：这是 `lift_shim` "整块回读"的**承重守卫**。只回读 EAX+ESP 的实现会
//   让 lifted 臂的 EDX 停在旧值 ⇒ 与解释器臂分叉（阶段 B 复审 M9 是绿、M9b 补这行后红）。
constexpr std::uint32_t kProbeEdx = 0xABCD0001u;
// ★ ③ 的 buf 实参（`[E+0x154]`）**固定**：要在被调方入口断言 `[esp+8] == 0xA5A51234`
//   （阶段 B 复审 P2-3：随机化时它藏在"出口前被返回地址覆盖"的槽里，M6 全绿）。
constexpr std::uint32_t kArgBuf = 0xA5A51234u;
// 栈顶取 `GuestLayout` 的**默认**值（夹具 `initBare` 不改布局）。⛔ 桩页域不在这里复制：
//   被调方/桩 EA 的判据一律从**本机** `fx.m.layout()` 取（见 `isShimEa`；阶段 B 复审 P3-2）。
constexpr std::uint32_t kStackTop = lift::GuestLayout{}.stack_top;

// 对象内偏移（本夹具只声明这几处；其它字节随机填，用来抓"读错偏移"）
constexpr std::uint32_t kOffVptr = 0x000u;
constexpr std::uint32_t kOffN = 0x150u;    // CRC 的 arg_0 = n
constexpr std::uint32_t kOffBuf = 0x154u;  // CRC 的 arg_4 = buf（n=0 不读）
constexpr std::uint32_t kOffFlag = 0x800u; // 1 = 热路径（不建表）
constexpr std::uint32_t kOffSlot = 0xB4u;  // vtable 槽（③ 的目标）
constexpr std::uint32_t kOffOut = 0x0FCu;  // `mov [esi+0FCh],eax` 的落点

// `sub_4D4B40` 的 45 B（PE 原始字节；见文件头）
constexpr std::uint8_t kBytes4D[] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x54, 0x01, 0x00, 0x00, 0x8B, 0x06, 0x8B, 0x96, 0x50, 0x01,
    0x00, 0x00, 0x8B, 0x80, 0xB4, 0x00, 0x00, 0x00, 0x51, 0x52, 0x8B, 0xCE, 0xFF, 0xD0, 0xFF,
    0x15, 0x50, 0xD4, 0x51, 0x00, 0x89, 0x86, 0xFC, 0x00, 0x00, 0x00, 0x5E, 0xC2, 0x04, 0x00,
};

// `sub_436D00` 的 80 B（CRC-32 反射变体；与阶段 A 的 `kBytes[kOffD00..]` 同一份真字节）
constexpr std::uint8_t kBytesCrc[] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x83, 0xBE, 0x00, 0x08, 0x00, 0x00, 0x00, 0x75, 0x14, 0xE8,
    0xBC, 0xFE, 0xFF, 0xFF, 0xE8, 0x47, 0xFF, 0xFF, 0xFF, 0xC7, 0x86, 0x00, 0x08, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x8B, 0x55, 0x08, 0x83, 0xC8, 0xFF, 0x4A, 0x78, 0x1D, 0x8B, 0x4D, 0x0C, 0x53,
    0x57, 0x0F, 0xB6, 0x39, 0x0F, 0xB6, 0xD8, 0x33, 0xFB, 0xC1, 0xE8, 0x08, 0x33, 0x84, 0xBE, 0x00,
    0x04, 0x00, 0x00, 0x41, 0x4A, 0x79, 0xEA, 0x5F, 0x5B, 0xF7, 0xD0, 0x5E, 0x5D, 0xC2, 0x08, 0x00,
};
static_assert(sizeof(kBytes4D) == 45, "sub_4D4B40 必须是 45 B");
static_assert(sizeof(kBytesCrc) == 80, "sub_436D00 必须是 80 B");

// 解释器臂的闭式（**解析**值，不是从跑出来的数里抄的）：4D4B40 的 14 条 + 0x436D00 在 n=0
// 热路径上的 14 条（push ebp / mov ebp,esp / push esi / mov esi,ecx / cmp / jnz / mov edx /
// or / dec / js / not / pop esi / pop ebp / retn 8）。
constexpr std::uint64_t kInsnsInterpN0 = 14u + 14u;
constexpr std::uint64_t kDispatchBudget = 100000u;
constexpr std::uint64_t kInterpBudget = 10000000u;

// =====================================================================================
// 探针 shim（③ 独立性的唯一来源）
// =====================================================================================
struct ProbeState {
    std::uint64_t calls = 0;
    std::uint32_t ret_ea = 0;
    std::uint32_t stub_ea = 0;
};
ProbeState g_probe;

void probeCore(lift::ShimCall& call, bool write_result) {
    ++g_probe.calls;
    g_probe.ret_ea = call.ret_ea;
    g_probe.stub_ea = call.def ? call.def->stub_ea : 0u;
    // ★ 探针**故意不用真 timeGetTime**：真实现是 `cpu.clock->nowMs()` = 累计指令数/10000，
    //   两臂指令数不同 ⇒ 比 `[E+0xFC]` 就成了比记账。这里返回固定常数。
    if (write_result) call.result = kProbeValue;
    // ★ 与 `call.result` **不同**的寄存器：只有"整块回读"才能把它带回 LiftRegs（P2-2）。
    call.cpu->edx() = kProbeEdx;
}

void probeTimeGetTime(lift::X86&, lift::ShimCall& call) { probeCore(call, true); }
void probeNoResult(lift::X86&, lift::ShimCall& call) { probeCore(call, false); }

// =====================================================================================
// 分派判据（桩页域取**本机** layout；⛔ 不新写桩页字面量）
// =====================================================================================
bool isShimEa(std::uint32_t ea, const lift::GuestLayout& layout) {
    // 桩页 = [shim_page, shim_page + shim_slots*4)，4 字节步进。
    // ⚠ 任务书给的字面判据 `(ea & ~0xFF) == shim_page` 对夹具桩（0x7FFE2000 起）是**假**：
    //   桩页有 0x2000 个槽 = 32 KiB，不是 256 B。这里用完整的范围判据。
    // ★ 布局来源 = `fx.m.layout()`（阶段 B 复审 P3-2）：机器若改布局，这里跟着改；
    //   ⛔ 不再 `constexpr GuestLayout kLayout{}` 自己造一份（会与机器静默漂）。
    return ea >= layout.shim_page && ea < layout.shim_page + layout.shim_slots * 4u && (ea & 3u) == 0u;
}

lift::LiftFn lookupLifted(std::uint32_t ea) {
    switch (ea) {
        case kEa4D4B40:
        case kEa4D4B5D:
        case kEa4D4B63: return &lift_cc_4D4B40;  // 一个函数体，按 %ea 分派到 3 个入口块
        case kEaCrc: return &lift_cc_436D00;
        default: return nullptr;
    }
}

[[noreturn]] void hardFailLiftedOnly(std::uint32_t ea) {
    std::fprintf(stderr, "lifted-only: EA 0x%08X 不在 lifted 表里（也不是 shim 桩）—— 解释器会回到执行路径上\n", ea);
    std::fflush(stderr);
    std::_Exit(9);
}

const char* hostStopKindName(lift::HostStopKind k) {
    switch (k) {
        case lift::HostStopKind::None: return "None";
        case lift::HostStopKind::ResourceExhausted: return "ResourceExhausted";
        case lift::HostStopKind::Trap: return "Trap";
    }
    return "?";
}

std::uint32_t xorShift(std::uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// =====================================================================================
// 夹具配置
// =====================================================================================
struct Config {
    std::uint32_t e = 0;          // 对象地址（随机）
    std::uint32_t v = 0;          // vtable 地址（随机）
    std::uint32_t entry_esp = 0;  // 栈帧位置（随机）
    std::uint32_t arg4 = 0;       // [E+0x154]（**固定 kArgBuf**：要在被调方入口断言它）
    std::uint32_t stack_arg = 0;  // [esp+4]（随机；函数体不读）
    std::uint32_t frame_buf = 0;  // [esp+8]（只有 CRC 读数用）
};

Config makeConfig(int rep) {
    std::uint32_t s = 0x9E3779B9u ^ static_cast<std::uint32_t>(rep) * 2654435761u;
    if (s == 0) s = 0xA5A5A5A5u;
    Config c;
    c.e = kEBase + 0x1000u * (xorShift(s) % 8u);
    c.v = kVBase + 0x1000u * (xorShift(s) % 4u);
    c.entry_esp = kStackTop - 0x100u - 4u * (xorShift(s) % 16u);
    // ★ 不随机：③ 的 buf 实参必须**可观测**（阶段 B 复审 P2-3）。随机化会让
    //   `[E+0x154]` 的值与 `[esp+8]` 的断言脱钩，而出口栈上它又被返回地址覆盖。
    c.arg4 = kArgBuf;
    c.stack_arg = xorShift(s);
    c.frame_buf = kEBase + kBufOff;  // 读数的 CRC 输入缓冲固定放第一个 E 页之后
    return c;
}

void fillRandom(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t n, std::uint32_t seed) {
    std::uint32_t s = seed ? seed : 0x12345678u;
    for (std::uint32_t i = 0; i + 4u <= n; i += 4u) mem.wr32(ea + i, xorShift(s));
}

bool setupFixture(lift_fixture::Fixture& fx, const Config& c, lift::ShimFn probe_fn, std::uint32_t* stub_out,
                  std::string* err) {
    if (!fx.ok) {
        *err = fx.err.empty() ? "fixture 初始化失败" : fx.err;
        return false;
    }
    if (!fx.m.placeCode(kEa4D4B40, kBytes4D, sizeof(kBytes4D), &fx.err) ||
        !fx.m.placeCode(kEaCrc, kBytesCrc, sizeof(kBytesCrc), &fx.err)) {
        *err = fx.err;
        return false;
    }
    // E 的对象区（含 0x804 + 后面 4096 B 的 CRC 输入缓冲）
    const std::vector<std::uint8_t> e_zero(kECommit, 0);
    if (!fx.m.placeData(c.e, e_zero.data(), e_zero.size(), &fx.err)) {
        *err = fx.err;
        return false;
    }
    const std::vector<std::uint8_t> v_zero(0x100, 0);
    if (!fx.m.placeData(c.v, v_zero.data(), v_zero.size(), &fx.err)) {
        *err = fx.err;
        return false;
    }
    // IAT 所在页（**不需要 PE**：lifted 与解释器臂都只是从这个 guest 地址读一个 EA）
    const std::vector<std::uint8_t> iat_zero(0x1000, 0);
    if (!fx.m.placeData(kIatPage, iat_zero.data(), iat_zero.size(), &fx.err)) {
        *err = fx.err;
        return false;
    }

    // ④ 的桩：`use()` 分唯一桩地址 + 装 shim 块；探针替换 `fn`（**两臂共用同一个探针**）
    // ★ bare 机器没有 PE ⇒ `bind()` 不跑，shim 必须显式注册（`use()` 自己发桩 EA + installShim）。
    lift::registerTimeShims(fx.m.shims());
    const std::uint32_t stub = fx.use("winmm", "timeGetTime");
    if (stub == 0) {
        *err = fx.err.empty() ? "winmm!timeGetTime 没注册" : fx.err;
        return false;
    }
    lift::ShimDef* def = fx.m.shims().findMutable("winmm", "timeGetTime");
    if (!def) {
        *err = "findMutable(winmm,timeGetTime) 返回空";
        return false;
    }
    def->fn = probe_fn;
    // ★ 夹具桩的范围断言（阶段 B 复审 P3-2）：`fixture.h` 的 `use()` 自己发号（kStubBase 起），
    //   本身**没有**上界检查；这里用**本机 layout** 断言它落在桩页内，且整 4 字节都在域内。
    //   两边一旦漂，`isShimEa` 会把桩当成"回解释器"⇒ 静默降级成 handoff（复审 M3 就是这个形状）。
    {
        const lift::GuestLayout& layout = fx.m.layout();
        const std::uint32_t page_end = layout.shim_page + layout.shim_slots * 4u;
        LIFT_CHECK(isShimEa(stub, layout));
        LIFT_CHECK(stub + 4u <= page_end);
    }
    fx.m.mem().wr32(kIatEa, stub);

    // 随机填充 E / V（抓"读错偏移"：两臂读同一份内存，读错地址才会看出差异）
    fillRandom(fx.m.mem(), c.v, 0x100u, 0xC0FFEE11u ^ c.v);
    fx.m.mem().wr32(c.v + kOffSlot, kEaCrc);  // 槽 → 一条 **lifted EA**（⛔ 不是 shim 桩）
    fillRandom(fx.m.mem(), c.e, 0x800u, 0xDEADBEEFu ^ c.e);
    fx.m.mem().wr32(c.e + kOffVptr, c.v);
    fx.m.mem().wr32(c.e + kOffN, 0u);  // n = 0：CRC 只走零迭代路径（随机化会改执行路径）
    fx.m.mem().wr32(c.e + kOffBuf, c.arg4);
    fx.m.mem().wr32(c.e + kOffFlag, 1u);  // 热路径：不建表（否则建表器会把 [E,E+0x800) 冲掉）

    fx.m.mem().wr32(c.entry_esp, kSentinel);      // 入口 [esp] = 返回地址哨兵
    fx.m.mem().wr32(c.entry_esp + 4u, c.stack_arg);
    fx.m.mem().wr32(c.entry_esp + 8u, c.frame_buf);
    *stub_out = stub;
    return true;
}

void setEntryState(lift_fixture::Fixture& fx, const Config& c) {
    lift::X86& cpu = fx.m.cpu();
    cpu.r[0] = 0xAAAAAAAAu;  // EAX
    cpu.r[1] = c.e;          // ECX = this
    cpu.r[2] = 0xBBBBBBBBu;  // EDX
    cpu.r[3] = 0x11111111u;  // EBX
    cpu.r[4] = c.entry_esp;  // ESP
    cpu.r[5] = 0x22222222u;  // EBP
    cpu.r[6] = 0x33333333u;  // ESI（`push esi` 保存的就是它）
    cpu.r[7] = 0x44444444u;  // EDI
    cpu.eflags = 0x202u;     // 本函数不写 flags ⇒ 出口应原样带回
    cpu.eip = kEa4D4B40;
}

// =====================================================================================
// 快照 / 比较
// =====================================================================================
struct Snap {
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t eip = 0;
    std::vector<std::uint8_t> e, v, stack;
};

std::vector<std::uint8_t> bytesAt(lift_fixture::Fixture& fx, std::uint32_t ea, std::size_t n) {
    const std::uint8_t* p = fx.m.mem().host(ea);
    return std::vector<std::uint8_t>(p, p + n);
}

Snap takeSnap(lift_fixture::Fixture& fx, const Config& c) {
    Snap s;
    for (int i = 0; i < 8; ++i) s.r[i] = fx.m.cpu().r[i];
    s.eflags = fx.m.cpu().eflags;
    s.eip = fx.m.cpu().eip;
    s.e = bytesAt(fx, c.e, 0x804u);
    s.v = bytesAt(fx, c.v, 0x100u);
    s.stack = bytesAt(fx, c.entry_esp - 0x100u, 0x140u);  // [esp-0x100, esp+0x40)
    return s;
}

void checkU32(const char* tag, const char* what, std::uint32_t actual, std::uint32_t expected) {
    ++lift_test::g_checks;
    if (actual != expected)
        lift_test::failf(__FILE__, __LINE__, "[%s] %s: actual=0x%08X expected=0x%08X", tag, what, actual, expected);
}

void checkRegion(const char* tag, const char* what, const std::vector<std::uint8_t>& a,
                 const std::vector<std::uint8_t>& b) {
    ++lift_test::g_checks;
    if (a == b) return;
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    lift_test::failf(__FILE__, __LINE__, "[%s] region %s: first diff @+0x%zX (sizes %zu/%zu)", tag, what, i,
                     a.size(), b.size());
}

std::uint32_t rdLe32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint32_t>(v[off]) | (static_cast<std::uint32_t>(v[off + 1]) << 8) |
           (static_cast<std::uint32_t>(v[off + 2]) << 16) | (static_cast<std::uint32_t>(v[off + 3]) << 24);
}

// =====================================================================================
// 一臂：统计 / 结果
// =====================================================================================
struct RunStats {
    std::uint64_t dispatches = 0;
    std::uint64_t lifted_calls = 0;
    std::uint64_t shim_dispatches = 0;
    std::uint64_t handoffs = 0;
    std::uint64_t insns = 0;  // 本臂解释器执行了多少条（lifted 臂应为 0）
    std::vector<std::uint32_t> seq;
    bool halted = false;
    std::string halt_detail;
};

struct ArmResult {
    Snap snap;
    RunStats stats;
    std::uint64_t probe_calls = 0;
    std::uint32_t probe_ret_ea = 0;
    std::uint32_t probe_stub_ea = 0;
    std::uint64_t wall_ns = 0;
    std::uint32_t stub_ea = 0;
    bool ok = false;
    std::string err;
};

void compareArms(const ArmResult& interp, const ArmResult& lifted, const Config& c, const char* tag) {
    static const char* kNames[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    for (int i = 0; i < 8; ++i) checkU32(tag, kNames[i], lifted.snap.r[i], interp.snap.r[i]);
    // ★ 本函数自己的 14 条**不写 flags**（mov/push/pop/call/retn）——但被调方 0x436D00 写，
    //   `call` 不恢复 flags ⇒ 出口 flags 是**被调方留下的**。两臂相等只证明"flags 穿过本函数没被改"，
    //   是**恒等、不是独立证据**（复审 §2.4 的口径）。
    checkU32(tag, "EFLAGS lifted==interp", lifted.snap.eflags, interp.snap.eflags);
    // ⛔ 不比 EIP：lifted 臂的 `cpu.eip` 是 `runLiftedArm` 收口时**赋**成 0xFFFFFFFF 的，
    //   与它比是恒真（阶段 B 复审判 P2-1 顺带）。真正的证据是"循环只因 `ea == kSentinel` 才 break"
    //   （`runLiftedArm` 的 while 条件）+ 解释器臂的 `StopReason::UntilEa`（`runInterpArm`）。
    checkRegion(tag, "[E,E+0x804)", lifted.snap.e, interp.snap.e);
    checkRegion(tag, "[V,V+0x100)", lifted.snap.v, interp.snap.v);
    checkRegion(tag, "[esp-0x100,esp+0x40)", lifted.snap.stack, interp.snap.stack);
    (void)c;
}

bool runInterpArm(lift_fixture::Fixture& fx, RunStats* st, std::string* err) {
    const std::uint64_t before = fx.m.cpu().insns;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = before + kInterpBudget;
    const lift::RunResult r = fx.m.run(o);
    st->insns = fx.m.cpu().insns - before;
    if (r.reason != lift::StopReason::UntilEa) {
        *err = std::string("解释器臂意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
        return false;
    }
    return true;
}

// lifted 臂的 trampoline（**本单新增的一层**：lifted 表 → shim 桩 → 混合模式）
bool runLiftedArm(lift_fixture::Fixture& fx, bool lifted_only, RunStats* st, std::string* err) {
    lift::X86& cpu = fx.m.cpu();
    LiftRegs regs;
    lift::liftCpuToRegs(cpu, regs);
    std::uint8_t* mem = fx.m.mem().host(0);
    const std::uint64_t insns0 = cpu.insns;
    std::uint32_t ea = kEa4D4B40;
    st->seq.push_back(ea);
    while (true) {
        if (ea == kSentinel) break;
        if (ea == kHalt) {  // shim 报停机：⛔ 不许静默当成"跑完了"
            st->halted = true;
            st->halt_detail = std::string("stop_kind=") + hostStopKindName(cpu.stop_kind) + " stop_why=" +
                              (cpu.stop_why ? cpu.stop_why : "(null)");
            lift::liftRegsToCpu(regs, cpu);
            // ⛔ 不覆写 `cpu.eip`（阶段 B 复审 P3-3）：解释器路径停机后 eip 留在**出事的那条 shim EA**
            //   （`shims.cpp` 的 trap 不改 eip；dispatcher 命中 shim 块时 eip 就是这个桩 EA）。
            //   把诊断面改成 0xFFFFFFFE 会让两路停机现场不一致。
            *err = "lift_shim 报停机（" + st->halt_detail + "）";
            return false;
        }
        if (st->dispatches >= kDispatchBudget) {
            *err = "dispatch 预算耗尽（lifted 侧可能死循环）";
            return false;
        }
        ++st->dispatches;
        if (lift::LiftFn f = lookupLifted(ea)) {
            ++st->lifted_calls;
            ea = f(&regs, mem, ea, &cpu);
        } else if (isShimEa(ea, fx.m.layout())) {
            ++st->shim_dispatches;
            ea = ::lift_shim(ea, &regs, &cpu);
        } else if (lifted_only) {
            hardFailLiftedOnly(ea);
        } else {
            // 混合模式：解释器从 ea 跑到 [esp]（本载荷正常情况下**不该**走到这里）
            ++st->handoffs;
            lift::liftRegsToCpu(regs, cpu);
            cpu.eip = ea;
            lift::RunOptions o;
            o.until_ea = fx.m.mem().rd32(regs.r[4]);
            o.max_insns = cpu.insns + kInterpBudget;
            const lift::RunResult r = fx.m.run(o);
            if (r.reason != lift::StopReason::UntilEa) {
                *err = std::string("混合模式解释器意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
                return false;
            }
            lift::liftCpuToRegs(cpu, regs);
            ea = cpu.eip;
        }
        st->seq.push_back(ea);
    }
    lift::liftRegsToCpu(regs, cpu);
    // ★ 收口状态 = 终止哨兵；**不参与断言**（与它比是恒真，见 `compareArms` 的注释）。
    cpu.eip = kSentinel;
    st->insns = cpu.insns - insns0;
    return true;
}

ArmResult runArm(bool lifted, const Config& c, bool probe_writes, bool lifted_only = false) {
    ArmResult out;
    lift_fixture::Fixture fx;
    if (!fx.ok) {
        out.err = fx.err.empty() ? "fixture 初始化失败" : fx.err;
        return out;
    }
    std::uint32_t stub = 0;
    if (!setupFixture(fx, c, probe_writes ? &probeTimeGetTime : &probeNoResult, &stub, &out.err)) return out;
    setEntryState(fx, c);
    g_probe = ProbeState{};
    const auto t0 = Clock::now();
    const bool ran = lifted ? runLiftedArm(fx, lifted_only, &out.stats, &out.err)
                            : runInterpArm(fx, &out.stats, &out.err);
    const auto t1 = Clock::now();
    out.wall_ns =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    out.snap = takeSnap(fx, c);
    out.probe_calls = g_probe.calls;
    out.probe_ret_ea = g_probe.ret_ea;
    out.probe_stub_ea = g_probe.stub_ea;
    out.stub_ea = stub;
    out.ok = ran;
    return out;
}

// =====================================================================================
// 控制流序列
// =====================================================================================
std::vector<std::uint32_t> expectedSequence(std::uint32_t stub) {
    return {kEa4D4B40, kEaCrc, kEa4D4B5D, stub, kEa4D4B63, kSentinel};
}

void checkSequence(const std::vector<std::uint32_t>& seq, std::uint32_t stub, const char* tag, const char* arm,
                   bool verbose) {
    const std::vector<std::uint32_t> want = expectedSequence(stub);
    ++lift_test::g_checks;
    if (seq != want) {
        std::string got;
        for (std::uint32_t v : seq) {
            char b[16];
            std::snprintf(b, sizeof(b), "%08X ", v);
            got += b;
        }
        lift_test::failf(__FILE__, __LINE__, "[%s] %s 控制流序列不符：got=[%s]", tag, arm, got.c_str());
    } else if (verbose) {
        std::printf("[lift-ir-shim] %s %s 控制流: ", tag, arm);
        for (std::uint32_t v : seq) std::printf("%08X → ", v);
        std::printf("(stop)\n");
    }
    // 0x436D00 只出现一次、桩 EA 只出现一次（堵"没走到 ③④"与重复派发）
    const auto countOf = [&seq](std::uint32_t v) {
        return static_cast<std::uint32_t>(std::count(seq.begin(), seq.end(), v));
    };
    checkU32(tag, std::string(std::string(arm) + " 0x436D00 次数").c_str(), countOf(kEaCrc), 1u);
    checkU32(tag, std::string(std::string(arm) + " 桩 EA 次数").c_str(), countOf(stub), 1u);
}

// 解释器臂：没有可插的 dispatch 钩子（runtime 不在本单的改动面）⇒ 从 `cpu.last_eip` 环形缓冲
// **真的拼**出执行轨迹，再用这条轨迹构造序列（⛔ 不再 `*out = expectedSequence(...)` 合成——
// 阶段 B 复审判 P2-1：那样 240 个 check 恒真、打印行是伪造的）。
//   · **窗口占用**：缓冲 `kLastInsns = 64`，本臂 28 条 ⇒ 断言 `last_count == insns`（截断即红）；
//   · 起点 = 轨迹第一条，必须是 0x4D4B40；
//   · `0x4D4B40` / `0x436D00` / `0x4D4B5D` / `0x4D4B63` 各**恰好 1 次**（`std::count`，不是只
//     `find` 首次下标），且顺序 436D00 < 4D4B5D < 4D4B63；
//   · 桩 EA：探针被调 1 次，`call.def->stub_ea` == 夹具桩、`call.ret_ea` == 0x4D4B63
//     （0x4D4B63 这个返回地址**只有** 0x4D4B5D 那条 `call ds:[51D450]` 才会压栈）；
//   · 末项 = `cpu.eip`（`until_ea` 命中 ⇒ 0xFFFFFFFF，是观测）；
//   · ⚠ 这是**第二次跑**（独立 Fixture，与差分不是同一次运行）。臂间不共享 Machine 是设计复审
//     的要求；序列证据与差分证据**来自两次运行**，不要把两者当同一次观测。
bool interpSequence(lift_fixture::Fixture& fx, const ArmResult& arm, std::vector<std::uint32_t>* out,
                    std::string* why) {
    const lift::X86& cpu = fx.m.cpu();
    const int n = cpu.last_count;
    // ★ 窗口完整性：轨迹必须覆盖本臂执行过的**全部**指令（28 条）。
    //   kLastInsns=24 时前 4 条已滚出窗口（含入口自己）——本单把它提到 64 并在这里断言；
    //   ⛔ 以后若再缩窗口，这条先红，而不是"三个目标 EA 恰好在窗口里"的侥幸。
    if (static_cast<std::uint64_t>(n) != arm.stats.insns) {
        *why = "last_eip 只留下 " + std::to_string(n) + " 条，本臂执行了 " +
               std::to_string(arm.stats.insns) + " 条 ⇒ 轨迹被截断";
        return false;
    }
    std::vector<std::uint32_t> ring;
    ring.reserve(static_cast<std::size_t>(n));
    const int start = (cpu.last_head - n + lift::X86::kLastInsns) % lift::X86::kLastInsns;
    for (int i = 0; i < n; ++i) ring.push_back(cpu.last_eip[(start + i) % lift::X86::kLastInsns]);
    if (ring.empty() || ring.front() != kEa4D4B40) {
        *why = "轨迹的第一条不是 0x4D4B40（入口）";
        return false;
    }
    const auto idx = [&ring](std::uint32_t v) {
        const auto it = std::find(ring.begin(), ring.end(), v);
        return (it == ring.end()) ? -1 : static_cast<int>(it - ring.begin());
    };
    const auto cnt = [&ring](std::uint32_t v) {
        return static_cast<std::uint32_t>(std::count(ring.begin(), ring.end(), v));
    };
    const int i_crc = idx(kEaCrc);
    const int i_5d = idx(kEa4D4B5D);
    const int i_63 = idx(kEa4D4B63);
    if (i_crc < 0 || i_5d < 0 || i_63 < 0) {
        *why = "last_eip 轨迹里缺 0x436D00 / 0x4D4B5D / 0x4D4B63 之一";
        return false;
    }
    if (cnt(kEa4D4B40) != 1u || cnt(kEaCrc) != 1u || cnt(kEa4D4B5D) != 1u || cnt(kEa4D4B63) != 1u) {
        *why = "轨迹里 4D4B40/436D00/4D4B5D/4D4B63 不是各恰好 1 次";
        return false;
    }
    if (!(i_crc < i_5d && i_5d < i_63)) {
        *why = "轨迹里的顺序不是 436D00 < 4D4B5D < 4D4B63";
        return false;
    }
    if (arm.probe_calls != 1u || arm.probe_stub_ea != arm.stub_ea || arm.probe_ret_ea != kEa4D4B63) {
        *why = "探针观测与 0x4D4B5D 的 call 不符（calls/stub/ret_ea）";
        return false;
    }
    if (cpu.eip != kSentinel) {
        *why = "解释器臂的停机 eip 不是哨兵（until_ea 没命中）";
        return false;
    }
    // ★ 序列的每一项都是**观测**：前 5 项来自轨迹/探针，末项来自停机 eip。
    out->clear();
    out->push_back(ring.front());      // 入口（轨迹第一条）
    out->push_back(ring[i_crc]);       // ③ 的槽目标
    out->push_back(ring[i_5d]);        // ③ 的返回点
    out->push_back(arm.probe_stub_ea); // ④ 的桩（shim 块没有指令 ⇒ 只能由探针给）
    out->push_back(ring[i_63]);        // ④ 的返回点
    out->push_back(cpu.eip);           // 停机 EA
    return true;
}

// =====================================================================================
// 写集合审计（P3-2）
// =====================================================================================
constexpr std::uint32_t kSmallRegionLimit = 4u * 1024u * 1024u;  // > 4 MiB 用页哈希（不比逐字节）

struct SmallSnap {
    std::uint32_t begin = 0;
    std::vector<std::uint8_t> bytes;
};
struct BigSnap {
    std::uint32_t begin = 0;
    std::uint32_t size = 0;
    std::vector<std::uint64_t> page_hash;
};
struct MemAudit {
    std::vector<SmallSnap> small;
    std::vector<BigSnap> big;
};
static_assert(sizeof(std::uint64_t) == 8, "页哈希按 64 位块读");

std::uint64_t hashPage(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    std::size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

MemAudit takeAudit(lift::Machine& m) {
    MemAudit a;
    // ★ 只快照**已提交**的页：`regions()` 里还有纯诊断用的未提交区间（`prepareStack` 的
    //   stack-guard 页就是），读它会在 VEH 里变成一次假 fault。按"最大连续已提交段"切，
    //   于是同一 region 里的洞不会被整段跳过（不会因为一个 guard 页丢掉整个 region）。
    for (const lift::Region& r : m.mem().regions()) {
        std::uint32_t cur = r.begin;
        while (cur < r.end) {
            if (!m.mem().committed(cur, 0x1000u)) {
                cur += 0x1000u;
                continue;
            }
            std::uint32_t run_end = cur;
            while (run_end < r.end && m.mem().committed(run_end, 0x1000u)) run_end += 0x1000u;
            const std::uint32_t n = run_end - cur;
            const std::uint8_t* p = m.mem().host(cur);
            if (n <= kSmallRegionLimit) {
                SmallSnap s;
                s.begin = cur;
                s.bytes.assign(p, p + n);
                a.small.push_back(std::move(s));
            } else {
                BigSnap b;
                b.begin = cur;
                b.size = n;
                for (std::uint32_t off = 0; off < n; off += 0x1000u) b.page_hash.push_back(hashPage(p + off, 0x1000u));
                a.big.push_back(std::move(b));
            }
            cur = run_end;
        }
    }
    return a;
}

bool writeAllowed(std::uint32_t ea, const Config& c) {
    const std::uint32_t lo = c.entry_esp - 0x40u;
    const std::uint32_t hi = c.entry_esp + 0x40u;
    if (ea >= lo && ea + 4u <= hi) return true;
    if (ea >= c.e && ea + 4u <= c.e + 0x804u) return true;
    if (ea >= kIatEa && ea + 4u <= kIatEa + 4u) return true;
    return false;
}

std::uint64_t checkWrites(const MemAudit& before, const MemAudit& after, const Config& c, const char* tag) {
    std::uint64_t changed = 0;
    ++lift_test::g_checks;
    if (before.small.size() != after.small.size() || before.big.size() != after.big.size()) {
        lift_test::failf(__FILE__, __LINE__, "[%s] 区域数在跑前/跑后不同", tag);
        return changed;
    }
    for (std::size_t ri = 0; ri < before.small.size(); ++ri) {
        const SmallSnap& a = before.small[ri];
        const SmallSnap& b = after.small[ri];
        ++lift_test::g_checks;
        if (a.begin != b.begin || a.bytes.size() != b.bytes.size()) {
            lift_test::failf(__FILE__, __LINE__, "[%s] 小区域 #%zu 形状变了", tag, ri);
            continue;
        }
        for (std::size_t off = 0; off + 4u <= a.bytes.size(); off += 4u) {
            std::uint32_t va = 0, vb = 0;
            std::memcpy(&va, a.bytes.data() + off, 4);
            std::memcpy(&vb, b.bytes.data() + off, 4);
            if (va == vb) continue;
            ++changed;
            const std::uint32_t ea = a.begin + static_cast<std::uint32_t>(off);
            if (!writeAllowed(ea, c))
                lift_test::failf(__FILE__, __LINE__, "[%s] 越界写：0x%08X（旧 0x%08X → 新 0x%08X）", tag, ea, va, vb);
        }
    }
    for (std::size_t ri = 0; ri < before.big.size(); ++ri) {
        const BigSnap& a = before.big[ri];
        const BigSnap& b = after.big[ri];
        ++lift_test::g_checks;
        if (a.begin != b.begin || a.size != b.size || a.page_hash.size() != b.page_hash.size()) {
            lift_test::failf(__FILE__, __LINE__, "[%s] 大区域 #%zu 形状变了", tag, ri);
            continue;
        }
        for (std::size_t pi = 0; pi < a.page_hash.size(); ++pi) {
            if (a.page_hash[pi] == b.page_hash[pi]) continue;
            lift_test::failf(__FILE__, __LINE__, "[%s] 大区域 0x%08X 的页 +0x%zX 被写了（越界）", tag, a.begin, pi * 0x1000u);
        }
    }
    return changed;
}

// =====================================================================================
// 读数夹具
// =====================================================================================
LiftRegs crcEntryRegs(const Config& c) {
    LiftRegs st{};
    st.r[0] = 0xDEADBEEFu;  // EAX
    st.r[1] = c.e;          // ECX = this
    st.r[2] = 0x12345678u;  // EDX
    st.r[3] = 0x11111111u;  // EBX
    st.r[4] = c.entry_esp;  // ESP
    st.r[5] = 0x22222222u;  // EBP
    st.r[6] = 0x33333333u;  // ESI
    st.r[7] = 0x44444444u;  // EDI
    st.eflags = 0x202u;
    return st;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::uint64_t elapsedNs(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// =====================================================================================
// main
// =====================================================================================
int runMain() {
    using namespace lift_test;
    const char* build =
#ifdef LIFT_IR_BUILD_TYPE
        LIFT_IR_BUILD_TYPE;
#else
        "(unknown)";
#endif
    std::printf("[lift-ir-shim] build=%s\n", build);

    // ---- 1) 差分 + 控制流 + shim 独立性（同一批组）----------------------------------------
    constexpr int kGroups = 80;  // 判据要求 ≥64
    std::uint64_t groups = 0, mismatches = 0;
    for (int rep = 0; rep < kGroups; ++rep) {
        const Config c = makeConfig(rep);
        char name[64];
        std::snprintf(name, sizeof(name), "diff-r%d", rep);
        g_current_case = name;
        const ArmResult in = runArm(false, c, true);
        const ArmResult li = runArm(true, c, true);
        ++g_checks;
        if (!in.ok || !li.ok) {
            failf(__FILE__, __LINE__, "[%s] 臂失败：interp=%s lifted=%s", name, in.ok ? "ok" : in.err.c_str(),
                  li.ok ? "ok" : li.err.c_str());
            continue;
        }
        ++groups;

        // ① 逐位差分
        const int before_failures = g_failures;
        compareArms(in, li, c, name);
        if (g_failures != before_failures) ++mismatches;

        // ③ shim 独立性：探针被调 1 次 + 返回值真的进了 guest 内存
        checkU32(name, "probe calls (interp)", static_cast<std::uint32_t>(in.probe_calls), 1u);
        checkU32(name, "probe calls (lifted)", static_cast<std::uint32_t>(li.probe_calls), 1u);
        checkU32(name, "probe ret_ea (interp)", in.probe_ret_ea, kEa4D4B63);
        checkU32(name, "probe ret_ea (lifted)", li.probe_ret_ea, kEa4D4B63);
        checkU32(name, "[E+0xFC] (interp)", rdLe32(in.snap.e, kOffOut), kProbeValue);
        checkU32(name, "[E+0xFC] (lifted)", rdLe32(li.snap.e, kOffOut), kProbeValue);
        // ③b 整块回读：探针写了 EDX（与 call.result 不同的寄存器）⇒ 两臂都必须看到它。
        //     只回读 EAX+ESP 的 lift_shim 会让 lifted 臂的 EDX 停在旧值（复审 M9 即此形态）。
        checkU32(name, "EDX==probe (interp)", in.snap.r[2], kProbeEdx);
        checkU32(name, "EDX==probe (lifted)", li.snap.r[2], kProbeEdx);

        // ⑥ 没早退：解释器闭式 + lifted 臂零回退
        checkU32(name, "interp insns", static_cast<std::uint32_t>(in.stats.insns),
                 static_cast<std::uint32_t>(kInsnsInterpN0));
        checkU32(name, "lifted insns(解释器部分)", static_cast<std::uint32_t>(li.stats.insns), 0u);
        checkU32(name, "lifted handoffs", static_cast<std::uint32_t>(li.stats.handoffs), 0u);

        // ② 控制流序列（lifted 臂：trampoline 自己记的；解释器臂：直接观测拼出来的同一条）
        checkSequence(li.stats.seq, li.stub_ea, name, "lifted", rep == 0);
        {
            lift_fixture::Fixture fx;
            std::uint32_t stub = 0;
            std::string err;
            std::vector<std::uint32_t> seq;
            std::string why;
            // 只为拿 last_eip 环形缓冲，重跑一次解释器臂（臂间不共享 Machine）
            if (fx.ok && setupFixture(fx, c, &probeTimeGetTime, &stub, &err)) {
                setEntryState(fx, c);
                g_probe = ProbeState{};
                RunStats st;
                if (!runInterpArm(fx, &st, &err)) {
                    ++g_checks;
                    failf(__FILE__, __LINE__, "[%s] 解释器臂（序列观测）失败：%s", name, err.c_str());
                } else {
                    ArmResult arm;
                    arm.stats = st;  // 轨迹完整性要 `stats.insns`
                    arm.probe_calls = g_probe.calls;
                    arm.probe_ret_ea = g_probe.ret_ea;
                    arm.probe_stub_ea = g_probe.stub_ea;
                    arm.stub_ea = stub;
                    checkU32(name, "interp insns(序列)", static_cast<std::uint32_t>(st.insns),
                             static_cast<std::uint32_t>(kInsnsInterpN0));
                    ++g_checks;
                    if (!interpSequence(fx, arm, &seq, &why)) {
                        failf(__FILE__, __LINE__, "[%s] 解释器臂序列观测失败：%s", name, why.c_str());
                    } else {
                        checkSequence(seq, stub, name, "interp", rep == 0);
                    }
                }
            } else {
                ++g_checks;
                failf(__FILE__, __LINE__, "[%s] 序列观测夹具失败：%s", name, err.c_str());
            }
        }
    }
    std::printf("[lift-ir-shim] diff: groups=%llu mismatches=%llu (n=0；随机化 V 槽/[esp+4]/栈帧/E 地址；"
                "[E+0x154] 固定 0x%08X 用于③实参断言)\n",
                static_cast<unsigned long long>(groups), static_cast<unsigned long long>(mismatches), kArgBuf);
    LIFT_CHECK(groups >= 64u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(mismatches), 0u);

    // ---- 2) 探针负控：不写 call.result ⇒ [E+0xFC] 必须变（证明"== 0x5EED1234"不是恒真）-------
    {
        g_current_case = "probe/negctl-no-result";
        const Config c = makeConfig(4242);
        const ArmResult li = runArm(true, c, false);  // 不写 result 的探针
        LIFT_CHECK(li.ok);
        if (li.ok) {
            const std::uint32_t got = rdLe32(li.snap.e, kOffOut);
            // ⛔ 只留 "!= kProbeValue"：再断言 `== 0` 就锁住了框架 `call.result` 的默认值
            //    （机制常量，改默认值会红得没意义；复审 §8 的口径）。
            LIFT_CHECK_NE_U32(got, kProbeValue);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.probe_calls), 1u);  // shim 仍被调到
            std::printf("[lift-ir-shim] probe negctl: [E+0xFC]=0x%08X（期望 != 0x%08X；探针 calls=%llu）\n", got,
                        kProbeValue, static_cast<unsigned long long>(li.probe_calls));
        }
    }

    // ---- 3) --lifted-only（本载荷 100% lifted）：必须通过，且零 handoff -----------------------
    {
        g_current_case = "lifted-only/payload";
        const Config c = makeConfig(7);
        const ArmResult li = runArm(true, c, true, /*lifted_only=*/true);
        LIFT_CHECK(li.ok);
        if (li.ok) {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.handoffs), 0u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.stats.shim_dispatches), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(li.probe_calls), 1u);
            std::printf("[lift-ir-shim] lifted-only: OK（dispatches=%llu handoffs=0 shim=%llu）\n",
                        static_cast<unsigned long long>(li.stats.dispatches),
                        static_cast<unsigned long long>(li.stats.shim_dispatches));
        } else {
            std::printf("[lift-ir-shim] lifted-only: 失败 — %s\n", li.err.c_str());
        }
    }

    // ---- 3b) lift_shim 的**失败面**：桩 EA 不在注册表 ⇒ 响亮停机 + trampoline 报 stop_kind ----
    //   ⛔ 堵"把一次响亮停机当成跑完了"：`lift_shim` 返回 kLiftHalt（0xFFFFFFFE，**不是** 0xFFFFFFFF），
    //   trampoline 必须在 `kHalt` 分支停下并把 `cpu.stop_kind` / `stop_why` 报出来。
    {
        g_current_case = "lift_shim/unknown-stub";
        const Config c = makeConfig(2027);
        lift_fixture::Fixture fx;
        std::string err;
        std::uint32_t stub = 0;
        if (fx.ok && setupFixture(fx, c, &probeTimeGetTime, &stub, &err)) {
            // 桩页里一个**没发出去**的 EA：`isShimEa` 为真、块缓存/注册表里都没有
            const lift::GuestLayout& layout = fx.m.layout();
            const std::uint32_t bogus = layout.shim_page + 0x700u * 4u;  // 0x7FFE1C00
            LIFT_CHECK(isShimEa(bogus, layout));
            LIFT_CHECK(fx.m.cache().peek(bogus) == nullptr);
            fx.m.mem().wr32(kIatEa, bogus);
            setEntryState(fx, c);
            g_probe = ProbeState{};
            RunStats st;
            const bool ok = runLiftedArm(fx, false, &st, &err);
            LIFT_CHECK(!ok);                  // ⛔ 不许被当成正常结束
            LIFT_CHECK(st.halted);            // 走的是 kHalt 分支
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(st.shim_dispatches), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(st.handoffs), 0u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g_probe.calls), 0u);  // 没调到任何 shim
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fx.m.cpu().running), 0u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fx.m.cpu().stop_kind),
                              static_cast<std::uint32_t>(lift::HostStopKind::Trap));
            LIFT_CHECK(err.find("stop_kind=Trap") != std::string::npos);
            // ★ P3-3：停机分支**不覆写** eip ⇒ 这里应停在**出事的那条 shim EA**（与解释器路径同口径）。
            LIFT_CHECK_EQ_U32(fx.m.cpu().eip, bogus);
            // 序列末项必须是**停机哨兵**（⛔ 不是 0xFFFFFFFF）：4D4B40 → 436D00 → 4D4B5D → bogus → kHalt
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.seq.size()), 5);
            if (st.seq.size() == 5) {
                LIFT_CHECK_EQ_U32(st.seq[3], bogus);
                LIFT_CHECK_EQ_U32(st.seq[4], kHalt);
                LIFT_CHECK(isShimEa(st.seq[4], layout) == false);
            }
            std::printf("[lift-ir-shim] lift_shim 失败面: halted=%d err=%s\n", st.halted ? 1 : 0, err.c_str());
        } else {
            ++g_checks;
            failf(__FILE__, __LINE__, "[unknown-stub] 夹具失败：%s", err.c_str());
        }
    }

    // ---- 3c) ③ `call eax` 的**实参**：被调方入口 [esp]/[esp+4]/[esp+8] ---------------------
    //   阶段 B 复审 P2-3：n=0 且 CRC 返回值被 ④ 的 EAX 覆盖 ⇒ ③ 的实参在**出口**不可见
    //   （buf 槽在 0x4D4B5D 被返回地址覆盖）。⇒ 必须停在**被调方入口**看。
    //   解释器臂：`until_ea = 0x436D00`（执行前停）；lifted 臂：直接调 0x4D4B40 入口、看它写回的 ESP。
    {
        g_current_case = "args/call-eax";
        const Config c = makeConfig(555);
        // (i) 解释器臂
        {
            lift_fixture::Fixture fx;
            std::string err;
            std::uint32_t stub = 0;
            if (!fx.ok || !setupFixture(fx, c, &probeTimeGetTime, &stub, &err)) {
                ++g_checks;
                failf(__FILE__, __LINE__, "[args/interp] 夹具失败：%s", err.c_str());
            } else {
                setEntryState(fx, c);
                const std::uint64_t insns_before = fx.m.cpu().insns;
                lift::RunOptions o;
                o.until_ea = kEaCrc;  // 被调方入口，**执行前**停
                o.max_insns = insns_before + kInterpBudget;
                const lift::RunResult r = fx.m.run(o);
                LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
                LIFT_CHECK_EQ_U32(fx.m.cpu().eip, kEaCrc);
                LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fx.m.cpu().insns - insns_before), 10);  // 进 ③ 前的 10 条
                const std::uint32_t esp = fx.m.cpu().esp();
                LIFT_CHECK_EQ_U32(esp, c.entry_esp - 16u);  // push esi + push ecx + push edx + call 返回址
                LIFT_CHECK_MEM_U32(fx.m.mem(), esp, kEa4D4B5D);       // ③ 的返回地址
                LIFT_CHECK_MEM_U32(fx.m.mem(), esp + 4u, 0u);          // arg0 = n = [E+0x150]
                LIFT_CHECK_MEM_U32(fx.m.mem(), esp + 8u, kArgBuf);     // arg1 = buf = [E+0x154]
                LIFT_CHECK_EQ_U32(fx.m.cpu().ecx(), c.e);              // thiscall：ECX = this
                std::printf("[lift-ir-shim] args/interp: esp=%08X [esp]=%08X [esp+4]=%08X [esp+8]=%08X\n", esp,
                            fx.m.mem().rd32(esp), fx.m.mem().rd32(esp + 4u), fx.m.mem().rd32(esp + 8u));
            }
        }
        // (ii) lifted 臂（直接调 resume 入口 0x4D4B40；返回 = ③ 的目标，ESP = 被调方入口）
        {
            lift_fixture::Fixture fx;
            std::string err;
            std::uint32_t stub = 0;
            if (!fx.ok || !setupFixture(fx, c, &probeTimeGetTime, &stub, &err)) {
                ++g_checks;
                failf(__FILE__, __LINE__, "[args/lifted] 夹具失败：%s", err.c_str());
            } else {
                setEntryState(fx, c);
                LiftRegs st;
                lift::liftCpuToRegs(fx.m.cpu(), st);
                std::uint8_t* mem = fx.m.mem().host(0);
                const std::uint32_t next = lift_cc_4D4B40(&st, mem, kEa4D4B40, &fx.m.cpu());
                LIFT_CHECK_EQ_U32(next, kEaCrc);  // ③ 的目标（槽值）
                LIFT_CHECK_EQ_U32(st.r[4], c.entry_esp - 16u);
                LIFT_CHECK_MEM_U32(fx.m.mem(), st.r[4], kEa4D4B5D);
                LIFT_CHECK_MEM_U32(fx.m.mem(), st.r[4] + 4u, 0u);
                LIFT_CHECK_MEM_U32(fx.m.mem(), st.r[4] + 8u, kArgBuf);
                LIFT_CHECK_EQ_U32(st.r[1], c.e);
                std::printf("[lift-ir-shim] args/lifted: esp=%08X [esp]=%08X [esp+4]=%08X [esp+8]=%08X\n", st.r[4],
                            fx.m.mem().rd32(st.r[4]), fx.m.mem().rd32(st.r[4] + 4u),
                            fx.m.mem().rd32(st.r[4] + 8u));
            }
        }
    }

    // ---- 4) 写集合 ⊆ 声明区域（P3-2）--------------------------------------------------------
    {
        const Config c = makeConfig(99);
        for (int arm = 0; arm < 2; ++arm) {
            const bool lifted = arm == 1;
            const char* tag = lifted ? "write-set/lifted" : "write-set/interp";
            g_current_case = tag;
            lift_fixture::Fixture fx;
            std::string err;
            std::uint32_t stub = 0;
            if (!fx.ok || !setupFixture(fx, c, &probeTimeGetTime, &stub, &err)) {
                ++g_checks;
                failf(__FILE__, __LINE__, "[%s] 夹具失败：%s", tag, err.c_str());
                continue;
            }
            setEntryState(fx, c);
            g_probe = ProbeState{};
            const MemAudit before = takeAudit(fx.m);
            RunStats st;
            const bool ran = lifted ? runLiftedArm(fx, false, &st, &err) : runInterpArm(fx, &st, &err);
            const MemAudit after = takeAudit(fx.m);
            ++g_checks;
            if (!ran) {
                failf(__FILE__, __LINE__, "[%s] 臂失败：%s", tag, err.c_str());
                continue;
            }
            const std::uint64_t changed = checkWrites(before, after, c, tag);
            LIFT_CHECK(changed > 0u);  // 非空：真的观测到了写（否则"写集合 ⊆ 区域"是恒真）
            std::printf("[lift-ir-shim] %s: 观测到 %llu 个变化的 dword，全部落在声明区域内\n", tag,
                        static_cast<unsigned long long>(changed));
        }
    }

    // ---- 5) 读数（⛔ 不进断言）--------------------------------------------------------------
    {
        // (a) 每次 trampoline 边界 ns：热路径 CRC n=4096，① 经 trampoline，② 直接调 lift_cc_436D00
        Config rc;
        rc.e = kEBase;
        rc.v = kVBase;
        rc.entry_esp = kStackTop - 0x100u;
        rc.arg4 = kEBase + kBufOff;
        rc.stack_arg = 4096u;
        rc.frame_buf = kEBase + kBufOff;
        lift_fixture::Fixture fx;
        std::string err;
        std::uint32_t stub = 0;
        if (fx.ok && setupFixture(fx, rc, &probeTimeGetTime, &stub, &err)) {
            lift::X86& cpu = fx.m.cpu();
            std::uint8_t* mem = fx.m.mem().host(0);
            lift::LiftFn direct_fn = &lift_cc_436D00;
            constexpr std::uint64_t kN = 300;
            std::uint64_t sink = 0;
            std::vector<double> tramp_ns, sym_ns, ptr_ns;
            // 预热：块解码 / 页触达 / icache 都算在计时之外。
            for (int w = 0; w < 3; ++w) {
                volatile std::uint32_t start = kEaCrc;
                LiftRegs st = crcEntryRegs(rc);
                std::uint32_t cur = start;
                while (cur != kSentinel) {
                    lift::LiftFn f = lookupLifted(cur);
                    if (!f) break;
                    cur = f(&st, mem, cur, &cpu);
                }
                LiftRegs st2 = crcEntryRegs(rc);
                sink += lift_cc_436D00(&st2, mem, start, &cpu);
                LiftRegs st3 = crcEntryRegs(rc);
                sink += direct_fn(&st3, mem, start, &cpu);
            }
            for (int rep = 0; rep < 5; ++rep) {
                volatile std::uint32_t start = kEaCrc;
                const auto t0 = Clock::now();
                for (std::uint64_t k = 0; k < kN; ++k) {
                    LiftRegs st = crcEntryRegs(rc);
                    std::uint32_t cur = start;
                    while (cur != kSentinel) {
                        lift::LiftFn f = lookupLifted(cur);
                        if (!f) break;
                        cur = f(&st, mem, cur, &cpu);
                    }
                    sink += cur;
                }
                const auto t1 = Clock::now();
                tramp_ns.push_back(static_cast<double>(elapsedNs(t0, t1)) / static_cast<double>(kN));
                const auto t2 = Clock::now();
                for (std::uint64_t k = 0; k < kN; ++k) {
                    LiftRegs st = crcEntryRegs(rc);
                    sink += lift_cc_436D00(&st, mem, start, &cpu);  // 任务书的"直接调"
                }
                const auto t3 = Clock::now();
                sym_ns.push_back(static_cast<double>(elapsedNs(t2, t3)) / static_cast<double>(kN));
                const auto t4 = Clock::now();
                for (std::uint64_t k = 0; k < kN; ++k) {
                    LiftRegs st = crcEntryRegs(rc);
                    sink += direct_fn(&st, mem, start, &cpu);  // 同一条路，但走**函数指针**
                }
                const auto t5 = Clock::now();
                ptr_ns.push_back(static_cast<double>(elapsedNs(t4, t5)) / static_cast<double>(kN));
            }
            const double t_tramp = median(tramp_ns);
            const double t_sym = median(sym_ns);
            const double t_ptr = median(ptr_ns);
            std::printf("[lift-ir-shim] 边界读数（n=4096 热路径 CRC，%llu 次，5 遍中位）:\n",
                        static_cast<unsigned long long>(kN));
            std::printf("  经 trampoline=%.1f ns · 直接调(符号)=%.1f ns · 直接调(函数指针)=%.1f ns\n", t_tramp, t_sym,
                        t_ptr);
            std::printf("  ⇒ 每次边界（经表查 + while + 间接调用）=%.1f ns；其中间接调用本身≈%.1f ns（sink=%llu）\n",
                        t_tramp - t_ptr, t_ptr - t_sym, static_cast<unsigned long long>(sink));

            // (a2) 短载荷控制：0x4D4B63 入口只做 1 次 note_write + 1 次 store + pop/retn。
            //      上面每次调用 7.4 µs ⇒ 1–2 ns 的边界成本落在噪声底以下；这里把被调方缩到最短，
            //      让"经表查 + while + 间接调用"这一层能被测出来（同一台 fixture，不参与任何断言）。
            {
                const std::uint32_t frame63 = kStackTop - 0x400u;
                fx.m.mem().wr32(frame63, 0x33333333u);  // pop esi 读回的值
                fx.m.mem().wr32(frame63 + 4u, kSentinel);
                const auto entry63 = [&](LiftRegs& st) {
                    st = LiftRegs{};
                    st.r[0] = 0x12345678u;
                    st.r[1] = 0xBBBBBBBBu;
                    st.r[2] = 0xCCCCCCCCu;
                    st.r[3] = 0x11111111u;
                    st.r[4] = frame63;
                    st.r[5] = 0x22222222u;
                    st.r[6] = rc.e;
                    st.r[7] = 0x44444444u;
                    st.eflags = 0x202u;
                };
                constexpr std::uint64_t kN2 = 20000;
                lift::LiftFn direct4d = &lift_cc_4D4B40;
                std::vector<double> t_tramp2, t_ptr2;
                for (int rep = 0; rep < 9; ++rep) {
                    {
                        volatile std::uint32_t start = kEa4D4B63;
                        LiftRegs st;
                        const auto a = Clock::now();
                        for (std::uint64_t k = 0; k < kN2; ++k) {
                            entry63(st);
                            std::uint32_t cur = start;
                            while (cur != kSentinel) {
                                lift::LiftFn f = lookupLifted(cur);
                                if (!f) break;
                                cur = f(&st, mem, cur, &cpu);
                            }
                        }
                        const auto b = Clock::now();
                        t_tramp2.push_back(static_cast<double>(elapsedNs(a, b)) / static_cast<double>(kN2));
                    }
                    {
                        volatile std::uint32_t start = kEa4D4B63;
                        LiftRegs st;
                        const auto a = Clock::now();
                        for (std::uint64_t k = 0; k < kN2; ++k) {
                            entry63(st);
                            (void)direct4d(&st, mem, start, &cpu);
                        }
                        const auto b = Clock::now();
                        t_ptr2.push_back(static_cast<double>(elapsedNs(a, b)) / static_cast<double>(kN2));
                    }
                }
                const double b_tramp = *std::min_element(t_tramp2.begin(), t_tramp2.end());
                const double b_direct = *std::min_element(t_ptr2.begin(), t_ptr2.end());
                std::printf("  短载荷控制（0x4D4B63 入口，%llu 次，9 遍取最小）: trampoline=%.1f ns 直接=%.1f ns "
                            "⇒ 每次边界=%.1f ns\n",
                            static_cast<unsigned long long>(kN2), b_tramp, b_direct, b_tramp - b_direct);
            }

            // (d) 阶段 A 的 n=4096 端到端倍数复跑（解释器 vs lifted 一次函数调用）
            std::vector<double> interp_ns, lifted_ns;
            // 预热一遍（同一台 fixture，块缓存/页触达都先热起来）
            {
                lift::X86& c0 = fx.m.cpu();
                c0.r[1] = rc.e;
                c0.r[4] = rc.entry_esp;
                c0.eflags = 0x202u;
                c0.eip = kEaCrc;
                lift::RunOptions o0;
                o0.until_ea = kSentinel;
                o0.max_insns = c0.insns + kInterpBudget;
                (void)fx.m.run(o0);
            }
            for (int rep = 0; rep < 3; ++rep) {
                lift::X86& c2 = fx.m.cpu();
                c2.r[0] = 0xDEADBEEFu;
                c2.r[1] = rc.e;
                c2.r[2] = 0x12345678u;
                c2.r[3] = 0x11111111u;
                c2.r[4] = rc.entry_esp;
                c2.r[5] = 0x22222222u;
                c2.r[6] = 0x33333333u;
                c2.r[7] = 0x44444444u;
                c2.eflags = 0x202u;
                c2.eip = kEaCrc;
                const std::uint64_t before = c2.insns;
                lift::RunOptions o;
                o.until_ea = kSentinel;
                o.max_insns = before + kInterpBudget;
                const auto t0 = Clock::now();
                const lift::RunResult r = fx.m.run(o);
                const auto t1 = Clock::now();
                if (r.reason == lift::StopReason::UntilEa) interp_ns.push_back(static_cast<double>(elapsedNs(t0, t1)));
                LiftRegs st = crcEntryRegs(rc);
                const auto t2 = Clock::now();
                const std::uint32_t cur = lift_cc_436D00(&st, mem, kEaCrc, &cpu);
                const auto t3 = Clock::now();
                (void)cur;
                lifted_ns.push_back(static_cast<double>(elapsedNs(t2, t3)));
            }
            const double ti = median(interp_ns);
            const double tl = median(lifted_ns);
            std::printf("[lift-ir-shim] 阶段 A e2e 复跑（0x436D00, n=4096）：interp=%.0f ns lifted=%.0f ns ratio=%.2fx\n",
                        ti, tl, (tl > 0.0) ? ti / tl : 0.0);
        } else {
            std::printf("[lift-ir-shim] 读数夹具失败：%s\n", err.c_str());
        }

        // (b) lift_shim 往返 ns：lift_shim(桩, st, cpu) vs 直接 invokeById(id, cpu)
        {
            Config c;
            c.e = kEBase;
            c.v = kVBase;
            c.entry_esp = kStackTop - 0x200u;
            c.arg4 = 0;
            c.stack_arg = 0;
            c.frame_buf = 0;
            lift_fixture::Fixture fx2;
            std::string err2;
            std::uint32_t stub2 = 0;
            if (fx2.ok && setupFixture(fx2, c, &probeTimeGetTime, &stub2, &err2)) {
                lift::ShimDef* def = fx2.m.shims().findMutable("winmm", "timeGetTime");
                const std::uint32_t id = def ? def->id : 0u;
                const std::uint32_t frame = c.entry_esp;
                fx2.m.mem().wr32(frame, kEa4D4B63);  // 一个合法返回地址（invoke 只读它）
                constexpr std::uint64_t kN = 100000;
                std::vector<double> via_lift_shim, via_invoke;
                for (int rep = 0; rep < 3; ++rep) {
                    LiftRegs st{};
                    const auto t0 = Clock::now();
                    for (std::uint64_t k = 0; k < kN; ++k) {
                        st = LiftRegs{};
                        st.r[0] = 0xAAAAAAAAu;
                        st.r[1] = 0xBBBBBBBBu;
                        st.r[2] = 0xCCCCCCCCu;
                        st.r[3] = 0x11111111u;
                        st.r[4] = frame;
                        st.r[5] = 0x22222222u;
                        st.r[6] = 0x33333333u;
                        st.r[7] = 0x44444444u;
                        st.eflags = 0x202u;
                        (void)::lift_shim(stub2, &st, &fx2.m.cpu());
                    }
                    const auto t1 = Clock::now();
                    via_lift_shim.push_back(static_cast<double>(elapsedNs(t0, t1)) / static_cast<double>(kN));
                    const auto t2 = Clock::now();
                    for (std::uint64_t k = 0; k < kN; ++k) {
                        lift::X86& cpu = fx2.m.cpu();
                        cpu.r[0] = 0xAAAAAAAAu;
                        cpu.r[1] = 0xBBBBBBBBu;
                        cpu.r[2] = 0xCCCCCCCCu;
                        cpu.r[3] = 0x11111111u;
                        cpu.r[4] = frame;
                        cpu.r[5] = 0x22222222u;
                        cpu.r[6] = 0x33333333u;
                        cpu.r[7] = 0x44444444u;
                        cpu.eflags = 0x202u;
                        cpu.eip = stub2;
                        (void)fx2.m.shims().invokeById(cpu, id);
                    }
                    const auto t3 = Clock::now();
                    via_invoke.push_back(static_cast<double>(elapsedNs(t2, t3)) / static_cast<double>(kN));
                }
                std::printf("[lift-ir-shim] lift_shim 往返（%llu 次，3 遍中位）: lift_shim=%.1f ns 直接 invokeById=%.1f ns "
                            "⇒ 差值=%.1f ns（物化+回读+id 反查+分派）\n",
                            static_cast<unsigned long long>(kN), median(via_lift_shim), median(via_invoke),
                            median(via_lift_shim) - median(via_invoke));
            } else {
                std::printf("[lift-ir-shim] lift_shim 读数夹具失败：%s\n", err2.c_str());
            }
        }

        // (c) 端到端：sub_4D4B40 全链 lifted vs 解释器（本载荷没有循环 ⇒ 倍数很小，预期的）
        {
            const Config c = makeConfig(13);
            lift_fixture::Fixture fx3;
            std::string err3;
            std::uint32_t stub3 = 0;
            if (fx3.ok && setupFixture(fx3, c, &probeTimeGetTime, &stub3, &err3)) {
                // 预热两种臂（同一台 fixture ⇒ 块缓存 / 页触达先热起来；⛔ 不把 setup 算进读数）
                for (int w = 0; w < 2; ++w) {
                    setEntryState(fx3, c);
                    RunStats st;
                    std::string e;
                    (void)runInterpArm(fx3, &st, &e);
                    setEntryState(fx3, c);
                    RunStats st2;
                    (void)runLiftedArm(fx3, false, &st2, &e);
                }
                std::vector<double> interp_ns, lifted_ns, empty_ns;
                for (int rep = 0; rep < 3; ++rep) {
                    setEntryState(fx3, c);
                    RunStats st;
                    std::string e;
                    const auto t0 = Clock::now();
                    const bool ok_i = runInterpArm(fx3, &st, &e);
                    const auto t1 = Clock::now();
                    if (ok_i) interp_ns.push_back(static_cast<double>(elapsedNs(t0, t1)));
                    setEntryState(fx3, c);
                    RunStats st2;
                    const auto t2 = Clock::now();
                    const bool ok_l = runLiftedArm(fx3, false, &st2, &e);
                    const auto t3 = Clock::now();
                    if (ok_l) lifted_ns.push_back(static_cast<double>(elapsedNs(t2, t3)));
                    // 空跑基线：`Machine::run` 的固定开销（0 条 guest 指令，起停都同一处 until_ea）
                    setEntryState(fx3, c);
                    fx3.m.cpu().eip = kEa4D4B40;
                    lift::RunOptions o;
                    o.until_ea = kEa4D4B40;
                    o.max_insns = fx3.m.cpu().insns + kInterpBudget;
                    const std::uint64_t insns_before_empty = fx3.m.cpu().insns;
                    const auto t4 = Clock::now();
                    const lift::RunResult rr = fx3.m.run(o);
                    const auto t5 = Clock::now();
                    if (rr.reason == lift::StopReason::UntilEa) {
                        empty_ns.push_back(static_cast<double>(elapsedNs(t4, t5)));
                        // 基线的可信度：这条 run **真的**是 0 条指令
                        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fx3.m.cpu().insns - insns_before_empty), 0);
                    }
                }
                const double ti = median(interp_ns);
                const double tl = median(lifted_ns);
                const double te = median(empty_ns);
                std::printf(
                    "[lift-ir-shim] 4D4B40 e2e（n=0，没有循环 ⇒ 倍数本该很小）：interp=%.0f ns lifted=%.0f ns "
                    "ratio=%.2fx\n",
                    ti, tl, (tl > 0.0) ? ti / tl : 0.0);
                std::printf("  其中 `Machine::run` 空跑固定开销=%.0f ns ⇒ 只算 28 条 guest 执行：interp=%.0f ns "
                            "ratio=%.2fx\n",
                            te, (ti > te) ? ti - te : 0.0, (tl > 0.0) ? ((ti > te) ? ti - te : 0.0) / tl : 0.0);
            } else {
                std::printf("[lift-ir-shim] 4D4B40 e2e 夹具失败：%s\n", err3.c_str());
            }
        }
    }

    return lift_test::finish("lift-ir-shim");
}

}  // namespace

int main() {
#ifdef LIFT_IR_CLANG
    return runMain();
#else
    (void)&runMain;  // 缺 clang 时 runMain 不会被调用（避免"未引用内部函数"告警）
    LIFT_SKIP("缺 clang（未定义 LIFT_IR_CLANG）：`tests/ir/lift_4D4B40.ll` 未编，链的是替身");
#endif
}
