#include "runtime/present_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/frame_blit.h"
#include "runtime/guest_token.h"  // ★ M0：SDL_RenderPresent 的宿主等待要放令牌（见 submitFrame）
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/overlay.h"

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
// ★ 输入消息 / 鼠标键位 / WM_* 常量在 `runtime/input.h`（**唯一一份**：SDL 与注入脚本共用）。
//   本文件只保留 SDL 平台特有的映射（SDL_Keycode → VK）与关窗宽限。
constexpr std::uint32_t kWmClose = 0x0010u;
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

// SDL 鼠标按钮 → 应用层的 `MouseButton`；不认识的按钮 ⇒ 没有对应值（调用方跳过）。
bool mouseButtonFromSdl(Uint8 button, MouseButton* out) {
    switch (button) {
        case SDL_BUTTON_LEFT: *out = MouseButton::Left; return true;
        case SDL_BUTTON_RIGHT: *out = MouseButton::Right; return true;
        case SDL_BUTTON_MIDDLE: *out = MouseButton::Middle; return true;
        default: return false;
    }
}

// ---- 调试叠层（`--overlay`）----
// ★ fps 的滚动窗口 = 最近 1 s；`kOverlayFpsWindowNs` 是这条口径的唯一定义。
constexpr std::uint64_t kOverlayFpsWindowNs = 1000ull * 1000ull * 1000ull;
// ★ 文本重建 / 上传的节流间隔：`frame` 每帧都变，不节流就每帧重传（直出还会多一次 submit）。
//   100 ms 对 fps / frame 的滞后上限；鼠标位置变化**不受**这个节流（见 `overlayRefreshDue`）。
constexpr std::uint64_t kOverlayRefreshNs = 100ull * 1000ull * 1000ull;  // 100 ms

std::string joinOverlayLines(const std::vector<std::string>& lines) {
    std::string key;
    for (const std::string& line : lines) {
        key += line;
        key.push_back('\n');
    }
    return key;
}

// `worst` 行的数值按 `%.1f` 显示 ⇒ 只有**这一位小数**变了才算"文本变了"
// （否则每帧一个微小差值都会让 dirty 恒真，节流形同虚设，见 `kOverlayRefreshNs`）。
bool sameTenth(double a, double b) {
    char ba[32];
    char bb[32];
    std::snprintf(ba, sizeof(ba), "%.1f", a);
    std::snprintf(bb, sizeof(bb), "%.1f", b);
    return std::strcmp(ba, bb) == 0;
}

