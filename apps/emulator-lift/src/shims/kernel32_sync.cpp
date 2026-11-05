// emulator-lift · kernel32 同步族（临界区 / 事件 / 信号量 / 互斥量 / SLIST / 等待 / 句柄复制 / Sleep）
//
// ★ 这一族的口径（README §2.5：**guest 业务单线程**）：
//   · 临界区**必须可重入**（这是本批点名的判据）：Enter 只改 guest 里的
//     `RecursionCount`(+0x08) / `OwningThread`(+0x0C)，不解引用任何宿主锁 ⇒ 天然可重入，
//     且重入次数写在 guest 结构里（guest 自己也能看见）。
//   · 等待对象（事件 / 信号量 / 互斥量）的宿主态挂在 `HandleTable` 的 `payload`（每 Machine 一份），
//     guest 只拿不透明整数句柄。payload 的回收统一走 `releasePayload()`（"还有别的活句柄吗"）。
//   · **命名对象**：名字 → 句柄表里同名同类的那一条（`HandleTable::findNamed`）。
//     `Create*` 命中已存在的名字 ⇒ 返回**同一个对象**的新句柄 + `ERROR_ALREADY_EXISTS(183)`；
//     `Open*` 找不到 ⇒ `NULL` + `ERROR_FILE_NOT_FOUND(2)`（MSDN 的 OpenMutex 原话）。
//     名字比较**大小写敏感**（MSDN OpenEventA/OpenMutexW/OpenSemaphoreW 原话："Name comparisons
//     are case sensitive"）；`Local\` / `Global\` 前缀按文档的两种写法剥掉（本模拟器只有一个会话）。
//   · **阻塞不实现**（单线程 guest ⇒ 真阻塞就是死锁）：未 signaled 的等待一律返回
//     `WAIT_TIMEOUT`(258)，**并打一行** —— 这是 SCOPE §3 允许的偏差形态，⛔ 不许静默。
//   · `Sleep` / `SleepEx` **不睡**：只把 `cpu.clock` 前进 `ms*10000`（100 ns 单位），保证确定性。
//   · **SList 是 guest 侧结构**：`SLIST_HEADER` 在 x86 上是 **8 字节**（`{Next(4), Depth(2),
//     CpuId/Sequence(2)}`，见 winnt.h 的 `_X86_` 分支），`SLIST_ENTRY` 是 **4 字节**（只有 `Next`）。
//     ⚠ 任务书写的"16 字节节点 / Alignment+Depth"是 **x64** 布局；本 guest 是 32 位 PE ⇒
//     必须按 x86 布局（用 x64 布局会写坏 guest 结构）。单线程下普通读改写即等价于 Interlocked。
//
// ⚠ 偏差（逐条）：
//   1. **CRITICAL_SECTION 的 SpinCount 在 +0x14，不是任务书写的 +0x1C**。真实 Win32 x86 的
//      CRITICAL_SECTION 是 0x18 字节：DebugInfo +0x00 / LockCount +0x04 / RecursionCount +0x08 /
//      OwningThread +0x0C / LockSemaphore +0x10 / **SpinCount +0x14**。
//      所以这里按真实布局（+0x14）落盘，并只清 0x18 字节。
//   2. **LockCount(+0x04) / LockSemaphore(+0x10) 不模拟**：前者只在有等待者时才非零（单线程
//      guest 没有等待者），后者是内核句柄。影响：直接读这两个字段的 guest 代码看到 0
//      （Win32 文档说它们是不透明的，⛔ 不该被读）。
//   3. **`TryEnterCriticalSection` 永远成功**（单线程 guest 没有竞争）—— 返回 TRUE 并真的进入。
//   4. **等待命中"需要等待" = 硬错误**（用户 2026-11-04）：`WaitForSingleObject` / `Ex` /
//      `SignalObjectAndWait` 在未 signaled 且 `dwMilliseconds != 0`（含 INFINITE）时 `shimAbort`
//      （打印 handle / timeout 后终止）；`WaitForMultipleObjects` / `Ex` 在"没有任何对象 signaled"
//      或"`bWaitAll=TRUE` 且至少一个未 signaled"且 `dwMilliseconds != 0` 时同样 abort。
//      ⛔ 不再返回 `WAIT_TIMEOUT`。`dwMilliseconds == 0` 的轮询仍返回 `WAIT_TIMEOUT`；
//      `bWaitAll=TRUE` 且全部 signaled ⇒ 返回 `WAIT_OBJECT_0` 并逐个消费。
//   5. **`PulseEvent` 不做"唤醒等待者再复位"**：先把 signaled 置真、**立刻**置假（单线程无等待者
//      ⇒ 这次脉冲等于丢失，与真机对空列表的行为一致）。
//   6. **`Sleep(INFINITE)` / `SleepEx(INFINITE)`** 不阻塞：按 1 秒推进虚拟时钟并打印一行。
//   7. **alertable 等待（`*Ex` / `SleepEx`）永不返回 `WAIT_IO_COMPLETION`(0xC0)**：本模拟器没有
//      APC 队列、没有 I/O 完成回调、没有第二个线程 ⇒ 唯一可能的返回值是 `WAIT_OBJECT_0` /
//      `WAIT_TIMEOUT` / `WAIT_FAILED`。`bAlertable` 被接受但只有"提示一次"的效果（文档口径：
//      该返回值**只**在 APC/完成例程真的排进队列时出现）。
//   8. **`SwitchToThread` 恒返回 0**：文档原话"若没有其它就绪线程，系统不切换，返回 0" ——
//      单线程 guest 正是这种情形。⛔ 不假装"切走了"。
//   9. **`DuplicateHandle` 只支持同一个 guest 进程**（本模拟器只有一个）：两个进程句柄都必须等于
//      `GetCurrentProcess()` 的伪句柄 `0xFFFFFFFF`（`hTargetProcessHandle` 可为 NULL +
//      `DUPLICATE_CLOSE_SOURCE`）。复制的句柄与源句柄**共享 payload**（文档："重复的句柄指向同一个
//      对象 … 文件位置对两个句柄始终相同"），生命周期由 `releasePayload()` 的"还有别的活句柄吗"
//      判定（最后一个关闭时才销毁 —— ⛔ 不用引用计数，见 handles.h）。
//      **伪句柄会被实体化**：源是 `GetCurrentThread()`(0xFFFFFFFE) / `GetCurrentProcess()`
//      (0xFFFFFFFF) 时按文档造一条**真**句柄表项（`payload = nullptr`、**没有 destroy**，
//      访问位 `DUPLICATE_SAME_ACCESS ⇒ THREAD_ALL_ACCESS / PROCESS_ALL_ACCESS`，否则取
//      `dwDesiredAccess`）。⛔ 仍不做"新句柄权限不得超过源句柄"的校验（没有安全描述符）：
//      `dwDesiredAccess` 只被记录，不与源句柄比较。
//   10. **伪线程**（用户 2026-11-05 裁决）：`CreateThread` **只发一个线程句柄**，
//      **不执行 `lpStartAddress`**。`ResumeThread` 递减挂起计数并返回旧值；
//      `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask` 按单核单线程
//      返回成功值；`GetThreadPriority` 对当前线程与伪线程都返回 `THREAD_PRIORITY_NORMAL(0)`。
//      ⇒ boot 路径上的声音线程 / 服务线程被"空转"掉，但它们的 start routine 一次都不会跑
//      （音频 silent / 视频 skip 已由 SCOPE 判掉）。⛔ 这不是真线程：不要用它承载必须执行的 guest 代码。
//   11. **仍未做的线程边角**：`CreateRemoteThread` / `_beginthreadex` / `ExitThread` /
//      `FreeLibraryAndExitThread` / `CreateTimerQueue*` / `DeleteTimerQueueTimer` 保持
//      `addUnimplemented`（执行到就响亮失败）。伪线程不执行 start routine ⇒ 这些通常不会被触发；
//      真触发 = 有 guest 代码依赖线程体，需人工检查后再补。
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---- 错误码（数值与 Win32 一致）----
constexpr std::uint32_t kErrFileNotFound = 2u;
constexpr std::uint32_t kErrAccessDenied = 5u;
constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrAlreadyExists = 183u;
constexpr std::uint32_t kErrNotOwner = 288u;
constexpr std::uint32_t kErrTooManyPosts = 298u;

