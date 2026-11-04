// emulator-lift · S6 前三族（内存 / 错误码·TLS / 同步）的**核心行为**夹具（pure：自造调用序列）
//
// ★ 补的缺口（审计单 REQ-01M6JFZ73GTZE9F6967RRSH6CE）：这三族此前只有扩展面的观察
//   （`sync_ext` 覆盖命名对象 / SList / DuplicateHandle；`process_ext` 只碰了 VirtualLock），
//   而**最常走的那条路**（HeapAlloc→HeapFree、TlsSetValue→TlsGetValue、CreateEvent→Set→Wait）
//   没有可再校验的观察。本夹具按族补齐，每条用例至少两个断言，且判据写成"实现坏了就红"
//   （例：`HeapFree` 之后 `HeapSize` 必须变成 0xFFFFFFFF+87；`TlsGetValue` 必须读回写进去的值）。
//
// 判据（逐族）：
//   ① 内存：`HeapAlloc` 普通 / `HEAP_ZERO_MEMORY` / 0 字节、`HeapFree`、
//      `HeapReAlloc`（保内容 / 失败不动原块）、`HeapSize`（活块 / 已释放 / 野指针）；
//      `LocalAlloc`/`LocalFree`/`LocalSize`、`GlobalAlloc`/`GlobalLock`/`GlobalUnlock`/
//      `GlobalSize`/`GlobalFree`；`VirtualAlloc(MEM_RESERVE|MEM_COMMIT)` → 写读 →
//      `VirtualFree(MEM_RELEASE)`；`MEM_DECOMMIT` 之后重新 `MEM_COMMIT`（新提交的页必须清零）；
//   ② 错误码 / TLS：`GetLastError`/`SetLastError` 往返、`TlsAlloc` 非 `0xFFFFFFFF`、
//      `TlsSetValue`/`TlsGetValue` 往返、`TlsFree`（含非法槽）、`Fls*` 一条、
//      `GetCurrentThreadId`/`GetCurrentProcessId` 非 0 且稳定；
//   ③ 同步：事件 `SetEvent`/`ResetEvent`/`WaitForSingleObject`（signaled ⇒ 0；未 signaled 的
//      **轮询**（timeout=0）⇒ 258；未 signaled 且 timeout!=0 ⇒ 硬错误，单独子进程验证 abort）、
//      信号量 `ReleaseSemaphore` 的 previous count 出参、互斥量非持有者释放 ⇒ FALSE + 288、
//      临界区 `Initialize`/`Enter`/`TryEnter`/`Leave`/`Delete`（含**可重入**）、`Interlocked*`。
//
// ★ 纯档 Fixture（`Machine::initBare`）不载 PE ⇒ `Machine::prepare()` 不跑 ⇒ **堆**与
//   **TEB / TLS 数组**要夹具自己铺。这里走生产路径（`GuestHeap::init` / `setupProcessStructures`），
//   ⛔ 不手抄 TEB 偏移（那会变成第二份布局真源）。
// ★ 钉住的是**当前实现的取值**（不是真机语义；报告里单列）：
//   · `HeapAlloc(h, 0, 0)` 的 `HeapSize` = 1（kernel32_mem.cpp 的 `saneBytes` 把 0 字节按 1 申请；
//     MSDN 只保证"返回可用指针"，块大小 ≥ 请求值 ⇒ 1 是**实现取值**，不是文档值）；
//   · `GlobalUnlock` 返回 TRUE：MSDN 原话 "If the specified memory block is fixed memory, this
//     function returns **TRUE**"；本实现把 `GMEM_MOVEABLE` 也当 fixed（同文件偏差 2）⇒ 这里断言
//     TRUE，**但没有**覆盖"可移动块锁计数归零后返回 FALSE + ERROR_NOT_LOCKED"那条（不做可移动块）；
//   · `Fls*` 与 `Tls*` 共用索引空间 / 同一份位图（kernel32_error_tls.cpp 偏差 4）；
//   · `VirtualAlloc(addr=0)` 的返回地址按 64 KiB 对齐（同文件偏差 3 的 bump 分配器）。
// ⛔ 不测 double free / 野指针 free：那条路按设计 abort（进程级，测它等于杀掉夹具）；
//   ⛔ 不测线程创建（SCOPE §1 硬 trap）；⛔ 不测 `WaitForMultipleObjects` 的阻塞行为（T1 待裁决）。
#include <cstdint>
#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "check.h"
#include "fixture.h"
#include "runtime/image.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Fixture;

