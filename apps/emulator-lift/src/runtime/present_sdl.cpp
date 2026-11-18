#include "runtime/present_sdl.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/cursor_map.h"
#include "runtime/font.h"
#include "runtime/frame_blit.h"
#include "runtime/guest_token.h"  // ★ M0：SDL_RenderPresent 的宿主等待要放令牌（见 submitFrame）
#include "runtime/host_cursor.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/log.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 脚本路径）
#include "runtime/warp_echo.h"
#include "runtime/window_scale.h"
#include "ui/hit.h"
#include "ui/present_ui.h"
#include "ui/script.h"

namespace lift {
namespace {

// ★ `--scale auto` 的**前提**：进程必须是 DPI aware。不 aware 时 Windows 会把显示内容缩放
//   虚拟化 ⇒ `SDL_GetDisplayContentScale` 恒报 1.0，`auto` 就静默退化成 1.0（正是"取不到"的
//   形状，但原因被藏起来了）。
//   SDL3 在 `SDL_Init(VIDEO)` 里自己会设 per-monitor v2，但那是**版本行为**；这里在 `SDL_Init`
//   **之前**显式设一次，让 auto 的口径不依赖 SDL 的实现细节（换 SDL 版本不改变这件事）。
//   · 已由 manifest / 更早的调用设过 ⇒ `SetProcessDpiAwarenessContext` 返回 FALSE +
//     ERROR_ACCESS_DENIED —— 那**不是**失败（awareness 已经是对的），⛔ 不打日志。
//   · 其它失败只记一条 info（⛔ 不拦启动：窗口照建；auto 的 readout 会把结果的数打出来）。
void ensureProcessDpiAware() {
#if defined(_WIN32) && defined(WINVER) && WINVER >= 0x0605
    if (::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    const DWORD e = ::GetLastError();
    if (e != ERROR_ACCESS_DENIED) {
        lift::logAt(lift::LogLevel::Info, lift::LogVideo, "[present] ",
                    "SetProcessDpiAwarenessContext(per-monitor-v2) 失败（error=%lu）⇒ 沿用当前 awareness；"
                    "--scale auto 可能因此报 1.0",
                    static_cast<unsigned long>(e));
    }
#endif
}

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

// ---- 调试叠层（`--overlay` / `--ui`）----
// ★ fps 的滚动窗口 = 最近 1 s；`kOverlayFpsWindowNs` 是这条口径的唯一定义。
constexpr std::uint64_t kOverlayFpsWindowNs = 1000ull * 1000ull * 1000ull;
// ★ 脚本每帧的**墙钟预算**（HUD 是诊断层：它超时 ⇒ 关掉叠层，⛔ 不拖慢 guest）。
//   `kUiScriptBudgetNs` = 每次 `render` 能用的墙钟上限；`kUiScriptOpLimit` = 字节码条数上限
//   （两个都是**上限**：正常 HUD 远小于它们）。
constexpr std::uint64_t kUiScriptBudgetNs = 20ull * 1000ull * 1000ull;  // 20 ms
constexpr std::uint32_t kUiScriptOpLimit = 1000000u;

// steady_clock 的绝对纳秒（`UiScript::setBudget` 的 deadline 口径；0 不用 —— 0 表示"不限"）。
std::uint64_t steadyNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

struct SdlWindow::Impl {
    // ★ 指回拥有者：`Impl` 的路径（`submitDirect`）要调 `SdlWindow::drawOverlay`（M0 唯一 HUD 入口）。
    //   对象生命周期 = 拥有者（`impl_` 是 SdlWindow 的成员）⇒ 非拥有指针恒有效。
    SdlWindow* owner = nullptr;
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
    // ★ `--scale` 的落点与一次性 readout 的数据（`runtime/window_scale.h`）：`init()` 里定稿，
    //   此后只读（`scaleReport()`）。缺省 = Fixed 1.0 ⇒ 1280×720（逐字节不变）。
    ScaleReport scale_report;
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
    // GPU 光栅化镜像（与窗口 renderer 共用）
    bool raster_gpu = false;
    std::unique_ptr<GpuRaster> gpu_raster;
    // ★ 夹具缝（`setGpuRasterForTest`）：非空时所有**转发**都到它，生命周期归夹具。
    //   ⛔ 生产路径永远是 nullptr；`init`/`shutdown` 只碰 `gpu_raster`（所有权那一份）。
    GpuRaster* raster_view = nullptr;
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
    // ★ 控制层 resume 的**陈旧事件闸门**（S4）：恢复时刻（`SDL_GetTicksNS`）；0 = 没启用。
    //   判据 / 纯映射在 `runtime/present.h` 的 `classifyStaleEvent`（SDL-free，pure CTest 也测它）。
    std::uint64_t stale_input_before_ns = 0;
    // ★ 本次 stop 的**原因**（控制层 `bye.reason` 用）：只有"暂停期间的关窗 / ESC"这一条给
    //   `"window-closed"`，其余停机原因保持 nullptr（⛔ 不冒充控制层口径）。
    const char* control_stop_reason = nullptr;

    // ---- 调试叠层（`--overlay` / `--ui`，默认关）----
    // ★ HUD 是**脚本**产出的 display list（`scripts/ui/hud.js`，QuickJS 同步跑）；两条呈现路径共用
    //   `PresentSink::presentOverlay` 一个合成入口；文本由**宿主字体**（`ui_font`）按最终像素
    //   栅格化（支持中文）。⛔ 不再有"renderer 一套 / 直出一套"的绘制代码，⛔ 也没有
    //   C++ 侧的文本格式化 / 100 ms 节流 / 文本缓存。
    bool overlay = false;
    SDL_Texture* overlay_tex = nullptr;  // renderer 路径的流式纹理（`presentOverlay` 建 / 用）
    // ★ `overlay_tex_w/h` 是**已用**尺寸（每帧 = 本帧叠层），`overlay_tex_cap_w/h` 是纹理**容量**
    //   （grow-only）。面板宽度随 `fps/worst/frame` 文本变化 ⇒ 高帧率下几乎每帧尺寸都变；按已用
    //   尺寸重建纹理会把上一帧可能仍在 GPU 队列里的 draw 的源纹理提前销毁 ⇒ 叠层部分/整块丢失。
    //   ⇒ 只有 `w > cap_w || h > cap_h` 才新建（容量至少 1024×512，典型 HUD 极少扩容）。
    int overlay_tex_w = 0;
    int overlay_tex_h = 0;
    int overlay_tex_cap_w = 0;
    int overlay_tex_cap_h = 0;
    // ★ 扩容 / 关叠层 / `disableOverlay` 退役下来的旧纹理：**不立即销毁**（上一帧的 draw 可能
    //   仍在 GPU 队列里）。增长次数有界 ⇒ 只留到 `shutdown()`（renderer 销毁前）统一
    //   `SDL_DestroyTexture`。⛔ 不许在 `presentOverlay` / `setOverlay(false)` 里直接销毁。
    std::vector<SDL_Texture*> retired_overlay_tex;
    // 宿主字体：**只在 `--overlay` 时构造**（默认档零成本）；字体不可用 ⇒ init 硬失败（R5）。
    std::unique_ptr<lift::FontEngine> ui_font;
    // 脚本内核 + 这一份脚本（`--ui <path>` 或随发行的 `scripts/ui/hud.js`）。空 = 没装 / 已关。
    std::unique_ptr<lift::ui::ScriptRuntime> ui_runtime;
    std::unique_ptr<lift::ui::UiScript> ui_script;
    // `--ui <path>` 的原样路径；空 = 默认 HUD（走 `LIFT_UI_SCRIPT_DIR/hud.js` 开发回退，
    // 发行布局那一跳由 `aot_main` 先解析好塞进来）。origin = 这一个路径串。
    std::string ui_script_path;
    // 最近 1 s 的 Present 时刻（单调 ns；只用于 fps 那一项；叠层关闭时一次也不记）
    std::vector<std::uint64_t> present_times_ns;
    // ★ `Machine` 推来的只读读数（`script` 身份 / `worst` 3 s 最大值）；脚本每帧读一次。
    UiReadings ui_readings;
    // ★ 帧列表读数源（`game.local_frames()`；非 owning，生命周期 = 调用方 = Machine / AotHost）。
    //   `nullptr` = 没接 ⇒ 脚本侧 `undefined`。⛔ 不在这里推表：脚本**按需**查询。
    lift::ui::UiFrameSource* ui_frame_source = nullptr;
    // ★ 命中 / 捕获路由（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半；`ui/hit.h` 的纯状态机）。
    //   · 命中表 = **上一帧成功 present** 的 display list 的 resolved rect（绘制序）；
    //   · 左键 down 命中 ⇒ 吞 + capture；捕获中的 up 无条件吞，抬在同一条 rect 内才产一次性 click；
    //   · ⛔ handleEvent 只调这里记 pending：不跑 JS、不开 profile、不写剪贴板。
    lift::ui::ClickRouter click_router;
    // ★ 宿主真光标（`--cursor-warp`，默认关）：只加宿主副作用，⛔ 不改 guest 可见状态。
    bool cursor_warp = false;
    // ★ `--cursor-warp` 的回灌抑制队列（**窗口客户区像素**的 FIFO；判据与为什么见
    //   `runtime/warp_echo.h`）。登记点 = `hostCursorWarp` 成功之后；消费点 = `handleEvent` 的
    //   `SDL_EVENT_MOUSE_MOTION` **最先**（命中 ⇒ 整条丢弃，连 `WM_MOUSEMOVE` 都不发）。
    //   ⛔ 它**只**吞 motion：按键 / 滚轮 / 键盘一律放行，并 `clear()`（真人动作 ⇒ 旧期望作废）。
    WarpEchoQueue warp_echo;
    // 一次性读回自检（第一次**成功** warp 之后查一次；此后不再查）。
    bool warp_selfcheck_done = false;

    // ★ 最近一次 Present 的**帧尺寸**（letterbox 的 image 尺寸；⛔ 不是常数 1280×720）。
    //   renderer 路径写在 `tex_w/tex_h`，GPU 直出路径写在 `direct_w/direct_h`；还没有帧 ⇒
    //   `WindowManager::kScreenW/H`（与 guest 虚拟屏同尺寸，是与"未呈现"最不坏的一致假设）。
    int frameImageW() const { return tex_w > 0 ? tex_w : (direct_w > 0 ? direct_w : WindowManager::kScreenW); }
    int frameImageH() const { return tex_h > 0 ? tex_h : (direct_h > 0 ? direct_h : WindowManager::kScreenH); }

    void notePresentTime();
    bool overlayMousePos(int* x, int* y) const;
    // ★ 装脚本：`ui_script_path` 非空 = `--ui <path>`；空 = 默认 HUD 的开发回退
    //   （`LIFT_UI_SCRIPT_DIR/hud.js`）。★ 幂等：已有 `ui_script` 时直接 true。
    //   false ⇒ `err` 必须填（字体 / 读文件 / 脚本编译 / 入口缺失）。
    bool loadUiScript(std::string* err);
    // 叠层画不出来 ⇒ 只告警 + 关掉叠层，呈现照旧（⛔ 不因为诊断层让整帧失败）。
    void disableOverlay(const char* why);
    // ★ 统一释放退役叠层纹理（**只在** `shutdown()`、`SDL_DestroyRenderer` 之前调用）：
    //   扩容 / 关叠层留下的旧纹理会到这里一次性销毁（增长次数有界，内存可接受）。
    void releaseRetiredOverlayTextures();

    void postMessage(std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam);
    // ★ `why` 是"这次为什么收 run"的**唯一指纹**（info 档一行）：`sdl-quit` / `grace-expired` /
    //   `close-deadline` / `no-guest-window`。⛔ 默认档（warn）不打印，别把普通游玩弄吵。
    void requestStop(const char* why);
    // ★ 陈旧事件闸门（S4）：分类 + 单条处理（`StaleAction` 直接复用纯映射的类型）。
    using StaleAction = lift::StaleEventAction;
    StaleAction classifySdlEvent(const SDL_Event& event) const;
    bool pumpOneEvent(const SDL_Event& event);
    void requestClose();
    void advanceCloseFrame();
    void handleEvent(const SDL_Event& event);
    void pumpEvents(int wait_ms);
    void mapMouse(float mx, float my, std::int32_t* gx, std::int32_t* gy) const;
    // ★ 指针的 SDL **窗口点** → **窗口像素**（命中表 = `presentGeometry` 的像素空间）。
    //   ⛔ 不复用 `mapMouse`：它映射到 **guest 客户区**（画面内 0..image-1），那是另一个空间。
    void pointerWindowPixels(float px, float py, int* x, int* y) const;
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

void SdlWindow::Impl::requestStop(const char* why) {
    if (stop) return;
    // ★ 诊断（info 档）：这条是"宿主为什么收口"的唯一指纹。判读表见 .tmp/close-path/wm-close.md §5.4。
    lift::logAt(lift::LogLevel::Info, lift::LogGeneral, "[present] ", "stop: %s", why ? why : "(unknown)");
    stop = true;
    // ★ 控制层口径：只有"暂停期间的关窗 / ESC"这条（`control-paused-close`）映射成
    //   `bye.reason:"window-closed"`；其它宿主停机原因不归控制层（由 run 结果映射）。
    if (why != nullptr && std::strcmp(why, "control-paused-close") == 0) {
        control_stop_reason = "window-closed";
    }
    // 唤醒可能正阻塞在 GetMessageA 里的 guest：WM_QUIT 是队列尾的虚拟项。
    if (windows != nullptr) windows->postQuit(0);
}

void SdlWindow::Impl::requestClose() {
    if (stop || close_pending) return;
    if (windows == nullptr || windows->primaryWindow() == 0) {
        // 还没有 guest 窗口（或没有消息泵）⇒ 没有可确认的对象，直接收口。
        requestStop("no-guest-window");
        return;
    }
    close_pending = true;
    close_frames_left = kCloseGraceFrames;
    close_deadline_ns = SDL_GetTicksNS() + kCloseGraceNs;
    // ★ 诊断（info 档）：投递是与"guest 真的派发了"配对的那一半（另一半见 user32_window.cpp
    //   的 `[msg] dispatch …`）。判读表见 .tmp/close-path/wm-close.md §5.4。
    lift::logAt(lift::LogLevel::Info, lift::LogGeneral, "[present] ",
                "post WM_CLOSE hwnd=%08X（宽限 %d 帧 / %llu ms）", windows->primaryWindow(),
                kCloseGraceFrames, static_cast<unsigned long long>(kCloseGraceNs / 1000000ull));
    postMessage(kWmClose, 0, 0);
}

void SdlWindow::Impl::advanceCloseFrame() {
    if (!close_pending || stop) return;
    if (close_frames_left > 0) --close_frames_left;
    if (close_frames_left <= 0 || SDL_GetTicksNS() >= close_deadline_ns) requestStop("grace-expired");
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
//   注入（`InputScript`；CLI 已退役）用的是**同一格**（guest 客户区坐标）。没有主窗口 ⇒ 整行省略。
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

// ★ 装脚本（B 的唯一加载点）。
//   · `ui_script_path` 非空 = `--ui <path>`（**显式优先**；origin = 原样路径，错误里因此点名
//     文件与行号）；
//   · 空 = `--overlay` 的默认 HUD：先试**开发回退** `LIFT_UI_SCRIPT_DIR/hud.js`（CMake 注入的
//     源码树路径）。★ 发行布局（`<exe 目录>/scripts/ui/hud.js`）由 `aot_main` **先**解析好再传进
//     `ui_script_path`（sink 不知道 argv[0]）。
//   · 两条都不在 ⇒ **硬失败**，err 里点名试过的两个路径（⛔ 不静默回退空 HUD）。
//   ★ 字体是**先决条件**（脚本的 `ui.measure` 走宿主字体）：没有字体 ⇒ 这里就失败（R5）。
//   ★ 幂等：已经装好 ⇒ 直接 true（`setOverlay(true)` 重复调用不会重建 context）。
bool SdlWindow::Impl::loadUiScript(std::string* err) {
    if (ui_script != nullptr) return true;
    // ① 宿主字体（`ui.measure` 的唯一来源）。缺失 ⇒ 硬失败（⛔ 不静默画空白）。
    if (ui_font == nullptr) {
        auto font = std::make_unique<lift::FontEngine>();
        lift::HostTextMetrics metrics;
        if (!font->metrics(lift::ui::kUiFontRefPx, false, &metrics)) {
            if (err) {
                *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ --overlay / --ui 拒绝启动"
                       "（R5：⛔ 不静默画空白）";
            }
            return false;
        }
        ui_font = std::move(font);
    }
    // ② 路径：显式 `--ui` 优先；否则开发回退（`LIFT_UI_SCRIPT_DIR`，编译期注入）。
    std::string path = ui_script_path;
    if (path.empty()) {
        path = std::string(LIFT_UI_SCRIPT_DIR) + "/hud.js";
        // ★ 存在性 = 能不能打开（同一份 UTF-8 路径口径；⛔ 不用 std::filesystem 的第二套编码规则）。
        std::FILE* probe = lift::openHostFileUtf8(path, "rb");
        if (probe == nullptr) {
            if (err) {
                *err = std::string("HUD 脚本不在：试过 <exe 所在目录>/scripts/ui/hud.js（发行布局，") +
                       "aot 臂会先找它）与 " + path +
                       "（开发回退 LIFT_UI_SCRIPT_DIR）；用 --ui <path> 指定脚本";
            }
            return false;
        }
        std::fclose(probe);
    }
    // ③ 读源码（UTF-8 路径走宿主宽字符 API ⇒ Windows 上的非 ASCII 路径也对）。
    std::string source;
    {
        std::FILE* f = lift::openHostFileUtf8(path, "rb");
        if (f == nullptr) {
            if (err) *err = "--ui 读不到脚本：" + path;
            return false;
        }
        char buf[8192];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
        const bool read_bad = std::ferror(f) != 0;
        std::fclose(f);
        if (read_bad) {
            if (err) *err = "--ui 读脚本失败：" + path;
            return false;
        }
    }
    // ④ 脚本内核（建一次；换脚本时复用同一个 runtime）+ 编译 + 抓入口。
    if (ui_runtime == nullptr) ui_runtime = lift::ui::makeQuickJsRuntime();
    if (ui_runtime == nullptr) {
        if (err) *err = "脚本内核不可用（QuickJS runtime 创建失败）";
        return false;
    }
    std::string load_err;
    ui_script = ui_runtime->load(source, path, ui_font.get(), &load_err);
    if (ui_script == nullptr) {
        if (err) *err = load_err.empty() ? (path + ": 脚本加载失败") : load_err;
        return false;
    }
    return true;
}

void SdlWindow::Impl::disableOverlay(const char* why) {
    std::fprintf(stderr, "[overlay] 叠层不可用 ⇒ 关掉（呈现照旧）：%s\n", why != nullptr ? why : "(unknown)");
    overlay = false;
    // ★ 屏幕上已经没有这层叠层了 ⇒ 命中表 / capture 一起作废（⛔ 留着旧按钮继续吃点击）。
    click_router.clearFrame();
    ui_script.reset();
    // ★ 不立即销毁：上一帧的 draw 可能仍在 GPU 队列里（提前释放 ⇒ 部分/整块叠层丢失）。
    //   退役到 `retired_overlay_tex`，由 `shutdown()` 统一释放。
    if (overlay_tex != nullptr) {
        retired_overlay_tex.push_back(overlay_tex);
        overlay_tex = nullptr;
    }
    overlay_tex_w = 0;
    overlay_tex_h = 0;
    overlay_tex_cap_w = 0;
    overlay_tex_cap_h = 0;
    if (direct != nullptr) {
        std::string ignored;
        direct->directOverlay(nullptr, 0, 0, 0, 0, &ignored);
    }
}

void SdlWindow::Impl::releaseRetiredOverlayTextures() {
    for (SDL_Texture*& tex : retired_overlay_tex) {
        if (tex != nullptr) SDL_DestroyTexture(tex);
        tex = nullptr;
    }
    retired_overlay_tex.clear();
}

// ★ B 的唯一 HUD 绘制入口：把这一帧的读数打包成 `UiFrameReadings` → 脚本产出 display list
//   → 宿主字体按最终像素栅格化 → `PresentSink::presentOverlay`（两条呈现路径由 sink 内部选）。
//   ★ **每帧都跑脚本**（⛔ 没有 100 ms 节流 / 文本缓存）：`frame` 本来每帧都变，脚本就是真源。
//   ★ 失败（脚本抛错 / 超预算 / 栅格化失败 / 合成失败）⇒ false + err，由调用方 `disableOverlay`
//     一次并照旧呈现（⛔ 不让诊断层把整帧打死）。
bool SdlWindow::drawOverlay(std::string* err) {
    if (!impl_->overlay) return true;
    if (impl_->ui_script == nullptr || impl_->ui_font == nullptr) {
        if (err) *err = "HUD 脚本未初始化（--overlay / --ui 需要 UiScript + FontEngine）";
        return false;
    }
    lift::ui::UiFrameReadings readings;
    readings.frame = impl_->frame_no;
    int mx = 0;
    int my = 0;
    if (impl_->overlayMousePos(&mx, &my)) {
        readings.mouse_ok = true;
        readings.mouse_x = mx;
        readings.mouse_y = my;
    }
    if (!impl_->present_times_ns.empty()) {
        readings.fps_ok = true;
        readings.fps = static_cast<double>(impl_->present_times_ns.size());  // 最近 1 s 的 Present 次数
    }
    const UiReadings& r = impl_->ui_readings;
    readings.script_ok = r.has_script;
    readings.script = r.script;
    readings.worst_ok = r.has_worst;
    readings.worst_wall_ms = r.wall_max_ms;
    readings.worst_guest_ms = r.guest_max_ms;
    readings.worst_guest_ok = r.has_guest;  // 块计时没开 ⇒ 脚本侧**没有** guest_ms 这个键
    readings.worst_shim_ms = r.shim_max_ms;
    // ★ 帧列表读数源（`game.local_frames()`）：**按需查询** —— 这里只把指针交出去，
    //   本帧脚本不调用它就一次 guest 读都不发生（⛔ 宿主不每帧重建表）。
    readings.frames = impl_->ui_frame_source;
    // ★ 命中 / 捕获（切片 2 后半）：全部读自**上一帧成功 present** 的命中表（= 屏幕上真正画着
    //   的东西）+ 本帧指针位置。⛔ 这里只读：不跑 JS、不改 capture。
    {
        std::string hover;
        // 指针在**本窗口**上时才算 hover（没有鼠标焦点 ⇒ 空；⛔ 不拿别的窗口的坐标猜）。
        if (impl_->window != nullptr && SDL_GetMouseFocus() == impl_->window) {
            float mouse_x = 0.0f;
            float mouse_y = 0.0f;
            SDL_GetMouseState(&mouse_x, &mouse_y);
            int hit_x = 0;
            int hit_y = 0;
            impl_->pointerWindowPixels(mouse_x, mouse_y, &hit_x, &hit_y);
            hover = impl_->click_router.hoverKey(hit_x, hit_y);
        }
        readings.hover_key = std::move(hover);
        readings.active_key = impl_->click_router.activeKey();
        // ★ click **一次性**：这一次 render 之前取走 ⇒ 下一次 render 就是 undefined
        //   （需求单「设计」第 3 条；判据在 `ui-click-router` 与 `ui-script-quickjs/key-on-rect`）。
        lift::ui::UiClick click;
        if (impl_->click_router.takeClick(&click)) {
            readings.click_ok = true;
            readings.click_key = std::move(click.key);
            readings.click_x = click.x;
            readings.click_y = click.y;
        }
    }
    // ★ 预算 = 这一帧的墙钟上限 + 字节码条数上限（`UiScript::setBudget` 的绝对 ns 口径）。
    impl_->ui_script->setBudget(steadyNowNs() + kUiScriptBudgetNs, kUiScriptOpLimit);
    lift::ui::UiPresentedFrame presented;
    const bool ok =
        lift::ui::presentUi(this, *impl_->ui_font, *impl_->ui_script, readings, &presented, err);
    // ★ 只有**成功 present** 的那一帧才更新命中表：没有几何 / 空 list / 合成失败 ⇒ 屏幕上仍是
    //   上一次的叠层，命中表必须跟着它（⛔ 不清空：那会让"画着按钮但点不中"）。
    if (ok && presented.presented) impl_->click_router.commitFrame(std::move(presented.hits));
    return ok;
}

void SdlWindow::Impl::pointerWindowPixels(float px, float py, int* x, int* y) const {
    *x = static_cast<int>(std::lround(px));
    *y = static_cast<int>(std::lround(py));
    if (window == nullptr) return;
    // ★ SDL 的指针事件坐标是**窗口点**（逻辑坐标），而 `presentGeometry` 给的画面矩形是
    //   **窗口像素**（`SDL_GetWindowSizeInPixels`）⇒ 按两者比值换算（DPR != 1 时必须）。
    //   取不到任一尺寸 ⇒ 原样返回（⛔ 不猜一个 DPI 比例）。
    int pixel_w = 0;
    int pixel_h = 0;
    int logical_w = 0;
    int logical_h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h)) return;
    if (!SDL_GetWindowSize(window, &logical_w, &logical_h)) return;
    if (pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0) return;
    const double sx = static_cast<double>(pixel_w) / static_cast<double>(logical_w);
    const double sy = static_cast<double>(pixel_h) / static_cast<double>(logical_h);
    *x = static_cast<int>(std::lround(static_cast<double>(px) * sx));
    *y = static_cast<int>(std::lround(static_cast<double>(py) * sy));
}

void SdlWindow::Impl::mapMouse(float mx, float my, std::int32_t* gx, std::int32_t* gy) const {
    int ww = 1280;
    int wh = 720;
    if (window != nullptr) SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0) ww = 1280;
    if (wh <= 0) wh = 720;
    // ★ 公式的**唯一一份**在 `runtime/cursor_map.h`（与宿主光标 warp 共用）：
    //   窗口客户区像素 → 画面像素（letterbox 反算；0..image-1 clamp 是既有口径）。
    //   image 尺寸取**最近一次 Present 的帧尺寸**（⛔ 不再是硬编码 1280×720 —— 后缓冲可以是
    //   15 种适配器模式之一，硬编码会让非 1280×720 的鼠标映射系统性偏移）。
    //   `mapWindowPixelToImage` 同时构造 letterbox 与反算 ⇒ 调用点只有这一处，不会再"构造用帧尺寸、
    //   反算用常数"地静默漂移（守卫：`tests/cursor_warp.cpp` 的 `cursor-map/window-pixel-to-image`）。
    int x = 0;
    int y = 0;
    if (!mapWindowPixelToImage(ww, wh, frameImageW(), frameImageH(), mx, my, &x, &y)) {
        // 几何不可用（窗口 / 帧尺寸非正）：保持旧行为（原样截断），⛔ 不返回野值。
        *gx = static_cast<std::int32_t>(mx);
        *gy = static_cast<std::int32_t>(my);
        return;
    }
    *gx = x;
    *gy = y;
}

void SdlWindow::Impl::handleEvent(const SDL_Event& event) {
    const std::uint32_t now_ms = static_cast<std::uint32_t>(SDL_GetTicks());
    switch (event.type) {
        case SDL_EVENT_QUIT: {
            // ★ SDL 3.4.18 在**最后一个窗口**收到关闭请求时会无条件再推一条 QUIT（hint 缺省 "1"）。
            //   那条不是"用户要退出应用" —— 已有关窗请求在途时必须给它让路，由 guest 的 WndProc
            //   决定要不要退（否则 guest 一条指令都跑不到，确认框永远建不出来）。
            //   纯映射见 `runtime/present.h` 的 `hostQuitAction`（判据在 tests/messagebox.cpp）。
            if (hostQuitAction(HostQuitSource::AppQuit, close_pending) == HostQuitAction::StopRun) {
                click_router.focusLost();  // 退出 ⇒ capture 作废（⛔ 不跨窗口会话留着）
                requestStop("sdl-quit");
            }
            break;
        }
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (window != nullptr && event.window.windowID == SDL_GetWindowID(window) &&
                hostQuitAction(HostQuitSource::WindowCloseRequested, close_pending) ==
                    HostQuitAction::PostWmClose) {
                // ★ 关窗 = 交互结束 ⇒ 清 capture（判据：需求单「设计」第 4 条）。
                click_router.focusLost();
                requestClose();
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            // ★ 焦点丢失只清 **OS 拥有** 的键 / 按钮（见 host_window.h 的 InputSource）：
            //   脚本注入的按下不因窗口失焦而丢 —— 那正是"注入不依赖焦点"的一半。
            if (windows != nullptr) windows->clearOsInput();
            // ★ 叠层的 capture 也清（失焦 ⇒ 这次按住不算数；抬起时放行给 guest，不发 click）。
            click_router.focusLost();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            // ⛔ 这里**不清** `warp_echo`：一条在 warp **之前**入队、warp 之后才排空的键事件，
            //   会抹掉刚登记的期望 ⇒ 它那条回灌随即被当成真人移动接受（反写虚拟光标）。
            //   旧期望的老化只靠"每条只消费一次 + 容量 8"，理由见 `runtime/warp_echo.h`。
            const bool down = event.type == SDL_EVENT_KEY_DOWN;
            const std::uint32_t vk = vkFromSdlKey(event.key.key);
            if (vk == 0) break;
            const std::uint32_t scan = event.key.scancode ? static_cast<std::uint32_t>(event.key.scancode) : 0u;
            // ★ 注入路径与真人路径在**同一段代码**里：状态 + WM_* 一起落地（所有权判定在里面）。
            if (windows != nullptr) input::key(*windows, vk, down, scan, InputSource::Os, now_ms);
            if (down && vk == 0x1B) requestClose();  // ESC = 用户要退出（与旧行为一致）
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            // ★ `--cursor-warp` 的回灌抑制**在这里、且最先**：warp 之后 OS 会把同一次移动回灌成
            //   一条 motion（窗口客户区像素）。命中 `warp_echo` 的一条登记 ⇒ 整条丢弃：⛔ 不调
            //   `mapMouse`（它会 clamp 越界值）、⛔ 不调 `input::mouseMove`（连 `WM_MOUSEMOVE`
            //   都不发）⇒ guest 可见状态逐位不变。判据与在窗口像素空间匹配的理由见
            //   `runtime/warp_echo.h`。
            if (window != nullptr && event.motion.windowID == SDL_GetWindowID(window) &&
                warp_echo.consume(event.motion.x, event.motion.y)) {
                break;
            }
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
            // ⛔ 同上：按键**不**清期望（清掉会让"按键先入队、warp 后排空"的那条回灌漏进 guest）。
            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            MouseButton b = MouseButton::Left;
            if (!mouseButtonFromSdl(event.button.button, &b)) break;
            // ★ **叠层命中 / 捕获**（切片 2 后半）：坐标用**窗口像素**（命中表同一空间），
            //   ⛔ 不是下面 `mapMouse` 的 guest 客户区坐标。⛔ 这里只把 pending 记进 router，
            //   不跑 JS、不开 profile、不写剪贴板（裁决 4）。
            if (overlay && window != nullptr && event.button.windowID == SDL_GetWindowID(window)) {
                int hit_x = 0;
                int hit_y = 0;
                pointerWindowPixels(event.button.x, event.button.y, &hit_x, &hit_y);
                lift::ui::UiPointerButton hit_button = lift::ui::UiPointerButton::Left;
                if (b == MouseButton::Right) {
                    hit_button = lift::ui::UiPointerButton::Right;
                } else if (b == MouseButton::Middle) {
                    hit_button = lift::ui::UiPointerButton::Middle;
                }
                const lift::ui::UiPointerDecision decision = click_router.handlePointer(
                    down ? lift::ui::UiPointerPhase::Down : lift::ui::UiPointerPhase::Up, hit_button, hit_x,
                    hit_y);
                // ★ 吞掉 = 这条事件**不进 guest**（判定全在纯状态机里；见 `ui/hit.h`）：
                //   左键 down 命中 / 捕获中的左键 up。
                if (decision.swallow) break;
            }
            std::int32_t x = 0;
            std::int32_t y = 0;
            mapMouse(event.button.x, event.button.y, &x, &y);
            if (windows != nullptr) input::mouseButton(*windows, x, y, b, down, InputSource::Os, now_ms);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            // ⛔ 同上：滚轮**不**清期望。
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

// ★ 控制层 resume 的陈旧事件闸门（S4）：把 SDL 事件映射成**纯**分类（判据在 present.h）。
//   ⛔ 不在这里写阈值比较：比较只有 `classifyStaleEvent` 一份（pure CTest 测它，SDL 这里只喂事实）。
SdlWindow::Impl::StaleAction SdlWindow::Impl::classifySdlEvent(const SDL_Event& event) const {
    HostEventClass cls = HostEventClass::Other;
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            cls = HostEventClass::OsInput;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            cls = HostEventClass::CloseRequest;
            break;
        case SDL_EVENT_QUIT:
            cls = HostEventClass::AppQuit;
            break;
        default:
            cls = HostEventClass::Other;
            break;
    }
    // ESC 是"用户要退出"的第二种形状（与关窗同一条意图）⇒ 单独一类。
    if (event.type == SDL_EVENT_KEY_DOWN && vkFromSdlKey(event.key.key) == 0x1B) {
        cls = HostEventClass::EscapeKey;
    }
    return classifyStaleEvent(cls, event.common.timestamp, stale_input_before_ns);
}

// 处理一条事件；返回 false = 已收口（调用方停止排空队列）。
bool SdlWindow::Impl::pumpOneEvent(const SDL_Event& event) {
    // ★ **闸门在 handleEvent 之前**：陈旧 OS 输入一次都不许进 `input::*`（它会写 guest 状态并
    //   post 无上界队列）；陈旧关窗 / ESC 当宿主 stop（⛔ 不投 `WM_CLOSE` 进 guest）。
    switch (classifySdlEvent(event)) {
        case StaleAction::Drop:
            return true;  // 丢弃：暂停期间的 OS 输入不是"刚发生"
        case StaleAction::HostStop:
            requestStop("control-paused-close");
            return false;
        case StaleAction::Keep:
            break;
    }
    handleEvent(event);
    return !stop;
}

void SdlWindow::Impl::pumpEvents(int wait_ms) {
    if (stop) return;
    SDL_Event event;
    if (wait_ms > 0) {
        if (SDL_WaitEventTimeout(&event, wait_ms)) {
            if (!pumpOneEvent(event)) return;
            while (SDL_PollEvent(&event)) {
                if (!pumpOneEvent(event)) return;
            }
        }
    } else {
        while (SDL_PollEvent(&event)) {
            if (!pumpOneEvent(event)) return;
        }
    }
    if (close_pending && !stop && SDL_GetTicksNS() >= close_deadline_ns) requestStop("close-deadline");
}

SdlWindow::SdlWindow() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
SdlWindow::~SdlWindow() { shutdown(); }

bool SdlWindow::init(const SdlWindowOptions& options, std::string* err) {
    if (impl_->window != nullptr) return true;
    impl_->screenshot_path = options.screenshot_path;
    impl_->screenshot_frame = options.screenshot_frame;
    impl_->raster_gpu = options.raster_gpu;
    impl_->overlay = options.overlay;
    impl_->ui_script_path = options.ui_script;
    // ★ B：`--overlay` / `--ui` 的叠层由**脚本**产出 display list（`scripts/ui/hud.js` 或 `--ui`）。
    //   装脚本 = ① 宿主字体（`ui.measure` 的来源，缺失 ⇒ 硬失败 R5）② 源码（默认 / `--ui` 文件）
    //   ③ QuickJS 编译 + 抓 `globalThis.render`。任何一步失败 ⇒ **启动硬错**（AOT 侧 exit 2；
    //   ⛔ 不静默画空白、⛔ 不退回 5×7 点阵）。
    //   ★ 只在 overlay 打开时构造 FontEngine / runtime ⇒ 默认档零成本。
    if (impl_->overlay) {
        std::string script_err;
        if (!impl_->loadUiScript(&script_err)) {
            if (err) *err = script_err;
            impl_->ui_script.reset();
            impl_->ui_runtime.reset();
            impl_->ui_font.reset();
            impl_->overlay = false;
            return false;
        }
    }
    // ★ `--cursor-warp`：开关归 sink（实际生效还要看窗口是否可见，见 `hostCursorWarpEnabled`）。
    impl_->cursor_warp = options.cursor_warp;
    impl_->warp_selfcheck_done = false;

    // ★ `--scale auto` 的 DPI awareness 必须在 SDL 起视频子系统**之前**定（见 `ensureProcessDpiAware`）。
    ensureProcessDpiAware();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        if (err) *err = std::string("SDL_Init(VIDEO) 失败：") + SDL_GetError();
        return false;
    }
    impl_->sdl_inited = true;
    // ★ 关窗语义（本修复的一半）：SDL3 缺省会在**最后一个窗口**收到关闭请求时**再推一条**
    //   `SDL_EVENT_QUIT` —— 那条 QUIT 不是"用户要退出应用"，只是"最后一个窗口在请求关闭"的
    //   同义反复；它会把关窗请求抢跑成"宿主直接收 run"，guest 的 `WM_CLOSE`（与确认框）永远
    //   来不及建立。显式设 "0"：关窗只走 guest 自己的 WndProc。
    //   ★ 宏/字符串名与缺省值都是**在本机 vcpkg 的 SDL 3.4.18 头里核过的**（不是凭记忆）：
    //     `out/vcpkg_installed/x64-windows/include/SDL3/SDL_hints.h:3211`
    //       `#define SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE "SDL_QUIT_ON_LAST_WINDOW_CLOSE"`
    //     同处文档块："- \"1\": SDL will send a quit event when the last window is requesting to
    //     close. (default)"（`\since` SDL 3.2.0）。发源：SDL_windowevents.c 的 `SDL_SendQuit()`。
    if (!SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0")) {
        lift::logAt(lift::LogLevel::Warn, lift::LogVideo, "[present] ",
                    "SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE,\"0\") 失败 ⇒ 按缺省 \"1\" 继续"
                    "（× 会被 SDL_EVENT_QUIT 抢跑）");
    }
    // ★ GPU device 先建（不需要窗口）：后端失败必须在开窗之前就是硬失败。
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

    // ---- `--scale <auto|数值>`：窗口客户区 = round(1280×s) × round(720×s) ----
    // ★ 公式 / auto 判据 / readout 文案的**唯一一份**在 `runtime/window_scale.h`（pure CTest 直接测它）。
    //   这里只做两件事：① 取数（auto ⇒ SDL 的 display content scale）；② 把**建完窗口之后**
    //   回读到的客户区尺寸写进 `scale_report`（⛔ 不把请求值抄回去冒充事实）。
    // ★ 缺省档：`options.scale` 是 Fixed 1.0 ⇒ 下面算出的就是 1280×720 ⇒ `SDL_CreateWindow` 的实参
    //   与改动前逐字节相同（这就是"缺省行为不变"的机械判据）。
    // ★ `auto` 取不到（SDL 返回 0 / NaN / 离谱值）⇒ 回 1.0，但 `auto_fallback` 会被 CLI 打出来
    //   （⛔ 不静默退回缺省）。
    {
        ScaleReport report;
        report.image_w = kBaseWindowW;  // 被呈现的画面尺寸（guest 后缓冲）；窗口尺寸不改变它
        report.image_h = kBaseWindowH;
        if (options.scale.kind == ScaleSpec::Kind::Auto) {
            report.auto_requested = true;
            const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
            // 取不到 display（无桌面 / 无显示）时 `SDL_GetDisplayContentScale` 也拿不到 ⇒ 当 0 处理。
            const double raw = primary != 0 ? static_cast<double>(SDL_GetDisplayContentScale(primary)) : 0.0;
            report.auto_content_scale = raw;
            report.auto_fallback = !autoContentScaleUsable(raw);
            report.scale = autoScaleFromContentScale(raw);
        } else {
            report.scale = options.scale.value;
        }
        const WindowSize size = windowSizeForScale(report.scale);
        report.window_w = size.w;
        report.window_h = size.h;
        impl_->scale_report = report;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
    impl_->window = SDL_CreateWindow(options.title.c_str(), impl_->scale_report.window_w,
                                     impl_->scale_report.window_h, flags);
    if (impl_->window == nullptr) {
        if (err) *err = std::string("SDL_CreateWindow 失败：") + SDL_GetError();
        shutdown();
        return false;
    }
    // ★ readout 的事实 = **建完之后回读**的客户区尺寸（窗口管理器可能约束尺寸；隐藏窗口也一样）。
    {
        int actual_w = 0;
        int actual_h = 0;
        SDL_GetWindowSize(impl_->window, &actual_w, &actual_h);
        if (actual_w > 0 && actual_h > 0) {
            impl_->scale_report.window_w = actual_w;
            impl_->scale_report.window_h = actual_h;
        }
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
                             "  ⇒ 回退 SDL_Renderer 上传呈现路径（光栅化仍是 GPU；"
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
        // ⛔ 不要覆盖 GPU 光栅化后端的名字：它才是真正的执行者
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
    // ★ 叠层纹理属于 renderer ⇒ 必须在 `SDL_DestroyRenderer` **之前**释放。
    //   当前纹理 + 退役纹理（扩容 / 关叠层留下的）**只在这里**销毁 —— 别处一律只退役。
    if (impl_->overlay_tex != nullptr) {
        impl_->retired_overlay_tex.push_back(impl_->overlay_tex);
        impl_->overlay_tex = nullptr;
    }
    impl_->releaseRetiredOverlayTextures();
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
    impl_->stale_input_before_ns = 0;
    impl_->control_stop_reason = nullptr;
    impl_->frame_no = 0;
    impl_->screenshot_path.clear();
    impl_->screenshot_frame = 0;
    impl_->renderer_name.clear();
    impl_->video_driver.clear();
    impl_->gpu_driver.clear();
    // `--scale` 的落点也回缺省（`--present auto` 的重试会再 init 一次，⛔ 不带着上一轮的数）。
    impl_->scale_report = ScaleReport{};
    impl_->capture_argb.clear();
    impl_->capture_w = 0;
    impl_->capture_h = 0;
    // 叠层状态（纹理已在 renderer 之前统一释放；直出那一份随 `directDetachWindow` 移交 retired）。
    impl_->overlay_tex_w = 0;
    impl_->overlay_tex_h = 0;
    impl_->overlay_tex_cap_w = 0;
    impl_->overlay_tex_cap_h = 0;
    // ★ 退役纹理已在上面的 `releaseRetiredOverlayTextures()` 清空；这里再兜一次（措辞见字段注释）。
    impl_->retired_overlay_tex.clear();
    impl_->overlay = false;
    // ★ 脚本 / 内核 / 字体 / 读数一起放掉（下一轮 init 或 `setOverlay(true)` 会按新 options 重装）。
    impl_->ui_script.reset();
    impl_->ui_runtime.reset();
    impl_->ui_font.reset();
    impl_->ui_script_path.clear();
    impl_->present_times_ns.clear();
    impl_->ui_readings = UiReadings{};
    // ★ 命中表 / capture 也不跨 init 会话（旧 key / rect 已经不代表屏幕上的东西）。
    impl_->click_router.clearFrame();
    // 宿主光标开关 / 一次性自检（下一轮 init 会按新 options 重设）。
    impl_->cursor_warp = false;
    impl_->warp_selfcheck_done = false;
    impl_->warp_echo.clear();
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
            // ★ 窗口 = **帧尺寸 × scale**（`window_scale.h` 的 `windowSizeForFrame`：换算是唯一那份）。
            //   `scale == 1.0` ⇒ 得到的就是 (w,h) ⇒ 与改动前逐字节相同；
            //   `--scale` 是倍率、图像尺寸跟 guest ⇒ guest 改分辨率时窗口跟着帧走、倍率不变。
            const WindowSize want = windowSizeForFrame(w, h, impl_->scale_report.scale);
            if (!SDL_SetWindowSize(impl_->window, want.w, want.h)) {
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
    //   ★ B：唯一入口 `drawOverlay`（读数 → 脚本产出 display list → 宿主字体 → `presentOverlay`）。
    if (impl_->overlay) {
        std::string oerr;
        if (!drawOverlay(&oerr)) impl_->disableOverlay(oerr.c_str());
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

// ★ `PresentSink`（M0）：本帧几何 = 窗口像素尺寸 + letterbox 之后的**画面矩形**。
//   UI 层只用它算画布原点（`uiCanvasOrigin(picture_w,picture_h)`）；⛔ 这里不碰叠层。
//   ★ 画面尺寸取**最近一次 Present 的帧尺寸**（与 `mapMouse` / 光标 warp 同一份 `frameImageW/H`）。
//   ★ `picture_*` 用与直出 `layoutLetterbox` **相同的整数除法**（(window−picture)/2）⇒ 同一份几何。
bool SdlWindow::presentGeometry(PresentGeometry* out) const {
    if (out == nullptr || impl_->window == nullptr) return false;
    int ww = 0;
    int wh = 0;
    if (!SDL_GetWindowSizeInPixels(impl_->window, &ww, &wh)) return false;
    if (ww <= 0 || wh <= 0) return false;
    const int iw = impl_->frameImageW();
    const int ih = impl_->frameImageH();
    const LetterboxMap map = letterboxFor(ww, wh, iw, ih);
    if (map.scale <= 0.0) return false;
    const int pw = static_cast<int>(std::lround(static_cast<double>(iw) * map.scale));
    const int ph = static_cast<int>(std::lround(static_cast<double>(ih) * map.scale));
    if (pw <= 0 || ph <= 0) return false;
    out->window_w = ww;
    out->window_h = wh;
    out->picture_x = (ww - pw) / 2;
    out->picture_y = (wh - ph) / 2;
    out->picture_w = pw;
    out->picture_h = ph;
    return true;
}

// ★ `PresentSink`：统一合成入口。输入是 UI 层已经栅格化好的 RGBA8（内存序 R,G,B,A）
//   与**画布原点**（窗口像素，`uiCanvasOrigin` 的结果）。两条路径在这里分派：
//     · GPU 直出：交给后端（`directOverlay`），真正的绘制在 `directPresent` 的 letterbox 之后。
//       ★ D 单起直出走的是一条**混合绘制管线**（`SDL_DrawGPUPrimitives` + SRC_ALPHA /
//         ONE_MINUS_SRC_ALPHA）⇒ 与 renderer 路径一样按 alpha 混合；`directReadback` 用同一整数公式
//         做 CPU 合成。
//     · SDL_Renderer：流式纹理 + **一次** 1:1 draw（B1：这次 draw 前后临时关掉 LETTERBOX，
//       画完立刻恢复 —— 因为叠层是本帧最后一次 draw，风险面最小；⛔ 不做逆缩放）。
//       ★ 这一条走 `SDL_BLENDMODE_BLEND` ⇒ 半透明底**真的**与画面混合（文本 alpha=255 不变淡）。
//   ⛔ 不写 guest、⛔ 不进 `--dump-frame`；`rgba` 只在本次调用内有效（这里自己拷进纹理 / GPU）。
bool SdlWindow::presentOverlay(const std::uint8_t* rgba, int w, int h, int x, int y, std::string* err) {
    if (!impl_->overlay) return true;  // 关掉时一次判空
    if (rgba == nullptr || w <= 0 || h <= 0) return true;
    if (impl_->direct_present && impl_->direct != nullptr) {
        return impl_->direct->directOverlay(rgba, w, h, x, y, err);
    }
    if (impl_->renderer == nullptr) {
        if (err) *err = "SDL 窗口未初始化";
        return false;
    }
    // ★ **grow-only 容量**：面板宽随 fps/worst/frame 文本变化 ⇒ 高帧率下几乎每帧尺寸都变。
    //   只有**超过当前容量**才新建；旧纹理**不销毁**（上一帧的 draw 可能还在 GPU 队列里）⇒
    //   退役到 `retired_overlay_tex`，`shutdown()` 统一释放。`overlay_tex_w/h` 只是**已用**尺寸。
    if (impl_->overlay_tex == nullptr || w > impl_->overlay_tex_cap_w || h > impl_->overlay_tex_cap_h) {
        const int cap_w = std::max(w, 1024);
        const int cap_h = std::max(h, 512);
        // ★ 字节序：图像是**内存序 RGBA8**；SDL 的格式名按 u32 位序定义 ⇒ 内存序 R,G,B,A = `ABGR8888`
        //   （用 `RGBA8888` 会变成 A,B,G,R ⇒ 颜色整体错位：深底 0x101820 被读成 0xFF2018）。
        SDL_Texture* fresh = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_ABGR8888,
                                                SDL_TEXTUREACCESS_STREAMING, cap_w, cap_h);
        if (fresh == nullptr) {
            if (err) *err = std::string("SDL_CreateTexture(overlay) 失败：") + SDL_GetError();
            return false;
        }
        // ★ **按 alpha 混合**（用户 2026-11-18：HUD 背景 80% 半透明）：
        //   脚本的底是 `#101820CC`（alpha 204）⇒ 这里必须 BLEND，否则半透明底会被当成不透明画上去。
        //   文本像素的 alpha 是 255 ⇒ 混合后仍是字色本身（⛔ 不会把字也变淡）。
        if (!SDL_SetTextureBlendMode(fresh, SDL_BLENDMODE_BLEND)) {
            if (err) *err = std::string("SDL_SetTextureBlendMode(overlay, BLEND) 失败：") + SDL_GetError();
            SDL_DestroyTexture(fresh);  // 刚建、一次 draw 都没被引用 ⇒ 可以立即销毁
            return false;
        }
        if (!SDL_SetTextureScaleMode(fresh, SDL_SCALEMODE_NEAREST)) {
            if (err) *err = std::string("SDL_SetTextureScaleMode(overlay, NEAREST) 失败：") + SDL_GetError();
            SDL_DestroyTexture(fresh);
            return false;
        }
        // ★ 旧纹理**退役**（⛔ 不 SDL_DestroyTexture）：本帧之前的 draw 可能仍在队列里。
        if (impl_->overlay_tex != nullptr) impl_->retired_overlay_tex.push_back(impl_->overlay_tex);
        impl_->overlay_tex = fresh;
        impl_->overlay_tex_cap_w = cap_w;
        impl_->overlay_tex_cap_h = cap_h;
    }
    impl_->overlay_tex_w = w;
    impl_->overlay_tex_h = h;
    // ★ **子矩形**上传：纹理容量可能大于本帧叠层（grow-only）⇒ 只写左上 w×h，pitch 按 w。
    const SDL_Rect src_rect{0, 0, w, h};
    if (!SDL_UpdateTexture(impl_->overlay_tex, &src_rect, rgba, w * 4)) {
        if (err) *err = std::string("SDL_UpdateTexture(overlay) 失败：") + SDL_GetError();
        return false;
    }
    // ---- B1：叠层这一 draw 用**窗口像素**（临时关掉 LETTERBOX，画完恢复）----
    const bool disabled =
        SDL_SetRenderLogicalPresentation(impl_->renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    bool drawn = false;
    if (disabled) {
        // ★ src = 本帧**已用**的子矩形（⛔ 不取整张容量：那会把未写区域 / 上一帧的旧像素采样进去），
        //   dst = 同一块 w×h（1:1，⛔ 不缩放）。
        const SDL_FRect src{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)};
        const SDL_FRect dst{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                            static_cast<float>(h)};
        drawn = SDL_RenderTexture(impl_->renderer, impl_->overlay_tex, &src, &dst);
    }
    // ★ 无论这次 draw 成败都要把逻辑呈现恢复成 LETTERBOX（否则下一帧的画面不再 letterbox，
    //   那是 B1 最危险的副作用；恢复用**当前帧**的 tex_w/tex_h）。
    const bool restored = SDL_SetRenderLogicalPresentation(
        impl_->renderer, impl_->tex_w, impl_->tex_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (!disabled) {
        if (err) *err = std::string("SDL_SetRenderLogicalPresentation(DISABLED, overlay) 失败：") + SDL_GetError();
        return false;
    }
    if (!drawn) {
        if (err) *err = std::string("SDL_RenderTexture(overlay) 失败：") + SDL_GetError();
        return false;
    }
    if (!restored) {
        if (err) *err = std::string("SDL_SetRenderLogicalPresentation(LETTERBOX 恢复) 失败：") + SDL_GetError();
        return false;
    }
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
    // 与 SDL_Renderer 路径一致：首帧 / 尺寸变化把窗口尺寸跟到后缓冲 —— **× scale**
    // （同一份换算 `windowSizeForFrame`，⛔ 两处各写一套就是这次的 bug）。
    if (!sized || direct_w != w || direct_h != h) {
        const WindowSize want = windowSizeForFrame(w, h, scale_report.scale);
        if (!SDL_SetWindowSize(window, want.w, want.h)) {
            if (err) *err = std::string("SDL_SetWindowSize 失败：") + SDL_GetError();
            return false;
        }
        // ★ 这两格仍是**帧**尺寸（判"要不要重设"用的是帧尺寸变化；窗口尺寸由 scale 唯一决定）。
        direct_w = w;
        direct_h = h;
        sized = true;
    }
    ++frame_no;
    notePresentTime();
    // ★ 叠层（B 的统一入口）：同一份脚本 display list → 宿主字体 → `presentOverlay`；
    //   直出路径的 `directOverlay` 只记下画布原点 + 上传 RGBA8，真正的 blit 在
    //   `directPresent` 的 letterbox 之后。画不出来 ⇒ 关掉叠层，呈现照旧。
    if (overlay) {
        std::string oerr;
        if (!owner->drawOverlay(&oerr)) disableOverlay(oerr.c_str());
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

// ★ 控制层 `resume`：记"恢复时刻"⇒ 早于它入队的 OS 输入一律丢弃（`pumpOneEvent` 的分类闸门）。
void SdlWindow::noteControlResume() { impl_->stale_input_before_ns = SDL_GetTicksNS(); }

const char* SdlWindow::controlStopReason() const { return impl_->control_stop_reason; }

// =======================================================================================
// 系统对话框（`runtime/message_box.h` 的 ModalSpec）
// ★ 只用正文 + 按钮：`SDL_ShowMessageBox` 由系统画（⛔ 不还原图标 / 位置 / 像素）。
// ★ 它会**阻塞**（OS 模态）：调用期间放掉 guest 令牌 ⇒ feeder 线程仍能跑（与 `SDL_RenderPresent`
//   同一条 M0 口径）。让出期间只持有宿主内存（spec / message / buttons 都是宿主的）。
// ★ 返回值：`spec.buttons[i].id` 原样当 SDL 的 buttonid；默认按钮标 RETURNKEY_DEFAULT，
//   `esc_result` 对应的按钮标 ESCAPEKEY_DEFAULT（系统把 X/Esc 交回该 id）。
// =======================================================================================
bool SdlWindow::showSystemMessageBox(const ModalSpec& spec, std::uint32_t* result) {
    if (result == nullptr) return false;
    if (impl_->window == nullptr) return false;
    if (spec.buttons.empty()) return false;  // 没有按钮的系统框没有返回值语义
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.reserve(spec.buttons.size());
    for (const ModalButton& b : spec.buttons) {
        SDL_MessageBoxButtonData data{};
        if (b.is_default) data.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if (b.id == spec.esc_result) data.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        data.buttonID = static_cast<int>(b.id);
        data.text = b.label.c_str();
        buttons.push_back(data);
    }
    const std::string message = modalMessageText(spec);
    SDL_MessageBoxData data{};
    data.flags = 0;  // ⛔ 不带 icon flag（用户明确不要图标）
    data.window = impl_->window;
    data.title = spec.caption.empty() ? nullptr : spec.caption.c_str();
    data.message = message.c_str();
    data.numbuttons = static_cast<int>(buttons.size());
    data.buttons = buttons.data();
    data.colorScheme = nullptr;
    int buttonid = -1;
    bool ok = false;
    {
        GuestToken::YieldGuard yield(guestToken());
        ok = SDL_ShowMessageBox(&data, &buttonid);
    }
    if (!ok) return false;
    // SDL 返回的 buttonid 必须能对上；对不上（某些平台 X 关闭给 -1 / 未定义）⇒ 用 X 的语义。
    for (const ModalButton& b : spec.buttons) {
        if (static_cast<int>(b.id) == buttonid) {
            *result = b.id;
            return true;
        }
    }
    *result = spec.close_result;
    return true;
}

// ★ `_ShowDialog@12` 的 2/0 臂：撤掉那次关窗请求（窗口留着，⛔ 不许宽限/截止把 run 收掉）。
//   4 个字段一起清 —— 少清任何一个，`pumpEvents` 的 500 ms 截止或 `advanceCloseFrame` 的
//   3 帧计数都会在下一帧 `requestStop()`（系统对话框期间 deadline 已经过期）。
void SdlWindow::cancelCloseRequest() {
    impl_->close_pending = false;
    impl_->close_frames_left = 0;
    impl_->close_deadline_ns = 0;
}

// 宿主侧请求关窗（`InputScript::close_window` / 测试）：与 SDL 的 X 同一条路（含宽限/二次确认）。
void SdlWindow::requestClose() { impl_->requestClose(); }

void SdlWindow::attachWindowManager(WindowManager* windows) { impl_->windows = windows; }

std::uint32_t SdlWindow::windowIdForTest() const {
    return impl_->window != nullptr ? SDL_GetWindowID(impl_->window) : 0u;
}

bool SdlWindow::stopRequested() const { return impl_->stop; }

bool SdlWindow::windowHidden() const {
    if (impl_->window == nullptr) return true;  // 没有窗口 ⇒ 当然没有"可见窗口"
    return (SDL_GetWindowFlags(impl_->window) & SDL_WINDOW_HIDDEN) != 0;
}

// ★ 宿主真光标（`--cursor-warp`，缺省关）。
//   生效判据是**运行时**的窗口可见性：`--present headless` 也建隐藏窗口 ⇒ `window != nullptr`
//   恒真、证明不了"看得见"；`windowHidden()` 读 `SDL_GetWindowFlags`，与 `describe()` 报出的
//   `window=hidden|visible` 是**同一份**判据（报出来的就是据以决定的）。
bool SdlWindow::hostCursorWarpEnabled() const { return impl_->cursor_warp && !windowHidden(); }

bool SdlWindow::hostCursorWarp(int guest_client_x, int guest_client_y) {
    if (!hostCursorWarpEnabled() || impl_->window == nullptr) return false;
    int ww = 0;
    int wh = 0;
    if (!SDL_GetWindowSize(impl_->window, &ww, &wh)) return false;
    // ② guest 客户区像素 → 宿主窗口客户区像素（与 `mapMouse` **同一份** letterbox 公式）。
    const LetterboxMap map = letterboxFor(ww, wh, impl_->frameImageW(), impl_->frameImageH());
    int wx = 0;
    int wy = 0;
    if (!imageToWindowPixel(map, guest_client_x, guest_client_y, &wx, &wy)) return false;
    // ③ 宿主窗口客户区 → 宿主屏幕坐标。`SDL_GetWindowPosition` 给的是**客户区原点**
    //    （SDL3 Win32 实现 = `ClientToScreen(hwnd,{0,0})`）；⛔ 不乘 `SDL_GetWindowPixelDensity`
    //    （窗口坐标与 SetCursorPos / CGWarp 都是同一套物理像素 / point 空间）。
    int win_x = 0;
    int win_y = 0;
    if (!SDL_GetWindowPosition(impl_->window, &win_x, &win_y)) return false;
    const int screen_x = win_x + wx;
    const int screen_y = win_y + wy;
    // ④ 平台原语（`runtime/host_cursor.h`）：false = 系统拒绝 / 平台不支持 ⇒ 调用方据此返回 FALSE。
    if (!hostCursorWarpToScreen(screen_x, screen_y)) return false;
    // ⑤ 登记回灌期望：记的是 **窗口客户区像素**（`wx/wy`）—— 也就是 SDL 之后会在
    //    `event.motion` 里报的那一对，**clamp 之前**。⛔ 不记 guest 坐标：`mapMouse` 的 clamp
    //    已经把越界信息丢了（`runtime/warp_echo.h` 的类注释）。
    impl_->warp_echo.note(wx, wy);
    // ★ 一次性读回自检（design §7.1）：第一次 warp 成功后读回实际位置；与目标差 >1 px 才 warn
    //   一行（含目标/实际/scale/off/window 位置），此后不再查。⛔ 不自动纠正（那会把映射 bug
    //   掩盖成"能跑"；Windows 的 RDP 上 SetCursorPos 可能"成功但没动"）。
    if (!impl_->warp_selfcheck_done) {
        impl_->warp_selfcheck_done = true;
        int qx = 0;
        int qy = 0;
        if (hostCursorQueryScreen(&qx, &qy) && (std::abs(qx - screen_x) > 1 || std::abs(qy - screen_y) > 1)) {
            lift::logAt(lift::LogLevel::Warn, lift::LogGeneral, "[present] ",
                        "--cursor-warp 读回自检：目标 (%d,%d) 实际 (%d,%d) 偏差 >1 px"
                        "（scale=%.3f off=%.1f,%.1f window=(%d,%d) guest_client=(%d,%d)）"
                        " ⇒ 映射可疑；⛔ 不自动纠正（design §2.3 查窗口原点 / DPI / letterbox）",
                        screen_x, screen_y, qx, qy, map.scale, map.off_x, map.off_y, win_x, win_y,
                        guest_client_x, guest_client_y);
        }
    }
    return true;
}

std::string SdlWindow::describe() const {
    std::string out = "driver=" + impl_->renderer_name + " video=" + impl_->video_driver;
    out += " gpu=" + (impl_->gpu_driver.empty() ? std::string("-") : impl_->gpu_driver);
    out += std::string(" window=") + (windowHidden() ? "hidden" : "visible");
    // ★ `--cursor-warp` 的**开关**状态（缺省关）。有效状态（on / on-but-hidden / unsupported）
    //   由 `main` 的另一行给：`describe` 只报 sink 自己知道的那一份。
    out += impl_->cursor_warp ? " cursor-warp=on" : " cursor-warp=off";
    if (impl_->direct_present && impl_->direct != nullptr) {
        out += std::string(" present=direct/") + impl_->direct->directPresentMode();
    } else if (!impl_->direct_error.empty()) {
        out += " present=renderer(直出不可用)";
    }
    return out;
}

bool SdlWindow::isGpuRenderer() const { return !impl_->gpu_driver.empty(); }

// `--scale` 的一次性 readout 数据：窗口档 = `init()` 建完窗口后的实测客户区；没建窗口 ⇒ 缺省 1280×720。
// ⛔ 这里不做任何换算（公式与文案在 `runtime/window_scale.h`），只把事实交出去。
ScaleReport SdlWindow::scaleReport() const { return impl_->scale_report; }

bool SdlWindow::directPresentActive() const { return impl_->direct_present && impl_->direct != nullptr; }

const char* SdlWindow::directPresentMode() const {
    return (impl_->direct_present && impl_->direct != nullptr) ? impl_->direct->directPresentMode() : "";
}

const char* SdlWindow::rendererName() const { return impl_->renderer_name.c_str(); }

// 调试叠层开关：关掉时把两条路径的叠层状态都**退役**（renderer 纹理进 `retired_overlay_tex`，
// 由 `shutdown()` 统一销毁；直出那一份由后端 `directOverlay(nullptr,...)` 移交 retired ——
// ⛔ 都不在关的这一下立即释放：上一帧的 draw 可能仍在 GPU 队列里，提前释放会让叠层撕裂/丢失）。
// 下一帧起窗口像素恢复成"只有画面"。默认关（`--overlay` / `--ui` 才打开）。
void SdlWindow::setOverlay(bool on) {
    impl_->overlay = on;
    // ★ 命中表 / capture 不跨叠层会话：新的脚本 / 新的一次开着，旧的 key 与 rect 都不再有效。
    impl_->click_router.clearFrame();
    if (on) {
        // ★ 生产路径在 `init()` 就装好字体 + 脚本（失败 ⇒ 硬错）；这里只兜"运行时再开"这条
        //   （测试 / 未来开关）：装不上 ⇒ **一次性 stderr + 保持关**（⛔ 不静默画空白、
        //   ⛔ 不让一个诊断层把 run 打死）。
        if (!impl_->loadUiScript(nullptr)) {
            std::fprintf(stderr, "[overlay] 脚本 / 宿主字体不可用 ⇒ 拒绝启用（--overlay / --ui；R5）\n");
            impl_->overlay = false;
        }
        return;
    }
    // ★ 关 ⇒ 丢掉这一份脚本（下次开会按 `ui_script_path` 重新装：`--ui` 可能换过内容）。
    impl_->ui_script.reset();
    // ★ 纹理只退役、不立即销毁（见字段注释）；保留到 `shutdown()` 的代价有界（关叠层次数很少）。
    if (impl_->overlay_tex != nullptr) {
        impl_->retired_overlay_tex.push_back(impl_->overlay_tex);
        impl_->overlay_tex = nullptr;
    }
    impl_->overlay_tex_w = 0;
    impl_->overlay_tex_h = 0;
    impl_->overlay_tex_cap_w = 0;
    impl_->overlay_tex_cap_h = 0;
    if (impl_->direct != nullptr) {
        std::string ignored;
        impl_->direct->directOverlay(nullptr, 0, 0, 0, 0, &ignored);
    }
}

bool SdlWindow::overlayEnabled() const { return impl_->overlay; }

// ★ `Machine` → 窗口 sink 的每帧只读读数（`script` / `worst`）。
//   ★ **只存值**：HUD 是 `scripts/ui/hud.js` 每帧重画 ⇒ 这里没有 dirty / 文本缓存可言。
void SdlWindow::setUiReadings(const UiReadings& readings) { impl_->ui_readings = readings; }

// ★ `Machine` / `AotHost` → 窗口 sink 的**帧列表读数源**（`game.local_frames()`）。
//   只存指针（非 owning）：真正的读发生在脚本调用那一下；⛔ 宿主不推表、不缓存。
void SdlWindow::setUiFrameSource(ui::UiFrameSource* src) { impl_->ui_frame_source = src; }

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
// GPU 光栅化（唯一实现）：独立 SDL_GPUDevice 的离屏 target（后端在 d3d9_gpu.cpp）
// ★ 本类是 `GpuRaster` 的**转发壳**：除 `rasterName`（缓存身份）这一条**有意的非纯转发**外，
//   每一条虚函数都必须显式转发到 `impl_->raster()`。漏一条 == 后端"实现了但永远收不到"（静默；不报错、不改像素、测试全绿）——
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
void SdlWindow::rasterRequestDepthClear(float z) {
    GpuRaster* r = impl_->raster();
    if (r != nullptr) r->rasterRequestDepthClear(z);
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


}  // namespace lift
