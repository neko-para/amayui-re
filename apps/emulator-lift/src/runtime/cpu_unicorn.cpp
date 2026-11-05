// emulator-lift · Unicorn（QEMU TCG）CPU 后端 —— 切片执行器（实验 POC）
//
// ★ 本文件是**唯一** include <unicorn/unicorn.h> 的翻译单元（默认构建根本不编译它）。
// ★ 设计（长注释在 cpu_unicorn.h）：
//   ① 内存：把**已提交**的 guest 页用 `uc_mem_map_ptr` 指到 `GuestMemory` 的同一份宿主内存
//      （⛔ 不复制 4 GiB；⛔ 不映射未提交页 ⇒ "未提交即 fault" 的机械护栏还在）。
//      运行期新提交的页在 `UC_HOOK_MEM_UNMAPPED` 里补映射；guest `VirtualFree` 掉的页在**切片边界**
//      被 unmap（否则 Unicorn 会直接踩 PROT_NONE 宿主页 —— 那是宿主 SIGSEGV，不是可诊断的停机）。
//   ② 寄存器：每个切片边界 `X86` ↔ Unicorn **双向**同步（EAX..EDI / EIP / EFLAGS / FS_BASE /
//      GS_BASE + x87（FP0..7 / FPCW / FPSW / FPTAG）+ XMM0..7）。段选择子不同步（见文件末尾注释）。
//   ③ 停点：`UC_HOOK_CODE` 只装在 **shim 桩页**（每个有 stub_ea 的页一个 hook）；
//      另有一个**计数 hook**（为了与解释器同语义：`clock_.tick()` 逐指令推进 + `--max-insns` 精确 +
//      `insns` 精确）。计数 hook 不是停点，它只记账、只在"切片预算用完 / max-insns 到"时停。
//      续延（`kContinuationEa`）与 winmm 定时器返回点（`kTimerCallbackReturnEa`）由
//      `UC_HOOK_MEM_UNMAPPED` 认出来：这两个地址**从不提交**，解释器也不在那里执行字节。
//   ④ 命中 shim：同步寄存器 → `ShimRegistry::invoke`（与解释器**同一个函数**）→ 同步回去 → 下一段。
//      ⛔ 不在 shim 的 hook 里嵌套 `uc_emu_start`；原因见上（续延语义，不是 Unicorn 不支持）。
//      `LIFT_UC_INLINE_SHIM=1` 时直接在 stub 的 UC_HOOK_CODE 内 invoke，不 stop/restart；
//      `LIFT_UC_NO_COUNT_HOOK=1` 时关掉全局逐指令计数 hook（只用于吞吐实验）。
//   ⑤ fault：`UC_ERR_*_UNMAPPED` / `*_PROT` / `INSN_INVALID` 映射到 `StopReason` 并打 EA + 区域 +
//      `.lst` 文本；⛔ 不静默继续。
#include <unicorn/unicorn.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/cpu_unicorn.h"
#include "runtime/exec.h"
#include "runtime/guest_call.h"
#include "runtime/x86.h"

namespace lift {
namespace {

constexpr std::uint32_t kGuestPage = 0x1000u;
// 一次 `uc_emu_start` 最多执行的指令数（同时是 `--time-limit` 的墙钟检查粒度：
// 1M 条在 50 MIPS 上是 ~20ms；⛔ 不用 uc_emu_start 的 timeout 参数 —— 它每片都起一个宿主线程）。
constexpr std::uint64_t kSliceInsns = 1u << 20;

// Unicorn 的 x86 寄存器枚举**不按 GPR 序号排列** ⇒ 显式表，⛔ 不要算术推。
struct UcRegPair {
    int uc;
    int idx;
};
constexpr UcRegPair kGprs[8] = {
    {UC_X86_REG_EAX, 0}, {UC_X86_REG_ECX, 1}, {UC_X86_REG_EDX, 2}, {UC_X86_REG_EBX, 3},
    {UC_X86_REG_ESP, 4}, {UC_X86_REG_EBP, 5}, {UC_X86_REG_ESI, 6}, {UC_X86_REG_EDI, 7},
};

enum class StopKind : std::uint8_t {
    None = 0,
    Shim,          // 落在 shim 桩地址上（stop/restart 模式：还没 invoke）
    ShimStopped,   // inline 模式：shim 已经把 cpu.running 置 false，只等宿主收尾
    Continuation,  // 落在 GuestCallStack::kContinuationEa
    TimerReturn,   // 落在 kTimerCallbackReturnEa
    Memory,        // 未映射 / 未提交访存（stop_detail 说明读/写 + EA + 区域）
    Slice,         // 切片预算用完（不是停机原因，继续）
    MaxInsns,      // 到了 --max-insns
};

// 一小段已映射进 Unicorn 的 guest 区间（[begin, end)，4 KiB 对齐）
struct MappedRun {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

class UcEngine {
public:
    UcEngine(GuestMemory& mem, ShimRegistry& shims) : mem_(mem), shims_(shims) {}
    ~UcEngine() { close(); }
    UcEngine(const UcEngine&) = delete;
    UcEngine& operator=(const UcEngine&) = delete;

    bool open(std::string* err);
    void close();

    // 把 X86 的全部状态写进 Unicorn / 读回 X86
    void syncTo(X86& cpu);
    void syncFrom(X86& cpu);
    // 只同步 shim 需要的核心寄存器（GPR / EIP / EFLAGS）；EIP 必须单寄存器写（见实现注释）
    void syncCoreTo(X86& cpu);
    void syncCoreFrom(X86& cpu);

    // 新登记的 stub 可能落在还没 hook 的页上（运行期 addRuntime / 夹具先登记后 run）
    void ensureShimHooks();
    // guest 的提交位图变了（heap 增长 / VirtualFree）⇒ 把我们映射过的区间重新核对一遍
    void refreshMappings();

    uc_engine* handle() const { return uc_; }
    ShimRegistry& shims() { return shims_; }

    // ---- hook 回调入口 ----
    void onCode(uc_engine* uc, std::uint32_t ea);
    bool onMem(uc_engine* uc, uc_mem_type type, std::uint64_t address, int size);
    void onShimCode(uc_engine* uc, std::uint32_t ea);

    // ---- 记账（由 Machine::runUnicorn 填）----
    X86* cpu = nullptr;
    VirtualClock* clock = nullptr;
    const LstIndex* lst = nullptr;
    bool trace_blocks = false;
    std::FILE* trace_out = nullptr;
    // ★ 实验开关（环境变量；见 runUnicorn）：
    //   LIFT_UC_NO_COUNT_HOOK=1  ⇒ 不装全局逐指令计数 hook（性能实验；insns/clock 不再精确）
    //   LIFT_UC_INLINE_SHIM=1    ⇒ 在 stub 的 UC_HOOK_CODE 内直接 invoke shim，不 stop/restart
    bool count_insns = true;
    bool inline_shim = false;
    std::uint64_t max_insns = 0;  // 0 = 不计上限
    std::uint64_t slice_start_insns = 0;
    std::uint64_t last_commit_epoch = 0;

