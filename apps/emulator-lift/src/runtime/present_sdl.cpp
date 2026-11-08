#include "runtime/present_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/frame_blit.h"
#include "runtime/host_window.h"

namespace lift {
namespace {

SDL_PixelFormat sdlFormatFor(frame_blit::HostPixelFormat fmt) {
    switch (fmt) {
        case frame_blit::HostPixelFormat::Xrgb8888: return SDL_PIXELFORMAT_XRGB8888;
        case frame_blit::HostPixelFormat::Argb8888: return SDL_PIXELFORMAT_ARGB8888;
        case frame_blit::HostPixelFormat::Rgb565: return SDL_PIXELFORMAT_RGB565;
        case frame_blit::HostPixelFormat::Xrgb1555: return SDL_PIXELFORMAT_XRGB1555;
        case frame_blit::HostPixelFormat::Argb1555: return SDL_PIXELFORMAT_ARGB1555;
        case frame_blit::HostPixelFormat::Argb4444: return SDL_PIXELFORMAT_ARGB4444;
        case frame_blit::HostPixelFormat::Unsupported: break;
    }
    return SDL_PIXELFORMAT_UNKNOWN;
}

std::string replaceFrameToken(std::string path, std::uint64_t frame) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04llu", static_cast<unsigned long long>(frame));
    const std::string token = "{frame}";
    for (std::size_t pos = path.find(token); pos != std::string::npos; pos = path.find(token, pos + std::strlen(buf))) {
        path.replace(pos, token.size(), buf);
    }
    return path;
}

bool hasDriver(const std::string& list, const char* name) {
    return list.find(name) != std::string::npos;
}

// ---- SDL → Win32 消息（P3 输入泵）----
constexpr std::uint32_t kWmClose = 0x0010u;
constexpr std::uint32_t kWmKeyDown = 0x0100u;
constexpr std::uint32_t kWmKeyUp = 0x0101u;
constexpr std::uint32_t kWmMouseMove = 0x0200u;
constexpr std::uint32_t kWmLButtonDown = 0x0201u;
constexpr std::uint32_t kWmLButtonUp = 0x0202u;
constexpr std::uint32_t kWmRButtonDown = 0x0204u;
constexpr std::uint32_t kWmRButtonUp = 0x0205u;
constexpr std::uint32_t kWmMButtonDown = 0x0207u;
constexpr std::uint32_t kWmMButtonUp = 0x0208u;
constexpr std::uint32_t kWmMouseWheel = 0x020Au;
constexpr std::uint32_t kMkLButton = 0x0001u;
constexpr std::uint32_t kMkRButton = 0x0002u;
constexpr std::uint32_t kMkMButton = 0x0010u;
constexpr int kCloseGraceFrames = 3;
constexpr std::uint64_t kCloseGraceNs = 500ull * 1000ull * 1000ull;

std::uint32_t vkFromSdlKey(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z) return 0x41u + static_cast<std::uint32_t>(key - SDLK_A);
    if (key >= SDLK_0 && key <= SDLK_9) return 0x30u + static_cast<std::uint32_t>(key - SDLK_0);
    if (key >= SDLK_F1 && key <= SDLK_F12) return 0x70u + static_cast<std::uint32_t>(key - SDLK_F1);
    switch (key) {
        case SDLK_ESCAPE: return 0x1B;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return 0x0D;
        case SDLK_TAB: return 0x09;
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_SPACE: return 0x20;
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_INSERT: return 0x2D;
        case SDLK_DELETE: return 0x2E;
        case SDLK_HOME: return 0x24;
        case SDLK_END: return 0x23;
        case SDLK_PAGEUP: return 0x21;
        case SDLK_PAGEDOWN: return 0x22;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT: return 0x10;
        case SDLK_LCTRL:
        case SDLK_RCTRL: return 0x11;
        case SDLK_LALT:
        case SDLK_RALT: return 0x12;
        default: return 0;
    }
}

std::uint32_t mouseBit(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return kMkLButton;
        case SDL_BUTTON_RIGHT: return kMkRButton;
        case SDL_BUTTON_MIDDLE: return kMkMButton;
        default: return 0;
    }
}

