// boot 流程：安装目录 → SYS4INI.BIN 索引 → 根脚本 → 窗口播放（或影片逐帧导出）。
//
// ★ 唯一的游戏流程就是它：没有"直接跳到 TITLE"的捷径。
#include <SDL3/SDL.h>

#include "app.h"
#include "input_script.h"

#include "core/agf.h"
#include "core/config.h"
#include "core/encoding.h"
#include "core/install.h"
#include "core/opcode_table.h"
#include "core/script.h"
#include "core/vm.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace app {

int run_core_agf(const std::string& agf_path) {
    try {
        std::ifstream input(core::path_from_utf8(agf_path), std::ios::binary);
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

int run_core_install(const InstallRunOptions& options) {
    const std::string& install_dir = options.install_dir;
    const std::string& table_path = options.table_path;
    const std::string& config_path = options.config_path;
    const std::string& config_keys_path = options.config_keys_path;
    const std::size_t max_steps = options.max_steps;
    const std::uint32_t stop_at = options.stop_at;
    const int trace_steps = options.trace_steps;
    const int play_frames = options.play_frames;
    const std::string& logo_dir = options.logo_dir;
    const int logo_stride = options.logo_stride;
    const int logo_frames = options.logo_frames;
    const bool footprint = options.footprint;
    const bool skip_logo = options.skip_logo;
    try {
        const core::Install install = core::open_install(core::path_from_utf8(install_dir));
        const auto& index = install.index;
        std::printf(
            "[core] alf signature=%s title=%s zone=%d archives=%zu files=%zu configDefaults=%zu\n",
            index.signature.c_str(),
            core::cp932_to_utf8(index.title).c_str(),
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
            core::cp932_to_utf8(root_record.name).c_str(),            root_record.archive_index,
            root_record.length,
            root.size());

        const core::ScriptHeader header = core::parse_script_header(root);
        const core::OpcodeTable table = core::OpcodeTable::load_from_file(core::path_from_utf8(table_path));
        core::ConfigVocabulary vocabulary = core::ConfigVocabulary::load_from_file(core::path_from_utf8(config_keys_path));
        core::ConfigStore config(std::move(vocabulary));
        config.load_defaults(index.config_defaults);
        std::filesystem::path config_root = install.root;
        if (config.get_int("set:UseAppDataFolder") != 0) {
#ifdef _WIN32
            wchar_t appdata[32768]{};
            const DWORD appdata_length =
                GetEnvironmentVariableW(L"APPDATA", appdata, static_cast<DWORD>(32768));
            if (appdata_length > 0 && appdata_length < 32768) {
                config_root = std::filesystem::path(appdata);
            }
#else
            if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr) {
                config_root = core::path_from_utf8(xdg);
            } else if (const char* home = std::getenv("HOME"); home != nullptr) {
                config_root = core::path_from_utf8(home) / ".config";
            }
#endif
        }
        std::filesystem::path player_path = config_path.empty()
            ? config_root / core::path_from_engine(config.get_string("set:RegFilePath")) / "SYS4REG.INI"
            : core::path_from_utf8(config_path);
        config.load_player_file(player_path);
        std::printf("[core] drawMode=%d named=%d\n",
                    config.get_int("set:DrawMode"),
                    config.has_named("set:DrawMode") ? 1 : 0);
        std::printf(
            "[core] config named=%zu script=%zu player=%s\n",
            config.named_size(),
            config.script_size(),
            core::path_to_utf8(player_path).c_str());
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
        // `--skip-logo`：根脚本用 `0x130` 读这个格决定是否 `call-script 5262`（LOGO）。
        // 这是引擎自身的 ShowLogo 通路，不是"直接跳到 TITLE"的宿主捷径。
        machine.set_show_logo(!skip_logo);
        machine.load_root(root, 0);
        machine.stop_at_opcode(stop_at);
        // 脚本化输入（宿主测试装置）：把 `--input-script` 归约成引擎要的四个量。
        // ★ 无脚本 ⇒ 不接任何输入源（`0xff` 采样恒 0、`0x109` 恒 −100000），与窗口路径"没有设备"同口径。
        std::optional<ScriptedInput> scripted;
        if (!options.input_script.empty()) {
            scripted.emplace(load_input_script(options.input_script));
            scripted->bind(machine);
            std::printf("[core] input-script events=%zu last=%ums\n", scripted->size(),
                        scripted->last_time_ms());
        }
        // 无窗口影片校验：从根脚本跑到 LOGO 的 `0x20f`，以后端时间推进，逐帧导出 GPU 合成。
        // `--logo-frames 0` = 不导出、只跑到影片收尾（CTest 用的确定性判据）。
        if (!logo_dir.empty()) {
            std::filesystem::create_directories(core::path_from_utf8(logo_dir));
            std::size_t steps = 0;
            while (!machine.halted() && !machine.movie_active() && steps < max_steps) {
                if (!machine.step()) break;
                ++steps;
            }
            std::printf("[movie] start steps=%zu clock=%ums script=%u active=%d\n", steps,
                        machine.scene_clock_ms(), machine.frame().script_id,
                        machine.movie_active() ? 1 : 0);
            if (!machine.movie_active()) {
                std::fprintf(stderr, "[movie] LOGO 影片没有启动（未到 0x20f 或绑定门失败）\n");
                return 1;
            }
            const bool dump = logo_frames > 0;
            bool ok = true;
            SDL_GPUDevice* device = nullptr;
            if (dump) {
                if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(logo-gpu)"); return 1; }
                if (!init_shader_toolchain()) {
                    SDL_Quit();
                    return 1;
                }
                device = create_gpu_device();
                if (device == nullptr) { quit_shader_toolchain(); SDL_Quit(); return 1; }
            }
            int requests = 0;
            int dumped = 0;
            while (ok && !machine.halted() && machine.movie_active()) {
                bool requested = false;
                for (int i = 0; i < 20000; ++i) {
                    if (!machine.step()) break;
                    if (machine.consume_frame_step()) { requested = true; break; }
                    if (!machine.movie_active()) break;
                }
                if (!requested) break;
                const int index = requests++;
                if (!dump) { machine.end_render(); continue; }
                if (index % (logo_stride > 0 ? logo_stride : 1) != 0) {
                    machine.end_render();
                    continue;
                }
                // 采样影片纹理的平均亮度：全黑 / 全同色的回归在这里就能看出来。
                double mean = 0.0;
                if (const core::TextureRecord* texture = machine.texture(42);
                    texture != nullptr && texture->rgba.size() >= 4) {
                    const std::size_t step = (texture->rgba.size() / 4 / 997) * 4;
                    std::uint64_t sum = 0;
                    std::size_t n = 0;
                    for (std::size_t i = 0; i + 3 < texture->rgba.size() && step > 0; i += step) {
                        sum += texture->rgba[i] + texture->rgba[i + 1] + texture->rgba[i + 2];
                        ++n;
                    }
                    mean = n > 0 ? static_cast<double>(sum) / static_cast<double>(n * 3) : 0.0;
                }
                char name[512];
                std::snprintf(name, sizeof(name), "%s/logo-%03d.bmp", logo_dir.c_str(), index);
                if (!render_scene_gpu_to_bmp(device, machine, name, 1280, 720)) {
                    ok = false;
                    break;
                }
                machine.end_render();
                ++dumped;
                std::printf("[movie] frame %d clock=%ums mean=%.1f -> %s\n", index,
                            machine.scene_clock_ms(), mean, name);
                if (dumped >= logo_frames) break;
            }
            std::printf("[movie] logo done requests=%d dumped=%d clock=%ums active=%d\n", requests,
                        dumped, machine.scene_clock_ms(), machine.movie_active() ? 1 : 0);
            if (device != nullptr) SDL_DestroyGPUDevice(device);
            if (dump) { quit_shader_toolchain(); SDL_Quit(); }
            if (!ok) return 1;
            // 判据：影片真的播了（至少一帧）且走到了收尾（导出模式按帧数截断时不要求）。
            if (requests < 1) { std::fprintf(stderr, "[movie] 一帧都没有请求\n"); return 1; }
            if (!dump && machine.movie_active()) {
                std::fprintf(stderr, "[movie] 影片没有走到收尾\n");
                return 1;
            }
            return 0;
        }
        if (footprint) {
            // 诊断口（`--footprint`）：跑到 `--max-steps` 为止，按**首次出现顺序**汇总
            // 「这段流程实际执行过哪些 opcode」。★ 这是"执行到 TITLE 用到的指令"的机械判据，
            // 不是静态足迹：只统计真的被取指的那一条。
            struct Occ {
                std::size_t steps = 0;
                std::uint32_t first_script = 0;
                std::size_t first_step = 0;
            };
            std::vector<std::uint32_t> order;
            std::map<std::uint32_t, Occ> seen;
            std::vector<std::uint32_t> scripts;
            std::size_t steps = 0;
            while (steps < max_steps) {
                const auto& code = machine.frame().code;
                const auto ip = machine.frame().ip;
                if (ip + 4 > code.size()) break;
                const std::uint32_t opcode = core::read_u32(code, ip);
                const std::uint32_t script = machine.frame().script_id;
                if (scripts.empty() || scripts.back() != script) scripts.push_back(script);
                auto it = seen.find(opcode);
                if (it == seen.end()) {
                    it = seen.emplace(opcode, Occ{0, script, steps}).first;
                    order.push_back(opcode);
                }
                ++it->second.steps;
                if (!machine.step()) break;
                ++steps;
            }
            const auto& stop = machine.last_stop();
            std::printf("[footprint] steps=%zu opcodes=%zu scripts=%zu stop=%d script=%u opcode=0x%X\n",
                        steps, order.size(), scripts.size(), static_cast<int>(stop.reason),
                        machine.frame().script_id, stop.opcode);
            std::printf("[footprint] script-chain:");
            for (const auto script : scripts) std::printf(" %u", script);
            std::printf("\n");
            for (const auto opcode : order) {
                const auto& occ = seen[opcode];
                const core::OpcodeRow* row = table.find(opcode);
                std::printf("[footprint] 0x%03X %-18s count=%-7zu first=script:%u@%zu\n", opcode,
                            row != nullptr ? row->name.c_str() : "?", occ.steps,
                            occ.first_script, occ.first_step);
            }
            return 0;
        }
        if (trace_steps > 0) {
            for (int i = 0; i < trace_steps; ++i) {
                const auto& code = machine.frame().code;
                const auto ip = machine.frame().ip;
                if (ip + 4 > code.size()) break;
                std::printf("[trace] #%d script=%u ip=%zu opcode=0x%X flags=0x%X\n", i,
                            machine.frame().script_id, ip, core::read_u32(code, ip),
                            machine.state_flags());
                if (!machine.step()) break;
            }
            const auto& stop = machine.last_stop();
            std::printf("[trace] stop reason=%d steps=%zu script=%u ip=%zu opcode=0x%X message=%s\n",
                        static_cast<int>(stop.reason), stop.steps, machine.frame().script_id,
                        stop.offset, stop.opcode, stop.message.c_str());
            return 0;  // 跟踪是 debug 口：给了帧数就只跟踪、不开窗（否则会一直跑下去）
        }

        // 唯一流程：从根脚本走完整 boot（LOGO → 影片 → TITLE），窗口按宿主真实时钟推进。
        // 颜色窗 / 影片都用宿主时钟，所以这里先把场景时钟对齐到 1000ms 再开窗。
        machine.reset_scene_clock(1000);
        machine.set_host_clock_ms(1000);
        if (options.headless) {
            // 无窗口播放（`--play-headless`）：同一套门 / 输入注入，呈现换成可选离屏导出。
            HeadlessPlayOptions play = options.headless_play;
            play.max_frames = options.play_frames > 0 ? options.play_frames : play.max_frames;
            if (play.limit_steps == 0) play.limit_steps = max_steps;
            const int code = run_headless_play(machine, scripted ? &*scripted : nullptr, play,
                                               kWidth, kHeight);
            if (machine.halted()) {
                const auto& stop = machine.last_stop();
                std::printf("[core] exit halted=1 reason=%d opcode=0x%X message=%s\n",
                            static_cast<int>(stop.reason), stop.opcode, stop.message.c_str());
            }
            return code;
        }
        if (!SDL_Init(SDL_INIT_VIDEO)) { log_sdl_error("SDL_Init(boot)"); return 1; }
        SDL_GPUDevice* device = create_gpu_device();
        SDL_Window* window = SDL_CreateWindow("emulator-cxx — boot", kWidth, kHeight, SDL_WINDOW_RESIZABLE);
        bool user_quit = false;
        if (device != nullptr && window != nullptr) {
            run_windowed_frames(device, window, machine, kWidth, kHeight, play_frames, &user_quit,
                                scripted ? &*scripted : nullptr);
        }
        // 引擎退出时保存玩家配置（`Reg::Save`）：窗口 Quit、根帧 `exit`、`abort`、错误对话框选“终止”
        // 都经主循环的退出捕获发送关闭消息，进入同一收尾；`--play-frames N` 的帧数上限不是引擎退出。
        // `machine.halted()` 是这些引擎退出在模拟器里的并集；host-only 的 unsupported-opcode halt 也保存，
        // 这是保守选择，不是引擎证据。
        const bool save_config = user_quit || machine.halted();
        const auto& stop = machine.last_stop();
        std::printf("[core] exit user_quit=%d halted=%d reason=%d opcode=0x%X message=%s\n",
                    user_quit ? 1 : 0, machine.halted() ? 1 : 0,
                    static_cast<int>(stop.reason), stop.opcode, stop.message.c_str());
        if (save_config) {
            try {
                machine.config().save_player_file(player_path);
                std::printf("[core] config saved: %s\n", core::path_to_utf8(player_path).c_str());
            } catch (const std::exception& e) {
                std::printf("[core] config save failed: %s\n", e.what());
            }
        }
        if (window != nullptr) SDL_DestroyWindow(window);
        if (device != nullptr) SDL_DestroyGPUDevice(device);
        SDL_Quit();

        const std::size_t show = std::min<std::size_t>(instructions.size(), 8);
        for (std::size_t i = 0; i < show; ++i) {
            const auto& ins = instructions[i];
            std::printf("[core]   #%zu off=0x%zX opcode=0x%X argc=%zu\n", i, ins.offset, ins.opcode,
                        ins.operands.size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[core] FAIL: %s\n", e.what());
        return 1;
    }
}

}  // namespace app
