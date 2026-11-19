// emulator-lift · 窗口调试叠层（`--overlay` / `--ui`）的守卫
//
// 判据（红了意味着什么，缺一条都不算）：
//   ① **3 s 窗口**：`uiWorstPush` / `uiWorstMax` 是纯函数 ⇒ 用**假时间戳**测：
//      窗口外的旧样本必须被丢、窗口内取最大、空窗口 ⇒ `has_worst=false`（⛔ 不编 0）；
//      时间倒退的样本丢弃；边界（恰好 3 s）仍在窗口内。
//   ② **身份载荷**：`uiScriptPayload` 的降级形态 = `id:0x%08X（降级）`（⛔ 不猜名字）。
//   ③ **renderer 路径**（`--present window`、SDL_Renderer 上传）：`SdlWindow` 用 software 渲染器
//      提交一帧 ⇒ 窗口像素的**画布原点**（16:9 帧 ⇒ == 画面原点）出现 HUD（深底 + 亮字），
//      矩形外仍是 guest 后缓冲的颜色；关掉 `setOverlay(false)` ⇒ 恢复成纯画面（逐像素）；
//      两种情况下 **guest 像素逐字节不变**。
//   ④ **直出路径**（`--present window`）：claim 成功 ⇒ 同一组断言在 GPU 直出上成立
//      （窗口不可 claim / 没有 GPU / 缺宿主字体 ⇒ SKIP 77 + 实测原因，⛔ 不拿 renderer 假绿）。
//   ④b **直出 letterbox / 画布原点**（B2 回归守卫）：把窗口强制成 1000×1000（target 320×240 ⇒
//      画面居中、上下有黑边）⇒ 叠层 blt 的 dst 原点必须 == **画布原点**
//      （画面原点 + `uiCanvasOrigin` 的居中偏移 `(0,93)` ⇒ `(0,218)`），窗口 (0,0) 是黑边。
//      ⛔ 不是"窗口 (0,0)"，⛔ 也不是"画面矩形原点"；⛔ 不依赖真实鼠标 / 真实分辨率。
//   ⑤ **尺寸 churn**（`overlay/size-churn`，renderer 与 `--gpu` 都跑）：`worst` 在
//      `wall=9.9` / `wall=100.0` 之间交替 + `frame` 每帧变 ⇒ HUD 面板宽每几帧就变一次；
//      连提 **200 帧**，每 10 帧回读**画布原点** = 期望 blend。红了 = 尺寸变化后叠层丢了 /
//      错位了（叠层纹理按尺寸销毁重建 ⇒ 在飞的 draw 源被提前释放的回归）。
//      ★ 这条**不**断言具体面板尺寸（`fps` 是本机读数）；只钉"原点必须还是那块混合底"。
//
// ★ B（2026-11-18）：HUD 的**内容**由外置脚本 `scripts/ui/hud.js` 产出（QuickJS 同步跑），
//   C++ 侧只剩"读数 → 脚本 → 栅格化 → `presentOverlay`"。⇒ **期望面板**只能由**同一个脚本 +
//   `FontEngine`** 跑一遍得到：取它产出的第一条 Rect 的 w/h（⛔ 不硬编码一个面板尺寸 ——
//   那会放过"脚本版式变了"的回归）。
// ★ 颜色：`#101820` / `#EBFFEB` 是 `hud.js` 里的 `BG` / `FG`，这里用**本地常量**（测试不引生产头）。
// ★ 本文件的断言只写**不变量**（行数 / 矩形边界 / 颜色精确值），⛔ 不断言 fps 或帧号
//   的具体数值（那是读数，不是契约）。★ 缺宿主字体 ⇒ SKIP 77。
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/host_encoding.h"
#include "runtime/memory.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"
#include "runtime/ui_readings.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 脚本路径）
#include "ui/canvas.h"
#include "ui/element.h"
#include "ui/raster.h"
#include "ui/script.h"

#include <SDL3/SDL.h>

