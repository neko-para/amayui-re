// emulator-lift · S7-IAT：`call *<IAT 槽>` 的**清理字节**（机制本身）+ `ret_cleanup` 沿尾跳壳传递
//
// ★ 判据（红了意味着什么）：`REQ-01M86NMT0YJ9V78XS2DZZV364Q` 的两件机制里，
//   ① **清理字节 = 表里的值**（`(conv==Stdcall) ? argc*4 : 0`）—— 表缺槽 / 寄存器间接 /
//      不是槽的绝对地址 / `modelled=false` 都必须**仍然降级**（⛔ 不许兜底成 0 装绿）；
//   ② **尾跳壳传递**（`ret_cleanup(C) = cleanup(owner(T))`，C 无 `ret` 且全部出口是同一条
//      跨分量 `jmp → T`）—— 少了它，`004B60C0` / `004B6130` 这类"静态 call 一个 jmp 壳"的单元
//      永远算成 `动态返回`；
//   ③ **发射形状不变**：同一分量的 IR 体在"可结构化 / 不可结构化"下**逐字节相同**
//      （`emitRet` 两条路同一条 IR ⇒ 判据只改"谁进 structured"，不改发射规则）。
//
// ★ 两半判据，各自独立成红：
//   · **生成期**（`LIFT_S7_IAT_CRITERIA` 存在时）：`tests/ir/s7_iat_roots.txt` 的 4 个根
//     （A=0x418F80 / F=0x4B8660 / G=0x485F90 / G'=0x47CC80）那趟 `lift-gen` 的
//     `[S7 criteria]` / `[S7 criteria/site accounts]` **逐字**（结构量）与两条 `[S7 downgrade]`。
//     ★ 这一半是**模式相关**的：期望行按 `LIFT_S7_IAT_GEN` 选（= 生成那份 `.ll` 的开关值）。
//   · **合成夹具**（9 个 case，⛔ 不读 PE / 不读 `.lst` / 不用 clang）：直接调
//     `analyzeStructure` / `emitModule`。★ 本 exe **强制** `LIFT_S7_IAT=1` 编译
//     （见 CMakeLists 的 target 定义）⇒ 这 9 个 case 在**任何配置**下都钉"机制打开时"的语义。
//
// ★ 为什么这些断言不能靠"运行期差分 / 与 Machine 逐位相同"：D3 之后那档错**只影响派发密度**
//   （`docs/03-emulator/aot-s7-evaluation.md` §5 的原话）⇒ 差分与五项指纹在放宽前后都绿。
//   ⇒ 红只能来自"判据的结论"（本文件）与"生成期读数"。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aot/lift_gen_emit.h"
#include "aot/lift_iat.h"
#include "aot/lift_struct.h"
#include "check.h"
#include "runtime/insn.h"

// ★ 本夹具钉的是"机制打开时"的语义 ⇒ 必须带上开关（⛔ 不要删这条 #error：
//   没有它，这个 exe 在 OFF 配置下会静默测"旧行为"，而旧行为本来就绿 ⇒ 假绿）。
#if !defined(LIFT_S7_IAT) || !LIFT_S7_IAT
#error "lift-s7-iat 必须以 LIFT_S7_IAT=1 编译（它钉的是机制打开时的公式与传递）"
#endif

// 生成期期望值（`tests/ir/s7_iat_roots.txt` 的 4 个根；结构量 ⇒ 与机器 / 编译器无关）。
// `LIFT_S7_IAT_GEN` = 生成那份 `criteria.txt` 的 `lift-gen` 用过的开关值（CMake 传进来）。
#ifndef LIFT_S7_IAT_GEN
#define LIFT_S7_IAT_GEN 0
#endif

