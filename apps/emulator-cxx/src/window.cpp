// 窗口播放：按 core 的渲染请求合成 / 呈现，帧节奏由宿主时钟决定。
//
// ★ 这里只消费 `consume_frame_step()`，不判断"该不该渲染"（那是 core 的门）。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include "core/scene_raster.h"

#include <cstdio>
#include <cstdint>

namespace app {

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

}  // namespace app
