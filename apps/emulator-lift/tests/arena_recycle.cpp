// emulator-lift · surface arena 的**回收 + 空闲块合并**（pure：只碰宿主 `GuestMemory`，不读资产 / 不起 Machine）
//
// ★ 为什么有这条：`runtime/image.h` 把 SurfaceArena 从 128 MiB 抬到 256 MiB 时写明了代价 ——
//   「128 MiB 时代它同时是"漏回收"的意外探测器（BUG-01 就是这么抓到的）—— 抬容量把那个探测器
//   变钝了 ⇒ 换屏 `in_use` 不增长的守卫不能省」。
//   ★ 既有守卫只钉了**一半**（点名，⛔ 不重复造）：
//     · `d3d9/texture-bound-then-guest-release`（`tests/d3d9.cpp`）：设备 + **一张**纹理建了再放，
//       arena 的 `surfaceCount` / `bytesInUse` 必须回到"建设备之前"的基线；
//     · `d3d9/arena-reclaim-under-other-machine`：同上（另一台 Machine 抢走 `g_active` 的局面）；
//     · `ddraw/release-owner-not-active`（`tests/ddraw.cpp`）：设备 / 主 surface 释放后
//       `arena.bytesInUse() == 0`；
//     · `d3dx9/surface-keeps-parent-alive`：surface / 纹理成对释放后 `surfaceCount` / guest 堆回基线。
//   ⇒ 它们都只覆盖"**一张** surface 的 create→release"。**没有**一条钉"**一批不同尺寸**全部释放
//   之后空闲块真的**合并**了（而不是只剩一堆小洞）"。抬容量之后这两种失效的可观测面不同：
//   漏回收 ⇒ `in_use` 不回落；只释放不合并 ⇒ `in_use` 回 0、但下一次大分配静默失败。
//
// 判据（红了意味着什么）：
//   ① 一批不同尺寸 create 之后 `in_use`/`surfaceCount` 都上去了；逐张 release 之后 `in_use`
//      **回到基线**、`surfaceCount() == 0`（漏回收 ⇒ 这一条红）；
//   ② 全部释放之后再要**一张比任何单张都大**的 surface 必须成功 —— 区域刻意只留一小段余量
//      （远小于那张的请求）⇒ **空闲块没合并就一定失败**（`GuestHeap` 的首次适配只认连续块）；
//   ③ 那张再释放之后 `largestFreeBlock()` 回到**刚 init 时**的同一个读数（相对判据 ⇒ 合并真的做完）。
//   ⛔ 不断言任何具体字节数 / 容量 / 地址（那是读数，`AGENTS.md` §10 的文档纪律）：本用例只用
//      `bytesInUse()` / `largestFreeBlock()` / `surfaceCount()` 的**相对**关系与 init 时的读数。
//   ★ 判据的"会红"形态（实测口径见交付）：`SurfaceArena::destroy` 不 `heap_.free` ⇒ ①②红；
//     `GuestHeap::free` 去掉 `coalesce` ⇒ ②③红（`in_use` 照样回 0 ⇒ ①那条**抓不到**它）。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/surface.h"

namespace {

using lift::GuestLayout;
using lift::GuestMemory;
using lift::SurfaceArena;
using lift::SurfaceFormat;

struct Size {
    std::int32_t w;
    std::int32_t h;
};

// 一批**互不相同**的尺寸（覆盖后缓冲 / 图集 / 小 surface；顺序 = 分配顺序）。
constexpr Size kBurst[] = {
    {512, 512}, {640, 480}, {256, 256}, {128, 128}, {320, 240},
};
constexpr std::size_t kBurstCount = sizeof(kBurst) / sizeof(kBurst[0]);

// 全部释放之后再要的"最大一张"：比上面任何一张都大（⇒ 只有合并出来的连续块装得下）。
constexpr Size kBiggest = {1024, 640};

// 32bpp（`SurfaceFormat` 的缺省）⇒ 每像素 4 字节；`create` 的 pitch 按 4 字节对齐（w*4 已对齐）。
constexpr std::uint32_t pixelBytes(const Size& s) {
    return static_cast<std::uint32_t>(s.w) * 4u * static_cast<std::uint32_t>(s.h);
}

}  // namespace