// SDL 鼠标键 → VK_LBUTTON/VK_RBUTTON/VK_MBUTTON/VK_XBUTTON1/2。
// ★ 与键盘共用**同一份**影子键状态（`WindowManager::key_down_`）：引擎的
//   `GetAsyncKeyState(1/2)` 轮询（`.lst` 0x47716D/0x477195/0x477241）只认这一份。
std::uint32_t vkFromMouseButton(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return 0x01u;   // VK_LBUTTON
        case SDL_BUTTON_RIGHT: return 0x02u;  // VK_RBUTTON
        case SDL_BUTTON_MIDDLE: return 0x04u; // VK_MBUTTON
        case SDL_BUTTON_X1: return 0x05u;     // VK_XBUTTON1
        case SDL_BUTTON_X2: return 0x06u;     // VK_XBUTTON2
        default: return 0;
    }
}

std::uint32_t win32MouseMessage(Uint8 button, bool down) {
    switch (button) {
        case SDL_BUTTON_LEFT: return down ? kWmLButtonDown : kWmLButtonUp;
        case SDL_BUTTON_RIGHT: return down ? kWmRButtonDown : kWmRButtonUp;
        case SDL_BUTTON_MIDDLE: return down ? kWmMButtonDown : kWmMButtonUp;
        default: return 0;
    }
}

}  // namespace

struct SdlWindow::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    frame_blit::HostPixelFormat tex_fmt = frame_blit::HostPixelFormat::Unsupported;
    bool sdl_inited = false;
    bool sized = false;
    bool stop = false;
    bool capture = false;
    bool device_lost = false;
    std::uint64_t frame_no = 0;
    std::string screenshot_path;
    std::string renderer_name;
    std::string video_driver;
    std::string gpu_driver;
    std::string raster_name;
    std::vector<std::uint32_t> capture_argb;
    int capture_w = 0;
    int capture_h = 0;
    // ---- GPU 呈现路径 ----
    // ★ 直出成立时 renderer == nullptr（同一个窗口不能既 claim 给 GPU device 又建 SDL_Renderer，
    //   见 init 的实测注释）。`direct == nullptr` ⇒ 走 SDL_Renderer 上传路径。
    GpuDirectPresenter* direct = nullptr;
    bool direct_present = false;
    std::string direct_error;  // claim 失败的实测错误串（回退 renderer 时打出来，不吞）
    int direct_w = 0;          // 直出时上一次同步给窗口的后缓冲尺寸
    int direct_h = 0;
    // GPU 光栅化镜像（与窗口 renderer 共用；--raster gpu）
    bool raster_gpu = false;
    bool raster_sprites = true;
    std::unique_ptr<GpuRaster> gpu_raster;
    SDL_Texture* raster_target = nullptr;
    int raster_w = 0;
    int raster_h = 0;
    frame_blit::HostPixelFormat raster_fmt = frame_blit::HostPixelFormat::Unsupported;
    bool raster_dirty = false;
    SDL_Texture* raster_tex = nullptr;
    const void* raster_tex_key = nullptr;
    int raster_tex_w = 0;
    int raster_tex_h = 0;
    frame_blit::HostPixelFormat raster_tex_fmt = frame_blit::HostPixelFormat::Unsupported;
    // P3：guest 消息泵 / 键鼠状态 / 关窗宽限
    WindowManager* windows = nullptr;
    std::uint32_t mouse_buttons = 0;
    bool close_pending = false;
    int close_frames_left = 0;
    std::uint64_t close_deadline_ns = 0;

    void postMessage(std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam);
    void requestStop();
    void requestClose();
    void advanceCloseFrame();
    void handleEvent(const SDL_Event& event);
    void pumpEvents(int wait_ms);
    void mapMouse(float mx, float my, std::int32_t* gx, std::int32_t* gy) const;
    // 截图 + 捕获（两条呈现路径共用；`shot` 归调用方销毁）。frame_no 必须已经自增。
    bool saveShot(SDL_Surface* shot, std::string* err);
    // GPU 直出的一帧（不经过 SDL_UpdateTexture / SDL_Renderer）。
    bool submitDirect(const SurfaceArena& arena, const GuestSurface& surface, std::string* err);
};

void SdlWindow::Impl::postMessage(std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam) {
    if (windows == nullptr) return;
    WinMessage m;
    m.hwnd = windows->primaryWindow();
    m.message = message;
    m.wparam = wparam;
    m.lparam = lparam;
    m.time = static_cast<std::uint32_t>(SDL_GetTicks());
    windows->post(m);
}

void SdlWindow::Impl::requestStop() {
    if (stop) return;
    stop = true;
    // 唤醒可能正阻塞在 GetMessageA 里的 guest：WM_QUIT 是队列尾的虚拟项。
    if (windows != nullptr) windows->postQuit(0);
}

