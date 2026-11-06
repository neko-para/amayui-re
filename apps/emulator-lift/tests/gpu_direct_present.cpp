// emulator-lift · GPU 直出（`--present window --raster gpu`）的机械守卫
//
// 判据（⛔ 不是"看了一眼窗口"）：
//   · 窗口被 claim 给 GPU device（`directPresentActive()`）⇒ `submitFrame` 之后从**窗口**
//     回读的像素必须与送进去的后缓冲逐像素相等（1:1，0 diff）。
//   · 同一窗口上换尺寸 / 换格式（含 16bpp）后仍然相等 —— 直出前 16bpp 要按掩码转成 BGRA8，
//     这条断言就是防"窗口停在上一帧"的。
//   · 连续两帧不同内容 ⇒ 第二帧必须真的换掉（同上，防 stale target）。
//   · 直出不可用（dummy 视频 / 驱动不支持 claim / 没有 GPU device）⇒ SKIP 77 并打印实测原因；
//     ⛔ 不拿 SDL_Renderer 路径假绿。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/gpu_raster.h"
#include "runtime/memory.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"

#include <SDL3/SDL.h>

namespace {

lift::SurfaceFormat format32(std::uint32_t amask) {
    lift::SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = amask;
    return f;
}

lift::SurfaceFormat format565() {
    lift::SurfaceFormat f;
    f.bpp = 16;
    f.rmask = 0xF800u;
    f.gmask = 0x07E0u;
    f.bmask = 0x001Fu;
    f.amask = 0;
    return f;
}

struct Fx {
    lift::GuestMemory mem;
    lift::SurfaceArena arena;
    std::uint32_t ea = 0;
    lift::GuestSurface* s = nullptr;
};

bool make(Fx& f, int w, int h, lift::SurfaceFormat fmt) {
    if (!f.mem.reserve() || !f.arena.init(&f.mem, 0x10000000u, 0x00100000u)) return false;
    f.ea = f.arena.create(w, h, fmt);
    f.s = f.arena.find(f.ea);
    return f.ea != 0 && f.s != nullptr;
}

void fill32(Fx& f, std::uint32_t argb) {
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 4, &argb, 4);
        }
    }
}

// 565 的一整屏纯色（直出要按掩码展开成 BGRA8）
void fill565(Fx& f, std::uint16_t rgb565) {
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 2, &rgb565, 2);
        }
    }
}

bool expectRgb(lift::SdlWindow& win, int x, int y, std::uint32_t rgb, const char* what) {
    std::uint32_t argb = 0;
    std::string err;
    if (!win.readbackPixel(x, y, &argb, &err)) {
        std::fprintf(stderr, "  readbackPixel(%s) 失败：%s\n", what, err.c_str());
        return false;
    }
    const std::uint32_t got = argb & 0x00FFFFFFu;
    if (got != (rgb & 0x00FFFFFFu)) {
        std::fprintf(stderr, "  %s: expected RGB %06X, got %06X (argb=%08X)\n", what, rgb & 0xFFFFFFu, got, argb);
        return false;
    }
    return true;
}

// 提交一帧；失败时把 err 打出来（否则只看到 CHECK 行，看不出是哪一步失败）
bool submit(lift::SdlWindow& win, const lift::SurfaceArena& arena, const lift::GuestSurface& s, const char* what) {
    std::string err;
    if (!win.submitFrame(arena, s, &err)) {
        std::fprintf(stderr, "  submitFrame(%s) 失败：%s\n", what, err.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-direct test";
    opt.hidden = true;
    opt.raster_gpu = true;
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        // 没有可用 GPU device（dummy 视频 / 驱动缺失）⇒ 环境缺失，不是直出回归。
        LIFT_SKIP(err.c_str());
    }
    if (!win.directPresentActive()) {
        std::fprintf(stderr, "GPU 直出未成立：%s\n", win.describe().c_str());
        win.shutdown();
        LIFT_SKIP("窗口 claim 失败 ⇒ 呈现回退 SDL_Renderer（直出路径没有被执行）");
    }
    std::printf("gpu-direct-present: %s\n", win.describe().c_str());

    // (1) 32bpp X8：窗口像素 == 送进去的后缓冲
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0)));
        fill32(f, 0x00112233u);
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 0, 0, 0x112233u, "direct x8(0,0)"));
        LIFT_CHECK(expectRgb(win, 63, 47, 0x112233u, "direct x8(63,47)"));
        // 连续第二帧必须真的换掉（防"target 停在上一帧"）
        fill32(f, 0x00A1B2C3u);
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 10, 10, 0xA1B2C3u, "direct x8 second frame"));
    }

    // (2) A8R8G8B8 + alpha=0：直出不做混合 ⇒ 像素原样（与 renderer 路径的 blend=NONE 同口径）
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0xFF000000u)));
        fill32(f, 0x00203040u);
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 0, 0, 0x203040u, "direct argb alpha=0"));
    }

    // (3) 16bpp 565：直出前按掩码展开成 BGRA8（不能停在上一帧）
    {
        Fx f;
        LIFT_CHECK(make(f, 40, 30, format565()));
        fill565(f, 0xF800u);  // 纯红
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 0, 0, 0xFF0000u, "direct 565 red"));
        fill565(f, 0x07E0u);  // 纯绿
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 39, 29, 0x00FF00u, "direct 565 green"));
    }

    // (4) 尺寸变化（同一窗口）：33x17，非紧凑 pitch
    {
        Fx f;
        LIFT_CHECK(make(f, 33, 17, format32(0)));
        fill32(f, 0x00FF8000u);
        LIFT_CHECK(submit(win, f.arena, *f.s, "frame"));
        LIFT_CHECK(expectRgb(win, 1, 1, 0xFF8000u, "direct 33x17"));
        LIFT_CHECK(expectRgb(win, 32, 16, 0xFF8000u, "direct 33x17 corner"));
    }

    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return lift_test::finish("gpu-direct-present");
}

