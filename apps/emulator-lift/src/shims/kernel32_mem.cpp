// emulator-lift · kernel32 内存族（Heap* / Local* / Global* / CoTaskMem* / Virtual*）
//
// ★ 这一族的口径（README §2.6）：**只有一个 guest 堆**。`HeapAlloc` / `LocalAlloc` /
//   `GlobalAlloc` / `CoTaskMemAlloc` 全部落到 `cpu.heap`（`GuestHeap`），所以
//   `HeapFree` 能释放 `LocalAlloc` 的块、`CoTaskMemFree` 能释放 `HeapAlloc` 的块 ——
//   ⛔ 不做"两个分配器各管一半"（那会让 free 跨 API 就炸）。返回的永远是 **guest 地址**。
// ★ 清零：`HEAP_ZERO_MEMORY`(8) / `LMEM_ZEROINIT`(0x40) / `GMEM_ZEROINIT`(0x40) /
//   `VirtualAlloc(MEM_COMMIT)` 都清零；`realloc` 保内容（由 GuestHeap 保证）。
// ★ 野指针 / double free ⇒ **响亮失败**（打印 + abort），不返回 0 蒙混过去。
//
// ⚠ 偏差（逐条）：
//   1. **只有一个堆**：`HeapCreate` 返回 `cpu.shim.heap_handle`（与 `GetProcessHeap` 同一个），
//      `HeapDestroy` 返回 TRUE 但不真的销毁。影响：guest 若靠"两个堆互不干扰"做隔离（没有
//      正当理由）会看到它们共享；`HeapDestroy` 之后的 HeapAlloc 仍能成功。
//   2. **`GlobalAlloc(GMEM_MOVEABLE)` 不做可移动内存**：句柄就是**可直接用的指针**（等价于
//      `GMEM_FIXED`）。⇒ `GlobalLock` 恒等、`GlobalUnlock` 返回 TRUE、`GlobalFlags` 返回 0。
//      影响：guest 若先 `GlobalLock` 再解引用没问题；若依赖"地址会变"则看不到变化（这是好事）。
//   3. **`VirtualAlloc` 是 bump 分配器**：`addr==0` 时从 `[0x20000000, 0x28000000)` 里 64 KiB
//      对齐切（128 MiB 上限；这些常量是本文件的口径，`vcpkg`/layout 都拿不到 layout）。
//      第一次调用时懒初始化（`va_cursor==0` ⇒ 置 0x20000000、`va_limit` 置 0x28000000）。
//      影响：**不可能**预留 1 GiB 以上的单块；超过 128 MiB 直接 OOM（返回 0 + 错误 8）。
//      ★ 与文档的对齐（2026-11-04 收口 REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4）：`MEM_COMMIT` 必须落在
//      **已预留**的区域里（显式地址不先 `MEM_RESERVE` ⇒ 0 + `ERROR_INVALID_ADDRESS(487)`，
//      ⛔ 不再"顺手把任意地址清零"）；bump 出来的块自动进区域账本（`GuestMemory::vaRanges()`），
//      所以"先 `MEM_RESERVE`、再对同一地址 `MEM_COMMIT`"成立。**仍然保留**的偏差：
//      `addr==0` 且只给 `MEM_COMMIT` 时按新块处理（文档要求先预留；这是 bump 口径的让步）。
//   4. **`VirtualFree(MEM_RELEASE)` 支持任意顺序的整段释放**（★ 2026-11-05 语义修正）：
//      要求 `dwSize==0`，`lpAddress` 必须是**某个活预留区的基址**（bump 出来的与显式地址预留的都算），
//      然后 `decommit [ea, 区域末尾)` 并把该区域从账本里摘掉。基址不对 / 重复释放 ⇒ 0 +
//      `ERROR_INVALID_ADDRESS(487)`，**别的区域不受影响**。
//      ★ 为什么改：旧实现（以及旧用例）只认"最后一块 bump 块"（LIFO），而实测**被测引擎**在窗口建立后
//      连续释放 5 个显式预留区（`0x3E000000` / `0x4D000000` / `0x4B000000` / `0x3D000000` / `0x31000000`），
//      五条全被拒成 `ERROR_INVALID_PARAMETER(87)` ⇒ 内存永远不回收。真实 Windows 允许任意顺序释放。
//      `MEM_DECOMMIT` 认 `dwSize==0`（文档：从 lpAddress 退到该区域末尾）——⛔ 旧实现直接拒收。
//   5. **`VirtualProtect` 不真的改保护**：本模拟器所有已提交页都是 RW，旧保护一律写回
//      `PAGE_READWRITE`(4)。请求值不是 PAGE_READWRITE 时打一行（否则每帧都要刷屏）。
//      影响：guest 靠"写只读页触发异常"做自检 / 自修改代码检测时不会触发 —— 这是本模拟器
//      的全局口径，不在这一族单独兜。
//   6. `HeapSize` / `LocalSize` / `GlobalSize` 的非法指针按 Windows 返回 0xFFFFFFFF + 错误 87（不 abort）；
//      而 `HeapFree` / `LocalFree` / `GlobalFree` 的野指针 **abort** —— 因为那才是真·内存错误。
#include <algorithm>
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrNotEnoughMemory = 8u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrInvalidAddress = 487u;  // ERROR_INVALID_ADDRESS

