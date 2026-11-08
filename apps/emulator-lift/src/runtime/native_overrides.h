// emulator-lift · guest 函数级 native override（试点原型）
//
// ★ 口径（REQ-01M6NZSAAD0WCNBE3RKMFF84R5）：
//   - 只在 **dispatcher 的 block 入口**按 EA 拦截（不 patch guest 字节 ⇒ 天然避开 SMC / 代码页保护）；
//   - native 实现只能经 `GuestMemory` 读写 guest；⛔ 宿主指针不进 guest 状态；
//   - 返回后由本框架按 ABI 设置 `EAX` / `ESP` / `EIP`，未覆盖路径照旧走解释器；
//   - 每一项都**默认关闭**，由 `--native-overrides=<list>` 显式启用。
// ★ 本文件 = **契约**（机制 + 纯核声明）；实现按域拆在 `src/runtime/native_*.cpp`（2026-11-08，
//   纯搬运）—— `native_overrides.cpp` 只留机制 / 已知表 / 段级锚校验。
// ★ 候选由 profile 选出，不先猜（见 requirement 的测试计划）。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/guest_call.h"

namespace lift {

struct X86;
class GuestMemory;
class LstIndex;

enum class GuestConv : std::uint8_t {
    Cdecl = 0,    // 调用方清栈
    Stdcall = 1,  // 被调用方清栈
    Thiscall = 2, // ECX = this；栈参数由被调用方清（MSVC x86）
};

const char* guestConvName(GuestConv c);

// native 实现：读参数（cpu.arg(i) / cpu.ecx()），经 mem 访存，写 *result（= 返回 EAX）。
// 返回 false ⇒ 本次**不做** override，由 dispatcher 回退解释器（例如 safe mode 下参数越界）。
// ⚠ 只有 EAX 一条返回通路：EDX/ECX/ST0 等 call-clobbered 寄存器不会按 guest 语义被改写
//   （调用方按 ABI 也不该依赖它们）；需要额外寄存器/EFLAGS 的段级替换走 NativeBlockFn。
using NativeFn = bool (*)(X86& cpu, GuestMemory& mem, std::uint32_t* result);

struct NativeOverride {
    std::uint32_t ea = 0;
    std::string name;
    GuestConv conv = GuestConv::Cdecl;
    std::uint32_t arg_bytes = 0;  // 栈参数总字节（不含返回地址；thiscall 不含 ECX）
    NativeFn fn = nullptr;
    std::uint64_t hits = 0;
    std::uint64_t fallbacks = 0;
};

class NativeOverrideTable {
public:
    // ★ 2026-11-07：8 → 12（BOOT profile 选出的 sub_429020 / sub_48AE90 两项 + 给后续留坑）。
    static constexpr std::size_t kMax = 12;

    bool add(std::uint32_t ea, const std::string& name, GuestConv conv, std::uint32_t arg_bytes, NativeFn fn,
             std::string* err);
    void clear();
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    NativeOverride* at(std::size_t i) { return i < count_ ? &entries_[i] : nullptr; }
    const NativeOverride* at(std::size_t i) const { return i < count_ ? &entries_[i] : nullptr; }

    NativeOverride* find(std::uint32_t ea);
    const NativeOverride* find(std::uint32_t ea) const;