    // ---- 停点（hook 写，切片循环读）----
    StopKind stop = StopKind::None;
    std::uint32_t stop_ea = 0;
    const ShimDef* stop_shim = nullptr;
    std::string stop_detail;

private:
    void addShimPageHook(std::uint32_t page);
    bool inShimPage(std::uint32_t ea) const;
    // 这条指令是否以 REP/REPNE 前缀开头（`F3`/`F2`）—— REP 折算的判据（见 onCode）。
    bool isRepPrefixed(std::uint32_t ea) const {
        if (!mem_.committed(ea, 1)) return false;
        const std::uint8_t b = *mem_.host(ea);
        return b == 0xF3u || b == 0xF2u;
    }
    bool mapCommittedRun(std::uint32_t ea, std::string* why);
    bool isMapped(std::uint64_t ea) const;
    void noteMemFault(const char* what, std::uint64_t address, int size);
public:
    // FS / GS 的**段基址**：Unicorn 2.1.4 在 32 位模式下**不支持**写 FS_BASE/GS_BASE
    // （qemu/target/i386/unicorn.c 里那两行是注释掉的，写下去是 no-op + 一行 deprecation 警告）。
    // ⇒ 用 x86 自己的机制：在 guest 里放一张 GDT、写 GDTR、再用选择子把 FS/GS 基址装进去。
    bool installSegmentBases(std::string* why);
private:

