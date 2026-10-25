// emulator-cxx —— 原生宿主入口：**只有 CLI 解析与分派**。
//
// 实现在 app/ 的那几个 TU 里（app.h 是它们之间的唯一接口）：
//   gpu_device.cpp 设备/读回/上传 · gpu_quad.cpp 四边形 · gpu_scene.cpp 场景批次
//   smoke.cpp --smoke · boot.cpp boot 流程 · window.cpp 窗口呈现
//
// ★ 唯一的游戏流程是从根脚本跑 boot；没有"直接跳到 TITLE"之类的捷径。

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
    std::string install_dir;
    std::string table_path;
    std::string config_path;
    std::string config_keys_path;
    std::string agf_path;
    std::size_t max_steps = 200000;
    std::uint32_t stop_at = 0;
    int trace_steps = 0;
    int play_frames = 0;
    std::string logo_dir;
    int logo_stride = 24;
    int logo_frames = 12;
    bool footprint = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) {
            install_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--opcode-table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_steps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--footprint") == 0) {
            footprint = true;
        } else if (std::strcmp(argv[i], "--play-frames") == 0 && i + 1 < argc) {
            play_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--render-logo-gpu") == 0 && i + 1 < argc) {
            logo_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--logo-stride") == 0 && i + 1 < argc) {
            logo_stride = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--logo-frames") == 0 && i + 1 < argc) {
            logo_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--stop-at") == 0 && i + 1 < argc) {
            stop_at = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--config-keys") == 0 && i + 1 < argc) {
            config_keys_path = argv[++i];
        } else if (std::strcmp(argv[i], "--agf") == 0 && i + 1 < argc) {
            agf_path = argv[++i];
        }
    }

    if (!agf_path.empty()) return run_core_agf(agf_path);

    if (!install_dir.empty()) {
        const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
        if (table_path.empty()) table_path = (exe_dir / "opcode-table.json").string();
        if (config_keys_path.empty()) config_keys_path = (exe_dir / "config-keys.json").string();
        return run_core_install(install_dir, table_path, config_path, config_keys_path, max_steps,
                                stop_at, trace_steps, play_frames, logo_dir, logo_stride,
                                logo_frames, footprint);
    }

    if (!smoke) {
        std::fprintf(stderr,
                     "用法：emulator-cxx --install <dir> [--play-frames N] [--trace N] [--footprint] "
                     "[--stop-at <opcode>] [--config <SYS4REG.INI>]\n"
                     "      emulator-cxx --install <dir> --render-logo-gpu <dir> "
                     "[--logo-stride N] [--logo-frames N]\n"
                     "      emulator-cxx --smoke\n"
                     "      emulator-cxx --agf <file>\n"
                     "★ 没有「直接跳到 TITLE」之类的捷径：唯一的游戏流程是从根脚本跑 boot。\n");
        return 1;
    }

    SDL_SetAppMetadata("emulator-cxx", "0.1.0", "dev.amayui.emulator-cxx");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log_sdl_error("SDL_Init");
        return 1;
    }

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    if (!SDL_ShaderCross_Init()) {
        log_sdl_error("SDL_ShaderCross_Init");
        SDL_Quit();
        return 1;
    }
#endif

    SDL_GPUDevice* device = create_gpu_device();
    if (device == nullptr) {
        SDL_Quit();
        return 1;
    }

    const SmokeResult result = run_smoke(device);
    std::printf(
        "[emulator-cxx] smoke: %s (%s)\n",
        result.ok ? "PASS" : "FAIL",
        result.detail.c_str());
    SDL_DestroyGPUDevice(device);
#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
    SDL_ShaderCross_Quit();
#endif
    SDL_Quit();
    return result.ok ? 0 : 1;
}

