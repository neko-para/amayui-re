// 窗口播放：按 core 的渲染请求合成 / 呈现，帧节奏由宿主时钟决定。
//
// ★ 这里只消费 `consume_frame_step()`，不判断"该不该渲染"（那是 core 的门）。
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "app.h"
#include "input_script.h"

#include "core/scene_raster.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace app {

namespace {

// ── 宿主输入（★ 平台边界，⛔ 不是引擎语义） ──
//
// 规范（`data/spec/chapters/60-ops-input.md`）的口径：**位号合成是引擎的决定** —— 平台只提供
// **原始状态**（某个系统键码是否按下、鼠标左右键；摇杆整套不实现，见 SCOPE §0 D4），键盘 / 鼠标的映射表由
// 引擎的输入系统初始化写入。所以这里**不再**把 SDL 的扫描码直接折成输入字（那是引擎的活），
// 只做两件平台的事：
//   ① 扫描码 ↔ **系统键码**（Win32 VK）的对应（平台的消息就是按 VK 来的）；
//   ② 把 SDL 事件泵成引擎窗口过程的两个写点（键按下消息 / 鼠标左右键消息）。
// ⛔ 这张表是**宿主策略**（物理键 → VK），不是引擎的"VK → 输入位"表（那张在 `core/vm.cpp` 的构造器里）。
struct SystemKey {
    std::int32_t vk;
    SDL_Scancode code;
};

constexpr SystemKey kSystemKeys[] = {
    {0x08, SDL_SCANCODE_BACKSPACE}, {0x09, SDL_SCANCODE_TAB},  {0x0D, SDL_SCANCODE_RETURN},
    {0x1B, SDL_SCANCODE_ESCAPE},    {0x20, SDL_SCANCODE_SPACE}, {0x25, SDL_SCANCODE_LEFT},
    {0x26, SDL_SCANCODE_UP},        {0x27, SDL_SCANCODE_RIGHT}, {0x28, SDL_SCANCODE_DOWN},
    {0x30, SDL_SCANCODE_0},         {0x31, SDL_SCANCODE_1},    {0x32, SDL_SCANCODE_2},
    {0x33, SDL_SCANCODE_3},         {0x34, SDL_SCANCODE_4},    {0x35, SDL_SCANCODE_5},
    {0x36, SDL_SCANCODE_6},         {0x37, SDL_SCANCODE_7},    {0x38, SDL_SCANCODE_8},
    {0x39, SDL_SCANCODE_9},         {0x41, SDL_SCANCODE_A},    {0x42, SDL_SCANCODE_B},
    {0x43, SDL_SCANCODE_C},         {0x44, SDL_SCANCODE_D},    {0x45, SDL_SCANCODE_E},
    {0x46, SDL_SCANCODE_F},         {0x47, SDL_SCANCODE_G},    {0x48, SDL_SCANCODE_H},
    {0x49, SDL_SCANCODE_I},         {0x4A, SDL_SCANCODE_J},    {0x4B, SDL_SCANCODE_K},
    {0x4C, SDL_SCANCODE_L},         {0x4D, SDL_SCANCODE_M},    {0x4E, SDL_SCANCODE_N},
    {0x4F, SDL_SCANCODE_O},         {0x50, SDL_SCANCODE_P},    {0x51, SDL_SCANCODE_Q},
    {0x52, SDL_SCANCODE_R},         {0x53, SDL_SCANCODE_S},    {0x54, SDL_SCANCODE_T},
    {0x55, SDL_SCANCODE_U},         {0x56, SDL_SCANCODE_V},    {0x57, SDL_SCANCODE_W},
    {0x58, SDL_SCANCODE_X},         {0x59, SDL_SCANCODE_Y},    {0x5A, SDL_SCANCODE_Z},
    {0x70, SDL_SCANCODE_F1},        {0x71, SDL_SCANCODE_F2},   {0x72, SDL_SCANCODE_F3},
    {0x73, SDL_SCANCODE_F4},        {0x74, SDL_SCANCODE_F5},   {0x75, SDL_SCANCODE_F6},
    {0x76, SDL_SCANCODE_F7},        {0x77, SDL_SCANCODE_F8},   {0x78, SDL_SCANCODE_F9},
    {0x79, SDL_SCANCODE_F10},       {0x7A, SDL_SCANCODE_F11},  {0x7B, SDL_SCANCODE_F12},
};

// 某个**系统键码**当前是否按下（平台边界）。
bool system_key_down(std::int32_t vk) {
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (state == nullptr) return false;
    for (const auto& entry : kSystemKeys) {
        if (entry.vk == vk) return state[entry.code];
    }
    return false;
}

// 扫描码 → 系统键码（窗口过程用）；找不到 ⇒ −1。
std::int32_t vk_from_scancode(SDL_Scancode code) {
    for (const auto& entry : kSystemKeys) {
        if (entry.code == code) return entry.vk;
    }
    return -1;
}

// 有没有任何已映射的键按下（只喂文本层的"抽干"这一处宿主观察）。
bool any_system_key_down() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (state == nullptr) return false;
    for (const auto& entry : kSystemKeys) {
        if (state[entry.code]) return true;
    }
    return false;
}

