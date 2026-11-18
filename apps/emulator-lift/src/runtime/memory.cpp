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

// ★ fault 文本钩子（每线程；见 memory.h）：生产者只有主线程的 `aot_main`（`--input-plan` 摘要）。
//   fault 在**信号上下文**里发生 ⇒ 这里只有一次裸指针判空 + 一次函数调用，不分配、不加锁。
thread_local FaultTextHook g_text_hook = nullptr;
thread_local void* g_text_ctx = nullptr;

std::uint64_t g_fault_count = 0;

// ---- fault 时的 guest 内存 dump（`LIFT_FAULT_DUMP`；进程级，启动期写一次、之后只读）----
// ★ 为什么不加锁 / 不用 atomic：写发生在起音频线程（`AotHost::init`）之前（`aot_main` 的调用点），
//   thread 创建即 happens-before；fault 路径只读。⚠ 若将来有人把安装挪进 `run()`，这条要重审。
FaultDumpProgram g_dump{};
}  // namespace

// ---------------------------------------------------------------------------------------
// fault 诊断的两块新输出（BUG-32）：`fault ea` 行 + opt-in guest 内存 dump
// ★ 约束：两者都可能在**信号上下文**里被调用 ⇒ 只 fprintf / committed() / memcpy（无分配、无锁）。
// ---------------------------------------------------------------------------------------
namespace {

const char* faultAccessName(FaultAccess access) {
    switch (access) {
        case FaultAccess::Read: return "READ";
        case FaultAccess::Write: return "WRITE";
        case FaultAccess::Execute: return "EXEC";
        case FaultAccess::Unknown: break;
    }
    return "unknown";
}

// `fault ea` 行：平台异常地址 → guest EA（host − base），并把**换算依据**打在同一行（人复核用）。
// ★ 没有 guest 空间 / 地址不在其中 ⇒ 如实打一句，⛔ 不编造 0（0 是合法 EA，编造会把排查带偏）。
void printFaultEa(const PlatformFault* plat, std::FILE* out) {
    const void* const base =
        (g_memory && g_memory->reserved()) ? static_cast<const void*>(g_memory->host(0)) : nullptr;
    if (plat == nullptr || !inGuestSpace(base, plat->host_addr)) {
        std::fprintf(out, "  fault ea   : (平台 fault 地址无法换算成 guest EA)\n");
        return;
    }
    const std::uintptr_t host_base = reinterpret_cast<std::uintptr_t>(base);
    // inGuestSpace 保证差值 < 4 GiB ⇒ 截断成 uint32_t 安全（64 位 guest 空间要重审）。
    const std::uint32_t ea = static_cast<std::uint32_t>(plat->host_addr - host_base);
    const Region* const region = g_memory->regionOf(ea);
    std::fprintf(out, "  fault ea   : 0x%08X  (host 0x%llX - base 0x%llX)  region=%s  dir=%s\n",
                 static_cast<unsigned>(ea), static_cast<unsigned long long>(plat->host_addr),
                 static_cast<unsigned long long>(host_base),
                 region != nullptr ? regionKindName(region->kind) : "(未登记：这段不属于任何已声明的区域)",
                 faultAccessName(plat->access));
}

// hexdump：16 B/行（最多 64 行 = 1024 B 的上限）；字节用大写 hex，EA 用 8 位。
void printDumpBytes(std::FILE* out, std::uint32_t ea, const std::uint8_t* data, std::uint32_t len) {
    for (std::uint32_t off = 0; off < len; ++off) {
        if (off % 16u == 0) {
            if (off != 0) std::fputc('\n', out);
            std::fprintf(out, "%08X: ", static_cast<unsigned>(ea + off));
        } else {
            std::fputc(' ', out);
        }
        std::fprintf(out, "%02X", static_cast<unsigned>(data[off]));
    }
    std::fputc('\n', out);
}

// EA：纯 hex（`0x` 可省）——与 `aot_main.cpp` 的 `parseU32Hex` 同口径（那份在匿名 namespace）。
bool parseHexU32(const char* s, std::uint32_t* out) {
    if (s == nullptr || s[0] == '\0') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 16);
    if (end == s || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

// LEN：十进制；`0x` 前缀 = hex。
bool parseDecU32(const char* s, std::uint32_t* out) {
    if (s == nullptr || s[0] == '\0') return false;
    const int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, base);
    if (end == s || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

}  // namespace

void setFaultDumpProgram(const FaultDumpProgram& p) { g_dump = p; }

bool parseFaultDumpSpec(const char* spec, FaultDumpProgram* out, char* err, std::size_t err_len) {
    if (out == nullptr) return false;
    *out = FaultDumpProgram{};
    if (spec == nullptr || spec[0] == '\0') return true;  // 空 spec = 不 dump（shell 里 `VAR=` 常见）
    const std::string text(spec);
    constexpr std::size_t kMaxSpecChars = 1024;
    if (text.size() > kMaxSpecChars) {
        if (err != nullptr && err_len != 0) {
            std::snprintf(err, err_len, "LIFT_FAULT_DUMP: spec 超过 %zu 字符（收到 %zu）", kMaxSpecChars,
                          text.size());
        }
        return false;
    }
    std::uint32_t total = 0;
    std::size_t begin = 0;
    std::uint32_t index = 0;
    while (true) {
        const std::size_t comma = text.find(',', begin);
        const std::string seg =
            text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        ++index;
        if (index > FaultDumpProgram::kMaxRanges) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：段数超过 %u",
                              static_cast<unsigned>(index),
                              static_cast<unsigned>(FaultDumpProgram::kMaxRanges));
            }
            return false;
        }
        const std::size_t colon = seg.find(':');
        if (colon == std::string::npos) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len,
                              "LIFT_FAULT_DUMP: 第 %u 段缺少 ':'（形如 0x1005AB58:64 / 0x1005AB58:*64）",
                              static_cast<unsigned>(index));
            }
            return false;
        }
        const std::string ea_text = seg.substr(0, colon);
        // 冒号之后 = `<LEN>` / `*<LEN>` / `*<delta>:<LEN>` / `4*<LEN>`
        std::string tail = seg.substr(colon + 1);
        bool follow = false;
        std::int32_t delta = 0;
        if (!tail.empty() && tail[0] == '*') {
            follow = true;
            tail.erase(0, 1);
            // `*<delta>:<LEN>`：delta 是**十进制带符号**整数；没有第二个冒号 ⇒ 老语法 `*LEN`（delta=0）
            const std::size_t sep = tail.find(':');
            if (sep != std::string::npos) {
                const std::string delta_text = tail.substr(0, sep);
                char* dend = nullptr;
                const long long d = std::strtoll(delta_text.c_str(), &dend, 10);
                if (delta_text.empty() || dend == delta_text.c_str() || *dend != '\0') {
                    if (err != nullptr && err_len != 0) {
                        std::snprintf(err, err_len,
                                      "LIFT_FAULT_DUMP: 第 %u 段：delta 不是合法十进制整数（`%s`）",
                                      static_cast<unsigned>(index), seg.c_str());
                    }
                    return false;
                }
                if (d < -FaultDumpProgram::kMaxFollowDelta || d > FaultDumpProgram::kMaxFollowDelta) {
                    if (err != nullptr && err_len != 0) {
                        std::snprintf(err, err_len,
                                      "LIFT_FAULT_DUMP: 第 %u 段：delta 必须在 -%d..+%d（收到 `%s`）",
                                      static_cast<unsigned>(index),
                                      static_cast<int>(FaultDumpProgram::kMaxFollowDelta),
                                      static_cast<int>(FaultDumpProgram::kMaxFollowDelta), seg.c_str());
                    }
                    return false;
                }
                delta = static_cast<std::int32_t>(d);
                tail = tail.substr(sep + 1);
            }
        } else {
            const std::size_t star = tail.find('*');
            if (star != std::string::npos) {
                // `EA:4*LEN` 是 `EA:*LEN` 的别名（只许 4、只许 delta=0；`4*-8:64` 不是合法写法）
                if (tail.compare(0, star, "4") != 0 || tail.find(':', star + 1) != std::string::npos) {
                    if (err != nullptr && err_len != 0) {
                        std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：EA/长度不是合法数字（`%s`）",
                                      static_cast<unsigned>(index), seg.c_str());
                    }
                    return false;
                }
                follow = true;
                tail.erase(0, star + 1);
            }
        }
        std::uint32_t ea = 0;
        std::uint32_t len = 0;
        if (!parseHexU32(ea_text.c_str(), &ea) || !parseDecU32(tail.c_str(), &len)) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：EA/长度不是合法数字（`%s`）",
                              static_cast<unsigned>(index), seg.c_str());
            }
            return false;
        }
        if (len < 1 || len > FaultDumpProgram::kMaxRangeLen) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：长度必须在 1..%u（收到 %u）",
                              static_cast<unsigned>(index),
                              static_cast<unsigned>(FaultDumpProgram::kMaxRangeLen),
                              static_cast<unsigned>(len));
            }
            return false;
        }
        if (static_cast<std::uint64_t>(ea) + len > GuestMemory::kSpace) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：EA+LEN 超过 4 GiB",
                              static_cast<unsigned>(index));
            }
            return false;
        }
        total += len;
        if (total > FaultDumpProgram::kMaxTotalBytes) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_FAULT_DUMP: 第 %u 段：总字节数超过 %u",
                              static_cast<unsigned>(index),
                              static_cast<unsigned>(FaultDumpProgram::kMaxTotalBytes));
            }
            return false;
        }
        out->ranges[out->count].ea = ea;
        out->ranges[out->count].len = len;
        out->ranges[out->count].follow = follow;
        out->ranges[out->count].delta = delta;
        ++out->count;
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return true;
}