// 推来的 status 会不会改变**画出来的文本**（⛔ 不是"结构体字节变没变"）。
bool overlayStatusTextChanged(const OverlayStatus& a, const OverlayStatus& b) {
    if (a.has_script != b.has_script || a.script != b.script) return true;
    if (a.has_worst != b.has_worst || a.has_guest != b.has_guest) return true;
    if (!a.has_worst) return false;
    return !sameTenth(a.wall_max_ms, b.wall_max_ms) || !sameTenth(a.guest_max_ms, b.guest_max_ms) ||
           !sameTenth(a.shim_max_ms, b.shim_max_ms);
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
    std::uint64_t screenshot_frame = 0;  // 0 = 每帧；N = 只在第 N 个 Present 回读
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
    // ★ 夹具缝（`setGpuRasterForTest`）：非空时所有**转发**都到它，生命周期归夹具。
    //   ⛔ 生产路径永远是 nullptr；`init`/`shutdown` 只碰 `gpu_raster`（所有权那一份）。
    GpuRaster* raster_view = nullptr;
    // ★ 夹具缝（`setDirectPresenterForTest`）：只替换直出呈现器，不经 `raster()`。
    //   生命周期归夹具；`init` 时把它接到 `direct` 上并置 `direct_present = true`。
    GpuDirectPresenter* direct_view = nullptr;
    // 转发壳的**唯一**取用点：所有 `SdlWindow::raster*` 都走它（⛔ 不许各处直接写 `gpu_raster`，
    // 否则夹具缝与"漏转发"两件事都会各自漏一条）。
    GpuRaster* raster() const { return raster_view != nullptr ? raster_view : gpu_raster.get(); }
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
    // ★ 键电平 / 按钮位图 / 虚拟光标**不在**这里：它们只在 `WindowManager`（唯一状态源），
    //   本类只把 SDL 事件翻译过去（`runtime/input.h`）。⛔ 不要再加一份 `mouse_buttons`。
    WindowManager* windows = nullptr;
    bool close_pending = false;
    int close_frames_left = 0;
    std::uint64_t close_deadline_ns = 0;
    // ---- 宿主自绘模态框（`runtime/message_box.h`）----
    // ★ 模态期间**不**走 `submitFrame` ⇒ 3 帧关窗宽限天然不动；但 `pumpEvents` 里那条
    //   500 ms 硬截止会走 ⇒ 用 `modal_active` 挂起它（AGERC 单的建议 2，见设计 §7.2）。
    //   X 在模态期间只置 `modal_close_requested`（⛔ 不 post WM_CLOSE、⛔ 不 stop）；
    //   Esc 在 `handleEvent` 里直接跳过 `requestClose`（模态只答框）。
    bool modal_active = false;
    bool modal_close_requested = false;
    SDL_Texture* modal_tex = nullptr;  // SDL_Renderer 路径：整幅模态图的纹理
    int modal_tex_w = 0;
    int modal_tex_h = 0;
    // 直出路径：本模态在 `directOverlay` 上留过图 ⇒ 收口时必须**释放**它（否则窗口停在这张图）。
    bool modal_direct_overlay = false;
    // boot（还没有任何 guest 帧）时给直出 target 铺的那张黑底：一次模态里只传一次
    // （悬停/按下会让模态多画几帧；⛔ 不必每帧重传 1280×720）。
    bool modal_target_uploaded = false;

    // ---- 调试叠层（`--overlay`，默认关）----
    // ★ 两条呈现路径共用**同一份**行 / 尺寸（`overlayLines`），只有上传方式不同。
    //   SDL_Renderer 路径：一张 STREAMING 小纹理；GPU 直出路径：后端 device 上的纹理
    //   （`GpuDirectPresenter::directOverlay`）。
    bool overlay = false;
    bool overlay_dirty = true;        // 开关变化 / 上帧失败后强制重传
    std::string overlay_text_key;     // 上一次**上传**的内容；相同就不重传
    SDL_Texture* overlay_tex = nullptr;  // 只有 SDL_Renderer 路径用
    int overlay_tex_w = 0;
    int overlay_tex_h = 0;
    int overlay_w = 0;  // 直出路径上次上传的尺寸
    int overlay_h = 0;
    // ★ 节流：文本（`frame` 每帧都变）最多每 `kOverlayRefreshNs` 重建 / 上传一次；
    //   鼠标位置变了**立即**重建（用户可感知的那一项优先）。fps / frame 允许滞后 ≤ 100 ms。
    std::uint64_t overlay_last_build_ns = 0;  // 0 = 还没生成过
    int overlay_last_mouse_x = 0;
    int overlay_last_mouse_y = 0;
    bool overlay_last_mouse_valid = false;
    // 最近 1 s 的 Present 时刻（单调 ns；只用于 fps 那一行；叠层关闭时一次也不记）
    std::vector<std::uint64_t> present_times_ns;
    // ★ `Machine` 推来的两行数据（`script` 身份 / `worst` 3 s 最大值）；不画叠层时也只是几个字节。
    OverlayStatus overlay_status;

    void notePresentTime();
    bool overlayMousePos(int* x, int* y) const;
    bool overlayRefreshDue(int mx, int my, bool mouse_ok, std::uint64_t now_ns) const;
    void overlayMarkBuilt(int mx, int my, bool mouse_ok, std::uint64_t now_ns);
    std::vector<std::string> overlayLines() const;
    bool drawOverlayRenderer(std::string* err);
    bool drawOverlayDirect(std::string* err);
    // 叠层画不出来 ⇒ 只告警 + 关掉叠层，呈现照旧（⛔ 不因为诊断层让整帧失败）。
    void disableOverlay(const char* why);

    void postMessage(std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam);
    void requestStop();
    void requestClose();
    void advanceCloseFrame();
    void handleEvent(const SDL_Event& event);
    void pumpEvents(int wait_ms);
    void mapMouse(float mx, float my, std::int32_t* gx, std::int32_t* gy) const;
    // 本帧要不要回读（截图 / 测试捕获）。frame_no 必须已经自增。
    bool shotWanted() const;
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
    // ★ 模态期间：X 只置位，不 post WM_CLOSE、不启动宽限（关窗要在答完框之后才发生）。
    if (modal_active) {
        modal_close_requested = true;
        return;
    }
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
    if (!close_pending || stop || modal_active) return;
    if (close_frames_left > 0) --close_frames_left;
    if (close_frames_left <= 0 || SDL_GetTicksNS() >= close_deadline_ns) requestStop();
}

bool SdlWindow::Impl::shotWanted() const {
    if (capture) return true;
    if (screenshot_path.empty()) return false;
    return screenshot_frame == 0 || frame_no == screenshot_frame;
}

