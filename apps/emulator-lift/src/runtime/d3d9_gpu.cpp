// emulator-lift · SDL_GPU 光栅化后端
//（屏幕空间三角形 + D3DXSprite 四边形 + D3D9 stage 0 纹理）
//
// ★ 与 SDL_Renderer 呈现解耦：自己的 SDL_GPUDevice + BGRA8 颜色 target + D32 深度 target。
// ★ 顶点由 CPU 做完 T&L/裁剪/屏幕映射；这里转成 NDC 后画 triangle list。
// ★ 读回只在 Present（或 CPU 回退）发生；A8R8G8B8 的 A 字节按 guest X8 语义归一化为 0。
// ★ shader 后端按 **SDL_GPU 的原生 shader 格式**分平台：
//   Windows = D3DCompile → DXBC；macOS = MSL 源码（Metal）。⛔ 不把 DXBC 带进 Metal。
//
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

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "runtime/gpu_raster.h"

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

class SdlGpuRaster final : public GpuRaster {
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

    bool rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err) override {
        if (format.bpp != 32) {
            if (err) *err = "SDL_GPU 只支持 32bpp 后缓冲";
            return false;
        }
        const bool alpha = format.amask != 0;
        if (target_ != nullptr && width_ == width && height_ == height && alpha_ == alpha) return true;
        destroyTargets();
        width_ = width;
        height_ = height;
        alpha_ = alpha;
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
        return true;
    }

    bool rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                             std::string* err) override {
        if (target_ == nullptr || width != width_ || height != height_) {
            if (err) *err = "SDL_GPU target 未准备";
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
        need_depth_clear_ = true;
        dirty_ = false;
        return true;
    }

    bool rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                           std::string* err) override {
        if (target_ == nullptr || width != width_ || height != height_) {
            if (err) *err = "SDL_GPU target 未准备";
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

    bool rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                    std::string* err) override {
        return drawBatch(vertices, count, state, false, nullptr, err);
    }

    bool rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterTexture& texture,
                            const GpuRasterState& state, std::string* err) override {
        return drawBatch(vertices, count, state, true, &texture, err);
    }

    bool rasterDirty() const override { return dirty_; }

