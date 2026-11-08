// emulator-lift · native override：Map::findIndex（sub_429020）
//
// ★ 槽 28 B = MSVC `std::string`（SSO / len@+0x10 / cap@+0x14）；空槽 ⇒ -1；只读 guest 内存。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// ---------------------------------------------------------------------------------------
// `sub_429020`：Map::findIndex(hash, key)
//
// ★ 逐字依据（`.lst` 0x429020..0x4290EC；`functionAt` = sub_429020，thiscall retn 8）：
//   · 0x42902D `mov ecx,[ecx+18h]` ⇒ 表**指针**在 `[this+0x18]`（不是"表在 this+0x18"）；
//     容量在 `[this+8]`（0x4290A5 `div dword ptr [ecx+8]`）。
//   · 槽大小 28 B：0x429030 `lea eax,ds:0[ebx*8]; sub eax,ebx` = 7*index，再 `[ecx+eax*4]`。
//   · 空槽判据 `[slot+0x10] == 0`（0x429039 / 0x4290B6）⇒ **先**判空再算 strlen。
//   · 字符串数据：`cmp [slot+0x14],10h; jb` ⇒ cap<16 用**槽内**（SSO），否则 `[slot]` 是堆指针。
//   · 命中：先 `sub_401000(slot_data, key, min(len,keylen))`（0x429084），再
//     `cmp esi,edi; setnz` ⇒ **memcmp 为 0 且 len==keylen** 才算命中（0x429090..0x4290E4）；
//     否则 `index=(index+1)%capacity`（0x4290A2 `lea eax,[ebx+1]; div [ecx+8]`）。
//   · 返回：空槽 / 探测到空槽 ⇒ `-1`（0x429046 / 0x4290DA `or eax,-1`）；命中 ⇒ index（`mov eax,ebx`）。
// ★ 优化点（语义不变）：guest 在**每一次探测**都重算 `strlen(a3)`（0x429057 / 0x4290C6），
//   而 a3 在这段时间里不会被写 ⇒ 宿主只算一次。
// ---------------------------------------------------------------------------------------
namespace {

// 槽 28 B = MSVC std::string：0x00..0x0F = SSO 缓冲，+0x10 = len，+0x14 = cap。
constexpr std::uint32_t kStringSlotBytes = 28u;
constexpr std::uint32_t kStringLenOff = 0x10u;
constexpr std::uint32_t kStringCapOff = 0x14u;
constexpr std::uint32_t kStringSsoCap = 0x10u;

// guest 侧 strlen（不越过 4 GiB；safe mode 按页查提交位）。
// ⛔ 不用宿主 strlen：理由同 nativeStrlen（非 safe 模式可能越过 4 GiB 预留）。
bool guestCStringLength(GuestMemory& mem, std::uint32_t ea, std::uint32_t* out) {
    if (ea == 0) return false;
    const std::uint8_t* h = mem.host(ea);
    const std::uint64_t remaining = GuestMemory::kSpace - static_cast<std::uint64_t>(ea);
    std::uint32_t n = 0;
    if (mem.safeMode()) {
        while (n < remaining) {
            if ((n & GuestMemory::kPageMask) == 0u && !mem.committed(ea + n, 1u)) return false;
            if (h[n] == 0) {
                *out = n;
                return true;
            }
            ++n;
        }
        return false;
    }
    while (n < remaining) {
        if (h[n] == 0) {
            *out = n;
            return true;
        }
        ++n;
    }
    return false;
}

// 读一个槽的 (data, len, cap)。cap>=16 时 data 来自 `[slot]`；否则 data = 槽本身（SSO）。
// 返回 false = 越界 / 未提交（调用方回退解释器）。⛔ 不在这里编造 -1。
// ★ 提交检查只在 **safe mode** 下做（与 nativeMemcpy / nativeStrlen 同一口径）：非 safe 模式下
//   读未提交的槽 / 字符串会走平台 fault（与 guest 自己的访存**同一结局**），而不是静默读到 0。
//   ⇒ 本实现不替 guest 兜底"表里有未提交指针"的情况，也不因此编造结果。
bool readStringSlot(GuestMemory& mem, std::uint32_t slot, std::uint32_t* data, std::uint32_t* len,
                    std::uint32_t* cap) {
    if (static_cast<std::uint64_t>(slot) + kStringSlotBytes > GuestMemory::kSpace) return false;
    if (mem.safeMode() && !mem.committed(slot, kStringSlotBytes)) return false;
    *len = mem.rd32(slot + kStringLenOff);
    *cap = mem.rd32(slot + kStringCapOff);
    if (*cap >= kStringSsoCap) {
        const std::uint32_t p = mem.rd32(slot);
        if (p == 0) return false;
        *data = p;
    } else {
        *data = slot;
    }
    return true;
}

}  // namespace