constexpr std::uint32_t kHeapZeroMemory = 0x00000008u;  // HEAP_ZERO_MEMORY
constexpr std::uint32_t kLmemZeroInit = 0x00000040u;    // LMEM_ZEROINIT
constexpr std::uint32_t kGmemZeroInit = 0x00000040u;    // GMEM_ZEROINIT

constexpr std::uint32_t kMemCommit = 0x00001000u;
constexpr std::uint32_t kMemReserve = 0x00002000u;
constexpr std::uint32_t kMemDecommit = 0x00004000u;
constexpr std::uint32_t kMemRelease = 0x00008000u;
constexpr std::uint32_t kPageReadWrite = 0x04u;

// VirtualAlloc 的 bump 区（文件头偏差 3）
constexpr std::uint32_t kVaBase = 0x20000000u;
constexpr std::uint32_t kVaEnd = 0x28000000u;
constexpr std::uint32_t kAllocGranularity = 0x00010000u;

std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) {
    const std::uint32_t rem = v % a;
    return rem == 0 ? v : static_cast<std::uint32_t>(v + (a - rem));
}

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

void requireHeap(X86& cpu, const char* api) {
    requireMem(cpu, api);
    if (!cpu.heap || !cpu.heap->ready()) {
        shimAbort("kernel32", api, "guest 堆不可用（GuestHeap::init 没跑）");
    }
}

// 0 字节请求在 Win32 里也返回**可用指针** ⇒ 统一按 1 字节申请。
std::uint32_t saneBytes(std::uint32_t bytes) { return bytes == 0 ? 1u : bytes; }

// 分配失败的统一出口（返回 0 + ERROR_NOT_ENOUGH_MEMORY + 一行日志）
std::uint32_t outOfMemory(X86& cpu, ShimCall& call, const char* api, std::uint32_t bytes) {
    cpu.shim.last_error = kErrNotEnoughMemory;
    shimLog("kernel32", api, "guest 堆耗尽：请求 %u 字节（%s）⇒ 返回 0", bytes,
            cpu.heap ? cpu.heap->lastError() : "heap=null");
    call.result = 0;
    return 0;
}

// 释放路径的指针校验：野指针 / double free / 宿主指针一律停（文件头偏差 6）
void checkFreePtr(X86& cpu, std::uint32_t ptr, const char* api) {
    if (!cpu.heap->owns(ptr)) {
        shimAbort("kernel32", api,
                  "0x%08X 不是本 guest 堆的块（野指针 / double free / 宿主指针）—— 响亮失败",
                  ptr);
    }
}

// ---- Heap* ----
void shimGetProcessHeap(X86& cpu, ShimCall& call) { call.result = cpu.shim.heap_handle; }

void shimHeapCreate(X86& cpu, ShimCall& call) {
    (void)call;
    // 偏差 1：本模拟器只有一个 guest 堆 ⇒ 返回同一个句柄
    call.result = cpu.shim.heap_handle;
}

