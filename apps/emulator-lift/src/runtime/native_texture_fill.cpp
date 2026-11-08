// emulator-lift · native override：纯色填充（sub_48AE90 NormalTexture 虚表第 6 项）
//
// ★ LockRect → 16bpp（A4R4G4B4）/ 32bpp → UnlockRect；位深读 `ds:dword_5518C0`，⛔ 不是 DDraw bpp。
// ★ 从 `native_overrides.cpp` 拆出（纯搬运，语义不变）；ABI / 回退契约见 `native_overrides.h`。
#include "runtime/native_overrides.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/d3d9.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
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

}  // namespace lift
