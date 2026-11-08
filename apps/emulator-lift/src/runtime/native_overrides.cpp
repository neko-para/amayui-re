#include "runtime/native_overrides.h"

#include <cstring>
#include <cstdio>
#include <utility>

#include "runtime/memory.h"
#include "runtime/x86.h"
#include "runtime/d3d9.h"
#include "runtime/ddraw_surface.h"
#include "runtime/machine.h"
#include "runtime/dshow/objects.h"
#include "corpus/lst_index.h"

namespace lift {
namespace {
bool g_nested_started = false;
}

bool startNestedGuestCall(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                          std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
                          std::uint32_t callee_ecx) {
    if (cpu.calls == nullptr) {
        if (err) *err = "native nested call：没有 GuestCallStack";
        return false;
    }
    if (!cpu.calls->start(cpu, fn, args, cleanup_bytes, api, std::move(cont), err, callee_ecx)) return false;
    g_nested_started = true;
    return true;
}

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

// thisCall 原语（见头文件的契约）：只读 `ECX` + `esp+4+4i`，不改任何状态。
// ★ 顺序就是 ABI 顺序：`a[0]` = 返回地址之后的第一个 dword = guest 的第 1 个形参。
ThisCallArgs thisCall(const X86& cpu, std::size_t arg_count) {
    ThisCallArgs f;
    f.self = cpu.ecx();
    if (arg_count > ThisCallArgs::kMax) arg_count = ThisCallArgs::kMax;
    for (std::size_t i = 0; i < arg_count; ++i) f.a[i] = cpu.arg(static_cast<int>(i));
    return f;
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

bool NativeOverrideTable::invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why,
                                 bool* suspended) {
    if (suspended) *suspended = false;
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
    g_nested_started = false;
    if (!ov.fn(cpu, mem, &result)) {
        if (g_nested_started) {
            // native 已经铺好 guest 回调帧；EIP/ESP 由 GuestCallStack 设置，不要回退。
            if (suspended) *suspended = true;
            ++ov.hits;
            return true;
        }
        if (why) *why = "native 实现要求回退（参数越界 / safe mode）";
        ++ov.fallbacks;
        return false;
    }
    cpu.eax() = result;
    cpu.eip = ret;
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

// ---------------------------------------------------------------------------------------
// sub_48B090（NormalTexture::UploadImage）：源像素 → 目标纹理的 32bpp 上传
//
// ★ 逐字依据（`.lst` 0x48B090..0x48B427 / 0x48B470..0x48B538；`.c` 只作阅读辅助，不当锚）：
//   · 目标尺寸 = img+0x18 / img+0x1C；源像素 = img+0x43C；源 bpp = u16 img+0x22；
//     调色板 = img+0x3C 起、每项 4 字节（只用前 3 字节：byte0→bit0 / byte1→bit8 / byte2→bit16）；
//   · 源行自**下**往上（0x48B476 每行 `sub [ebp-40Ch], stride`），目标行自**上**往下
//     （0x48B516 每行 `add v78, pitch`）⇒ 源 row = height-1-目标 row；
//   · 目标 pitch / 像素指针**只从 LockRect 的 D3DLOCKED_RECT 取**（0x48B234 把 [ebp-484] 传进去，
//     之后 v78 = v59[1]、行步长 = v59[0]），⛔ 不自己按 width 推；
//   · alpha（arg1）**按目标行序线性消费**（0x48B49F `movzx eax, byte ptr [edi]` + `inc edi`，
//     行循环里不重置）⇒ index = row*cols + col；32bpp 源**不读** alpha（0x48B31x 起的
//     default/32 分支里根本没有 a4）；
//   · 目标 bpp 由 guest 的 `display:TextureBit` 决定（0x48B0E0）：本线只实现 32bpp ⇒ 16bpp 目标
//     显式失败（⛔ 不猜着写，见 uploadPixelsToTexture）。
//   · 4bpp / 1bpp 源：guest 有分支（0x48B543 / 0x48B812），但本线没有证据级用例 ⇒ 显式拒绝，
//     ⛔ 不留"看起来能跑"的死分支。
// ---------------------------------------------------------------------------------------
namespace {

// guest 0x48B264..0x48B2B2：DIB 行跨度 = ((w*bpp + 31) / 32) * 4（4 字节对齐）。
std::uint32_t dibStride(std::uint32_t w, std::uint32_t bpp) { return ((w * bpp + 31u) / 32u) * 4u; }

// 32bpp 目标的一个像素（内存序：最低字节 = 源第 0 字节）。
void storePixel(std::uint8_t* dst, std::uint32_t value) { std::memcpy(dst, &value, 4); }

}  // namespace

void convertPal8To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                     const std::uint8_t* pal, std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha) {
    if (src == nullptr || pal == nullptr || dst == nullptr) return;
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t srow = h - 1u - row;
        const std::uint8_t* s = src + static_cast<std::size_t>(srow) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * pitch;
        // ★ alpha 走**目标**行序（guest 的 a4 指针跨行连续 `inc`，行循环不重置）。
        const std::uint8_t* a = alpha != nullptr ? alpha + static_cast<std::size_t>(row) * w : nullptr;
        for (std::uint32_t col = 0; col < w; ++col) {
            const std::uint8_t* p = pal + static_cast<std::size_t>(s[col]) * 4u;
            const std::uint32_t av = a != nullptr ? a[col] : 0xFFu;
            storePixel(d + static_cast<std::size_t>(col) * 4u,
                       (av << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
                           (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[0]));
        }
    }
}

void convert24To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch, const std::uint8_t* alpha) {
    if (src == nullptr || dst == nullptr) return;
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t srow = h - 1u - row;
        const std::uint8_t* s = src + static_cast<std::size_t>(srow) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * pitch;
        const std::uint8_t* a = alpha != nullptr ? alpha + static_cast<std::size_t>(row) * w : nullptr;
        for (std::uint32_t col = 0; col < w; ++col) {
            const std::uint8_t* p = s + static_cast<std::size_t>(col) * 3u;
            const std::uint32_t av = a != nullptr ? a[col] : 0xFFu;
            storePixel(d + static_cast<std::size_t>(col) * 4u,
                       (av << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
                           (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[0]));
        }
    }
}