// ---- 等待返回值 / 错误码（数值与 Win32 一致）----
constexpr std::uint32_t kWaitObject0 = 0u;
constexpr std::uint32_t kWaitTimeout = 0x102u;  // WAIT_TIMEOUT 258
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrNotEnoughMemory = 8u;
constexpr std::uint32_t kErrNotOwner = 288u;
constexpr std::uint32_t kErrTooManyPosts = 298u;

// ---- 内存族标志 ----
constexpr std::uint32_t kHeapZeroMemory = 0x00000008u;  // HEAP_ZERO_MEMORY
constexpr std::uint32_t kLmemZeroInit = 0x00000040u;    // LMEM_ZEROINIT
constexpr std::uint32_t kGmemZeroInit = 0x00000040u;    // GMEM_ZEROINIT
constexpr std::uint32_t kGmemMoveable = 0x00000002u;    // GMEM_MOVEABLE（本实现忽略）
constexpr std::uint32_t kMemCommit = 0x00001000u;
constexpr std::uint32_t kMemReserve = 0x00002000u;
constexpr std::uint32_t kMemDecommit = 0x00004000u;
constexpr std::uint32_t kMemRelease = 0x00008000u;
constexpr std::uint32_t kPageReadWrite = 0x04u;

// 纯档堆：1 MiB（⛔ 本夹具不测 > 1 MiB 的分配）
constexpr std::uint32_t kHeapBegin = 0x30000000u;
constexpr std::uint32_t kHeapSize = 0x00100000u;

// 纯档 Fixture 没有 PE ⇒ 自己铺 guest 堆（生产路径 `GuestHeap::init`，⛔ 不手搭块表）
bool bringUpHeap(Fixture& f) {
    if (!f.ok) return false;
    if (!f.m.heap().init(&f.m.mem(), kHeapBegin, kHeapSize)) {
        f.ok = false;
        f.err = std::string("guest 堆 init 失败：") + f.m.heap().lastError();
    }
    return f.ok;
}

