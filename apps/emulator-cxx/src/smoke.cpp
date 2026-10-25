// `--smoke`：离屏自检（clear / 纹理往返 / 四边形混合 / shadercross 编译）。
//
// ★ 这是**测试**，不是产品路径：它不读游戏安装、不跑 VM，只验 GPU 后端本身。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
namespace app {

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

}  // namespace app
