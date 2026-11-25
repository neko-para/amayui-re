// emulator-lift · 真实 SDL 窗口 / GPU 呈现的 CTest
//
// 用法：
//   lift-present-window            → SDL_VIDEODRIVER=dummy + software 渲染器（测窗口/纹理/回读）
//   lift-present-window --gpu      → 默认 GPU 驱动列表；SDL_GetGPURendererDevice()!=NULL 才算通过
//
// 判据：像素在 1:1 下精确相等（含 565、alpha=0、33×17 非紧凑 pitch）；GPU 测试必须有正证据
// （isGpuRenderer()），否则 SKIP 77（⛔ 不拿 software 假绿）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/host_window.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 脚本路径）
#include "ui/canvas.h"
#include "ui/element.h"
#include "ui/script.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <SDL3/SDL.h>

namespace {

// HUD 底色（与 `scripts/ui/hud.js` 的 `BG = "#101820CC"` 一致）。★ B 起 C++ 侧不再有这份常量：
// 颜色是**脚本**的版式，生产头里只剩 `ui_readings.h`（读数）与 `present_ui.h`（合成入口）。
constexpr std::uint32_t kOverlayBgRgb = 0x101820u;  // #101820
constexpr int kOverlayBgAlpha = 204;                // #101820CC ⇒ 80% 不透明

// 半透明底与底下画面的整数混合（与 `tests/overlay.cpp` / `d3d9_gpu.cpp` 的 CPU 混合同一条公式）：
//     d = (s*a + d*(255-a) + 127) / 255
constexpr std::uint32_t blendChannel(std::uint32_t s, std::uint32_t d, int a) {
    return (s * static_cast<std::uint32_t>(a) + d * static_cast<std::uint32_t>(255 - a) + 127u) / 255u;
}
constexpr std::uint32_t blendOver(std::uint32_t src_rgb, std::uint32_t dst_rgb, int a) {
    return (blendChannel((src_rgb >> 16) & 0xFFu, (dst_rgb >> 16) & 0xFFu, a) << 16) |
           (blendChannel((src_rgb >> 8) & 0xFFu, (dst_rgb >> 8) & 0xFFu, a) << 8) |
           blendChannel(src_rgb & 0xFFu, dst_rgb & 0xFFu, a);
}
// ±`tol` 逐通道相等（GPU / 软件 blend 的舍入可能与本文件的整数公式差 1）。
constexpr bool nearRgb(std::uint32_t got, std::uint32_t want, int tol) {
    for (int shift = 0; shift <= 16; shift += 8) {
        const long a = static_cast<long>((got >> shift) & 0xFFu);
        const long b = static_cast<long>((want >> shift) & 0xFFu);
        if (a - b > tol || b - a > tol) return false;
    }
    return true;
}

struct Fx {
    lift::GuestMemory mem;
    lift::SurfaceArena arena;
    std::uint32_t ea = 0;
    lift::GuestSurface* s = nullptr;
};

lift::SurfaceFormat format32(std::uint32_t amask) {
    lift::SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = amask;
    return f;
}

lift::SurfaceFormat format565() {
    lift::SurfaceFormat f;
    f.bpp = 16;
    f.rmask = 0xF800;
    f.gmask = 0x07E0;
    f.bmask = 0x001F;
    f.amask = 0;
    return f;
}

bool make(Fx& f, int w, int h, lift::SurfaceFormat fmt) {
    if (!f.mem.reserve() || !f.arena.init(&f.mem, 0x10000000u, 0x00100000u)) return false;
    f.ea = f.arena.create(w, h, fmt);
    f.s = f.arena.find(f.ea);
    return f.ea != 0 && f.s != nullptr;
}

bool openWindow(lift::SdlWindow& win, bool gpu, std::string* err) {
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift present test";
    opt.driver_list = gpu ? "gpu" : "software";
    opt.hidden = true;
    win.setCaptureForTest(true);
    return win.init(opt, err);
}

// 软件/dummy 档是**机械守卫**：打不开就是红，不许 SKIP 掉真实回归。
// GPU 档允许 SKIP（机器可能没有 GPU/桌面），但必须把失败原因/实际 driver 打出来。
bool requireWindow(lift::SdlWindow& win, bool gpu, std::string* err) {
    if (openWindow(win, gpu, err)) return true;
    std::fprintf(stderr, "openWindow 失败（gpu=%d）：%s\n", gpu ? 1 : 0, err->c_str());
    return false;
}

void fill32(Fx& f, std::uint32_t value, std::uint32_t alpha) {
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            const std::uint32_t v = (value & 0x00FFFFFFu) | (alpha & 0xFF000000u);
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 4, &v, 4);
        }
    }
}

bool expectRgb(lift::SdlWindow& win, int x, int y, std::uint32_t rgb, const char* what) {
    std::uint32_t argb = 0;
    std::string err;
    if (!win.readbackPixel(x, y, &argb, &err)) {
        std::fprintf(stderr, "  readbackPixel(%s) 失败：%s\n", what, err.c_str());
        return false;
    }
    const std::uint32_t got = argb & 0x00FFFFFFu;
    if (got != (rgb & 0x00FFFFFFu)) {
        std::fprintf(stderr, "  %s: expected RGB %06X, got %06X (argb=%08X)\n", what, rgb & 0xFFFFFFu, got, argb);
        return false;
    }
    return true;
}

// =====================================================================================
// ★ 控制层 resume 的**陈旧事件闸门**（判据 13 的机制面）——真 SDL 队列 + `SDL_PushEvent`
//
// 生产现场：`pause` 是完全停（暂停期间 `pumpEvents` 一次不跑）⇒ 暂停 10 s 里的点击 / 拖动 / 关窗
// 先排在 **Win32 队列**里（下一次 `SDL_PumpEvents` 才转换进 SDL 队列）。`resume` 首轮排空时：
//   · 早于"恢复时刻"的 **OS 输入** 必须丢弃（⛔ 不压进 guest 的无上界队列）；
//   · 早于"恢复时刻"的 **ESC** 当**宿主 stop**（⛔ 不投 `WM_CLOSE` 进 guest）。
//   ★ 关窗**拿不到**陈旧戳（`SDL_SendWindowEvent` 置 0 ⇒ `SDL_PushEvent` 填当下）⇒ 生产上到不了这个
//     分支；本条用 `SDL_PushEvent` 造戳，只证明纯映射 + sink 一侧，见 `REQ-01M7JNVD59RD49RSKD9N2S9TXW`。
// 判据 = `queueSize` / `mouseButtons` / `stopRequested` + `controlStopReason`。
// 红了意味着：resume 首轮把暂停期间的输入当"刚发生"灌给 guest，或暂停时的关窗把这一轮结束掉。
//
// ★ **本用例只是"映射 + sink"这一层**：它自己调 `win.noteControlResume()` 造恢复时刻。
//   "控制层到底有没有调到宿主"是**另一条**判据，见 `tests/control_endpoint.cpp` 的
//   `control-resume-arms-host-hook`（真协议 + 真 `serve()` 数 `controlOnResume()` 次数）——
//   缺陷 `REQ-01M7JFDZK1SZW72SBAW6BC3DRT` 就是"这里绿、接线根本不存在"。⛔ 两层都必须在。
// =====================================================================================
bool pushStaleEvent(SDL_Event* e, std::uint64_t timestamp_ns) {
    e->common.timestamp = timestamp_ns;
    return SDL_PushEvent(e);
}

