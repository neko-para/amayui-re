// emulator-cxx —— 原生宿主 POC。
//
// 目标：窗口 + 自有 surface + 显式 present，不经过 Electron/Chromium。
//
// 模式：
//   --smoke   隐藏窗口；离屏 clear → 纹理四边形（alpha / reverse-subtract）→ 读回断言；
//   默认      可见窗口，每帧 clear → submit → present。
//
// 依赖：vcpkg manifest 的 sdl3；--smoke 的四边形需要 shader feature
//（SDL_shadercross，运行时把 HLSL 编成当前后端格式）。

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

#include "core/agf.h"
#include "core/config.h"
#include "core/encoding.h"
#include "core/install.h"
#include "core/opcode_table.h"
#include "core/script.h"
#include "core/scene_raster.h"
#include "core/vm.h"

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <bit>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

constexpr SDL_FColor kClearColor{0.039f, 0.051f, 0.086f, 1.0f};

// 2x2 棋盘：四个像素都取同一个半透明色，便于断言。
constexpr Uint8 kTexR = 20;
constexpr Uint8 kTexG = 10;
constexpr Uint8 kTexB = 5;
constexpr Uint8 kTexA = 128;

void log_sdl_error(const char* what) {
    std::fprintf(stderr, "[emulator-cxx] %s: %s\n", what, SDL_GetError());
}

SDL_GPUDevice* create_gpu_device() {
    const SDL_GPUShaderFormat formats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, true, nullptr);
    if (device == nullptr) log_sdl_error("SDL_CreateGPUDevice");
    return device;
}

struct SmokeResult {
    bool ok = false;
    std::string detail;
};

// ---------------------------------------------------------------------------
// 读回
// ---------------------------------------------------------------------------

struct Readback {
    SDL_GPUTransferBuffer* buffer = nullptr;
    void* mapped = nullptr;
};

Readback readback_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture, int width, int height) {
    Readback result;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = static_cast<Uint32>(width * height * 4);
    result.buffer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (result.buffer == nullptr) {
        log_sdl_error("SDL_CreateGPUTransferBuffer");
        return result;
    }

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (command == nullptr) {
        log_sdl_error("SDL_AcquireGPUCommandBuffer");
        return result;
    }

    SDL_GPUTextureRegion source{};
    source.texture = texture;
    source.w = static_cast<Uint32>(width);
    source.h = static_cast<Uint32>(height);
    source.d = 1;

    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = result.buffer;
    destination.pixels_per_row = static_cast<Uint32>(width);
    destination.rows_per_layer = static_cast<Uint32>(height);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence == nullptr) {
        log_sdl_error("SDL_SubmitGPUCommandBufferAndAcquireFence");
        return result;
    }
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);

    result.mapped = SDL_MapGPUTransferBuffer(device, result.buffer, false);
    return result;
}

void release_readback(SDL_GPUDevice* device, Readback& readback) {
    if (readback.mapped != nullptr) SDL_UnmapGPUTransferBuffer(device, readback.buffer);
    if (readback.buffer != nullptr) SDL_ReleaseGPUTransferBuffer(device, readback.buffer);
    readback = {};
}

std::array<Uint8, 4> center_pixel(const Readback& readback) {
    const auto* pixels = static_cast<const Uint8*>(readback.mapped);
    const int offset = ((kHeight / 2) * kWidth + (kWidth / 2)) * 4;
    return {pixels[offset + 0], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

// ---------------------------------------------------------------------------
// 渲染资源
// ---------------------------------------------------------------------------

struct QuadPipeline {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTexture* texture = nullptr;
};

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS

constexpr const char* kVertexShader = R"(
struct VSInput {
    float2 position : TEXCOORD0;
    float4 color : TEXCOORD1;
};
struct VSOutput {
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position.x / 640.0 - 1.0,
                             1.0 - input.position.y / 360.0,
                             0.0, 1.0);
    output.color = input.color;
    return output;
}
)";

constexpr const char* kFragmentShader = R"(
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);
float4 PSMain(float4 position : SV_Position, float4 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(samp, uv.xy);
}
)";

SDL_GPUShader* compile_shader(
    SDL_GPUDevice* device,
    const char* source,
    const char* entrypoint,
    SDL_ShaderCross_ShaderStage stage,
    Uint32 num_samplers) {
    SDL_ShaderCross_HLSL_Info hlsl_info{};
    hlsl_info.source = source;
    hlsl_info.entrypoint = entrypoint;
    hlsl_info.include_dir = nullptr;
    hlsl_info.defines = nullptr;
    hlsl_info.shader_stage = stage;
    hlsl_info.props = 0;

    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

    // D3D12：直接 DXIL，避免 SPIRV 中转后的 root signature 绑定差异。
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        size_t dxil_size = 0;
        void* dxil = SDL_ShaderCross_CompileDXILFromHLSL(&hlsl_info, &dxil_size);
        if (dxil == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: DXIL 编译失败: %s\n", SDL_GetError());
            return nullptr;
        }
        SDL_GPUShaderCreateInfo create_info{};
        create_info.code_size = dxil_size;
        create_info.code = static_cast<const Uint8*>(dxil);
        create_info.entrypoint = entrypoint;
        create_info.format = SDL_GPU_SHADERFORMAT_DXIL;
        create_info.stage = stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX
            ? SDL_GPU_SHADERSTAGE_VERTEX
            : SDL_GPU_SHADERSTAGE_FRAGMENT;
        create_info.num_samplers = num_samplers;
        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &create_info);
        SDL_free(dxil);
        if (shader == nullptr) {
            std::fprintf(stderr, "[emulator-cxx] shadercross: DXIL shader 创建失败: %s\n", SDL_GetError());
        }
        return shader;
    }

    size_t spirv_size = 0;
    void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size);
    if (spirv == nullptr) {
        std::fprintf(stderr, "[emulator-cxx] shadercross: SPIRV 编译失败: %s\n", SDL_GetError());
        return nullptr;
    }

    auto* metadata = SDL_ShaderCross_ReflectGraphicsSPIRV(
        static_cast<const Uint8*>(spirv), spirv_size, 0);
    if (metadata == nullptr) {
        std::fprintf(stderr, "[emulator-cxx] shadercross: 反射失败: %s\n", SDL_GetError());
        SDL_free(spirv);
        return nullptr;
    }

    SDL_ShaderCross_SPIRV_Info info{
        static_cast<const Uint8*>(spirv),
        spirv_size,
        entrypoint,
        stage,
        0,
    };
    SDL_GPUShader* shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
        device, &info, &metadata->resource_info, 0);
    SDL_free(metadata);
    SDL_free(spirv);
    if (shader == nullptr) {
        std::fprintf(stderr, "[emulator-cxx] shadercross: GPU shader 创建失败: %s\n", SDL_GetError());
    }
    return shader;
}
bool create_quad_pipeline(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat target_format,
    SDL_GPUBlendOp color_op,
    SDL_GPUBlendFactor src_factor,
    SDL_GPUBlendFactor dst_factor,
    QuadPipeline& out) {
    SDL_GPUShader* vertex = compile_shader(
        device, kVertexShader, "VSMain", SDL_SHADERCROSS_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = compile_shader(
        device, kFragmentShader, "PSMain", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, 1);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) SDL_ReleaseGPUShader(device, vertex);
        if (fragment != nullptr) SDL_ReleaseGPUShader(device, fragment);
        return false;
    }

    SDL_GPUVertexBufferDescription buffer_description{};
    buffer_description.slot = 0;
    buffer_description.pitch = sizeof(float) * 6;
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[2]{};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset = sizeof(float) * 2;

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.vertex_buffer_descriptions = &buffer_description;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attributes;
    vertex_input.num_vertex_attributes = 2;

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = true;
    blend.src_color_blendfactor = src_factor;
    blend.dst_color_blendfactor = dst_factor;
    blend.color_blend_op = color_op;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription target{};
    target.format = target_format;
    target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.vertex_shader = vertex;
    pipeline_info.fragment_shader = fragment;
    pipeline_info.vertex_input_state = vertex_input;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipeline_info.target_info.color_target_descriptions = &target;
    pipeline_info.target_info.num_color_targets = 1;

    out.pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (out.pipeline == nullptr) {
        log_sdl_error("SDL_CreateGPUGraphicsPipeline");
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    out.sampler = SDL_CreateGPUSampler(device, &sampler_info);
    if (out.sampler == nullptr) {
        log_sdl_error("SDL_CreateGPUSampler");
        return false;
    }

    const std::array<float, 36> vertices{
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, static_cast<float>(kHeight), 0.0f, 1.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
        static_cast<float>(kWidth), static_cast<float>(kHeight), 1.0f, 1.0f, 0.0f, 1.0f,
        0.0f, static_cast<float>(kHeight), 0.0f, 1.0f, 0.0f, 1.0f,
    };

    SDL_GPUBufferCreateInfo buffer_info{};
    buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(float));
    out.vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
    if (out.vertex_buffer == nullptr) {
        log_sdl_error("SDL_CreateGPUBuffer");
        return false;
    }

    SDL_GPUTransferBufferCreateInfo upload_info{};
    upload_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    upload_info.size = buffer_info.size;
    SDL_GPUTransferBuffer* upload = SDL_CreateGPUTransferBuffer(device, &upload_info);
    if (upload == nullptr) {
        log_sdl_error("SDL_CreateGPUTransferBuffer(upload)");
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(float));
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTransferBufferLocation location{upload, 0};
    SDL_GPUBufferRegion region{out.vertex_buffer, 0, buffer_info.size};
    SDL_UploadToGPUBuffer(copy, &location, &region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, upload);

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = 2;
    texture_info.height = 2;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    out.texture = SDL_CreateGPUTexture(device, &texture_info);
    if (out.texture == nullptr) {
        log_sdl_error("SDL_CreateGPUTexture(2x2)");
        return false;
    }
    const std::array<Uint8, 16> texels{
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
    };
    SDL_GPUTransferBufferCreateInfo tex_upload_info{};
    tex_upload_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tex_upload_info.size = static_cast<Uint32>(texels.size());
    SDL_GPUTransferBuffer* tex_upload = SDL_CreateGPUTransferBuffer(device, &tex_upload_info);
    if (tex_upload == nullptr) {
        log_sdl_error("SDL_CreateGPUTransferBuffer(texture)");
        return false;
    }
    void* tex_mapped = SDL_MapGPUTransferBuffer(device, tex_upload, false);
    std::memcpy(tex_mapped, texels.data(), texels.size());
    SDL_UnmapGPUTransferBuffer(device, tex_upload);
    command = SDL_AcquireGPUCommandBuffer(device);
    copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo tex_source{};
    tex_source.transfer_buffer = tex_upload;
    tex_source.pixels_per_row = 2;
    tex_source.rows_per_layer = 2;
    SDL_GPUTextureRegion tex_region{};
    tex_region.texture = out.texture;
    tex_region.w = 2;
    tex_region.h = 2;
    tex_region.d = 1;
    SDL_UploadToGPUTexture(copy, &tex_source, &tex_region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, tex_upload);

    return true;
}

