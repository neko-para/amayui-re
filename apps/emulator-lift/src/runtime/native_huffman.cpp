// emulator-lift · native override 实现：Huffman 子树的两片叶子
// （`sub_4E1570` / `sub_4E1950`；契约、逐字依据与已知边界全部在 `native_huffman.h` 的头注）
//
// ★ 本文件的结构：
//   ① 字段偏移 / 表地址（EA 锚）与两个"位读原语"的逐字复刻（`huffLook` / `huffAdv`）；
//   ② `sub_4E1510` 的 bitrev32；③ 码本 / 位读的**准入**；④ `sub_4E1570` 的整棵 decode；
//   ⑤ `sub_4E1950` 的 decode + 分量累加。
// ★ 零写约定：准入与 decode 全程只碰**局部副本**（`HuffReader` / `HuffCodebook` / 局部 idx），
//   对 guest 的唯一写发生在 `adv` 的写回与 `dst` 的累加 —— 两者都在所有闸门之后。
//   ★ `sub_4E1950` 是**两遍**的（先解出全部 entry，再累加）：`dst` 是就地 `+=`，
//   中途回退会被解释器重放成"加两次"⇒ 结构上不允许多次写。
#include "runtime/native_huffman.h"

#include <cmath>
#include <cstring>

#include "runtime/fpu_bridge.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// ---------------------------------------------------------------------------------------
// ① 字段偏移（EA 锚见头注）与位读原语
// ---------------------------------------------------------------------------------------

// `oggpack_buffer`（libogg 的五个 dword；`look` / `adv` 读写的就是它们）
constexpr std::uint32_t kBrEndbyte = 0x00;  // `0x513238` / `0x5132EB`：已消费字节
constexpr std::uint32_t kBrEndbit = 0x04;   // `0x51323B` / `0x5132F0`：字节内位偏移（0..7）
constexpr std::uint32_t kBrBuffer = 0x08;   // 位流起点（本实现用它算窗口；`look` 不读）
constexpr std::uint32_t kBrPtr = 0x0C;      // `0x51325C` / `0x5132E8`：当前字节指针
constexpr std::uint32_t kBrStorage = 0x10;  // `0x513232`：位流总字节
constexpr std::uint32_t kBrBytes = 0x14;

// 码本（`codebook`）——本实现要读的字段
constexpr std::uint32_t kCbDim = 0x00;          // `0x4E195A` / `0x4E1982`
constexpr std::uint32_t kCbUsed = 0x08;         // `0x4E15A0`
constexpr std::uint32_t kCbValueList = 0x10;    // `0x4E1989`
constexpr std::uint32_t kCbDecIndex = 0x14;     // `0x4E1626`
constexpr std::uint32_t kCbCodeLengths = 0x1C;  // `0x4E15B8` / `0x4E165B`
constexpr std::uint32_t kCbFirstTable = 0x20;   // `0x4E1596`
constexpr std::uint32_t kCbFirstTabLen = 0x24;  // `0x4E157F`
constexpr std::uint32_t kCbMaxLength = 0x28;    // `0x4E1582`
constexpr std::uint32_t kCbBytes = 0x2C;        // 本实现读的最大偏移 + 4

// `int dword_55BED8[34]`（`mask[n] = (1<<n)-1`，`0..32` 共 33 项有效；`[32] = 0xFFFFFFFF`）。
// ★ 从 **guest 内存**读，不写死常数：它是 guest 的只读表，写死了就"不读同一份"了。
constexpr std::uint32_t kMaskTableEa = 0x0055BED8u;
constexpr std::uint32_t kMaskTableWords = 33;

// 闸门的上界（**不是**语义，只是"别让 committed() 去扫上百万页"的护栏）：
constexpr std::int32_t kMaxUsed = 1 << 20;
constexpr std::int32_t kMaxFirstTabLen = 16;
// `sub_4E1950` 一次调用最多先攒多少个 entry（两遍结构的栈上限；超了 ⇒ 回退解释器，零写）。
constexpr std::int32_t kMaxVectorEntries = 256;