namespace {

// HUD 的两种颜色（与 `scripts/ui/hud.js` 的 `BG` / `FG` 一致；⛔ 不从生产头里取 —— 生产侧已经
// 没有这两个常量了，脚本才是版式/颜色的真源）。
// ★ 底是 `#101820CC`（alpha 204 ⇒ **80% 不透明**）：renderer 与直出**两条路径都按 alpha 与画面混合**
//   （直出 = 一条 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 的专用混合管线；D 单之前那一轮是拷贝 blit ⇒ 不透明）。
constexpr std::uint32_t kOverlayBgRgb = 0x101820u;  // #101820（RGB；alpha 见下）
constexpr int kOverlayBgAlpha = 204;                // #101820CC
constexpr std::uint8_t kOverlayFg[4] = {235, 255, 235, 255};  // #EBFFEB（alpha 255：字不变淡）

// ★ 半透明底的**唯一**一份整数混合公式（与 `d3d9_gpu.cpp` 的 CPU 混合同源）：
//     d = (s*a + d*(255-a) + 127) / 255
//   它只用在**期望值**这一侧：`kOverlayBgAlpha` = 204 时"底 80% + 画面 20%"。
constexpr std::uint32_t blendChannel(std::uint32_t s, std::uint32_t d, int a) {
    return (s * static_cast<std::uint32_t>(a) + d * static_cast<std::uint32_t>(255 - a) + 127u) / 255u;
}

constexpr std::uint32_t blendOver(std::uint32_t src_rgb, std::uint32_t dst_rgb, int a) {
    return (blendChannel((src_rgb >> 16) & 0xFFu, (dst_rgb >> 16) & 0xFFu, a) << 16) |
           (blendChannel((src_rgb >> 8) & 0xFFu, (dst_rgb >> 8) & 0xFFu, a) << 8) |
           blendChannel(src_rgb & 0xFFu, dst_rgb & 0xFFu, a);
}

// 两个 RGB 是否在 ±`tol` 内相等（逐通道）。★ GPU 的 alpha blend 在浮点上算，与本文件的整数公式
// 可能差 1 ⇒ 混合那一侧给 1 的容差（字 alpha = 255，直通 ⇒ 仍然**精确**比较）。
constexpr bool nearRgb(std::uint32_t got, std::uint32_t want, int tol) {
    for (int shift = 0; shift <= 16; shift += 8) {
        const long a = static_cast<long>((got >> shift) & 0xFFu);
        const long b = static_cast<long>((want >> shift) & 0xFFu);
        if (a - b > tol || b - a > tol) return false;
    }
    return true;
}

// =======================================================================================
// ① 最近 3 s 窗口：纯函数 + **假时间戳**（⛔ 不依赖真实时钟 / 真实帧）
// =======================================================================================
std::chrono::steady_clock::time_point fakeAt(std::int64_t ms) {
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ms);
}

lift::UiFrameSample sampleAt(std::int64_t ms, double wall, double guest, double shim) {
    lift::UiFrameSample s;
    s.at = fakeAt(ms);
    s.wall_ms = wall;
    s.guest_ms = guest;
    s.shim_ms = shim;
    return s;
}