void destroy_quad_pipeline(SDL_GPUDevice* device, QuadPipeline& pipeline) {
    if (pipeline.vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, pipeline.vertex_buffer);
    if (pipeline.texture != nullptr) SDL_ReleaseGPUTexture(device, pipeline.texture);
    if (pipeline.sampler != nullptr) SDL_ReleaseGPUSampler(device, pipeline.sampler);
    if (pipeline.pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
    pipeline = {};
}

bool draw_quad(SDL_GPUCommandBuffer* command, SDL_GPUTexture* target, const QuadPipeline& pipeline) {
    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = target;
    target_info.clear_color = kClearColor;
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
    if (pass == nullptr) {
        log_sdl_error("SDL_BeginGPURenderPass");
        return false;
    }

    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{0, 0, kWidth, kHeight};
    SDL_SetGPUScissor(pass, &scissor);

    SDL_GPUTextureSamplerBinding sampler_binding{pipeline.texture, pipeline.sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
    SDL_GPUBufferBinding binding{pipeline.vertex_buffer, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_BindGPUGraphicsPipeline(pass, pipeline.pipeline);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return true;
}

// ---------------------------------------------------------------------------
// 场景 GPU 渲染（绘制项 / 4 顶点网格）
// ---------------------------------------------------------------------------
SDL_GPUTexture* upload_texture(SDL_GPUDevice* device, const core::Bytes& rgba, int width, int height);
// ★ 这是呈现路径；软件 `rasterize_scene` 只作为命令档 / 无后端导出用的 oracle。
struct SceneVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct SceneRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t texture = 0;  // 0 = 内置白纹理（网格）
};

struct ScenePipeline {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;
};

constexpr const char* kSceneVertexShader = R"(
struct VSInput {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

constexpr const char* kSceneFragmentShader = R"(
Texture2D tex : register(t0, space2);
SamplerState samp : register(s0, space2);
float4 PSMain(float2 uv : TEXCOORD0, float4 color : TEXCOORD1) : SV_Target {
    return tex.Sample(samp, uv) * color;
}
)";

bool create_scene_pipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat format, ScenePipeline& out) {
    SDL_GPUShader* vertex = compile_shader(
        device, kSceneVertexShader, "VSMain", SDL_SHADERCROSS_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragment = compile_shader(
        device, kSceneFragmentShader, "PSMain", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, 1);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) SDL_ReleaseGPUShader(device, vertex);
        if (fragment != nullptr) SDL_ReleaseGPUShader(device, fragment);
        return false;
    }

    SDL_GPUVertexBufferDescription buffer_description{};
    buffer_description.slot = 0;
    buffer_description.pitch = sizeof(SceneVertex);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[3]{};
    attributes[0].location = 0;
    attributes[0].buffer_slot = 0;
    attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].buffer_slot = 0;
    attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attributes[1].offset = sizeof(float) * 2;
    attributes[2].location = 2;
    attributes[2].buffer_slot = 0;
    attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[2].offset = sizeof(float) * 4;

    SDL_GPUVertexInputState vertex_input{};
    vertex_input.vertex_buffer_descriptions = &buffer_description;
    vertex_input.num_vertex_buffers = 1;
    vertex_input.vertex_attributes = attributes;
    vertex_input.num_vertex_attributes = 3;

    SDL_GPUColorTargetBlendState blend{};
    blend.enable_blend = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUColorTargetDescription target{};
    target.format = format;
    target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state = vertex_input;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    out.pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    SDL_ReleaseGPUShader(device, vertex);
    SDL_ReleaseGPUShader(device, fragment);
    if (out.pipeline == nullptr) {
        log_sdl_error("SDL_CreateGPUGraphicsPipeline(scene)");
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    out.sampler = SDL_CreateGPUSampler(device, &sampler_info);
    return out.sampler != nullptr;
}

void destroy_scene_pipeline(SDL_GPUDevice* device, ScenePipeline& pipeline) {
    if (pipeline.sampler != nullptr) SDL_ReleaseGPUSampler(device, pipeline.sampler);
    if (pipeline.pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(device, pipeline.pipeline);
    pipeline = {};
}

struct SceneTextureCache {
    std::unordered_map<std::uint32_t, SDL_GPUTexture*> textures;
    std::unordered_map<std::uint32_t, std::uint32_t> revisions;
    SDL_GPUTexture* white = nullptr;
};

void release_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache) {
    for (auto& [id, texture] : cache.textures) {
        (void)id;
        if (texture != nullptr) SDL_ReleaseGPUTexture(device, texture);
    }
    if (cache.white != nullptr) SDL_ReleaseGPUTexture(device, cache.white);
    cache.textures.clear();
    cache.revisions.clear();
    cache.white = nullptr;
}