// ---- 等待返回值 ----
constexpr std::uint32_t kWaitObject0 = 0u;
constexpr std::uint32_t kWaitTimeout = 0x102u;
constexpr std::uint32_t kWaitFailed = 0xFFFFFFFFu;
constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;

// ---- 访问位（Synchronization Object Security and Access Rights）----
constexpr std::uint32_t kMutexModifyState = 0x00000001u;
constexpr std::uint32_t kSemaphoreModifyState = 0x00000001u;
constexpr std::uint32_t kEventModifyState = 0x00000002u;
constexpr std::uint32_t kSynchronize = 0x00100000u;
constexpr std::uint32_t kStandardRightsRequired = 0x000F0000u;
constexpr std::uint32_t kEventAllAccess = kStandardRightsRequired | kSynchronize | 0x0003u;
constexpr std::uint32_t kMutexAllAccess = kStandardRightsRequired | kSynchronize | 0x0001u;
constexpr std::uint32_t kSemaphoreAllAccess = kStandardRightsRequired | kSynchronize | 0x0003u;

// DuplicateHandle 的 dwOptions
constexpr std::uint32_t kDuplicateCloseSource = 0x00000001u;
constexpr std::uint32_t kDuplicateSameAccess = 0x00000002u;

// 伪句柄实体化时 `DUPLICATE_SAME_ACCESS` 用的"全权"值（winnt.h 的 x86 数值）
constexpr std::uint32_t kThreadAllAccess = 0x001F03FFu;   // THREAD_ALL_ACCESS
constexpr std::uint32_t kProcessAllAccess = 0x001F0FFFu;  // PROCESS_ALL_ACCESS

// GetThreadPriority
constexpr std::uint32_t kThreadPriorityNormal = 0u;
constexpr std::uint32_t kThreadPriorityErrorReturn = 0x7FFFFFFFu;

// CreateThread 的 dwCreationFlags：只有这一位对伪线程有意义（初始挂起计数）
constexpr std::uint32_t kCreateSuspended = 0x00000004u;
constexpr std::uint32_t kThreadAffinityMask = 0x00000001u;  // 单核：只有 0 号 CPU

// CRITICAL_SECTION（x86，0x18 字节；见文件头偏差 1）
constexpr std::uint32_t kCsSize = 0x18u;
constexpr std::uint32_t kCsRecursionCount = 0x08u;
constexpr std::uint32_t kCsOwningThread = 0x0Cu;
constexpr std::uint32_t kCsSpinCount = 0x14u;

// SyncObj 的种类标记（同时写进 HandleEntry::aux）
constexpr std::uint32_t kSyncEvent = 1u;
constexpr std::uint32_t kSyncSemaphore = 2u;
constexpr std::uint32_t kSyncMutex = 3u;

// x86 SLIST_HEADER / SLIST_ENTRY（见文件头"口径"与偏差）
constexpr std::uint32_t kSlistHeaderSize = 8u;   // union { ULONGLONG Alignment; struct { Next; Depth; CpuId; } }
constexpr std::uint32_t kSlistEntrySize = 4u;    // struct { SLIST_ENTRY *Next; }
constexpr std::uint32_t kSlistDepthOff = 4u;     // WORD Depth

struct SyncObj {
    std::uint32_t marker = 0;    // kSyncEvent / kSyncSemaphore / kSyncMutex
    bool manual = false;         // 事件：手动复位？
    bool signaled = false;
    std::uint32_t count = 0;     // 信号量：当前计数；互斥量：递归层数
    std::uint32_t max_count = 0; // 信号量：上限
    std::uint32_t owner = 0;     // 互斥量：拥有者 thread_id
};

void destroySyncObj(void* p) { delete static_cast<SyncObj*>(p); }

// 伪线程的宿主侧记录（payload 挂在 `HandleKind::Thread` 的句柄上）。
// ★ 不保存 guest 栈 / 上下文：start routine 不会被调用，这里只记录可诊断的身份与挂起计数。
struct PseudoThread {
    std::uint32_t id = 0;
    std::uint32_t start = 0;
    std::uint32_t param = 0;
    std::uint32_t suspend_count = 0;
};

void destroyPseudoThread(void* p) { delete static_cast<PseudoThread*>(p); }

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

void requireCommitted(X86& cpu, std::uint32_t ea, std::size_t n, const char* api, const char* what) {
    if (ea == 0) shimAbort("kernel32", api, "%s 为 NULL", what);
    if (!cpu.mem->committed(ea, n)) {
        shimAbort("kernel32", api, "%s 未提交：ea=0x%08X n=%zu", what, ea, n);
    }
}

HandleEntry* entryOf(X86& cpu, std::uint32_t h, const char* api) {
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) {
        cpu.handles->invalidHandle(h, api);  // [[noreturn]]：打印 kind/值 + 已分配范围后停止
    }
    return e;
}

PseudoThread* pseudoThreadOf(X86& cpu, std::uint32_t h, const char* api) {
    HandleEntry* e = entryOf(cpu, h, api);
    if (e->kind != HandleKind::Thread || e->payload == nullptr) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "0x%08X 不是本模拟器建的伪线程句柄（kind=%s）⇒ 失败", h,
                handleKindName(e->kind));
        return nullptr;
    }
    return static_cast<PseudoThread*>(e->payload);
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

// 修改类 API 的访问位检查（文档：事件要 EVENT_MODIFY_STATE，信号量/互斥量要 *_MODIFY_STATE）
// ⛔ 没有 "access == 0 ⇒ true" 的短路：`Open*` 与 `DuplicateHandle` 都能给出**零访问位**的句柄，
//   那种句柄按文档就是**不许**改状态的（`Create*` 记的是 *_ALL_ACCESS，不受影响）。
bool hasModifyAccess(X86& cpu, std::uint32_t h, std::uint32_t mask, const char* api) {
    const HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) return false;
    if (e->aux < kSyncEvent || e->aux > kSyncMutex) {
        // 防御：调用点都在 `syncOf()` 之后（它已经拒绝非本族句柄）⇒ 走到这里说明调用顺序错了
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 不是本族的同步对象（aux=%u）⇒ FALSE + 错误 6", h,
                e->aux);
        return false;
    }
    if ((e->access & mask) != 0) return true;
    cpu.shim.last_error = kErrAccessDenied;
    shimLog("kernel32", api,
            "句柄 0x%08X 的访问位 0x%08X 不含需要的 0x%08X ⇒ FALSE + ERROR_ACCESS_DENIED(5)", h,
            e->access, mask);
    return false;
}