bool initFaultDumpFromEnv(char* err, std::size_t err_len) {
    const char* const spec = std::getenv("LIFT_FAULT_DUMP");
    FaultDumpProgram program{};
    if (spec != nullptr && spec[0] != '\0') {
        if (!parseFaultDumpSpec(spec, &program, err, err_len)) return false;
    }
    setFaultDumpProgram(program);
    return true;
}

void printFaultDump(std::FILE* out) {
    if (out == nullptr || g_dump.count == 0) return;
    if (!g_memory || !g_memory->reserved()) {
        std::fprintf(out, "  dump       : (guest 空间未预留，无法 dump)\n");
        return;
    }
    for (std::uint32_t i = 0; i < g_dump.count; ++i) {
        const FaultDumpRange& range = g_dump.ranges[i];
        const std::uint32_t len = range.len;
        // ★ 每段都先 committed(...) 再 memcpy：一段坏线索不能吞掉其余线索（失败只打一行 ✗ 并继续）。
        if (range.follow) {
            // 段描述（fault 上下文：定长缓冲 + snprintf，不分配）：delta=0 时与老语法逐字相同。
            char desc[48];
            if (range.delta != 0) {
                std::snprintf(desc, sizeof(desc), "0x%08X:*%d:%u", static_cast<unsigned>(range.ea),
                              static_cast<int>(range.delta), static_cast<unsigned>(len));
            } else {
                std::snprintf(desc, sizeof(desc), "0x%08X:*%u", static_cast<unsigned>(range.ea),
                              static_cast<unsigned>(len));
            }
            if (!g_memory->committed(range.ea, 4)) {
                std::fprintf(out, "  dump       : [%u] %s  ✗ 指针所在页未提交（不跟随）\n",
                             static_cast<unsigned>(i), desc);
                continue;
            }
            std::uint32_t ptr = 0;
            std::memcpy(&ptr, g_memory->host(range.ea), 4);
            if (ptr == 0) {
                std::fprintf(out, "  dump       : [%u] %s  ✗ 指针为 0（不跟随）\n", static_cast<unsigned>(i),
                             desc);
                continue;
            }
            // ★ `ptr + delta` 按**无符号 32 位**目标判定：下溢 / 越过 4 GiB ⇒ ✗ 并继续
            //   （⛔ 不让一个坏线索把整条 dump 变成二次 fault）。
            const std::int64_t target64 = static_cast<std::int64_t>(ptr) + range.delta;
            if (target64 < 0 || target64 + static_cast<std::int64_t>(len) >
                                    static_cast<std::int64_t>(GuestMemory::kSpace)) {
                std::fprintf(out, "  dump       : [%u] %s  ✗ ptr+delta 越界（不跟随）\n",
                             static_cast<unsigned>(i), desc);
                continue;
            }
            const std::uint32_t target = static_cast<std::uint32_t>(target64);
            if (!g_memory->committed(target, len)) {
                std::fprintf(out, "  dump       : [%u] %s  ✗ 指针 0x%08X 指向未提交页（不跟随）\n",
                             static_cast<unsigned>(i), desc, static_cast<unsigned>(target));
                continue;
            }
            if (range.delta != 0) {
                std::fprintf(out, "  dump       : [%u] %s  → ptr=0x%08X delta=%d\n", static_cast<unsigned>(i),
                             desc, static_cast<unsigned>(ptr), static_cast<int>(range.delta));
            } else {
                std::fprintf(out, "  dump       : [%u] %s  → ptr=0x%08X\n", static_cast<unsigned>(i), desc,
                             static_cast<unsigned>(ptr));
            }
            std::uint8_t buf[FaultDumpProgram::kMaxRangeLen];
            std::memcpy(buf, g_memory->host(target), len);
            printDumpBytes(out, target, buf, len);
        } else {
            if (!g_memory->committed(range.ea, len)) {
                std::fprintf(out, "  dump       : [%u] 0x%08X + %u  ✗ 未提交（整段拒绝读）\n",
                             static_cast<unsigned>(i), static_cast<unsigned>(range.ea),
                             static_cast<unsigned>(len));
                continue;
            }
            std::fprintf(out, "  dump       : [%u] 0x%08X + %u\n", static_cast<unsigned>(i),
                         static_cast<unsigned>(range.ea), static_cast<unsigned>(len));
            std::uint8_t buf[FaultDumpProgram::kMaxRangeLen];
            std::memcpy(buf, g_memory->host(range.ea), len);
            printDumpBytes(out, range.ea, buf, len);
        }
    }
}

