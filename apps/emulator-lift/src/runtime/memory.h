// emulator-lift · guest 地址空间（4 GiB 预留 + 按需提交 + 区域表 + fault 诊断）
//
// ★ 为什么不是 `std::vector<uint8_t>`（骨架里的 16 MiB）：那会让**越界读静默返回 0**
//   —— 而"未提交即 fault"是这一层**唯一的机械护栏**（README §2.6）。
// ★ 模型：一次性预留 4 GiB（`MEM_RESERVE` / `mmap(PROT_NONE)`），之后按页提交。
//   guest 地址 `a` 的宿主字节 = `base_ + a`（平坦映射；64 位宿主上 4 GiB 虚拟预留很便宜）。
//   ⛔ 不做稀疏页表（README §2.6 明确否决）。
// ★ 两条护栏，**默认走第一条**：
//   ① 平台 fault handler（Windows VEH / POSIX SIGSEGV）—— 未提交页真被碰 ⇒ 抓下来打印诊断；
//   ② `LIFT_SAFE_MEMORY=1` ⇒ 每次访存先查提交位图（慢，但把"抓到"变成"确定抓到"）。
//   ②存在的理由：判据 A1 要求"再跑一遍 safe memory 全绿"，那是对①的独立核对。
// ⚠ 访存的 EA/宽度/读写方向记在 `g_access` 里（每次访存写三个字），fault 诊断靠它定位。
//   这是**故意**的性能让步：先要"红得有意义"，S8 的 bench 再谈快。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lift {

enum class RegionKind : std::uint8_t {
    Image = 0, Stack, StackGuard, Heap, Teb, Peb, ShimPage, Other,
};

struct Region {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;  // [begin, end)
    RegionKind kind = RegionKind::Other;
    char name[48] = {};
};

// 最近一次 guest 访存的意图
struct AccessSite {
    std::uint32_t ea = 0;
    std::uint8_t size = 0;
    std::uint8_t write = 0;
};

extern thread_local AccessSite g_access;

// fault 诊断钩子：由 Machine 安装，负责打印 EA + `.lst` 文本 + 寄存器 + guest 返回栈。
// ★ 它会在**平台异常 / 信号上下文**里被调用 ⇒ 不许抛异常，**不许分配**（`std::string` / `new` /
//   任何会增长缓冲的容器都不行）：只许 fprintf / 写**调用方给的**定长缓冲 / 读已加载的只读表。
//   ⚠ 2026-11-04 的实测教训：`LstIndex::textAt` 原来返回 `std::string`（每次调用都分配）⇒
//   钩子一进异常上下文就分配。fault 侧改用 `LstIndex::textPtr`（返回内部指针，零分配）。
using FaultHook = void (*)(const AccessSite& site);
void setGuestFaultHook(FaultHook hook);
[[noreturn]] void guestFault(const AccessSite& site, const char* why);

// ---- fault 归因（两个平台的 handler 共用；纯函数，便于单测）----
// ★ 判据是**异常地址**：落在 guest 空间 `[guest_base, guest_base + GuestMemory::kSpace)` 内 ⇒ guest fault
//   （EA / 宽度 / 方向 + `_Exit(3)`，行为不变）；不在 ⇒ 宿主自己的 bug ⇒ `hostFault`。
// ⛔ 不要拿 `g_access.size != 0` 当"这次是不是 guest 访存"的判据：`g_access` 记的是**最近一次** guest 访存，
//   宿主 AV 时它可能是完全无关的陈旧值（REQ-01M6JCBYBW8DV0920T28337ACF 的成因）。
// `guest_base` 为 nullptr（没有预留的 guest 空间）⇒ 一律 false。
bool inGuestSpace(const void* guest_base, std::uintptr_t fault_addr);
// 宿主自身的 fault：打印**与 guest fault 不同**的诊断（不含 `access     :` 行），`_Exit(4)`。
// ★ 退出码 4 与 guest fault 的 3 区分开；最近一次 guest 访存只作"可能无关"的附注打印。
[[noreturn]] void hostFault(std::uintptr_t fault_addr, const char* why);

