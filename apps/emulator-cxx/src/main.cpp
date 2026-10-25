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
#include "core/install.h"
#include "core/opcode_table.h"
#include "core/script.h"
#include "core/vm.h"

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

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

std::filesystem::path path_from_utf8(const std::string& text);

int run_core_agf(const std::string& agf_path) {
    try {
        std::ifstream input(path_from_utf8(agf_path), std::ios::binary);
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

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

int run_core_install(const std::string& install_dir, const std::string& table_path,
                     const std::string& config_path, const std::string& config_keys_path,
                     std::size_t max_steps, std::uint32_t stop_at) {
    try {
        const core::Install install = core::open_install(path_from_utf8(install_dir));
        const auto& index = install.index;
        std::printf(
            "[core] alf signature=%s title=%s zone=%d archives=%zu files=%zu configDefaults=%zu\n",
            index.signature.c_str(),
            index.title.c_str(),
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
            root_record.name.c_str(),
            root_record.archive_index,
            root_record.length,
            root.size());

        const core::ScriptHeader header = core::parse_script_header(root);
        const core::OpcodeTable table = core::OpcodeTable::load_from_file(table_path);
        core::ConfigVocabulary vocabulary = core::ConfigVocabulary::load_from_file(config_keys_path);
        core::ConfigStore config(std::move(vocabulary));
        config.load_defaults(index.config_defaults);
        std::filesystem::path player_path = config_path.empty()
            ? install.root / config.get_string("set:RegFilePath") / "SYS4REG.INI"
            : path_from_utf8(config_path);
        config.load_player_file(player_path);
        std::printf(
            "[core] config named=%zu script=%zu player=%s\n",
            config.named_size(),
            config.script_size(),
            player_path.string().c_str());
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
        const core::RunResult vm = machine.run(max_steps);
        std::printf(
            "[core] vm stop=%d steps=%zu opcode=0x%X offset=%zu message=%s\n",
            static_cast<int>(vm.reason),
            vm.steps,
            vm.opcode,
            vm.offset,
            vm.message.c_str());

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

int main(int argc, char** argv) {
    bool smoke = false;
    std::string install_dir;
    std::string table_path;
    std::string config_path;
    std::string config_keys_path;
    std::string agf_path;
    std::size_t max_steps = 200000;
    std::uint32_t stop_at = 0x20F;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            install_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--opcode-table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
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
        return run_core_install(install_dir, table_path, config_path, config_keys_path, max_steps, stop_at);
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