    GuestMemory& mem_;
    ShimRegistry& shims_;
    uc_engine* uc_ = nullptr;
    std::vector<MappedRun> mapped_;
    std::vector<std::uint32_t> shim_pages_;
    std::uint32_t shim_page_lo_ = 1;  // 空区间 = lo > hi
    std::uint32_t shim_page_hi_ = 0;
    std::uint32_t last_counted_ea_ = 0;   // 计数 hook 上一次计入的 EA（重放 / REP 折算用，见 onCode）
    bool mapped_since_last_count_ = false;  // 上一次计数之后补映射过（⇒ 那条指令会被 Unicorn 重放）
    std::uint32_t gdt_ea_ = 0;            // GDT 所在的 guest 页（0 = 还没装）
    std::uint32_t fs_base_loaded_ = 0;    // 已经装进 Unicorn 的 FS 基址
    std::uint32_t gs_base_loaded_ = 0;
};

// ---- hook 回调（C 函数指针；user_data = UcEngine*）----
void ucCodeHook(uc_engine* uc, std::uint64_t address, std::uint32_t size, void* user) {
    (void)size;
    static_cast<UcEngine*>(user)->onCode(uc, static_cast<std::uint32_t>(address));
}
void ucShimHook(uc_engine* uc, std::uint64_t address, std::uint32_t size, void* user) {
    (void)size;
    static_cast<UcEngine*>(user)->onShimCode(uc, static_cast<std::uint32_t>(address));
}
bool ucMemHook(uc_engine* uc, uc_mem_type type, std::uint64_t address, int size, std::int64_t value, void* user) {
    (void)value;
    return static_cast<UcEngine*>(user)->onMem(uc, type, address, size);
}

// ---- 寄存器读写（16 字节缓冲：Unicorn 只写它自己那份宽度；最大的是 XMM = 16）----
bool writeReg(uc_engine* uc, int reg, const void* p) { return uc_reg_write(uc, reg, p) == UC_ERR_OK; }
bool readReg(uc_engine* uc, int reg, void* p) { return uc_reg_read(uc, reg, p) == UC_ERR_OK; }

bool UcEngine::open(std::string* err) {
    const uc_err e = uc_open(UC_ARCH_X86, UC_MODE_32, &uc_);
    if (e != UC_ERR_OK) {
        if (err) *err = std::string("uc_open(UC_ARCH_X86, UC_MODE_32) 失败：") + uc_strerror(e);
        uc_ = nullptr;
        return false;
    }
    // 未映射访存：已提交 ⇒ 补映射并重试；未提交 ⇒ 停（MemoryFault），⛔ 不静默返回 0。
    uc_hook h = 0;
    if (uc_hook_add(uc_, &h, UC_HOOK_MEM_UNMAPPED, reinterpret_cast<void*>(ucMemHook), this, 1, 0) != UC_ERR_OK) {
        if (err) *err = "uc_hook_add(UC_HOOK_MEM_UNMAPPED) 失败";
        close();
        return false;
    }
    // ★ 计数 hook（逐指令）：**唯一**的全局 code hook，它只记账、不是停机原因。
    //   它必须覆盖全部地址（begin=1,end=0 = "任何地址"，见 unicorn.h 的 uc_hook_add 注释）。
    //   shim 桩页上的指令由 onCode 里的 inShimPage 提前返回（桩 hook 会先停，不执行那条指令）。
    //   ⚠ 官方 FAQ 点名的头号减速项就是"hook 每条指令"；`LIFT_UC_NO_COUNT_HOOK=1` 可关掉它做
    //   A/B 性能实验（此时 insns / clock 不再精确，只用于测吞吐）。
    if (count_insns) {
        uc_hook hcount = 0;
        if (uc_hook_add(uc_, &hcount, UC_HOOK_CODE, reinterpret_cast<void*>(ucCodeHook), this, 1, 0) !=
            UC_ERR_OK) {
            if (err) *err = "uc_hook_add(UC_HOOK_CODE, 计数) 失败";
            close();
            return false;
        }
    }
    return true;
}

void UcEngine::close() {
    if (!uc_) return;
    uc_close(uc_);
    uc_ = nullptr;
    mapped_.clear();
    shim_pages_.clear();
    shim_page_lo_ = 1;
    shim_page_hi_ = 0;
}

// 计数 hook：**唯一**的逐指令回调。它做四件事（与解释器同语义）：
//   · `++cpu.insns` / `clock_.tick()`（虚拟时钟的确定性判据 A4 依赖"每条指令一次 tick"）
//   · `--max-insns` 的**精确**停点（停在下一条要执行的指令上，与解释器同 eip 口径）
//   · 切片预算（让 --time-limit 有墙钟检查点；⛔ 不用 uc_emu_start 的 timeout，它每片起线程）
//   · `--trace-blocks` 的诊断输出（只有开了才付这份代价）
// ★ shim 桩页上的指令**不计数**：桩 hook 会在它们执行前停下（解释器里桩块也不执行指令）。
void UcEngine::onCode(uc_engine* uc, std::uint32_t ea) {
    if (inShimPage(ea)) return;
    // ★ REP 折算（**与解释器同口径**）：QEMU 会把 `rep movs/stos/...` 按 TB 拆成多段，每段都会
    //   回到**同一个 EA** 再进一次 code hook；解释器把整条 rep 算**一条**指令（`exec_string.cpp`
    //   自己跑完整个迭代）。⇒ "同一个 EA 的连续重入 + 该 EA 以 F3/F2 开头" 不再计数、也不再 tick。
    //   实测（boot 到 ExitProcess）：不折算时 insns 比解释器多 347 条（0x4F3D7D = `F3 A5` rep movsd
    //   被算了 141 次）；折算后两边**逐条相同**。判据见 tests/unicorn_backend.cpp 的 max-insns 用例。
    // ★ 重放折算：Unicorn 在 `UC_HOOK_MEM_UNMAPPED` 里补映射之后会**重试**那次访存（有时整条指令重放），
    //   于是同一条指令的 code hook 会被调两次。判据 = "同一个 EA 连续 + 上一次计数之后刚补过映射"。
    //   ⛔ 不能无条件跳过"同 EA 连续"：`jmp $` 就是同 EA 无限循环，跳过会让 --max-insns 永远到不了。
    if (ea == last_counted_ea_ && (isRepPrefixed(ea) || mapped_since_last_count_)) {
        mapped_since_last_count_ = false;
        return;
    }
    mapped_since_last_count_ = false;
    last_counted_ea_ = ea;
    if (max_insns && cpu->insns >= max_insns) {
        stop = StopKind::MaxInsns;
        stop_ea = ea;
        uc_emu_stop(uc);
        return;
    }
    if (cpu->insns - slice_start_insns >= kSliceInsns) {
        stop = StopKind::Slice;
        stop_ea = ea;
        uc_emu_stop(uc);
        return;
    }
    ++cpu->insns;
    if (clock) clock->tick();
    cpu->noteInsn(ea, nullptr);
    if (trace_blocks) {
        const char* text = lst ? lst->textPtr(ea) : nullptr;
        std::fprintf(trace_out ? trace_out : stdout, "[insn] %08X %s\n", ea, (text && text[0]) ? text : "?");
    }
}

void UcEngine::onShimCode(uc_engine* uc, std::uint32_t ea) {
    const ShimDef* def = shims_.byStubEa(ea);
    if (!def) return;  // 桩页里没登记的地址：与解释器一致，当普通代码（解释器会解出 0 字节指令）
    if (!inline_shim) {
        stop = StopKind::Shim;
        stop_ea = ea;
        stop_shim = def;
        uc_emu_stop(uc);
        return;
    }

    // ★ 实验模式：不退出 uc_emu_start。Qiling 的 WinAPI 分发就是这个形状
    //   （windows.py 里 hook 内直接调 shim，然后在 os.py 里写 PC/SP）。
    //   ⚠ EIP 必须用单寄存器 API 写：uc_reg_write_batch 不置 skip_sync_pc_on_exit，
    //   写完可能被 cpu_loop_exit_restore 回滚到桩指令（源码推断；本实验可验证）。
    syncCoreFrom(*cpu);
    if (stop != StopKind::None) {
        stop_ea = ea;
        uc_emu_stop(uc);
        return;
    }
    ShimDef* mutable_def = const_cast<ShimDef*>(def);
    const bool had_impl = mutable_def->fn != nullptr;
    shims_.invoke(*cpu, *mutable_def);
    if (!cpu->running) {
        stop = StopKind::ShimStopped;
        stop_ea = ea;
        stop_shim = def;
        stop_detail = std::string(had_impl ? "shim 主动结束运行：" : "未实现的导入 ") + def->module + "!" +
                      def->name;
        uc_emu_stop(uc);
        return;
    }
    syncCoreTo(*cpu);
    if (stop != StopKind::None) {
        stop_ea = ea;
        uc_emu_stop(uc);
    }
}

void UcEngine::noteMemFault(const char* what, std::uint64_t address, int size) {
    const std::uint32_t ea = static_cast<std::uint32_t>(address);
    const Region* r = mem_.regionOf(ea);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s 0x%08X size=%d%s%s", what, ea, size, r ? " region=" : "",
                  r ? regionKindName(r->kind) : "");
    stop_detail = buf;
}

bool UcEngine::onMem(uc_engine* uc, uc_mem_type type, std::uint64_t address, int size) {
    const bool is_fetch = type == UC_MEM_FETCH_UNMAPPED || type == UC_MEM_FETCH_PROT;
    if (const char* dbg = std::getenv("LIFT_UC_TRACE_FAULTS")) {
        if (dbg[0] && dbg[0] != '0')
            std::fprintf(stderr, "[uc-mem] type=%d addr=%08llX size=%d insns=%llu eip=%08X\n",
                         static_cast<int>(type), static_cast<unsigned long long>(address), size,
                         cpu ? static_cast<unsigned long long>(cpu->insns) : 0ull, cpu ? cpu->eip : 0u);
    }
    // ---- ① 两个哨兵地址：解释器在 dispatcher 层认出它们，**从不**在那里执行字节 ----
    if (is_fetch &&
        (address == GuestCallStack::kContinuationEa || address == kTimerCallbackReturnEa)) {
        stop = address == GuestCallStack::kContinuationEa ? StopKind::Continuation : StopKind::TimerReturn;
        stop_ea = static_cast<std::uint32_t>(address);
        uc_emu_stop(uc);
        return false;
    }
    // ---- ② guest 地址空间之外 / 越界 ----
    const std::uint64_t n = static_cast<std::uint64_t>(size > 0 ? size : 1);
    if (address >= GuestMemory::kSpace || address + n > GuestMemory::kSpace) {
        noteMemFault(is_fetch ? "FETCH 越界（>4GiB）" : (type == UC_MEM_WRITE_UNMAPPED ? "WRITE 越界" : "READ 越界"),
                     address, size);
        stop = StopKind::Memory;
        stop_ea = static_cast<std::uint32_t>(address);
        uc_emu_stop(uc);
        return false;
    }
    const std::uint32_t ea = static_cast<std::uint32_t>(address);
    // ---- ③ 未提交 ⇒ guest fault（⛔ 不补映射、不返回 0）----
    if (!mem_.committed(ea, static_cast<std::size_t>(n))) {
        noteMemFault(is_fetch ? "FETCH 未提交页"
                              : (type == UC_MEM_WRITE_UNMAPPED ? "WRITE 未提交页" : "READ 未提交页"),
                     address, size);
        stop = StopKind::Memory;
        stop_ea = ea;
        uc_emu_stop(uc);
        return false;
    }
    // ---- ④ 已提交 ⇒ 补映射（uc_mem_map_ptr 指向**同一份** GuestMemory 宿主页）----
    std::string why;
    if (!mapCommittedRun(ea, &why)) {
        stop_detail = why;
        stop = StopKind::Memory;
        stop_ea = ea;
        uc_emu_stop(uc);
        return false;
    }
    return true;  // Unicorn 会重试这次访问
}

bool UcEngine::isMapped(std::uint64_t ea) const {
    for (const MappedRun& r : mapped_) {
        if (ea >= r.begin && ea < r.end) return true;
    }
    return false;
}

// 把 ea 所在的那一整段"连续已提交、且还没映射"的 4 KiB 页映射进来。
// ★ 不越过已映射区间（否则 uc_mem_map_ptr 返回 UC_ERR_MAP）；也不越过未提交页（保留 fault 语义）。
bool UcEngine::mapCommittedRun(std::uint32_t ea, std::string* why) {
    if (isMapped(ea)) {
        if (why) *why = "内部错误：Unicorn 报未映射，但我们的记账里这一段已映射";
        return false;
    }
    std::uint64_t begin = ea & ~static_cast<std::uint64_t>(kGuestPage - 1u);
    std::uint64_t end = begin + kGuestPage;
    while (begin >= kGuestPage && mem_.committed(static_cast<std::uint32_t>(begin - kGuestPage)) &&
           !isMapped(begin - kGuestPage)) {
        begin -= kGuestPage;
    }
    while (end < GuestMemory::kSpace && mem_.committed(static_cast<std::uint32_t>(end)) && !isMapped(end)) {
        end += kGuestPage;
    }
    const uc_err e =
        uc_mem_map_ptr(uc_, begin, end - begin, UC_PROT_ALL, mem_.host(static_cast<std::uint32_t>(begin)));
    if (e != UC_ERR_OK) {
        if (why) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "uc_mem_map_ptr(%08llX, %llu 字节) 失败：%s",
                          static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin),
                          uc_strerror(e));
            *why = buf;
        }
        return false;
    }
    mapped_.push_back(MappedRun{begin, end});
    mapped_since_last_count_ = true;  // 触发这次映射的指令会被 Unicorn 重放一次（见 onCode）
    return true;
}