void shimHeapDestroy(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 1;  // 偏差 1：不真的销毁（销毁后 HeapAlloc 依旧能用）
}

void shimHeapAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "HeapAlloc");
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t bytes = call.u32(2);
    const std::uint32_t ptr = cpu.heap->alloc(saneBytes(bytes), (flags & kHeapZeroMemory) != 0);
    if (ptr == 0) {
        outOfMemory(cpu, call, "HeapAlloc", bytes);
        return;
    }
    call.result = ptr;
}

void shimHeapFree(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "HeapFree");
    const std::uint32_t ptr = call.u32(2);
    if (ptr == 0) {
        call.result = 1;  // HeapFree(NULL) 在 Win32 里成功
        return;
    }
    checkFreePtr(cpu, ptr, "HeapFree");
    if (!cpu.heap->free(ptr)) {
        shimAbort("kernel32", "HeapFree", "堆拒绝了 0x%08X（%s）", ptr, cpu.heap->lastError());
    }
    call.result = 1;
}

void shimHeapReAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "HeapReAlloc");
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t ptr = call.u32(2);
    const std::uint32_t bytes = call.u32(3);
    if (ptr == 0) {  // Win32：HeapReAlloc(NULL) == HeapAlloc
        const std::uint32_t fresh = cpu.heap->alloc(saneBytes(bytes), (flags & kHeapZeroMemory) != 0);
        if (fresh == 0) {
            outOfMemory(cpu, call, "HeapReAlloc", bytes);
            return;
        }
        call.result = fresh;
        return;
    }
    checkFreePtr(cpu, ptr, "HeapReAlloc");
    const std::uint32_t out =
        cpu.heap->realloc(ptr, saneBytes(bytes), (flags & kHeapZeroMemory) != 0);
    if (out == 0) {
        outOfMemory(cpu, call, "HeapReAlloc", bytes);
        return;
    }
    call.result = out;
}

void shimHeapSize(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "HeapSize");
    const std::uint32_t ptr = call.u32(2);
    if (ptr == 0 || !cpu.heap->owns(ptr)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "HeapSize", "0x%08X 不是本 guest 堆的块 ⇒ 返回 0xFFFFFFFF", ptr);
        call.result = 0xFFFFFFFFu;
        return;
    }
    call.result = cpu.heap->sizeOf(ptr);
}

// ---- Local* ----
void shimLocalAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "LocalAlloc");
    const std::uint32_t flags = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    const std::uint32_t ptr = cpu.heap->alloc(saneBytes(bytes), (flags & kLmemZeroInit) != 0);
    if (ptr == 0) {
        outOfMemory(cpu, call, "LocalAlloc", bytes);
        return;
    }
    call.result = ptr;
}

void shimLocalFree(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "LocalFree");
    const std::uint32_t ptr = call.u32(0);
    if (ptr == 0) {
        call.result = 0;  // LocalFree(NULL) 返回 NULL
        return;
    }
    checkFreePtr(cpu, ptr, "LocalFree");
    if (!cpu.heap->free(ptr)) {
        shimAbort("kernel32", "LocalFree", "堆拒绝了 0x%08X（%s）", ptr, cpu.heap->lastError());
    }
    call.result = 0;  // Win32：成功返回 NULL
}

void shimLocalReAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "LocalReAlloc");
    const std::uint32_t ptr = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    const std::uint32_t flags = call.u32(2);
    if (ptr == 0) {
        const std::uint32_t fresh = cpu.heap->alloc(saneBytes(bytes), (flags & kLmemZeroInit) != 0);
        if (fresh == 0) {
            outOfMemory(cpu, call, "LocalReAlloc", bytes);
            return;
        }
        call.result = fresh;
        return;
    }
    checkFreePtr(cpu, ptr, "LocalReAlloc");
    const std::uint32_t out = cpu.heap->realloc(ptr, saneBytes(bytes), (flags & kLmemZeroInit) != 0);
    if (out == 0) {
        outOfMemory(cpu, call, "LocalReAlloc", bytes);
        return;
    }
    call.result = out;
}