std::uint32_t loadU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
void storeU32(std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
// `movsx byte`（`0x4E15BB` / `0x4E165E`）：**带符号**取码长（≥0x80 的字节是负数）
std::int32_t loadI8(const std::uint8_t* p) { return static_cast<std::int32_t>(*reinterpret_cast<const std::int8_t*>(p)); }

// `oggpack_buffer` 的**局部快照**（见头注 ③：状态在 guest 内存，这里是一次调用内的副本；
// `adv` 的写回落在 guest 同一份上）。
struct HuffReader {
    std::uint32_t endbyte = 0;
    std::uint32_t endbit = 0;
    std::uint32_t buffer = 0;
    std::uint32_t ptr = 0;
    std::uint32_t storage = 0;
};

// `sub_513220` 的逐字复刻：`bits` ∈ [0,32]（准入保证），`mask` = `mask[bits]`（`0x55BED8`）。
// 返回 `-1` = 位不够（guest 的判据就是 `test eax,eax / js`）。
// ★ 全程只读；字节窗口在准入时已按 `[buffer, buffer+storage+5)` 判过提交 ⇒ 用 `host()` 裸指针。
std::int32_t huffLook(const GuestMemory& mem, const HuffReader& r, std::int32_t bits, std::uint32_t mask) {
    const std::int32_t endbyte = static_cast<std::int32_t>(r.endbyte);
    const std::int32_t storage = static_cast<std::int32_t>(r.storage);
    const std::int32_t endbit = static_cast<std::int32_t>(r.endbit);
    const std::int32_t edx = bits + endbit;  // `0x513241 add edx,edi`
    // `0x513243 cmp ebx,esi / jl 0x51325C`：`(endbyte+4) < storage` ⇒ 快路（直接去读）
    if (!(endbyte + 4 < storage)) {
        // `0x513247..0x513250`：`endbyte*8 + bits + endbit > storage*8` ⇒ 位不够
        const std::int32_t limit = storage * 8;
        const std::int32_t need = edx + endbyte * 8;
        if (need > limit) return -1;
    }
    const std::uint8_t* p = mem.host(r.ptr);  // 窗口已提交（见准入）
    std::uint32_t v = static_cast<std::uint32_t>(p[0]) >> static_cast<std::uint32_t>(endbit);
    // `0x51326C..0x5132B9`：逐段按 `edx` 的门槛取第 2..5 个字节
    if (edx > 8) v |= static_cast<std::uint32_t>(p[1]) << static_cast<std::uint32_t>(8 - endbit);
    if (edx > 0x10) v |= static_cast<std::uint32_t>(p[2]) << static_cast<std::uint32_t>(16 - endbit);
    if (edx > 0x18) v |= static_cast<std::uint32_t>(p[3]) << static_cast<std::uint32_t>(24 - endbit);
    // `0x5132A8 test edi,edi / jz 0x5132BB`：第 5 个字节只在 bitoff != 0 时才取
    if (edx > 0x20 && endbit != 0) v |= static_cast<std::uint32_t>(p[4]) << static_cast<std::uint32_t>(32 - endbit);
    v &= mask;  // `0x5132BB and eax, mask[bits]`
    return static_cast<std::int32_t>(v);
}

// `sub_5132D0` 的逐字复刻：只改**局部副本**（写回由调用方在最后做一次）。
// `n = (bits+endbit)/8`（`0x5132DF cdq / and edx,7 / add / sar 3` = C 的**向零截断**除法），
// `endbit = (bits+endbit)&7`（`0x5132ED and esi,7`）。
void huffAdv(HuffReader* r, std::int32_t bits) {
    const std::int32_t s = static_cast<std::int32_t>(r->endbit + static_cast<std::uint32_t>(bits));
    const std::int32_t n = s / 8;
    r->endbyte += static_cast<std::uint32_t>(n);
    r->ptr += static_cast<std::uint32_t>(n);
    r->endbit = static_cast<std::uint32_t>(s) & 7u;
}

// ---------------------------------------------------------------------------------------
// ② `sub_4E1510`：32 位位反转（`rol 16` + 四段 shift-xor-mask，逐条照抄）
// ---------------------------------------------------------------------------------------
std::uint32_t bitRev32(std::uint32_t v) {
    v = (v << 16) | (v >> 16);  // `0x4E1510 rol ecx,10h`
    std::uint32_t t = ((v >> 8) ^ (v << 8)) & 0x00FF00FFu;
    v = (v << 8) ^ t;  // `0x4E1517..0x4E1527`
    t = ((v >> 4) ^ (v << 4)) & 0x0F0F0F0Fu;
    v = (v << 4) ^ t;  // `0x4E1529..0x4E153E`
    t = ((v >> 2) ^ (v << 2)) & 0x33333333u;
    v = (v << 2) ^ t;  // `0x4E1540..0x4E1558`
    t = ((v >> 1) ^ (v << 1)) & 0x55555555u;
    return (v << 1) ^ t;  // `0x4E155A..0x4E156A`
}

// ---------------------------------------------------------------------------------------
// ③ 准入：码本 / 位读
// ---------------------------------------------------------------------------------------

// 一次调用内反复用的码本视图（全部是**已校验过**的 guest 指针/标量）。
struct HuffCodebook {
    std::int32_t used = 0;           // `[a1+08]`
    std::int32_t firsttablen = 0;    // `[a1+24]`
    std::int32_t maxlength = 0;      // `[a1+28]`
    std::uint32_t first_table = 0;   // `[a1+20]`
    std::uint32_t code_lengths = 0;  // `[a1+1C]`
    std::uint32_t dec_index = 0;     // `[a1+14]`（used>1 时必非 0）
};

bool spanOk(std::uint32_t ea, std::uint64_t bytes) {
    return static_cast<std::uint64_t>(ea) + bytes <= GuestMemory::kSpace;
}

// 码本准入（只读）。`why` 只用于诊断分桶。
bool readCodebook(const GuestMemory& mem, std::uint32_t a1, HuffCodebook* cb, HuffWhy* why) {
    *why = HuffWhy::BadCodebook;
    if (a1 == 0u || !spanOk(a1, kCbBytes) || !mem.committed(a1, kCbBytes)) return false;
    const std::int32_t used = static_cast<std::int32_t>(mem.rd32(a1 + kCbUsed));
    if (used < 1 || used > kMaxUsed) return false;
    const std::int32_t firsttablen = static_cast<std::int32_t>(mem.rd32(a1 + kCbFirstTabLen));
    const std::int32_t maxlength = static_cast<std::int32_t>(mem.rd32(a1 + kCbMaxLength));
    const std::uint32_t first_table = mem.rd32(a1 + kCbFirstTable);
    if (firsttablen < 0 || firsttablen > kMaxFirstTabLen || maxlength < 0 || maxlength > 32 || first_table == 0u) {
        *why = HuffWhy::BadFirstTable;
        return false;
    }
    const std::uint64_t first_table_bytes = 4ull << static_cast<unsigned>(firsttablen);
    if (!spanOk(first_table, first_table_bytes) ||
        !mem.committed(first_table, static_cast<std::size_t>(first_table_bytes))) {
        *why = HuffWhy::BadFirstTable;
        return false;
    }
    const std::uint32_t code_lengths = mem.rd32(a1 + kCbCodeLengths);
    if (code_lengths == 0u || !spanOk(code_lengths, static_cast<std::uint64_t>(used)) ||
        !mem.committed(code_lengths, static_cast<std::size_t>(used))) {
        *why = HuffWhy::BadCodeLengths;
        return false;
    }
    // used == 1 时二分区间恒 <= 1（`span <= 1`、`lo >= 0` ⇒ `d <= 1`）⇒ 不读 dec_index，
    // 允许它为 0（单符号码本）；used > 1 时它必须非 0 且整张表已提交。
    const std::uint32_t dec_index = mem.rd32(a1 + kCbDecIndex);
    if (used > 1) {
        if (dec_index == 0u || !spanOk(dec_index, 4ull * static_cast<std::uint64_t>(used)) ||
            !mem.committed(dec_index, 4u * static_cast<std::size_t>(used))) {
            *why = HuffWhy::BadDecIndex;
            return false;
        }
    }
    if (!spanOk(kMaskTableEa, 4ull * kMaskTableWords) || !mem.committed(kMaskTableEa, 4u * kMaskTableWords))
        return false;  // 掩码表不在已提交页 ⇒ 归 BadCodebook
    cb->used = used;
    cb->firsttablen = firsttablen;
    cb->maxlength = maxlength;
    cb->first_table = first_table;
    cb->code_lengths = code_lengths;
    cb->dec_index = dec_index;
    *why = HuffWhy::Ok;
    return true;
}

// 位读准入：五个字段可读 + `ptr == buffer + endbyte` + `endbit ∈ [0,7]` +
// **一次**判整个位流窗口 `[buffer, buffer+storage+5)`（`look` 最坏读 5 字节）。
// ★ 为什么整窗一次判：`look` 只在 `storage` 内部挪 `ptr`（位预算 `0x513250` 保证
//   `endbyte <= storage`，`adv` 与 `ptr`/`endbyte` 同步推进）⇒ 窗口一旦提交，后续每一次
//   `look` 都落在里面 ⇒ 循环中途不会再有"要回退"的点（`sub_4E1950` 零写要求这条）。
bool readReader(const GuestMemory& mem, std::uint32_t a2, HuffReader* r, HuffWhy* why) {
    *why = HuffWhy::BadReader;
    if (a2 == 0u || !spanOk(a2, kBrBytes) || !mem.committed(a2, kBrBytes)) return false;
    const std::uint32_t endbyte = mem.rd32(a2 + kBrEndbyte);
    const std::uint32_t endbit = mem.rd32(a2 + kBrEndbit);
    const std::uint32_t buffer = mem.rd32(a2 + kBrBuffer);
    const std::uint32_t ptr = mem.rd32(a2 + kBrPtr);
    const std::uint32_t storage = mem.rd32(a2 + kBrStorage);
    if (endbit > 7u) return false;
    if (storage > 0x0FFFFFFFu) return false;  // 见头注：让 `storage*8` 落在正数区
    if (endbyte > storage) return false;
    if (buffer == 0u || ptr != buffer + endbyte) return false;  // libogg 的不变量
    const std::uint64_t window = static_cast<std::uint64_t>(storage) + 5u;
    if (!spanOk(buffer, window)) return false;
    if (!mem.committed(buffer, static_cast<std::size_t>(window))) {
        *why = HuffWhy::BadReaderWindow;
        return false;
    }
    r->endbyte = endbyte;
    r->endbit = endbit;
    r->buffer = buffer;
    r->ptr = ptr;
    r->storage = storage;
    *why = HuffWhy::Ok;
    return true;
}

std::uint32_t maskWord(const GuestMemory& mem, std::int32_t bits) {
    return loadU32(mem.host(kMaskTableEa) + 4u * static_cast<std::uint32_t>(bits));
}

// ---------------------------------------------------------------------------------------
// ④ `sub_4E1570` 的整棵 decode：look → 首表 → 二分 → adv
//
// ★ 返回 -1 是**语义**（guest 也返回 -1），不是"回退"：只有 `false` 才是回退（`why` 落桶）。
// ★ `*r` 是局部副本：写回 guest 由调用方在**最后**决定（零写保证）。
// ---------------------------------------------------------------------------------------
bool huffDecodeOnce(const GuestMemory& mem, const HuffCodebook& cb, HuffReader* r, std::int32_t* out,
                    HuffWhy* why) {
    *why = HuffWhy::Ok;
    // 第一次 look：位宽 = firsttablen（`0x4E1585 push ecx` / `0x4E158A call sub_513220`）
    const std::int32_t read1 = huffLook(mem, *r, cb.firsttablen, maskWord(mem, cb.firsttablen));
    std::int32_t lo = 0;          // esi
    std::int32_t span = cb.used;  // edi
    if (read1 >= 0) {             // `0x4E1592 test eax,eax / js 0x4E15D4`
        const std::uint32_t entry = loadU32(mem.host(cb.first_table) + 4u * static_cast<std::uint32_t>(read1));
        if (static_cast<std::int32_t>(entry) >= 0) {  // `0x4E159C test ebx,ebx / jns 0x4E15B8`
            // ★ entry == 0：guest 会读 `codelengths[-1]`（`0x4E15BB`）—— 不复刻越界读，回退。
            if (entry == 0u) {
                *why = HuffWhy::EntryZero;
                return false;
            }
            if (entry > static_cast<std::uint32_t>(cb.used)) {
                *why = HuffWhy::EntryOutOfRange;
                return false;
            }
            // 直通：adv(`codelengths[entry-1]`) 并返回 `entry-1`
            //（`0x4E15BB movsx` / `0x4E15C2 call sub_5132D0` / `0x4E15CC lea eax,[ebx-1]`）
            // ★ 地址算术按 **guest 的 32 位宽度**写（`code_lengths + (entry-1)` 会回绕），
            //   这样即使 `entry == 0` 漏过闸门也是"读 codelengths[-1]"而不是宿主指针越界
            //   （与解释器臂同解；见 `tests/native_override_assets.cpp` 的 `huff/entry-zero`）。
            const std::int32_t len = loadI8(mem.host(cb.code_lengths + (entry - 1u)));
            if (len < 0) {  // 负码长只有损坏数据才有；`adv` 会倒退 ⇒ 交解释器（零写）
                *why = HuffWhy::BadCodeLengths;
                return false;
            }
            huffAdv(r, len);
            *out = static_cast<std::int32_t>(entry - 1u);
            return true;
        }
        // `0x4E15A5 sar esi,0Fh`（**算术**右移）+ `and 7FFFh` ⇒ 二分下界
        lo = (static_cast<std::int32_t>(entry) >> 15) & 0x7FFF;
        // `0x4E15A8 and ebx,7FFFh` / `0x4E15B4 sub edi,ebx` ⇒ 上界 = used_entries - 低 15 位
        span = cb.used - (static_cast<std::int32_t>(entry) & 0x7FFF);
    }
    // 第二次 look：位宽 = maxlength（`0x4E15D9` 起）
    std::int32_t bits = cb.maxlength;  // ebx / var_4
    std::int32_t read = huffLook(mem, *r, bits, maskWord(mem, bits));
    if (read < 0) {
        // `0x4E15F0` 的降位重试：每次 `dec` 一位；降到 1 仍失败 ⇒ 返回 -1 且**不 adv**
        for (;;) {
            if (bits <= 1) {  // `0x4E160C` / `0x4E1611 js 0x4E1689`
                *out = -1;
                return true;
            }
            --bits;
            read = huffLook(mem, *r, bits, maskWord(mem, bits));
            if (read >= 0) break;  // `0x4E1603 test eax,eax / js loc_4E15F0`
        }
    }
    // ★ 二分区间必须落在 `[0, used_entries)` 里，否则上面那两行就会越界读
    //   （`codelengths[lo]` / `dec_index[mid]`）。合法码本恒满足；不满足 ⇒ 交解释器（零写）。
    if (lo < 0 || lo >= cb.used) {
        *why = HuffWhy::BadSearchRange;
        return false;
    }
    if (span - lo > 1 && (span > cb.used || span < lo)) {
        *why = HuffWhy::BadSearchRange;
        return false;
    }
    // `0x4E1613 mov ecx,eax / 0x4E1615 call sub_4E1510`（bitrev32）
    const std::uint32_t rev = bitRev32(static_cast<std::uint32_t>(read));
    std::int32_t hi = span;  // edi
    std::int32_t d = hi - lo;
    if (d > 1) {  // `0x4E161E cmp edx,1 / jle 0x4E1658`
        const std::uint8_t* idx = mem.host(cb.dec_index);
        for (;;) {
            d >>= 1;                          // `0x4E1633 sar edx,1`
            const std::int32_t mid = lo + d;  // `0x4E1635 lea ecx,[edx+esi]`
            // `0x4E1638 cmp eax,[ebx+ecx*4]`（**无符号**：`sbb ecx,ecx` 取的是 CF）
            if (rev < loadU32(idx + 4u * static_cast<std::uint32_t>(mid))) {
                hi -= d;  // `0x4E1648 sub edi,ecx`
            } else {
                lo += d;  // `0x4E164A add esi,ebx`
            }
            d = hi - lo;
            if (!(d > 1)) break;  // `0x4E1650 cmp edx,1 / jg 0x4E1630`
        }
    }
    // `0x4E1658 movsx codelengths[esi]` / `0x4E1662 cmp eax,ebx`（ebx = 降位后的位宽）
    const std::int32_t len = loadI8(mem.host(cb.code_lengths) + static_cast<std::uint32_t>(lo));
    if (len < 0) {  // 同直通支：负码长 ⇒ 交解释器
        *why = HuffWhy::BadCodeLengths;
        return false;
    }
    if (len > bits) {  // `0x4E1664 jg 0x4E167C`：**读到的位**不够长 ⇒ adv(read) 并返回 -1
        huffAdv(r, bits);  // `0x4E1681 adv(read)`  ← ★ 与成功支的字节数**不同**
        *out = -1;
        return true;
    }
    huffAdv(r, len);  // `0x4E166B adv(codelengths[lo])`
    *out = lo;
    return true;
}

// ---------------------------------------------------------------------------------------
// ⑤ `sub_4E1950` 的分量累加：`fld [val] / fadd [dst] / fstp dword [dst]`
//
// ★ 逐位等价（与解释器同一条结果）依据：
//   · 单精度落盘 ⇒ 只要"精确和的 float32 舍入"一致，结果就一致；
//   · 两个 **float32** 的精确和，在 |指数差| <= 24 时有效位 <= 48 ⇒ 整段落进宿主 double（53 位）
//     ⇒ `(double)a + (double)b` 是**精确值**，再 `(float)` 一次舍入 = 正确舍入 = SoftFloat 的
//     `extF80`（64 位有效位，同样精确）+ `extF80_to_f32`；
//   · |指数差| > 24 时小操作数 `< 0.5 ulp`，其精确和的小数部分 < 半个 ulp 且大操作数自身
//     在 ulp 里是整的 ⇒ 两种实现都给出**大操作数本身**（不会出现双重舍入差）。
//   ⇒ 只有 `inf`/`NaN` 不走这条推理（NaN payload / 静默位是另一套规则）⇒ 那两个走 SoftFloat。
// ---------------------------------------------------------------------------------------
float bitsToFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool isFiniteF32(std::uint32_t bits) { return (bits & 0x7F800000u) != 0x7F800000u; }

// `ST0 = val; ST0 += dst; fstp dword [dst]` 的等价物。
// `flags` = SoftFloat 的异常位（1=PE / 2=UE / 4=OE / 8=ZE / 16=IE），与 `exec_fpu.cpp` 的
// `syncStatus` 同口径。
std::uint32_t f32AddStore(std::uint32_t val_bits, std::uint32_t dst_bits, std::uint32_t* flags) {
    if (!isFiniteF32(val_bits) || !isFiniteF32(dst_bits)) {
        Ext80 a, b, r;
        std::uint8_t raw[4];
        std::memcpy(raw, &val_bits, 4);
        x87MemToReg(raw, a, 4);
        std::memcpy(raw, &dst_bits, 4);
        x87MemToReg(raw, b, 4);
        x87Add(a, b, r);
        x87RegToMem(r, raw, 4);
        *flags |= x87TakeExceptionFlags();
        std::uint32_t out = 0;
        std::memcpy(&out, raw, 4);
        return out;
    }
    const double s = static_cast<double>(bitsToFloat(val_bits)) + static_cast<double>(bitsToFloat(dst_bits));
    // ★ 上溢判据只能是"**舍入之后**是不是 inf"：`fstp dword` 走 IEEE 的溢出规则 ——
    //   `s ∈ (FLT_MAX, 2^128 − 2^103)` 这一段**舍到 FLT_MAX**（只置 PE），从阈值起才舍到 inf（OE|PE）。
    //   ⛔ 别写成 `|s| > FLT_MAX`：那会把"刚好超过 FLT_MAX 一点点"错判成上溢。
    const float rf = static_cast<float>(s);
    std::uint32_t out = 0;
    std::memcpy(&out, &rf, 4);
    if ((out & 0x7F800000u) == 0x7F800000u) {  // 舍入后是 inf（入参已保证有限）
        *flags |= 1u | 4u;                     // PE | OE
        return out;
    }
    if (static_cast<double>(rf) != s) {  // 不精确
        *flags |= 1u;                    // PE
        // `fstp dword` 落到 subnormal 且不精确 ⇒ `extF80_to_f32` 置 UE
        if ((out & 0x7F800000u) == 0u && (out & 0x007FFFFFu) != 0u) *flags |= 2u;
    }
    return out;
}

// `exec_fpu.cpp` 的 `syncStatus`（`fld`/`fadd`/`fstp` 这一族不动 C0..C3、不改 TOP）。
void syncX87Flags(X86& cpu, std::uint32_t flags) {
    if (flags == 0u) return;  // ⛔ 没有异常位时**一位都不动**
    std::uint16_t s = cpu.fpu.status;
    if (flags & 1u) s |= 0x0020u;   // PE
    if (flags & 2u) s |= 0x0010u;   // UE
    if (flags & 4u) s |= 0x0008u;   // OE
    if (flags & 8u) s |= 0x0004u;   // ZE
    if (flags & 16u) s |= 0x0001u;  // IE
    if (s & 0x003Fu) s |= 0x0080u;  // ES
    cpu.fpu.status = s;
}

bool fpuStackEmpty(const X86& cpu) {
    for (int i = 0; i < 8; ++i)
        if (!cpu.fpu.empty(i)) return false;
    return true;
}

}  // namespace

