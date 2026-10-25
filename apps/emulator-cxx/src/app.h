// emulator-cxx 的**宿主内部 API**（不是 core，也不是公共库）。
//
// 分层（`main.cpp` 只剩 CLI 解析 + 分派）：
//   app.h           这份声明：常量 / 小类型 / 跨 TU 的函数
//   gpu_device.cpp  SDL_GPU 设备、读回、纹理上传、shader 编译
//   gpu_quad.cpp    简单贴图四边形（软件回退 / 冒烟测试用）
//   gpu_scene.cpp   场景批次：顶点构建、按 revision 缓存纹理、离屏导出
//   smoke.cpp       `--smoke` 的三个离屏自检（测试，不是产品路径）
//   boot.cpp        boot 流程（根脚本 → LOGO → 影片 → TITLE）与单 AGF 解码
//   window.cpp      窗口播放循环（按 core 的渲染请求呈现）
//
// ⛔ 这里不放"引擎语义"：引擎状态全在 `core::Machine`，本层只做 SDL / 呈现。
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

// ── 四边形管线（软件回退 / 冒烟用）──
struct QuadPipeline {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUBuffer* vertex_buffer = nullptr;
    SDL_GPUTexture* texture = nullptr;
};

#ifdef EMULATOR_CXX_HAVE_SHADERCROSS
SDL_GPUShader* compile_shader(SDL_GPUDevice* device, const char* source, const char* entrypoint,
                              SDL_ShaderCross_ShaderStage stage, Uint32 num_samplers);
// 四边形的 HLSL 源码（gpu_quad.cpp 定义；冒烟测试也用它验 shadercross 编译链）。
extern const char* const kVertexShader;
extern const char* const kFragmentShader;
#endif

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
int run_core_agf(const std::string& agf_path);
int run_core_install(const std::string& install_dir, const std::string& table_path,
                     const std::string& config_path, const std::string& config_keys_path,
                     std::size_t max_steps, std::uint32_t stop_at, int trace_steps, int play_frames,
                     const std::string& logo_dir, int logo_stride, int logo_frames,
                     bool footprint = false);
int run_windowed_frames(SDL_GPUDevice* device, SDL_Window* window, core::Machine& machine,
                        int width, int height, int max_frames);

}  // namespace app
