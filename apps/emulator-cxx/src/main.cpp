// emulator-cxx —— 原生宿主入口：**只有 CLI 解析与分派**。
//
// 实现在 app/ 的那几个 TU 里（app.h 是它们之间的唯一接口）：
//   gpu_device.cpp 设备/读回/上传 · gpu_quad.cpp 四边形 · gpu_scene.cpp 场景批次
//   smoke.cpp --smoke · boot.cpp boot 流程 · window.cpp 窗口呈现 · play_headless.cpp 无窗口播放
//   input_script.cpp 脚本化输入（`--input-script`）
//
// ★ 唯一的游戏流程是从根脚本跑 boot；没有"直接跳到 TITLE"之类的捷径。
// ★ `--input-script` / `--play-headless` / `--expect-script` 都是**宿主的测试装置**
//   （平台边界替身），不改变 `data/spec` 的任何合同。

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace app;

int main(int argc, char** argv) {
#ifdef _WIN32
    // 引擎字符串在核心内部是 CP932 字节；显示前统一解码成 UTF-8，控制台也切 UTF-8。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    bool smoke = false;
    InstallRunOptions options;
    std::string agf_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            options.install_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--opcode-table") == 0 && i + 1 < argc) {
            options.table_path = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            options.config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            options.trace_steps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--footprint") == 0) {
            options.footprint = true;
        } else if (std::strcmp(argv[i], "--skip-logo") == 0) {
            options.skip_logo = true;
        } else if (std::strcmp(argv[i], "--play-frames") == 0 && i + 1 < argc) {
            options.play_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--render-logo-gpu") == 0 && i + 1 < argc) {
            options.logo_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--logo-stride") == 0 && i + 1 < argc) {
            options.logo_stride = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--logo-frames") == 0 && i + 1 < argc) {
            options.logo_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--stop-at") == 0 && i + 1 < argc) {
            options.stop_at = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            options.max_steps = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--config-keys") == 0 && i + 1 < argc) {
            options.config_keys_path = argv[++i];
        } else if (std::strcmp(argv[i], "--agf") == 0 && i + 1 < argc) {
            agf_path = argv[++i];
        } else if (std::strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
            options.input_script = argv[++i];
        } else if (std::strcmp(argv[i], "--play-headless") == 0) {
            options.headless = true;
        } else if (std::strcmp(argv[i], "--expect-script") == 0 && i + 1 < argc) {
            options.headless_play.expect_script =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--anchor-script") == 0 && i + 1 < argc) {
            options.headless_play.anchor_script =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--require-script") == 0 && i + 1 < argc) {
            options.headless_play.require_script =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--tick-ms") == 0 && i + 1 < argc) {
            options.headless_play.tick_ms = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--bmp") == 0 && i + 1 < argc) {
            options.headless_play.bmp_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--bmp-stride") == 0 && i + 1 < argc) {
            options.headless_play.bmp_stride = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--bmp-limit") == 0 && i + 1 < argc) {
            options.headless_play.bmp_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--trace-script") == 0 && i + 1 < argc) {
            options.headless_play.trace_script =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--trace-script-steps") == 0 && i + 1 < argc) {
            options.headless_play.trace_script_steps =
                static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--trace-opcodes") == 0 && i + 1 < argc) {
            const char* cursor = argv[++i];
            while (cursor != nullptr && *cursor != '\0') {
                char* end = nullptr;
                const unsigned long value = std::strtoul(cursor, &end, 0);
                if (end == cursor) break;
                options.headless_play.trace_opcodes.push_back(static_cast<std::uint32_t>(value));
                cursor = (*end == ',') ? end + 1 : end;
            }
        } else if (std::strcmp(argv[i], "--sweep-missing") == 0) {
            options.headless_play.sweep_missing = true;
        } else if (std::strcmp(argv[i], "--dump-config") == 0 && i + 1 < argc) {
            options.headless_play.dump_config = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-scene-at") == 0 && i + 1 < argc) {
            options.headless_play.dump_scene_at = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--dump-texture") == 0 && i + 1 < argc) {
            options.headless_play.dump_texture_id =
                static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--dump-texture-out") == 0 && i + 1 < argc) {
            options.headless_play.dump_texture_out = argv[++i];
        } else if (std::strcmp(argv[i], "--watch-locals") == 0 && i + 1 < argc) {
            // 逗号分隔的局部整型格编号（诊断口：看脚本把输入读成了什么）。
            const char* cursor = argv[++i];
            while (cursor != nullptr && *cursor != '\0') {
                char* end = nullptr;
                const unsigned long value = std::strtoul(cursor, &end, 0);
                if (end == cursor) break;
                options.headless_play.watch_locals.push_back(static_cast<std::uint32_t>(value));
                cursor = (*end == ',') ? end + 1 : end;
            }
        }
    }

    if (!agf_path.empty()) return run_core_agf(agf_path);

    if (!options.install_dir.empty()) {
        const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
        if (options.table_path.empty()) options.table_path = (exe_dir / "opcode-table.json").string();
        if (options.config_keys_path.empty()) {
            options.config_keys_path = (exe_dir / "config-keys.json").string();
        }
        return run_core_install(options);
    }

    if (!smoke) {
        std::fprintf(stderr,
                     "用法：emulator-cxx --install <dir> [--play-frames N] [--trace N] [--footprint] "
                     "[--skip-logo] [--stop-at <opcode>] [--config <SYS4REG.INI>]\n"
                     "      emulator-cxx --install <dir> --render-logo-gpu <dir> "
                     "[--logo-stride N] [--logo-frames N]\n"
                     "      emulator-cxx --install <dir> --play-headless [--input-script <file>] "
                     "[--anchor-script <id>] [--expect-script <id>] [--tick-ms N] [--bmp <dir>]\n"
                     "      （诊断）--play-headless --sweep-missing：缺 handler 的 opcode 记账后跳过\n"
                     "      emulator-cxx --smoke\n"
                     "      emulator-cxx --agf <file>\n"
                     "★ 没有「直接跳到 TITLE」之类的捷径：唯一的游戏流程是从根脚本跑 boot。\n"
                     "★ --input-script / --play-headless 是宿主测试装置（平台边界替身），不是引擎语义。\n");
        return 1;
    }

    SDL_SetAppMetadata("emulator-cxx", "0.1.0", "dev.amayui.emulator-cxx");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_sdl_error("SDL_Init");
        return 1;
    }

    if (!init_shader_toolchain()) {
        SDL_Quit();
        return 1;
    }

    SDL_GPUDevice* device = create_gpu_device();
    if (device == nullptr) {
        quit_shader_toolchain();
        SDL_Quit();
        return 1;
    }

    const SmokeResult result = run_smoke(device);
    std::printf(
        "[emulator-cxx] smoke: %s (%s)\n",
        result.ok ? "PASS" : "FAIL",
        result.detail.c_str());
    SDL_DestroyGPUDevice(device);
    quit_shader_toolchain();
    SDL_Quit();
    return result.ok ? 0 : 1;
}