void convert32To32(const std::uint8_t* src, std::uint32_t stride, std::uint32_t w, std::uint32_t h,
                   std::uint8_t* dst, std::uint32_t pitch) {
    if (src == nullptr || dst == nullptr) return;
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t srow = h - 1u - row;
        const std::uint8_t* s = src + static_cast<std::size_t>(srow) * stride;
        std::uint8_t* d = dst + static_cast<std::size_t>(row) * pitch;
        // guest 的 32bpp 分支就是逐字节 4 B 搬运（0x48B3D3 之后 `memcpy(v80, &Src, v17)`，v17 = 4）。
        std::memcpy(d, s, static_cast<std::size_t>(w) * 4u);
    }
}

bool convertImageTo32(const ImageSource& src, std::uint8_t* dst, std::uint32_t dst_pitch, std::string* why) {
    auto bad = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (dst == nullptr) return bad("目标像素指针为空");
    if (src.pixels == nullptr) return bad("源像素指针为空");
    if (src.width == 0 || src.height == 0) return bad("图像尺寸为 0");
    if (src.bpp != 8 && src.bpp != 24 && src.bpp != 32)
        return bad("源 bpp 只实现 8 / 24 / 32（4 / 1 / 16bpp 显式不支持）");
    if (src.bpp == 8 && src.palette == nullptr) return bad("8bpp 源没有调色板");
    const std::uint64_t row_bytes = (static_cast<std::uint64_t>(src.width) * src.bpp + 7u) / 8u;
    if (src.stride < row_bytes) return bad("源行跨度小于一行像素（stride 与 bpp/width 不符）");
    if (dst_pitch < static_cast<std::uint64_t>(src.width) * 4u) return bad("目标 pitch 小于 4*width（目标不是 32bpp）");
    switch (src.bpp) {
        case 8:
            convertPal8To32(src.pixels, src.stride, src.width, src.height, src.palette, dst, dst_pitch, src.alpha);
            break;
        case 24:
            convert24To32(src.pixels, src.stride, src.width, src.height, dst, dst_pitch, src.alpha);
            break;
        default:
            // ★ 32bpp 源：alpha 参数**被忽略**（guest 用源像素的第 4 字节）。
            convert32To32(src.pixels, src.stride, src.width, src.height, dst, dst_pitch);
            break;
    }
    return true;
}

bool uploadPixelsToTexture(X86& cpu, std::uint32_t texture_object, const ImageSource& src, std::string* why) {
    auto bad = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (texture_object == 0) return bad("目标纹理对象为 0");
    if (cpu.mem == nullptr) return bad("cpu.mem 为空");
    d3d9::TextureInfo ti;
    if (!d3d9::textureInfo(cpu, texture_object, &ti, why)) return false;
    if (ti.bpp != 32 || ti.arena == 0)
        return bad("目标纹理不是 32bpp（display:TextureBit=16 的 16bpp 上传本线未实现）");
    if (ti.width != src.width || ti.height != src.height)
        return bad("目标纹理尺寸与源图像尺寸不一致（guest 的 Create 与 img 字段不符）");
    if (ti.locked) return bad("目标纹理已处于锁定态（guest 的 LockRect 前置条件不成立）");

    // D3DLOCKED_RECT 落在 guest 栈的**空闲处**（调用方 esp 之下 8 B）。
    // ★ 为什么安全：native override 在函数入口替换掉整个 `sub_48B090`，它的序言（`push ebp` /
    //   `sub esp,488h` / /GS cookie 槽）**从不执行** ⇒ esp 之下这一段没有被 guest 占用的帧槽，
    //   而且这里写完立刻读回、中间不跑 guest 代码。
    const std::uint32_t esp = cpu.esp();
    if (esp < 8u || !cpu.mem->committed(esp - 8u, 8u)) return bad("guest 栈放不下 D3DLOCKED_RECT");
    const std::uint32_t locked_rect_ea = esp - 8u;
    if (!d3d9::lockRect(cpu, ti, 0, locked_rect_ea, 0, 0, why)) return false;
    const std::uint32_t pitch = cpu.mem->rd32(locked_rect_ea + 0u);
    const std::uint32_t bits = cpu.mem->rd32(locked_rect_ea + 4u);

    // ★ 失败路径：**一定**把锁还回去（否则下一次上传/绘制会撞"已锁"），且不写像素。
    auto unlock = [&]() {
        d3d9::TextureInfo fresh;
        if (d3d9::textureInfo(cpu, texture_object, &fresh, nullptr)) {
            std::string ignored;
            d3d9::unlockRect(cpu, fresh, 0, &ignored);
        }
    };

    // guest 的列数 = `min(width, Pitch)` —— 两个量纲不同（0x48B2F4 拿 width 和 Lock 的**字节**
    // pitch 比大小）。本线只在 `pitch >= 4*width` 时接手（那正是 pitch 正常时的形状），
    // 此时 min 恒等于 width；pitch 更小 = 目标不是 32bpp / 布局异常 ⇒ 拒绝而不是照 guest 截断。
    if (pitch < 4u * src.width) {
        unlock();
        return bad("Lock 的 Pitch 小于 4*width（目标布局不是 32bpp top-down）");
    }
    const std::uint64_t need = static_cast<std::uint64_t>(pitch) * src.height;
    if (bits == 0 || static_cast<std::uint64_t>(bits) + need > GuestMemory::kSpace ||
        !cpu.mem->committed(bits, static_cast<std::size_t>(need))) {
        unlock();
        return bad("Lock 返回的像素区间不可读（pBits 越界 / 未提交）");
    }
    if (!convertImageTo32(src, cpu.mem->host(bits), pitch, why)) {
        unlock();
        return false;
    }
    unlock();
    return true;
}

