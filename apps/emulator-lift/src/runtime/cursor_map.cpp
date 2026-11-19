#include "runtime/cursor_map.h"

#include <cmath>

namespace lift {
namespace {

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

LetterboxMap letterboxFor(int window_w, int window_h, int image_w, int image_h) {
    LetterboxMap map;
    map.image_w = image_w;
    map.image_h = image_h;
    if (window_w <= 0 || window_h <= 0 || image_w <= 0 || image_h <= 0) return map;  // scale 保持 0
    const double sx = static_cast<double>(window_w) / static_cast<double>(image_w);
    const double sy = static_cast<double>(window_h) / static_cast<double>(image_h);
    map.scale = sx < sy ? sx : sy;
    if (map.scale <= 0.0) {
        map.scale = 0.0;
        return map;
    }
    map.off_x = (static_cast<double>(window_w) - static_cast<double>(image_w) * map.scale) * 0.5;
    map.off_y = (static_cast<double>(window_h) - static_cast<double>(image_h) * map.scale) * 0.5;
    return map;
}

bool imageToWindowPixel(const LetterboxMap& map, int image_x, int image_y, int* window_x, int* window_y) {
    if (map.scale <= 0.0 || window_x == nullptr || window_y == nullptr) return false;
    *window_x = static_cast<int>(std::lround(map.off_x + static_cast<double>(image_x) * map.scale));
    *window_y = static_cast<int>(std::lround(map.off_y + static_cast<double>(image_y) * map.scale));
    return true;
}

bool windowPixelToImage(const LetterboxMap& map, double window_x, double window_y, int* image_x, int* image_y) {
    if (map.scale <= 0.0 || map.image_w <= 0 || map.image_h <= 0 || image_x == nullptr || image_y == nullptr) {
        return false;
    }
    const int x = static_cast<int>(std::lround((window_x - map.off_x) / map.scale));
    const int y = static_cast<int>(std::lround((window_y - map.off_y) / map.scale));
    *image_x = clampInt(x, 0, map.image_w - 1);
    *image_y = clampInt(y, 0, map.image_h - 1);
    return true;
}

bool mapWindowPixelToImage(int window_w, int window_h, int image_w, int image_h, double window_x,
                           double window_y, int* image_x, int* image_y) {
    return windowPixelToImage(letterboxFor(window_w, window_h, image_w, image_h), window_x, window_y, image_x,
                              image_y);
}

bool overlayPointerWindowPixels(int client_x, int client_y, int logical_w, int logical_h, int pixel_w,
                                int pixel_h, int image_w, int image_h, int* window_px, int* window_py) {
    if (window_px == nullptr || window_py == nullptr) return false;
    // ① guest 客户区 → 宿主窗口**点**（letterbox 正算；几何非正 ⇒ `imageToWindowPixel` 已经 false）。
    int wx = 0;
    int wy = 0;
    if (!imageToWindowPixel(letterboxFor(logical_w, logical_h, image_w, image_h), client_x, client_y, &wx, &wy)) {
        return false;  // ⛔ 不猜一个几何
    }
    // ② 窗口点 → 窗口像素（`pixel/logical`，四舍五入）。与 `Impl::pointerWindowPixels` 同口径；
    //    ⛔ 纯函数不依赖那个 SDL 方法（它要真窗口），这里只吃两个尺寸。
    if (logical_w <= 0 || logical_h <= 0 || pixel_w <= 0 || pixel_h <= 0) return false;
    const double sx = static_cast<double>(pixel_w) / static_cast<double>(logical_w);
    const double sy = static_cast<double>(pixel_h) / static_cast<double>(logical_h);
    *window_px = static_cast<int>(std::lround(static_cast<double>(wx) * sx));
    *window_py = static_cast<int>(std::lround(static_cast<double>(wy) * sy));
    return true;
}

bool referenceToClientPixel(int client_w, int client_h, int ref_x, int ref_y, int* x, int* y) {
    if (x == nullptr || y == nullptr) return false;
    // 0 / 非正 = 还没有窗口 ⇒ 目标就是参考系（恒等）；⛔ 不在这里猜 1280×720 之外的尺寸。
    const int w = client_w > 0 ? client_w : kReferenceFrameW;
    const int h = client_h > 0 ? client_h : kReferenceFrameH;
    // 各轴独立（非 16:9 时拉伸参考系），四舍五入；⛔ 不 clamp（人手写的点原样投出去）。
    *x = static_cast<int>(std::lround(static_cast<double>(ref_x) * static_cast<double>(w) /
                                      static_cast<double>(kReferenceFrameW)));
    *y = static_cast<int>(std::lround(static_cast<double>(ref_y) * static_cast<double>(h) /
                                      static_cast<double>(kReferenceFrameH)));
    return true;
}

}  // namespace lift
