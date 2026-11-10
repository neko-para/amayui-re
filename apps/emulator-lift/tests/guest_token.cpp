// emulator-lift · **pure** 并发压力守卫：单令牌（guest_token.h）+ 真实 `GuestHeap`
//
// 红了意味着什么（判据，缺一条都不算绿）：
//   ① 两个宿主线程各做 200000 次 guest 堆 alloc/free，**走真实 `GuestHeap` 与真实令牌**，不崩；
//   ② 结束后 `bytesInUse() == 0`（每次 alloc 都配到一次 free）；
//   ③ `guestToken().violations() == 0`（没有任何一次容器操作发生在令牌之外）；
//   ④ `guestToken().maxConcurrent() == 1`（令牌真的把两条执行流串起来；把它短路的空实现 ⇒ 0）
//   ⑤ 两条线程**直接走真实 `ShimRegistry::invoke`**（不依赖游戏/assets，pure）：shim 边界令牌
//      必须覆盖容器访问 —— 去掉 `invoke` 里的 Guard ⇒ heap check / `shim.invoke` check 当场红。
//   ⑥ `guest-token/wait-split`：**等令牌**的墙钟从 shim 的 `ns` 里分出来了 —— 真竞争（两条线程争
//      同一个令牌，主线程在窗口里进 shim）下 `waits() > 0` 且 `waitNs()` 很大，而同一个 shim 的
//      `ns`（实现自身）**不跟着变大**。把计时起点挪回 Guard 之前 ⇒ 这条当场红。
//   ⑦ `guest-token/arena-outside`：`SurfaceArena` 的簿记（create/find/destroy/…）也进了令牌检查点 ——
//      持令牌访问**不**计数（负控），令牌之外 `find` ⇒ `violations` ++ 且 first-site = `arena.find`，
//      同时真的往 stderr 喊过一次（一次运行只喊一次）。删掉 `checkToken` ⇒ 这条红。
//   ⑧ `guest-token/yield-revalidate`：Yield 之后**必须重新校验**（`guest_token.h` 的取/放规则）——
//      YieldGuard 窗口里另一条线程真的拿到令牌并改了容器（它 `destroy` 了 surface）；取回后
//      重新 `check()` 通过、且**重新查表**得到"已经没了"（让出前读到的结论作废）。
//      YieldGuard 不真的放开 ⇒ 另一条流在 2 s 窗口里拿不到令牌 ⇒ 这条红。
//
// ★ 为什么这条必须存在：`0xC0000374`（宿主进程堆损坏、WER 无应用帧）的成因就是音频 feeder
//   真线程与主线程并发改 `GuestHeap::blocks_`（`std::map`）。这条把"令牌 + 真实堆 + shim 边界"
//   的并发路径钉进默认 `ctest`（顶层无条件注册 ⇒ pure 档）。
// ★ 判据是确定性的：两条线程都**先取令牌再碰堆** ⇒ 正确实现下 ③④ 恒成立；只有令牌坏了才红。
//   若把 `GuestToken::acquire/release` 改成空操作 ⇒ ③（违规 > 0）与 ④（max=0）**当场红**；
//   而且 `GuestHeap::free` 的 magic/used 双判可能先 `abort()`（同样是红）。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "check.h"
#include "runtime/guest_token.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/shims.h"
#include "runtime/surface.h"
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

    // ---- ⑥ M1（REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9）：**等令牌**的墙钟从 `ns` 里分出来 ----
    // 夹具 = 真竞争：另一条宿主线程持令牌 300 ms，主线程在这个窗口里**进 shim** ⇒ 它的取令牌
    // 一定被挡住。判据两条缺一不可：
    //   ① `waits() > 0` 且 `waitNs()` 真的很大（> 100 ms）—— 记账接上了；
    //   ② 同一个 shim 的 `ns`（口径 = 实现自身，起点在 Guard 之后）**远小于**那段等待。
    // 把 `time_begin` 挪回 Guard 之前 ⇒ ② 红（旧代码正是把等待记进了 shim 的墙钟）。
    g_current_case = "guest-token/wait-split";
    {
        token.resetForTest();
        token.arm();
        ShimRegistry timed;
        timed.setTiming(true);  // 只有 `--profile`/frame-trace 才打开的那条路径，夹具直接打开
        timed.add("test", "timed-heap-roundtrip", Conv::Stdcall, 0, &shimHeapRoundtrip);
        ShimDef* tdef = timed.findMutable("test", "timed-heap-roundtrip");
        LIFT_CHECK(tdef != nullptr);
        const std::uint32_t tdef_id = tdef ? tdef->id : 0u;
        X86 cpu_t;
        cpu_t.mem = &mem;
        cpu_t.heap = &heap;
        cpu_t.esp() = kStackSlot;

        std::atomic<bool> holder_in{false};
        std::atomic<bool> main_entering{false};
        std::thread holder([&] {
            token.acquire();
            holder_in.store(true, std::memory_order_release);
            // 等主线程说"我要进临界区了"再持一段：让它的 `try_lock` 一定撞上（否则夹具会
            // 变成"没竞争"⇒ 断言红得没有意义）。
            while (!main_entering.load(std::memory_order_acquire)) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            token.release();
        });
        while (!holder_in.load(std::memory_order_acquire)) std::this_thread::yield();
        const std::uint64_t waits_before = token.waits();
        main_entering.store(true, std::memory_order_release);
        cpu_t.esp() = kStackSlot;  // invoke 出口会 `esp += 4`（argc=0）
        timed.invoke(cpu_t, *tdef);  // ← 这里阻塞 ~300 ms 等令牌
        holder.join();

        const std::uint64_t waited = token.waitNs();
        const ShimDef* tdone = timed.byId(tdef_id);
        LIFT_CHECK(tdone != nullptr);
        LIFT_CHECK(token.waits() > waits_before);
        LIFT_CHECK(waited >= 100ull * 1000ull * 1000ull);  // ≥ 100 ms（实际 ~300 ms）
        LIFT_CHECK(token.lastAcquireWaitNs() >= 100ull * 1000ull * 1000ull);  // 线程局部口径也接上了
        LIFT_CHECK(tdone != nullptr && tdone->ns > 0);  // shim 实现自身真的被计时了（否则②恒真）
        // ★ 核心判据：`ns` **不含**等令牌。旧口径下这里会 ≈ 300 ms ⇒ `ns*2 < waited` 红。
        LIFT_CHECK(tdone != nullptr && tdone->ns * 2ull < waited);
        std::printf("guest-token/wait-split: waits=%llu wait=%.1f ms shim.ns=%.3f ms\n",
                    static_cast<unsigned long long>(token.waits()), static_cast<double>(waited) / 1e6,
                    tdone ? static_cast<double>(tdone->ns) / 1e6 : 0.0);
    }

    // ---- ⑦ SurfaceArena 的簿记也进令牌检查点（`SurfaceArena::checkToken`）----
    // 负控在前：**持令牌**访问簿记必须 0 违规（否则这条守卫只会恒红、谁也证明不了）。
    // 正控在后：令牌之外 `find` ⇒ violations ++、first-site 点名 `arena.find`、且真的喊过。
    g_current_case = "guest-token/arena-outside";
    {
        token.arm();  // 清上一案例的 wait / violations 账
        SurfaceArena arena;
        LIFT_CHECK(arena.init(&mem, 0x28000000u, 0x00400000u));
        std::uint32_t ea = 0;
        {
            GuestToken::Guard g(token);
            ea = arena.create(8, 8, SurfaceFormat{});
            LIFT_CHECK(ea != 0);
            LIFT_CHECK(arena.find(ea) != nullptr);
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 0u);  // 负控
        token.arm();                                                          // 清零再开正控
        // 下面这一次**故意**在令牌之外访问簿记：紧跟其后的那行 stderr `[guest-token] 违规…`
        // 是**正控的产物**，不是本条用例失败（进程最终退出码仍看 `finish()`）。
        std::printf("guest-token/arena-outside: 接下来那条 stderr 违规是故意触发的（正控）\n");
        std::fflush(stdout);  // stderr 不缓冲 ⇒ 不先刷一下，这行会排到违规行后面
        (void)arena.find(ea);  // ← 令牌之外查表：这一次就是被守的坏行为
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 1u);
        LIFT_CHECK(token.firstViolationSite() != nullptr);
        LIFT_CHECK(token.firstViolationSite() != nullptr &&
                   std::strcmp(token.firstViolationSite(), "arena.find") == 0);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(arena.violationReports()), 1u);  // 响亮：真的喊过
        // 计数继续累加，但 stderr 只喊一次（热路径不刷屏）
        (void)arena.find(ea);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 2u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(arena.violationReports()), 1u);
        {
            GuestToken::Guard g(token);
            LIFT_CHECK(arena.destroy(ea));  // 收尾（持令牌，不污染计数）
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 2u);
    }

    // ---- ⑧ Yield 之后必须**重新校验**（guest_token.h 的取/放规则）----
    // 规则：让出期间令牌可能被别人拿走，而别人有权改容器 ⇒ 让出前读到的结论/指针一律作废。
    // 用例把这条规则变成机械判据：窗口里另一条线程（用 `tryAcquire`，拿不到就超时 ⇒ 不挂死）
    // 真的改了簿记；取回后重新 `check()` 通过，且**重新查表**得到"已经没了"。
    g_current_case = "guest-token/yield-revalidate";
    {
        token.arm();
        SurfaceArena arena;
        LIFT_CHECK(arena.init(&mem, 0x28400000u, 0x00400000u));  // 与 ⑦ 的区间错开
        token.acquire();  // ★ 让出**之前**必须先拿着（YieldGuard 未持有时是无操作，那是另一种路径）
        std::uint32_t ea = 0;
        ea = arena.create(16, 16, SurfaceFormat{});
        LIFT_CHECK(ea != 0);
        LIFT_CHECK(arena.find(ea) != nullptr);  // ← 让出**之前**读到的结论
        std::atomic<int> other{0};  // 0=没跑 1=拿到令牌并真的改了簿记 2=窗口里没拿到 3=destroy 失败
        {
            GuestToken::YieldGuard yield(token);  // 真的让出（depth 1 → 0）
            LIFT_CHECK(yield.released());
            LIFT_CHECK(!token.heldByCurrentThread());
            std::thread t([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (!token.tryAcquire()) {
                        std::this_thread::yield();
                        continue;
                    }
                    const bool destroyed = arena.destroy(ea);  // 持令牌改簿记（合法）
                    token.release();
                    other.store(destroyed ? 1 : 3, std::memory_order_release);
                    return;
                }
                other.store(2, std::memory_order_release);  // Yield 没真的放开 ⇒ 后面判红
            });
            t.join();
        }
        LIFT_CHECK(token.heldByCurrentThread());  // YieldGuard 析构后取回
        LIFT_CHECK_EQ_I64(other.load(), 1);       // 窗口里真的被别人改了容器
        LIFT_CHECK(token.check("yield-revalidate/recheck"));  // 取回后重新校验"自己拿着"
        LIFT_CHECK(arena.find(ea) == nullptr);                // 让出前的结论作废：重新查表 ⇒ 没了
        token.release();
        LIFT_CHECK(!token.heldByCurrentThread());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(token.violations()), 0u);
    }

    std::printf("guest-token: threads=%d iters=%d allocs=%llu frees=%llu violations=%llu max-concurrent=%u "
                "in_use=%u shim-invoke=%d\n",
                kThreads, kIters, static_cast<unsigned long long>(allocs.load()),
                static_cast<unsigned long long>(frees.load()),
                static_cast<unsigned long long>(token.violations()), token.maxConcurrent(),
                heap.bytesInUse(), kShimIters * 2);
    return finish("guest-token-stress");
}
