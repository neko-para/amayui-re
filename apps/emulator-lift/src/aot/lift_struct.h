// emulator-lift · S7（D53）：**单元根可否"一对一结构翻译"**的判据 + 调用图不动点
//
// ★ 为什么要有这个文件：`ret` → **宿主 `ret`** 与 `call` → **宿主 `call`** 的前提是
//   "guest 栈上那一格返回地址 ↔ 宿主栈上的返回地址"一一对应。这**只能按函数**判定
//   （⛔ 不按 EA 特判、⛔ 不放宽到"看起来没事"）。
//
// ★ 判据跑在 **`lift-gen` 自己的 Zydis 解码结果**上（`GenComponent` 的块 / `Insn`）：
//   ⛔ 不读 `.tmp/aot/s7/census.mjs` 那套 `.lst` 分析 —— 那份对 IDA collapsed 的 1,805 个函数
//   （`.ll` 里 10.7% 的出口，含 `dispatch-top` 第 2 名 `__ftol2`）一条指令都没看，而 `lift-gen`
//   自己解 PE 是看得见的。`.tmp/aot/s7/census.md` 只当**参考读数**与**方法提示**（D54）。
//
// ★ 自身分析（父级裁决 D53，逐条落在这里的 `analyzeSelf`）：
//   1. **esp 全程可静态跟踪**（数据流：push/pop、`add/sub esp,imm`、`mov esp,ebp`、`lea esp,[esp/ebp+k]`、
//      `leave`；**`call` 的栈清理**按被调方的 `retn imm16` 建模 —— D54 §1 的方法学）。
//      ⛔ 任何 `sub/add esp,reg`、`and esp,imm`、`mov esp,<非帧reg>`、`pop esp`、`xchg` 碰 ESP
//      ⇒ 该函数不可结构化（`__alloca_probe` 家族就是这样被挡下的：它自己 `add esp,eax`）。
//   2. **没有任何指令写"返回地址那一格"**：语法判据 = 目的内存操作数的
//      `base==esp && disp==0`（入口那一格）或 `base==ebp && disp==4`（帧指针形态）。
//      ⚠ 残余风险（如实记账）：先把 esp 拷到别的寄存器再写不会被这条抓住 —— 语料普查里
//      `.lst` 可见部分这类写入是 **0**（D54 §2，含一次 false positive 的复核）。
//   3. **无动态返回**：每个 `ret` 站点的 `esp` 偏移必须是静态已知的 **0**（= 入口那一格）。
//      未知 / 非 0 ⇒ 这个 `ret` 的"返回地址"不是调用者压的那一格 ⇒ 不可结构化。
//   4. **无间接 `call reg/mem`**：这类站点的目标是运行期才知道的（要落 `lift_dyn`），
//      按 D53 的裁决，caller 的宿主帧在那次尾调用里就没了 ⇒ 它自己的 `ret` 不能落宿主 `ret`。
//      （`jmp reg/mem` 是**尾巴**位置，不要求 continuation ⇒ 不算阻塞，落 `lift_dyn` 尾调用。）
//   5. **所有静态 callee 都是已知的已发射单元**（`entry_owner` 里有它）：目标不在表里
//      （被丢弃的 leader / unbuildable / 表外）⇒ 无法证明 ⇒ 不可结构化（单调、可回退）。
//
// ★ **传递闭包**（父级裁决 D53 第 2 条，fixpoint）：
//     structured(F) = selfOk(F) && ∀ C ∈ callees(F): structured(C)
//   理由（父级原文）：一旦某个 `call` 站点走了 `lift_dyn`，caller 的宿主帧就在尾调用里没了，
//   它的 continuation 只能被当作新的派发入口进来 ⇒ 它自己的 `ret` 也不能用宿主 `ret`。
//   ⇒ 按不动点从"自身不过"的集合反向传染（单调递减 ⇒ 必收敛），**降级清单带原因**。
//
// ★ **单调可回退**：任何没能转换的站点都保持今天的语义（`ret i32 <EA>` 回 trampoline）。
//   ⇒ 部分完成是安全的；⛔ 不要为了"全转"而放宽判据。
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "aot/lift_gen_emit.h"