// 纯档 Fixture 没有 PE ⇒ TEB / PEB / TLS 数组要自己铺。
// 直接调 `Machine::prepare()` 里那一步（`setupProcessStructures`），只把 fs_base 接上。
bool bringUpTeb(Fixture& f) {
    if (!f.ok) return false;
    lift::ProcessSetup setup;
    std::string err;
    if (!lift::setupProcessStructures(f.m.mem(), lift::PeImage{}, f.m.layout(), &setup, &err)) {
        f.ok = false;
        f.err = err.empty() ? "setupProcessStructures 失败" : err;
        return false;
    }
    f.m.cpu().fs_base = setup.teb;
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    // =====================================================================================
    // ① 内存族
    // =====================================================================================

    // HeapAlloc：普通 / HEAP_ZERO_MEMORY / 0 字节；HeapSize 对活块返回请求字节数。
    // ★ ZERO_MEMORY 的判据靠"写脏 → free → 同尺寸再申请"：free 后块被合并回起始空闲块，
    //   首次适配必然复用同一块 ⇒ 不清零就会读回 0xA5A5A5A5（这才是"清零真的发生"的判据）。
    //   ⛔ 不断言"新块地址恰好等于旧块"以外的分配策略（那是 allocator 的内部选择）。
    {
        g_current_case = "heap-alloc-zero-and-size";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpHeap(f));
        const std::uint32_t heap = f.call(f.use("GetProcessHeap"), {});
        LIFT_CHECK_NE_U32(heap, 0u);
        const std::uint32_t alloc = f.use("HeapAlloc");
        const std::uint32_t free_ = f.use("HeapFree");
        const std::uint32_t size = f.use("HeapSize");

        const std::uint32_t p = f.call(alloc, {heap, 0u, 64u});
        LIFT_CHECK_NE_U32(p, 0u);
        LIFT_CHECK_EQ_U32(f.call(size, {heap, 0u, p}), 64u);
        f.m.mem().wr32(p, 0xA5A5A5A5u);  // 载荷在 guest 内存里、可读写
        LIFT_CHECK_MEM_U32(f.m.mem(), p, 0xA5A5A5A5u);
        const std::uint32_t in_use_with_p = f.m.heap().bytesInUse();

        // HEAP_ZERO_MEMORY：d 写脏 → free → 同尺寸带 ZERO 再申请
        const std::uint32_t d = f.call(alloc, {heap, 0u, 48u});
        LIFT_CHECK_NE_U32(d, 0u);
        f.m.mem().wr32(d, 0xFFFFFFFFu);
        f.m.mem().wr32(d + 40u, 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.call(free_, {heap, 0u, d}), 1u);
        const std::uint32_t z = f.call(alloc, {heap, kHeapZeroMemory, 48u});
        LIFT_CHECK_EQ_U32(z, d);  // 首次适配复用同一块（清零的前提）
        LIFT_CHECK_MEM_U32(f.m.mem(), z, 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), z + 40u, 0u);
        LIFT_CHECK_EQ_U32(f.call(free_, {heap, 0u, z}), 1u);
        LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), in_use_with_p);  // 宿主可见：只剩 p

        // 0 字节：Win32 也返回**可用指针**（本实现按 1 字节申请 ⇒ HeapSize = 1，钉住）
        const std::uint32_t p0 = f.call(alloc, {heap, 0u, 0u});
        LIFT_CHECK_NE_U32(p0, 0u);
        LIFT_CHECK_EQ_U32(f.call(size, {heap, 0u, p0}), 1u);
        LIFT_CHECK_EQ_U32(f.call(free_, {heap, 0u, p0}), 1u);

        // HeapFree(NULL) 在 Win32 里成功
        LIFT_CHECK_EQ_U32(f.call(free_, {heap, 0u, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.call(free_, {heap, 0u, p}), 1u);
        LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), 0u);  // 宿主可见：全部归还
        LIFT_CHECK(!f.m.heap().owns(p));
        LIFT_CHECK(f.ok);
    }

    // HeapSize：已释放 / NULL / 非堆指针 ⇒ 0xFFFFFFFF + ERROR_INVALID_PARAMETER（文件头偏差 6）
    {
        g_current_case = "heap-free-invalidates-size";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpHeap(f));
        const std::uint32_t heap = f.call(f.use("GetProcessHeap"), {});
        const std::uint32_t p = f.call(f.use("HeapAlloc"), {heap, 0u, 16u});
        LIFT_CHECK_NE_U32(p, 0u);
        LIFT_CHECK_EQ_U32(f.call(f.use("HeapFree"), {heap, 0u, p}), 1u);

        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(f.use("HeapSize"), {heap, 0u, p}), 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        // NULL 与"根本没在堆里的 guest 地址"走同一条失败值
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(f.use("HeapSize"), {heap, 0u, 0u}), 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(f.use("HeapSize"), {heap, 0u, lift_fixture::kDataBase}), 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // HeapReAlloc：保内容；失败返回 0 + 8 且**不动原块**（原地址、原大小、原内容都还在）
    {
        g_current_case = "heap-realloc-keeps-content";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpHeap(f));
        const std::uint32_t heap = f.call(f.use("GetProcessHeap"), {});
        const std::uint32_t realloc_ = f.use("HeapReAlloc");
        const std::uint32_t p = f.call(f.use("HeapAlloc"), {heap, 0u, 16u});
        LIFT_CHECK_NE_U32(p, 0u);
        f.m.mem().wr32(p, 0x11223344u);
        f.m.mem().wr32(p + 8u, 0x55667788u);

        const std::uint32_t q = f.call(realloc_, {heap, 0u, p, 64u});
        LIFT_CHECK_NE_U32(q, 0u);
        LIFT_CHECK_EQ_U32(f.m.heap().sizeOf(q), 64u);
        LIFT_CHECK_MEM_U32(f.m.mem(), q, 0x11223344u);      // 旧内容被搬过去
        LIFT_CHECK_MEM_U32(f.m.mem(), q + 8u, 0x55667788u);

        // 失败路径：请求 0xFFFFFFF8 字节（bytes + 块头在 64 位里越过 4 GiB）⇒ 0 + 8
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(realloc_, {heap, 0u, q, 0xFFFFFFF8u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNotEnoughMemory);
        LIFT_CHECK(f.m.heap().owns(q));                    // 原块仍在
        LIFT_CHECK_EQ_U32(f.m.heap().sizeOf(q), 64u);      // 大小没变
        LIFT_CHECK_MEM_U32(f.m.mem(), q, 0x11223344u);     // 内容没被清
        LIFT_CHECK_MEM_U32(f.m.mem(), q + 8u, 0x55667788u);

        // HeapReAlloc(NULL) 等价于 HeapAlloc
        const std::uint32_t fresh = f.call(realloc_, {heap, 0u, 0u, 8u});
        LIFT_CHECK_NE_U32(fresh, 0u);
        LIFT_CHECK_EQ_U32(f.m.heap().sizeOf(fresh), 8u);

        LIFT_CHECK_EQ_U32(f.call(f.use("HeapFree"), {heap, 0u, q}), 1u);
        LIFT_CHECK_EQ_U32(f.call(f.use("HeapFree"), {heap, 0u, fresh}), 1u);
        LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), 0u);
        LIFT_CHECK(f.ok);
    }

    // Local* / Global*：同一个 guest 堆（README §2.6）⇒ 返回 guest 指针、free 返回 NULL
    {
        g_current_case = "local-and-global";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpHeap(f));

        // LocalAlloc(LMEM_ZEROINIT) / LocalSize / LocalFree（成功返回 NULL）
        const std::uint32_t lp = f.call(f.use("LocalAlloc"), {kLmemZeroInit, 32u});
        LIFT_CHECK_NE_U32(lp, 0u);
        LIFT_CHECK_EQ_U32(f.call(f.use("LocalSize"), {lp}), 32u);
        f.m.mem().wr32(lp, 0xDEADBEEFu);
        LIFT_CHECK_MEM_U32(f.m.mem(), lp, 0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(f.use("LocalFree"), {lp}), 0u);
        LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), 0u);
        // 释放之后 LocalSize 返回 0 + 87（不是 0xFFFFFFFF —— 与 HeapSize 的失败值不同）
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(f.use("LocalSize"), {lp}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(f.use("LocalFree"), {0u}), 0u);  // LocalFree(NULL) == NULL

        // GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT)：偏差 2 ⇒ 句柄即指针、GlobalLock 恒等。
        // ★ GlobalUnlock 返回 TRUE 与 MSDN 一致（"If the specified memory block is fixed memory,
        //   this function returns TRUE"）—— 只是本实现把 MOVEABLE 也当 fixed（偏差 2），
        //   ⛔ 因此"可移动块锁计数归零 ⇒ FALSE + ERROR_NOT_LOCKED"那条没有覆盖。
        const std::uint32_t gp = f.call(f.use("GlobalAlloc"), {kGmemMoveable | kGmemZeroInit, 16u});
        LIFT_CHECK_NE_U32(gp, 0u);
        LIFT_CHECK_EQ_U32(f.call(f.use("GlobalLock"), {gp}), gp);
        LIFT_CHECK_EQ_U32(f.call(f.use("GlobalSize"), {gp}), 16u);
        f.m.mem().wr32(gp, 0x0BADF00Du);
        LIFT_CHECK_MEM_U32(f.m.mem(), gp, 0x0BADF00Du);
        LIFT_CHECK_EQ_U32(f.call(f.use("GlobalUnlock"), {gp}), 1u);
        LIFT_CHECK_EQ_U32(f.call(f.use("GlobalFree"), {gp}), 0u);  // 成功返回 NULL
        LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), 0u);
        LIFT_CHECK(f.ok);
    }

    // VirtualAlloc(MEM_RESERVE|MEM_COMMIT) → 写入/读回 → VirtualFree(MEM_RELEASE)
    {
        g_current_case = "virtual-alloc-commit-release";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t va = f.use("VirtualAlloc");
        const std::uint32_t vf = f.use("VirtualFree");
        // 16 KiB（= 最坏情况的宿主页大小）⇒ decommit 在 16 KiB 宿主页上也真的发生
        const std::uint32_t ea =
            f.call(va, {0u, 0x4000u, kMemReserve | kMemCommit, kPageReadWrite});
        LIFT_CHECK_NE_U32(ea, 0u);
        LIFT_CHECK_EQ_U32(ea & 0xFFFFu, 0u);  // 偏差 3：bump 分配器按 64 KiB 对齐
        LIFT_CHECK(f.m.mem().committed(ea, 0x4000u));
        f.m.mem().wr32(ea, 0x11223344u);
        LIFT_CHECK_MEM_U32(f.m.mem(), ea, 0x11223344u);

        // MEM_RELEASE 要求 dwSize == 0（文档）⇒ 给非 0 ⇒ 0 + 87，且没释放
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(vf, {ea, 0x4000u, kMemRelease}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.m.mem().committed(ea, 0x4000u));

        LIFT_CHECK_EQ_U32(f.call(vf, {ea, 0u, kMemRelease}), 1u);
        LIFT_CHECK(!f.m.mem().committed(ea, 0x4000u));  // 宿主可见：页真的退掉了
        // 再释放一次 ⇒ 0 + 87（游标已退回，这个地址不再是 bump 出来的块基址）
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(vf, {ea, 0u, kMemRelease}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // MEM_DECOMMIT 之后重新 MEM_COMMIT：新提交的页必须清零（commitZeroed）
    {
        g_current_case = "virtual-decommit-recommit";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t va = f.use("VirtualAlloc");
        const std::uint32_t vf = f.use("VirtualFree");
        // 64 KiB：连 64 KiB 宿主页也完整覆盖 ⇒ "退提交之后 committed 为 false" 在
        // 4 KiB / 16 KiB / 64 KiB 三种宿主页上都成立（⛔ 不用单页：16 KiB 宿主页上退不掉半页，
        // 那是平台事实，不是缺陷 —— 见 tests/memory_pages.cpp 的同一条注记）。
        constexpr std::uint32_t kSpan = 0x00010000u;
        const std::uint32_t ea =
            f.call(va, {0u, kSpan, kMemReserve | kMemCommit, kPageReadWrite});
        LIFT_CHECK_NE_U32(ea, 0u);
        f.m.mem().wr32(ea, 0xCAFEBABEu);
        LIFT_CHECK_MEM_U32(f.m.mem(), ea, 0xCAFEBABEu);

        LIFT_CHECK_EQ_U32(f.call(vf, {ea, kSpan, kMemDecommit}), 1u);
        LIFT_CHECK(!f.m.mem().committed(ea, kSpan));
        // 重新提交：返回同一个地址、页可读写、内容被清零
        LIFT_CHECK_EQ_U32(f.call(va, {ea, kSpan, kMemCommit, kPageReadWrite}), ea);
        LIFT_CHECK(f.m.mem().committed(ea, kSpan));
        LIFT_CHECK_MEM_U32(f.m.mem(), ea, 0u);
        LIFT_CHECK_EQ_U32(f.call(vf, {ea, 0u, kMemRelease}), 1u);
        LIFT_CHECK(!f.m.mem().committed(ea, kSpan));
        LIFT_CHECK(f.ok);
    }

    // =====================================================================================
    // ② 错误码 / TLS
    // =====================================================================================

    // GetLastError / SetLastError 往返 + 进程/线程标识非 0 且稳定
    {
        g_current_case = "last-error-and-ids";
        Fixture f;
        lift::registerErrorTlsShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t set = f.use("SetLastError");
        const std::uint32_t get = f.use("GetLastError");

        f.call(set, {1234u});
        LIFT_CHECK_EQ_U32(f.call(get, {}), 1234u);
        f.call(set, {0u});
        LIFT_CHECK_EQ_U32(f.call(get, {}), 0u);
        f.call(set, {0xFFFFFFFFu});  // 全 1 也要能原样读回（⛔ 不许被当成"没设置"）
        LIFT_CHECK_EQ_U32(f.call(get, {}), 0xFFFFFFFFu);

        // 线程 / 进程 id：非 0、两次调用稳定、不相等
        const std::uint32_t tid1 = f.call(f.use("GetCurrentThreadId"), {});
        const std::uint32_t tid2 = f.call(f.use("GetCurrentThreadId"), {});
        const std::uint32_t pid = f.call(f.use("GetCurrentProcessId"), {});
        LIFT_CHECK_NE_U32(tid1, 0u);
        LIFT_CHECK_EQ_U32(tid2, tid1);
        LIFT_CHECK_NE_U32(pid, 0u);
        LIFT_CHECK_NE_U32(pid, tid1);
        // 伪句柄（文档：GetCurrentProcess = -1、GetCurrentThread = -2）
        LIFT_CHECK_EQ_U32(f.call(f.use("GetCurrentProcess"), {}), 0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.call(f.use("GetCurrentThread"), {}), 0xFFFFFFFEu);
        LIFT_CHECK(f.ok);
    }

    // TLS：TlsAlloc 非 TLS_OUT_OF_INDEXES；Set/Get 往返；新槽初值 0 且成功时清 last_error；
    //      TlsFree 释放索引（再分配拿到同一个）；非法槽 ⇒ FALSE + 87
    {
        g_current_case = "tls-slots";
        Fixture f;
        lift::registerErrorTlsShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpTeb(f));
        const std::uint32_t alloc = f.use("TlsAlloc");
        const std::uint32_t free_ = f.use("TlsFree");
        const std::uint32_t get = f.use("TlsGetValue");
        const std::uint32_t set = f.use("TlsSetValue");

        const std::uint32_t s1 = f.call(alloc, {});
        LIFT_CHECK_NE_U32(s1, 0xFFFFFFFFu);  // TLS_OUT_OF_INDEXES
        LIFT_CHECK_NE_U32(s1, 0u);           // 槽 0 保留
        LIFT_CHECK_EQ_U32(s1, 1u);           // 空的位图 ⇒ 从最低可用槽开始给
        // 新槽的值必须是 0；且 TlsGetValue 成功时把 last error 清成 ERROR_SUCCESS
        // （MSDN：TlsGetValue 是"成功也 SetLastError"的那条例外 —— 调用方靠它区分
        //  "值就是 0" 与 "失败"；见 TlsGetValue 的 Return value 一节）
        f.m.cpu().shim.last_error = 1234u;
        LIFT_CHECK_EQ_U32(f.call(get, {s1}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 0u);
        LIFT_CHECK_EQ_U32(f.call(set, {s1, 0xCAFEBABEu}), 1u);
        LIFT_CHECK_EQ_U32(f.call(get, {s1}), 0xCAFEBABEu);

        const std::uint32_t s2 = f.call(alloc, {});
        LIFT_CHECK_NE_U32(s2, 0xFFFFFFFFu);
        LIFT_CHECK_NE_U32(s2, s1);
        LIFT_CHECK_EQ_U32(f.call(get, {s2}), 0u);  // 每个新槽各自从 0 开始

        LIFT_CHECK_EQ_U32(f.call(free_, {s1}), 1u);
        LIFT_CHECK_EQ_U32(f.call(get, {s1}), 0u);  // free 会把槽里的值清掉
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(free_, {s1}), 0u);  // 已经释放过 ⇒ FALSE + 87
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(free_, {0u}), 0u);  // 槽 0 保留 ⇒ FALSE + 87
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(get, {64u}), 0u);  // 越界槽 ⇒ 0 + 87
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(alloc, {}), s1);  // 位图真的清了 ⇒ 又给回最低的空槽
        LIFT_CHECK(f.ok);
    }

    // Fls*：至少一条往返（与 TLS 同实现，偏差 4 —— 共用索引空间，钉住）
    {
        g_current_case = "fls-slots";
        Fixture f;
        lift::registerErrorTlsShims(f.m.shims());
        LIFT_CHECK(f.ok);
        LIFT_CHECK(bringUpTeb(f));
        const std::uint32_t s = f.call(f.use("FlsAlloc"), {0u});  // 回调指针被丢弃
        LIFT_CHECK_NE_U32(s, 0xFFFFFFFFu);
        LIFT_CHECK_NE_U32(s, 0u);
        LIFT_CHECK_EQ_U32(f.call(f.use("FlsGetValue"), {s}), 0u);
        LIFT_CHECK_EQ_U32(f.call(f.use("FlsSetValue"), {s, 0xA5A55A5Au}), 1u);
        LIFT_CHECK_EQ_U32(f.call(f.use("FlsGetValue"), {s}), 0xA5A55A5Au);
        LIFT_CHECK_EQ_U32(f.call(f.use("FlsFree"), {s}), 1u);
        LIFT_CHECK_EQ_U32(f.call(f.use("FlsGetValue"), {s}), 0u);
        LIFT_CHECK(f.ok);
    }

    // =====================================================================================
    // ③ 同步族
    // =====================================================================================

    // 事件：signaled 立即返回 WAIT_OBJECT_0；Reset 之后等待超时（timeout 用 1 ms）
    {
        g_current_case = "event-set-reset-wait";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t set = f.use("SetEvent");
        const std::uint32_t reset = f.use("ResetEvent");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t close = f.use("CloseHandle");

        const std::uint32_t h = f.call(create, {0u, 1u, 0u, 0u});  // 手动复位、初始未置位
        LIFT_CHECK_NE_U32(h, 0u);
        // 未 signaled 的**轮询**（timeout=0）⇒ WAIT_TIMEOUT；timeout!=0 会硬错误（另测）
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 0u}), kWaitTimeout);
        LIFT_CHECK_EQ_U32(f.call(set, {h}), 1u);
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 1u}), kWaitObject0);
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 1u}), kWaitObject0);  // 手动复位 ⇒ 不被消费
        LIFT_CHECK_EQ_U32(f.call(reset, {h}), 1u);
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 0u}), kWaitTimeout);
        LIFT_CHECK_EQ_U32(f.call(close, {h}), 1u);
        LIFT_CHECK(f.m.handles().get(h) == nullptr);  // 宿主可见：句柄表项真的被回收
        // 关闭之后再 CloseHandle ⇒ FALSE + 6（不 abort 的失败路径）
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(close, {h}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 6u);
        LIFT_CHECK(f.ok);
    }