void caseControlResumeStaleEvents() {
    ::lift_test::g_current_case = "control-resume-stale-events";

    auto open = [](lift::SdlWindow& win, lift::WindowManager& wm, std::string* err) -> bool {
        wm.registerClass("ctl-stale", 0, 0);
        const std::uint32_t hwnd = wm.createWindow("ctl-stale", 0, 0, 0, 0, 640, 480, 0);
        if (hwnd == 0) return false;
        lift::SdlWindowOptions opt;
        opt.title = "control stale events";
        opt.driver_list = "software";
        opt.hidden = true;
        if (!win.init(opt, err)) return false;
        win.attachWindowManager(&wm);
        win.noteControlResume();  // 恢复时刻 = 现在（之后的事件都算"新"）
        return true;
    };

    // ---- ① 陈旧 OS 输入 ⇒ 丢弃（队列 / 按钮电平一位都不动）----
    {
        lift::SdlWindow win;
        lift::WindowManager wm;
        std::string err;
        if (!open(win, wm, &err)) {
            std::fprintf(stderr, "open 失败（dummy+software 是机械守卫）：%s\n", err.c_str());
            LIFT_CHECK(false);  // ★ 不许 SKIP 掉真实回归（与文件头的 requireWindow 同口径）
            return;
        }
        const std::uint64_t resume_ns = SDL_GetTicksNS();
        win.noteControlResume();  // 恢复时刻 = resume_ns 之后的一刻
        const std::uint64_t stale = resume_ns > 5'000'000'000ull ? resume_ns - 5'000'000'000ull : 1ull;

        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = win.windowIdForTest();
        motion.motion.x = 100.0f;
        motion.motion.y = 120.0f;
        LIFT_CHECK(pushStaleEvent(&motion, stale));
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.windowID = win.windowIdForTest();
        down.button.button = SDL_BUTTON_LEFT;
        down.button.x = 100.0f;
        down.button.y = 120.0f;
        LIFT_CHECK(pushStaleEvent(&down, stale));
        win.pumpHostEvents();
        const std::size_t stale_queue = wm.queueSize();
        const std::uint32_t stale_buttons = wm.mouseButtons();
        std::printf("[judge13-stale] stale motion+down: queue=%zu buttons=%u\n", stale_queue,
                    static_cast<unsigned>(stale_buttons));
        LIFT_CHECK(stale_queue == 0);      // ⛔ 没有 WM_MOUSEMOVE 进 guest 队列
        LIFT_CHECK(stale_buttons == 0u);   // ⛔ 没有按钮电平

        // 对照：**新鲜** motion 必须照旧（证明上面的 0 不是因为"泵坏了"）。
        SDL_Event fresh{};
        fresh.type = SDL_EVENT_MOUSE_MOTION;
        fresh.motion.windowID = win.windowIdForTest();
        fresh.motion.x = 200.0f;
        fresh.motion.y = 220.0f;
        LIFT_CHECK(pushStaleEvent(&fresh, SDL_GetTicksNS() + 1'000'000ull));
        win.pumpHostEvents();
        std::printf("[judge13-stale] fresh motion: queue=%zu\n", wm.queueSize());
        LIFT_CHECK(wm.queueSize() >= 1);
        win.shutdown();
    }

    // ---- ② 陈旧关窗 ⇒ 宿主 stop（`bye.reason:"window-closed"`），⛔ 不投 WM_CLOSE ----
    {
        lift::SdlWindow win;
        lift::WindowManager wm;
        std::string err;
        if (!open(win, wm, &err)) {
            std::fprintf(stderr, "open 失败（dummy+software 是机械守卫）：%s\n", err.c_str());
            LIFT_CHECK(false);  // ★ 不许 SKIP 掉真实回归（与文件头的 requireWindow 同口径）
            return;
        }
        const std::uint64_t now = SDL_GetTicksNS();
        const std::uint64_t stale = now > 3'000'000'000ull ? now - 3'000'000'000ull : 1ull;
        SDL_Event close_ev{};
        close_ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        close_ev.window.windowID = win.windowIdForTest();
        LIFT_CHECK(pushStaleEvent(&close_ev, stale));
        win.pumpHostEvents();
        std::printf("[judge13-close] stop=%d reason=%s queue=%zu\n", win.stopRequested() ? 1 : 0,
                    win.controlStopReason() != nullptr ? win.controlStopReason() : "(null)", wm.queueSize());
        LIFT_CHECK(win.stopRequested());
        LIFT_CHECK(win.controlStopReason() != nullptr);
        LIFT_CHECK(std::string(win.controlStopReason()) == "window-closed");
        LIFT_CHECK(wm.queueSize() == 0);  // ⛔ 没有 WM_CLOSE 进 guest
        win.shutdown();
    }

    // ---- ③ 陈旧 ESC ⇒ 同一条宿主 stop（⛔ 不进 guest 的键盘路径）----
    {
        lift::SdlWindow win;
        lift::WindowManager wm;
        std::string err;
        if (!open(win, wm, &err)) {
            std::fprintf(stderr, "open 失败（dummy+software 是机械守卫）：%s\n", err.c_str());
            LIFT_CHECK(false);  // ★ 不许 SKIP 掉真实回归（与文件头的 requireWindow 同口径）
            return;
        }
        const std::uint64_t now = SDL_GetTicksNS();
        const std::uint64_t stale = now > 3'000'000'000ull ? now - 3'000'000'000ull : 1ull;
        SDL_Event esc{};
        esc.type = SDL_EVENT_KEY_DOWN;
        esc.key.windowID = win.windowIdForTest();
        esc.key.key = SDLK_ESCAPE;
        esc.key.scancode = SDL_SCANCODE_ESCAPE;
        LIFT_CHECK(pushStaleEvent(&esc, stale));
        win.pumpHostEvents();
        std::printf("[judge13-esc] stop=%d reason=%s queue=%zu keydown(ESC)=%d\n", win.stopRequested() ? 1 : 0,
                    win.controlStopReason() != nullptr ? win.controlStopReason() : "(null)", wm.queueSize(),
                    wm.keyDown(0x1B) ? 1 : 0);
        LIFT_CHECK(win.stopRequested());
        LIFT_CHECK(win.controlStopReason() != nullptr);
        LIFT_CHECK(std::string(win.controlStopReason()) == "window-closed");
        LIFT_CHECK(!wm.keyDown(0x1B));  // ⛔ 进 guest 的键盘状态没被改
        win.shutdown();
    }
}