void SdlWindow::Impl::requestClose() {
    if (stop || close_pending) return;
    if (windows == nullptr || windows->primaryWindow() == 0) {
        // 还没有 guest 窗口（或没有消息泵）⇒ 没有可确认的对象，直接收口。
        requestStop();
        return;
    }
    close_pending = true;
    close_frames_left = kCloseGraceFrames;
    close_deadline_ns = SDL_GetTicksNS() + kCloseGraceNs;
    postMessage(kWmClose, 0, 0);
}

void SdlWindow::Impl::advanceCloseFrame() {
    if (!close_pending || stop) return;
    if (close_frames_left > 0) --close_frames_left;
    if (close_frames_left <= 0 || SDL_GetTicksNS() >= close_deadline_ns) requestStop();
}

void SdlWindow::Impl::mapMouse(float mx, float my, std::int32_t* gx, std::int32_t* gy) const {
    int ww = 1280;
    int wh = 720;
    if (window != nullptr) SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0) ww = 1280;
    if (wh <= 0) wh = 720;
    // 与 present 的 SDL_LOGICAL_PRESENTATION_LETTERBOX 同一套映射：窗口像素 → 虚拟 1280x720。
    const float scale = std::min(static_cast<float>(ww) / 1280.0f, static_cast<float>(wh) / 720.0f);
    if (scale <= 0.0f) {
        *gx = static_cast<std::int32_t>(mx);
        *gy = static_cast<std::int32_t>(my);
        return;
    }
    const float dw = 1280.0f * scale;
    const float dh = 720.0f * scale;
    const float ox = (static_cast<float>(ww) - dw) * 0.5f;
    const float oy = (static_cast<float>(wh) - dh) * 0.5f;
    int x = static_cast<int>(std::lround((mx - ox) / scale));
    int y = static_cast<int>(std::lround((my - oy) / scale));
    x = std::clamp(x, 0, 1279);
    y = std::clamp(y, 0, 719);
    *gx = x;
    *gy = y;
}

void SdlWindow::Impl::handleEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
            // 应用级退出：直接 post WM_QUIT（不再走 guest 的 WM_CLOSE 确认）。
            requestStop();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (window != nullptr && event.window.windowID == SDL_GetWindowID(window)) requestClose();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            mouse_buttons = 0;
            if (windows != nullptr) windows->clearKeys();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = event.type == SDL_EVENT_KEY_DOWN;
            const std::uint32_t vk = vkFromSdlKey(event.key.key);
            if (vk == 0) break;
            if (windows != nullptr) windows->setKeyDown(vk, down);
            const std::uint32_t scan = event.key.scancode ? static_cast<std::uint32_t>(event.key.scancode) : 0u;
            std::uint32_t lparam = 1u | (scan << 16);
            if (!down) lparam |= 0xC0000000u;  // previous state + transition
            postMessage(down ? kWmKeyDown : kWmKeyUp, vk, lparam);
            if (down && vk == 0x1B) requestClose();  // ESC = 用户要退出（与旧行为一致）
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.motion.x, event.motion.y, &x, &y);
            if (windows != nullptr) windows->setCursorClientPos(windows->primaryWindow(), x, y);
            if (std::getenv("LIFT_TRACE_INPUT") != nullptr) {
                std::fprintf(stderr, "[input] motion sdl=(%.1f,%.1f) -> guest=(%d,%d)\n",
                             event.motion.x, event.motion.y, x, y);
            }
            postMessage(kWmMouseMove, mouse_buttons,
                        (static_cast<std::uint32_t>(y) << 16) | static_cast<std::uint32_t>(x));
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const std::uint32_t bit = mouseBit(event.button.button);
            if (bit == 0) break;
            if (down) mouse_buttons |= bit;
            else mouse_buttons &= ~bit;
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.button.x, event.button.y, &x, &y);
            if (windows != nullptr) {
                windows->setCursorClientPos(windows->primaryWindow(), x, y);
                // ★ 按键电平：引擎 `GetAsyncKeyState(VK_LBUTTON/VK_RBUTTON)` 只读影子键状态，
                //   不写这里 ⇒ 轮询恒 0（实测 1827 次全 0）。消息通道照旧。
                const std::uint32_t vk = vkFromMouseButton(event.button.button);
                if (vk != 0) windows->setKeyDown(vk, down);
            }
            postMessage(win32MouseMessage(event.button.button, down), mouse_buttons,
                        (static_cast<std::uint32_t>(y) << 16) | static_cast<std::uint32_t>(x));
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const int delta = static_cast<int>(std::lround(event.wheel.y * 120.0f));
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.wheel.mouse_x, event.wheel.mouse_y, &x, &y);
            if (windows != nullptr) windows->setCursorClientPos(windows->primaryWindow(), x, y);
            const std::uint32_t wp = (static_cast<std::uint32_t>(delta) << 16) | mouse_buttons;
            postMessage(kWmMouseWheel, wp,
                        (static_cast<std::uint32_t>(y) << 16) | static_cast<std::uint32_t>(x));
            break;
        }
        case SDL_EVENT_RENDER_DEVICE_LOST:
            if (!device_lost) {
                device_lost = true;
                std::fprintf(stderr, "[present] SDL 渲染设备丢失（不会自动重建；后续帧会回退 headless）\n");
            }
            break;
        default:
            break;
    }
}

