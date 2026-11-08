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
//   `rasterFlush` 一次做完：一个 copy pass（所有待上传纹理 + 所有顶点）→ 一个 render pass
//   （逐条切 pipeline/viewport/scissor/sampler/纹理，顺序不变）→ 一次 `SDL_SubmitGPUCommandBuffer`。
//   ⛔ 只合并提交，不合并 draw；⛔ 不跨 target 共用一个 render pass。
// ★ 屏障清单（凡是要读/写 target 或换 target 的，先 flush；要 guest 像素的再 syncToGuest）：
//   · 读 target：`rasterSyncToGuest`（Present / CPU 回退 / dump）、SDL_GPU 的 `directReadback`；
//   · 写 target（CPU 侧）：`d3d9::clear`（TARGET 位）、`d3d9::lockRect`（锁的是当前镜像目标时）；
//   · 换 target：`rasterPrepare`（基址/尺寸/格式任一变化）、`d3d9_pipeline::setRenderTarget`；
//   · 呈现：`d3d9::present`（读回）、SDL_GPU 的 `directPresent`。
//   ★ `rasterSyncFromGuest` 反过来：调用方保证 guest 是权威 ⇒ 队列非空是**错误**（⛔ 不静默覆盖）。
// ★ 能力边界（准入在 d3d9_pipeline.cpp 的 gpuEligible；这里只负责"画得出"）：
//   · 混合 = SDL_GPUColorTargetBlendState 的 **color / alpha 两段**（支持 D3D 的 separate alpha）；
//     映射不了的因子/op 明确失败（d3d9_pipeline 会先拒绝，这里再兜一层）。
//   · 纹理 = stage 0 单级 32bpp；片元置换（GpuTexMode）选择对应 entrypoint。
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
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "runtime/gpu_raster.h"
#include "runtime/pixel_format.h"

namespace lift {
namespace {

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
        // ★ guest 是权威 ⇒ 队列里未提交的 draw 会被这次上传覆盖。调用方必须先 flush（约定见
        //   gpu_raster.h）；这里明确拒绝，⛔ 不静默丢像素。
        if (!queue_.empty()) {
            if (err) *err = "SDL_GPU：队列非空时不能从 guest 覆盖镜像（先 rasterFlush）";
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
        need_depth_clear_ = true;
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
        // ★ 纹理在**入队时**就交给后端（像素拷进 staging；⛔ 不存 guest 指针）。
        if (texture != nullptr) {
            if (!ensureTexture(*texture, &draw.texture, err)) return false;
            draw.sampler = samplerFor(texture->sampler, err);
            if (draw.sampler == nullptr) return false;
        }
        // ★ pipeline 也在入队时解析：flush 里只剩"录命令"，把可失败点前移（失败 ⇒ 调用方回退 CPU）。
        draw.pipeline = pipelineFor(state, draw.textured, err);
        if (draw.pipeline == nullptr) return false;
        // 顶点缓冲在入队时就长够（flush 里不再创建 GPU 缓冲）。
        const std::uint64_t need_bytes = (static_cast<std::uint64_t>(queued_vertices_) + count) * sizeof(Vertex);
        if (need_bytes > 0xFFFFFFFFull) {
            if (err) *err = "SDL_GPU：一批顶点的总字节数超过 4 GiB";
            return false;
        }
        if (!ensureVertexBuffer(static_cast<Uint32>(need_bytes), err)) return false;
        draw.vertex_offset = static_cast<std::uint32_t>(queued_vertices_);
        queued_vertices_ += count;
        queue_.push_back(std::move(draw));
        return true;
    }

    // ★ 一次 flush = 一个 command buffer：copy pass（纹理 + 顶点）→ render pass（逐条 draw）→ submit。
    //   队列空 ⇒ 零成本 true（⛔ 不提交空 command buffer）。
    bool rasterFlush(std::string* err) override {
        if (queue_.empty()) return true;
        if (target_ == nullptr || depth_ == nullptr) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }

