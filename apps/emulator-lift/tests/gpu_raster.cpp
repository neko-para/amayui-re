// emulator-lift · GPU 光栅化后端守卫（SDL_GPU 光栅化 + 回读；Windows DXBC / macOS MSL）
// ★ 纯机械：不依赖游戏素材。dummy+software 档验证光栅/回读；--gpu 档要求 GPU 驱动。
// ★ 两档都真的需要 SDL_GPU 设备：没有可用设备（典型：dummy 视频 + Metal）⇒ SKIP 77，
//   ⛔ 不拿"renderer 是 software"冒充 GPU 光栅化通过。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/frame_blit.h"
#include "runtime/present_sdl.h"

#include <SDL3/SDL.h>

namespace {
lift::SurfaceFormat format32(std::uint32_t amask = 0) {
    lift::SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = amask;
    return f;
}
}  // namespace

int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
    const int w = 64;
    const int h = 64;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4, 0);
    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-raster test";
    opt.driver_list = gpu ? "gpu" : "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) {
        // 两档都要 GPU 设备：dummy 视频下 Metal 设备可能建不出来 ⇒ 这是环境缺失，不是后端回归。
        LIFT_SKIP(err.c_str());
    }
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false");
    }
    LIFT_CHECK(win.rasterPrepare(format32(), w, h, &err));
    LIFT_CHECK(win.rasterSyncFromGuest(pixels.data(), w * 4, w, h, &err));
    lift::GpuRasterVertex tri[3];
    tri[0] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    tri[1] = {64.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    tri[2] = {0.0f, 64.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    lift::GpuRasterState st;
    st.viewport_w = w;
    st.viewport_h = h;
    st.scissor_x1 = w;
    st.scissor_y1 = h;
    LIFT_CHECK(win.rasterDraw(tri, 3, st, &err));
    LIFT_CHECK(win.rasterDirty());
    LIFT_CHECK(win.rasterSyncToGuest(pixels.data(), w * 4, w, h, &err));
    LIFT_CHECK(!win.rasterDirty());
    const auto* p0 = pixels.data();
    const auto* p1 = pixels.data() + (static_cast<std::size_t>(h - 1) * w + (w - 1)) * 4;
    LIFT_CHECK_EQ_U32(p0[0], 0u);    // B
    LIFT_CHECK_EQ_U32(p0[1], 0u);    // G
    LIFT_CHECK_EQ_U32(p0[2], 255u);  // R
    LIFT_CHECK_EQ_U32(p0[3], 0u);    // X8 的 A 字节必须归一化为 0
    LIFT_CHECK_EQ_U32(p1[0], 0u);
    LIFT_CHECK_EQ_U32(p1[1], 0u);
    LIFT_CHECK_EQ_U32(p1[2], 0u);
    // 纹理路径：1×1 绿色纹理 + 白色顶点色 ⇒ 覆盖 sampler / 顶点色调制（MSL 与 DXBC 各自的入口）。
    const std::uint8_t texel[4] = {0, 255, 0, 255};  // BGRA
    lift::GpuRasterTexture texture;
    texture.pixels = texel;
    texture.pitch = 4;
    texture.width = 1;
    texture.height = 1;
    texture.format = format32(0xFF000000u);
    texture.key = texel;
    lift::GpuRasterVertex quad[6] = {
        {32.0f, 32.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {64.0f, 32.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {32.0f, 32.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {32.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
    };
    LIFT_CHECK(win.rasterDrawTextured(quad, 6, texture, st, &err));
    LIFT_CHECK(win.rasterDirty());
    LIFT_CHECK(win.rasterSyncToGuest(pixels.data(), w * 4, w, h, &err));
    const auto* ptex = pixels.data() + (static_cast<std::size_t>(48) * w + 48) * 4;
    LIFT_CHECK_EQ_U32(ptex[0], 0u);
    LIFT_CHECK_EQ_U32(ptex[1], 255u);
    LIFT_CHECK_EQ_U32(ptex[2], 0u);
    std::printf("gpu-raster%s: driver=%s\n", gpu ? "-gpu" : "", win.rendererName());
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return lift_test::finish("gpu-raster");
}