void shimLocalSize(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "LocalSize");
    const std::uint32_t ptr = call.u32(0);
    if (ptr == 0 || !cpu.heap->owns(ptr)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "LocalSize", "0x%08X 不是本 guest 堆的块 ⇒ 返回 0", ptr);
        call.result = 0;
        return;
    }
    call.result = cpu.heap->sizeOf(ptr);
}

// ---- Global* ----
void shimGlobalAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "GlobalAlloc");
    const std::uint32_t flags = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    // 偏差 2：GMEM_MOVEABLE / GMEM_DISCARDABLE 被忽略，句柄就是指针
    const std::uint32_t ptr = cpu.heap->alloc(saneBytes(bytes), (flags & kGmemZeroInit) != 0);
    if (ptr == 0) {
        outOfMemory(cpu, call, "GlobalAlloc", bytes);
        return;
    }
    call.result = ptr;
}

void shimGlobalFree(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "GlobalFree");
    const std::uint32_t ptr = call.u32(0);
    if (ptr == 0) {
        call.result = 0;
        return;
    }
    checkFreePtr(cpu, ptr, "GlobalFree");
    if (!cpu.heap->free(ptr)) {
        shimAbort("kernel32", "GlobalFree", "堆拒绝了 0x%08X（%s）", ptr, cpu.heap->lastError());
    }
    call.result = 0;  // Win32：成功返回 NULL
}

void shimGlobalReAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "GlobalReAlloc");
    const std::uint32_t ptr = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    const std::uint32_t flags = call.u32(2);
    if (ptr == 0) {
        const std::uint32_t fresh = cpu.heap->alloc(saneBytes(bytes), (flags & kGmemZeroInit) != 0);
        if (fresh == 0) {
            outOfMemory(cpu, call, "GlobalReAlloc", bytes);
            return;
        }
        call.result = fresh;
        return;
    }
    checkFreePtr(cpu, ptr, "GlobalReAlloc");
    const std::uint32_t out = cpu.heap->realloc(ptr, saneBytes(bytes), (flags & kGmemZeroInit) != 0);
    if (out == 0) {
        outOfMemory(cpu, call, "GlobalReAlloc", bytes);
        return;
    }
    call.result = out;
}

void shimGlobalSize(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "GlobalSize");
    const std::uint32_t ptr = call.u32(0);
    if (ptr == 0 || !cpu.heap->owns(ptr)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "GlobalSize", "0x%08X 不是本 guest 堆的块 ⇒ 返回 0", ptr);
        call.result = 0;
        return;
    }
    call.result = cpu.heap->sizeOf(ptr);
}

void shimGlobalLock(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = call.u32(0);  // 偏差 2：句柄即指针 ⇒ 恒等
}

void shimGlobalUnlock(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 1;  // 偏差 2
}

void shimGlobalFlags(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 0;  // 偏差 2：没有可移动 / 可丢弃的块
}

// ---- CoTaskMem*（同一个堆）----
void shimCoTaskMemAlloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "CoTaskMemAlloc");
    const std::uint32_t bytes = call.u32(0);
    const std::uint32_t ptr = cpu.heap->alloc(saneBytes(bytes), false);
    if (ptr == 0) {
        outOfMemory(cpu, call, "CoTaskMemAlloc", bytes);
        return;
    }
    call.result = ptr;
}

void shimCoTaskMemFree(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "CoTaskMemFree");
    const std::uint32_t ptr = call.u32(0);
    if (ptr == 0) {
        call.result = 0;
        return;
    }
    checkFreePtr(cpu, ptr, "CoTaskMemFree");
    if (!cpu.heap->free(ptr)) {
        shimAbort("kernel32", "CoTaskMemFree", "堆拒绝了 0x%08X（%s）", ptr, cpu.heap->lastError());
    }
    call.result = 0;
}