void SdlWindow::Impl::pumpEvents(int wait_ms) {
    if (stop) return;
    SDL_Event event;
    if (wait_ms > 0) {
        if (SDL_WaitEventTimeout(&event, wait_ms)) {
            handleEvent(event);
            while (SDL_PollEvent(&event)) handleEvent(event);
        }
    } else {
        while (SDL_PollEvent(&event)) handleEvent(event);
    }
    if (close_pending && !stop && SDL_GetTicksNS() >= close_deadline_ns) requestStop();
}

SdlWindow::SdlWindow() : impl_(std::make_unique<Impl>()) {}
SdlWindow::~SdlWindow() { shutdown(); }

bool SdlWindow::init(const SdlWindowOptions& options, std::string* err) {
    if (impl_->window != nullptr) return true;
    impl_->screenshot_path = options.screenshot_path;
    impl_->raster_gpu = options.raster_gpu;
    impl_->raster_sprites = options.raster_sprites;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (err) *err = std::string("SDL_Init(VIDEO) 失败：") + SDL_GetError();
        return false;
    }
    impl_->sdl_inited = true;
    // ★ GPU device 先建（不需要窗口）：`--raster gpu` 的后端失败必须在开窗之前就是硬失败。
    if (options.raster_gpu) {
        std::string gerr;
        impl_->gpu_raster = createSdlGpuRaster(&gerr);
        if (impl_->gpu_raster == nullptr) {
            if (err) *err = "GPU 光栅化后端创建失败：" + gerr;
            shutdown();
            return false;
        }
        impl_->raster_name = impl_->gpu_raster->rasterName();
    }
    impl_->video_driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(unknown)";

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
    impl_->window = SDL_CreateWindow(options.title.c_str(), 1280, 720, flags);
    if (impl_->window == nullptr) {
        if (err) *err = std::string("SDL_CreateWindow 失败：") + SDL_GetError();
        shutdown();
        return false;
    }

    // ★ 窗口创建之后才可能 claim swapchain ⇒ GPU 直出的 attach 必须在这里，不能在 init 开头。
    //   ★ 实测口径：窗口一旦 claim 给 GPU device，就不能再对它 `SDL_CreateRenderer`
    //   （同一窗口的 swapchain 只能有一个主人）——所以直出成立时**不建 renderer**；
    //   claim 失败（dummy 视频 / 驱动不支持）才回退 SDL_Renderer 上传路径，并把 SDL 的错误打出来。
    if (impl_->gpu_raster != nullptr) {
        GpuDirectPresenter* dp = impl_->gpu_raster->directPresenter();
        if (dp != nullptr) {
            std::string derr;
            if (dp->directAttachWindow(impl_->window, options.vsync, &derr)) {
                impl_->direct = dp;
                impl_->direct_present = true;
                impl_->renderer_name = "gpu-direct";
                impl_->gpu_driver = dp->directDriverName();
            } else {
                impl_->direct_error = derr;
                std::fprintf(stderr,
                             "[present] GPU 直出不可用：%s\n"
                             "  ⇒ 回退 SDL_Renderer 上传呈现路径（光栅化仍是 --raster gpu；"
                             "只是每帧多一次 CPU→GPU 上传）\n",
                             derr.c_str());
            }
        }
    }

    if (!impl_->direct_present) {
        const char* driver = options.driver_list.empty() ? nullptr : options.driver_list.c_str();
        impl_->renderer = SDL_CreateRenderer(impl_->window, driver);
        if (impl_->renderer == nullptr) {
            if (err) {
                *err = std::string("SDL_CreateRenderer(") + (driver ? driver : "default") + ") 失败：" +
                       SDL_GetError();
                // 直出也失败过时把两条原因都带上（否则 SKIP/报错只说 renderer，看不出直出为什么没成立）
                if (!impl_->direct_error.empty()) *err += "；GPU 直出也不可用：" + impl_->direct_error;
            }
            shutdown();
            return false;
        }
        const char* name = SDL_GetRendererName(impl_->renderer);
        impl_->renderer_name = name ? name : "(unknown)";
        if (impl_->renderer_name == "software" && !hasDriver(options.driver_list, "software")) {
            if (err) *err = "SDL 只给出了 software 渲染器（不是 GPU API 路径）；"
                            "需要显式 --present-driver software 才接受";
            shutdown();
            return false;
        }
        // ★ 只有 renderer 名字是 "gpu" 才问 GPU 设备：非 GPU renderer 上 SDL_GetGPURendererDevice
        //   会**设置** SDL error（"Renderer isn't a GPU renderer"），污染后面的 SDL_GetError()。
        SDL_GPUDevice* gpu = nullptr;
        if (impl_->renderer_name == "gpu") gpu = SDL_GetGPURendererDevice(impl_->renderer);
        impl_->gpu_driver =
            (gpu != nullptr && SDL_GetGPUDeviceDriver(gpu) != nullptr) ? SDL_GetGPUDeviceDriver(gpu) : "";
        // ⛔ 不要覆盖 GPU 光栅化后端的名字：`--raster gpu` 时它才是真正的执行者
        //   （`sdl-gpu/<driver>/<shader format>`），renderer 只是像素上传/呈现的 sink。
        if (!options.raster_gpu) impl_->raster_name = "sdl-renderer/" + impl_->renderer_name;

        if (!SDL_SetRenderVSync(impl_->renderer, options.vsync ? 1 : 0)) {
            std::fprintf(stderr, "[present] SDL_SetRenderVSync(%d) 失败：%s\n", options.vsync ? 1 : 0,
                         SDL_GetError());
        }
        SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    }
    return true;
}

