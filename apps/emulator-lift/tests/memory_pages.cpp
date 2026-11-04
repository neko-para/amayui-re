// emulator-lift · guest 页的提交 / 退回（pure：不碰游戏 / LFS）
//
// ★ 盯的是"**宿主页可能比 guest 页大**"这一层（Apple Silicon macOS = 16 KiB，Windows = 4 KiB）：
//   ① 提交一页 4 KiB guest 页之后可读写 —— 宿主页对齐错了，提交直接失败，这里立刻红；
//   ② 退掉一页**不许打掉相邻的已提交页**（16 KiB 宿主上最易踩：PROT_NONE 会波及同宿主页的邻页）；
//   ③ 退掉之后能重新提交并再次使用；
//   ④ `commitZeroed` 真的清零（PE 节 / 栈首次提交依赖它）；
//   ⑤ 提交的**排他上界 = 4 GiB**（最后一页）不被 uint32 截断；
//   ⑥ 堆的 `bytes + 块头` 溢出请求必须返回 0（⛔ 不许切出 size=0 的块再 4 GiB memset）。
// ⚠ 判据里**不写**"退掉一页必须立刻不可访问"：16 KiB 宿主上退不掉半页，那是平台事实，不是缺陷
//   （见 src/runtime/memory.cpp 的 GuestMemory::decommit）。
#include "check.h"
#include "runtime/heap.h"
#include "runtime/memory.h"

