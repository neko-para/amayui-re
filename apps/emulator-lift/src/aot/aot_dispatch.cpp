// emulator-lift · Shape A / T2b：派发表（`EA → LiftFn`）—— 实现
//
// ★ 本 TU 是**生成物唯一的消费点**：`out/aot/lift_table.inc` 在这里被展开两次。
//   生成物由 `lift-gen --table` 产出（形态与理由见 `lift_gen.cpp` 的 `writeAotTable` 头注）。
// ⛔ 不在这里手写任何 EA：手写清单与 `.ll` 的 `switch` label 一漂，派发就会落到 `bad:`（`llvm.trap`
//   = 进程当场死），而那是**生成器不一致**的形状，不该由人肉维护。
//
// ★ D28（2026-11-15）：查找从"升序数组 + 二分"换成**开放寻址哈希表**（线性探测，`EA → LiftFn`）。
//   读数口径见 `D28`：二分在加载/渲染段是 0.094 次/条 × 14.11 次**串行依赖比较** = 1.33 次比较/条，
//   而 14 次比较是 14 次**必须等上一次 load 回来**才能算下一次下标的依赖链。
//   · 表在**首次查找时**从同一份 `LIFT_AOT_ENTRY` 展开结果（`kTable`，升序）摊进哈希槽，
//     ⛔ 生成器侧零改动（`.inc` 的格式与内容一个字节都不变），⛔ 没有第二份 EA 清单。
//   · 几何：槽数 = 最小的 2 的幂 且 ≥ 2×表长 ⇒ **装载因子 ≤ 0.5**；当前表 33,496 项 ⇒ 131,072 槽
//     （实测装载因子 0.256、平均 1.11 次探测、最坏 7 次；`probes avg/max` 由运行摘要打）。
//   · 哈希 = **先右移 2 位吃掉指令对齐的低零位，再乘 2^32/φ 取乘积的高位**（Fibonacci hashing）：
//     `slot = ((ea >> 2) * 0x9E3779B1u) >> (32 - 槽位数)`。⛔ 不用 `ea & mask`（入口 EA 是任意
//     指令边界，低 2 位只有 56.7% 为 0 ⇒ 直接取低位会成簇）。
//   · **语义零变化**：命中 ⇒ 返回那个函数指针；不命中 ⇒ 返回 nullptr ⇒ 调用方响亮停机（D10 P4）。
//     ⛔ 没有 fallback、⛔ 没有"哈希没中就去线性扫一遍"。
#include "aot/aot_dispatch.h"

#include <mutex>

