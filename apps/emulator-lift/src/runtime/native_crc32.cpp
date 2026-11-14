// emulator-lift · native override：存档 CRC-32 的两个变体（`sub_436D00` / `sub_436D50`）
//
// ★ 需求单 REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ；契约（闸门 / 回退桶 / 已知边界）在 `native_overrides.h`
//   的 `CrcWhy` 段，纯核与"为什么不许硬编码多项式"在 `native_crc32.h`。
// ★ 逐字依据（`.lst`；EA 是锚，⛔ 不抄行号）：
//   · `0x436D00..0x436D4D`（`sub_436D00`）/ `0x436D50..0x436D9B`（`sub_436D50`）：都是
//     `__thiscall` + `retn 8` ⇒ `ECX = this`（= 表对象）、2 个栈参 = `(n, buf)`；
//   · 入口 `cmp dword ptr [esi+800h],0` / `jnz`：**表已建（== 1）就跳过初始化**，
//     否则 `call sub_436BD0`（建 `+0`）/ `call sub_436C60`（建 `+0x400`）+ `mov [esi+800h],1`；
//   · `mov edx,[ebp+arg_0]` / `or eax,0FFFFFFFFh` / `dec edx` / `js`：循环上界（见 `guestCrcIterations`）；
//   · 循环体见 `native_crc32.h`（反射 8 条 / 非反射 9 条）；收尾 `not eax` / `retn 8`。
// ★ 本项**零写**：不写 guest 内存、不分配、不调 guest 回调、不碰 x87 ⇒ 准入门 ② 天然成立；
//   唯一的"写"是框架按 ABI 写 `EAX`（`native_overrides.h` 的 `NativeFn` 契约）。
// ★ 惰性初始化那一支**一律回退**（`CrcWhy::TableNotBuilt`）：⛔ 不许"顺手替 guest 建表" ——
//   建表是 guest 的 `sub_436BD0` / `sub_436C60`（256×8 位、写 512 个 dword + 一个标志），
//   在 native 里复刻它 = 在 native 里重新引入多项式（正是本单禁止的那件事）；交给解释器重放
//   两步之后 `[this+0x800] == 1`，下一次调用自然接管。
#include "runtime/native_crc32.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/x86.h"

