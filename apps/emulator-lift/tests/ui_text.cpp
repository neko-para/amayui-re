// emulator-lift · UI 文本走**宿主字体**（U1；FreeType）的判据
//
// 判据（红了意味着什么）：
//   ① **中文有字形**：`天`（U+5929）/ `結`（U+7D50）经 `FontEngine::glyph` 得到**非空覆盖率**
//      （`black_box_x/y > 0` 且 coverage 里存在非 0）。红了 ⇒ 生产 HUD 不支持中文（U1 的硬要求），
//      或有人把文本路径换回了只覆盖 ASCII 的 5×7 点阵。
//   ② **栅格化走最终像素**：`builder.rect` + `builder.text` 造一份最小 display list，`ui::rasterize`
//      在 640×360 画面（s=0.5）上产出非空的 RGBA8 图像，图像原点 == **画布原点**
//      （16:9 ⇒ 0,0），且同时含**底 / 字两种精确颜色**；
//      320×240（4:3）⇒ 原点 `(0,30)`（B2 的居中偏移真的参与了文本路径，⛔ 不是恒 0）。
//      红了 ⇒ 文本没有走通用合成路径 / 画布原点没接上 / 文本被当成空白。
//   ③ **字体缺失 = 硬失败**：把宿主字体配置置空后，`rasterize` 返回 **false + 非空 err**
//      （⛔ 不静默画空白）。红了 ⇒ R5 的"字体缺失 ⇒ 启动硬错"没有落到实现上。
//   ④ **超大 rect = 本帧 UI 失败（⛔ 不穿给宿主）**：`builder.rect(..., 1000000, 1000000, ...)` 喂
//      `rasterize` ⇒ false + 非空 err + 不分配像素。红了 ⇒ `--ui` 脚本的 `ui.rect({w:1e6,h:1e6})`
//      又能触发宿主级超大分配（`std::length_error` / SIGKILL）。
//      ★ 这一条与字体无关（Rect 不走向度量）⇒ 在字体可用性判断**之前**跑。
// ★ B（2026-11-18）：HUD 的版式 / 内容归外置脚本 `scripts/ui/hud.js`（C++ 侧不再有"行 → list"的
//   构造器）—— 本用例只验**栅格化 + 画布原点**这一段通用路径，⛔ 不测任何 HUD 文案。
// ★ 缺字体文件 ⇒ `LIFT_SKIP`（77），⛔ 不假绿（与 `tests/gdi_font.cpp` 同一档）。
#include <cstdint>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/font.h"
#include "ui/canvas.h"
#include "ui/element.h"
#include "ui/raster.h"

namespace {

constexpr std::uint32_t kTian = 0x5929;  // 天
constexpr std::uint32_t kJie = 0x7D50;   // 結

// HUD 的两种颜色（与 `scripts/ui/hud.js` 的 `BG` / `FG` 一致）。★ 只是**栅格化的输入**：
// 本用例验的是"这些颜色真的出现在图像里"，与脚本无关。
constexpr std::uint8_t kBg[4] = {16, 24, 32, 255};     // #101820
constexpr std::uint8_t kFg[4] = {235, 255, 235, 255};  // #EBFFEB

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
    lift::ui::UiBuilder builder;
    // 1 块底 + 2 行文本（含中文行）：HUD 脚本产出的也是**这同一形状**的 list（这里手工造最小一份）。
    builder.rect(0, 0, 220, 48, lift::ui::Color{kBg[0], kBg[1], kBg[2], kBg[3]});
    builder.text(8, 8, "天結", 16, lift::ui::Color{kFg[0], kFg[1], kFg[2], kFg[3]});
    builder.text(8, 32, "frame 1", 16, lift::ui::Color{kFg[0], kFg[1], kFg[2], kFg[3]});
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(builder.elements().size()), 3);  // 1 块底 + 2 行 text

    const lift::ui::CanvasOrigin wide = lift::ui::uiCanvasOrigin(640, 360);
    lift::ui::RasterImage image;
    std::string err;
    LIFT_CHECK(lift::ui::rasterize(builder.elements(), wide, 0, 0, font, &image, &err));
    LIFT_CHECK(!image.rgba.empty());
    LIFT_CHECK_EQ_I64(image.x, 0);  // 16:9 ⇒ 画布原点 == 画面原点
    LIFT_CHECK_EQ_I64(image.y, 0);
    LIFT_CHECK(image.w > 0 && image.h > 0);
    const std::size_t bg = countRgba(image.rgba, kBg);
    const std::size_t fg = countRgba(image.rgba, kFg);
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