// ---------------------------------------------------------------------------------------
// 调试叠层（`--overlay`）：行 / 时间窗 / 两条路径各自的上传与绘制
// ---------------------------------------------------------------------------------------
// ★ 鼠标坐标的真源 = `WindowManager::cursorClientPos(hwnd)` —— 与 `click x y` /
//   `--input-script` 用的是**同一格**（guest 客户区坐标）。没有主窗口 ⇒ 整行省略。
void SdlWindow::Impl::notePresentTime() {
    if (!overlay) return;  // ★ 叠层关闭时一次也不记（零成本）
    const std::uint64_t now = SDL_GetTicksNS();
    present_times_ns.push_back(now);
    const std::uint64_t cutoff = now > kOverlayFpsWindowNs ? now - kOverlayFpsWindowNs : 0;
    std::size_t drop = 0;
    while (drop < present_times_ns.size() && present_times_ns[drop] < cutoff) ++drop;
    if (drop > 0) {
        present_times_ns.erase(present_times_ns.begin(),
                               present_times_ns.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    // 兜底上限：窗口极长且从不清理时也不让这个向量无限长（正常 1 s 窗口远小于它）。
    if (present_times_ns.size() > 8192) present_times_ns.erase(present_times_ns.begin(), present_times_ns.begin() + 4096);
}

// 光标在**当前主窗口**的客户区坐标（= `click x y` 的同一格）。false = 还没有 guest 窗口。
bool SdlWindow::Impl::overlayMousePos(int* x, int* y) const {
    if (windows == nullptr) return false;
    const std::uint32_t hwnd = windows->primaryWindow();
    if (hwnd == 0) return false;
    windows->cursorClientPos(hwnd, x, y);
    return true;
}

// ★ 节流判据（两条路径共用同一份口径）：
//   ① 开关刚变 / 还没生成过 ⇒ 立刻；
//   ② **鼠标位置变了** ⇒ 立刻（用户可感知的那一项优先）；
//   ③ 否则最多每 `kOverlayRefreshNs`（100 ms）重建 / 上传一次 —— `frame` 每帧都变，
//      不节流就是每帧一次 CPU 栅格化 + 一次纹理上传（直出还多一次 submit）。
bool SdlWindow::Impl::overlayRefreshDue(int mx, int my, bool mouse_ok, std::uint64_t now_ns) const {
    if (overlay_dirty || overlay_last_build_ns == 0) return true;
    if (mouse_ok != overlay_last_mouse_valid) return true;
    if (mouse_ok && (mx != overlay_last_mouse_x || my != overlay_last_mouse_y)) return true;
    return now_ns - overlay_last_build_ns >= kOverlayRefreshNs;
}

void SdlWindow::Impl::overlayMarkBuilt(int mx, int my, bool mouse_ok, std::uint64_t now_ns) {
    overlay_last_build_ns = now_ns;
    overlay_last_mouse_valid = mouse_ok;
    overlay_last_mouse_x = mx;
    overlay_last_mouse_y = my;
    overlay_dirty = false;
}

std::vector<std::string> SdlWindow::Impl::overlayLines() const {
    OverlayState st;
    // frame = Present 计数（`frame_no`；调用方已经自增过这一帧）。
    st.frame_available = true;
    st.frame = frame_no;
    if (windows != nullptr) {
        const std::uint32_t hwnd = windows->primaryWindow();
        if (hwnd != 0) {
            int mx = 0;
            int my = 0;
            windows->cursorClientPos(hwnd, &mx, &my);
            st.mouse_available = true;
            st.mouse_x = mx;
            st.mouse_y = my;
        }
    }
    if (!present_times_ns.empty()) {
        st.fps_available = true;
        st.fps = static_cast<double>(present_times_ns.size());  // 最近 1 s 的 Present 次数
    }
    // raster = 这一帧**真的**由谁光栅化（不是"想要谁"）：后端未激活就如实写 cpu。
    const GpuRaster* r = raster();
    st.raster = (r != nullptr && r->rasterActive()) ? "gpu" : "cpu";
    st.present = (direct_present && direct != nullptr) ? "direct" : "renderer";
    return formatOverlayLines(st, overlay_status);
}

void SdlWindow::Impl::disableOverlay(const char* why) {
    std::fprintf(stderr, "[overlay] 叠层不可用 ⇒ 关掉（呈现照旧）：%s\n", why != nullptr ? why : "(unknown)");
    overlay = false;
    overlay_dirty = true;
    overlay_text_key.clear();
    overlay_w = 0;
    overlay_h = 0;
    if (overlay_tex != nullptr) {
        SDL_DestroyTexture(overlay_tex);
        overlay_tex = nullptr;
    }
    overlay_tex_w = 0;
    overlay_tex_h = 0;
    if (direct != nullptr) {
        std::string ignored;
        direct->directOverlay(nullptr, 0, 0, &ignored);
    }
}

// SDL_Renderer 路径：小纹理（尺寸随文本变）+ 节流后才 `SDL_UpdateTexture`，画在主纹理之后。
// ★ 没到重建时间也要**每帧画**（纹理还在，画面每帧都要有叠层）；重建/上传按 `overlayRefreshDue`。
bool SdlWindow::Impl::drawOverlayRenderer(std::string* err) {
    if (renderer == nullptr) return true;
    int mx = 0;
    int my = 0;
    const bool mouse_ok = overlayMousePos(&mx, &my);
    const std::uint64_t now = SDL_GetTicksNS();
    if (overlayRefreshDue(mx, my, mouse_ok, now)) {
        const std::vector<std::string> lines = overlayLines();
        const OverlaySize size = overlaySize(lines);
        if (size.w <= 0 || size.h <= 0) {
            overlayMarkBuilt(mx, my, mouse_ok, now);
            return true;
        }
        if (overlay_tex == nullptr || overlay_tex_w != size.w || overlay_tex_h != size.h) {
            if (overlay_tex != nullptr) SDL_DestroyTexture(overlay_tex);
            // ★ 字节序：叠层图像是**内存序 RGBA8**（`renderOverlay` 写 R,G,B,A）。
            //   SDL 的像素格式名按 u32 的位序定义 ⇒ 内存序 R,G,B,A 对应 `ABGR8888`
            //   （A 在最高位 ⇒ 小端内存里的字节序是 R,G,B,A）。用 `RGBA8888` 会变成
            //   A,B,G,R ⇒ 颜色整体错位（实测：深底 0x101820 被读成 0xFF2018）。
            overlay_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                            size.w, size.h);
            if (overlay_tex == nullptr) {
                if (err) *err = std::string("SDL_CreateTexture(overlay) 失败：") + SDL_GetError();
                return false;
            }
            // ★ 不透明底：混合模式必须是 NONE（默认混合会按 alpha 混，深底就变半透明了）。
            if (!SDL_SetTextureBlendMode(overlay_tex, SDL_BLENDMODE_NONE)) {
                if (err) *err = std::string("SDL_SetTextureBlendMode(overlay, NONE) 失败：") + SDL_GetError();
                return false;
            }
            if (!SDL_SetTextureScaleMode(overlay_tex, SDL_SCALEMODE_NEAREST)) {
                if (err) *err = std::string("SDL_SetTextureScaleMode(overlay, NEAREST) 失败：") + SDL_GetError();
                return false;
            }
            overlay_tex_w = size.w;
            overlay_tex_h = size.h;
            overlay_text_key.clear();  // 新纹理 ⇒ 必须重传
        }
        const std::string key = joinOverlayLines(lines);
        if (key != overlay_text_key) {
            const std::vector<std::uint8_t> image = renderOverlayImage(lines, nullptr, nullptr);
            if (image.empty() || !SDL_UpdateTexture(overlay_tex, nullptr, image.data(), size.w * 4)) {
                if (err) *err = std::string("SDL_UpdateTexture(overlay) 失败：") + SDL_GetError();
                return false;
            }
            overlay_text_key = key;
        }
        overlayMarkBuilt(mx, my, mouse_ok, now);
    }
    if (overlay_tex == nullptr || overlay_tex_w <= 0 || overlay_tex_h <= 0) return true;
    // ★ 逻辑坐标 (0,0) = **呈现画面**左上角（`SDL_SetRenderLogicalPresentation(LETTERBOX)` 生效时
    //   窗口像素 ≠ 逻辑坐标；这里要的是叠在画面上，不是叠在黑边里）。
    const SDL_FRect dst{0.0f, 0.0f, static_cast<float>(overlay_tex_w), static_cast<float>(overlay_tex_h)};
    if (!SDL_RenderTexture(renderer, overlay_tex, nullptr, &dst)) {
        if (err) *err = std::string("SDL_RenderTexture(overlay) 失败：") + SDL_GetError();
        return false;
    }
    return true;
}

// GPU 直出路径：把同一份 RGBA8 图像交给后端（同一 device / swapchain 格式纹理）；
// 真正的绘制在 `directPresent` 的 letterbox blit 之后。节流见 `overlayRefreshDue`。
bool SdlWindow::Impl::drawOverlayDirect(std::string* err) {
    if (direct == nullptr) return true;
    int mx = 0;
    int my = 0;
    const bool mouse_ok = overlayMousePos(&mx, &my);
    const std::uint64_t now = SDL_GetTicksNS();
    if (overlayRefreshDue(mx, my, mouse_ok, now)) {
        const std::vector<std::string> lines = overlayLines();
        const OverlaySize size = overlaySize(lines);
        if (size.w <= 0 || size.h <= 0) {
            overlayMarkBuilt(mx, my, mouse_ok, now);
            return true;
        }
        const std::string key = joinOverlayLines(lines);
        if (overlay_dirty || key != overlay_text_key || overlay_w != size.w || overlay_h != size.h) {
            const std::vector<std::uint8_t> image = renderOverlayImage(lines, nullptr, nullptr);
            if (image.empty()) return true;
            if (!direct->directOverlay(image.data(), size.w, size.h, err)) return false;
            overlay_w = size.w;
            overlay_h = size.h;
            overlay_text_key = key;
        }
        overlayMarkBuilt(mx, my, mouse_ok, now);
    }
    return true;
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
    const std::uint32_t now_ms = static_cast<std::uint32_t>(SDL_GetTicks());
    switch (event.type) {
        case SDL_EVENT_QUIT:
            // 应用级退出：直接 post WM_QUIT（不再走 guest 的 WM_CLOSE 确认）。
            requestStop();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (window != nullptr && event.window.windowID == SDL_GetWindowID(window)) requestClose();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // ★ 焦点丢失只清 **OS 拥有** 的键 / 按钮（见 host_window.h 的 InputSource）：
            //   脚本注入的按下不因窗口失焦而丢 —— 那正是"注入不依赖焦点"的一半。
            if (windows != nullptr) windows->clearOsInput();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const bool down = event.type == SDL_EVENT_KEY_DOWN;
            const std::uint32_t vk = vkFromSdlKey(event.key.key);
            if (vk == 0) break;
            const std::uint32_t scan = event.key.scancode ? static_cast<std::uint32_t>(event.key.scancode) : 0u;
            // ★ 注入路径与真人路径在**同一段代码**里：状态 + WM_* 一起落地（所有权判定在里面）。
            if (windows != nullptr) input::key(*windows, vk, down, scan, InputSource::Os, now_ms);
            // ESC = 用户要退出（与旧行为一致）。★ 模态期间**只答框**：⛔ 不启动关窗宽限
            //   （否则一次 Esc 既答了框、又让窗口开始关；见 `modal_active` 那一段）。
            if (down && vk == 0x1B && !modal_active) requestClose();
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.motion.x, event.motion.y, &x, &y);
            if (std::getenv("LIFT_TRACE_INPUT") != nullptr) {
                std::fprintf(stderr, "[input] motion sdl=(%.1f,%.1f) -> guest=(%d,%d)\n",
                             event.motion.x, event.motion.y, x, y);
            }
            if (windows != nullptr) input::mouseMove(*windows, x, y, InputSource::Os, now_ms);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            MouseButton b = MouseButton::Left;
            if (!mouseButtonFromSdl(event.button.button, &b)) break;
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.button.x, event.button.y, &x, &y);
            if (windows != nullptr) input::mouseButton(*windows, x, y, b, down, InputSource::Os, now_ms);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            const int delta = static_cast<int>(std::lround(event.wheel.y * 120.0f));
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.wheel.mouse_x, event.wheel.mouse_y, &x, &y);
            if (windows != nullptr) input::mouseWheel(*windows, x, y, delta, InputSource::Os, now_ms);
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
    // ★ 500 ms 硬截止在模态期间必须挂起（模态就是等人；否则弹框会被"关窗"打断）。
    if (!modal_active && close_pending && !stop && SDL_GetTicksNS() >= close_deadline_ns) requestStop();
}

