// 窗口播放：按 core 的渲染请求合成 / 呈现，帧节奏由宿主时钟决定。
//
// ★ 这里只消费 `consume_frame_step()`，不判断"该不该渲染"（那是 core 的门）。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include "core/scene_raster.h"

#include <cstdio>
#include <cstdint>
#include <deque>

namespace app {

namespace {

// ── 宿主输入（★ 平台边界，⛔ 不是引擎语义） ──
//
// 规范只规定「平台把当前键 / 鼠标状态归约成一个 32 位输入字 + 光标 + 按钮位图 + 一条事件队列」，
// **物理键 ↔ 输入位号的映射属宿主**（`data/spec/chapters/60-ops-input.md` 开头）。这张表就是那个策略：
// 位号 0..31 与 `0xfb` / `0x100` / `0x104` 用的是同一套编号。要改键位只改这里。
struct KeyBit {
    std::uint32_t bit;
    SDL_Scancode code;
};

constexpr KeyBit kKeyBits[] = {
    {0, SDL_SCANCODE_RETURN},  {1, SDL_SCANCODE_ESCAPE},  {2, SDL_SCANCODE_SPACE},
    {3, SDL_SCANCODE_LEFT},    {4, SDL_SCANCODE_UP},      {5, SDL_SCANCODE_RIGHT},
    {6, SDL_SCANCODE_DOWN},    {7, SDL_SCANCODE_BACKSPACE}, {8, SDL_SCANCODE_TAB},
    {9, SDL_SCANCODE_LCTRL},   {10, SDL_SCANCODE_LSHIFT}, {11, SDL_SCANCODE_Z},
    {12, SDL_SCANCODE_X},      {13, SDL_SCANCODE_C},      {14, SDL_SCANCODE_A},
    {15, SDL_SCANCODE_S},      {16, SDL_SCANCODE_D},      {17, SDL_SCANCODE_Q},
    {18, SDL_SCANCODE_W},      {19, SDL_SCANCODE_E},      {20, SDL_SCANCODE_R},
    {21, SDL_SCANCODE_1},      {22, SDL_SCANCODE_2},      {23, SDL_SCANCODE_3},
    {24, SDL_SCANCODE_4},      {25, SDL_SCANCODE_5},      {26, SDL_SCANCODE_6},
    {27, SDL_SCANCODE_7},      {28, SDL_SCANCODE_8},      {29, SDL_SCANCODE_9},
    {30, SDL_SCANCODE_0},      {31, SDL_SCANCODE_F1},
};

// 采样一次键盘（`0xff` 的平台请求）：位号 = 上表的 bit，置起 = 当前按下。
std::uint32_t sample_key_mask() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (state == nullptr) return 0;
    std::uint32_t mask = 0;
    for (const auto& entry : kKeyBits) {
        if (state[entry.code]) mask |= 1u << entry.bit;
    }
    return mask;
}

// 窗口像素 → 逻辑分辨率的客户区坐标（窗口可缩放；引擎坐标是逻辑分辨率那一套）。
bool window_to_logical(SDL_Window* window, int logical_width, int logical_height, float window_x,
                       float window_y, std::int32_t& x, std::int32_t& y) {
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSize(window, &window_width, &window_height) || window_width <= 0 ||
        window_height <= 0) {
        return false;
    }
    x = static_cast<std::int32_t>(window_x * static_cast<float>(logical_width) /
                                  static_cast<float>(window_width));
    y = static_cast<std::int32_t>(window_y * static_cast<float>(logical_height) /
                                  static_cast<float>(window_height));
    return true;
}

// 「光标在客户区」的判据：窗口有鼠标焦点。没有焦点 ⇒ 设备不可用（`0x109` 写 −100000）。
bool mouse_focused(SDL_Window* window) {
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS) != 0;
}

}  // namespace

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
    // 宿主输入：键盘 / 鼠标（**不接摇杆**）。三者都只在脚本执行到对应 opcode 时才被取用：
    // `0xff` 采样输入字、`0x109` 光标、`0x108` 按钮位图、`0x2fc` 取一条事件、`0xcc`/`0xcd` 鼠标回调。
    std::deque<core::Machine::MouseEvent> mouse_events;
    machine.set_key_sampler([] { return sample_key_mask(); });
    machine.set_cursor_source([window, width, height](std::int32_t& x, std::int32_t& y) -> bool {
        if (!mouse_focused(window)) return false;
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        return window_to_logical(window, width, height, mouse_x, mouse_y, x, y);
    });
    machine.set_mouse_button_source([window](std::int32_t& mask) -> bool {
        if (!mouse_focused(window)) return false;
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        // SDL 的 LMASK / RMASK 恰好就是位 0 左 / 位 1 右（与 `CORE-INPUT-MOUSE-BUTTON` 同口径）。
        mask = static_cast<std::int32_t>(SDL_GetMouseState(&mouse_x, &mouse_y));
        return true;
    });
    machine.set_mouse_event_source([&mouse_events](core::Machine::MouseEvent& out) -> bool {
        if (mouse_events.empty()) return false;  // 没有事件：`0x2fc` 只写结果 0
        out = mouse_events.front();
        mouse_events.pop_front();
        return true;
    });
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
            else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                      event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                     event.button.windowID == SDL_GetWindowID(window)) {
                // 事件标志低两位由宿主定义（规范只要求原样传递，见 `CORE-INPUT-MOUSE-EVENT` 的残差）。
                core::Machine::MouseEvent queued;
                std::int32_t x = 0;
                std::int32_t y = 0;
                window_to_logical(window, width, height, event.button.x, event.button.y, x, y);
                queued.x = x;
                queued.y = y;
                queued.flags = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0x11 : 0x12;
                queued.extra = static_cast<std::int32_t>(event.button.button) - 1;  // 0 = 左 / 1 = 右
                mouse_events.push_back(queued);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL &&
                       event.wheel.windowID == SDL_GetWindowID(window)) {
                core::Machine::MouseEvent queued;
                std::int32_t x = 0;
                std::int32_t y = 0;
                window_to_logical(window, width, height, event.wheel.mouse_x, event.wheel.mouse_y, x,
                                  y);
                queued.x = x;
                queued.y = y;
                queued.flags = 0x40;  // 滚轮
                queued.extra = static_cast<std::int32_t>(event.wheel.integer_y != 0
                                                             ? event.wheel.integer_y
                                                             : static_cast<Sint32>(event.wheel.y));
                mouse_events.push_back(queued);
            }
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