namespace {

using lift::Cond;
using lift::Conv;
using lift::Insn;
using lift::Op;
using lift::OpKind;
using lift::Reg;
using lift::aot::GenBlock;
using lift::aot::GenClosureStats;
using lift::aot::GenComponent;
using lift::aot::GenEmitStats;
using lift::aot::GenStructure;
using lift::aot::GenUnsupported;
using lift::aot::IatModel;
using lift::aot::IatRow;
using lift::aot::StructResult;

// 根集（与 `tests/ir/s7_iat_roots.txt` 一一对应）
constexpr std::uint32_t kA = 0x00418F80u;   // 调用者：静态 call F，一臂 jmp 0x004B7D20
constexpr std::uint32_t kF = 0x004B8660u;   // `call *0x51D3C4`（USER32!SetCursor，stdcall 1）+ `ret`
constexpr std::uint32_t kG = 0x00485F90u;   // 形态 A 的运行期槽目标：`xor eax,eax / retn 4`
constexpr std::uint32_t kGp = 0x0047CC80u;  // 形态 B 的运行期槽目标：`xor eax,eax / retn 8`

// 合成夹具的 EA（⛔ 与任何真实 guest 代码无关）
constexpr std::uint32_t kFA = 0x90000000u;    // 合成 F：push eax; call <op0>; ret
constexpr std::uint32_t kFA2 = 0x90100000u;   // 合成 A：call 合成 F（只用于 case 7 / 9）
constexpr std::uint32_t kSlot = 0x0051D180u;  // 合成槽（表里"有"的那一槽）
constexpr std::uint32_t kSlotMissing = 0x0051DA0Cu;  // 表里没有的绝对地址
constexpr std::uint32_t kShimBegin = 0x7FFE0000u;
constexpr std::uint32_t kShimEnd = 0x7FFE8000u;

// ---- 合成指令（与 `tests/emit_edges.cpp` 的 mkInsn 同一套写法）----
Insn mkInsn(std::uint32_t ea, std::uint8_t len, Op op) {
    Insn in{};
    in.ea = ea;
    in.length = len;
    in.op = op;
    in.mnemonic = "synthetic";
    return in;
}

std::uint32_t rel32(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    const std::int64_t rel = static_cast<std::int64_t>(target) -
                             (static_cast<std::int64_t>(ea) + static_cast<std::int64_t>(len));
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(rel));
}

Insn mkPushReg(std::uint32_t ea, std::uint8_t len, Reg r) {
    Insn in = mkInsn(ea, len, Op::Push);
    in.ops[0].kind = OpKind::Reg;
    in.ops[0].size = 4;
    in.ops[0].reg = r;
    return in;
}

// `call [abs]`：绝对 IAT 槽形态（`decode.cpp` 的 `absolute` = base/index 都空 + 只有 disp）
Insn mkCallAbs(std::uint32_t ea, std::uint8_t len, std::uint32_t slot) {
    Insn in = mkInsn(ea, len, Op::Call);
    in.ops[0].kind = OpKind::Mem;
    in.ops[0].size = 4;
    in.ops[0].mem.absolute = true;
    in.ops[0].mem.base = Reg::None;
    in.ops[0].mem.index = Reg::None;
    in.ops[0].mem.size = 4;
    in.ops[0].mem.disp = slot;
    return in;
}

// `call *%reg`：寄存器间接（目标运行期才知道）
Insn mkCallReg(std::uint32_t ea, std::uint8_t len, Reg r) {
    Insn in = mkInsn(ea, len, Op::Call);
    in.ops[0].kind = OpKind::Reg;
    in.ops[0].size = 4;
    in.ops[0].reg = r;
    return in;
}

Insn mkCallRel(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    Insn in = mkInsn(ea, len, Op::Call);
    in.ops[0].kind = OpKind::Imm;
    in.ops[0].size = 4;
    in.ops[0].imm = rel32(ea, len, target);
    return in;
}

Insn mkJmpRel(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    Insn in = mkInsn(ea, len, Op::Jmp);
    in.ops[0].kind = OpKind::Imm;
    in.ops[0].size = 4;
    in.ops[0].imm = rel32(ea, len, target);
    return in;
}

// `ret imm`（stdcall 被调方）/ `ret`（cdecl 口径）
Insn mkRetImm(std::uint32_t ea, std::uint16_t imm) {
    Insn in = mkInsn(ea, 3, Op::Ret);
    in.ops[0].kind = OpKind::Imm;
    in.ops[0].size = 2;
    in.ops[0].imm = imm;
    return in;
}

