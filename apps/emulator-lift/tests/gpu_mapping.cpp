// emulator-lift · GPU 准入 / 状态映射的纯函数守卫（不建机器、不碰 SDL）
//
// ★ 这条守卫存在的理由：TITLE 的黑场渐变 DrawPrimitive 之所以回退 CPU，根因是
//   `gpuEligible()` 拒绝了 separate alpha blend / stage 0 纹理。准入逻辑必须是**可测的纯函数**，
//   否则"扩了能力"只能靠跑真游戏 + 肉眼看，红了也说不清是哪一条状态映射错了。
// ★ TITLE 的实测状态（来自 `LIFT_SLOW_DRAW=1`，2026-11-06）：
//     blend color = SRCALPHA/INVSRCALPHA（5/6）、separate alpha = ONE/INVSRCALPHA（2/6）、
//     color/alpha op = MODULATE（4），arg1/arg2 = TEXTURE/TFACTOR（2/3）、
//     sampler = WRAP/WRAP + LINEAR/LINEAR、color write = 0xF、FVF = 0x244（XYZRHW|DIFFUSE|TEX1）、
//     TEXCOORDINDEX = 0、纹理 = 2048×2048 A8R8G8B8。
//   这里把关键数字钉成期望值；反例用 D3DBLEND_BOTHSRCALPHA（SDL_GPU 的固定 blend state
//   表达不了"同时改源和目标因子"）—— 必须返回 false。
#include <cstdint>
#include <string>

#include "check.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/gpu_raster.h"

namespace {

using lift::GpuBlendFactor;
using lift::GpuBlendOp;
using lift::GpuRasterState;
using lift::GpuTexMode;

// TITLE：color SRCALPHA/INVSRCALPHA + ADD，separate alpha ONE/INVSRCALPHA + ADD
void titleBlendAccepted() {
    GpuRasterState st;
    std::string why;
    const bool ok = lift::d3d9::gpuBlendStateFor(
        lift::d3d9::kBlendSrcAlpha, lift::d3d9::kBlendInvSrcAlpha, lift::d3d9::kBlendOpAdd,
        /*separate_alpha=*/true, lift::d3d9::kBlendOne, lift::d3d9::kBlendInvSrcAlpha,
        lift::d3d9::kBlendOpAdd, /*blend_enabled=*/true, &st, &why);
    LIFT_CHECK(ok);
    LIFT_CHECK(why.empty());
    LIFT_CHECK(st.blend);
    LIFT_CHECK(st.src_blend == GpuBlendFactor::SrcAlpha);
    LIFT_CHECK(st.dst_blend == GpuBlendFactor::InvSrcAlpha);
    LIFT_CHECK(st.blend_op == GpuBlendOp::Add);
    LIFT_CHECK(st.separate_alpha_blend);
    LIFT_CHECK(st.src_blend_alpha == GpuBlendFactor::One);
    LIFT_CHECK(st.dst_blend_alpha == GpuBlendFactor::InvSrcAlpha);
    LIFT_CHECK(st.blend_op_alpha == GpuBlendOp::Add);
}

// 不支持的因子必须**红**（返回 false + 非空 why），⛔ 不能悄悄当成 SRCALPHA。
void unsupportedBlendRejected() {
    GpuRasterState st;
    std::string why;
    // D3DBLEND_BOTHSRCALPHA：同时改源和目标因子，SDL_GPU 的固定 blend state 表达不了。
    const bool both = lift::d3d9::gpuBlendStateFor(
        lift::d3d9::kBlendBothSrcAlpha, lift::d3d9::kBlendInvSrcAlpha, lift::d3d9::kBlendOpAdd,
        /*separate_alpha=*/false, lift::d3d9::kBlendOne, lift::d3d9::kBlendZero,
        lift::d3d9::kBlendOpAdd, /*blend_enabled=*/true, &st, &why);
    LIFT_CHECK(!both);
    LIFT_CHECK(!why.empty());
    // separate alpha 段里塞一个不支持的因子，也必须红。
    why.clear();
    const bool sep = lift::d3d9::gpuBlendStateFor(
        lift::d3d9::kBlendSrcAlpha, lift::d3d9::kBlendInvSrcAlpha, lift::d3d9::kBlendOpAdd,
        /*separate_alpha=*/true, lift::d3d9::kBlendBothInvSrcAlpha, lift::d3d9::kBlendInvSrcAlpha,
        lift::d3d9::kBlendOpAdd, /*blend_enabled=*/true, &st, &why);
    LIFT_CHECK(!sep);
    LIFT_CHECK(!why.empty());
    why.clear();
    const bool bad_op = lift::d3d9::gpuBlendStateFor(
        lift::d3d9::kBlendSrcAlpha, lift::d3d9::kBlendInvSrcAlpha, /*color_op=*/99,
        /*separate_alpha=*/false, lift::d3d9::kBlendOne, lift::d3d9::kBlendZero,
        lift::d3d9::kBlendOpAdd, /*blend_enabled=*/true, &st, &why);
    LIFT_CHECK(!bad_op);
    LIFT_CHECK(!why.empty());
}

// blend=off ⇒ factor/op 不进账（真机语义）；但仍要成功。
void blendDisabledIsNoop() {
    GpuRasterState st;
    std::string why;
    const bool ok = lift::d3d9::gpuBlendStateFor(lift::d3d9::kBlendDestColor, lift::d3d9::kBlendInvDestColor,
                                                 lift::d3d9::kBlendOpSubtract, true,
                                                 lift::d3d9::kBlendDestAlpha, lift::d3d9::kBlendInvDestAlpha,
                                                 lift::d3d9::kBlendOpMax, /*blend_enabled=*/false, &st, &why);
    LIFT_CHECK(ok);
    LIFT_CHECK(!st.blend);
    LIFT_CHECK(!st.separate_alpha_blend);
}

// TITLE 的纹理级组合：color/alpha 都是 MODULATE(TEXTURE, TFACTOR)（实测 op=4 arg=2/3）。
// ⇒ ModulateBoth + 顶点色整批换成 TFACTOR；TEXCOORDINDEX 取 stage 的值（TITLE 是 0）。
void titleTextureModeAccepted() {
    GpuTexMode mode = GpuTexMode::ModulateBoth;
    bool from_tf = false;
    std::string why;
    const bool ok = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopModulate, lift::d3d9::kTaTexture, lift::d3d9::kTaTFactor, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaTFactor, /*texture_present=*/true, /*tex_coord_index=*/0, &mode,
        &from_tf, &why);
    LIFT_CHECK(ok);
    LIFT_CHECK(mode == GpuTexMode::ModulateBoth);
    LIFT_CHECK(from_tf);

