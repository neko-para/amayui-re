// emulator-lift · 同步族扩展夹具（pure：guest 侧自造调用序列，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "同步族"）：本轮从 stub 变真实现的每一条都要有**行为**断言，
// 而且断言要能红 —— 不是"返回 1 就算过"：
//   ① SList 的链表序（LIFO）、Depth、Flush 的返回值（整条链）按文档；
//   ② **命名对象**：`Create*` 命中同名 ⇒ 同一对象（183）；`Open*` 按名字打开**同一个对象**
//      （通过一个句柄 SetEvent、另一个句柄 Wait 成功）；名字**大小写敏感**；找不到 ⇒ 0 + 2；
//      种类不符 ⇒ 0 + 2；
//   ③ `DuplicateHandle`：新句柄 ≠ 旧句柄、指向同一对象、`DUPLICATE_SAME_ACCESS`、引用计数
//      （关掉一个之后另一个仍然可用；关第二个才销毁）；
//   ④ `SignalObjectAndWait` = signal 后 wait（两个对象的最终状态都要对）；
//   ⑤ `*Ex` / `SleepEx` 的返回值与 `bAlertable` 的语义缺口（永不 WAIT_IO_COMPLETION）；
//      虚拟时钟按 ms 前进；
//   ⑥ `SwitchToThread` 返回 0（无其它就绪线程）、`GetThreadPriority` 对当前线程返回 0。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Fixture;

constexpr std::uint32_t kSentinel = lift_fixture::kSentinel;
constexpr std::uint32_t kWaitObject0 = 0u;
constexpr std::uint32_t kWaitTimeout = 258u;
constexpr std::uint32_t kErrFileNotFound = 2u;
constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrAlreadyExists = 183u;

// 访问位（与 kernel32_sync.cpp 同一组 Win32 数值）
constexpr std::uint32_t kEventModifyState = 0x0002u;
constexpr std::uint32_t kSynchronize = 0x00100000u;
constexpr std::uint32_t kDuplicateSameAccess = 0x00000002u;
constexpr std::uint32_t kCurrentProcess = 0xFFFFFFFFu;

}  // namespace