Insn mkRet(std::uint32_t ea) { return mkInsn(ea, 1, Op::Ret); }

GenBlock blockWith(std::uint32_t ea, std::vector<Insn> insns) {
    GenBlock b;
    b.ea = ea;
    b.terminated = true;
    b.insns = std::move(insns);
    return b;
}

// 一个分量 = root + 一组块（entries = root ∪ call 的 resume；与 `lift-gen.cpp:3895-3898` 同一条式子）
GenComponent mkComp(std::uint32_t root, std::vector<GenBlock> blocks) {
    GenComponent c;
    c.root = root;
    c.entries = {root};
    for (GenBlock& b : blocks) {
        for (const Insn& in : b.insns) {
            if (in.op == Op::Call && in.op0().kind == OpKind::Imm) {
                c.entries.push_back(in.ea + in.length);
            }
        }
        c.blocks.emplace(b.ea, std::move(b));
    }
    return c;
}

// `F: { push eax; <call> }` + `{ ret }`（design §5.4 case 1 的合成形态）
GenComponent mkPushCallRetComponent(std::uint32_t root, const Insn& call) {
    return mkComp(root, {blockWith(root, {mkPushReg(root, 1, Reg::EAX), call}),
                         blockWith(static_cast<std::uint32_t>(call.ea + call.length),
                                   {mkRet(static_cast<std::uint32_t>(call.ea + call.length))})});
}

IatRow mkRow(std::uint32_t slot, Conv conv, std::int32_t argc, bool modelled) {
    IatRow row;
    row.slot_ea = slot;
    row.conv = conv;
    row.argc = argc;
    row.cleanup = (conv == Conv::Stdcall) ? static_cast<std::uint32_t>(argc) * 4u : 0u;
    row.modelled = modelled;
    return row;
}

StructResult analyze(const std::vector<GenComponent>& comps, const IatModel* iat) {
    std::map<std::uint32_t, std::uint32_t> entry_owner;
    for (const GenComponent& c : comps) {
        for (const std::uint32_t ea : c.entries) entry_owner.emplace(ea, c.root);
    }
    const std::set<std::uint32_t> dropped;
    return lift::aot::analyzeStructure(comps, entry_owner, dropped, kShimBegin, kShimEnd, iat);
}

std::string emit(const std::vector<GenComponent>& comps, const StructResult& res, bool* ok) {
    std::set<std::uint32_t> entry_set;
    for (const GenComponent& c : comps) entry_set.insert(c.entries.begin(), c.entries.end());
    const std::set<std::uint32_t> dropped;
    GenStructure structure;
    structure.enabled = true;
    structure.structured_roots = res.structured;
    structure.shim_begin = kShimBegin;
    structure.shim_end = kShimEnd;
    std::string ir;
    GenEmitStats stats;
    std::vector<GenUnsupported> unsupported;
    *ok = lift::aot::emitModule(comps, entry_set, dropped, GenClosureStats{}, structure, &ir, &stats,
                                &unsupported);
    if (!unsupported.empty()) *ok = false;
    return ir;
}

// `i32c` 的同一份口径（按 int32 打印；`lift_gen_emit.cpp` 的 `i32c`）
std::string dec(std::uint32_t v) { return std::to_string(static_cast<std::int32_t>(v)); }

// 一份函数体（从 `define i32 @<name>(` 到下一个 `\n}\n`）—— 用于 case 9 的"逐字节相同"
std::string functionBody(const std::string& ir, const std::string& name) {
    const std::string head = "define i32 @" + name + "(";
    const std::size_t at = ir.find(head);
    if (at == std::string::npos) return std::string();
    const std::size_t end = ir.find("\n}\n", at);
    if (end == std::string::npos) return std::string();
    return ir.substr(at, end + 3 - at);
}

std::string lineWith(const std::string& text, const std::string& needle) {
    std::size_t at = text.find(needle);
    if (at == std::string::npos) return std::string();
    std::size_t lo = text.rfind('\n', at);
    lo = (lo == std::string::npos) ? 0 : lo + 1;
    std::size_t hi = text.find('\n', at);
    if (hi == std::string::npos) hi = text.size();
    return text.substr(lo, hi - lo);
}

