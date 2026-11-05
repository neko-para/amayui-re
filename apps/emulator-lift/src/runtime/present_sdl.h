// emulator-lift · 真实 SDL 窗口 + GPU API 呈现（可选后端）
//
// ★ 头文件不暴露任何 SDL 类型（pimpl）：main / 测试只看见普通 C++。
// ★ 驱动列表：SDL_CreateRenderer 接受逗号列表；缺省 `gpu,direct3d12,direct3d11,vulkan,metal,opengl`。
//   `gpu` 命中时 `SDL_GetGPURendererDevice()!=NULL` —— 这是“确实走 SDL_GPU”的正证据；
//   其它驱动是“GPU API 路径”，但可能是 WARP/GDI，日志必须如实打 driver。
// ★ 像素格式按 **masks** 选（六种 D3D9 后缓冲格式），`SDL_UpdateTexture` 直接用 guest pitch；
//   `SDL_SetTextureBlendMode(NONE)` 是承重墙（alpha=0 的帧默认混合会整窗黑）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "runtime/present.h"

namespace lift {

struct SdlWindowOptions {
    std::string title = "emulator-lift";
    std::string driver_list = "gpu,direct3d12,direct3d11,vulkan,metal,opengl";
    bool vsync = false;
    bool hidden = false;
    std::string screenshot_path;  // 支持 {frame}
};

class SdlWindow final : public PresentSink {
public:
    SdlWindow();
    ~SdlWindow() override;
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    bool init(const SdlWindowOptions& options, std::string* err);
    void shutdown();

    bool submitFrame(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) override;
    void pumpHostEvents() override;
    bool stopRequested() const override;
    std::string describe() const override;

    // 诊断 / 测试
    bool isGpuRenderer() const;          // SDL_GetGPURendererDevice() != NULL
    const char* rendererName() const;
    void setCaptureForTest(bool on);     // 回读像素到内存（测试用）
    bool readbackPixel(int x, int y, std::uint32_t* argb, std::string* err) const;  // 0xAARRGGBB

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lift