// =====================================================================================
// HUD 面板几何（D4：面板**贴左 + 垂直居中**）—— 取样点只能从**面板自己**的矩形来
//
// ★ 为什么不再取画布原点：D4（2026-11-25 父级裁决）后面板 `x = 0`、`y = round((720 - panelH)/2)`
//   ⇒ 画布原点 `(0,0)` 落在面板**上方**的 guest 画面上（实测 `#8040C0` = 纯 guest）。
//   ⛔ 不写死第二个魔法数（`(0,171)` 那种）：面板的 x/y/w/h 由 `scripts/ui/hud.js` + 宿主字体
//   决定，行数一变就变 ⇒ 用**同一个脚本 + 同一份读数**跑一遍，取 display list 的第一条 Rect
//   （面板恒为 `elements[0]`，与 `tests/overlay.cpp` 的 `expectedPanel` 同法），再按本用例的
//   画布几何（`uiCanvasOrigin(640, 360)`，1:1 窗口）换算成窗口像素。
//
// ★ 读数与 `SdlWindow::drawOverlay` 同源（本窗口**第一次** Present）：
//   `notePresentTime()` 在 `drawOverlay` 之前 ⇒ `fps` 有值（最近 1 s 的 Present 次数）；
//   没有挂 `WindowManager` ⇒ 没有 `mouse` 行；没有 `Machine` ⇒ 没有 `script` / `worst` 行。
//   ⇒ 本夹具的面板 = `fps <n>` + `settings…`。★ 只取 **x / y**（宽高随 fps 位数变，不参与判据）。
// =====================================================================================
struct PanelGeom {
    int x = 0;  // 面板左上角（窗口像素）
    int y = 0;
    int w = 0;  // 面板尺寸（窗口像素）
    int h = 0;
};

bool hudPanelGeom(lift::FontEngine& font, int frame_w, int frame_h, PanelGeom* out) {
    const std::string path = std::string(LIFT_UI_SCRIPT_DIR) + "/hud.js";
    std::string source;
    {
        std::FILE* f = lift::openHostFileUtf8(path, "rb");
        if (f == nullptr) return false;
        char buf[8192];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
        const bool bad = std::ferror(f) != 0;
        std::fclose(f);
        if (bad || source.empty()) return false;
    }
    auto runtime = lift::ui::makeQuickJsRuntime();
    if (runtime == nullptr) return false;
    std::string err;
    std::unique_ptr<lift::ui::UiScript> script = runtime->load(source, path, &font, &err);
    if (script == nullptr) return false;
    lift::ui::UiFrameReadings readings;
    readings.frame = 1;
    readings.fps_ok = true;   // 第一次 Present 之前 `notePresentTime()` 已经记了一格
    readings.fps = 1.0;
    lift::ui::UiBuilder builder;
    if (!script->render(builder, readings, &err)) return false;
    if (builder.elements().empty()) return false;
    const lift::ui::Element& panel = builder.elements().front();
    if (panel.kind != lift::ui::ElementKind::Rect) return false;
    const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(frame_w, frame_h);
    if (canvas.scale <= 0.0) return false;
    // 参考像素 → 窗口像素（与 `uiCanvasOrigin` 的取整同一口径：round-half-up）。
    const auto scaleRef = [&canvas](int ref_px) {
        return static_cast<int>(static_cast<long long>(ref_px) * canvas.scale + 0.5);
    };
    out->x = canvas.offset_x + scaleRef(panel.x);
    out->y = canvas.offset_y + scaleRef(panel.y);
    out->w = scaleRef(panel.w);
    out->h = scaleRef(panel.h);
    if (out->w < 1) out->w = 1;
    if (out->h < 1) out->h = 1;
    return true;
}

// =====================================================================================
// M0 · 统一合成：`--overlay` 在**窗口像素**上按画布原点 1:1 落位（renderer / 直出两条路径都过）
//
// 判据：16:9 帧（640×360）⇒ 画布原点 == 画面原点 (0,0)；**面板左上角**那一个像素是
//       HUD 底 `kOverlayBg` 与 guest 的 80% 混合值（D4：面板贴左 + 垂直居中 ⇒ 取样点取自
//       `hud.js` 的 display list，⛔ 不是画布原点 —— 那里现在是面板上方的 guest）；
//       窗口右下角仍是 guest 后缓冲的颜色（⛔ 叠层没有铺满整窗）；guest 像素逐字节不变。
//       ★ `gpu=true` 变体必须真的走**直出**：`opt.raster_gpu = true` 才会建 `GpuDirectPresenter`
//         （缺这一行 ⇒ `directPresentActive()` 恒 false ⇒ 本用例一条断言都跑不到）。
//       缺宿主字体 / 无 GPU 直出 ⇒ **返回 false**，由调用方 `LIFT_SKIP`（exit 77）。
//       ⛔ 不许"打印一行然后 return 0"：那在 CTest 输出里跟跑绿了长得一样（`check.h` 文件头）。
// =====================================================================================
bool caseOverlayCanvasComposition(bool gpu, std::string* skip_why) {
    ::lift_test::g_current_case = "overlay-canvas-1to1";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体（LIFT_HOST_FONT_PATH）⇒ overlay-canvas-1to1 无法判定";
        return false;
    }
    Fx f;
    if (!make(f, 640, 360, format32(0))) {
        LIFT_CHECK(false);
        return true;
    }
    fill32(f, 0x008040C0u, 0xFF000000u);
    const auto* p = f.arena.pixels(*f.s);
    const std::vector<std::uint8_t> before(p, p + static_cast<std::size_t>(f.s->pitch) * f.s->height);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift overlay canvas test";
    opt.driver_list = gpu ? "gpu" : "software";
    opt.raster_gpu = gpu;  // ★ 直出变体必须真建 GPU 直出（否则 directPresentActive() 恒 false）
    opt.hidden = true;
    opt.overlay = true;
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        if (gpu) {
            *skip_why = "gpu 窗口 / GPU device 不可用：" + err;
            return false;
        }
        LIFT_CHECK(false);  // 软件 / dummy 档是机械守卫：打不开就是红（⛔ 不 SKIP）
        return true;
    }
    if (gpu && !win.directPresentActive()) {
        *skip_why = std::string("gpu 直出未成立：") + win.describe();
        win.shutdown();
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());
    LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
    // ★ HUD 底是 `#101820CC`（alpha 204 ⇒ 80% 不透明）：**两条路径都按 alpha 与画面混合**
    //   · renderer 路径走 `SDL_BLENDMODE_BLEND`；
    //   · 直出（`gpu` 变体）走 D 单的 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 混合管线
    //     （⛔ 不再是 `SDL_BlitGPUTexture` 拷贝）。
    //   ⇒ 期望同值 = `blend(#101820, guest, 204)`，±1 舍入。
    const std::uint32_t guest_rgb = 0x8040C0u;  // `fill32(f, 0x008040C0u, ...)` 的 RGB
    const std::uint32_t bg = blendOver(kOverlayBgRgb, guest_rgb, kOverlayBgAlpha);
    // ★ 取样点 = **面板左上角**（从 `hud.js` 的 display list 算，见上面的 `hudPanelGeom`）。
    //   为什么这一点是**纯面板底**：面板四边内边距 `PAD = 8`（参考像素）⇒ 左上角那一格在
    //   第一行文本**上方**、也不在任何带 `key` 的命中 Rect 里（设置入口那条透明 Rect 盖的是
    //   最后一行）。⇒ 期望值仍是 80% 混合底，⛔ 不需要为取样点换期望值。
    PanelGeom panel;
    if (!hudPanelGeom(font, f.s->width, f.s->height, &panel)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    // 判据（几何前提，红了意味着取样点本身没意义）：面板贴左、整块在画面内、且**不是**画布原点。
    LIFT_CHECK_EQ_I64(panel.x, 0);  // D4：贴左
    LIFT_CHECK(panel.y > 0);        // D4：垂直居中 ⇒ 画布原点已是面板上方的 guest
    LIFT_CHECK(panel.y + panel.h <= f.s->height);
    LIFT_CHECK(panel.w <= f.s->width);
    std::uint32_t px = 0;
    LIFT_CHECK(win.readbackPixel(panel.x, panel.y, &px, &err));
    // ★ 读数（不是断言）：旧取样点（画布原点）现在读到的是什么 —— D4 之后它是**面板上方的 guest**。
    std::uint32_t corner = 0;
    const bool corner_ok = win.readbackPixel(0, 0, &corner, &err);
    // ★ 读数（不是断言）：`ctest -V` 直接看到混合后的实际值与期望值。
    std::printf("overlay-canvas-1to1: 面板=(%d,%d %dx%d) 面板角=#%06X 期望 #%06X（%s，tol=1）"
                " · 旧取样点(0,0)=%s#%06X（D4 后是 guest）\n",
                panel.x, panel.y, panel.w, panel.h, px & 0x00FFFFFFu, bg,
                gpu ? "直出：80% 混合" : "renderer：80% 混合", corner_ok ? "" : "(读不到)",
                corner & 0x00FFFFFFu);
    // 混合侧给 1 的容差。
    LIFT_CHECK(nearRgb(px & 0x00FFFFFFu, bg, 1));
    // ★ 有区分力：取样点不是 guest 色（旧 `(0,0)` 现在读到 guest ⇒ 这一条会红）。
    LIFT_CHECK(!nearRgb(px & 0x00FFFFFFu, guest_rgb, 1));
    LIFT_CHECK(win.readbackPixel(639, 359, &px, &err));
    LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, 0x8040C0u);  // 右下角仍是 guest
    const auto* after = f.arena.pixels(*f.s);
    LIFT_CHECK(before == std::vector<std::uint8_t>(after, after + static_cast<std::size_t>(f.s->pitch) * f.s->height));
    win.shutdown();
    return true;
}

