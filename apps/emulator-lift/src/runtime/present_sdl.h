// emulator-lift · 真实 SDL 窗口 + 呈现（两条路径：GPU 直出 / SDL_Renderer 上传）
//
// ★ 头文件不暴露任何 SDL 类型（pimpl）：main / 测试只看见普通 C++。
// ★ 两条呈现路径，`--present window` 时按**实测**选择（⛔ 不猜）：
//   ① **GPU 直出**（窗口被 claim 给光栅化的 SDL_GPUDevice）：
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

#include "runtime/message_box.h"
#include "runtime/present.h"
#include "runtime/ui_input.h"
#include "runtime/window_scale.h"

namespace lift {

class WindowManager;
class FontEngine;

namespace ui {
// ★ C2b-2：叠层脚本的**语言无关**接口（`ui/script.h`）。本头只前向声明 ——
//   `present.h` 不声明 `UiScript`，而 `present_sdl.cpp` 才 include `ui/script.h`。
class UiScript;
}  // namespace ui

struct SdlWindowOptions {
    std::string title = "emulator-lift";
    std::string driver_list = "gpu,direct3d12,direct3d11,vulkan,metal,opengl";
    bool vsync = false;
    bool hidden = false;
    bool raster_gpu = false;  // 同一 SDL_GPUDevice 上的 GPU 光栅化（GPU 是唯一光栅实现）
    // ★ 左上角调试叠层（`--overlay` / `--ui`，默认关）：HUD 由**脚本**（`scripts/ui/hud.js`，
    //   QuickJS 同步跑）产出 display list，文本用**宿主字体**（`FontEngine` / FreeType；支持中文）
    //   按最终像素栅格化，两条呈现路径统一到 `PresentSink::presentOverlay`。只改**宿主送显的那一份
    //   像素** —— ⛔ 不写 guest 内存、⛔ 不进 `--dump-frame`。字体不可用 ⇒ `init()` 硬失败
    //   （R5；⛔ 不静默画空白）。脚本加载失败同样硬失败（AOT 侧 exit 2）。
    bool overlay = false;
    // ★ `--ui <path>`：叠层用**这份** JS 脚本代替默认的 `scripts/ui/hud.js`（空 = 默认）。
    //   ★ 它**不**是总开关 —— 总开关仍是 `overlay`（CLI 里 `--ui` 会一并把 overlay 打开）。
    std::string ui_script;
    // ★ `--ui-input=consume|pass`（S9；D2 / D6 / R3）：**注入点击**（`--input-plan` / 控制层）
    //   是否也过叠层的命中路由。缺省 `Consume` = 命中即吞（down 吞 ⇒ up 吞）；`Pass` = 注入点击
    //   **完全不问** router ⇒ guest 逐位与没有 UI 相同。
    //   ★ 只在 `overlay` 打开时才有意义（没有叠层 ⇒ 没有 router ⇒ 两种模式都是 no-op）。
    UiInputMode ui_input = UiInputMode::Consume;
    std::string screenshot_path;  // 支持 {frame}
    // ★ 只回读**这一帧**（1 起；0 = 每帧都写）。配合 `--max-frames N` 就是"定帧截图"：
    //   `--present-screenshot out.bmp --present-screenshot-frame N --max-frames N` 只产出一张。
    std::uint64_t screenshot_frame = 0;
    // ★ 宿主真光标（`--cursor-warp`，**默认关**）：只加**宿主副作用**，⛔ 不改变 guest 可见状态
    //   （虚拟光标 / `GetCursorPos` / `WM_MOUSEMOVE` 的 lParam 一律不变）。
    //   实际生效还要求窗口可见（运行时 `windowHidden()` 判据）——见 `hostCursorWarpEnabled`。
    bool cursor_warp = false;
    // ★ `--scale <auto|数值>`（缺省 `Fixed 1.0` = 窗口客户区 1280×720，逐字节不变）。
    //   口径**唯一一份**在 `runtime/window_scale.h`（窗口尺寸 / auto 的 DPI / readout 文案）。
    //   ⛔ 只由**窗口档**设置：`--present headless` 的调用方不要设它（`--scale` 对 headless
    //   无副作用：隐藏离屏窗口仍是 1280×720，也不去查 DPI —— 那会让"只是拿 GPU device"多一次平台依赖）。
    ScaleSpec scale;
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
    // ★ `PresentSink`（M0）：本帧几何（窗口像素 + letterbox 之后画面矩形）→ UI 层算画布原点。
    bool presentGeometry(PresentGeometry* out) const override;
    // ★ `PresentSink`（M0）：**统一合成入口** —— 一块 RGBA8 叠层按窗口像素 1:1 画到 (x,y)。
    //   renderer 路径在画叠层那一次 draw 前后临时关掉 LETTERBOX（B1；画完恢复）；
    //   GPU 直出路径把同一块交给后端（`GpuDirectPresenter::directOverlay`），在 letterbox blit
    //   之后用**同一个 origin**合成。⛔ 不缩放 / 不写 guest。
    bool presentOverlay(const std::uint8_t* rgba, int w, int h, int x, int y, std::string* err) override;
    void pumpHostEvents() override;
    // ★ P3：GetMessageA 的事件源。wait_ms>0 时用 SDL_WaitEventTimeout(≈1..10ms) 等一个事件；
    //   没有事件源时 PresentSink 的 0ms 轮询语义不变。
    void pumpHostEvents(int wait_ms);
    bool stopRequested() const override;
    // ★ `PresentSink`（控制层 S4）：resume 时记"恢复时刻"⇒ 丢弃陈旧 OS 输入；暂停期间的关窗/ESC
    //   当宿主 stop（`controlStopReason() == "window-closed"`）。判据在 `runtime/present.h`
    //   的 `classifyStaleEvent`（pure），实现见 `present_sdl.cpp` 的 `pumpOneEvent`。
    void noteControlResume() override;
    const char* controlStopReason() const override;
    // ★ `PresentSink`：宿主侧请求关窗（与 SDL 的 WINDOW_CLOSE_REQUESTED 同一条路）。
    void requestClose() override;
    // ★ `PresentSink`：**系统对话框**（`SLD_ShowMessageBox`）。会阻塞 ⇒ 实现里套 `GuestToken::YieldGuard`；
    //   `spec.buttons[i].id` 原样当 SDL buttonid（返回哪个 id 就用哪个）。失败/无窗口 ⇒ false。
    bool showSystemMessageBox(const ModalSpec& spec, std::uint32_t* result) override;
    // ★ `PresentSink`：AGERC 回答 2/0（不关窗）时撤掉那次关窗请求（清 close_pending / 宽限 / deadline）。
    void cancelCloseRequest() override;
    // 把 SDL 事件翻译成 guest 的 Win32 消息（WM_CLOSE / 键鼠 / 滚轮）；窗口关掉后置 stop。
    void attachWindowManager(WindowManager* windows);
    // 测试用：拿 SDL 窗口 ID 构造 WINDOW_CLOSE_REQUESTED。
    std::uint32_t windowIdForTest() const;
    std::string describe() const override;
    GpuRaster* gpuRaster() override;
    // ★ 诊断 / 判据：SDL 的窗口 flag 里有没有 `SDL_WINDOW_HIDDEN`。
    //   `--present headless` 的承诺是"**不出现可见窗口**"（不是"不建窗口"——建不出窗口/GPU
    //   device 就直接拒绝启动），这条给日志一个可读的机械证据。
    bool windowHidden() const;
    // ★ `PresentSink`：宿主光标 warp（`--cursor-warp`，缺省关）。
    //   `hostCursorWarpEnabled()` = 开关打开 **且** 窗口**当前可见**（运行时 `windowHidden()`，
    //   ⛔ 不是"有没有窗口"：`--present headless` 也建隐藏窗口）。
    //   `hostCursorWarp(cx,cy)` 的入参是 **guest 客户区像素**（见 `runtime/present.h`）；
    //   实现 = letterbox（`runtime/cursor_map.h` 的**同一份**公式，image 尺寸取最近一次 Present
    //   的帧尺寸）+ `SDL_GetWindowPosition` 的客户区原点 + 平台原语（`runtime/host_cursor.h`）。
    //   false = 没挪（隐藏窗口 / 几何不可用 / 系统拒绝）；⛔ 不假装成功。
    bool hostCursorWarpEnabled() const override;
    bool hostCursorWarp(int guest_client_x, int guest_client_y) override;

