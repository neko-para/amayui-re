// emulator-lift · **C2b-2**：窗口装配把插件脚本接进**已有**的叠层 host（`adoptOverlayScript`）
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **同一个实例既跑 Run 又画帧**：`SdlWindow::adoptOverlayScript(*plugin_host->script())` 之后
//      `overlayEnabled()` 为真、命中路由真的装到 `WindowManager` 上（`wm.uiInputRouter() != nullptr`）。
//      红了 ⇒ `--plugin` 在窗口档没有画帧面 / 插件画了按钮但注入点击问不到命中表（静默丢输入）。
//   ② **插件 render 的 display list 真的上屏**：整屏 `ctx.draw.rect` 的 fill 出现在窗口像素里
//      （⛔ 不是 guest 后缓冲色，也不是 hud.js 的底色）。红了 ⇒ render 没被调 / display list 被丢。
//   ③ **FontEngine 进 PluginHost**：`ctx.draw.measure` 在 render 里可用（`PluginHost::setFont` →
//      `PluginScript::setFont` → `ctx.draw.measure`）。夹具第一件事就是 measure，尺寸非正就 throw
//      ⇒ 字体没接线时 render 返回 false、本用例红（⛔ 不是"返回 0 尺寸"）。
//   ④ **`game.mouse().buttons` 的生产读数**：三键电平来自 `WindowManager`（`InputDevice` 的
//      `key_level_` 唯一电平表，⛔ 不新开输入状态源）。夹具按电平画**三种不同颜色**：
//      · `buttons === undefined`（没有源）⇒ `#112233`；
//      · 有源、左键**未按** ⇒ `#CCAA22`；
//      · 有源、左键**按下** ⇒ `#33CC44`。
//      判据 = 默认 `#CCAA22`（有源未按 —— 不是 `#112233`，所以"缺键"会当场红）、
//      `setMouseButton(Left,true,InputSource::Script)` 后 `#33CC44`、抬起后回到 `#CCAA22`。
//      红了 ⇒ 插件看到的按钮电平是编的 / 不跟随真实输入设备 / 一直按着。
//   ⑤ **⛔ 叠层不写 guest**：整轮 `SurfaceArena` 的 guest 像素逐字节不变（硬边界）。
//   ⑥ **release 收口**：`releaseOverlayScript()` 之后 `overlayEnabled()==false`、命中路由摘掉、
//      下一帧窗口像素回到 guest 色（证明"摘掉之后不再画"，而不是只把指针藏起来）。
//
// ★ pure 档（顶层 `add_test`，`AGENTS.md` §12.4 判据 = 注册位置）：不需要游戏安装 / GPU / 真桌面
//   —— `SDL_VIDEODRIVER=dummy` + software 渲染器（与 `tests/overlay.cpp` 的 renderer 路径同一条）。
//   缺宿主字体 / 建不出窗口 ⇒ `LIFT_SKIP(77)`（输入假定在场，⛔ 不假绿）；**adopt / render / 像素
//   不符一律红**（⛔ 不许用 SKIP 吞掉"接线坏了"）。
// ★ 本用例不 spawn 产品 exe ⇒ 不需要 `add_dependencies`。产品侧的窗口装配（aot_main 的
//   `--plugin` → adopt）由 `aot-overlay` 的子进程跑法与 `cli-aot-flags` 的 help 面覆盖。
// ★ 颜色是本文件的**本地常量**（不引生产头）：它们就是夹具 JS 里的三种 fill。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "plugin/plugin_host.h"      // 真 PluginHost（覆盖 setFont 转发）
#include "runtime/font.h"            // FontEngine::fontFileAvailable（窄 SKIP）
#include "runtime/host_window.h"     // WindowManager（唯一输入设备面）
#include "runtime/input_device.h"    // MouseButton / InputSource
#include "runtime/intent_executor.h" // ReadingsSource（PluginHost 的构造形参）
#include "runtime/memory.h"          // GuestMemory
#include "runtime/present_sdl.h"     // SdlWindow（叠层 host）
#include "runtime/surface.h"         // SurfaceArena / GuestSurface
#include "runtime/trigger.h"         // TriggerReadings
#include "ui/script.h"               // UiScript（adopt 的形参类型）