std::uint32_t huffBitRev32(std::uint32_t v) { return bitRev32(v); }

// ---------------------------------------------------------------------------------------
// `sub_4E1570`：`__cdecl` 8 B（codebook*, oggpack_buffer*）⇒ EAX = entry index / -1
// ---------------------------------------------------------------------------------------
bool nativeHuffDecode4E1570(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    auto decline = [&cpu](HuffWhy why) {
        if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        return false;
    };
    const std::uint32_t a1 = cpu.arg(0);
    const std::uint32_t a2 = cpu.arg(1);
    HuffCodebook cb;
    HuffReader r;
    HuffWhy why = HuffWhy::BadCodebook;
    if (!readCodebook(mem, a1, &cb, &why)) return decline(why);
    if (!readReader(mem, a2, &r, &why)) return decline(why);
    const std::uint32_t endbyte0 = r.endbyte;
    const std::uint32_t endbit0 = r.endbit;
    const std::uint32_t ptr0 = r.ptr;
    std::int32_t idx = 0;
    if (!huffDecodeOnce(mem, cb, &r, &idx, &why)) return decline(why);
    // ---- 从这里开始写 guest（与 guest 的 `adv` 写的是同一三个 dword）----
    if (r.endbyte != endbyte0 || r.endbit != endbit0 || r.ptr != ptr0) {
        mem.wr32(a2 + kBrPtr, r.ptr);          // `0x5132E8`
        mem.wr32(a2 + kBrEndbyte, r.endbyte);  // `0x5132EB`
        mem.wr32(a2 + kBrEndbit, r.endbit);    // `0x5132F0`
    }
    *result = static_cast<std::uint32_t>(idx);
    return true;
}