    // GpuRaster
    // ★ 除 `rasterName()`（init 时缓存的那个后端的名字；生产只有一个后端 ⇒ 与转发观测等价）
    //   这一条**有意的非纯转发**，其余每一条都必须显式转发到 `impl_->raster()`。
    bool rasterActive() const override;
    const char* rasterName() const override;
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
    //   账上一切正常（`REQ-01M73P3GXW22Y467YGYPQSM571` 的实测根因：GPU 光栅化臂的
    //   Live2D 角色整层丢失就是这个）。★ 它属于"转发壳漏一个虚函数"这一类错 —— 不改像素、
    //   不报错、测试全绿 ⇒ 守卫必须是"逐条转发"的（`tests/gpu_batch.cpp::caseDepthClearForwarded`）。
    void rasterRequestDepthClear(float z) override;
    // GPU 直出接口转发到真正的后端（后端在 d3d9_gpu.cpp）。
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
    // ★ `--scale` 的一次性 readout 数据（`runtime/window_scale.h` 的 `ScaleReport`）：
    //   `window_w/h` 是**建完窗口之后 `SDL_GetWindowSize` 回读**的客户区尺寸（⛔ 不是把请求值
    //   抄回来 —— 窗口管理器可能约束尺寸）。`applied` 由 caller 按档位决定：`--present headless`
    //   时 sink 建的只是隐藏离屏窗口，`--scale` 没生效 ⇒ caller 把它置 false 再打。
    //   窗口没建（init 失败）⇒ 缺省 1280×720。
    ScaleReport scaleReport() const;
    // ★ 调试叠层开关（`--overlay` / `--ui`）。`init()` 之后也能改：关掉时把已建的叠层纹理
    //   **退役**（不立即销毁 —— 在飞的 draw 可能还在 GPU 队列里；`shutdown()` 统一释放）；
    //   再打开时按 `ui_script` 重新装脚本（**失败 ⇒ 一次性 stderr + 保持关**，⛔ 不崩）。
    //   默认关；只影响窗口像素（⛔ 不碰 guest 内存）。
    void setOverlay(bool on);
    // ★ `PresentSink`：`Machine` 每帧 / 身份变化时推过来的只读读数（`script` / `worst`）。
    //   ★ 只**存值**：HUD 是 `scripts/ui/hud.js` 每帧重画（⛔ 没有 dirty / 100 ms 节流 / 文本缓存）。
    void setUiReadings(const UiReadings& readings) override;
    // ★ `PresentSink`：帧列表读数源（`game.local_frames()`；非 owning，生命周期 = 调用方）。
    //   ⛔ 只存指针：`drawOverlay` 每帧把它塞进 `UiFrameReadings::frames`，脚本**按需**查询。
    void setUiFrameSource(ui::UiFrameSource* src) override;
    // ★ `PresentSink`：宿主能力表（`host.profileBegin/End`；非 owning，生命周期 = 调用方 = `AotHost`）。
    //   ⛔ 只存指针：`drawOverlay` 每帧把它塞进 `UiFrameReadings::host`。
    void setUiHostOps(const ui::UiHostOps* ops) override;