void SdlWindow::shutdown() {
    if (impl_ == nullptr) return;
    // ★ claim 必须**先于** SDL_DestroyWindow 释放（swapchain 绑定在窗口上）。
    if (impl_->direct != nullptr) {
        impl_->direct->directDetachWindow();
        impl_->direct = nullptr;
    }
    impl_->direct_present = false;
    impl_->direct_error.clear();
    impl_->direct_w = 0;
    impl_->direct_h = 0;
    if (impl_->raster_target != nullptr) {
        SDL_DestroyTexture(impl_->raster_target);
        impl_->raster_target = nullptr;
    }
    if (impl_->raster_tex != nullptr) {
        SDL_DestroyTexture(impl_->raster_tex);
        impl_->raster_tex = nullptr;
    }
    impl_->raster_tex_key = nullptr;
    impl_->raster_tex_w = 0;
    impl_->raster_tex_h = 0;
    impl_->raster_tex_fmt = frame_blit::HostPixelFormat::Unsupported;
    impl_->raster_w = 0;
    impl_->raster_h = 0;
    impl_->raster_fmt = frame_blit::HostPixelFormat::Unsupported;
    impl_->raster_dirty = false;
    impl_->raster_gpu = false;
    impl_->raster_name.clear();
    if (impl_->texture != nullptr) {
        SDL_DestroyTexture(impl_->texture);
        impl_->texture = nullptr;
    }
    if (impl_->renderer != nullptr) {
        SDL_DestroyRenderer(impl_->renderer);
        impl_->renderer = nullptr;
    }
    if (impl_->window != nullptr) {
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    // GPU 光栅化持有自己的 SDL_GPUDevice：必须在 SDL_Quit 之前释放（Metal 后端尤其如此）。
    impl_->gpu_raster.reset();
    if (impl_->sdl_inited) {
        SDL_Quit();
        impl_->sdl_inited = false;
    }
    impl_->tex_w = 0;
    impl_->tex_h = 0;
    impl_->tex_fmt = frame_blit::HostPixelFormat::Unsupported;
    impl_->sized = false;
    impl_->stop = false;
    impl_->device_lost = false;
    impl_->capture = false;
    impl_->windows = nullptr;
    impl_->mouse_buttons = 0;
    impl_->close_pending = false;
    impl_->close_frames_left = 0;
    impl_->close_deadline_ns = 0;
    impl_->frame_no = 0;
    impl_->screenshot_path.clear();
    impl_->renderer_name.clear();
    impl_->video_driver.clear();
    impl_->gpu_driver.clear();
    impl_->capture_argb.clear();
    impl_->capture_w = 0;
    impl_->capture_h = 0;
}

bool SdlWindow::submitFrame(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) {
    // 关窗宽限按**呈现帧**计：WM_CLOSE 在上一帧末尾入队，本帧开始才消耗一格。
    impl_->advanceCloseFrame();
    if (impl_->device_lost) {
        if (err) *err = "SDL 渲染设备已丢失";
        return false;
    }
    // ★ GPU 直出：target 已经是最新的（GPU 画完 / d3d9::present 上传过 guest）⇒ 直接 blit 到
    //   swapchain。这条路上没有 SDL_UpdateTexture，也没有 SDL_Renderer。
    if (impl_->direct_present && impl_->direct != nullptr) return impl_->submitDirect(arena, surface, err);
    if (impl_->renderer == nullptr) {
        if (err) *err = "SDL 窗口未初始化";
        return false;
    }
    // GPU 光栅化可能把 render target 留在离屏纹理上；呈现前必须回到窗口。
    SDL_SetRenderTarget(impl_->renderer, nullptr);
    SDL_SetRenderViewport(impl_->renderer, nullptr);
    SDL_SetRenderClipRect(impl_->renderer, nullptr);
    const int w = surface.width;
    const int h = surface.height;
    if (w <= 0 || h <= 0) {
        if (err) *err = "surface 尺寸非法";
        return false;
    }
    const std::uint8_t* pixels = arena.pixels(surface);
    if (pixels == nullptr) {
        if (err) *err = "arena.pixels(surface) == nullptr";
        return false;
    }
    const frame_blit::HostPixelFormat fmt = frame_blit::chooseHostFormat(surface.format);
    if (fmt == frame_blit::HostPixelFormat::Unsupported) {
        if (err) *err = std::string("不支持的 guest 像素格式（bpp=") + std::to_string(surface.format.bpp) + "）";
        return false;
    }

    const bool size_changed = (impl_->tex_w != w || impl_->tex_h != h);
    if (impl_->texture == nullptr || size_changed || impl_->tex_fmt != fmt) {
        if (impl_->texture != nullptr) SDL_DestroyTexture(impl_->texture);
        impl_->texture = SDL_CreateTexture(impl_->renderer, sdlFormatFor(fmt), SDL_TEXTUREACCESS_STREAMING, w, h);
        if (impl_->texture == nullptr) {
            if (err) *err = std::string("SDL_CreateTexture 失败：") + SDL_GetError();
            return false;
        }
        if (!SDL_SetTextureBlendMode(impl_->texture, SDL_BLENDMODE_NONE)) {
            if (err) *err = std::string("SDL_SetTextureBlendMode(NONE) 失败：") + SDL_GetError();
            return false;
        }
        if (!SDL_SetTextureScaleMode(impl_->texture, SDL_SCALEMODE_NEAREST)) {
            if (err) *err = std::string("SDL_SetTextureScaleMode(NEAREST) 失败：") + SDL_GetError();
            return false;
        }
        if (!SDL_SetRenderLogicalPresentation(impl_->renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            if (err) *err = std::string("SDL_SetRenderLogicalPresentation 失败：") + SDL_GetError();
            return false;
        }
        impl_->tex_w = w;
        impl_->tex_h = h;
        impl_->tex_fmt = fmt;
        if (!impl_->sized || size_changed) {
            if (!SDL_SetWindowSize(impl_->window, w, h)) {
                if (err) *err = std::string("SDL_SetWindowSize 失败：") + SDL_GetError();
                return false;
            }
            impl_->sized = true;
        }
    }

    if (!SDL_UpdateTexture(impl_->texture, nullptr, pixels, static_cast<int>(surface.pitch))) {
        if (err) *err = std::string("SDL_UpdateTexture 失败：") + SDL_GetError();
        return false;
    }
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    SDL_RenderClear(impl_->renderer);
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr);
    ++impl_->frame_no;

    if (!impl_->screenshot_path.empty() || impl_->capture) {
        SDL_Surface* shot = SDL_RenderReadPixels(impl_->renderer, nullptr);
        if (shot == nullptr) {
            if (err) *err = std::string("SDL_RenderReadPixels 失败：") + SDL_GetError();
            return false;
        }
        const bool shot_ok = impl_->saveShot(shot, err);
        SDL_DestroySurface(shot);
        if (!shot_ok) return false;
    }

    if (!SDL_RenderPresent(impl_->renderer)) {
        if (err) *err = std::string("SDL_RenderPresent 失败：") + SDL_GetError();
        return false;
    }
    pumpHostEvents();
    return true;
}

// 截图（`--present-screenshot`，`{frame}` = 已自增的帧号）+ 测试捕获缓冲。
// 两条呈现路径共用同一份写盘 / ARGB 转换口径 ⇒ 产物可比。
bool SdlWindow::Impl::saveShot(SDL_Surface* shot, std::string* err) {
    if (shot == nullptr) {
        if (err) *err = "截图 surface 为空";
        return false;
    }
    if (!screenshot_path.empty()) {
        const std::string path = replaceFrameToken(screenshot_path, frame_no);
        if (!SDL_SaveBMP(shot, path.c_str())) {
            if (err) *err = std::string("SDL_SaveBMP(") + path + ") 失败：" + SDL_GetError();
            return false;
        }
    }
    if (capture) {
        SDL_Surface* conv = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888);
        if (conv != nullptr) {
            capture_w = conv->w;
            capture_h = conv->h;
            capture_argb.assign(static_cast<std::size_t>(conv->w) * conv->h, 0);
            for (int y = 0; y < conv->h; ++y) {
                std::memcpy(capture_argb.data() + static_cast<std::size_t>(y) * conv->w,
                            static_cast<const std::uint8_t*>(conv->pixels) + static_cast<std::size_t>(y) * conv->pitch,
                            static_cast<std::size_t>(conv->w) * 4);
            }
            SDL_DestroySurface(conv);
        }
    }
    return true;
}