    // 颜色 MODULATE(TEXTURE, CURRENT=DIFFUSE)，alpha SELECTARG1(TEXTURE)（另一档常见组合）。
    why.clear();
    from_tf = true;
    const bool cur = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopModulate, lift::d3d9::kTaTexture, lift::d3d9::kTaCurrent, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaTexture, /*alpha_arg2=*/0, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(cur);
    LIFT_CHECK(mode == GpuTexMode::ModulateColorSelectAlpha);
    LIFT_CHECK(!from_tf);

    // 颜色取纹理、alpha 取 diffuse（SELECTARG1×2）。
    why.clear();
    const bool tex_diff = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopSelectArg1, lift::d3d9::kTaTexture, 0, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaDiffuse, 0, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(tex_diff);
    LIFT_CHECK(mode == GpuTexMode::SelectColorDiffuseAlpha);
    LIFT_CHECK(!from_tf);

    // TEXCOORDINDEX=7 可接受；8 越界必须红（⛔ 不静默当成 TEXCOORD0）。
    why.clear();
    const bool coord7 = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopModulate, lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, true, 7, &mode, &from_tf, &why);
    LIFT_CHECK(coord7);
    why.clear();
    const bool coord8 = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopModulate, lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, true, 8, &mode, &from_tf, &why);
    LIFT_CHECK(!coord8);
    LIFT_CHECK(!why.empty());
}

// 不能等价表达的 op/arg 必须红：ADD 的颜色级、带 COMPLEMENT 的 arg、DIFFUSE 与 TFACTOR 混用。
void unsupportedTextureModesRejected() {
    GpuTexMode mode = GpuTexMode::ModulateBoth;
    bool from_tf = false;
    std::string why;
    const bool add = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopAdd, lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!add);
    LIFT_CHECK(!why.empty());

    why.clear();
    const bool complement = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopSelectArg1, lift::d3d9::kTaTexture | lift::d3d9::kTaComplement, 0,
        lift::d3d9::kTopSelectArg1, lift::d3d9::kTaTexture, 0, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!complement);
    LIFT_CHECK(!why.empty());

    // color 用 DIFFUSE、alpha 用 TFACTOR：GPU 只有一个顶点色输入槽 ⇒ 必须红。
    why.clear();
    const bool mixed = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopModulate, lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaTFactor, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!mixed);
    LIFT_CHECK(!why.empty());

    why.clear();
    const bool diffuse_only = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopSelectArg1, lift::d3d9::kTaDiffuse, 0, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaDiffuse, 0, /*texture_present=*/true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!diffuse_only);  // 颜色/alpha 都不碰纹理 ⇒ 不是"有纹理的 GPU 路径"这一类
}