HandleKind kindOfMarker(std::uint32_t marker) {
    switch (marker) {
        case kSyncEvent: return HandleKind::Event;
        case kSyncSemaphore: return HandleKind::Semaphore;
        case kSyncMutex: return HandleKind::Mutex;
        default: return HandleKind::None;
    }
}

// 文档允许的两种命名空间前缀；本模拟器只有一个会话 ⇒ 两种都归一到同一个名字空间。
// ⛔ 前缀按文档的写法（大小写敏感）匹配，其余写法一律算名字的一部分。
std::string normalizeObjectName(std::string name) {
    if (name.size() > 6 && name.compare(0, 6, "Local\\") == 0) return name.substr(6);
    if (name.size() > 7 && name.compare(0, 7, "Global\\") == 0) return name.substr(7);
    return name;
}

// 按名字找"同类的活句柄"（Create* 的复用 / Open* 的查找共用）
std::uint32_t findNamedHandle(X86& cpu, std::uint32_t marker, const std::string& name) {
    if (name.empty()) return 0;
    const std::uint32_t h = cpu.handles->findNamed(kindOfMarker(marker), name);
    if (h == 0) return 0;
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->payload || e->aux != marker) return 0;
    return h;
}

std::uint32_t newSyncHandle(X86& cpu, HandleKind kind, std::uint32_t marker, const std::string& name,
                            SyncObj* obj, std::uint32_t access, bool inherit) {
    const std::uint32_t h = cpu.handles->add(kind, name.c_str(), obj, marker);
    if (HandleEntry* e = cpu.handles->get(h)) {
        e->destroy = destroySyncObj;
        e->access = access;
        e->inherit = inherit;
    }
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

// =======================================================================================
// 事件 / 信号量 / 互斥量（命名对象：Create* 复用、Open* 按名字打开）
// =======================================================================================
void createEvent(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "CreateEventW" : "CreateEventA";
    requireHandles(cpu, api);
    const std::uint32_t manual = call.u32(1);
    const std::uint32_t initial = call.u32(2);
    // ★ 真机签名是 4 个参数（CreateEventA(lpEventAttributes, bManualReset, bInitialState, lpName)）；
    //   `bInheritHandle` 在 SECURITY_ATTRIBUTES 里（第 0 个参数），本模拟器没有安全描述符 ⇒
    //   lpEventAttributes 被忽略（与文件族的"忽略 lpSecurityAttributes"同口径）⇒ inherit=false。
    const std::string name = normalizeObjectName(wide ? narrowAscii(call.wstr(3)) : call.str(3));
    const bool inherit = false;

    // 同名对象已存在 ⇒ 返回同一个对象的新句柄，参数被忽略（文档：
    // "If the named event object existed before the function call, the function returns a handle
    //  to the existing object and GetLastError returns ERROR_ALREADY_EXISTS"）
    if (const std::uint32_t existing = findNamedHandle(cpu, kSyncEvent, name)) {
        HandleEntry* e = cpu.handles->get(existing);
        auto* obj = static_cast<SyncObj*>(e->payload);
        const std::uint32_t h =
            newSyncHandle(cpu, HandleKind::Event, kSyncEvent, name, obj, kEventAllAccess, inherit);
        cpu.shim.last_error = kErrAlreadyExists;
        shimLog("kernel32", api, "命名事件 \"%s\" 已存在 ⇒ 返回同一对象的新句柄 0x%08X + 错误 183",
                name.c_str(), h);
        call.result = h;
        return;
    }

    auto* obj = new SyncObj{};
    obj->marker = kSyncEvent;
    obj->manual = manual != 0;
    obj->signaled = initial != 0;
    call.result = newSyncHandle(cpu, HandleKind::Event, kSyncEvent, name, obj, kEventAllAccess, inherit);
}

void shimCreateEventA(X86& cpu, ShimCall& call) { createEvent(cpu, call, false); }
void shimCreateEventW(X86& cpu, ShimCall& call) { createEvent(cpu, call, true); }

void openObject(X86& cpu, ShimCall& call, std::uint32_t marker, const char* api) {
    requireHandles(cpu, api);
    const std::uint32_t access = call.u32(0);
    const bool inherit = call.u32(1) != 0;
    const std::string name = normalizeObjectName(call.str(2));
    if (name.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpName 为 NULL / 空 ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    const std::uint32_t existing = findNamedHandle(cpu, marker, name);
    if (existing == 0) {
        cpu.shim.last_error = kErrFileNotFound;
        shimLog("kernel32", api, "没有名为 \"%s\" 的对象 ⇒ 返回 0 + 错误 2（ERROR_FILE_NOT_FOUND）",
                name.c_str());
        call.result = 0;
        return;
    }
    HandleEntry* e = cpu.handles->get(existing);
    auto* obj = static_cast<SyncObj*>(e->payload);
    call.result = newSyncHandle(cpu, kindOfMarker(marker), marker, name, obj, access, inherit);
}

void shimOpenEventA(X86& cpu, ShimCall& call) { openObject(cpu, call, kSyncEvent, "OpenEventA"); }
void shimOpenSemaphoreA(X86& cpu, ShimCall& call) {
    openObject(cpu, call, kSyncSemaphore, "OpenSemaphoreA");
}
void shimOpenMutexA(X86& cpu, ShimCall& call) { openObject(cpu, call, kSyncMutex, "OpenMutexA"); }

void shimSetEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "SetEvent");
    const std::uint32_t h = call.u32(0);
    SyncObj* obj = syncOf(cpu, h, kSyncEvent, "SetEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (!hasModifyAccess(cpu, h, kEventModifyState, "SetEvent")) {
        call.result = 0;
        return;
    }
    obj->signaled = true;
    call.result = 1;
}

void shimResetEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ResetEvent");
    const std::uint32_t h = call.u32(0);
    SyncObj* obj = syncOf(cpu, h, kSyncEvent, "ResetEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (!hasModifyAccess(cpu, h, kEventModifyState, "ResetEvent")) {
        call.result = 0;
        return;
    }
    obj->signaled = false;
    call.result = 1;
}

void shimPulseEvent(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "PulseEvent");
    const std::uint32_t h = call.u32(0);
    SyncObj* obj = syncOf(cpu, h, kSyncEvent, "PulseEvent");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (!hasModifyAccess(cpu, h, kEventModifyState, "PulseEvent")) {
        call.result = 0;
        return;
    }
    // 偏差 5：置真 → 立刻置假（真机语义：唤醒所有等待者后复位；单线程无等待者 ⇒ 信号丢失）
    obj->signaled = true;
    obj->signaled = false;
    call.result = 1;
}

