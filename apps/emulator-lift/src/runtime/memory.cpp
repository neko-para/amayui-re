// emulator-lift · guest 地址空间实现（4 GiB 预留 + 按需提交 + 平台 fault handler）
//
// ★ 平台差异只在这一层：Windows 用 MEM_RESERVE / MEM_COMMIT + Vectored Exception Handler，
//   POSIX 用 mmap(PROT_NONE) / mprotect + sigaction(SIGSEGV)。
// ★ 未提交页被碰 ⇒ **平台 fault** → 打印诊断 → `_Exit(3)`。
//   ⛔ 绝不"返回 0 当作读到 0"：那会让越界读变成静默数据损坏（骨架里的 16 MiB 缓冲就是这样）。
#include "runtime/memory.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
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
    // 用 _Exit 而不是 abort：不弹调试器对话框、不留半写的宿主缓冲（诊断已经 flush）
    std::_Exit(3);
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
    // 只有"落在 guest 空间里"的才当 guest fault 处理；否则交回系统（那是宿主自己的 bug）
    bool in_guest = false;
    if (g_memory && g_memory->reserved()) {
        const auto base = reinterpret_cast<std::uintptr_t>(g_memory->host(0));
        in_guest = fault_addr >= base && fault_addr < base + GuestMemory::kSpace;
    }
    if (!in_guest && g_access.size == 0) return EXCEPTION_CONTINUE_SEARCH;
    char why[160];
    std::snprintf(why, sizeof(why), "平台异常 0x%08lX @ host 0x%llX（%s 未提交页）",
                  static_cast<unsigned long>(code), static_cast<unsigned long long>(fault_addr),
                  in_guest ? "guest 空间内" : "guest 空间外");
    guestFault(g_access, why);
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
    char why[160];
    std::snprintf(why, sizeof(why), "SIGSEGV @ host %p", info ? info->si_addr : nullptr);
    guestFault(g_access, why);
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
    auto* p = static_cast<std::uint8_t*>(base) + off;
    return ::mprotect(p, size, PROT_READ | PROT_WRITE) == 0;
}
bool platformDecommit(void* base, std::size_t off, std::size_t size) {
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
    return (commit_bits_[page >> 6] >> (page & 63u)) & 1u;
}

void GuestMemory::setBit(std::uint32_t page, bool on) {
    if (page >= commit_bits_.size() * 64u) return;
    if (on) {
        commit_bits_[page >> 6] |= (1ull << (page & 63u));
    } else {
        commit_bits_[page >> 6] &= ~(1ull << (page & 63u));
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

bool GuestMemory::commitPages(std::uint32_t begin, std::uint32_t end) {
    if (begin >= end) return true;
    if (end > kSpace) return false;
    // 找连续未提交的段，一次系统调用提交一段
    std::uint32_t p = begin / kPage;
    const std::uint32_t last = (end + kPage - 1) / kPage;
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
    const std::uint32_t end = static_cast<std::uint32_t>((end64 + kPageMask) & ~static_cast<std::uint64_t>(kPageMask));
    if (!commitPages(begin, end)) return false;
    if (kind != RegionKind::Other || (name && name[0])) annotate(begin, end - begin, kind, name);
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
    const std::uint32_t end = static_cast<std::uint32_t>(end64);
    if (begin < end) platformDecommit(base_, begin, end - begin);
    for (std::uint32_t p = begin / kPage; p < end / kPage; ++p) setBit(p, false);
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
