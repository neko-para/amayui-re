// emulator-lift · **pure** 并发压力守卫：单令牌（guest_token.h）+ 真实 `GuestHeap`
//
// 红了意味着什么（判据，缺一条都不算绿）：
//   ① 两个宿主线程各做 200000 次 guest 堆 alloc/free，**走真实 `GuestHeap` 与真实令牌**，不崩；
//   ② 结束后 `bytesInUse() == 0`（每次 alloc 都配到一次 free）；
//   ③ `guestToken().violations() == 0`（没有任何一次容器操作发生在令牌之外）；
//   ④ `guestToken().maxConcurrent() == 1`（令牌真的把两条执行流串起来；把它短路的空实现 ⇒ 0）
//   ⑤ 两条线程**直接走真实 `ShimRegistry::invoke`**（不依赖游戏/assets，pure）：shim 边界令牌
//      必须覆盖容器访问 —— 去掉 `invoke` 里的 Guard ⇒ heap check / `shim.invoke` check 当场红。
//
// ★ 为什么这条必须存在：`0xC0000374`（宿主进程堆损坏、WER 无应用帧）的成因就是音频 feeder
//   真线程与主线程并发改 `GuestHeap::blocks_`（`std::map`）。这条把"令牌 + 真实堆 + shim 边界"
//   的并发路径钉进默认 `ctest`（顶层无条件注册 ⇒ pure 档）。
// ★ 判据是确定性的：两条线程都**先取令牌再碰堆** ⇒ 正确实现下 ③④ 恒成立；只有令牌坏了才红。
//   若把 `GuestToken::acquire/release` 改成空操作 ⇒ ③（违规 > 0）与 ④（max=0）**当场红**；
//   而且 `GuestHeap::free` 的 magic/used 双判可能先 `abort()`（同样是红）。
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "check.h"
#include "runtime/guest_token.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/shims.h"
#include "runtime/x86.h"

namespace {
// shim 边界用例的实现：真的碰 GuestHeap（alloc + free）。
void shimHeapRoundtrip(lift::X86& cpu, lift::ShimCall& call) {
    const std::uint32_t p = cpu.heap->alloc(64u, true);
    if (p != 0) cpu.heap->free(p);
    call.result = p;
}
}  // namespace