private:
    struct Vertex {
        float x, y, z;
        float r, g, b, a;
        float u, v;
    };
    static_assert(sizeof(Vertex) == 36, "vertex stride");

    struct TextureEntry {
        SDL_GPUTexture* texture = nullptr;
        std::uint64_t hash = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t generation = 0;  // 上传时的 guest 内容版本（0 = 未知）
        std::uint64_t use = 0;
    };

    bool drawBatch(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state, bool textured,
                   const GpuRasterTexture* texture, std::string* err) {
        if (count == 0) return true;
        SDL_GPUTexture* tex = nullptr;
        if (textured && !ensureTexture(*texture, &tex, err)) return false;
        if (target_ == nullptr || depth_ == nullptr) {
            if (err) *err = "SDL_GPU target 未准备";
            return false;
        }
        const Uint32 bytes = static_cast<Uint32>(count * sizeof(Vertex));
        if (bytes > vertex_capacity_) {
            if (vertex_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            bi.size = bytes;
            vertex_buffer_ = SDL_CreateGPUBuffer(device_, &bi);
            vertex_capacity_ = bytes;
            if (vertex_buffer_ == nullptr) {
                if (err) *err = "SDL_CreateGPUBuffer(vertex) 失败";
                return false;
            }
        }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bytes;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = "SDL_CreateGPUTransferBuffer(vertex) 失败";
            return false;
        }
        auto* mapped = static_cast<Vertex*>(SDL_MapGPUTransferBuffer(device_, tb, false));
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(vertex) 失败";
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            Vertex& v = mapped[i];
            v.x = vertices[i].x / static_cast<float>(width_) * 2.0f - 1.0f;
            v.y = 1.0f - vertices[i].y / static_cast<float>(height_) * 2.0f;
            v.z = vertices[i].z;
            v.r = vertices[i].r;
            v.g = vertices[i].g;
            v.b = vertices[i].b;
            v.a = vertices[i].a;
            v.u = vertices[i].u;
            v.v = vertices[i].v;
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{};
        src.transfer_buffer = tb;
        src.offset = 0;
        SDL_GPUBufferRegion dst{};
        dst.buffer = vertex_buffer_;
        dst.offset = 0;
        dst.size = bytes;
        SDL_UploadToGPUBuffer(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);

        SDL_GPUGraphicsPipeline* pso = pipelineFor(state, textured, err);
        if (pso == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            SDL_SubmitGPUCommandBuffer(cmd);
            return false;
        }
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
        SDL_BindGPUGraphicsPipeline(pass, pso);
        SDL_GPUViewport vp{};
        vp.x = static_cast<float>(state.viewport_x);
        vp.y = static_cast<float>(state.viewport_y);
        vp.w = static_cast<float>(state.viewport_w);
        vp.h = static_cast<float>(state.viewport_h);
        vp.min_depth = 0.0f;
        vp.max_depth = 1.0f;
        SDL_SetGPUViewport(pass, &vp);
        SDL_Rect scissor{state.scissor_x0, state.scissor_y0, state.scissor_x1 - state.scissor_x0,
                         state.scissor_y1 - state.scissor_y0};
        if (!state.scissor_enabled ||
            (state.scissor_x0 == 0 && state.scissor_y0 == 0 && state.scissor_x1 == width_ &&
             state.scissor_y1 == height_)) {
            scissor = SDL_Rect{0, 0, width_, height_};
        }
        SDL_SetGPUScissor(pass, &scissor);
        SDL_GPUBufferBinding binding{vertex_buffer_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        if (textured) {
            SDL_GPUSampler* samp = samplerFor(texture->sampler, err);
            if (samp == nullptr) {
                SDL_EndGPURenderPass(pass);
                SDL_SubmitGPUCommandBuffer(cmd);
                SDL_ReleaseGPUTransferBuffer(device_, tb);
                return false;
            }
            SDL_GPUTextureSamplerBinding tsb{tex, samp};
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        }
        SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(count), 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        need_depth_clear_ = false;
        dirty_ = true;
        return true;
    }

    // 纹理缓存：key = guest 像素指针 + 尺寸 + alpha 语义；内容哈希不变 ⇒ 不重传。
    bool ensureTexture(const GpuRasterTexture& t, SDL_GPUTexture** out, std::string* err) {
        if (t.format.bpp != 32) {
            if (err) *err = "SDL_GPU 纹理只支持 32bpp";
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
        if (same_size && t.generation != 0 && it->second.generation == t.generation) {
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
        SDL_GPUTexture* tex = it != textures_.end() ? it->second.texture : nullptr;
        const bool needs_create =
            tex == nullptr || it->second.width != static_cast<std::uint32_t>(width) ||
            it->second.height != static_cast<std::uint32_t>(height);
        if (needs_create) {
            if (tex != nullptr) SDL_ReleaseGPUTexture(device_, tex);
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
        const Uint32 size = static_cast<Uint32>(static_cast<std::size_t>(width) * 4u * height);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = size;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) {
            if (err) *err = "SDL_CreateGPUTransferBuffer(texture) 失败";
            return false;
        }
        auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_MapGPUTransferBuffer(texture) 失败";
            return false;
        }
        for (int y = 0; y < height; ++y) {
            std::uint8_t* dst_row = mapped + static_cast<std::size_t>(y) * width * 4;
            std::memcpy(dst_row, pixels + static_cast<std::size_t>(y) * pitch,
                        static_cast<std::size_t>(width) * 4);
            if (!has_alpha) {
                // guest X8R8G8B8：X 字节不是 alpha。CPU 侧按 amask==0 ⇒ alpha=255。
                for (int x = 0; x < width; ++x) dst_row[x * 4 + 3] = 255;
            }
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb;
        src.pixels_per_row = static_cast<Uint32>(width);
        src.rows_per_layer = static_cast<Uint32>(height);
        SDL_GPUTextureRegion dst{};
        dst.texture = tex;
        dst.w = static_cast<Uint32>(width);
        dst.h = static_cast<Uint32>(height);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        TextureEntry& e = textures_[key];
        e.texture = tex;
        e.hash = hash;
        e.width = static_cast<std::uint32_t>(width);
        e.height = static_cast<std::uint32_t>(height);
        e.generation = t.generation;
        e.use = ++texture_use_;
        evictTextures();
        *out = tex;
        return true;
    }

    void evictTextures() {
        // 只防失控增长：超过 128 个纹理就回收最久未用的。
        constexpr std::size_t kMaxTextures = 128;
        while (textures_.size() > kMaxTextures) {
            auto victim = textures_.begin();
            for (auto it = textures_.begin(); it != textures_.end(); ++it) {
                if (it->second.use < victim->second.use) victim = it;
            }
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
    bool need_depth_clear_ = true;
    bool dirty_ = false;
    std::string name_;
};

}  // namespace

std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err) {
    auto r = std::make_unique<SdlGpuRaster>();
    if (!r->init(err)) return nullptr;
    return r;
}

}  // namespace lift