SDL_GPUTexture* scene_texture(SDL_GPUDevice* device, SceneTextureCache& cache, const core::Machine& machine,
                              std::uint32_t id) {
    if (id == 0) {
        if (cache.white == nullptr) {
            const core::Bytes white{255, 255, 255, 255};
            cache.white = upload_texture(device, white, 1, 1);
        }
        return cache.white;
    }
    const core::TextureRecord* record = machine.texture(id);
    if (record == nullptr || record->rgba.empty()) return nullptr;
    const auto revision_it = cache.revisions.find(id);
    const auto texture_it = cache.textures.find(id);
    if (texture_it != cache.textures.end() && revision_it != cache.revisions.end() &&
        revision_it->second == record->revision) {
        return texture_it->second;
    }
    SDL_GPUTexture* uploaded = upload_texture(device, record->rgba, static_cast<int>(record->width),
                                              static_cast<int>(record->height));
    if (uploaded == nullptr) return nullptr;
    if (texture_it != cache.textures.end() && texture_it->second != nullptr) {
        SDL_ReleaseGPUTexture(device, texture_it->second);
    }
    cache.textures[id] = uploaded;
    cache.revisions[id] = record->revision;
    return uploaded;
}

void push_quad(std::vector<SceneVertex>& vertices, std::vector<SceneRange>& ranges,
               float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
               std::uint32_t argb, std::uint32_t texture_id, float width, float height) {
    const float a = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>(argb & 0xFFu) / 255.0f;
    const auto px = [&](float x) { return x / width * 2.0f - 1.0f; };
    const auto py = [&](float y) { return 1.0f - y / height * 2.0f; };
    const auto push = [&](float x, float y, float u, float v) {
        vertices.push_back({px(x), py(y), u, v, r, g, b, a});
    };
    const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
    push(x0, y0, u0, v0);
    push(x1, y0, u1, v0);
    push(x1, y1, u1, v1);
    push(x0, y0, u0, v0);
    push(x1, y1, u1, v1);
    push(x0, y1, u0, v1);
    ranges.push_back({first, 6, texture_id});
}

void build_scene_vertices(const core::Machine& machine, float width, float height,
                          std::vector<SceneVertex>& vertices, std::vector<SceneRange>& ranges) {
    std::vector<std::uint32_t> keys;
    for (const auto& [key, item] : machine.draw_items()) { (void)item; keys.push_back(key); }
    for (const auto& [key, mesh] : machine.meshes()) {
        if (static_cast<std::int32_t>(key) < 0) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b);
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    const std::uint32_t now = machine.scene_clock_ms();
    const bool forced = machine.forced_complete();
    for (const auto key : keys) {
        const auto mesh_it = machine.meshes().find(key);
        if (mesh_it != machine.meshes().end()) {
            const core::MeshRecord& mesh = mesh_it->second;
            if ((mesh.flags & 1u) != 0 && mesh.count == 4 && mesh.copies[0].size() >= 4 &&
                mesh.copies[1].size() >= 4) {
                const auto color = core::derive_color(mesh, now, forced);
                if ((color >> 24) != 0) {
                    const auto fx = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[0][i]); };
                    const auto fy = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[1][i]); };
                    // 两个三角形：0-1-2 / 1-3-2（与软件光栅同一拓扑）。
                    const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
                    const auto push = [&](std::size_t i) {
                        vertices.push_back({fx(i) / width * 2.0f - 1.0f, 1.0f - fy(i) / height * 2.0f,
                                            0.0f, 0.0f,
                                            static_cast<float>((color >> 16) & 0xFFu) / 255.0f,
                                            static_cast<float>((color >> 8) & 0xFFu) / 255.0f,
                                            static_cast<float>(color & 0xFFu) / 255.0f,
                                            static_cast<float>((color >> 24) & 0xFFu) / 255.0f});
                    };
                    push(0); push(1); push(2); push(1); push(3); push(2);
                    ranges.push_back({first, 6, 0});
                }
            }
        }

        const auto item_it = machine.draw_items().find(key);
        if (item_it == machine.draw_items().end()) continue;
        const core::DrawItem& item = item_it->second;
        if ((item.flags & 1u) == 0) continue;
        const core::TextureRecord* texture = machine.texture(item.texture_id);
        if (texture == nullptr || texture->rgba.empty()) continue;
        const int sx0 = std::max(0, item.left);
        const int sy0 = std::max(0, item.top);
        const int sx1 = std::min(static_cast<int>(texture->width), item.right);
        const int sy1 = std::min(static_cast<int>(texture->height), item.bottom);
        if (sx1 <= sx0 || sy1 <= sy0) continue;
        const float dx = item.aux[0] + static_cast<float>(sx0 - item.left);
        const float dy = item.aux[1] + static_cast<float>(sy0 - item.top);
        const auto color = core::derive_color(item, now, forced);
        if ((color >> 24) == 0) continue;
        push_quad(vertices, ranges, dx, dy, dx + static_cast<float>(sx1 - sx0),
                  dy + static_cast<float>(sy1 - sy0),
                  static_cast<float>(sx0) / static_cast<float>(texture->width),
                  static_cast<float>(sy0) / static_cast<float>(texture->height),
                  static_cast<float>(sx1) / static_cast<float>(texture->width),
                  static_cast<float>(sy1) / static_cast<float>(texture->height),
                  color, item.texture_id, width, height);
    }
}