#if !defined(_WIN32)
    // 用户 2026-11-04：未 signaled 且 timeout != 0（含 INFINITE）⇒ **硬错误**（shimAbort），
    // 不再返回 WAIT_TIMEOUT。用子进程验证 abort（父进程不能被 abort 掉）。
    {
        g_current_case = "unsignaled-wait-traps";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateEventA");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t h = f.call(create, {0u, 1u, 0u, 0u});
        LIFT_CHECK_NE_U32(h, 0u);
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::freopen("/dev/null", "w", stderr);
            (void)f.call(wait, {h, 1u});  // 必须 abort；若返回 ⇒ _exit(0) 让父级判红
            ::_exit(0);
        }
        LIFT_CHECK(pid > 0);
        if (pid > 0) {
            int status = 0;
            LIFT_CHECK(::waitpid(pid, &status, 0) == pid);
            LIFT_CHECK(WIFSIGNALED(status));
            LIFT_CHECK_EQ_I64(WTERMSIG(status), SIGABRT);
        }
        // 多对象等待：同一条硬错误策略（bWaitAll=0 / 1 各验一次）
        const std::uint32_t waitn = f.use("WaitForMultipleObjects");
        const std::uint32_t h2 = f.call(create, {0u, 1u, 0u, 0u});
        LIFT_CHECK_NE_U32(h2, 0u);
        const std::uint32_t arr = f.zeroData(8);
        f.m.mem().wr32(arr, h);
        f.m.mem().wr32(arr + 4u, h2);
        auto expectAbort = [&](std::uint32_t wait_all) {
            const pid_t p = ::fork();
            if (p == 0) {
                std::freopen("/dev/null", "w", stderr);
                (void)f.call(waitn, {2u, arr, wait_all, 1u});  // 必须 abort
                ::_exit(0);
            }
            LIFT_CHECK(p > 0);
            if (p > 0) {
                int st = 0;
                LIFT_CHECK(::waitpid(p, &st, 0) == p);
                LIFT_CHECK(WIFSIGNALED(st));
                LIFT_CHECK_EQ_I64(WTERMSIG(st), SIGABRT);
            }
        };
        expectAbort(0u);  // bWaitAll=FALSE，没有任何对象 signaled
        expectAbort(1u);  // bWaitAll=TRUE，至少一个未 signaled
        LIFT_CHECK_EQ_U32(f.call(f.use("CloseHandle"), {h}), 1u);
        LIFT_CHECK_EQ_U32(f.call(f.use("CloseHandle"), {h2}), 1u);
    }
