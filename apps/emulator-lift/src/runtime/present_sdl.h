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
    // ★ 左上角调试叠层（`--overlay`，默认关）：鼠标 guest 客户区坐标 / fps / frame / raster / present。
    //   只改**宿主送显的那一份像素** —— ⛔ 不写 guest 内存、⛔ 不进 `--dump-frame`。
    //   两条呈现路径各自绘制（见 present_sdl.cpp）：SDL_Renderer 用小纹理；GPU 直出在
    //   letterbox blit 之后再 blit 一次。
    bool overlay = false;
    std::string screenshot_path;  // 支持 {frame}
    // ★ 只回读**这一帧**（1 起；0 = 每帧都写）。配合 `--max-frames N` 就是"定帧截图"：
    //   `--present-screenshot out.bmp --present-screenshot-frame N --max-frames N` 只产出一张。
    std::uint64_t screenshot_frame = 0;
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
    // ★ 除下面两条**有意的非纯转发**，其余每一条都必须显式转发到 `impl_->raster()`：
    //   · `rasterName()` = init 时缓存的那个后端的名字（生产只有一个后端 ⇒ 与转发观测等价）；
    //   · `rasterSpritesEnabled()` = **壳级** `--raster-sprites` A/B 与后端能力的**合成**。
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
    // ★ 深度清（D3D9 `Clear(..., D3DCLEAR_ZBUFFER)`）的转发。
    //   ⛔ 少了这一行，后端的 D32 就只在"建 target"时清过一次：上一帧末尾那张全屏 **z=0** 的
    //   quad 会把整屏深度写成 0，下一帧所有 `z>0` 的批都 `z <= 0` 判假 ⇒ 整层消失，而且
    //   `fallback` 仍是 0（`REQ-01M73P3GXW22Y467YGYPQSM571` 的实测根因：`--raster gpu` 的
    //   Live2D 角色整层丢失就是这个）。★ 它属于"转发壳漏一个虚函数"这一类错 —— 不改像素、
    //   不报错、测试全绿 ⇒ 守卫必须是"逐条转发"的（`tests/gpu_batch.cpp::caseDepthClearForwarded`）。
    void rasterRequestDepthClear() override;
    // GPU 直出接口转发到真正的后端（`--raster gpu` 的后端在 d3d9_gpu.cpp）。
    GpuDirectPresenter* directPresenter() override;

    // ★ 夹具缝：把 GPU 后端换成**替身**（不建 device、不 claim 窗口）。`nullptr` = 恢复自己那份。
    //   存在理由见上面 `rasterRequestDepthClear` 的注释：转发壳漏一条转发是**静默**的，必须能在
    //   没有 GPU 的机器上逐条断言"调用真的到后端了"。
    //   ⛔ 只许在 `init()` **之前**用（`init()` 之后生产路径手里就是这个壳 ⇒ 换后端会静默改行为；
    //   实现里有**运行时**检查：`init()` 之后调用直接忽略）。替身的生命周期归调用方，本类不持有。
    void setGpuRasterForTest(GpuRaster* raster);

    // 诊断 / 测试
    // ★ 「走的是不是 SDL_GPU」在两条路径上都成立：renderer 路径看 `SDL_GetGPURendererDevice()`，
    //   直出路径看 swapchain 所用的 GPU device（`present=direct/<mode>` 是正证据）。
    bool isGpuRenderer() const;
    // ★ 调试叠层开关（`--overlay`）。`init()` 之后也能改：关掉时立刻把已建的叠层纹理丢掉。
    //   默认关；只影响窗口像素（⛔ 不碰 guest 内存）。
    void setOverlay(bool on);
    bool overlayEnabled() const;
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