// =====================================================================================
// `--scale-filter`（2026-11-21）：放大滤波器**真的**接到呈现上（1.5× 的确定性对照）
//
// 判据（红了意味着什么）：
//   ① `nearest` 档在 1.5× 上逐像素 == **旧 NEAREST 映射**（公式显式写在这里：
//      `sx = (wx-rect.x) * 1280 / 1920`，整数截断）。红 ⇒ 回退口径失效 ——
//      像素党 / A/B / 既有截图守卫的基准不再"逐字节相同"。
//   ② `linear` 档在**同一个位置**给出两相邻源像素之间的值（不是纯色）；红 ⇒ 缺省档静默退回
//      硬边（放大后比真机 150% DPI 的平滑放大糙）。
//   ③ `--scale 1.0`（1:1）两档逐字节同值：红 ⇒ 缺省窗口的画面被滤波器动过（"缺省行为不变"崩）。
// ★ 为什么这段能当判据：guest 内容由本文件造（竖条纹），没有动画 / 没有墙钟 ⇒ 像素是确定的
//   （游戏截图做不到这一点：LOAD / TITLE 屏的背景都按墙钟动，同配置两跑已有 44~59% 像素不同）。
// ★ 覆盖哪条路径：`gpu=false` = SDL_Renderer（software）⇒ 回读是**真实窗口像素**
//   （`SDL_RenderReadPixels`，滤镜由渲染器施加）；`gpu=true` = 真 GPU renderer（同一句
//   `SDL_SetTextureScaleMode`，只是施加在 GPU 上）。
// =====================================================================================
// 竖条纹：偶数列红、奇数列蓝（1.5× 时每 3 个窗口像素对 2 个源像素 ⇒ 边界落在 x%3==2）。
void fillStripes(Fx& f) {
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            const std::uint32_t rgb = (x % 2 == 0) ? 0x00FF0000u : 0x000000FFu;  // 0x00RRGGBB
            // guest 后缓冲的内存序 = B,G,R,X（X8R8G8B8）⇒ 按字写 0x00RRGGBB 正好是 B,G,R,0
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 4, &rgb, 4);
        }
    }
}

// 旧 NEAREST 映射（`d3d9_gpu.cpp::directReadback` 的整数公式；1.5× 时 3:2）：
//   sx = (wx - 0) * 1280 / 1920（**整数截断**）
int oldNearestSrcX(int wx) { return (wx * 1280) / 1920; }

// 1280×720 的 fixture：`make()` 的 arena 只有 1 MiB（够小图，不够 1280×720×4 ≈ 3.5 MiB）。
bool makeBig(Fx& f) {
    if (!f.mem.reserve() || !f.arena.init(&f.mem, 0x10000000u, 0x00800000u)) return false;
    f.ea = f.arena.create(1280, 720, format32(0));
    f.s = f.arena.find(f.ea);
    return f.ea != 0 && f.s != nullptr;
}

