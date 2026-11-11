// emulator-lift · SDL_GPU 光栅化后端
//（屏幕空间三角形 + D3DXSprite 四边形 + D3D9 stage 0 纹理 + 窗口直出）
//
// ★ 与 SDL_Renderer 呈现解耦：自己的 SDL_GPUDevice + BGRA8 颜色 target + D32 深度 target。
// ★ 顶点由 CPU 做完 T&L/裁剪/屏幕映射；这里转成 NDC 后画 triangle list。
// ★ 读回只在 Present（或 CPU 回退）发生；A8R8G8B8 的 A 字节按 guest X8 语义归一化为 0。
// ★ `--present window` 时窗口被 claim 给同一台 device：呈现 = target → swapchain 的
//   `SDL_BlitGPUTexture`（letterbox 用 load_op=CLEAR 画黑边），**不再** `SDL_UpdateTexture`。
//   claim 失败（dummy 视频 / 驱动不支持）⇒ 调用方回退 SDL_Renderer 上传路径并打印实测错误。
// ★ shader 后端按 **SDL_GPU 的原生 shader 格式**分平台：
//   Windows = D3DCompile → DXBC；macOS = MSL 源码（Metal）。⛔ 不把 DXBC 带进 Metal。
//
// ★ 合批（2026-11）：`rasterQueue` 只入队（顶点 CPU 拷贝、纹理在入队时就把像素拷成 staging），
//   `rasterFlush` 一次做完：按段 `[copy 上传][render 画]` 交替 → 只 `SDL_SubmitGPUCommandBuffer`
//   一次。★ 方案 C（先脏矩形判、再切段）：纹理内容在队列里被引用期间变化时，先判"写入矩形 × 本段
//   已入队 draw 的采样矩形"是否相交；相交才在边界处切段，不相交就不切（上传并进本段开头）。
//   ⛔ 只有合并提交，不合并 draw；⛔ 不跨 target 共用一个 render pass。
// ★ 屏障清单（凡是要读/写 target 或换 target 的，先 flush；要 guest 像素的再 syncToGuest）：
//   · 读 target：`rasterSyncToGuest`（Present / CPU 回退 / dump）、SDL_GPU 的 `directReadback`；
//   · 写 target（CPU 侧）：`d3d9::clear`（TARGET 位）、`d3d9::lockRect`（锁的是当前镜像目标时）；
//   · 换 target：`rasterPrepare`（基址/尺寸/格式任一变化）、`d3d9_pipeline::setRenderTarget`；
//   · 呈现：`d3d9::present`（读回）、SDL_GPU 的 `directPresent`。
//   ★ `rasterSyncFromGuest` 反过来：调用方保证 guest 是权威 ⇒ 队列非空 / 有未提交上传是**错误**
//     （⛔ 不静默覆盖）。
//   ★ 失败口径：所有 staging 都在 CPU 侧（`PendingUpload::bytes`），命令缓冲只在 flush 里录、录完
//     立刻提交 ⇒ 失败时把队列与段**原样保留**重试即可，⛔ 不会留下"已录进 CB 却没提交"的半成品。
// ★ 能力边界（准入在 d3d9_pipeline.cpp 的 gpuEligible；这里只负责"画得出"）：
//   · 混合 = SDL_GPUColorTargetBlendState 的 **color / alpha 两段**（支持 D3D 的 separate alpha）；
//     映射不了的因子/op 明确失败（d3d9_pipeline 会先拒绝，这里再兜一层）。
//   · 纹理 = stage 0 单级 32bpp；片元置换（GpuTexMode）选择对应 entrypoint。
//   · 派生 mip（`REQ-01M73G2724479XAVS176WBZ65Q`）：`num_levels` = 整链，level 1..N 由 box filter
//     从**完整 level 0** 算好逐级整级上传；sampler 显式 `max_lod > 0`（缺省 0 会静默只用 level 0）。
//   · sampler = 缓存 (address U/V × MAG/MIN)；缺省 NEAREST+CLAMP（D3DXSprite 现状不变）。
//   · 纹理上传按 (guest 指针/尺寸/alpha) 缓存，**像素内容未变就不重传**
//     （TITLE 的 ID3DXSprite::End 曾因每次 draw 完整 upload 而 ~118–206 ms）。
#include "runtime/present_sdl.h"

#if defined(_WIN32)
#include <d3dcompiler.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "runtime/gpu_raster.h"
#include "runtime/d3d9.h"  // 派生 mip 的尺寸 / 偏移 / box filter（唯一一份实现，见 d3d9.cpp）
#include "runtime/pixel_format.h"

namespace lift {
namespace {

// ★ 进程级诊断计数（理由见 gpu_raster.h 的 `GpuRasterStats`）：同一时刻只有一个 SDL_GPU 后端。
GpuRasterStats g_stats;

#if defined(_WIN32)
const char* kShaderSource = R"(
struct VSIn { float3 pos : TEXCOORD0; float4 color : TEXCOORD1; float2 uv : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float4 color : TEXCOORD0; float2 uv : TEXCOORD1; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); o.color = i.color; o.uv = i.uv; return o; }
float4 PSMain(VSOut i) : SV_Target { return i.color; }
Texture2D g_tex : register(t0, space2);
SamplerState g_samp : register(s0, space2);
// 0: MODULATE(texture, diffuse)（颜色与 alpha 都是）
float4 PSMainTex(VSOut i) : SV_Target { return g_tex.Sample(g_samp, i.uv) * i.color; }
// 1: SELECTARG1(TEXTURE)（颜色与 alpha 都取纹理）
float4 PSMainTexSelect(VSOut i) : SV_Target { return g_tex.Sample(g_samp, i.uv); }
// 2: 颜色 MODULATE，alpha 取纹理
float4 PSMainTexModAlphaTex(VSOut i) : SV_Target {
    float4 t = g_tex.Sample(g_samp, i.uv); float4 c = t * i.color; c.a = t.a; return c;
}
// 3: 颜色取纹理，alpha MODULATE
float4 PSMainTexSelAlphaMod(VSOut i) : SV_Target {
    float4 t = g_tex.Sample(g_samp, i.uv); float4 c = t; c.a = t.a * i.color.a; return c;
}
// 4: 颜色 MODULATE，alpha 取 diffuse
float4 PSMainTexModAlphaDiff(VSOut i) : SV_Target {
    float4 t = g_tex.Sample(g_samp, i.uv); float4 c = t * i.color; c.a = i.color.a; return c;
}
// 5: 颜色取纹理，alpha 取 diffuse
float4 PSMainTexSelAlphaDiff(VSOut i) : SV_Target {
    float4 t = g_tex.Sample(g_samp, i.uv); float4 c = t; c.a = i.color.a; return c;
}
)";
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_DXBC;
constexpr const char* kShaderFormatName = "DXBC";
#elif defined(__APPLE__)
// SDL_GPU 的 Metal 后端直接吃 MSL 源码：
// · 顶点属性走 [[stage_in]]（location 与 pipeline 的 vertex attributes 对齐）；
// · 片段插值走 [[user(locnN)]]（与 HLSL 的 TEXCOORD0/1 对齐）；
// · 纹理/采样器 = [[texture(0)]] / [[sampler(0)]]（SDL 的 MSL 绑定约定）。
const char* kShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSIn {
    float3 pos [[attribute(0)]];
    float4 color [[attribute(1)]];
    float2 uv [[attribute(2)]];
};

struct VSOut {
    float4 pos [[position]];
    float4 color [[user(locn0)]];
    float2 uv [[user(locn1)]];
};

vertex VSOut VSMain(VSIn in [[stage_in]]) {
    VSOut out;
    out.pos = float4(in.pos, 1.0);
    out.color = in.color;
    out.uv = in.uv;
    return out;
}

struct FSIn {
    float4 color [[user(locn0)]];
    float2 uv [[user(locn1)]];
};

fragment float4 PSMain(FSIn in [[stage_in]]) {
    return in.color;
}

fragment float4 PSMainTex(FSIn in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler samp [[sampler(0)]]) {
    return tex.sample(samp, in.uv) * in.color;
}

fragment float4 PSMainTexSelect(FSIn in [[stage_in]],
                                texture2d<float> tex [[texture(0)]],
                                sampler samp [[sampler(0)]]) {
    return tex.sample(samp, in.uv);
}

fragment float4 PSMainTexModAlphaTex(FSIn in [[stage_in]],
                                     texture2d<float> tex [[texture(0)]],
                                     sampler samp [[sampler(0)]]) {
    float4 t = tex.sample(samp, in.uv);
    float4 c = t * in.color;
    c.a = t.a;
    return c;
}

fragment float4 PSMainTexSelAlphaMod(FSIn in [[stage_in]],
                                     texture2d<float> tex [[texture(0)]],
                                     sampler samp [[sampler(0)]]) {
    float4 t = tex.sample(samp, in.uv);
    float4 c = t;
    c.a = t.a * in.color.a;
    return c;
}

fragment float4 PSMainTexModAlphaDiff(FSIn in [[stage_in]],
                                      texture2d<float> tex [[texture(0)]],
                                      sampler samp [[sampler(0)]]) {
    float4 t = tex.sample(samp, in.uv);
    float4 c = t * in.color;
    c.a = in.color.a;
    return c;
}

fragment float4 PSMainTexSelAlphaDiff(FSIn in [[stage_in]],
                                      texture2d<float> tex [[texture(0)]],
                                      sampler samp [[sampler(0)]]) {
    float4 t = tex.sample(samp, in.uv);
    float4 c = t;
    c.a = in.color.a;
    return c;
}
)MSL";
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_MSL;
constexpr const char* kShaderFormatName = "MSL";
#endif

// 纹理片元置换的 entrypoint 表（下标 = GpuTexMode 的整数值）
struct TexEntry {
    const char* entry;
    GpuTexMode mode;
};
constexpr TexEntry kTexEntries[] = {
    {"PSMainTex", GpuTexMode::ModulateBoth},
    {"PSMainTexSelect", GpuTexMode::SelectTextureBoth},
    {"PSMainTexModAlphaTex", GpuTexMode::ModulateColorSelectAlpha},
    {"PSMainTexSelAlphaMod", GpuTexMode::SelectColorModulateAlpha},
    {"PSMainTexModAlphaDiff", GpuTexMode::ModulateColorDiffuseAlpha},
    {"PSMainTexSelAlphaDiff", GpuTexMode::SelectColorDiffuseAlpha},
};
constexpr std::size_t kTexEntryCount = sizeof(kTexEntries) / sizeof(kTexEntries[0]);

// ★ 诊断用：到目前为止入队的**带批号** draw 数（`LIFT_GPU_FF_TEX` 的 `[gpu-depth]` 用它定位
//   `Clear(ZBUFFER)` 落在哪两批之间）。只在探针开启时被读，⛔ 不参与任何判据。
std::uint64_t g_ff_tag_seq = 0;