void shimCoTaskMemRealloc(X86& cpu, ShimCall& call) {
    requireHeap(cpu, "CoTaskMemRealloc");
    const std::uint32_t ptr = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    if (ptr == 0) {
        const std::uint32_t fresh = cpu.heap->alloc(saneBytes(bytes), false);
        if (fresh == 0) {
            outOfMemory(cpu, call, "CoTaskMemRealloc", bytes);
            return;
        }
        call.result = fresh;
        return;
    }
    checkFreePtr(cpu, ptr, "CoTaskMemRealloc");
    const std::uint32_t out = cpu.heap->realloc(ptr, saneBytes(bytes), false);
    if (out == 0) {
        outOfMemory(cpu, call, "CoTaskMemRealloc", bytes);
        return;
    }
    call.result = out;
}

// ---- Virtual* ----
// `MEM_COMMIT` 只把**未提交**的页变成已提交：文档说已提交的页**内容不变** ⇒ 不清零那一部分
// （⛔ 旧实现无条件 `commitZeroed` ⇒ "对已提交的地址再 COMMIT"会静默清掉调用方的数据）。
bool commitZeroedUncommitted(X86& cpu, std::uint32_t ea, std::uint32_t span) {
    if (cpu.mem->committed(ea, span)) return true;  // 整段都已提交：成功，且不动内容
    std::uint32_t first = ea;
    while (first < ea + span &&
           cpu.mem->committed(first, GuestMemory::kPage)) {
        first += GuestMemory::kPage;
    }
    if (first >= ea + span) return true;
    return cpu.mem->commitZeroed(first, static_cast<std::size_t>(ea + span - first), RegionKind::Other,
                                 "VirtualAlloc");
}

void shimVirtualAlloc(X86& cpu, ShimCall& call) {
    requireMem(cpu, "VirtualAlloc");
    const std::uint32_t want = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    const std::uint32_t type = call.u32(2);
    const bool do_commit = (type & kMemCommit) != 0;
    const bool do_reserve = (type & kMemReserve) != 0;
    if (bytes == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "VirtualAlloc", "dwSize == 0 ⇒ 返回 0");
        call.result = 0;
        return;
    }
    if (!do_commit && !do_reserve) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "VirtualAlloc", "flAllocationType=0x%08X 既不是 MEM_COMMIT 也不是 "
                                            "MEM_RESERVE ⇒ 返回 0", type);
        call.result = 0;
        return;
    }
    const std::uint32_t page_span = alignUp(bytes, GuestMemory::kPage);
    const std::uint32_t reserve_span = alignUp(bytes, kAllocGranularity);

    if (want != 0) {  // 指定地址：不动 bump 游标
        if ((want & GuestMemory::kPageMask) != 0) {
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", "VirtualAlloc", "指定地址 0x%08X 没有页对齐 ⇒ 返回 0", want);
            call.result = 0;
            return;
        }
        VaRangeTable& va = cpu.mem->vaRanges();
        if (do_reserve) {
            // 文档：同一段不能**重复预留** ⇒ ERROR_INVALID_ADDRESS
            if (va.overlapsLive(want, reserve_span)) {
                cpu.shim.last_error = kErrInvalidAddress;
                shimLog("kernel32", "VirtualAlloc",
                        "0x%08X 起 0x%X 字节已经在保留区里 ⇒ 返回 0（ERROR_INVALID_ADDRESS）", want,
                        reserve_span);
                call.result = 0;
                return;
            }
            if (do_commit && !commitZeroedUncommitted(cpu, want, page_span)) {
                cpu.shim.last_error = kErrNotEnoughMemory;
                shimLog("kernel32", "VirtualAlloc", "提交 [0x%08X,+0x%X) 失败：%s ⇒ 返回 0", want,
                        page_span, cpu.mem->lastError());
                call.result = 0;
                return;
            }
            cpu.mem->annotate(want, reserve_span, RegionKind::Other,
                              do_commit ? "VirtualAlloc(hint,reserve|commit)" : "VirtualAlloc(hint,reserve)");
            va.add(want, want + reserve_span, do_commit ? page_span : 0u, /*bump=*/false);
        } else {
            // 文档（VirtualAlloc 的 Remarks / MEM_COMMIT）：**只有已保留的页**才能提交，
            // 否则失败 + ERROR_INVALID_ADDRESS(487)。⛔ 不许对任意地址 `commitZeroed`。
            if (!va.covers(want, page_span)) {
                cpu.shim.last_error = kErrInvalidAddress;
                shimLog("kernel32", "VirtualAlloc",
                        "0x%08X 起 0x%X 字节没有先 MEM_RESERVE ⇒ 返回 0（ERROR_INVALID_ADDRESS）", want,
                        page_span);
                call.result = 0;
                return;
            }
            if (!commitZeroedUncommitted(cpu, want, page_span)) {
                cpu.shim.last_error = kErrNotEnoughMemory;
                shimLog("kernel32", "VirtualAlloc", "提交 [0x%08X,+0x%X) 失败：%s ⇒ 返回 0", want,
                        page_span, cpu.mem->lastError());
                call.result = 0;
                return;
            }
            if (VaRange* r = va.find(want)) {
                const std::uint32_t end_committed =
                    want + page_span > r->begin + r->committed ? want + page_span - r->begin : r->committed;
                r->committed = end_committed;
            }
        }
        call.result = want;
        return;
    }

    // 偏差 3：addr==0 ⇒ 从 [kVaBase, kVaEnd) bump，64 KiB 对齐
    if (cpu.shim.va_cursor == 0) {
        cpu.shim.va_cursor = kVaBase;
        cpu.shim.va_limit = kVaEnd;
    }
    const std::uint32_t span = reserve_span;
    if (span == 0 || cpu.shim.va_cursor >= cpu.shim.va_limit ||
        span > cpu.shim.va_limit - cpu.shim.va_cursor) {
        cpu.shim.last_error = kErrNotEnoughMemory;
        shimLog("kernel32", "VirtualAlloc",
                "bump 区 [0x%08X,0x%08X) 用尽（请求 %u 字节，剩余 0x%X）⇒ 返回 0", kVaBase, kVaEnd,
                bytes, cpu.shim.va_cursor < cpu.shim.va_limit ? cpu.shim.va_limit - cpu.shim.va_cursor : 0u);
        call.result = 0;
        return;
    }
    const std::uint32_t ea = cpu.shim.va_cursor;
    if (do_commit) {
        if (!commitZeroedUncommitted(cpu, ea, page_span)) {
            cpu.shim.last_error = kErrNotEnoughMemory;
            shimLog("kernel32", "VirtualAlloc", "提交 [0x%08X,+0x%X) 失败：%s ⇒ 返回 0", ea,
                    page_span, cpu.mem->lastError());
            call.result = 0;
            return;
        }
    } else {
        cpu.mem->annotate(ea, span, RegionKind::Other, "VirtualAlloc(bump,reserve)");
    }
    cpu.mem->vaRanges().add(ea, ea + span, do_commit ? page_span : 0u, /*bump=*/true);
    cpu.shim.va_cursor += span;  // 只有成功才推进（失败不留洞）
    call.result = ea;
}

