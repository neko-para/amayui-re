#include "ui/raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace lift::ui {
namespace {

// UTF-8 → 码位（⛔ 不猜：非法序列按 U+FFFD 前进 1 字节；截断序列按剩余字节数前进）。
// 插件文本 / HUD 文案都是 UTF-8（引擎的 CP932 在 `runtime/cp932.h` 那一侧就已经转完）。
char32_t nextCodepoint(const std::string& s, std::size_t* i) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(s[at]); };
    const unsigned char c0 = byte(*i);
    if (c0 < 0x80) {
        ++*i;
        return c0;
    }
    int extra = 0;
    char32_t cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        extra = 1;
        cp = c0 & 0x1Fu;
    } else if ((c0 & 0xF0) == 0xE0) {
        extra = 2;
        cp = c0 & 0x0Fu;
    } else if ((c0 & 0xF8) == 0xF0) {
        extra = 3;
        cp = c0 & 0x07u;
    } else {
        ++*i;
        return 0xFFFDu;
    }
    if (*i + static_cast<std::size_t>(extra) >= s.size()) {
        ++*i;
        return 0xFFFDu;
    }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char c = byte(*i + static_cast<std::size_t>(k));
        if ((c & 0xC0) != 0x80) {
            ++*i;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (c & 0x3Fu);
    }
    *i += static_cast<std::size_t>(extra) + 1u;
    return cp;
}

// 覆盖率的二值门：M0 只画**两种颜色**（底 / 字），这样窗口像素判据能命中精确值。
// ⚠ 这不是最终排版质量（M4 由宿主字体重绘时再谈抗锯齿）；M0 的承诺只是"中文经宿主字体出字"。
constexpr std::uint8_t kCoverageThreshold = 128;

struct Box {
    int x = std::numeric_limits<int>::max();
    int y = std::numeric_limits<int>::max();
    int right = std::numeric_limits<int>::min();
    int bottom = std::numeric_limits<int>::min();
    bool empty() const { return x >= right || y >= bottom; }
    void include(int l, int t, int r, int b) {
        x = std::min(x, l);
        y = std::min(y, t);
        right = std::max(right, r);
        bottom = std::max(bottom, b);
    }
};

int scaleRef(int ref, double s, bool round_up_min) {
    const int v = static_cast<int>(static_cast<long long>(ref) * s + (ref >= 0 ? 0.5 : -0.5));
    if (round_up_min && v < 1) return 1;
    return v;
}

struct Resolved {
    const Element* element = nullptr;
    int x = 0;  // 窗口像素（绝对）
    int y = 0;
    int w = 0;
    int h = 0;
    int font_px = 1;  // Text 的最终像素字号
};

// 解析一个元素（M0 只做绝对 at / size）。文本尺寸用宿主字体度量（⛔ 不按字符数猜）。
bool resolveOne(const Element& e, const CanvasOrigin& canvas, int picture_x, int picture_y, FontEngine& font,
                Resolved* out, std::string* err) {
    out->element = &e;
    out->x = picture_x + canvas.offset_x + scaleRef(e.x, canvas.scale, false);
    out->y = picture_y + canvas.offset_y + scaleRef(e.y, canvas.scale, false);
    if (e.kind == ElementKind::Rect) {
        out->w = scaleRef(e.w, canvas.scale, true);
        out->h = scaleRef(e.h, canvas.scale, true);
        return true;
    }
    out->font_px = scaleRef(e.font_size, canvas.scale, true);
    int w = 0;
    int h = 0;
    if (!measureText(e.text, out->font_px, e.bold, font, &w, &h, err)) return false;
    out->w = std::max(1, w);
    out->h = std::max(1, h);
    return true;
}

void putPixel(std::vector<std::uint8_t>& rgba, int width, int height, int x, int y, const Color& c) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x)) * 4u;
    rgba[at + 0] = c.r;
    rgba[at + 1] = c.g;
    rgba[at + 2] = c.b;
    rgba[at + 3] = c.a;
}

