// emulator-lift · guest 地址空间实现（4 GiB 预留 + 按需提交 + 平台 fault handler）
//
// ★ 平台差异只在这一层：Windows 用 MEM_RESERVE / MEM_COMMIT + Vectored Exception Handler，
//   POSIX 用 mmap(PROT_NONE) / mprotect + sigaction(SIGSEGV)。
// ★ 未提交页被碰 ⇒ **平台 fault** → 打印诊断 → `_Exit(3)`。
//   ⛔ 绝不"返回 0 当作读到 0"：那会让越界读变成静默数据损坏（骨架里的 16 MiB 缓冲就是这样）。
// ★ 宿主自己的 AV（异常地址**不在** guest 空间）不是 guest fault：走 hostFault 的独立诊断 + `_Exit(4)`
//   （归因判据 inGuestSpace；见下方"归因"一节）。
#include "runtime/memory.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "runtime/host_encoding.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace lift {

thread_local AccessSite g_access{};

namespace {
FaultHook g_hook = nullptr;
GuestMemory* g_memory = nullptr;  // 当前活动实例（guest 是单线程的，见 README §2.5）

std::uint64_t g_fault_count = 0;
}  // namespace

// ★ 进程级关掉"崩了就弹窗"（见 memory.h 的注释）。幂等；POSIX 上是空操作。
//   ★ 关键：`_set_abort_behavior(0, …)` 关的是**整个进程**的 abort 行为 ⇒ 只调一次就覆盖
//   `heap.cpp` / `handles.cpp` / `shims/register.h` 那几处直接 `std::abort()`。
//   ⛔ 不要为了"安静"把 abort 改成 exit(0)：日志照旧打 stderr、退出码照旧非 0，那才是判据。
void suppressCrashDialogsOnce() {
#if defined(_WIN32)
    static const bool once = [] {
        ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        return true;
    }();
    (void)once;
#endif
}

// ★ 宿主页大小（guest 页恒为 4 KiB，见 memory.h 的 kPage）。**以系统为准，⛔ 不硬编码**：
//   · Windows：`GetSystemInfo().dwPageSize`（提交 / 退回的对齐粒度就是它）；
//   · Apple Silicon macOS：宿主页是 **16 KiB** ⇒ `mprotect` 只接受宿主页对齐的起始地址（否则 EINVAL），
//     所以提交/退回都要按宿主页对齐（见 platformCommit / GuestMemory::decommit）。
//   ★ 为什么不在 Windows 写 0x1000：那是"这台机器恰好如此"，不是平台契约 —— 换成 16 KiB 宿主页的
//     机器（或 ARM64 的 64 KiB 配置）就会把对齐算错，而那正是 REQ-01M6JBQ05P1QF56DQX5B9YJ4V8 踩过的坑。
std::uint32_t hostPageSize() {
#if defined(_WIN32)
    static const std::uint32_t size = [] {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        return si.dwPageSize != 0 ? static_cast<std::uint32_t>(si.dwPageSize) : 0x1000u;
    }();
    return size;
#else
    static const std::uint32_t size = [] {
        const long n = ::sysconf(_SC_PAGESIZE);
        return n > 0 ? static_cast<std::uint32_t>(n) : 0x1000u;
    }();
    return size;
#endif
}

void setGuestFaultHook(FaultHook hook) { g_hook = hook; }

const char* regionKindName(RegionKind kind) {
    switch (kind) {
        case RegionKind::Image: return "image";
        case RegionKind::Stack: return "stack";
        case RegionKind::StackGuard: return "stack-guard";
        case RegionKind::Heap: return "heap";
        case RegionKind::Teb: return "teb";
        case RegionKind::Peb: return "peb";
        case RegionKind::ShimPage: return "shim-page";
        case RegionKind::Other: return "other";
    }
    return "?";
}

void dumpRegions(const std::vector<Region>& regions, std::FILE* out) {
    std::fprintf(out, "%-10s %08X-%08X %8u  %s\n", "kind", 0, 0, 0, "name");
    for (const Region& r : regions) {
        std::fprintf(out, "%-10s %08X-%08X %8u  %s\n", regionKindName(r.kind), r.begin, r.end,
                     static_cast<unsigned>(r.end - r.begin), r.name);
    }
}

