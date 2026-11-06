// emulator-lift · SDL_GPU 光栅化后端（屏幕空间三角形 + D3DXSprite 纹理四边形）
//
// ★ 与 SDL_Renderer 呈现解耦：自己的 SDL_GPUDevice + BGRA8 颜色 target + D32 深度 target。
// ★ 顶点由 CPU 做完 T&L/裁剪/屏幕映射；这里转成 NDC 后画 triangle list。
// ★ 读回只在 Present（或 CPU 回退）发生；A8R8G8B8 的 A 字节按 guest X8 语义归一化为 0。
// ★ shader 后端按 **SDL_GPU 的原生 shader 格式**分平台：
//   Windows = D3DCompile → DXBC；macOS = MSL 源码（Metal）。⛔ 不把 DXBC 带进 Metal。
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
float4 PSMainTex(VSOut i) : SV_Target { return g_tex.Sample(g_samp, i.uv) * i.color; }
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
)MSL";
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_MSL;
constexpr const char* kShaderFormatName = "MSL";
#endif

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
        if (!createSampler(err)) return false;
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
            if (err) *err = "SDL_GPU phase 1 只支持 32bpp 后缓冲";
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

    bool drawBatch(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state, bool textured,
                   const GpuRasterTexture* texture, std::string* err) {
        if (count == 0) return true;
        if (textured && (texture == nullptr || !ensureSpriteTexture(*texture, err))) return false;
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

        SDL_GPUGraphicsPipeline* pso = pipelineFor(state, textured);
        if (pso == nullptr) {
            SDL_ReleaseGPUTransferBuffer(device_, tb);
            if (err) *err = "SDL_CreateGPUGraphicsPipeline 失败";
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
            SDL_GPUTextureSamplerBinding tb{sprite_texture_, sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
        }
        SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(count), 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        need_depth_clear_ = false;
        dirty_ = true;
        return true;
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

    bool ensureSpriteTexture(const GpuRasterTexture& t, std::string* err) {
        if (t.format.bpp != 32 || t.format.amask == 0) {
            if (err) *err = "SDL_GPU 精灵只支持 32bpp 带 alpha 的纹理";
            return false;
        }
        static const std::uint8_t kWhite[4] = {255, 255, 255, 255};
        const std::uint8_t* pixels = t.pixels != nullptr ? t.pixels : kWhite;
        const std::uint32_t pitch = t.pixels != nullptr ? t.pitch : 4;
        if (sprite_texture_ == nullptr || sprite_texture_key_ != t.key || sprite_texture_w_ != t.width ||
            sprite_texture_h_ != t.height) {
            if (sprite_texture_ != nullptr) SDL_ReleaseGPUTexture(device_, sprite_texture_);
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D;
            ci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ci.width = static_cast<Uint32>(t.width);
            ci.height = static_cast<Uint32>(t.height);
            ci.layer_count_or_depth = 1;
            ci.num_levels = 1;
            ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
            sprite_texture_ = SDL_CreateGPUTexture(device_, &ci);
            if (sprite_texture_ == nullptr) {
                if (err) *err = std::string("SDL_CreateGPUTexture(sprite) 失败：") + SDL_GetError();
                return false;
            }
            sprite_texture_key_ = t.key;
            sprite_texture_w_ = t.width;
            sprite_texture_h_ = t.height;
        }
        const Uint32 size = static_cast<Uint32>(static_cast<std::size_t>(t.width) * 4u * t.height);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = size;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &ti);
        if (tb == nullptr) return false;
        auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, tb, false));
        for (int y = 0; y < t.height; ++y) {
            std::memcpy(mapped + static_cast<std::size_t>(y) * t.width * 4,
                        pixels + static_cast<std::size_t>(y) * pitch, static_cast<std::size_t>(t.width) * 4);
        }
        SDL_UnmapGPUTransferBuffer(device_, tb);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{};
        src.transfer_buffer = tb;
        src.pixels_per_row = static_cast<Uint32>(t.width);
        src.rows_per_layer = static_cast<Uint32>(t.height);
        SDL_GPUTextureRegion dst{};
        dst.texture = sprite_texture_;
        dst.w = static_cast<Uint32>(t.width);
        dst.h = static_cast<Uint32>(t.height);
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        return true;
    }

    void destroyTargets() {
        if (target_ != nullptr) SDL_ReleaseGPUTexture(device_, target_);
        if (depth_ != nullptr) SDL_ReleaseGPUTexture(device_, depth_);
        target_ = nullptr;
        depth_ = nullptr;
        for (auto& [k, p] : pipelines_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
        pipelines_.clear();
    }

    SDL_GPUGraphicsPipeline* pipelineFor(const GpuRasterState& state, bool textured) {
        const std::uint32_t key = (state.blend ? 1u : 0u) | (state.depth_test ? 2u : 0u) |
                                  (state.depth_write ? 4u : 0u) | (state.depth_func << 3) |
                                  (textured ? 0x10000u : 0u);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        color.blend_state.enable_blend = state.blend;
        if (state.blend) {
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
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
        ci.fragment_shader = textured ? pixel_shader_tex_ : pixel_shader_;
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
        if (p != nullptr) pipelines_[key] = p;
        return p;
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
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "d3d9_gpu.hlsl", nullptr, nullptr, "PSMain",
                              "ps_5_1", flags, 0, &psb, &errors))) {
            if (err) *err = std::string("D3DCompile(PS) 失败：") + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            if (errors) errors->Release();
            vsb->Release();
            return false;
        }
        if (errors) errors->Release();
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
        pixel_shader_ = make(psb, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", 0);
        vsb->Release();
        psb->Release();

        ID3DBlob* pst = nullptr;
        if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "d3d9_gpu.hlsl", nullptr, nullptr, "PSMainTex",
                              "ps_5_1", flags, 0, &pst, &errors))) {
            if (err) *err = std::string("D3DCompile(PS texture) 失败：") +
                            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            if (errors) errors->Release();
            return false;
        }
        if (errors) errors->Release();
        pixel_shader_tex_ = make(pst, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMainTex", 1);
        pst->Release();
        if (vertex_shader_ == nullptr || pixel_shader_ == nullptr || pixel_shader_tex_ == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUShader(DXBC) 失败：") + SDL_GetError();
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
        pixel_shader_tex_ = make("PSMainTex", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
        if (vertex_shader_ == nullptr || pixel_shader_ == nullptr || pixel_shader_tex_ == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUShader(MSL) 失败：") + SDL_GetError();
            return false;
        }
        return true;
    }
#else
    bool createShaders(std::string*) { return false; }
#endif

    bool createSampler(std::string* err) {
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = SDL_GPU_FILTER_NEAREST;
        si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_ = SDL_CreateGPUSampler(device_, &si);
        if (sampler_ == nullptr) {
            if (err) *err = std::string("SDL_CreateGPUSampler 失败：") + SDL_GetError();
            return false;
        }
        return true;
    }

    void shutdown() {
        if (device_ == nullptr) return;
        destroyTargets();
        if (vertex_buffer_ != nullptr) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
        vertex_buffer_ = nullptr;
        if (sprite_texture_ != nullptr) SDL_ReleaseGPUTexture(device_, sprite_texture_);
        sprite_texture_ = nullptr;
        if (sampler_ != nullptr) SDL_ReleaseGPUSampler(device_, sampler_);
        sampler_ = nullptr;
        if (vertex_shader_ != nullptr) SDL_ReleaseGPUShader(device_, vertex_shader_);
        if (pixel_shader_ != nullptr) SDL_ReleaseGPUShader(device_, pixel_shader_);
        if (pixel_shader_tex_ != nullptr) SDL_ReleaseGPUShader(device_, pixel_shader_tex_);
        vertex_shader_ = pixel_shader_ = pixel_shader_tex_ = nullptr;
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUShader* vertex_shader_ = nullptr;
    SDL_GPUShader* pixel_shader_ = nullptr;
    SDL_GPUShader* pixel_shader_tex_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* sprite_texture_ = nullptr;
    const void* sprite_texture_key_ = nullptr;
    int sprite_texture_w_ = 0;
    int sprite_texture_h_ = 0;
    SDL_GPUTexture* target_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    SDL_GPUBuffer* vertex_buffer_ = nullptr;
    Uint32 vertex_capacity_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool alpha_ = false;
    bool need_depth_clear_ = true;
    bool dirty_ = false;
    std::string name_;
    std::map<std::uint32_t, SDL_GPUGraphicsPipeline*> pipelines_;
};

}  // namespace

std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err) {
    auto r = std::make_unique<SdlGpuRaster>();
    if (!r->init(err)) return nullptr;
    return r;
}

}  // namespace lift