bool nativeUpload48B090(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t self = cpu.ecx();
    const std::uint32_t img = cpu.arg(0);
    const std::uint32_t alpha = cpu.arg(1);
    const std::uint32_t key = cpu.arg(2);
    *result = 0;
    if (self == 0 || img == 0) return false;
    // ── ① 不支持的实参：**在产生任何 guest 副作用之前**回退解释器 ──────────────────────────
    //   · key ≠ 0：guest 的色键语义按源格式 / alpha 组合比较（0x48B3BB / 0x48B4A8 / 0x48B5FB），
    //     本线没实现 ⇒ 回退，让 guest 自己算（⛔ 不静默忽略色键）。
    //   · 源 bpp 4 / 1：guest 有分支但没有证据级用例 ⇒ 回退（下面按 bpp 拒）。
    if (key != 0) return false;
    if (!mem.committed(self, 4) || !mem.committed(self + 0x40C, 4) || !mem.committed(img + 0x18, 0x428))
        return false;
    // ★ 这一步的 w / h / bpp / src 只用于**准入**（"要不要接手"），不参与像素写入：
    //   一旦 startNestedGuestCall() 铺了帧就不能回退，而"新纹理已建、像素未写"是不可接受的半状态。
    //   真正喂给转换的值在续延里 Create **之后**重读（guest 也是 0x48B24F / 0x48B2DC 才读）。
    const std::uint32_t w = mem.rd32(img + 0x18);
    const std::uint32_t h = mem.rd32(img + 0x1C);
    const std::uint32_t bpp_admit = mem.rd16(img + 0x22);
    const std::uint32_t src_admit = mem.rd32(img + 0x43C);
    if (w == 0 || h == 0 || w > d3d9::kMaxTextureWidth || h > d3d9::kMaxTextureHeight) return false;
    if (bpp_admit != 8 && bpp_admit != 24 && bpp_admit != 32) return false;
    const std::uint32_t stride = dibStride(w, bpp_admit);
    const std::uint64_t need = static_cast<std::uint64_t>(stride) * h;
    if (src_admit == 0 || need > GuestMemory::kSpace ||
        static_cast<std::uint64_t>(src_admit) + need > GuestMemory::kSpace ||
        !mem.committed(src_admit, static_cast<std::size_t>(need)))
        return false;
    if (bpp_admit == 8 && !mem.committed(img + 0x3C, 1024)) return false;
    if (alpha != 0 && bpp_admit != 32) {
        // 8 / 24bpp 才消费 alpha（按目标行序 h*w 字节）；32bpp 的 alpha 参数 guest 根本不读。
        const std::uint64_t n = static_cast<std::uint64_t>(w) * h;
        if (n > GuestMemory::kSpace || static_cast<std::uint64_t>(alpha) + n > GuestMemory::kSpace ||
            !mem.committed(alpha, static_cast<std::size_t>(n)))
            return false;
    }
    // ★ 目标显示位深必须在 Create **之前**判掉：16bpp 目标走到 Create 之后才失败会留下
    //   "新纹理已建、像素没写"的半状态（且无法回退）。用 DDraw/D3D9 共用的虚拟显示器状态
    //   （`sub_48B090` 的 `dword_5518C0` 就是它），避免再硬编码 guest EA；读不到 / 不是 32bpp
    //   ⇒ 回退解释器。
    {
        int display_w = 0;
        int display_h = 0;
        unsigned display_bpp = 0;
        if (!ddraw::displayMode(cpu, &display_w, &display_h, &display_bpp, nullptr) || display_bpp != 32)
            return false;
    }
    // ── ② 目标函数 = `[[self] + 0xC]`（guest 0x48B0A6/A8/B0：`mov edx,[ecx]; mov edx,[edx+0Ch]`），
    //   ⛔ 不按名字硬编码 `sub_48AC40`：类布局 / 版本一变就会静默调错。只接受**`.lst` 里认得的
    //   函数入口**（`functionAt` 的起点必须等于它），否则回退解释器（guest 自己 call 同样是这个地址）。
    const std::uint32_t vtbl = mem.rd32(self);
    if (vtbl == 0 || !mem.committed(vtbl + 0x0C, 4)) return false;
    const std::uint32_t create_ea = mem.rd32(vtbl + 0x0C);
    if (create_ea == 0 || !mem.committed(create_ea, 1)) return false;
    Machine* machine = activeMachine();
    if (machine == nullptr || !machine->lst().loaded()) return false;
    const LstFunction* create_fn = machine->lst().functionAt(create_ea);
    if (create_fn == nullptr || create_fn->ea != create_ea) return false;

    GuestMemory* m = &mem;
    const std::uint32_t self_ea = self;
    const std::uint32_t img_ea = img;
    const std::uint32_t alpha_ea = alpha;
    std::string call_err;
    const bool started = startNestedGuestCall(
        cpu, create_ea, {w, h, 0}, 16, "native:NormalTexture::UploadImage",
        [m, self_ea, img_ea, alpha_ea, w, h](X86& c, std::uint32_t create_hr, std::uint32_t* out) {
            *out = 0;
            // guest 0x48B0C8：Create 的非 0 返回 ⇒ `return 0`（纹理没建出来，什么都不用做）。
            if (create_hr != 0) return true;
            // ★ guest 在 Create **之后**才读源描述（0x48B24F 读 bpp / 0x48B2DC 读源指针）：
            //   这里也重读，喂给转换的是这一刻的值（与 guest 的控制流同序）。
            //   w / h 相反：guest 在调 Create **之前**就读（0x48B0A3..AF），所以沿用准入时的值。
            const std::uint32_t bpp = m->rd16(img_ea + 0x22);
            const std::uint32_t src_ea = m->rd32(img_ea + 0x43C);
            const std::uint32_t tex = m->rd32(self_ea + 0x40C);
            const std::uint32_t stride = dibStride(w, bpp);
            std::string why;
            auto fail = [&](const char* stage) {
                // ★ 已经 suspend，**不能**再回退解释器。选返回 0（guest 的"上传失败"返回值）而不是
                //   1：返回 1 = 声称成功却没写像素（静默的坏纹理）。纹理本身是 guest 的 Create
                //   建出来的，这一点与 guest 自己的"Create 成功但上传提前 return 0"（0x48B0F6）同形。
                std::fprintf(stderr, "[native] sub_48B090 续延失败（%s）⇒ 返回 0：%s\n", stage, why.c_str());
                *out = 0;
                return true;
            };
            if (tex == 0) {
                why = "Create 之后 self+0x40C 仍为 0";
                return fail("取纹理对象");
            }
            const std::uint64_t need = static_cast<std::uint64_t>(stride) * h;
            if ((bpp != 8 && bpp != 24 && bpp != 32) || src_ea == 0 || need > GuestMemory::kSpace ||
                static_cast<std::uint64_t>(src_ea) + need > GuestMemory::kSpace ||
                !m->committed(src_ea, static_cast<std::size_t>(need))) {
                why = "Create 之后源像素描述变了（bpp / 指针 / 提交状态）";
                return fail("读源描述");
            }
            if (bpp == 8 && !m->committed(img_ea + 0x3C, 1024)) {
                why = "调色板不在已提交内存里";
                return fail("读调色板");
            }
            if (alpha_ea != 0 && bpp != 32) {
                const std::uint64_t n = static_cast<std::uint64_t>(w) * h;
                if (static_cast<std::uint64_t>(alpha_ea) + n > GuestMemory::kSpace ||
                    !m->committed(alpha_ea, static_cast<std::size_t>(n))) {
                    why = "alpha 缓冲不在已提交内存里";
                    return fail("读 alpha");
                }
            }
            ImageSource s;
            s.bpp = bpp;
            s.pixels = m->host(src_ea);
            s.stride = stride;
            s.palette = bpp == 8 ? m->host(img_ea + 0x3C) : nullptr;
            s.alpha = bpp != 32 && alpha_ea != 0 ? m->host(alpha_ea) : nullptr;
            s.width = w;
            s.height = h;
            if (!uploadPixelsToTexture(c, tex, s, &why)) return fail("写像素");
            *out = 1;
            return true;
        },
        &call_err, /*callee_ecx=*/self);
    if (!started) {
        // ★ 不吞 call_err：铺帧失败的原因直接进诊断（invoke() 的 why 只有一句通用的）。
        std::fprintf(stderr, "[native] sub_48B090 铺 guest 回调帧失败（回退解释器）：%s\n", call_err.c_str());
        return false;
    }
    return false;  // invoke() 看到 g_nested_started ⇒ suspended
}

