// emulator-lift · kernel32 同步族（CRITICAL_SECTION / 事件 / 信号量 / 互斥量 / 等待 / Sleep）
//
// ★ 这一族的口径（README §2.5：**guest 业务单线程**）：
//   · 临界区**必须可重入**（这是本批点名的判据）：Enter 只改 guest 里的
//     `RecursionCount`(+0x08) / `OwningThread`(+0x0C)，不解引用任何宿主锁 ⇒ 天然可重入，
//     且重入次数写在 guest 结构里（guest 自己也能看见）。
//   · 等待对象（事件 / 信号量 / 互斥量）的宿主态挂在 `HandleTable` 的 `payload`（每 Machine 一份），
//     guest 只拿不透明整数句柄。`CloseHandle` 按 `aux` 标记回收 payload。
//   · **阻塞不实现**（单线程 guest ⇒ 真阻塞就是死锁）：未 signaled 的等待一律返回
//     `WAIT_TIMEOUT`(258)，**并打一行** —— 这是 SCOPE §3 允许的偏差形态，⛔ 不许静默。
//   · `Sleep` **不睡**：只把 `cpu.clock` 前进 `ms*10000`（100 ns 单位），保证确定性。
//
// ⚠ 偏差（逐条）：
//   1. **CRITICAL_SECTION 的 SpinCount 在 +0x14，不是任务书写的 +0x1C**。真实 Win32 x86 的
//      CRITICAL_SECTION 是 0x18 字节：DebugInfo +0x00 / LockCount +0x04 / RecursionCount +0x08 /
//      OwningThread +0x0C / LockSemaphore +0x10 / **SpinCount +0x14**。任务书的 +0x1C 会写到
//      结构**之后** 4 字节上 —— guest 通常把 CRITICAL_SECTION 内嵌在别的对象里，那会写坏相邻字段。
//      所以这里按真实布局（+0x14）落盘，并只清 0x18 字节。影响：`GetCriticalSectionSpinCount`
//      之类的读回值与调用方给的值一致；真机上"自旋"本来也不发生（单线程）。
//   2. **LockCount(+0x04) / LockSemaphore(+0x10) 不模拟**：前者只在有等待者时才非零（单线程
//      guest 没有等待者），后者是内核句柄。影响：直接读这两个字段的 guest 代码看到 0
//      （Win32 文档说它们是不透明的，⛔ 不该被读）。
//   3. **`TryEnterCriticalSection` 永远成功**（单线程 guest 没有竞争）—— 返回 TRUE 并真的进入，
//      不是"假成功"：它改的状态与 `EnterCriticalSection` 完全一致。
//   4. **`WaitForSingleObject` / `WaitForMultipleObjects` 永不阻塞**：未 signaled 时返回
//      `WAIT_TIMEOUT` 并打日志。影响：靠"等待另一个线程"推进的逻辑（本批的 guest 里没有）会在
//      这里走超时分支；这也是 A5 缺口报告里要能看见的形态。
//   5. **`WaitForMultipleObjects` 只支持 `bWaitAll==FALSE`**；`bWaitAll==TRUE` 一律打印 +
//      `WAIT_TIMEOUT`（即使全部 signaled 也不返回成功 —— 按本批口径，宁可响亮也不假装）。
//   6. **`PulseEvent` 不做"唤醒等待者再复位"**：先把 signaled 置真、**立刻**置假（真机语义），
//      所以单线程下这次脉冲等于丢失；没有等待者的 PulseEvent 在真机上同样丢信号。
//   7. **`CreateEventA/W` 不查重用名对象**：每次调用都新建（单进程 guest，命名对象只用于
//      "单实例守卫"，M1a 不模拟跨进程语义）。`CreateSemaphoreA` 的 `lpName` 记为句柄名（诊断用）。
//   8. **`Sleep(INFINITE)`** 不阻塞：按 1 秒推进虚拟时钟并打印一行（否则 guest 会卡死）。
//   9. **线程 / 定时器队列整族不做**：`CreateThread` / `CreateTimerQueue` / `CreateTimerQueueTimer` /
//      `SignalObjectAndWait` / `WaitForSingleObjectEx` / `WaitForMultipleObjectsEx` 用
//      `addUnimplemented` 登记（README §2.5 / D5），执行到就响亮失败。
#include <cstdint>
#include <string>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrNotOwner = 288u;
constexpr std::uint32_t kErrTooManyPosts = 298u;