void createSemaphore(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "CreateSemaphoreW" : "CreateSemaphoreA";
    requireHandles(cpu, api);
    const std::uint32_t initial = call.u32(1);
    const std::uint32_t max_count = call.u32(2);
    // 4 个参数（lpSemaphoreAttributes, lInitialCount, lMaximumCount, lpName）；继承标志在
    // SECURITY_ATTRIBUTES 里 ⇒ 忽略（同 CreateEvent 的口径）。
    const std::string name = normalizeObjectName(wide ? narrowAscii(call.wstr(3)) : call.str(3));
    const bool inherit = false;

    if (const std::uint32_t existing = findNamedHandle(cpu, kSyncSemaphore, name)) {
        HandleEntry* e = cpu.handles->get(existing);
        auto* obj = static_cast<SyncObj*>(e->payload);
        const std::uint32_t h = newSyncHandle(cpu, HandleKind::Semaphore, kSyncSemaphore, name, obj,
                                              kSemaphoreAllAccess, inherit);
        cpu.shim.last_error = kErrAlreadyExists;
        shimLog("kernel32", api, "命名信号量 \"%s\" 已存在 ⇒ 返回同一对象的新句柄 0x%08X + 错误 183",
                name.c_str(), h);
        call.result = h;
        return;
    }

    if (max_count == 0 || initial > max_count) {
        // 真实 Win32：参数非法 ⇒ 返回 0 + ERROR_INVALID_PARAMETER
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api,
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
    call.result = newSyncHandle(cpu, HandleKind::Semaphore, kSyncSemaphore, name, obj,
                                kSemaphoreAllAccess, inherit);
}

void shimCreateSemaphoreA(X86& cpu, ShimCall& call) { createSemaphore(cpu, call, false); }
void shimCreateSemaphoreW(X86& cpu, ShimCall& call) { createSemaphore(cpu, call, true); }

void shimReleaseSemaphore(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ReleaseSemaphore");
    const std::uint32_t h = call.u32(0);
    SyncObj* obj = syncOf(cpu, h, kSyncSemaphore, "ReleaseSemaphore");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (!hasModifyAccess(cpu, h, kSemaphoreModifyState, "ReleaseSemaphore")) {
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
        requireCommitted(cpu, prev_ptr, 4, "ReleaseSemaphore", "lpPreviousCount");
        cpu.mem->wr32(prev_ptr, prev);
    }
    call.result = 1;
}

void createMutex(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "CreateMutexW" : "CreateMutexA";
    requireHandles(cpu, api);
    const std::uint32_t initial_owner = call.u32(1);
    // 3 个参数（lpMutexAttributes, bInitialOwner, lpName）
    const std::string name = normalizeObjectName(wide ? narrowAscii(call.wstr(2)) : call.str(2));
    const bool inherit = false;

    if (const std::uint32_t existing = findNamedHandle(cpu, kSyncMutex, name)) {
        HandleEntry* e = cpu.handles->get(existing);
        auto* obj = static_cast<SyncObj*>(e->payload);
        const std::uint32_t h =
            newSyncHandle(cpu, HandleKind::Mutex, kSyncMutex, name, obj, kMutexAllAccess, inherit);
        cpu.shim.last_error = kErrAlreadyExists;
        shimLog("kernel32", api, "命名互斥量 \"%s\" 已存在 ⇒ 返回同一对象的新句柄 0x%08X + 错误 183",
                name.c_str(), h);
        call.result = h;
        return;
    }

    auto* obj = new SyncObj{};
    obj->marker = kSyncMutex;
    obj->manual = false;  // 互斥量是"自动复位"的：Release 才置 signaled
    obj->signaled = initial_owner == 0;
    obj->count = initial_owner != 0 ? 1u : 0u;
    obj->owner = initial_owner != 0 ? cpu.shim.thread_id : 0u;
    call.result = newSyncHandle(cpu, HandleKind::Mutex, kSyncMutex, name, obj, kMutexAllAccess, inherit);
}

void shimCreateMutexA(X86& cpu, ShimCall& call) { createMutex(cpu, call, false); }
void shimCreateMutexW(X86& cpu, ShimCall& call) { createMutex(cpu, call, true); }

void shimReleaseMutex(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ReleaseMutex");
    const std::uint32_t h = call.u32(0);
    SyncObj* obj = syncOf(cpu, h, kSyncMutex, "ReleaseMutex");
    if (!obj) {
        call.result = 0;
        return;
    }
    if (!hasModifyAccess(cpu, h, kMutexModifyState, "ReleaseMutex")) {
        call.result = 0;
        return;
    }
    if (obj->count == 0 || obj->owner != cpu.shim.thread_id) {
        cpu.shim.last_error = kErrNotOwner;
        shimLog("kernel32", "ReleaseMutex",
                "0x%08X 不是本线程持有的互斥量（owner=0x%08X, count=%u）⇒ 返回 0", h, obj->owner,
                obj->count);
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

// =======================================================================================
// 等待
// =======================================================================================
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

// alertable 的语义缺口：本模拟器没有 APC 队列 / I/O 完成例程 / 第二个线程
// ⇒ `*Ex` 的返回集里**永远没有** WAIT_IO_COMPLETION(0xC0)。只提示一次。
void noteAlertable(X86& cpu, const char* api, bool alertable) {
    if (!alertable || cpu.shim.warned_alertable) return;
    cpu.shim.warned_alertable = true;
    shimLog("kernel32", api,
            "bAlertable=TRUE：本模拟器没有 APC 队列 / I/O 完成例程 ⇒ 永不返回 WAIT_IO_COMPLETION(0xC0)"
            "（只可能是 WAIT_OBJECT_0 / WAIT_TIMEOUT / WAIT_FAILED）");
}

// 单个对象的一次等待（WaitForSingleObject / *Ex / SignalObjectAndWait 共用）。
// ⛔ 不阻塞：未 signaled 且 timeout != 0 ⇒ WAIT_TIMEOUT + 一行日志（偏差 4 / 7）。
std::uint32_t waitOne(X86& cpu, std::uint32_t h, std::uint32_t timeout, const char* api) {
    SyncObj* obj = syncOf(cpu, h, 0, api);
    if (!obj) return kWaitFailed;
    // 互斥量的递归获取：已经由本线程持有时不必等（真机同样立即成功）
    if (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) {
        ++obj->count;
        return kWaitObject0;
    }
    if (obj->signaled) {
        consumeSignal(cpu, *obj);
        return kWaitObject0;
    }
    if (timeout == 0) return kWaitTimeout;  // dwMilliseconds==0 ⇒ 只是轮询，不是阻塞 ⇒ 不硬错
    // ★ 用户 2026-11-04 裁决：命中"需要等待"的场景 ⇒ **硬错误**（不是返回 WAIT_TIMEOUT）。
    //   单线程 guest 里真阻塞会死锁；返回 TIMEOUT 又会给出 INFINITE 下不可能的值、且不取得对象
    //   ⇒ 停下来让人检查具体调用场景并设计等待方案。
    shimAbort("kernel32", api,
              "需要等待：hHandle=0x%08X 未 signaled，dwMilliseconds=%u（0x%08X）⇒ 本模拟器不阻塞；"
              "请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",
              h, timeout, timeout);
}

void shimWaitForSingleObject(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForSingleObject");
    call.result = waitOne(cpu, call.u32(0), call.u32(1), "WaitForSingleObject");
}

void shimWaitForSingleObjectEx(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForSingleObjectEx");
    noteAlertable(cpu, "WaitForSingleObjectEx", call.u32(2) != 0);
    call.result = waitOne(cpu, call.u32(0), call.u32(1), "WaitForSingleObjectEx");
}

// 多对象等待（base / Ex 共用）。`api` 只影响日志与诊断文本。
std::uint32_t waitMultiple(X86& cpu, std::uint32_t count, std::uint32_t handles_ea,
                           std::uint32_t wait_all, std::uint32_t timeout, const char* api) {
    if (count == 0 || count > 64 || handles_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "nCount=%u / lpHandles=0x%08X 非法 ⇒ WAIT_FAILED", count, handles_ea);
        return kWaitFailed;
    }
    // 先校验全部句柄（无效句柄要响亮失败，而不是被"前面那个先 signaled"掩盖）
    SyncObj* objs[64] = {};
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t h = cpu.mem->rd32(handles_ea + 4u * i);
        objs[i] = syncOf(cpu, h, 0, api);
        if (!objs[i]) return kWaitFailed;
    }
    if (wait_all != 0) {
        // bWaitAll=TRUE：全部 signaled 才返回 WAIT_OBJECT_0（并逐个消费）；
        // 有未 signaled ⇒ 需要等待（timeout=0 轮询 258；否则硬错误，见下）。
        bool all_ready = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            const SyncObj* obj = objs[i];
            const bool ready = (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) ||
                               obj->signaled;
            if (!ready) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            for (std::uint32_t i = 0; i < count; ++i) {
                SyncObj* obj = objs[i];
                if (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) {
                    ++obj->count;
                } else {
                    consumeSignal(cpu, *obj);
                }
            }
            return kWaitObject0;
        }
        if (timeout == 0) return kWaitTimeout;
        shimAbort("kernel32", api,
                  "需要等待：bWaitAll=TRUE 且至少一个对象未 signaled，dwMilliseconds=%u（0x%08X）⇒ "
                  "本模拟器不阻塞；请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",
                  timeout, timeout);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        SyncObj* obj = objs[i];
        if (obj->marker == kSyncMutex && obj->owner == cpu.shim.thread_id && obj->count > 0) {
            ++obj->count;
            return kWaitObject0 + i;
        }
        if (obj->signaled) {
            consumeSignal(cpu, *obj);
            return kWaitObject0 + i;
        }
    }
    if (timeout == 0) return kWaitTimeout;
    shimAbort("kernel32", api,
              "需要等待：nCount=%u 里没有任何对象 signaled，dwMilliseconds=%u（0x%08X）⇒ "
              "本模拟器不阻塞；请人工检查该调用场景并设计等待方案（SCOPE §1 等待条目）",
              count, timeout, timeout);
}