// ---------------------------------------------------------------------------------------
// 段级 override 表（函数中段；见头文件的口径）
// ---------------------------------------------------------------------------------------

bool NativeBlockTable::add(std::uint32_t entry_ea, std::uint32_t resume_ea, const std::string& name,
                           NativeBlockFn fn, std::string* err) {
    if (count_ >= kMax) {
        if (err) *err = "段级 native override 表已满（kMax=" + std::to_string(kMax) + "）";
        return false;
    }
    if (fn == nullptr) {
        if (err) *err = "段级 native override 没有实现：" + name;
        return false;
    }
    if (entry_ea == 0 || resume_ea == 0) {
        if (err) *err = "段级 native override 的 entry/resume EA 为 0：" + name;
        return false;
    }
    if (entry_ea == resume_ea) {
        if (err) *err = "段级 native override 的 entry == resume（会原地打转）：" + name;
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == entry_ea) {
            if (err) *err = "段级 native override 重复 entry EA：" + name;
            return false;
        }
    }
    NativeBlockOverride& e = entries_[count_++];
    e.entry_ea = entry_ea;
    e.resume_ea = resume_ea;
    e.name = name;
    e.fn = fn;
    e.hits = 0;
    e.declines = 0;
    e.fallbacks = 0;
    e.disabled = false;
    return true;
}

void NativeBlockTable::clear() {
    for (std::size_t i = 0; i < count_; ++i) entries_[i] = NativeBlockOverride{};
    count_ = 0;
}

NativeBlockOverride* NativeBlockTable::find(std::uint32_t eip) {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == eip && !entries_[i].disabled) return &entries_[i];
    }
    return nullptr;
}

const NativeBlockOverride* NativeBlockTable::find(std::uint32_t eip) const {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == eip && !entries_[i].disabled) return &entries_[i];
    }
    return nullptr;
}

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

// ---------------------------------------------------------------------------------------
// `sub_48AE90`：NormalTexture 虚表第 6 项 = Fill(rect, color888, alpha)
//
// ★ 逐字依据（`.lst` 0x48AE90..0x48B087；thiscall retn 0Ch）：
//   · 0x48AE96 `mov eax,[ecx+40Ch]` ⇒ 纹理对象；0x48AEAF `mov eax,[ecx+4Ch]` 是 **vtable +0x4C**，
//     参数 (this=纹理, Level=0, &D3DLOCKED_RECT, pRect=a2, Flags=0) ⇒ `LockRect(rect)`；
//     0x48AEB4 `test eax,eax; jnz` ⇒ LockRect 失败时**原样返回 HRESULT**（本线回退让 guest 自己做）。
//   · 0x48AEBC `cmp ds:dword_5518C0,10h` ⇒ 虚拟显示器 16bpp 走 A4R4G4B4 分支，否则 32bpp 分支。
//   · 16bpp：0x48AECB..0x48AEF1 把 color888 折成 `R4<<8|G4<<4|B4`；`a4>=0` 时
//     `0x48AF59: and ecx,0FFF0h; shl ecx,8; or ecx,eax` ⇒ A4 = a4 高 4 位；
//     `a4<0` 时 `0x48AF35..0x48AF3D: and dx,0F000h; or dx,arg_4` ⇒ **保留**原有 A4。
//     ★ 行首偏移：`a4>=0` 是 `2*((pitch*row)/2)`（0x48AF76），`a4<0` 是 `2*((pitch*row)/4)`
//       （0x48AF16）—— 后者等于 pitch 的一半，**与 32bpp 同形但不是 pitch 步进**。
//       ⛔ 这是 guest 的真实写法，逐字照抄；不要"顺手修正"。
//   · 32bpp：`4*((pitch*row)/4)`（0x48AFE0 / 0x48B037）；`a4>=0` ⇒ `(a4<<24)|(color&0xFFFFFF)`，
//     `a4<0` ⇒ `(color&0xFFFFFF)|(old&0xFF000000)`（0x48B052）。
//   · 0x48B06C 起 `UnlockRect`（vtable +0x50），然后 `xor eax,eax` ⇒ **返回 0**（不检查 unlock 结果）。
// ★ 宽 / 高 = rect.right-left / rect.bottom-top；rect 由调用方保证（LockRect 先校验，失败即返回）。
// ---------------------------------------------------------------------------------------
namespace {

// guest 的 `imul` 是 32 位回绕；`cdq; and edx,N; add; sar` 是 C 的**向 0 截断**除法。
std::int32_t wrapMul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
std::int32_t cDiv(std::int32_t v, std::int32_t d) { return v / d; }  // C++11：向 0 截断

std::uint16_t rgb888ToA4R4G4B4(std::uint32_t color888) {
    // 0x48AED2..0x48AEF1：((c>>12)&0xF00) | ((c>>8)&0xF0) | ((c>>4)&0x0F)
    return static_cast<std::uint16_t>(((color888 >> 12) & 0xF00u) | ((color888 >> 8) & 0xF0u) |
                                      ((color888 >> 4) & 0x0Fu));
}

// 一个分支的行首字节偏移（= guest 的 `lea eax,[edx+eax*N]`），逐字照 `.lst`。
std::int32_t rowOffset16(std::int32_t pitch, std::int32_t row, bool preserve_alpha) {
    const std::int32_t mul = wrapMul(pitch, row);
    return 2 * (preserve_alpha ? cDiv(mul, 4) : cDiv(mul, 2));
}
std::int32_t rowOffset32(std::int32_t pitch, std::int32_t row) { return 4 * cDiv(wrapMul(pitch, row), 4); }

}  // namespace