constexpr std::uint32_t kWaitObject0 = 0u;
constexpr std::uint32_t kWaitTimeout = 258u;
constexpr std::uint32_t kWaitFailed = 0xFFFFFFFFu;
constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;

// CRITICAL_SECTION（x86，0x18 字节；见文件头偏差 1）
constexpr std::uint32_t kCsSize = 0x18u;
constexpr std::uint32_t kCsRecursionCount = 0x08u;
constexpr std::uint32_t kCsOwningThread = 0x0Cu;
constexpr std::uint32_t kCsSpinCount = 0x14u;

// SyncObj 的种类标记（同时写进 HandleEntry::aux，CloseHandle 靠它回收 payload）
constexpr std::uint32_t kSyncEvent = 1u;
constexpr std::uint32_t kSyncSemaphore = 2u;
constexpr std::uint32_t kSyncMutex = 3u;

struct SyncObj {
    std::uint32_t marker = 0;    // kSyncEvent / kSyncSemaphore / kSyncMutex
    bool manual = false;         // 事件：手动复位？
    bool signaled = false;
    std::uint32_t count = 0;     // 信号量：当前计数；互斥量：递归层数
    std::uint32_t max_count = 0; // 信号量：上限
    std::uint32_t owner = 0;     // 互斥量：拥有者 thread_id
};

std::string narrowAscii(const std::u16string& w) {
    std::string s;
    s.reserve(w.size());
    for (const char16_t c : w) {
        s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return s;
}

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

void requireHandles(X86& cpu, const char* api) {
    requireMem(cpu, api);
    if (!cpu.handles) shimAbort("kernel32", api, "cpu.handles 为空（Machine 没装配句柄表）");
}

HandleEntry* entryOf(X86& cpu, std::uint32_t h, const char* api) {
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) {
        cpu.handles->invalidHandle(h, api);  // [[noreturn]]：打印 kind/值 + 已分配范围后停止
    }
    return e;
}

// 取同步对象；`want` 非 0 ⇒ 还要求种类匹配（种类不符按 Windows 返回 FALSE + 错误 6）
SyncObj* syncOf(X86& cpu, std::uint32_t h, std::uint32_t want, const char* api) {
    HandleEntry* e = entryOf(cpu, h, api);
    if (!e->payload || e->aux < kSyncEvent || e->aux > kSyncMutex) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 不是本族建的同步对象（kind=%s）⇒ 失败", h,
                handleKindName(e->kind));
        return nullptr;
    }
    auto* obj = static_cast<SyncObj*>(e->payload);
    if (obj->marker != e->aux) {
        shimAbort("kernel32", api, "句柄 0x%08X 的 payload 与 aux 不一致（marker=%u, aux=%u）—— 句柄表被写坏",
                  h, obj->marker, e->aux);
    }
    if (want != 0 && obj->marker != want) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 的种类是 %u，本 API 需要 %u ⇒ 失败", h, obj->marker,
                want);
        return nullptr;
    }
    return obj;
}

std::uint32_t newSyncHandle(X86& cpu, HandleKind kind, std::uint32_t marker, const std::string& name,
                            SyncObj* obj) {
    const std::uint32_t h = cpu.handles->add(kind, name.c_str(), obj, marker);
    return h;
}