void caseWorstWindow() {
    lift_test::g_current_case = "overlay/worst-window";

    // 空窗口 ⇒ has_worst=false（⛔ 不编 0 的一行）
    const lift::UiWorst empty = lift::uiWorstMax({});
    LIFT_CHECK(!empty.has_worst);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(empty.wall_max_ms * 10.0), 0);

    std::deque<lift::UiFrameSample> q;
    lift::uiWorstPush(q, sampleAt(0, 100.0, 1.0, 2.0));
    lift::uiWorstPush(q, sampleAt(1000, 50.0, 3.0, 9.0));
    lift::uiWorstPush(q, sampleAt(2500, 75.0, 2.0, 1.0));
    // 窗口 = 最新样本往前 3 s ⇒ 这里 3 个都还在
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 3);
    lift::UiWorst w = lift::uiWorstMax(q);
    LIFT_CHECK(w.has_worst);
    LIFT_CHECK(w.wall_max_ms == 100.0);
    LIFT_CHECK(w.guest_max_ms == 3.0);
    LIFT_CHECK(w.shim_max_ms == 9.0);

    // ★ 推一个 3500 的样本 ⇒ cutoff = 500 ⇒ t=0 的旧样本**必须被丢**
    //   （把窗口改成 30 s ⇒ 这一条当场红：旧样本还在 ⇒ size=4 / wall_max 仍 100）。
    lift::uiWorstPush(q, sampleAt(3500, 10.0, 0.5, 0.5));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 3);
    LIFT_CHECK(q.front().at == fakeAt(1000));
    w = lift::uiWorstMax(q);
    LIFT_CHECK(w.wall_max_ms == 75.0);
    LIFT_CHECK(w.guest_max_ms == 3.0);
    LIFT_CHECK(w.shim_max_ms == 9.0);

    // 空窗之后的第一个样本：窗口内**只剩它**（⛔ 不把上一轮的读数带过来）
    lift::uiWorstPush(q, sampleAt(10000, 12.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 1);
    w = lift::uiWorstMax(q);
    LIFT_CHECK(w.has_worst);
    LIFT_CHECK(w.wall_max_ms == 12.0);
    LIFT_CHECK(w.guest_max_ms == 0.0);
    LIFT_CHECK(w.shim_max_ms == 0.0);

    // 时间倒退的样本直接丢（窗口口径只认"最新样本"；⛔ 不排序 / 不猜）
    lift::uiWorstPush(q, sampleAt(9000, 999.0, 999.0, 999.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(q.size()), 1);
    w = lift::uiWorstMax(q);
    LIFT_CHECK(w.wall_max_ms == 12.0);

    // 边界：恰好 3 s 差的样本仍在窗口内（窗口 = [at-3s, at]），再旧 1 ms 就丢
    std::deque<lift::UiFrameSample> edge;
    lift::uiWorstPush(edge, sampleAt(0, 5.0, 0.0, 0.0));
    lift::uiWorstPush(edge, sampleAt(3000, 6.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(edge.size()), 2);
    lift::uiWorstPush(edge, sampleAt(3001, 7.0, 0.0, 0.0));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(edge.size()), 2);  // t=0 被丢，剩 3000 / 3001
    LIFT_CHECK(edge.front().at == fakeAt(3000));
}

// =======================================================================================
// ② 身份载荷（降级形态不依赖名字表）
// =======================================================================================
void caseScriptPayload() {
    lift_test::g_current_case = "overlay/script-payload";
    LIFT_CHECK(lift::uiScriptPayload("startup", false, 0x0000002Au) == "startup");
    // 名字为空但**没**降级 ⇒ 原样（空串）：是不是要省略这一行由消费者（脚本）判空串。
    LIFT_CHECK(lift::uiScriptPayload("", false, 0x0000002Au).empty());
    LIFT_CHECK(lift::uiScriptPayload("", true, 0x0000002Au) == "id:0x0000002A（降级）");
    // 降级时**不用**传进来的名字（⛔ 不猜）
    LIFT_CHECK(lift::uiScriptPayload("startup", true, 0x0000002Au) == "id:0x0000002A（降级）");
    LIFT_CHECK(lift::uiScriptPayload("startup", true, 0xDEADBEEFu) == "id:0xDEADBEEF（降级）");
}

// =======================================================================================
// 窗口路径夹具（③ renderer / ④ 直出）
// =======================================================================================
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

constexpr std::uint32_t kGuestArgb = 0x008040C0u;  // R=80 G=40 B=C0（X8R8G8B8）
constexpr std::uint32_t kGuestRgb = kGuestArgb & 0x00FFFFFFu;

// 扫窗口像素里叠层覆盖的那块区域（起点 = **画布原点**）；返回统计。⛔ 不依赖文字内容的具体
// 长度 —— 这里只问"深底占了这块、亮字出现了、矩形外还是 guest 色"。
struct TopLeftScan {
    long bg = 0;
    long fg = 0;
    long guest = 0;
    long other = 0;
};

// `want_bg` / `bg_tol`：这一条路径的**期望底色**（两条路径都是 80% 混合值 ±1）。
bool scanRegion(lift::SdlWindow& win, int x0, int y0, int w, int h, std::uint32_t want_bg, int bg_tol,
                TopLeftScan* out, std::string* err) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            std::uint32_t argb = 0;
            if (!win.readbackPixel(x, y, &argb, err)) return false;
            const std::uint32_t rgb = argb & 0x00FFFFFFu;
            if (nearRgb(rgb, want_bg, bg_tol)) {
                ++out->bg;
            } else if (rgb == (static_cast<std::uint32_t>(kOverlayFg[0]) << 16 |
                               static_cast<std::uint32_t>(kOverlayFg[1]) << 8 | kOverlayFg[2])) {
                ++out->fg;
            } else if (rgb == kGuestRgb) {
                ++out->guest;
            } else {
                ++out->other;
            }
        }
    }
    return true;
}

