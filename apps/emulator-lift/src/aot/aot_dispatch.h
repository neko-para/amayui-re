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

// ★ 构建期库内建表（`aot/intrinsics.txt` 的已校验内容；`lift_pin.h` 的 X-macro 列表）。
//   只给**一次性启动 readout** 用：这一趟 AOT 带了哪几个内建 EA。⛔ 运行期不读表文件。
struct AotIntrinsic {
    std::uint32_t ea = 0;
    const char* name = "";
};
std::size_t pinnedIntrinsicCount();
AotIntrinsic pinnedIntrinsic(std::size_t index);

// ★ S0（缺陷 3）：这一趟构建**实际注入**的 `lift_instr_hook` 站点数
//   （`lift_pin.h` 的 `LIFT_AOT_PIN_INSTR_HOOK_SITES`；没有生成物 ⇒ 0）。
//   0 = `--no-s7` 或闭包没覆盖脚本派发 EA ⇒ `ctx.hook.*` 永不命中。
//   ★ 它只被 `ensureDispatchTable()` 消费一次（`instrHookSetInjectionSites`），⛔ 不是运行期读数。
std::uint32_t pinnedInstrHookSites();

// 表长（= 入口 EA 数）。
std::size_t tableSize();

// ★ 预热派发表（**主线程**调，起第二条线程之前）：把"第一次摊表"显式钉在起 feeder 之前，
//   而不是依赖"主线程恰好先查过一次"的时序。
//   · 摊表本身已经是 `std::call_once`（feeder 的 IR 执行器也会查找，见 .cpp 的线程口径注记）；
//   · 本函数**不写任何计数**（它不是一次查找，⛔ 不要实现成 `lookup(0)` —— 那会污染
//     `probe :` / `table :` 的读数）。
//   ★ S0（缺陷 3）：它还负责把"注入面是否存在"带给插件 runtime
//     （`instrHookSetInjectionSites(pinnedInstrHookSites())`）—— 这是 AOT 唯一的初始化路径，
//     `lift_pin.h` 也在本 TU 里（⛔ 不要在别处再解析一次 pin）。
void ensureDispatchTable();

// ★ 前置声明：`lookup` 的形参是它的引用（定义在下面，调用方只需引用 ⇒ 不需要完整类型）。
struct DispatchStats;

// EA → LiftFn。返回 nullptr = **不在表里**（调用方必须响亮停机，⛔ 不返回哨兵函数）。
// ★ **`stats` 是必填的引用**（P2 §3.2）：统计是**每线程一份**（`thread_local DispatchStats`：feeder
//   的查找不写主线程的账，也不并发写同一份非原子计数）⇒ 那个 thread_local 的地址由**调用方每轮取一次**
//   （`dispatchStatsRef()`），⛔ 不在每次查找里重取。热路径（`AotHost::run` 的 7e8 次）只付一次
//   TLS 访问 / run；冷调用方用下面的便利重载（每次付一次，与改动前等价）。
// `comparisons`（可空）= 这一次查找做了几次**关键字比较**（哈希表里 = 探测次数：每次探测比较一次
//   槽里的 EA；⛔ 那是"查找成本"的读数，不是二分那种 log2 比较）。
//   ★ **nullptr = 本次不记账**（`stats.comparisons` / `stats.max_probes` 都不动）：这两个字段是
//     profile 归因读数，只有 `--profile` 开档（`prof_.on`）才有人读 ⇒ 缺省档一次都不写。
//     `lookups` / `hits` / `misses` **永远记**（语义指纹里有 `table misses`）。
LiftFn lookup(std::uint32_t ea, DispatchStats& stats, std::uint64_t* comparisons);

// ★ S7（D53）：**同一条查找路径、但不计 trampoline 的派发账**。给生成代码的 `lift_dyn` 用
//   （间接 `jmp` / `call reg|mem` 的尾调用）：那一次查找**不是**一次派发（没有回主循环），
//   若记进 `DispatchStats` 会把 `table lookups/misses` 的口径弄脏（间接跳转的目标很大一类是
//   shim 桩 EA ⇒ 每次都会记一个 "miss"，而旧口径的 `misses=0` 是六项语义指纹之一）。
//   ⇒ 它只记自己的 `lift_dyn_lookups`（读数，见 `DispatchStats`）。
//   ⚠ 语义与 `lookup` **逐字相同**（同一个哈希槽数组、同一套探测、同一份 miss 语义）。
LiftFn lookupUncounted(std::uint32_t ea);

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
    // ★ 上面两个数只在 `lookup` 的 `comparisons != nullptr`（= `--profile` 开档）时才记。
    //   这一位回答"本轮到底记没记"：缺省档是 **0 次记账，不是 0 次比较** ⇒ 消费方打 `n/a`，
    //   ⛔ 不许把"没记"印成 "0"（假读数）。`lookups/hits/misses` 的语义不受影响。
    bool comparisons_armed = false;
    // ★ S7：`lift_dyn`（间接 jmp / 间接 call 的尾调用）的查找次数 —— **单独一档**，不混进上面
    //   那四个数（理由见 `lookupUncounted` 的注释）。
    std::uint64_t lift_dyn_lookups = 0;
    std::uint64_t lift_dyn_misses = 0;  // 其中"不是生成函数"的（绝大多数是 shim 桩 EA，交回 trampoline）
};
void resetDispatchStats();
DispatchStats dispatchStats();

// ★ 本线程那份 `DispatchStats` 的**引用**（P2 §3.2）：热路径在循环外取**一次**，再逐次传给 `lookup`。
//   ⛔ 不要在每次查找里调它 —— 那正是要消掉的 `__tls_get_addr`（mac arm64 实测：`adrp/add/ldr/blr`）。
//   取到的引用只对**取它的那个线程**有效（thread_local）；跨线程共享 = 数据竞争。
DispatchStats& dispatchStatsRef();

// 便利重载（冷调用方：测试 / `aot_host.cpp` 的一次性探针）：每次现取一次 TLS 引用。
// ⛔ 热路径（`AotHost::run` 的派发循环）不要用它 —— 用上面那条，把引用提到循环外。
inline LiftFn lookup(std::uint32_t ea, std::uint64_t* comparisons) {
    return lookup(ea, dispatchStatsRef(), comparisons);
}

// 一行**新增读数**（D28）：`probes avg / max | table cap / load factor`。
// ★ 为什么单独一行、而不是塞进既有那行 `table :`：既有统计行的落点是 `aot_host.cpp`（本轮不动它），
//   本轮的改动面按任务书限定在 `aot_dispatch.*` + `aot_run.cpp` ⇒ 读数由本 TU 自己出。
void printProbeLine(std::FILE* out);

}  // namespace aot
}  // namespace lift