int main() {
    using namespace lift_test;

    // ① SList：LIFO 序 / Depth / Flush 返回整条链
    {
        g_current_case = "slist-lifo";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t init = f.use("InitializeSListHead");
        const std::uint32_t push = f.use("InterlockedPushEntrySList");
        const std::uint32_t pop = f.use("InterlockedPopEntrySList");
        const std::uint32_t flush = f.use("InterlockedFlushSList");
        const std::uint32_t depth = f.use("QueryDepthSList");
        const std::uint32_t head = f.zeroData(8);
        const std::uint32_t e1 = f.zeroData(4);
        const std::uint32_t e2 = f.zeroData(4);
        const std::uint32_t e3 = f.zeroData(4);

        // 先把 header 写成非 0，验证 InitializeSListHead 真的清满 8 字节（Depth 也必须清）
        f.m.mem().wr32(head, 0xDEADBEEFu);
        f.m.mem().wr16(head + 4u, 7u);
        LIFT_CHECK_EQ_U32(f.call(init, {head}), 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), head, 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(head + 4u), 0u);

        LIFT_CHECK_EQ_U32(f.call(push, {head, e1}), 0u);  // 空表 ⇒ 返回 NULL
        LIFT_CHECK_EQ_U32(f.call(push, {head, e2}), e1);
        LIFT_CHECK_EQ_U32(f.call(push, {head, e3}), e2);
        LIFT_CHECK_EQ_U32(f.call(depth, {head}), 3u);
        LIFT_CHECK_MEM_U32(f.m.mem(), e3, e2);  // 表头是 e3 ⇒ e3.Next = e2
        LIFT_CHECK_EQ_U32(f.call(pop, {head}), e3);
        LIFT_CHECK_EQ_U32(f.call(pop, {head}), e2);
        LIFT_CHECK_EQ_U32(f.call(depth, {head}), 1u);
        LIFT_CHECK_EQ_U32(f.call(pop, {head}), e1);
        LIFT_CHECK_EQ_U32(f.call(pop, {head}), 0u);  // 空 ⇒ NULL
        LIFT_CHECK_EQ_U32(f.call(depth, {head}), 0u);

        LIFT_CHECK_EQ_U32(f.call(push, {head, e1}), 0u);
        LIFT_CHECK_EQ_U32(f.call(push, {head, e2}), e1);
        LIFT_CHECK_EQ_U32(f.call(flush, {head}), e2);  // 整条链（表头 e2）
        LIFT_CHECK_MEM_U32(f.m.mem(), head, 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(head + 4u), 0u);
        LIFT_CHECK_EQ_U32(f.call(flush, {head}), 0u);  // 再 flush ⇒ NULL
        LIFT_CHECK(f.ok);
    }

    // ② 命名事件：Create 命中同名 ⇒ 同一对象 + 183；Open 按名字打开同一对象；
    //    名字大小写敏感；找不到 ⇒ 0 + ERROR_FILE_NOT_FOUND
    {
        g_current_case = "named-event-open";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t open = f.use("OpenEventA");
        const std::uint32_t set = f.use("SetEvent");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t close = f.use("CloseHandle");
        const std::uint32_t name = f.cstr("LiftEvent1");
        const std::uint32_t other_case = f.cstr("liftevent1");
        const std::uint32_t missing = f.cstr("NoSuchObject");

        const std::uint32_t h1 = f.call(create, {0u, 0u, 0u, name});
        LIFT_CHECK_NE_U32(h1, 0u);
        const std::uint32_t h2 = f.call(create, {0u, 1u, 0u, name});  // 同名 ⇒ 同一对象
        LIFT_CHECK_NE_U32(h2, h1);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAlreadyExists);

        const std::uint32_t h3 = f.call(open, {kEventModifyState | kSynchronize, 0u, name});
        LIFT_CHECK_NE_U32(h3, 0u);
        LIFT_CHECK_NE_U32(h3, h1);

        // 通过 h3 置位 ⇒ 通过 h1 等到（同一对象）
        LIFT_CHECK_EQ_U32(f.call(set, {h3}), 1u);
        LIFT_CHECK_EQ_U32(f.call(wait, {h1, 0u}), kWaitObject0);

        // 大小写不同 ⇒ 不是同一个名字（文档："Name comparisons are case sensitive"）
        LIFT_CHECK_EQ_U32(f.call(open, {kEventModifyState, 0u, other_case}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrFileNotFound);
        // 不存在 ⇒ 0 + 2
        LIFT_CHECK_EQ_U32(f.call(open, {kEventModifyState, 0u, missing}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrFileNotFound);
        // 名字为 NULL ⇒ 0 + 87
        LIFT_CHECK_EQ_U32(f.call(open, {kEventModifyState, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 87u);

        LIFT_CHECK_EQ_U32(f.call(close, {h1}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {h2}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {h3}), 1u);
        // 三个句柄都关了 ⇒ 名字消失
        LIFT_CHECK_EQ_U32(f.call(open, {kEventModifyState, 0u, name}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrFileNotFound);
        LIFT_CHECK(f.ok);
    }

    // ③ 命名信号量 / 互斥量：种类不符 ⇒ 0 + 2；信号量计数经打开的句柄释放
    {
        g_current_case = "named-semaphore-mutex";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t mcreate = f.use("CreateMutexA");
        const std::uint32_t mopen = f.use("OpenMutexA");
        const std::uint32_t screate = f.use("CreateSemaphoreA");
        const std::uint32_t sopen = f.use("OpenSemaphoreA");
        const std::uint32_t srelease = f.use("ReleaseSemaphore");
        const std::uint32_t close = f.use("CloseHandle");
        const std::uint32_t mtx_name = f.cstr("LiftMutex1");
        const std::uint32_t sem_name = f.cstr("LiftSem1");

        const std::uint32_t mh = f.call(mcreate, {0u, 0u, mtx_name});
        LIFT_CHECK_NE_U32(mh, 0u);
        LIFT_CHECK_NE_U32(f.call(mopen, {kSynchronize, 0u, mtx_name}), 0u);
        // 用 OpenSemaphoreA 打开一个互斥量的名字 ⇒ 种类不符 ⇒ 0 + ERROR_FILE_NOT_FOUND
        LIFT_CHECK_EQ_U32(f.call(sopen, {0x0001u, 0u, mtx_name}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrFileNotFound);

        const std::uint32_t sh = f.call(screate, {0u, 1u, 2u, sem_name});
        LIFT_CHECK_NE_U32(sh, 0u);
        const std::uint32_t sh2 = f.call(sopen, {0x0001u, 0u, sem_name});
        LIFT_CHECK_NE_U32(sh2, 0u);
        // 通过 sh2 释放 1 ⇒ 上一计数 = 1（同一对象）
        const std::uint32_t prev_ea = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(srelease, {sh2, 1u, prev_ea}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), prev_ea, 1u);
        // 再释放 1 ⇒ 超过 max=2 ⇒ 0 + 298
        LIFT_CHECK_EQ_U32(f.call(srelease, {sh, 1u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 298u);

        LIFT_CHECK_EQ_U32(f.call(close, {mh}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {sh}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {sh2}), 1u);
        LIFT_CHECK(f.ok);
    }

    // ④ DuplicateHandle：新句柄指向同一对象；引用计数（关一个不销毁，关完才销毁）
    {
        g_current_case = "duplicate-handle";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t dup = f.use("DuplicateHandle");
        const std::uint32_t set = f.use("SetEvent");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t close = f.use("CloseHandle");
        const std::uint32_t out = f.u32data(0xDEADBEEFu);

        const std::uint32_t h1 = f.call(create, {0u, 1u, 0u, 0u});  // 手动复位事件
        LIFT_CHECK_NE_U32(h1, 0u);
        LIFT_CHECK_EQ_U32(f.call(dup, {kCurrentProcess, h1, kCurrentProcess, out, 0u, 0u,
                                       kDuplicateSameAccess}),
                          1u);
        const std::uint32_t h2 = f.m.mem().rd32(out);
        LIFT_CHECK_NE_U32(h2, 0u);
        LIFT_CHECK_NE_U32(h2, h1);

        // 通过副本置位 ⇒ 原件等到（同一对象；手动复位 ⇒ 不会被消费掉）
        LIFT_CHECK_EQ_U32(f.call(set, {h2}), 1u);
        LIFT_CHECK_EQ_U32(f.call(wait, {h1, 0u}), kWaitObject0);
        // 关掉原件 ⇒ 对象仍在（还有副本句柄），副本还能用
        LIFT_CHECK_EQ_U32(f.call(close, {h1}), 1u);
        LIFT_CHECK_EQ_U32(f.call(set, {h2}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {h2}), 1u);
        // ⚠ 两个都关了之后**不能**再拿 h2 调 SetEvent：无效句柄按设计会 abort（响亮失败），
        //   不是返回 FALSE —— 那条路径是进程级的，测它等于杀掉夹具。

        // 目标指针为 NULL ⇒ 仍然复制成功（文档的向后兼容路径），但没有句柄可回传
        const std::uint32_t h3 = f.call(create, {0u, 1u, 0u, 0u});
        LIFT_CHECK_EQ_U32(f.call(dup, {kCurrentProcess, h3, kCurrentProcess, 0u, 0u, 0u,
                                       kDuplicateSameAccess}),
                          1u);
        LIFT_CHECK_EQ_U32(f.call(close, {h3}), 1u);
        LIFT_CHECK(f.ok);
    }

    // ⑤ SignalObjectAndWait = signal + wait（两个对象的最终状态都要对）
    {
        g_current_case = "signal-object-and-wait";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t sowa = f.use("SignalObjectAndWait");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t close = f.use("CloseHandle");

        const std::uint32_t signal_me = f.call(create, {0u, 1u, 0u, 0u});  // 手动复位，初始未置位
        const std::uint32_t wait_on = f.call(create, {0u, 0u, 1u, 0u});    // 自动复位，初始已置位
        LIFT_CHECK_NE_U32(signal_me, 0u);
        LIFT_CHECK_NE_U32(wait_on, 0u);

        LIFT_CHECK_EQ_U32(f.call(sowa, {signal_me, wait_on, 0u, 0u}), kWaitObject0);
        // 第一个对象被置位（手动 ⇒ 等得到且不消费）
        LIFT_CHECK_EQ_U32(f.call(wait, {signal_me, 0u}), kWaitObject0);
        // 第二个对象被消费掉（自动复位）⇒ 再等超时
        LIFT_CHECK_EQ_U32(f.call(wait, {wait_on, 0u}), kWaitTimeout);
        LIFT_CHECK_EQ_U32(f.call(close, {signal_me}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {wait_on}), 1u);
        LIFT_CHECK(f.ok);
    }

    // ⑥ *Ex 与 SleepEx：返回值差异 / alertable 语义缺口 / 虚拟时钟前进
    {
        g_current_case = "ex-waits-and-sleep";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t wait1 = f.use("WaitForSingleObjectEx");
        const std::uint32_t waitn = f.use("WaitForMultipleObjectsEx");
        const std::uint32_t sleep = f.use("SleepEx");
        const std::uint32_t close = f.use("CloseHandle");

        const std::uint32_t a = f.call(create, {0u, 1u, 0u, 0u});  // 未置位
        const std::uint32_t b = f.call(create, {0u, 1u, 1u, 0u});  // 已置位
        LIFT_CHECK_NE_U32(a, 0u);
        LIFT_CHECK_NE_U32(b, 0u);

        LIFT_CHECK_EQ_U32(f.call(wait1, {a, 0u, 1u}), kWaitTimeout);  // alertable 也不会 IO_COMPLETION
        LIFT_CHECK_EQ_U32(f.call(wait1, {b, 0u, 1u}), kWaitObject0);
        // 多对象：第 2 个被置位 ⇒ WAIT_OBJECT_0 + 1
        const std::uint32_t arr = f.zeroData(8);
        f.m.mem().wr32(arr, a);
        f.m.mem().wr32(arr + 4u, b);
        LIFT_CHECK_EQ_U32(f.call(waitn, {2u, arr, 0u, 0u, 0u}), kWaitObject0 + 1u);
        // bWaitAll=TRUE 不实现 ⇒ WAIT_TIMEOUT（⛔ 不假装成功）
        LIFT_CHECK_EQ_U32(f.call(waitn, {2u, arr, 1u, 0u, 0u}), kWaitTimeout);
        // ⚠ 无效句柄按设计**响亮失败**（HandleTable::invalidHandle → abort），所以这里不测
        //   "invalid handle ⇒ WAIT_FAILED"：那条路径是进程级的，测它等于杀掉夹具。

        const std::uint64_t before = f.m.clock().now100ns();
        LIFT_CHECK_EQ_U32(f.call(sleep, {25u, 1u}), 0u);
        const std::uint64_t after = f.m.clock().now100ns();
        // SleepEx 精确推进 25 ms = 250000 × 100ns；差值里还含这次调用本身的指令 tick
        // （每条指令 1×100ns，几条而己）⇒ 断言"至少 25 ms、且不超过 25 ms + 1 µs"，
        // ⛔ 不写死 250000（那会把"夹具执行了几条指令"也钉进判据里）。
        const std::int64_t delta = static_cast<std::int64_t>(after - before);
        LIFT_CHECK(delta >= 25 * 10000);
        LIFT_CHECK(delta < 25 * 10000 + 10000);

        LIFT_CHECK_EQ_U32(f.call(close, {a}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {b}), 1u);
        LIFT_CHECK(f.ok);
    }

    // ⑦ SwitchToThread / GetThreadPriority
    {
        g_current_case = "switch-and-thread-priority";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t sw = f.use("SwitchToThread");
        const std::uint32_t prio = f.use("GetThreadPriority");
        LIFT_CHECK_EQ_U32(f.call(sw, {}), 0u);  // 没有其它就绪线程 ⇒ 0
        LIFT_CHECK_EQ_U32(f.call(prio, {0xFFFFFFFEu}), 0u);  // THREAD_PRIORITY_NORMAL
        LIFT_CHECK_EQ_U32(f.call(prio, {0x00001234u}), 0x7FFFFFFFu);  // ERROR_RETURN
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidHandle);
        LIFT_CHECK(f.ok);
    }

    return finish("sync-ext");
}