// ---------------------------------------------------------------------------------------
// opt-in 的 guest / shim **写观察**（`LIFT_WATCH`；BUG-32 的"谁在 P-8 写了那 16 字节"）
// ★ 与 dump 同一条启动期口径：解析 / 安装全部在起线程之前完成；热路径只有一次 `watchOn()` 判断。
// ★ follow 的观察段是**运行期**追加的（只增不改）⇒ release/acquire 发布：读者看到 count 就一定
//   看到该段的字段（写者在 `count.store` 之前写好字段）。追加只发生在 follow 命中时（罕见），
//   用一把进程级 spinlock 串行化两个线程同时追加。
// ★ 为什么 pending 是 `thread_local`：命中 follow 的那次 store 与"下一次写"必须在**同一线程**上
//   才谈得上 happens-before（guest store 全部在主线程；feeder 的 shim 写各自一份 pending）。
// ---------------------------------------------------------------------------------------
std::atomic<std::uint32_t> g_watch_count{0};

namespace {

FaultWatchRange g_watch_ranges[FaultWatchProgram::kMax];
std::atomic_flag g_watch_append_lock = ATOMIC_FLAG_INIT;
thread_local bool t_watch_pending = false;          // follow 待补读（store 还没发生）
thread_local std::uint32_t t_watch_pending_ea = 0;  // 要补读的那个 EA（follow 段的 EA）
// 当前 shim 名（`ShimRegistry::invoke` 在 `def->fn` 前后设置/恢复；见 memory.h 的契约）。
// 只在 `LIFT_WATCH` 开档时被写 ⇒ 缺省档一次都不碰。
thread_local const char* t_watch_shim_name = nullptr;

void watchLock() {
    while (g_watch_append_lock.test_and_set(std::memory_order_acquire)) {
    }
}
void watchUnlock() { g_watch_append_lock.clear(std::memory_order_release); }

// `[ea, ea+n)` 与 `[r.ea, r.ea+r.len)` 相交吗（用 64 位算 ea+n，⛔ 不让 32 位回绕）
bool watchIntersects(std::uint32_t ea, std::uint32_t n, const FaultWatchRange& r) {
    const std::uint64_t wbeg = ea;
    const std::uint64_t wend = static_cast<std::uint64_t>(ea) + n;
    const std::uint64_t rbeg = r.ea;
    const std::uint64_t rend = static_cast<std::uint64_t>(r.ea) + r.len;
    return wbeg < rend && rbeg < wend;
}

bool watchAnyHit(std::uint32_t ea, std::uint32_t n) {
    const std::uint32_t count = g_watch_count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (g_watch_ranges[i].armed && watchIntersects(ea, n, g_watch_ranges[i])) return true;
    }
    return false;
}

