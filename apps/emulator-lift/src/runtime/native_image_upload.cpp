// emulator-lift · native override：纹理上传（sub_48B090 NormalTexture::UploadImage）
//
// ★ 源像素 → 目标 32bpp 纹理；4/1bpp 源、16bpp 目标、色键 != 0 显式回退解释器。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "corpus/lst_index.h"
#include "runtime/d3d9.h"
#include "runtime/ddraw_surface.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
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

}  // namespace lift