// 打印一条区域表（诊断 / 报告用）
void dumpRegions(const std::vector<Region>& regions, std::FILE* out);
const char* regionKindName(RegionKind kind);

// ---- VirtualAlloc 的**区域账本**（`kernel32_mem.cpp` 的 Virtual* 语义靠它）----
// 记的是"`VirtualAlloc` 真的发出过哪些区域、提交到哪、释放了没有"。
// ★ 为什么不复用 `regions_`：区域表只服务**诊断**，它 append-only、**永不删除**；而下面三件事
//   都必须能回答"这块还活着吗"，否则：
//   · `VirtualFree(MEM_RELEASE)` 同一块释放两次都会成功（`shims-core#virtual-alloc-commit-release` 就靠它变红）；
//   · `VirtualAlloc(hint, …, MEM_COMMIT)` 无法判断该地址是否**先** `MEM_RESERVE` 过（文档：⇒ 487）；
//   · `VirtualFree(MEM_DECOMMIT, dwSize=0)` 找不到"整段"的边界（文档：退到该区域末尾）。
// ★ 语义（与 `kernel32_mem.cpp` 文件头偏差 3/4 一致）：`addr==0` 的 bump 块 `bump=true`；
//   `MEM_RELEASE` **按任意顺序**整段释放（2026-11-05 修正：判据是"lpAddress 等于某个活区域的基址"，
//   ⛔ 不再是"必须是最后一块 bump 块"—— 那条会拒掉引擎真实的 5 次释放）。
struct VaRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;        // 预留上界（bump 块 = 64 KiB 对齐后的跨度）
    std::uint32_t committed = 0;  // [begin, begin+committed) 已提交；0 = 只预留
    bool bump = false;            // true = addr==0 的 bump 块
    bool live = true;             // MEM_RELEASE 之后置 false（条目留着，供诊断 / 重复释放判据）
};

class VaRangeTable {
public:
    VaRange& add(std::uint32_t begin, std::uint32_t end, std::uint32_t committed, bool bump) {
        ranges_.push_back(VaRange{begin, end, committed, bump, true});
        return ranges_.back();
    }

    // 包含 ea 的**活**区域（新→旧；bump 块之间不重叠，显式地址区域也不重叠）
    VaRange* find(std::uint32_t ea) {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->live && ea >= it->begin && ea < it->end) return &*it;
        }
        return nullptr;
    }
    const VaRange* find(std::uint32_t ea) const {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->live && ea >= it->begin && ea < it->end) return &*it;
        }
        return nullptr;
    }

    // [ea, ea+size) **完整**落在某个活区域里吗（MEM_COMMIT-only 的前提）
    bool covers(std::uint32_t ea, std::uint32_t size) const {
        if (size == 0) return true;
        const std::uint64_t last = static_cast<std::uint64_t>(ea) + size - 1u;
        for (const VaRange& r : ranges_) {
            if (!r.live) continue;
            if (ea >= r.begin && last < r.end) return true;
        }
        return false;
    }

    // 与任何活区域**重叠**吗（MEM_RESERVE 的前提：不能重复预留）
    bool overlapsLive(std::uint32_t ea, std::uint32_t size) const {
        if (size == 0) return false;
        const std::uint64_t last = static_cast<std::uint64_t>(ea) + size - 1u;
        for (const VaRange& r : ranges_) {
            if (!r.live) continue;
            if (ea < r.end && last >= r.begin) return true;
        }
        return false;
    }

    // **最后一块**活着的 bump 块（LIFO 判据）；nullptr = 没有
    const VaRange* lastBump() const {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->live && it->bump) return &*it;
        }
        return nullptr;
    }

    // 基址**正好等于** ea 的活区域（`MEM_RELEASE` 的判据：Win32 要求 lpAddress 是预留的基址）。
    // ★ 2026-11-05 新增：`find()` 是"包含"，而释放必须认基址，否则释放中间地址会被当成合法。
    VaRange* findBegin(std::uint32_t ea) {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->live && it->begin == ea) return &*it;
        }
        return nullptr;
    }
    const VaRange* findBegin(std::uint32_t ea) const {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->live && it->begin == ea) return &*it;
        }
        return nullptr;
    }

    void release(std::uint32_t begin) {
        for (VaRange& r : ranges_) {
            if (r.live && r.begin == begin) r.live = false;
        }
    }

    void clear() { ranges_.clear(); }
    const std::vector<VaRange>& all() const { return ranges_; }