const FaultWatchRange* watchFollowHit(std::uint32_t ea, std::uint32_t n) {
    const std::uint32_t count = g_watch_count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < count; ++i) {
        const FaultWatchRange& r = g_watch_ranges[i];
        if (r.armed && r.follow && watchIntersects(ea, n, r)) return &r;
    }
    return nullptr;
}

// 追加一段（follow 命中时）。返回 false = 观察集已满（`kMax` = 8 段）⇒ 调用方如实打 ✗。
// 已 armed 的同一段 ⇒ 幂等返回 true（重复写同一个槽不该把 8 段预算吃光）。
bool watchArmRange(std::uint32_t ea, std::uint32_t len) {
    watchLock();
    const std::uint32_t count = g_watch_count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (g_watch_ranges[i].ea == ea && g_watch_ranges[i].len == len) {
            watchUnlock();
            return true;
        }
    }
    if (count >= FaultWatchProgram::kMax) {
        watchUnlock();
        return false;
    }
    g_watch_ranges[count] = FaultWatchRange{ea, len, false, true};
    g_watch_count.store(count + 1, std::memory_order_release);  // 字段先写好，再发布
    watchUnlock();
    return true;
}

// follow 的补读：在**本次写的最开头**调用（上一次命中时的 store 现在已经完成）。
// ★ 只读 `committed()` 之后的 `host()`，⛔ 不经过 `rd32`（那会写 `g_access`，把 fault 归因弄脏）。
void watchCompleteFollow() {
    if (!t_watch_pending) return;
    t_watch_pending = false;
    const std::uint32_t slot = t_watch_pending_ea;
    if (!g_memory || !g_memory->reserved() || !g_memory->committed(slot, 4)) {
        std::fprintf(stderr, "watch      : follow 0x%08X → ✗ 槽所在页未提交（不 arm）\n",
                     static_cast<unsigned>(slot));
        return;
    }
    std::uint32_t ptr = 0;
    std::memcpy(&ptr, g_memory->host(slot), 4);
    if (ptr == 0) {
        std::fprintf(stderr, "watch      : follow 0x%08X → ✗ 指针为 0（不 arm）\n",
                     static_cast<unsigned>(slot));
        return;
    }
    // P-8 是块的**头**、P+8 是对象首 4 字节之后 ⇒ 两边都必须落在 guest 空间内（32 位无符号）。
    if (ptr < 8u || static_cast<std::uint64_t>(ptr) + 8u > GuestMemory::kSpace) {
        std::fprintf(stderr, "watch      : follow 0x%08X → ptr=0x%08X  ✗ P-8/P+8 越过 guest 空间（不 arm）\n",
                     static_cast<unsigned>(slot), static_cast<unsigned>(ptr));
        return;
    }
    const std::uint32_t begin = ptr - 8u;
    const std::uint32_t end = ptr + 8u;
    if (!g_memory->committed(begin, 16)) {
        std::fprintf(stderr,
                     "watch      : follow 0x%08X → ptr=0x%08X  ✗ [0x%08X,0x%08X) 未提交（不 arm）\n",
                     static_cast<unsigned>(slot), static_cast<unsigned>(ptr), static_cast<unsigned>(begin),
                     static_cast<unsigned>(end));
        return;
    }
    if (!watchArmRange(begin, 16)) {
        std::fprintf(stderr,
                     "watch      : follow 0x%08X → ptr=0x%08X  ✗ 观察区间已满（%u 段，不 arm）\n",
                     static_cast<unsigned>(slot), static_cast<unsigned>(ptr),
                     static_cast<unsigned>(FaultWatchProgram::kMax));
        return;
    }
    std::fprintf(stderr, "watch      : follow 0x%08X → ptr=0x%08X; arm [0x%08X,0x%08X)\n",
                 static_cast<unsigned>(slot), static_cast<unsigned>(ptr), static_cast<unsigned>(begin),
                 static_cast<unsigned>(end));
}