int main() {
    using namespace lift_test;
    using lift::GuestMemory;
    using lift::RegionKind;

    constexpr std::uint32_t kA = 0x00401000u;
    constexpr std::uint32_t kB = 0x00402000u;
    constexpr std::uint32_t kC = 0x00403000u;

    lift::GuestMemory m;
    if (!m.reserved()) {
        std::fprintf(stderr, "GuestMemory 预留失败：%s\n", m.lastError());
        return 1;
    }

    g_current_case = "commit-write-read";
    LIFT_CHECK(m.commit(kA, GuestMemory::kPage, RegionKind::Heap, "a"));
    LIFT_CHECK(m.committed(kA, 4));
    m.wr32(kA, 0x12345678u);
    LIFT_CHECK_EQ_U32(m.rd32(kA), 0x12345678u);

    g_current_case = "neighbour-survives-decommit";
    LIFT_CHECK(m.commit(kB, GuestMemory::kPage, RegionKind::Heap, "b"));
    m.wr32(kB, 0xA5A5A5A5u);
    m.decommit(kA, GuestMemory::kPage);
    // ★ 这一条在 16 KiB 宿主上就是回归判据：旧实现把 A 的 4 KiB 对齐 mprotect 到宿主页上，
    //   会连 B 一起打掉 ⇒ 下面这行读 B 直接 SIGSEGV（响亮失败），或者位图说已提交但实际不可读。
    LIFT_CHECK(m.committed(kB, 4));
    LIFT_CHECK_EQ_U32(m.rd32(kB), 0xA5A5A5A5u);

    g_current_case = "recommit-after-decommit";
    LIFT_CHECK(m.commit(kA, GuestMemory::kPage, RegionKind::Heap, "a2"));
    m.wr32(kA, 0x0BADF00Du);
    LIFT_CHECK_EQ_U32(m.rd32(kA), 0x0BADF00Du);

    g_current_case = "commitZeroed-clears";
    LIFT_CHECK(m.commit(kC, GuestMemory::kPage, RegionKind::Heap, "c"));
    m.wr32(kC, 0xFFFFFFFFu);
    LIFT_CHECK(m.commitZeroed(kC, GuestMemory::kPage, RegionKind::Heap, "c0"));
    LIFT_CHECK_EQ_U32(m.rd32(kC), 0u);

    g_current_case = "committed-bytes";
    LIFT_CHECK(m.committedBytes() >= 3ull * GuestMemory::kPage);

    // ⑦ 跨宿主页的"整页 decommit"：kA/kB/kC 同属一个 16 KiB 宿主页 ⇒ 单页 decommit 在 16 KiB 上
    //   本来就是 no-op（保守口径）。这里用两个宿主页，让 decommit 真的发生：
    //   退掉 [0x400000,0x404000) 后，下一页 0x404000 必须还活着（且内容不变）。
    g_current_case = "decommit-whole-host-page";
    constexpr std::uint32_t kD = 0x00400000u;  // 16 KiB 对齐
    constexpr std::uint32_t kE = 0x00404000u;  // 下一个宿主页
    LIFT_CHECK(m.commit(kD, GuestMemory::kPage, RegionKind::Heap, "d"));
    LIFT_CHECK(m.commit(kE, GuestMemory::kPage, RegionKind::Heap, "e"));
    m.wr32(kD, 0x11111111u);
    m.wr32(kE, 0x5A5A5A5Au);
    m.decommit(kD, 0x4000u);
    LIFT_CHECK(!m.committed(kD, 1));
    LIFT_CHECK(m.committed(kE, 4));
    LIFT_CHECK_EQ_U32(m.rd32(kE), 0x5A5A5A5Au);

    // ⑤ 最后一页：commit 的排他上界 = 4 GiB。旧实现把它截成 uint32 0 ⇒ `begin >= end` 直接
    //   返回 true，于是"提交成功"但一个字节都没提交（位图也说未提交）。
    g_current_case = "commit-last-page";
    constexpr std::uint32_t kLast = static_cast<std::uint32_t>(GuestMemory::kSpace - GuestMemory::kPage);
    LIFT_CHECK(m.commit(kLast, GuestMemory::kPage, RegionKind::Other, ""));
    LIFT_CHECK(m.committed(kLast, 4));
    m.wr32(kLast, 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(m.rd32(kLast), 0xDEADBEEFu);
    // ⛔ 不断言"退掉后必须不可访问"：16 KiB 宿主上最后一页与前一页共用宿主页 ⇒ 退不掉（平台事实）。
    m.decommit(kLast, GuestMemory::kPage);

    // ⑥ 堆请求溢出：bytes + header 在 uint32 里回绕 ⇒ 旧实现切出 size=0 的块并**返回非空指针**；
    //   ZERO_MEMORY 分支再按 `b.size - header` 回绕成 4 GiB memset。这里必须返回 0。
    g_current_case = "heap-request-overflow";
    lift::GuestHeap h;
    LIFT_CHECK(h.init(&m, 0x30000000u, 0x00100000u));
    LIFT_CHECK_EQ_U32(h.alloc(0xFFFFFFF8u, false), 0u);
    LIFT_CHECK_EQ_U32(h.alloc(0xFFFFFFF8u, true), 0u);
    LIFT_CHECK_EQ_U32(h.alloc(0xFFFFFFFFu, true), 0u);
    LIFT_CHECK(h.lastError()[0] != 0);
    // 正常分配 / 释放仍工作（新检查不许把整条路径掐掉）
    const std::uint32_t ok = h.alloc(16u, true);
    LIFT_CHECK(ok != 0u);
    LIFT_CHECK_EQ_U32(h.sizeOf(ok), 16u);
    LIFT_CHECK(h.free(ok));

    // realloc 同一条：旧实现 need=0 会误判"现有块够大"，再按 `bytes - old_req` 写 4 GiB；
    // 失败必须返回 0 且**不动原块**。
    g_current_case = "heap-realloc-overflow";
    const std::uint32_t p = h.alloc(8u);
    LIFT_CHECK(p != 0u);
    LIFT_CHECK_EQ_U32(h.realloc(p, 0xFFFFFFF8u, true), 0u);
    LIFT_CHECK_EQ_U32(h.sizeOf(p), 8u);
    LIFT_CHECK(h.free(p));

    return finish("memory-pages");
}