namespace lift {
namespace {

constexpr std::uint32_t kTableDwords = 256u;             // 每张表 = 256 个 dword
constexpr std::uint32_t kReflectedTableOffset = 0x400u;  // `[esi+edi*4+400h]`（反射变体）
constexpr std::uint32_t kInitFlagOffset = 0x800u;        // `[esi+800h]`（惰性初始化标志）
// 一次 `committed()` 覆盖两张表 + 标志那 4 字节：`[this, this+0x800)` 是设计稿的 G2，
// 再加上"标志本身"（`this+0x800` 的 4 字节）⇒ 0x804。★ 标志不在 G2 的跨度里，**必须**单独覆盖：
// 不覆盖就读它（safe mode 下裸 `rd32` 会 guestFault ⇒ `std::_Exit(3)`）。
constexpr std::uint32_t kObjectBytes = kInitFlagOffset + 4u;

// guest 的**循环次数**（`.lst` 0x436D29 `dec edx` / 0x436D2A `js`，`0x436D79/0x436D7A` 同形；
// 循环尾 0x436D44/0x436D45 与 0x436D92/0x436D93 的 `dec edx; jns` 是同一个计数器的另一半）。
//   edx = n；先 `dec`，再看 **SF**：
//     n > 0            ⇒ n 次
//     n == 0           ⇒ 0 次（`0-1 = -1` ⇒ `js` 直落收尾）
//     n < 0（INT_MIN 除外）⇒ 0 次（`n-1` 仍为负）
//     n == INT32_MIN   ⇒ **0x80000000 次**：`n-1` 回绕成 0x7FFFFFFF（SF=0）⇒ `js` **不跳** ⇒ 真进循环
//   ★ 最后一格与需求单正文"`n <= 0` ⇒ 循环 0 次"不一致 —— 本实现按**机器**走（按指令流），
//     并把那一格挡在 G4（跨度）：0x80000000 字节的 `[buf, buf+2^31)` 只要跨 4 GiB 或未提交就
//     回退解释器，绝不"返回 0"假装等价。来龙去脉见 `.tmp/lift-tickets/crc-report.md` 的
//     "我没做 / 与单不一致"。
constexpr std::int32_t kInt32Min = static_cast<std::int32_t>(0x80000000u);

std::uint32_t guestCrcIterations(std::int32_t n) {
    if (n > 0) return static_cast<std::uint32_t>(n);
    if (n == kInt32Min) return 0x80000000u;
    return 0u;
}

// 两个变体共用的实现。`table_offset` = 该变体查的那张表（`0x400` 反射 / `0` 非反射）。
bool crcOverride(X86& cpu, GuestMemory& mem, std::uint32_t* result, std::uint32_t table_offset, bool reflected) {
    // ⛔ 任何闸门之前不许有 guest 可见副作用：`*result` 只写 0，回退时框架不读它。
    *result = 0;
    auto decline = [&cpu](CrcWhy why) {
        if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        return false;
    };
    const std::uint32_t self = cpu.ecx();
    const std::int32_t n = static_cast<std::int32_t>(cpu.arg(0));
    const std::uint32_t buf_ea = cpu.arg(1);
    const std::uint32_t count = guestCrcIterations(n);

    // ── G2 + G1（两张表 + 标志，一次 committed 判完；顺序：先 committed 再读，safe mode 下
    //    未提交页的裸 `rd32` 会 guestFault）─────────────────────────────────────────────
    if (self == 0u) return decline(CrcWhy::BadThis);
    if (static_cast<std::uint64_t>(self) + kObjectBytes > GuestMemory::kSpace) return decline(CrcWhy::BadThis);
    if (!mem.committed(self, kObjectBytes)) return decline(CrcWhy::BadThis);
    // `[this+0x800] == 1` ⇔ 两张表已建好（guest 自己只写 1；别的值同样当"没建好"）。
    if (mem.rd32(self + kInitFlagOffset) != 1u) return decline(CrcWhy::TableNotBuilt);

    // ── G3 `count == 0` ⇒ **接管**并返回 0（不是回退）──────────────────────────────────────
    // 理由：guest 在那一支只做 `or eax,-1 / dec / js / not eax` ⇒ 值恒 0，且**一个字节都不读**
    // ⇒ 没有任何我们需要"让解释器去表态"的东西（fault / 时钟 / 写）。回退只会多付一次整函数重放。
    if (count == 0u) return true;  // *result 已是 0（= `~0xFFFFFFFF`）

    // ── G4 `count > 0` ⇒ `[buf, buf+count)` 不跨 4 GiB 且已提交 ───────────────────────────
    // 与 `nativeMemcpy` 的 `CrossBoundary` / `Uncommitted` 同一个理由：宿主平坦映射只有 4 GiB
    // （越界就是读出界），而 guest 在未提交页上的行为是 fault ⇒ 让解释器自己表态。
    // ★ 端点用 `uint64_t` 算（`(uint64_t)buf + count`）：`count` 可达 0x80000000，32 位会回绕。
    if (static_cast<std::uint64_t>(buf_ea) + count > GuestMemory::kSpace)
        return decline(CrcWhy::BufUnreadable);
    if (!mem.committed(buf_ea, count)) return decline(CrcWhy::BufUnreadable);

    // 表的**宿主视图**：先 `memcpy` 进局部（1 KiB）再算，而不是把 `mem.host()` 强转成
    // `const std::uint32_t*`。理由：强转要求 4 字节对齐且不违反严格别名，而 guest 自己的
    // `[esi+edi*4+400h]` 没有这两个前提（x86 的 dword 载入容忍非对齐）⇒ 拷本（`memcpy` 对有/无
    // 对齐都成立）比再加一条"对齐不满足就回退"的闸门更省事、也更不容易漏。
    // ★ 开销：每次命中一次 1 KiB `memcpy`；被校验的 payload 是 KB～MB 级（`.lst` 0x43758A /
    //   0x4375A6 / 0x437842 / 0x437858 四个调用点）⇒ 常数项可忽略。
    std::uint32_t table[kTableDwords] = {};
    std::memcpy(table, mem.host(self + table_offset), sizeof(table));
    *result = crc32Bytes(table, mem.host(buf_ea), count, reflected);
    return true;
}

}  // namespace

std::uint32_t crc32Bytes(const std::uint32_t* table, const std::uint8_t* buf, std::uint32_t n, bool reflected) {
    std::uint32_t crc = 0xFFFFFFFFu;  // `or eax, -1`
    if (reflected) {
        // `xor edi,ebx` / `shr eax,8` / `xor eax,[esi+edi*4+400h]`
        for (std::uint32_t k = 0; k < n; ++k) crc = table[(crc ^ buf[k]) & 0xFFu] ^ (crc >> 8);
    } else {
        // `shr edi,18h` / `xor edi,ebx` / `shl eax,8` / `xor eax,[esi+edi*4]`
        for (std::uint32_t k = 0; k < n; ++k) crc = table[((crc >> 24) ^ buf[k]) & 0xFFu] ^ (crc << 8);
    }
    return ~crc;  // `not eax`
}

// 反射变体：表在 `this+0x400`，`idx = (crc ^ b) & 0xFF`，`crc = T[idx] ^ (crc >> 8)`。
bool nativeCrc436D00(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    return crcOverride(cpu, mem, result, kReflectedTableOffset, /*reflected=*/true);
}

// 非反射变体：表在 `this+0`，`idx = (b ^ (crc >> 24)) & 0xFF`，`crc = T[idx] ^ (crc << 8)`。
bool nativeCrc436D50(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    return crcOverride(cpu, mem, result, /*table_offset=*/0u, /*reflected=*/false);
}

}  // namespace lift
