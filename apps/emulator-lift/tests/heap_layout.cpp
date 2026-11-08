// emulator-lift · guest 堆的**布局语义**与首次适配/收口的**可观测计数** —— pure 守卫
//
// ★ 为什么有它：`GuestHeap` 的首次适配从"扫整张块表"改成"只扫空闲块索引"，收口从"每次 free
//   扫整张块表"改成"只合并相邻块"。这两条都是**性能**改动，所以必须有人钉住"语义没变"，否则
//   "更快了但分配地址变了 / 未写页不再读 0"这类回归会一路绿着过去。
//
// 守的四件事（每条都钉在"坏了会有什么后果"上）：
//   ① **批量提交后未写页仍读 0**：`GuestMemory::commit` 一整段（不清零）⇒ 段内每个 4 KiB 页
//      读出来必须是 0（guest 读未写页必须读到 0 是模拟器的基本契约）；且**段外**必须仍未提交
//      （`committed()` 为 false ⇒ safe 模式下访问就是 fault —— 越界不能"悄悄变成可读"）。
//   ② **首次适配仍是"偏移最小的那一块"**：A/B/C 三块 → 释放 B → 再申请 ⇒ 必须拿到 B 的地址。
//      （改成 next-fit / best-fit 会在这里红；地址变了就等于 guest 可见行为变了。）
//   ③ **首次适配的比较次数只随空闲块数增长，不随块表规模增长**：造 2000 个已用块 + 1 个空闲块，
//      再申请 500 次 ⇒ 比较次数必须 ≪ 500×块数。改回扫整张块表立刻红。
//   ④ **收口是完整的**：全部释放后必须只剩 1 块、且最大空闲块 = 整堆（⛔ 留下相邻空闲块 =
//      后续大请求会假失败）。
//   ⑤ `HEAP_ZERO_MEMORY` 的清零字节数 = 申请的载荷字节（语义面，不是优化目标）。
#include <cstdint>
#include <cstdio>

#include "check.h"
#include "runtime/heap.h"
#include "runtime/memory.h"