private:
    std::vector<VaRange> ranges_;
};

class GuestMemory {
public:
    static constexpr std::uint64_t kSpace = 0x100000000ull;  // 4 GiB
    static constexpr std::uint32_t kPage = 0x1000u;
    static constexpr std::uint32_t kPageMask = 0xFFFu;

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // 预留 4 GiB（构造时自动做；失败 ⇒ 对象不可用）
    bool reserve();
    bool reserved() const { return base_ != nullptr; }
    const char* lastError() const { return last_error_; }

    // 提交 [ea, ea+size)（按页向上取整；幂等）。`name` 进区域表（诊断用）。
    bool commit(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name);
    // 提交并清零（PE 节 / 堆 / 栈首次提交都要清零）
    bool commitZeroed(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name);
    void decommit(std::uint32_t ea, std::size_t size);
    // 只登记区域（不提交）：用于"这段属于谁"的诊断；stealth 页（guard）就是这样登记的
    void annotate(std::uint32_t ea, std::size_t size, RegionKind kind, const char* name);
    const Region* regionOf(std::uint32_t ea) const;
    const std::vector<Region>& regions() const { return regions_; }

    // VirtualAlloc 的区域账本（见上面的 VaRange 说明；由 kernel32_mem.cpp 维护）
    VaRangeTable& vaRanges() { return va_ranges_; }
    const VaRangeTable& vaRanges() const { return va_ranges_; }

    bool safeMode() const { return safe_; }
    void setSafeMode(bool on) { safe_ = on; }
    static bool safeModeFromEnv();  // LIFT_SAFE_MEMORY=1

    bool committed(std::uint32_t ea, std::size_t size = 1) const;
    std::uint64_t committedBytes() const;
    // ★ 提交位图的**版本号**：每次 commit / decommit 成功就 +1（只增不减）。
    //   给 Unicorn 后端做 O(1) 的"映射要不要复核"判据 —— 否则每个切片都要跑一遍
    //   `committedBytes()`（O(已提交页数)，实测是 shim 密集 boot 的主要开销之一）。
    std::uint64_t commitEpoch() const { return commit_epoch_; }

    std::uint8_t* host(std::uint32_t ea) { return base_ + ea; }
    const std::uint8_t* host(std::uint32_t ea) const { return base_ + ea; }

    // ---- 访存（热路径；内联）----
    // ★ 都是 const：解码器与诊断要在 const 上下文里读字节。`g_access` 是 thread_local 全局，
    //   改它不需要对象 const（它记的是"最近一次 guest 访存意图"，不是对象状态）。
    std::uint8_t rd8(std::uint32_t ea) const {
        g_access = {ea, 1, 0};
        if (safe_ && !committed(ea, 1)) guestFault(g_access, "读未提交页 (safe mode)");
        return base_[ea];
    }
    std::uint16_t rd16(std::uint32_t ea) const {
        g_access = {ea, 2, 0};
        if (safe_ && !committed(ea, 2)) guestFault(g_access, "读未提交页 (safe mode)");
        std::uint16_t v = 0;
        std::memcpy(&v, base_ + ea, 2);
        return v;
    }
    std::uint32_t rd32(std::uint32_t ea) const {
        g_access = {ea, 4, 0};
        if (safe_ && !committed(ea, 4)) guestFault(g_access, "读未提交页 (safe mode)");
        std::uint32_t v = 0;
        std::memcpy(&v, base_ + ea, 4);
        return v;
    }
    std::uint64_t rd64(std::uint32_t ea) const {
        g_access = {ea, 8, 0};
        if (safe_ && !committed(ea, 8)) guestFault(g_access, "读未提交页 (safe mode)");
        std::uint64_t v = 0;
        std::memcpy(&v, base_ + ea, 8);
        return v;
    }
    void wr8(std::uint32_t ea, std::uint8_t v) {
        g_access = {ea, 1, 1};
        if (safe_ && !committed(ea, 1)) guestFault(g_access, "写未提交页 (safe mode)");
        base_[ea] = v;
    }
    void wr16(std::uint32_t ea, std::uint16_t v) {
        g_access = {ea, 2, 1};
        if (safe_ && !committed(ea, 2)) guestFault(g_access, "写未提交页 (safe mode)");
        std::memcpy(base_ + ea, &v, 2);
    }
    void wr32(std::uint32_t ea, std::uint32_t v) {
        g_access = {ea, 4, 1};
        if (safe_ && !committed(ea, 4)) guestFault(g_access, "写未提交页 (safe mode)");
        std::memcpy(base_ + ea, &v, 4);
    }
    void wr64(std::uint32_t ea, std::uint64_t v) {
        g_access = {ea, 8, 1};
        if (safe_ && !committed(ea, 8)) guestFault(g_access, "写未提交页 (safe mode)");
        std::memcpy(base_ + ea, &v, 8);
    }