// 把 SDL 的四类真实设备接到 core 的采样口（窗口路径的默认输入源）。
// ★ `scripted != nullptr` 时不调用它：那时输入来自 `--input-script` 的时间线。
// ★ 摇杆：**整套输入不实现**（SCOPE §0 D4）——本函数据此**不打开任何设备**，返回值恒为空。
SDL_Joystick* bind_sdl_input(core::Machine& machine, SDL_Window* window, int width, int height) {
    // ① 键盘：**逐系统键码**查询（映射表在引擎里；这里只回答"这个 VK 按没按下"）。
    machine.set_key_code_source([](int vk) { return system_key_down(vk); });
    // ② 鼠标：左 / 右键的**原始状态**（位 0 左 / 位 1 右；归约到引擎位是核心的事）。
    machine.set_mouse_button_source([](std::int32_t& mask) -> bool {
        if (!SDL_HasMouse()) return false;
        const auto sdl = SDL_GetMouseState(nullptr, nullptr);
        mask = 0;
        if ((sdl & SDL_BUTTON_LMASK) != 0) mask |= 1;
        if ((sdl & SDL_BUTTON_RMASK) != 0) mask |= 2;
        return true;
    });
    machine.set_click_on_up(static_cast<std::int32_t>(machine.config().get_int("set:ClickOnUp")));
    // ③ 摇杆：**整套输入不实现**（SCOPE §0 D4）——产品路径**不打开任何摇杆设备**，
    //    在 VM 边界按「无对应设备」拦截：按钮数恒 0、不装按钮 / 轴 / 帽子来源 ⇒ 方向位与按钮位永不置起。
    //    ⛔ 不要去猜"设备能力"或补缺省阈值：没有设备就是没有设备（`0x100` / `0x101` 的摇杆支因此在产品路径不可达）。
    machine.set_joy_button_count(0);
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
    // 摇杆：按 SCOPE §0 D4 **不返回设备**（调用方因此不会去关它、也不会装任何摇杆来源）。
    return nullptr;
}

}  // namespace