        // ---- 1) 待上传纹理：把所有 staging 合到**一个** transfer buffer ----
        std::vector<TextureEntry*> uploads;
        std::size_t texture_bytes = 0;
        for (auto& kv : textures_) {
            if (!kv.second.pending) continue;
            uploads.push_back(&kv.second);
            texture_bytes += kv.second.staged.size();
            // SDL_GPU 建议 transfer buffer 内的偏移按 512 对齐（D3D12 否则会再拷一份）。
            texture_bytes = (texture_bytes + 511u) & ~static_cast<std::size_t>(511u);
        }
        SDL_GPUTransferBuffer* tex_tb = nullptr;
        SDL_GPUTransferBuffer* vtx_tb = nullptr;
        auto release_tbs = [&]() {
            if (tex_tb != nullptr) SDL_ReleaseGPUTransferBuffer(device_, tex_tb);
            if (vtx_tb != nullptr) SDL_ReleaseGPUTransferBuffer(device_, vtx_tb);
            tex_tb = nullptr;
            vtx_tb = nullptr;
        };
        std::vector<std::uint32_t> tex_offsets(uploads.size(), 0);
        if (!uploads.empty()) {
            SDL_GPUTransferBufferCreateInfo ti{};
            ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            ti.size = static_cast<Uint32>(texture_bytes);
            tex_tb = SDL_CreateGPUTransferBuffer(device_, &ti);
            if (tex_tb == nullptr) {
                if (err) *err = std::string("SDL_CreateGPUTransferBuffer(textures) 失败：") + SDL_GetError();
                return false;  // 队列保留：下次还能重试
            }
            auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, tex_tb, false));
            if (mapped == nullptr) {
                release_tbs();
                if (err) *err = "SDL_MapGPUTransferBuffer(textures) 失败";
                return false;
            }
            std::size_t cursor = 0;
            for (std::size_t i = 0; i < uploads.size(); ++i) {
                cursor = (cursor + 511u) & ~static_cast<std::size_t>(511u);
                tex_offsets[i] = static_cast<std::uint32_t>(cursor);
                std::memcpy(mapped + cursor, uploads[i]->staged.data(), uploads[i]->staged.size());
                cursor += uploads[i]->staged.size();
            }
            SDL_UnmapGPUTransferBuffer(device_, tex_tb);
        }

        // ---- 2) 所有顶点合到**一个** transfer buffer（对应一个 GPU vertex buffer）----
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
            return false;  // 队列保留
        }
        // ---- 3) 一个 copy pass：纹理 + 顶点 ----
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        for (std::size_t i = 0; i < uploads.size(); ++i) {
            TextureEntry* e = uploads[i];
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = tex_tb;
            src.offset = tex_offsets[i];
            src.pixels_per_row = e->width;
            src.rows_per_layer = e->height;
            SDL_GPUTextureRegion dst{};
            dst.texture = e->texture;
            dst.w = e->width;
            dst.h = e->height;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, true);
        }
        if (vertex_bytes > 0) {
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

        // ---- 4) 一个 render pass：按入队顺序逐条切状态 + draw ----
        SDL_GPUColorTargetInfo color{};
        color.texture = target_;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = depth_;
        depth.clear_depth = 1.0f;
        depth.load_op = need_depth_clear_ ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
        for (const QueuedDraw& d : queue_) {
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
            SDL_GPUBufferBinding binding{vertex_buffer_, d.vertex_offset * static_cast<Uint32>(sizeof(Vertex))};
            SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
            if (d.textured) {
                SDL_GPUTextureSamplerBinding tsb{d.texture, d.sampler};
                SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
            }
            SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(d.vertices.size()), 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            release_tbs();
            if (err) *err = std::string("SDL_SubmitGPUCommandBuffer 失败：") + SDL_GetError();
            return false;  // 队列保留（没提交成功 ⇒ 下次还能重试，⛔ 不静默丢像素）
        }
        release_tbs();

        // ---- 5) 记账：pending 纹理已落盘、队列清空 ----
        for (TextureEntry* e : uploads) {
            e->pending = false;
            e->staged.clear();
            e->staged.shrink_to_fit();
        }
        queue_.clear();
        queued_vertices_ = 0;
        need_depth_clear_ = false;
        dirty_ = true;
        evictTextures();
        return true;
    }

    bool rasterDirty() const override { return dirty_ || !queue_.empty(); }
    const void* rasterTargetBase() const override { return target_base_; }
    std::size_t rasterQueuedDraws() const override { return queue_.size(); }

    void rasterDiscardTarget() override {
        // ★ 设备销毁前调用：队列与镜像都指向即将释放的 guest 像素 ⇒ 丢掉，⛔ 不回读。
        queue_.clear();
        queued_vertices_ = 0;
        for (auto& kv : textures_) {
            if (!kv.second.pending) continue;
            // staging 没上传过 ⇒ 内容版本作废（下次按内容哈希重传）
            kv.second.pending = false;
            kv.second.staged.clear();
            kv.second.staged.shrink_to_fit();
            kv.second.hash = 0;
            kv.second.generation = 0;
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
        need_depth_clear_ = true;
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
            SDL_SubmitGPUCommandBuffer(cmd);  // 不 Cancel：Cancel 与 swapchain 的状态机容易踩边界
            if (err) *err = why;
            return false;
        }
        if (swap == nullptr || sw == 0 || sh == 0) {
            // 最小化 / 隐藏到零尺寸：这一帧没有可呈现的目标。⛔ 不算失败，也不冒充"画上去了"。
            SDL_SubmitGPUCommandBuffer(cmd);
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
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            if (err) *err = std::string("SDL_SubmitGPUCommandBuffer 失败：") + SDL_GetError();
            return false;
        }
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
        if (width != nullptr) *width = sw;
        if (height != nullptr) *height = sh;
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
        SDL_SubmitGPUCommandBuffer(cmd);
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
        std::uint64_t hash = 0;         // 已上传 / 待上传内容的哈希
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t generation = 0;   // 上传时的 guest 内容版本（0 = 未知）
        std::uint64_t use = 0;
        // ★ 入队阶段拷下来的 BGRA 像素（⛔ 不保存 guest 指针；D3DXSprite 在 End 末尾就 Release
        //   纹理）。flush 时把所有 pending 合到**一个 copy pass** 上传。
        bool pending = false;
        std::vector<std::uint8_t> staged;
    };

    // 一条已入队的 draw：状态/句柄逐条保留（⛔ 不合并两条状态不同的 draw）。
    struct QueuedDraw {
        std::vector<GpuRasterVertex> vertices;  // 屏幕空间（flush 时按当时的 target 尺寸转 NDC）
        GpuRasterState state;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUSampler* sampler = nullptr;
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        bool textured = false;
        std::uint32_t vertex_offset = 0;  // 在 flush 的顶点缓冲里（单位 = Vertex）
    };

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
    // ★ 合批后：**入队时**把 guest 像素拷进 staging（⛔ 不保存 guest 指针），flush 时统一上传。
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
        // ★ 身份键：serial != 0 时用进程级纹理 serial（⛔ 不用 arena 指针 / 状态块 EA —— 两者都会复用）；
        //   serial == 0（夹具的 1×1 白纹理）才退回 guest 指针键。
        const std::uint64_t key = gpuTextureCacheKey(t, width, height);
        auto it = textures_.find(key);
        const bool same_size = it != textures_.end() && it->second.texture != nullptr &&
                               it->second.width == static_cast<std::uint32_t>(width) &&
                               it->second.height == static_cast<std::uint32_t>(height);
        // ★ 快路径：guest 给了内容版本且没变 ⇒ **不哈希、不重传**（TITLE 的 Sprite 曾因每 draw
        //   完整 upload 而 ~118–206 ms；哈希本身在 2048² 纹理上也要 ~100 ms/帧）。
        if (same_size && !it->second.pending && t.generation != 0 && it->second.generation == t.generation) {
            it->second.use = ++texture_use_;
            *out = it->second.texture;
            return true;
        }
        const std::uint64_t hash = hashPixels(pixels, pitch, width, height);
        if (same_size && it->second.hash == hash) {
            // 版本变了但像素内容没变（例如 Unlock 后没改）⇒ 只更新版本，不重传。
            it->second.generation = t.generation;
            it->second.use = ++texture_use_;
            *out = it->second.texture;
            return true;
        }
        // ★ 同一个 GPU 纹理句柄的内容变了，而它**正被队列里的 draw 引用**（含已上传、pending=false
        //   的情况：这批更早的 sprite 已经入队，句柄还在 queue_ 里）⇒ 必须先把队列 flush 掉。
        //   理由：upload 是 GPU 时间线上 render pass 之前才完成的，如果不切命令缓冲，先入队的 draw
        //   会采样到**新**上传的内容（⛔ 不是它入队时的那份像素）。
        if (it != textures_.end() && (it->second.pending || !queue_.empty())) {
            if (!rasterFlush(err)) return false;
            // flush 可能顺手回收缓存项（LRU）⇒ 重新查找；找不到就走"新建纹理"路径。
            it = textures_.find(key);
        }
        SDL_GPUTexture* tex = it != textures_.end() ? it->second.texture : nullptr;
        if (tex == nullptr) {
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D;
            ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ci.width = static_cast<Uint32>(width);
            ci.height = static_cast<Uint32>(height);
            ci.layer_count_or_depth = 1;
            ci.num_levels = 1;
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
            created.width = static_cast<std::uint32_t>(width);
            created.height = static_cast<std::uint32_t>(height);
        }
        // ★ 入队时就把像素拷成 staging（BGRA；无 alpha 的 X8 格式把 A 填 255）。
        TextureEntry& e = textures_[key];
        e.texture = tex;
        e.hash = hash;
        e.width = static_cast<std::uint32_t>(width);
        e.height = static_cast<std::uint32_t>(height);
        e.generation = t.generation;
        e.use = ++texture_use_;
        e.pending = true;
        e.staged.resize(static_cast<std::size_t>(width) * 4u * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::uint8_t* dst_row = e.staged.data() + static_cast<std::size_t>(y) * width * 4;
            std::memcpy(dst_row, pixels + static_cast<std::size_t>(y) * pitch,
                        static_cast<std::size_t>(width) * 4);
            if (!has_alpha) {
                // guest X8R8G8B8：X 字节不是 alpha。CPU 侧按 amask==0 ⇒ alpha=255。
                for (int x = 0; x < width; ++x) dst_row[x * 4 + 3] = 255;
            }
        }
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
        ci.depth_stencil_state.compare_op = compareOp(state.depth_func);
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
        for (auto& kv : textures_) {
            kv.second.pending = false;
            kv.second.staged.clear();
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
    std::size_t queued_vertices_ = 0;
    bool need_depth_clear_ = true;
    // ★ dirty_ = 「**已提交**但尚未读回 guest」；队列本身也算脏 —— 对外只暴露
    //   `rasterDirty() = dirty_ || !queue_.empty()`（见 gpu_raster.h 的语义）。
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
};

}  // namespace

std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err) {
    auto r = std::make_unique<SdlGpuRaster>();
    if (!r->init(err)) return nullptr;
    return r;
}

}  // namespace lift
