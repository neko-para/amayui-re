// emulator-lift · guest 页的提交 / 退回（pure：不碰游戏 / LFS）
//
// ★ 盯的是"**宿主页可能比 guest 页大**"这一层（Apple Silicon macOS = 16 KiB，Windows = 4 KiB）：
//   ① 提交一页 4 KiB guest 页之后可读写 —— 宿主页对齐错了，提交直接失败，这里立刻红；
//   ② 退掉一页**不许打掉相邻的已提交页**（16 KiB 宿主上最易踩：PROT_NONE 会波及同宿主页的邻页）；
//   ③ 退掉之后能重新提交并再次使用；
//   ④ `commitZeroed` 真的清零（PE 节 / 栈首次提交依赖它）。
// ⚠ 判据里**不写**"退掉一页必须立刻不可访问"：16 KiB 宿主上退不掉半页，那是平台事实，不是缺陷
//   （见 src/runtime/memory.cpp 的 GuestMemory::decommit）。
#include "check.h"
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

    return finish("memory-pages");
}