    // =====================================================================================
    // C2b-2：把**外部拥有**的叠层脚本接进本窗口**已有**的叠层 host
    //
    // ★ 用途（生产 = `aot_main`）：`--plugin` 的 `PluginScript` 同时是 `UiScript`（同一个实例既跑
    //   Run 回调、又画本帧）⇒ 窗口装配把**它**接进来，⛔ **不新开第二套 UI 运行时**
    //   （合同 §6 行 9 / 11；`plugin-runtime.md` §4.4c）。
    // ★ 语义：
    //   · 需要宿主字体（R5）：缺 ⇒ false + `*err`（⛔ 不静默画空白）；已有 ⇒ 复用同一个
    //     `FontEngine`（插件装载期可能已经借走这个指针）；
    //   · 会丢掉本窗口**自己拥有**的那份脚本（`--ui` / 默认 `hud.js`）与它的 QJS 内核 ⇒
    //     同一时刻只有一个叠层脚本 / 一个 QJS context；
    //   · **会打开 `overlay`**（有 `render` 的插件在窗口档就该画；`--present headless` 不 adopt）；
    //   · 作废命中表（旧脚本的 key / resolved rect 不代表新脚本），并重装注入路由；
    //   · `script` **非拥有**：调用方必须保证它活到 `releaseOverlayScript()`（生产 = `aot_main`
    //     在 run 收口处调用；`plugin_host` 的析构早于本窗口对象）。
    //   · `window == nullptr`（`init` 之前 / 已 shutdown）⇒ false + err（⛔ 不静默"接进空气"）。
    // ★ `setOverlay(false)` **不**摘 adopted（只是不画）；`setOverlay(true)` 重新打开**同一份**
    //   （⛔ 不会退回 hud.js）。真正"叠层不可用"（`disableOverlay`）/ `shutdown` 才摘。
    bool adoptOverlayScript(ui::UiScript& script, std::string* err);
    // 摘掉 `adoptOverlayScript` 接进来的那一份（**不**销毁对象）；没有别的脚本可画 ⇒ 关掉 `overlay`
    // 并退役叠层纹理（与 `setOverlay(false)` 同一条 teardown）。幂等。
    void releaseOverlayScript();
    // ★ `ctx.draw.measure` / `draw.text` 的宿主字体来源（非拥有；C2b-2）。已存在 ⇒ 返回同一个；
    //   缺 ⇒ 建 + 校验度量（`kUiFontRefPx`），失败 ⇒ nullptr + `*err`（R5）。
    //   生产调用点 = `aot_main` 的窗口装配（**得知插件有 render 之后**），插件再经
    //   `PluginHost::setFont` 借到同一个指针。
    FontEngine* ensureOverlayFont(std::string* err);
    bool overlayEnabled() const override;
    const char* rendererName() const;    // 直出时是 `gpu-direct`
    // GPU 直出是否成立（窗口已 claim 给光栅化的 device）。false ⇒ 呈现走 SDL_Renderer 上传。
    bool directPresentActive() const;
    const char* directPresentMode() const;  // 未直出 ⇒ ""
    void setCaptureForTest(bool on);     // 回读像素到内存（测试用）
    bool readbackPixel(int x, int y, std::uint32_t* argb, std::string* err) const;  // 0xAARRGGBB

private:
    struct Impl;
    // ★ M0 / B：唯一的 HUD 绘制入口（打包读数 → 脚本产出 display list → 宿主字体栅格化 →
    //   `presentOverlay`）。脚本每帧重画 ⇒ ⛔ 这里没有节流 / 文本缓存。
    bool drawOverlay(std::string* err);
    std::unique_ptr<Impl> impl_;
};

// 用 SDL_GPU 设备创建真正的 GPU 光栅化后端（需要 SDL_Init(VIDEO)；
// Windows = DXBC，macOS = MSL/Metal，其它平台明确失败）。
std::unique_ptr<GpuRaster> createSdlGpuRaster(std::string* err);

}  // namespace lift