bool draw_scene_gpu(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command, SDL_GPUTexture* target,
                    const core::Machine& machine, const ScenePipeline& pipeline,
                    SceneTextureCache& cache, int width, int height, bool clear) {
    std::vector<SceneVertex> vertices;
    std::vector<SceneRange> ranges;
    build_scene_vertices(machine, static_cast<float>(width), static_cast<float>(height), vertices,
                         ranges);

    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTransferBuffer* transfer = nullptr;
    if (!vertices.empty()) {
        SDL_GPUBufferCreateInfo buffer_info{};
        buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneVertex));
        vertex_buffer = SDL_CreateGPUBuffer(device, &buffer_info);
        SDL_GPUTransferBufferCreateInfo transfer_info{};
        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneVertex));
        transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
        if (vertex_buffer == nullptr || transfer == nullptr) {
            if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
            if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUBuffer(device, vertex_buffer);
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            return false;
        }
        std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(SceneVertex));
        SDL_UnmapGPUTransferBuffer(device, transfer);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
        SDL_GPUTransferBufferLocation source{transfer, 0};
        SDL_GPUBufferRegion destination{vertex_buffer, 0, static_cast<Uint32>(vertices.size() * sizeof(SceneVertex))};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        SDL_EndGPUCopyPass(copy);
    }

    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = target;
    target_info.clear_color = kClearColor;
    target_info.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
    if (pass == nullptr) {
        if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
        if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
        return false;
    }
    SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_Rect scissor{0, 0, width, height};
    SDL_SetGPUScissor(pass, &scissor);
    SDL_BindGPUGraphicsPipeline(pass, pipeline.pipeline);
    if (vertex_buffer != nullptr) {
        SDL_GPUBufferBinding binding{vertex_buffer, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        for (const SceneRange& range : ranges) {
            SDL_GPUTexture* texture = scene_texture(device, cache, machine, range.texture);
            if (texture == nullptr) continue;
            SDL_GPUTextureSamplerBinding sampler_binding{texture, pipeline.sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler_binding, 1);
            SDL_DrawGPUPrimitives(pass, range.count, 1, range.first, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
    if (vertex_buffer != nullptr) SDL_ReleaseGPUBuffer(device, vertex_buffer);
    if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(device, transfer);
    return true;
}

void preload_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache,
                            const core::Machine& machine) {
    for (const auto& [key, item] : machine.draw_items()) {
        (void)key;
        if ((item.flags & 1u) != 0) scene_texture(device, cache, machine, item.texture_id);
    }
}

bool render_scene_gpu_to_bmp(SDL_GPUDevice* device, const core::Machine& machine,
                             const std::string& path, int width, int height) {
    ScenePipeline pipeline;
    if (!create_scene_pipeline(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, pipeline)) return false;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &info);
    if (target == nullptr) {
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SceneTextureCache cache;
    preload_scene_textures(device, cache, machine);
    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (command == nullptr) {
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    if (!draw_scene_gpu(device, command, target, machine, pipeline, cache, width, height, true)) {
        SDL_CancelGPUCommandBuffer(command);
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    if (fence == nullptr) {
        release_scene_textures(device, cache);
        SDL_ReleaseGPUTexture(device, target);
        destroy_scene_pipeline(device, pipeline);
        return false;
    }
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    Readback readback = readback_texture(device, target, width, height);
    bool ok = false;
    if (readback.mapped != nullptr) {
        core::Bytes canvas(static_cast<std::size_t>(width) * height * 4);
        std::memcpy(canvas.data(), readback.mapped, canvas.size());
        core::save_bmp_rgba(path, canvas, static_cast<std::uint32_t>(width),
                            static_cast<std::uint32_t>(height));
        ok = true;
    }
    release_readback(device, readback);
    release_scene_textures(device, cache);
    SDL_ReleaseGPUTexture(device, target);
    destroy_scene_pipeline(device, pipeline);
    return ok;
}


#endif  // EMULATOR_CXX_HAVE_SHADERCROSS

// ---------------------------------------------------------------------------
// smoke
// ---------------------------------------------------------------------------

SmokeResult run_smoke_clear(SDL_GPUDevice* device) {
    SmokeResult result;

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = kWidth;
    texture_info.height = kHeight;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &texture_info);
    if (target == nullptr) {
        log_sdl_error("SDL_CreateGPUTexture");
        result.detail = "create target texture failed";
        return result;
    }

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = target;
    target_info.clear_color = kClearColor;
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
    SDL_EndGPURenderPass(pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);

    Readback readback = readback_texture(device, target, kWidth, kHeight);
    if (readback.mapped == nullptr) {
        SDL_ReleaseGPUTexture(device, target);
        result.detail = "readback failed";
        return result;
    }

    const auto pixel = center_pixel(readback);
    const int expected[3] = {10, 13, 22};
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        if (pixel[i] < expected[i] - 2 || pixel[i] > expected[i] + 2) ok = false;
    }

    char buffer[128];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "clear center=rgba(%u,%u,%u,%u) expected≈(%d,%d,%d)",
        pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1], expected[2]);
    result.ok = ok;
    result.detail = buffer;

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        kWidth, kHeight, SDL_PIXELFORMAT_RGBA32, readback.mapped, kWidth * 4);
    if (surface != nullptr) {
        SDL_SaveBMP(surface, "emulator-cxx-smoke.bmp");
        SDL_DestroySurface(surface);
    }

    release_readback(device, readback);
    SDL_ReleaseGPUTexture(device, target);
    return result;
}

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS

[[maybe_unused]] SmokeResult run_smoke_quad(
    SDL_GPUDevice* device,
    SDL_GPUBlendOp color_op,
    SDL_GPUBlendFactor src_factor,
    SDL_GPUBlendFactor dst_factor,
    const char* label,
    const int expected[3]) {
    SmokeResult result;

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = kWidth;
    texture_info.height = kHeight;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &texture_info);
    if (target == nullptr) {
        log_sdl_error("SDL_CreateGPUTexture");
        result.detail = "create target texture failed";
        return result;
    }

    QuadPipeline pipeline;
    if (!create_quad_pipeline(device, texture_info.format, color_op, src_factor, dst_factor, pipeline)) {
        SDL_ReleaseGPUTexture(device, target);
        result.detail = "pipeline creation failed";
        return result;
    }

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    if (!draw_quad(command, target, pipeline)) {
        SDL_CancelGPUCommandBuffer(command);
        destroy_quad_pipeline(device, pipeline);
        SDL_ReleaseGPUTexture(device, target);
        result.detail = "draw failed";
        return result;
    }

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);

    Readback readback = readback_texture(device, target, kWidth, kHeight);
    if (readback.mapped == nullptr) {
        destroy_quad_pipeline(device, pipeline);
        SDL_ReleaseGPUTexture(device, target);
        result.detail = "readback failed";
        return result;
    }

    const auto pixel = center_pixel(readback);
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        if (pixel[i] < expected[i] - 2 || pixel[i] > expected[i] + 2) ok = false;
    }

    char buffer[160];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s center=rgba(%u,%u,%u,%u) expected≈(%d,%d,%d)",
        label, pixel[0], pixel[1], pixel[2], pixel[3], expected[0], expected[1], expected[2]);
    result.ok = ok;
    result.detail = buffer;

    release_readback(device, readback);
    destroy_quad_pipeline(device, pipeline);
    SDL_ReleaseGPUTexture(device, target);
    return result;
}

#endif

SmokeResult run_smoke_texture(SDL_GPUDevice* device) {
    SmokeResult result;

    SDL_GPUTextureCreateInfo texture_info{};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = 2;
    texture_info.height = 2;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &texture_info);
    if (texture == nullptr) {
        log_sdl_error("SDL_CreateGPUTexture(roundtrip)");
        result.detail = "create 2x2 texture failed";
        return result;
    }

    const std::array<Uint8, 16> texels{
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
        kTexR, kTexG, kTexB, kTexA, kTexR, kTexG, kTexB, kTexA,
    };
    SDL_GPUTransferBufferCreateInfo upload_info{};
    upload_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    upload_info.size = static_cast<Uint32>(texels.size());
    SDL_GPUTransferBuffer* upload = SDL_CreateGPUTransferBuffer(device, &upload_info);
    void* mapped = SDL_MapGPUTransferBuffer(device, upload, false);
    std::memcpy(mapped, texels.data(), texels.size());
    SDL_UnmapGPUTransferBuffer(device, upload);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = upload;
    source.pixels_per_row = 2;
    source.rows_per_layer = 2;
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = 2;
    region.h = 2;
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, upload);

    Readback readback = readback_texture(device, texture, 2, 2);
    if (readback.mapped == nullptr) {
        SDL_ReleaseGPUTexture(device, texture);
        result.detail = "texture readback failed";
        return result;
    }

    const auto* pixels = static_cast<const Uint8*>(readback.mapped);
    const bool ok = pixels[0] == kTexR && pixels[1] == kTexG && pixels[2] == kTexB && pixels[3] == kTexA;
    char buffer[128];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "texture roundtrip=rgba(%u,%u,%u,%u) expected=(%u,%u,%u,%u)",
        pixels[0], pixels[1], pixels[2], pixels[3], kTexR, kTexG, kTexB, kTexA);
    result.ok = ok;
    result.detail = buffer;

    release_readback(device, readback);
    SDL_ReleaseGPUTexture(device, texture);
    return result;
}

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
SmokeResult run_smoke_shader(SDL_GPUDevice* device) {
    SmokeResult result;
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

    SDL_ShaderCross_HLSL_Info info{};
    info.include_dir = nullptr;
    info.defines = nullptr;
    info.props = 0;

    size_t vertex_size = 0;
    size_t fragment_size = 0;
    void* vertex_code = nullptr;
    void* fragment_code = nullptr;
    SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;

    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        info.source = kVertexShader;
        info.entrypoint = "VSMain";
        info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
        vertex_code = SDL_ShaderCross_CompileDXILFromHLSL(&info, &vertex_size);
        info.source = kFragmentShader;
        info.entrypoint = "PSMain";
        info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        fragment_code = SDL_ShaderCross_CompileDXILFromHLSL(&info, &fragment_size);
        format = SDL_GPU_SHADERFORMAT_DXIL;
    } else if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        info.source = kVertexShader;
        info.entrypoint = "VSMain";
        info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
        vertex_code = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &vertex_size);
        info.source = kFragmentShader;
        info.entrypoint = "PSMain";
        info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
        fragment_code = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &fragment_size);
        format = SDL_GPU_SHADERFORMAT_SPIRV;
    }

    if (vertex_code == nullptr || fragment_code == nullptr) {
        result.detail = std::string("shader compile failed: ") + SDL_GetError();
        if (vertex_code != nullptr) SDL_free(vertex_code);
        if (fragment_code != nullptr) SDL_free(fragment_code);
        return result;
    }

    SDL_GPUShaderCreateInfo vertex_info{};
    vertex_info.code_size = vertex_size;
    vertex_info.code = static_cast<const Uint8*>(vertex_code);
    vertex_info.entrypoint = "VSMain";
    vertex_info.format = format;
    vertex_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertex_info.num_samplers = 0;
    SDL_GPUShader* vertex = SDL_CreateGPUShader(device, &vertex_info);

    SDL_GPUShaderCreateInfo fragment_info{};
    fragment_info.code_size = fragment_size;
    fragment_info.code = static_cast<const Uint8*>(fragment_code);
    fragment_info.entrypoint = "PSMain";
    fragment_info.format = format;
    fragment_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragment_info.num_samplers = 1;
    SDL_GPUShader* fragment = SDL_CreateGPUShader(device, &fragment_info);

    SDL_free(vertex_code);
    SDL_free(fragment_code);

    const bool ok = vertex != nullptr && fragment != nullptr;
    result.ok = ok;
    result.detail = ok
        ? std::string("shader compile=ok format=") + (format == SDL_GPU_SHADERFORMAT_DXIL ? "DXIL" : "SPIRV")
        : std::string("SDL_CreateGPUShader failed: ") + SDL_GetError();

    if (vertex != nullptr) SDL_ReleaseGPUShader(device, vertex);
    if (fragment != nullptr) SDL_ReleaseGPUShader(device, fragment);
    return result;
}
#endif