std::uint64_t hashPixels(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height) {
    std::uint64_t h = 1469598103934665603ull;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * pitch;
        std::size_t i = 0;
        for (; i + 8 <= row_bytes; i += 8) {
            std::uint64_t v = 0;
            std::memcpy(&v, row + i, sizeof(v));
            h ^= v;
            h *= 1099511628211ull;
            h ^= h >> 29;
        }
        for (; i < row_bytes; ++i) {
            h ^= row[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

// 32bpp 的**内存布局**必须是 BGRA（B,G,R,X / B,G,R,A）：target、纹理上传、回读都按这个布局
// 逐字节搬运（CPU 侧 pixel_format 也按同一份掩码做换算）。⛔ 不猜其它掩码 —— 换通道会把颜色画错。
bool bgraLayout(const SurfaceFormat& f) {
    return f.bpp == 32 && f.rmask == 0x00FF0000u && f.gmask == 0x0000FF00u && f.bmask == 0x000000FFu &&
           (f.amask == 0u || f.amask == 0xFF000000u);
}

bool sameFormat(const SurfaceFormat& a, const SurfaceFormat& b) {
    return a.bpp == b.bpp && a.rmask == b.rmask && a.gmask == b.gmask && a.bmask == b.bmask && a.amask == b.amask;
}

bool sdlBlendFactor(GpuBlendFactor f, SDL_GPUBlendFactor* out) {
    switch (f) {
        case GpuBlendFactor::Zero: *out = SDL_GPU_BLENDFACTOR_ZERO; return true;
        case GpuBlendFactor::One: *out = SDL_GPU_BLENDFACTOR_ONE; return true;
        case GpuBlendFactor::SrcColor: *out = SDL_GPU_BLENDFACTOR_SRC_COLOR; return true;
        case GpuBlendFactor::InvSrcColor: *out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR; return true;
        case GpuBlendFactor::DstColor: *out = SDL_GPU_BLENDFACTOR_DST_COLOR; return true;
        case GpuBlendFactor::InvDstColor: *out = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR; return true;
        case GpuBlendFactor::SrcAlpha: *out = SDL_GPU_BLENDFACTOR_SRC_ALPHA; return true;
        case GpuBlendFactor::InvSrcAlpha: *out = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; return true;
        case GpuBlendFactor::DstAlpha: *out = SDL_GPU_BLENDFACTOR_DST_ALPHA; return true;
        case GpuBlendFactor::InvDstAlpha: *out = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA; return true;
        case GpuBlendFactor::SrcAlphaSaturate: *out = SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE; return true;
    }
    return false;
}

bool sdlBlendOp(GpuBlendOp op, SDL_GPUBlendOp* out) {
    switch (op) {
        case GpuBlendOp::Add: *out = SDL_GPU_BLENDOP_ADD; return true;
        case GpuBlendOp::Subtract: *out = SDL_GPU_BLENDOP_SUBTRACT; return true;
        case GpuBlendOp::ReverseSubtract: *out = SDL_GPU_BLENDOP_REVERSE_SUBTRACT; return true;
        case GpuBlendOp::Min: *out = SDL_GPU_BLENDOP_MIN; return true;
        case GpuBlendOp::Max: *out = SDL_GPU_BLENDOP_MAX; return true;
    }
    return false;
}

SDL_GPUColorComponentFlags sdlColorMask(std::uint8_t mask) {
    SDL_GPUColorComponentFlags out = 0;
    // D3DCOLORWRITEENABLE_RED=1 / GREEN=2 / BLUE=4 / ALPHA=8；SDL 的位序相同但语义各自声明。
    if ((mask & 0x1u) != 0) out |= SDL_GPU_COLORCOMPONENT_R;
    if ((mask & 0x2u) != 0) out |= SDL_GPU_COLORCOMPONENT_G;
    if ((mask & 0x4u) != 0) out |= SDL_GPU_COLORCOMPONENT_B;
    if ((mask & 0x8u) != 0) out |= SDL_GPU_COLORCOMPONENT_A;
    return out;
}

class SdlGpuRaster final : public GpuRaster, public GpuDirectPresenter {
public:
    ~SdlGpuRaster() override { shutdown(); }

    bool init(std::string* err) {
#if defined(_WIN32) || defined(__APPLE__)
        device_ = SDL_CreateGPUDevice(kShaderFormat, true, nullptr);
        if (device_ == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUDevice(") + kShaderFormatName + ") 失败：" + SDL_GetError();
            return false;
        }
        const char* drv = SDL_GetGPUDeviceDriver(device_);
        name_ = std::string("sdl-gpu/") + (drv ? drv : "?") + "/" + kShaderFormatName;
        if (!createShaders(err)) return false;
        return true;
#else
        if (err) *err = "SDL_GPU 光栅化后端：当前平台没有内置 shader 后端（Windows=DXBC / macOS=MSL）";
        return false;
#endif
    }

    bool rasterActive() const override { return device_ != nullptr && vertex_shader_ != nullptr; }
    const char* rasterName() const override { return name_.c_str(); }
    bool rasterSpritesEnabled() const override { return true; }

    bool rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err,
                       const std::uint8_t* color_base = nullptr) override {
        if (format.bpp != 32) {
            if (err) *err = "SDL_GPU 只支持 32bpp 后缓冲";
            return false;
        }
        if (!bgraLayout(format)) {
            if (err) *err = "SDL_GPU 只支持 BGRA 内存布局的 32bpp 后缓冲（mask 不是 X8R8G8B8/A8R8G8B8）";
            return false;
        }
        const bool alpha = format.amask != 0;
        // ★ 身份 = (guest 像素基址, 尺寸, 格式)。`color_base == nullptr` = 调用方不提供身份
        //   （夹具 / 离屏上传）⇒ 只按尺寸/格式判（旧口径），⛔ 不要因此把已绑定的身份清掉。
        const bool same_identity = color_base == nullptr || target_base_ == nullptr || target_base_ == color_base;
        if (target_ != nullptr && width_ == width && height_ == height && alpha_ == alpha &&
            sameFormat(target_format_, format) && same_identity) {
            // 首次带着 base 调进来（镜像由夹具建、身份还是 nullptr）⇒ 记下身份。
            if (color_base != nullptr) target_base_ = color_base;
            return true;
        }
        // ★ 换 target（哪怕同尺寸！）：先把已入队的 draw 画进**旧**镜像并回读到旧目标的 guest 像素。
        //   ⛔ 不能把旧像素留在即将销毁/复用的纹理里，也不能让下一条 draw 画进上一块 surface 的镜像。
        if (target_ != nullptr && (dirty_ || !queue_.empty())) {
            if (!rasterFlush(err)) return false;
            if (dirty_ && target_base_ != nullptr) {
                std::string sync_err;
                const std::uint32_t old_pitch = target_pitch_ != 0 ? target_pitch_ : static_cast<std::uint32_t>(width_) * 4u;
                if (!downloadInto(const_cast<std::uint8_t*>(target_base_), old_pitch, width_, height_, &sync_err)) {
                    if (err) *err = "换 target 前读回旧镜像失败：" + sync_err;
                    return false;
                }
                dirty_ = false;
            }
        }
        destroyTargets();
        width_ = width;
        height_ = height;
        alpha_ = alpha;
        target_format_ = format;
        target_base_ = color_base;
        // ★ 注意：这里先记身份；下面建纹理失败时 destroyTargets() 会把 target_ 清掉 ⇒ 必须同时
        //   把 target_base_ 清回去，否则 rasterTargetBase() 会指向一块**不存在**的镜像。
        target_pitch_ = static_cast<std::uint32_t>(width) * 4u;
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = static_cast<Uint32>(width);
        ci.height = static_cast<Uint32>(height);
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        target_ = SDL_CreateGPUTexture(device_, &ci);
        ci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_ = SDL_CreateGPUTexture(device_, &ci);
        if (target_ == nullptr || depth_ == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUTexture 失败：") + SDL_GetError();
            destroyTargets();
            target_base_ = nullptr;
            width_ = 0;
            height_ = 0;
            return false;
        }
        need_depth_clear_ = true;
        dirty_ = false;
        // 新 target 的内容是未定义的 ⇒ 不能声称"本帧已经在 target 里"
        target_current_ = false;
        return true;
    }

    bool rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                             std::string* err) override {
        // ★ guest 是权威 ⇒ 队列里未提交的 draw / 未提交的纹理上传都会被这次覆盖。调用方必须先
        //   flush（约定见 gpu_raster.h）；这里明确拒绝，⛔ 不静默丢像素。
        if (!queue_.empty() || hasPendingUploads()) {
            if (err) *err = "SDL_GPU：队列非空（或有未提交的纹理上传）时不能从 guest 覆盖镜像（先 rasterFlush）";
            return false;
        }
        if (target_ == nullptr || width != width_ || height != height_) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        if (target_base_ != nullptr && pixels != target_base_) {
            if (err) *err = "SDL_GPU：上传的像素基址不是当前镜像的目标（换 target 必须先准备）";
            return false;
        }
        if (!uploadGuest(pixels, pitch, width, height, err)) return false;
        // ★ 这里**不**置 need_depth_clear_：只换颜色内容（guest 后缓冲是权威）不改深度。
        //   D3D9 里只有 `Clear(ZBUFFER)`（⇒ rasterRequestDepthClear）与"新 target"会清深度；
        //   旧实现每次颜色同步都清 ⇒ 帧内 Clear(TARGET)/CPU 回退之后的下一批 draw 会把先前的
        //   深度一起清掉（多段渲染下就是"先画的深度写失效"）。
        dirty_ = false;
        target_current_ = true;
        return true;
    }

    bool rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                           std::string* err) override {
        // ★ 读 target 前必须先 flush（屏障清单第一条）。队列空时是零成本。
        if (!rasterFlush(err)) return false;
        return downloadInto(pixels, pitch, width, height, err);
    }

    // target → guest 像素（BGRA8；A 字节按 guest X8 语义归一化）。只做搬运，不碰队列；
    // 调用方必须先 flush（`rasterSyncToGuest` / `rasterPrepare` 的换 target 路径各自负责）。
    bool downloadInto(std::uint8_t* pixels, std::uint32_t pitch, int width, int height, std::string* err) {
        if (target_ == nullptr || width != width_ || height != height_) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        if (target_base_ != nullptr && pixels != target_base_) {
            if (err) *err = "SDL_GPU：读回的像素基址不是当前镜像的目标（换 target 未同步）";
            return false;
        }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = static_cast<Uint32>(static_cast<std::size_t>(width) * 4u * height);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUTransferBuffer(download) 失败：") + SDL_GetError();
            return false;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = target_;
        src.w = static_cast<Uint32>(width);
        src.h = static_cast<Uint32>(height);
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        dst.pixels_per_row = static_cast<Uint32>(width);
        dst.rows_per_layer = static_cast<Uint32>(height);
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence != nullptr) {
            ++g_stats.submits;
            ++g_stats.submits_readback;
        }
        if (fence == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_SubmitGPUCommandBufferAndAcquireFence 失败";
            return false;
        }
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        const void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(download) 失败";
            return false;
        }
        for (int y = 0; y < height; ++y) {
            auto* out = pixels + static_cast<std::size_t>(y) * pitch;
            std::memcpy(out, static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * width * 4,
                        static_cast<std::size_t>(width) * 4);
            if (!alpha_) {
                for (int x = 0; x < width; ++x) out[x * 4 + 3] = 0;
            }
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        dirty_ = false;
        return true;
    }

    // -----------------------------------------------------------------------------------
    // 诊断探针（`LIFT_GPU_FF_TEX=1`）：把一张**采样纹理**的 level 0 整级读回 CPU。
    // ★ 只读：独立的 command buffer + fence 等待，⛔ 不改任何状态、⛔ 不计入 `g_stats`
    //   （计数器的口径是"光栅化/呈现路径的提交次数"，诊断不该污染它）。
    // ★ 为什么读**整级**而不是 1×1：D3D12 的 buffer row pitch 有 256 字节对齐要求，1 texel 宽的
    //   下载会落到"行距 4 字节"这种边角上；整级读回的行距 = w*4（2048 ⇒ 8192，天然对齐），
    //   而且顺带给出"整幅是不是空"的判据。代价 = 一张 2048² 的 16 MiB 拷贝，只在探针开启时付一次。
    bool downloadTextureLevel0(SDL_GPUTexture* tex, int width, int height, std::vector<std::uint8_t>* out) {
        if (tex == nullptr || width <= 0 || height <= 0 || out == nullptr) return false;
        const std::size_t bytes = static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height);
        if (bytes > 0xFFFFFFFFull) return false;
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = static_cast<Uint32>(bytes);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) return false;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            return false;
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = tex;
        src.w = static_cast<Uint32>(width);
        src.h = static_cast<Uint32>(height);
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        // 0 = 由 SDL 按目标区域宽度推行距（= w*4，紧密）⇒ 映射出来的字节就是 BGRA 紧排。
        dst.pixels_per_row = 0;
        dst.rows_per_layer = 0;
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            return false;
        }
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        bool ok = false;
        const void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
        if (mapped != nullptr) {
            const auto* p = static_cast<const std::uint8_t*>(mapped);
            out->assign(p, p + bytes);
            SDL_UnmapGPUTransferBuffer(device_, tb);
            ok = true;
        }
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return ok;
    }

    // env 只读一次（`LIFT_GPU_FF_TEX` 关掉时下面所有调用点都在第一行短路）。
    static const char* ffTexProbeEnv() {
        static const char* const env = std::getenv("LIFT_GPU_FF_TEX");
        return env;
    }

    // 诊断**轨迹**总开关：`LIFT_GPU_FF_TEX`（纹理探针）**或** `LIFT_GPU_DEPTH_TRACE`（深度/批次轨迹）。
    // ★ 为什么分开：修复自身的可观测性（`[gpu-depth] arm=clear` / `[gpu-pass]` / `[gpu-ff-z]`）不该
    //   挂在"纹理探针"那个无关的 env 上 —— 只想看深度时序的人不该被迫付整级纹理下载的钱。
    static bool gpuTraceEnabled() {
        static const bool on = [] {
            const char* a = std::getenv("LIFT_GPU_FF_TEX");
            const char* b = std::getenv("LIFT_GPU_DEPTH_TRACE");
            return (a != nullptr && a[0] != '\0') || (b != nullptr && b[0] != '\0');
        }();
        return on;
    }

    // ★ 旧接口 = 入队 + 立即 flush（语义不变；fixtures / `--gpu-verify` 靠它"画完立刻能读"）。
    bool rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                    std::string* err) override {
        if (!rasterQueue(vertices, count, nullptr, state, err)) return false;
        return rasterFlush(err);
    }

    bool rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterTexture& texture,
                            const GpuRasterState& state, std::string* err) override {
        if (!rasterQueue(vertices, count, &texture, state, err)) return false;
        return rasterFlush(err);
    }

    bool rasterQueue(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterTexture* texture,
                     const GpuRasterState& state, std::string* err) override {
        if (count == 0) return true;
        if (target_ == nullptr || depth_ == nullptr) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        QueuedDraw draw;
        draw.vertices.assign(vertices, vertices + count);
        draw.state = state;
        draw.textured = texture != nullptr;
        // ★ pipeline 在入队时解析：flush 里只剩"录命令"，把可失败点前移（失败 ⇒ 调用方回退 CPU）。
        draw.pipeline = pipelineFor(state, draw.textured, err);
        if (draw.pipeline == nullptr) return false;
        // 顶点缓冲在入队时就长够（flush 里不再创建 GPU 缓冲）。
        const std::uint64_t need_bytes = (static_cast<std::uint64_t>(queued_vertices_) + count) * sizeof(Vertex);
        if (need_bytes > 0xFFFFFFFFull) {
            if (err) *err = "SDL_GPU：一批顶点的总字节数超过 4 GiB";
            return false;
        }
        if (!ensureVertexBuffer(static_cast<Uint32>(need_bytes), err)) return false;
        // ★ 纹理在**入队时**就交给后端（像素拷进 staging；⛔ 不存 guest 指针）。
        //   采样矩形先算好（后面的内容变更要拿它判"要不要切段"）；`ensureTexture` 可能因此
        //   在本段后面切开一个新段。
        const std::uint64_t uploads_before = g_stats.uploads;
        if (texture != nullptr) {
            const int tex_w = texture->pixels != nullptr ? texture->width : 1;
            const int tex_h = texture->pixels != nullptr ? texture->height : 1;
            const bool wrap_u = texture->sampler.address_u != GpuAddress::Clamp;
            const bool wrap_v = texture->sampler.address_v != GpuAddress::Clamp;
            draw.sample_rect = gpuDrawSampleRect(vertices, count, tex_w, tex_h, wrap_u, wrap_v);
            if (!ensureTexture(*texture, &draw.texture, err)) return false;
            draw.sampler = samplerFor(texture->sampler, err);
            if (draw.sampler == nullptr) return false;
        }
        draw.vertex_offset = static_cast<std::uint32_t>(queued_vertices_);
        queued_vertices_ += count;
        // ★ 批号在**入队那一刻**取值（生产者随队列设/清）；flush 时按条目回报，⛔ 不读全局当前值。
        draw.batch_tag = gpuRasterBatchTag();
        // ★ 诊断探针（`LIFT_GPU_FF_TEX=1`）：装不装、采哪个 texel 全由纯函数判（`gpu_raster.h`）。
        //   ⛔ 这一段无论走不走都不碰像素/状态；env 关闭时 `gpuFfTexProbeWanted` 第一行就 false。
        if (gpuTraceEnabled() && draw.batch_tag != 0 && !draw.vertices.empty()) {
            ++g_ff_tag_seq;
            // 每批一行的 z 范围（`a12` §5 的 `<待核>` #1：`[draw]` 的 `z=` 只是状态位，不是顶点 z）。
            float zlo = draw.vertices[0].z;
            float zhi = zlo;
            float xlo = draw.vertices[0].x, xhi = xlo;
            float ylo = draw.vertices[0].y, yhi = ylo;
            for (const GpuRasterVertex& v : draw.vertices) {
                if (v.z < zlo) zlo = v.z;
                if (v.z > zhi) zhi = v.z;
                if (v.x < xlo) xlo = v.x;
                if (v.x > xhi) xhi = v.x;
                if (v.y < ylo) ylo = v.y;
                if (v.y > yhi) yhi = v.y;
            }
            std::fprintf(stderr,
                         "[gpu-ff-z] tag=%llu tex=%d zr=%.4f..%.4f depth=%d/%d/%u count=%zu "
                         "bb=%.1f,%.1f..%.1f,%.1f cw=%u blend=%d/%u,%u/%u\n",
                         static_cast<unsigned long long>(draw.batch_tag), draw.textured ? 1 : 0,
                         static_cast<double>(zlo), static_cast<double>(zhi), draw.state.depth_test ? 1 : 0,
                         draw.state.depth_write ? 1 : 0, draw.state.depth_func, count, static_cast<double>(xlo),
                         static_cast<double>(ylo), static_cast<double>(xhi), static_cast<double>(yhi),
                         static_cast<unsigned>(draw.state.color_write), draw.state.blend ? 1 : 0,
                         static_cast<unsigned>(draw.state.src_blend), static_cast<unsigned>(draw.state.dst_blend),
                         static_cast<unsigned>(draw.state.tex_mode));
        }
        if (texture != nullptr && texture->pixels != nullptr) {
            const int tex_w = texture->width;
            const int tex_h = texture->height;
            const std::uint64_t key = gpuTextureCacheKey(*texture, tex_w, tex_h);
            const bool already = std::find(ff_probe_keys_.begin(), ff_probe_keys_.end(), key) !=
                                 ff_probe_keys_.end();
            int px = 0;
            int py = 0;
            if (ff_probe_keys_.size() < kFfProbeMax && ff_probe_round_ < kFfProbeRounds &&
                gpuFfTexProbeWanted(ffTexProbeEnv(), already, draw.batch_tag, true) && !draw.vertices.empty() &&
                gpuFfTexProbeTexel(draw.vertices[0].u, draw.vertices[0].v, tex_w, tex_h, &px, &py)) {
                ff_probe_keys_.push_back(key);
                draw.probe.armed = true;
                draw.probe.x = px;
                draw.probe.y = py;
                draw.probe.width = static_cast<std::uint32_t>(tex_w);
                draw.probe.height = static_cast<std::uint32_t>(tex_h);
                draw.probe.generation = texture->generation;
                draw.probe.has_alpha = texture->format.amask != 0;
                draw.probe.serial = texture->serial;
                draw.probe.upload_blocks = static_cast<std::uint32_t>(g_stats.uploads - uploads_before);
                draw.probe.round = ff_probe_round_;
                const auto rec = textures_.find(key);
                draw.probe.recorded = rec != textures_.end() ? rec->second.recorded_generation : 0u;
                const std::uint8_t* gp = texture->pixels + static_cast<std::size_t>(py) *
                                                               static_cast<std::size_t>(texture->pitch) +
                                         static_cast<std::size_t>(px) * 4u;
                std::memcpy(draw.probe.guest, gp, 4);
                // 整幅哈希：**这一条**才是"GPU 上的副本与 guest 是同一份吗"的判据（单 texel 只是定位）。
                draw.probe.guest_hash = hashPixels(texture->pixels, texture->pitch, tex_w, tex_h);
                const GpuRasterVertex& v0 = draw.vertices[0];
                const float nx = v0.x / static_cast<float>(width_) * 2.0f - 1.0f;
                const float ny = 1.0f - v0.y / static_cast<float>(height_) * 2.0f;
                // 整批 z 的范围：只打 `v0.z` 会把"深度被谁判掉"这条线索藏起来
                //   （`a12` §5 的 `<待核>` #1：`[draw]` 的 `z=` 是状态位，不是顶点 z 范围）。
                float zmin = v0.z;
                float zmax = v0.z;
                for (const GpuRasterVertex& v : draw.vertices) {
                    if (v.z < zmin) zmin = v.z;
                    if (v.z > zmax) zmax = v.z;
                }
                std::fprintf(stderr,
                             "[gpu-ff-tex] round=%zu tag=%llu texserial=%u tex=%dx%d gen=%u rec=%u up=%u "
                             "v0_screen=(%.1f,%.1f,%.4f) v0_ndc=(%.4f,%.4f,%.4f) uv=(%.4f,%.4f) zr=%.4f..%.4f "
                             "bind_off=%u count=%zu tgt=%dx%d vp=%d,%d,%d,%d sc=%d,%d..%d,%d "
                             "depth=%d/%d/%u texel_xy=%d,%d\n",
                             draw.probe.round, static_cast<unsigned long long>(draw.batch_tag), draw.probe.serial,
                             tex_w, tex_h, draw.probe.generation, draw.probe.recorded, draw.probe.upload_blocks,
                             static_cast<double>(v0.x), static_cast<double>(v0.y), static_cast<double>(v0.z),
                             static_cast<double>(nx), static_cast<double>(ny), static_cast<double>(v0.z),
                             static_cast<double>(v0.u), static_cast<double>(v0.v), static_cast<double>(zmin),
                             static_cast<double>(zmax), draw.vertex_offset, count, width_, height_,
                             draw.state.viewport_x, draw.state.viewport_y, draw.state.viewport_w,
                             draw.state.viewport_h, draw.state.scissor_x0, draw.state.scissor_y0,
                             draw.state.scissor_x1, draw.state.scissor_y1, draw.state.depth_test ? 1 : 0,
                             draw.state.depth_write ? 1 : 0, draw.state.depth_func, px, py);
            }
        }
        queue_.push_back(std::move(draw));
        return true;
    }

    // ★ 一次 flush = **一个** command buffer，内部按段 `[copy 上传][render 画]` 交替，最后只 Submit 一次。
    //   · 队列为空且没有待上传内容 ⇒ 零成本 true（⛔ 不提交空 command buffer）；
    //   · depth 只在**本次 flush 的第一个 render pass** 清（一帧一次；分段后第二段起必须 LOAD，
    //     ⛔ 不能每段都清 —— 那会把前面已画进去的颜色/深度一起丢掉）；
    //   · 所有段的上传合到**一个** transfer buffer，每块各自 512 对齐 offset、**只写一次**
    //     ⇒ 同一 command buffer 内不会覆盖仍在途的数据（SDL 在提交之后才执行 upload）。
    bool rasterFlush(std::string* err) override {
        const bool has_uploads = hasPendingUploads();
        const bool had_draws = !queue_.empty();
        if (!had_draws && !has_uploads) return true;
        if (had_draws && (target_ == nullptr || depth_ == nullptr)) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        // 统一按"段序 = 编码序"遍历所有待上传块
        auto for_each_upload = [&](auto&& fn) {
            for (Segment& s : segments_) {
                for (PendingUpload& u : s.uploads) fn(u);
            }
        };

        // ---- 1) 待上传纹理：所有段的所有块合到**一个** transfer buffer ----
        std::size_t texture_bytes = 0;
        for_each_upload([&](PendingUpload& u) {
            texture_bytes = (texture_bytes + 511u) & ~static_cast<std::size_t>(511u);
            texture_bytes += u.size;
        });
        if (texture_bytes > 0xFFFFFFFFull) {
            if (err) *err = "SDL_GPU：一帧待上传的纹理字节数超过 4 GiB";
            return false;
        }
        SDL_GPUTransferBuffer* tex_tb = nullptr;
        SDL_GPUTransferBuffer* vtx_tb = nullptr;
        auto release_tbs = [&]() {
            if (tex_tb != nullptr) SDL_ReleaseGPUTransferBuffer(device_, tex_tb);
            if (vtx_tb != nullptr) SDL_ReleaseGPUTransferBuffer(device_, vtx_tb);
            tex_tb = nullptr;
            vtx_tb = nullptr;
        };
        if (texture_bytes > 0) {
            SDL_GPUTransferBufferCreateInfo ti{};
            ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            ti.size = static_cast<Uint32>(texture_bytes);
            tex_tb = SDL_CreateGPUTransferBuffer(device_, &ti);
            if (tex_tb == nullptr) {
                if (err) *err = std::string("SDL_CreateGPUTransferBuffer(textures) 失败：") + SDL_GetError();
                return false;  // 队列/段保留：下次还能重试
            }
            auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, tex_tb, false));
            if (mapped == nullptr) {
                release_tbs();
                if (err) *err = "SDL_MapGPUTransferBuffer(textures) 失败";
                return false;
            }
            std::size_t cursor = 0;
            for_each_upload([&](PendingUpload& u) {
                cursor = (cursor + 511u) & ~static_cast<std::size_t>(511u);
                u.transfer_offset = static_cast<std::uint32_t>(cursor);
                std::memcpy(mapped + cursor, staging_.data() + u.staging_offset, u.size);
                cursor += u.size;
            });
            SDL_UnmapGPUTransferBuffer(device_, tex_tb);
        }

        // ---- 2) 所有顶点合到**一个** transfer buffer（在第一个 copy pass 里上传）----
        const std::size_t vertex_bytes = static_cast<std::size_t>(queued_vertices_) * sizeof(Vertex);
        if (vertex_bytes > 0) {
            SDL_GPUTransferBufferCreateInfo ti{};
            ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            ti.size = static_cast<Uint32>(vertex_bytes);
            vtx_tb = SDL_CreateGPUTransferBuffer(device_, &ti);
            if (vtx_tb == nullptr) {
                release_tbs();
                if (err) *err = std::string("SDL_CreateGPUTransferBuffer(vertices) 失败：") + SDL_GetError();
                return false;
            }
            auto* mapped = static_cast<Vertex*>(SDL_MapGPUTransferBuffer(device_, vtx_tb, false));
            if (mapped == nullptr) {
                release_tbs();
                if (err) *err = "SDL_MapGPUTransferBuffer(vertices) 失败";
                return false;
            }
            Vertex* dst = mapped;
            for (const QueuedDraw& d : queue_) {
                for (const GpuRasterVertex& v : d.vertices) {
                    // 屏幕空间 → NDC（尺寸取**这一帧**的 target；换 target 前已经 flush，不会串）
                    dst->x = v.x / static_cast<float>(width_) * 2.0f - 1.0f;
                    dst->y = 1.0f - v.y / static_cast<float>(height_) * 2.0f;
                    dst->z = v.z;
                    dst->r = v.r;
                    dst->g = v.g;
                    dst->b = v.b;
                    dst->a = v.a;
                    dst->u = v.u;
                    dst->v = v.v;
                    ++dst;
                }
            }
            SDL_UnmapGPUTransferBuffer(device_, vtx_tb);
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            release_tbs();
            if (err) *err = std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError();
            return false;  // 队列/段保留
        }

        // ---- 3) 逐段：copy pass（本段的上传）→ render pass（本段区间的 draw）----
        bool first_render_pass = true;
        bool encoded_render_pass = false;
        for (std::size_t seg_index = 0; seg_index < segments_.size(); ++seg_index) {
            Segment& seg = segments_[seg_index];
            const std::size_t draw_end =
                (seg_index + 1 < segments_.size()) ? segments_[seg_index + 1].first_draw : queue_.size();
            const bool seg_has_draws = seg.first_draw < draw_end;
            const bool first_copy_pass = seg_index == 0;
            if (first_copy_pass || !seg.uploads.empty()) {
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
                for (PendingUpload& u : seg.uploads) {
                    SDL_GPUTextureTransferInfo src{};
                    src.transfer_buffer = tex_tb;
                    src.offset = u.transfer_offset;
                    // 0 = 由 SDL 按 dst 区域的 w/h 推"紧密排列"（见上面 ⑥ 的注释）。
                    src.pixels_per_row = 0;
                    src.rows_per_layer = 0;
                    SDL_GPUTextureRegion dst{};
                    dst.texture = u.texture;
                    dst.mip_level = u.mip_level;
                    dst.x = static_cast<Uint32>(u.rect.x);
                    dst.y = static_cast<Uint32>(u.rect.y);
                    dst.w = static_cast<Uint32>(u.rect.w);
                    dst.h = static_cast<Uint32>(u.rect.h);
                    dst.d = 1;
                    SDL_UploadToGPUTexture(copy, &src, &dst, /*cycle=*/u.cycle);
                }
                if (first_copy_pass && vertex_bytes > 0) {
                    SDL_GPUTransferBufferLocation src{};
                    src.transfer_buffer = vtx_tb;
                    src.offset = 0;
                    SDL_GPUBufferRegion dst{};
                    dst.buffer = vertex_buffer_;
                    dst.offset = 0;
                    dst.size = static_cast<Uint32>(vertex_bytes);
                    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                }
                SDL_EndGPUCopyPass(copy);
            }
            if (!seg_has_draws) continue;
            if (gpuTraceEnabled()) {
                std::fprintf(stderr,
                             "[gpu-pass] seg=%zu/%zu draws=%zu..%zu first=%d need_clear=%d load=%d queue=%zu\n",
                             seg_index, segments_.size(), seg.first_draw, draw_end, first_render_pass ? 1 : 0,
                             need_depth_clear_ ? 1 : 0,
                             static_cast<int>((need_depth_clear_ && first_render_pass) ? SDL_GPU_LOADOP_CLEAR
                                                                                        : SDL_GPU_LOADOP_LOAD),
                             queue_.size());
            }
            // render pass：color 始终 LOAD（第二段起必须保留前面画的内容）
            SDL_GPUColorTargetInfo color{};
            color.texture = target_;
            color.load_op = SDL_GPU_LOADOP_LOAD;
            color.store_op = SDL_GPU_STOREOP_STORE;
            // ★ depth：**一帧只清一次**（本 flush 的第一个 render pass）。分段后第二段必须 LOAD，
            //   ⛔ 每段都 CLEAR 会把先画进去的深度清掉（后画的内容就会被错误接受）。
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = depth_;
            depth.clear_depth = 1.0f;
            // ★ 诊断（`LIFT_GPU_DEPTH_CLEAR=frame`）：把"每个 flush 的第一段"当成需要清 —— 用来
            //   **验证**"深度附件自创建后没再清过"这条根因（`LIFT_GPU_FF_TEX` 的 `[gpu-depth]` 显示
            //   `arm=clear` 一次都没有）。语义上它就是"每帧清一次深度"，⛔ 缺省关闭、不是修复。
            depth.load_op = ((need_depth_clear_ || depthClearEveryFlush()) && first_render_pass)
                                ? SDL_GPU_LOADOP_CLEAR
                                : SDL_GPU_LOADOP_LOAD;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
            for (std::size_t i = seg.first_draw; i < draw_end; ++i) {
                const QueuedDraw& d = queue_[i];
                SDL_BindGPUGraphicsPipeline(pass, d.pipeline);
                SDL_GPUViewport vp{};
                vp.x = static_cast<float>(d.state.viewport_x);
                vp.y = static_cast<float>(d.state.viewport_y);
                vp.w = static_cast<float>(d.state.viewport_w);
                vp.h = static_cast<float>(d.state.viewport_h);
                vp.min_depth = 0.0f;
                vp.max_depth = 1.0f;
                SDL_SetGPUViewport(pass, &vp);
                SDL_Rect scissor{d.state.scissor_x0, d.state.scissor_y0, d.state.scissor_x1 - d.state.scissor_x0,
                                 d.state.scissor_y1 - d.state.scissor_y0};
                if (!d.state.scissor_enabled ||
                    (d.state.scissor_x0 == 0 && d.state.scissor_y0 == 0 && d.state.scissor_x1 == width_ &&
                     d.state.scissor_y1 == height_)) {
                    scissor = SDL_Rect{0, 0, width_, height_};
                }
                SDL_SetGPUScissor(pass, &scissor);
                SDL_GPUBufferBinding binding{vertex_buffer_,
                                             d.vertex_offset * static_cast<Uint32>(sizeof(Vertex))};
                SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
                if (d.textured) {
                    SDL_GPUTextureSamplerBinding tsb{d.texture, d.sampler};
                    SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
                }
                SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(d.vertices.size()), 1, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
            first_render_pass = false;
            encoded_render_pass = true;
        }
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            release_tbs();
            if (err) *err = std::string("SDL_SubmitGPUCommandBuffer 失败：") + SDL_GetError();
            return false;  // 队列/段保留（没提交成功 ⇒ 下次还能重试，⛔ 不静默丢像素）
        }
        release_tbs();
        ++g_stats.submits;
        ++g_stats.submits_flush;
        // ★ 覆盖账本（`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`）：**提交成功之后**才算是"真的录进了
        //   render pass"（提交失败的路径上面已经 `return false` 且保留队列）⇒ 这里逐条回报批号。
        //   ⛔ 不要放到 `rasterQueue` 里：那正是"入队当已覆盖"的原缺陷。
        for (const QueuedDraw& d : queue_) {
            if (d.batch_tag != 0) gpuLedgerNoteRecordedDraw(d.batch_tag);
        }

        // ---- 3b) 诊断探针（`LIFT_GPU_FF_TEX=1`）：**提交成功之后**把 level 0 读回来，与
        //   `rasterQueue` 里存下的 guest 同一 texel 对打。⛔ 只读：不改状态、不计入 `g_stats`、
        //   失败只打一行（⛔ 不许因此改 draw 的成败）。`probe.armed` 默认 false ⇒ 常态零成本。
        bool ff_probe_ran = false;
        for (const QueuedDraw& d : queue_) {
            if (!d.probe.armed || d.texture == nullptr) continue;
            ff_probe_ran = true;
            const int pw = static_cast<int>(d.probe.width);
            const int ph = static_cast<int>(d.probe.height);
            std::vector<std::uint8_t> level0;
            if (!downloadTextureLevel0(d.texture, pw, ph, &level0)) {
                std::fprintf(stderr, "[gpu-ff-tex] round=%zu texserial=%u texel=(%d,%d) download=FAILED\n",
                             d.probe.round, d.probe.serial, d.probe.x, d.probe.y);
                continue;
            }
            const std::size_t off = (static_cast<std::size_t>(d.probe.y) * static_cast<std::size_t>(pw) +
                                     static_cast<std::size_t>(d.probe.x)) * 4u;
            const std::uint8_t* g = level0.data() + off;
            std::size_t nonzero = 0;
            std::size_t alpha_nonzero = 0;
            for (std::size_t i = 0; i + 3u < level0.size(); i += 4u) {
                if ((level0[i] | level0[i + 1] | level0[i + 2] | level0[i + 3]) != 0u) ++nonzero;
                if (level0[i + 3u] != 0u) ++alpha_nonzero;
            }
            // 整幅内容对打：GPU 下载的 level 0（行距 = w*4）vs 入队时 guest 的整幅哈希。
            const std::uint64_t gpu_hash = hashPixels(level0.data(), static_cast<std::uint32_t>(pw) * 4u, pw, ph);
            const std::uint8_t* q = d.probe.guest;  // guest 原始字节序 = B,G,R,X/A
            std::fprintf(
                stderr,
                "[gpu-ff-tex] round=%zu texserial=%u texel=(%d,%d) guest=(R%u,G%u,B%u,A%u)[B%u G%u R%u X%u] "
                "gpu=(R%u,G%u,B%u,A%u) same=%d hash_same=%d gpu_nonzero=%zu alpha_nonzero=%zu n=%zu "
                "amask=%d gen=%u rec=%u up=%u tex=%dx%d\n",
                d.probe.round, d.probe.serial, d.probe.x, d.probe.y, q[2], q[1], q[0],
                d.probe.has_alpha ? q[3] : static_cast<std::uint8_t>(255), q[0], q[1], q[2], q[3], g[2], g[1],
                g[0], g[3], gpuFfTexProbeSameTexel(q, g, d.probe.has_alpha) ? 1 : 0,
                gpu_hash == d.probe.guest_hash ? 1 : 0, nonzero, alpha_nonzero, level0.size() / 4u,
                d.probe.has_alpha ? 1 : 0, d.probe.generation, d.probe.recorded, d.probe.upload_blocks, pw, ph);
        }
        if (ff_probe_ran) {
            // 这一轮的纹理键作废 ⇒ 下一轮（下一帧）按同样的门控再采一次（最多 `kFfProbeRounds` 轮）。
            ++ff_probe_round_;
            ff_probe_keys_.clear();
        }

        // ---- 4) 记账：上传已落盘、段与队列清空、depth 已经清过 ----
        for (auto& kv : textures_) kv.second.pending = false;
        segments_.assign(1, Segment{});
        staging_used_ = 0;  // 已全部落进 transfer buffer ⇒ arena 还给下一帧（容量留着）
        queue_.clear();
        queued_vertices_ = 0;
        // ★ 只有本次 flush **真的录过 render pass** 才算"深度已经清过"：只录 copy pass 的 flush
        //   （上传批次没配成 draw 的失败边）不许把 pending 的深度清吃掉。
        if (encoded_render_pass) need_depth_clear_ = false;
        if (had_draws) dirty_ = true;
        evictTextures();
        return true;
    }

    // ★ 语义 = 「镜像/纹理缓存 != guest」：队列非空 **或** 已提交但尚未读回 **或** 还有已入队
    //   但没提交的纹理上传。缺了最后一项，屏障会漏判（已录未提交的上传被当成已落地）。
    bool rasterDirty() const override { return dirty_ || !queue_.empty() || hasPendingUploads(); }
    const void* rasterTargetBase() const override { return target_base_; }
    std::size_t rasterQueuedDraws() const override { return queue_.size(); }
    // 有没有"已入队但还没提交"的纹理上传（段里的 uploads）。
    bool hasPendingUploads() const {
        for (const Segment& s : segments_) {
            if (!s.uploads.empty()) return true;
        }
        return false;
    }

    void rasterRequestDepthClear() override {
        // ★ 交付契约（与 `gpu_raster.h` 的同名注释同口径）：一次 arm ⇒ **下一次真的录到 render pass
        //   的 flush** 的第一个 render pass 用 `LOADOP_CLEAR` 清成 1.0；多次 arm 合并；不绑 target 身份。
        //   ⛔ 调用方必须**先 flush 队列再 arm**；本场景实测第一帧 `arm=clear seq=0 queue=1`
        //   （窄窗**已命中**，但那条是不带批号的 sprite ⇒ 它关深度测试/写 ⇒ 无后果；见 gpu_raster.h）。
        //   ★ 诊断（`LIFT_GPU_FF_TEX` 或 `LIFT_GPU_DEPTH_TRACE`）：`seq` = 到目前为止入队的带批号
        //   draw 数 ⇒ 能判 arm 落在哪两批之间。
        if (gpuTraceEnabled()) {
            std::fprintf(stderr, "[gpu-depth] arm=clear seq=%llu queue=%zu\n",
                         static_cast<unsigned long long>(g_ff_tag_seq), queue_.size());
        }
        need_depth_clear_ = true;
    }

    void rasterDiscardTarget() override {
        // ★ 设备销毁前调用：队列与镜像都指向即将释放的 guest 像素 ⇒ 丢掉，⛔ 不回读。
        queue_.clear();
        queued_vertices_ = 0;
        segments_.assign(1, Segment{});
        staging_used_ = 0;
        staging_.clear();
        staging_.shrink_to_fit();
        for (auto& kv : textures_) {
            // 内容没上传过 ⇒ 缓存里的版本作废（下次按内容哈希重传 / 整幅上传）
            kv.second.pending = false;
            kv.second.hash = 0;
            kv.second.recorded_generation = 0;
        }
        destroyTargets();
        width_ = 0;
        height_ = 0;
        alpha_ = false;
        target_base_ = nullptr;
        target_pitch_ = 0;
        target_format_ = SurfaceFormat{};
        need_depth_clear_ = true;
        dirty_ = false;
        target_current_ = false;
    }

    GpuDirectPresenter* directPresenter() override { return device_ != nullptr ? this : nullptr; }

    // -----------------------------------------------------------------------------------
    // GPU 直出：窗口 swapchain（`--present window` 时由 SdlWindow 在创建窗口之后调用 attach）
    // -----------------------------------------------------------------------------------
    bool directAttachWindow(void* sdl_window, bool vsync, std::string* err) override {
        if (device_ == nullptr) {
            if (err) *err = "SDL_GPU device 未建";
            return false;
        }
        if (sdl_window == nullptr) {
            if (err) *err = "窗口指针为空";
            return false;
        }
        if (claimed_ && window_ == sdl_window) return true;
        directDetachWindow();
        window_ = static_cast<SDL_Window*>(sdl_window);
        // ★ 这是"直出到底有没有成立"的唯一判据：claim 成功才算。失败就如实交出 SDL 错误串，
        //   调用方回退 SDL_Renderer 上传路径（⛔ 不假装直出）。
        if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
            if (err) *err = std::string("SDL_ClaimWindowForGPUDevice 失败：") + SDL_GetError();
            window_ = nullptr;
            return false;
        }
        claimed_ = true;
        const SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        if (fmt != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && fmt != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
            if (err) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "swapchain 格式 %d 不是 8 位 UNORM（无法从 BGRA8 target blit）",
                              static_cast<int>(fmt));
                *err = buf;
            }
            directDetachWindow();
            return false;
        }
        swapchain_format_ = fmt;
        // present mode：vsync=1 用 VSYNC；vsync=0 优先 IMMEDIATE，不支持再退 MAILBOX / VSYNC（并打印实际值）。
        SDL_GPUPresentMode want = vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
        if (!SDL_WindowSupportsGPUPresentMode(device_, window_, want)) {
            std::fprintf(stderr, "[present] GPU 直出：%s 不受支持 ⇒ ",
                         vsync ? "VSYNC" : "IMMEDIATE");
            if (!vsync && SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX)) {
                want = SDL_GPU_PRESENTMODE_MAILBOX;
                std::fprintf(stderr, "退到 MAILBOX\n");
            } else {
                want = SDL_GPU_PRESENTMODE_VSYNC;
                std::fprintf(stderr, "退到 VSYNC\n");
            }
        }
        if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, want)) {
            // SDR+VSYNC 是规范里"永远支持"的组合；连它也失败 ⇒ 这块窗口不能直出。
            if (err) *err = std::string("SDL_SetGPUSwapchainParameters 失败：") + SDL_GetError();
            directDetachWindow();
            return false;
        }
        present_mode_ = want;
        switch (want) {
            case SDL_GPU_PRESENTMODE_IMMEDIATE: present_mode_name_ = "immediate"; break;
            case SDL_GPU_PRESENTMODE_MAILBOX: present_mode_name_ = "mailbox"; break;
            case SDL_GPU_PRESENTMODE_VSYNC:
            default: present_mode_name_ = "vsync"; break;
        }
        return true;
    }

    void directDetachWindow() override {
        releaseOverlayTexture();
        if (claimed_ && device_ != nullptr && window_ != nullptr) SDL_ReleaseWindowFromGPUDevice(device_, window_);
        claimed_ = false;
        window_ = nullptr;
        swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    }

    bool directActive() const override {
        return device_ != nullptr && claimed_ && window_ != nullptr &&
               swapchain_format_ != SDL_GPU_TEXTUREFORMAT_INVALID;
    }
    const char* directDriverName() const override {
        const char* drv = device_ != nullptr ? SDL_GetGPUDeviceDriver(device_) : nullptr;
        return drv != nullptr ? drv : "(unknown)";
    }
    const char* directPresentMode() const override { return present_mode_name_; }

    bool directUploadGuest(const SurfaceFormat& format, const std::uint8_t* pixels, std::uint32_t pitch, int width,
                           int height, std::string* err) override {
        if (pixels == nullptr) {
            if (err) *err = "guest 后缓冲指针为空";
            return false;
        }
        // ★ target 固定是 BGRA8（GPU 光栅化只认 32bpp）。guest 后缓冲是 16bpp / 非 BGRA8 的
        //   32bpp 时，按**同一份掩码语义**（pixel_format）转成 BGRA8 再上传 —— ⛔ 不让"直出"
        //   在这种后缓冲上退化成"窗口停在上一帧"。常规 32bpp 后缓冲走 verbatim 快路径。
        SurfaceFormat target_fmt = format;
        const bool same_bgra = format.bpp == 32 && format.rmask == 0x00FF0000u && format.gmask == 0x0000FF00u &&
                               format.bmask == 0x000000FFu;
        if (!same_bgra) {
            target_fmt = SurfaceFormat{};
            target_fmt.bpp = 32;
            target_fmt.rmask = 0x00FF0000u;
            target_fmt.gmask = 0x0000FF00u;
            target_fmt.bmask = 0x000000FFu;
            target_fmt.amask = 0u;
        }
        // ★ 上传前先 flush：队列里未提交的 draw 不属于这块 guest 像素（guest 是权威）⇒
        //   先把它们落到它们自己的 target，再整块覆盖；⛔ 不让未提交的 draw 悬在队列里。
        if (!rasterFlush(err)) return false;
        if (!rasterPrepare(target_fmt, width, height, err, pixels)) return false;
        if (same_bgra) {
            if (!uploadGuest(pixels, pitch, width, height, err)) return false;
        } else {
            std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height));
            const unsigned bpp = format.bytesPerPixel();
            for (int y = 0; y < height; ++y) {
                const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * pitch;
                std::uint8_t* out = bgra.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
                for (int x = 0; x < width; ++x) {
                    const std::uint32_t raw =
                        pixel_format::loadPixel(row + static_cast<std::size_t>(x) * bpp, bpp);
                    out[x * 4 + 0] = static_cast<std::uint8_t>(pixel_format::expandChannel(raw, format.bmask));
                    out[x * 4 + 1] = static_cast<std::uint8_t>(pixel_format::expandChannel(raw, format.gmask));
                    out[x * 4 + 2] = static_cast<std::uint8_t>(pixel_format::expandChannel(raw, format.rmask));
                    out[x * 4 + 3] = 255;
                }
            }
            if (!uploadGuest(bgra.data(), static_cast<std::uint32_t>(width) * 4u, width, height, err)) return false;
        }
        // ★ 同 rasterSyncFromGuest：整块颜色上传不等于"深度要清"。
        dirty_ = false;
        target_current_ = true;  // 上传完 target == guest
        return true;
    }

    void directSetTargetCurrent(bool current) override { target_current_ = current; }
    bool directTargetCurrent() const override { return target_current_; }

    bool directPresent(std::string* err) override {
        // ★ 呈现屏障：先把队列提交（正常路径 d3d9::Present 已经读过回、队列是空的，这里是零成本兜底）。
        if (!rasterFlush(err)) return false;
        if (!directActive()) {
            if (err) *err = "GPU 直出未建立（窗口没有 claim）";
            return false;
        }
        if (target_ == nullptr || width_ <= 0 || height_ <= 0) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            if (err) *err = std::string("SDL_AcquireGPUCommandBuffer 失败：") + SDL_GetError();
            return false;
        }
        SDL_GPUTexture* swap = nullptr;
        Uint32 sw = 0;
        Uint32 sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swap, &sw, &sh)) {
            std::string why = std::string("SDL_WaitAndAcquireGPUSwapchainTexture 失败：") + SDL_GetError();
            if (SDL_SubmitGPUCommandBuffer(cmd)) {  // 不 Cancel：Cancel 与 swapchain 的状态机容易踩边界
                ++g_stats.submits;
                ++g_stats.submits_present;
            }
            if (err) *err = why;
            return false;
        }
        if (swap == nullptr || sw == 0 || sh == 0) {
            // 最小化 / 隐藏到零尺寸：这一帧没有可呈现的目标。⛔ 不算失败，也不冒充"画上去了"。
            if (SDL_SubmitGPUCommandBuffer(cmd)) {
                ++g_stats.submits;
                ++g_stats.submits_present;
            }
            ++present_skipped_;
            return true;
        }
        SDL_GPUBlitInfo info{};
        info.source.texture = target_;
        info.source.w = static_cast<Uint32>(width_);
        info.source.h = static_cast<Uint32>(height_);
        info.destination.texture = swap;
        info.destination.w = static_cast<Uint32>(width_);
        info.destination.h = static_cast<Uint32>(height_);
        info.destination.x = 0;
        info.destination.y = 0;
        // ★ letterbox：窗口不一定与 target 同尺寸。整张 swapchain 先清成黑（= SDL 逻辑呈现的
        //   LETTERBOX 行为），再把目标矩形按整数像素缩放置中。过滤用 NEAREST —— 与 CPU 路径
        //   `SDL_SetTextureScaleMode(NEAREST)` 一致，⛔ 不在这里引入第二条缩放口径。
        info.load_op = SDL_GPU_LOADOP_CLEAR;
        info.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        info.flip_mode = SDL_FLIP_NONE;
        info.filter = SDL_GPU_FILTER_NEAREST;
        info.cycle = false;
        layoutLetterbox(static_cast<int>(sw), static_cast<int>(sh), &info.destination);
        SDL_BlitGPUTexture(cmd, &info);
        // ★ 调试叠层（`--overlay`）：letterbox blit **之后**再 blit 一次，落到**画面**矩形的
        //   左上角（= `info.destination.x/y`；⛔ 不是窗口左上角 —— 否则窗口 resize / 出现黑边时
        //   叠层会落在黑边里）。叠层保持 **1:1 窗口像素**（不随画面缩放）。
        //   `LOADOP_LOAD` 是承重墙（CLEAR 会把刚画上去的画面清掉）。
        //   ⛔ 叠层只画进 swapchain，⛔ 不碰光栅化 target（否则会经 rasterSyncToGuest 写回 guest）。
        if (overlay_visible_ && overlay_tex_ != nullptr) {
            overlay_dst_x_ = static_cast<int>(info.destination.x);
            overlay_dst_y_ = static_cast<int>(info.destination.y);
            overlay_dst_valid_ = true;
            blitOverlay(cmd, swap, overlay_dst_x_, overlay_dst_y_, static_cast<int>(sw), static_cast<int>(sh));
        } else {
            overlay_dst_valid_ = false;
        }
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            if (err) *err = std::string("SDL_SubmitGPUCommandBuffer 失败：") + SDL_GetError();
            return false;
        }
        ++g_stats.submits;
        ++g_stats.submits_present;
        ++presented_;
        return true;
    }

    bool directReadback(std::vector<std::uint8_t>* xrgb, int* width, int* height, std::string* err) override {
        // ★ 读 target 的屏障（截图走这里）：先 flush，否则读到的是"少几批 draw"的镜像。
        if (!rasterFlush(err)) return false;
        if (!directActive()) {
            if (err) *err = "GPU 直出未建立（窗口没有 claim）";
            return false;
        }
        if (target_ == nullptr || width_ <= 0 || height_ <= 0) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        int sw = 0;
        int sh = 0;
        SDL_GetWindowSizeInPixels(window_, &sw, &sh);
        if (sw <= 0 || sh <= 0) {
            if (err) *err = "窗口像素尺寸为 0（最小化？）";
            return false;
        }
        // ★ swapchain 纹理是 write-only（SDL 明确禁止读回），所以窗口像素来自**同一份 target**
        //   再做一次同样的 letterbox —— 呈现路径没画别的东西，这个等价关系是可核对的。
        std::vector<std::uint8_t> src;
        if (!downloadTarget(&src, err)) return false;
        xrgb->assign(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 4u, 0);
        SDL_GPUBlitRegion rect{};
        rect.w = static_cast<Uint32>(width_);
        rect.h = static_cast<Uint32>(height_);
        layoutLetterbox(sw, sh, &rect);
        for (int y = 0; y < sh; ++y) {
            std::uint8_t* dst = xrgb->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(sw) * 4u;
            const bool row_in = y >= static_cast<int>(rect.y) && y < static_cast<int>(rect.y + rect.h);
            for (int x = 0; x < sw; ++x) {
                std::uint8_t* d = dst + static_cast<std::size_t>(x) * 4u;
                if (!row_in || x < static_cast<int>(rect.x) || x >= static_cast<int>(rect.x + rect.w)) {
                    d[0] = 0;
                    d[1] = 0;
                    d[2] = 0;
                    d[3] = 0;  // letterbox 黑边（与 present 的 clear_color 同值）
                    continue;
                }
                // NEAREST：整像素映射回 target（1:1 时就是 x,y 本身）
                int sx = static_cast<int>((static_cast<long long>(x) - rect.x) * width_ / static_cast<int>(rect.w));
                int sy = static_cast<int>((static_cast<long long>(y) - rect.y) * height_ / static_cast<int>(rect.h));
                sx = std::clamp(sx, 0, width_ - 1);
                sy = std::clamp(sy, 0, height_ - 1);
                std::memcpy(d, src.data() + (static_cast<std::size_t>(sy) * width_ + sx) * 4u, 4u);
                d[3] = 0;  // XRGB 的 X 字节没有语义（guest X8 的归一化口径见 rasterSyncToGuest）
            }
        }
        // ★ 调试叠层：与 `directPresent` **同一块矩形**合成进回读像素 —— 原点是 letterbox 之后
        //   **画面**矩形的左上角（`rect.x/rect.y`），⛔ 不是窗口左上角（否则 resize 时叠层落进黑边）。
        //   swapchain 是 write-only ⇒ 窗口像素只能由 target + 同一份叠层重建；这里只改**回读缓冲**，
        //   ⛔ 不碰 target、⛔ 不碰 guest 像素。
        if (overlay_visible_ && overlay_w_ > 0 && overlay_h_ > 0 &&
            overlay_rgba_.size() >= static_cast<std::size_t>(overlay_w_) * static_cast<std::size_t>(overlay_h_) * 4u) {
            const int ox = static_cast<int>(rect.x);
            const int oy = static_cast<int>(rect.y);
            int ow = overlay_w_;
            int oh = overlay_h_;
            if (ox + ow > sw) ow = sw - ox;
            if (oy + oh > sh) oh = sh - oy;
            if (ow > 0 && oh > 0) {
                for (int y = 0; y < oh; ++y) {
                    std::uint8_t* drow = xrgb->data() + static_cast<std::size_t>(oy + y) *
                                                          static_cast<std::size_t>(sw) * 4u;
                    const std::uint8_t* srow =
                        overlay_rgba_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(overlay_w_) * 4u;
                    for (int x = 0; x < ow; ++x) {
                        const std::uint8_t* s = srow + static_cast<std::size_t>(x) * 4u;
                        std::uint8_t* d = drow + static_cast<std::size_t>(ox + x) * 4u;
                        d[0] = s[2];  // RGBA8 → XRGB8888 的字节序（B,G,R,X）
                        d[1] = s[1];
                        d[2] = s[0];
                        d[3] = 0;
                    }
                }
            }
        }
        if (width != nullptr) *width = sw;
        if (height != nullptr) *height = sh;
        return true;
    }

    // -----------------------------------------------------------------------------------
    // 调试叠层（`--overlay`）：RGBA8 → **同一 device** 上的一张 swapchain 格式纹理。
    // ★ 真正的"画到窗口"发生在 `directPresent` 的 letterbox blit 之后（`LOADOP_LOAD`）；
    //   这里只负责建纹理 + 上传。文本不变时调用方不会重复调用（省掉每帧一次上传）。
    // ★ 格式取 `SDL_GetGPUSwapchainTextureFormat` 的**实际值**（`swapchain_format_`）：
    //   blit 源/目的的兼容性由后端决定，用实际 swapchain 格式是唯一能保证的组合；
    //   不支持 SAMPLER 用法就如实失败，⛔ 不硬上。
    // -----------------------------------------------------------------------------------
    bool directOverlay(const std::uint8_t* rgba, int w, int h, std::string* err) override {
        if (rgba == nullptr || w <= 0 || h <= 0) {
            // ★ 关掉叠层 = **释放** GPU 纹理（⛔ 不许只把 visible 置 false 把纹理留在 device 上：
            //   那是一次真实的泄漏，而且下次开叠层会再多一张）。
            releaseOverlayTexture();
            return true;
        }
        if (!directActive()) {
            if (err) *err = "GPU 直出未建立（窗口没有 claim）";
            return false;
        }
        if (!SDL_GPUTextureSupportsFormat(device_, swapchain_format_, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            if (err) *err = "swapchain 格式不支持 SAMPLER 用法（叠层不能作为 blit 源）";
            return false;
        }
        if (overlay_tex_ == nullptr || overlay_w_ != w || overlay_h_ != h) {
            releaseOverlayTexture();
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D;
            ci.format = swapchain_format_;
            ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ci.width = static_cast<Uint32>(w);
            ci.height = static_cast<Uint32>(h);
            ci.layer_count_or_depth = 1;
            ci.num_levels = 1;
            ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
            overlay_tex_ = SDL_CreateGPUTexture(device_, &ci);
            if (overlay_tex_ == nullptr) {
                if (err) *err = std::string("SDL_CreateGPUTexture(overlay) 失败：") + SDL_GetError();
                return false;
            }
            overlay_w_ = w;
            overlay_h_ = h;
        }
        // RGBA8 → swapchain 的字节序（B8G8R8A8 = 交换 R/B；R8G8B8A8 = verbatim）。
        const bool bgra = swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        const std::size_t bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        std::vector<std::uint8_t> upload(bytes);
        for (std::size_t i = 0; i + 3 < bytes; i += 4) {
            upload[i + 0] = bgra ? rgba[i + 2] : rgba[i + 0];
            upload[i + 1] = rgba[i + 1];
            upload[i + 2] = bgra ? rgba[i + 0] : rgba[i + 2];
            upload[i + 3] = rgba[i + 3];
        }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = static_cast<Uint32>(bytes);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUTransferBuffer(overlay) 失败：") + SDL_GetError();
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(overlay) 失败";
            return false;
        }
        std::memcpy(mapped, upload.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = std::string("SDL_AcquireGPUCommandBuffer(overlay) 失败：") + SDL_GetError();
            return false;
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb;
        src.pixels_per_row = static_cast<Uint32>(w);
        src.rows_per_layer = static_cast<Uint32>(h);
        SDL_GPUTextureRegion dst{};
        dst.texture = overlay_tex_;
        dst.w = static_cast<Uint32>(w);
        dst.h = static_cast<Uint32>(h);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        const bool ok = SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        if (!ok) {
            if (err) *err = std::string("SDL_SubmitGPUCommandBuffer(overlay) 失败：") + SDL_GetError();
            return false;
        }
        ++g_stats.submits;
        ++g_stats.submits_upload;
        // CPU 侧副本（`directReadback` 合成同一块叠层用；⛔ 不进 guest、⛔ 不进 target）。
        overlay_rgba_.assign(rgba, rgba + bytes);
        overlay_visible_ = true;
        return true;
    }

    // 诊断 / 夹具：上一次直出帧里叠层 blit 的 dst 原点（窗口像素；= letterbox 之后画面矩形原点）。
    // ★ 这是 B1 的回归守卫取数口：测试断言它与"画面 rect 的原点"相等（⛔ 不是恒 0）。
    bool directLastOverlayOrigin(int* x, int* y) const override {
        if (!overlay_dst_valid_) return false;
        if (x != nullptr) *x = overlay_dst_x_;
        if (y != nullptr) *y = overlay_dst_y_;
        return true;
    }

private:
    // 整张 swapchain 里"目标矩形"的尺寸/居中偏移（letterbox）。present 与 readback 共用同一份算术。
    void layoutLetterbox(int sw, int sh, SDL_GPUBlitRegion* out) const {
        const double sx = static_cast<double>(sw) / static_cast<double>(width_);
        const double sy = static_cast<double>(sh) / static_cast<double>(height_);
        // ⛔ 不要写 `std::min`：<windows.h> 的 min 宏会把它撑坏（本文件经 SDL.h 间接包含）
        const double scale = (sx < sy) ? sx : sy;
        int dw = static_cast<int>(std::lround(static_cast<double>(width_) * scale));
        int dh = static_cast<int>(std::lround(static_cast<double>(height_) * scale));
        dw = std::clamp(dw, 1, sw);
        dh = std::clamp(dh, 1, sh);
        out->x = static_cast<Uint32>((sw - dw) / 2);
        out->y = static_cast<Uint32>((sh - dh) / 2);
        out->w = static_cast<Uint32>(dw);
        out->h = static_cast<Uint32>(dh);
    }

    // 叠层纹理 → **画面**矩形左上角（窗口**像素**空间；超出 swapchain 就裁掉，⛔ 不缩放）。
    // ★ 必须在 letterbox blit 之后调用同一个 command buffer：`LOADOP_LOAD` 保留画面；
    //   原点由 `directPresent` 从 `layoutLetterbox()` 的 `info.destination` 传入。
    void blitOverlay(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swap, int dst_x, int dst_y, int sw, int sh) {
        int dw = overlay_w_;
        int dh = overlay_h_;
        if (dst_x + dw > sw) dw = sw - dst_x;
        if (dst_y + dh > sh) dh = sh - dst_y;
        if (dw <= 0 || dh <= 0) return;
        SDL_GPUBlitInfo oi{};
        oi.source.texture = overlay_tex_;
        oi.source.w = static_cast<Uint32>(dw);  // 源也取同一块：1:1 裁剪，⛔ 不是缩放
        oi.source.h = static_cast<Uint32>(dh);
        oi.destination.texture = swap;
        oi.destination.x = static_cast<Uint32>(dst_x);
        oi.destination.y = static_cast<Uint32>(dst_y);
        oi.destination.w = static_cast<Uint32>(dw);
        oi.destination.h = static_cast<Uint32>(dh);
        oi.load_op = SDL_GPU_LOADOP_LOAD;
        oi.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        oi.flip_mode = SDL_FLIP_NONE;
        oi.filter = SDL_GPU_FILTER_NEAREST;
        oi.cycle = false;
        SDL_BlitGPUTexture(cmd, &oi);
    }

    // 释放叠层纹理 + CPU 侧副本（device 销毁 / 解 claim / 关掉叠层之前必须调用）。
    void releaseOverlayTexture() {
        if (overlay_tex_ != nullptr && device_ != nullptr) SDL_ReleaseGPUTexture(device_, overlay_tex_);
        overlay_tex_ = nullptr;
        overlay_w_ = 0;
        overlay_h_ = 0;
        overlay_visible_ = false;
        overlay_dst_valid_ = false;
        overlay_rgba_.clear();
    }

    // guest 后缓冲 → target（逐行 verbatim；X8 的 X 字节按 guest 原样，与 rasterSyncToGuest 的归一化对称）
    bool uploadGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height, std::string* err) {
        if (target_ == nullptr || width != width_ || height != height_) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        if (pixels == nullptr) {
            if (err) *err = "guest 后缓冲指针为空";
            return false;
        }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = static_cast<Uint32>(static_cast<std::size_t>(width) * 4u * height);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUTransferBuffer(upload) 失败：") + SDL_GetError();
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(upload) 失败";
            return false;
        }
        for (int y = 0; y < height; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * width * 4,
                        pixels + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(width) * 4);
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb;
        src.pixels_per_row = static_cast<Uint32>(width);
        src.rows_per_layer = static_cast<Uint32>(height);
        SDL_GPUTextureRegion dst{};
        dst.texture = target_;
        dst.w = static_cast<Uint32>(width);
        dst.h = static_cast<Uint32>(height);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        if (SDL_SubmitGPUCommandBuffer(cmd)) {
            ++g_stats.submits;
            ++g_stats.submits_upload;
        }
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return true;
    }

    // target → CPU（BGRA8；A 字节按 guest X8 语义归一化）。给 `--present-screenshot` 用。
    bool downloadTarget(std::vector<std::uint8_t>* out, std::string* err) {
        const int width = width_;
        const int height = height_;
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = static_cast<Uint32>(static_cast<std::size_t>(width) * 4u * height);
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUTransferBuffer(download) 失败：") + SDL_GetError();
            return false;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = target_;
        src.w = static_cast<Uint32>(width);
        src.h = static_cast<Uint32>(height);
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        dst.pixels_per_row = static_cast<Uint32>(width);
        dst.rows_per_layer = static_cast<Uint32>(height);
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence != nullptr) {
            ++g_stats.submits;
            ++g_stats.submits_readback;
        }
        if (fence == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_SubmitGPUCommandBufferAndAcquireFence 失败";
            return false;
        }
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        const void* mapped = SDL_MapGPUTransferBuffer(device_, tb, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(download) 失败";
            return false;
        }
        out->resize(static_cast<std::size_t>(width) * 4u * height);
        for (int y = 0; y < height; ++y) {
            std::uint8_t* row = out->data() + static_cast<std::size_t>(y) * width * 4u;
            std::memcpy(row, static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * width * 4u,
                        static_cast<std::size_t>(width) * 4u);
            if (!alpha_) {
                for (int x = 0; x < width; ++x) row[x * 4 + 3] = 0;
            }
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return true;
    }

    struct Vertex {
        float x, y, z;
        float r, g, b, a;
        float u, v;
    };
    static_assert(sizeof(Vertex) == 36, "vertex stride");

    struct TextureEntry {
        SDL_GPUTexture* texture = nullptr;
        // 整幅内容哈希（0 = 不可用）。★ 只在"上次上传的是整幅"或"算过整幅哈希"时有效：
        // 一次脏矩形上传之后 GPU 内容与 guest 一致但没重算哈希 ⇒ 置 0（下次整幅一律重传）。
        std::uint64_t hash = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // ★ 已**录进命令缓冲**的内容版本（= 这份内容执行完以后的 guest 版本）。0 = 未知。
        std::uint32_t recorded_generation = 0;
        std::uint64_t use = 0;
        // 有已录但未提交的上传（LRU 回收保护；flush 完清掉）。
        bool pending = false;
    };

    // 一段"内容变更边界"的上传：目标纹理的一块 **texel 子矩形** + 紧排的 BGRA 字节。
    // ★ 入队时就从 guest 像素拷走（⛔ 不保存 guest 指针）；flush 时按各自 **独立的 transfer
    //   buffer offset** 写入 ⇒ 同一 command buffer 内不会覆盖仍在途的数据。
    // ★ 字节放在**帧内 staging arena**（`staging_`）里、只记 (offset, size)：arena 的容量只增，
    //   扩容时零初始化一次就够（⛔ 不要每块 `std::vector::resize` —— 那会先 memset 再 memcpy，
    //   一帧十几 MB 的整幅上传会白白多付一倍带宽）。
    struct PendingUpload {
        SDL_GPUTexture* texture = nullptr;
        GpuTexelRect rect;            // 目标子矩形（texel；右/下开）
        // ★ 目标 mip 级（`REQ-01M73G2724479XAVS176WBZ65Q`）：0 = level 0（脏区上传），>=1 = 派生级
        //   （整级上传；源是 box filter 从**完整** level 0 逐级算出来的）。
        std::uint32_t mip_level = 0;
        std::size_t staging_offset = 0;  // `staging_` 内偏移
        std::size_t size = 0;            // 字节数 = row_pitch * rect.h
        // staging 里每行的字节数：按 **256 对齐**（D3D12 的 texture row pitch 要求 256 字节对齐；
        // 不对齐时 SDL 会替我们做一次临时拷贝 —— 那正是"只传脏区"想省的带宽）。
        std::uint32_t row_pitch = 0;
        // flush 时算出来的 transfer buffer 内偏移（512 对齐；每次 flush 重算）。
        std::uint32_t transfer_offset = 0;
        // 这块上传是否覆盖**整幅**纹理。★ 只有整幅才能给 `SDL_UploadToGPUTexture` 传 cycle=true
        //   （cycle = 丢弃该纹理原有内容）；子矩形上传传 true 会把矩形外的内容**清成未定义**
        //   —— 实测：整幅蓝 + 子矩形红 ⇒ 矩形外全变黑（真后端像素用例当场红）。
        bool whole = false;
        // ★ 真正传给 SDL 的 `cycle`：**有 mip 链的纹理一律 false** —— D3D12 的 cycle=true 会换掉
        //   整张物理纹理（`SDL_gpu_d3d12.c` 的 CycleActiveTexture），多级上传没法"cycle 一半"。
        bool cycle = false;
    };

    // 方案 C 的"段"：`uploads` 在本段第一个 draw 之前用 copy pass 上传，然后 render pass 画
    // `[first_draw, 下一段的 first_draw)`。切段只在内容变更边界发生，且**不提交**。
    // `segments_` 恒非空（`segments_[0].first_draw == 0`）；可能出现空段（只有 copy 没有 draw）。
    struct Segment {
        std::size_t first_draw = 0;
        std::vector<PendingUpload> uploads;
    };

    // 一条已入队的 draw：状态/句柄逐条保留（⛔ 不合并两条状态不同的 draw）。
    struct QueuedDraw {
        std::vector<GpuRasterVertex> vertices;  // 屏幕空间（flush 时按当时的 target 尺寸转 NDC）
        GpuRasterState state;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUSampler* sampler = nullptr;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        bool textured = false;
        // 这条 draw 采样到的 texel 区域（`gpuDrawSampleRect`；textured 且非空）——切段判据用它。
        GpuTexelRect sample_rect;
        std::uint32_t vertex_offset = 0;  // 在 flush 的顶点缓冲里（单位 = Vertex）
        // ★ 覆盖账本的批号（入队那一刻的 `gpuRasterBatchTag()`；0 = 生产者没标）。
        //   flush **提交成功之后**用它回报"这条 draw 真的进了 render pass"。
        std::uint64_t batch_tag = 0;
        // ---------------------------------------------------------------------------
        // ★ 诊断探针（`LIFT_GPU_FF_TEX=1`；`REQ-01M73P3GXW22Y467YGYPQSM571`）：入队时按 v0 的
        //   uv 从 **guest 像素**取同一 texel 的 4 字节（⛔ 不保存 guest 指针 —— 契约只允许在
        //   入队那一刻读），flush 提交成功之后再下载 GPU level 0 的同一 texel 对打。
        //   `armed == false` 时下面所有分支都不进（默认零成本）。
        struct FfProbe {
            bool armed = false;
            int x = 0;
            int y = 0;
            std::uint8_t guest[4] = {0, 0, 0, 0};
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t generation = 0;        // 入队时的 guest 内容版本（0 = 未知）
            std::uint32_t recorded = 0;          // 入队后后端记下的"已录版本"
            std::uint32_t upload_blocks = 0;     // 本次 ensureTexture 入队的上传块数（0 = 走了版本快路径）
            std::uint64_t guest_hash = 0;         // 入队那一刻 guest 整幅 level 0 的内容哈希
            std::size_t round = 0;                // 第几轮探针（1 = 首次上传那一轮；2 = 没重传那一轮）
            bool has_alpha = false;
            std::uint32_t serial = 0;
        };
        FfProbe probe;
    };

    // 帧内 staging arena：容量只增、**只在扩容时**付一次零初始化（摊薄）。
    // ★ 返回的是"本次写入起点"（调用方自己 memcpy 进去）；`staging_used_` 同时前移。
    // ★ `staging_.data()` 可能被下一次扩容搬家 ⇒ 只记 offset、⛔ 不存指针。
    bool reserveStaging(std::size_t bytes, std::uint8_t** out, std::string* err) {
        if (bytes == 0) {
            *out = staging_.data();
            return true;
        }
        if (bytes > 0xFFFFFFFFull || staging_used_ + bytes > 0xFFFFFFFFull) {
            if (err) *err = "SDL_GPU：一帧的 staging 超过 4 GiB";
            return false;
        }
        if (staging_used_ + bytes > staging_.size()) {
            std::size_t want = staging_.size() != 0 ? staging_.size() : (1u << 20);
            while (want < staging_used_ + bytes) want *= 2u;
            staging_.resize(want);
        }
        *out = staging_.data() + staging_used_;
        staging_used_ += bytes;
        return true;
    }

    // GPU 顶点缓冲 grow-only：入队时就长够，flush 里不再创建（把可失败点前移）。
    bool ensureVertexBuffer(Uint32 bytes, std::string* err) {
        if (bytes <= vertex_capacity_) return true;
        Uint32 want = vertex_capacity_ != 0 ? vertex_capacity_ : 4096u;
        while (want < bytes) {
            if (want > 0x80000000u) {
                want = bytes;
                break;
            }
            want *= 2u;
        }
        if (vertex_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = want;
        vertex_buffer_ = SDL_CreateGPUBuffer(device_, &bi);
        vertex_capacity_ = want;
        if (vertex_buffer_ == nullptr) {
            vertex_capacity_ = 0;
            if (err) *err = "SDL_CreateGPUBuffer(vertex) 失败";
            return false;
        }
        return true;
    }

    // 纹理缓存：key = 纹理身份（serial / 指针）+ 尺寸 + alpha 语义；内容版本/哈希不变 ⇒ 不重传。
    // ★ 入队时把要上传的像素拷成 staging（⛔ 不保存 guest 指针），并**就地判**要不要切段：
    //   · 版本没变 ⇒ 不哈希、不重传（TITLE 的历史快路径，⛔ 不许砍）；
    //   · 版本变了 + 写区/版本链对得上 ⇒ 只传脏矩形（方案 C，字节数从"整幅"降到"这块"）；
    //   · 写区未知 ⇒ 整幅，并按整幅内容哈希比较（"版本变了但像素没变" ⇒ 不重传）；
    //   · 脏区与本段已入队、采样**同一纹理**的 draw 的采样矩形相交 ⇒ 在本边界**切段**（不提交）。
    bool ensureTexture(const GpuRasterTexture& t, SDL_GPUTexture** out, std::string* err) {
        if (t.format.bpp != 32) {
            if (err) *err = "SDL_GPU 纹理只支持 32bpp";
            return false;
        }
        if (!bgraLayout(t.format)) {
            if (err) *err = "SDL_GPU 纹理只支持 BGRA 内存布局（mask 不是 X8R8G8B8/A8R8G8B8）";
            return false;
        }
        static const std::uint8_t kWhite[4] = {255, 255, 255, 255};
        const std::uint8_t* pixels = t.pixels != nullptr ? t.pixels : kWhite;
        const std::uint32_t pitch = t.pixels != nullptr ? t.pitch : 4u;
        int width = t.width;
        int height = t.height;
        if (t.pixels == nullptr) {
            width = 1;
            height = 1;
        }
        if (width <= 0 || height <= 0 || pitch < static_cast<std::uint32_t>(width) * 4u) {
            if (err) *err = "SDL_GPU 纹理尺寸/步长非法";
            return false;
        }
        const bool has_alpha = t.format.amask != 0;
        // ★ 派生 mip 链（`REQ-01M73G2724479XAVS176WBZ65Q`）：SDL_GPU **不**替我们生成 mip
        //   （没有 glGenerateMipmap 对应物）⇒ 我们自己按 box filter 逐级算好、**逐级整级上传**；
        //   `num_levels` 必须 = 链长，sampler 的 `max_lod` 才取得到级（见 `samplerFor`）。
        //   ★ 策略与 CPU 一致：凡 32bpp 纹理都建整链（不按"这一批缩没缩小"逐批摇摆），
        //     否则同一张纹理会出现"CPU 有链、GPU 没链"的静默分叉。
        const std::uint32_t mip_levels = d3d9::mipLevelCount(static_cast<std::uint32_t>(width),
                                                             static_cast<std::uint32_t>(height));
        // ★ 身份键：serial != 0 时用进程级纹理 serial（⛔ 不用 arena 指针 / 状态块 EA —— 两者都会复用）；
        //   serial == 0（夹具的 1×1 白纹理）才退回 guest 指针键。
        const std::uint64_t key = gpuTextureCacheKey(t, width, height);
        auto it = textures_.find(key);
        const bool same_size = it != textures_.end() && it->second.texture != nullptr &&
                               it->second.width == static_cast<std::uint32_t>(width) &&
                               it->second.height == static_cast<std::uint32_t>(height);
        const std::uint32_t recorded = same_size ? it->second.recorded_generation : 0u;
        // ① 版本快路径：已**录进命令缓冲**的内容就是这个版本 ⇒ 不哈希、不重传。
        //   ★ 这里**不**看"还有没有未提交的上传"：本段里已入队 draw 看到的就是 staging 里的
        //     那份内容（录进命令缓冲后按提交序执行）⇒ 版本相同就意味着采样结果相同。
        if (same_size && t.generation != 0 && recorded == t.generation) {
            it->second.use = ++texture_use_;
            *out = it->second.texture;
            return true;
        }
        // ② 脏区判定（真后端与 Mock 共用 `gpuContentDirtyRegion`，⛔ 不要各写一份）。
        const GpuDirtyRegion dirty = gpuContentDirtyRegion(t, recorded, width, height);
        // ③ 整幅（或"够大"）时先做整幅内容哈希比较：版本变了但像素没变 ⇒ 不重传。
        //   ★ 小脏区故意**不**哈希（一整幅哈希正是历史 0.64 ms/条里的固定成本之一）；
        //   ★ `!same_size`（新纹理）也**不**哈希：没有可比对的旧哈希，算了也只能丢掉。
        std::uint64_t hash = 0;
        bool hash_valid = false;
        const std::size_t texel_area = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t dirty_area =
            static_cast<std::size_t>(dirty.rect.w) * static_cast<std::size_t>(dirty.rect.h);
        if (same_size && (dirty.whole || (texel_area != 0 && dirty_area * 2u >= texel_area))) {
            hash = hashPixels(pixels, pitch, width, height);
            hash_valid = true;
            if (it->second.hash != 0 && it->second.hash == hash) {
                // 版本变了但像素内容没变（例如 Unlock 后没改）⇒ 只更新版本，不重传。
                it->second.recorded_generation = t.generation;
                it->second.use = ++texture_use_;
                *out = it->second.texture;
                return true;
            }
        }
        // ④ 取/建 GPU 纹理
        SDL_GPUTexture* tex = it != textures_.end() ? it->second.texture : nullptr;
        if (tex == nullptr) {
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D;
            ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ci.width = static_cast<Uint32>(width);
            ci.height = static_cast<Uint32>(height);
            ci.layer_count_or_depth = 1;
            ci.num_levels = mip_levels;
            ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
            tex = SDL_CreateGPUTexture(device_, &ci);
            if (tex == nullptr) {
                if (err) *err = std::string("SDL_CreateGPUTexture(texture) 失败：") + SDL_GetError();
                return false;
            }
            // ★ 立刻把新纹理挂回缓存：后面任何一步失败都不会留下悬垂指针（hash=0 ⇒ 下次重传）。
            TextureEntry& created = textures_[key];
            created.texture = tex;
            created.hash = 0;
            created.recorded_generation = 0;
            created.width = static_cast<std::uint32_t>(width);
            created.height = static_cast<std::uint32_t>(height);
        }
        // ⑤ 切段判定：只比**本段**已入队、且采样同一 GPU 纹理的 draw（纯函数判交）。
        //   ★ 新纹理不可能被已入队 draw 引用 ⇒ 首次上传天然不切。
        bool need_cut = false;
        {
            const Segment& seg = segments_.back();
            for (std::size_t i = seg.first_draw; i < queue_.size(); ++i) {
                const QueuedDraw& d = queue_[i];
                if (!d.textured || d.texture != tex) continue;
                // ★ 有 mip 链时内容一变就等于**整张**变（派生级要整级重传）⇒ 一律切段；
                //   ⛔ 不能只按 dirty.rect 判交（那会让更早的 draw 采到旧的上层 mip）。
                if (mip_levels > 1u || gpuTexelRectsIntersect(dirty.rect, d.sample_rect)) {
                    need_cut = true;
                    break;
                }
            }
        }
        if (need_cut) {
            // ★ 切段 ≠ 提交：只是把命令缓冲在后面切开一个 copy 边界（新段从下一条 draw 起）。
            segments_.push_back(Segment{queue_.size(), {}});
            ++g_stats.cuts;
        }
        // ⑥ staging：**只拷脏区**，行**紧密排列**（pitch = w*4）。
        //   ⛔ 不要自己按 256 对齐：SDL 的 **Metal** 后端在 `METAL_UploadToTexture` 里**根本不读**
        //   `source->pixels_per_row` —— 它用 `BytesPerRow(destination->w, …)`（目标区域宽度）当行距
        //   （`src/gpu/metal/SDL_gpu_metal.m` @ release-3.4.18；同一个文件的 Download 路径**是**读的）。
        //   ⇒ staging 行距一旦比 w*4 宽，每行就错开 (行距 − w*4) 字节，整张纹理被削成横条
        //   （1280/1664/2048 宽的纹理 w*4 恰好 256 对齐 ⇒ 看不出来；1120x672 就必现）。
        //   ✅ **D3D12 反过来是读的**（`D3D12_UploadToTexture` 用 `pixels_per_row` 算 rowPitch，
        //   不对齐时它自己临时拷贝一份 256 对齐的）⇒ 这个坑只在 Metal 上炸。
        //   ⇒ 统一发**紧密**布局：Metal 正确、D3D12 走它自己的 temp-copy 路径（正确，代价是那次拷贝）。
        //   翻回去的判据：SDL 修好 `METAL_UploadToTexture` 之后才允许再依赖"有富余的行距"。
        const std::uint32_t row_pitch = static_cast<std::uint32_t>(dirty.rect.w) * 4u;
        const std::size_t stage_size = static_cast<std::size_t>(row_pitch) * static_cast<std::size_t>(dirty.rect.h);
        std::uint8_t* stage = nullptr;
        if (!reserveStaging(stage_size, &stage, err)) return false;
        PendingUpload up;
        up.texture = tex;
        up.rect = dirty.rect;
        up.staging_offset = staging_used_ - stage_size;
        up.size = stage_size;
        up.row_pitch = row_pitch;
        up.whole = dirty.whole;
        // ★ 有 mip 链的纹理一律不 cycle（D3D12 的 cycle 会换掉整张物理纹理，多级上传没法"cycle 一半"）。
        up.cycle = dirty.whole && mip_levels == 1u;
        for (int y = 0; y < dirty.rect.h; ++y) {
            const std::uint8_t* src_row = pixels +
                                          static_cast<std::size_t>(dirty.rect.y + y) * pitch +
                                          static_cast<std::size_t>(dirty.rect.x) * 4u;
            std::uint8_t* dst_row = stage + static_cast<std::size_t>(y) * static_cast<std::size_t>(row_pitch);
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(dirty.rect.w) * 4u);
            if (!has_alpha) {
                // guest X8R8G8B8：X 字节不是 alpha。CPU 侧按 amask==0 ⇒ alpha=255。
                for (int x = 0; x < dirty.rect.w; ++x) dst_row[x * 4 + 3] = 255;
            }
        }
        segments_.back().uploads.push_back(std::move(up));
        ++g_stats.uploads;
        if (dirty.whole) ++g_stats.full_uploads;
        // ⑥b 派生 mip：**从完整 level 0**（`pixels`，不是 dirty 区）box filter 逐级算，逐级整级上传。
        //    ★ 顺序：level 0 的脏区上传已经入队在前 ⇒ 链随后覆盖。
        if (mip_levels > 1u) {
            const std::uint32_t w0 = static_cast<std::uint32_t>(width);
            const std::uint32_t h0 = static_cast<std::uint32_t>(height);
            const std::uint64_t chain_bytes = d3d9::mipChainBytes(w0, h0, 32u);
            if (chain_bytes == 0u || chain_bytes > 0x40000000ull) {
                if (err) *err = "SDL_GPU：派生 mip 链字节数非法";
                return false;
            }
            mip_scratch_.assign(static_cast<std::size_t>(chain_bytes), 0u);
            for (std::uint32_t lvl = 1u; lvl < mip_levels; ++lvl) {
                const std::uint32_t sw = d3d9::mipLevelWidth(w0, lvl - 1u);
                const std::uint32_t sh = d3d9::mipLevelHeight(h0, lvl - 1u);
                const std::uint32_t spitch = (lvl == 1u) ? pitch : d3d9::mipLevelPitch(w0, 32u, lvl - 1u);
                const std::uint8_t* s = (lvl == 1u) ? pixels
                                                    : mip_scratch_.data() +
                                                          d3d9::mipLevelOffset(w0, h0, 32u, lvl - 1u);
                const std::uint32_t dw = d3d9::mipLevelWidth(w0, lvl);
                const std::uint32_t dh = d3d9::mipLevelHeight(h0, lvl);
                const std::uint32_t dpitch = d3d9::mipLevelPitch(w0, 32u, lvl);
                d3d9::mipDownsampleBox(s, sw, sh, spitch, mip_scratch_.data() + d3d9::mipLevelOffset(w0, h0, 32u, lvl),
                                       dw, dh, dpitch, 4u);
            }
            for (std::uint32_t lvl = 1u; lvl < mip_levels; ++lvl) {
                const std::uint32_t dw = d3d9::mipLevelWidth(w0, lvl);
                const std::uint32_t dh = d3d9::mipLevelHeight(h0, lvl);
                const std::uint32_t dpitch = d3d9::mipLevelPitch(w0, 32u, lvl);
                const std::size_t lvl_bytes = static_cast<std::size_t>(dpitch) * dh;
                std::uint8_t* stage2 = nullptr;
                if (!reserveStaging(lvl_bytes, &stage2, err)) return false;
                const std::uint8_t* src_lvl =
                    mip_scratch_.data() + d3d9::mipLevelOffset(w0, h0, 32u, lvl);
                std::memcpy(stage2, src_lvl, lvl_bytes);
                if (!has_alpha) {
                    // 与 level 0 同口径：guest X8R8G8B8 的 X 字节不是 alpha ⇒ 派生级也按 255。
                    for (std::uint32_t y = 0; y < dh; ++y) {
                        std::uint8_t* row = stage2 + static_cast<std::size_t>(y) * dpitch;
                        for (std::uint32_t x = 0; x < dw; ++x) row[x * 4u + 3u] = 255;
                    }
                }
                PendingUpload mup;
                mup.texture = tex;
                mup.mip_level = lvl;
                mup.rect = GpuTexelRect{0, 0, static_cast<int>(dw), static_cast<int>(dh)};
                mup.staging_offset = staging_used_ - lvl_bytes;
                mup.size = lvl_bytes;
                mup.row_pitch = dpitch;
                mup.whole = false;
                mup.cycle = false;
                segments_.back().uploads.push_back(std::move(mup));
                ++g_stats.uploads;
            }
        }
        // ⑦ 记账：这一版的内容**即将**进 GPU（录进命令缓冲），所以 recorded = 这个版本。
        TextureEntry& e = textures_[key];
        e.texture = tex;
        e.width = static_cast<std::uint32_t>(width);
        e.height = static_cast<std::uint32_t>(height);
        e.recorded_generation = t.generation;
        // ★ 脏矩形上传后 GPU 内容 == guest 内容，但没重算整幅哈希 ⇒ 0（下次整幅一律重传）。
        e.hash = hash_valid ? hash : 0;
        e.pending = true;
        e.use = ++texture_use_;
        *out = tex;
        return true;
    }

    void evictTextures() {
        // 只防失控增长：超过 128 个纹理就回收最久未用的。
        // ★ 待上传 / 被本帧队列引用的纹理不回收（否则 flush 会写到已释放的句柄）。
        constexpr std::size_t kMaxTextures = 128;
        while (textures_.size() > kMaxTextures) {
            auto victim = textures_.end();
            for (auto it = textures_.begin(); it != textures_.end(); ++it) {
                if (it->second.pending) continue;
                if (victim == textures_.end() || it->second.use < victim->second.use) victim = it;
            }
            if (victim == textures_.end()) return;  // 全都在用（罕见）：宁可不回收
            if (victim->second.texture != nullptr) SDL_ReleaseGPUTexture(device_, victim->second.texture);
            textures_.erase(victim);
        }
    }

    SDL_GPUSampler* samplerFor(const GpuSampler& s, std::string* err) {
        const std::uint32_t key = static_cast<std::uint32_t>(s.address_u) |
                                  (static_cast<std::uint32_t>(s.address_v) << 3) |
                                  (static_cast<std::uint32_t>(s.mag_filter) << 6) |
                                  (static_cast<std::uint32_t>(s.min_filter) << 7);
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return it->second;
        auto addr = [](GpuAddress a) {
            switch (a) {
                case GpuAddress::Wrap: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
                case GpuAddress::Mirror: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
                case GpuAddress::Clamp: break;
            }
            return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        };
        auto filt = [](GpuFilter f) {
            return f == GpuFilter::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        };
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = filt(s.min_filter);
        si.mag_filter = filt(s.mag_filter);
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        // ★ `max_lod` 的缺省值在 SDL_GPU 里是 **0**（= LOD 被钳到 0 ⇒ 只采 level 0）。
        //   不显式放开，就是"CPU 修好、GPU 静默走样"（`REQ-01M73G2724479XAVS176WBZ65Q`）。
        //   采样器缓存键不用带它：这里是个常量（1 级纹理由 API 自己钳住）。
        si.min_lod = 0.0f;
        si.max_lod = 1000.0f;
        si.address_mode_u = addr(s.address_u);
        si.address_mode_v = addr(s.address_v);
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        SDL_GPUSampler* samp = SDL_CreateGPUSampler(device_, &si);
        if (samp == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUSampler 失败：") + SDL_GetError();
            return nullptr;
        }
        samplers_[key] = samp;
        return samp;
    }

    void destroyTargets() {
        if (target_ != nullptr) SDL_ReleaseGPUTexture(device_, target_);
        if (depth_ != nullptr) SDL_ReleaseGPUTexture(device_, depth_);
        target_ = nullptr;
        depth_ = nullptr;
        for (auto& [k, p] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
        pipelines_.clear();
    }

    static std::uint64_t pipelineKey(const GpuRasterState& s, bool textured) {
        std::uint64_t k = 0;
        int shift = 0;
        auto put = [&](std::uint64_t v, int bits) {
            k |= (v & ((1ull << bits) - 1ull)) << shift;
            shift += bits;
        };
        put(textured ? 1u : 0u, 1);
        put(static_cast<std::uint64_t>(s.tex_mode), 3);
        put(s.blend ? 1u : 0u, 1);
        put(static_cast<std::uint64_t>(s.src_blend), 4);
        put(static_cast<std::uint64_t>(s.dst_blend), 4);
        put(static_cast<std::uint64_t>(s.blend_op), 3);
        put(s.separate_alpha_blend ? 1u : 0u, 1);
        put(static_cast<std::uint64_t>(s.src_blend_alpha), 4);
        put(static_cast<std::uint64_t>(s.dst_blend_alpha), 4);
        put(static_cast<std::uint64_t>(s.blend_op_alpha), 3);
        put(s.color_write, 4);
        put(s.depth_test ? 1u : 0u, 1);
        put(s.depth_write ? 1u : 0u, 1);
        put(s.depth_func, 4);
        return k;
    }

    SDL_GPUGraphicsPipeline* pipelineFor(const GpuRasterState& state, bool textured, std::string* err) {
        const std::uint64_t key = pipelineKey(state, textured);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        if (state.blend) {
            SDL_GPUBlendFactor sc = SDL_GPU_BLENDFACTOR_ONE;
            SDL_GPUBlendFactor dc = SDL_GPU_BLENDFACTOR_ZERO;
            SDL_GPUBlendFactor sa = SDL_GPU_BLENDFACTOR_ONE;
            SDL_GPUBlendFactor da = SDL_GPU_BLENDFACTOR_ZERO;
            SDL_GPUBlendOp cop = SDL_GPU_BLENDOP_ADD;
            SDL_GPUBlendOp aop = SDL_GPU_BLENDOP_ADD;
            if (!sdlBlendFactor(state.src_blend, &sc) || !sdlBlendFactor(state.dst_blend, &dc) ||
                !sdlBlendFactor(state.src_blend_alpha, &sa) || !sdlBlendFactor(state.dst_blend_alpha, &da) ||
                !sdlBlendOp(state.blend_op, &cop) || !sdlBlendOp(state.blend_op_alpha, &aop)) {
                if (err) *err = "SDL_GPU 不支持该 blend factor/op 组合";
                return nullptr;
            }
            color.blend_state.enable_blend = true;
            color.blend_state.src_color_blendfactor = sc;
            color.blend_state.dst_color_blendfactor = dc;
            color.blend_state.color_blend_op = cop;
            color.blend_state.src_alpha_blendfactor = sa;
            color.blend_state.dst_alpha_blendfactor = da;
            color.blend_state.alpha_blend_op = aop;
        }
        color.blend_state.enable_color_write_mask = state.color_write != 0xFu;
        color.blend_state.color_write_mask = sdlColorMask(state.color_write);
        SDL_GPUVertexBufferDescription vb{};
        vb.slot = 0;
        vb.pitch = sizeof(Vertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attrs[3]{};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[1].offset = sizeof(float) * 3;
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[2].offset = sizeof(float) * 7;
        SDL_GPUVertexInputState vin{};
        vin.vertex_buffer_descriptions = &vb;
        vin.num_vertex_buffers = 1;
        vin.vertex_attributes = attrs;
        vin.num_vertex_attributes = 3;
        SDL_GPUGraphicsPipelineCreateInfo ci{};
        ci.vertex_shader = vertex_shader_;
        ci.fragment_shader = textured ? texShaderFor(state.tex_mode) : pixel_shader_;
        if (ci.fragment_shader == nullptr) {
            if (err) *err = "SDL_GPU 纹理片元置换不可用";
            return nullptr;
        }
        ci.vertex_input_state = vin;
        ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        ci.depth_stencil_state.compare_op = ztestAlways() ? SDL_GPU_COMPAREOP_ALWAYS : compareOp(state.depth_func);
        ci.depth_stencil_state.enable_depth_test = state.depth_test;
        ci.depth_stencil_state.enable_depth_write = state.depth_test && state.depth_write;
        ci.target_info.color_target_descriptions = &color;
        ci.target_info.num_color_targets = 1;
        ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        ci.target_info.has_depth_stencil_target = true;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device_, &ci);
        if (p == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUGraphicsPipeline 失败：") + SDL_GetError();
            return nullptr;
        }
        pipelines_[key] = p;
        return p;
    }

    // -----------------------------------------------------------------------------------
    // 诊断（`LIFT_GPU_ZTEST=always`）：把深度比较强制成 ALWAYS —— **下一层判据**，不是修复
    // -----------------------------------------------------------------------------------
    // ★ 用途（`REQ-01M73P3GXW22Y467YGYPQSM571` 的 D1 被否掉之后的判别实验）：
    //   `--raster gpu` 臂整层丢 FF 纹理批；纹理副本已用 `LIFT_GPU_FF_TEX` 证明与 guest 逐字节相同
    //   ⇒ 下一次要分的是"**片元被深度判掉**"还是"片元根本没到 target / 采样错"。
    //   ALWAYS 让深度测试**完全不拒片元**：角色区从 0 变非 0 ⇒ 深度是机制；仍 0 ⇒ 深度被否掉。
    // ★ ⛔ 不是修复：缺省关闭、env 不设时与原行为逐指令相同（只是多读一次 static bool）。
    // ★ 诊断（`LIFT_GPU_DEPTH_CLEAR=frame`）：每个 flush 的第一段都把深度清成 1.0。
    //   用途 = 验证"深度附件只在 `rasterPrepare` 建 target 时清过一次"这条根因：
    //   `d3d9::clear(ZBUFFER)` 的 arm 走的是 `m->gpuRaster()->rasterRequestDepthClear()`，而
    //   `SdlWindow`（present_sdl）**没有转发**这个虚函数 ⇒ 后端拿到的一直是基类 no-op。
    //   ⛔ 缺省关闭、不是修复（真修法是把转发补到 SdlWindow，那不在本单的允许改动面里）。
    static bool depthClearEveryFlush() {
        static const bool on = [] {
            const char* v = std::getenv("LIFT_GPU_DEPTH_CLEAR");
            const bool want = v != nullptr && std::strcmp(v, "frame") == 0;
            if (want) {
                std::fprintf(stderr, "[gpu-depth] mode=clear-every-flush（诊断；⛔ 不是修复）\n");
            }
            return want;
        }();
        return on;
    }

    static bool ztestAlways() {        static const bool on = [] {
            const char* v = std::getenv("LIFT_GPU_ZTEST");
            const bool want = v != nullptr && std::strcmp(v, "always") == 0;
            if (want) {
                std::fprintf(stderr, "[gpu-ztest] compare_op=ALWAYS（诊断覆盖：深度不拒片元；⛔ 不是修复）\n");
            }
            return want;
        }();
        return on;
    }

    SDL_GPUShader* texShaderFor(GpuTexMode mode) const {
        for (std::size_t i = 0; i < kTexEntryCount; ++i) {
            if (kTexEntries[i].mode == mode) return pixel_shaders_tex_[i];
        }
        return nullptr;
    }

    static SDL_GPUCompareOp compareOp(std::uint32_t f) {
        switch (f) {
            case 1: return SDL_GPU_COMPAREOP_NEVER;
            case 2: return SDL_GPU_COMPAREOP_LESS;
            case 3: return SDL_GPU_COMPAREOP_EQUAL;
            case 4: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
            case 5: return SDL_GPU_COMPAREOP_GREATER;
            case 6: return SDL_GPU_COMPAREOP_NOT_EQUAL;
            case 7: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
            default: return SDL_GPU_COMPAREOP_ALWAYS;
        }
    }

#if defined(_WIN32)
    bool createShaders(std::string* err) {
        ID3DBlob* vsb = nullptr;
        ID3DBlob* psb = nullptr;
        ID3DBlob* errors = nullptr;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "d3d9_gpu.hlsl", nullptr, nullptr, "VSMain",
                              "vs_5_1", flags, 0, &vsb, &errors))) {
            if (err) *err = std::string("D3DCompile(VS) 失败：") + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            if (errors) errors->Release();
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
        auto make = [&](ID3DBlob* b, SDL_GPUShaderStage stage, const char* entry, Uint32 samplers) -> SDL_GPUShader* {
            SDL_GPUShaderCreateInfo ci{};
            ci.code = static_cast<const Uint8*>(b->GetBufferPointer());
            ci.code_size = b->GetBufferSize();
            ci.entrypoint = entry;
            ci.format = kShaderFormat;
            ci.stage = stage;
            ci.num_samplers = samplers;
            return SDL_CreateGPUShader(device_, &ci);
        };
        vertex_shader_ = make(vsb, SDL_GPU_SHADERSTAGE_VERTEX, "VSMain", 0);
        vsb->Release();
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "d3d9_gpu.hlsl", nullptr, nullptr, "PSMain",
                              "ps_5_1", flags, 0, &psb, &errors))) {
            if (err) *err = std::string("D3DCompile(PS) 失败：") + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            if (errors) errors->Release();
            return false;
        }
        if (errors) { errors->Release(); errors = nullptr; }
        pixel_shader_ = make(psb, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", 0);
        psb->Release();
        bool ok = vertex_shader_ != nullptr && pixel_shader_ != nullptr;
        for (std::size_t i = 0; i < kTexEntryCount; ++i) {
            ID3DBlob* ps = nullptr;
            if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "d3d9_gpu.hlsl", nullptr, nullptr,
                                  kTexEntries[i].entry, "ps_5_1", flags, 0, &ps, &errors))) {
                if (err) *err = std::string("D3DCompile(") + kTexEntries[i].entry + ") 失败：" +
                                (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors) errors->Release();
                ok = false;
                continue;
            }
            if (errors) { errors->Release(); errors = nullptr; }
            pixel_shaders_tex_[i] = make(ps, SDL_GPU_SHADERSTAGE_FRAGMENT, kTexEntries[i].entry, 1);
            ps->Release();
            if (pixel_shaders_tex_[i] == nullptr) ok = false;
        }
        if (!ok) {
            if (err && err->empty()) *err = std::string("SDL_CreateGPUShader(DXBC) 失败：") + SDL_GetError();
            return false;
        }
        return true;
    }
