// emulator-lift · MessageBox 的**窗口像素**守卫（T3）+ 直出失败清理（M6）
//
// 判据（红了意味着什么）：
//   ① **renderer 路径真的把合成图贴到窗口上**：`SdlWindow::presentModal` 之后回读窗口像素 ——
//      角 = 变暗后的末帧（纯白 backdrop ×95/255 ⇒ `(95,95,95)`）、面板 = `(226,226,230)`、
//      默认按钮内部 = `(242,242,246)`、默认按钮描边 = accent `(40,110,220)`、
//      面板水平居中（bbox 宽 = `kModalPanelW-2`、中心距 640 ≤ 1px）、正文区有**精确文字色**像素。
//      ⇒ texture 格式（ABGR8888）/ `BLENDMODE_NONE` / 逻辑坐标 LETTERBOX / 不透明合成四件事
//      任何一件坏掉，这里都会红（fake sink 的 `tests/messagebox.cpp` 抓不到它们）。
//   ② **直出失败路径必须清理**（M6 复审）：`directOverlay` 成功、`directPresent` 失败时，
//      `endModal` 仍必须 `directOverlay(nullptr)` 释放模态纹理，且 `target_current` 被消费掉
//      （否则下一次 `submitFrame` 会跳过 guest 上传、模态图留在 GPU 上继续盖住后续帧）。
//      用 `setDirectPresenterForTest` 注入替身（不 claim 窗口），⛔ 不需要 GPU。
//   ③ `--gpu` 臂：真 GPU 直出（`raster_gpu=true`）下同一组像素判据（GPU 不可用 ⇒ SKIP 77）。
//
// ★ 档位：顶层无条件注册 ⇒ **pure**（`AGENTS.md` §5 的判据 = 注册位置）。默认臂跑
//   `SDL_VIDEODRIVER=dummy` + software 渲染器；`-gpu` 臂**不**设 dummy（直出要真 claim 窗口）。
//   无窗口系统 / 无 GPU ⇒ `LIFT_SKIP(77)`，⛔ 不算红。
// ★ 字体用 `FontEngine::setSyntheticForTest(true)`（裸字体文件是 LFS；窗口守卫不该依赖它）。
//   真正"日文字形画得出来"由 assets 档的 `gdi-font` + 人工截图核；这里钉的是**窗口合成/贴图**。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/host_encoding.h"
#include "runtime/message_box.h"
#include "runtime/present_sdl.h"

namespace {

constexpr std::uint32_t rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
}

// 合成一帧确定的模态图：白色 backdrop 当"末帧"⇒ 角必须正好是 95（= 255*95/255，变暗可断言）。
bool composeTestFrame(std::vector<std::uint8_t>* img, int* w, int* h, lift::ModalSpec* out_spec,
                      lift::ModalLayout* out_layout) {
    lift::FontEngine fonts;
    fonts.setSyntheticForTest(true);
    lift::ModalSpec spec = lift::makeMessageBoxSpec(0x34u, "lift", "line one\r\nline two");
    const lift::ModalLayout layout = lift::modalLayout(spec, 1280, 720);
    std::vector<std::uint8_t> backdrop(static_cast<std::size_t>(1280) * 720u * 4u, 255u);
    lift::ModalInput in;
    in.mouse_x = layout.panel.x + 3;
    in.mouse_y = layout.panel.y + 3;
    bool font_missing = true;
    const bool ok = lift::composeModalFrame(spec, layout, in, &fonts, backdrop.data(), 1280, 720, 1280 * 4, img,
                                            w, h, &font_missing);
    *out_spec = spec;
    *out_layout = layout;
    return ok && !font_missing;
}

lift::ModalRect rectForId(const lift::ModalSpec& spec, const lift::ModalLayout& layout, std::uint32_t id) {
    for (std::size_t i = 0; i < spec.buttons.size() && i < layout.button_rects.size(); ++i) {
        if (spec.buttons[i].id == id) return layout.button_rects[i];
    }
    return lift::ModalRect{};
}