void fillRect16(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha) {
    if (base == nullptr || width <= 0 || height <= 0) return;
    const std::uint16_t rgb = rgb888ToA4R4G4B4(color888);
    if (alpha >= 0) {
        const std::uint16_t word =
            static_cast<std::uint16_t>(rgb | (static_cast<std::uint32_t>(alpha & 0xFFF0) << 8));
        for (std::int32_t row = 0; row < height; ++row) {
            std::uint8_t* p = base + rowOffset16(pitch, row, /*preserve_alpha=*/false);
            for (std::int32_t col = 0; col < width; ++col) std::memcpy(p + 2 * col, &word, 2);
        }
    } else {
        for (std::int32_t row = 0; row < height; ++row) {
            std::uint8_t* p = base + rowOffset16(pitch, row, /*preserve_alpha=*/true);
            for (std::int32_t col = 0; col < width; ++col) {
                std::uint16_t v = 0;
                std::memcpy(&v, p + 2 * col, 2);
                v = static_cast<std::uint16_t>((v & 0xF000u) | rgb);  // 0x48AF35..0x48AF3D
                std::memcpy(p + 2 * col, &v, 2);
            }
        }
    }
}

void fillRect32(std::uint8_t* base, std::int32_t pitch, std::int32_t width, std::int32_t height,
                std::uint32_t color888, std::int32_t alpha) {
    if (base == nullptr || width <= 0 || height <= 0) return;
    const std::uint32_t rgb = color888 & 0xFFFFFFu;
    if (alpha >= 0) {
        const std::uint32_t word = (static_cast<std::uint32_t>(alpha) << 24) | rgb;
        for (std::int32_t row = 0; row < height; ++row) {
            std::uint8_t* p = base + rowOffset32(pitch, row);
            for (std::int32_t col = 0; col < width; ++col) std::memcpy(p + 4 * col, &word, 4);
        }
    } else {
        for (std::int32_t row = 0; row < height; ++row) {
            std::uint8_t* p = base + rowOffset32(pitch, row);
            for (std::int32_t col = 0; col < width; ++col) {
                std::uint32_t v = 0;
                std::memcpy(&v, p + 4 * col, 4);
                v = (v & 0xFF000000u) | rgb;
                std::memcpy(p + 4 * col, &v, 4);
            }
        }
    }
}

bool nativeFill48AE90(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    const std::uint32_t self = cpu.ecx();
    const std::uint32_t rect_ea = cpu.arg(0);
    const std::uint32_t color = cpu.arg(1);
    const std::int32_t alpha = static_cast<std::int32_t>(cpu.arg(2));
    *result = 0;
    if (self == 0 || rect_ea == 0) return false;
    if (static_cast<std::uint64_t>(self) + 0x410u > GuestMemory::kSpace) return false;
    if (!mem.committed(self + 0x40Cu, 4u)) return false;
    const std::uint32_t tex_obj = mem.rd32(self + 0x40Cu);
    if (tex_obj == 0) return false;
    // ── ① 全部前置条件在**加锁之前**判完（加锁后失败会留下 locks/version 的额外增量）─────
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfo(cpu, tex_obj, &ti, &why)) return false;
    if (ti.bpp != 16 && ti.bpp != 32) return false;
    if (ti.pitch == 0) return false;
    if (!mem.committed(rect_ea, 16u)) return false;
    const std::int32_t left = static_cast<std::int32_t>(mem.rd32(rect_ea + 0u));
    const std::int32_t top = static_cast<std::int32_t>(mem.rd32(rect_ea + 4u));
    const std::int32_t right = static_cast<std::int32_t>(mem.rd32(rect_ea + 8u));
    const std::int32_t bottom = static_cast<std::int32_t>(mem.rd32(rect_ea + 12u));
    // 与 d3d9::lockRect 同一套矩形校验：不合法 ⇒ 回退（guest 自己会拿到 HRESULT 并原样返回）。
    if (left < 0 || top < 0 || right > static_cast<std::int32_t>(ti.width) ||
        bottom > static_cast<std::int32_t>(ti.height) || right <= left || bottom <= top)
        return false;
    // 显示/纹理位深 = guest 的 `ds:dword_5518C0`。★ 它**不是** DDraw 的显示 bpp：
    //   `.lst` 0x406B14 是**唯一**写点（`mov ds:dword_5518C0, eax`，值来自配置
    //   `display:TextureBit`），0x48AEBC 这里读它。⛔ 不要换成 ddraw::displayMode ——
    //   那是另一件事（本线曾按它实现，A/B 恰好同值时看不出来）。
    constexpr std::uint32_t kTextureBitEa = 0x005518C0u;
    if (!mem.committed(kTextureBitEa, 4u)) return false;
    const std::uint32_t display_bpp = mem.rd32(kTextureBitEa);
    if (display_bpp != 16 && display_bpp != 32) return false;
    const std::int32_t width = right - left;
    const std::int32_t height = bottom - top;
    const std::int32_t pixel_bytes = static_cast<std::int32_t>(display_bpp / 8u);  // guest 的 store 宽度
    // pBits 的 left 偏移用**纹理**的 bpp（= `d3d9::lockRect` 里的 `gs->format.bytesPerPixel()`；
    // `createTextureState` 用同一个 D3DFORMAT 建 arena 与 `kTxBpp`，所以 `ti.bpp/8` 与它逐字相等）。
    const std::int32_t tex_pixel_bytes = static_cast<std::int32_t>(ti.bpp / 8u);
    // 预测 LockRect 会返回的 pBits（与 d3d9::lockRect 的公式逐字一致），先校验范围。
    const std::uint64_t predicted_bits = static_cast<std::uint64_t>(ti.arena) +
                                         static_cast<std::uint64_t>(static_cast<std::uint32_t>(top)) * ti.pitch +
                                         static_cast<std::uint64_t>(static_cast<std::uint32_t>(left)) *
                                             static_cast<std::uint32_t>(tex_pixel_bytes);
    if (predicted_bits > GuestMemory::kSpace) return false;
    const std::int32_t last_off =
        display_bpp == 16 ? rowOffset16(static_cast<std::int32_t>(ti.pitch), height - 1, alpha < 0)
                          : rowOffset32(static_cast<std::int32_t>(ti.pitch), height - 1);
    if (last_off < 0) return false;
    const std::uint64_t span = static_cast<std::uint64_t>(last_off) +
                               static_cast<std::uint64_t>(pixel_bytes) * static_cast<std::uint64_t>(width);
    if (predicted_bits + span > GuestMemory::kSpace) return false;
    if (!mem.committed(static_cast<std::uint32_t>(predicted_bits), static_cast<std::size_t>(span))) return false;
    // ── ② 加锁（D3DLOCKED_RECT 放在 esp 之下 8 B：guest 的序言从不执行）────────────────────
    const std::uint32_t esp = cpu.esp();
    if (esp < 8u || !mem.committed(esp - 8u, 8u)) return false;
    const std::uint32_t locked_rect_ea = esp - 8u;
    if (!d3d9::lockRect(cpu, ti, 0u, locked_rect_ea, rect_ea, 0u, &why)) return false;
    const std::uint32_t pitch = mem.rd32(locked_rect_ea + 0u);
    const std::uint32_t bits = mem.rd32(locked_rect_ea + 4u);
    auto unlock = [&]() {
        d3d9::TextureInfo fresh;
        if (d3d9::textureInfo(cpu, tex_obj, &fresh, nullptr)) {
            std::string ignored;
            d3d9::unlockRect(cpu, fresh, 0u, &ignored);
        }
    };
    // LockRect 的返回**必须**与预测一致（d3d9::lockRect 的 pitch/pBits 就是 ti.pitch / 上面的公式；
    // 不一致 = 状态块与预测漂了 ⇒ 解锁后回退）。★ 解锁仍会让 locks/version +1（这条路径不该出现）。
    if (pitch != ti.pitch || bits != static_cast<std::uint32_t>(predicted_bits)) {
        unlock();
        std::fprintf(stderr,
                     "[native] sub_48AE90：LockRect 返回 pitch/pBits 与预测不一致（%u/%u · %08X/%08X）⇒ 回退\n",
                     pitch, ti.pitch, bits, static_cast<std::uint32_t>(predicted_bits));
        return false;
    }
    // ── ③ 像素核（纯函数；同一份代码也在测试里直接跑）────────────────────────────────────
    if (display_bpp == 16) {
        fillRect16(mem.host(bits), static_cast<std::int32_t>(pitch), width, height, color, alpha);
    } else {
        fillRect32(mem.host(bits), static_cast<std::int32_t>(pitch), width, height, color, alpha);
    }
    unlock();
    *result = 0;
    return true;
}

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