#elif defined(__APPLE__)
    bool createShaders(std::string* err) {
        auto make = [&](const char* entry, SDL_GPUShaderStage stage, Uint32 samplers) -> SDL_GPUShader* {
            SDL_GPUShaderCreateInfo ci{};
            ci.code = reinterpret_cast<const Uint8*>(kShaderSource);
            ci.code_size = std::strlen(kShaderSource);
            ci.entrypoint = entry;
            ci.format = kShaderFormat;
            ci.stage = stage;
            ci.num_samplers = samplers;
            return SDL_CreateGPUShader(device_, &ci);
        };
        vertex_shader_ = make("VSMain", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        pixel_shader_ = make("PSMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);
        bool ok = vertex_shader_ != nullptr && pixel_shader_ != nullptr;
        for (std::size_t i = 0; i < kTexEntryCount; ++i) {
            pixel_shaders_tex_[i] = make(kTexEntries[i].entry, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
            if (pixel_shaders_tex_[i] == nullptr) ok = false;
        }
        if (!ok) {
            if (err) *err = std::string("SDL_CreateGPUShader(MSL) 失败：") + SDL_GetError();
            return false;
        }
        return true;
    }
#else
    bool createShaders(std::string*) { return false; }
#endif

    void shutdown() {
        if (device_ == nullptr) return;
        // ★ 窗口 claim 必须在 SDL_DestroyWindow 之前释放（调用方保证这个顺序）；
        //   设备销毁也必须在 release 之后。
        queue_.clear();
        queued_vertices_ = 0;
        segments_.assign(1, Segment{});
        staging_used_ = 0;
        staging_.clear();
        for (auto& kv : textures_) {
            kv.second.pending = false;
        }
        directDetachWindow();
        destroyTargets();
        if (vertex_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        vertex_buffer_ = nullptr;
        for (auto& [k, e] : textures_) {
            if (e.texture != nullptr) SDL_ReleaseGPUTexture(device_, e.texture);
        }
        textures_.clear();
        for (auto& [k, s] : samplers_) {
            if (s != nullptr) SDL_ReleaseGPUSampler(device_, s);
        }
        samplers_.clear();
        if (vertex_shader_ != nullptr) SDL_ReleaseGPUShader(device_, vertex_shader_);
        if (pixel_shader_ != nullptr) SDL_ReleaseGPUShader(device_, pixel_shader_);
        for (SDL_GPUShader*& ps : pixel_shaders_tex_) {
            if (ps != nullptr) SDL_ReleaseGPUShader(device_, ps);
            ps = nullptr;
        }
        vertex_shader_ = nullptr;
        pixel_shader_ = nullptr;
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vertex_shader_ = nullptr;
    SDL_GPUShader* pixel_shader_ = nullptr;
    SDL_GPUShader* pixel_shaders_tex_[kTexEntryCount] = {};
    SDL_GPUTexture* target_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    SDL_GPUBuffer* vertex_buffer_ = nullptr;
    Uint32 vertex_capacity_ = 0;
    std::map<std::uint64_t, SDL_GPUGraphicsPipeline*> pipelines_;
    std::map<std::uint64_t, TextureEntry> textures_;
    std::map<std::uint32_t, SDL_GPUSampler*> samplers_;
    std::uint64_t texture_use_ = 0;
    // ★ 诊断探针（`LIFT_GPU_FF_TEX`）：已经采过的纹理键 / 上限。
    //   上限 = "每轮每条纹理一次、最多 4 条 / 只采前 2 轮"：第 1 轮证明**首次上传**的内容对不对，
    //   第 2 轮证明**没重传的那一帧**（`up=0`、版本没变的快路径）GPU 副本是否仍等于 guest。
    static constexpr std::size_t kFfProbeMax = 4;
    static constexpr std::size_t kFfProbeRounds = 2;
    std::vector<std::uint64_t> ff_probe_keys_;
    std::size_t ff_probe_round_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool alpha_ = false;
    // ★ 镜像身份：这块 target 对应的 guest 像素基址（nullptr = 不知道/不参与判断）。
    //   基址 / 尺寸 / 格式任一变化 ⇒ 换 target（先 flush + 回读旧目标，见 rasterPrepare）。
    const std::uint8_t* target_base_ = nullptr;
    std::uint32_t target_pitch_ = 0;   // guest 行距（32bpp ⇒ width*4）
    SurfaceFormat target_format_{};    // 建 target 时的完整格式（⛔ 只比 bpp+alpha 会漏通道差异）
    // ★ 合批队列：只保存 CPU 拷贝的顶点 + 状态 + GPU 句柄（⛔ 不保存 guest 像素指针）。
    std::vector<QueuedDraw> queue_;
    // ★ 方案 C 的段：内容变更边界把命令缓冲切开（copy→render→copy→render…），但只有 flush 才提交。
    //   `segments_` 恒非空；`segments_[0].first_draw == 0`。
    std::vector<Segment> segments_ = std::vector<Segment>(1);
    // 帧内 staging arena（见 reserveStaging）；flush 成功或 discard 后 `staging_used_ = 0`。
    std::vector<std::uint8_t> staging_;
    std::size_t staging_used_ = 0;
    // ★ 派生 mip 的中间缓冲（等级链，按 `mipLevelOffset` 排）：容量只增、跨上传复用，
    //   ⛔ 不放 staging_（staging 只放**要上传的字节**，混用会让 offset 记账变复杂）。
    std::vector<std::uint8_t> mip_scratch_;
    std::size_t queued_vertices_ = 0;
    bool need_depth_clear_ = true;
    // ★ dirty_ = 「**已提交**但尚未读回 guest」；队列/未提交上传也算脏 —— 对外只暴露
    //   `rasterDirty() = dirty_ || !queue_.empty() || hasPendingUploads()`（见 gpu_raster.h 的语义）。
    bool dirty_ = false;
    std::string name_;
    // ---- GPU 直出（窗口 swapchain）----
    SDL_Window* window_ = nullptr;
    bool claimed_ = false;
    // 本帧的最新像素是否已经在 target 里（d3d9::Present 在读回/同步之后声明；见 gpu_raster.h）
    bool target_current_ = false;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUPresentMode present_mode_ = SDL_GPU_PRESENTMODE_VSYNC;
    const char* present_mode_name_ = "vsync";
    std::uint64_t presented_ = 0;
    std::uint64_t present_skipped_ = 0;
    // ---- 调试叠层（`--overlay`；只存在于宿主送显的那一份像素里）----
    // ★ 纹理建在**同一 device**、格式 = 实际 swapchain 格式（blit 源/目的兼容的唯一保证）。
    SDL_GPUTexture* overlay_tex_ = nullptr;
    int overlay_w_ = 0;
    int overlay_h_ = 0;
    bool overlay_visible_ = false;           // `directPresent` 是否要在 letterbox 之后再 blit 一次
    int overlay_dst_x_ = 0;                  // 上一次叠层 blit 的 dst 原点（窗口像素 = 画面 rect 原点）
    int overlay_dst_y_ = 0;
    bool overlay_dst_valid_ = false;         // false = 上一帧没画叠层（隐藏 / 还没 present 过）
    std::vector<std::uint8_t> overlay_rgba_;  // CPU 副本：`directReadback` 合成用（RGBA8）
};

}  // namespace

GpuRasterStats gpuRasterStats() { return g_stats; }

// ---------------------------------------------------------------------------------------
// 覆盖账本的批号通道（声明与"为什么"见 `gpu_raster.h`）
// ---------------------------------------------------------------------------------------
// ★ 生产者（`d3d9_pipeline.cpp`）在每次 `rasterQueue` 前后设/清；后端只读**入队那一刻**的值，
//   存进队列条目 ⇒ flush 时按条目回报（一条 draw 一个批号，⛔ 不按"当前值"猜）。
namespace {
std::uint64_t g_batch_tag = 0;
}  // namespace

void gpuRasterSetBatchTag(std::uint64_t tag) { g_batch_tag = tag; }

std::uint64_t gpuRasterBatchTag() { return g_batch_tag; }

std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err) {
    auto r = std::make_unique<SdlGpuRaster>();
    if (!r->init(err)) return nullptr;
    return r;
}

}  // namespace lift