    void readBytes(std::uint32_t ea, void* dst, std::size_t n);
    void writeBytes(std::uint32_t ea, const void* src, std::size_t n);
    void fillBytes(std::uint32_t ea, std::uint8_t value, std::size_t n);

    // 读一条 C 字符串（最多 max_len 字节；不越过未提交页 —— 越界即 fault，不静默截断）
    std::string readCString(std::uint32_t ea, std::size_t max_len = 4096);

private:
    // ★ `end` 是**排他**上界，允许等于 4 GiB（0x1_0000_0000）⇒ 参数必须是 64 位：
    //   用 uint32 接会在"提交最后一页"时截断成 0，`begin >= end` 直接返回 true（什么都没提交）。
    bool commitPages(std::uint32_t begin, std::uint64_t end);
    void setBit(std::uint32_t page, bool on);
    bool bit(std::uint32_t page) const;

    std::uint8_t* base_ = nullptr;
    std::vector<std::uint64_t> commit_bits_;
    std::vector<Region> regions_;
    VaRangeTable va_ranges_;
    bool safe_ = false;
    std::uint64_t commit_epoch_ = 0;  // 见 commitEpoch()（commit / decommit 时 +1）
    char last_error_[160] = {};
};

// 宿主页大小（guest 页恒为 4 KiB，见 GuestMemory::kPage）。
// ★ Windows = `GetSystemInfo().dwPageSize`，POSIX = `sysconf(_SC_PAGESIZE)`（失败回退 4 KiB）。
// ⛔ 不许硬编码 0x1000：Apple Silicon macOS 是 16 KiB；Windows 的页大小也属于**系统事实**，
//   不该由本仓替它写死（REQ-01M6JFZFN15MBPG7Y89WNZACFW #2；16 KiB 宿主页踩过
//   REQ-01M6JBQ05P1QF56DQX5B9YJ4V8）。
std::uint32_t hostPageSize();

// 安装平台 fault handler（Windows: AddVectoredExceptionHandler；POSIX: sigaction）
void installPlatformFaultHandler();

// ★ Windows 上关掉 CRT 的 abort 行为与 WER/GPF 弹框（幂等；POSIX 上是空操作）。
//   为什么放在 runtime 而不是只放 `shims/register.h`（2026-11-04 用户实测踩了两次）：
//   本仓有**多处**直接 `std::abort()`（`heap.cpp` 的非法 free / `handles.cpp` 的句柄校验 /
//   `shims/register.h` 的 `shimAbort`）。只修 `shimAbort` 那一条，其余路径照样弹**模态框**
//   ⇒ 无人值守的流水线整轮挂死在对话框上（而且 stdout 还没 flush，连日志都看不到）。
//   `_set_abort_behavior` 是**进程级**的 ⇒ 在 `main()` 最前面调一次就覆盖所有 abort 点。
//   ⛔ 这不是"把错误藏起来"：日志照旧打到 stderr，退出码照旧非 0。
void suppressCrashDialogsOnce();

}  // namespace lift