namespace {

// 夹具 JS：判据全在脚本里（宿主只读"render 返回 true"与像素）。⛔ 不放行任何"画不出也算过"的分支。
constexpr const char* kPluginOverlaySource = R"JS(
// C2b-2 夹具 · 窗口装配 + 插件 render。判据（红了意味着什么）见 C++ 文件头。
const COLOR_NO_SOURCE = "#112233";  // buttons 缺键（宿主没有读数源）
const COLOR_UP = "#CCAA22";         // 有源、左键抬起
const COLOR_DOWN = "#33CC44";       // 有源、左键按下

const plugin = {
    render(ctx) {
        // ③ 字体：measure 必须在 render 里可用（宿主字体经 PluginHost::setFont 接线）。
        //    没有字体 ⇒ TypeError（render false）⇒ 本用例红；⛔ 不返回 0 尺寸冒充可用。
        const m = ctx.draw.measure("W", { size: 16 });
        if (!(m.width > 0) || !(m.height > 0)) {
            throw new Error("measure: " + m.width + "x" + m.height);
        }
        // ④ 三键电平：没有源 ⇒ **缺键**（⛔ 不编 false）。
        const mouse = ctx.game.mouse();
        if (mouse === undefined) throw new Error("mouse:undefined");
        let color = COLOR_NO_SOURCE;
        if (mouse.buttons !== undefined) {
            color = mouse.buttons.left ? COLOR_DOWN : COLOR_UP;
        }
        // ② 整屏 rect（1280×720 参考系；窗口 1280×720 ⇒ 画满整个客户区）⇒ 像素判据最省。
        ctx.draw.rect(0, 0, 1280, 720, { fill: color });
    },
};

export default plugin;
)JS";

constexpr std::uint32_t kGuestArgb = 0x00402080u;  // X8R8G8B8：R=40 G=20 B=80
constexpr std::uint32_t kGuestRgb = kGuestArgb & 0x00FFFFFFu;
constexpr std::uint32_t kColorNoSource = 0x112233u;
constexpr std::uint32_t kColorUp = 0xCCAA22u;
constexpr std::uint32_t kColorDown = 0x33CC44u;

// 插件宿主的读数源（本用例不跑 Run ⇒ 只满足构造形参；字段值不是判据）。
class StubReadings final : public lift::plugin::ReadingsSource {
public:
    lift::plugin::TriggerReadings sample(lift::InputPoint point) override {
        lift::plugin::TriggerReadings r;
        r.script = "SC0000";
        r.script_ok = true;
        r.pos = 0x1104;
        r.pos_ok = true;
        r.mouse_ok = true;
        r.point = point;
        return r;
    }
};

// guest 后缓冲（判据 ⑤ 的"⛔ 不写 guest"）。
struct Fx {
    lift::GuestMemory mem;
    lift::SurfaceArena arena;
    std::uint32_t ea = 0;
    lift::GuestSurface* s = nullptr;
};

lift::SurfaceFormat format32() {
    lift::SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = 0;
    return f;
}

bool make(Fx& f, int w, int h, std::uint32_t argb) {
    if (!f.mem.reserve() || !f.arena.init(&f.mem, 0x10000000u, 0x00100000u)) return false;
    f.ea = f.arena.create(w, h, format32());
    f.s = f.arena.find(f.ea);
    if (f.ea == 0 || f.s == nullptr) return false;
    auto* p = f.arena.pixels(*f.s);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::memcpy(p + static_cast<std::size_t>(y) * f.s->pitch + static_cast<std::size_t>(x) * 4, &argb, 4);
        }
    }
    return true;
}

std::vector<std::uint8_t> guestBytes(Fx& f) {
    const auto* p = f.arena.pixels(*f.s);
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(f.s->pitch) * f.s->height);
}