// ---------------------------------------------------------------------------------------
// fault：打印诊断 → 退出。**不是**可恢复的异常（M1 不做 SEH 展开，见 plan §9 R1）。
// ---------------------------------------------------------------------------------------
[[noreturn]] void guestFault(const AccessSite& site, const char* why) {
    ++g_fault_count;
    std::fprintf(stderr, "\n=== guest memory fault ===\n");
    std::fprintf(stderr, "  why        : %s\n", why ? why : "(unspecified)");
    std::fprintf(stderr, "  access     : %s 0x%08X  size=%u\n", site.write ? "WRITE" : "READ", site.ea,
                 static_cast<unsigned>(site.size));
    if (g_memory) {
        const Region* r = g_memory->regionOf(site.ea);
        std::fprintf(stderr, "  region(ea) : %s\n",
                     r ? regionKindName(r->kind) : "(未登记：这段不属于任何已声明的区域)");
    }
    if (g_hook) g_hook(site);
    std::fflush(nullptr);
    restoreConsoleUtf8();  // _Exit 不跑析构；控制台码页必须显式恢复
    // 用 _Exit 而不是 abort：不弹调试器对话框、不留半写的宿主缓冲（诊断已经 flush）
    std::_Exit(3);
}

// ---------------------------------------------------------------------------------------
// 归因：异常地址在不在 guest 空间（纯函数；两个平台的 handler 都只走这一条判据）
// ---------------------------------------------------------------------------------------
bool inGuestSpace(const void* guest_base, std::uintptr_t fault_addr) {
    if (!guest_base) return false;  // 没有预留的 guest 空间 ⇒ 没有可归因给 guest 的地址
    const std::uint64_t base = reinterpret_cast<std::uintptr_t>(guest_base);
    const std::uint64_t addr = fault_addr;
    // 用减法判区间：base + kSpace 在 32 位宿主上会溢出，减法不会。
    return addr >= base && (addr - base) < GuestMemory::kSpace;
}

// ---------------------------------------------------------------------------------------
// 宿主 fault：**不是** guest 访存引起的（异常地址不在 guest 空间）⇒ 换一份诊断 + 不同的退出码。
// ★ 判据见 inGuestSpace；这里刻意**不打** `access     :` 行（那是 guest fault 的行，打了就会把
//   排查方向带回上一条无关的 guest 指令）；最近一次 guest 访存只作附注，并显式标注可能无关。
// ---------------------------------------------------------------------------------------
[[noreturn]] void hostFault(std::uintptr_t fault_addr, const char* why) {
    std::fprintf(stderr, "\n=== host fault（宿主自身的异常，不是 guest 访存）===\n");
    std::fprintf(stderr, "  why        : %s\n", why ? why : "(unspecified)");
    std::fprintf(stderr, "  host addr  : 0x%llX\n", static_cast<unsigned long long>(fault_addr));
    if (g_memory && g_memory->reserved()) {
        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(g_memory->host(0));
        std::fprintf(stderr, "  guest range: 0x%llX-0x%llX（本次异常地址**不在**其中）\n",
                     static_cast<unsigned long long>(base),
                     static_cast<unsigned long long>(base + GuestMemory::kSpace));
    } else {
        std::fprintf(stderr, "  guest range: (guest 空间未预留)\n");
    }
    if (g_access.size != 0) {
        std::fprintf(stderr,
                     "  last guest : %s 0x%08X  size=%u  ← **最近一次** guest 访存，与本次 fault 可能无关\n",
                     g_access.write ? "WRITE" : "READ", g_access.ea, static_cast<unsigned>(g_access.size));
    }
    std::fflush(nullptr);
    restoreConsoleUtf8();
    // ★ 4 而不是 3：`3` 是 guest fault 的判据（有测试与人工排查都靠它区分）；也不 return 给平台
    //   （POSIX 上返回会回到同一条 fault 指令 ⇒ 无限重入）。
    std::_Exit(4);
}

