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

    // ---------------------------------------------------------------------------------
    // TITLE 能力守卫：linear sampler + separate alpha blend 的**像素结果**
    // ---------------------------------------------------------------------------------
    // 下面两条都读像素中心 (32,32)，与 CPU 参考的判定口径一致（≤1 LSB 舍入差）。
    // 目标换成带 alpha 的 32bpp（否则读回会把 A 清零）。
    LIFT_CHECK(win.rasterPrepare(format32(0xFF000000u), w, h, &err));

    auto put = [&](int x, int y, unsigned b, unsigned g, unsigned r, unsigned a) {
        std::uint8_t* p = pixels.data() + (static_cast<std::size_t>(y) * w + x) * 4;
        p[0] = static_cast<std::uint8_t>(b);
        p[1] = static_cast<std::uint8_t>(g);
        p[2] = static_cast<std::uint8_t>(r);
        p[3] = static_cast<std::uint8_t>(a);
    };
    auto pixel_at = [&](int x, int y) {
        return pixels.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    };

    // (1) LINEAR：2×2 纹理 (0,0)=0 / (1,0)=100 / 其余 0；在像素中心 u≈0.5078 ⇒ 双线性 ≈ 25。
    //     NEAREST 会取 texel(1,1)=0 —— 这条断言把"支持线性过滤"变成不可伪造的像素判据。
    for (auto& v : pixels) v = 0;
    LIFT_CHECK(win.rasterSyncFromGuest(pixels.data(), w * 4, w, h, &err));
    const unsigned tex_r[4] = {0, 100, 0, 0};
    std::uint8_t tex2x2[2 * 2 * 4] = {};
    for (int i = 0; i < 4; ++i) {
        tex2x2[i * 4 + 0] = 0;
        tex2x2[i * 4 + 1] = 0;
        tex2x2[i * 4 + 2] = static_cast<std::uint8_t>(tex_r[i]);
        tex2x2[i * 4 + 3] = 255;
    }
    lift::GpuRasterTexture ltex;
    ltex.pixels = tex2x2;
    ltex.pitch = 2 * 4;
    ltex.width = 2;
    ltex.height = 2;
    ltex.format = format32(0xFF000000u);
    ltex.key = tex2x2;
    ltex.sampler.mag_filter = lift::GpuFilter::Linear;
    ltex.sampler.min_filter = lift::GpuFilter::Linear;
    ltex.sampler.address_u = lift::GpuAddress::Clamp;
    ltex.sampler.address_v = lift::GpuAddress::Clamp;
    lift::GpuRasterVertex lquad[6] = {
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {64.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 64.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f},
    };
    lift::GpuRasterState lst;
    lst.viewport_w = w;
    lst.viewport_h = h;
    lst.scissor_x1 = w;
    lst.scissor_y1 = h;
    lst.blend = false;
    lst.depth_test = false;
    lst.depth_write = false;
    lst.tex_mode = lift::GpuTexMode::SelectTextureBoth;  // 只测采样：颜色/alpha 都取纹理
    LIFT_CHECK(win.rasterDrawTextured(lquad, 6, ltex, lst, &err));
    LIFT_CHECK(win.rasterSyncToGuest(pixels.data(), w * 4, w, h, &err));
    const unsigned lin_r = pixel_at(32, 32)[2];
    if (lin_r < 24u || lin_r > 26u) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "  linear sample R=%u（期望 24..26；NEAREST 会是 0）\n", lin_r);
    }

    // (2) separate alpha blend：1×1 红纹理 alpha=128 + 顶点白。
    //     颜色 = SRCALPHA/INVSRCALPHA、alpha = ONE/INVSRCALPHA（TITLE 的 5/6 + 2/6）。
    //     目标清成 (0,0,0,0) ⇒ 期望 R=128、B/G=0、A=128；若 separate alpha 被忽略，A 会是 64。
    for (auto& v : pixels) v = 0;
    LIFT_CHECK(win.rasterSyncFromGuest(pixels.data(), w * 4, w, h, &err));
    const std::uint8_t red_half_alpha[4] = {0, 0, 255, 128};  // BGRA
    lift::GpuRasterTexture atex;
    atex.pixels = red_half_alpha;
    atex.pitch = 4;
    atex.width = 1;
    atex.height = 1;
    atex.format = format32(0xFF000000u);
    atex.key = red_half_alpha;
    lift::GpuRasterState ast;
    ast.viewport_w = w;
    ast.viewport_h = h;
    ast.scissor_x1 = w;
    ast.scissor_y1 = h;
    ast.blend = true;
    ast.src_blend = lift::GpuBlendFactor::SrcAlpha;
    ast.dst_blend = lift::GpuBlendFactor::InvSrcAlpha;
    ast.blend_op = lift::GpuBlendOp::Add;
    ast.separate_alpha_blend = true;
    ast.src_blend_alpha = lift::GpuBlendFactor::One;
    ast.dst_blend_alpha = lift::GpuBlendFactor::InvSrcAlpha;
    ast.blend_op_alpha = lift::GpuBlendOp::Add;
    ast.depth_test = false;
    ast.depth_write = false;
    ast.tex_mode = lift::GpuTexMode::ModulateColorSelectAlpha;  // 颜色 tex*顶点色，alpha 取 tex
    LIFT_CHECK(win.rasterDrawTextured(lquad, 6, atex, ast, &err));
    LIFT_CHECK(win.rasterSyncToGuest(pixels.data(), w * 4, w, h, &err));
    const std::uint8_t* ablend = pixel_at(32, 32);
    const unsigned ar = ablend[2];
    const unsigned aa = ablend[3];
    if (ar < 127u || ar > 129u || aa < 127u || aa > 129u) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "  separate-alpha blend R=%u A=%u（期望 127..129；A=64 表示 separate alpha 没生效）\n",
                     ar, aa);
    }
    LIFT_CHECK_EQ_U32(ablend[0], 0u);
    LIFT_CHECK_EQ_U32(ablend[1], 0u);
    std::printf("gpu-raster%s: driver=%s\n", gpu ? "-gpu" : "", win.rendererName());
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return lift_test::finish("gpu-raster");
}
