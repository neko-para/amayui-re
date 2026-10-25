// SDL_GPU 设备、读回、纹理上传。
//
// ★ 这里只做"把字节搬上 GPU / 搬回来"，不碰场景语义。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"
namespace app {

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

// 主循环宿主的呈现：每次迭代（门前）由 core 决定是否请求渲染；有请求才合成 / 上传 / 呈现。

}  // namespace app