// ---------------------------------------------------------------------------------------
// 平台 handler
// ---------------------------------------------------------------------------------------
#if defined(_WIN32)

namespace {
LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const std::uintptr_t fault_addr =
        (info->ExceptionRecord->NumberParameters >= 2) ? static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                                                      : 0;
    // ★ 归因看**异常地址**（每平台同一条判据 inGuestSpace），不看 `g_access`（宿主 AV 时那是陈旧值）。
    const void* guest_base = (g_memory && g_memory->reserved()) ? static_cast<const void*>(g_memory->host(0)) : nullptr;
    // ★ 调试基建（**默认关**）：`LIFT_VEH_PASSTHROUGH=1` ⇒ 非 guest 地址的异常**交回下一位 handler**，
    //   而不是在这里判成宿主 bug。存在的理由：ASAN 的 shadow 是 MEM_RESERVE 后**惰性提交**的，它首次
    //   碰某页会合法地 AV（由 ASAN 自己的 VEH 提交）；本 handler 是 `AddVectoredExceptionHandler(1, …)`
    //   注册的 ⇒ 先于 ASAN 执行 ⇒ 没有这个开关时 ASAN 版二进制起不来（实测：那些 fault 的地址全落在
    //   ASAN 自己打印的 LowShadow / HighShadow 区间内，却被判成 host fault 而 `_Exit(4)`）。调试器要
    //   第一机会异常时同理。
    //   ⛔ 默认路径**不变**（无该环境变量 ⇒ 下面照旧 hostFault → `_Exit(4)`）；`tests/fault_report.cpp`
    //     的 `host-av-attributed-to-host`（期望退出码 4）就是这条语义的守卫。⛔ 不要把它改成无条件
    //     passthrough：那会把"宿主自己的 AV 给出一份可读诊断 + 退出码 4"的契约删掉（3 vs 4 有守卫依赖）。
    static const bool passthrough = std::getenv("LIFT_VEH_PASSTHROUGH") != nullptr;
    if (passthrough && !inGuestSpace(guest_base, fault_addr)) {
        // 让开时留一行痕迹：否则"ASAN 里为什么多/少一次 AV"无从判断（这一行只在开关打开时出现）。
        // ★ Rip 也打出来：栈/符号化拿不到时，至少能看出"这一次 AV 是**谁**在访谁"。
        std::fprintf(stderr, "[veh] passthrough: 0x%08lX %s @ host 0x%llX  rip=0x%llX（交给下一位 handler）\n",
                     static_cast<unsigned long>(code),
                     (info->ExceptionRecord->NumberParameters >= 1 &&
                      info->ExceptionRecord->ExceptionInformation[0] == 1)
                         ? "WRITE"
                         : "READ",
                     static_cast<unsigned long long>(fault_addr),
                     info->ContextRecord ? static_cast<unsigned long long>(info->ContextRecord->Rip) : 0ull);
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char why[200];
    if (inGuestSpace(guest_base, fault_addr)) {
        std::snprintf(why, sizeof(why), "平台异常 0x%08lX @ host 0x%llX（guest 空间内未提交页）",
                      static_cast<unsigned long>(code), static_cast<unsigned long long>(fault_addr));
        guestFault(g_access, why);
    }
    // 不在 guest 空间 ⇒ 宿主自己的 bug：走 host 诊断并 `_Exit(4)`。
    // ⛔ 不 return EXCEPTION_CONTINUE_SEARCH：那等于把 AV 再抛回系统（与本 handler 的语义不一致），
    //   也⛔ 不套用 `g_access`（会把排查方向带到上一条无关的 guest 指令）。
    std::snprintf(why, sizeof(why), "平台异常 0x%08lX @ host 0x%llX（不在 guest 空间）",
                  static_cast<unsigned long>(code), static_cast<unsigned long long>(fault_addr));
    hostFault(fault_addr, why);
}
}  // namespace

void installPlatformFaultHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    ::AddVectoredExceptionHandler(1, vectoredHandler);
}