// ---- CRITICAL_SECTION ----
void initCriticalSection(X86& cpu, ShimCall& call, std::uint32_t spin, const char* api) {
    requireMem(cpu, api);
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", api, "lpCriticalSection 为 NULL");
    cpu.mem->fillBytes(ea, 0, kCsSize);                 // 前 0x18 字节清零
    cpu.mem->wr32(ea + kCsSpinCount, spin);              // SpinCount @ +0x14（偏差 1）
    ++cpu.shim.critical_section_count;                   // 统计（Machine 诊断用）
    call.result = 1;                                     // InitializeCriticalSection* 返回 BOOL
}

void enterCriticalSection(X86& cpu, std::uint32_t ea, const char* api) {
    const std::uint32_t rc = cpu.mem->rd32(ea + kCsRecursionCount);
    if (rc == 0) {
        cpu.mem->wr32(ea + kCsOwningThread, cpu.shim.thread_id);
        cpu.mem->wr32(ea + kCsRecursionCount, 1);
        return;
    }
    const std::uint32_t owner = cpu.mem->rd32(ea + kCsOwningThread);
    if (owner != cpu.shim.thread_id) {
        shimAbort("kernel32", api,
                  "0x%08X 被 thread 0x%08X 持有（本 guest 是单线程，thread_id=0x%08X）—— 状态崩坏",
                  ea, owner, cpu.shim.thread_id);
    }
    cpu.mem->wr32(ea + kCsRecursionCount, rc + 1);  // **可重入**：只加递归计数
}

void shimInitializeCriticalSection(X86& cpu, ShimCall& call) {
    initCriticalSection(cpu, call, 0, "InitializeCriticalSection");
}

void shimInitializeCriticalSectionAndSpinCount(X86& cpu, ShimCall& call) {
    initCriticalSection(cpu, call, call.u32(1), "InitializeCriticalSectionAndSpinCount");
}

void shimInitializeCriticalSectionEx(X86& cpu, ShimCall& call) {
    initCriticalSection(cpu, call, call.u32(1), "InitializeCriticalSectionEx");
}

void shimEnterCriticalSection(X86& cpu, ShimCall& call) {
    requireMem(cpu, "EnterCriticalSection");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "EnterCriticalSection", "lpCriticalSection 为 NULL");
    enterCriticalSection(cpu, ea, "EnterCriticalSection");
    call.result = 0;  // void
}

void shimLeaveCriticalSection(X86& cpu, ShimCall& call) {
    requireMem(cpu, "LeaveCriticalSection");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "LeaveCriticalSection", "lpCriticalSection 为 NULL");
    const std::uint32_t rc = cpu.mem->rd32(ea + kCsRecursionCount);
    if (rc == 0) {
        shimAbort("kernel32", "LeaveCriticalSection",
                  "0x%08X 的 RecursionCount 已经是 0 —— Leave 没有配对的 Enter", ea);
    }
    const std::uint32_t owner = cpu.mem->rd32(ea + kCsOwningThread);
    if (owner != cpu.shim.thread_id) {
        shimAbort("kernel32", "LeaveCriticalSection",
                  "0x%08X 的拥有者是 0x%08X，不是本线程 0x%08X —— 非拥有者 Leave", ea, owner,
                  cpu.shim.thread_id);
    }
    cpu.mem->wr32(ea + kCsRecursionCount, rc - 1);
    if (rc - 1 == 0) {
        cpu.mem->wr32(ea + kCsOwningThread, 0);
    }
    call.result = 0;
}

void shimTryEnterCriticalSection(X86& cpu, ShimCall& call) {
    requireMem(cpu, "TryEnterCriticalSection");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "TryEnterCriticalSection", "lpCriticalSection 为 NULL");
    enterCriticalSection(cpu, ea, "TryEnterCriticalSection");
    call.result = 1;  // 偏差 3：单线程 guest ⇒ 永远能进（真的进了，状态与 Enter 一致）
}

void shimDeleteCriticalSection(X86& cpu, ShimCall& call) {
    requireMem(cpu, "DeleteCriticalSection");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "DeleteCriticalSection", "lpCriticalSection 为 NULL");
    cpu.mem->fillBytes(ea, 0, kCsSize);
    if (cpu.shim.critical_section_count > 0) --cpu.shim.critical_section_count;
    call.result = 0;
}