void shimVirtualFree(X86& cpu, ShimCall& call) {
    requireMem(cpu, "VirtualFree");
    const std::uint32_t ea = call.u32(0);
    const std::uint32_t bytes = call.u32(1);
    const std::uint32_t type = call.u32(2);

    if ((type & kMemRelease) != 0) {
        if (bytes != 0) {  // Win32：MEM_RELEASE 要求 dwSize == 0
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", "VirtualFree", "MEM_RELEASE 要求 dwSize==0（给的是 %u）⇒ 返回 0",
                    bytes);
            call.result = 0;
            return;
        }
        // ★ 任意顺序的整段释放（2026-11-05 修正，实测驱动）：
        //   真实 Windows 允许**按任意顺序** `MEM_RELEASE` 每一个独立预留区。旧实现只认
        //   "最后一块 bump 块"（LIFO），而实测引擎在窗口建立后连续释放 5 个**显式预留**区
        //   （`0x3E000000` / `0x4D000000` / `0x4B000000` / `0x3D000000` / `0x31000000`）
        //   ⇒ 五条全被拒成 `ERROR_INVALID_PARAMETER`，内存永远不回收（boot 里可见）。
        //   判据仍是 Win32 的：lpAddress 必须是**预留区的基址**，否则 `ERROR_INVALID_ADDRESS(487)`。
        VaRange* r = cpu.mem->vaRanges().findBegin(ea);
        if (!r) {
            cpu.shim.last_error = kErrInvalidAddress;
            shimLog("kernel32", "VirtualFree",
                    "0x%08X 不是任何活预留区的基址（重复释放 / 未预留）⇒ 返回 0（ERROR_INVALID_ADDRESS）",
                    ea);
            call.result = 0;
            return;
        }
        // 整段归还：decommit [ea, 区域末尾) 并把它从活区域里摘掉。
        const std::uint32_t span = r->end - r->begin;
        cpu.mem->decommit(ea, span);
        const bool was_bump = r->bump;
        cpu.mem->vaRanges().release(ea);
        // bump 块若正好是游标所在的位置，游标退回去（否则保留高水位：中间挖洞是合法的，
        // 真实 Windows 也不保证再分配的连续性）。
        if (was_bump && cpu.shim.va_cursor == ea) cpu.shim.va_cursor = ea;
        call.result = 1;
        return;
    }

    if ((type & kMemDecommit) != 0) {
        if ((ea & GuestMemory::kPageMask) != 0) {
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", "VirtualFree", "MEM_DECOMMIT 需要页对齐的地址 (0x%08X) ⇒ 返回 0", ea);
            call.result = 0;
            return;
        }
        VaRange* r = cpu.mem->vaRanges().find(ea);
        if (!r) {
            // 文档：MEM_DECOMMIT 的 lpAddress 必须在**已保留**的区域里（否则 ERROR_INVALID_ADDRESS）
            cpu.shim.last_error = kErrInvalidAddress;
            shimLog("kernel32", "VirtualFree",
                    "0x%08X 不在任何保留区里 ⇒ 返回 0（ERROR_INVALID_ADDRESS）", ea);
            call.result = 0;
            return;
        }
        if (bytes == 0) {
            // 文档：dwSize==0 ⇒ 从 lpAddress **退到该区域末尾**（整段）
            cpu.mem->decommit(ea, r->end - ea);
            r->committed = ea > r->begin ? (r->committed > ea - r->begin ? ea - r->begin : r->committed) : 0u;
            call.result = 1;
            return;
        }
        const std::uint32_t span = alignUp(bytes, GuestMemory::kPage);
        cpu.mem->decommit(ea, span);
        // 记账：只有"退到尾部"的 decommit 才会让已提交前缀变短
        if (ea + span >= r->begin + r->committed) r->committed = ea - r->begin;
        call.result = 1;
        return;
    }

    cpu.shim.last_error = kErrInvalidParameter;
    shimLog("kernel32", "VirtualFree", "dwFreeType=0x%08X 既不是 MEM_RELEASE 也不是 MEM_DECOMMIT ⇒ 返回 0",
            type);
    call.result = 0;
}