// ---------------------------------------------------------------------------------------
// 段级：`sub_414AC0` 的填池循环（`.lst` 0x415384..0x41539D）
//
// ★ 逐字依据（`pnpm tools disasm-at at --ea 0x415340 --lines 45`）：
//     xor  eax,eax                 ; 0x41537E 循环前
//     test edx,edx / jle 0x41539F  ; 0x415380（count<=0 时整段跳过）
//   loc_415384:
//     mov  ecx,[esi-10h]           ; 目标数组（前面 VirtualAlloc 的结果）
//     mov  edx,[ebx+5EC90h]        ; 填充值
//     mov  [ecx+eax*4],edx
//     mov  ecx,[ebx+5D7E8h]
//     inc  eax
//     lea  edx,[ecx+edi+1]         ; count = [ebx+5D7E8] + edi + 1
//     cmp  eax,edx
//     jl   loc_415384              ; **唯一**回边（全语料只有这里引用 0x415384）
//   loc_41539F:
//     mov  eax,[ebx+5D7ECh]        ; 循环后不读 EFLAGS
// ★ `0x415384` 只被 `sub_414AC0` 的这次循环引用（`Select-String 415384` 只有 1 条 `jl`），
//   而 `sub_414AC0` 只有一个调用方 `_WinMain@16` ⇒ 按 EIP 拦是安全的（用户前提：guest 代码只读）。
// ---------------------------------------------------------------------------------------
BlockOverrideOutcome nativeFillPool414AC0(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns) {
    *replaced_insns = 0;
    // 段入口判据：`xor eax,eax` 之后才是 0；回边（`jl`）进来时 EAX>=1 ⇒ 只尝试一次。
    if (cpu.eax() != 0) return BlockOverrideOutcome::Declined;
    const std::uint32_t esi = cpu.esi();
    const std::uint32_t ebx = cpu.ebx();
    const std::uint32_t edi = cpu.edi();
    if (esi < 0x10u) return BlockOverrideOutcome::Declined;
    if (static_cast<std::uint64_t>(ebx) + 0x5EC94u > GuestMemory::kSpace) return BlockOverrideOutcome::Declined;
    if (!mem.committed(esi - 0x10u, 4u) || !mem.committed(ebx + 0x5D7E8u, 4u) ||
        !mem.committed(ebx + 0x5EC90u, 4u))
        return BlockOverrideOutcome::Declined;
    const std::uint32_t target = mem.rd32(esi - 0x10u);
    const std::uint32_t value = mem.rd32(ebx + 0x5EC90u);
    const std::uint32_t base = mem.rd32(ebx + 0x5D7E8u);
    const std::uint32_t count_u = base + edi + 1u;  // guest 的 `lea edx,[ecx+edi+1]` 是 32 位回绕
    const std::int32_t count = static_cast<std::int32_t>(count_u);
    if (count <= 0) return BlockOverrideOutcome::Declined;  // `test edx,edx; jle 0x41539F` ⇒ 不执行循环
    const std::uint64_t bytes = static_cast<std::uint64_t>(static_cast<std::uint32_t>(count)) * 4u;
    if (target == 0) return BlockOverrideOutcome::Failed;
    if (static_cast<std::uint64_t>(target) + bytes > GuestMemory::kSpace) return BlockOverrideOutcome::Failed;
    if (!mem.committed(target, static_cast<std::size_t>(bytes))) return BlockOverrideOutcome::Failed;
    std::uint8_t* p = mem.host(target);
    for (std::int32_t i = 0; i < count; ++i) {
        std::memcpy(p + 4ull * static_cast<std::uint32_t>(i), &value, 4);
    }
    // 循环尾的 guest 状态（EAX=EDX=count，ECX=[ebx+5D7E8]；EFLAGS = `cmp eax,edx` 相等：
    // ZF=1 / PF=1 / CF=AF=SF=OF=0；0x41539F 之后不读标志，但差分测试会逐位比）。
    cpu.eax() = count_u;
    cpu.edx() = count_u;
    cpu.ecx() = base;
    cpu.eflags = (cpu.eflags & ~(FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF)) | FLAG_PF | FLAG_ZF;
    cpu.eip = 0x0041539Fu;
    // 每次迭代 = 8 条指令（mov/mov/mov/mov/inc/lea/cmp/jl，最后那次 jl 不跳也算执行）
    *replaced_insns = 8ull * static_cast<std::uint64_t>(count_u);
    return BlockOverrideOutcome::Replaced;
}

