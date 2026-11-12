// emulator-lift · native override：Huffman 子树的两片叶子（REQ-01M75WED0VFTEYXX43KGHNPPP0）
//
// ★ 两项（都是**函数级**，⛔ 不换整块 `sub_4E3EE0`）：
//   · `sub_4E1570`（`0x4E1570..0x4E1692`，`__cdecl` 8 B = `(codebook*, oggpack_buffer*)`）
//     = look → 首表 → 二分 → adv 的**整棵** decode 叶子；返回 entry index（失败 `-1`）。
//   · `sub_4E1950`（`0x4E1950..0x4E1A65` + 跳表 `0x4E1A68`，`__cdecl` 16 B
//     = `(codebook*, float* dst, oggpack_buffer*, int n)`）= 逐 entry decode + 分量累加。
//   ⇒ 两者都**零分配**、**零 guest 回调**、状态全在 guest 内存（位读结构 + 码本 + dst）
//     ⇒ `sub_4E3EE0`（仍在 guest 里跑）用**同一批参数、同一顺序**调 `sub_4D7A60`
//     ⇒ arena 的 5 个字段与分配序列逐字节不变（评估 §3.2 的四条判据都成立）。
//
// ★ 逐字依据（锚全是 EA；`.c` 只作阅读辅助，⛔ 不当锚）：
//   `sub_513220`（look，`0x513220..0x5132C2`）：
//       `0x513232 [ecx+10h]`=storage / `0x51323B [ecx+4]`=endbit / `0x513238 [ecx]`=endbyte /
//       `0x51325C [ecx+0Ch]`=ptr；`0x513243 cmp (endbyte+4),storage; jl 读`；否则
//       `0x513247..0x513250` 的 `storage*8` vs `endbyte*8 + bits + endbit` 位预算（**有符号**）；
//       `0x51325C..0x5132B9` 按 bits 取 1–5 个字节（第 5 个字节只在 `edx>0x20 且 endbit!=0`）；
//       `0x5132BB and eax, mask[bits]`（表在 `0x55BED8`，33 项 `(1<<n)-1`）。
//   `sub_5132D0`（adv，`0x5132D0..0x5132F5`）：`0x5132DA` 起 `n=(bits+endbit)/8`（`cdq/and 7/add/sar 3`
//       = **向零截断**除法）、`0x5132E8 [ecx+0Ch]+=n` / `0x5132EB [ecx]+=n` / `0x5132F0 [ecx+4]=(bits+endbit)&7`。
//   `sub_4E1510`（bitrev32，`0x4E1510..0x4E156C`）：`rol 16` + 四段 shift-xor-mask。
//   `sub_4E1570`：
//       `0x4E157F [a1+24h]`=firsttablen / `0x4E1582 [a1+28h]`=maxlength；
//       `0x4E1596 [a1+20h]`=firsttable / `0x4E1599 firsttable[look]`=entry；
//       `0x4E159C test ebx,ebx / 0x4E159E jns 0x4E15B8` ⇒ **entry>=0 直通**：
//         `0x4E15B8 [a1+1Ch]`=codelengths、`0x4E15BB movsx codelengths[entry-1]`、
//         `0x4E15C2 adv(len)`、`0x4E15CC lea eax,[ebx-1]` ⇒ 返回 `entry-1`；
//       entry<0 ⇒ `0x4E15A0 [a1+8]`=used_entries、`0x4E15A5 sar esi,0Fh`（**算术**）+ `and 7FFFh`
//         ⇒ `lo=(entry>>15)&0x7FFF`（= 二分下界，进 esi）、`span=used_entries-(entry&0x7FFF)`（进 edi）；
//       `0x4E15D9` 第二次 look（位宽 = maxlength）；失败 ⇒ `0x4E15F0` 的降位重试（每次 `dec` 一位），
//         降到 1 仍失败 ⇒ `0x4E1611 js 0x4E1689` ⇒ 返回 `-1` 且**不 adv**；
//       `0x4E1615 call sub_4E1510`（bitrev）、`0x4E1630` 二分（`cmp eax,[ebx+ecx*4]` 后 `sbb/neg/lea`
//         的 CF 三分支：`rev < dec_index[mid]` ⇒ `edi -= d`，否则 `esi += d`；`d=(edi-esi)>>1`），
//       `0x4E1658 movsx codelengths[esi]`；`0x4E1662 cmp eax,ebx / jg 0x4E167C`
//         ⇒ **成功支 `0x4E166B adv(codelengths[lo])` 返回 `lo`**；
//         **失败支 `0x4E1681 adv(read)` 返回 `-1`**（★ 两条分支的 adv 字节数**不同**）。
//   `sub_4E1950`：`0x4E195A cmp [ebx],8 / jle 0x4E19B9` ⇒ dim<=8 走 8 路跳表、>8 走通用循环，
//       两者语义都是"每 entry：decode → `valuelist + 4*dim*idx` 起取 dim 个分量，
//       `fld/fadd/fstp dword [dst+4*esi]`，esi++"；`0x4E197D cmp eax,-1 / jz 0x4E19B1` ⇒ 只有
//       **恰好 -1** 才是失败；`0x4E195E jle 0x4E1A5F` ⇒ n<=0 直接返回 0（**一个 FP 都不做**）。
//
// ★ 三处**别按"标准 Huffman"想当然**（都写在上面）：
//   ① `0x4E1666` 与 `0x4E167C` 两条分支的 adv 字节数不同（`codelengths[lo]` vs `read`）；
//   ② `0x4E15A5` 是 `sar` 不是 `shr`（entry<0 ⇒ 高 15 位要**符号**右移再掩码）；
//   ③ 位读状态在 **guest 内存**（`0x5132E8/0x5132EB/0x5132F0` 写的三个 dword）⇒ override 读它、
//      也在**同一份**上写回；⛔ 不许在宿主留副本（跨调用状态为零）。
//
// ★ 失败是**静默的**（评估 §2.4.1 第 1 条）：`0x4E40A9 xor eax,eax` 让成功与失败**都返回 0**
//   ⇒ 多返回一次 `-1` 不会让引擎报错，只会让这一段残差静音（听得出、引擎零信号）。所以守卫必须是
//   "**解释器 vs native 逐字节**"（`tests/native_override_assets.cpp` 的真字节两臂），
//   ⛔ 不能靠 hits/fallbacks 之类的伴随计数当判据。
//
// ★ 准入（不满足 ⇒ `return false` ⇒ dispatcher 回退解释器；**回退前零写**）：
//   两项共用码本/位读的闸门（见 `native_huffman.cpp` 的 `readCodebook` / `readReader`），
//   `sub_4E1950` 另加 dim / valuelist / dst 跨度 / x87 栈与舍入模式。
//   ★ 关键一条：**`firsttable[look] == 0` ⇒ 必须 decline** —— 直通支会去读 `codelengths[-1]`
//     （`0x4E15BB`），我们不复刻越界读，交解释器（`HuffWhy::EntryZero`）。
//
// ★ 已知边界（登记，不是缺陷）：
//   ① 函数级 override 只记 1 条 guest 指令 + 1 tick ⇒ 虚拟时钟相位（见 `native_overrides.h` 的准入门）；
//   ② `sub_4E1950` 的 `fld/fadd/fstp dword` 用宿主 double 中间值 + 单精度落盘：两个 **float32**
//      的精确和在 |指数差| <= 24 时能整段落进 double（<= 48 位有效位），指数差 > 24 时小操作数
//      `< 0.5 ulp` ⇒ 结果是**大操作数本身**（两种实现都是）⇒ 与 SoftFloat 的 `extF80` 路径**逐位相同**
//      （推导见 `.cpp` 的 `f32AddStore`）。但**非有限值**（inf/NaN）走 SoftFloat 同一路径，避免伪值。
//      舍入模式不是"最近偶数"（`fpu.control & 0x0C00 != 0`）⇒ decline（那时 `(float)` 的语义不同）。
//   ③ 不复刻 x87 的 `pseudo-denormal` / unnormal（与 `fpu_bridge.cpp` 同一条已知边界）。
//   ④ 窄义：位流窗口按 `[buffer, buffer+storage+5)` 一次判提交（见 `.cpp`）；窗口不提交 ⇒ 回退解释器
//      （只在包尾/损坏包上走到，正常游玩看不到）。
#pragma once

