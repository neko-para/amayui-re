// emulator-lift · **发射形状守卫**：同分量边直连（D1）+ 跨分量 `jcc` 尾巴调用（D2）
//
// ★ 判据（红了意味着什么）：`emitModule` 对一份**合成**的分量图吐出的 IR 文本里，
//   ① **本分量里有块**的目标 EA 必须是 `br label %bb_<EA>`（直连），⛔ 不许再 `ret i32 <EA>`；
//   ② **三类必须继续派发**（⛔ 一处都不许改成直连）——
//        · 宿主语义钩子 EA（`mustDispatchAt`：钩子跑在派发边界上，直连 = 静默失效）；
//        · intrinsic 根（`aot/intrinsics.txt`：闸门块只从 `switch` 的根 case 进）；
//        · `dropped_`（没有块 ⇒ 直连会指向不存在的标签）；
//   ③ 跨分量 `jcc` 的臂：目标可结构化 ⇒ 与静态 `jmp` **同一形态**的尾巴调用
//        （`%r = tail call i32 @lift_cc_<owner>(ptr %st, ptr %mem, i32 <EA>, ptr %cpu)` + `ret i32 %r`）；
//      目标不可结构化 ⇒ 保持 `ret i32 <EA>`（派发）。**两条都在这里钉住**（⛔ 只钉"变多了"会把
//      "无脑全转"当成绿）。
//
// ★ 为什么它是**形状**断言而不是行为断言（`.tmp/emit-edges/report.md` §4）：
//   把 D1 改回 `ret i32` 之后 guest 可见语义**完全不变**（只是每步多一圈 trampoline）⇒
//   指纹 / 差分 / 端到端 ctest 全绿。唯一会红的就是"这条边发成了什么"⇒ 判据只能落在 IR 文本上。
//
// ★ 为什么用**合成** `GenComponent`（不读 PE / 不读 `.lst` / 不用 clang）：
//   `emitModule` 是**公开 API**、只消费分区结果 ⇒ 夹具可以手写 5 个分量、11 个块、0 条数据指令，
//   整条用例是毫秒级、且与语料 / 闭包 / 资产生效无关（缺资产时也照跑）。
//   ⚠ 这里**刻意不做**"端到端跑一遍生成的代码"：那是 `lift-s7-structured` / `lift-ir-gen-*` 的事。
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aot/lift_gen_emit.h"
#include "check.h"
#include "runtime/insn.h"

namespace {

using lift::Cond;
using lift::Insn;
using lift::Op;
using lift::OpKind;
using lift::aot::GenBlock;
using lift::aot::GenClosureStats;
using lift::aot::GenComponent;
using lift::aot::GenEmitStats;
using lift::aot::GenStructure;
using lift::aot::GenUnsupported;

// rel32 的**存法**与解码器一致：32 位补码（`genBranchTarget` 按 `(int32)imm` 回加）。
std::uint32_t rel32(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    const std::int64_t rel = static_cast<std::int64_t>(target) -
                             (static_cast<std::int64_t>(ea) + static_cast<std::int64_t>(len));
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(rel));
}

Insn mkInsn(std::uint32_t ea, std::uint8_t len, Op op) {
    Insn in{};
    in.ea = ea;
    in.length = len;
    in.op = op;
    in.mnemonic = "synthetic";
    return in;
}

Insn mkRel(std::uint32_t ea, std::uint8_t len, Op op, std::uint32_t target) {
    Insn in = mkInsn(ea, len, op);
    in.ops[0].kind = OpKind::Imm;
    in.ops[0].size = 4;
    in.ops[0].imm = rel32(ea, len, target);
    return in;
}

Insn mkJcc(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    Insn in = mkRel(ea, len, Op::Jcc, target);
    in.cc = Cond::E;  // `je`：只读 ZF（能融合就融合，不能就按位读 base）
    return in;
}

Insn mkJmp(std::uint32_t ea, std::uint8_t len, std::uint32_t target) {
    return mkRel(ea, len, Op::Jmp, target);
}

Insn mkRet(std::uint32_t ea) { return mkInsn(ea, 1, Op::Ret); }

