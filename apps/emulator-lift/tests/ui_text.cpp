// emulator-lift · UI 文本走**宿主字体**（U1；FreeType）的判据
//
// 判据（红了意味着什么）：
//   ① **中文有字形**：`天`（U+5929）/ `結`（U+7D50）经 `FontEngine::glyph` 得到**非空覆盖率**
//      （`black_box_x/y > 0` 且 coverage 里存在非 0）。红了 ⇒ 生产 HUD 不支持中文（U1 的硬要求），
//      或有人把文本路径换回了只覆盖 ASCII 的 5×7 点阵。
//   ② **栅格化走最终像素**：`ui::buildHud` + `ui::rasterize` 在 640×360 画面（s=0.5）上产出
//      非空的 RGBA8 图像，图像原点 == **画布原点**（16:9 ⇒ 0,0），且同时含**底 / 字两种精确颜色**；
//      320×240（4:3）⇒ 原点 `(0,30)`（B2 的居中偏移真的参与了文本路径，⛔ 不是恒 0）。
//      红了 ⇒ 文本没有走通用合成路径 / 画布原点没接上 / 文本被当成空白。
//   ③ **字体缺失 = 硬失败**：把宿主字体配置置空后，`buildHud` / `rasterize` 返回 **false + 非空 err**
//      （⛔ 不静默退回点阵、⛔ 不画空白）。红了 ⇒ R5 的"字体缺失 ⇒ 启动硬错"没有落到实现上。
// ★ 缺字体文件 ⇒ `LIFT_SKIP`（77），⛔ 不假绿（与 `tests/gdi_font.cpp` 同一档）。
#include <cstdint>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "runtime/overlay.h"
#include "ui/canvas.h"
#include "ui/hud.h"
#include "ui/raster.h"

namespace {

constexpr std::uint32_t kTian = 0x5929;  // 天
constexpr std::uint32_t kJie = 0x7D50;   // 結

std::size_t countCoverage(const lift::GlyphMask& mask) {
    std::size_t nonzero = 0;
    for (const std::uint8_t c : mask.coverage) {
        if (c != 0) ++nonzero;
    }
    return nonzero;
}

std::size_t countRgba(const std::vector<std::uint8_t>& rgba, const std::uint8_t (&want)[4]) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 0] == want[0] && rgba[i + 1] == want[1] && rgba[i + 2] == want[2] && rgba[i + 3] == want[3]) {
            ++n;
        }
    }
    return n;
}

// ---- ① 中文串经宿主字体出字（非空覆盖率）----
void caseChineseCoverage(lift::FontEngine& font) {
    lift_test::g_current_case = "ui-text/host-font-chinese";
    for (const std::uint32_t cp : {kTian, kJie}) {
        lift::GlyphMask glyph;
        const bool ok = font.glyph(static_cast<char32_t>(cp), 16, false, &glyph);
        ++lift_test::g_checks;
        if (!ok) lift_test::failf(__FILE__, __LINE__, "U+%04X 取字失败（宿主字体不可用？）", cp);
        ++lift_test::g_checks;
        if (glyph.black_box_x <= 0 || glyph.black_box_y <= 0 || countCoverage(glyph) == 0) {
            lift_test::failf(__FILE__, __LINE__, "U+%04X 的覆盖率是空的（点阵路径会这样）", cp);
        }
    }
}

// ---- ② display list → 宿主字体栅格化（最终像素）+ 画布原点 ----
void caseRaster(lift::FontEngine& font) {
    lift_test::g_current_case = "ui-text/raster-origin-16x9";
    const std::vector<std::string> lines = {"天結", "frame 1"};
    lift::ui::UiBuilder builder;
    std::string err;
    LIFT_CHECK(lift::ui::buildHud(builder, font, lines, &err));
    if (!err.empty()) lift_test::failf(__FILE__, __LINE__, "buildHud: %s", err.c_str());
    LIFT_CHECK(builder.elements().size() == lines.size() + 1);  // 1 块底 + 每行一个 text

    const lift::ui::CanvasOrigin wide = lift::ui::uiCanvasOrigin(640, 360);
    lift::ui::RasterImage image;
    LIFT_CHECK(lift::ui::rasterize(builder.elements(), wide, 0, 0, font, &image, &err));
    LIFT_CHECK(!image.rgba.empty());
    LIFT_CHECK_EQ_I64(image.x, 0);  // 16:9 ⇒ 画布原点 == 画面原点
    LIFT_CHECK_EQ_I64(image.y, 0);
    LIFT_CHECK(image.w > 0 && image.h > 0);
    const std::size_t bg = countRgba(image.rgba, lift::kOverlayBg);
    const std::size_t fg = countRgba(image.rgba, lift::kOverlayFg);
    LIFT_CHECK(bg > 0);
    LIFT_CHECK(fg > 0);  // ★ 文本真的画出来了（含中文行）

    lift_test::g_current_case = "ui-text/raster-origin-4x3";
    const lift::ui::CanvasOrigin tall = lift::ui::uiCanvasOrigin(320, 240);
    lift::ui::RasterImage tall_image;
    LIFT_CHECK(lift::ui::rasterize(builder.elements(), tall, 0, 0, font, &tall_image, &err));
    LIFT_CHECK(!tall_image.rgba.empty());
    // 4:3 ⇒ 画布纵向居中 30 px（B2）；文本路径必须吃同一个画布原点。
    LIFT_CHECK_EQ_I64(tall_image.x, 0);
    LIFT_CHECK_EQ_I64(tall_image.y, 30);
}

// ---- ③ 字体缺失 ⇒ 硬失败（⛔ 不静默退回点阵）----
void caseFontMissing() {
    lift_test::g_current_case = "ui-text/font-missing-is-hard-error";
    const lift::FontConfig saved = lift::fontConfig();
    lift::FontConfig none;
    none.regular_path.clear();
    none.bold_path.clear();
    lift::fontSetConfig(none);
    {
        lift::FontEngine font;
        lift::ui::UiBuilder builder;
        std::string err;
        const bool built = lift::ui::buildHud(builder, font, {"天結"}, &err);
        LIFT_CHECK(!built);
        LIFT_CHECK(!err.empty());
        // ★ `buildHud` 失败时会 `clear()` builder ⇒ 这里另造一个**非空** list 再喂 `rasterize`，
        //   否则空 list 会静默返回 true（那正是"没有字体也画出空白"的形状，但不是本条的判据）。
        lift::ui::UiBuilder raster_builder;
        raster_builder.text(0, 0, "天結", 16, lift::ui::Color{255, 255, 255, 255});
        lift::ui::RasterImage image;
        err.clear();
        const bool rasterized = lift::ui::rasterize(raster_builder.elements(), lift::ui::uiCanvasOrigin(1280, 720),
                                                    0, 0, font, &image, &err);
        LIFT_CHECK(!rasterized);
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(image.rgba.empty());
    }
    lift::fontSetConfig(saved);
}

}  // namespace

int main() {
    lift::FontEngine font;
    if (!font.fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }
    caseChineseCoverage(font);
    caseRaster(font);
    caseFontMissing();
    return lift_test::finish("ui-text");
}
