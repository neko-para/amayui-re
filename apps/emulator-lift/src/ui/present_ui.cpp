#include "ui/present_ui.h"

#include <cstddef>

#include "runtime/font.h"
#include "runtime/present.h"
#include "ui/canvas.h"
#include "ui/element.h"
#include "ui/raster.h"

namespace lift::ui {

bool presentUi(PresentSink* sink, FontEngine& font, UiScript& script, const UiFrameReadings& readings,
               std::string* err) {
    if (sink == nullptr) return true;
    PresentGeometry geometry;
    if (!sink->presentGeometry(&geometry)) {
        // 没有几何（窗口还没建 / 最小化到 0）：这一帧不画，⛔ 不算合成失败。
        return true;
    }
    const CanvasOrigin canvas = uiCanvasOrigin(geometry.picture_w, geometry.picture_h);
    if (canvas.scale <= 0.0) return true;
    // ★ 脚本拿到的 builder 是**空的**（`UiScript::render` 的合同：它只管追加）。
    UiBuilder builder;
    builder.clear();
    if (!script.render(builder, readings, err)) return false;
    // ★ 空 list = "这一帧什么都不画"（脚本自己决定的，例如版式无关的暂停态）⇒ 正常返回。
    if (builder.elements().empty()) return true;
    RasterImage image;
    if (!rasterize(builder.elements(), canvas, geometry.picture_x, geometry.picture_y, font, &image, err)) {
        return false;
    }
    if (image.rgba.empty()) return true;
    // ★ 接口契约：`rgba` 只在本次调用内有效（sink 自己取所需：SDL_UpdateTexture / GPU 上传）。
    return sink->presentOverlay(image.rgba.data(), image.w, image.h, image.x, image.y, err);
}

}  // namespace lift::ui