int run_windowed_image(SDL_GPUDevice* device, SDL_Window* window, const core::Bytes& rgba,
                       int width, int height);

int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames);

int run_core_agf(const std::string& agf_path) {
    try {
        std::ifstream input(core::path_from_utf8(agf_path), std::ios::binary);
        if (!input) throw core::CoreError("agf-open-failed:" + agf_path);
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        input.seekg(0, std::ios::beg);
        core::Bytes bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw core::CoreError("agf-read-failed:" + agf_path);
        const core::DecodedAgf image = core::decode_agf(bytes);
        std::printf(
            "[core] agf %s: %ux%u bpp=%u rgba=%zu first=%u,%u,%u,%u\n",
            agf_path.c_str(),
            image.width,
            image.height,
            image.bpp,
            image.rgba.size(),
            image.rgba[0],
            image.rgba[1],
            image.rgba[2],
            image.rgba[3]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[core] AGF FAIL: %s\n", e.what());
        return 1;
    }
}

int run_core_install(const std::string& install_dir, const std::string& table_path,
                     const std::string& config_path, const std::string& config_keys_path,
                     std::size_t max_steps, std::uint32_t stop_at, bool to_title,
                     bool render_title, bool show_title, bool play_title, int render_frames,
                     int trace_steps, int play_frames, bool play_boot, bool gpu_title,
                     const std::string& logo_dir, int logo_stride, int logo_frames) {
    try {
        const core::Install install = core::open_install(core::path_from_utf8(install_dir));
        const auto& index = install.index;
        std::printf(
            "[core] alf signature=%s title=%s zone=%d archives=%zu files=%zu configDefaults=%zu\n",
            index.signature.c_str(),
            core::cp932_to_utf8(index.title).c_str(),
            index.zone,
            index.archives.size(),
            index.files.size(),
            index.config_defaults.size());

        if (index.files.empty()) {
            std::fprintf(stderr, "[core] no files in TOC\n");
            return 1;
        }
        const auto& root_record = index.files[0];
        const core::Bytes root = core::read_install_file(install, 0);
        std::printf(
            "[core] root id=0 name=%s archive=%u length=%u bytes=%zu\n",
            core::cp932_to_utf8(root_record.name).c_str(),            root_record.archive_index,
            root_record.length,
            root.size());

        const core::ScriptHeader header = core::parse_script_header(root);
        const core::OpcodeTable table = core::OpcodeTable::load_from_file(core::path_from_utf8(table_path));
        core::ConfigVocabulary vocabulary = core::ConfigVocabulary::load_from_file(core::path_from_utf8(config_keys_path));
        core::ConfigStore config(std::move(vocabulary));
        config.load_defaults(index.config_defaults);
        std::filesystem::path config_root = install.root;
        if (config.get_int("set:UseAppDataFolder") != 0) {
#ifdef _WIN32
            wchar_t appdata[32768]{};
            const DWORD appdata_length =
                GetEnvironmentVariableW(L"APPDATA", appdata, static_cast<DWORD>(32768));
            if (appdata_length > 0 && appdata_length < 32768) {
                config_root = std::filesystem::path(appdata);
            }
#else
            if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr) {
                config_root = core::path_from_utf8(xdg);
            } else if (const char* home = std::getenv("HOME"); home != nullptr) {
                config_root = core::path_from_utf8(home) / ".config";
            }
#endif
        }
        std::filesystem::path player_path = config_path.empty()
            ? config_root / core::path_from_engine(config.get_string("set:RegFilePath")) / "SYS4REG.INI"
            : core::path_from_utf8(config_path);
        config.load_player_file(player_path);
        std::printf("[core] drawMode=%d named=%d\n",
                    config.get_int("set:DrawMode"),
                    config.has_named("set:DrawMode") ? 1 : 0);
        std::printf(
            "[core] config named=%zu script=%zu player=%s\n",
            config.named_size(),
            config.script_size(),
            core::path_to_utf8(player_path).c_str());
        std::string error;
        const auto instructions = core::decode_instructions(
            std::span<const std::uint8_t>(root).subspan(header.body_offset),
            table,
            200000,
            &error);
        std::printf(
            "[core] script magic=%s version=%s header=%zu body=%zu instructions=%zu table=%zu\n",
            header.magic.c_str(),
            header.version.c_str(),
            header.header_size,
            root.size() - header.body_offset,
            instructions.size(),
            table.size());
        if (!error.empty()) std::printf("[core] decode-stop=%s\n", error.c_str());

        std::random_device random;
        std::uint32_t key = random();
        if (key == 0) key = 1;
        core::Machine machine(key, table, &install, config);
        machine.load_root(root, 0);
        machine.stop_at_opcode(stop_at);
        if (play_boot) {
            // 从根脚本开始逐帧呈现：颜色窗用宿主真实时钟推进，能看到 LOGO 之后的渐变过程。
            machine.reset_scene_clock(1000);
            machine.set_host_clock_ms(1000);
            if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(play-boot)"); return 1; }
            SDL_GPUDevice* device = create_gpu_device();
            SDL_Window* window = SDL_CreateWindow("emulator-cxx — boot", 1280, 720, SDL_WINDOW_RESIZABLE);
            if (device != nullptr && window != nullptr) {
                run_windowed_frames(device, window, machine, 1280, 720, play_frames);
            }
            if (window != nullptr) SDL_DestroyWindow(window);
            if (device != nullptr) SDL_DestroyGPUDevice(device);
            SDL_Quit();
            return 0;
        }
        // 无窗口影片校验：从根脚本跑到 LOGO 的 `0x20f`，以后端时间推进，逐帧导出 GPU 合成。
        // `--logo-frames 0` = 不导出、只跑到影片收尾（CTest 用的确定性判据）。
        if (!logo_dir.empty()) {
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
            std::filesystem::create_directories(core::path_from_utf8(logo_dir));
            std::size_t steps = 0;
            while (!machine.halted() && !machine.movie_active() && steps < max_steps) {
                if (!machine.step()) break;
                ++steps;
            }
            std::printf("[movie] start steps=%zu clock=%ums script=%u active=%d\n", steps,
                        machine.scene_clock_ms(), machine.frame().script_id,
                        machine.movie_active() ? 1 : 0);
            if (!machine.movie_active()) {
                std::fprintf(stderr, "[movie] LOGO 影片没有启动（未到 0x20f 或绑定门失败）\n");
                return 1;
            }
            const bool dump = logo_frames > 0;
            bool ok = true;
            SDL_GPUDevice* device = nullptr;
            if (dump) {
                if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(logo-gpu)"); return 1; }
                if (!SDL_ShaderCross_Init()) {
                    log_sdl_error("SDL_ShaderCross_Init");
                    SDL_Quit();
                    return 1;
                }
                device = create_gpu_device();
                if (device == nullptr) { SDL_ShaderCross_Quit(); SDL_Quit(); return 1; }
            }
            int requests = 0;
            int dumped = 0;
            while (ok && !machine.halted() && machine.movie_active()) {
                bool requested = false;
                for (int i = 0; i < 20000; ++i) {
                    if (!machine.step()) break;
                    if (machine.consume_frame_step()) { requested = true; break; }
                    if (!machine.movie_active()) break;
                }
                if (!requested) break;
                const int index = requests++;
                if (!dump) { machine.end_render(); continue; }
                if (index % (logo_stride > 0 ? logo_stride : 1) != 0) {
                    machine.end_render();
                    continue;
                }
                // 采样影片纹理的平均亮度：全黑 / 全同色的回归在这里就能看出来。
                double mean = 0.0;
                if (const core::TextureRecord* texture = machine.texture(42);
                    texture != nullptr && texture->rgba.size() >= 4) {
                    const std::size_t step = (texture->rgba.size() / 4 / 997) * 4;
                    std::uint64_t sum = 0;
                    std::size_t n = 0;
                    for (std::size_t i = 0; i + 3 < texture->rgba.size() && step > 0; i += step) {
                        sum += texture->rgba[i] + texture->rgba[i + 1] + texture->rgba[i + 2];
                        ++n;
                    }
                    mean = n > 0 ? static_cast<double>(sum) / static_cast<double>(n * 3) : 0.0;
                }
                char name[512];
                std::snprintf(name, sizeof(name), "%s/logo-%03d.bmp", logo_dir.c_str(), index);
                if (!render_scene_gpu_to_bmp(device, machine, name, 1280, 720)) {
                    ok = false;
                    break;
                }
                machine.end_render();
                ++dumped;
                std::printf("[movie] frame %d clock=%ums mean=%.1f -> %s\n", index,
                            machine.scene_clock_ms(), mean, name);
                if (dumped >= logo_frames) break;
            }
            std::printf("[movie] logo done requests=%d dumped=%d clock=%ums active=%d\n", requests,
                        dumped, machine.scene_clock_ms(), machine.movie_active() ? 1 : 0);
            if (device != nullptr) SDL_DestroyGPUDevice(device);
            if (dump) { SDL_ShaderCross_Quit(); SDL_Quit(); }
            if (!ok) return 1;
            // 判据：影片真的播了（至少一帧）且走到了收尾（导出模式按帧数截断时不要求）。
            if (requests < 1) { std::fprintf(stderr, "[movie] 一帧都没有请求\n"); return 1; }
            if (!dump && machine.movie_active()) {
                std::fprintf(stderr, "[movie] 影片没有走到收尾\n");
                return 1;
            }
            return 0;
#else
            std::fprintf(stderr, "[movie] --render-logo-gpu requires shadercross\n");
            return 1;
#endif
        }
        // 窗口播放：在 TITLE 刚成为当前帧时就停，交给窗口按真实时钟跑 TITLE 的渐变；
        // 静态导出仍跑到步数上限，保证画面已建好。
        const core::RunResult vm = (play_title && !render_title)
            ? machine.run_until_script(21092, max_steps)
            : machine.run(max_steps);
        const bool title_reached = machine.frame().script_id == 21092;
        std::printf(
            "[core] vm stop=%d steps=%zu frame=%zu script=%u opcode=0x%X offset=%zu message=%s\n",
            static_cast<int>(vm.reason),
            vm.steps,
            machine.current_frame(),
            machine.frame().script_id,
            vm.opcode,
            vm.offset,
            vm.message.c_str());

        std::printf("[core] state drawItems=%zu textures=%zu meshes=%zu\n", machine.draw_item_count(), machine.texture_count(), machine.mesh_count());
        if (render_title) {
            const core::Bytes canvas = core::rasterize_scene(machine, 1280, 720);
            std::size_t visible = 0;
            for (std::size_t i = 3; i < canvas.size(); i += 4) if (canvas[i] != 0) ++visible;
            core::save_bmp_rgba("title-frame.bmp", canvas, 1280, 720);
            std::printf("[render] title-frame 1280x720 visible=%zu -> title-frame.bmp\n", visible);
            if (visible == 0) { std::fprintf(stderr, "[render] empty frame\n"); return 1; }
            if (show_title) {
                if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(show-title)"); return 1; }
                SDL_GPUDevice* device = create_gpu_device();
                SDL_Window* window = SDL_CreateWindow("emulator-cxx — TITLE", 1280, 720, SDL_WINDOW_RESIZABLE);
                if (device != nullptr && window != nullptr) run_windowed_image(device, window, canvas, 1280, 720);
                if (window != nullptr) SDL_DestroyWindow(window);
                if (device != nullptr) SDL_DestroyGPUDevice(device);
                SDL_Quit();
            }
        }
        if (trace_steps > 0) {
            for (int i = 0; i < trace_steps; ++i) {
                const auto& code = machine.frame().code;
                const auto ip = machine.frame().ip;
                if (ip + 4 > code.size()) break;
                std::printf("[trace] #%d ip=%zu opcode=0x%X flags=0x%X\n", i, ip,
                            core::read_u32(code, ip), machine.state_flags());
                if (!machine.step()) break;
            }
        }
        if (render_frames > 0) {
            machine.consume_frame_step();
            // 以「到达 TITLE 时的场景时钟」为基准继续走；归零会让已盖章的窗回到延迟前。
            const std::uint32_t clock_base = machine.scene_clock_ms();
            for (int f = 0; f < render_frames; ++f) {
                // 无窗口导出：每帧显式请求一次渲染步（等价于 0x20C），再推进到请求被消费。
                machine.set_host_clock_ms(clock_base + static_cast<std::uint32_t>(f) * 16u);
                machine.request_render();
                for (int i = 0; i < 20000; ++i) {
                    if (!machine.step()) break;
                    if (machine.consume_frame_step()) break;
                }
                const core::Bytes frame = core::rasterize_scene(machine, 1280, 720);
                std::size_t visible = 0;
                for (std::size_t i = 3; i < frame.size(); i += 4) if (frame[i] != 0) ++visible;
                char name[64];
                std::snprintf(name, sizeof(name), "title-frame-%02d.bmp", f);
                core::save_bmp_rgba(name, frame, 1280, 720);
                std::printf("[render] frame %d clock=%ums visible=%zu -> %s\n", f,
                            machine.scene_clock_ms(), visible, name);
                if (visible == 0) { std::fprintf(stderr, "[render] empty frame %d\n", f); return 1; }
            }
        }
        if (gpu_title) {
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
            if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(gpu-title)"); return 1; }
            if (!SDL_ShaderCross_Init()) { log_sdl_error("SDL_ShaderCross_Init"); SDL_Quit(); return 1; }
            SDL_GPUDevice* device = create_gpu_device();
            bool ok = false;
            if (device != nullptr) {
                ok = render_scene_gpu_to_bmp(device, machine, "title-gpu.bmp", 1280, 720);
                SDL_DestroyGPUDevice(device);
            }
            SDL_ShaderCross_Quit();
            SDL_Quit();
            if (!ok) { std::fprintf(stderr, "[render] GPU TITLE frame failed\n"); return 1; }
            std::printf("[render] GPU TITLE frame -> title-gpu.bmp\n");
#else
            std::fprintf(stderr, "[render] --render-title-gpu requires shadercross\n");
            return 1;
#endif
        }
        if (play_title) {
            if (title_reached) {
                if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(play-title)"); return 1; }
                SDL_GPUDevice* device = create_gpu_device();
                SDL_Window* window = SDL_CreateWindow("emulator-cxx — TITLE (play)", 1280, 720, SDL_WINDOW_RESIZABLE);
                if (device != nullptr && window != nullptr) run_windowed_frames(device, window, machine, 1280, 720, play_frames);
                if (window != nullptr) SDL_DestroyWindow(window);
                if (device != nullptr) SDL_DestroyGPUDevice(device);
                SDL_Quit();
            } else {
                std::fprintf(stderr, "[render] title not reached\n");
                return 1;
            }
        }
        if (to_title) {
            std::printf("[core] title-loop reached=%s\n", title_reached ? "true" : "false");
            if (!title_reached) return 1;
        }

        const std::size_t show = std::min<std::size_t>(instructions.size(), 8);
        for (std::size_t i = 0; i < show; ++i) {
            const auto& ins = instructions[i];
            std::printf(
                "[core]   #%zu off=0x%zX opcode=0x%X argc=%zu\n",
                i,
                ins.offset,
                ins.opcode,
                ins.operands.size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[core] FAIL: %s\n", e.what());
        return 1;
    }
}

SmokeResult run_smoke(SDL_GPUDevice* device) {
    SmokeResult clear = run_smoke_clear(device);
    if (!clear.ok) return clear;

    SmokeResult texture = run_smoke_texture(device);
    if (!texture.ok) return texture;

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    const int alpha_expected[3] = {15, 11, 13};
    SmokeResult alpha = run_smoke_quad(
        device,
        SDL_GPU_BLENDOP_ADD,
        SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        "alpha",
        alpha_expected);
    if (!alpha.ok) return alpha;

    const int subtract_expected[3] = {0, 8, 20};
    SmokeResult subtract = run_smoke_quad(
        device,
        SDL_GPU_BLENDOP_REVERSE_SUBTRACT,
        SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_ONE,
        "reverse-subtract",
        subtract_expected);
    if (!subtract.ok) return subtract;

    SmokeResult shader = run_smoke_shader(device);
    if (!shader.ok) return shader;
    return SmokeResult{true, clear.detail + " | " + texture.detail + " | " + alpha.detail + " | " + subtract.detail + " | " + shader.detail};
#else
    return SmokeResult{true, clear.detail + " | " + texture.detail};
#endif
}

// ---------------------------------------------------------------------------
// 窗口
// ---------------------------------------------------------------------------

int run_windowed(SDL_GPUDevice* device, SDL_Window* window) {
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        log_sdl_error("SDL_ClaimWindowForGPUDevice");
        return 1;
    }

    std::printf(
        "[emulator-cxx] device=%s window=%dx%d\n",
        SDL_GetGPUDeviceDriver(device),
        kWidth,
        kHeight);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
        if (command == nullptr) {
            log_sdl_error("SDL_AcquireGPUCommandBuffer");
            break;
        }

        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(
                command, window, &swapchain, nullptr, nullptr)) {
            log_sdl_error("SDL_WaitAndAcquireGPUSwapchainTexture");
            SDL_CancelGPUCommandBuffer(command);
            break;
        }

        if (swapchain != nullptr) {
            SDL_GPUColorTargetInfo target_info{};
            target_info.texture = swapchain;
            target_info.clear_color = kClearColor;
            target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            target_info.store_op = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass =
                SDL_BeginGPURenderPass(command, &target_info, 1, nullptr);
            SDL_EndGPURenderPass(pass);
        }

        if (!SDL_SubmitGPUCommandBuffer(command)) {
            log_sdl_error("SDL_SubmitGPUCommandBuffer");
            break;
        }
    }

    SDL_ReleaseWindowFromGPUDevice(device, window);
    return 0;
}

}  // namespace