// 脚本画出的一个矩形（窗口像素；画布原点 + `uiCanvasOrigin` 的 scale 之后）。
struct PixelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 所有"叠层矩形"之外必须**逐像素**还是 guest 色（叠层"只占脚本画出的那几块"，⛔ 不遮别的区域）。
// ★ 切片 3 起叠层不止一个矩形（面板 + 面板下方的 PROFILE 按钮）⇒ 覆盖集 = **每个 Rect 的并集**，
//   ⛔ 不是它们的包围盒（面板与按钮之间那条 PAD 缝里仍是 guest 画面，包围盒会把它当成合法覆盖）。
// ★ `covered` = `expectedPanel()` 用**生产脚本**跑出来的实际矩形（⛔ 不要拍脑袋给尺寸 ——
//   那可能比实际叠层更大，等于放过"叠层画到矩形外"的回归）。
void expectGuestUntouched(lift::SdlWindow& win, const lift::GuestSurface& s,
                          const std::vector<PixelRect>& covered) {
    std::string err;
    for (int y = 0; y < s.height; y += 7) {
        for (int x = 0; x < s.width; x += 11) {
            std::uint32_t argb = 0;
            if (!win.readbackPixel(x, y, &argb, &err)) {
                LIFT_CHECK(false);
                return;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) {
                // 叠层实际矩形内会被覆盖 ⇒ 那里不要求 guest 色
                bool inside = false;
                for (const PixelRect& r : covered) {
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                        inside = true;
                        break;
                    }
                }
                if (inside) continue;
                lift_test::failf(__FILE__, __LINE__, "(%d,%d) 被遮住了：expected %06X, got %06X", x, y, kGuestRgb,
                                 argb & 0xFFFFFFu);
                ++lift_test::g_checks;
                continue;
            }
            ++lift_test::g_checks;
        }
    }
}

// 生产回退的同一份脚本路径（CMake 注入 `LIFT_UI_SCRIPT_DIR`；`SdlWindow` 的 `loadUiScript` 用它）。
std::string hudScriptPath() { return std::string(LIFT_UI_SCRIPT_DIR) + "/hud.js"; }

bool readAll(const std::string& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path, "rb");
    if (f == nullptr) return false;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

// 第一帧的叠层矩形，**用生产脚本跑一遍**算出来（⛔ 不猜宽度；面板尺寸由脚本 + 宿主字体决定）。
// ★ 第一帧的读数（与 `SdlWindow::drawOverlay` 的取值同源）：frame = 1（Present 计数），
//   fps = 1.0（最近 1 s 只有这一次 Present），没有 mouse（测试不挂 `WindowManager`）、
//   没有 script / worst（`Machine` 没跑）。
// ★ `frame_w/h` = 被呈现的帧尺寸（画布 `s` 从 `uiCanvasOrigin` 取）。
// ★ `covered` = 脚本画出的**每一个 Rect**（切片 3 起 = 面板 + PROFILE 按钮；`expectGuestUntouched`
//   按它判"叠层只占画出的那几块"）。面板本身仍是 `elements[0]`（`w/h` 只回它）。
bool expectedPanel(lift::FontEngine& font, int frame_w, int frame_h, int* ox, int* oy, int* w, int* h,
                   std::vector<PixelRect>* covered) {
    std::string source;
    if (!readAll(hudScriptPath(), &source)) return false;
    auto runtime = lift::ui::makeQuickJsRuntime();
    if (runtime == nullptr) return false;
    std::string err;
    std::unique_ptr<lift::ui::UiScript> script = runtime->load(source, hudScriptPath(), &font, &err);
    if (script == nullptr) return false;
    lift::ui::UiFrameReadings readings;
    readings.frame = 1;
    readings.fps_ok = true;
    readings.fps = 1.0;
    lift::ui::UiBuilder builder;
    if (!script->render(builder, readings, &err)) return false;
    if (builder.elements().empty()) return false;
    const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(frame_w, frame_h);
    const lift::ui::Element& panel = builder.elements().front();
    if (panel.kind != lift::ui::ElementKind::Rect) return false;
    *ox = canvas.offset_x;  // 画面原点 = (0,0)（窗口 = 帧尺寸）
    *oy = canvas.offset_y;
    *w = static_cast<int>(static_cast<long long>(panel.w) * canvas.scale + 0.5);
    *h = static_cast<int>(static_cast<long long>(panel.h) * canvas.scale + 0.5);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    covered->clear();
    for (const lift::ui::Element& e : builder.elements()) {
        if (e.kind != lift::ui::ElementKind::Rect) continue;
        PixelRect r;
        r.x = canvas.offset_x + static_cast<int>(static_cast<long long>(e.x) * canvas.scale + 0.5);
        r.y = canvas.offset_y + static_cast<int>(static_cast<long long>(e.y) * canvas.scale + 0.5);
        r.w = static_cast<int>(static_cast<long long>(e.w) * canvas.scale + 0.5);
        r.h = static_cast<int>(static_cast<long long>(e.h) * canvas.scale + 0.5);
        if (r.w < 1) r.w = 1;
        if (r.h < 1) r.h = 1;
        covered->push_back(r);
    }
    return !covered->empty();
}

