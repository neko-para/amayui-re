// emulator-lift · Shape A / T2b：**AOT 的派发表**（`EA → LiftFn`）与构建期生成物的身份
//
// ★ 为什么需要它：lifted 代码的入口是**入口 EA**（分量根 ∪ 每条 `call` 的下一条 EA），一个 `.ll`
//   函数体里用 `switch i32 %ea` 分派到多个 entry label（见 `lift_gen_emit.cpp` 的 `FnEmit::run`）。
//   trampoline 手里只有"下一个 guest EA"，所以必须有一张 `EA → 函数指针` 的表。
//
// ★ 表的真源是**生成器**（`lift-gen --table`），⛔ 不是手写清单：
//   `out/aot/lift_table.inc` 一行一个入口 EA → 它所在分量的 `lift_cc_<根>`。那份 `.inc` 在本 TU
//   用**两个不同的宏**展开两次（声明 + 表）⇒ "声明了但表里没有" / "表里有但没声明" 都是编译错。
//
// ★ **查找形态 = 开放寻址哈希表**（D28，2026-11-15）：`.inc` 的展开结果仍是"升序 EA 列表"，
//   本 TU 在**首次查找时**把它摊成一张 `EA → LiftFn` 的线性探测表（槽数 = 最小的 2 的幂 ≥ 2×表长）。
//   ⛔ 生成器侧一个字节都不改（`.inc` 的格式与内容不变）；⛔ 不改"不在表里 ⇒ 响亮停机"的语义。
//   读数见 `DispatchStats` + `printProbeLine`（命中探测次数 / 装载因子 / 槽数）。
//   ★ 为什么换：二分查找平均 14.11 次**串行依赖 load**（每次都要等上一次比较出结果），
//     在"块只有 10.6 条指令"的加载/渲染段占每指令成本的第一位（D28 读数）。
//
// ★ 表里**只许有真实存在的入口**：每一行都在对应函数体的 `switch i32 %ea` 里有 label。
//   **不在**表里的 EA ⇒ 调用方响亮停机并打印 EA（D10 P4：⛔ 不设"返回哨兵 = 结束"的约定，
//   `0xFFFFFFFF` / `0xFFFFFFFE` 因此自然成为响亮错，而不是某种 out-of-band 结局）。
//
// ★ 构建期生成物**可以不在场**（缺 clang / 缺执行源 / 缺 `closure.txt`）：那时这张表是空的，
//   `generatedPresent()` 返回 false，AOT 启动时**响亮报告"没有生成物"**（⛔ 不许静默跑成空壳）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "runtime/lift_abi.h"

namespace lift {
namespace aot {

// 一条派发表项。`ea` = 入口 EA（guest 地址），`fn` = 该 EA 所在分量的 `lift_cc_<根>`。
struct AotEntry {
    std::uint32_t ea;
    LiftFn fn;
};

// 构建期生成物是否在场（= CMake 真的把 `lift_aot.obj` + `lift_pin.h` + `lift_table.inc` 给了本 exe）。
bool generatedPresent();

// 生成器写下的**所读 PE** 的身份（D1）。没有生成物时：空串 / 0。
const char* pinnedSha256();
std::uint32_t pinnedEntry();
// 生成器报的 roots / entries / functions（读数用；没有生成物时全 0）。
std::uint64_t pinnedRoots();
std::uint64_t pinnedEntries();
std::uint64_t pinnedFunctions();

// 表长（= 入口 EA 数）。
std::size_t tableSize();

// EA → LiftFn。返回 nullptr = **不在表里**（调用方必须响亮停机，⛔ 不返回哨兵函数）。
// `comparisons`（可空）= 这一次查找做了几次**关键字比较**（哈希表里 = 探测次数：每次探测比较一次
//   槽里的 EA；⛔ 那是"查找成本"的读数，不是二分那种 log2 比较）。
LiftFn lookup(std::uint32_t ea, std::uint64_t* comparisons);

// ---- 哈希表的几何（读数用；⛔ 不是调用方该依赖的实现细节）----
std::size_t tableCapacity();          // 槽数（2 的幂；没有生成物时 0）
double tableLoadFactor();             // 表长 / 槽数（没有生成物时 0）
unsigned tableBuildMaxProbes();       // 建表时实测的**最坏插入探测次数**（表已建好后才有效）

// 查找成本 / 命中率的计数（跑到停为止；`resetDispatchStats` 在每次 run 开头）。
struct DispatchStats {
    std::uint64_t lookups = 0;      // 走了表查找的 EA 数（= 非 shim 分派次数）
    std::uint64_t hits = 0;         // 查到
    std::uint64_t misses = 0;       // 查不到（响亮停机的那一次也计入）
    std::uint64_t comparisons = 0;  // 关键字比较次数总和（哈希表：= 探测次数总和）
    std::uint64_t max_probes = 0;   // 单次查找的最坏探测次数（**运行期实测**，与建表那个分开）
};
void resetDispatchStats();
DispatchStats dispatchStats();

// 一行**新增读数**（D28）：`probes avg / max | table cap / load factor`。
// ★ 为什么单独一行、而不是塞进既有那行 `table :`：既有统计行的落点是 `aot_host.cpp`（本轮不动它），
//   本轮的改动面按任务书限定在 `aot_dispatch.*` + `aot_run.cpp` ⇒ 读数由本 TU 自己出。
void printProbeLine(std::FILE* out);

}  // namespace aot
}  // namespace lift