void* platformReserve(std::size_t size) {
    return ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}
bool platformCommit(void* base, std::size_t off, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(base) + off;
    return ::VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
bool platformDecommit(void* base, std::size_t off, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(base) + off;
    return ::VirtualFree(p, size, MEM_DECOMMIT) != 0;
}
void platformRelease(void* base) { ::VirtualFree(base, 0, MEM_RELEASE); }

#else  // POSIX

namespace {
void segvHandler(int sig, siginfo_t* info, void*) {
    (void)sig;
    const std::uintptr_t fault_addr = info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
    // ★ 归因看**异常地址**（与 Windows 同一条判据 inGuestSpace），不看 `g_access`（宿主 AV 时那是陈旧值）。
    const void* guest_base = (g_memory && g_memory->reserved()) ? static_cast<const void*>(g_memory->host(0)) : nullptr;
    char why[200];
    if (inGuestSpace(guest_base, fault_addr)) {
        std::snprintf(why, sizeof(why), "SIGSEGV @ host %p（guest 空间内未提交页）",
                      reinterpret_cast<const void*>(fault_addr));
        guestFault(g_access, why);
    }
    // 不在 guest 空间 ⇒ 宿主自己的 bug：走 host 诊断并 `_Exit(4)`。
    // ⛔ 不 return（返回会回到同一条 fault 指令 ⇒ 无限重入）。
    std::snprintf(why, sizeof(why), "SIGSEGV @ host %p（不在 guest 空间）",
                  reinterpret_cast<const void*>(fault_addr));
    hostFault(fault_addr, why);
}
}  // namespace

void installPlatformFaultHandler() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction sa {};
    sa.sa_sigaction = segvHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void* platformReserve(std::size_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    return ::mmap(nullptr, size, PROT_NONE, flags, -1, 0);
}
bool platformCommit(void* base, std::size_t off, std::size_t size) {
    // ★ 范围扩到**宿主页**：16 KiB 宿主上，4 KiB 的 guest 页只能整页提交 ⇒ 同宿主页里的邻页会一起变得可读写
    //   （提交位图仍是 4 KiB 粒度，safe 模式照旧逐页判定；非 safe 模式的保护粒度 = 宿主页）。
    const std::size_t hp = hostPageSize();
    const std::size_t begin = off / hp * hp;
    const std::size_t end = (off + size + hp - 1) / hp * hp;
    auto* p = static_cast<std::uint8_t*>(base) + begin;
    return ::mprotect(p, end - begin, PROT_READ | PROT_WRITE) == 0;
}
bool platformDecommit(void* base, std::size_t off, std::size_t size) {
    // ★ off / size 由调用方保证已按宿主页对齐（GuestMemory::decommit 只退"被请求范围完整覆盖"的宿主页）：
    //   否则 PROT_NONE 会把同宿主页里的**已提交邻页**一起打掉，那些页的位图仍为已提交 ⇒ 后续访问直接 SIGSEGV。
    auto* p = static_cast<std::uint8_t*>(base) + off;
    ::madvise(p, size, MADV_DONTNEED);
    return ::mprotect(p, size, PROT_NONE) == 0;
}
void platformRelease(void* base) { ::munmap(base, static_cast<std::size_t>(GuestMemory::kSpace)); }

#endif

// ---------------------------------------------------------------------------------------
// GuestMemory
// ---------------------------------------------------------------------------------------
GuestMemory::GuestMemory() { reserve(); }

GuestMemory::~GuestMemory() {
    if (base_) {
        if (g_memory == this) g_memory = nullptr;
        platformRelease(base_);
        base_ = nullptr;
    }
}

bool GuestMemory::reserve() {
    if (base_) return true;
    installPlatformFaultHandler();
    void* p = platformReserve(static_cast<std::size_t>(kSpace));
#if defined(_WIN32)
    if (!p) {
        std::snprintf(last_error_, sizeof(last_error_), "VirtualAlloc(MEM_RESERVE 4GiB) 失败，GetLastError=%lu",
                      static_cast<unsigned long>(::GetLastError()));
        return false;
    }
#else
    if (p == MAP_FAILED) {
        std::snprintf(last_error_, sizeof(last_error_), "mmap(PROT_NONE 4GiB) 失败：%s", std::strerror(errno));
        return false;
    }
#endif
    base_ = static_cast<std::uint8_t*>(p);
    commit_bits_.assign(static_cast<std::size_t>(kSpace / kPage / 64), 0);
    g_memory = this;
    return true;
}

bool GuestMemory::bit(std::uint32_t page) const {
    if (page >= commit_bits_.size() * 64u) return false;
    // ★ 并发：feeder 的 Decoder 在读提交位（decode.cpp 的页边界检查），主线程可能同时在
    //   commit/decommit（VirtualAlloc）。`commit_bits_` 在 reserve 时就定长（不会扩容）⇒
    //   用 atomic_ref 做位读写即可，不需要锁（safe mode 的每访问检查也不加锁）。
    const std::uint64_t word =
        std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(commit_bits_[page >> 6]))
            .load(std::memory_order_relaxed);
    return (word >> (page & 63u)) & 1u;
}

