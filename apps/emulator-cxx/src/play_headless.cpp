// 无窗口「按帧推进」的播放路径（`--play-headless`）：与窗口路径共用同一个门、同一套输入注入，
// 只是把呈现换成可选的离屏 BMP 导出。
//
// ★ 它**不是**"跳到某个画面"的捷径：仍然从根脚本跑完整 boot（`boot.cpp` 已经装载好根帧）。
// ★ 时钟是**仿真**的：每外层迭代 `tick_ms` 毫秒（与窗口路径"一次迭代一批、墙钟推进"同形），
//   所以输入脚本的 `at` 是确定的 —— 这是把 TITLE 的输入环变成可回归判据的前提。
#include <SDL3/SDL.h>

#include "app.h"
#include "input_script.h"

#include <cstdio>
#include <string>
#include <vector>

namespace app {

int run_headless_play(core::Machine& machine, ScriptedInput* scripted,
                      const HeadlessPlayOptions& options, int width, int height) {
    const std::uint32_t tick = options.tick_ms == 0 ? 16 : options.tick_ms;
    std::uint32_t clock = machine.scene_clock_ms();
    bool anchored = options.anchor_script == 0;
    std::uint32_t anchor_clock = clock;

    // 呈现只在要求导出 BMP 时初始化（CTest 的判据不依赖 GPU / 窗口）。
    SDL_GPUDevice* device = nullptr;
    if (!options.bmp_dir.empty()) {
        SDL_CreateDirectory(options.bmp_dir.c_str());
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            log_sdl_error("SDL_Init(headless-play)");
            return 1;
        }
        if (!init_shader_toolchain()) {
            SDL_Quit();
            return 1;
        }
        device = create_gpu_device();
        if (device == nullptr) {
            quit_shader_toolchain();
            SDL_Quit();
            return 1;
        }
    }

    std::vector<std::uint32_t> chain{machine.frame().script_id};
    std::size_t steps = 0;
    int presented = 0;
    int dumped = 0;
    bool reached = false;
    bool settle = false;  // 达成后：跑到下一次渲染请求，给"设置界面第一帧"留一张图
    std::string dump_failure;
    const std::uint32_t start_clock = clock;
    machine.consume_frame_step();  // 丢掉 boot 预跑期留下的请求（与窗口路径同口径）
    std::vector<std::int32_t> watch_last(options.watch_locals.size(), 0);
    bool watch_valid = false;
    std::size_t trace_left = options.trace_script_steps;
    const auto dump_watch = [&](const char* why) {
        if (options.watch_locals.empty()) return;
        std::printf("[watch] %s clock=%ums script=%u", why, clock - start_clock,
                    machine.frame().script_id);
        for (std::size_t i = 0; i < options.watch_locals.size(); ++i) {
            const auto value = machine.local_int(options.watch_locals[i]);
            watch_last[i] = value;
            std::printf(" loc[%u]=%d", options.watch_locals[i], value);
        }
        std::printf("\n");
        watch_valid = true;
    };