bool UcEngine::inShimPage(std::uint32_t ea) const {
    const std::uint32_t page = ea >> 12;
    if (page < shim_page_lo_ || page > shim_page_hi_) return false;
    for (std::uint32_t p : shim_pages_) {
        if (p == page) return true;
    }
    return false;
}

void UcEngine::ensureShimHooks() {
    for (const ShimDef& d : shims_.all()) {
        if (d.stub_ea == 0) continue;
        if (inShimPage(d.stub_ea)) continue;
        addShimPageHook(d.stub_ea >> 12);
    }
}

void UcEngine::addShimPageHook(std::uint32_t page) {
    shim_pages_.push_back(page);
    if (page < shim_page_lo_) shim_page_lo_ = page;
    if (page > shim_page_hi_) shim_page_hi_ = page;
    uc_hook h = 0;
    const std::uint64_t begin = static_cast<std::uint64_t>(page) << 12;
    const std::uint64_t end = begin + kGuestPage - 1u;  // hook 区间是**闭区间**
    const uc_err e = uc_hook_add(uc_, &h, UC_HOOK_CODE, reinterpret_cast<void*>(ucShimHook), this, begin, end);
    if (e != UC_ERR_OK) {
        std::fprintf(stderr, "emulator-lift: ⚠ uc_hook_add(shim page %08X) 失败：%s\n", page, uc_strerror(e));
    }
}