// ---- 事件 ----
void createEvent(X86& cpu, ShimCall& call, bool wide) {
    requireHandles(cpu, wide ? "CreateEventW" : "CreateEventA");
    const std::uint32_t manual = call.u32(1);
    const std::uint32_t initial = call.u32(2);
    const std::string name = wide ? narrowAscii(call.wstr(3)) : call.str(3);
    auto* obj = new SyncObj{};
    obj->marker = kSyncEvent;
    obj->manual = manual != 0;
    obj->signaled = initial != 0;
    call.result = newSyncHandle(cpu, HandleKind::Event, kSyncEvent, name, obj);
}

void shimCreateEventA(X86& cpu, ShimCall& call) { createEvent(cpu, call, false); }
void shimCreateEventW(X86& cpu, ShimCall& call) { createEvent(cpu, call, true); }

void shimSetEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "SetEvent");
    SyncObj* obj = syncOf(cpu, call.u32(0), kSyncEvent, "SetEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    obj->signaled = true;
    call.result = 1;
}

void shimResetEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ResetEvent");
    SyncObj* obj = syncOf(cpu, call.u32(0), kSyncEvent, "ResetEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    obj->signaled = false;
    call.result = 1;
}

void shimPulseEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "PulseEvent");
    SyncObj* obj = syncOf(cpu, call.u32(0), kSyncEvent, "PulseEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    // 偏差 6：置真 → 立刻置假（真机语义：唤醒所有等待者后复位；单线程无等待者 ⇒ 信号丢失）
    obj->signaled = true;
    obj->signaled = false;
    call.result = 1;
}

// ---- 信号量 ----
void createSemaphore(X86& cpu, ShimCall& call, bool wide) {
    requireHandles(cpu, wide ? "CreateSemaphoreW" : "CreateSemaphoreA");
    const std::uint32_t initial = call.u32(1);
    const std::uint32_t max_count = call.u32(2);
    const std::string name = wide ? narrowAscii(call.wstr(3)) : call.str(3);
    if (max_count == 0 || initial > max_count) {
        // 真实 Win32：参数非法 ⇒ 返回 0 + ERROR_INVALID_PARAMETER
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", wide ? "CreateSemaphoreW" : "CreateSemaphoreA",
                "lInitialCount=%u / lMaximumCount=%u 非法（要求 max>0 且 initial<=max）⇒ 返回 0",
                initial, max_count);
        call.result = 0;
        return;
    }
    auto* obj = new SyncObj{};
    obj->marker = kSyncSemaphore;
    obj->count = initial;
    obj->max_count = max_count;
    obj->signaled = initial > 0;
    call.result = newSyncHandle(cpu, HandleKind::Semaphore, kSyncSemaphore, name, obj);
}

void shimCreateSemaphoreA(X86& cpu, ShimCall& call) { createSemaphore(cpu, call, false); }
void shimCreateSemaphoreW(X86& cpu, ShimCall& call) { createSemaphore(cpu, call, true); }

void shimReleaseSemaphore(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ReleaseSemaphore");
    SyncObj* obj = syncOf(cpu, call.u32(0), kSyncSemaphore, "ReleaseSemaphore");
    if (!obj) {
        call.result = 0;
        return;
    }
    const std::uint32_t release = call.u32(1);
    const std::uint32_t prev_ptr = call.u32(2);
    if (release == 0 || obj->count + release > obj->max_count) {
        // 真实 Win32：超过 lMaximumCount ⇒ FALSE + ERROR_TOO_MANY_POSTS(298)
        cpu.shim.last_error = kErrTooManyPosts;
        shimLog("kernel32", "ReleaseSemaphore",
                "count=%u + release=%u 超过 max=%u ⇒ 返回 0（ERROR_TOO_MANY_POSTS）", obj->count,
                release, obj->max_count);
        call.result = 0;
        return;
    }
    const std::uint32_t prev = obj->count;
    obj->count = prev + release;
    obj->signaled = obj->count > 0;
    if (prev_ptr != 0) {
        cpu.mem->wr32(prev_ptr, prev);
    }
    call.result = 1;
}