bool findLine(const std::string& text, const std::string& needle, std::string* out) {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (line.find(needle) != std::string::npos) {
            if (out != nullptr) *out = line;
            return true;
        }
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return false;
}

// =====================================================================================
// 生成期期望（`tests/ir/s7_iat_roots.txt` 的 4 个根；结构量 ⇒ 与机器 / 编译器无关）
// ★ 两档各一份：`LIFT_S7_IAT_GEN` = 生成那份 `criteria.txt` 的 `lift-gen` 用过的开关值。
//
// ⚠ **与 design §5.2 的偏差（实测，2026-11-24）**：design 写的期望是 `unit roots=4 self-pass=4
//   structurable=4 … off-table-callee=0` 与 `host-call=1`。实测**做不到**，原因是根集里
//   `A=00418F80` 的另一臂 `pop esi / jmp 004B7D20` 落进的是**另一个函数**，而那个函数里有
//   `004B7D2D call 004B7B70` —— 只要静态 call 闭包（D14）关着，`004B7B70` 就不在入口集 ⇒
//   A 永远 `off-table-callee` 降级（**与本单机制无关**）。两条出路都已被否决：
//     · 把闭包打开 ⇒ 软根 132 个、模块变成 **179 个单元**（判据不再是"这四个根"的性质）；
//     · 从根集里删掉 A ⇒ 就丢掉"F 变绿后 A 是否跟着变"的见证。
//   ⇒ 保留 4 根 + `--no-close-calls`（模块**确定、极小、与闭包无关**），判据落在 **F** 上
//     （`indirect-call` 1→0、`structurable` 2→3、`ret in structured units` 2→3、F 的降级行消失）。
//     A 在**两档里都**是 `静态 call 目标不是已发射单元`（只打进输出，不作断言）。
// =====================================================================================
#if LIFT_S7_IAT_GEN
constexpr const char* kExpectedCriteria =
    "unit roots=4 self-pass=3 structurable=3 (self-downgrade=1 - callgraph contagion=0) - primary "
    "causes variable-stack=0 writes-return-slot=0 indirect-call=0 dynamic-return=0 off-table-callee=1";
constexpr const char* kExpectedSites =
    "ret=5 (in structured units=3) - call=5 (static=2 indirect=3) => host-call=0 lift_dyn=3 "
    "shim-dispatch=0 other-dispatch=2 - jmp=1(static=1 indirect=0)";
constexpr bool kExpectFStructured = true;  // F=004B8660 不在 `[S7 downgrade]`
#else
constexpr const char* kExpectedCriteria =
    "unit roots=4 self-pass=2 structurable=2 (self-downgrade=2 - callgraph contagion=0) - primary "
    "causes variable-stack=0 writes-return-slot=0 indirect-call=1 dynamic-return=0 off-table-callee=1";
constexpr const char* kExpectedSites =
    "ret=5 (in structured units=2) - call=5 (static=2 indirect=3) => host-call=0 lift_dyn=3 "
    "shim-dispatch=0 other-dispatch=2 - jmp=1(static=1 indirect=0)";
constexpr bool kExpectFStructured = false;  // OFF 档：F 回到 `间接 call`（回退面）
#endif

}  // namespace