// 一组窗口像素判据（renderer / 直出两条路径共用同一份期望）。
bool checkPixels(lift::SdlWindow& win) {
    std::vector<std::uint8_t> img;
    int w = 0;
    int h = 0;
    lift::ModalSpec spec;
    lift::ModalLayout layout;
    if (!composeTestFrame(&img, &w, &h, &spec, &layout)) {
        lift_test::failf(__FILE__, __LINE__, "composeTestFrame 失败（synthetic 字体不可用？）");
        return false;
    }
    win.setCaptureForTest(true);
    std::string err;
    if (!win.presentModal(img.data(), w, h, &err)) {
        lift_test::failf(__FILE__, __LINE__, "presentModal 失败：%s", err.c_str());
        return false;
    }
    bool ok = true;
    auto expect_px = [&](int x, int y, std::uint32_t want, const char* what) {
        ++lift_test::g_checks;  // ⛔ 别让"精确像素都对"时一条 check 都不记（空夹具不能算通过）
        std::uint32_t px = 0;
        std::string rerr;
        if (!win.readbackPixel(x, y, &px, &rerr)) {
            lift_test::failf(__FILE__, __LINE__, "%s: readbackPixel(%d,%d) 失败：%s", what, x, y, rerr.c_str());
            ok = false;
            return;
        }
        if ((px & 0x00FFFFFFu) != want) {
            lift_test::failf(__FILE__, __LINE__, "%s: (%d,%d) expected 0x%06X got 0x%06X", what, x, y, want,
                        px & 0x00FFFFFFu);
            ok = false;
        }
    };
    // ① 角 = 变暗后的白 backdrop；面板底 / 按钮底 / accent 三档精确色。
    expect_px(0, 0, rgb(95, 95, 95), "dim-corner");
    expect_px(layout.panel.x + 2, layout.panel.y + 2, rgb(226, 226, 230), "panel-bg");
    const lift::ModalRect no = rectForId(spec, layout, 7);  // MB_YESNO 默认键 = No
    ++lift_test::g_checks;
    if (no.empty()) {
        lift_test::failf(__FILE__, __LINE__, "找不到默认按钮 No 的矩形");
        ok = false;
    } else {
        expect_px(no.x + 4, no.y + 4, rgb(242, 242, 246), "button-bg");
        expect_px(no.x + 1, no.y + 1, rgb(40, 110, 220), "button-accent(default)");
    }
    // ② 面板居中：bbox 宽 = kModalPanelW-2（两侧各 1px 边框）。
    int min_x = -1;
    int max_x = -1;
    for (int x = 0; x < w; ++x) {
        std::uint32_t px = 0;
        std::string rerr;
        if (!win.readbackPixel(x, layout.panel.y + 2, &px, &rerr)) {
            ok = false;
            break;
        }
        if ((px & 0x00FFFFFFu) == rgb(226, 226, 230)) {
            if (min_x < 0) min_x = x;
            max_x = x;
        }
    }
    if (min_x < 0) {
        lift_test::failf(__FILE__, __LINE__, "面板色一个像素都没找到（模态帧没贴上去？）");
        ok = false;
    } else {
        ++lift_test::g_checks;
        if (max_x - min_x + 1 != lift::kModalPanelW - 2) {
            lift_test::failf(__FILE__, __LINE__, "面板 bbox 宽 expected %d got %d", lift::kModalPanelW - 2,
                        max_x - min_x + 1);
            ok = false;
        }
        const int center = (min_x + max_x) / 2;
        ++lift_test::g_checks;
        if (center < 639 || center > 641) {
            lift_test::failf(__FILE__, __LINE__, "面板中心 expected ~640 got %d", center);
            ok = false;
        }
    }
    // ③ 正文层真的画了字（synthetic 字形的满覆盖像素在合成后**正好**是文字色）。
    int text_pixels = 0;
    for (int y = layout.text.y; y < layout.text.y + layout.text.h && text_pixels == 0; ++y) {
        for (int x = layout.text.x; x < layout.text.x + layout.text.w; ++x) {
            std::uint32_t px = 0;
            std::string rerr;
            if (!win.readbackPixel(x, y, &px, &rerr)) {
                ok = false;
                break;
            }
            if ((px & 0x00FFFFFFu) == rgb(36, 38, 46)) {
                ++text_pixels;
                break;
            }
        }
    }
    ++lift_test::g_checks;
    if (text_pixels == 0) {
        lift_test::failf(__FILE__, __LINE__, "正文区没有任何精确文字色像素（文字层没合成？）");
        ok = false;
    }
    win.endModal();
    return ok;
}

// ---- ① renderer 路径（dummy + software）与 ③ 真 GPU 直出共用 ----
bool caseWindowPixels(bool gpu, std::string* skip) {
    lift_test::g_current_case = gpu ? "messagebox-window-gpu/pixels" : "messagebox-window/pixels";
    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.hidden = true;
    if (gpu) {
        opt.raster_gpu = true;  // 真直出：init 里 claim 窗口；不可用 ⇒ SKIP
    } else {
        opt.driver_list = "software";
    }
    std::string err;
    if (!win.init(opt, &err)) {
        *skip = gpu ? ("GPU 直出不可用：" + err) : ("窗口/software 渲染器不可用：" + err);
        return false;
    }
    const bool ok = checkPixels(win);
    win.shutdown();
    return ok;
}

// ---- ② 直出失败清理（M6）：directOverlay 成功 + directPresent 失败 ⇒ endModal 必须清 ----
struct FakeDirect final : lift::GpuDirectPresenter {
    bool present_ok = true;
    int upload_calls = 0;
    int present_calls = 0;
    int overlay_uploads = 0;
    int overlay_clears = 0;
    bool overlay_visible = false;
    bool target_current = false;
    int last_overlay_w = 0;
    int last_overlay_h = 0;

