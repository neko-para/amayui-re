// emulator-lift · native override：字形位图 → 已锁定纹理表面的逐像素混合叶子（sub_46D9F0）
//
// ★ 16bpp / 32bpp 两支；裁剪 = `sub_46CE20` 的成功路径（失败路径有 sprintf 副作用 ⇒ Declined）。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstring>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
// ---------------------------------------------------------------------------------------
// `sub_46D9F0`：字形位图 → **已锁定**纹理表面的逐像素混合叶子
//
// ★ 逐字依据（`.lst` 0x46D9F0..0x46DEC2；`.c` 只作阅读辅助，⛔ 不当锚）：
//   · ABI：`retn 20h` ⇒ 8 个栈参数（32 B），`ECX = this`（T_Font*）。参数（按 0x46E98A 的调用点核对）：
//       a2 目标 pitch（**字节**）· a3 目标像素基址（调用方已 Lock）·
//       a4 → 4 个 dword（宽 / 高 / 原点 x / 原点 y）· a5 源行跨度 · a6 源基址 · a7/a8 目标 x/y ·
//       a9 颜色（**低字节是第一通道**，与 0x46DC76 起的字节反序一致）。
//   · 裁剪矩形 = guest `sub_46CE20` 的**成功路径**：obj = `[mgr+0xA5D8+4*texid]`（0x46CE29），
//     rect = `[obj+41Ch]/[obj+420h]/[obj+424h]/[obj+428h]`（0x46CE73..0x46CE93）；
//     mgr = `[Font+0x410]`（0x46D9FA）、texid = `[mgr+0xB648]`（0x46DA00）。
//     ★ obj == 0 时 guest 走 `_sprintf_s(mgr+8,…)` + `sub_4034C0(mgr+8)`（0x46CE34..0x46CE4F）——
//       那条路径有 **guest 内存副作用**，本实现不复制 ⇒ obj == 0 一律 Declined。
//   · 位深：`0x46DA14 cmp ds:dword_5518C0,10h` / `0x46DA25 jnz` ⇒ == 0x10 走 16 位支，否则 32 位支。
//     ⛔ 不是 DDraw 的显示 bpp（`dword_5518C0` 的唯一写点是配置 `display:TextureBit`）。
//   · 源字形格式：`[Font+0x548]`（0x46DA80）0 = 1bpp 掩码 / 非 0 = 8bpp 级数；
//     `[Font+0x35584]`（0x46DA93）== 1 ⇒ 除 65（65 级，满覆盖 251）/ 否则除 17（17 级，满覆盖 240）。
//   · **覆盖值在裁剪判断之前算**（0x46DA80..0x46DB0E 早于 0x46DB2D）⇒ 被裁掉的像素照样读源字节。
//   · 三条写支路共用同一个写入点（16 位 0x46DC39 / 32 位 0x46DE89）：
//       覆盖值 = 0 ⇒ 一个字节都不写（0x46DB11 / 0x46DD5E）；
//       覆盖值 == 满值（15 / 255）⇒ 整像素覆写（0x46DB72 / 0x46DDC0），**不读目标**；
//       目标自身高半字节 / 高字节 == 0 ⇒ 整像素覆写（0x46DB86 / 0x46DDD5）；
//       其余 ⇒ 逐通道 `(目标×(满-覆盖) + 源×覆盖) ÷ 满`，输出不透明度 = `max(目标, 覆盖)`
//       （16 位 0x46DB8E..0x46DC25，满 = 15；32 位 0x46DDDB..0x46DE76，满 = 255）。
//   · 地址：`pe = 向零截断(pitch/2)`（0x46DB51..0x46DB57）/ `(pitch/4)`（0x46DD9E..0x46DDA7）；
//     `off = pe*y + x`（32 位回绕）→ `bits + off*2` / `bits + off*4`。
//   · 返回：h ≤ 0 的出口 EAX = 0（0x46DA50 `xor eax,eax` / 0x46DA5E `jle`；32 位 0x46DC9D / 0x46DCAB），
//     正常出口 EAX = a5（0x46DC55 / 0x46DEA4）⇒ `EAX = (a4[1] > 0) ? a5 : 0`。
//   · 全函数 0 条 x87、0 个间接调用、只有上面两个非栈写 ⇒ 纯叶子。
// ---------------------------------------------------------------------------------------
namespace {

// `cdq; sub eax,edx; sar eax,1`（0x46DB54..0x46DB57）= **向零截断**的 /2。
// ⛔ 不是 `>>1`：`-3/2 = -1`，而 `-3>>1 = -2`；guest 用的是前者。
std::int32_t truncDiv2(std::int32_t v) { return v / 2; }
// `cdq; and edx,3; add eax,edx; sar eax,2`（0x46DD9E..0x46DDA7）= 向零截断的 /4。
std::int32_t truncDiv4(std::int32_t v) { return v / 4; }

// MSVC 的魔数除法序列 —— 逐字复刻 `.lst` 里的四条：
//   `imul`（64 位积）→ edx = 高 32 位 → [可选 `add edx, 被乘数`] → `sar edx, shift`
//   → `mov eax,edx; shr eax,1Fh; add eax,edx`（向零修正）。
//   · ÷65 ：0x46DAA1..0x46DAAD（M=0x7E07E07F，sar 5，**不**加 n）
//   · ÷17 ：0x46DAB4..0x46DAC0（M=0x78787879，sar 3，**不**加 n）
//   · ÷15 ：0x46DBB4..0x46DBC0（M=0x88888889，sar 3，**加** n）
//   · ÷255：0x46DE06..0x46DE12（M=0x80808081，sar 7，**加** n）
// ★ 乘 / 加 / 移位都在 32 位里回绕；⛔ 不要换成 C 的 `/`（"数学上等价"≠"逐位相同"）。
std::int32_t magicDiv(std::int32_t n, std::uint32_t magic, int shift, bool add_n) {
    const std::int64_t p = static_cast<std::int64_t>(n) * static_cast<std::int32_t>(magic);
    std::uint32_t edx = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32);
    if (add_n) edx += static_cast<std::uint32_t>(n);
    const std::int32_t shifted = static_cast<std::int32_t>(edx) >> shift;  // C++20：算术右移
    const std::uint32_t sign = static_cast<std::uint32_t>(shifted) >> 31;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(shifted) + sign);
}