std::vector<std::uint8_t> guestBytes(Fx& f) {
    const auto* p = f.arena.pixels(*f.s);
    return std::vector<std::uint8_t>(p, p + static_cast<std::size_t>(f.s->pitch) * f.s->height);
}

// ③ renderer 路径：software 渲染器（CTest 里配 SDL_VIDEODRIVER=dummy）。
// 返回 false **只**表示环境不可用（没有窗口系统 / 缺宿主字体）；真实回归用 LIFT_CHECK 记红。
bool caseRendererWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/renderer-window";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）";
        return false;
    }
    // ★ 16:9 帧 ⇒ 画布原点 == 画面矩形原点（B2 的第一条用例）。
    Fx f;
    if (!make(f, 640, 360, kGuestArgb)) {
        LIFT_CHECK(false);
        return true;
    }
    const std::vector<std::uint8_t> before = guestBytes(f);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift overlay test (renderer)";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.overlay = true;  // ui_script 空 ⇒ 走开发回退（`LIFT_UI_SCRIPT_DIR/hud.js`）
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip_why = "窗口系统 / HUD 脚本不可用：" + err;
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());
    int ox = 0;
    int oy = 0;
    int rw = 0;
    int rh = 0;
    std::vector<PixelRect> covered;
    if (!expectedPanel(font, 640, 360, &ox, &oy, &rw, &rh, &covered)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    LIFT_CHECK_EQ_I64(ox, 0);  // 16:9 ⇒ 居中偏移 0
    LIFT_CHECK_EQ_I64(oy, 0);
    // ★ renderer 路径：底是 `#101820CC`（alpha 204）⇒ 它**与底下的 guest 画面混合**（SDL_BLENDMODE_BLEND）。
    //   期望 = 混合值；⛔ 不是 `#101820` —— 那个值意味着"半透明没生效"。
    //   ★ 混合是 GPU 的浮点运算，与本文件的整数公式可能差 1 ⇒ 容差 1（不透明侧仍精确）。
    const std::uint32_t want_bg = blendOver(kOverlayBgRgb, kGuestRgb, kOverlayBgAlpha);
    LIFT_CHECK(want_bg != kOverlayBgRgb);  // 有区分力：204 ≠ 255 ⇒ 混合值必须不同于不透明值
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanRegion(win, ox, oy, rw, rh, want_bg, 1, &scan, &err)) LIFT_CHECK(false);
    // 深底（80% 混合值）+ 亮字真的出现（扫描范围 = 生产脚本算出的面板矩形）
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    LIFT_CHECK(scan.guest == 0);
    // 画布原点那一个像素是**混合后的底**（不是 guest / 不是黑 / 也不是不透明的 #101820）
    // —— B2 的"原点 == 画布原点"像素级证据 + 80% 半透明的像素级证据。
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(ox, oy, &argb, &err));
    // ★ 读数（不是断言）：`ctest -V` 直接看到 renderer 这一路的 (0,0) 实际值与期望值。
    std::printf("overlay/renderer-window: canvas_origin=(%d,%d) 像素=#%06X 期望 #%06X（80%% 混合，tol=1）\n", ox, oy,
                argb & 0x00FFFFFFu, want_bg);
    LIFT_CHECK(nearRgb(argb & 0x00FFFFFFu, want_bg, 1));
    LIFT_CHECK(!nearRgb(argb & 0x00FFFFFFu, kOverlayBgRgb, 1));
    expectGuestUntouched(win, *f.s, covered);
    LIFT_CHECK(guestBytes(f) == before);  // ⛔ 叠层不写 guest 像素

    // 关掉开关 ⇒ 整窗**逐像素**恢复成纯画面（证明"默认关 / 可关"，不是一直在画）
    win.setOverlay(false);
    LIFT_CHECK(!win.overlayEnabled());
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    long not_guest = 0;
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            if (!win.readbackPixel(x, y, &argb, &err)) {
                ++not_guest;
                continue;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) ++not_guest;
        }
    }
    LIFT_CHECK_EQ_I64(not_guest, 0);
    LIFT_CHECK(guestBytes(f) == before);
    win.shutdown();
    return true;
}