void shimVirtualProtect(X86& cpu, ShimCall& call) {
    requireMem(cpu, "VirtualProtect");
    const std::uint32_t new_protect = call.u32(2);
    const std::uint32_t old_ptr = call.u32(3);
    if (old_ptr != 0) {
        cpu.mem->wr32(old_ptr, kPageReadWrite);  // 偏差 5：所有已提交页都是 RW
    }
    if (new_protect != kPageReadWrite) {
        // 偏差 5：不真的改保护 —— 但请求了别的保护时必须留一行（⛔ 不许静默）
        shimLog("kernel32", "VirtualProtect",
                "请求 flNewProtect=0x%08X：本模拟器所有已提交页都是 PAGE_READWRITE，忽略",
                new_protect);
    }
    call.result = 1;
}

// ---- VirtualLock ----
// 文档：把一段虚拟地址锁进物理内存；**区内的每一页都必须已提交**（PAGE_NOACCESS 不能锁）。
// 本模拟器的 guest 内存永远是宿主常驻的（没有换页），所以"锁"没有额外动作 —— 这不是 no-op
// 假成功，而是**契约本来就已满足**。仍然按文档做那一项可判的前提检查：整段必须已提交。
// ★ `ERROR_INVALID_ADDRESS(487)` 的常量已在文件顶部（Virtual* 与 VirtualLock 共用）。
void shimVirtualLock(X86& cpu, ShimCall& call) {
    const char* api = "VirtualLock";
    requireMem(cpu, api);
    const std::uint32_t addr = call.u32(0);
    const std::uint32_t size = call.u32(1);
    if (size == 0) {
        // 文档没说 0 长度是否合法；给 TRUE（没有页需要锁）但留一行，避免静默猜
        shimLog("kernel32", api, "dwSize=0 ⇒ 没有页需要锁，返回 TRUE");
        call.result = 1;
        return;
    }
    if (!cpu.mem->committed(addr, size)) {
        cpu.shim.last_error = kErrInvalidAddress;
        shimLog("kernel32", api, "0x%08X 起 %u 字节不是全部已提交 ⇒ 返回 0（ERROR_INVALID_ADDRESS）",
                addr, size);
        call.result = 0;
        return;
    }
    call.result = 1;
}

}  // namespace