namespace lift {
namespace aot {
namespace {

// =====================================================================================
// ① 哈希形态（`constexpr`：槽数 / 哈希函数在**编译期**就是常量，运行期只剩两次 load）
// =====================================================================================
// ★ 常数选 `0x9E3779B1` = round(2^32 / φ)：乘法把输入的每一位扩散到高位，取高位就等于
//   "按黄金分割比例切分 2^32 的圆环" ⇒ 相邻 EA（低 2 位清零后步长 1）落到相差 ~0.618 槽的位置。
constexpr std::uint32_t kHashMul = 0x9E3779B1u;
// ★ 右移 2：入口 EA 是**指令边界**，实测低 2 位只有 **56.7%** 为 0（低 3 位 23.9%、低 4 位 17.9%）。
//   移 2 位不是为了"吃掉全零"，而是把 4 字节粒度内的地址位挪走、让乘法作用在更均匀的位上。
constexpr unsigned kHashShift = 2;

// 槽数 = 最小的 2 的幂 且 ≥ 2 × 表长（⇒ 装载因子 ≤ 0.5；线性探测在 0.5 以下才不结长簇）。
constexpr unsigned slotBitsFor(std::size_t n) {
    unsigned bits = 1;  // 至少 2 槽（空表也有一张合法的表，lookup 才不会读到越界）
    while ((std::size_t{1} << bits) < n * 2) ++bits;
    return bits;
}

#ifdef LIFT_AOT_HAS_GENERATED

// ★ 生成物里的 PE 身份（D1 的判据载体；`lift-gen --pin` 写的 `#define`）
#include "lift_pin.h"

// ---- 展开 ①：把表里出现的每个函数**声明**一遍（`extern "C"`：`.ll` 的符号不带 C++ 修饰名）----
//   ★ 签名必须与 `LiftFn` 逐字相同（`LiftRegs* / uint8_t* / uint32_t / void*` → `uint32_t`）：
//     类型不匹配在这里就是**编译错**，而不是运行期把栈/寄存器读歪。
#define LIFT_AOT_ENTRY(ea, name) \
    extern "C" std::uint32_t name(::LiftRegs*, std::uint8_t*, std::uint32_t, void*);
#include "lift_table.inc"
#undef LIFT_AOT_ENTRY

// ---- 展开 ②：同一份行变成**摊表源**（升序；生成器已按 EA 升序写出）----
//   ★ 它只被"建哈希槽"读一次；⛔ 查找路径不碰它（查找只走下面的 `g_slots`）。
#define LIFT_AOT_ENTRY(ea, name) {ea, &name},
constexpr AotEntry kTable[] = {
#include "lift_table.inc"
};
#undef LIFT_AOT_ENTRY

constexpr std::size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

// ★ 升序是"同输入同字节"的一部分（生成器已保证）⇒ 编译期钉住（不是运行期抽查）。
constexpr bool tableIsSorted() {
    for (std::size_t i = 1; i < kTableSize; ++i) {
        if (!(kTable[i - 1].ea < kTable[i].ea)) return false;
    }
    return true;
}
static_assert(tableIsSorted(), "派发表必须严格升序（lift-gen 已保证；这是防手改 / 防生成器回归）");

// ★ 空槽哨兵用 `ea == 0` ⇒ 前提是"表里没有 EA 0"。升序 ⇒ 只看首项（EA 0 不可执行：PE 入口在 .text）。
static_assert(kTableSize == 0 || kTable[0].ea != 0,
              "空槽哨兵（ea==0）的前提：入口 EA 不许是 0（升序 ⇒ 只看首项）");

constexpr const AotEntry* kTablePtr = kTable;

#else  // !LIFT_AOT_HAS_GENERATED —— 构建期没有生成物：表是空的，**启动时必须响亮报告**

constexpr std::size_t kTableSize = 0;
constexpr const AotEntry* kTablePtr = nullptr;

#endif

// =====================================================================================
// ② 哈希槽（**开放寻址**：`ea == 0` = 空槽，线性探测；⛔ 不做链地址 / ⛔ 不做墓碑）
// =====================================================================================
struct Slot {
    std::uint32_t ea;  // 0 = 空槽（见上面的 static_assert）
    LiftFn fn;         // 空槽 = nullptr（⛔ 不是哨兵函数）
};

constexpr unsigned kSlotBits = slotBitsFor(kTableSize);
constexpr std::size_t kSlotCount = std::size_t{1} << kSlotBits;
constexpr std::size_t kSlotMask = kSlotCount - 1;

constexpr std::size_t slotOf(std::uint32_t ea) {
    // `(ea >> kHashShift) * kHashMul` 是 uint32 的无符号回绕（定义良好）⇒ 取乘积**高** `kSlotBits` 位。
    const std::uint32_t mixed = (ea >> kHashShift) * kHashMul;
    return static_cast<std::size_t>(mixed >> (32 - kSlotBits));
}

Slot g_slots[kSlotCount];     // 静态零初始化 ⇒ 全空槽（BSS；⛔ 不进 exe 的 .rdata，见 D28 的取舍）
unsigned g_buildMaxProbes = 0;

// 首次查找时摊表（一次性；⛔ 不在每次 lookup 里摊销，也⛔ 不"边查边建"）。
// ★ 线程口径（2026-11-17 改）：AOT 的派发现在**不止主线程** —— 音频 feeder 的 IR 执行器
//   （`src/aot/aot_feeder.cpp`）也会调 `lookupUncounted`。⇒ 摊表必须 `std::call_once`：
//   两个线程同时第一次查找时，只有一个摊表，另一个在 `call_once` 上等它完成
//   （`call_once` 的完成与后续调用的返回之间是 happens-before ⇒ 之后读槽数组不需要锁）。
//   ★ 另外 `AotHost::run()` 还会**主动预热**一次（`ensureDispatchTable()`，见下），
//     让"表已建好"发生在起 feeder 线程之前，不依赖"主线程恰好先查过一次"这种时序。
std::once_flag g_slotsOnce;
void buildSlotsOnce() {
    std::call_once(g_slotsOnce, [] {
        std::uint32_t worst = 0;
        for (std::size_t i = 0; i < kTableSize; ++i) {
            const std::uint32_t ea = kTablePtr[i].ea;
            std::size_t h = slotOf(ea);
            std::uint32_t probes = 1;
            while (g_slots[h].ea != 0) {
                h = (h + 1) & kSlotMask;
                ++probes;
            }
            g_slots[h].ea = ea;
            g_slots[h].fn = kTablePtr[i].fn;
            if (probes > worst) worst = probes;
        }
        g_buildMaxProbes = worst;
    });
}

// ★ **每线程一份**的派发统计：feeder 的查找不能并发写主线程的计数（那是数据竞争），
//   也不该把它的查找混进 `probe :` / `table :` 的读数（那些是**主线程**执行循环的归因）。
//   `resetDispatchStats()` / `printProbeLine()` 只作用于调用线程 = 主线程 ⇒ 摘要口径不变。
thread_local DispatchStats g_stats;

}  // namespace

bool generatedPresent() {
#ifdef LIFT_AOT_HAS_GENERATED
    return true;
#else
    return false;
#endif
}

const char* pinnedSha256() {
#ifdef LIFT_AOT_HAS_GENERATED
    return LIFT_AOT_PIN_SHA256;
#else
    return "";
#endif
}

std::uint32_t pinnedEntry() {
#ifdef LIFT_AOT_HAS_GENERATED
    return static_cast<std::uint32_t>(LIFT_AOT_PIN_ENTRY);
#else
    return 0u;
#endif
}

std::uint64_t pinnedRoots() {
#ifdef LIFT_AOT_HAS_GENERATED
    return static_cast<std::uint64_t>(LIFT_AOT_PIN_ROOTS);
#else
    return 0u;
#endif
}

std::uint64_t pinnedEntries() {
#ifdef LIFT_AOT_HAS_GENERATED
    return static_cast<std::uint64_t>(LIFT_AOT_PIN_ENTRIES);
#else
    return 0u;
#endif
}

std::uint64_t pinnedFunctions() {
#ifdef LIFT_AOT_HAS_GENERATED
    return static_cast<std::uint64_t>(LIFT_AOT_PIN_FUNCTIONS);
#else
    return 0u;
#endif
}

std::size_t tableSize() { return kTableSize; }

void ensureDispatchTable() { buildSlotsOnce(); }

std::size_t tableCapacity() { return kTablePtr != nullptr ? kSlotCount : 0; }

double tableLoadFactor() {
    if (kTablePtr == nullptr || kSlotCount == 0) return 0.0;
    return static_cast<double>(kTableSize) / static_cast<double>(kSlotCount);
}

unsigned tableBuildMaxProbes() { return g_buildMaxProbes; }

LiftFn lookup(std::uint32_t ea, DispatchStats& stats, std::uint64_t* comparisons) {
    // ★ P2 §3.2：`stats` 是**调用方在循环外取好的**引用（`dispatchStatsRef()`）⇒ 每派发不再做一次
    //   TLS 访问。`lookups/hits/misses` 永远记（语义指纹里有 `table misses`）；`comparisons/max_probes`
    //   只在调用方要（= `--profile` 开档，`comparisons != nullptr`）时记 —— 它们是归因读数。
    ++stats.lookups;
    std::uint64_t probes = 0;
    LiftFn found = nullptr;
    // ---- 首次使用 ⇒ 摊表（这一次分支在 6,200 万次查找里只有第一次为真，分支预测吃得下）----
    buildSlotsOnce();
    std::size_t h = slotOf(ea);
    for (;;) {
        const Slot& s = g_slots[h];
        ++probes;
        if (s.ea == ea) {
            // ★ 空槽也有 `ea == 0`：只有查询 `ea == 0` 时才会在这里"相等"，而空槽的 `fn` 是 nullptr
            //   ⇒ 仍然返回 nullptr（miss），⛔ 不会把 EA 0 变成"命中一个空函数"。
            found = s.fn;
            break;
        }
        if (s.ea == 0) break;  // 空槽 ⇒ 线性探测链断 ⇒ 一定不在表里
        h = (h + 1) & kSlotMask;
        if (probes >= kSlotCount) break;  // 表满（装载因子 ≤ 0.5，不该发生）⇒ 防御性收口为 miss
    }
    if (comparisons != nullptr) {
        stats.comparisons += probes;
        if (probes > stats.max_probes) stats.max_probes = probes;
        *comparisons = probes;
    }
    if (found != nullptr) {
        ++stats.hits;
    } else {
        ++stats.misses;
    }
    return found;
}

// ★ S7：与 `lookup` **逐字相同**的查找，只是不记 trampoline 的派发账（改记 `lift_dyn_*`）。
//   ⛔ 不要把它实现成"调 lookup 再减计数"：那会同时弄脏 max_probes / comparisons。
LiftFn lookupUncounted(std::uint32_t ea) {
    buildSlotsOnce();
    ++g_stats.lift_dyn_lookups;
    std::size_t h = slotOf(ea);
    LiftFn found = nullptr;
    for (std::size_t probes = 1;; ++probes) {
        const Slot& s = g_slots[h];
        if (s.ea == ea) {
            found = s.fn;
            break;
        }
        if (s.ea == 0) break;
        h = (h + 1) & kSlotMask;
        if (probes >= kSlotCount) break;
    }
    if (found == nullptr) ++g_stats.lift_dyn_misses;
    return found;
}

void resetDispatchStats() { g_stats = DispatchStats{}; }

DispatchStats dispatchStats() { return g_stats; }

DispatchStats& dispatchStatsRef() { return g_stats; }

void printProbeLine(std::FILE* out) {
    if (out == nullptr) return;
    const double avg = g_stats.lookups != 0
                           ? static_cast<double>(g_stats.comparisons) / static_cast<double>(g_stats.lookups)
                           : 0.0;
    std::fprintf(out,
                 "probe     : probes=%llu avg=%.2f max=%llu | table cap=%zu load=%.3f entries=%zu "
                 "build_max=%u | lift_dyn lookups=%llu misses=%llu\n",
                 static_cast<unsigned long long>(g_stats.comparisons), avg,
                 static_cast<unsigned long long>(g_stats.max_probes), tableCapacity(),
                 tableLoadFactor(), kTableSize, g_buildMaxProbes,
                 static_cast<unsigned long long>(g_stats.lift_dyn_lookups),
                 static_cast<unsigned long long>(g_stats.lift_dyn_misses));
}

}  // namespace aot
}  // namespace lift