// 8bpp 级数 → 覆盖值：`raw * 255` 再除 65 / 17（0x46DA89..0x46DAC0，32 位支同形）。
// ★ 值域由**源数据**保证（GGO_GRAY8 = 0..64、GGO_GRAY4 = 0..16）⇒ 满覆盖 251 / 240。
//   guest **没有** clamp ⇒ 超值域时结果无意义（会溢出到 alpha / 负的 `满-覆盖`）⇒
//   准入层直接 Declined（见 nativeBlend46D9F0 的"级数越界"）。
std::int32_t glyphCoverage8(std::uint8_t raw, bool levels65) {
    const std::int32_t scaled = static_cast<std::int32_t>(raw) * 0xFF;  // 0x46DA8D imul ecx,0FFh
    return levels65 ? magicDiv(scaled, 0x7E07E07Fu, 5, /*add_n=*/false)
                    : magicDiv(scaled, 0x78787879u, 3, /*add_n=*/false);
}

}  // namespace

std::uint32_t blendGlyph46D9F0(const GlyphBlit& b) {
    if (b.height <= 0) return 0;  // 0x46DA5E（EAX 来自 0x46DA50 的 xor）/ 0x46DCAB
    // ★ 纯核的防呆：空指针 ⇒ 什么都不写、但仍按 guest 语义给 EAX。
    //   ⚠ 可达性：走 override 的路径**到不了这里**（`nativeBlend46D9F0` 在准入层就把 src/dst == 0
    //   判成 Declined）。所以这条只对**直接调核的测试**有意义；⛔ 不要把它当成"空指针也能跑"。
    if (b.dst == nullptr || b.src == nullptr) return static_cast<std::uint32_t>(b.src_stride);
    const std::uint32_t ret = static_cast<std::uint32_t>(b.src_stride);
    const std::int32_t pe = b.sixteen_bpp ? truncDiv2(b.dst_pitch) : truncDiv4(b.dst_pitch);
    const bool levels65 = b.levels == GlyphLevels::Levels65;
    const bool level8 = b.source == GlyphMaskSource::Level8;
    const std::uint8_t* srow = b.src;  // guest 也是逐行 `add`（0x46DC5C / 0x46DEAB）

    if (b.sixteen_bpp) {
        // 颜色解包（0x46DA2B..0x46DA4D）：三个半字节 → bit8..11 / bit4..7 / bit0..3
        const std::uint32_t c_lo = (b.color >> 4) & 0xFu;    // var_1C
        const std::uint32_t c_mid = (b.color >> 12) & 0xFu;  // var_24
        const std::uint32_t c_hi = (b.color >> 20) & 0xFu;   // var_2C
        const std::uint16_t flat = static_cast<std::uint16_t>((c_lo << 8) | (c_mid << 4) | c_hi);
        for (std::int32_t row = 0; row < b.height; ++row, srow += b.src_stride) {
            // y = row - a4[3] + a8（0x46DB1C..0x46DB2A，32 位回绕）
            const std::int32_t y = static_cast<std::int32_t>(static_cast<std::uint32_t>(row) -
                                                             static_cast<std::uint32_t>(b.origin_y) +
                                                             static_cast<std::uint32_t>(b.dst_y));
            if (y < b.clip_top || y >= b.clip_bottom) continue;  // 0x46DB3F..0x46DB4B
            for (std::int32_t col = 0; col < b.width; ++col) {
                std::int32_t cov;
                if (level8) {
                    cov = glyphCoverage8(srow[col], levels65);  // 0x46DA89..0x46DAC0
                } else {
                    // 1bpp：字节下标 `col>>3`、位序 `7 - (col&7)`（0x46DAC2..0x46DB06）；置位 ⇒ 255
                    const std::uint32_t c = static_cast<std::uint32_t>(col);
                    const std::uint32_t byte = static_cast<std::uint32_t>(srow[c >> 3]);
                    const std::uint32_t bit = (byte >> (7u - (c & 7u))) & 1u;
                    cov = bit != 0u ? 0xFF : 0;
                }
                cov >>= 4;               // 0x46DB0E `sar esi,4`：8 位覆盖值 → 4 位通道值
                if (cov == 0) continue;  // 0x46DB11..0x46DB13（一个字节都不写）
                // x = a4[2] + col + a7（0x46DB19..0x46DB27）
                const std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(b.origin_x) +
                                                                 static_cast<std::uint32_t>(col) +
                                                                 static_cast<std::uint32_t>(b.dst_x));
                if (x < b.clip_left || x >= b.clip_right) continue;  // 0x46DB2D..0x46DB39
                const std::uint32_t off = static_cast<std::uint32_t>(pe) * static_cast<std::uint32_t>(y) +
                                          static_cast<std::uint32_t>(x);  // 0x46DB59..0x46DB5F
                std::uint8_t* p = b.dst + static_cast<std::size_t>(off) * 2u;  // 0x46DB61
                const std::uint16_t want =
                    static_cast<std::uint16_t>((static_cast<std::uint32_t>(cov) << 12) | flat);  // 0x46DB64..0x46DB6F
                if (cov == 0xF) {  // 0x46DB72..0x46DB75（满覆盖 ⇒ 整像素覆写）
                    std::memcpy(p, &want, 2);
                    continue;
                }
                std::uint16_t old = 0;
                std::memcpy(&old, p, 2);  // 0x46DB7B
                std::uint32_t out_a = static_cast<std::uint32_t>(old) >> 12;
                if (out_a == 0u) {  // 0x46DB86..0x46DB88（目标 A = 0 ⇒ 覆写）
                    std::memcpy(p, &want, 2);
                    continue;
                }
                if (out_a < static_cast<std::uint32_t>(cov)) out_a = static_cast<std::uint32_t>(cov);  // max
                const std::uint32_t k = static_cast<std::uint32_t>(0xF - cov);  // 0x46DB9D..0x46DBA2
                const std::uint32_t ccov = static_cast<std::uint32_t>(cov);
                const std::uint32_t oldv = old;
                // 三路加权（0x46DBB4 / 0x46DBF1 / 0x46DC14）：`(dst*k + cov*c) ÷15 & 0xF`
                const std::uint32_t n8 = ((oldv >> 8) & 0xFu) * k + ccov * c_lo;
                const std::uint32_t n4 = ((oldv >> 4) & 0xFu) * k + ccov * c_mid;
                const std::uint32_t n0 = (oldv & 0xFu) * k + ccov * c_hi;
                const std::uint32_t o8 =
                    static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(n8), 0x88888889u, 3, true)) & 0xFu;
                const std::uint32_t o4 =
                    static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(n4), 0x88888889u, 3, true)) & 0xFu;
                const std::uint32_t o0 =
                    static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(n0), 0x88888889u, 3, true)) & 0xFu;
                const std::uint16_t out =
                    static_cast<std::uint16_t>((out_a << 12) | (o8 << 8) | (o4 << 4) | o0);  // 0x46DC33
                std::memcpy(p, &out, 2);                                                     // 0x46DC39
            }
        }
        return ret;
    }

    // ── 32 位支（0x46DC76..0x46DEC2）────────────────────────────────────────────────────
    const std::uint32_t c0 = b.color & 0xFFu;           // var_2C → bit16..23
    const std::uint32_t c1 = (b.color >> 8) & 0xFFu;    // var_28 → bit8..15
    const std::uint32_t c2 = (b.color >> 16) & 0xFFu;   // var_24 → bit0..7
    const std::uint32_t flat = (c0 << 16) | (c1 << 8) | c2;  // 0x46DC7E..0x46DC9A
    for (std::int32_t row = 0; row < b.height; ++row, srow += b.src_stride) {
        const std::int32_t y = static_cast<std::int32_t>(static_cast<std::uint32_t>(row) -
                                                         static_cast<std::uint32_t>(b.origin_y) +
                                                         static_cast<std::uint32_t>(b.dst_y));
        if (y < b.clip_top || y >= b.clip_bottom) continue;  // 0x46DD83..0x46DD98
        for (std::int32_t col = 0; col < b.width; ++col) {
            std::int32_t cov;
            if (level8) {
                cov = glyphCoverage8(srow[col], levels65);  // 0x46DCD9..0x46DD58（**没有** >>4）
            } else {
                const std::uint32_t c = static_cast<std::uint32_t>(col);
                const std::uint32_t byte = static_cast<std::uint32_t>(srow[c >> 3]);
                const std::uint32_t bit = (byte >> (7u - (c & 7u))) & 1u;
                cov = bit != 0u ? 0xFF : 0;
            }
            if (cov == 0) continue;  // 0x46DD5E..0x46DD60
            const std::int32_t x = static_cast<std::int32_t>(static_cast<std::uint32_t>(b.origin_x) +
                                                             static_cast<std::uint32_t>(col) +
                                                             static_cast<std::uint32_t>(b.dst_x));
            if (x < b.clip_left || x >= b.clip_right) continue;  // 0x46DD7A..0x46DD86
            const std::uint32_t off = static_cast<std::uint32_t>(pe) * static_cast<std::uint32_t>(y) +
                                      static_cast<std::uint32_t>(x);  // 0x46DDAA..0x46DDB0
            std::uint8_t* p = b.dst + static_cast<std::size_t>(off) * 4u;  // 0x46DDB2
            const std::uint32_t want = (static_cast<std::uint32_t>(cov) << 24) | flat;  // 0x46DDB5..0x46DDBA
            if (cov == 0xFF) {  // 0x46DDC0..0x46DDC6（满覆盖 ⇒ 整像素覆写）
                std::memcpy(p, &want, 4);
                continue;
            }
            std::uint32_t old = 0;
            std::memcpy(&old, p, 4);  // 0x46DDCC
            std::uint32_t out_a = old >> 24;
            if (out_a == 0u) {  // 0x46DDD3..0x46DDD5（目标 A = 0 ⇒ 覆写）
                std::memcpy(p, &want, 4);
                continue;
            }
            if (out_a < static_cast<std::uint32_t>(cov)) out_a = static_cast<std::uint32_t>(cov);  // max
            const std::uint32_t k = 255u - static_cast<std::uint32_t>(cov);  // 0x46DDEC..0x46DDF1
            const std::uint32_t ccov = static_cast<std::uint32_t>(cov);
            // 三路加权（0x46DE06 / 0x46DE3A / 0x46DE63）：`(dst*k + cov*c) ÷255 & 0xFF`
            const std::uint32_t nR = ((old >> 16) & 0xFFu) * k + ccov * c0;
            const std::uint32_t nG = ((old >> 8) & 0xFFu) * k + ccov * c1;
            const std::uint32_t nB = (old & 0xFFu) * k + ccov * c2;
            const std::uint32_t oR =
                static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(nR), 0x80808081u, 7, true)) & 0xFFu;
            const std::uint32_t oG =
                static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(nG), 0x80808081u, 7, true)) & 0xFFu;
            const std::uint32_t oB =
                static_cast<std::uint32_t>(magicDiv(static_cast<std::int32_t>(nB), 0x80808081u, 7, true)) & 0xFFu;
            const std::uint32_t out = (out_a << 24) | (oR << 16) | (oG << 8) | oB;  // 0x46DE7E
            std::memcpy(p, &out, 4);                                               // 0x46DE89
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------------------
// `sub_46D9F0` 的准入层：能判的全判；判不了（或判出来不该接手）⇒ 返回 false 回退解释器。
//
// ★ 为什么不"顺手"把 sub_46CE20 也 native：它在 obj == 0 时写 mgr+8 的错误串并调
//   sub_4034C0（guest 函数）⇒ 那条路径不可省 ⇒ 本实现只在**成功路径**（obj != 0）接手。
// ★ 为什么 Declined 不计入"半状态"：所有判据都在**写第一个像素之前**做完（含逐行的源跨度 /
//   级数预扫），失败 ⇒ 一个字节都没改过。
// ---------------------------------------------------------------------------------------
namespace {

// 纹理槽表容量：`+0xA5D8` .. `+0xB574` 恰好 1000 格（`docs/02-engine/sn0000-triage/report-e-texture-render.md`
// 记的 0x499C51 `mov edi,3E7h` / 0x499C56 `lea esi,[ebx+0B574h]` / 0x499C71 `dec edi` / 0x499C72 `jns`）。
// ★ 越界时 guest 读的是对象里的别的字段（再远就出对象）⇒ ⛔ 不猜，Declined。
constexpr std::uint32_t kFontTextureSlots = 1000u;
// 字形宽高的理性上界（引擎的字形黑框远小于它）。超了说明 a4 不是我们认得的形状 ⇒ Declined。
constexpr std::int32_t kGlyphMaxSide = 0x10000;
// 纹理位深配置 `ds:dword_5518C0`（唯一写点 0x406B14，值来自 `display:TextureBit`）。
constexpr std::uint32_t kTextureBitEa = 0x005518C0u;

// [lo, hi) 是否都在已提交页里（hi 允许等于 4 GiB）。
bool committedSpan(const GuestMemory& mem, std::uint64_t lo, std::uint64_t hi) {
    if (lo >= hi || hi > GuestMemory::kSpace) return false;
    return mem.committed(static_cast<std::uint32_t>(lo), static_cast<std::size_t>(hi - lo));
}

}  // namespace