void registerMemShims(ShimRegistry& shims) {
    // ---- Heap*（单一 guest 堆；见文件头偏差 1）----
    shims.add("kernel32", "GetProcessHeap", Conv::Stdcall, 0, shimGetProcessHeap);
    shims.add("kernel32", "HeapCreate", Conv::Stdcall, 3, shimHeapCreate);
    shims.add("kernel32", "HeapDestroy", Conv::Stdcall, 1, shimHeapDestroy);
    shims.add("kernel32", "HeapAlloc", Conv::Stdcall, 3, shimHeapAlloc);
    shims.add("kernel32", "HeapFree", Conv::Stdcall, 3, shimHeapFree);
    shims.add("kernel32", "HeapReAlloc", Conv::Stdcall, 4, shimHeapReAlloc);
    shims.add("kernel32", "HeapSize", Conv::Stdcall, 3, shimHeapSize);

    // ---- Local* ----
    shims.add("kernel32", "LocalAlloc", Conv::Stdcall, 2, shimLocalAlloc);
    shims.add("kernel32", "LocalFree", Conv::Stdcall, 1, shimLocalFree);
    shims.add("kernel32", "LocalReAlloc", Conv::Stdcall, 3, shimLocalReAlloc);
    shims.add("kernel32", "LocalSize", Conv::Stdcall, 1, shimLocalSize);

    // ---- Global*（GMEM_MOVEABLE 不做；见偏差 2）----
    shims.add("kernel32", "GlobalAlloc", Conv::Stdcall, 2, shimGlobalAlloc);
    shims.add("kernel32", "GlobalFree", Conv::Stdcall, 1, shimGlobalFree);
    shims.add("kernel32", "GlobalReAlloc", Conv::Stdcall, 3, shimGlobalReAlloc);
    shims.add("kernel32", "GlobalSize", Conv::Stdcall, 1, shimGlobalSize);
    shims.add("kernel32", "GlobalLock", Conv::Stdcall, 1, shimGlobalLock);
    shims.add("kernel32", "GlobalUnlock", Conv::Stdcall, 1, shimGlobalUnlock);
    shims.add("kernel32", "GlobalFlags", Conv::Stdcall, 1, shimGlobalFlags);

    // ---- CoTaskMem*（同一个堆）----
    shims.add("kernel32", "CoTaskMemAlloc", Conv::Stdcall, 1, shimCoTaskMemAlloc);
    shims.add("kernel32", "CoTaskMemFree", Conv::Stdcall, 1, shimCoTaskMemFree);
    shims.add("kernel32", "CoTaskMemRealloc", Conv::Stdcall, 2, shimCoTaskMemRealloc);

    // ---- Virtual*（bump 区 / 只支持 LIFO 释放 / 保护不生效；见偏差 3–5）----
    shims.add("kernel32", "VirtualAlloc", Conv::Stdcall, 4, shimVirtualAlloc);
    shims.add("kernel32", "VirtualFree", Conv::Stdcall, 3, shimVirtualFree);
    shims.add("kernel32", "VirtualProtect", Conv::Stdcall, 4, shimVirtualProtect);
    shims.add("kernel32", "VirtualLock", Conv::Stdcall, 2, shimVirtualLock);
}

}  // namespace lift