namespace {

SDL_GPUTexture* upload_texture(SDL_GPUDevice* device, const core::Bytes& rgba, int width, int height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (texture == nullptr) return nullptr;

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<Uint32>(rgba.size());
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (transfer == nullptr) return nullptr;
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    std::memcpy(mapped, rgba.data(), rgba.size());
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(command);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.pixels_per_row = static_cast<Uint32>(width);
    source.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion region{};
    region.texture = texture;
    region.w = static_cast<Uint32>(width);
    region.h = static_cast<Uint32>(height);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, true);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(command);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return texture;
}

int run_windowed_image(SDL_GPUDevice* device, SDL_Window* window, const core::Bytes& rgba,
                       int width, int height) {
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        log_sdl_error("SDL_ClaimWindowForGPUDevice");
        return 1;
    }
    QuadPipeline pipeline;
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    std::printf("[render] swapchain format=%d\n", static_cast<int>(swapchain_format));
    if (!create_quad_pipeline(device, swapchain_format,
                              SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                              SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, pipeline)) {
        return 1;
    }
    SDL_GPUTexture* image = upload_texture(device, rgba, width, height);
    if (image == nullptr) {
        destroy_quad_pipeline(device, pipeline);
        return 1;
    }
    SDL_ReleaseGPUTexture(device, pipeline.texture);
    pipeline.texture = image;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;
        }
        SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
        if (command == nullptr) break;
        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &swapchain, nullptr, nullptr)) {
            SDL_CancelGPUCommandBuffer(command);
            break;
        }
        if (swapchain != nullptr) draw_quad(command, swapchain, pipeline);
        if (!SDL_SubmitGPUCommandBuffer(command)) break;
    }
    destroy_quad_pipeline(device, pipeline);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    return 0;
}