void GuestMemory::setBit(std::uint32_t page, bool on) {
    if (page >= commit_bits_.size() * 64u) return;
    auto ref = std::atomic_ref<std::uint64_t>(commit_bits_[page >> 6]);
    if (on) {
        ref.fetch_or(1ull << (page & 63u), std::memory_order_relaxed);
    } else {
        ref.fetch_and(~(1ull << (page & 63u)), std::memory_order_relaxed);
    }
}

bool GuestMemory::committed(std::uint32_t ea, std::size_t size) const {
    if (!base_) return false;
    if (size == 0) return true;
    const std::uint64_t first = ea / kPage;
    const std::uint64_t last = (static_cast<std::uint64_t>(ea) + size - 1ull) / kPage;
    for (std::uint64_t p = first; p <= last; ++p)
        if (!bit(static_cast<std::uint32_t>(p))) return false;
    return true;
}

std::uint64_t GuestMemory::committedBytes() const {
    std::uint64_t pages = 0;
    for (std::uint64_t w : commit_bits_) {
        std::uint64_t v = w;
        while (v) {
            pages += (v & 1ull);
            v >>= 1;
        }
    }
    return pages * kPage;
}

bool GuestMemory::commitPages(std::uint32_t begin, std::uint64_t end) {
    if (begin >= end) return true;
    // ★ `end` 可以等于 4 GiB（提交最后一页时）⇒ 用 64 位接；页号最大 0x100000，uint32 够。
    // if (end > kSpace) return false;
    // 找连续未提交的段，一次系统调用提交一段
    std::uint32_t p = begin / kPage;
    const std::uint32_t last = static_cast<std::uint32_t>(end / kPage);
    while (p < last) {
        if (bit(p)) {
            ++p;
            continue;
        }
        std::uint32_t q = p;
        while (q < last && !bit(q)) ++q;
        const std::size_t off = static_cast<std::size_t>(p) * kPage;
        const std::size_t len = static_cast<std::size_t>(q - p) * kPage;
        if (!platformCommit(base_, off, len)) {
            std::snprintf(last_error_, sizeof(last_error_), "commit(%08X..%08X) 失败", p * kPage, q * kPage);
            return false;
        }
        for (std::uint32_t i = p; i < q; ++i) setBit(i, true);
        p = q;
    }
    return true;
}

bool GuestMemory::commit(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name) {
    if (!base_) return false;
    if (size == 0) return true;
    const std::uint64_t end64 = static_cast<std::uint64_t>(ea) + size;
    if (end64 > kSpace) {
        std::snprintf(last_error_, sizeof(last_error_), "commit 越过 4 GiB：%08X+%zu", ea, size);
        return false;
    }
    const std::uint32_t begin = ea & ~kPageMask;
    const std::uint64_t end = (end64 + kPageMask) & ~static_cast<std::uint64_t>(kPageMask);
    if (!commitPages(begin, end)) return false;
    if (kind != RegionKind::Other || (name && name[0]))
        annotate(begin, static_cast<std::size_t>(end - begin), kind, name);
    return true;
}

