// emulator-lift · AOT 构建期库内建函数（v1：静态 CRT 的 `_memcpy` @ 0x004E74B0）
//
// ★ 本文件只做三件事：① **闸门判定**（只读，唯一真源）② **执行**（宿主 `memmove`）
//   ③ C 链接面的两个入口（生成物直接 `call`）。语义一行都不新发明 ——
//   `_memcpy` 的契约来自反汇编（cdecl / EAX=dst / 重叠等价 `memmove`），见 `lift_intrinsics.h` 头注。
//
// ★ 为什么 `do` 能直接信任闸门：生成代码**只在 `gate` 返回 true 时**才调它；`gate` 已经保证
//   `n <= 16`、`dst+n <= 4 GiB`、`src+n <= 4 GiB`、两段都 `committed`。`do` 里仍保留 `n == 0`
//   的早退（不调 `memmove`，避免 0 长度 + 垃圾指针的边界）与 `mem == nullptr` 的防御性早退
//   （那是装配错误，不是可执行状态；⛔ 不在这里造第二条闸门）。
#include "runtime/lift_intrinsics.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/lift_abi.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

std::atomic<bool> g_enabled{true};

// ★ 计数是**单线程**口径（AOT 主线程；`lift_memcpy_diag` 的 `_memcpy` 画像也在主线程）。
//   ⚠ 不用 `std::atomic`：每次闸门两次 relaxed RMW 在 2.3M 次上就是几千万周期的税，
//   而这份计数只服务"这次 A/B 到底走了几条快路径"的读数，不是并发安全的数据结构。
struct Counters {
    std::uint64_t fast = 0;
    std::uint64_t decline = 0;
    std::uint64_t disabled = 0;
};
Counters g_counters;

inline void noteCounters(IntrinsicWhy why) {
    switch (why) {
        case IntrinsicWhy::Ok: ++g_counters.fast; break;
        case IntrinsicWhy::Disabled: ++g_counters.disabled; break;
        default: ++g_counters.decline; break;
    }
}

}  // namespace

bool intrinsicsEnabled() { return g_enabled.load(std::memory_order_relaxed); }

void setIntrinsicsEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

IntrinsicCounters intrinsicCounters() {
    IntrinsicCounters out;
    out.fast = g_counters.fast;
    out.decline = g_counters.decline;
    out.disabled = g_counters.disabled;
    return out;
}

void resetIntrinsicCounters() { g_counters = Counters{}; }

MemcpyGatePlan memcpyGatePlan(const X86& cpu, std::uint32_t dst, std::uint32_t src,
                              std::uint32_t n) {
    MemcpyGatePlan plan;
    // ① 开关（`--no-intrinsics` = A/B 与诊断；缺省开）。
    if (!intrinsicsEnabled()) {
        plan.why = IntrinsicWhy::Disabled;
        return plan;
    }
    // ② `n == 0`：算成功，且**一个字节都不碰**（`memmove` 都不调；见 `do` 的早退）。
    if (n == 0) {
        plan.why = IntrinsicWhy::Ok;
        return plan;
    }
    // ③ 阈值（单点常量；实测依据见头注）。
    if (n > kMemcpyFastMaxBytes) {
        plan.why = IntrinsicWhy::SizeTooLarge;
        return plan;
    }
    // ④ 两条实际访问区间都不许越 4 GiB。★ 用 `uint64` 算：32 位 `dst + n` 在
    //    `dst = 0xFFFFFFF8, n = 16` 时会回绕成 8，闸门当场失效。
    plan.dst_end = static_cast<std::uint64_t>(dst) + n;
    plan.src_end = static_cast<std::uint64_t>(src) + n;
    if (plan.dst_end > GuestMemory::kSpace || plan.src_end > GuestMemory::kSpace) {
        plan.why = IntrinsicWhy::CrossBoundary;
        return plan;
    }
    // ⑤ 装配面：没有 guest 内存就没法搬（也读不了 committed）。
    if (cpu.mem == nullptr) {
        plan.why = IntrinsicWhy::NoMemory;
        return plan;
    }
    // ⑥ committed 预判 —— **safe / 非 safe 都查**。未提交 ⇒ 原函数体会 fault；
    //    替它搬等于把 fault 掩盖成"部分拷贝"（与 `repBulkPlan` 的 Uncommitted 同一条理由）。
    //    n <= 16 ⇒ 最多跨两页，`committed` 的最坏开销有界。
    if (!cpu.mem->committed(dst, static_cast<std::size_t>(n)) ||
        !cpu.mem->committed(src, static_cast<std::size_t>(n))) {
        plan.why = IntrinsicWhy::Uncommitted;
        return plan;
    }
    plan.why = IntrinsicWhy::Ok;
    return plan;
}

// 只给下面两个 `extern "C"` 入口用的内部转发（匿名 namespace 的名字不参与外部限定查找）。
void noteIntrinsicGateResult(IntrinsicWhy why) { noteCounters(why); }

}  // namespace lift

// ---------------------------------------------------------------------------------------
// C 链接面（生成物直接 call；`lift_abi.h` 是唯一声明点）
// ---------------------------------------------------------------------------------------
extern "C" bool lift_intrinsic_memcpy_gate(void* cpu_ptr, void* st, std::uint32_t dst,
                                           std::uint32_t src, std::uint32_t n) {
    (void)st;  // 闸门只看 `cpu.mem`；`%st` 是给夹具 / 未来内建函数留的 ABI 一致性参数
    if (cpu_ptr == nullptr) {
        std::fprintf(stderr, "[lift_intrinsic_memcpy_gate] 调用方传了空 cpu 指针\n");
        std::fflush(stderr);
        std::_Exit(4);
    }
    const lift::MemcpyGatePlan plan =
        lift::memcpyGatePlan(*static_cast<const lift::X86*>(cpu_ptr), dst, src, n);
    lift::noteIntrinsicGateResult(plan.why);
    return plan.why == lift::IntrinsicWhy::Ok;
}

extern "C" void lift_intrinsic_memcpy_do(void* cpu_ptr, std::uint32_t dst, std::uint32_t src,
                                         std::uint32_t n) {
    if (cpu_ptr == nullptr) {
        std::fprintf(stderr, "[lift_intrinsic_memcpy_do] 调用方传了空 cpu 指针\n");
        std::fflush(stderr);
        std::_Exit(4);
    }
    lift::X86& cpu = *static_cast<lift::X86*>(cpu_ptr);
    if (n == 0 || cpu.mem == nullptr) return;  // 见文件头：都不是"可执行但零长度"以外的状态

    // ★ SMC 记账（写之前；与 `repBulkExecute` / 逐元素臂同一份语义）。AOT 臂 `cache == nullptr`
    //   ⇒ no-op；夹具（真 BlockCache）会真正记账。
    lift::noteGuestWrite(cpu.cache, dst, n);

    // ★ 诊断面 `g_access`：先记整段（fault 归因），完成后记最后一次写（与逐元素臂一致）。
    lift::g_access = {dst, static_cast<std::uint8_t>(n > 255u ? 255u : n), 1};
    // ★ **`memmove` 而不是 `memcpy`**：`_memcpy` 在重叠时走 `std; rep movsl; cld`（逆向拷贝），
    //   语义与 `memmove` 相同。重叠安全是这里的硬要求（fixture 的 dst<src / dst>src 两组）。
    std::memmove(cpu.mem->host(dst), cpu.mem->host(src), static_cast<std::size_t>(n));
    const std::uint32_t last = static_cast<std::uint32_t>(static_cast<std::uint64_t>(dst) + n - 1u);
    lift::g_access = {last, 1, 1};
}