// 主循环宿主的呈现：每次迭代（门前）由 core 决定是否请求渲染；有请求才合成 / 上传 / 呈现。
// 静态场景没有新请求时保留最后一帧；`max_frames > 0` 时按帧数强制推进，便于无人值守验证。
int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames) {
    if (!SDL_ClaimWindowForGPUDevice(device, window)) {
        log_sdl_error("SDL_ClaimWindowForGPUDevice");
        return 1;
    }
    QuadPipeline pipeline;
    const SDL_GPUTextureFormat swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, window);
    std::printf("[render] swapchain format=%d\n", static_cast<int>(swapchain_format));
    if (!create_quad_pipeline(device, swapchain_format,
                              SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                              SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, pipeline)) {
        return 1;
    }
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    ScenePipeline scene_pipeline;
    const bool scene_gpu_ok = create_scene_pipeline(device, swapchain_format, scene_pipeline);
    SceneTextureCache scene_cache;
#endif
    machine.consume_frame_step();  // 丢掉预跑期留下的请求
    // 只建一张 1×1 占位纹理给软件回退路径；⛔ 不要在这里 rasterize（那会清脏、跳过首帧）。
    core::Bytes canvas{0, 0, 0, 0};
    SDL_GPUTexture* image = upload_texture(device, canvas, 1, 1);
    if (image == nullptr) {
        destroy_quad_pipeline(device, pipeline);
        return 1;
    }
    SDL_ReleaseGPUTexture(device, pipeline.texture);
    pipeline.texture = image;

    bool running = true;
    std::size_t frame = 0;
    std::size_t max_windows = 0;
    std::size_t idle_batches = 0;
    const std::uint32_t clock_base = machine.scene_clock_ms();
    const Uint64 start_ticks = SDL_GetTicks();
    const auto present = [&]() -> bool {
        SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
        if (command == nullptr) return false;
        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &swapchain, nullptr, nullptr)) {
            SDL_CancelGPUCommandBuffer(command);
            return false;
        }
        if (swapchain != nullptr) draw_quad(command, swapchain, pipeline);
        return SDL_SubmitGPUCommandBuffer(command);
    };
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) running = false;
        }
        if (!running) break;
        if (machine.halted()) {
            // 无人值守（给了帧数）时"脚本结束"就是跑完；交互模式保留最后一帧等你关窗。
            if (max_frames > 0) break;
            SDL_Delay(10);
            continue;
        }

        machine.set_host_clock_ms(clock_base +
                                  static_cast<std::uint32_t>(SDL_GetTicks() - start_ticks));
        // 一次迭代 = 至多一条指令；门前渲染由 core 决定。没有渲染请求时本迭代不呈现，
        // 但宿主可以连续跑一批迭代（不是“一帧 N 条”的语义，只是调度批量）。
        bool requested = false;
        for (int i = 0; i < 4096 && !requested; ++i) {
            if (!machine.step()) break;
            if (machine.consume_frame_step()) requested = true;
            // 影片期间门被影片位关住：同一时钟下的后续迭代都不会推进脚本，交给外层的 1ms 节拍。
            if (machine.movie_active()) break;
        }
        if (!requested) {
            // 场景变成静态以后，引擎就不会再请求渲染 ⇒ 按帧数推进的无人值守模式会空转；
            // 连续一批都没有请求就收工（交互模式仍然等窗口事件 / Esc）。
            if (max_frames > 0 && ++idle_batches > 3000) {
                std::printf("[render] play idle: %zu 批没有渲染请求，按帧数收工\n", idle_batches);
                break;
            }
            SDL_Delay(1);
            continue;
        }
        idle_batches = 0;

        bool presented = false;
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
        if (scene_gpu_ok && machine.scene_commands().empty()) {
            preload_scene_textures(device, scene_cache, machine);
            SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
            if (command == nullptr) break;
            SDL_GPUTexture* swapchain = nullptr;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &swapchain, nullptr, nullptr)) {
                SDL_CancelGPUCommandBuffer(command);
                break;
            }
            if (swapchain != nullptr) {
                if (!draw_scene_gpu(device, command, swapchain, machine, scene_pipeline, scene_cache,
                                    width, height, false)) {
                    SDL_CancelGPUCommandBuffer(command);
                    break;
                }
            }
            if (!SDL_SubmitGPUCommandBuffer(command)) break;
            machine.end_render();  // 清脏 + 命令表生命周期（GPU 路径没有走 rasterize_scene）
            presented = true;
        }