    while (true) {
        if (machine.halted()) break;
        machine.set_host_clock_ms(clock);
        if (scripted != nullptr) {
            // 锚点之前不应用任何事件（`-1`）：否则 boot 期间就把光标搬到按钮上，
            // TITLE 的 hover 判据永远看不到"坐标变了"。
            scripted->advance(anchored ? static_cast<std::int64_t>(clock) -
                                             static_cast<std::int64_t>(anchor_clock)
                                       : -1);
            machine.add_wheel_delta(scripted->take_wheel_delta());
        }
        bool requested = false;
        bool stop = false;
        for (int i = 0; i < 4096; ++i) {
            const bool in_trace_script = options.trace_script == 0 ||
                                         machine.frame().script_id == options.trace_script;
            if (trace_left > 0 && in_trace_script) {
                const auto& code = machine.frame().code;
                const auto ip = machine.frame().ip;
                if (ip + 4 <= code.size()) {
                    const auto opcode = core::read_u32(code, ip);
                    bool wanted = options.trace_opcodes.empty();
                    for (const auto candidate : options.trace_opcodes) {
                        if (candidate == opcode) { wanted = true; break; }
                    }
                    if (wanted) {
                        std::printf("[step] clock=%ums script=%u ip=%zu opcode=0x%X\n",
                                    clock - start_clock, machine.frame().script_id, ip, opcode);
                        --trace_left;
                    }
                }
            }
            if (!machine.step()) break;
            ++steps;
            const std::uint32_t script = machine.frame().script_id;
            if (script != chain.back()) {
                chain.push_back(script);
                if (scripted != nullptr && !anchored && options.anchor_script != 0 &&
                    script == options.anchor_script) {
                    anchored = true;
                    anchor_clock = clock;
                    std::printf("[play] 输入锚点：script=%u clock=%ums\n", script, clock - start_clock);
                }
            }
            if (!reached && options.expect_script != 0 && script == options.expect_script) {
                reached = true;
                std::printf("[play] reached script=%u clock=%ums steps=%zu\n", script,
                            clock - start_clock, steps);
                settle = !options.bmp_dir.empty();
                if (!settle) { stop = true; break; }
            }
            if (machine.consume_frame_step()) {
                requested = true;
                break;
            }
            if (machine.movie_active()) break;
            if (machine.halted()) break;
        }
        if (stop || machine.halted()) break;
        if (requested) {
            const int index = presented++;
            const bool want = !options.bmp_dir.empty() &&
                              (options.bmp_stride <= 1 || index % options.bmp_stride == 0) &&
                              (options.bmp_limit <= 0 || dumped < options.bmp_limit);
            if (want) {
                char name[512];
                std::snprintf(name, sizeof(name), "%s/frame-%04d.bmp", options.bmp_dir.c_str(), index);
                if (!render_scene_gpu_to_bmp(device, machine, name, width, height)) {
                    dump_failure = name;
                    machine.end_render();
                    break;
                }
                ++dumped;
            }
            machine.end_render();
            if (settle && reached) {
                char name[512];
                std::snprintf(name, sizeof(name), "%s/reached-%u.bmp", options.bmp_dir.c_str(),
                              options.expect_script);
                if (!render_scene_gpu_to_bmp(device, machine, name, width, height)) {
                    dump_failure = name;
                } else {
                    std::printf("[play] reached-frame -> %s\n", name);
                }
                break;
            }
        }
        if (options.max_frames > 0 && presented >= options.max_frames) break;
        // 步数上限：与窗口路径不同，这里必须显式封顶（CTest 不许跑飞）。
        if (options.limit_steps > 0 && steps >= options.limit_steps) {
            std::printf("[play] 步数上限 %zu 到了（停因不是引擎的）\n", options.limit_steps);
            break;
        }
        // 观察口：只在被观察的格变化时打一行（噪声可控）。
        if (!options.watch_locals.empty()) {
            bool changed = !watch_valid;
            for (std::size_t i = 0; i < options.watch_locals.size() && !changed; ++i) {
                changed = machine.local_int(options.watch_locals[i]) != watch_last[i];
            }
            if (changed) dump_watch("change");
        }
        clock += tick;
    }

    if (device != nullptr) SDL_DestroyGPUDevice(device);
    if (!options.bmp_dir.empty()) {
        quit_shader_toolchain();
        SDL_Quit();
    }

    const auto& stop = machine.last_stop();
    std::printf(
        "[play] steps=%zu presented=%d clock=%ums script=%u halted=%d reason=%d opcode=0x%X message=%s\n",
        steps, presented, clock - start_clock, machine.frame().script_id, machine.halted() ? 1 : 0,
        static_cast<int>(stop.reason), stop.opcode, stop.message.c_str());
    std::printf("[play] script-chain:");
    for (const auto script : chain) std::printf(" %u", script);
    std::printf("\n");
    if (scripted != nullptr) {
        std::printf("[play] input applied=%zu exhausted=%d last=%lld\n", scripted->applied(),
                    scripted->exhausted() ? 1 : 0, static_cast<long long>(scripted->applied_until()));
    }
    dump_watch("final");
    if (!dump_failure.empty()) {
        std::fprintf(stderr, "[play] BMP 导出失败：%s\n", dump_failure.c_str());
        return 1;
    }
    if (options.expect_script != 0 && !reached) {
        std::fprintf(stderr, "[play] 没有进入目标脚本 %u\n", options.expect_script);
        return 1;
    }
    if (options.require_script != 0 && machine.frame().script_id != options.require_script) {
        std::fprintf(stderr, "[play] 收口时不在要求的脚本 %u（当前 %u）\n", options.require_script,
                     machine.frame().script_id);
        return 1;
    }
    // ★ 未实现 / 非法 opcode / 内部错误都算失败（正常的 `exit` 停机不算）。
    if (machine.halted()) {
        const auto reason = stop.reason;
        if (reason == core::StopReason::UnsupportedOpcode || reason == core::StopReason::Error ||
            reason == core::StopReason::BadOpcode) {
            std::fprintf(stderr, "[play] 停机原因不是正常退出：reason=%d opcode=0x%X %s\n",
                         static_cast<int>(reason), stop.opcode, stop.message.c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace app
