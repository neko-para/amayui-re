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

struct GenEmitStats {
    std::uint64_t traps_unsupported = 0;  // 未实现 op 的 `llvm.trap`（T1c 起应恒为 0）
    std::uint64_t traps_bad = 0;          // 每个入口函数 switch 的 `bad:` 一个 `llvm.trap`
    // ★ T1c：单指令宿主原语 `lift_one` 的调用点（= 不再 inline 的那些指令，静态站点数）
    std::uint64_t lift_one_sites = 0;
    std::map<std::string, std::uint64_t> lift_one_by_op;  // 按 op 分组（读数用；std::map ⇒ 确定性）
    bool used_tick = false;       // 发过 `@lift_tick`
    bool used_lift_one = false;   // 发过 `@lift_one`
    bool used_note_write = false;
    bool used_ctpop = false;
    bool used_cttz = false;
    bool used_ctlz = false;
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
                std::string* out, GenEmitStats* stats,
                std::vector<GenUnsupported>* unsupported);

}  // namespace aot
}  // namespace lift