void UcEngine::refreshMappings() {
    // ★ O(1) 判据：`commitEpoch()` 只在 commit / decommit 时变。
    //   ⛔ 不要用 `committedBytes()`：它 O(已提交页数)（265 MiB 已提交时每次 ~50µs），
    //   而这里**每个切片**都会跑一次（boot 到 ExitProcess 有 658 次 shim）。
    const std::uint64_t now = mem_.commitEpoch();
    if (now == last_commit_epoch) return;
    last_commit_epoch = now;
    for (std::size_t i = mapped_.size(); i-- > 0;) {
        const MappedRun r = mapped_[i];
        if (mem_.committed(static_cast<std::uint32_t>(r.begin), static_cast<std::size_t>(r.end - r.begin))) continue;
        // 这段里有一部分被 guest 退掉了（VirtualFree）：整段 unmap，之后按需重映射**已提交**的页。
        // ⛔ 不能留着：Unicorn 是直接踩宿主指针的，而 GuestMemory::decommit 已经把它 PROT_NONE 了。
        std::fprintf(stderr, "[lift] Unicorn: guest 退掉了 %08llX-%08llX 的一部分 ⇒ unmap 这段\n",
                     static_cast<unsigned long long>(r.begin), static_cast<unsigned long long>(r.end));
        uc_mem_unmap(uc_, r.begin, r.end - r.begin);
        mapped_.erase(mapped_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// x86 的"可读写数据段"描述子（P=1、S=1、type=2（RW 数据）、D/B=1；DPL 可指定）。
// ★ 这是**段基址**在 32 位保护模式下唯一的装载途径（见 installSegmentBases 的注释）。
std::uint64_t dataSegmentDescriptor(std::uint32_t base, std::uint32_t limit, std::uint8_t dpl,
                                    bool page_granular) {
    const std::uint64_t access = 0x90ull | 0x02ull | ((static_cast<std::uint64_t>(dpl) & 3ull) << 5);
    std::uint64_t flags = 0x4ull;  // D/B = 1（32 位栈 / 32 位偏移）
    if (page_granular) flags |= 0x8ull;  // G = 1（limit 以 4 KiB 为单位）
    std::uint64_t d = 0;
    d |= static_cast<std::uint64_t>(limit & 0xFFFFu);              // [15:0]  limit[15:0]
    d |= static_cast<std::uint64_t>(base & 0xFFFFu) << 16;         // [31:16] base[15:0]
    d |= static_cast<std::uint64_t>((base >> 16) & 0xFFu) << 32;   // [39:32] base[23:16]
    d |= access << 40;                                             // [47:40] access
    d |= static_cast<std::uint64_t>((limit >> 16) & 0xFu) << 48;   // [51:48] limit[19:16]
    d |= flags << 52;                                              // [55:52] flags
    d |= static_cast<std::uint64_t>((base >> 24) & 0xFFu) << 56;   // [63:56] base[31:24]
    return d;
}

// ★★ FS / GS 的**段基址**：Unicorn 2.1.4 在 32 位模式下**不支持**直接写 FS_BASE/GS_BASE ——
//   `qemu/target/i386/unicorn.c` 的 reg_write 里那两行是**注释掉**的（"Don't think base registers
//   are a 'thing' on x86"），写下去是 no-op + 一行 deprecation 警告（CHECK_RET_DEPRECATE 把
//   UC_ERR_ARG 吞成 OK）。对 Windows 32 位 guest 这是致命的：TEB 只能通过 `fs:` 访问。
//   ⇒ 用 x86 自己的机制：在 guest 里放一张 GDT（null / flat-data(SS) / FS / GS），写 GDTR，
//     再用**选择子**把 FS/GS 基址装进段缓存。实测（见报告）：
//       读回的 FS_BASE == 装进去的基址，且 guest 的 `mov eax, fs:[0]` 拿到那个地址的内容。
//   ⚠⚠ 还有一个**必须一起做**的坑（实测踩到）：Unicorn 32 位初始的 SS 段缓存 flags 是 0（B=0），
//     而 `uc_reg_write(FS)` 会走 `cpu_x86_load_seg` → `cpu_x86_update_hflags` **重算** HF_SS32
//     ⇒ 之后所有 push/ret/call 的栈地址被截成 16 位（实测 `[esp]` 读的是 `esp & 0xFFFF`）。
//     所以装 FS 之前必须先重装 SS 为一个 **flat 数据段**（base=0 / limit=4GiB / D/B=1）。
//   ⚠ 副作用（写进报告）：guest 看到的 SS/FS/GS **选择子**从 0 变成 0x08|CPL / 0x13 / 0x1B
//     （解释器里恒为 0），且 GDTR 变成我们这张表。
bool UcEngine::installSegmentBases(std::string* why) {
    const bool need_fs = cpu->fs_base != 0;
    const bool need_gs = cpu->gs_base != 0;
    if (!need_fs && !need_gs) return true;  // 裸夹具（没有 TEB）：不动 guest 的段表
    if (!gdt_ea_) {
        std::uint32_t page = 0;
        for (std::uint32_t cand = 0x7FFF0000u; cand < 0x80000000u; cand += kGuestPage) {
            if (mem_.regionOf(cand) || mem_.committed(cand, kGuestPage)) continue;
            page = cand;
            break;
        }
        if (!page) {
            if (why) *why = "找不到空闲 guest 页放 GDT ⇒ FS/GS 基址装不上（32 位 Unicorn 不支持直接写 FS_BASE）";
            return false;
        }
        if (!mem_.commitZeroed(page, kGuestPage, RegionKind::Other, "unicorn-gdt")) {
            if (why) *why = "提交 GDT 页失败";
            return false;
        }
        gdt_ea_ = page;
        // 描述子 [0] = null、[1] = flat 数据段（给 SS）、[2] = FS、[3] = GS
        std::memset(mem_.host(gdt_ea_), 0, kGuestPage);
        uc_x86_mmr gdtr{};
        gdtr.base = gdt_ea_;
        gdtr.limit = 4 * 8 - 1;
        if (!writeReg(uc_, UC_X86_REG_GDTR, &gdtr)) {
            if (why) *why = "uc_reg_write(GDTR) 失败 ⇒ FS/GS 基址装不上";
            return false;
        }
        // ---- 先重装 SS 为 flat 数据段（见上面 ⚠⚠：不这么做之后 push/ret 会变 16 位栈）----
        std::uint16_t cs = 0;
        const std::uint32_t cpl = readReg(uc_, UC_X86_REG_CS, &cs) ? (cs & 3u) : 0u;
        const std::uint64_t flat = dataSegmentDescriptor(0, 0xFFFFFu, static_cast<std::uint8_t>(cpl), true);
        std::memcpy(mem_.host(gdt_ea_ + 8u), &flat, sizeof(flat));
        const std::uint16_t ss_sel = static_cast<std::uint16_t>(0x08u | cpl);
        const uc_err se = uc_reg_write(uc_, UC_X86_REG_SS, &ss_sel);
        if (se != UC_ERR_OK) {
            if (why) *why = std::string("重装 SS（flat 数据段）失败：") + uc_strerror(se);
            return false;
        }
    }
    auto loadBase = [&](const char* name, std::uint32_t desc_index, int seg_reg, std::uint32_t base) -> bool {
        const std::uint64_t desc = dataSegmentDescriptor(base, 0x0FFFu, 3, false);  // 一页（TEB 就是一页）
        std::memcpy(mem_.host(gdt_ea_ + desc_index * 8u), &desc, sizeof(desc));
        const std::uint16_t sel = static_cast<std::uint16_t>((desc_index << 3) | 3u);
        const uc_err e = uc_reg_write(uc_, seg_reg, &sel);
        if (e != UC_ERR_OK) {
            if (why) *why = std::string("装载 ") + name + " 选择子失败：" + uc_strerror(e);
            return false;
        }
        return true;
    };
    if (need_fs && cpu->fs_base != fs_base_loaded_) {
        if (!loadBase("FS", 2, UC_X86_REG_FS, cpu->fs_base)) return false;
        fs_base_loaded_ = cpu->fs_base;
        // FS_BASE 在 32 位模式下**读**是支持的（只有写不支持）⇒ 读回校验，⛔ 不靠"应该装上了"。
        std::uint32_t got = 0;
        if (!readReg(uc_, UC_X86_REG_FS_BASE, &got) || got != cpu->fs_base) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "FS 基址装载后读回不一致（想要 0x%08X，读到 0x%08X）⇒ TEB 访问会错",
                          cpu->fs_base, got);
            if (why) *why = buf;
            return false;
        }
    }
    if (need_gs && cpu->gs_base != gs_base_loaded_) {
        if (!loadBase("GS", 3, UC_X86_REG_GS, cpu->gs_base)) return false;
        // ⚠ 32 位模式下 GS_BASE **读不回来**（unicorn 未实现）⇒ 只能单向写，无法校验。
        gs_base_loaded_ = cpu->gs_base;
    }
    return true;
}

// ★ inline shim 只用核心寄存器：GPR + EIP + EFLAGS。x87/XMM 留在 Unicorn 内部
//   （shim 通常不读它们；本实验只测控制流/性能，不把"没同步 FPU"说成全量正确）。
//   ★ EIP 单独写：见 onShimCode 的注释（batch 写 PC 会被回滚）。
void UcEngine::syncCoreFrom(X86& cpu_) {
    int regs[10];
    void* vals[10];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        regs[n] = kGprs[i].uc;
        vals[n] = &cpu_.r[i];
        ++n;
    }
    std::uint32_t eip = 0, eflags = 0;
    regs[n] = UC_X86_REG_EIP;
    vals[n] = &eip;
    ++n;
    regs[n] = UC_X86_REG_EFLAGS;
    vals[n] = &eflags;
    ++n;
    if (uc_reg_read_batch(uc_, regs, vals, n) != UC_ERR_OK) {
        stop = StopKind::Memory;
        stop_detail = "uc_reg_read_batch（inline shim 核心寄存器）失败";
        return;
    }
    cpu_.eip = eip;
    cpu_.eflags = eflags;
}

void UcEngine::syncCoreTo(X86& cpu_) {
    int regs[9];
    void* vals[9];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        regs[n] = kGprs[i].uc;
        vals[n] = &cpu_.r[i];
        ++n;
    }
    regs[n] = UC_X86_REG_EFLAGS;
    vals[n] = &cpu_.eflags;
    ++n;
    if (uc_reg_write_batch(uc_, regs, vals, n) != UC_ERR_OK) {
        stop = StopKind::Memory;
        stop_detail = "uc_reg_write_batch（inline shim 核心寄存器）失败";
        return;
    }
    const std::uint32_t eip = cpu_.eip;
    if (uc_reg_write(uc_, UC_X86_REG_EIP, &eip) != UC_ERR_OK) {
        stop = StopKind::Memory;
        stop_detail = "uc_reg_write(EIP)（inline shim）失败";
    }
}

