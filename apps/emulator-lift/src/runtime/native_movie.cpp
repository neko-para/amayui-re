// emulator-lift · native override：影片样本 → 纹理（sub_487C60）
//
// ★ 只覆盖 A8R8G8B8 / 无 alpha 面 / 无 alpha 拷贝 / 3 或 4 字节源的主路径；其余回退解释器。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstdio>
#include <string>

#include "runtime/d3d9.h"
#include "runtime/dshow/objects.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
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

}  // namespace lift
