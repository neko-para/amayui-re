// emulator-cxx 的**宿主内部 API**（不是 core，也不是公共库）。
//
// 分层（`main.cpp` 只剩 CLI 解析 + 分派）：
//   app.h           这份声明：常量 / 小类型 / 跨 TU 的函数
//   gpu_device.cpp  SDL_GPU 设备、读回、纹理上传、着色器路径判定
//   gpu_quad.cpp    简单贴图四边形（冒烟测试 / 兜底）+ 着色器源与编译分派
//   gpu_scene.cpp   场景批次：顶点构建、按 revision 缓存纹理、离屏导出
//   smoke.cpp       `--smoke` 的离屏自检（测试，不是产品路径）
//   boot.cpp        boot 流程（根脚本 → LOGO → 影片 → TITLE）与单 AGF 解码
//   window.cpp      窗口播放循环（按 core 的渲染请求呈现）
//
// ⛔ 这里不放"引擎语义"：引擎状态全在 `core::Machine`，本层只做 SDL / 呈现。
//
// ★ 着色器跨平台口径（三平台都有 GPU 路径，没有"软件回退"这一档）：
//   Windows → HLSL 经 shadercross 编成 DXIL；Linux → 同一份 HLSL 编成 SPIR-V；
//   macOS   → MSL 源文本直接交给 Metal（SDL_GPU_SHADERFORMAT_MSL，不需要 shadercross）。
//   分派在 `compile_shader()`，源在 `gpu_quad.cpp` / `gpu_scene.cpp`。
#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "core/bytes.h"
#include "core/vm.h"

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace app {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// 四边形路径的清屏色（冒烟测试与软件回退共用）。
constexpr SDL_FColor kClearColor{0.039f, 0.051f, 0.086f, 1.0f};

// 四边形默认纹理的 2×2 采样色（冒烟测试拿它断言混合结果）。
constexpr Uint8 kTexR = 20;
constexpr Uint8 kTexG = 10;
constexpr Uint8 kTexB = 5;
constexpr Uint8 kTexA = 128;

void log_sdl_error(const char* what);
SDL_GPUDevice* create_gpu_device();

// ── 读回 ──
struct Readback {
    SDL_GPUTransferBuffer* buffer = nullptr;
    void* mapped = nullptr;
};
Readback readback_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture, int width, int height);
void release_readback(SDL_GPUDevice* device, Readback& readback);
std::array<Uint8, 4> center_pixel(const Readback& readback);

// ── 纹理上传 ──
SDL_GPUTexture* upload_texture(SDL_GPUDevice* device, const core::Bytes& rgba, int width, int height);

// ── 冒烟（`--smoke`）──
struct SmokeResult {
    bool ok = false;
    std::string detail;
};
SmokeResult run_smoke(SDL_GPUDevice* device);

// ── 着色器源（同一对管线的两个语言版本）──
//
// · `hlsl` 给 **SDL_shadercross** 在运行时编成 DXIL（Windows/D3D12）或 SPIR-V（Linux/Vulkan）；
// · `msl`  给 **Metal**（macOS）—— SDL 的 `SDL_GPU_SHADERFORMAT_MSL` 收的是**源码文本**，
//   Metal 后端自己 `newLibraryWithSource:` 编译它 ⇒ macOS 不需要 shadercross。
// ⇒ 两份描述同一个管线，**必须同步改**（细节与理由见 `gpu_quad.cpp` 顶部）。
// ★ 入口名是**约定**：HLSL 侧永远是 `VSMain` / `PSMain`，MSL 侧永远是 `kShaderEntrypoint`。
//   写错就是 DXC / Metal 的编译错误（会带一行 `[emulator-cxx] ...` 打印出来），不会静默。
struct ShaderPair {
    const char* hlsl = nullptr;
    const char* msl = nullptr;
};

// MSL 入口名：与 SDL_shadercross 生成的 MSL 对齐（将来换成生成物时调用点不用动）。
constexpr const char* kShaderEntrypoint = "main0";

// 按设备**实际支持**的着色器格式挑一套源并建 shader（DXIL → SPIR-V → MSL 依次尝试）。
// 失败返回 nullptr 并打一行可读原因（这台设备的格式不在支持列表里也走这条）。
SDL_GPUShader* compile_shader(SDL_GPUDevice* device, const ShaderPair& shaders,
                              SDL_GPUShaderStage stage, Uint32 num_samplers);

// shadercross 工具链的生命周期：只有编译 HLSL 的平台需要；其它平台是 no-op（macOS 走 MSL）。
bool init_shader_toolchain();
void quit_shader_toolchain();

// 这台设备最后会走哪条着色器路径（日志 / 冒烟用）："dxil" / "spirv" / "msl" / "unsupported"。
const char* shader_backend_name(SDL_GPUDevice* device);

// ── 四边形管线（冒烟用 / 场景批次不可用时的兜底）──
struct QuadPipeline {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTexture* texture = nullptr;
};