// 两个观察点的共同实现（`guest` 只决定 `block=` 那一截：guest 打 `lift_cc_<块EA>`，shim 打 `shim`）。
void watchWrite(std::uint32_t block_ea, std::uint32_t ea, std::uint32_t n, bool guest) {
    if (n == 0) return;
    // ① 先补上一次 follow（它的 store 已经完成）——顺序就是判据：**先** arm，**再**判本次写。
    watchCompleteFollow();
    // ② 本次写命中 follow 段 ⇒ 只记 pending（store 还没发生，此刻读到的还是旧值）。
    if (const FaultWatchRange* const f = watchFollowHit(ea, n); f != nullptr) {
        t_watch_pending = true;
        t_watch_pending_ea = f->ea;
        std::fprintf(stderr,
                     "watch      : follow pending 0x%08X（store 前记账；下一次写开头补读新值）\n",
                     static_cast<unsigned>(f->ea));
    }
    // ③ 用（可能刚更新过的）观察集判本次写。
    if (!watchAnyHit(ea, n)) return;
    if (guest) {
        std::fprintf(stderr, "watch      : WRITE 0x%08X n=%u block=lift_cc_%08X\n",
                     static_cast<unsigned>(ea), static_cast<unsigned>(n),
                     static_cast<unsigned>(block_ea));
    } else {
        // 名字由 `ShimRegistry::invoke` 设（嵌套时恢复上一层）；未设 / 空 ⇒ `?`（⛔ 不编造名字）。
        std::fprintf(stderr, "watch      : WRITE 0x%08X n=%u shim=%s\n", static_cast<unsigned>(ea),
                     static_cast<unsigned>(n), t_watch_shim_name != nullptr ? t_watch_shim_name : "?");
    }
}

}  // namespace