bool caseScaleFilter(bool gpu, std::string* skip_why) {
    ::lift_test::g_current_case = "present-window/scale-filter-1.5";
    // (a) 1.5× + nearest：窗口像素 == 旧整数映射取到的源像素（逐点精确）
    {
        Fx f;
        LIFT_CHECK(makeBig(f));
        fillStripes(f);
        lift::SdlWindow win;
        lift::SdlWindowOptions opt;
        opt.title = "emulator-lift scale-filter nearest";
        opt.driver_list = gpu ? "gpu" : "software";
        opt.raster_gpu = gpu;
        opt.hidden = true;
        opt.scale.kind = lift::ScaleSpec::Kind::Fixed;
        opt.scale.value = 1.5;
        opt.scale.filter = lift::ScaleFilter::Nearest;
        win.setCaptureForTest(true);
        std::string err;
        if (!win.init(opt, &err)) {
            if (gpu) {
                *skip_why = "gpu 窗口 / GPU device 不可用：" + err;
                return false;
            }
            LIFT_CHECK(false);
            return true;
        }
        if (gpu && !win.directPresentActive()) {
            *skip_why = std::string("gpu 直出未成立：") + win.describe();
            win.shutdown();
            return false;
        }
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK_EQ_I64(win.scaleReport().window_w, 1920);
        LIFT_CHECK_EQ_I64(win.scaleReport().window_h, 1080);
        // 采样点：wx = 0/1/2/3/4/5 覆盖"3 窗口像素对 2 源像素"的相位；wy 取 1:1 的行（sy = wy*2/3）。
        for (int wx = 0; wx < 12; ++wx) {
            const int sx = oldNearestSrcX(wx);
            const std::uint32_t want = (sx % 2 == 0) ? 0x00FF0000u : 0x000000FFu;
            std::uint32_t got = 0;
            LIFT_CHECK(win.readbackPixel(wx, 3, &got, &err));
            ++lift_test::g_checks;
            if ((got & 0x00FFFFFFu) != want) {
                lift_test::failf(__FILE__, __LINE__,
                                 "nearest 1.5x：窗口 (%d,3) 期望旧映射 sx=%d 的颜色 #%06X，实得 #%06X",
                                 wx, sx, want, got & 0x00FFFFFFu);
            }
        }
        if (gpu) std::printf("[scale-filter] nearest 1.5x（%s）：逐点命中旧整数映射\n", win.describe().c_str());
        win.shutdown();
    }
    // (b) 1.5× + linear：同一位置必须是**混合值**（两相邻源像素之间），且不等于 nearest 的那一个。
    {
        Fx f;
        LIFT_CHECK(makeBig(f));
        fillStripes(f);
        lift::SdlWindow win;
        lift::SdlWindowOptions opt;
        opt.title = "emulator-lift scale-filter linear";
        opt.driver_list = gpu ? "gpu" : "software";
        opt.raster_gpu = gpu;
        opt.hidden = true;
        opt.scale.kind = lift::ScaleSpec::Kind::Fixed;
        opt.scale.value = 1.5;
        opt.scale.filter = lift::ScaleFilter::Linear;
        win.setCaptureForTest(true);
        std::string err;
        if (!win.init(opt, &err)) {
            if (gpu) {
                *skip_why = "gpu 窗口 / GPU device 不可用：" + err;
                return false;
            }
            LIFT_CHECK(false);
            return true;
        }
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        // wx=3 ⇒ 源坐标 fx = (3+0.5)*1280/1920 - 0.5 = 1.8333 ⇒ 采源像素 1(蓝)/2(红) 之间
        //   ⇒ R 与 B **同时**非零（nearest 在这里是纯色）。容差放宽到"严格在 8..247 之间"，
        //   只钉"确实是双线性"，不钉具体舍入（软件 / GPU 后端的舍入可能不同）。
        std::uint32_t got = 0;
        LIFT_CHECK(win.readbackPixel(3, 3, &got, &err));
        const std::uint32_t r = (got >> 16) & 0xFFu;
        const std::uint32_t b = got & 0xFFu;
        std::printf("[scale-filter] linear 1.5x：窗口 (3,3)=#%06X（nearest 同点是纯色 #%06X）\n", got & 0x00FFFFFFu,
                    ((oldNearestSrcX(3) % 2 == 0) ? 0x00FF0000u : 0x000000FFu));
        ++lift_test::g_checks;
        if (!(r > 8u && r < 247u && b > 8u && b < 247u)) {
            lift_test::failf(__FILE__, __LINE__,
                             "linear 1.5x：窗口 (3,3) 应为两相邻源像素的混合（R/B 都非纯），实得 R=%u B=%u（#%06X）",
                             r, b, got & 0x00FFFFFFu);
        }
        win.shutdown();
    }
    // (c) 1:1（缺省 scale 1.0）两档逐字节同值：滤波器在 1:1 上没有可见差别。
    {
        const std::uint32_t kWant[2] = {0x00FF0000u, 0x000000FFu};
        for (int pass = 0; pass < 2; ++pass) {
            Fx f;
            LIFT_CHECK(make(f, 64, 48, format32(0)));
            fillStripes(f);
            lift::SdlWindow win;
            lift::SdlWindowOptions opt;
            opt.title = "emulator-lift scale-filter 1to1";
            opt.driver_list = gpu ? "gpu" : "software";
            opt.raster_gpu = gpu;
            opt.hidden = true;
            opt.scale.filter = pass == 0 ? lift::ScaleFilter::Linear : lift::ScaleFilter::Nearest;
            win.setCaptureForTest(true);
            std::string err;
            if (!win.init(opt, &err)) {
                if (gpu) {
                    *skip_why = "gpu 窗口 / GPU device 不可用：" + err;
                    return false;
                }
                LIFT_CHECK(false);
                return true;
            }
            LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
            for (int x = 0; x < 8; ++x) {
                std::uint32_t got = 0;
                LIFT_CHECK(win.readbackPixel(x, 2, &got, &err));
                ++lift_test::g_checks;
                if ((got & 0x00FFFFFFu) != kWant[x % 2]) {
                    lift_test::failf(__FILE__, __LINE__, "1:1（filter=%s）：窗口 (%d,2) 期望 #%06X，实得 #%06X",
                                     lift::scaleFilterName(opt.scale.filter), x, kWant[x % 2], got & 0x00FFFFFFu);
                }
            }
            win.shutdown();
        }
    }
    return true;
}


}  // namespace

