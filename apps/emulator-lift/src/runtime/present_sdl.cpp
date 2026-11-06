#include "runtime/present_sdl.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/frame_blit.h"

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
};

SdlWindow::SdlWindow() : impl_(std::make_unique<Impl>()) {}
SdlWindow::~SdlWindow() { shutdown(); }

bool SdlWindow::init(const SdlWindowOptions& options, std::string* err) {
    if (impl_->renderer != nullptr) return true;
    impl_->screenshot_path = options.screenshot_path;
    impl_->raster_gpu = options.raster_gpu;
    impl_->raster_sprites = options.raster_sprites;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (err) *err = std::string("SDL_Init(VIDEO) 失败：") + SDL_GetError();
        return false;
    }
    impl_->sdl_inited = true;
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

    const char* driver = options.driver_list.empty() ? nullptr : options.driver_list.c_str();
    impl_->renderer = SDL_CreateRenderer(impl_->window, driver);
    if (impl_->renderer == nullptr) {
        if (err) *err = std::string("SDL_CreateRenderer(") + (driver ? driver : "default") + ") 失败：" +
                        SDL_GetError();
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
    impl_->gpu_driver = (gpu != nullptr && SDL_GetGPUDeviceDriver(gpu) != nullptr) ? SDL_GetGPUDeviceDriver(gpu) : "";
    // ⛔ 不要覆盖 GPU 光栅化后端的名字：`--raster gpu` 时它才是真正的执行者
    //   （`sdl-gpu/<driver>/<shader format>`），renderer 只是像素上传/呈现的 sink。
    if (!options.raster_gpu) impl_->raster_name = "sdl-renderer/" + impl_->renderer_name;

    if (!SDL_SetRenderVSync(impl_->renderer, options.vsync ? 1 : 0)) {
        std::fprintf(stderr, "[present] SDL_SetRenderVSync(%d) 失败：%s\n", options.vsync ? 1 : 0, SDL_GetError());
    }
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    return true;
}

void SdlWindow::shutdown() {
    if (impl_ == nullptr) return;
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
    if (impl_->renderer == nullptr) {
        if (err) *err = "SDL 窗口未初始化";
        return false;
    }
    if (impl_->device_lost) {
        if (err) *err = "SDL 渲染设备已丢失";
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
        if (!impl_->screenshot_path.empty()) {
            const std::string path = replaceFrameToken(impl_->screenshot_path, impl_->frame_no);
            if (!SDL_SaveBMP(shot, path.c_str())) {
                if (err) *err = std::string("SDL_SaveBMP(") + path + ") 失败：" + SDL_GetError();
                SDL_DestroySurface(shot);
                return false;
            }
        }
        if (impl_->capture) {
            SDL_Surface* conv = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888);
            if (conv != nullptr) {
                impl_->capture_w = conv->w;
                impl_->capture_h = conv->h;
                impl_->capture_argb.assign(static_cast<std::size_t>(conv->w) * conv->h, 0);
                for (int y = 0; y < conv->h; ++y) {
                    std::memcpy(impl_->capture_argb.data() + static_cast<std::size_t>(y) * conv->w,
                                static_cast<const std::uint8_t*>(conv->pixels) + static_cast<std::size_t>(y) * conv->pitch,
                                static_cast<std::size_t>(conv->w) * 4);
                }
                SDL_DestroySurface(conv);
            }
        }
        SDL_DestroySurface(shot);
    }

    if (!SDL_RenderPresent(impl_->renderer)) {
        if (err) *err = std::string("SDL_RenderPresent 失败：") + SDL_GetError();
        return false;
    }
    pumpHostEvents();
    return true;
}

void SdlWindow::pumpHostEvents() {
    if (impl_->window == nullptr) return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                impl_->stop = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(impl_->window)) impl_->stop = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) impl_->stop = true;
                break;
            case SDL_EVENT_RENDER_DEVICE_LOST:
                if (!impl_->device_lost) {
                    impl_->device_lost = true;
                    std::fprintf(stderr, "[present] SDL 渲染设备丢失（不会自动重建；后续帧会回退 headless）\n");
                }
                break;
            default:
                break;
        }
    }
}

bool SdlWindow::stopRequested() const { return impl_->stop; }

std::string SdlWindow::describe() const {
    std::string out = "driver=" + impl_->renderer_name + " video=" + impl_->video_driver;
    out += " gpu=" + (impl_->gpu_driver.empty() ? std::string("-") : impl_->gpu_driver);
    return out;
}

bool SdlWindow::isGpuRenderer() const { return !impl_->gpu_driver.empty(); }

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

bool SdlWindow::rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err) {
    return impl_->gpu_raster ? impl_->gpu_raster->rasterPrepare(format, width, height, err) : false;
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
bool SdlWindow::rasterDirty() const { return impl_->gpu_raster != nullptr && impl_->gpu_raster->rasterDirty(); }

}  // namespace lift
