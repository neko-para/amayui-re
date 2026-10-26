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

SmokeResult run_smoke_quad(
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

// 两条 pipeline × 两种目标格式（离屏 RGBA8 / 呈现用的 BGRA8）。只建不画：
// 验的是**这套着色器在当前后端能不能编出来、顶点属性/混合状态对不对**（Metal 的 API 校验
// 会在建 pipeline 时当场报错），像素判据在上面两条混合断言上。
// ★ 场景批次的 pipeline 只有真的渲染场景时才建（需要游戏数据），所以这里单独建一次。
SmokeResult run_smoke_pipelines(SDL_GPUDevice* device) {
    constexpr SDL_GPUTextureFormat formats[] = {
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
    };
    for (const SDL_GPUTextureFormat format : formats) {
        QuadPipeline quad;
        if (!create_quad_pipeline(device, format, SDL_GPU_BLENDOP_ADD,
                                  SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                                  SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, quad)) {
            destroy_quad_pipeline(device, quad);
            return SmokeResult{false, "quad pipeline failed: format=" + std::to_string(int(format))};
        }
        destroy_quad_pipeline(device, quad);

        ScenePipeline scene;
        if (!create_scene_pipeline(device, format, scene)) {
            destroy_scene_pipeline(device, scene);
            return SmokeResult{false, "scene pipeline failed: format=" + std::to_string(int(format))};
        }
        destroy_scene_pipeline(device, scene);
        std::printf("[emulator-cxx] smoke: pipelines ok format=%d\n", int(format));
    }
    return SmokeResult{true, "pipelines ok (rgba8/bgra8 × quad/scene)"};
}

SmokeResult run_smoke(SDL_GPUDevice* device) {
    SmokeResult clear = run_smoke_clear(device);
    if (!clear.ok) return clear;

    SmokeResult texture = run_smoke_texture(device);
    if (!texture.ok) return texture;

    // 两条混合断言都靠 fixed-function 的 blend state，着色器只负责采样 ⇒ 三个平台应该给出
    // 同一组像素（着色器那一步的差别只有 DXIL / SPIR-V / MSL 三种形式）。
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

    SmokeResult pipelines = run_smoke_pipelines(device);
    if (!pipelines.ok) return pipelines;

    // 走到这里 ⇒ 设备的原生着色器格式真的建出了两条 pipeline；把路径记进 detail 便于对照平台。
    return SmokeResult{true,
                       clear.detail + " | " + texture.detail + " | " + alpha.detail + " | " +
                           subtract.detail + " | " + pipelines.detail + " | shader=" +
                           shader_backend_name(device)};
}

}  // namespace app