bool GuestMemory::commitZeroed(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name) {
    if (!commit(ea, size, kind, name)) return false;
    std::memset(base_ + ea, 0, size);
    return true;
}

void GuestMemory::decommit(std::uint32_t ea, std::size_t size) {
    if (!base_ || size == 0) return;
    const std::uint32_t begin = ea & ~kPageMask;
    const std::uint64_t end64 = (static_cast<std::uint64_t>(ea) + size + kPageMask) & ~static_cast<std::uint64_t>(kPageMask);
    if (end64 > kSpace) return;
    if (static_cast<std::uint64_t>(begin) >= end64) return;
    // ★ 宿主页可能大于 guest 页（Apple Silicon = 16 KiB）：只有**被 [begin,end) 完整覆盖**的宿主页才能真退。
    //   边缘不足一个宿主页的 guest 页**保持已提交**（宁可多留，也不能 PROT_NONE 打掉同宿主页的邻页）。
    // ★ 对齐算术用 64 位：上界可以等于 4 GiB（最后一页），uint32 在这里会回绕成 0
    //   ⇒ `begin >= end` 直接返回，位图永远清不掉（Windows 4 KiB 页上就是"退提交静默失败"）。
    const std::uint64_t hp = hostPageSize();
    const std::uint64_t hostBegin = (static_cast<std::uint64_t>(begin) + hp - 1) / hp * hp;
    const std::uint64_t hostEnd = end64 / hp * hp;
    if (hostBegin >= hostEnd) return;
    platformDecommit(base_, static_cast<std::size_t>(hostBegin),
                     static_cast<std::size_t>(hostEnd - hostBegin));
    for (std::uint32_t p = static_cast<std::uint32_t>(hostBegin / kPage);
         p < static_cast<std::uint32_t>(hostEnd / kPage); ++p) {
        setBit(p, false);
    }
}

void GuestMemory::annotate(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name) {
    Region r;
    r.begin = ea;
    r.end = static_cast<std::uint32_t>(static_cast<std::uint64_t>(ea) + size);
    r.kind = kind;
    std::snprintf(r.name, sizeof(r.name), "%s", name ? name : "");
    regions_.push_back(r);
}

const Region* GuestMemory::regionOf(std::uint32_t ea) const {
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
        if (ea >= it->begin && ea < it->end) return &*it;
    }
    return nullptr;
}

bool GuestMemory::safeModeFromEnv() {
    const char* v = std::getenv("LIFT_SAFE_MEMORY");
    return v && v[0] && !(v[0] == '0' && v[1] == '\0');
}

void GuestMemory::readBytes(std::uint32_t ea, void* dst, std::size_t n) {
    g_access = {ea, static_cast<std::uint8_t>(n > 255 ? 255 : n), 0};
    if (safe_ && !committed(ea, n)) guestFault(g_access, "readBytes 越过未提交页 (safe mode)");
    std::memcpy(dst, base_ + ea, n);
}

void GuestMemory::writeBytes(std::uint32_t ea, const void* src, std::size_t n) {
    g_access = {ea, static_cast<std::uint8_t>(n > 255 ? 255 : n), 1};
    if (safe_ && !committed(ea, n)) guestFault(g_access, "writeBytes 越过未提交页 (safe mode)");
    std::memcpy(base_ + ea, src, n);
}

void GuestMemory::fillBytes(std::uint32_t ea, std::uint8_t value, std::size_t n) {
    g_access = {ea, static_cast<std::uint8_t>(n > 255 ? 255 : n), 1};
    if (safe_ && !committed(ea, n)) guestFault(g_access, "fillBytes 越过未提交页 (safe mode)");
    std::memset(base_ + ea, value, n);
}

std::string GuestMemory::readCString(std::uint32_t ea, std::size_t max_len) {
    std::string s;
    for (std::size_t i = 0; i < max_len; ++i) {
        const char c = static_cast<char>(rd8(ea + static_cast<std::uint32_t>(i)));
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

}  // namespace lift