void putBlock(GenComponent* c, std::uint32_t ea, Insn term) {
    GenBlock b;
    b.ea = ea;
    b.terminated = true;
    b.insns.push_back(term);
    c->blocks.emplace(ea, std::move(b));
}

// `emitModule` 的输出里，含 `needle` 的那一行（没有 ⇒ 空串）。
std::string lineWith(const std::string& text, const std::string& needle) {
    std::size_t at = text.find(needle);
    if (at == std::string::npos) return std::string();
    std::size_t lo = text.rfind('\n', at);
    lo = (lo == std::string::npos) ? 0 : lo + 1;
    std::size_t hi = text.find('\n', at);
    if (hi == std::string::npos) hi = text.size();
    return text.substr(lo, hi - lo);
}

std::size_t countOf(const std::string& text, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// `i32c`（生成器的十进制拼法）的同一份口径：按 int32 打印。
std::string dec(std::uint32_t v) { return std::to_string(static_cast<std::int32_t>(v)); }

// 一条"派发出口"的完整形状：`bb_exit_<EA>:` + `ret i32 <EA>`。
bool hasExitRet(const std::string& text, std::uint32_t ea) {
    char label[32];
    std::snprintf(label, sizeof(label), "bb_exit_%X:", ea);
    if (text.find(label) == std::string::npos) return false;
    return text.find("  ret i32 " + dec(ea) + "\n") != std::string::npos;
}}  // namespace