int main() {
    using namespace lift_test;
    g_current_case = "s7-iat";

    // =================================================================================
    // ① 生成期判据（模式相关；`LIFT_S7_IAT_CRITERIA` 由 CMake 传 —— 缺资产时这一半不跑）
    // =================================================================================
#if defined(LIFT_S7_IAT_CRITERIA)
    {
        std::ifstream in(LIFT_S7_IAT_CRITERIA, std::ios::binary);
        if (!in) {
            failf(__FILE__, __LINE__, "打不开生成期读数 `%s`（构建产物缺失？）", LIFT_S7_IAT_CRITERIA);
        } else {
            const std::string criteria((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            std::string criteria_line;
            std::string site_line;
            const bool has_criteria = findLine(criteria, "lift-gen: [S7 criteria] ", &criteria_line);
            const bool has_sites =
                findLine(criteria, "lift-gen: [S7 criteria/site accounts] ", &site_line);
            std::printf("lift-s7-iat: criteria: %s\n", has_criteria ? criteria_line.c_str() : "(缺行)");
            std::printf("lift-s7-iat: sites   : %s\n", has_sites ? site_line.c_str() : "(缺行)");
            LIFT_CHECK(has_criteria);
            LIFT_CHECK(has_sites);
            if (has_criteria) {
                LIFT_CHECK(criteria_line.find(kExpectedCriteria) != std::string::npos);
                if (criteria_line.find(kExpectedCriteria) == std::string::npos) {
                    failf(__FILE__, __LINE__, "期望（LIFT_S7_IAT_GEN=%d）: `%s`", LIFT_S7_IAT_GEN,
                          kExpectedCriteria);
                }
            }
            if (has_sites) {
                LIFT_CHECK(site_line.find(kExpectedSites) != std::string::npos);
                if (site_line.find(kExpectedSites) == std::string::npos) {
                    failf(__FILE__, __LINE__, "期望（LIFT_S7_IAT_GEN=%d）: `%s`", LIFT_S7_IAT_GEN,
                          kExpectedSites);
                }
            }
            // 两根根的 `[S7 downgrade]` 行：**F 的**那一行是机制的见证（ON 档必须不在、OFF 档必须在）。
            // ⚠ A 在**两档里都**降级（`off-table-callee`，与本单机制无关；见文件顶的偏差说明）⇒ 只打读数。
            char tag_a[16];
            char tag_f[16];
            std::snprintf(tag_a, sizeof(tag_a), "%08X", kA);
            std::snprintf(tag_f, sizeof(tag_f), "%08X", kF);
            std::string why;
            const bool a_down = findLine(criteria, std::string("[S7 downgrade] ") + tag_a, &why);
            const bool f_down = findLine(criteria, std::string("[S7 downgrade] ") + tag_f, &why);
            std::printf("lift-s7-iat: downgrade %s=%d（不是机制信号） %s=%d（期望 structured=%d）\n", tag_a,
                        a_down ? 1 : 0, tag_f, f_down ? 1 : 0, kExpectFStructured ? 1 : 0);
            LIFT_CHECK(f_down == !kExpectFStructured);
        }
    }
#else
    std::printf("lift-s7-iat: 生成期读数未链接（缺 clang / 缺 AGE.EXE）⇒ 只跑合成夹具那 9 个 case\n");
#endif

    // =================================================================================
    // ② 合成夹具（9 个 case；机制本身的判据）
    // =================================================================================

    // ---- case 1 · stdcall 1 记账：槽在表里且 modelled ⇒ 单元可结构化 ----
    {
        g_current_case = "s7-iat/case1-stdcall1";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, true);
        const StructResult res = analyze(comps, &iat);
        std::printf("case1: structured=%zu call_indirect=%llu lift_dyn=%llu self_ok=%llu\n",
                    res.structured.size(),
                    static_cast<unsigned long long>(res.stats.call_sites_indirect),
                    static_cast<unsigned long long>(res.stats.call_sites_lift_dyn),
                    static_cast<unsigned long long>(res.stats.self_ok));
        LIFT_CHECK(res.structured.count(kFA) == 1);
        LIFT_CHECK(res.stats.self_ok == 1);
        LIFT_CHECK(res.stats.call_sites_indirect == 1);
        LIFT_CHECK(res.stats.call_sites_lift_dyn == 1);
        LIFT_CHECK(res.stats.reason_indirect_call == 0);
    }

    // ---- case 2 · 表缺这一槽 ⇒ 仍然降级（首因 = 间接 call）----
    {
        g_current_case = "s7-iat/case2-missing-slot";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        const IatModel iat;  // 空表
        const StructResult res = analyze(comps, &iat);
        LIFT_CHECK(res.structured.empty());
        LIFT_CHECK(res.stats.reason_indirect_call == 1);
    }

    // ---- case 3 · **表把 conv 写错**（表说 Cdecl/0，槽其实是 stdcall 1）⇒ 公式用的就是表里的值 ----
    {
        g_current_case = "s7-iat/case3-wrong-conv";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Cdecl, 0, true);  // cleanup = 0（错的那一侧）
        const StructResult res = analyze(comps, &iat);
        std::printf("case3: dynamic_ret=%llu indirect=%llu structured=%zu\n",
                    static_cast<unsigned long long>(res.stats.reason_dynamic_ret),
                    static_cast<unsigned long long>(res.stats.reason_indirect_call),
                    res.structured.size());
        // esp 停在 −4 ⇒ 不是"未知"⇒ 归因是**动态返回**（`lift_struct.cpp` 的 ret 判据），不是间接 call。
        LIFT_CHECK(res.stats.reason_dynamic_ret == 1);
        LIFT_CHECK(res.stats.reason_indirect_call == 0);
        LIFT_CHECK(res.structured.empty());
    }

    // ---- case 4 · 寄存器间接：模型非空也一样（`call *%ecx` 的目标运行期才知道）----
    {
        g_current_case = "s7-iat/case4-reg-indirect";
        const Insn call = mkCallReg(static_cast<std::uint32_t>(kFA + 1), 2, Reg::ECX);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, true);
        const StructResult res = analyze(comps, &iat);
        LIFT_CHECK(res.structured.empty());
        LIFT_CHECK(res.stats.reason_indirect_call == 1);
    }

    // ---- case 5 · 绝对地址但**不是**本镜像的槽（模型里没有）⇒ 仍然降级 ----
    {
        g_current_case = "s7-iat/case5-not-a-slot";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlotMissing);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, true);  // 表非空，但没有 kSlotMissing
        const StructResult res = analyze(comps, &iat);
        LIFT_CHECK(res.structured.empty());
        LIFT_CHECK(res.stats.reason_indirect_call == 1);
    }

    // ---- case 6 · `modelled == false`（未实现导入 / 会 take_control）⇒ 仍然降级 ----
    {
        g_current_case = "s7-iat/case6-not-modelled";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, false);  // 有行，但不可建模
        const StructResult res = analyze(comps, &iat);
        LIFT_CHECK(res.structured.empty());
        LIFT_CHECK(res.stats.reason_indirect_call == 1);
    }

    // ---- case 7 · 发射形状：宿主 `call` ↔ 派发 `ret i32`（两侧都钉）----
    {
        g_current_case = "s7-iat/case7-emit-shape";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        std::vector<GenComponent> comps = {mkComp(kFA2, {blockWith(kFA2, {mkCallRel(kFA2, 5, kFA)}),
                                                         blockWith(static_cast<std::uint32_t>(kFA2 + 5),
                                                                   {mkRet(static_cast<std::uint32_t>(kFA2 + 5))})}),
                                           mkPushCallRetComponent(kFA, call)};
        const std::string fname = lift::aot::genFunctionName(kFA);
        // (a) 模型里有这一槽 ⇒ F 可结构化 ⇒ A 的 call 是**宿主 call**
        {
            IatModel iat;
            iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, true);
            const StructResult res = analyze(comps, &iat);
            LIFT_CHECK(res.structured.count(kFA) == 1);
            LIFT_CHECK(res.structured.count(kFA2) == 1);
            bool ok = false;
            const std::string ir = emit(comps, res, &ok);
            LIFT_CHECK(ok);
            LIFT_CHECK(ir.find("call i32 @" + fname) != std::string::npos);
            LIFT_CHECK(ir.find("ret i32 " + dec(kFA)) == std::string::npos);
            std::printf("case7-on : %s\n", lineWith(ir, "call i32 @" + fname).c_str());
        }
        // (b) 空模型 ⇒ F 不可结构化（A 也传染）⇒ 同一条边必须是**派发** `ret i32 <F>`
        {
            const IatModel iat;
            const StructResult res = analyze(comps, &iat);
            LIFT_CHECK(res.structured.empty());
            bool ok = false;
            const std::string ir = emit(comps, res, &ok);
            LIFT_CHECK(ok);
            LIFT_CHECK(ir.find("call i32 @" + fname) == std::string::npos);
            LIFT_CHECK(ir.find("ret i32 " + dec(kFA)) != std::string::npos);
            std::printf("case7-off: %s\n", lineWith(ir, "ret i32 " + dec(kFA)).c_str());
        }
    }

    // ---- case 8 · **尾跳壳的 cleanup 传递**（`ret_cleanup(C) = cleanup(owner(T))`）----
    //   T = { jmp X }（无 `ret`，唯一出口 = 跨分量 jmp → X）；X = { ret 4 }；F = { push eax; call T; ret }
    //   带传递 ⇒ cleanup(T) = 4 ⇒ F 的 esp 回到 0 ⇒ structured；不带 ⇒ F 停在 −4（变异 M2 的靶子）。
    {
        g_current_case = "s7-iat/case8-thunk-cleanup";
        constexpr std::uint32_t kT = 0x92000000u;   // 尾跳壳：jmp X
        constexpr std::uint32_t kX = 0x92001000u;   // 真正的 `retn 4`
        const std::vector<GenComponent> comps = {
            mkComp(kFA, {blockWith(kFA, {mkPushReg(kFA, 1, Reg::EAX), mkCallRel(kFA + 1, 5, kT)}),
                         blockWith(kFA + 6, {mkRet(kFA + 6)})}),
            mkComp(kT, {blockWith(kT, {mkJmpRel(kT, 5, kX)})}),
            mkComp(kX, {blockWith(kX, {mkRetImm(kX, 4)})}),
        };
        const IatModel iat;  // 不需要表：这条只考 thunk 传递
        const StructResult res = analyze(comps, &iat);
        std::printf("case8: structured=%zu (F=%d T=%d X=%d) F.reason=%s\n", res.structured.size(),
                    res.structured.count(kFA) ? 1 : 0, res.structured.count(kT) ? 1 : 0,
                    res.structured.count(kX) ? 1 : 0,
                    res.reason.count(kFA) ? lift::aot::structReasonName(res.reason.at(kFA)) : "-");
        LIFT_CHECK(res.structured.count(kT) == 1);
        LIFT_CHECK(res.structured.count(kX) == 1);
        LIFT_CHECK(res.structured.count(kFA) == 1);
        LIFT_CHECK(res.stats.reason_dynamic_ret == 0);
    }

    // ---- case 9 · 形状回归：同一分量的 IR 体在"可结构化 / 不可结构化"下**逐字节相同** ----
    {
        g_current_case = "s7-iat/case9-ret-shape";
        const Insn call = mkCallAbs(static_cast<std::uint32_t>(kFA + 1), 6, kSlot);
        const std::vector<GenComponent> comps = {mkPushCallRetComponent(kFA, call)};
        IatModel iat;
        iat[kSlot] = mkRow(kSlot, Conv::Stdcall, 1, true);
        const StructResult on = analyze(comps, &iat);
        const IatModel empty;
        const StructResult off = analyze(comps, &empty);
        bool ok1 = false;
        bool ok2 = false;
        const std::string ir_on = emit(comps, on, &ok1);
        const std::string ir_off = emit(comps, off, &ok2);
        LIFT_CHECK(ok1);
        LIFT_CHECK(ok2);
        const std::string name = lift::aot::genFunctionName(kFA);
        const std::string body_on = functionBody(ir_on, name);
        const std::string body_off = functionBody(ir_off, name);
        LIFT_CHECK(!body_on.empty());
        LIFT_CHECK(!body_off.empty());
        // `emitRet` 两条路发**同一条** IR（只差 `stats_` 计数）⇒ 函数体必须逐字节相同。
        LIFT_CHECK(body_on == body_off);
        if (body_on != body_off) {
            failf(__FILE__, __LINE__, "F 的函数体在 structured/非 structured 下不同（emitRet 形状变了？）");
        }
        std::printf("case9: F 函数体 %zu 字节（两侧逐字节相同）\n", body_on.size());
    }

    return finish("lift-s7-iat");
}