bool nativeBlend46D9F0(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const ThisCallArgs tc = thisCall(cpu, 8);
    const std::uint32_t self = tc.self;          // ECX = T_Font*
    const std::uint32_t dst_pitch = tc[0];       // a2（字节）
    const std::uint32_t dst_bits = tc[1];        // a3
    const std::uint32_t metrics = tc[2];         // a4 → 宽/高/原点 x/原点 y
    const std::uint32_t src_stride = tc[3];      // a5
    const std::uint32_t src = tc[4];             // a6
    // ★ a7 / a8 是**有符号**坐标（guest 用 `jl/jge` 比裁剪矩形）⇒ 在入口就按 int32 取，
    //   ⛔ 不要留成 uint32：负坐标一旦被当成 4e9 就会在"坐标溢出"判据上被误拒。
    const std::int32_t dst_x = static_cast<std::int32_t>(tc[5]);  // a7
    const std::int32_t dst_y = static_cast<std::int32_t>(tc[6]);  // a8
    const std::uint32_t color = tc[7];           // a9
    *result = 0;

    // ── ① Font：`[this+0x410]` / `[this+0x548]` / `[this+0x35584]` 三个读点 ────────────────
    if (self == 0) return false;
    if (static_cast<std::uint64_t>(self) + 0x35588u > GuestMemory::kSpace) return false;
    if (!mem.committed(self + 0x410u, 4u) || !mem.committed(self + 0x548u, 4u) ||
        !mem.committed(self + 0x35584u, 4u))
        return false;
    const std::uint32_t mgr = mem.rd32(self + 0x410u);
    if (mgr == 0) return false;

    // ── ② 裁剪矩形（= `sub_46CE20` 的成功路径；失败路径有 sprintf 副作用 ⇒ 不接手）────────
    if (static_cast<std::uint64_t>(mgr) + 0xB64Cu > GuestMemory::kSpace) return false;
    if (!mem.committed(mgr + 0xB648u, 4u)) return false;
    const std::uint32_t texid = mem.rd32(mgr + 0xB648u);
    if (texid >= kFontTextureSlots) return false;
    const std::uint64_t slot64 = static_cast<std::uint64_t>(mgr) + 0xA5D8u + 4ull * texid;
    if (slot64 + 4u > GuestMemory::kSpace) return false;
    const std::uint32_t slot = static_cast<std::uint32_t>(slot64);
    if (!mem.committed(slot, 4u)) return false;
    const std::uint32_t clip_obj = mem.rd32(slot);
    if (clip_obj == 0) return false;  // guest 会 sprintf_s + sub_4034C0（有副作用的错误路径）
    if (static_cast<std::uint64_t>(clip_obj) + 0x42Cu > GuestMemory::kSpace) return false;
    if (!mem.committed(clip_obj + 0x41Cu, 16u)) return false;
    const std::int32_t clip_left = static_cast<std::int32_t>(mem.rd32(clip_obj + 0x41Cu));
    const std::int32_t clip_top = static_cast<std::int32_t>(mem.rd32(clip_obj + 0x420u));
    const std::int32_t clip_right = static_cast<std::int32_t>(mem.rd32(clip_obj + 0x424u));
    const std::int32_t clip_bottom = static_cast<std::int32_t>(mem.rd32(clip_obj + 0x428u));

    // ── ③ 位深（0x46DA14）；本线只认 16 / 32（别的值 guest 会当 32 位支写，没证据 ⇒ 不接手）──
    if (!mem.committed(kTextureBitEa, 4u)) return false;
    const std::uint32_t display_bpp = mem.rd32(kTextureBitEa);
    if (display_bpp != 16u && display_bpp != 32u) return false;
    const bool sixteen = display_bpp == 16u;

    // ── ④ 参数（a4 的四个 dword）───────────────────────────────────────────────────────
    if (metrics == 0 || !mem.committed(metrics, 16u)) return false;
    const std::int32_t width = static_cast<std::int32_t>(mem.rd32(metrics + 0u));
    const std::int32_t height = static_cast<std::int32_t>(mem.rd32(metrics + 4u));
    const std::int32_t origin_x = static_cast<std::int32_t>(mem.rd32(metrics + 8u));
    const std::int32_t origin_y = static_cast<std::int32_t>(mem.rd32(metrics + 0xCu));
    // ★ 返回值的两条出口（0x46DA50/0x46DA5E 与 0x46DC9D/0x46DCAB、0x46DC55/0x46DEA4）：
    *result = height > 0 ? src_stride : 0u;
    // ★ 需求单把 `w<=0` 列为 Declined；这里**接手**它：`[edi]` 的 `jle` 只跳过整行（0x46DA75/jle），
    //   一个字节都不写、返回值与 h<=0 同形 —— 完全可判。若把它算 fallback，空格 / 空字形会让
    //   "fallbacks=0" 这条判据失真（本项进 `all` 的硬条件）。同理 h<=0 也不 Declined。
    if (height <= 0) return true;
    if (width <= 0) return true;
    if (width > kGlyphMaxSide || height > kGlyphMaxSide) return false;  // 形状不像字形 ⇒ 不猜
    if (src == 0 || dst_bits == 0) return false;
    const std::int32_t pb = sixteen ? 2 : 4;
    const std::int32_t pe = sixteen ? truncDiv2(static_cast<std::int32_t>(dst_pitch))
                                    : truncDiv4(static_cast<std::int32_t>(dst_pitch));
    if (pe <= 0) return false;  // pitch 的元素数 <= 0（行步进 <= 0）⇒ 不接手

    // ── ⑤ 会被写的地址区间（= 裁剪矩形 ∩ 字形矩形）；空交集 ⇒ 一个字节都不写 ───────────────
    // ★ x = a4[2] + col + a7、y = row - a4[3] + a8 都是 32 位回绕加法；这里用 int64 判"没有回绕"：
    //   只要**整段**落在 int32 里，回绕后的值就等于 int64 的值（模 2^32 加法可结合）。
    const std::int64_t gx0 = static_cast<std::int64_t>(origin_x) + static_cast<std::int64_t>(dst_x);
    const std::int64_t gx1 = gx0 + static_cast<std::int64_t>(width);
    const std::int64_t gy0 = static_cast<std::int64_t>(dst_y) - static_cast<std::int64_t>(origin_y);
    const std::int64_t gy1 = gy0 + static_cast<std::int64_t>(height);
    constexpr std::int64_t kI32Min = -2147483648ll;
    constexpr std::int64_t kI32Max = 2147483647ll;
    if (gx0 < kI32Min || gx1 - 1 > kI32Max || gy0 < kI32Min || gy1 - 1 > kI32Max) return false;
    const std::int64_t ix0 = clip_left > gx0 ? static_cast<std::int64_t>(clip_left) : gx0;
    const std::int64_t ix1 = clip_right < gx1 ? static_cast<std::int64_t>(clip_right) : gx1;
    const std::int64_t iy0 = clip_top > gy0 ? static_cast<std::int64_t>(clip_top) : gy0;
    const std::int64_t iy1 = clip_bottom < gy1 ? static_cast<std::int64_t>(clip_bottom) : gy1;
    if (ix0 >= ix1 || iy0 >= iy1) return true;  // 全被裁掉 ⇒ 不写
    const std::int64_t off_lo = static_cast<std::int64_t>(pe) * iy0 + ix0;
    const std::int64_t off_hi = static_cast<std::int64_t>(pe) * (iy1 - 1) + (ix1 - 1);
    if (off_lo < 0) return false;
    const std::uint64_t kMaxOffset = GuestMemory::kSpace / static_cast<std::uint64_t>(pb);
    if (static_cast<std::uint64_t>(off_hi) >= kMaxOffset) return false;  // 先挡溢出，再算字节地址
    const std::uint64_t abs_lo = static_cast<std::uint64_t>(dst_bits) + static_cast<std::uint64_t>(off_lo) * static_cast<std::uint64_t>(pb);
    const std::uint64_t abs_hi =
        static_cast<std::uint64_t>(dst_bits) + static_cast<std::uint64_t>(off_hi) * static_cast<std::uint64_t>(pb) + static_cast<std::uint64_t>(pb);
    if (!committedSpan(mem, abs_lo, abs_hi)) return false;

    // ── ⑥ 源跨度 + 级数值域（逐行预扫，**在写任何像素之前**）────────────────────────────
    const std::uint32_t mask_format = mem.rd32(self + 0x548u);
    const bool level8 = mask_format != 0u;                       // 0x46DA80/0x46DA87
    const bool levels65 = mem.rd32(self + 0x35584u) == 1u;       // 0x46DA93/0x46DA9A
    const std::uint8_t max_level = levels65 ? 64u : 16u;         // 65 级 / 17 级的满值
    const std::uint32_t row_bytes = level8 ? static_cast<std::uint32_t>(width)
                                           : (static_cast<std::uint32_t>(width) + 7u) / 8u;
    const std::int32_t stride = static_cast<std::int32_t>(src_stride);  // guest 逐行 `add`（有符号 32 位）
    std::uint64_t src_lo = UINT64_MAX;
    std::uint64_t src_hi = 0;
    // ★ **两遍**：先只算 [lo, hi) 并做提交检查，**之后**才碰宿主指针。
    //   ⛔ 不要在同一遍里"边算边读"：`mem.host()` 是裸指针（memory.h），越过未提交页会直接撞
    //   平台 fault —— 那会走 `guestFault`（并且归因到**上一条**访存），而不是我们想要的"干净 Declined"。
    for (std::int32_t row = 0; row < height; ++row) {
        const std::int64_t a = static_cast<std::int64_t>(src) + static_cast<std::int64_t>(stride) * row;
        if (a < 0) return false;
        const std::uint64_t au = static_cast<std::uint64_t>(a);
        if (au + row_bytes > GuestMemory::kSpace) return false;  // 越过 4 GiB ⇒ 地址会回绕 ⇒ 不接手
        if (au < src_lo) src_lo = au;
        if (au + row_bytes > src_hi) src_hi = au + row_bytes;
    }
    if (!committedSpan(mem, src_lo, src_hi)) return false;
    if (level8) {
        // 级数越界 ⇒ guest 的覆盖值会超出该格式的满值（16 位支 `满-覆盖` 变负、32 位支把溢出位
        // 挤进 alpha）⇒ 结果不是"画得好不好"而是没定义 ⇒ Declined（⛔ 不 clamp）。
        // ★ 走到这里 [src_lo, src_hi) 已确认提交 ⇒ 下面的 `host()` 读是安全的。
        for (std::int32_t row = 0; row < height; ++row) {
            const std::uint64_t au = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(src) + static_cast<std::int64_t>(stride) * row);
            const std::uint8_t* p = mem.host(static_cast<std::uint32_t>(au));
            for (std::uint32_t i = 0; i < row_bytes; ++i) {
                if (p[i] > max_level) return false;
            }
        }
    }

    // ── ⑦ 像素核（纯函数；G1 直接跑同一份代码）────────────────────────────────────────
    GlyphBlit b;
    b.dst = mem.host(dst_bits);
    b.dst_pitch = static_cast<std::int32_t>(dst_pitch);
    b.width = width;
    b.height = height;
    b.origin_x = origin_x;
    b.origin_y = origin_y;
    b.src = mem.host(src);
    b.src_stride = stride;
    b.dst_x = static_cast<std::int32_t>(dst_x);
    b.dst_y = static_cast<std::int32_t>(dst_y);
    b.color = color;
    b.clip_left = clip_left;
    b.clip_top = clip_top;
    b.clip_right = clip_right;
    b.clip_bottom = clip_bottom;
    b.sixteen_bpp = sixteen;
    b.source = level8 ? GlyphMaskSource::Level8 : GlyphMaskSource::BitMask1;
    b.levels = levels65 ? GlyphLevels::Levels65 : GlyphLevels::Levels17;
    *result = blendGlyph46D9F0(b);
    return true;
}

}  // namespace lift