void shimWaitForMultipleObjects(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForMultipleObjects");
    call.result = waitMultiple(cpu, call.u32(0), call.u32(1), call.u32(2), call.u32(3),
                               "WaitForMultipleObjects");
}

// Ex 版：参数多一个 bAlertable（第 5 个），其余语义与 base 相同（偏差 7）
void shimWaitForMultipleObjectsEx(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "WaitForMultipleObjectsEx");
    noteAlertable(cpu, "WaitForMultipleObjectsEx", call.u32(4) != 0);
    call.result = waitMultiple(cpu, call.u32(0), call.u32(1), call.u32(2), call.u32(3),
                               "WaitForMultipleObjectsEx");
}

// ---- SignalObjectAndWait ----
// 文档：signal 一个对象 + wait 另一个对象**作为一次操作**；hObjectToSignal 可以是事件 / 互斥量 /
// 信号量。单线程下"原子性"天然成立（没有第二条执行流能观察中间态），所以 = signal 后立刻 wait。
// signal 侧的失败语义按文档：互斥量不是调用者拥有 ⇒ ERROR_NOT_OWNER；信号量超上限 ⇒
// ERROR_TOO_MANY_POSTS；事件要求 EVENT_MODIFY_STATE。
std::uint32_t signalObject(X86& cpu, std::uint32_t h, const char* api) {
    SyncObj* obj = syncOf(cpu, h, 0, api);
    if (!obj) return kWaitFailed;
    switch (obj->marker) {
        case kSyncEvent: {
            if (!hasModifyAccess(cpu, h, kEventModifyState, api)) return kWaitFailed;
            obj->signaled = true;
            return kWaitObject0;
        }
        case kSyncSemaphore: {
            if (!hasModifyAccess(cpu, h, kSemaphoreModifyState, api)) return kWaitFailed;
            if (obj->count + 1 > obj->max_count) {
                cpu.shim.last_error = kErrTooManyPosts;
                shimLog("kernel32", api, "信号量 count=%u 已达 max=%u ⇒ WAIT_FAILED", obj->count,
                        obj->max_count);
                return kWaitFailed;
            }
            ++obj->count;
            obj->signaled = true;
            return kWaitObject0;
        }
        case kSyncMutex: {
            if (!hasModifyAccess(cpu, h, kMutexModifyState, api)) return kWaitFailed;
            if (obj->count == 0 || obj->owner != cpu.shim.thread_id) {
                cpu.shim.last_error = kErrNotOwner;
                shimLog("kernel32", api,
                        "0x%08X 的互斥量不是本线程持有的（owner=0x%08X）⇒ WAIT_FAILED + 错误 288",
                        h, obj->owner);
                return kWaitFailed;
            }
            if (obj->count > 0) --obj->count;
            if (obj->count == 0) {
                obj->owner = 0;
                obj->signaled = true;
            }
            return kWaitObject0;
        }
        default:
            cpu.shim.last_error = kErrInvalidHandle;
            return kWaitFailed;
    }
}

void shimSignalObjectAndWait(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "SignalObjectAndWait");
    const std::uint32_t hs = call.u32(0);
    const std::uint32_t hw = call.u32(1);
    const std::uint32_t timeout = call.u32(2);
    const std::uint32_t alertable = call.u32(3);
    noteAlertable(cpu, "SignalObjectAndWait", alertable != 0);
    // 先校验两个句柄都在表里（顺序与文档一致：先检查参数，再动作）
    if (!cpu.handles->get(hs) || !cpu.handles->get(hw)) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "SignalObjectAndWait",
                "句柄非法（hObjectToSignal=0x%08X, hObjectToWaitOn=0x%08X）⇒ WAIT_FAILED", hs, hw);
        call.result = kWaitFailed;
        return;
    }
    if (signalObject(cpu, hs, "SignalObjectAndWait") == kWaitFailed) {
        call.result = kWaitFailed;
        return;
    }
    // 文档允许 hObjectToWaitOn 是"mutex / semaphore / event …"——本模拟器只有这几类；
    // 其余种类的句柄由 waitOne 的 syncOf 拒绝（WAIT_FAILED + 错误 6）。
    call.result = waitOne(cpu, hw, timeout, "SignalObjectAndWait");
}

// ---- SleepEx / SwitchToThread ----
void shimSleepEx(X86& cpu, ShimCall& call) {
    if (!cpu.clock) shimAbort("kernel32", "SleepEx", "cpu.clock 为空（Machine 没装配虚拟时钟）");
    std::uint32_t ms = call.u32(0);
    noteAlertable(cpu, "SleepEx", call.u32(1) != 0);
    if (ms == kInfinite) {
        // 偏差 6：真机永不返回；这里按 1 秒推进并留一行
        shimLog("kernel32", "SleepEx", "SleepEx(INFINITE) 不阻塞（单线程 guest）—— 按 1 秒推进虚拟时钟");
        ms = 1000;
    }
    cpu.clock->advance100ns(static_cast<std::uint64_t>(ms) * 10000ull);
    call.result = 0;  // 时间到 ⇒ 0（永不 WAIT_IO_COMPLETION，偏差 7）
}

void shimSwitchToThread(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 偏差 8：单线程 guest 没有"其它就绪线程"⇒ 文档所说的"不切换、返回 0"正是这里的行为
    call.result = 0;
}