void watchGuestWrite(std::uint32_t block_ea, std::uint32_t ea, std::uint32_t n) {
    watchWrite(block_ea, ea, n, true);
}
void watchShimWrite(std::uint32_t ea, std::uint32_t n) { watchWrite(0, ea, n, false); }

void setWatchShimName(const char* name) {
    // 空串与 nullptr 同义（⛔ 不让 `shim=` 后面空着：那在日志里看起来像"名字丢了"）。
    t_watch_shim_name = (name != nullptr && name[0] != '\0') ? name : nullptr;
}
const char* watchShimName() { return t_watch_shim_name; }

void setFaultWatchProgram(const FaultWatchProgram& p) {
    watchLock();
    std::uint32_t n = 0;
    const std::uint32_t limit = p.count < FaultWatchProgram::kMax ? p.count : FaultWatchProgram::kMax;
    for (std::uint32_t i = 0; i < limit; ++i) {
        if (!p.r[i].armed) continue;  // `armed` 是唯一判据（解析出的段恒 true）
        g_watch_ranges[n++] = p.r[i];
    }
    g_watch_count.store(n, std::memory_order_release);
    watchUnlock();
    t_watch_pending = false;  // 换程序 ⇒ 本线程的待补读作废（其它线程各自那份在下次命中时覆盖）
}