int main() {
    using namespace lift;
    using namespace lift_test;
    g_current_case = "guest-token/stress";

    GuestMemory mem;
    if (!mem.reserve()) LIFT_SKIP("guest 4 GiB 预留失败（宿主地址空间不足）");

    constexpr std::uint32_t kHeapBegin = 0x30000000u;
    constexpr std::uint32_t kHeapSize = 0x00400000u;  // 4 MiB
    GuestHeap heap;
    LIFT_CHECK(heap.init(&mem, kHeapBegin, kHeapSize));

    GuestToken& token = guestToken();
    token.resetForTest();
    token.arm();

    constexpr int kThreads = 2;
    constexpr int kIters = 200000;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> allocs{0};
    std::atomic<std::uint64_t> frees{0};
    std::atomic<int> failures{0};

    auto worker = [&](int seed) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < kIters; ++i) {
            // 大小混着来（8..508 B）：既压 alloc 的首次适配，也压 free 的合并路径。
            const std::uint32_t bytes = 8u + static_cast<std::uint32_t>((i * 37 + seed * 101) % 501);
            token.acquire();
            const std::uint32_t p = heap.alloc(bytes, (i & 1) != 0);
            if (p == 0) {
                token.release();
                failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            allocs.fetch_add(1, std::memory_order_relaxed);
            const bool freed = heap.free(p);
            token.release();
            if (freed) {
                frees.fetch_add(1, std::memory_order_relaxed);
            } else {
                failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t0(worker, 1);
    std::thread t1(worker, 2);
    while (ready.load(std::memory_order_acquire) != kThreads) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    t0.join();
    t1.join();

    const std::uint64_t expect = static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kIters);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(failures.load()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(allocs.load()), static_cast<std::int64_t>(expect));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(frees.load()), static_cast<std::int64_t>(expect));
    LIFT_CHECK_EQ_U32(heap.bytesInUse(), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.maxConcurrent()), 1u);

    // ---- YieldGuard：**深度 > 1** 的让出/取回（复审指出的零覆盖路径）----
    // 正确语义：YieldGuard 把整套深度**一次放光**（不只是最外层），析构时恢复原深度。
    g_current_case = "guest-token/yield-depth";
    token.acquire();  // depth = 1
    token.acquire();  // depth = 2（重入）
    {
        GuestToken::YieldGuard yield(token);
        LIFT_CHECK(!token.heldByCurrentThread());
        LIFT_CHECK_EQ_U32(token.concurrent(), 0u);  // 真的放开了（不是只减一层）
        // 让出期间别的执行流能拿到令牌（这里用 tryAcquire 模拟；它同时是死码的覆盖）
        LIFT_CHECK(token.tryAcquire());
        token.release();
        LIFT_CHECK(!token.heldByCurrentThread());  // 取回之前仍然是空的
    }
    LIFT_CHECK(token.heldByCurrentThread());       // 析构后恢复 depth
    token.release();                               // depth 2 → 1
    LIFT_CHECK(token.heldByCurrentThread());
    token.release();                               // depth 1 → 0
    LIFT_CHECK(!token.heldByCurrentThread());
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.maxConcurrent()), 1u);  // Yield 不产生假 2

    // ---- shim 边界：两条线程**直接走真实 `ShimRegistry::invoke`**（pure，不依赖 assets）----
    // 最终并发设计的互斥点是"每次 shim 调用"（所有容器访问都在 shim 实现里）。这条用例把
    // "去掉 invoke 里的 Guard"变成**第一次 shim 就红**，而不是等 feeder 恰好碰到 heap。
    g_current_case = "guest-token/shim-invoke";
    constexpr std::uint32_t kStackSlot = 0x40000000u;
    LIFT_CHECK(mem.commit(kStackSlot, 0x1000u, RegionKind::Other, "shim-test-stack"));
    mem.wr32(kStackSlot, 0xFFFFFFFFu);  // invoke() 从 [esp] 读返回地址；不真的跳回去
    ShimRegistry shims;
    ShimDef def;
    def.module = "test";
    def.name = "heap-roundtrip";
    def.conv = Conv::Stdcall;
    def.argc = 0;
    def.fn = &shimHeapRoundtrip;
    X86 cpu_a;
    cpu_a.mem = &mem;
    cpu_a.heap = &heap;
    cpu_a.esp() = kStackSlot;
    X86 cpu_b = cpu_a;  // 每线程一份 X86（invoke 会写 eax/esp/eip）
    constexpr int kShimIters = 20000;
    std::atomic<int> shim_fail{0};
    auto shim_worker = [&](X86& cpu) {
        for (int i = 0; i < kShimIters; ++i) {
            cpu.esp() = kStackSlot;  // invoke 的出口会 `esp += 4`（argc=0）⇒ 每轮重置
            shims.invoke(cpu, def);
            if (cpu.eax() == 0) shim_fail.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread s0([&] { shim_worker(cpu_a); });
    std::thread s1([&] { shim_worker(cpu_b); });
    s0.join();
    s1.join();
    LIFT_CHECK_EQ_I64(shim_fail.load(), 0);
    LIFT_CHECK_EQ_U32(heap.bytesInUse(), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.maxConcurrent()), 1u);

    std::printf("guest-token: threads=%d iters=%d allocs=%llu frees=%llu violations=%llu max-concurrent=%u "
                "in_use=%u shim-invoke=%d\n",
                kThreads, kIters, static_cast<unsigned long long>(allocs.load()),
                static_cast<unsigned long long>(frees.load()),
                static_cast<unsigned long long>(token.violations()), token.maxConcurrent(),
                heap.bytesInUse(), kShimIters * 2);
    return finish("guest-token-stress");
}
