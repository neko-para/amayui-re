// 无窗口「按帧推进」的播放路径（`--play-headless`）：与窗口路径共用同一个门、同一套输入注入，
// 只是把呈现换成可选的离屏 BMP 导出。
//
// ★ 它**不是**"跳到某个画面"的捷径：仍然从根脚本跑完整 boot（`boot.cpp` 已经装载好根帧）。
// ★ 时钟是**仿真**的：每外层迭代 `tick_ms` 毫秒（与窗口路径"一次迭代一批、墙钟推进"同形），
//   所以输入脚本的 `at` 是确定的 —— 这是把 TITLE 的输入环变成可回归判据的前提。
#include <SDL3/SDL.h>

#include "app.h"
#include "input_script.h"

#include "core/encoding.h"
#include "core/scene_raster.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace app {
namespace {

// 把一张纹理记录写成 24 位 BMP（bottom-up BGR）—— 诊断用（看"那一格纹理到底是什么"）。
bool write_texture_raw(const core::TextureRecord& texture, const std::string& path) {
    if (texture.width == 0 || texture.height == 0 || texture.rgba.size() < texture.width * texture.height * 4u) return false;
    std::ofstream output(core::path_from_utf8(path), std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(texture.rgba.data()), static_cast<std::streamsize>(texture.rgba.size()));
    return output.good();
}

bool write_texture_bmp(const core::TextureRecord& texture, const std::string& path) {
    if (texture.width == 0 || texture.height == 0 || texture.rgba.size() < texture.width * texture.height * 4u) {
        return false;
    }
    const std::uint32_t row = texture.width * 3u;
    const std::uint32_t pad = (4u - (row % 4u)) % 4u;
    const std::uint32_t image = (row + pad) * texture.height;
    std::vector<std::uint8_t> header(54, 0);
    header[0] = 'B';
    header[1] = 'M';
    const std::uint32_t size = 54u + image;
    header[2] = static_cast<std::uint8_t>(size);
    header[3] = static_cast<std::uint8_t>(size >> 8);
    header[4] = static_cast<std::uint8_t>(size >> 16);
    header[5] = static_cast<std::uint8_t>(size >> 24);
    header[10] = 54;
    header[14] = 40;
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        header[offset] = static_cast<std::uint8_t>(value);
        header[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        header[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        header[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put32(18, texture.width);
    put32(22, texture.height);
    header[26] = 1;   // biPlanes
    header[28] = 24;  // biBitCount
    put32(34, image);
    std::ofstream output(core::path_from_utf8(path), std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(header.data()), 54);
    std::vector<std::uint8_t> line(row + pad, 0);
    for (std::uint32_t y = texture.height; y-- > 0;) {
        for (std::uint32_t x = 0; x < texture.width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y) * texture.width + x) * 4u;
            line[x * 3u + 0] = texture.rgba[source + 2];
            line[x * 3u + 1] = texture.rgba[source + 1];
            line[x * 3u + 2] = texture.rgba[source + 0];
        }
        output.write(reinterpret_cast<const char*>(line.data()), row + pad);
    }
    return output.good();
}

}  // namespace

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
            if (options.dump_scene_at == index) {
                std::printf("[scene] frame=%d\n", index);
                for (const auto& [key, item] : machine.draw_items()) {
                    std::printf(
                        "[scene] item key=%d flags=%u tex=%u src=(%d,%d,%d,%d) aux=(%.1f,%.1f,%.1f) color=%08X\n",
                        static_cast<std::int32_t>(key), item.flags, item.texture_id, item.left, item.top,
                        item.right, item.bottom, item.aux[0], item.aux[1], item.aux[2], item.color);
                }
                for (const auto& [key, mesh] : machine.meshes()) {
                    std::printf("[scene] mesh key=%d flags=%u count=%d op10=%d color=%08X\n",
                                static_cast<std::int32_t>(key), mesh.flags, mesh.count, mesh.op10,
                                mesh.color);
                    std::printf("[scene]   copies0:");
                    for (const auto value : mesh.copies[0]) std::printf(" %u", value);
                    std::printf("\n[scene]   copies1:");
                    for (const auto value : mesh.copies[1]) std::printf(" %u", value);
                    std::printf("\n[scene]   colors:");
                    for (const auto value : mesh.colors) std::printf(" %08X", value);
                    std::printf("\n");
                }
            }
            const bool want = !options.bmp_dir.empty() &&
                              (options.bmp_stride <= 1 || index % options.bmp_stride == 0) &&
                              (options.bmp_limit <= 0 || dumped < options.bmp_limit);
            if (want) {
                char name[512];
                std::snprintf(name, sizeof(name), "%s/frame-%04d.bmp", options.bmp_dir.c_str(), index);
                if (std::getenv("EMULATOR_CXX_SOFT_BMP") != nullptr) {
                    std::size_t ncmd = machine.scene_commands().size();
                    std::fprintf(stderr, "[soft] frame=%d t=%u commands=%zu", index, machine.scene_clock_ms(), ncmd);
                    for (const auto& [key, rec] : machine.scene_commands()) {
                        std::fprintf(stderr, " {k=%u kind=%d a=%d/%d b=%d/%d delay=%d dur=%d ts=%u wipe=%d split=%d}",
                                     key, rec.kind, rec.a_start, rec.a_count, rec.b_start, rec.b_count,
                                     rec.delay, rec.duration, rec.timestamp, rec.wipe_type, rec.split);
                    }
                    std::fprintf(stderr, "\n");
                    core::Bytes canvas = core::rasterize_scene(machine, static_cast<std::uint32_t>(width),
                                                               static_cast<std::uint32_t>(height));
                    core::TextureRecord record;
                    record.width = static_cast<std::uint32_t>(width);
                    record.height = static_cast<std::uint32_t>(height);
                    record.rgba = std::move(canvas);
                    if (write_texture_bmp(record, name)) { ++dumped; machine.end_render(); continue; }
                }
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
    if (!machine.errors().empty()) {
        std::printf("[play] errors=%zu\n", machine.errors().size());
        for (std::size_t i = 0; i < machine.errors().size() && i < 8; ++i) {
            std::printf("[play]   error[%zu]=%s\n", i, machine.errors()[i].c_str());
        }
    }
    if (options.dump_texture_id != 0 && !options.dump_texture_out.empty()) {
        const core::TextureRecord* texture = machine.texture(options.dump_texture_id);
        if (texture == nullptr) {
            std::fprintf(stderr, "[play] 纹理 %u 不存在\n", options.dump_texture_id);
        } else if (!write_texture_bmp(*texture, options.dump_texture_out) ||
                   (std::getenv("EMULATOR_CXX_RAW_TEXTURE") != nullptr &&
                    !write_texture_raw(*texture, options.dump_texture_out + ".rgba"))) {
            std::fprintf(stderr, "[play] 纹理 %u 导出失败\n", options.dump_texture_id);
        } else {
            std::printf("[play] texture %u (%ux%u) -> %s\n", options.dump_texture_id, texture->width,
                        texture->height, options.dump_texture_out.c_str());
            // 顺带报一下 alpha 分布：区分"贴图本身是黑的"与"alpha=0 但 RGB=黑"。
            std::size_t zero = 0;
            std::size_t full = 0;
            for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
                if (texture->rgba[i] == 0) ++zero;
                if (texture->rgba[i] == 255) ++full;
            }
            const std::size_t center =
                (static_cast<std::size_t>(texture->height / 2) * texture->width + texture->width / 2) * 4;
            std::printf("[play] texture alpha0=%zu opaque=%zu center=%02X%02X%02X%02X\n", zero, full,
                        texture->rgba[center], texture->rgba[center + 1], texture->rgba[center + 2],
                        texture->rgba[center + 3]);
            for (const auto [px, py] : {std::pair<std::uint32_t, std::uint32_t>{1160, 700},
                                        {1160, 800}, {1160, 900}, {900, 300}}) {
                if (px >= texture->width || py >= texture->height) continue;
                const std::size_t at = (static_cast<std::size_t>(py) * texture->width + px) * 4;
                std::printf("[play] texture (%u,%u)=%02X%02X%02X%02X\n", px, py, texture->rgba[at],
                            texture->rgba[at + 1], texture->rgba[at + 2], texture->rgba[at + 3]);
            }
        }
    }
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
