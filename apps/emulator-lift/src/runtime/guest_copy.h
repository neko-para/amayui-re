// emulator-lift · guest 字节的**方向保持**分块拷贝 / 图样填充（纯宿主 helper，没有 guest 语义）
//
// ★ 为什么单独一份（而不是各留一份）：有两处要"按方向分块"搬 guest 字节 ——
//   ① `native_crt.cpp` 的 `_memcpy` / `_memcpy_0` 重叠支（对应 guest `0x4E74F4` 的前向支与
//      `0x4E7670` 的后向支：`std; rep movsd; cld`）；
//   ② `exec_string.cpp` 的 `rep movs*` 快路径（REQ-01M75XK0XRTSWCZ4G8A5W2GW8）。
//   两处的**等价性论证是同一个**（下面这段），分成两份实现的话，哪天真漂出不同的分块下界，
//   两臂差分只会说"某处不一样"，不会说"哪份错了"。
//
// ★ 等价性（唯一一条前提，任何调用方都必须先满足）：块内被写到的源字节，要么在**本块内**
//   （本块已整体读过），要么在**已经处理过**的那一侧。满足它就与"逐字节搬"逐位等价：
//   · `copyForwardBytes`：**只对 `dst < src`** 成立 —— 写地址恒 ≤ 读地址；
//   · `copyBackwardBytes`：**只对 `dst > src`** 成立 —— 写地址恒 ≥ 读地址；
//   · `dst == src` 两支都不需要（内存不变）；不重叠时两支都对（顺序不影响结果）。
//   ⚠ 反过来用（`dst > src` 却走前向 / `dst < src` 却走后向）就是**涂抹**：4 B 分块会先读完 4 B
//   再写 4 B，与"读 1 写 1"的逐字节结果**不同**（位移 1..3 字节时必错）。
//   ★ 这条不是什么"惯例"：`rep movs` 的元素臂（`exec_string.cpp` 的 `once()`）本身也是
//   "整块读进寄存器再整块写回"，且相邻两轮之间读起点严格在已写区之外 ⇒ 它与逐字节同解；
//   所以"退回元素臂"与"分块搬"在方向正确时才是同一条语义（见 `runtime/rep_bulk.h` 的闸门）。
#pragma once

#include <cstdint>
#include <cstring>

namespace lift {

// ★ 分块下界：与 guest 自己的分界同值（**dword 计数 8** = 32 B）。来处与"为什么它不参与语义"
//   见 `native_overrides.h` 的 `_memcpy` 契约段（`0x4E7502 / 0x4E7506` 与 `0x4E7686 / 0x4E7689`）。
//   ⛔ 它只影响速度、不影响结果：小于它逐字节，大于它 4 B 一轮 —— 两者见文件头的等价性。
inline constexpr std::uint32_t kCopyChunkMin = 32;

// 4 字节**整体读进寄存器再整体写回**（⛔ 不是两次 `std::memcpy` 直接在 guest 的两个重叠区间上搬：
// 那正是 UB）。
inline void copyChunk4(std::uint8_t* d, const std::uint8_t* s) {
    std::uint32_t w = 0;
    std::memcpy(&w, s, 4);
    std::memcpy(d, &w, 4);
}

// 严格**前向**（低 → 高）：对应 guest `0x4E74F4` 那支（`rep movsd` + 字节尾表）。
// ★ 只有 `dst < src` 的重叠可以用它：写地址永远落后于**尚未读**的源地址。
inline void copyForwardBytes(std::uint8_t* d, const std::uint8_t* s, std::uint32_t n) {
    std::uint32_t i = 0;
    if (n >= kCopyChunkMin) {
        for (; n - i >= 4u; i += 4u) copyChunk4(d + i, s + i);  // `n - i` 不溢出（i <= n）
    }
    for (; i < n; ++i) d[i] = s[i];
}

// 严格**后向**（高 → 低）：对应 guest `0x4E7670` 那支（`std; rep movsd; cld`）。
// ★ 只有 `dst > src` 的重叠可以用它：写地址永远领先于**尚未读**的源地址。
inline void copyBackwardBytes(std::uint8_t* d, const std::uint8_t* s, std::uint32_t n) {
    std::uint32_t i = n;
    if (n >= kCopyChunkMin) {
        while (i >= 4u) {
            i -= 4u;
            copyChunk4(d + i, s + i);
        }
    }
    while (i > 0u) {
        --i;
        d[i] = s[i];
    }
}

// `stos*` 的**图样填充**：把 `pat`（元素宽度 1 / 2 / 4 字节）重复 n 字节。
// ★ 没有方向问题（只有目标区，不读源）⇒ DF 只影响"从哪一头开始填"，结果逐字节相同。
// ★ 两步，两边都不亏：
//   ① 先按元素写满前 `head = min(n, 256)` 字节（小 n 就只写这一遍；大 n 得到一个 256 B 的样板）；
//   ② 再**倍增**把 `[d, d+head)` 复制到紧后面 —— 两段相邻、不重叠 ⇒ `memcpy` 合法，
//      次数是 O(log n)，不是"每个元素一次宿主 store"（那正是本单要压掉的那笔税）。
//   ⚠ 反面教材（本实现的第一版）：无脑先拼 256 B 样板再复制 —— 对 `n=4`（1 个 dword）来说
//      那要 64 次 store，比逐元素臂还慢；"大 n 快、小 n 反而退步"是不许有的。
inline void fillPatternBytes(std::uint8_t* d, const std::uint8_t* pat, std::size_t pat_len,
                             std::uint32_t n) {
    if (pat_len == 0 || n == 0) return;
    if (pat_len == 1) {
        std::memset(d, pat[0], n);
        return;
    }
    const std::uint32_t head = (n < 256u) ? n : 256u;  // `pat_len` 整除 n ⇒ 也整除 head
    if (pat_len == 2) {
        const std::uint16_t v = static_cast<std::uint16_t>(pat[0] | (static_cast<std::uint16_t>(pat[1]) << 8));
        for (std::uint32_t i = 0; i < head; i += 2) std::memcpy(d + i, &v, 2);
    } else {
        std::uint32_t v = 0;
        std::memcpy(&v, pat, 4);
        for (std::uint32_t i = 0; i < head; i += 4) std::memcpy(d + i, &v, 4);
    }
    std::uint32_t written = head;
    while (written < n) {
        const std::uint32_t chunk = (n - written < written) ? (n - written) : written;
        std::memcpy(d + written, d, chunk);
        written += chunk;
    }
}

}  // namespace lift