// ---------------------------------------------------------------------------------------
// 已知名字表：名字 → ABI + 实现（EA 由 `.lst` 解析，⛔ 不硬编码）
// ---------------------------------------------------------------------------------------

namespace {
// ---------------------------------------------------------------------------------------
// `sub_487C60`：`CMovieTextureRenderer::DoRenderSample`（thiscall，`retn 4`）
//
// ★ 语义（`.lst` 0x487C60..0x488061 / `.c` 104246）：把样本缓冲里的 **BGR(A)** 逐像素搬到
//   纹理的 A8R8G8B8（`D3DFORMAT`=21）里，行序**自底向上**，可选 alpha 面 / 色键。
//   `v21 = s[0] | s[1]<<8 | s[2]<<16`（= B|G<<8|R<<16，与 doc §3-3 一致），
//   再 `| 0xFF000000`（或 `s[3]<<24`），等于色键（`[+0x584]`）时写 0。
// ★ 本 override **只覆盖影片主路径**：目标 A8R8G8B8、无 alpha 面（`[+0x588]==0`）、无 alpha 拷贝
//   （`[+0x590]==0`）、源 3/4 字节/像素；其余情况一律返回 false 回退解释器。
// ★ 实测（RelWithDebInfo，LOGO 1280×720）：这条函数占 guest 块时间 **77.5%**（4980.8 ms / 44.25M 块）。
bool nativeRender487C60(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    const std::uint32_t self = cpu.ecx();
    const std::uint32_t sample = cpu.arg(0);
    if (self == 0 || sample == 0) return false;
    if (static_cast<std::uint64_t>(self) + 0x5B8u > GuestMemory::kSpace) return false;
    if (!mem.committed(self + 0x568u, 0x50u)) return false;  // 0x568..0x5B7
    const std::uint32_t surface = mem.rd32(self + 0x568u);
    const std::uint32_t tex_obj = mem.rd32(self + 0x56Cu);
    if (surface == 0 || tex_obj == 0) return false;
    if (mem.rd32(self + 0x570u) != 21u) return false;          // 只做 A8R8G8B8（16bpp 走解释器）
    if (mem.rd32(self + 0x588u) != 0u) return false;           // alpha 面模式
    if (mem.rd32(self + 0x590u) != 0u) return false;           // alpha 拷贝模式
    const std::uint32_t width = mem.rd32(self + 0x5A4u);
    const std::uint32_t height = mem.rd32(self + 0x5A8u);
    const std::uint32_t src_stride = mem.rd32(self + 0x5ACu);
    const std::uint32_t src_bpp = mem.rd32(self + 0x5B0u);
    if (width == 0 || height == 0 || (src_bpp != 3u && src_bpp != 4u)) return false;
    if (src_stride < width * src_bpp) return false;
    // 裁剪矩形：引擎读 left/top/right/bottom（right/bottom 为 0 视为 宽/高）。
    const std::uint32_t left = mem.rd32(self + 0x574u);
    const std::uint32_t top = mem.rd32(self + 0x578u);
    std::uint32_t right = mem.rd32(self + 0x57Cu);
    std::uint32_t bottom = mem.rd32(self + 0x580u);
    if (right == 0u || right > width) right = width;
    if (bottom == 0u || bottom > height) bottom = height;
    if (left >= right || top >= bottom) return false;
    const std::uint32_t buf = dshow::sampleBufferEa(cpu, sample);
    if (buf == 0) return false;
    if (!mem.committed(buf, static_cast<std::size_t>(src_stride) * height)) return false;
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfo(cpu, tex_obj, &ti, &why)) return false;
    if (ti.bpp != 32u || ti.pitch == 0) return false;
    const std::uint64_t span = static_cast<std::uint64_t>(ti.pitch) * height;
    if (static_cast<std::uint64_t>(ti.arena) + span > GuestMemory::kSpace) return false;
    if (!mem.committed(ti.arena, static_cast<std::size_t>(span))) return false;
    // `D3DLOCKED_RECT` 放在 esp 之下 8 B（guest 的序言从不执行）；与 `d3d9::lockRect` 同一套校验。
    const std::uint32_t esp = cpu.esp();
    if (esp < 8u || !mem.committed(esp - 8u, 8u)) return false;
    const std::uint32_t locked_rect_ea = esp - 8u;
    if (!d3d9::lockRect(cpu, ti, 0u, locked_rect_ea, 0u, 0u, &why)) return false;
    const std::uint32_t pitch = mem.rd32(locked_rect_ea + 0u);
    const std::uint32_t bits = mem.rd32(locked_rect_ea + 4u);
    auto unlock = [&]() {
        d3d9::TextureInfo fresh;
        if (d3d9::textureInfo(cpu, tex_obj, &fresh, nullptr)) {
            std::string ignored;
            d3d9::unlockRect(cpu, fresh, 0u, &ignored);
        }
    };
    // LockRect 的返回必须与预测一致（全纹理锁：pitch=ti.pitch、pBits=ti.arena）。
    if (pitch != ti.pitch || bits != ti.arena) {
        unlock();
        std::fprintf(stderr,
                     "[native] sub_487C60：LockRect 返回 pitch/pBits 与预测不一致（%u/%08X vs %u/%08X）⇒ 回退\n",
                     pitch, bits, ti.pitch, ti.arena);
        return false;
    }
    const std::uint32_t key = mem.rd32(self + 0x584u);
    const std::uint8_t* src_base = mem.host(buf);
    std::uint8_t* dst_base = mem.host(bits);
    for (std::uint32_t y = top; y < bottom; ++y) {
        const std::uint8_t* s = src_base + static_cast<std::size_t>(src_stride) * (height - 1u - y) +
                                static_cast<std::size_t>(left) * src_bpp;
        std::uint32_t* d = reinterpret_cast<std::uint32_t*>(dst_base + static_cast<std::size_t>(y) * pitch) + left;
        for (std::uint32_t x = left; x < right; ++x) {
            const std::uint32_t v = static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
                                    (static_cast<std::uint32_t>(s[2]) << 16);
            std::uint32_t px = (src_bpp == 4u) ? ((static_cast<std::uint32_t>(s[3]) << 24) | v) : (v | 0xFF000000u);
            if (px == key) px = 0;
            *d++ = px;
            s += src_bpp;
        }
    }
    unlock();
    mem.wr32(self + 0x5B4u, 1u);  // 引擎尾部置的"已渲染"位
    *result = 0;
    return true;
}

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
    // ★ `sub_48B090`（NormalTexture::UploadImage）已进入 `all`。Windows 侧曾观察到纹理问题，
    //   但 macOS 未复现；后续继续排查。可用 `all,-sub_48B090` 单独排除。
    //   已修：alpha 目标行序、24bpp/32bpp、锁走 lockRect/unlockRect、失败路径无半状态、
    //   ECX/this、cols/pitch、目标格式从设备状态判断、bpp/src 在 Create 后重读。
    //   仍显式回退：4/1bpp 源、16bpp 目标、色键≠0；时间补偿不在本轮范围。
    {"sub_48B090", GuestConv::Thiscall, 12, nativeUpload48B090},
    // ★ 2026-11-07：boot profile 第 2 热点（`sub_429020` Map::findIndex，thiscall retn 8）。
    {"sub_429020", GuestConv::Thiscall, 8, nativeFindIndex429020},
    // ★ 2026-11-07：boot profile 第 3 热点（`sub_48AE90` NormalTexture 纯色填充，thiscall retn 0Ch）。
    {"sub_48AE90", GuestConv::Thiscall, 12, nativeFill48AE90},
    // ★ 2026-11-07：影片渲染热点（LOGO 1280×720 占 guest 块时间 77.5%）。只覆盖 A8R8G8B8 / 无 alpha 面
    //   的主路径，其余回退解释器。差分守卫见 `tests/movie_logo.cpp`（开/关 override 的 `tex movie #1` hash 必须一致）。
    {"sub_487C60", GuestConv::Thiscall, 4, nativeRender487C60},
    // ★ 2026-11-08：字形位图 → 已锁定纹理表面的逐像素混合叶子（thiscall `retn 20h` = 32 B）。
    //   `.lst` 0x46D9F0..0x46DEC2；16bpp / 32bpp 两支；裁剪 = `sub_46CE20` 的成功路径。
    //   准入失败（this/mgr/texid/裁剪对象/参数跨度/级数越界）⇒ 回退解释器，⛔ 不猜。
    //   纯像素核 = `blendGlyph46D9F0`（G1 直接跑）；帧级 A/B 见需求单
    //   REQ-01M6V1KDFTE933AD3YB5P6XJK2（`all` vs `all,-sub_46D9F0` 逐帧逐字节相同）。
    {"sub_46D9F0", GuestConv::Thiscall, 32, nativeBlend46D9F0},
};
constexpr std::size_t kKnownCount = sizeof(kKnown) / sizeof(kKnown[0]);
// ★ 表容量是**硬约束**：`--native-overrides=all` 会把每个 `debug == false` 的项都 `add()` 一遍
//   （machine.cpp 的 configureNativeOverrides），装不下 ⇒ add() 失败 ⇒ configure 返回 false ⇒
//   **模拟器直接拒绝启动**。把它变成编译期错误，而不是等到运行时"all 突然起不来"。
static_assert(kKnownCount <= NativeOverrideTable::kMax,
              "kKnown[] 的项数超过 NativeOverrideTable::kMax：请同时抬高 kMax（见头文件的注释）");

