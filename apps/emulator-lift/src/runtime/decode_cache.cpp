// emulator-lift · EA → Insn 解码缓存（实现；契约 / 为什么**必须**自带字节校验见 `decode_cache.h`）
#include "runtime/decode_cache.h"

#include <cstring>

namespace lift {
namespace {

static_assert(kDecodeCacheSlots != 0 && (kDecodeCacheSlots & (kDecodeCacheSlots - 1)) == 0,
              "kDecodeCacheSlots 必须是 2 的幂（槽位 = 乘法散列的右移位数）");
static_assert(sizeof(DecodeCacheEntry) <= 160, "条目应保持紧凑（每线程 = 槽数 × sizeof(条目)）");

// 槽位：EA 的乘法散列取高位。★ 不用 `ea & mask`：指令 EA 低位模式很规整（4 字节对齐、集中
// 在少数几页里），直接取低位会在同一页的不同指令之间打架。
std::size_t slotOf(std::uint32_t ea) {
    constexpr unsigned kShift = 32u - 9u;  // 512 槽 ⇒ 取散列高 9 位
    static_assert(kDecodeCacheSlots == 512, "kShift 跟着 kDecodeCacheSlots 走");
    return static_cast<std::size_t>((ea * 2654435761u) >> kShift);
}

}  // namespace

bool decodeCached(DecodeCache& cache, const Decoder& dec, const GuestMemory& mem, std::uint32_t ea,
                  Insn* out, std::string* why) {
    DecodeCacheEntry& e = cache.slots[slotOf(ea)];
    // ---- 命中判定：EA 相同 + 15 字节窗口仍完整 + 那 `len` 个字节逐字节相同 ----
    // ★ 三条缺一不可。只比 EA 会把 SMC / 页重映射之后的**旧解码结果**当成答案（静默跑旧代码）；
    //   只比字节不要求窗口完整，则"当时只读到半条指令"的条目会被误判为仍然有效
    //   （`9B` 前缀合并的长度 / op 会随可读字节数变化，见头注）。
    if (e.len != 0 && e.ea == ea && mem.committed(ea, kDecodeCacheMaxLen)) {
        const std::uint8_t* live = mem.host(ea);
        bool same = true;
        for (std::uint8_t i = 0; i < e.len; ++i) {
            if (live[i] != e.bytes[i]) {
                same = false;
                break;
            }
        }
        if (same) {
            ++cache.hits;
            *out = e.insn;
            return true;
        }
        // 字节变了（guest 自我修改 / 页被重新映射）：⛔ 绝不返回旧条目，落到下面重解并覆盖。
        ++cache.invalidated;
    } else {
        ++cache.misses;
    }

    if (!dec.decode(mem, ea, out, why)) return false;  // 失败 ⇒ 不动缓存（下次照样重试解码）

    // ---- 只有"只由这 15 个字节决定"的结果才留成条目（见头注）----
    if (out->length == 0 || out->length > kDecodeCacheMaxLen || !mem.committed(ea, kDecodeCacheMaxLen)) {
        ++cache.uncached;
        return true;
    }
    const std::uint8_t* live = mem.host(ea);
    e.ea = ea;
    e.len = out->length;
    std::memcpy(e.bytes, live, out->length);
    e.insn = *out;
    return true;
}

}  // namespace lift
