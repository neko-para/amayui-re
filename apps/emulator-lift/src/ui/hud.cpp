#include "ui/hud.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "runtime/font.h"
#include "runtime/overlay.h"
#include "runtime/present.h"
#include "ui/canvas.h"
#include "ui/raster.h"

namespace lift::ui {
namespace {

Color overlayBg() { return Color{kOverlayBg[0], kOverlayBg[1], kOverlayBg[2], kOverlayBg[3]}; }
Color overlayFg() { return Color{kOverlayFg[0], kOverlayFg[1], kOverlayFg[2], kOverlayFg[3]}; }

}  // namespace

bool buildHud(UiBuilder& builder, FontEngine& font, const std::vector<std::string>& lines, std::string* err) {
    builder.clear();
    if (lines.empty()) return true;
    HostTextMetrics metrics;
    if (!font.metrics(kHudFontRefPx, false, &metrics)) {
        if (err) *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ 无法构建 HUD 文本";
        return false;
    }
    int max_width = 0;
    for (const std::string& line : lines) {
        int w = 0;
        int h = 0;
        if (!measureText(line, kHudFontRefPx, false, font, &w, &h, err)) return false;
        max_width = std::max(max_width, w);
    }
    const int line_h = std::max(kHudFontRefPx + kHudLineGapRef, static_cast<int>(metrics.height) + 2);
    const int panel_w = max_width + 2 * kHudPadRef;
    const int panel_h = static_cast<int>(lines.size()) * line_h + 2 * kHudPadRef;
    builder.rect(0, 0, panel_w, panel_h, overlayBg());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const int top = kHudPadRef + static_cast<int>(i) * line_h;
        builder.text(kHudPadRef, top, lines[i], kHudFontRefPx, overlayFg(), false);
    }
    return true;
}

bool presentHud(PresentSink* sink, FontEngine& font, const std::vector<std::string>& lines, std::string* err) {
    if (sink == nullptr) return true;
    PresentGeometry geometry;
    if (!sink->presentGeometry(&geometry)) {
        // 没有几何（窗口还没建 / 最小化到 0）：这一帧不画，⛔ 不算合成失败。
        return true;
    }
    const CanvasOrigin canvas = uiCanvasOrigin(geometry.picture_w, geometry.picture_h);
    if (canvas.scale <= 0.0) return true;
    UiBuilder builder;
    if (!buildHud(builder, font, lines, err)) return false;
    RasterImage image;
    if (!rasterize(builder.elements(), canvas, geometry.picture_x, geometry.picture_y, font, &image, err)) {
        return false;
    }
    if (image.rgba.empty()) return true;
    // ★ 接口契约：`rgba` 只在本次调用内有效（sink 自己取所需：SDL_UpdateTexture / GPU 上传）。
    return sink->presentOverlay(image.rgba.data(), image.w, image.h, image.x, image.y, err);
}

}  // namespace lift::ui