    // 执行一次命中：成功 ⇒ 设好 EAX / EIP / ESP 并返回 true；false ⇒ 原因写进 *why（调用方回退解释器）。
    // suspended=true ⇒ native 实现已经用 startNestedGuestCall() 铺了一个 guest 回调帧，
    //   EIP/ESP 已由 GuestCallStack 设置，调用方必须直接 continue（⛔ 不要回退解释器）。
    bool invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why, bool* suspended = nullptr);

private:
    std::array<NativeOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------------------
// 段级 override（**函数中段**的固定 EA，不是函数入口）
//
// ★ 为什么现有机制够不着：函数级 override 只在 `call` 目标（= 下一条 EIP）拦，所以只覆盖
//   "整个函数被一次调用替换"。`sub_414AC0` 的 1.18 s 全在函数中段的填池循环（`.lst` 0x415384，
//   ~7.37M 次迭代）—— 入口进去也没用，克隆整函数（文件 I/O + guest 容器/堆）不值得。
// ★ 前提（用户口径）：**guest 代码只读** ⇒ 某个固定 EA 处的代码是固定的，可以按 EA 拦。
//   ⛔ 不 patch guest 字节；⛔ 不每迭代拦一次 —— 一次调用只拦一次，然后跳到段的出口。
// ★ 命中点：dispatcher 在**每个 block 入口**按 EIP 查表（不是只在 call 目标）。
// ★ 语义契约（fn 必须自己保证）：
//   · 先判断"现在是不是段入口"（例如循环入口的 `xor eax,eax` ⇒ EAX==0）；不匹配 ⇒ Declined；
//   · 只在**所有前置条件**（参数 / 范围 / 计数 / 提交状态）匹配时才替 guest 执行；
//   · Replaced 时必须已经写好 **EIP（= 段出口）/ EAX / EDX / ECX / EFLAGS** 等 guest 可观察状态；
//     `replaced_insns` 报告被替代的 guest 指令数（dispatcher 用它补 `insns` 与虚拟时钟，
//     见 README §4.3 —— 否则 `timeGetTime` 会少走一截，首帧可能与解释器路径不同）。
// ---------------------------------------------------------------------------------------
enum class BlockOverrideOutcome : std::uint8_t {
    Replaced = 0,  // 已整段替换：fn 写好了 EIP / 寄存器 / EFLAGS
    Declined = 1,  // 前置条件不匹配（例如不是段入口）⇒ 回退解释器；不计 fallback
    Failed = 2,    // 前置条件匹配但执行失败 ⇒ 回退解释器；计 fallback + 打一行诊断
};

using NativeBlockFn = BlockOverrideOutcome (*)(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns);

struct NativeBlockOverride {
    std::uint32_t entry_ea = 0;   // 固定 EA（`.lst` 锚；必须是一条指令的起点）
    std::uint32_t resume_ea = 0;  // 段的出口 EA（Replaced 后 EIP 落到这里）
    std::string name;
    NativeBlockFn fn = nullptr;
    std::uint64_t hits = 0;
    std::uint64_t declines = 0;
    std::uint64_t fallbacks = 0;
    // ★ 第一次 Declined 之后**永久停用**（本进程内）：段级 override 的前提是静态的
    //   （同一个 EA + 同一段代码），一次不匹配就说明这次运行的形状不是我们认得的那种；
    //   继续尝试只会在热路径上每迭代白付一次函数调用。
    bool disabled = false;
};

class NativeBlockTable {
public:
    static constexpr std::size_t kMax = 4;

    bool add(std::uint32_t entry_ea, std::uint32_t resume_ea, const std::string& name, NativeBlockFn fn,
             std::string* err);
    void clear();
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    NativeBlockOverride* at(std::size_t i) { return i < count_ ? &entries_[i] : nullptr; }
    const NativeBlockOverride* at(std::size_t i) const { return i < count_ ? &entries_[i] : nullptr; }

