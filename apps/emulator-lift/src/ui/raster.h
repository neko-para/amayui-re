// emulator-lift · display list → 窗口像素 + 宿主字体栅格化（M0）
//
// ★ 两条呈现路径（SDL_Renderer / GPU 直出）都**只**消费这里产出的那一块 RGBA8 图像：
//   `presentOverlay(rgba,w,h,x,y)`。⛔ 不存在第二套文本 / 矩形绘制路径（`overlay.md` D12）。
// ★ 文本走**宿主字体**（`runtime/font.h` 的 `FontEngine` / FreeType）：按**最终像素**取字
//   （`overlay.md` D5 / U1），⛔ 不放大位图、⛔ 不用 5×7 点阵当生产字形。
//   字体不可用 ⇒ 返回 false + err（调用方必须响亮失败，⛔ 不静默画空白 / 退回点阵）。
// ★ 解析规则（M0 子集）：参考像素的绝对 `at` + `size`；`s = uiCanvasOrigin(...).scale`；
//   画布原点 = 画面矩形原点 + 居中偏移。⛔ 这里不做 anchor / 百分比 / when / 嵌套 / 命中。
// ★ 纯 CPU、无 SDL / GPU：唯一的外部依赖是 `FontEngine`（FreeType 桥）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/font.h"
#include "ui/canvas.h"
#include "ui/element.h"

namespace lift::ui {

// 一行文本在给定**最终像素**字号下的宽 / 高（rasterize 与 hud 共用这一份度量）。
// 宽 = Σ `glyph.advance`，高 = `metrics.height`。false = 宿主字体不可用（err 必须填）。
bool measureText(const std::string& text, int pixel_size, bool bold, FontEngine& font, int* w, int* h,
                 std::string* err);

// 栅格化结果：一块 RGBA8 图像 + 它在**窗口像素**空间的左上角。
struct RasterImage {
    std::vector<std::uint8_t> rgba;
    int w = 0;
    int h = 0;
    int x = 0;
    int y = 0;
};

// 把有序 display list 解析成窗口像素并合成成一块 RGBA8：
//   · Rect：绝对 at/size × s（四舍五入）；填不透明 `fill`；
//   · Text：字号 = round(font_size*s)（至少 1 px），逐 UTF-8 码位向宿主字体取字，按最终像素画；
//   · bbox = 所有元素并集；`out->x/y` = bbox 左上角在窗口像素空间的坐标。
// `picture_x` / `picture_y` = 画面矩形原点（`PresentSink::presentGeometry`）；`canvas` = 画布缩放 + 居中偏移。
// false = 几何非法 / 宿主字体不可用（err 必须填）。
bool rasterize(const std::vector<Element>& elements, const CanvasOrigin& canvas, int picture_x, int picture_y,
               FontEngine& font, RasterImage* out, std::string* err);

}  // namespace lift::ui
