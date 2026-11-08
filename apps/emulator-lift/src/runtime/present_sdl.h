// emulator-lift · 真实 SDL 窗口 + 呈现（两条路径：GPU 直出 / SDL_Renderer 上传）
//
// ★ 头文件不暴露任何 SDL 类型（pimpl）：main / 测试只看见普通 C++。
// ★ 两条呈现路径，`--present window` 时按**实测**选择（⛔ 不猜）：
//   ① **GPU 直出**（`--raster gpu` 且窗口被 claim 给光栅化的 SDL_GPUDevice）：
//      `SDL_ClaimWindowForGPUDevice` → target 用 `SDL_BlitGPUTexture` 直接拷到 swapchain
//      （letterbox 用 `load_op=CLEAR` 画黑边）。**没有** `SDL_UpdateTexture` / `SDL_RenderTexture`。
//      这条路径下**不创建 SDL_Renderer**（同一个窗口不能既 claim 给 GPU device 又建 renderer）。
//   ② **SDL_Renderer 上传**（缺省；CPU 光栅化 / 直出不可用时的回退）：
//      `SDL_UpdateTexture` + `SDL_RenderTexture` + `SDL_RenderPresent`。
// ★ 驱动列表：`SDL_CreateRenderer` 接受逗号列表；缺省 `gpu,direct3d12,direct3d11,vulkan,metal,opengl`。
//   `gpu` 命中时 `SDL_GetGPURendererDevice()!=NULL` —— 这是"确实走 SDL_GPU"的正证据；
//   其它驱动是"GPU API 路径"，但可能是 WARP/GDI，日志必须如实打 driver。
// ★ 像素格式按 **masks** 选（六种 D3D9 后缓冲格式），SDL_Renderer 路径 `SDL_UpdateTexture` 直接用
//   guest pitch；`SDL_SetTextureBlendMode(NONE)` 是承重墙（alpha=0 的帧默认混合会整窗黑）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/present.h"

namespace lift {

class WindowManager;

struct SdlWindowOptions {
    std::string title = "emulator-lift";
    std::string driver_list = "gpu,direct3d12,direct3d11,vulkan,metal,opengl";
    bool vsync = false;
    bool hidden = false;
    bool raster_gpu = false;  // 同一 SDL_GPUDevice 上的 GPU 光栅化（`--raster gpu`）
    bool raster_sprites = true;  // D3DXSprite 也走 GPU（A/B 可关）
    std::string screenshot_path;  // 支持 {frame}
};

class SdlWindow final : public PresentSink, public GpuRaster {
public:
    SdlWindow();
    ~SdlWindow() override;
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    bool init(const SdlWindowOptions& options, std::string* err);
    void shutdown();

    bool submitFrame(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) override;
    void pumpHostEvents() override;
    // ★ P3：GetMessageA 的事件源。wait_ms>0 时用 SDL_WaitEventTimeout(≈1..10ms) 等一个事件；
    //   没有事件源时 PresentSink 的 0ms 轮询语义不变。
    void pumpHostEvents(int wait_ms);
    bool stopRequested() const override;
    // 把 SDL 事件翻译成 guest 的 Win32 消息（WM_CLOSE / 键鼠 / 滚轮）；窗口关掉后置 stop。
    void attachWindowManager(WindowManager* windows);
    // 测试用：拿 SDL 窗口 ID 构造 WINDOW_CLOSE_REQUESTED。
    std::uint32_t windowIdForTest() const;
    std::string describe() const override;
    GpuRaster* gpuRaster() override;

    // GpuRaster
    bool rasterActive() const override;
    const char* rasterName() const override;
    bool rasterSpritesEnabled() const override;
    bool rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err,
                       const std::uint8_t* color_base = nullptr) override;
    bool rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                             std::string* err) override;
    bool rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                           std::string* err) override;
    bool rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                    std::string* err) override;
    bool rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count,
                            const GpuRasterTexture& texture, const GpuRasterState& state,
                            std::string* err) override;
    // ★ 合批：必须显式转发（基类默认实现走**虚** rasterDraw ⇒ 每条 draw 各自提交，
    //   批处理会"实现了但没生效"，而且测试全绿）。
    bool rasterQueue(const GpuRasterVertex* vertices, std::size_t count,
                     const GpuRasterTexture* texture, const GpuRasterState& state, std::string* err) override;
    bool rasterFlush(std::string* err) override;
    const void* rasterTargetBase() const override;
    std::size_t rasterQueuedDraws() const override;
    void rasterDiscardTarget() override;
    bool rasterDirty() const override;
    // GPU 直出接口转发到真正的后端（`--raster gpu` 的后端在 d3d9_gpu.cpp）。
    GpuDirectPresenter* directPresenter() override;

    // 诊断 / 测试
    // ★ 「走的是不是 SDL_GPU」在两条路径上都成立：renderer 路径看 `SDL_GetGPURendererDevice()`，
    //   直出路径看 swapchain 所用的 GPU device（`present=direct/<mode>` 是正证据）。
    bool isGpuRenderer() const;
    const char* rendererName() const;    // 直出时是 `gpu-direct`
    // GPU 直出是否成立（窗口已 claim 给光栅化的 device）。false ⇒ 呈现走 SDL_Renderer 上传。
    bool directPresentActive() const;
    const char* directPresentMode() const;  // 未直出 ⇒ ""
    void setCaptureForTest(bool on);     // 回读像素到内存（测试用）
    bool readbackPixel(int x, int y, std::uint32_t* argb, std::string* err) const;  // 0xAARRGGBB

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 用 SDL_GPU 设备创建真正的 GPU 光栅化后端（需要 SDL_Init(VIDEO)；
// Windows = DXBC，macOS = MSL/Metal，其它平台明确失败）。
std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err);

}  // namespace lift