// 在**绝对窗口像素**坐标 (x0,y0) 处画一行文本（左上角 = 行顶）。返回 false = 字体不可用。
bool drawText(std::vector<std::uint8_t>& rgba, int img_w, int img_h, int origin_x, int origin_y,
              const Element& e, int font_px, FontEngine& font, std::string* err) {
    HostTextMetrics metrics;
    if (!font.metrics(font_px, e.bold, &metrics)) {
        if (err) *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ 无法栅格化 UI 文本";
        return false;
    }
    const int baseline = origin_y + static_cast<int>(metrics.ascent);
    int pen = origin_x;
    std::size_t i = 0;
    while (i < e.text.size()) {
        const char32_t cp = nextCodepoint(e.text, &i);
        GlyphMask glyph;
        if (!font.glyph(cp, font_px, e.bold, &glyph)) {
            if (err) *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ 无法栅格化 UI 文本";
            return false;
        }
        const int ink_x = pen + glyph.origin_x;
        const int ink_y = baseline - glyph.origin_y;
        for (int gy = 0; gy < glyph.black_box_y; ++gy) {
            for (int gx = 0; gx < glyph.black_box_x; ++gx) {
                const std::size_t at = static_cast<std::size_t>(gy) * static_cast<std::size_t>(glyph.black_box_x) +
                                       static_cast<std::size_t>(gx);
                if (at >= glyph.coverage.size()) continue;
                if (glyph.coverage[at] < kCoverageThreshold) continue;
                putPixel(rgba, img_w, img_h, ink_x + gx, ink_y + gy, e.color);
            }
        }
        pen += glyph.advance;
    }
    return true;
}

}  // namespace

bool measureText(const std::string& text, int pixel_size, bool bold, FontEngine& font, int* w, int* h,
                 std::string* err) {
    HostTextMetrics metrics;
    if (!font.metrics(pixel_size, bold, &metrics)) {
        if (err) *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ 无法度量 UI 文本";
        return false;
    }
    int width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = nextCodepoint(text, &i);
        GlyphMask glyph;
        if (!font.glyph(cp, pixel_size, bold, &glyph)) {
            if (err) *err = "宿主字体不可用（LIFT_HOST_FONT_PATH）⇒ 无法度量 UI 文本";
            return false;
        }
        width += glyph.advance;
    }
    if (w != nullptr) *w = width;
    if (h != nullptr) *h = static_cast<int>(metrics.height);
    return true;
}

bool rasterize(const std::vector<Element>& elements, const CanvasOrigin& canvas, int picture_x, int picture_y,
               FontEngine& font, RasterImage* out, std::string* err) {
    if (out == nullptr) {
        if (err) *err = "rasterize: out == nullptr";
        return false;
    }
    out->rgba.clear();
    out->w = 0;
    out->h = 0;
    out->x = 0;
    out->y = 0;
    if (elements.empty()) return true;
    if (canvas.scale <= 0.0) {
        if (err) *err = "rasterize: 画布几何非法（scale <= 0）";
        return false;
    }

    // ① resolve：顺序数组 → 窗口像素 rect（⛔ 不做布局；M0 只有绝对 at/size）。
    std::vector<Resolved> resolved;
    resolved.reserve(elements.size());
    Box box;
    for (const Element& e : elements) {
        Resolved r;
        if (!resolveOne(e, canvas, picture_x, picture_y, font, &r, err)) return false;
        box.include(r.x, r.y, r.x + r.w, r.y + r.h);
        resolved.push_back(r);
    }
    if (box.empty()) return true;
    out->x = box.x;
    out->y = box.y;
    out->w = box.right - box.x;
    out->h = box.bottom - box.y;
    out->rgba.assign(static_cast<std::size_t>(out->w) * static_cast<std::size_t>(out->h) * 4u, 0u);

    // ② 绘制序：z 升序、同 z 按 seq 升序（稳定排序；M0 的 z 全 0 ⇒ 纯构造序）。
    std::stable_sort(resolved.begin(), resolved.end(), [](const Resolved& a, const Resolved& b) {
        if (a.element->z != b.element->z) return a.element->z < b.element->z;
        return a.element->seq < b.element->seq;
    });

    for (const Resolved& r : resolved) {
        const Element& e = *r.element;
        const int ox = r.x - out->x;  // 图像内坐标
        const int oy = r.y - out->y;
        if (e.kind == ElementKind::Rect) {
            for (int y = 0; y < r.h; ++y) {
                for (int x = 0; x < r.w; ++x) putPixel(out->rgba, out->w, out->h, ox + x, oy + y, e.fill);
            }
            continue;
        }
        if (!drawText(out->rgba, out->w, out->h, ox, oy, e, r.font_px, font, err)) return false;
    }
    return true;
}

}  // namespace lift::ui