// ★ 寄存器同步**必须走批量 API**（`uc_reg_read/write_batch`）：Unicorn 每次单寄存器访问都会
//   `UC_INIT` + `restore_jit_state`（macOS 上是 `jit_write_protect` 的 mprotect）⇒ 实测 ~2µs/次；
//   一次切片 66 次单写/单读 ≈ 130µs，boot 到 ExitProcess 有 658 次 shim ⇒ 直接掉到 0.4 MIPS。
//   批量后同一个切片只付一次开销（实测见报告）。
void UcEngine::syncTo(X86& cpu_) {
    int regs[32];
    void* vals[32];
    int n = 0;
    // ---- 通用寄存器 + EIP + EFLAGS ----
    std::uint32_t gpr[8];
    for (int i = 0; i < 8; ++i) {
        gpr[i] = cpu_.r[i];
        regs[n] = kGprs[i].uc;
        vals[n] = &gpr[i];
        ++n;
    }
    std::uint32_t eip = cpu_.eip;
    std::uint32_t eflags = cpu_.eflags;
    regs[n] = UC_X86_REG_EIP;
    vals[n] = &eip;
    ++n;
    regs[n] = UC_X86_REG_EFLAGS;
    vals[n] = &eflags;
    ++n;
    // ★ FS_BASE / GS_BASE **不在这里写**：32 位模式下 Unicorn 不支持（no-op + 警告）——
    //   基址由 installSegmentBases() 用 GDT 描述子 + 选择子装载。⛔ 不要"补一行写 FS_BASE 求心安"。
    // ---- x87：X87State 的 16 字节槽里前 10 字节 = SoftFloat extFloat80 =
    //      {uint64 signif; uint16 signExp;}（小端），与 Unicorn 的 FP0..7（cpu_get_fp80 写 8+2）同布局。
    for (int i = 0; i < 8; ++i) {
        regs[n] = UC_X86_REG_FP0 + i;
        vals[n] = cpu_.fpu.regs[i];
        ++n;
    }
    std::uint16_t w = cpu_.fpu.control;
    regs[n] = UC_X86_REG_FPCW;
    vals[n] = &w;
    ++n;
    const std::uint16_t fsw = static_cast<std::uint16_t>((cpu_.fpu.status & ~0x3800u) | ((cpu_.fpu.top & 7u) << 11));
    regs[n] = UC_X86_REG_FPSW;
    vals[n] = const_cast<std::uint16_t*>(&fsw);
    ++n;
    const std::uint16_t tag = cpu_.fpu.tag;
    regs[n] = UC_X86_REG_FPTAG;
    vals[n] = const_cast<std::uint16_t*>(&tag);
    ++n;
    for (int i = 0; i < 8; ++i) {
        regs[n] = UC_X86_REG_XMM0 + i;
        vals[n] = cpu_.xmm[i];
        ++n;
    }
    if (uc_reg_write_batch(uc_, regs, vals, n) != UC_ERR_OK) {
        // 同步失败 = 后端不可用（⛔ 不静默跑下去：guest 会带着错状态执行）
        stop = StopKind::Memory;
        stop_detail = "uc_reg_write_batch 失败（寄存器同步）";
        stop_ea = cpu_.eip;
    }
}

void UcEngine::syncFrom(X86& cpu_) {
    int regs[32];
    void* vals[32];
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        regs[n] = kGprs[i].uc;
        vals[n] = &cpu_.r[i];
        ++n;
    }
    std::uint32_t eip = 0;
    std::uint32_t eflags = 0;
    regs[n] = UC_X86_REG_EIP;
    vals[n] = &eip;
    ++n;
    regs[n] = UC_X86_REG_EFLAGS;
    vals[n] = &eflags;
    ++n;
    // FS 基址：装过就读回来跟踪（guest 自己改 fs 时能看见）；GS 基址在 32 位模式下读不回来。
    std::uint32_t fs_base = 0;
    if (fs_base_loaded_ != 0) {
        regs[n] = UC_X86_REG_FS_BASE;
        vals[n] = &fs_base;
        ++n;
    }
    for (int i = 0; i < 8; ++i) {
        regs[n] = UC_X86_REG_FP0 + i;
        vals[n] = cpu_.fpu.regs[i];
        ++n;
    }
    std::uint16_t fpcw = 0, fpsw = 0, fptag = 0;
    regs[n] = UC_X86_REG_FPCW;
    vals[n] = &fpcw;
    ++n;
    regs[n] = UC_X86_REG_FPSW;
    vals[n] = &fpsw;
    ++n;
    regs[n] = UC_X86_REG_FPTAG;
    vals[n] = &fptag;
    ++n;
    for (int i = 0; i < 8; ++i) {
        regs[n] = UC_X86_REG_XMM0 + i;
        vals[n] = cpu_.xmm[i];
        ++n;
    }
    if (uc_reg_read_batch(uc_, regs, vals, n) != UC_ERR_OK) {
        stop = StopKind::Memory;
        stop_detail = "uc_reg_read_batch 失败（寄存器同步）";
        return;
    }
    cpu_.eip = eip;
    cpu_.eflags = eflags;
    if (fs_base_loaded_ != 0) cpu_.fs_base = fs_base;
    cpu_.fpu.control = fpcw;
    cpu_.fpu.status = static_cast<std::uint16_t>(fpsw & ~0x3800u);
    cpu_.fpu.top = static_cast<std::uint8_t>((fpsw >> 11) & 7u);
    cpu_.fpu.tag = fptag;
}

StopReason mapUcError(uc_err e, std::string* detail) {
    switch (e) {
        case UC_ERR_OK:
            return StopReason::None;
        case UC_ERR_READ_UNMAPPED:
        case UC_ERR_WRITE_UNMAPPED:
        case UC_ERR_FETCH_UNMAPPED:
        case UC_ERR_READ_PROT:
        case UC_ERR_WRITE_PROT:
        case UC_ERR_FETCH_PROT:
            *detail = std::string("Unicorn 内存 fault：") + uc_strerror(e);
            return StopReason::MemoryFault;
        case UC_ERR_INSN_INVALID:
            *detail = "Unicorn：非法指令（UC_ERR_INSN_INVALID）";
            return StopReason::DecodeError;
        case UC_ERR_EXCEPTION:
            *detail = "Unicorn：guest CPU 异常（UC_ERR_EXCEPTION；本仓没有 SEH / 中断层）";
            return StopReason::Unimplemented;
        default:
            *detail = std::string("Unicorn 内部错误：") + uc_strerror(e);
            return StopReason::Unimplemented;
    }
}

}  // namespace