// =======================================================================================
// SLIST（x86 布局：header 8 字节 / entry 4 字节）
// =======================================================================================
void slistRequire(X86& cpu, std::uint32_t head, const char* api) {
    requireMem(cpu, api);
    requireCommitted(cpu, head, kSlistHeaderSize, api, "ListHead");
}

void shimInitializeSListHead(X86& cpu, ShimCall& call) {
    const std::uint32_t head = call.u32(0);
    slistRequire(cpu, head, "InitializeSListHead");
    // 文档：把列表初始化为空。x86 的 SLIST_HEADER 是 8 字节（Next + Depth + CpuId/Sequence）
    // ⇒ 必须清满 8 字节（只清前 4 字节会留下非 0 的 Depth ⇒ QueryDepthSList 说谎）。
    cpu.mem->fillBytes(head, 0, kSlistHeaderSize);
    call.result = 0;  // void
}

void shimInterlockedPushEntrySList(X86& cpu, ShimCall& call) {
    const char* api = "InterlockedPushEntrySList";
    const std::uint32_t head = call.u32(0);
    const std::uint32_t entry = call.u32(1);
    slistRequire(cpu, head, api);
    requireCommitted(cpu, entry, kSlistEntrySize, api, "ListEntry");
    const std::uint32_t old = cpu.mem->rd32(head);      // 旧的首项（返回值）
    cpu.mem->wr32(entry, old);                          // entry->Next = 旧首项
    cpu.mem->wr32(head, entry);                         // header->Next = entry（插到表头）
    const auto depth = static_cast<std::uint16_t>(cpu.mem->rd16(head + kSlistDepthOff));
    cpu.mem->wr16(head + kSlistDepthOff, static_cast<std::uint16_t>(depth + 1u));
    call.result = old;  // 文档：返回"之前的第一个条目"；空表 ⇒ NULL
}

void shimInterlockedPopEntrySList(X86& cpu, ShimCall& call) {
    const char* api = "InterlockedPopEntrySList";
    const std::uint32_t head = call.u32(0);
    slistRequire(cpu, head, api);
    const std::uint32_t old = cpu.mem->rd32(head);
    if (old != 0) {
        requireCommitted(cpu, old, kSlistEntrySize, api, "首项的 Next");
        const std::uint32_t next = cpu.mem->rd32(old);
        cpu.mem->wr32(head, next);
        const auto depth = static_cast<std::uint16_t>(cpu.mem->rd16(head + kSlistDepthOff));
        cpu.mem->wr16(head + kSlistDepthOff, static_cast<std::uint16_t>(depth - 1u));
    }
    call.result = old;  // 文档：返回被摘下的条目；空表 ⇒ NULL
}

void shimInterlockedFlushSList(X86& cpu, ShimCall& call) {
    const char* api = "InterlockedFlushSList";
    const std::uint32_t head = call.u32(0);
    slistRequire(cpu, head, api);
    const std::uint32_t old = cpu.mem->rd32(head);
    cpu.mem->wr32(head, 0);
    cpu.mem->wr16(head + kSlistDepthOff, 0);
    call.result = old;  // 文档：返回被移除的那一串；空表 ⇒ NULL（**整条链**原样返回）
}

void shimQueryDepthSList(X86& cpu, ShimCall& call) {
    const char* api = "QueryDepthSList";
    const std::uint32_t head = call.u32(0);
    slistRequire(cpu, head, api);
    // 文档：返回条目数（USHORT；>65535 截断到 16 位）
    call.result = static_cast<std::uint32_t>(cpu.mem->rd16(head + kSlistDepthOff));
}

// =======================================================================================
// DuplicateHandle / CloseHandle / GetThreadPriority
// =======================================================================================
void shimDuplicateHandle(X86& cpu, ShimCall& call) {
    const char* api = "DuplicateHandle";
    requireHandles(cpu, api);
    const std::uint32_t src_proc = call.u32(0);
    const std::uint32_t src = call.u32(1);
    const std::uint32_t dst_proc = call.u32(2);
    const std::uint32_t target_ptr = call.u32(3);
    const std::uint32_t desired_access = call.u32(4);
    const std::uint32_t inherit = call.u32(5);
    const std::uint32_t options = call.u32(6);
    const bool close_source = (options & kDuplicateCloseSource) != 0;
    const bool same_access = (options & kDuplicateSameAccess) != 0;

    // 文档：hTargetProcessHandle 可为 NULL，但**只有**带 DUPLICATE_CLOSE_SOURCE 时
    if (dst_proc == 0 && !close_source) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "hTargetProcessHandle=NULL 但没带 DUPLICATE_CLOSE_SOURCE ⇒ FALSE + 错误 87");
        call.result = 0;
        return;
    }
    if (src_proc != cpu.shim.process_handle || (dst_proc != 0 && dst_proc != cpu.shim.process_handle)) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api,
                "只支持同一个 guest 进程（本模拟器只有一个进程）：src=0x%08X dst=0x%08X，"
                "期望 0x%08X ⇒ FALSE + 错误 6",
                src_proc, dst_proc, cpu.shim.process_handle);
        call.result = 0;
        return;
    }
    // 文档：`hSourceHandle` 是 `GetCurrentThread()` / `GetCurrentProcess()` 的**伪句柄**时，
    // DuplicateHandle 把它**转化成一个真句柄**（线程 / 进程）。本模拟器没有真的内核对象 ⇒
    // 实体化成一条 `payload = nullptr`、**没有 destroy** 的句柄表项。
    // ⛔ 绝不能给 destroy：`releasePayload` 会在最后一个句柄关闭时拿它去 free 一个不存在的对象。
    const bool pseudo_thread = (src == cpu.shim.thread_handle);
    const bool pseudo_process = (src == cpu.shim.process_handle);
    if (pseudo_thread || pseudo_process) {
        const char* what = pseudo_thread ? "GetCurrentThread()" : "GetCurrentProcess()";
        const HandleKind kind = pseudo_thread ? HandleKind::Thread : HandleKind::Other;
        const std::uint32_t all = pseudo_thread ? kThreadAllAccess : kProcessAllAccess;
        if (dst_proc == 0) {
            // 文档：hTargetProcessHandle 为 NULL 只允许配 DUPLICATE_CLOSE_SOURCE ⇒ 只关源、不复制。
            // 伪句柄本来就不在句柄表里（文档也不允许 CloseHandle 它）⇒ 没有可关的实体。
            shimLog("kernel32", api,
                    "%s 伪句柄 + hTargetProcessHandle=NULL + CLOSE_SOURCE ⇒ 不复制（伪句柄无可关实体）",
                    what);
            call.result = 1;
            return;
        }
        const std::uint32_t dup_pseudo = cpu.handles->add(kind, what, nullptr, 0);
        if (HandleEntry* ne = cpu.handles->get(dup_pseudo)) {
            ne->destroy = nullptr;  // 不是真对象（见上）
            ne->access = same_access ? all : desired_access;
            ne->inherit = inherit != 0;
        }
        // 文档：lpTargetHandle 可为 NULL（句柄照样被复制，只是不回传值）
        if (target_ptr != 0) {
            requireCommitted(cpu, target_ptr, 4, api, "lpTargetHandle");
            cpu.mem->wr32(target_ptr, dup_pseudo);
        }
        shimLog("kernel32", api,
                "%s 伪句柄 ⇒ 实体化成真句柄 0x%08X（kind=%s，访问位 0x%08X%s）",
                what, dup_pseudo, handleKindName(kind),
                same_access ? all : desired_access, same_access ? " SAME_ACCESS" : "");
        call.result = 1;
        return;
    }

    HandleEntry* e = cpu.handles->get(src);
    if (!e || !e->used) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "源句柄 0x%08X 无效 ⇒ FALSE + 错误 6", src);
        call.result = 0;
        return;
    }
    // ★ 先把要复制的字段**拷出来**：`HandleTable::add()` 会 push 进 `std::vector<Slot>`，
    //   扩容会让刚拿到的 `HandleEntry*` 失效（实测：随后读 `e->name` 是悬垂指针，
    //   崩在 fprintf 里；⛔ 不要先 add 再读 e->*）。
    const HandleKind src_kind = e->kind;
    const std::string src_name = e->name;
    void* const src_payload = e->payload;
    void (*const src_destroy)(void*) = e->destroy;
    const std::uint32_t src_aux = e->aux;
    const std::uint32_t src_access = e->access;

    std::uint32_t dup = 0;
    if (dst_proc != 0) {
        // 复制：与源句柄**共享** payload（文档："duplicate handle refers to the same object"）。
        // ★ 生命周期由 `releasePayload()` 的"还有没有别的活句柄指着同一个 payload"判定，
        //   ⛔ 这里不要写引用计数（两个句柄各自记数必然对不上）。
        dup = cpu.handles->add(src_kind, src_name.c_str(), src_payload, src_aux);
        if (HandleEntry* ne = cpu.handles->get(dup)) {
            ne->destroy = src_destroy;
            ne->access = same_access ? src_access : desired_access;
            ne->inherit = inherit != 0;
        }
        // 文档：lpTargetHandle 可为 NULL（只为兼容旧版本；句柄照样被复制，只是不回传值）
        if (target_ptr != 0) {
            requireCommitted(cpu, target_ptr, 4, api, "lpTargetHandle");
            cpu.mem->wr32(target_ptr, dup);
        }
    }

    if (close_source) {
        releasePayload(*cpu.handles, src);
        cpu.handles->close(src);  // 文档：无论错误状态如何都关闭源句柄
    }

    shimLog("kernel32", api,
            "0x%08X ⇒ 0x%08X（kind=%s%s%s%s）%s", src, dup, handleKindName(src_kind),
            src_name.empty() ? "" : " name=", src_name.c_str(),
            same_access ? " SAME_ACCESS" : "", close_source ? " CLOSE_SOURCE" : "");
    call.result = 1;
}

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
    // ★ 唯一的通用关闭入口：payload 的回收走 `releasePayload()`（别的句柄可能还指着它）
    releasePayload(*cpu.handles, h);
    if (!cpu.handles->close(h)) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "CloseHandle", "句柄表拒绝关闭 0x%08X ⇒ 返回 0", h);
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimGetThreadPriority(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "GetThreadPriority");
    const std::uint32_t h = call.u32(0);
    const bool is_current = h == cpu.shim.thread_handle;
    HandleEntry* e = cpu.handles->get(h);
    const bool is_thread = e && e->used && e->kind == HandleKind::Thread;
    if (!is_current && !is_thread) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "GetThreadPriority",
                "0x%08X 不是线程句柄（本模拟器只有当前线程 0x%08X）⇒ THREAD_PRIORITY_ERROR_RETURN",
                h, cpu.shim.thread_handle);
        call.result = kThreadPriorityErrorReturn;
        return;
    }
    call.result = kThreadPriorityNormal;  // 单线程 ⇒ 恒 THREAD_PRIORITY_NORMAL
}