SdlWindow::SdlWindow() : impl_(std::make_unique<Impl>()) {}
SdlWindow::~SdlWindow() { shutdown(); }

bool SdlWindow::init(const SdlWindowOptions& options, std::string* err) {
    if (impl_->window != nullptr) return true;
    impl_->screenshot_path = options.screenshot_path;
    impl_->screenshot_frame = options.screenshot_frame;
    impl_->raster_gpu = options.raster_gpu;
    impl_->raster_sprites = options.raster_sprites;
    impl_->overlay = options.overlay;
    impl_->overlay_dirty = true;

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
    // ★ 夹具缝优先：`setDirectPresenterForTest` 注入的替身**不 claim** 窗口（见头文件），
    //   只用来在无 GPU 机器上钉 `presentModal` 的直出失败清理（M6）。
    if (impl_->direct_view != nullptr) {
        impl_->direct = impl_->direct_view;
        impl_->direct_present = true;
        impl_->renderer_name = "fake-direct";
        impl_->gpu_driver = "fake";
    } else if (impl_->gpu_raster != nullptr) {
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
    impl_->direct_view = nullptr;  // 夹具替身已 detach；⛔ 不要让同一个 SdlWindow 重新 init 时还挂着它
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
    // ★ 叠层纹理属于 renderer ⇒ 必须在 `SDL_DestroyRenderer` **之前**释放。
    if (impl_->overlay_tex != nullptr) {
        SDL_DestroyTexture(impl_->overlay_tex);
        impl_->overlay_tex = nullptr;
    }
    // 模态纹理同理（它也只活在 renderer 上）。
    if (impl_->modal_tex != nullptr) {
        SDL_DestroyTexture(impl_->modal_tex);
        impl_->modal_tex = nullptr;
    }
    impl_->modal_tex_w = 0;
    impl_->modal_tex_h = 0;
    impl_->modal_active = false;
    impl_->modal_close_requested = false;
    impl_->modal_direct_overlay = false;
    impl_->modal_target_uploaded = false;
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
    impl_->close_pending = false;
    impl_->close_frames_left = 0;
    impl_->close_deadline_ns = 0;
    impl_->frame_no = 0;
    impl_->screenshot_path.clear();
    impl_->screenshot_frame = 0;
    impl_->renderer_name.clear();
    impl_->video_driver.clear();
    impl_->gpu_driver.clear();
    impl_->capture_argb.clear();
    impl_->capture_w = 0;
    impl_->capture_h = 0;
    // 叠层状态（纹理已在 renderer 之前释放；直出那一份随 `directDetachWindow` 释放）。
    impl_->overlay_tex_w = 0;
    impl_->overlay_tex_h = 0;
    impl_->overlay_w = 0;
    impl_->overlay_h = 0;
    impl_->overlay = false;
    impl_->overlay_dirty = true;
    impl_->overlay_text_key.clear();
    impl_->present_times_ns.clear();
    impl_->overlay_status = OverlayStatus{};
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
    impl_->notePresentTime();
    // ★ 叠层画在主纹理**之后**、截图**之前** ⇒ `--present-screenshot` 里可见（窗口像素）。
    //   ⛔ `--dump-frame` 读的是 guest arena，天然不含它。
    if (impl_->overlay) {
        std::string oerr;
        if (!impl_->drawOverlayRenderer(&oerr)) impl_->disableOverlay(oerr.c_str());
    }

    if (impl_->shotWanted()) {
        SDL_Surface* shot = SDL_RenderReadPixels(impl_->renderer, nullptr);
        if (shot == nullptr) {
            if (err) *err = std::string("SDL_RenderReadPixels 失败：") + SDL_GetError();
            return false;
        }
        const bool shot_ok = impl_->saveShot(shot, err);
        SDL_DestroySurface(shot);
        if (!shot_ok) return false;
    }

    // ★ M0（REQ-01M75WWFGT8ZEBGEPENQHSYTB8）：`SDL_RenderPresent` 会**等呈现队列 / vsync**
    //   （阻塞型宿主等待），而令牌只保护宿主容器 ⇒ 在 `YieldGuard` 里做（临时让出、回来取回）。
    //   **为什么放令牌安全**：① 这条 sink 按 `present.h` 的**单线程契约**只在主线程被调用，
    //   `impl_`（`SdlWindow::Impl`：window / renderer / texture）没有第二个线程会改；
    //   ② 让出这段里**不持有任何宿主容器引用** —— guest 像素早在上面 `SDL_UpdateTexture` 就
    //   拷进纹理了（arena / surface 的指针没有跨过这里）；③ 取回后不沿用任何放令牌之前的查表结论。
    bool presented = false;
    {
        GuestToken::YieldGuard yield(guestToken());
        presented = SDL_RenderPresent(impl_->renderer);
    }
    if (!presented) {
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
    if (!screenshot_path.empty() && (screenshot_frame == 0 || frame_no == screenshot_frame)) {
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
    notePresentTime();
    // ★ 叠层：先把 RGBA8 交给后端（文本不变不重传），真正的 blit 在 `directPresent` 的
    //   letterbox 之后。画不出来 ⇒ 关掉叠层，呈现照旧（⛔ 不因为诊断层让整帧失败）。
    if (overlay) {
        std::string oerr;
        if (!drawOverlayDirect(&oerr)) disableOverlay(oerr.c_str());
    }
    if (!direct->directPresent(err)) return false;
    // 一帧用过就消费掉：下一位调用方（DDraw Flip / 直接 submitFrame）仍然会先上传 guest。
    direct->directSetTargetCurrent(false);
    if (shotWanted()) {
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

// =======================================================================================
// 宿主自绘模态框（`runtime/message_box.h` 的 ModalPresenter）
// ★ 一次贴一整幅**不透明 RGBA8**（CPU 已合成好）：两条呈现路径都只需要一次拷贝式 blit，
//   不需要混合 ⇒ 同一个像素结果（软件渲染器上也能断言）。
// ★ ⛔ `presentModal` **不** `++frame_no`、**不** 盖 present-tail 章、**不** 消耗关窗宽限帧：
//   帧号 / 帧预算 / 运行摘要不被对话框污染（设计 §7.2 的裁决）。
// =======================================================================================
bool SdlWindow::presentModal(const std::uint8_t* rgba, int w, int h, std::string* err) {
    if (rgba == nullptr || w <= 0 || h <= 0) {
        if (err) *err = "模态帧为空 / 尺寸非法";
        return false;
    }
    impl_->modal_active = true;
    // ---- 路径 ①：GPU 直出（1:1 贴到画面原点；窗口默认 == 后台缓冲 ⇒ 正常路径恰好正确）----
    if (impl_->direct_present && impl_->direct != nullptr) {
        // boot（还没有任何 guest 帧）⇒ 直出的 target 可能还没建：先铺一张黑底当"画面"，
        // 否则 `directPresent` 会以 "target 未准备" 失败（模态图本身已含变暗的底）。
        if (!impl_->sized && !impl_->modal_target_uploaded) {
            std::vector<std::uint8_t> black(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0u);
            for (std::size_t i = 3; i < black.size(); i += 4) black[i] = 255;
            const SurfaceFormat fmt{};  // 32bpp BGRA（直出 target 的固定格式）
            if (!impl_->direct->directUploadGuest(fmt, black.data(), static_cast<std::uint32_t>(w) * 4u, w, h,
                                                  err)) {
                return false;
            }
            impl_->modal_target_uploaded = true;
        }
        if (!impl_->direct->directOverlay(rgba, w, h, err)) return false;
        // ★ 只要 overlay 上传成功，就记下"本模态在直出叠层上留过图"——**即使下面 present 失败**
        //   也必须在 `endModal` 里清掉（M6 复审：窗口 resize / swapchain out-of-date / device lost
        //   都会让 `directPresent` 失败；不清理则模态纹理留在 GPU 上一直盖住后续帧）。
        impl_->modal_direct_overlay = true;
        // ★ 一帧用过就消费掉（成功 / 失败都算）：下一次 `submitFrame` 仍会先把 guest 上传
        //   （boot 分支的 `directUploadGuest` 会把 `target_current_` 置 true；⛔ 不许停在模态那一帧）。
        impl_->direct->directSetTargetCurrent(false);
        if (!impl_->direct->directPresent(err)) return false;
        // 可选像素证据（与 renderer 路径同一口径：`directReadback` 会合成叠层 = 模态图）。
        if (impl_->shotWanted()) {
            std::vector<std::uint8_t> xrgb;
            int rw = 0;
            int rh = 0;
            if (!impl_->direct->directReadback(&xrgb, &rw, &rh, err)) return false;
            SDL_Surface* shot = SDL_CreateSurfaceFrom(rw, rh, SDL_PIXELFORMAT_XRGB8888, xrgb.data(), rw * 4);
            if (shot == nullptr) {
                if (err) *err = std::string("SDL_CreateSurfaceFrom(modal) 失败：") + SDL_GetError();
                return false;
            }
            const bool shot_ok = impl_->saveShot(shot, err);
            SDL_DestroySurface(shot);
            if (!shot_ok) return false;
        }
        return true;
    }
    // ---- 路径 ②：SDL_Renderer 上传（逻辑坐标 = 整幅画面；LETTERBOX 自动吞掉黑边）----
    if (impl_->renderer == nullptr) {
        if (err) *err = "SDL 窗口未初始化";
        return false;
    }
    SDL_SetRenderTarget(impl_->renderer, nullptr);
    SDL_SetRenderViewport(impl_->renderer, nullptr);
    SDL_SetRenderClipRect(impl_->renderer, nullptr);
    if (!SDL_SetRenderLogicalPresentation(impl_->renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        if (err) *err = std::string("SDL_SetRenderLogicalPresentation(modal) 失败：") + SDL_GetError();
        return false;
    }
    if (impl_->modal_tex == nullptr || impl_->modal_tex_w != w || impl_->modal_tex_h != h) {
        if (impl_->modal_tex != nullptr) SDL_DestroyTexture(impl_->modal_tex);
        // ★ 字节序：模态图是**内存序 RGBA8**（composeModalFrame 写 R,G,B,A）；SDL 的格式名按
        //   u32 位序 ⇒ 内存序 R,G,B,A = `ABGR8888`（与叠层 / 主纹理同一条口径，见 drawOverlayRenderer）。
        impl_->modal_tex = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, w, h);
        if (impl_->modal_tex == nullptr) {
            if (err) *err = std::string("SDL_CreateTexture(modal) 失败：") + SDL_GetError();
            return false;
        }
        // 不透明：混合必须是 NONE（默认混合会按 alpha 混，面板就变半透明了）。
        if (!SDL_SetTextureBlendMode(impl_->modal_tex, SDL_BLENDMODE_NONE) ||
            !SDL_SetTextureScaleMode(impl_->modal_tex, SDL_SCALEMODE_NEAREST)) {
            if (err) *err = std::string("SDL_SetTexture(modal) 状态失败：") + SDL_GetError();
            return false;
        }
        impl_->modal_tex_w = w;
        impl_->modal_tex_h = h;
    }
    if (!SDL_UpdateTexture(impl_->modal_tex, nullptr, rgba, w * 4)) {
        if (err) *err = std::string("SDL_UpdateTexture(modal) 失败：") + SDL_GetError();
        return false;
    }
    SDL_SetRenderDrawColor(impl_->renderer, 0, 0, 0, 255);
    SDL_RenderClear(impl_->renderer);
    if (!SDL_RenderTexture(impl_->renderer, impl_->modal_tex, nullptr, nullptr)) {
        if (err) *err = std::string("SDL_RenderTexture(modal) 失败：") + SDL_GetError();
        return false;
    }
    // ★ 可选：模态帧也走**同一份窗口像素回读**（`--present-screenshot` / 测试捕获）——
    //   这是"框真的画上了"的唯一像素证据。⛔ 帧号**不自增**：文件名里的 {frame} 仍是最后一只
    //   guest 帧的号（boot 还没 Present 过 ⇒ 0），与"present-tail 不被污染"是同一条裁决。
    if (impl_->shotWanted()) {
        SDL_Surface* shot = SDL_RenderReadPixels(impl_->renderer, nullptr);
        if (shot == nullptr) {
            if (err) *err = std::string("SDL_RenderReadPixels(modal) 失败：") + SDL_GetError();
            return false;
        }
        const bool shot_ok = impl_->saveShot(shot, err);
        SDL_DestroySurface(shot);
        if (!shot_ok) return false;
    }
    // ★ 与 `submitFrame` 同一条 M0 口径：`SDL_RenderPresent` 是会等的宿主调用 ⇒ 放令牌。
    bool presented = false;
    {
        GuestToken::YieldGuard yield(guestToken());
        presented = SDL_RenderPresent(impl_->renderer);
    }
    if (!presented) {
        if (err) *err = std::string("SDL_RenderPresent(modal) 失败：") + SDL_GetError();
        return false;
    }
    return true;
}

void SdlWindow::waitHostEvents(int wait_ms) {
    // 端到端等待（真实睡）；调用方（runModalDialog）已经把它套在 YieldGuard 里。
    if (impl_->window == nullptr) return;
    impl_->pumpEvents(wait_ms);
}

bool SdlWindow::closeRequested() const { return impl_->modal_close_requested; }
bool SdlWindow::modalActive() const { return impl_->modal_active; }

void SdlWindow::endModal() {
    if (!impl_->modal_active) return;
    impl_->modal_active = false;
    const bool close_after = impl_->modal_close_requested;
    impl_->modal_close_requested = false;
    if (impl_->modal_tex != nullptr) {
        SDL_DestroyTexture(impl_->modal_tex);
        impl_->modal_tex = nullptr;
    }
    impl_->modal_tex_w = 0;
    impl_->modal_tex_h = 0;
    impl_->modal_target_uploaded = false;
    // ★ 逻辑呈现尺寸恢复成**最后一只 guest 帧**的尺寸（模态帧每帧都设 (w,h)，而 `submitFrame`
    //   只在纹理重建时设 ⇒ 不恢复的话，异常路径下可能把 guest 逻辑尺寸停在模态那一份上）。
    if (impl_->renderer != nullptr && impl_->texture != nullptr && impl_->tex_w > 0 && impl_->tex_h > 0) {
        SDL_SetRenderLogicalPresentation(impl_->renderer, impl_->tex_w, impl_->tex_h,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
    // 直出路径：模态图是**叠层纹理**，收口时必须显式释放（否则窗口永远停在框上）。
    if (impl_->modal_direct_overlay) {
        impl_->modal_direct_overlay = false;
        if (impl_->direct != nullptr) {
            std::string ignored;
            impl_->direct->directOverlay(nullptr, 0, 0, &ignored);
            // ★ 与 `presentModal` 同一口径：模态期间 target 的"最新像素"属于模态，不是 guest
            //   ⇒ 收口时**无条件**消费掉（即使上一次 `directPresent` 失败，next submit 也要重新上传）。
            impl_->direct->directSetTargetCurrent(false);
        }
        // 真·调试叠层（`--overlay`）下一帧要重传（它的尺寸/内容键已经不对了）。
        impl_->overlay_w = 0;
        impl_->overlay_h = 0;
        impl_->overlay_text_key.clear();
    }
    // 用户真的点了 X（不是 Esc）⇒ 答完框之后才启动常规关窗宽限。
    if (close_after) impl_->requestClose();
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

// 调试叠层开关：关掉时**立刻**把两条路径的叠层状态都释放（renderer 纹理 / 直出纹理 + CPU 副本
// —— 直出那一份由后端 `directOverlay(nullptr,...)` **真的释放** GPU 纹理，不是只置 flag），
// 下一帧起窗口像素恢复成"只有画面"。默认关（`--overlay` 才打开）。
void SdlWindow::setOverlay(bool on) {
    impl_->overlay = on;
    impl_->overlay_dirty = true;
    // 开关变化 ⇒ 丢掉节流状态：下次开启时立刻重建，不受上一轮 100 ms 窗口影响。
    impl_->overlay_last_build_ns = 0;
    impl_->overlay_last_mouse_valid = false;
    if (on) return;
    impl_->overlay_text_key.clear();
    impl_->overlay_w = 0;
    impl_->overlay_h = 0;
    if (impl_->overlay_tex != nullptr) {
        SDL_DestroyTexture(impl_->overlay_tex);
        impl_->overlay_tex = nullptr;
    }
    impl_->overlay_tex_w = 0;
    impl_->overlay_tex_h = 0;
    if (impl_->direct != nullptr) {
        std::string ignored;
        impl_->direct->directOverlay(nullptr, 0, 0, &ignored);
    }
}

bool SdlWindow::overlayEnabled() const { return impl_->overlay; }

// ★ `Machine` → 窗口 sink 的两行数据（`script` / `worst`）。只在**文本会变**时让 dirty：
//   · 身份（script 名字 / 有-无）变了 ⇒ 立刻重建（用户可感知的那一项优先，与鼠标同一档）；
//   · `worst` 的三个最大值**按显示精度**（一位小数）变了 ⇒ 重建；
//   · 其余情况**不**动 dirty ⇒ 仍走既有的 100 ms 节流（⛔ 不每帧重传纹理）。
//   ★ 这里只存数据、不重建文本：两条呈现路径下一帧都会走 `overlayLines()`（同一份格式化）。
void SdlWindow::setOverlayStatus(const OverlayStatus& status) {
    if (overlayStatusTextChanged(impl_->overlay_status, status)) impl_->overlay_dirty = true;
    impl_->overlay_status = status;
}

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
// ★ 本类是 `GpuRaster` 的**转发壳**：除 `rasterName`（缓存身份）与 `rasterSpritesEnabled`
//   （壳级 A/B 与后端能力的合成）两条**有意的非纯转发**外，每一条虚函数都必须显式转发到
//   `impl_->raster()`。漏一条 == 后端"实现了但永远收不到"（静默；不报错、不改像素、测试全绿）——
//   `rasterRequestDepthClear` 就这么漏过，代价是"GPU 臂整层丢 Live2D 角色"
//   （`REQ-01M73P3GXW22Y467YGYPQSM571`）。守卫：`tests/gpu_batch.cpp::caseDepthClearForwarded`
//   逐条断言"调用真的到后端了"（用 `setGpuRasterForTest` 注入替身，不需要 GPU 设备）。
// ---------------------------------------------------------------------------------------
GpuRaster* SdlWindow::gpuRaster() { return impl_->raster() != nullptr ? this : nullptr; }

bool SdlWindow::rasterActive() const {
    const GpuRaster* r = impl_->raster();
    return r != nullptr && r->rasterActive();
}

const char* SdlWindow::rasterName() const { return impl_->raster_name.c_str(); }

bool SdlWindow::rasterSpritesEnabled() const {
    // `--raster-sprites 0` 是 A/B 开关：即使后端本身支持 sprite，也必须能退回 CPU 路径。
    const GpuRaster* r = impl_->raster();
    return impl_->raster_sprites && r != nullptr && r->rasterSpritesEnabled();
}

bool SdlWindow::rasterPrepare(const SurfaceFormat& format, int width, int height, std::string* err,
                              const std::uint8_t* color_base) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterPrepare(format, width, height, err, color_base) : false;
}
bool SdlWindow::rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                    std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterSyncFromGuest(pixels, pitch, width, height, err) : false;
}
bool SdlWindow::rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                                  std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterSyncToGuest(pixels, pitch, width, height, err) : false;
}
bool SdlWindow::rasterDraw(const GpuRasterVertex* vertices, std::size_t count, const GpuRasterState& state,
                           std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterDraw(vertices, count, state, err) : false;
}
bool SdlWindow::rasterDrawTextured(const GpuRasterVertex* vertices, std::size_t count,
                                   const GpuRasterTexture& texture, const GpuRasterState& state,
                                   std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterDrawTextured(vertices, count, texture, state, err) : false;
}
bool SdlWindow::rasterQueue(const GpuRasterVertex* vertices, std::size_t count,
                            const GpuRasterTexture* texture, const GpuRasterState& state, std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterQueue(vertices, count, texture, state, err) : false;
}
bool SdlWindow::rasterFlush(std::string* err) {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterFlush(err) : true;
}
const void* SdlWindow::rasterTargetBase() const {
    const GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterTargetBase() : nullptr;
}
std::size_t SdlWindow::rasterQueuedDraws() const {
    const GpuRaster* r = impl_->raster();
    return r != nullptr ? r->rasterQueuedDraws() : 0;
}
void SdlWindow::rasterDiscardTarget() {
    GpuRaster* r = impl_->raster();
    if (r != nullptr) r->rasterDiscardTarget();
}
// ★ 这一条曾经**整条漏掉**（`REQ-01M73P3GXW22Y467YGYPQSM571`）：`d3d9::clear` 在
//   `D3DCLEAR_ZBUFFER` 上就是调它 ⇒ 后端 D32 永不再清，上一帧的 z=0 全屏 quad 把深度写成 0，
//   下一帧 z=0.5 的 FF 批全被 LESSEQUAL 判掉。
void SdlWindow::rasterRequestDepthClear() {
    GpuRaster* r = impl_->raster();
    if (r != nullptr) r->rasterRequestDepthClear();
}
bool SdlWindow::rasterDirty() const {
    const GpuRaster* r = impl_->raster();
    return r != nullptr && r->rasterDirty();
}

GpuDirectPresenter* SdlWindow::directPresenter() {
    GpuRaster* r = impl_->raster();
    return r != nullptr ? r->directPresenter() : nullptr;
}

void SdlWindow::setGpuRasterForTest(GpuRaster* raster) {
    // ⛔ 只许在 `init()` **之前**用：`init()` 之后生产路径手里就是这个壳（`Machine::gpuRaster()`），
    //   换后端会静默改掉全部 raster 行为（像素/屏障/直出）。替身生命周期归调用方，本类不持有。
    // ★ 用**运行时**检查而不是 `assert`：RelWithDebInfo 带 `/DNDEBUG`（实测 `out/build.ninja` 的
    //   FLAGS 里有它）⇒ `assert` 会被编掉，等于没有约束。
    if (impl_->gpu_raster != nullptr) return;
    impl_->raster_view = raster;
}

void SdlWindow::setDirectPresenterForTest(GpuDirectPresenter* direct) {
    // ⛔ 只许在 `init()` 之前用：`init()` 之后生产路径的 `direct` 已经接好，换掉会静默改呈现语义。
    // ★ 运行时检查（RelWithDebInfo 会编掉 assert）：窗口已建 / 已有真直出后端 ⇒ 忽略。
    if (impl_->window != nullptr) return;
    if (impl_->gpu_raster != nullptr) return;
    impl_->direct_view = direct;
}

}  // namespace lift
