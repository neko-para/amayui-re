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

}  // namespace lift
