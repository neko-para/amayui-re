// 窗口播放：按 core 的渲染请求合成 / 呈现，帧节奏由宿主时钟决定。
//
// ★ 这里只消费 `consume_frame_step()`，不判断"该不该渲染"（那是 core 的门）。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"

#include "core/scene_raster.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>

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

}  // namespace

int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames, bool* user_quit) {
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
    ScenePipeline scene_pipeline;
    const bool scene_gpu_ok = create_scene_pipeline(device, swapchain_format, scene_pipeline);
    SceneTextureCache scene_cache;
    machine.consume_frame_step();  // 丢掉预跑期留下的请求
    // 宿主输入：键盘 / 鼠标（**不接摇杆**）。三者都只在脚本执行到对应 opcode 时才被取用：
    // `0xff` 采样输入字、`0x109` 光标、`0x108` 按钮位图、`0xcc`/`0xcd` 鼠标回调。
    // ★ `0x2fc` 是**触点查询**（不是鼠标事件队列）：桌面鼠标-only 不提供触点源 ⇒ 恒写 0。
    // 测试 / 诊断：到点投递一次 SDL_EVENT_QUIT，用来端到端验证“关窗保存配置”。
    // 只在显式设置环境变量时启用；⛔ 不是产品退出路径。
    if (const char* quit_ms = std::getenv("EMULATOR_CXX_QUIT_AFTER_MS")) {
        const Uint32 delay = static_cast<Uint32>(std::strtoul(quit_ms, nullptr, 10));
        if (delay > 0) {
            SDL_AddTimer(delay, [](void* data, SDL_TimerID, Uint32) -> Uint32 {
                SDL_Event close{};
                close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
                close.window.windowID = SDL_GetWindowID(static_cast<SDL_Window*>(data));
                SDL_PushEvent(&close);
                return 0;
            }, window);
        }
    }
    machine.set_key_sampler([] { return sample_key_mask(); });
    machine.set_cursor_source([window, width, height](std::int32_t& x, std::int32_t& y) -> bool {
        static int probe = 0;
        const bool probe_on = std::getenv("EMULATOR_CXX_INPUT_TRACE") != nullptr;
        // ★ 规范口径（`CORE-INPUT-MOUSE-POS`）：**设备不可用 = 没有窗口 / 光标不在客户区**。
        //   所以"可用性"由**光标是否落在客户区内**决定，⛔ 不是"窗口有没有鼠标焦点"：
        //   实测窗口已显示、鼠标也在客户区时 `SDL_WINDOW_MOUSE_FOCUS` 仍可能是 0（合成器/焦点策略），
        //   拿它当哨兵会让 `0x109` 恒写 −100000、hover 整段失效。
        int window_width = 0;
        int window_height = 0;
        if (!SDL_GetWindowSize(window, &window_width, &window_height) || window_width <= 0 ||
            window_height <= 0) {
            return false;
        }
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        const bool has_mouse_focus = (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        if (has_mouse_focus) {
            // 有鼠标焦点：SDL 直接给**客户区**坐标（与 `SDL_GetWindowSize` 同一套点坐标）。
            SDL_GetMouseState(&mouse_x, &mouse_y);
        } else {
            // 没有焦点：用全局光标减窗口位置（两者都是 SDL 屏幕坐标，同一套点坐标）。
            float global_x = 0.0f;
            float global_y = 0.0f;
            int window_x = 0;
            int window_y = 0;
            if (!SDL_GetGlobalMouseState(&global_x, &global_y) ||
                !SDL_GetWindowPosition(window, &window_x, &window_y)) {
                return false;
            }
            mouse_x = global_x - static_cast<float>(window_x);
            mouse_y = global_y - static_cast<float>(window_y);
        }
        // ★ 规范口径（`CORE-INPUT-MOUSE-POS`）：哨兵只由“输入轮询被停用 / 没有输入源”触发。
        //   窗口存在时，**光标在客户区外也返回真实客户区坐标（可为负 / 超界）**；把它写成 −100000
        //   会让脚本无法区分“出窗”与“没有输入设备”。
        const bool inside = mouse_x >= 0.0f && mouse_y >= 0.0f &&
                            mouse_x < static_cast<float>(window_width) &&
                            mouse_y < static_cast<float>(window_height);
        if (probe_on && (++probe % 30) == 0) {
            std::fprintf(stderr,
                         "[input] cursor inside=%d focus=%d mouse=%.1f,%.1f win=%dx%d\n",
                         inside ? 1 : 0, has_mouse_focus ? 1 : 0, mouse_x, mouse_y, window_width,
                         window_height);
        }
        x = static_cast<std::int32_t>(mouse_x * static_cast<float>(width) /
                                      static_cast<float>(window_width));
        y = static_cast<std::int32_t>(mouse_y * static_cast<float>(height) /
                                      static_cast<float>(window_height));
        return true;
    });
    machine.set_mouse_button_source([](std::int32_t& mask) -> bool {
        if (!SDL_HasMouse()) return false;
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
        // ★ 宿主映射（平台边界）：SDL 的右键是 `SDL_BUTTON_RMASK` = 1<<2，而引擎的位图是
        //   **位 0 左 / 位 1 右**（`CORE-INPUT-MOUSE-BUTTON`）。⛔ 不能把 SDL 掩码原样传进核心：
        //   那会让右键落在 bit2，TITLE 的 `and 3f2, 2` 永远看不到右键。
        const auto sdl = SDL_GetMouseState(&mouse_x, &mouse_y);
        mask = 0;
        if ((sdl & SDL_BUTTON_LMASK) != 0) mask |= 1;
        if ((sdl & SDL_BUTTON_RMASK) != 0) mask |= 2;
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
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(window))) {
                running = false;
                if (user_quit != nullptr) *user_quit = true;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
                if (user_quit != nullptr) *user_quit = true;
            }
        }
        if (!running) break;
        if (machine.halted()) {
            // 脚本结束（TITLE 的 `exit` 把根帧结束成 Halted）就是**整个游戏结束**：
            // 关窗并回到 boot 收尾。⛔ 不能"保留最后一帧等用户关窗"——那会让退出路径
            // 看起来像进程没退出。错误型 halt 也一并收工，由 boot 打印的 stop 诊断负责。
            const auto& stop = machine.last_stop();
            std::printf("[render] script halted reason=%d opcode=0x%X message=%s\n",
                        static_cast<int>(stop.reason), stop.opcode, stop.message.c_str());
            break;
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
    release_scene_textures(device, scene_cache);
    destroy_scene_pipeline(device, scene_pipeline);
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