RunResult Machine::runUnicorn(const RunOptions& opts) {
    RunResult r;
    r.cpu = CpuBackend::Unicorn;
    r.insns_counted = true;  // 下面按 LIFT_UC_NO_COUNT_HOOK 覆盖
    if (!initialized_) {
        r.reason = StopReason::Unimplemented;
        r.detail = "Machine 未 init ⇒ 没有 guest 地址空间";
        return r;
    }
    shims_.setTrace(opts.trace_shims, opts.trace_out ? opts.trace_out : stdout);
    if (opts.real_time) clock_.setRealTime(true);
    if (opts.trace_inputs) dumpTraceInputs(stdout);

    UcEngine eng(mem_, shims_);
    eng.cpu = &cpu_;
    eng.clock = &clock_;
    eng.max_insns = opts.max_insns;
    eng.lst = &lst_;
    eng.trace_blocks = opts.trace_blocks;
    eng.trace_out = opts.trace_out ? opts.trace_out : stdout;
    // ★ 性能实验开关（只影响 Unicorn POC；默认两个都关 = 旧行为）：
    //   LIFT_UC_NO_COUNT_HOOK=1 ⇒ 不装全局逐指令 hook（insns/clock 不精确，仅测吞吐）
    //   LIFT_UC_INLINE_SHIM=1   ⇒ stub hook 内直接 invoke，不 stop/restart
    {
        const char* no_count = std::getenv("LIFT_UC_NO_COUNT_HOOK");
        const char* inline_shim = std::getenv("LIFT_UC_INLINE_SHIM");
        const bool disable_count = no_count != nullptr && no_count[0] != '\0' && no_count[0] != '0';
        if (disable_count && opts.max_insns != 0) {
            std::fprintf(stderr,
                         "[lift] LIFT_UC_NO_COUNT_HOOK 与 --max-insns 冲突：保留计数 hook 以保证 --max-insns\n");
        } else {
            eng.count_insns = !disable_count;
        }
        eng.inline_shim = inline_shim != nullptr && inline_shim[0] != '\0' && inline_shim[0] != '0';
    }
    r.insns_counted = eng.count_insns;
    std::string open_err;
    if (!eng.open(&open_err)) {
        r.reason = StopReason::Unimplemented;
        r.detail = open_err;
        r.ea = cpu_.eip;
        return r;
    }

    // 开跑前把所有"已经发了 stub_ea 的"页都挂上 hook（夹具常常先登记后 run）。
    eng.ensureShimHooks();
    // FS / GS 的段基址（TEB）：32 位 Unicorn 不支持直接写，走 GDT + 选择子。
    {
        std::string seg_err;
        if (!eng.installSegmentBases(&seg_err)) {
            r.reason = StopReason::Unimplemented;
            r.detail = seg_err;
            r.ea = cpu_.eip;
            return r;
        }
    }
    if (!mem_.committed(cpu_.eip, 1)) {
        // eip 落在未提交页：让 mem hook 报 MemoryFault（那里有 EA + 区域），⛔ 不静默换成"没跑"。
        std::fprintf(stderr, "[lift] Unicorn: eip=%08X 不在已提交页上 ⇒ 会以 memory-fault 停下\n", cpu_.eip);
    }

    cpu_.running = true;
    eng.last_commit_epoch = mem_.commitEpoch();
    const bool profile = std::getenv("LIFT_UC_PROFILE") != nullptr;
    // ★ 诊断开关（不是行为开关）：`LIFT_UC_PROFILE=1` 打印每个阶段的累计耗时。
    //   实测（boot 到 ExitProcess，658 次 shim）：`uc_emu_start` 往返 ≈ 33µs/次，是 shim 密集路径的主要成本；
    //   紧循环（无 shim）时它摊薄到可以忽略 —— 见报告里的吞吐数字。
    double t_hooks = 0, t_refresh = 0, t_prep = 0, t_emu = 0, t_sync_from = 0, t_invoke = 0;
    std::uint64_t slices = 0;
    auto clock_now = [] { return std::chrono::steady_clock::now(); };
    const auto wall_start = std::chrono::steady_clock::now();

    while (cpu_.running) {
        // ---- 墙钟上限：切片边界检查（粒度 ≤ kSliceInsns 条指令）----
        if (opts.time_limit_sec > 0.0) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - wall_start).count();
            if (elapsed >= opts.time_limit_sec) {
                r.reason = StopReason::TimeLimit;
                r.detail = "达到 --time-limit（墙钟上限；Unicorn 切片粒度 ≤ 1M 指令）";
                break;
            }
        }
        // ---- 嵌套 guest 回调的续延点（与解释器同一份逻辑）----
        if (cpu_.eip == GuestCallStack::kContinuationEa) {
            std::string call_err;
            if (!guest_calls_.resume(cpu_, &call_err)) {
                r.reason = StopReason::Unimplemented;
                r.detail = call_err;
                break;
            }
            continue;
        }
        // ---- winmm 定时器回调的返回点 ----
        if (cpu_.eip == kTimerCallbackReturnEa) {
            if (!cpu_.shim.timer_firing) {
                r.reason = StopReason::Unimplemented;
                r.detail = "定时器续延点被调到，但没有正在跑的定时器回调";
                break;
            }
            cpu_.esp() = cpu_.shim.timer_saved_esp;
            cpu_.eip = cpu_.shim.timer_resume_ea;
            cpu_.shim.timer_firing = false;
            continue;
        }
        if (opts.until_ea && cpu_.eip == opts.until_ea) {
            r.reason = StopReason::UntilEa;
            break;
        }
        if (opts.max_insns && cpu_.insns >= opts.max_insns) {
            r.reason = StopReason::MaxInsns;
            break;
        }
        // ---- 到期的 winmm 定时器（同一个 fireDueTimer；在**切片边界**触发）----
        if (!cpu_.shim.timer_firing && !cpu_.shim.timers.empty() && !cpu_.shim.callback.active) {
            if (fireDueTimer(r)) continue;
            if (r.reason != StopReason::None) break;
        }
        const auto tp0 = profile ? clock_now() : std::chrono::steady_clock::time_point{};
        // ---- 运行期新登记的 stub（COM 方法 / 动态导出）可能落在新页上 ----
        eng.ensureShimHooks();
        const auto tp1 = profile ? clock_now() : std::chrono::steady_clock::time_point{};
        // ---- guest 的提交位图变了（heap 增长 / VirtualFree）⇒ 把映射核对一遍 ----
        eng.refreshMappings();
        const auto tp2 = profile ? clock_now() : std::chrono::steady_clock::time_point{};
        // ---- FS/GS 基址变了（shim 改了 fs_base）⇒ 重新装段基址 ----
        {
            std::string seg_err;
            if (!eng.installSegmentBases(&seg_err)) {
                r.reason = StopReason::Unimplemented;
                r.detail = seg_err;
                break;
            }
        }

        eng.stop = StopKind::None;
        eng.stop_ea = 0;
        eng.stop_shim = nullptr;
        eng.stop_detail.clear();
        eng.slice_start_insns = cpu_.insns;
        eng.syncTo(cpu_);
        const auto tp3 = profile ? clock_now() : std::chrono::steady_clock::time_point{};

        const std::uint64_t begin = cpu_.eip;
        const uc_err e = uc_emu_start(eng.handle(), begin, opts.until_ea ? opts.until_ea : 0, 0, 0);
        const auto tp4 = profile ? clock_now() : std::chrono::steady_clock::time_point{};
        eng.syncFrom(cpu_);
        if (profile) {
            const auto tp5 = clock_now();
            t_hooks += std::chrono::duration<double>(tp1 - tp0).count();
            t_refresh += std::chrono::duration<double>(tp2 - tp1).count();
            t_prep += std::chrono::duration<double>(tp3 - tp2).count();
            t_emu += std::chrono::duration<double>(tp4 - tp3).count();
            t_sync_from += std::chrono::duration<double>(tp5 - tp4).count();
            ++slices;
        }

        bool leave = false;
        switch (eng.stop) {
            case StopKind::Shim: {
                cpu_.eip = eng.stop_ea;  // 桩页那条指令没有执行；invoke 会自己弹参并改 eip
                if (opts.stop_on_first_shim) {
                    r.reason = StopReason::FirstShim;
                    leave = true;
                    break;
                }
                ShimDef* def = const_cast<ShimDef*>(eng.stop_shim);
                if (!def) {  // 不该发生：stop_shim 与 stop 同时设
                    r.reason = StopReason::NoCode;
                    r.detail = "shim 停点没有对应的 def";
                    leave = true;
                    break;
                }
                const bool had_impl = def->fn != nullptr;
                const auto ti0 = profile ? clock_now() : std::chrono::steady_clock::time_point{};
                shims_.invoke(cpu_, *def);
                if (profile) t_invoke += std::chrono::duration<double>(clock_now() - ti0).count();
                if (!cpu_.running) {
                    if (!had_impl) {
                        r.reason = StopReason::Unimplemented;
                        r.detail = std::string("未实现的导入 ") + def->module + "!" + def->name;
                    } else {
                        r.reason = StopReason::Stopped;
                        r.detail = std::string("shim 主动结束运行：") + def->module + "!" + def->name;
                    }
                    leave = true;
                }
                break;
            }
            case StopKind::ShimStopped:
                if (eng.stop_shim && eng.stop_shim->fn == nullptr) {
                    r.reason = StopReason::Unimplemented;
                } else {
                    r.reason = StopReason::Stopped;
                }
                r.detail = eng.stop_detail;
                r.ea = eng.stop_ea;
                leave = true;
                break;
            case StopKind::Continuation:
                cpu_.eip = GuestCallStack::kContinuationEa;
                break;
            case StopKind::TimerReturn:
                cpu_.eip = kTimerCallbackReturnEa;
                break;
            case StopKind::Slice:
                break;
            case StopKind::MaxInsns:
                r.reason = StopReason::MaxInsns;
                r.detail = "达到 --max-insns（Unicorn 计数 hook；精确）";
                leave = true;
                break;
            case StopKind::Memory:
                r.reason = StopReason::MemoryFault;
                r.detail = eng.stop_detail;
                r.ea = eng.stop_ea;
                leave = true;
                break;
            case StopKind::None:
                break;
        }
        if (leave || r.reason != StopReason::None) break;
        // Shim / Continuation / TimerReturn / Slice ⇒ 回循环顶部（那里有 until / 续延 / 定时器的判断）
        if (eng.stop == StopKind::Shim || eng.stop == StopKind::Continuation ||
            eng.stop == StopKind::TimerReturn || eng.stop == StopKind::Slice) {
            continue;
        }

        // ---- 没有 hook、没有 until：可能是 until / hlt / eip 走到 0 / 引擎错误 ----
        if (opts.until_ea && cpu_.eip == opts.until_ea) {
            r.reason = StopReason::UntilEa;
            break;
        }
        if (e != UC_ERR_OK) {
            r.reason = mapUcError(e, &r.detail);
            r.ea = cpu_.eip;
            break;
        }
        if (!opts.until_ea && cpu_.eip == 0) {
            // Unicorn 的 until=0 语义：PC 走到 0 就按 HLT 停。这不是正常停机，也不许静默。
            r.reason = StopReason::NoCode;
            r.detail = "eip 走到 0（未提交地址；Unicorn 把它当 until=0 停机）";
            r.ea = 0;
            break;
        }
        if (cpu_.eip == begin) {
            // 一条都没走、也没有任何停点 ⇒ 引擎没有进展（⛔ 不能当"跑完了"）
            r.reason = StopReason::NoCode;
            r.detail = "Unicorn 切片没有任何进展（eip 不变，且没有 hook/until/错误）";
            r.ea = cpu_.eip;
            break;
        }
        // ★ 剩下的情形 = guest 自己 `hlt`（EXCP_HLT：Unicorn 返回 UC_ERR_OK，eip 已 +1）
        r.reason = StopReason::Stopped;
        r.detail = "guest hlt（Unicorn EXCP_HLT）";
        r.ea = cpu_.eip;
        break;
    }

    if (r.reason == StopReason::None && !cpu_.running) {
        r.reason = StopReason::Stopped;
        r.detail = "guest 停机（cpu.running=false）";
    }
    if (r.reason == StopReason::None) {
        // 到这一步还没有原因 = 逻辑漏了一条出口；⛔ 不许当成功
        r.reason = StopReason::Unimplemented;
        r.detail = "Unicorn 后端没有给出停机原因（内部错误）";
    }
    if (r.ea == 0) r.ea = cpu_.eip;
    r.insns = cpu_.insns;
    if (profile) {
        std::fprintf(stderr,
                     "[uc-profile] slices=%llu hooks=%.2fms refresh=%.2fms prep(seg+syncTo)=%.2fms "
                     "emu=%.2fms syncFrom=%.2fms invoke=%.2fms\n",
                     static_cast<unsigned long long>(slices), t_hooks * 1e3, t_refresh * 1e3, t_prep * 1e3,
                     t_emu * 1e3, t_sync_from * 1e3, t_invoke * 1e3);
    }

    // ★ 诊断（对齐解释器 decode-error 时的输出）：非正常停机时打 eip + 区域 + `.lst` 文本 + 寄存器。
    if (!r.ok()) {
        std::fprintf(stderr, "\n=== Unicorn 停机诊断 ===\n");
        std::fprintf(stderr, "  stop       : %s @ eip=%08X\n", stopReasonName(r.reason), r.ea);
        if (!r.detail.empty()) std::fprintf(stderr, "  detail     : %s\n", r.detail.c_str());
        const Region* reg = mem_.regionOf(r.ea);
        std::fprintf(stderr, "  region     : %s\n",
                     reg ? regionKindName(reg->kind) : "(未登记：不属于任何已声明的区域)");
        const char* text = lst_.textPtr(r.ea);
        std::fprintf(stderr, "  instr      : %s\n", (text && text[0]) ? text : "(不在 .lst 索引里)");
        dumpRegisters(cpu_, stderr);
        dumpGuestStack(cpu_, stderr, 16);
    }
    return r;
}

// ★ 段选择子（ES/CS/SS/DS/FS/GS）**不**逐切片同步：解释器在 flat 模型下跑，只有 FS/GS 的**基址**
//   参与地址计算，而基址是同步的。Unicorn 的段选择子写入会走 `cpu_x86_load_seg`（可能改段基址 /
//   边界），把解释器里恒为 0 的选择子写回去反而会改掉 Unicorn 自己的段缓存 —— 得不偿失。
//   ⚠ 若将来有 shim 读 `X86::seg[]`，这条要重新评估（当前没有）。

}  // namespace lift