bool parseFaultWatchSpec(const char* spec, FaultWatchProgram* out, char* err, std::size_t err_len) {
    if (out == nullptr) return false;
    *out = FaultWatchProgram{};
    if (spec == nullptr || spec[0] == '\0') return true;  // 空 spec = 关闭（shell 里 `VAR=` 常见）
    const std::string text(spec);
    constexpr std::size_t kMaxSpecChars = 1024;
    if (text.size() > kMaxSpecChars) {
        if (err != nullptr && err_len != 0) {
            std::snprintf(err, err_len, "LIFT_WATCH: spec 超过 %zu 字符（收到 %zu）", kMaxSpecChars,
                          text.size());
        }
        return false;
    }
    std::size_t begin = 0;
    std::uint32_t index = 0;
    while (true) {
        const std::size_t comma = text.find(',', begin);
        const std::string seg =
            text.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        ++index;
        if (index > FaultWatchProgram::kMax) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：段数超过 %u", static_cast<unsigned>(index),
                              static_cast<unsigned>(FaultWatchProgram::kMax));
            }
            return false;
        }
        const std::size_t colon = seg.find(':');
        if (colon == std::string::npos) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len,
                              "LIFT_WATCH: 第 %u 段缺少 ':'（形如 0x1005AB58:4 / 0x1005AB58:4>follow）",
                              static_cast<unsigned>(index));
            }
            return false;
        }
        std::string len_text = seg.substr(colon + 1);
        bool follow = false;
        const std::size_t gt = len_text.find('>');
        if (gt != std::string::npos) {
            // `>follow` 只许出现在末尾且拼写完整（⛔ 不静默当成普通段：那会把"跟随一层"静默关掉）。
            if (len_text.compare(gt + 1, std::string::npos, "follow") != 0) {
                if (err != nullptr && err_len != 0) {
                    std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：'>' 之后只许是 follow（`%s`）",
                                  static_cast<unsigned>(index), seg.c_str());
                }
                return false;
            }
            follow = true;
            len_text.erase(gt);
        }
        std::uint32_t ea = 0;
        std::uint32_t len = 0;
        if (!parseHexU32(seg.substr(0, colon).c_str(), &ea) || !parseDecU32(len_text.c_str(), &len)) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：EA/长度不是合法数字（`%s`）",
                              static_cast<unsigned>(index), seg.c_str());
            }
            return false;
        }
        if (len < 1 || len > FaultWatchProgram::kMaxLen) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：长度必须在 1..%u（收到 %u）",
                              static_cast<unsigned>(index),
                              static_cast<unsigned>(FaultWatchProgram::kMaxLen), static_cast<unsigned>(len));
            }
            return false;
        }
        if (follow && len != 4) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：follow 只支持 LEN=4（收到 %u）",
                              static_cast<unsigned>(index), static_cast<unsigned>(len));
            }
            return false;
        }
        if (static_cast<std::uint64_t>(ea) + len > GuestMemory::kSpace) {
            if (err != nullptr && err_len != 0) {
                std::snprintf(err, err_len, "LIFT_WATCH: 第 %u 段：EA+LEN 超过 4 GiB",
                              static_cast<unsigned>(index));
            }
            return false;
        }
        out->r[out->count] = FaultWatchRange{ea, len, follow, true};
        ++out->count;
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return true;
}

bool initFaultWatchFromEnv(char* err, std::size_t err_len) {
    const char* const spec = std::getenv("LIFT_WATCH");
    FaultWatchProgram program{};
    if (spec != nullptr && spec[0] != '\0') {
        if (!parseFaultWatchSpec(spec, &program, err, err_len)) return false;
    }
    setFaultWatchProgram(program);
    return true;
}

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

