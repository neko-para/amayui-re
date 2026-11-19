// emulator-lift · Shape A / T1a：`emulator-lift-gen` 的**生成器核心**（分区产物 → LLVM IR 文本）
//
// ★ 本文件只描述"生成器的输入形状"与"一个块的后继边"，⛔ 不含任何 x86 语义：
//   语义只照 `src/runtime/exec_*.cpp`（解释器臂）抄，一处都不新发明（见 lift_gen_emit.cpp 头注）。
//
// ★ 单位 = **入口函数**（父级已裁）：一个 `GenComponent` ⇒ 一个
//   `define i32 @lift_cc_<EA>(ptr %st, ptr %mem, i32 %ea, ptr %cpu)`。
//   分量根 + 该分量内的 resume EA（`call` 的下一条 EA）都进同一个 `switch i32 %ea`。
//
// ★ 分区（把 guest 的块图切成互不重叠的分量）**不在本文件**：那是 `lift_gen.cpp` 的事
//   （leader 发现 + 冲突提升到收敛）。本文件只消费分区结果。
//
// ★ 后继边的判定**只此一处**（`genBlockEdges`）：分区与发射共用它，⛔ 不写第二份。
//   否则"分区时以为有这条边、发射时又按另一套走"是静默错。
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/insn.h"

