// emulator-lift · 真实 SDL 窗口 / GPU 呈现的 CTest
//
// 用法：
//   lift-present-window            → SDL_VIDEODRIVER=dummy + software 渲染器（测窗口/纹理/回读）
//   lift-present-window --gpu      → 默认 GPU 驱动列表；SDL_GetGPURendererDevice()!=NULL 才算通过
//
// 判据：像素在 1:1 下精确相等（含 565、alpha=0、33×17 非紧凑 pitch）；GPU 测试必须有正证据
// （isGpuRenderer()），否则 SKIP 77（⛔ 不拿 software 假绿）。
#include <cstdint>
#include <cstring>
#include <string>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"

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
        win->shutdown();
    }

    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return lift_test::finish("present-window");
}
