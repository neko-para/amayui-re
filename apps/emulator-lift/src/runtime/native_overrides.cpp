#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

bool frameCommitted(GuestMemory& mem, std::uint32_t esp, std::uint32_t arg_bytes) {
    return mem.committed(esp, static_cast<std::size_t>(4u + arg_bytes));
}

}  // namespace

const char* guestConvName(GuestConv c) {
    switch (c) {
        case GuestConv::Cdecl: return "cdecl";
        case GuestConv::Stdcall: return "stdcall";
        case GuestConv::Thiscall: return "thiscall";
    }
    return "?";
}

bool NativeOverrideTable::add(std::uint32_t ea, const std::string& name, GuestConv conv, std::uint32_t arg_bytes,
                              NativeFn fn, std::string* err) {
    if (count_ >= kMax) {
        if (err) *err = "native override 表已满（kMax=" + std::to_string(kMax) + "）";
        return false;
    }
    if (fn == nullptr) {
        if (err) *err = "native override 没有实现：" + name;
        return false;
    }
    if (ea == 0) {
        if (err) *err = "native override 的 EA 为 0：" + name;
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].ea == ea) {
            if (err) *err = "native override 重复 EA：" + name;
            return false;
        }
    }
    NativeOverride& e = entries_[count_++];
    e.ea = ea;
    e.name = name;
    e.conv = conv;
    e.arg_bytes = arg_bytes;
    e.fn = fn;
    e.hits = 0;
    e.fallbacks = 0;
    return true;
}

void NativeOverrideTable::clear() {
    for (std::size_t i = 0; i < count_; ++i) entries_[i] = NativeOverride{};
    count_ = 0;
}

NativeOverride* NativeOverrideTable::find(std::uint32_t ea) {
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].ea == ea) return &entries_[i];
    return nullptr;
}

const NativeOverride* NativeOverrideTable::find(std::uint32_t ea) const {
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].ea == ea) return &entries_[i];
    return nullptr;
}

bool NativeOverrideTable::invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why) {
    if (!ov.fn) {
        if (why) *why = "没有 native 实现";
        ++ov.fallbacks;
        return false;
    }
    const std::uint32_t esp = cpu.esp();
    if (!frameCommitted(mem, esp, ov.arg_bytes)) {
        if (why) *why = "返回地址 / 栈参数不在已提交页";
        ++ov.fallbacks;
        return false;
    }
    const std::uint32_t ret = mem.rd32(esp);
    std::uint32_t result = 0;
    if (!ov.fn(cpu, mem, &result)) {
        if (why) *why = "native 实现要求回退（参数越界 / safe mode）";
        ++ov.fallbacks;
        return false;
    }
    cpu.eax() = result;
    cpu.eip = ret;
    // cdecl：调用方清栈（我们只弹返回地址）；stdcall / thiscall：被调用方清栈。
    cpu.esp() = esp + 4u + (ov.conv == GuestConv::Cdecl ? 0u : ov.arg_bytes);
    ++ov.hits;
    return true;
}

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

// sub_436A80：S4IC / LZSS 解压（this 持有 4096 B 环形字典；a4[0]=输出大小，a4[2]=压缩大小）。
// 算法逐字照 .lst / .c：N=4096、F=18、初始 r=4078、length=low_nibble+3。
bool lzssDecode(const std::uint8_t* src, std::uint32_t comp_size, std::uint8_t* dst, std::uint32_t out_size,
                std::uint8_t ring[4096]) {
    if (src == nullptr || dst == nullptr || ring == nullptr) return false;
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

bool nativeLzss436A80(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t self = cpu.ecx();  // thiscall: this 在 ECX，栈上是 (Src, dst, info)
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

// ---------------------------------------------------------------------------------------
// 已知名字表：名字 → ABI + 实现（EA 由 `.lst` 解析，⛔ 不硬编码）
// ---------------------------------------------------------------------------------------

namespace {
const KnownNative kKnown[] = {
    {"_memcpy", GuestConv::Cdecl, 12, nativeMemcpy},
    {"_memcpy_0", GuestConv::Cdecl, 12, nativeMemcpy},
    {"_memset", GuestConv::Cdecl, 12, nativeMemset},
    {"_strlen", GuestConv::Cdecl, 4, nativeStrlen},
    // ★ 由 profile 选出的自定义 memcmp（`.lst` 里是 `sub_401000 proc near`）。
    //   语义见 `.lst` 401066 / `.c`：**只返回符号**（-1/0/+1），不是字节差。
    {"sub_401000", GuestConv::Cdecl, 12, nativeSignMemcmp},
    // ★ profile boot 第一热点：S4IC/LZSS 解压（.c: 43752；this=字典对象, a4[0]=out, a4[2]=comp）。
    {"sub_436A80", GuestConv::Thiscall, 12, nativeLzss436A80},
};
constexpr std::size_t kKnownCount = sizeof(kKnown) / sizeof(kKnown[0]);
}  // namespace

const KnownNative* findKnownNative(const std::string& name) {
    for (std::size_t i = 0; i < kKnownCount; ++i)
        if (name == kKnown[i].name) return &kKnown[i];
    return nullptr;
}

std::size_t knownNativeCount() { return kKnownCount; }

const KnownNative& knownNativeAt(std::size_t i) { return kKnown[i]; }

}  // namespace lift