// ---- ③ 字体缺失 ⇒ 硬失败（⛔ 不静默画空白）----
void caseFontMissing() {
    lift_test::g_current_case = "ui-text/font-missing-is-hard-error";
    const lift::FontConfig saved = lift::fontConfig();
    lift::FontConfig none;
    none.regular_path.clear();
    none.bold_path.clear();
    lift::fontSetConfig(none);
    {
        lift::FontEngine font;
        // ★ 自己造一个**非空** list 再喂 `rasterize`：空 list 会静默返回 true（"没有内容"与
        //   "没有字体"是两件事，那不是本条的判据）。
        lift::ui::UiBuilder raster_builder;
        raster_builder.text(0, 0, "天結", 16, lift::ui::Color{255, 255, 255, 255});
        lift::ui::RasterImage image;
        std::string err;
        const bool rasterized = lift::ui::rasterize(raster_builder.elements(), lift::ui::uiCanvasOrigin(1280, 720),
                                                    0, 0, font, &image, &err);
        LIFT_CHECK(!rasterized);
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(image.rgba.empty());
    }
    lift::fontSetConfig(saved);
}

// ---- ④ 超大 rect ⇒ 本帧 UI 失败（⛔ 不许让 std::length_error / 宿主级 OOM 穿出去）----
// ★ 与字体无关：这里走的是 Rect 的 bbox 上界（`rasterize` 分配前的那两条硬上界），不调字体度量。
void caseHugeRect(lift::FontEngine& font) {
    lift_test::g_current_case = "ui-text/huge-rect-rejected";
    const lift::ui::CanvasOrigin canvas = lift::ui::uiCanvasOrigin(1280, 720);
    {
        // ① 面积型：1e6 × 1e6 = 4 TB（评审实测：夹具进程被 SIGKILL / 抛 std::length_error）。
        lift::ui::UiBuilder builder;
        builder.rect(0, 0, 1000000, 1000000, lift::ui::Color{kBg[0], kBg[1], kBg[2], kBg[3]});
        lift::ui::RasterImage image;
        std::string err;
        const bool rasterized = lift::ui::rasterize(builder.elements(), canvas, 0, 0, font, &image, &err);
        LIFT_CHECK(!rasterized);
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(image.rgba.empty());  // 分配前就失败 ⇒ 一个像素都没申请
    }
    {
        // ② 单边型：1e6 × 1 = 4 MB（面积**远小于** 64 MiB ⇒ 只有单边上限能拦）。
        //   ★ 为什么要这一条：面积型被 64 MiB 上限兜住，单边上限（8192）单独失效时它仍是 false
        //     ⇒ 只测 ① 会让"把 8192 放宽"的变异**假绿**（AGENTS.md §12.7）。
        lift::ui::UiBuilder builder;
        builder.rect(0, 0, 1000000, 1, lift::ui::Color{kBg[0], kBg[1], kBg[2], kBg[3]});
        lift::ui::RasterImage image;
        std::string err;
        const bool rasterized = lift::ui::rasterize(builder.elements(), canvas, 0, 0, font, &image, &err);
        LIFT_CHECK(!rasterized);
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(image.rgba.empty());
    }
}

}  // namespace

int main() {
    lift::FontEngine font;
    // ★ 超大 rect 的拒绝**不依赖字体** ⇒ 放在字体可用性判断之前（字体缺席时这条也成立）。
    caseHugeRect(font);
    if (!font.fontFileAvailable(false)) {
        LIFT_SKIP("缺宿主字体 Amayui-CN_cnjp.ttf（LIFT_HOST_FONT_PATH / corpus/assets/fonts）");
    }
    caseChineseCoverage(font);
    caseRaster(font);
    caseFontMissing();
    return lift_test::finish("ui-text");
}