// ---------------------------------------------------------------------------------------
// `sub_4E1950`：`__cdecl` 16 B（codebook*, float* dst, oggpack_buffer*, int n）⇒ EAX = 0 / -1
// ---------------------------------------------------------------------------------------
bool nativeHuffVector4E1950(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    auto decline = [&cpu](HuffWhy why) {
        if (cpu.current_override != nullptr) ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        return false;
    };
    const std::uint32_t a1 = cpu.arg(0);
    const std::uint32_t dst = cpu.arg(1);
    const std::uint32_t a3 = cpu.arg(2);
    const std::int32_t n = static_cast<std::int32_t>(cpu.arg(3));
    // `0x4E195A cmp dword ptr [ebx],8` 是**进函数就读**的 ⇒ n<=0 也要先判 a1 那 4 字节可读。
    if (a1 == 0u || !spanOk(a1, 4u) || !mem.committed(a1, 4u)) return decline(HuffWhy::BadVector);
    if (n <= 0) {  // `0x4E195C jle 0x4E1A5F`（dim<=8 侧）/ `0x4E1963`（>8 侧）：直接返回 0
        *result = 0;
        return true;
    }
    HuffCodebook cb;
    HuffReader r;
    HuffWhy why = HuffWhy::BadCodebook;
    if (!readCodebook(mem, a1, &cb, &why)) return decline(why);
    if (!readReader(mem, a3, &r, &why)) return decline(why);
    const std::int32_t dim = static_cast<std::int32_t>(mem.rd32(a1 + kCbDim));
    if (dim < 1 || dim > 4096) return decline(HuffWhy::BadVector);
    const std::uint32_t valuelist = mem.rd32(a1 + kCbValueList);
    const std::uint64_t vl_bytes = 4ull * static_cast<std::uint64_t>(dim) * static_cast<std::uint64_t>(cb.used);
    if (valuelist == 0u || vl_bytes > 0xFFFFFFFFull || !spanOk(valuelist, vl_bytes) ||
        !mem.committed(valuelist, static_cast<std::size_t>(vl_bytes)))
        return decline(HuffWhy::BadVector);
    // dst 跨度：每 entry 至少推进 1 个分量 ⇒ entry 数 <= n；最后一个 entry 可越过 n 至多 dim-1
    //（`0x4E19A1 cmp eax,[ebx] / jl` 逐分量推进 esi，然后才 `0x4E19A5 cmp esi,arg_C / jl`）
    const std::uint64_t dst_bytes = 4ull * static_cast<std::uint64_t>(n + dim - 1);
    if (dst_bytes > 0xFFFFFFFFull || !spanOk(dst, dst_bytes) ||
        !mem.committed(dst, static_cast<std::size_t>(dst_bytes)))
        return decline(HuffWhy::BadVector);
    // x87：本实现不碰 guest 的寄存器栈（净栈深 0）；栈非空时 guest 的 `fld` 可能上溢 ⇒ 交解释器。
    if (!fpuStackEmpty(cpu)) return decline(HuffWhy::BadFpu);
    // 舍入模式必须是"最近偶数"（`(float)double` 用宿主缺省 RC；其余 RC 交解释器）
    if ((cpu.fpu.control & 0x0C00u) != 0u) return decline(HuffWhy::BadFpu);

    // ---- 第一遍：只 decode（`huffDecodeOnce` 对 guest 零写），把 entry 攒进栈数组 ----
    // ★ 为什么必须两遍：`dst` 是就地 `+=`，中途回退会被解释器重放成"加两次"。
    std::uint32_t entries[kMaxVectorEntries];
    std::int32_t count = 0;
    std::int32_t total = 0;
    std::int32_t ret = 0;
    while (total < n) {  // `0x4E19A5 cmp esi,arg_C / jl 0x4E1970`
        if (count >= kMaxVectorEntries) return decline(HuffWhy::BadVector);  // 零写（还没累加）
        std::int32_t idx = 0;
        if (!huffDecodeOnce(mem, cb, &r, &idx, &why)) return decline(why);  // 零写
        if (idx == -1) {  // `0x4E197D cmp eax,-1 / jz 0x4E19B1` ⇒ 返回 -1（**保留**已累加的部分）
            ret = -1;
            break;
        }
        entries[count++] = static_cast<std::uint32_t>(idx);
        total += dim;
    }
    // ---- 写回位读状态（与 guest 的 `adv` 同一三个 dword）----
    mem.wr32(a3 + kBrPtr, r.ptr);
    mem.wr32(a3 + kBrEndbyte, r.endbyte);
    mem.wr32(a3 + kBrEndbit, r.endbit);
    // ---- 第二遍：累加（dim<=8 的 8 路跳表与 >8 的通用循环同语义）----
    x87ApplyControlWord(cpu.fpu.control);
    std::uint32_t flags = 0u;
    std::uint8_t* dp = mem.host(dst);
    const std::uint8_t* vl = mem.host(valuelist);
    std::int32_t esi = 0;
    for (std::int32_t k = 0; k < count; ++k) {
        const std::uint8_t* base = vl + 4u * static_cast<std::uint32_t>(dim) * entries[k];
        for (std::int32_t c = 0; c < dim; ++c) {
            const std::uint32_t val = loadU32(base + 4u * static_cast<std::uint32_t>(c));
            const std::uint32_t cur = loadU32(dp + 4u * static_cast<std::uint32_t>(esi));
            const std::uint32_t sum = f32AddStore(val, cur, &flags);
            storeU32(dp + 4u * static_cast<std::uint32_t>(esi), sum);
            ++esi;
        }
    }
    syncX87Flags(cpu, flags);
    *result = static_cast<std::uint32_t>(ret);
    return true;
}

}  // namespace lift