int main() {
    using namespace lift_test;

    g_current_case = "lift-emit-edges";

    // =============================================================================
    // 合成分量图（EA 都是编造的，⛔ 与任何真实 guest 代码无关）：
    //   A 0x401000  结构化；含 5 条要判形状的边（D1×2 / D2 结构化×1 / D2 不可结构化×1 /
    //               钩子×1 / dropped×1）
    //   B 0x402000  结构化（D2 的**该转**那一侧）
    //   C 0x403000  **不可结构化**（D2 的**不许转**那一侧）
    //   D 0x404000  钩子 EA（`mustDispatchAt`）
    //   E 0x405000  intrinsic 根（`_memcpy` 的闸门块只从 `switch` 的根 case 进）
    // =============================================================================
    std::vector<GenComponent> comps(5);
    comps[0].root = 0x401000u;
    comps[0].entries = {0x401000u, 0x401010u};
    putBlock(&comps[0], 0x401000u, mkJcc(0x401000u, 6, 0x401010u));  // D1：目标是**本分量的入口**
    putBlock(&comps[0], 0x401006u, mkJmp(0x401006u, 5, 0x401000u));  // D1：目标 = 本分量的根
    putBlock(&comps[0], 0x401010u, mkRet(0x401010u));
    putBlock(&comps[0], 0x401020u, mkJcc(0x401020u, 6, 0x402000u));  // D2：跨分量 + 可结构化
    putBlock(&comps[0], 0x401026u, mkRet(0x401026u));
    putBlock(&comps[0], 0x401030u, mkJcc(0x401030u, 6, 0x403000u));  // D2：跨分量 + 不可结构化
    putBlock(&comps[0], 0x401036u, mkRet(0x401036u));
    putBlock(&comps[0], 0x401040u, mkJcc(0x401040u, 6, 0x404000u));  // 钩子 ⇒ 必须派发
    putBlock(&comps[0], 0x401046u, mkRet(0x401046u));
    putBlock(&comps[0], 0x401050u, mkJcc(0x401050u, 6, 0x406000u));  // dropped ⇒ 必须派发
    putBlock(&comps[0], 0x401056u, mkRet(0x401056u));

    comps[1].root = 0x402000u;
    comps[1].entries = {0x402000u};
    putBlock(&comps[1], 0x402000u, mkRet(0x402000u));

    comps[2].root = 0x403000u;
    comps[2].entries = {0x403000u};
    putBlock(&comps[2], 0x403000u, mkRet(0x403000u));

    comps[3].root = 0x404000u;
    comps[3].entries = {0x404000u};
    putBlock(&comps[3], 0x404000u, mkRet(0x404000u));

    comps[4].root = 0x405000u;
    comps[4].entries = {0x405000u};
    putBlock(&comps[4], 0x405000u, mkJcc(0x405000u, 6, 0x405000u));  // intrinsic 根 ⇒ 必须派发
    putBlock(&comps[4], 0x405006u, mkRet(0x405006u));

    std::set<std::uint32_t> entry_set;
    for (const GenComponent& c : comps) entry_set.insert(c.entries.begin(), c.entries.end());
    entry_set.insert(0x406000u);  // dropped：在入口集里、但**没有块**（派发才是唯一安全的落点）
    const std::set<std::uint32_t> dropped = {0x406000u};

    GenStructure structure;
    structure.enabled = true;
    structure.structured_roots = {0x401000u, 0x402000u, 0x404000u};  // C / E 刻意不可结构化
    structure.hook_eas = {0x404000u};
    structure.intrinsic_roots = {{0x405000u, "memcpy"}};
    structure.shim_begin = 0x7FFE0000u;
    structure.shim_end = 0x7FFE8000u;

    GenEmitStats stats;
    std::vector<GenUnsupported> unsupported;
    std::string ir;
    const bool ok = lift::aot::emitModule(comps, entry_set, dropped, GenClosureStats{}, structure, &ir,
                                          &stats, &unsupported);
    LIFT_CHECK(ok);
    LIFT_CHECK(unsupported.empty());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(ir, "define i32 @lift_cc_")), 5);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(ir, "  ret i32 " + dec(0x401010u) + "\n")), 0);

    // ---- D1 ①：`jcc` 目标 = 本分量的**入口** ⇒ 两个臂都直连（⛔ 不发 `bb_exit_401010`）----
    {
        // ⚠ 搜两个臂的**连写**：`switch` 的 case 行也含 `, label %bb_401010`（裸标签会先命中它）。
        const std::string line = lineWith(ir, "label %bb_401010, label %bb_401006");
        LIFT_CHECK(!line.empty());
        LIFT_CHECK(line.find("br i1 ") != std::string::npos);
        LIFT_CHECK(ir.find("bb_exit_401010:") == std::string::npos);
        std::printf("D1-a (jcc → 本分量入口): %s\n", line.c_str());
    }

    // ---- D1 ②：`jmp` 目标 = 本分量的**根** ⇒ 直连（⛔ 不发 `ret i32 0x401000`）----
    {
        LIFT_CHECK(ir.find("br label %bb_401000") != std::string::npos);
        LIFT_CHECK(ir.find("bb_exit_401000:") == std::string::npos);
    }

    // ---- D1 ③：intrinsic 根（**自己就是本分量的块**）⇒ 必须继续派发（闸门块只从 switch 进）----
    {
        LIFT_CHECK(ir.find("lintr_entry_405000:") != std::string::npos);  // 夹具真的带上了闸门块
        LIFT_CHECK(hasExitRet(ir, 0x405000u));
        const std::string line = lineWith(ir, "label %bb_405006");
        LIFT_CHECK(line.find(", label %bb_exit_405000, label %bb_405006") != std::string::npos);
        std::printf("D1-b (intrinsic 根): %s\n", line.c_str());
    }

    // ---- D2 ①：跨分量 + 目标可结构化 ⇒ 与静态 `jmp` 同一形态的尾巴调用 ----
    {
        const std::string call = "tail call i32 @lift_cc_402000(ptr %st, ptr %mem, i32 " + dec(0x402000u) +
                                 ", ptr %cpu)";
        LIFT_CHECK(ir.find(call) != std::string::npos);
        LIFT_CHECK(ir.find("bb_exit_402000:") == std::string::npos);
        const std::string line = lineWith(ir, "label %bb_401026");
        LIFT_CHECK(line.find("label %s7_jc_") != std::string::npos);
        // 尾巴调用的下一行必须是 `ret i32 %…`（`tail` 契约：本帧换掉，返回值原样上抛）
        const std::size_t at = ir.find(call);
        const std::size_t next = ir.find("ret i32 %", at);
        LIFT_CHECK(at != std::string::npos && next != std::string::npos && next - at < 200);
        std::printf("D2-a (跨分量 jcc 尾巴调用): %s\n", call.c_str());
    }

    // ---- D2 ②：跨分量 + 目标**不可结构化** ⇒ 保持 `ret i32`（⛔ 不许"无脑全转"）----
    {
        LIFT_CHECK(hasExitRet(ir, 0x403000u));
        LIFT_CHECK(ir.find("tail call i32 @lift_cc_403000") == std::string::npos);
    }

    // ---- 三类必须继续派发之二：钩子 EA（jcc 的臂）----
    {
        LIFT_CHECK(hasExitRet(ir, 0x404000u));
        LIFT_CHECK(ir.find("tail call i32 @lift_cc_404000") == std::string::npos);
    }

    // ---- 三类必须继续派发之三：dropped（没有块 ⇒ 只能派发；⛔ 不许 `br` 到不存在的标签）----
    {
        LIFT_CHECK(hasExitRet(ir, 0x406000u));
        LIFT_CHECK(ir.find("%bb_406000") == std::string::npos);
    }

    // ---- 读数：**恰好**一条 `jcc` 臂走了尾巴调用（多一条 = 判据放宽了）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats.s7_jcc_tail), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(ir, "tail call i32 @lift_cc_")), 1);
    std::printf("stats: s7_jcc_tail=%llu s7_tail_calls=%llu s7_jmp_tail=%llu\n",
                static_cast<unsigned long long>(stats.s7_jcc_tail),
                static_cast<unsigned long long>(stats.s7_tail_calls),
                static_cast<unsigned long long>(stats.s7_jmp_tail));
    // 静态 `jmp` 那一路的判据没被这次改动碰到：本夹具里唯一的 `jmp` 是**单元内**回边 ⇒ `br`，
    // 既不进尾巴调用、也不进派发。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats.s7_jmp_tail), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats.s7_jmp_dispatch), 0);

    // ---- D1/D2 的**开关**：`--no-s7`（`structure_.enabled == false`）= 旧派发形状 ----
    //   为什么它也是判据：`--no-s7` 的语义是"回到 D55 之前的形状（每个块边界都回 trampoline）"，
    //   而 `tests/ops_diff.cpp` / `tests/x87_diff.cpp` 那几条**逐块**差分夹具就是按
    //   "一次宿主调用 = 一个块"写的（`if (++dispatches > 1) break;`）⇒ 这条开关一漏，
    //   它们会把一个 guest 循环整段跑在一次调用里（夹具预算失效）。所以这里钉住"关掉就是旧形状"。
    {
        GenStructure off = structure;
        off.enabled = false;
        GenEmitStats stats_off;
        std::vector<GenUnsupported> unsupported_off;
        std::string ir_off;
        LIFT_CHECK(lift::aot::emitModule(comps, entry_set, dropped, GenClosureStats{}, off, &ir_off,
                                        &stats_off, &unsupported_off));
        LIFT_CHECK(unsupported_off.empty());
        LIFT_CHECK(hasExitRet(ir_off, 0x401010u));  // 同分量入口 ⇒ 又回 `bb_exit_` + `ret i32`
        // ⚠ `jmp` 到**本分量的块**这条路是**既有**行为（`emitTerminator` 的 Jmp 分支自己就判
        //   `comp_.blocks`），不在 D1 的开关上 ⇒ `--no-s7` 下它**仍然**是直连。⛔ 不要把它读成
        //   "开关漏了"：本单只收 `emitEdge` / `labelFor` 那两条路由。
        LIFT_CHECK(ir_off.find("br label %bb_401000") != std::string::npos);
        LIFT_CHECK(hasExitRet(ir_off, 0x402000u));  // 跨分量 jcc 臂 ⇒ 又回派发（没有尾巴调用）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_off.s7_jcc_tail), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(ir_off, "tail call i32 @lift_cc_")), 0);
        std::printf("--no-s7: jcc_tail=%llu bb_exit_401010=%d（旧形状）\n",
                    static_cast<unsigned long long>(stats_off.s7_jcc_tail),
                    static_cast<int>(ir_off.find("bb_exit_401010:") != std::string::npos));
    }

    return finish("lift-emit-edges");
}