    bool directAttachWindow(void*, bool, std::string*) override { return true; }
    void directDetachWindow() override {}
    bool directActive() const override { return true; }
    const char* directDriverName() const override { return "fake-direct"; }
    const char* directPresentMode() const override { return "fake"; }
    bool directUploadGuest(const lift::SurfaceFormat&, const std::uint8_t*, std::uint32_t, int, int,
                           std::string*) override {
        ++upload_calls;
        target_current = true;  // 与真后端一致：上传完 target == guest
        return true;
    }
    void directSetTargetCurrent(bool current) override { target_current = current; }
    bool directTargetCurrent() const override { return target_current; }
    bool directPresent(std::string* err) override {
        ++present_calls;
        if (!present_ok) {
            if (err) *err = "注入的 directPresent 失败（模拟 swapchain out-of-date）";
            return false;
        }
        return true;
    }
    bool directReadback(std::vector<std::uint8_t>*, int*, int*, std::string* err) override {
        if (err) *err = "fake 直出没有回读";
        return false;
    }
    bool directOverlay(const std::uint8_t* rgba, int w, int h, std::string*) override {
        if (rgba == nullptr || w <= 0 || h <= 0) {
            ++overlay_clears;
            overlay_visible = false;
            last_overlay_w = 0;
            last_overlay_h = 0;
            return true;
        }
        ++overlay_uploads;
        overlay_visible = true;
        last_overlay_w = w;
        last_overlay_h = h;
        return true;
    }
};

bool caseDirectFailureCleanup(std::string* skip) {
    lift_test::g_current_case = "messagebox-window/direct-failure-cleanup";
    FakeDirect fake;
    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.hidden = true;
    opt.driver_list = "software";
    win.setDirectPresenterForTest(&fake);
    std::string err;
    if (!win.init(opt, &err)) {
        *skip = "窗口不可用：" + err;
        return false;
    }
    std::vector<std::uint8_t> img;
    int w = 0;
    int h = 0;
    lift::ModalSpec spec;
    lift::ModalLayout layout;
    LIFT_CHECK(composeTestFrame(&img, &w, &h, &spec, &layout));

    // ---- 失败分支：overlay 上传成功、present 失败 ----
    fake.present_ok = false;
    std::string perr;
    LIFT_CHECK(!win.presentModal(img.data(), w, h, &perr));
    LIFT_CHECK(fake.overlay_uploads == 1);       // 模态纹理真的传到了直出后端
    LIFT_CHECK(fake.overlay_visible);
    LIFT_CHECK_EQ_I64(fake.last_overlay_w, w);
    LIFT_CHECK_EQ_I64(fake.last_overlay_h, h);
    LIFT_CHECK(!fake.target_current);            // ★ M6：失败也要消费掉 target_current
    LIFT_CHECK_EQ_I64(fake.present_calls, 1);
    win.endModal();
    LIFT_CHECK_EQ_I64(fake.overlay_clears, 1);   // ★ M6：endModal 必须释放模态纹理
    LIFT_CHECK(!fake.overlay_visible);
    LIFT_CHECK(!fake.target_current);

    // ---- 成功分支：同一窗口再来一次 ⇒ 正常呈现后收口也清理干净 ----
    fake.present_ok = true;
    LIFT_CHECK(win.presentModal(img.data(), w, h, &perr));
    LIFT_CHECK_EQ_I64(fake.present_calls, 2);
    LIFT_CHECK_EQ_I64(fake.overlay_uploads, 2);
    LIFT_CHECK(fake.overlay_visible);
    LIFT_CHECK(!fake.target_current);
    win.endModal();
    LIFT_CHECK_EQ_I64(fake.overlay_clears, 2);
    LIFT_CHECK(!fake.overlay_visible);
    LIFT_CHECK(!fake.target_current);
    win.shutdown();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    const bool gpu = args.size() >= 2 && args[1] == "--gpu";
    std::string skip;
    // ★ `skip` 非空 = **环境不可用**（无窗口 / 无 GPU）⇒ SKIP 77；空 = 判据红（`failf` 已记 ⇒ finish 1）。
    if (gpu) {
        if (!caseWindowPixels(true, &skip)) {
            if (!skip.empty()) LIFT_SKIP(skip.c_str());
            return finish("messagebox-window");
        }
    } else {
        if (!caseWindowPixels(false, &skip)) {
            if (!skip.empty()) LIFT_SKIP(skip.c_str());
            return finish("messagebox-window");
        }
        if (!caseDirectFailureCleanup(&skip)) {
            if (!skip.empty()) LIFT_SKIP(skip.c_str());
            return finish("messagebox-window");
        }
    }
    return finish("messagebox-window");
}
