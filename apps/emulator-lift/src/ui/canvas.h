// emulator-lift · UI 画布原点（D5 的 1280×720 参考系）
//
// ★ 纯函数、**SDL-free / GPU-free**：pure CTest 直接 include（`tests/ui_canvas.cpp`）。
// ★ 这是 B2 裁定的**唯一一份**算术：声明用 1280×720 参考系，画面内**等比 + 居中** ⇒
//     s        = min(pic_w/1280, pic_h/720)
//     画布原点 = 画面矩形原点 + ((pic_w − 1280s)/2, (pic_h − 720s)/2)
//   ⛔ 不要在任何窗口 / 渲染实现里另写一份（`present_sdl.cpp` / `d3d9_gpu.cpp` 只消费这里的结果）。
// ★ 16:9 ⇒ 偏移 (0,0)（画布原点 == 画面矩形原点）；非 16:9 ⇒ 有一侧居中偏移
//   （例：320×240 ⇒ s = 0.25，偏移 (0,30)）。这与 `runtime/cursor_map.h` 的 letterbox 是**两个**
//   概念：那个是"帧 → 窗口"，这里是"UI 参考系 → 画面矩形"。
#pragma once

namespace lift::ui {

// 参考系尺寸（与 D5 同源；⛔ 不要散落成字面量 1280/720）。
inline constexpr int kCanvasRefW = 1280;
inline constexpr int kCanvasRefH = 720;

struct CanvasOrigin {
    // 参考像素 → 窗口像素的等比缩放。0 = 几何不可用（画面尺寸非正）。
    double scale = 0.0;
    // 画布原点相对**画面矩形原点**的偏移（窗口像素，已取整）。
    int offset_x = 0;
    int offset_y = 0;
};

// `pic_w` / `pic_h` = letterbox 之后**画面矩形**的窗口像素尺寸（⛔ 不是窗口尺寸）。
// 画面尺寸非正 ⇒ `scale = 0` 且偏移 0（调用方据此跳过这一帧，⛔ 不猜）。
inline CanvasOrigin uiCanvasOrigin(int pic_w, int pic_h) {
    if (pic_w <= 0 || pic_h <= 0) return CanvasOrigin{};
    const double sx = static_cast<double>(pic_w) / static_cast<double>(kCanvasRefW);
    const double sy = static_cast<double>(pic_h) / static_cast<double>(kCanvasRefH);
    const double s = (sx < sy) ? sx : sy;
    // 画布在画面内居中：用**取整后的**画布尺寸算偏移，保证原点与像素栅格对齐。
    const int cw = static_cast<int>(static_cast<long long>(kCanvasRefW) * s + 0.5);
    const int ch = static_cast<int>(static_cast<long long>(kCanvasRefH) * s + 0.5);
    const int ox = (pic_w - cw) / 2;
    const int oy = (pic_h - ch) / 2;
    return CanvasOrigin{s, ox, oy};
}

}  // namespace lift::ui