// stage0 DISABLE 的语义：CPU 的 applyTextureOp 对 DISABLE 落 default 分支、取 **arg1**（不是恒等的
// DIFFUSE）。有纹理 + color DISABLE + arg1=TEXTURE 时 CPU 取的是纹理；GPU 必须表达成取纹理，
// 不能当成顶点色。
void stageDisabledSemantics() {
    GpuTexMode mode = GpuTexMode::ModulateBoth;
    bool from_tf = false;
    std::string why;
    // color DISABLE(arg1=TEXTURE) + alpha SELECTARG1(TEXTURE) ⇒ 两个通道都取纹理。
    const bool tex = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopDisable, lift::d3d9::kTaTexture, 0, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaTexture, 0, /*texture_present=*/true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(tex);
    LIFT_CHECK(mode == GpuTexMode::SelectTextureBoth);
    LIFT_CHECK(!from_tf);

    // color DISABLE(arg1=TEXTURE) + alpha MODULATE(TEXTURE, DIFFUSE) ⇒ 取纹理颜色 + 调制 alpha。
    why.clear();
    const bool mixed = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopDisable, lift::d3d9::kTaTexture, 0, lift::d3d9::kTopModulate,
        lift::d3d9::kTaTexture, lift::d3d9::kTaDiffuse, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(mixed);
    LIFT_CHECK(mode == GpuTexMode::SelectColorModulateAlpha);

    // color DISABLE(arg1=DIFFUSE) + alpha SELECTARG1(DIFFUSE)：两个通道都只取常量 ⇒ 没有对应的
    // 纹理片元置换，必须明确 false（回退 CPU），⛔ 不能悄悄当成"取纹理"或别的状态。
    why.clear();
    const bool diffuse = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopDisable, lift::d3d9::kTaDiffuse, 0, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaDiffuse, 0, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!diffuse);
    LIFT_CHECK(!why.empty());

    // color DISABLE(arg1=DIFFUSE) + alpha SELECTARG1(TEXTURE)：颜色常量、alpha 纹理 ⇒ 也 false。
    why.clear();
    const bool color_const = lift::d3d9::gpuTextureModeFor(
        lift::d3d9::kTopDisable, lift::d3d9::kTaDiffuse, 0, lift::d3d9::kTopSelectArg1,
        lift::d3d9::kTaTexture, 0, true, 0, &mode, &from_tf, &why);
    LIFT_CHECK(!color_const);
    LIFT_CHECK(!why.empty());

    // 无纹理路径的同一语义（CPU 采样=白色）：DISABLE(arg1=TEXTURE) 得到白色，不能冒充顶点色。
    LIFT_CHECK(lift::d3d9::gpuUntexturedIsIdentity(lift::d3d9::kTopDisable, lift::d3d9::kTaDiffuse, 0));
    LIFT_CHECK(!lift::d3d9::gpuUntexturedIsIdentity(lift::d3d9::kTopDisable, lift::d3d9::kTaTexture, 0));
    LIFT_CHECK(lift::d3d9::gpuUntexturedIsIdentity(lift::d3d9::kTopModulate, lift::d3d9::kTaTexture,
                                                   lift::d3d9::kTaDiffuse));
    LIFT_CHECK(!lift::d3d9::gpuUntexturedIsIdentity(lift::d3d9::kTopSelectArg1, lift::d3d9::kTaTexture, 0));
}

// GPU 纹理缓存身份：同 guest 指针 / 尺寸 / alpha / generation，但不同 **serial** 不能命中
// 同一个缓存项（arena 复用 + 新纹理 generation 也从 0/1 起，指针键会读旧像素）。
void textureCacheIdentity() {
    lift::GpuRasterTexture t;
    t.format = lift::SurfaceFormat{};  // 32bpp + amask=FF000000
    t.key = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x12345u));
    t.generation = 3;
    t.serial = 41;
    const std::uint64_t a = lift::gpuTextureCacheKey(t, 64, 64);
    t.serial = 42;
    const std::uint64_t b = lift::gpuTextureCacheKey(t, 64, 64);
    LIFT_CHECK(a != b);  // 其余字段完全一样，只有 serial 不同

    // serial == 0（夹具 / 白纹理）退回指针键：同指针同尺寸稳定，尺寸变化则不同。
    t.serial = 0;
    t.generation = 0;
    const std::uint64_t p1 = lift::gpuTextureCacheKey(t, 64, 64);
    const std::uint64_t p2 = lift::gpuTextureCacheKey(t, 64, 64);
    const std::uint64_t p3 = lift::gpuTextureCacheKey(t, 128, 64);
    LIFT_CHECK(p1 == p2);
    LIFT_CHECK(p1 != p3);

    // serial 键与指针键用 bit63 区分，不能撞在一起。
    t.serial = 41;
    const std::uint64_t s1 = lift::gpuTextureCacheKey(t, 64, 64);
    t.serial = 0;
    const std::uint64_t q1 = lift::gpuTextureCacheKey(t, 64, 64);
    LIFT_CHECK(s1 != q1);
}

}  // namespace

int main() {
    titleBlendAccepted();
    unsupportedBlendRejected();
    blendDisabledIsNoop();
    titleTextureModeAccepted();
    unsupportedTextureModesRejected();
    stageDisabledSemantics();
    textureCacheIdentity();
    return lift_test::finish("gpu-mapping");
}