// GPU 直出的一帧：窗口尺寸跟随后缓冲 → target → swapchain（letterbox）→（可选）窗口回读。
bool SdlWindow::Impl::submitDirect(const SurfaceArena& arena, const GuestSurface& surface, std::string* err) {
    const int w = surface.width;
    const int h = surface.height;
    if (w <= 0 || h <= 0) {
        if (err) *err = "surface 尺寸非法";
        return false;
    }
    if (arena.pixels(surface) == nullptr) {
        if (err) *err = "arena.pixels(surface) == nullptr";
        return false;
    }
    // ★ 本帧的最新像素在哪儿：`directTargetCurrent()` 由 `d3d9::Present` 在读回之后声明。
    //   false ⇒ 本帧没有 GPU 绘制 / 不是 D3D9 Present（DDraw Flip、测试直接 submitFrame）⇒
    //   呈现前必须把 guest 后缓冲上传到 target，否则窗口会停在上一帧。
    if (!direct->directTargetCurrent()) {
        if (!direct->directUploadGuest(surface.format, arena.pixels(surface), surface.pitch, w, h, err)) return false;
    }
    // 与 SDL_Renderer 路径一致：首帧 / 尺寸变化把窗口尺寸跟到后缓冲。
    if (!sized || direct_w != w || direct_h != h) {
        if (!SDL_SetWindowSize(window, w, h)) {
            if (err) *err = std::string("SDL_SetWindowSize 失败：") + SDL_GetError();
            return false;
        }
        direct_w = w;
        direct_h = h;
        sized = true;
    }
    ++frame_no;
    if (!direct->directPresent(err)) return false;
    // 一帧用过就消费掉：下一位调用方（DDraw Flip / 直接 submitFrame）仍然会先上传 guest。
    direct->directSetTargetCurrent(false);
    if (!screenshot_path.empty() || capture) {
        // ★ swapchain 是 write-only ⇒ 窗口像素从**同一份 target** 回读，再套同一份 letterbox。
        std::vector<std::uint8_t> xrgb;
        int rw = 0;
        int rh = 0;
        if (!direct->directReadback(&xrgb, &rw, &rh, err)) return false;
        SDL_Surface* shot = SDL_CreateSurfaceFrom(rw, rh, SDL_PIXELFORMAT_XRGB8888, xrgb.data(), rw * 4);
        if (shot == nullptr) {
            if (err) *err = std::string("SDL_CreateSurfaceFrom 失败：") + SDL_GetError();
            return false;
        }
        const bool shot_ok = saveShot(shot, err);
        SDL_DestroySurface(shot);
        if (!shot_ok) return false;
    }
    pumpEvents(0);
    return true;
}