bool nativeFindIndex429020(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t self = cpu.ecx();
    const std::uint32_t index0 = cpu.arg(0);
    const std::uint32_t key_ea = cpu.arg(1);
    *result = 0xFFFFFFFFu;
    if (self == 0 || key_ea == 0) return false;
    if (static_cast<std::uint64_t>(self) + 0x1Cu > GuestMemory::kSpace) return false;
    if (mem.safeMode() && (!mem.committed(self + 0x18u, 4u) || !mem.committed(self + 8u, 4u))) return false;
    const std::uint32_t table = mem.rd32(self + 0x18u);
    if (table == 0) return false;
    // ① 容量：guest 只在**探测下一次**时才 div；这里提前读 —— 0 / index 越界 ⇒ 回退解释器。
    //    ★ 回退（而不是编造 -1）保证语义：guest 自己会按 `.lst` 的顺序给出同样的结果。
    const std::uint32_t capacity = mem.rd32(self + 8u);
    if (capacity == 0 || index0 >= capacity) return false;
    // ② 判**第一个槽**是否空（guest 0x429039 在 strlen 之前就这么做）：
    //    空槽 ⇒ -1，且**根本不读 key**（key 指针非法时也必须返回 -1 而不是回退）。
    {
        const std::uint64_t slot64 = static_cast<std::uint64_t>(table) + kStringSlotBytes * index0;
        if (slot64 + kStringSlotBytes > GuestMemory::kSpace) return false;
        const std::uint32_t slot = static_cast<std::uint32_t>(slot64);
        if (mem.safeMode() && !mem.committed(slot, kStringSlotBytes)) return false;
        if (mem.rd32(slot + kStringLenOff) == 0u) {
            *result = 0xFFFFFFFFu;
            return true;
        }
    }
    // ③ key 长度只算一次（guest 每个探测都重算）
    std::uint32_t key_len = 0;
    if (!guestCStringLength(mem, key_ea, &key_len)) return false;
    const std::uint8_t* key = mem.host(key_ea);
    std::uint32_t index = index0;
    for (std::uint32_t probe = 0; probe < capacity; ++probe) {
        std::uint32_t data = 0;
        std::uint32_t len = 0;
        std::uint32_t cap = 0;
        const std::uint64_t slot64 = static_cast<std::uint64_t>(table) + kStringSlotBytes * index;
        if (slot64 + kStringSlotBytes > GuestMemory::kSpace) return false;
        if (!readStringSlot(mem, static_cast<std::uint32_t>(slot64), &data, &len, &cap)) return false;
        (void)cap;
        // 空槽 ⇒ -1（探测到这里就结束）
        if (len == 0) {
            *result = 0xFFFFFFFFu;
            return true;
        }
        // ★ 逐字照 guest：先比 min(len,keylen) 字节（**长度不等也要比** —— guest 就是这么做的，
        //   这样 slot 数据指针非法时的 fault 行为与 guest 一致），再比长度。
        const std::uint32_t n = len < key_len ? len : key_len;
        bool equal = true;
        if (n != 0) {
            if (static_cast<std::uint64_t>(data) + n > GuestMemory::kSpace) return false;
            if (mem.safeMode() && !mem.committed(data, n)) return false;
            equal = std::memcmp(mem.host(data), key, n) == 0;
        }
        if (equal && len == key_len) {
            *result = index;
            return true;
        }
        index = (index + 1u) % capacity;
    }
    // 整张表没有空槽 ⇒ guest 会**无限循环**。⛔ 不编造 -1；回退解释器（让 guest 自己表现）。
    return false;
}

}  // namespace lift