// 取窗口像素（0x00RRGGBB）。
bool windowRgb(lift::SdlWindow& win, int x, int y, std::uint32_t* out, std::string* err) {
    std::uint32_t argb = 0;
    if (!win.readbackPixel(x, y, &argb, err)) return false;
    *out = argb & 0x00FFFFFFu;
    return true;
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---- 窄 SKIP：缺宿主字体 ⇒ 判不了（R5 的输入假定在场）----
    {
        lift::FontEngine probe;
        if (!probe.fontFileAvailable(false)) {
            LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
        }
    }

    Fx f;
    if (!make(f, 640, 360, kGuestArgb)) {
        LIFT_CHECK(false);  // guest 后缓冲建不出来（GuestMemory / SurfaceArena）
        return lift_test::finish("plugin-overlay");
    }
    const std::vector<std::uint8_t> guest_before = guestBytes(f);

    // ★ 对象顺序 = 析构反序：`wm` 最后死（`SdlWindow::shutdown` 会把它身上的 router 摘掉）；
    //   `host`（插件）最先死（叠层 host 只存非拥有指针 ⇒ 生产上 `aot_main` 在 run 收口处 release）。
    lift::WindowManager wm;
    lift::SdlWindow win;
    StubReadings readings;
    std::unique_ptr<lift::plugin::PluginHost> host;

    const std::uint32_t hwnd = wm.createWindow("PluginOverlayTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    if (hwnd == 0) {
        LIFT_CHECK(false);  // createWindow 失败（没有主窗口 ⇒ game.mouse() 一定 undefined）
        return lift_test::finish("plugin-overlay");
    }

    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift plugin-overlay test";
    opt.driver_list = "software";
    opt.hidden = true;
    // ★ 不给 `--overlay`：C2b-2 的 adopt 自己打开叠层（`--plugin` 有 render ⇒ 窗口档就该画）。
    opt.overlay = false;
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        std::string why = "窗口系统 / software 渲染器不可用：" + err;
        LIFT_SKIP(why.c_str());
    }
    win.attachWindowManager(&wm);
    LIFT_CHECK(wm.uiInputRouter() == nullptr);  // 还没有叠层脚本 ⇒ 不装 router
    LIFT_CHECK(!win.overlayEnabled());
    LIFT_CHECK(win.ensureOverlayFont(&err) != nullptr);  // 字体来源（R5；插件的 measure 靠它）

    // ---- 装载插件：字体走**装载之后**的接线（与 aot_main 的窗口装配同一顺序）----
    g_current_case = "plugin-overlay/load";
    host = std::make_unique<lift::plugin::PluginHost>(readings);
    if (!host->init(kPluginOverlaySource, "builtin:c2b2-plugin-overlay", {}, nullptr, &err)) {
        LIFT_CHECK(false);  // PluginHost::init 失败
        std::fprintf(stderr, "  load error: %s\n", err.c_str());
        return lift_test::finish("plugin-overlay");
    }
    LIFT_CHECK(host->script() != nullptr);
    LIFT_CHECK(host->script() != nullptr && host->script()->hasRender());
    lift::FontEngine* const font = win.ensureOverlayFont(&err);
    LIFT_CHECK(font != nullptr);
    if (font != nullptr) host->setFont(font);  // `PluginHost::setFont` → `PluginScript::setFont`

    // ---- ① adopt：接进**已有**的叠层 host（⛔ 不是第二套运行时）----
    g_current_case = "plugin-overlay/adopt";
    if (host->script() == nullptr) return lift_test::finish("plugin-overlay");
    const bool adopted = win.adoptOverlayScript(*host->script(), &err);
    if (!adopted) std::fprintf(stderr, "  adopt error: %s\n", err.c_str());
    LIFT_CHECK(adopted);  // adopt 失败必须是红（⛔ 不 SKIP）
    LIFT_CHECK(win.overlayEnabled());
    LIFT_CHECK(wm.uiInputRouter() != nullptr);  // 插件画的按钮能进命中表（注入点击会问它）

    // ---- ②③④ 像素判据：插件 render 的 display list 真的上屏；三键电平来自真实输入设备 ----
    // 有源、左键**未按** ⇒ `#CCAA22`（⛔ 不是 `#112233` 那张"没有源"的色 —— 缺键会当场红）。
    g_current_case = "plugin-overlay/render-up";
    LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
    std::uint32_t rgb = 0;
    LIFT_CHECK(windowRgb(win, 0, 0, &rgb, &err));
    std::printf("plugin-overlay/render-up: (0,0)=#%06X want #%06X\n", rgb, kColorUp);
    LIFT_CHECK_EQ_U32(rgb, kColorUp);
    // ★ 窗口尺寸 = **帧尺寸 × `--scale`**（`window_scale.h`；本用例 scale 缺省 1.0 ⇒ 窗口 = 640×360）。
    //   `(frame_w-1, frame_h-1)` 是窗口右下角像素 ⇒ 整屏 rect（1280×720 参考系）必须也盖住它
    //   （红了 = display list 只画了一角 / 被裁剪）。
    LIFT_CHECK(windowRgb(win, f.s->width - 1, f.s->height - 1, &rgb, &err));
    LIFT_CHECK_EQ_U32(rgb, kColorUp);
    LIFT_CHECK(rgb != kColorNoSource);  // 有区分力：缺键那张色与"未按"必须不同

    // 真按：写的是**已有输入设备**（`WindowManager::setMouseButton` → `InputDevice` 的电平表）。
    g_current_case = "plugin-overlay/render-down";
    const lift::WriteResult press = wm.setMouseButton(lift::MouseButton::Left, true, lift::InputSource::Script);
    LIFT_CHECK(press.accepted);
    LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
    LIFT_CHECK(windowRgb(win, 0, 0, &rgb, &err));
    std::printf("plugin-overlay/render-down: (0,0)=#%06X want #%06X\n", rgb, kColorDown);
    LIFT_CHECK_EQ_U32(rgb, kColorDown);

    // 抬起 ⇒ 回到"有源、未按"（电平是读数，不是 latch）。
    g_current_case = "plugin-overlay/render-up-again";
    const lift::WriteResult release = wm.setMouseButton(lift::MouseButton::Left, false, lift::InputSource::Script);
    LIFT_CHECK(release.accepted);
    LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
    LIFT_CHECK(windowRgb(win, 0, 0, &rgb, &err));
    LIFT_CHECK_EQ_U32(rgb, kColorUp);

    // ⑤ 叠层只改**窗口像素**：guest 后缓冲逐字节不变（硬边界：⛔ 不写 guest）。
    g_current_case = "plugin-overlay/guest-untouched";
    LIFT_CHECK(guestBytes(f) == guest_before);

    // ---- ⑥ release：摘掉 adopted ⇒ 不再画、router 摘掉、像素回 guest 色 ----
    g_current_case = "plugin-overlay/release";
    win.releaseOverlayScript();
    LIFT_CHECK(!win.overlayEnabled());
    LIFT_CHECK(wm.uiInputRouter() == nullptr);
    LIFT_CHECK(win.submitFrame(f.arena, *f.s, &err));
    LIFT_CHECK(windowRgb(win, 0, 0, &rgb, &err));
    std::printf("plugin-overlay/release: (0,0)=#%06X want guest #%06X\n", rgb, kGuestRgb);
    LIFT_CHECK_EQ_U32(rgb, kGuestRgb);
    LIFT_CHECK(guestBytes(f) == guest_before);

    // 计数读数（不是断言）：让 `ctest -V` 直接看到这一轮的 checks 数。
    std::printf("plugin-overlay: adopted=%d overlay=%d checks-so-far=%d\n", adopted ? 1 : 0,
                win.overlayEnabled() ? 1 : 0, g_checks);
    win.shutdown();
    return lift_test::finish("plugin-overlay");
}