bool create_quad_pipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat target_format,
                          SDL_GPUBlendOp color_op, SDL_GPUBlendFactor src_factor,
                          SDL_GPUBlendFactor dst_factor, QuadPipeline& out);
void destroy_quad_pipeline(SDL_GPUDevice* device, QuadPipeline& pipeline);
bool draw_quad(SDL_GPUCommandBuffer* command, SDL_GPUTexture* target, const QuadPipeline& pipeline);

// ── 场景批次（绘制项 / 网格）──
struct SceneVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct SceneRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t texture = 0;  // 0 = 内置白纹理（网格）
};

struct ScenePipeline {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;
};

struct SceneTextureCache {
    std::unordered_map<std::uint32_t, SDL_GPUTexture*> textures;
    std::unordered_map<std::uint32_t, std::uint32_t> revisions;
    SDL_GPUTexture* white = nullptr;
};

bool create_scene_pipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat format, ScenePipeline& out);
void destroy_scene_pipeline(SDL_GPUDevice* device, ScenePipeline& pipeline);
void release_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache);
SDL_GPUTexture* scene_texture(SDL_GPUDevice* device, SceneTextureCache& cache,
                              const core::Machine& machine, std::uint32_t id);
void preload_scene_textures(SDL_GPUDevice* device, SceneTextureCache& cache,
                            const core::Machine& machine);
bool draw_scene_gpu(SDL_GPUDevice* device, SDL_GPUCommandBuffer* command, SDL_GPUTexture* target,
                    const core::Machine& machine, const ScenePipeline& pipeline,
                    SceneTextureCache& cache, int width, int height, bool clear);
bool render_scene_gpu_to_bmp(SDL_GPUDevice* device, const core::Machine& machine,
                             const std::string& path, int width, int height);

// ── 流程 ──

// 脚本化输入（`--input-script`）：文件落点由宿主读、事件按仿真毫秒应用。
// 定义在 `input_script.h`；这里是前向声明，避免 app.h 拖进 core 的输入接口。
class ScriptedInput;

// 无窗口「按帧推进」的选项（`--play-headless`）：给 CTest 一个确定性判据。
struct HeadlessPlayOptions {
    std::uint32_t tick_ms = 16;      // 每次外层迭代推进的仿真毫秒（不是墙钟）
    std::uint32_t expect_script = 0; // 非 0 ⇒ 进入该脚本即算达成（退出码 0），否则算失败
    std::uint32_t require_script = 0;// 非 0 ⇒ **收口时**当前脚本必须仍是它（否则算失败）
    std::uint32_t anchor_script = 0; // 非 0 ⇒ 输入脚本的时刻以**首次进入该脚本**为 0 点
    int max_frames = 0;              // 0 = 不限（直到达成 / 停机 / 步数上限）
    std::size_t limit_steps = 0;     // 0 = 不限；否则到点收口并打印（不是引擎停因）
    std::string bmp_dir;             // 非空 ⇒ 每次渲染请求导出一张 BMP
    int bmp_stride = 1;
    int bmp_limit = 0;               // 0 = 不限
    // 诊断：观察这些**当前帧局部整型格**，值变化时打一行（`--watch-locals 1008,1013`）。
    // ★ 宿主观察口，不改引擎状态；用途是"输入到底被脚本读成了什么"。
    std::vector<std::uint32_t> watch_locals;
    // 诊断：只跟踪某个脚本 id 的取指（`--trace-script 21092 --trace-script-steps 200`）。
    // ⛔ 这是 debug 口，不是产品路径。
    std::uint32_t trace_script = 0;
    std::size_t trace_script_steps = 0;
    // 诊断：只打这些 opcode 的取指（空 = 不打）。与 `trace_script` 叠加生效。
    std::vector<std::uint32_t> trace_opcodes;
};

// 一次 `--install` 运行的全部输入（CLI 解析只在 main.cpp，这里只是搬运）。
struct InstallRunOptions {
    std::string install_dir;
    std::string table_path;
    std::string config_path;
    std::string config_keys_path;
    std::size_t max_steps = 200000;
    std::uint32_t stop_at = 0;
    int trace_steps = 0;
    int play_frames = 0;
    std::string logo_dir;
    int logo_stride = 24;
    int logo_frames = 12;
    bool footprint = false;
    bool skip_logo = false;
    std::string input_script;    // 空 = 没有输入设备（与窗口路径的取舍一致）
    bool headless = false;       // true = `--play-headless`：不开窗、按仿真节拍推进
    HeadlessPlayOptions headless_play;
};

int run_core_agf(const std::string& agf_path);
int run_core_install(const InstallRunOptions& options);
int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames, bool* user_quit = nullptr,
                        ScriptedInput* scripted = nullptr);
// 无窗口播放：与窗口路径同一个门 / 同一套输入注入，只是呈现换成离屏导出（可关）。
int run_headless_play(core::Machine& machine, ScriptedInput* scripted,
                      const HeadlessPlayOptions& options, int width, int height);

}  // namespace app