namespace lift {
namespace aot {

// 一个单元根被判"不可结构化"的**首因**（只用于报告 / `.ll` 头部读数；判据在 analyzeSelf）。
enum class StructReason : std::uint8_t {
    None = 0,
    VarStack,        // 可变长栈（esp 不再可静态跟踪 / 调用了这类被调方）
    ReturnSlotWrite, // 写了返回地址那一格
    IndirectCall,    // 间接 `call reg/mem`（要落 lift_dyn，caller 的帧会没）
    DynamicReturn,   // `ret` 站点的 esp 偏移不是静态已知的 0
    UnknownCallee,   // 静态 call 的目标不是已发射单元（表外 / 被丢弃）
    CalleeCascade,   // 调用图传染（自身没问题，但某个可达 callee 不可结构化）
};

const char* structReasonName(StructReason r);

// 读数（**生成期确定性计数**；⛔ 不是运行期埋点）。分母 = `.ll` 里实际的站点。
struct StructStats {
    std::uint64_t functions = 0;         // 参与判定的单元根数
    std::uint64_t self_ok = 0;           // 自身分析通过
    std::uint64_t structured = 0;        // fixpoint 之后仍可结构化
    std::uint64_t downgraded_self = 0;   // 自身分析就不过
    std::uint64_t downgraded_callee = 0; // 只被调用图传染
    // 首因分布（自身分析；一个函数只归一类，先命中先算）
    std::uint64_t reason_varstack = 0;
    std::uint64_t reason_return_slot = 0;
    std::uint64_t reason_indirect_call = 0;
    std::uint64_t reason_dynamic_ret = 0;
    std::uint64_t reason_unknown_callee = 0;
    // ---- 站点账（静态；按**发射出去的形状**分类，与 `GenEmitStats::s7_*` 交叉核对）----
    std::uint64_t ret_sites = 0;          // guest `ret` 站点
    std::uint64_t ret_sites_structured = 0;  // 在可结构化单元里 ⇒ 宿主 `ret`
    std::uint64_t call_sites = 0;         // `call` 站点（静态 + 间接）
    std::uint64_t call_sites_static = 0;
    std::uint64_t call_sites_indirect = 0;
    std::uint64_t call_sites_host = 0;    // 目标可结构化 ⇒ 宿主 `call`
    std::uint64_t call_sites_lift_dyn = 0;  // 间接 ⇒ `lift_dyn`
    std::uint64_t call_sites_shim = 0;    // 目标是 shim 桩 ⇒ `ret i32`（派发）
    std::uint64_t call_sites_dispatch = 0;  // 目标不可结构化 / 表外 ⇒ `ret i32`
    std::uint64_t jmp_sites = 0;
    std::uint64_t jmp_sites_static = 0;
    std::uint64_t jmp_sites_indirect = 0;
    std::uint64_t jmp_sites_tail = 0;     // 跨函数 + 目标可结构化 ⇒ 尾调用
    std::uint64_t jmp_sites_lift_dyn = 0; // 间接 ⇒ `lift_dyn` 尾调用
    std::uint64_t jmp_sites_dispatch = 0; // 跨函数 + 目标不可结构化 ⇒ `ret i32`
};

struct StructResult {
    std::set<std::uint32_t> structured;                  // 可结构化的单元根
    std::map<std::uint32_t, StructReason> reason;         // 每个**被降级**根的（最终）原因
    std::map<std::uint32_t, std::uint32_t> reason_ea;     // 首因站点 EA（报告用；0 = 无）
    std::map<std::uint32_t, std::uint32_t> contagious_from;  // 传染源根（仅 CalleeCascade）
    StructStats stats;
};

// `entry_owner`：入口 EA → 该单元的根（调用目标归属）。`dropped` = 没有块的派生 leader。
// `shim_begin/end` = shim 桩页（桩 EA 不是 guest 单元 ⇒ 不参与调用图，也不算"不可结构化"）。
StructResult analyzeStructure(const std::vector<GenComponent>& comps,
                              const std::map<std::uint32_t, std::uint32_t>& entry_owner,
                              const std::set<std::uint32_t>& dropped, std::uint32_t shim_begin,
                              std::uint32_t shim_end);

}  // namespace aot
}  // namespace lift
