// emulator-lift · guest 后缓冲格式选择（pure）
//
// 判据：六种 D3D9 后缓冲格式（X8R8G8B8 / A8R8G8B8 / R5G6B5 / X1R5G5B5 / A1R5G5B5 / A4R4G4B4）
// 必须按 masks 命中；未知 mask 必须 Unsupported（⛔ 不按 bpp 猜颜色）。
#include "check.h"
#include "runtime/frame_blit.h"

int main() {
    using lift::SurfaceFormat;
    using F = lift::frame_blit::HostPixelFormat;
    auto f32 = [](std::uint32_t amask) {
        SurfaceFormat f;
        f.bpp = 32;
        f.rmask = 0x00FF0000u;
        f.gmask = 0x0000FF00u;
        f.bmask = 0x000000FFu;
        f.amask = amask;
        return f;
    };
    auto f16 = [](std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
        SurfaceFormat f;
        f.bpp = 16;
        f.rmask = r;
        f.gmask = g;
        f.bmask = b;
        f.amask = a;
        return f;
    };
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f32(0)) == F::Xrgb8888);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f32(0xFF000000u)) == F::Argb8888);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f16(0xF800, 0x07E0, 0x001F, 0)) == F::Rgb565);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f16(0x7C00, 0x03E0, 0x001F, 0)) == F::Xrgb1555);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f16(0x7C00, 0x03E0, 0x001F, 0x8000)) == F::Argb1555);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f16(0x0F00, 0x00F0, 0x000F, 0xF000)) == F::Argb4444);
    // 非标准：565 的 amask 非 0 / 32bpp 的 amask 非标准
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f16(0xF800, 0x07E0, 0x001F, 0x8000)) == F::Unsupported);
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(f32(0x80000000u)) == F::Unsupported);
    SurfaceFormat odd;
    odd.bpp = 24;
    LIFT_CHECK(lift::frame_blit::chooseHostFormat(odd) == F::Unsupported);
    return lift_test::finish("frame-blit");
}