// ---- 互斥量 ----
void createMutex(X86& cpu, ShimCall& call, bool wide) {
    requireHandles(cpu, wide ? "CreateMutexW" : "CreateMutexA");
    const std::uint32_t initial_owner = call.u32(1);
    const std::string name = wide ? narrowAscii(call.wstr(2)) : call.str(2);
    auto* obj = new SyncObj{};
    obj->marker = kSyncMutex;
    obj->manual = false;  // 互斥量是"自动复位"的：Release 才置 signaled
    obj->signaled = initial_owner == 0;
    obj->count = initial_owner != 0 ? 1u : 0u;
    obj->owner = initial_owner != 0 ? cpu.shim.thread_id : 0u;
    call.result = newSyncHandle(cpu, HandleKind::Mutex, kSyncMutex, name, obj);
}

void shimCreateMutexA(X86& cpu, ShimCall& call) { createMutex(cpu, call, false); }
void shimCreateMutexW(X86& cpu, ShimCall& call) { createMutex(cpu, call, true); }

void shimReleaseMutex(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ReleaseMutex");
    SyncObj* obj = syncOf(cpu, call.u32(0), kSyncMutex, "ReleaseMutex");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (obj->count == 0 || obj->owner != cpu.shim.thread_id) {
        cpu.shim.last_error = kErrNotOwner;
        shimLog("kernel32", "ReleaseMutex",
                "0x%08X 不是本线程持有的互斥量（owner=0x%08X, count=%u）⇒ 返回 0", call.u32(0),
                obj->owner, obj->count);
        call.result = 0;
        return;
    }
    if (obj->count > 0) --obj->count;
    if (obj->count == 0) {
        obj->owner = 0;
        obj->signaled = true;  // 可被下一个 WaitForSingleObject 拿走
    }
    call.result = 1;
}

// ---- 等待 ----
// 拿走一个信号（自动复位事件 / 信号量 / 互斥量）；调用前已确认可以拿
void consumeSignal(X86& cpu, SyncObj& obj) {
    switch (obj.marker) {
        case kSyncEvent:
            if (!obj.manual) obj.signaled = false;  // 自动复位事件：消费掉
            break;
        case kSyncSemaphore:
            if (obj.count > 0) --obj.count;
            obj.signaled = obj.count > 0;
            break;
        case kSyncMutex:
            obj.signaled = false;
            obj.owner = cpu.shim.thread_id;
            obj.count = 1;
            break;
        default:
            break;
    }
}

void shimWaitForSingleObject(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForSingleObject");
    const std::uint32_t h = call.u32(0);
    const std::uint32_t timeout = call.u32(1);
    SyncObj* obj = syncOf(cpu, h, 0, "WaitForSingleObject");
    if (!obj) {
        call.result = kWaitFailed;
        return;
    }
    // 互斥量的递归获取：已经由本线程持有时不必等（真机同样立即成功）
    if (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) {
        ++obj->count;
        call.result = kWaitObject0;
        return;
    }
    if (obj->signaled) {
        consumeSignal(cpu, *obj);
        call.result = kWaitObject0;
        return;
    }
    if (timeout == 0) {
        call.result = kWaitTimeout;  // dwMilliseconds==0 ⇒ 只是轮询，不是阻塞 ⇒ 不必打日志
        return;
    }
    // 偏差 4：⛔ 不阻塞（单线程 guest 真阻塞 = 死锁）—— 必须留一行
    shimLog("kernel32", "WaitForSingleObject",
            "阻塞等待在本模拟器里不实现（单线程 guest），返回 WAIT_TIMEOUT");
    call.result = kWaitTimeout;
}