int main() {
    using lift_test::failf;

    GuestMemory mem;
    if (!mem.reserved()) {
        std::fprintf(stderr, "GuestMemory 预留失败：%s\n", mem.lastError());
        return 1;
    }

    // 区域 = 这批的像素总量 + 一条**小**余量：
    //   · 余量 ≫ 块头开销（每块 8 B）⇒ 正常分配都装得下；
    //   · 余量 ≪ 释放后要的那张（`kBiggest`）⇒ 碎片化的洞（每张各自一块）都装不下 ⇒ ② 真的判得动。
    std::uint32_t burst_bytes = 0;
    for (const Size& s : kBurst) burst_bytes += pixelBytes(s);
    constexpr std::uint32_t kSlack = 0x00040000u;  // 256 KiB
    const std::uint32_t region_size = burst_bytes + kSlack;

    SurfaceArena arena;
    lift_test::g_current_case = "arena-recycle/batch-release-then-max-alloc";
    // ★ 起点取布局真源的 arena 基址（⛔ 不在这里抄一份地址字面量）；容量是本用例自己的小窗口。
    LIFT_CHECK(arena.init(&mem, GuestLayout{}.arena_base, region_size));
    const std::uint32_t baseline_in_use = arena.bytesInUse();
    const std::uint32_t baseline_largest = arena.largestFreeBlock();

    // 两遍：第二遍只是在"已经完全回收 / 合并"的 arena 上重放 ⇒ 顺带钉住"换屏多轮之后也不增长"。
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::uint32_t> eas;
        for (const Size& s : kBurst) {
            const std::uint32_t ea = arena.create(s.w, s.h, SurfaceFormat{});
            LIFT_CHECK_NE_U32(ea, 0u);
            if (ea != 0) eas.push_back(ea);
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(arena.surfaceCount()),
                          static_cast<std::uint32_t>(kBurstCount));
        LIFT_CHECK(arena.bytesInUse() > baseline_in_use);  // 前提：这一批确实占了 arena

        // ① 逐张 release ⇒ arena 的账回到基线（漏回收 / 只在最后一次时才回收的洞 ⇒ 这里红）
        for (const std::uint32_t ea : eas) LIFT_CHECK(arena.destroy(ea));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(arena.surfaceCount()), 0u);
        LIFT_CHECK_EQ_U32(arena.bytesInUse(), baseline_in_use);

        // ② 合并判据：要一张**比任何单张都大**的 surface（余量刻意留得很小 ⇒ 不合并必失败）
        const std::uint32_t big = arena.create(kBiggest.w, kBiggest.h, SurfaceFormat{});
        if (big == 0) {
            failf(__FILE__, __LINE__,
                  "第 %d 遍：全部释放之后仍分配不出 %dx%d（%u B）—— 空闲块没有合并（碎片化）；"
                  "最大空闲块 = %u B，in_use = %u B",
                  pass, kBiggest.w, kBiggest.h, pixelBytes(kBiggest), arena.largestFreeBlock(),
                  arena.bytesInUse());
        }
        LIFT_CHECK_NE_U32(big, 0u);
        if (big != 0) LIFT_CHECK(arena.destroy(big));

        // ③ 合并之后"最大空闲块"必须回到 init 时的读数（不等 ⇒ 有一段没并回去）
        LIFT_CHECK_EQ_U32(arena.largestFreeBlock(), baseline_largest);
        LIFT_CHECK_EQ_U32(arena.bytesInUse(), baseline_in_use);
    }

    return lift_test::finish("arena-recycle");
}