// =======================================================================================
// 伪线程（用户 2026-11-05 裁决：只发句柄，不执行 start routine）
// =======================================================================================
void shimCreateThread(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "CreateThread");
    const std::uint32_t start = call.u32(2);
    const std::uint32_t param = call.u32(3);
    const std::uint32_t flags = call.u32(4);
    const std::uint32_t id_out = call.u32(5);
    if (start == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "CreateThread", "lpStartAddress 为 NULL ⇒ 0 + ERROR_INVALID_PARAMETER(87)");
        call.result = 0;
        return;
    }
    if (id_out != 0 && !cpu.mem->committed(id_out, 4)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "CreateThread", "lpThreadId=0x%08X 未提交 ⇒ 0 + ERROR_INVALID_PARAMETER(87)",
                id_out);
        call.result = 0;
        return;
    }
    auto* thread = new PseudoThread();
    thread->id = cpu.shim.next_pseudo_thread_id;
    cpu.shim.next_pseudo_thread_id += 4u;
    thread->start = start;
    thread->param = param;
    thread->suspend_count = (flags & kCreateSuspended) != 0 ? 1u : 0u;
    const std::uint32_t h = cpu.handles->add(HandleKind::Thread, "pseudo-thread", thread, thread->id);
    HandleEntry* entry = cpu.handles->get(h);
    if (entry == nullptr) shimAbort("kernel32", "CreateThread", "句柄表拒绝登记伪线程");
    entry->access = kThreadAllAccess;
    entry->destroy = &destroyPseudoThread;
    if (id_out != 0) cpu.mem->wr32(id_out, thread->id);
    shimLog("kernel32", "CreateThread",
            "伪线程 #%u（start=0x%08X param=0x%08X flags=0x%08X）⇒ 句柄 0x%08X；"
            "**不执行 start routine**（SCOPE §1 伪线程；音频 silent / 视频 skip）",
            thread->id, start, param, flags, h);
    call.result = h;
}

void shimResumeThread(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "ResumeThread");
    const std::uint32_t h = call.u32(0);
    if (h == cpu.shim.thread_handle) {
        shimLog("kernel32", "ResumeThread", "当前（唯一主）线程 0x%08X 未被挂起 ⇒ 返回 0", h);
        call.result = 0;
        return;
    }
    PseudoThread* thread = pseudoThreadOf(cpu, h, "ResumeThread");
    if (thread == nullptr) {
        call.result = 0xFFFFFFFFu;  // (DWORD)-1
        return;
    }
    const std::uint32_t previous = thread->suspend_count;
    if (thread->suspend_count > 0) --thread->suspend_count;
    shimLog("kernel32", "ResumeThread", "伪线程 #%u 的挂起计数 %u ⇒ %u（start routine 仍不执行）",
            thread->id, previous, thread->suspend_count);
    call.result = previous;
}

void shimSetThreadPriority(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "SetThreadPriority");
    const std::uint32_t h = call.u32(0);
    const int priority = call.i32(1);
    if (h == cpu.shim.thread_handle) {
        shimLog("kernel32", "SetThreadPriority", "当前线程优先级设为 %d（单线程实现，仅记账）", priority);
        call.result = 1;
        return;
    }
    PseudoThread* thread = pseudoThreadOf(cpu, h, "SetThreadPriority");
    if (thread == nullptr) {
        call.result = 0;
        return;
    }
    shimLog("kernel32", "SetThreadPriority", "伪线程 #%u 优先级设为 %d（仅记账）", thread->id, priority);
    call.result = 1;
}