void shimWaitForMultipleObjects(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForMultipleObjects");
    const std::uint32_t count = call.u32(0);
    const std::uint32_t handles_ea = call.u32(1);
    const std::uint32_t wait_all = call.u32(2);
    const std::uint32_t timeout = call.u32(3);
    if (count == 0 || count > 64 || handles_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "WaitForMultipleObjects", "nCount=%u / lpHandles=0x%08X 非法 ⇒ WAIT_FAILED",
                count, handles_ea);
        call.result = kWaitFailed;
        return;
    }
    if (wait_all != 0) {
        // 偏差 5：bWaitAll==TRUE 不支持 —— 打印 + WAIT_TIMEOUT（⛔ 不假装成功）
        shimLog("kernel32", "WaitForMultipleObjects",
                "bWaitAll=TRUE 在本模拟器里不实现（只支持 FALSE 且任一 signaled），返回 WAIT_TIMEOUT");
        call.result = kWaitTimeout;
        return;
    }
    // 先校验全部句柄（无效句柄要响亮失败，而不是被"前面那个先 signaled"掩盖）
    SyncObj* objs[64] = {};
    for (std::uint32_t i = 0; i < count; ++i) {
        objs[i] = syncOf(cpu, cpu.mem->rd32(handles_ea + 4u * i), 0, "WaitForMultipleObjects");
        if (!objs[i]) {
            call.result = kWaitFailed;
            return;
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        SyncObj* obj = objs[i];
        if (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) {
            ++obj->count;
            call.result = kWaitObject0 + i;
            return;
        }
        if (obj->signaled) {
            consumeSignal(cpu, *obj);
            call.result = kWaitObject0 + i;
            return;
        }
    }
    if (timeout != 0) {
        shimLog("kernel32", "WaitForMultipleObjects",
                "阻塞等待在本模拟器里不实现（单线程 guest），返回 WAIT_TIMEOUT");
    }
    call.result = kWaitTimeout;
}

// ---- 句柄关闭 ----
void shimCloseHandle(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "CloseHandle");
    const std::uint32_t h = call.u32(0);
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "CloseHandle", "无效句柄 0x%08X ⇒ 返回 0（ERROR_INVALID_HANDLE）", h);
        call.result = 0;
        return;
    }
    // 只回收本族建的 payload（靠 aux 标记；别的族的 payload 由它们自己管）
    if (e->payload && e->aux >= kSyncEvent && e->aux <= kSyncMutex) {
        delete static_cast<SyncObj*>(e->payload);
        e->payload = nullptr;
        e->aux = 0;
    }
    if (!cpu.handles->close(h)) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "CloseHandle", "句柄表拒绝关闭 0x%08X ⇒ 返回 0", h);
        call.result = 0;
        return;
    }
    call.result = 1;
}

// ---- Sleep（不睡，只推虚拟时钟）----
void shimSleep(X86& cpu, ShimCall& call) {
    if (!cpu.clock) shimAbort("kernel32", "Sleep", "cpu.clock 为空（Machine 没装配虚拟时钟）");
    std::uint32_t ms = call.u32(0);
    if (ms == kInfinite) {
        // 偏差 8：真机永不返回；这里按 1 秒推进并留一行
        shimLog("kernel32", "Sleep", "Sleep(INFINITE) 不阻塞（单线程 guest）—— 按 1 秒推进虚拟时钟");
        ms = 1000;
    }
    cpu.clock->advance100ns(static_cast<std::uint64_t>(ms) * 10000ull);
    call.result = 0;
}

}  // namespace