// ④ 直出路径：真 GPU claim（没有 GPU / claim 不成立 ⇒ SKIP，⛔ 不拿 renderer 假绿）。
// ★ 帧用 **4:3（320×240）**：画布纵向居中偏移 30 px ⇒ 同时钉住 B2 的非 16:9 分支。
bool caseDirectWindow(std::string* skip_why) {
    lift_test::g_current_case = "overlay/direct-window";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）";
        return false;
    }
    Fx f;
    if (!make(f, 320, 240, kGuestArgb)) {
        LIFT_CHECK(false);
        return true;
    }
    const std::vector<std::uint8_t> before = guestBytes(f);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift overlay test (direct)";
    opt.hidden = true;
    opt.raster_gpu = true;
    opt.overlay = true;  // ui_script 空 ⇒ 走开发回退（`LIFT_UI_SCRIPT_DIR/hud.js`）
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip_why = "GPU / 窗口 / HUD 脚本不可用：" + err;
        return false;
    }
    if (!win.directPresentActive()) {
        *skip_why = std::string("GPU 直出未成立：") + win.describe();
        win.shutdown();
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());
    int ox = 0;
    int oy = 0;
    int rw = 0;
    int rh = 0;
    std::vector<PixelRect> covered;
    if (!expectedPanel(font, 320, 240, &ox, &oy, &rw, &rh, &covered)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    LIFT_CHECK_EQ_I64(ox, 0);
    LIFT_CHECK_EQ_I64(oy, 30);  // 4:3 ⇒ 画布纵向居中 30 px（B2）
    // ★ **两条路径都是 80% 混合**（D 单）：renderer 走 `SDL_BLENDMODE_BLEND`；直出走一条
    //   `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 的专用混合管线（⛔ 不再是拷贝 blit）。
    //   期望 = 混合值；⛔ 不是 `#101820` —— 那个值意味着"半透明没生效"。
    //   ★ 混合是 GPU 的浮点运算，与本文件的整数公式可能差 1 ⇒ 容差 1（字仍是精确的 255）。
    const std::uint32_t want_bg = blendOver(kOverlayBgRgb, kGuestRgb, kOverlayBgAlpha);
    LIFT_CHECK(want_bg != kOverlayBgRgb);  // 有区分力：204 ≠ 255 ⇒ 混合值必须不同于不透明值
    if (!win.submitFrame(f.arena, *f.s, &err)) {
        LIFT_CHECK(false);
        win.shutdown();
        return true;
    }
    TopLeftScan scan;
    if (!scanRegion(win, ox, oy, rw, rh, want_bg, 1, &scan, &err)) LIFT_CHECK(false);
    LIFT_CHECK(scan.bg > 0);
    LIFT_CHECK(scan.fg > 0);
    LIFT_CHECK(scan.other == 0);
    LIFT_CHECK(scan.guest == 0);
    std::uint32_t argb = 0;
    LIFT_CHECK(win.readbackPixel(ox, oy, &argb, &err));
    // ★ 直出这一条读的是 `directReadback` 的**同一份**窗口像素（截图 / 窗口像素与屏幕同值）：
    //   它按与 GPU 混合管线同一条整数公式合成 ⇒ 混合值（容差 1），⛔ 不再是不透明 `#101820`。
    // ★ 读数（不是断言）：`ctest -V` 直接看到直出这一路的 (0,0) 实际值与期望值。
    std::printf("overlay/direct-window: canvas_origin=(%d,%d) 像素=#%06X 期望 #%06X（80%% 混合，tol=1）\n", ox, oy,
                argb & 0x00FFFFFFu, want_bg);
    LIFT_CHECK(nearRgb(argb & 0x00FFFFFFu, want_bg, 1));
    LIFT_CHECK(!nearRgb(argb & 0x00FFFFFFu, kOverlayBgRgb, 1));
    // 画布原点上方（画面原点那一块）仍是 guest 色 ⇒ 证明原点不是恒 0、也不是画面原点。
    LIFT_CHECK(win.readbackPixel(0, 0, &argb, &err));
    LIFT_CHECK_EQ_U32(argb & 0x00FFFFFFu, kGuestRgb);
    expectGuestUntouched(win, *f.s, covered);
    LIFT_CHECK(guestBytes(f) == before);

    win.setOverlay(false);
    if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
    long not_guest = 0;
    for (int y = 0; y < f.s->height; ++y) {
        for (int x = 0; x < f.s->width; ++x) {
            if (!win.readbackPixel(x, y, &argb, &err)) {
                ++not_guest;
                continue;
            }
            if ((argb & 0x00FFFFFFu) != kGuestRgb) ++not_guest;
        }
    }
    LIFT_CHECK_EQ_I64(not_guest, 0);
    LIFT_CHECK(guestBytes(f) == before);

    // ---- B2 回归守卫：窗口 > 画面 ⇒ letterbox；叠层必须锚在**画布原点** ----
    // ★ 用 SDL 自己的窗口 API 强制一个与 target 不同尺寸的窗口（test-only，不改生产路径；
    //   `submitDirect` 只在 **surface 尺寸变化** 时跟随窗口 ⇒ 这里窗口会保持 1000×1000）。
    win.setOverlay(true);
    LIFT_CHECK(win.overlayEnabled());
    SDL_Window* sdl_window = SDL_GetWindowFromID(win.windowIdForTest());
    LIFT_CHECK(sdl_window != nullptr);
    if (sdl_window != nullptr) {
        LIFT_CHECK(SDL_SetWindowSize(sdl_window, 1000, 1000));
        SDL_SyncWindow(sdl_window);
        // 连提两帧：第一帧给 swapchain 跟上 resize 的机会，判据只看第二帧。
        if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
        if (!win.submitFrame(f.arena, *f.s, &err)) LIFT_CHECK(false);
        int pw = 0;
        int ph = 0;
        SDL_GetWindowSizeInPixels(sdl_window, &pw, &ph);
        LIFT_CHECK(pw > 320 && ph > 240);
        // ① 画面矩形（letterbox 320×240 → 窗口像素 pw×ph）独立算一遍。
        const double sx = static_cast<double>(pw) / 320.0;
        const double sy = static_cast<double>(ph) / 240.0;
        const double scale = sx < sy ? sx : sy;
        const int dw = static_cast<int>(std::lround(320.0 * scale));
        const int dh = static_cast<int>(std::lround(240.0 * scale));
        const int ex = (pw - dw) / 2;
        const int ey = (ph - dh) / 2;
        LIFT_CHECK(ex > 0 || ey > 0);  // 真的出现黑边（否则这条断言没有区分力）
        // ② 画布原点 = 画面原点 + `uiCanvasOrigin(画面宽,画面高)` 的居中偏移（**唯一一份**算术）。
        const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(dw, dh);
        const int want_x = ex + canvas.offset_x;
        const int want_y = ey + canvas.offset_y;
        LIFT_CHECK(want_y > ey);  // 4:3 ⇒ 画布原点真的低于画面原点（否则区分不了两个口径）
        lift::GpuDirectPresenter* dp = win.directPresenter();
        LIFT_CHECK(dp != nullptr);
        int got_x = -1;
        int got_y = -1;
        LIFT_CHECK(dp != nullptr && dp->directLastOverlayOrigin(&got_x, &got_y));
        std::printf("letterbox: window=%dx%d picture=(%d,%d %dx%d) canvas_origin=(%d,%d) overlay_origin=(%d,%d)\n",
                    pw, ph, ex, ey, dw, dh, want_x, want_y, got_x, got_y);
        LIFT_CHECK_EQ_I64(got_x, want_x);
        LIFT_CHECK_EQ_I64(got_y, want_y);
        // ③ 像素级证据：叠层底色在**画布原点**；画面原点处仍是 guest 色；窗口原点 (0,0) 是黑边。
        std::uint32_t px = 0;
        LIFT_CHECK(win.readbackPixel(want_x, want_y, &px, &err));
        LIFT_CHECK(nearRgb(px & 0x00FFFFFFu, want_bg, 1));  // D 单：直出也按 80% 混合落地
        LIFT_CHECK(win.readbackPixel(ex, ey, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, kGuestRgb);
        LIFT_CHECK(win.readbackPixel(0, 0, &px, &err));
        LIFT_CHECK_EQ_U32(px & 0x00FFFFFFu, 0u);
        LIFT_CHECK(guestBytes(f) == before);
    }
    win.shutdown();
    return true;
}

// ⑤ 尺寸 churn（renderer 与直出共用）：`worst` 交替 + `frame` 变化 ⇒ 面板宽度每几帧变一次。
// ★ 期望值 = **画布原点那一个像素**的混合底（与 ③④ 同一条整数公式）；⛔ 不猜面板尺寸，
//   也⛔ 不断言 fps（那是本机读数）—— 只问"尺寸变过之后原点还是不是那块 80% 混合底"。
bool caseSizeChurn(bool gpu, std::string* skip_why) {
    lift_test::g_current_case = "overlay/size-churn";
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        *skip_why = "缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）";
        return false;
    }
    const int frame_w = gpu ? 320 : 640;
    const int frame_h = gpu ? 240 : 360;
    Fx f;
    if (!make(f, frame_w, frame_h, kGuestArgb)) {
        LIFT_CHECK(false);
        return true;
    }
    const std::vector<std::uint8_t> before = guestBytes(f);

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = gpu ? "emulator-lift overlay churn (direct)" : "emulator-lift overlay churn (renderer)";
    if (!gpu) opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = gpu;
    opt.overlay = true;  // ui_script 空 ⇒ 走开发回退（`LIFT_UI_SCRIPT_DIR/hud.js`）
    win.setCaptureForTest(true);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip_why = "窗口系统 / HUD 脚本不可用：" + err;
        return false;
    }
    if (gpu && !win.directPresentActive()) {
        *skip_why = std::string("GPU 直出未成立：") + win.describe();
        win.shutdown();
        return false;
    }
    LIFT_CHECK(win.overlayEnabled());

    // 画布原点：窗口 == 帧尺寸 ⇒ 画面原点 (0,0) + `uiCanvasOrigin` 的居中偏移（与 ③④ 同一份算术）。
    const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(frame_w, frame_h);
    const std::uint32_t want_bg = blendOver(kOverlayBgRgb, kGuestRgb, kOverlayBgAlpha);
    LIFT_CHECK(want_bg != kOverlayBgRgb);  // 有区分力：与"半透明没生效"的不透明值不同
    int sampled = 0;
    int wrong = 0;
    std::uint32_t first = 0;
    for (int i = 1; i <= 200; ++i) {
        lift::UiReadings readings;
        readings.has_worst = true;
        // 面板宽变：偶数帧 `wall=100.0ms`、奇数帧 `9.9ms`；`frame` 本身每帧都变（位数 1→2→3）。
        readings.wall_max_ms = (i % 2 == 0) ? 100.0 : 9.9;
        readings.shim_max_ms = (i % 3 == 0) ? 10.0 : 1.0;
        // `guest=--`（4 个字符）与 `guest=3.3ms`（5 个字符）交替 ⇒ 再变一次宽度。
        readings.has_guest = (i % 5 == 0);
        readings.guest_max_ms = 3.3;
        win.setUiReadings(readings);
        if (!win.submitFrame(f.arena, *f.s, &err)) {
            LIFT_CHECK(false);
            break;
        }
        if (i % 10 != 0) continue;
        std::uint32_t argb = 0;
        if (!win.readbackPixel(canvas.offset_x, canvas.offset_y, &argb, &err)) {
            LIFT_CHECK(false);
            continue;
        }
        ++sampled;
        if (i == 10) first = argb & 0x00FFFFFFu;
        if (!nearRgb(argb & 0x00FFFFFFu, want_bg, 1)) ++wrong;
        LIFT_CHECK(nearRgb(argb & 0x00FFFFFFu, want_bg, 1));
    }
    std::printf(
        "overlay/size-churn (%s): 200 frames, sampled=%d canvas_origin=(%d,%d) 首读=#%06X 期望=#%06X 错=%d\n",
        gpu ? "direct" : "renderer", sampled, canvas.offset_x, canvas.offset_y, first, want_bg, wrong);
    LIFT_CHECK_EQ_I64(sampled, 20);
    LIFT_CHECK_EQ_I64(wrong, 0);
    // churn 200 帧之后叠层仍然**活着**（⛔ 不是失败后被 `disableOverlay` 静默关掉）。
    LIFT_CHECK(win.overlayEnabled());
    LIFT_CHECK(guestBytes(f) == before);  // ⛔ 叠层不写 guest 像素

    win.shutdown();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    const bool gpu = args.size() >= 2 && args[1] == "--gpu";

    caseScriptPayload();
    caseWorstWindow();

    std::string skip_why;
    if (gpu) {
        if (!caseDirectWindow(&skip_why)) LIFT_SKIP(skip_why.c_str());
        if (!caseSizeChurn(true, &skip_why)) LIFT_SKIP(skip_why.c_str());
    } else {
        if (!caseRendererWindow(&skip_why)) LIFT_SKIP(skip_why.c_str());
        if (!caseSizeChurn(false, &skip_why)) LIFT_SKIP(skip_why.c_str());
    }
    return finish("overlay");
}