void SdlWindow::pumpHostEvents() { pumpHostEvents(0); }

void SdlWindow::pumpHostEvents(int wait_ms) {
    if (impl_->window == nullptr) return;
    impl_->pumpEvents(wait_ms);
}

void SdlWindow::attachWindowManager(WindowManager* windows) { impl_->windows = windows; }

std::uint32_t SdlWindow::windowIdForTest() const {
    return impl_->window != nullptr ? SDL_GetWindowID(impl_->window) : 0u;
}

bool SdlWindow::stopRequested() const { return impl_->stop; }

std::string SdlWindow::describe() const {
    std::string out = "driver=" + impl_->renderer_name + " video=" + impl_->video_driver;
    out += " gpu=" + (impl_->gpu_driver.empty() ? std::string("-") : impl_->gpu_driver);
    if (impl_->direct_present && impl_->direct != nullptr) {
        out += std::string(" present=direct/") + impl_->direct->directPresentMode();
    } else if (!impl_->direct_error.empty()) {
        out += " present=renderer(直出不可用)";
    }
    return out;
}

bool SdlWindow::isGpuRenderer() const { return !impl_->gpu_driver.empty(); }

bool SdlWindow::directPresentActive() const { return impl_->direct_present && impl_->direct != nullptr; }

const char* SdlWindow::directPresentMode() const {
    return (impl_->direct_present && impl_->direct != nullptr) ? impl_->direct->directPresentMode() : "";
}