    // 按 EIP 找**仍然启用**的项（disabled 的项不返回 ⇒ dispatcher 走解释器）。
    NativeBlockOverride* find(std::uint32_t eip);
    const NativeBlockOverride* find(std::uint32_t eip) const;

private:
    std::array<NativeBlockOverride, kMax> entries_{};
    std::size_t count_ = 0;
};

// 已知的 CRT 叶子（按名字解析 EA；EA 来自 `.lst` 的 `proc near` / `COLLAPSED FUNCTION` 行）。
// ★ 分层：`debug == false` 是**稳定**项，`--native-overrides all` 只含这一层；
//   `debug == true` 是保留用于复现/调试的原型，必须**显式点名**才能启用
//   （例如 `--native-overrides all,某个debug项`），⛔ 不会被 `all` 带进来。
struct KnownNative {
    const char* name;
    GuestConv conv;
    std::uint32_t arg_bytes;
    NativeFn fn;
    bool debug = false;
};
const KnownNative* findKnownNative(const std::string& name);
std::size_t knownNativeCount();
const KnownNative& knownNativeAt(std::size_t i);

// ★ 段级项（函数中段）的已知表：`name` 同时是 `--native-overrides` 里写的名字。
//   `enclosing_fn` 只用于**启用时的锚校验**（EA 必须落在这个函数里）——EA 是固定的 `.lst` 锚，
//   ⛔ 不从名字推 EA（中段没有符号）；校验失败 ⇒ 拒绝启用（⛔ 不静默换一个 EA）。
struct KnownBlockNative {
    const char* name;
    const char* enclosing_fn;
    std::uint32_t entry_ea;
    std::uint32_t resume_ea;
    NativeBlockFn fn;
    bool debug = false;
};
const KnownBlockNative* findKnownBlockNative(const std::string& name);
std::size_t knownBlockNativeCount();
const KnownBlockNative& knownBlockNativeAt(std::size_t i);
// 用 `.lst` 校验段级项的锚（entry 是一条真实指令、属于 `enclosing_fn`、出口也是一条指令）。
// 失败 ⇒ false + err（点名哪一条校验没过）。
bool verifyKnownBlockAnchor(const LstIndex& lst, const KnownBlockNative& k, std::string* err);

// ---------------------------------------------------------------------------------------
// thiscall 原语（MSVC x86）：`ECX = this`，栈参数从返回地址之后按 **0 基** 展开。
//
// ★ 为什么单独抽出来：本文件的 thiscall 项越来越多（`sub_436A80` / `sub_48B090` / `sub_429020` /
//   `sub_48AE90` / `sub_487C60` / `sub_46D9F0`）。每个实现各写一遍 `cpu.ecx()` + `cpu.arg(i)` 的话，
//   "第几个 arg 对应第几个形参"就散落在各处、没有一处可复核；顺序错一位不会编译报错，只会在
//   某个分支上静默读错参数。⇒ 把"读参数的顺序"钉在一处：`self` 之后是 `a[0..arg_count-1]`，
//   与 `retn N` 的字节数无关（栈帧的提交状态由 `NativeOverrideTable::invoke()` 统一校验）。
// ★ 只**读**：不改栈、不弹参、不判 ABI。⛔ 不要在这里做安全判断（那是各实现的事）。
// ---------------------------------------------------------------------------------------
struct ThisCallArgs {
    static constexpr std::size_t kMax = 8;  // 本线 thiscall 项最多的栈参数数（sub_46D9F0 = 8）
    std::uint32_t self = 0;                 // ECX
    std::uint32_t a[kMax] = {};             // 栈参数（0 基，`a[0]` = 返回地址之后的第一个 dword）
    std::uint32_t operator[](std::size_t i) const { return i < kMax ? a[i] : 0u; }
};
// `arg_count` 超过 `ThisCallArgs::kMax` 时**截断**（不会越界读；调用方自己保证够用）。
ThisCallArgs thisCall(const X86& cpu, std::size_t arg_count);

// 具体实现（也直接给测试用）
bool nativeMemcpy(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeMemset(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_401000` 的语义是**符号**（`.lst` 401066: `sar eax,1Fh; or eax,1` ⇒ -1/0/+1），
// 不是字节差。`_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节）⇒ 暂不覆盖。
bool nativeSignMemcmp(X86& cpu, GuestMemory& mem, std::uint32_t* result);
bool nativeStrlen(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_436A80`：S4IC / LZSS 解压（profile 选出的 boot 第一热点）。
bool nativeLzss436A80(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_48B090`（NormalTexture::UploadImage）：thiscall（ECX = NormalTexture），栈参数 (img, alpha, key)。
// 准入失败（不支持的源格式 / 色键未实现 / 目标函数认不出）⇒ 返回 false，dispatcher 回退解释器。
bool nativeUpload48B090(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_429020`（Map::findIndex(hash, key)）：thiscall `retn 8`。开放寻址表；槽 28 B = MSVC
// `std::string`（16 B SSO + len@+0x10 + cap@+0x14）；表指针 = `[this+0x18]`、容量 = `[this+8]`；
// 空槽判据 `len==0`；命中 = 先比 `min(len)` 字节、再比长度；不中线性探测；返回 index / -1。
// ★ 只读 guest 内存；所有前置条件（this / 表 / 容量 / index / 提交状态）不成立 ⇒ 返回 false 回退。
bool nativeFindIndex429020(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_487C60`（CMovieTextureRenderer::DoRenderSample，thiscall `retn 4`）：影片样本 → A8R8G8B8 纹理。
// 只覆盖主路径（无 alpha 面 / 无 alpha 拷贝 / 3 或 4 字节源）；其余回退解释器。
bool nativeRender487C60(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// `sub_48AE90`（NormalTexture 虚表第 6 项 = 纯色填充 Fill(rect, color888, alpha)）：thiscall `retn 0Ch`。
// 对 `[self+0x40C]` 的纹理 `LockRect(rect)`（vtable +0x4C）→ 按**虚拟显示器 bpp**（guest 全局
// `ds:dword_5518C0`，⛔ **不是** `ddraw::displayMode`）走 16bpp（A4R4G4B4）或 32bpp →
// `UnlockRect`（+0x50）；返回 0。
bool nativeFill48AE90(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// 纯像素核（只碰宿主缓冲；测试可直接用）。`base` = LockRect 返回的 pBits（**已按 rect.left/top
// 偏移**），`pitch` = LockRect 返回的 Pitch。语义逐字照 `.lst` 的四个分支 —— 包括 16bpp
// `alpha<0` 分支里 `2*(pitch*row/4)`（= pitch 的一半）这个**与 32bpp 同形、但不是 pitch 步进**的
// 写法：⛔ 不要"顺手修正"，那是与 guest 不一致。
void fillRect16(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha);
void fillRect32(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha);
// `sub_46D9F0`：字形位图 → **已锁定**纹理表面的逐像素混合叶子（thiscall `retn 20h`，8 个栈参数 = 32 B）。
// 参数：ECX = T_Font*；a2 = 目标 pitch（字节）· a3 = 目标像素基址（调用方已 Lock）· a4 = 4 个 dword
// （宽/高/原点 x/原点 y）· a5 = 源行跨度 · a6 = 源基址（GetGlyphOutlineA 的字形缓冲）· a7/a8 = 目标 x/y ·
// a9 = 颜色（**低字节是第一通道**，与 `.lst` 0x46DC76 的字节反转一致）。返回 EAX = `(a4[1] > 0) ? a5 : 0`。
// ★ 语义逐字照 `.lst` 0x46D9F0..0x46DEC2（16bpp / 32bpp 两支）；裁剪矩形 = guest `sub_46CE20` 的成功路径
//   （`[mgr+0xA5D8+4*texid]+0x41C..0x428`，mgr=`[Font+0x410]`、texid=`[mgr+0xB648]`）。
// ★ 准入失败（this/mgr/texid/裁剪对象/参数跨度/级数越界…）⇒ 返回 false，dispatcher 回退解释器。
bool nativeBlend46D9F0(X86& cpu, GuestMemory& mem, std::uint32_t* result);
// 源字形格式的选择来自 Font 的两个字段（⛔ 不是宿主"抗锯齿开关"）：
//   `[Font+0x548] == 0` ⇒ 1bpp 位图；非 0 ⇒ 8bpp 级数（每像素 1 字节）。
enum class GlyphMaskSource : std::uint8_t {
    BitMask1 = 0,  // 0x46DAC2..0x46DB08：位序 `7 - col%8`，置位 ⇒ 255
    Level8 = 1,    // 0x46DA89..0x46DAC0：字节 × 255 后除 65 / 17
};
// `[Font+0x35584]`：== 1 ⇒ 65 级（除 65，满覆盖 251）；否则 17 级（除 17，满覆盖 240）。
enum class GlyphLevels : std::uint8_t { Levels17 = 0, Levels65 = 1 };
// 纯像素核的入参（`sub_46D9F0` 的 8 个栈参数 + 裁剪矩形 + 两个格式位）。
// ★ 契约：调用方保证 `[dst, dst + 最后一行偏移 + 像素)` 与 `[src, src + 最后一行偏移 + 行字节)`
//   都在**同一段不回绕**的地址里（`nativeBlend46D9F0` 已经逐行校验过）；核里只做 guest 的
//   32 位地址算术（回绕），不做准入、不做提交检查。
struct GlyphBlit {
    std::uint8_t* dst = nullptr;        // a3（已按 pitch/bits 定位的像素基址，**不**预先偏移 x/y）
    std::int32_t dst_pitch = 0;         // a2（字节）
    std::int32_t width = 0;             // a4[0]
    std::int32_t height = 0;            // a4[1]
    std::int32_t origin_x = 0;          // a4[2]
    std::int32_t origin_y = 0;          // a4[3]
    const std::uint8_t* src = nullptr;  // a6
    std::int32_t src_stride = 0;        // a5（字节；guest 逐行 `add`，可为负）
    std::int32_t dst_x = 0;             // a7
    std::int32_t dst_y = 0;             // a8
    std::uint32_t color = 0;            // a9
    std::int32_t clip_left = 0;         // sub_46CE20 的 rect[0..3]
    std::int32_t clip_top = 0;
    std::int32_t clip_right = 0;
    std::int32_t clip_bottom = 0;
    bool sixteen_bpp = true;            // `ds:dword_5518C0 == 0x10`
    GlyphMaskSource source = GlyphMaskSource::BitMask1;
    GlyphLevels levels = GlyphLevels::Levels17;
};
// 返回 guest 的 EAX（= `(height > 0) ? src_stride : 0`）。⛔ 不做 clamp：`.lst` 里没有 clamp。
std::uint32_t blendGlyph46D9F0(const GlyphBlit& b);

// 段级：`sub_414AC0` 中段填池循环（entry 0x415384 / resume 0x41539F）。
// 语义（`.lst` 0x41537E..0x41539D）：`ecx=[esi-0x10]`（目标数组）、`edx=[ebx+0x5EC90]`（填充值）、
// count = `[ebx+0x5D7E8]+edi+1`、EAX 从 0 起每次写 `[ecx+eax*4]`。完成后 guest 状态：
// EAX=EDX=count、ECX=[ebx+0x5D7E8]、EFLAGS = `cmp eax,edx` 相等的结果（ZF=1/PF=1，其余状态位 0）。
BlockOverrideOutcome nativeFillPool414AC0(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns);
// 纯解码核（测试直接用；src/comp = 输入，dst/out = 输出，ring = 4096 B 字典）。
bool lzssDecode(const std::uint8_t* src, std::uint32_t comp_size, std::uint8_t* dst, std::uint32_t out_size,
                std::uint8_t ring[4096]);
// sub_48B090 的像素核（纯函数：只碰宿主缓冲，测试可以直接用）。
// ★ 语义逐字照 `.lst` 的 `sub_48B090`：
//   - 源是 **bottom-up** DIB（源 row = height-1-目标 row），目标是 **top-down**（行按 `dst_pitch` 前进）；
//   - `alpha`（可为空）**按目标行序线性消费**（guest 在行循环里只 `inc` 指针、不重置）
//     ⇒ index = row*width+col；32bpp 源**不读** alpha（用源像素自带的第 4 字节）；
//   - 每个像素的 32 位值就是内存里的字节序（R 在最低字节、A 在最高字节）；
//   - 8bpp 的调色板项 = 4 字节步长、只用前 3 字节（byte0→bit0 / byte1→bit8 / byte2→bit16），
//     没有 alpha 时补 0xFF。
// 4bpp / 1bpp / 16bpp **不支持**（本线没有证据级用例；`convertImageTo32` 显式拒绝）。
void convertPal8To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                     const std::uint8_t* pal, std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
void convert24To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha);
// 32bpp 源：**逐字节搬运**（guest 0x48B3xx 的 default/32 分支就是 `memcpy(dst, &Src, 4)`），
// alpha 参数不存在 —— 源像素的第 4 字节就是目标 alpha。
void convert32To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch);

// 源像素描述（宿主指针；由调用方从 guest 内存里解析出来）。
struct ImageSource {
    std::uint32_t bpp = 0;                  // 8 / 24 / 32
    const std::uint8_t* pixels = nullptr;   // 源像素首字节（行按 stride、bottom-up）
    std::uint32_t stride = 0;               // 源行字节数（DIB 的 4 字节对齐）
    const std::uint8_t* palette = nullptr;  // 8bpp 才用：256 项 × 4 字节
    const std::uint8_t* alpha = nullptr;    // 8/24bpp 才用：h*w 字节，按目标行序；32bpp 忽略
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// 校验 + 分发到上面三个核。返回 false + why（**不写** dst 之外的任何东西）。
bool convertImageTo32(const ImageSource& src, std::uint8_t* dst, std::uint32_t dst_pitch, std::string* why);

// 把源像素写进一个**已存在**的纹理：走 `d3d9::lockRect` / `unlockRect`（与 guest 0x48B234 /
// 0x48B06C 同一路径），所以 pitch / pBits / 锁定计数 / 已锁拒绝都是那一份语义。
// ★ 前置条件（guest 侧的观察，不是本函数编的）：目标必须是 32bpp、尺寸与 src 相符。
// ★ 失败时不写像素、并保证**不留下锁**（除非锁本来就不是我们加的）。
bool uploadPixelsToTexture(X86& cpu, std::uint32_t texture_object, const ImageSource& src, std::string* why);

// 从 native override 里铺一个 guest 回调帧（必须由 NativeOverrideTable::invoke 的 suspended 机制接住）。
// `callee_ecx` = 进入 callee 前要写进 ECX 的 `this`（thiscall）；0 = 不动。
bool startNestedGuestCall(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                          std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
                          std::uint32_t callee_ecx = 0);

}  // namespace lift
