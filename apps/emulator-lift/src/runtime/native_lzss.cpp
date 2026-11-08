// emulator-lift · native override：S4IC / LZSS 解压（sub_436A80）
//
// ★ `this` 持有 4096 B 环形字典；a4[0]=输出大小 / a4[2]=压缩大小；算法逐字照 `.lst`（N=4096、F=18、r=4078）。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// sub_436A80：S4IC / LZSS 解压（this 持有 4096 B 环形字典；a4[0]=输出大小，a4[2]=压缩大小）。
// 算法逐字照 .lst / .c：N=4096、F=18、初始 r=4078、length=low_nibble+3。
bool lzssDecode(const std::uint8_t* src, std::uint32_t comp_size, std::uint8_t* dst, std::uint32_t out_size,
                std::uint8_t ring[4096]) {    if (src == nullptr || dst == nullptr || ring == nullptr) return false;
    if (out_size == 0) return true;
    if (comp_size == out_size) {
        std::memcpy(dst, src, out_size);
        return true;
    }
    if (comp_size == 0) return false;
    std::memset(ring, 0, 4096);
    const std::uint8_t* in = src;
    const std::uint8_t* in_end = src + comp_size;
    std::uint8_t* out = dst;
    std::uint8_t* out_end = dst + out_size;
    std::uint32_t bits = 0;
    std::uint32_t r = 4078;
    while (out < out_end) {
        bits >>= 1;
        if ((bits & 0x100u) == 0) {
            if (in >= in_end) break;
            bits = static_cast<std::uint32_t>(*in++) | 0xFF00u;
        }
        if ((bits & 1u) != 0) {
            if (in >= in_end) break;
            const std::uint8_t v = *in++;
            *out++ = v;
            ring[r] = v;
            r = (r + 1u) & 0xFFFu;
        } else {
            if (in + 1 >= in_end) break;
            const std::uint8_t lo = *in++;
            const std::uint8_t hi = *in++;
            const std::uint32_t offset = (static_cast<std::uint32_t>(hi & 0xF0u) << 4) | lo;
            const std::uint32_t count = (hi & 0x0Fu) + 3u;
            for (std::uint32_t i = 0; i < count && out < out_end; ++i) {
                const std::uint8_t v = ring[(offset + i) & 0xFFFu];
                *out++ = v;
                ring[r] = v;
                r = (r + 1u) & 0xFFFu;
            }
        }
    }
    return true;
}

bool nativeLzss436A80(X86& cpu, GuestMemory& mem, std::uint32_t* result) {    const std::uint32_t self = cpu.ecx();  // thiscall: this 在 ECX，栈上是 (Src, dst, info)
    const std::uint32_t src_ea = cpu.arg(0);
    const std::uint32_t dst_ea = cpu.arg(1);
    const std::uint32_t info_ea = cpu.arg(2);
    *result = 0;
    if (info_ea == 0 || dst_ea == 0) return false;
    if (!mem.committed(info_ea, 12)) return false;
    const std::uint32_t out_size = mem.rd32(info_ea + 0);
    const std::uint32_t comp_size = mem.rd32(info_ea + 8);
    if (out_size == 0) return true;
    if (static_cast<std::uint64_t>(dst_ea) + out_size > GuestMemory::kSpace) return false;
    if (static_cast<std::uint64_t>(src_ea) + (comp_size ? comp_size : out_size) > GuestMemory::kSpace) return false;
    std::uint8_t* dst = mem.host(dst_ea);
    const std::uint8_t* src = mem.host(src_ea);
    if (dst == nullptr || src == nullptr) return false;
    if (mem.safeMode() && (!mem.committed(dst_ea, out_size) || !mem.committed(src_ea, comp_size ? comp_size : out_size)))
        return false;
    std::uint8_t ring[4096] = {};
    if (!lzssDecode(src, comp_size, dst, out_size, ring)) return false;
    // 保持 guest 里 this+4 的字典可见（调用方若复用对象，语义不漂移）。
    if (self != 0 && static_cast<std::uint64_t>(self) + 4u + sizeof(ring) <= GuestMemory::kSpace &&
        mem.committed(self + 4u, sizeof(ring))) {
        std::uint8_t* ring_guest = mem.host(self + 4u);
        if (ring_guest != nullptr) std::memcpy(ring_guest, ring, sizeof(ring));
    }
    return true;
}

}  // namespace lift