void shimSetThreadAffinityMask(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "SetThreadAffinityMask");
    const std::uint32_t h = call.u32(0);
    const std::uint32_t mask = call.u32(1);
    if (mask == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "SetThreadAffinityMask", "dwThreadAffinityMask=0 ⇒ 0 + "
                                                     "ERROR_INVALID_PARAMETER(87)");
        call.result = 0;
        return;
    }
    if (h != cpu.shim.thread_handle && pseudoThreadOf(cpu, h, "SetThreadAffinityMask") == nullptr) {
        call.result = 0;
        return;
    }
    // 本模拟器只有 0 号 CPU；前一个掩码恒为 1（伪线程也共用这一颗核）
    shimLog("kernel32", "SetThreadAffinityMask", "0x%08X 的亲和性掩码 ⇒ 0x%08X（前一个 0x%08X）", h,
            mask, kThreadAffinityMask);
    call.result = kThreadAffinityMask;
}

void shimGetProcessAffinityMask(X86& cpu, ShimCall& call) {
    requireHandles(cpu, "GetProcessAffinityMask");
    const std::uint32_t process_out = call.u32(1);
    const std::uint32_t system_out = call.u32(2);
    if (process_out == 0 || system_out == 0 || !cpu.mem->committed(process_out, 4) ||
        !cpu.mem->committed(system_out, 4)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "GetProcessAffinityMask",
                "输出指针无效（process=0x%08X system=0x%08X）⇒ 0 + ERROR_INVALID_PARAMETER(87)",
                process_out, system_out);
        call.result = 0;
        return;
    }
    cpu.mem->wr32(process_out, kThreadAffinityMask);
    cpu.mem->wr32(system_out, kThreadAffinityMask);
    shimLog("kernel32", "GetProcessAffinityMask", "单核模拟器 ⇒ 进程/系统掩码都是 0x%08X",
            kThreadAffinityMask);
    call.result = 1;
}

// ---- Sleep（不睡，只推虚拟时钟）----
void shimSleep(X86& cpu, ShimCall& call) {
    if (!cpu.clock) shimAbort("kernel32", "Sleep", "cpu.clock 为空（Machine 没装配虚拟时钟）");
    std::uint32_t ms = call.u32(0);
    if (ms == kInfinite) {
        // 偏差 6：真机永不返回；这里按 1 秒推进并留一行
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
    shims.add("kernel32", "OpenEventA", Conv::Stdcall, 3, shimOpenEventA);
    shims.add("kernel32", "SetEvent", Conv::Stdcall, 1, shimSetEvent);
    shims.add("kernel32", "ResetEvent", Conv::Stdcall, 1, shimResetEvent);
    shims.add("kernel32", "PulseEvent", Conv::Stdcall, 1, shimPulseEvent);

    // ---- 信号量（真实 Win32 是 4 个参数：含 lpName）----
    shims.add("kernel32", "CreateSemaphoreA", Conv::Stdcall, 4, shimCreateSemaphoreA);
    shims.add("kernel32", "CreateSemaphoreW", Conv::Stdcall, 4, shimCreateSemaphoreW);
    shims.add("kernel32", "OpenSemaphoreA", Conv::Stdcall, 3, shimOpenSemaphoreA);
    shims.add("kernel32", "ReleaseSemaphore", Conv::Stdcall, 3, shimReleaseSemaphore);

    // ---- 互斥量（真实 Win32 是 3 个参数：含 lpName）----
    shims.add("kernel32", "CreateMutexA", Conv::Stdcall, 3, shimCreateMutexA);
    shims.add("kernel32", "CreateMutexW", Conv::Stdcall, 3, shimCreateMutexW);
    shims.add("kernel32", "OpenMutexA", Conv::Stdcall, 3, shimOpenMutexA);
    shims.add("kernel32", "ReleaseMutex", Conv::Stdcall, 1, shimReleaseMutex);

    // ---- 等待 / 关闭 / 睡眠 / 线程查询 ----
    shims.add("kernel32", "WaitForSingleObject", Conv::Stdcall, 2, shimWaitForSingleObject);
    shims.add("kernel32", "WaitForSingleObjectEx", Conv::Stdcall, 3, shimWaitForSingleObjectEx);
    shims.add("kernel32", "WaitForMultipleObjects", Conv::Stdcall, 4, shimWaitForMultipleObjects);
    shims.add("kernel32", "WaitForMultipleObjectsEx", Conv::Stdcall, 5, shimWaitForMultipleObjectsEx);
    shims.add("kernel32", "SignalObjectAndWait", Conv::Stdcall, 4, shimSignalObjectAndWait);
    shims.add("kernel32", "CloseHandle", Conv::Stdcall, 1, shimCloseHandle);
    shims.add("kernel32", "DuplicateHandle", Conv::Stdcall, 7, shimDuplicateHandle);
    shims.add("kernel32", "Sleep", Conv::Stdcall, 1, shimSleep);
    shims.add("kernel32", "SleepEx", Conv::Stdcall, 2, shimSleepEx);
    shims.add("kernel32", "SwitchToThread", Conv::Stdcall, 0, shimSwitchToThread);
    shims.add("kernel32", "GetThreadPriority", Conv::Stdcall, 1, shimGetThreadPriority);

    // ---- SLIST（x86 布局；单线程 ⇒ 普通读改写等价于 Interlocked）----
    shims.add("kernel32", "InitializeSListHead", Conv::Stdcall, 1, shimInitializeSListHead);
    shims.add("kernel32", "InterlockedPushEntrySList", Conv::Stdcall, 2,
              shimInterlockedPushEntrySList);
    shims.add("kernel32", "InterlockedPopEntrySList", Conv::Stdcall, 1,
              shimInterlockedPopEntrySList);
    shims.add("kernel32", "InterlockedFlushSList", Conv::Stdcall, 1, shimInterlockedFlushSList);
    shims.add("kernel32", "QueryDepthSList", Conv::Stdcall, 1, shimQueryDepthSList);

    // ---- 伪线程（用户 2026-11-05 裁决）：只发句柄，不执行 start routine；见文件头偏差 10 ----
    shims.add("kernel32", "CreateThread", Conv::Stdcall, 6, shimCreateThread);
    shims.add("kernel32", "ResumeThread", Conv::Stdcall, 1, shimResumeThread);
    shims.add("kernel32", "SetThreadPriority", Conv::Stdcall, 2, shimSetThreadPriority);
    shims.add("kernel32", "SetThreadAffinityMask", Conv::Stdcall, 2, shimSetThreadAffinityMask);
    shims.add("kernel32", "GetProcessAffinityMask", Conv::Stdcall, 3, shimGetProcessAffinityMask);

    // ---- 仍未做的线程边角 / 定时器队列 ⇒ 保持响亮失败（伪线程不跑 start routine，通常不会触发）----
    shims.addUnimplemented("kernel32", "CreateRemoteThread",
                           "伪线程只覆盖本进程的 CreateThread；远程线程不做");
    shims.addUnimplemented("kernel32", "_beginthreadex",
                           "CRT 线程入口（伪线程不执行 start routine；触发 = 有代码依赖线程体）");
    shims.addUnimplemented("kernel32", "ExitThread",
                           "伪线程不执行 start routine；主线程 ExitThread 需单独裁决");
    shims.addUnimplemented("kernel32", "FreeLibraryAndExitThread",
                           "CRT 卸载并退出线程（伪线程不执行 start routine）");
    shims.addUnimplemented("kernel32", "CreateTimerQueue",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "CreateTimerQueueTimer",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
    shims.addUnimplemented("kernel32", "DeleteTimerQueueTimer",
                           "定时器队列 = 服务线程（README §2.5 / D5）；M1 不做");
}

}  // namespace lift