#endif
        if (!presented) {
            // 命令档 / 文字等仍由软件 oracle 合成；GPU 场景路径覆盖绘制项 + 4 顶点网格。
            canvas = core::rasterize_scene(machine, static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height));
            SDL_GPUTexture* next = upload_texture(device, canvas, width, height);
            if (next != nullptr) {
                SDL_ReleaseGPUTexture(device, pipeline.texture);
                pipeline.texture = next;
            }
            if (!present()) break;
        }
        {
            std::size_t active = 0;
            for (const auto& [key, item] : machine.draw_items()) {
                (void)key;
                if ((item.flags & 2u) != 0) ++active;
            }
            max_windows = std::max(max_windows, active);
        }
        ++frame;
        if (max_frames > 0 && static_cast<int>(frame) >= max_frames) break;
    }
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    release_scene_textures(device, scene_cache);
    destroy_scene_pipeline(device, scene_pipeline);
#endif
    destroy_quad_pipeline(device, pipeline);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    std::size_t windows = 0;
    for (const auto& [key, item] : machine.draw_items()) {
        (void)key;
        if ((item.flags & 2u) != 0) ++windows;
    }
    std::printf("[render] play frames=%zu clock=%ums script=%u windows=%zu maxWindows=%zu\n", frame,
                machine.scene_clock_ms(), machine.frame().script_id, windows, max_windows);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 引擎字符串在核心内部是 CP932 字节；显示前统一解码成 UTF-8，控制台也切 UTF-8。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    bool smoke = false;
    std::string install_dir;
    std::string table_path;
    std::string config_path;
    std::string config_keys_path;
    std::string agf_path;
    std::size_t max_steps = 200000;
    std::uint32_t stop_at = 0x20F;
    bool to_title = false;
    bool render_title = false;
    bool show_title = false;
    bool play_title = false;
    int render_frames = 0;
    int trace_steps = 0;
    int play_frames = 0;
    bool play_boot = false;
    bool gpu_title = false;
    std::string logo_dir;
    int logo_stride = 24;
    int logo_frames = 12;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            install_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--opcode-table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--show-title") == 0) {
            show_title = true;
            render_title = true;
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_steps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--render-frames") == 0 && i + 1 < argc) {
            render_frames = std::atoi(argv[++i]);
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--play-title") == 0) {
            play_title = true;
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--render-title-gpu") == 0) {
            gpu_title = true;
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--play-boot") == 0) {
            play_boot = true;
            stop_at = 0;
        } else if (std::strcmp(argv[i], "--render-logo-gpu") == 0 && i + 1 < argc) {
            logo_dir = argv[++i];
            stop_at = 0;
        } else if (std::strcmp(argv[i], "--logo-stride") == 0 && i + 1 < argc) {
            logo_stride = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--logo-frames") == 0 && i + 1 < argc) {
            logo_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--play-frames") == 0 && i + 1 < argc) {
            play_title = true;
            play_frames = std::atoi(argv[++i]);
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--render-title") == 0) {
            render_title = true;
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--to-title") == 0) {
            to_title = true;
            stop_at = 0;
            if (max_steps < 400000) max_steps = 400000;
        } else if (std::strcmp(argv[i], "--stop-at") == 0 && i + 1 < argc) {
            stop_at = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--config-keys") == 0 && i + 1 < argc) {
            config_keys_path = argv[++i];
        } else if (std::strcmp(argv[i], "--agf") == 0 && i + 1 < argc) {
            agf_path = argv[++i];
        }
    }

    if (!agf_path.empty()) return run_core_agf(agf_path);

    if (!install_dir.empty()) {
        const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
        if (table_path.empty()) table_path = (exe_dir / "opcode-table.json").string();
        if (config_keys_path.empty()) config_keys_path = (exe_dir / "config-keys.json").string();
        return run_core_install(install_dir, table_path, config_path, config_keys_path, max_steps, stop_at, to_title, render_title, show_title, play_title, render_frames, trace_steps, play_frames, play_boot, gpu_title, logo_dir, logo_stride, logo_frames);
    }

    SDL_SetAppMetadata("emulator-cxx", "0.1.0", "dev.amayui.emulator-cxx");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_sdl_error("SDL_Init");
        return 1;
    }

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    if (smoke && !SDL_ShaderCross_Init()) {
        log_sdl_error("SDL_ShaderCross_Init");
        SDL_Quit();
        return 1;
    }
#endif

    SDL_GPUDevice* device = create_gpu_device();
    if (device == nullptr) {
        SDL_Quit();
        return 1;
    }

    if (smoke) {
        const SmokeResult result = run_smoke(device);
        std::printf(
            "[emulator-cxx] smoke: %s (%s)\n",
            result.ok ? "PASS" : "FAIL",
            result.detail.c_str());
        SDL_DestroyGPUDevice(device);
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
        SDL_ShaderCross_Quit();
#endif
        SDL_Quit();
        return result.ok ? 0 : 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "emulator-cxx — native host",
        kWidth,
        kHeight,
        SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        log_sdl_error("SDL_CreateWindow");
        SDL_DestroyGPUDevice(device);
        SDL_Quit();
        return 1;
    }

    const int code = run_windowed(device, window);

    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    SDL_ShaderCross_Quit();
#endif
    SDL_Quit();
    return code;
}