int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames, bool* user_quit,
                        ScriptedInput* scripted) {
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
    // 宿主输入：键盘 / 鼠标（**摇杆整套不实现**，见 SCOPE §0 D4 ⇒ 组头按钮数恒 0、不装轴/帽子来源）。
    // 两者都只在脚本执行到对应 opcode 时才被取用：
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
    // 窗口路径的输入源：**给了脚本化输入就用脚本**（可复现的自动化）；否则用 SDL 的真实设备。
    // ★ 两者是同一层的替身：`--input-script` 不是引擎语义，它只是把"平台归约"换成时间线。
    SDL_Joystick* joystick = nullptr;
    if (scripted == nullptr) joystick = bind_sdl_input(machine, window, width, height);
    // ★ 短睡眠（`0x0c8` 的 n < 10 那一支）的**宿主执行口**：窗口路径的时钟是真实墙钟
    //   （`SDL_GetTicks`），所以"睡 n 毫秒"必须真的花掉 n 毫秒。让 core 自己往 `host_clock_ms_`
    //   上加 n 会与本函数的墙钟采样**互相踩**：core 把钟推到未来、下一次采样又把它拨回真实值
    //   ⇒ 场景时钟出现"往回走"。`0x300` 演示闸的「停期限」按无符号减法 `现在 − 起点 ≥ 期限`
    //   判到期，往回走会当场成立 ⇒ 设置界面底部的示例文本偶尔连播 2–5 次。
    //   装了这条 ⇒ 处理器体内不再自己推进宿主时钟（见 `Machine::set_sleep_host`）。
    //   ★ 用 `SDL_DelayPrecise` 而不是 `SDL_Delay`：后者的粒度是系统定时器（本机实测 ≈15ms），
    //     而引擎这一支的 n < 10ms 是"让出 1ms 给输入轮询"的意思 —— 用 15ms 的粒度会把
    //     设置界面压到 ~3fps（实测：22s 只呈现 67 帧）。
    machine.set_sleep_host([](std::uint32_t ms) {
        SDL_DelayPrecise(static_cast<Uint64>(ms) * 1000000ull);
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
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                // 引擎自己的窗口过程那一支：**键按下消息**（`event.key.repeat` 为 true 的是按住
                // 产生的重复消息，spec 明文不算）⇒ 把对应的输入位写进位号锁存字。
                if (!event.key.repeat) {
                    machine.push_key_down_message(vk_from_scancode(event.key.scancode));
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                // 左右键消息：按配置 `set:ClickOnUp` 选按下 / 抬起那一条（由核心判断）。
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (event.button.button == SDL_BUTTON_LEFT) {
                    machine.push_mouse_button_message(false, down);
                } else if (event.button.button == SDL_BUTTON_RIGHT) {
                    machine.push_mouse_button_message(true, down);
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                // ★ 滚轮：把增量推进引擎那一格（`0x10d` 读并清）。刻度 / 符号是宿主策略。
                machine.add_wheel_delta(static_cast<std::int32_t>(event.wheel.y));
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
            // ★ 异常停机补帧链（哪个 BIN 的哪个偏移）：正常 `exit`（Halted）不刷这一段。
            if (stop.reason != core::StopReason::Halted) {
                std::fputs(machine.stop_stack_report().c_str(), stdout);
            }
            break;
        }

        machine.set_host_clock_ms(clock_base +
                                  static_cast<std::uint32_t>(SDL_GetTicks() - start_ticks));
        // 文本层的抽干（`ADV-TEXT-APPEND`）：主循环**被动观察**输入 —— 有键按下 / 鼠标键按下就把
        // 剩下的待画格一次画完。⛔ 与脚本主动取键（`0xff` / `0x109`）无关。
        {
            const bool any_key = any_system_key_down();
            const bool any_mouse = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0;
            machine.set_input_pressed(any_key || any_mouse);
        }
        if (scripted != nullptr) {
            scripted->advance(machine.scene_clock_ms());
            machine.add_wheel_delta(scripted->take_wheel_delta());
        }
        // 一次迭代 = 至多一条指令；门前渲染由 core 决定。没有渲染请求时本迭代不呈现，
        // 但宿主可以连续跑一批迭代（不是“一帧 N 条”的语义，只是调度批量）。
        bool requested = false;
        // ★ 批量的**墙钟上限**（宿主调度，⛔ 不是"一帧 N 条"的引擎语义）：脚本的短睡眠（`0x0c8`）
        //   现在是**真的**花掉 n 毫秒（`SDL_DelayPrecise`），所以一批 4096 条迭代可能要几百毫秒；
        //   而输入 / 脚本时间线是**每个外层迭代**喂一次的 ⇒ 批次太长会把"按下 → 抬起"整段吞掉
        //   （实测：设置界面进不去）。这里只把批次切短，门 / 时钟 / 渲染请求的语义一个字不变。
        const Uint64 batch_start_ns = SDL_GetTicksNS();
        for (int i = 0; i < 4096 && !requested; ++i) {
            if (!machine.step()) break;
            if (machine.consume_frame_step()) requested = true;
            if (SDL_GetTicksNS() - batch_start_ns >= 8000000ull) break;
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
        // 渲染步入口（`CORE-OPS-RENDER-FRAME-STEP` 的「渲染步（合同边界）」）：清屏位判定 →
        // 请求开始一次批次。**被拒绝 ⇒ 整步立即结束**：这一拍不遍历 / 不呈现 / 不复位目标 /
        // 不清命令表。★ GPU 与 CPU 两条路各自走一次入口，同一拍只会走其中一条。
        bool step_refused = false;
        if (scene_gpu_ok && machine.scene_commands().empty()) {
            if (!machine.begin_render_step()) {
                step_refused = true;
            } else {
                preload_scene_textures(device, scene_cache, machine);
                SDL_GPUCommandBuffer* command = SDL_AcquireGPUCommandBuffer(device);
                if (command == nullptr) break;
                SDL_GPUTexture* swapchain = nullptr;
                if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &swapchain, nullptr, nullptr)) {
                    SDL_CancelGPUCommandBuffer(command);
                    break;
                }
                if (swapchain != nullptr) {
                    // ★ 宿主呈现策略：交换链每帧**清一次**（`clear = true`）。
                    //   引擎的"渲染步不自动清"说的是它的渲染目标；游戏靠**全屏背景项**盖满画面。
                    //   本宿主的场景批次如果没盖满（例如行带 40px 之间的 10px 背景缝、半透明/未绘区域），
                    //   取 `LOAD` 会把上一帧留在交换链上 ⇒ 滚动/切页时同一行文字在两个 y 位置都可见
                    //   （"两个位置 + 背景缝"）。交换链是平台的呈现表面，清它不影响引擎的渲染步语义；
                    //   无头 `--bmp` 每帧新建目标 ⇒ 看不到这类现象。
                    if (!draw_scene_gpu(device, command, swapchain, machine, scene_pipeline, scene_cache,
                                        width, height, true)) {
                        SDL_CancelGPUCommandBuffer(command);
                        break;
                    }
                }
                if (!SDL_SubmitGPUCommandBuffer(command)) break;
                // 结束批次 → 呈现（门 = 步末目标是后缓冲）→ 步内收尾（GPU 路径没有走 rasterize_scene）。
                machine.end_render_step();
                presented = true;
            }
        }
        if (step_refused) continue;  // 批次闸拒绝 ⇒ 整步结束（不呈现、不计数）
        if (!presented) {
            // 命令档 / 文字等仍由软件 oracle 合成；GPU 场景路径覆盖绘制项 + 4 顶点网格。
            canvas = core::rasterize_scene(machine, static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height));
            // 闸拒绝 ⇒ 整步结束：没有可呈现的帧（`rasterize_scene` 内部已把请求序列停在「开始批次」）。
            if (canvas.empty()) continue;
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
    if (joystick != nullptr) SDL_CloseJoystick(joystick);
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