void setGuestFaultTextHook(void* ctx, FaultTextHook hook) {
    g_text_ctx = ctx;
    g_text_hook = hook;
}

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
[[noreturn]] void guestFault(const AccessSite& site, const char* why, const PlatformFault* plat) {
    ++g_fault_count;
    std::fprintf(stderr, "\n=== guest memory fault ===\n");
    std::fprintf(stderr, "  why        : %s\n", why ? why : "(unspecified)");
    // ★ `access` 行**逐字不变**（safe-mode / shim 路径照旧）；**只有平台 fault** 时追加后缀：
    //   那种场合 `g_access` 是"最近一次 shim 侧访存"，带着它冒充 fault 会把排查方向带偏。
    const char* const access_suffix =
        plat != nullptr
            ? "  ← 最近一次 guest 访存（shim 侧记录）；不是本次 fault（本次见 fault ea）"
            : "";
    std::fprintf(stderr, "  access     : %s 0x%08X  size=%u%s\n", site.write ? "WRITE" : "READ", site.ea,
                 static_cast<unsigned>(site.size), access_suffix);
    if (g_memory) {
        const Region* r = g_memory->regionOf(site.ea);
        std::fprintf(stderr, "  region(ea) : %s\n",
                     r ? regionKindName(r->kind) : "(未登记：这段不属于任何已声明的区域)");
    }
    // ★ 本次 fault 的 guest EA（只有平台 handler 知道异常地址 ⇒ `plat != nullptr` 是结构性条件；
    //   ⛔ 不要改成无条件调用：safe-mode 没有平台地址，打了就是编造一行假的现场）。
    if (plat != nullptr) printFaultEa(plat, stderr);
    // ★ opt-in 内存 dump（缺省 count=0 ⇒ 一条都不多打）。
    if (g_dump.count != 0) printFaultDump(stderr);
    if (g_hook) g_hook(site);
    // ★ 追加文本（`--input-plan` 的摘要；没有登记 ⇒ 一条也不打 ⇒ 缺省档的 fault 文本逐字不变）。
    //   放在现场钩子之后：现场先出（人第一眼要的是寄存器 / 栈），配方进度随后。
    if (g_text_hook != nullptr) g_text_hook(g_text_ctx, stderr);
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
        // ★ 方向只有 Windows 给得出：ExceptionInformation[0] = 0 读 / 1 写 / 8 执行。
        //   `NumberParameters < 2` ⇒ 连 ExceptionInformation[1]（异常地址）都不可信 ⇒ 方向也标 unknown。
        FaultAccess access = FaultAccess::Unknown;
        if (info->ExceptionRecord->NumberParameters >= 2) {
            switch (info->ExceptionRecord->ExceptionInformation[0]) {
                case 0: access = FaultAccess::Read; break;
                case 1: access = FaultAccess::Write; break;
                case 8: access = FaultAccess::Execute; break;
                default: break;  // 其它/未知 ⇒ unknown（⛔ 不折叠成 READ）
            }
        }
        const PlatformFault plat{fault_addr, access};
        guestFault(g_access, why, &plat);
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
        // POSIX 的信号上下文没有访存方向（`si_code` 只区分 MAPERR / ACCERR，不区分读/写/执行）
        // ⇒ 恒 unknown（⛔ 不猜成 READ）。
        const PlatformFault plat{fault_addr, FaultAccess::Unknown};
        guestFault(g_access, why, &plat);
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
        ++platform_commit_calls_;              // 临时诊断
        platform_commit_bytes_ += len;         // 临时诊断
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
    ++commit_api_calls_;       // 临时诊断
    commit_api_bytes_ += size; // 临时诊断
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
    // ★ opt-in 写观察（`LIFT_WATCH`；缺省 = 一次 load + 分支）：必须在 memcpy **之前** ——
    //   follow 的 pending 要靠"下一次写在 store 之后开头补读"才看得到新值（见 memory.h 的契约）。
    if (watchOn()) watchShimWrite(ea, static_cast<std::uint32_t>(n > 0xFFFFFFFFull ? 0xFFFFFFFFu : n));
    std::memcpy(base_ + ea, src, n);
}

void GuestMemory::fillBytes(std::uint32_t ea, std::uint8_t value, std::size_t n) {
    g_access = {ea, static_cast<std::uint8_t>(n > 255 ? 255 : n), 1};
    if (safe_ && !committed(ea, n)) guestFault(g_access, "fillBytes 越过未提交页 (safe mode)");
    if (watchOn()) watchShimWrite(ea, static_cast<std::uint32_t>(n > 0xFFFFFFFFull ? 0xFFFFFFFFu : n));
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