#endif

    // 信号量：ReleaseSemaphore 的 previous count 出参 + 超上限 ⇒ FALSE + 298
    {
        g_current_case = "semaphore-release-previous";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateSemaphoreA");
        const std::uint32_t release = f.use("ReleaseSemaphore");
        const std::uint32_t wait = f.use("WaitForSingleObject");

        const std::uint32_t h = f.call(create, {0u, 1u, 2u, 0u});  // 初始 1、上限 2
        LIFT_CHECK_NE_U32(h, 0u);
        const std::uint32_t prev = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(release, {h, 1u, prev}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), prev, 1u);  // 释放前的计数 = 1（文档的 lpPreviousCount）
        // 再加 1 就超过上限 2 ⇒ FALSE + ERROR_TOO_MANY_POSTS(298)
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(release, {h, 1u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrTooManyPosts);
        // 计数 2：连等两次都该拿到（GET 之后计数递减）
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 0u}), kWaitObject0);
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 0u}), kWaitObject0);
        LIFT_CHECK_EQ_U32(f.call(wait, {h, 0u}), kWaitTimeout);
        // 参数非法：初始计数 > 上限 ⇒ 0 + 87
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(create, {0u, 3u, 2u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(f.use("CloseHandle"), {h}), 1u);
        LIFT_CHECK(f.ok);
    }

    // 互斥量：非持有者释放 ⇒ FALSE + ERROR_NOT_OWNER(288)；持有者可递归取 / 释放
    {
        g_current_case = "mutex-release-owner";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateMutexA");
        const std::uint32_t release = f.use("ReleaseMutex");
        const std::uint32_t wait = f.use("WaitForSingleObject");
        const std::uint32_t close = f.use("CloseHandle");

        // 未持有（bInitialOwner=FALSE）⇒ ReleaseMutex 失败 + 288
        const std::uint32_t m1 = f.call(create, {0u, 0u, 0u});
        LIFT_CHECK_NE_U32(m1, 0u);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(release, {m1}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNotOwner);

        // 本线程持有 ⇒ 可递归取（同一线程再等一次立刻成功）、释放两次后释放计数到 0
        const std::uint32_t m2 = f.call(create, {0u, 1u, 0u});
        LIFT_CHECK_NE_U32(m2, 0u);
        LIFT_CHECK_EQ_U32(f.call(wait, {m2, 0u}), kWaitObject0);  // 递归获取
        LIFT_CHECK_EQ_U32(f.call(release, {m2}), 1u);
        LIFT_CHECK_EQ_U32(f.call(release, {m2}), 1u);
        // 计数归 0 ⇒ 再释放就是"非持有者"
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(release, {m2}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNotOwner);
        // Release 之后互斥量变为 signaled ⇒ 等得到（并重新变成本线程持有）
        LIFT_CHECK_EQ_U32(f.call(wait, {m2, 0u}), kWaitObject0);
        LIFT_CHECK_EQ_U32(f.call(release, {m2}), 1u);

        LIFT_CHECK_EQ_U32(f.call(close, {m1}), 1u);
        LIFT_CHECK_EQ_U32(f.call(close, {m2}), 1u);
        LIFT_CHECK(f.ok);
    }

    // 临界区：Initialize / Enter / TryEnter / Leave / Delete，且**可重入**（判据看 guest 结构）
    {
        g_current_case = "critical-section-reentrant";
        Fixture f;
        lift::registerSyncShims(f.m.shims());
        lift::registerErrorTlsShims(f.m.shims());  // OwningThread 要与 guest 的线程 id 对比
        LIFT_CHECK(f.ok);
        const std::uint32_t init = f.use("InitializeCriticalSectionAndSpinCount");
        const std::uint32_t init_plain = f.use("InitializeCriticalSection");
        const std::uint32_t enter = f.use("EnterCriticalSection");
        const std::uint32_t try_enter = f.use("TryEnterCriticalSection");
        const std::uint32_t leave = f.use("LeaveCriticalSection");
        const std::uint32_t del = f.use("DeleteCriticalSection");

        const std::uint32_t cs = f.zeroData(0x18);  // CRITICAL_SECTION 在 x86 上是 0x18 字节
        f.m.mem().wr32(cs, 0xDEADBEEFu);            // DebugInfo 必须是 0（不能留残值）
        f.m.mem().wr32(cs + 0x08u, 7u);
        f.m.mem().wr32(cs + 0x14u, 0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(init, {cs, 0x1234u}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs, 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x08u, 0u);        // RecursionCount
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x14u, 0x1234u);   // SpinCount @ +0x14（偏差 1）

        const std::uint32_t tid = f.call(f.use("GetCurrentThreadId"), {});
        f.call(enter, {cs});
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x08u, 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x0Cu, tid);  // OwningThread = 本线程
        f.call(enter, {cs});                             // 可重入：只加递归计数
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x08u, 2u);
        LIFT_CHECK_EQ_U32(f.call(try_enter, {cs}), 1u);  // 偏差 3：单线程 ⇒ 永远能进（真的进了）
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x08u, 3u);
        f.call(leave, {cs});
        f.call(leave, {cs});
        f.call(leave, {cs});
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x08u, 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x0Cu, 0u);  // 归 0 时清 OwningThread

        f.call(del, {cs});
        LIFT_CHECK_MEM_U32(f.m.mem(), cs + 0x14u, 0u);  // Delete 清满 0x18 字节

        // InitializeCriticalSection（不带 spin）⇒ SpinCount = 0 且结构仍被清零
        const std::uint32_t cs2 = f.zeroData(0x18);
        f.m.mem().wr32(cs2 + 0x14u, 0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(init_plain, {cs2}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs2 + 0x14u, 0u);
        LIFT_CHECK_EQ_U32(f.call(try_enter, {cs2}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cs2 + 0x08u, 1u);
        f.call(leave, {cs2});
        f.call(del, {cs2});
        LIFT_CHECK(f.ok);
    }

    // Interlocked*：Increment 返回新值、Exchange 返回旧值，且 guest 内存真的被改
    {
        g_current_case = "interlocked-increment-exchange";
        Fixture f;
        lift::registerErrorTlsShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t cell = f.u32data(41u);
        LIFT_CHECK_EQ_U32(f.call(f.use("InterlockedIncrement"), {cell}), 42u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cell, 42u);
        LIFT_CHECK_EQ_U32(f.call(f.use("InterlockedExchange"), {cell, 0x1234u}), 42u);  // 旧值
        LIFT_CHECK_MEM_U32(f.m.mem(), cell, 0x1234u);
        LIFT_CHECK_EQ_U32(f.call(f.use("InterlockedDecrement"), {cell}), 0x1233u);
        LIFT_CHECK_MEM_U32(f.m.mem(), cell, 0x1233u);
        LIFT_CHECK(f.ok);
    }

    return finish("shims-core");
}
