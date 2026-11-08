// emulator-lift · native override：CRT 叶子（memcpy / memset / sign-memcmp / strlen）
//
// ★ 只经 `GuestMemory` 访存、guest 指针保持 32 位；不重叠的 `_memcpy` 才接手，重叠时回退解释器。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// ---------------------------------------------------------------------------------------
// CRT 叶子实现（全部只经 GuestMemory 读写 guest；guest 指针保持 32 位）
// ---------------------------------------------------------------------------------------

bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t dst = cpu.arg(0);
    const std::uint32_t src = cpu.arg(1);
    const std::uint32_t n = cpu.arg(2);
    *result = dst;
    if (n == 0) return true;
    if (static_cast<std::uint64_t>(dst) + n > GuestMemory::kSpace ||
        static_cast<std::uint64_t>(src) + n > GuestMemory::kSpace)
        return false;
    // ★ `_memcpy` 的契约是不重叠；重叠时**回退解释器**，让 guest 的 `rep movsb` 给出它自己的
    //   （确定性的）结果 —— 宿主 `std::memcpy` 对重叠是 UB，不能拿它冒充。
    const std::uint64_t d64 = dst;
    const std::uint64_t s64 = src;
    const std::uint64_t n64 = n;
    if (d64 < s64 + n64 && s64 < d64 + n64) return false;
    if (mem.safeMode() && (!mem.committed(dst, n) || !mem.committed(src, n))) return false;
    std::memcpy(mem.host(dst), mem.host(src), n);
    return true;
}

bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t dst = cpu.arg(0);
    const std::uint32_t value = cpu.arg(1) & 0xFFu;
    const std::uint32_t n = cpu.arg(2);
    *result = dst;
    if (n == 0) return true;
    if (static_cast<std::uint64_t>(dst) + n > GuestMemory::kSpace) return false;
    if (mem.safeMode() && !mem.committed(dst, n)) return false;
    std::memset(mem.host(dst), static_cast<int>(value), n);
    return true;
}

bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t a = cpu.arg(0);
    const std::uint32_t b = cpu.arg(1);
    const std::uint32_t n = cpu.arg(2);
    *result = 0;
    if (n == 0) return true;
    if (static_cast<std::uint64_t>(a) + n > GuestMemory::kSpace ||
        static_cast<std::uint64_t>(b) + n > GuestMemory::kSpace)
        return false;
    if (mem.safeMode() && (!mem.committed(a, n) || !mem.committed(b, n))) return false;
    const std::uint8_t* pa = mem.host(a);
    const std::uint8_t* pb = mem.host(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            // ★ 与 `.lst` 的 `sar eax,1Fh; or eax,1` 一致：只返回符号（-1 / +1），不是字节差。
            *result = (pa[i] < pb[i]) ? 0xFFFFFFFFu : 1u;
            return true;
        }
    }
    return true;
}

bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t p = cpu.arg(0);
    // ★ 不直接用宿主 `strlen`：非 safe mode 下它可能越过 4 GiB 预留（宿主 OOB，fault 会归错）。
    //   逐字节走 `rd8`（safe mode 的提交检查仍在），并用 64 位地址挡住边界。
    std::uint64_t addr = p;
    while (addr < GuestMemory::kSpace) {
        if (mem.rd8(static_cast<std::uint32_t>(addr)) == 0) {
            *result = static_cast<std::uint32_t>(addr - p);
            return true;
        }
        ++addr;
    }
    return false;
}

}  // namespace lift