void registerSyncShims(ShimRegistry& shims) {
    // ---- 临界区（可重入；SpinCount @ +0x14，见偏差 1）----
    shims.add("kernel32", "InitializeCriticalSection", Conv::Stdcall, 1,
              shimInitializeCriticalSection);
    shims.add("kernel32", "InitializeCriticalSectionAndSpinCount", Conv::Stdcall, 2,
              shimInitializeCriticalSectionAndSpinCount);
    shims.add("kernel32", "InitializeCriticalSectionEx", Conv::Stdcall, 3,
              shimInitializeCriticalSectionEx);
    shims.add("kernel32", "EnterCriticalSection", Conv::Stdcall, 1, shimEnterCriticalSection);
    shims.add("kernel32", "LeaveCriticalSection", Conv::Stdcall, 1, shimLeaveCriticalSection);
    shims.add("kernel32", "TryEnterCriticalSection", Conv::Stdcall, 1, shimTryEnterCriticalSection);
    shims.add("kernel32", "DeleteCriticalSection", Conv::Stdcall, 1, shimDeleteCriticalSection);

    // ---- 事件 ----
    shims.add("kernel32", "CreateEventA", Conv::Stdcall, 4, shimCreateEventA);
    shims.add("kernel32", "CreateEventW", Conv::Stdcall, 4, shimCreateEventW);
    shims.add("kernel32", "SetEvent", Conv::Stdcall, 1, shimSetEvent);
    shims.add("kernel32", "ResetEvent", Conv::Stdcall, 1, shimResetEvent);
    shims.add("kernel32", "PulseEvent", Conv::Stdcall, 1, shimPulseEvent);

    // ---- 信号量（真实 Win32 是 4 个参数：含 lpName）----
    shims.add("kernel32", "CreateSemaphoreA", Conv::Stdcall, 4, shimCreateSemaphoreA);
    shims.add("kernel32", "CreateSemaphoreW", Conv::Stdcall, 4, shimCreateSemaphoreW);
    shims.add("kernel32", "ReleaseSemaphore", Conv::Stdcall, 3, shimReleaseSemaphore);

    // ---- 互斥量（真实 Win32 是 3 个参数：含 lpName）----
    shims.add("kernel32", "CreateMutexA", Conv::Stdcall, 3, shimCreateMutexA);
    shims.add("kernel32", "CreateMutexW", Conv::Stdcall, 3, shimCreateMutexW);
    shims.add("kernel32", "ReleaseMutex", Conv::Stdcall, 1, shimReleaseMutex);

    // ---- 等待 / 关闭 / 睡眠 ----
    shims.add("kernel32", "WaitForSingleObject", Conv::Stdcall, 2, shimWaitForSingleObject);
    shims.add("kernel32", "WaitForMultipleObjects", Conv::Stdcall, 4, shimWaitForMultipleObjects);
    shims.add("kernel32", "CloseHandle", Conv::Stdcall, 1, shimCloseHandle);
    shims.add("kernel32", "Sleep", Conv::Stdcall, 1, shimSleep);

    // ---- 线程 / 定时器队列：整族不做（README §2.5 / D5）⇒ 登记为未实现，执行到就响亮失败 ----
    shims.addUnimplemented("kernel32", "CreateThread",
                           "guest 单线程（README §2.5 / D5）；M1 不做线程");
    shims.addUnimplemented("kernel32", "CreateRemoteThread",
                           "guest 单线程（README §2.5 / D5）；M1 不做线程");
    shims.addUnimplemented("kernel32", "_beginthreadex",
                           "CRT 线程（README §2.5 / D5）；M1 不做线程");
    shims.addUnimplemented("kernel32", "CreateTimerQueue",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "CreateTimerQueueTimer",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "DeleteTimerQueueTimer",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "SignalObjectAndWait",
                           "原子 signal+wait 需要内核等待语义；本模拟器不阻塞（单线程 guest）");
    shims.addUnimplemented("kernel32", "WaitForSingleObjectEx",
                           "alertable 等待需要 APC / 线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "WaitForMultipleObjectsEx",
                           "alertable 等待需要 APC / 线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "OpenEventA",
                           "命名对象跨进程语义（README §2.5：单进程 guest）；M1a 不做");
    shims.addUnimplemented("kernel32", "OpenMutexA",
                           "命名对象跨进程语义（README §2.5：单进程 guest）；M1a 不做");
    shims.addUnimplemented("kernel32", "OpenSemaphoreA",
                           "命名对象跨进程语义（README §2.5：单进程 guest）；M1a 不做");
}

}  // namespace lift