int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";

    // ---- 32bpp X8：1:1 精确回读 ----
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0)));
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        LIFT_CHECK(std::strcmp(win.rendererName(), gpu ? "gpu" : "software") == 0);
        if (gpu && !win.isGpuRenderer()) LIFT_SKIP(win.describe().c_str());
        fill32(f, 0x000020u, 0xFF000000u);
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        std::printf("present-window%s: driver=%s\n", gpu ? "-gpu" : "", win.rendererName());
        LIFT_CHECK(expectRgb(win, 0, 0, 0x000020u, "x8(0,0)"));
        fill32(f, 0x00112233u, 0xFF000000u);
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(expectRgb(win, 1, 1, 0x112233u, "x8(1,1)"));
        // P0-2：关窗事件必须真的被 PollEvent 取到（PumpEvents 不会出队）。
        SDL_Event quit_event{};
        quit_event.type = SDL_EVENT_QUIT;
        LIFT_CHECK(SDL_PushEvent(&quit_event));
        win.pumpHostEvents();
        LIFT_CHECK(win.stopRequested());
        win.shutdown();
    }

    // ---- P3：SDL 事件 → guest Win32 消息（键鼠 / 滚轮 / 关窗宽限）----
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0)));
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        lift::WindowManager wm;
        const std::uint32_t hwnd = wm.createWindow("EventTest", 0x10000000u, 0, 0, 0, 1280, 720, 0);
        LIFT_CHECK_NE_U32(hwnd, 0u);
        win.attachWindowManager(&wm);
        const SDL_WindowID wid = win.windowIdForTest();

        SDL_Event e{};
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.windowID = wid;
        e.key.key = SDLK_A;
        e.key.scancode = SDL_SCANCODE_A;
        e.key.down = true;
        LIFT_CHECK(SDL_PushEvent(&e));
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = wid;
        e.motion.x = 640.0f;
        e.motion.y = 360.0f;
        LIFT_CHECK(SDL_PushEvent(&e));
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.windowID = wid;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = 100.0f;
        e.button.y = 200.0f;
        e.button.down = true;
        LIFT_CHECK(SDL_PushEvent(&e));
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = wid;
        e.wheel.y = 1.0f;
        e.wheel.mouse_x = 640.0f;
        e.wheel.mouse_y = 360.0f;
        LIFT_CHECK(SDL_PushEvent(&e));
        win.pumpHostEvents();

        lift::WinMessage msg;
        auto take = [&](std::uint32_t expected) {
            LIFT_CHECK(wm.peek(&msg, true));
            LIFT_CHECK_EQ_U32(msg.message, expected);
            return msg;
        };
        const lift::WinMessage key = take(0x0100u);
        LIFT_CHECK_EQ_U32(key.wparam, 0x41u);
        LIFT_CHECK(wm.keyDown(0x41u));
        const lift::WinMessage move = take(0x0200u);
        LIFT_CHECK_EQ_U32(move.lparam, (360u << 16) | 640u);
        const lift::WinMessage down = take(0x0201u);
        LIFT_CHECK_EQ_U32(down.wparam & 0x0001u, 0x0001u);
        // ★ 按键电平：引擎 `GetAsyncKeyState(VK_LBUTTON)` 只读影子键状态（`host_window.h` 的
        //   同一份 `key_down_`）；SDL 的 DOWN 必须写进去，否则轮询恒 0（实测 1827 次全 0）。
        LIFT_CHECK(wm.keyDown(0x01u));
        const lift::WinMessage wheel = take(0x020Au);
        LIFT_CHECK_EQ_U32((wheel.wparam >> 16) & 0xFFFFu, 120u);

        e = SDL_Event{};
        e.type = SDL_EVENT_KEY_UP;
        e.key.windowID = wid;
        e.key.key = SDLK_A;
        e.key.scancode = SDL_SCANCODE_A;
        e.key.down = false;
        LIFT_CHECK(SDL_PushEvent(&e));
        win.pumpHostEvents();
        take(0x0101u);
        LIFT_CHECK(!wm.keyDown(0x41u));

        // 左键抬起：消息与电平都要回到"未按下"。
        e = SDL_Event{};
        e.type = SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = wid;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = 640.0f;
        e.button.y = 360.0f;
        LIFT_CHECK(SDL_PushEvent(&e));
        win.pumpHostEvents();
        take(0x0202u);
        LIFT_CHECK(!wm.keyDown(0x01u));

        // 关窗先发 WM_CLOSE；两帧宽限后才 stopRequested（guest 有机会自己确认）。
        e = SDL_Event{};
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = wid;
        LIFT_CHECK(SDL_PushEvent(&e));
        win.pumpHostEvents();
        take(0x0010u);
        LIFT_CHECK(!win.stopRequested());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(!win.stopRequested());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(!win.stopRequested());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(win.stopRequested());
        LIFT_CHECK(wm.quitPosted());
        win.shutdown();
    }

    // ---- 565 ----
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format565()));
        auto* p = f.arena.pixels(*f.s);
        for (int y = 0; y < f.s->height; ++y) {
            for (int x = 0; x < f.s->width; ++x) {
                const std::uint16_t v = 0xF800;  // red
                std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 2, &v, 2);
            }
        }
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        LIFT_CHECK(std::strcmp(win.rendererName(), gpu ? "gpu" : "software") == 0);
        if (gpu && !win.isGpuRenderer()) LIFT_SKIP(win.describe().c_str());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(expectRgb(win, 0, 0, 0xFF0000u, "565 red"));
        win.shutdown();
    }

    // ---- ARGB8888 + alpha=0：blend NONE 的承重测试（默认 BLEND 会整窗黑）----
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0xFF000000u)));
        fill32(f, 0x00203040u, 0x00000000u);
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        LIFT_CHECK(std::strcmp(win.rendererName(), gpu ? "gpu" : "software") == 0);
        if (gpu && !win.isGpuRenderer()) LIFT_SKIP(win.describe().c_str());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(expectRgb(win, 0, 0, 0x203040u, "argb alpha=0"));
        win.shutdown();
    }

    // ---- 33x17 **16bpp**：pitch = 68 ≠ 33*2（非紧凑）----
    {
        Fx f;
        LIFT_CHECK(make(f, 33, 17, format565()));
        auto* p = f.arena.pixels(*f.s);
        std::memset(p, 0xAB, static_cast<std::size_t>(f.s->pitch) * f.s->height);
        const std::uint16_t red = 0xF800;
        std::memcpy(p + 1u * f.s->pitch + 1u * 2u, &red, 2);
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        LIFT_CHECK(std::strcmp(win.rendererName(), gpu ? "gpu" : "software") == 0);
        if (gpu && !win.isGpuRenderer()) LIFT_SKIP(win.describe().c_str());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(expectRgb(win, 1, 1, 0xFF0000u, "33x17 565 pitch"));
        win.shutdown();
    }

    // ---- 同一窗口：64x48 32bpp → 64x48 16bpp → 33x17 16bpp（texture/逻辑尺寸重建）----
    {
        Fx f;
        LIFT_CHECK(make(f, 64, 48, format32(0)));
        fill32(f, 0x00112233u, 0xFF000000u);
        lift::SdlWindow win;
        std::string err;
        if (!requireWindow(win, gpu, &err)) {
            if (gpu) LIFT_SKIP(err.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        LIFT_CHECK(std::strcmp(win.rendererName(), gpu ? "gpu" : "software") == 0);
        if (gpu && !win.isGpuRenderer()) LIFT_SKIP(win.describe().c_str());
        LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
        LIFT_CHECK(expectRgb(win, 0, 0, 0x112233u, "recreate 32bpp"));

        const std::uint32_t ea2 = f.arena.create(64, 48, format565());
        lift::GuestSurface* s2 = f.arena.find(ea2);
        LIFT_CHECK(s2 != nullptr);
        if (s2 == nullptr) {
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        auto* p2 = f.arena.pixels(*s2);
        for (int y = 0; y < s2->height; ++y) {
            for (int x = 0; x < s2->width; ++x) {
                const std::uint16_t red = 0xF800;
                std::memcpy(p2 + static_cast<std::size_t>(y) * s2->pitch + static_cast<std::size_t>(x) * 2, &red, 2);
            }
        }
        LIFT_CHECK(win.submitFrame(f.arena, *s2, &err));  // 同尺寸、换格式
        LIFT_CHECK(expectRgb(win, 0, 0, 0xFF0000u, "recreate 565"));

        const std::uint32_t ea3 = f.arena.create(33, 17, format565());
        lift::GuestSurface* s3 = f.arena.find(ea3);
        LIFT_CHECK(s3 != nullptr);
        if (s3 == nullptr) {
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        auto* p3 = f.arena.pixels(*s3);
        for (int y = 0; y < s3->height; ++y) {
            for (int x = 0; x < s3->width; ++x) {
                const std::uint16_t red = 0xF800;
                std::memcpy(p3 + static_cast<std::size_t>(y) * s3->pitch + static_cast<std::size_t>(x) * 2, &red, 2);
            }
        }
        LIFT_CHECK(win.submitFrame(f.arena, *s3, &err));  // 尺寸变化 ⇒ 重建 + 重设窗口
        LIFT_CHECK(expectRgb(win, 1, 1, 0xFF0000u, "recreate 33x17"));
        win.shutdown();
    }

    // ---- P3：窗口关闭 → Machine::run 以 StopReason::Stopped 收口（exit 0 的判据）----
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        const std::uint8_t loop[] = {0xEB, 0xFE};  // jmp $
        LIFT_CHECK(m.placeCode(0x00401000u, loop, sizeof(loop), &err));
        auto sink = std::make_unique<lift::SdlWindow>();
        std::string werr;
        if (!requireWindow(*sink, gpu, &werr)) {
            if (gpu) LIFT_SKIP(werr.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        lift::SdlWindow* win = sink.get();
        const std::uint32_t hwnd = m.cpu().windows->createWindow("CloseTest", 0x10000000u, 0, 0, 0, 1280, 720, 0);
        LIFT_CHECK_NE_U32(hwnd, 0u);
        win->attachWindowManager(m.cpu().windows);
        m.cpu().windows->setHostEventPump([win](lift::X86&) { win->pumpHostEvents(1); });
        m.presenter().setSink(std::move(sink));
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        LIFT_CHECK(SDL_PushEvent(&quit));
        m.cpu().eip = 0x00401000u;
        lift::RunOptions opt;
        opt.max_insns = 200000u;
        const lift::RunResult r = m.run(opt);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(r.detail.find("窗口关闭") != std::string::npos);
        LIFT_CHECK(r.ok());
        LIFT_CHECK(win->stopRequested());
    }

    // ---- P3：鼠标移动必须同时更新 guest 的**屏幕**光标位置（TITLE hover 依赖 0x109）----
    //   判据：SDL motion 是客户区坐标；`GetCursorPos` 返回屏幕坐标（客户区 + 窗口原点）。
    //   ⛔ 只 post WM_MOUSEMOVE 而不更新虚拟光标 ⇒ TITLE 的命中查找永远读到 (0,0)。
    {
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        auto win = std::make_unique<lift::SdlWindow>();
        std::string werr;
        if (!requireWindow(*win, gpu, &werr)) {
            if (gpu) LIFT_SKIP(werr.c_str());
            LIFT_CHECK(false);
            caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
        }
        const std::uint32_t hwnd =
            m.cpu().windows->createWindow("MouseTest", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
        LIFT_CHECK_NE_U32(hwnd, 0u);
        const lift::HostWindow* w = m.cpu().windows->find(hwnd);
        LIFT_CHECK(w != nullptr);
        win->attachWindowManager(m.cpu().windows);
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = win->windowIdForTest();
        motion.motion.x = 1165.0f;
        motion.motion.y = 365.0f;
        LIFT_CHECK(SDL_PushEvent(&motion));
        win->pumpHostEvents(0);
        int cx = -1;
        int cy = -1;
        m.cpu().windows->getCursorPos(&cx, &cy);
        if (w != nullptr) {
            LIFT_CHECK_EQ_I64(cx, static_cast<std::int64_t>(w->x) + 1165);
            LIFT_CHECK_EQ_I64(cy, static_cast<std::int64_t>(w->y) + 365);
        }
        bool saw_move = false;
        for (std::size_t i = 0; i < m.cpu().windows->queueSize(); ++i) {
            lift::WinMessage msg;
            if (m.cpu().windows->peekAt(i, &msg) && msg.message == 0x0200u) {
                saw_move = true;
                LIFT_CHECK_EQ_U32(msg.lparam & 0xFFFFu, 1165u);
                LIFT_CHECK_EQ_U32((msg.lparam >> 16) & 0xFFFFu, 365u);
            }
        }
        LIFT_CHECK(saw_move);

        // 按键：同样更新虚拟光标 + 队列里是 WM_LBUTTONDOWN。
        SDL_Event button{};
        button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        button.button.windowID = win->windowIdForTest();
        button.button.button = SDL_BUTTON_LEFT;
        button.button.x = 200.0f;
        button.button.y = 300.0f;
        LIFT_CHECK(SDL_PushEvent(&button));
        win->pumpHostEvents(0);
        m.cpu().windows->getCursorPos(&cx, &cy);
        if (w != nullptr) {
            LIFT_CHECK_EQ_I64(cx, static_cast<std::int64_t>(w->x) + 200);
            LIFT_CHECK_EQ_I64(cy, static_cast<std::int64_t>(w->y) + 300);
        }
        bool saw_button = false;
        for (std::size_t i = 0; i < m.cpu().windows->queueSize(); ++i) {
            lift::WinMessage msg;
            if (m.cpu().windows->peekAt(i, &msg) && msg.message == 0x0201u) {
                saw_button = true;
                LIFT_CHECK_EQ_U32(msg.lparam & 0xFFFFu, 200u);
                LIFT_CHECK_EQ_U32((msg.lparam >> 16) & 0xFFFFu, 300u);
            }
        }
        LIFT_CHECK(saw_button);

        // 滚轮：光标位置也要更新（lParam 的屏幕/客户区口径是既有问题，不在本断言范围）。
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.windowID = win->windowIdForTest();
        wheel.wheel.mouse_x = 400.0f;
        wheel.wheel.mouse_y = 500.0f;
        wheel.wheel.y = 1.0f;
        LIFT_CHECK(SDL_PushEvent(&wheel));
        win->pumpHostEvents(0);
        m.cpu().windows->getCursorPos(&cx, &cy);
        if (w != nullptr) {
            LIFT_CHECK_EQ_I64(cx, static_cast<std::int64_t>(w->x) + 400);
            LIFT_CHECK_EQ_I64(cy, static_cast<std::int64_t>(w->y) + 500);
        }

        // ---- P3：**帧尺寸**必须进 `mapMouse`（⛔ 不是常数 1280×720）----
        //   提交一张 640×480 的帧 ⇒ `present` 用 `SDL_SetWindowSize` 把窗口设成帧尺寸
        //   （实测 dummy 后端跟随：`SDL_GetWindowSize` = 640x480）。随后窗口像素 motion (320,240)
        //   必须按 letterbox(640×480 窗口 + 640×480 image ⇒ 恒等) 映射到 guest 客户区 (320,240)。
        //   ★ 若 `mapMouse` 把 image 写回常数 1280×720：scale=min(640/1280, 480/720)=0.5、
        //     off_y=(480-720*0.5)/2=60 ⇒ 窗口 (320,240) 映射成画面 (640,360)（x 还在 0..1279 内，
        //     不被 clamp）⇒ guest 屏幕偏成 (w.x+640, w.y+360)，实测红在 expected 420/290 got 740/410。
        {
            const std::uint32_t fea = m.surfaces().create(640, 480, format32(0u));
            lift::GuestSurface* fs = m.surfaces().find(fea);
            LIFT_CHECK_NE_U32(fea, 0u);
            LIFT_CHECK(fs != nullptr);
            if (fs != nullptr) {
                std::string ferr;
                LIFT_CHECK(win->submitFrame(m.surfaces(), *fs, &ferr));
                int ww = -1;
                int wh = -1;
                SDL_Window* sw = SDL_GetWindowFromID(win->windowIdForTest());
                LIFT_CHECK(sw != nullptr);
                if (sw != nullptr) SDL_GetWindowSize(sw, &ww, &wh);
                LIFT_CHECK_EQ_I64(ww, 640);  // 前提：窗口跟随帧尺寸（不成立则本用例无意义）
                LIFT_CHECK_EQ_I64(wh, 480);
                SDL_Event m2{};
                m2.type = SDL_EVENT_MOUSE_MOTION;
                m2.motion.windowID = win->windowIdForTest();
                m2.motion.x = 320.0f;
                m2.motion.y = 240.0f;
                LIFT_CHECK(SDL_PushEvent(&m2));
                win->pumpHostEvents(0);
                int gx = -1;
                int gy = -1;
                m.cpu().windows->getCursorPos(&gx, &gy);
                if (w != nullptr) {
                    LIFT_CHECK_EQ_I64(gx, static_cast<std::int64_t>(w->x) + 320);
                    LIFT_CHECK_EQ_I64(gy, static_cast<std::int64_t>(w->y) + 240);
                    // 落在 640×480 帧内（guest 客户区坐标）。
                    LIFT_CHECK(gx - w->x >= 0 && gx - w->x < 640);
                    LIFT_CHECK(gy - w->y >= 0 && gy - w->y < 480);
                }
            }
        }
        win->shutdown();
    }

    // ---- `--scale` 的**调用点**守卫：窗口 = **帧尺寸 × scale**（2026-11-19 裁决 c-i）----
    //   ★ 为什么必须在**真窗口**上测：`tests/window_scale.cpp` 只测公式，抓不到"呈现汇点根本
    //     没调用它" —— 而这次的失效正是两处 `SDL_SetWindowSize` 把窗口设回 guest 后缓冲尺寸
    //     （1280×720），把 `--scale` 在**第一帧 Present 之后**静默丢掉；只加纯公式测试时
    //     "把调用点改回帧尺寸"仍然全绿。
    //   判据（c-i：窗口跟 guest 帧尺寸、保持倍率）：
    //     ① Fixed 1.5 + submitFrame(1280×720) ⇒ 窗口 **1920×1080**（bug 态 = 1280×720）；
    //     ② 再 submitFrame(640×480)（模拟 guest `SetDisplayMode` 改帧尺寸）⇒ 窗口 **960×720**
    //        （⛔ 不是 1920×1080：那是 c-ii，且会让既有 `present-window` 的"窗口跟随帧尺寸"判据作废）。
    //   ★ 本用例固定 software 渲染器（与 `--gpu` 变体无关）：判据是**窗口几何**，与呈现后端无关。
    {
        ::lift_test::g_current_case = "present-window/scale-window-follows-frame";
        lift::GuestMemory mem;
        lift::SurfaceArena arena;
        LIFT_CHECK(mem.reserve());
        // 8 MiB：1280×720×4 ≈ 3.5 MiB + 640×480×4 ≈ 1.2 MiB（`make()` 的 1 MiB 装不下）
        LIFT_CHECK(arena.init(&mem, 0x10000000u, 0x00800000u));
        const std::uint32_t ea_big = arena.create(1280, 720, format32(0u));
        const std::uint32_t ea_small = arena.create(640, 480, format32(0u));
        lift::GuestSurface* big = arena.find(ea_big);
        lift::GuestSurface* small = arena.find(ea_small);
        LIFT_CHECK_NE_U32(ea_big, 0u);
        LIFT_CHECK_NE_U32(ea_small, 0u);
        LIFT_CHECK(big != nullptr);
        LIFT_CHECK(small != nullptr);

        lift::SdlWindow win;
        lift::SdlWindowOptions opt;
        opt.title = "emulator-lift scale window test";
        opt.driver_list = "software";
        opt.hidden = true;
        opt.scale.kind = lift::ScaleSpec::Kind::Fixed;
        opt.scale.value = 1.5;
        std::string err;
        if (!win.init(opt, &err)) {
            std::fprintf(stderr, "scale 窗口 init 失败（dummy+software 是机械守卫）：%s\n", err.c_str());
            LIFT_CHECK(false);  // ★ 不许 SKIP 掉真实回归（与文件头的 requireWindow 同口径）
            return lift_test::finish("present-window");
        }
        if (big != nullptr) {
            int ww = -1;
            int wh = -1;
            LIFT_CHECK(win.submitFrame(arena, *big, &err));
            SDL_Window* sw = SDL_GetWindowFromID(win.windowIdForTest());
            LIFT_CHECK(sw != nullptr);
            if (sw != nullptr) SDL_GetWindowSize(sw, &ww, &wh);
            std::printf("[scale-window] 帧 1280x720 × 1.5 ⇒ 窗口 %dx%d（期望 1920x1080）\n", ww, wh);
            LIFT_CHECK_EQ_I64(ww, 1920);  // ★ 调用点必须乘 scale（⛔ 帧尺寸本身）
            LIFT_CHECK_EQ_I64(wh, 1080);
        }
        if (small != nullptr) {
            int ww = -1;
            int wh = -1;
            LIFT_CHECK(win.submitFrame(arena, *small, &err));
            SDL_Window* sw = SDL_GetWindowFromID(win.windowIdForTest());
            LIFT_CHECK(sw != nullptr);
            if (sw != nullptr) SDL_GetWindowSize(sw, &ww, &wh);
            std::printf("[scale-window] 帧 640x480 × 1.5 ⇒ 窗口 %dx%d（期望 960x720）\n", ww, wh);
            LIFT_CHECK_EQ_I64(ww, 960);  // c-i：窗口跟帧尺寸、保持倍率
            LIFT_CHECK_EQ_I64(wh, 720);
        }
        win.shutdown();
    }

    // ---- `--scale-filter`：放大滤波器真的接到呈现上（1.5× / 1:1 的确定性对照）----
    //   判据与"为什么游戏截图不行"见 `caseScaleFilter` 上方的注释块。
    {
        std::string skip_why;
        if (!caseScaleFilter(gpu, &skip_why)) LIFT_SKIP(skip_why.c_str());
    }

    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    {
        // ★ 缺字体 / 无 GPU 直出 ⇒ **CTest SKIP 77**（用例无法判定），⛔ 不用"return 0"装 Passed。
        std::string skip_why;
        if (!caseOverlayCanvasComposition(gpu, &skip_why)) LIFT_SKIP(skip_why.c_str());
    }
    caseControlResumeStaleEvents();
    return lift_test::finish("present-window");
}