// ★ 段级项（函数中段）：entry/resume 是固定的 `.lst` EA（中段没有符号），启用前用 `.lst` 校验锚。
//   `name` 就是 `--native-overrides` 里写的名字（与函数级同名不会冲突：函数级表里没有 sub_414AC0）。
const KnownBlockNative kKnownBlocks[] = {
    {"sub_414AC0", "sub_414AC0", 0x00415384u, 0x0041539Fu, nativeFillPool414AC0},
};
constexpr std::size_t kKnownBlockCount = sizeof(kKnownBlocks) / sizeof(kKnownBlocks[0]);
}  // namespace

const KnownNative* findKnownNative(const std::string& name) {
    for (std::size_t i = 0; i < kKnownCount; ++i)
        if (name == kKnown[i].name) return &kKnown[i];
    return nullptr;
}

std::size_t knownNativeCount() { return kKnownCount; }

const KnownNative& knownNativeAt(std::size_t i) { return kKnown[i]; }

const KnownBlockNative* findKnownBlockNative(const std::string& name) {
    for (std::size_t i = 0; i < kKnownBlockCount; ++i)
        if (name == kKnownBlocks[i].name) return &kKnownBlocks[i];
    return nullptr;
}

std::size_t knownBlockNativeCount() { return kKnownBlockCount; }

const KnownBlockNative& knownBlockNativeAt(std::size_t i) { return kKnownBlocks[i]; }

namespace {
// 把 `.lst` 的指令文本归一化（列宽对齐的连续空白压成一个空格）。
std::string normalizeAsm(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}
}  // namespace

bool verifyKnownBlockAnchor(const LstIndex& lst, const KnownBlockNative& k, std::string* err) {
    auto bad = [&](const std::string& msg) {
        if (err) *err = std::string("段级 override ") + k.name + " 的锚校验失败：" + msg;
        return false;
    };
    if (!lst.loaded()) return bad("没有 .lst（段级 EA 无法核验）");
    const LstFunction* fn = lst.findFunction(k.enclosing_fn);
    if (fn == nullptr) return bad(std::string("`.lst` 里找不到函数 ") + k.enclosing_fn);
    const LstFunction* owner = lst.functionAt(k.entry_ea);
    if (owner == nullptr || owner->ea != fn->ea)
        return bad("entry EA 不在 " + std::string(k.enclosing_fn) + " 里");
    const LstEntry* entry = lst.find(k.entry_ea);
    if (entry == nullptr) return bad("entry EA 不是 `.lst` 里的一条指令");
    // 首指令必须是我们取证到的那条（换镜像 / 换 EA ⇒ 拒绝启用，⛔ 不静默换一个 EA）。
    if (normalizeAsm(entry->text) != "mov ecx, [esi-10h]")
        return bad("entry EA 的首指令不是 `mov ecx, [esi-10h]`（读到：" + normalizeAsm(entry->text) + "）");
    const LstEntry* resume = lst.find(k.resume_ea);
    if (resume == nullptr) return bad("resume EA 不是 `.lst` 里的一条指令");
    if (normalizeAsm(resume->text) != "mov eax, [ebx+5D7ECh]")
        return bad("resume EA 的首指令不是 `mov eax, [ebx+5D7ECh]`（读到：" + normalizeAsm(resume->text) + "）");
    return true;
}

}  // namespace lift