const char* SdlWindow::rendererName() const { return impl_->renderer_name.c_str(); }

void SdlWindow::setCaptureForTest(bool on) { impl_->capture = on; }

bool SdlWindow::readbackPixel(int x, int y, std::uint32_t* argb, std::string* err) const {
    if (impl_->capture_argb.empty() || x < 0 || y < 0 || x >= impl_->capture_w || y >= impl_->capture_h) {
        if (err) *err = "没有可用的回读缓冲（setCaptureForTest 未开 / 坐标越界）";
        return false;
    }
    *argb = impl_->capture_argb[static_cast<std::size_t>(y) * impl_->capture_w + x];
    return true;
}

// ---------------------------------------------------------------------------------------
// GPU 光栅化（--raster gpu）：独立 SDL_GPUDevice 的离屏 target（后端在 d3d9_gpu.cpp）
// ---------------------------------------------------------------------------------------
GpuRaster* SdlWindow::gpuRaster() { return impl_->gpu_raster ? this : nullptr; }

bool SdlWindow::rasterActive() const { return impl_->gpu_raster != nullptr && impl_->gpu_raster->rasterActive(); }

const char* SdlWindow::rasterName() const { return impl_->raster_name.c_str(); }

bool SdlWindow::rasterSpritesEnabled() const {
    // `--raster-sprites 0` 是 A/B 开关：即使后端本身支持 sprite，也必须能退回 CPU 路径。
    return impl_->raster_sprites && impl_->gpu_raster != nullptr && impl_->gpu_raster->rasterSpritesEnabled();
}

bool SdlWindow::rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err,
                              const std::uint8_t* color_base) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterPrepare(format, width, height, err, color_base) : false;
}
bool SdlWindow::rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                    std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterSyncFromGuest(pixels, pitch, width, height, err) : false;
}
bool SdlWindow::rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                  std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterSyncToGuest(pixels, pitch, width, height, err) : false;
}
bool SdlWindow::rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                           std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterDraw(vertices, count, state, err) : false;
}
bool SdlWindow::rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count,
                                   const GpuRasterTexture& texture, const GpuRasterState& state,
                                   std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterDrawTextured(vertices, count, texture, state, err) : false;
}
bool SdlWindow::rasterQueue(const GpuRasterVertex* vertices, std::size_t count,
                            const GpuRasterTexture* texture, const GpuRasterState& state, std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterQueue(vertices, count, texture, state, err) : false;
}
bool SdlWindow::rasterFlush(std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterFlush(err) : true;
}
const void* SdlWindow::rasterTargetBase() const {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterTargetBase() : nullptr;
}
std::size_t SdlWindow::rasterQueuedDraws() const {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterQueuedDraws() : 0;
}
void SdlWindow::rasterDiscardTarget() {
    if (impl_->gpu_raster) impl_->gpu_raster->rasterDiscardTarget();
}
bool SdlWindow::rasterDirty() const { return impl_->gpu_raster != nullptr && impl_->gpu_raster->rasterDirty(); }

GpuDirectPresenter* SdlWindow::directPresenter() {
    return impl_->gpu_raster ? impl_->gpu_raster->directPresenter() : nullptr;
}

}  // namespace lift