namespace lift {
namespace aot {

// 一个**基本块**：从 leader 起，到控制流指令（含）或到"下一条是 leader"为止。
struct GenBlock {
    std::uint32_t ea = 0;
    std::vector<lift::Insn> insns;
    bool terminated = false;  // 最后一条是不是控制流（false ⇒ 落空到下一个 leader）
};

// 一个入口函数（分量）的产物。`blocks` / `entries` 一律**升序**（确定性来自容器本身的顺序）。
struct GenComponent {
    std::uint32_t root = 0;
    std::vector<std::uint32_t> entries;         // 升序；含 root
    std::map<std::uint32_t, GenBlock> blocks;   // 块首 EA → 块（升序遍历）
};

struct GenEdge {
    std::uint32_t ea = 0;
    bool call_ret = false;  // true ⇒ 这条边是 `call` 的下一条 EA（resume EA ⇒ 入口）
    bool dynamic = false;   // true ⇒ 目标运行期才知道（`jmp reg/mem` / `ret` 弹栈）；`ea` 无意义
};

// 块的后继边（唯一一份判据）。`dynamic` 的边不参与静态分区。
std::vector<GenEdge> genBlockEdges(const GenBlock& block);

// `jmp/jcc/call rel32` 的目标 = ea + length + (int32)imm（32 位回绕），与 `exec_control.cpp` 同式。
std::uint32_t genBranchTarget(const lift::Insn& insn);

// 一个分量的入口函数符号名（`lift_cc_<root>`）。
// ★ **唯一一份**：`.ll` 里的 `define i32 @<名>` 与 AOT 派发表（`--table` 的 `EA → <名>`）都从这里来。
//   ⛔ 不要在别处自己拼：拼法（大小写 / 补零 / 前缀）一漂，链接期就是满屏 undefined symbol，
//   而那种错只在**构建**里显形、不在生成物里显形（`lift-gen` 自己会"成功"）。
std::string genFunctionName(std::uint32_t root);

// 本轮不支持的 guest 指令（EA + op 名 + 原因）。**只剩 `llvm.trap`**：T1c 起"未实现的 op"
// 一律走单指令宿主原语 `lift_one`（见 `lift_gen_emit.cpp` 的头注），所以这个清单为空是正常态；
// 有内容 ⇒ 生成器内部不一致（控制转移掉进了默认路径）⇒ 退出码 3。
struct GenUnsupported {
    std::uint32_t ea = 0;
    const char* op = "?";
    std::string reason;
};

// ★ D14（T3a）：**静态调用闭包**在生成期的账（只进 `.ll` 头部注释与 stderr 读数，⛔ 不参与语义）。
//   软根 = `call rel32` 的目标里、原先不在入口集、也不是 shim 桩 EA、且落在可执行节内的那些 EA。
//   ⛔ 不在这里放 EA 清单：`.ll` 只写计数（清单由 `--verbose` / stderr 样例承担），否则产物会带
//   "输入顺序无关但集合顺序相关"的隐患，也让 `.ll` 头部膨胀。
// ★ D15（G1）/ D17（G3）：软根有**三类**，账**按类分开**（⛔ 不合并成一个数）：
//   第一类（D14）= `call rel32` 的目标；
//   第二类（D15）= **写内存的 `mov` 的 imm32**（代码指针立即数，函数指针表初始化那一类）；
//   第三类（D17）= **非可执行节里静态初始化的代码指针**（`.data` 里 4 字节对齐、值落在可执行节内的
//                  dword：PE 阶段就写好的函数指针表，没有任何静态代码边指向它）。
//   每类都有 added + 三档丢弃（越界 / 解不出 / 重叠）+ shim 跳过，字段一一对应。
struct GenClosureStats {
    bool close_calls = false;            // 本次是否开着第一类软根（D14 静态调用闭包）
    std::uint64_t soft_roots = 0;        // 被接受为软根（= 新增 unit 根）的 EA 数
    std::uint64_t soft_outside = 0;      // 丢弃：`call rel32` 目标不在任何可执行节
    std::uint64_t soft_undecodable = 0;  // 丢弃：软根解不出
    std::uint64_t soft_overlap = 0;      // 丢弃：软根落在已发射块的字节区间内（D10 P3 ②）
    std::uint64_t shim_targets = 0;      // 跳过：目标是 shim 桩页里的 EA（不是 guest 单元）
    // ---- 第二类（D15）：写内存 mov 的 imm32 ----
    bool close_stores = false;                 // 本次是否开着第二类软根
    std::uint64_t store_soft_roots = 0;        // 被接受为软根的 imm32 数
    std::uint64_t store_outside = 0;           // 丢弃：imm32 不落在任何可执行节（绝大多数是数据常量）
    std::uint64_t store_undecodable = 0;       // 丢弃：该软根解不出
    std::uint64_t store_overlap = 0;           // 丢弃：该软根落在已发射块的字节区间内（D10 P3 ②）
    std::uint64_t store_shim_targets = 0;      // 跳过：imm32 是 shim 桩页里的 EA
    std::uint64_t store_imm_sites = 0;         // 读数（不是判据）：解出来的 `mov [mem], imm32` 站点数
    std::uint64_t store_imm_in_exec = 0;       // 读数：其中 imm 落在可执行节内的
    // ---- 第三类（D17）：非可执行节里静态初始化的代码指针 ----
    bool close_data = false;                   // 本次是否开着第三类软根
    std::uint64_t data_soft_roots = 0;         // 被接受为软根的 dword 目标数
    std::uint64_t data_outside = 0;            // 丢弃：目标不在任何可执行节（结构性为 0：候选定义就是"在内"）
    std::uint64_t data_undecodable = 0;        // 丢弃：该软根解不出
    std::uint64_t data_overlap = 0;            // 丢弃：该软根落在已发射块的字节区间内（D10 P3 ②）
    std::uint64_t data_overlap_pre = 0;        // 读数：其中在**收之前**就挡下的（候选集是静态的 ⇒ 判据前移）
    std::uint64_t data_shim_targets = 0;       // 跳过：目标是 shim 桩页里的 EA
    std::uint64_t data_ptr_sites = 0;          // 读数：扫过的 4 字节对齐 dword 站点数（判据的输入面）
    std::uint64_t data_ptr_in_exec = 0;        // 读数：其中值落在可执行节内的站点数
    std::uint64_t data_ptr_distinct = 0;       // 读数：去重后的候选目标数
    std::uint64_t data_ptr_skipped = 0;        // 读数：候选里已在入口集 / 已判过 ⇒ 不重复收
    // ---- 分区收敛的护栏读数（不是软根，是"入口集"里没有块的那些 EA）----
    std::uint64_t unbuildable_entries = 0;     // 入口 EA 里没有块（落在已发射块字节区间内 / 解不出）⇒ 退出入口集
    // ---- 第四类（D21）：**可执行节**里 4 字节对齐的跳表 dword（值 = 已发射块的首 EA）----
    //   ★ 这一类**不是软根**：它不解码、不新增块 / 指令，只把已有的块变成"可被 trampoline 派发"
    //     （switch 多一个 case + 派发表多一行）。⛔ 别把 added 读成"新增 unit 根"。
    std::uint64_t jump_table_sites = 0;        // 读数：可执行节里 4 字节对齐的 dword 站点数
    std::uint64_t jump_table_hits = 0;         // 读数：其中值 == 某个已建块首 EA 的站点数
    std::uint64_t jump_table_distinct = 0;     // 读数：不同目标 EA 数
    std::uint64_t jump_table_added = 0;        // 新加进 entries（switch case + 派发表行）的 EA 数
    std::uint64_t jump_table_skipped = 0;      // 已在入口集 / 已判过 ⇒ 不重复加
    std::uint64_t jump_table_unclaimed = 0;    // 是块首但没有任何分量 claim ⇒ 走既有孤儿提升
    // ---- 第五类（D26）：**可执行节**里 4 字节对齐的 dword → **尚未解码**的代码 ----
    //   ★ 与第四类的本质区别：这一类**会新增块**（`added` = 新增 unit 根），所以它记在"软根"这一侧，
    //     但账**独立成一组**（⛔ 不与 D14/D15/D17 合并）。
    bool close_jmptab = false;                 // 本次是否开着第五类软根
    std::uint64_t jumptab_soft_roots = 0;      // 被接受为软根（= 新增 unit 根 / 新增块 / 新指令）的 EA 数
    std::uint64_t jumptab_outside = 0;         // 丢弃：目标不在任何可执行节（候选定义就是"在内" ⇒ 结构性为 0）
    std::uint64_t jumptab_undecodable = 0;     // 丢弃：该软根解不出
    std::uint64_t jumptab_overlap = 0;         // 丢弃：该软根落在已发射块的字节区间内（D10 P3 ②）
    std::uint64_t jumptab_shim_targets = 0;    // 跳过：目标是 shim 桩页里的 EA
    std::uint64_t jumptab_rounds = 0;          // 读数：不动点迭代跑了几轮（0 = 关 / 无候选）
    // ---- 第五类的**候选面**（"先量后收"；⛔ 不是判据）----
    //   恒等式：`distinct = in_entries + shim + block_starts + overlap_pre + decodable + undecodable_pre`
    //   （每个候选**只归一类**；`decodable` 里再分出事后被丢的 `jumptab_undecodable` / `jumptab_overlap`）
    std::uint64_t jumptab_sites = 0;           // 扫过的可执行节 4 字节对齐 dword 站点数
    std::uint64_t jumptab_in_exec = 0;         // 其中值落在可执行节内的站点数
    std::uint64_t jumptab_distinct = 0;        // 去重后的不同候选目标数
    std::uint64_t jumptab_in_entries = 0;      // 已在入口集（含前三类软根）/ 已被前三类判过
    std::uint64_t jumptab_shim = 0;            // 落在 shim 桩页（不是 guest 单元）
    std::uint64_t jumptab_block_starts = 0;    // **已是已建块首** ⇒ 第四类（D21）负责，这一趟不收
    std::uint64_t jumptab_overlap_pre = 0;     // 落在已发射块的字节区间内 ⇒ 收之前就挡下
    std::uint64_t jumptab_decodable = 0;       // 剩下里 `Decoder::decode` 成功的（收之前的试解一次）
    std::uint64_t jumptab_undecodable_pre = 0;  // 剩下里试解失败的（收之前的读数）
    std::uint64_t jumptab_live_roots = 0;      // 最终**仍在入口集**的第五类软根数（= 实际新增 unit 根）
};

// ★ S7（D53）：结构翻译的**输入形状**。判据在 `aot/lift_struct.*`（跑在 `lift-gen` 自己的
//   Zydis 解码结果上），发射器**只消费结论**、⛔ 不在这里重新判定。
struct GenStructure {
    // ★ `--no-s7` 时 false：**整套** S7 lowering 关闭（宿主 call / lift_dyn / 返回判据），
    //   回到 D55 之前的形状（每个 `call`/`ret`/间接跳转都回 trampoline）。
    //   用途见 `lift_gen.cpp` 的 `Options::s7`：协议形状夹具（`lift-ir-gen-shim`）断言的是旧形状。
    bool enabled = true;
    std::set<std::uint32_t> structured_roots;  // 可一对一转译的单元根（`analyzeStructure` 的 fixpoint 结果）
    // ★ 宿主语义钩子 EA（skip-logo / recovery）：指向它们的静态边**必须**留在派发边界
    //   （`ret i32 <EA>`）—— 否则 S7 之后它们再也见不到 trampoline ⇒ 钩子静默失效。
    //   见 `aot/aot_hook_ea.h` 的完整理由。
    std::set<std::uint32_t> hook_eas;
    std::uint32_t shim_begin = 0;  // shim 桩页（`GuestLayout::shim_page`）
    std::uint32_t shim_end = 0;    // `shim_page + shim_slots*4`
};

struct GenEmitStats {
    std::uint64_t traps_unsupported = 0;  // 未实现 op 的 `llvm.trap`（T1c 起应恒为 0）
    std::uint64_t traps_bad = 0;          // 每个入口函数 switch 的 `bad:` 一个 `llvm.trap`
    // ★ S3-a / S3-b 的读数（**生成期确定性计数**，⛔ 不是运行期埋点；只进 `.ll` 头部注释）。
    //   口径见 `.tmp/aot/s3design/plan.md` §1.8：物化 / 逐位交付 / 融合读者 / 回退原因分桶。
    //   ★ "融合读者"是本步的**收益指标**（⛔ 不用墙钟）。
    std::uint64_t flags_materialize_calls = 0;   // `materialize(mask != 0)` 的调用次数
    std::uint64_t flags_materialize_stores = 0;  // 其中**真的发了 IR** 的（有 lazy 位 ⇒ 一次 store）
    std::uint64_t flags_deliver[6] = {0, 0, 0, 0, 0, 0};  // 按位 deliverBit 次数（cf..of）
    std::uint64_t flags_kill = 0;                // killBit 次数（死位；S3-a/b 应恒 0）
    std::uint64_t flags_fused = 0;               // 融合读者总数（jcc+setcc+cmovcc）
    std::uint64_t flags_fused_jcc = 0;
    std::uint64_t flags_fused_setcc = 0;
    std::uint64_t flags_fused_cmovcc = 0;
    std::uint64_t flags_fallback_pf = 0;          // 退化：cc ∈ {P,NP}（PF 不是一条 icmp）
    std::uint64_t flags_fallback_multi = 0;       // 退化：需要的位有多个生产者
    std::uint64_t flags_fallback_other_op = 0;    // 退化：最近写者不是 cmp/test
    std::uint64_t flags_fallback_cross_block = 0;  // 退化：块内没有写者（跨块）
    std::uint64_t flags_fallback_not_direct = 0;   // 退化：cmp 的 o/no（OF 不是一条 icmp）
    std::uint64_t flags_fuse_missed = 0;           // 读数：后向扫描说不能融、前向其实能（应恒 0）
    // ★ S3-c：块图上的 flags 活性读数（**生成期确定性计数**，⛔ 不是运行期埋点；只进 `.ll` 头部）。
    //   口径见 `.tmp/aot/s3design/plan.md` §1.4 / §1.8：零 flags 块 / escape 分类 / 块尾物化位数分布。
    std::uint64_t flags_blocks = 0;                // 参与活性计算的块数（= 发射的块数）
    std::uint64_t flags_zero_blocks = 0;           // 零 flags 块：need_out=∅ 且块内没有活写
    std::uint64_t flags_escape_blocks = 0;         // escape（need_out = ALL）的块数
    std::uint64_t flags_escape_ret = 0;            // 其中末条是 `ret`（目标运行期才知道）
    std::uint64_t flags_escape_indirect = 0;       // 其中末条是间接 `jmp`/`call`（目标运行期才知道）
    std::uint64_t flags_escape_no_block = 0;       // 其中"边目标 ∉ 任何块"（shim 桩/合成 EA/dropped/…）
    std::uint64_t flags_edge_no_block = 0;         // 静态边落空（逐条计；应 = 0 之外的都必须可解释）
    std::uint64_t flags_need_in_all = 0;           // need_in == kAllBits 的块数（含屏障的块 + 其前驱）
    std::uint64_t flags_need_out_all = 0;          // need_out == kAllBits 的块数
    std::uint64_t flags_tail_bits[6] = {0, 0, 0, 0, 0, 0};  // 块尾 `need_out` 的按位分布（cf..of）
    // ★ T1c：单指令宿主原语 `lift_one` 的调用点（= 不再 inline 的那些指令，静态站点数）
    std::uint64_t lift_one_sites = 0;
    std::map<std::string, std::uint64_t> lift_one_by_op;  // 按 op 分组（读数用；std::map ⇒ 确定性）
    // ★ 检查点 2 / D49：x87 抬进 IR 的读数（**生成期确定性计数**，⛔ 不是运行期埋点）
    //   `x87_inline_sites` = 发射成普通 LLVM 浮点指令 / 状态机的站点数；
    //   `x87_helper_sites` = 走 `@lift_x87_sem`（超越函数 + fprem/fscale/fxtract/fxam）的站点数。
    //   两者之和 + `lift_one_by_op` 里的 x87 桶 = x87 全族的站点数（覆盖表的分母）。
    std::uint64_t x87_inline_sites = 0;
    std::uint64_t x87_helper_sites = 0;
    bool used_x87_sem = false;
    bool used_tick = false;       // 发过 `@lift_tick`
    bool used_lift_one = false;   // 发过 `@lift_one`
    bool used_note_write = false;
    bool used_ctpop = false;
    bool used_cttz = false;
    bool used_ctlz = false;
    // ★ 2026-11-15（本轮：`lift_one` 调用点 → 0）：停机原语 `@lift_stop` 的站点数
    //   （`Hlt/Int3/Int/Other/Invalid` + `Div/Idiv` 的 `#DE`）。`used_lift_stop` 决定发不发 `declare`。
    bool used_lift_stop = false;
    std::uint64_t lift_stop_sites = 0;
    // ★ 同轮：MMX/SSE / 串操作 / 乘除 的内联站点数（生成期确定性读数）
    std::uint64_t packed_sites = 0;
    std::uint64_t sse_sites = 0;
    std::uint64_t string_sites = 0;
    std::uint64_t imul_sites = 0;
    std::uint64_t div_sites = 0;
    std::uint64_t rotate_sites = 0;
    std::uint64_t shxd_sites = 0;
    std::uint64_t cmpxchg_sites = 0;
    bool used_ldexp = false;
    bool used_ctlz64 = false;
    // ★ S7（D53）读数：**按 `.ll` 里实际发射的站点**分类（⛔ 不是估值）。口径见 `lift_struct.h`。
    //   ret：在可结构化单元里 ⇒ 宿主 `ret`（返回给宿主调用者）；否则 ⇒ 回派发器（`ret i32`）。
    std::uint64_t s7_ret_host = 0;
    std::uint64_t s7_ret_dispatch = 0;
    std::uint64_t s7_call_host = 0;       // 静态目标 + 可结构化 caller/callee ⇒ 宿主 `call`
    std::uint64_t s7_call_lift_dyn = 0;   // 间接 `call reg/mem` ⇒ `lift_dyn` 尾调用
    std::uint64_t s7_call_shim = 0;       // 目标 = shim 桩 ⇒ 保持今天的 `ret i32`（派发）
    std::uint64_t s7_call_dispatch = 0;   // 其余 ⇒ 保持今天的 `ret i32`（派发）
    std::uint64_t s7_jmp_tail = 0;        // 跨单元 `jmp` + 目标可结构化 ⇒ 尾调用
    std::uint64_t s7_jmp_lift_dyn = 0;    // 间接 `jmp` ⇒ `lift_dyn` 尾调用
    std::uint64_t s7_jmp_dispatch = 0;    // 跨单元 `jmp` + 目标不可结构化 ⇒ `ret i32`
    std::uint64_t s7_tail_calls = 0;      // 所有 `tail call` 站点（jmp 尾调用 + lift_dyn）
    bool used_lift_dyn = false;
};

// =====================================================================================
// ★ 本单：AOT `.ll` 里每个 `define i32 @lift_cc_*` 的**函数属性**（`"target-cpu"` /
//   `"target-features"`）—— 与运行库 TU 对齐的那一份。
//
//   为什么需要它（⛔ 不是"让 codegen 用上更多指令"）：
//   · `clang -###` 摊开看，`.ll` 输入与 C++ 输入的 codegen 参数**逐项相同**（同一 `-target-cpu`
//     + 同一串 feature）⇒ **codegen 早就一致**，差的只是**函数属性**这一层。
//   · LLVM 的 LTO 内联判据 `areInlineCompatible()` 比的是**函数属性**（callee 的 feature 必须是
//     caller 的子集）：C++ 前端会把 CPU 名写进每个函数的属性，而本生成器过去一条都不写
//     ⇒ 一侧缺属性 = 不相容 ⇒ `lift_tick` / `lift_note_write` 一条都内联不进 AOT。
//   · 给 `.ll` 的 driver 加 `-mcpu=...` **无效**（实测产物逐字节相同）：`-mcpu` 只影响 codegen，
//     ⛔ 不会给函数补属性。
//   · 补属性是 **codegen 中性** 的：手工把 C++ TU 的属性串注进 part00 后 `.obj` 与默认逐字节相同
//     （`.tmp/codegen/report.md`）⇒ 本单的判据就是"8 个 obj 的 sha256 逐一不变"。
//
//   ★ **取值唯一来源** = configure 期的 clang 探针（`CMakeLists.txt` 的 `ir_target_probe.c` →
//     `lift-gen --ir-target-cpu/--ir-target-features`）。⛔ 不写死 `apple-m1`：win 上是
//     `x86-64` + 空 feature 集，写死就等于把 mac 的裁决带进别处。
//   ★ **两者任一为空 ⇒ 输出必须逐字节等于今天**（默认构造的 POD 走的就是这条路）：
//     既有 C++ 测试 / `--check` / golden 夹具都直接调这些 API，⛔ 不能因为"顺手对齐一下"变字节。
struct GenIrTarget {
    std::string cpu;
    std::string features;
    // =================================================================================
    // ★ `--note-write`（CMake `LIFT_AOT_NOTE_WRITE`，**缺省 false**）：是否发射
    //   `lift_note_write`（SMC 失效记账）。⛔ 缺省**不发** —— Release / RelWithDebInfo 里它是纯税：
    //   AOT 臂的 `cpu.cache == nullptr`（`aot_host.cpp:88`）⇒ `lift_note_write` 是 no-op，
    //   而每个 guest store 之前都要为它做一次 `syncBeforeHostCall()`（= `flushTick() + flushRegs()`），
    //   默认关时那是一笔**纯税**。实测（2026-11-20 / win32 / shape=split8-O2-lto-abi，同一份闭包）：
    //   note 站点 39,644；关掉后 `.ll` 里 `store i32 … align 4`（`%st` 回写）202,937 → 187,604（−7.6%）、
    //   `call void @lift_tick` 107,827 → 74,375、AOT 可执行体 `__text` 15,126,710 → 12,760,422（−15.6%）。
    //   只在 debug / 诊断（`LIFT_WATCH` 观测 SMC）构建里打开。
    //   ★ false ⇒ **不发** `lift_note_write` call / `declare`、不置 `stats_->used_note_write`，
    //     也**不发** `syncBeforeHostCall()`（纯 guest store 不是宿主调用）。
    //   ★ true ⇒ 与今天逐字节一致（debug 诊断保留）。
    bool note_write = false;
    // 两个都非空才发属性（见上：任一为空 ⇒ 今天的字节）。
    bool empty() const { return cpu.empty() || features.empty(); }
};

// 把分区结果整份吐成 LLVM IR 文本（单模块）。
// `entry_set` = 全部分量根 ∪ 全部 resume EA（决定"目标是不是入口 EA ⇒ ret i32"）。
// `dropped_blocks` = 被丢弃的派生 leader（D10 P3）：它们**没有块**，所以任何指向它们的边
//   一律发 `ret i32 <EA>`（交回 trampoline ⇒ 解释器裁决），⛔ 不发 `br`（那会指向不存在的标签）。
// `closure` = 静态调用闭包的读数（D14），只写进 `.ll` 头部注释。
// 返回 false 只表示生成器内部不一致（不是 guest 代码的问题）。
bool emitModule(const std::vector<GenComponent>& components,
                const std::set<std::uint32_t>& entry_set,
                const std::set<std::uint32_t>& dropped_blocks, const GenClosureStats& closure,
                const GenStructure& structure, std::string* out, GenEmitStats* stats,
                std::vector<GenUnsupported>* unsupported,
                const GenIrTarget& ir_target = GenIrTarget{});

// ★ 拆分输出（`lift-gen --split N`）：**全局分析一次**，只把"输出"按分量分桶。
//   红线的机械形态（design.md §3.2-3）：`entry_owner` / `block_flags` / `structure.structured_roots`
//   全部由**全集**算一次，且走的是与 `emitModule` **同一个** `emitBodies`（⛔ 不许每份重算 ——
//   按份重算会让跨份 `call` 静默退化成 `ret i32`、并改变 flags 物化）。
//   `parts[i]` = 第 i 份的完整 `.ll` 文本（头部注释 + `declare` + 该份 body）；
//   `part_roots[i]` = 该份里的分量根（升序；各份两两不交、并集 = 全部根）。
//   分桶规则（确定性）：按分量根 EA 升序、贪心填到 `Σbody/N`。
//   `split == 1` ⇒ `parts[0]` 与 `emitModule` 的 `out` **逐字节相同**（同一份实现拼出来的）。
bool emitModuleSplit(const std::vector<GenComponent>& components,
                     const std::set<std::uint32_t>& entry_set,
                     const std::set<std::uint32_t>& dropped_blocks, const GenClosureStats& closure,
                     const GenStructure& structure, std::size_t split, GenEmitStats* stats,
                     std::vector<GenUnsupported>* unsupported, std::vector<std::string>* parts,
                     std::vector<std::vector<std::uint32_t>>* part_roots,
                     const GenIrTarget& ir_target = GenIrTarget{});

}  // namespace aot
}  // namespace lift