#include <cstdint>

#include "runtime/native_overrides.h"

namespace lift {

// 回退原因（**枚举值就是 `NativeOverride::declines[]` 的下标**；0 = 未分类，不落桶）。
// ★ 12 个桶 = `NativeOverride::kDeclineSlots` 的**整张表**（0..11；0 = 未分类不落桶）。
enum class HuffWhy : std::uint8_t {
    Ok = 0,               // 接管（不落桶）
    BadCodebook = 1,      // a1 不可读 / `[a1, a1+0x2C)` 不在已提交页 / used_entries 越界 / 掩码表不提交
    BadReader = 2,        // a2 的 5 个字段不可读 / endbit 不在 [0,7] / `ptr != buffer+endbyte` / storage 越界
    BadReaderWindow = 3,  // 位流窗口 `[buffer, buffer+storage+5)` 不在已提交页（含 4 GiB 回绕）
    BadFirstTable = 4,    // firsttablen / maxlength 越界 · firsttable 空 · 首表跨度不提交
    BadCodeLengths = 5,   // codelengths 空 / `[codelengths, +used)` 不提交 / 码长字节为负
    BadDecIndex = 6,      // used>1 时 dec_index 空 / `[dec_index, +4*used)` 不提交
    EntryZero = 7,        // `firsttable[look] == 0` ⇒ guest 会读 `codelengths[-1]`
    EntryOutOfRange = 8,  // 直通 entry > used_entries（`codelengths[entry-1]` 会越界）
    BadVector = 9,        // dim / n / valuelist / dst 跨度 / entry 数超栈上限（`sub_4E1950` 专属）
    BadFpu = 10,          // x87 栈非空 / 舍入模式不是"最近偶数"（`sub_4E1950` 专属）
    BadSearchRange = 11,  // 二分区间 `[lo, hi)` 落在 `[0, used_entries)` 之外（越界读 ⇒ 交解释器）
};

// `sub_4E1570`：look → 首表 → 二分 → adv（`__cdecl` 8 B，返回 EAX = entry index / -1）。
bool nativeHuffDecode4E1570(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_4E1950`：逐 entry decode + 分量累加（`__cdecl` 16 B，返回 EAX = 0 / -1）。
bool nativeHuffVector4E1950(X86& cpu, GuestMemory& mem, std::uint32_t* result);

// `sub_4E1510` 的 32 位位反转（纯函数；测试直接拿它跟"逐位移位"的参考实现对账）。
std::uint32_t huffBitRev32(std::uint32_t v);

}  // namespace lift