int main() {
    using namespace lift_test;
    using lift::GuestMemory;
    using lift::RegionKind;

    // ---------------------------------------------------------------------------------
    // ① 批量提交（commit，不清零）+ 未写页读 0 + 段外未提交
    // ---------------------------------------------------------------------------------
    g_current_case = "batch-commit-unwritten-zero";
    lift::GuestMemory m;
    if (!m.reserved()) {
        std::fprintf(stderr, "GuestMemory 预留失败：%s\n", m.lastError());
        return 1;
    }
    constexpr std::uint32_t kBatch = 0x00600000u;  // 4 KiB 对齐
    constexpr std::uint32_t kPages = 64;           // 一次提交 256 KiB（"批量"而不是一页一页）
    constexpr std::uint32_t kBatchBytes = kPages * GuestMemory::kPage;
    LIFT_CHECK(m.commit(kBatch, kBatchBytes, RegionKind::Heap, "batch"));
    // 段内**一个字节都没写过** ⇒ 每一页都必须读 0（宿主 MEM_COMMIT 的页是零页）。
    for (std::uint32_t i = 0; i < kPages; ++i) {
        const std::uint32_t ea = kBatch + i * GuestMemory::kPage;
        LIFT_CHECK(m.committed(ea, 4));
        LIFT_CHECK_EQ_U32(m.rd32(ea), 0u);
        LIFT_CHECK_EQ_U32(m.rd32(ea + GuestMemory::kPage - 4u), 0u);
    }
    // 段外（紧跟批量段之后的一页）必须**仍未提交** ⇒ 越界访问会 fault，不会"顺手可读"。
    const std::uint32_t kAfter = kBatch + kBatchBytes;
    LIFT_CHECK(!m.committed(kAfter, 1));
    LIFT_CHECK(!m.committed(kBatch - 1u, 1));
    // 写过的页照旧可读（批量提交不许把写路径弄坏）。
    m.wr32(kBatch + 3u * GuestMemory::kPage, 0x11223344u);
    LIFT_CHECK_EQ_U32(m.rd32(kBatch + 3u * GuestMemory::kPage), 0x11223344u);

    // ---------------------------------------------------------------------------------
    // ②③④⑤ 堆：首次适配的最低偏移 / 比较次数的量级 / 收口完整性 / 清零字节
    // ---------------------------------------------------------------------------------
    constexpr std::uint32_t kHeapBase = 0x30000000u;
    constexpr std::uint32_t kHeapSize = 0x00100000u;  // 1 MiB
    lift::GuestHeap h;
    g_current_case = "heap-init";
    LIFT_CHECK(h.init(&m, kHeapBase, kHeapSize));

    g_current_case = "first-fit-lowest-offset";
    const std::uint32_t a = h.alloc(32u);
    const std::uint32_t b = h.alloc(32u);
    const std::uint32_t c = h.alloc(32u);
    LIFT_CHECK(a != 0u && b != 0u && c != 0u);
    LIFT_CHECK(a < b && b < c);
    LIFT_CHECK(h.free(b));
    const std::uint32_t d = h.alloc(32u);
    // ★ 关键判据：首次适配从**最低偏移**开始 ⇒ 刚释放的 b 必须被重新用上。
    LIFT_CHECK_EQ_U32(d, b);
    LIFT_CHECK(h.free(a));
    LIFT_CHECK(h.free(c));
    LIFT_CHECK(h.free(d));

    g_current_case = "coalesce-whole-heap";
    // 全部释放后必须并回**一块**（⛔ 留下相邻空闲块 ⇒ 后续大请求假失败）。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h.blockCount()), 1u);
    LIFT_CHECK_EQ_U32(h.largestFreeBlock(), kHeapSize);

    g_current_case = "first-fit-scan-bounded";
    // 造 2000 个已用块，再释放正中间那块（⇒ 块表 2000 个节点、空闲块 2 个）。
    constexpr std::uint32_t kUsed = 2000u;
    std::uint32_t ptrs[kUsed] = {};
    for (std::uint32_t i = 0; i < kUsed; ++i) {
        ptrs[i] = h.alloc(16u);
        LIFT_CHECK(ptrs[i] != 0u);
    }
    LIFT_CHECK(h.free(ptrs[kUsed / 2u]));
    const std::uint64_t scans_before = h.firstFitScans();
    constexpr std::uint32_t kRounds = 500u;
    for (std::uint32_t i = 0; i < kRounds; ++i) {
        const std::uint32_t p = h.alloc(16u);
        LIFT_CHECK(p != 0u);
        LIFT_CHECK(h.free(p));  // 放回去 ⇒ 下一轮仍然要再找一次
    }
    const std::uint64_t scanned = h.firstFitScans() - scans_before;
    std::printf("first-fit: %u allocs over %zu blocks ⇒ %llu block comparisons\n", kRounds, h.blockCount(),
                (unsigned long long)scanned);
    // ★ 判据的量级：只允许"每个空闲块至多比较一次 × 每轮"这个量级（外加常数余量）。
    //   改回"扫整张块表"⇒ 每轮 ≈ 块数（2000）⇒ scanned ≈ 1e6，这条立刻红。
    LIFT_CHECK(scanned <= static_cast<std::uint64_t>(kRounds) * 8ull);
    for (std::uint32_t i = 0; i < kUsed; ++i) {
        if (i == kUsed / 2u) continue;  // 那一条已经释放过（本用例不是 double-free 用例）
        if (ptrs[i] != 0u) LIFT_CHECK(h.free(ptrs[i]));
    }
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h.blockCount()), 1u);

    g_current_case = "zeroed-alloc-clears";
    const std::uint64_t zero_before = h.zeroBytes();
    // 先在堆里留点脏数据，再要一块 ZERO_MEMORY 的（必须真的清零）。
    const std::uint32_t dirt = h.alloc(256u, false);
    LIFT_CHECK(dirt != 0u);
    for (std::uint32_t i = 0; i < 256u; i += 4u) m.wr32(dirt + i, 0xA5A5A5A5u);
    LIFT_CHECK(h.free(dirt));
    const std::uint32_t clean = h.alloc(256u, true);
    LIFT_CHECK(clean != 0u);
    LIFT_CHECK_EQ_U32(m.rd32(clean), 0u);
    LIFT_CHECK_EQ_U32(m.rd32(clean + 252u), 0u);
    LIFT_CHECK(h.zeroBytes() - zero_before >= 256ull);
    LIFT_CHECK(h.free(clean));

    return finish("heap-layout");
}
