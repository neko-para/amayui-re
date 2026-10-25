#include "core/scene_raster.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace core {
namespace {

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// ARGB 逐通道插值（截断）。`t` 可以越出 [0,1]：规范只判上界，负延迟 / 未夹取的进度会外推。
std::uint32_t lerp_argb(std::uint32_t from, std::uint32_t to, double t) {
    const auto channel = [&](int shift) {
        const double a = static_cast<double>((from >> shift) & 0xFFu);
        const double b = static_cast<double>((to >> shift) & 0xFFu);
        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(a + (b - a) * t), 0, 255));
    };
    return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

double lerp_double(double from, double to, double t) { return from + (to - from) * t; }

// 画布里的 32 位字是 **RGBA 字节序**（低字节 = R）；合成 / 插值用 ARGB 数值表达。
std::uint32_t rgba_to_argb(std::uint32_t rgba) {
    return (rgba & 0xFF000000u) | ((rgba & 0xFFu) << 16) | (rgba & 0xFF00u) |
           ((rgba >> 16) & 0xFFu);
}

std::uint32_t argb_to_rgba(std::uint32_t argb) {
    return (argb & 0xFF000000u) | ((argb & 0xFFu) << 16) | (argb & 0xFF00u) |
           ((argb >> 16) & 0xFFu);
}

struct Surface {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Bytes rgba;

    Surface() = default;
    Surface(std::uint32_t w, std::uint32_t h)
        : width(w), height(h), rgba(static_cast<std::size_t>(w) * h * 4, 0) {}

    std::uint32_t* pixel(int x, int y) {
        return reinterpret_cast<std::uint32_t*>(rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4);
    }
    const std::uint32_t* pixel(int x, int y) const {
        return reinterpret_cast<const std::uint32_t*>(rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4);
    }
};

// 源矩形窗（`0x239` / `CORE-OPS-TEXTURE-DRAW-CONSUMER` 4.1）：推进中按步序平移四条边；
// 延迟内按开关字决定「原样 / 塌成空」；到期取末帧或塌成空（回写由推进做）。
void source_rect(const DrawItem& item, std::uint32_t now, bool forced, int& left, int& top,
                 int& right, int& bottom) {
    left = item.left;
    top = item.top;
    right = item.right;
    bottom = item.bottom;
    if ((item.flags & 2u) == 0 || item.rect_duration <= 0) return;
    const int width = std::max(0, right - left);
    const int height = std::max(0, bottom - top);
    const auto start = item.window_start;
    const auto delay = static_cast<std::uint32_t>(item.rect_delay);
    const auto duration = static_cast<std::uint32_t>(item.rect_duration);
    const bool expired = forced || now > start + delay + duration;
    const auto offset = [&](std::int64_t step_index) {
        const int row_steps = item.rect_row_steps;
        if (row_steps <= 0 || item.rect_step_total <= 0) return std::pair<int, int>{0, 0};
        const std::int64_t dx = static_cast<std::int64_t>(width) * (step_index % row_steps);
        const std::int64_t dy = static_cast<std::int64_t>(height) * (step_index / row_steps);
        return std::pair<int, int>{static_cast<int>(dx), static_cast<int>(dy)};
    };
    if (expired) {
        if ((item.rect_switch & 1) != 0) {
            const auto [dx, dy] = offset(item.rect_step_total - 1);
            left += dx; right += dx; top += dy; bottom += dy;
        } else {
            right = left;
            bottom = top;
        }
        return;
    }
    if (now <= start + delay) {
        if ((item.rect_switch & 2) == 0) {  // 开关字第 1 位 = 0 ⇒ 塌成空
            right = left;
            bottom = top;
        }
        return;
    }
    const auto step = static_cast<std::int64_t>(now - start - delay) * item.rect_step_total /
                      std::max<std::int32_t>(1, item.rect_duration);
    const auto [dx, dy] = offset(step);
    left += dx; right += dx; top += dy; bottom += dy;
}

void blend_pixel(Surface& target, int x, int y, std::uint32_t argb) {
    if (x < 0 || x >= static_cast<int>(target.width) || y < 0 || y >= static_cast<int>(target.height)) return;
    const int alpha = static_cast<int>((argb >> 24) & 0xFFu);
    if (alpha <= 0) return;
    const int r = static_cast<int>((argb >> 16) & 0xFFu);
    const int g = static_cast<int>((argb >> 8) & 0xFFu);
    const int b = static_cast<int>(argb & 0xFFu);
    const std::size_t d = (static_cast<std::size_t>(y) * target.width + x) * 4;
    target.rgba[d + 0] = clamp_byte((r * alpha + target.rgba[d + 0] * (255 - alpha)) / 255);
    target.rgba[d + 1] = clamp_byte((g * alpha + target.rgba[d + 1] * (255 - alpha)) / 255);
    target.rgba[d + 2] = clamp_byte((b * alpha + target.rgba[d + 2] * (255 - alpha)) / 255);
    target.rgba[d + 3] = clamp_byte(alpha + target.rgba[d + 3] * (255 - alpha) / 255);
}

void raster_triangle(Surface& target, float ax, float ay, float bx, float by, float cx, float cy,
                     std::uint32_t argb) {
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int max_x = std::min(static_cast<int>(target.width) - 1,
                               static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int max_y = std::min(static_cast<int>(target.height) - 1,
                               static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    const auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0);
    };
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float e0 = edge(ax, ay, bx, by, px, py);
            const float e1 = edge(bx, by, cx, cy, px, py);
            const float e2 = edge(cx, cy, ax, ay, px, py);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                blend_pixel(target, x, y, argb);
            }
        }
    }
}

// 一帧内被访问的键：有符号升序；网格的负键不参与（`CORE-SCENE-DRAW-ORDER`）。
std::vector<std::uint32_t> sorted_keys(const Machine& machine) {
    std::vector<std::uint32_t> keys;
    keys.reserve(machine.draw_items().size() + machine.meshes().size());
    for (const auto& [key, item] : machine.draw_items()) {
        (void)item;
        keys.push_back(key);
    }
    for (const auto& [key, mesh] : machine.meshes()) {
        (void)mesh;
        if (static_cast<std::int32_t>(key) < 0) continue;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(), [](std::uint32_t a, std::uint32_t b) {
        return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b);
    });
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

bool draw_mesh(Machine& machine, std::uint32_t key, Surface& target, std::uint32_t now, bool forced) {
    const auto it = machine.meshes().find(key);
    if (it == machine.meshes().end()) return false;
    const MeshRecord& mesh = it->second;
    if ((mesh.flags & 1u) == 0) return false;
    if (mesh.count != 4 || mesh.copies[0].size() < 4 || mesh.copies[1].size() < 4) return false;
    const auto color = derive_color(mesh, now, forced);
    if ((color >> 24) == 0) return false;
    const auto fx = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[0][i]); };
    const auto fy = [&](std::size_t i) { return std::bit_cast<float>(mesh.copies[1][i]); };
    raster_triangle(target, fx(0), fy(0), fx(1), fy(1), fx(2), fy(2), color);
    raster_triangle(target, fx(1), fy(1), fx(3), fy(3), fx(2), fy(2), color);
    return true;
}

bool draw_item(Machine& machine, std::uint32_t key, Surface& target, std::uint32_t now, bool forced) {
    const auto it = machine.draw_items().find(key);
    if (it == machine.draw_items().end()) return false;
    const DrawItem& item = it->second;
    if ((item.flags & 1u) == 0) return false;
    const TextureRecord* texture = machine.texture(item.texture_id);
    if (texture == nullptr || texture->rgba.empty()) return false;

    int left = 0, top = 0, right = 0, bottom = 0;
    source_rect(item, now, forced, left, top, right, bottom);
    const int sx0 = std::max(0, left);
    const int sy0 = std::max(0, top);
    const int sx1 = std::min(static_cast<int>(texture->width), right);
    const int sy1 = std::min(static_cast<int>(texture->height), bottom);
    if (sx1 <= sx0 || sy1 <= sy0) return false;

    const int dx = static_cast<int>(std::lround(item.aux[0]));
    const int dy = static_cast<int>(std::lround(item.aux[1]));
    const auto item_color = derive_color(item, now, forced);
    const int item_alpha = static_cast<int>((item_color >> 24) & 0xFFu);
    if (item_alpha <= 0) return false;
    const int tint_r = static_cast<int>((item_color >> 16) & 0xFFu);
    const int tint_g = static_cast<int>((item_color >> 8) & 0xFFu);
    const int tint_b = static_cast<int>(item_color & 0xFFu);

    for (int y = sy0; y < sy1; ++y) {
        const int canvas_y = dy + (y - top);
        if (canvas_y < 0 || canvas_y >= static_cast<int>(target.height)) continue;
        for (int x = sx0; x < sx1; ++x) {
            const int canvas_x = dx + (x - left);
            if (canvas_x < 0 || canvas_x >= static_cast<int>(target.width)) continue;
            const std::size_t source = (static_cast<std::size_t>(y) * texture->width + x) * 4;
            const std::size_t destination =
                (static_cast<std::size_t>(canvas_y) * target.width + canvas_x) * 4;
            const int alpha = (static_cast<int>(texture->rgba[source + 3]) * item_alpha) / 255;
            if (alpha == 0) continue;
            const int source_r = texture->rgba[source + 0] * tint_r / 255;
            const int source_g = texture->rgba[source + 1] * tint_g / 255;
            const int source_b = texture->rgba[source + 2] * tint_b / 255;
            target.rgba[destination + 0] =
                clamp_byte((source_r * alpha + target.rgba[destination + 0] * (255 - alpha)) / 255);
            target.rgba[destination + 1] =
                clamp_byte((source_g * alpha + target.rgba[destination + 1] * (255 - alpha)) / 255);
            target.rgba[destination + 2] =
                clamp_byte((source_b * alpha + target.rgba[destination + 2] * (255 - alpha)) / 255);
            target.rgba[destination + 3] =
                clamp_byte(alpha + target.rgba[destination + 3] * (255 - alpha) / 255);
        }
    }
    return true;
}

// 命令档：把一段绘制键区间画进离屏画布，并记「本轮已消费」。
void paint_interval(Machine& machine, Surface& canvas, std::int32_t start, std::int32_t count,
                    std::uint32_t now, bool forced) {
    if (count <= 0) return;
    for (std::int64_t offset = 0; offset < count; ++offset) {
        const auto key = static_cast<std::uint32_t>(static_cast<std::int64_t>(start) + offset);
        draw_mesh(machine, key, canvas, now, forced);
        draw_item(machine, key, canvas, now, forced);
        if (DrawItem* item = machine.draw_item_mut(key)) item->flags |= (1u << 16);
        if (MeshRecord* mesh = machine.mesh_mut(key)) mesh->flags |= (1u << 16);
    }
}

// 把一幅画布按矩形写进纹理（左上角对齐；超出纹理的部分裁掉）。
void composite_plain(Surface& target, const Surface& source, int offset_x, int offset_y,
                     std::uint32_t tint = 0xFFFFFFFFu) {
    const int width = std::min(static_cast<int>(source.width),
                               static_cast<int>(target.width) - offset_x);
    const int height = std::min(static_cast<int>(source.height),
                                static_cast<int>(target.height) - offset_y);
    const int tr = static_cast<int>((tint >> 16) & 0xFFu);
    const int tg = static_cast<int>((tint >> 8) & 0xFFu);
    const int tb = static_cast<int>(tint & 0xFFu);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint32_t argb = rgba_to_argb(*source.pixel(x, y));
            const int a = static_cast<int>((argb >> 24) & 0xFFu);
            if (a == 0) continue;
            const int r = static_cast<int>((argb >> 16) & 0xFFu) * tr / 255;
            const int g = static_cast<int>((argb >> 8) & 0xFFu) * tg / 255;
            const int b = static_cast<int>(argb & 0xFFu) * tb / 255;
            blend_pixel(target, offset_x + x, offset_y + y,
                        (static_cast<std::uint32_t>(a) << 24) |
                            (static_cast<std::uint32_t>(r) << 16) |
                            (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b));
        }
    }
}

// 种类 0 的 Fade：两块画布按进度逐像素合成（低档同形四通道 `c0(1-a)+c1a`）。
void composite_fade(Surface& target, const Surface& first, const Surface& second, double progress) {
    const int width = std::min(static_cast<int>(target.width),
                               std::min(static_cast<int>(first.width), static_cast<int>(second.width)));
    const int height = std::min(static_cast<int>(target.height),
                                std::min(static_cast<int>(first.height), static_cast<int>(second.height)));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint32_t a = rgba_to_argb(*first.pixel(x, y));
            const std::uint32_t b = rgba_to_argb(*second.pixel(x, y));
            *target.pixel(x, y) = argb_to_rgba(lerp_argb(a, b, progress) | 0xFF000000u);
        }
    }
}

// 抹除图案：判定某像素取画布 A 还是 B。12 种图案的边界条件来自
// `CORE-OPS-SCENE-COMMAND-RECORDS` 的条件表；矩形之外的像素由第二块画布覆盖。
bool wipe_uses_first(std::int32_t type, int x, int y, int left, int top, int right, int bottom,
                     std::int32_t split_src, std::uint32_t now, std::uint32_t stamp,
                     std::int32_t delay, std::int32_t duration) {
    const int width = std::max(0, right - left);
    const int height = std::max(0, bottom - top);
    const int split = std::max(1, split_src);
    if (now <= stamp + static_cast<std::uint32_t>(delay)) {
        // 延迟内：位移为 0（第一笔宽 0 ⇒ 全部由第二块画布覆盖）。
        return false;
    }
    const auto elapsed = static_cast<std::int64_t>(now - stamp - static_cast<std::uint32_t>(delay));
    if (duration <= 0) return false;
    const bool horizontal = (type == 0 || type == 1 || type == 4 || type == 5 || type == 8 || type == 9);
    const int axis = horizontal ? width : height;
    if (axis <= 0) return false;

    if (type <= 3) {
        const int steps = axis / split + 2;
        const std::int64_t step = std::max<std::int64_t>(1, duration) / steps;
        const std::int64_t index = step <= 0 ? 0 : elapsed / step;
        const bool from_far = (type % 2) == 1;
        const std::int64_t reveal = static_cast<std::int64_t>(split) * (index + (from_far ? 1 : 0));
        if (type == 0) return (x - left) < reveal;
        if (type == 1) return (x - left) >= (width - reveal);
        if (type == 2) return (y - top) < reveal;
        return (y - top) >= (height - reveal);
    }

    const int strips = axis / split + 1;
    const int remainder = axis % split;
    const std::int64_t divisor = (type <= 7) ? (split + 1) : (split + strips);
    const std::int64_t step = std::max<std::int64_t>(1, duration) / divisor;
    const std::int64_t index = step <= 0 ? 0 : elapsed / step;
    int coordinate = horizontal ? (x - left) : (y - top);
    if (coordinate < 0 || coordinate >= axis) return false;
    int strip = coordinate / split;
    int band_start = strip * split;
    // 条带网格：4/8、6/10 最后一条带贴远边；5/9、7/11 第 0 带贴远边。
    if (remainder != 0) {
        const bool far_anchored = (type == 5 || type == 9 || type == 7 || type == 11);
        if (far_anchored && strip == 0) band_start = -(split - remainder);
        if (!far_anchored && strip == strips - 1) band_start = axis - split;
    }
    std::int64_t reveal = 0;
    if (type <= 7) {
        reveal = std::clamp<std::int64_t>(index, 0, split);
    } else {
        reveal = std::clamp<std::int64_t>(index - strip, 0, split);
    }
    const int band_index = coordinate - band_start;
    switch (type) {
        case 4: case 8: return band_index >= reveal;   // 左→右
        case 5: case 9: return band_index < (split - reveal);  // 右→左
        case 6: case 10: return band_index >= reveal;  // 上→下
        default: return band_index < (split - reveal); // 7 / 11：下→上
    }
}

// 种类 3：SlideBlur（对称 21 抽）与 ZoomBlur（朝中心单侧 10 抽）。
void blur_into(Surface& target, const Surface& source, std::int32_t variant, double length,
               double param_a, double param_b) {
    const int width = std::min(static_cast<int>(target.width), static_cast<int>(source.width));
    const int height = std::min(static_cast<int>(target.height), static_cast<int>(source.height));
    const double max_dim = static_cast<double>(std::max(source.width, source.height));
    if (max_dim <= 0.0) return;
    if (variant == 0) {
        const double radians = param_a * 3.14159265358979323846 / 180.0;
        const double dir_x = std::cos(radians);
        const double dir_y = std::sin(radians);
        const double k = length / max_dim * 0.05;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                double r = 0, g = 0, b = 0, a = 0;
                for (int i = -10; i <= 10; ++i) {
                    const double t = static_cast<double>(i) / 10.0;
                    const int sx = std::clamp(static_cast<int>(std::lround(x + dir_x * t * k)), 0,
                                              static_cast<int>(source.width) - 1);
                    const int sy = std::clamp(static_cast<int>(std::lround(y + dir_y * t * k)), 0,
                                              static_cast<int>(source.height) - 1);
                    const std::uint32_t pixel = rgba_to_argb(*source.pixel(sx, sy));
                    const double weight = 0.0476;
                    r += ((pixel >> 16) & 0xFFu) * weight;
                    g += ((pixel >> 8) & 0xFFu) * weight;
                    b += (pixel & 0xFFu) * weight;
                    a += ((pixel >> 24) & 0xFFu) * weight;
                }
                *target.pixel(x, y) = argb_to_rgba((clamp_byte(static_cast<int>(a)) << 24) |
                                                   (clamp_byte(static_cast<int>(r)) << 16) |
                                                   (clamp_byte(static_cast<int>(g)) << 8) |
                                                   clamp_byte(static_cast<int>(b)));
            }
        }
        return;
    }
    const double center_x = param_a / static_cast<double>(source.width);
    const double center_y = param_b / static_cast<double>(source.height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dx = static_cast<double>(x) / source.width - center_x;
            const double dy = static_cast<double>(y) / source.height - center_y;
            const double magnitude = std::sqrt(dx * dx + dy * dy);
            double r = 0, g = 0, b = 0, a = 0;
            if (magnitude > 1e-9) {
                const double nx = dx / magnitude;
                const double ny = dy / magnitude;
                for (int i = 0; i <= 9; ++i) {
                    const double t = static_cast<double>(i) / 10.0;
                    const double offset = (1.0 / 1024.0) * length * magnitude * t;
                    const int sx = std::clamp(static_cast<int>(std::lround(x + nx * offset)), 0,
                                              static_cast<int>(source.width) - 1);
                    const int sy = std::clamp(static_cast<int>(std::lround(y + ny * offset)), 0,
                                              static_cast<int>(source.height) - 1);
                    const std::uint32_t pixel = rgba_to_argb(*source.pixel(sx, sy));
                    const double weight = 0.1;
                    r += ((pixel >> 16) & 0xFFu) * weight;
                    g += ((pixel >> 8) & 0xFFu) * weight;
                    b += (pixel & 0xFFu) * weight;
                    a += ((pixel >> 24) & 0xFFu) * weight;
                }
            }
            *target.pixel(x, y) = argb_to_rgba((clamp_byte(static_cast<int>(a)) << 24) |
                                               (clamp_byte(static_cast<int>(r)) << 16) |
                                               (clamp_byte(static_cast<int>(g)) << 8) |
                                               clamp_byte(static_cast<int>(b)));
        }
    }
}

// 种类 1 的逐像素遮罩：只乘最高字节（`源高字节 × 系数 / 256`，截断）。
void apply_alpha_factor(Surface& target, int factor) {
    for (std::size_t i = 0; i + 3 < target.rgba.size(); i += 4) {
        target.rgba[i + 3] = static_cast<std::uint8_t>(
            (static_cast<int>(target.rgba[i + 3]) * factor) / 256);
    }
}

// 进度：到期 / 强制完成 ⇒ 1；未开始 ⇒ 0；其余线性（不夹取上界以外）。
double command_progress(const SceneCommandRecord& record, std::uint32_t now, bool forced) {
    if (forced) return 1.0;
    const auto start = record.timestamp + static_cast<std::uint32_t>(record.delay);
    if (now <= start) return 0.0;
    if (record.duration <= 0) return 1.0;
    return static_cast<double>(now - start) / static_cast<double>(record.duration);
}

}  // namespace

std::uint32_t derive_color(const DrawItem& item, std::uint32_t now, bool forced) {
    if ((item.flags & 2u) == 0 || item.duration <= 0) return item.color;
    if (forced) return item.target_color;
    if (item.window_start == 0) return item.color;
    const auto start = item.window_start;
    const auto delay = static_cast<std::uint32_t>(item.delay);
    const auto duration = static_cast<std::uint32_t>(item.duration);
    if (now > start + delay + duration) return item.target_color;
    if (now <= start + delay) return item.color;
    const auto progress = static_cast<double>(now - start - delay) / static_cast<double>(duration);
    return lerp_argb(item.color, item.target_color, progress);
}

std::uint32_t derive_color(const MeshRecord& mesh, std::uint32_t now, bool forced) {
    if ((mesh.flags & 2u) == 0 || mesh.duration <= 0) {
        return mesh.colors.empty() ? mesh.color : mesh.colors[0];
    }
    if (forced) return mesh.target_color;
    if (mesh.window_start == 0) return mesh.color;
    const auto start = mesh.window_start;
    const auto delay = static_cast<std::uint32_t>(mesh.delay);
    const auto duration = static_cast<std::uint32_t>(mesh.duration);
    if (now > start + delay + duration) return mesh.target_color;
    if (now <= start + delay) return mesh.color;
    const auto progress = static_cast<double>(now - start - delay) / static_cast<double>(duration);
    return lerp_argb(mesh.color, mesh.target_color, progress);
}

Bytes rasterize_scene(Machine& machine, std::uint32_t width, std::uint32_t height) {
    machine.begin_render();
    const std::uint32_t now = machine.render_now_ms();
    bool forced = machine.forced_complete();

    Surface layer(width, height);
    Surface canvas_a(width, height);
    Surface canvas_b(width, height);

    // 命令档：按命令键升序；每条命令覆盖一段（或两段）绘制键区间。
    const auto keys = sorted_keys(machine);
    bool abort_command_pass = false;
    if (!machine.scene_commands().empty()) {
        for (auto& [key, record] : machine.scene_commands_mut()) {
            (void)key;
            if (record.timestamp == 0) record.timestamp = now;
            const bool first_frame = record.timestamp == now;
            if (record.kind == 2 && record.wipe_type < 0) {
                machine.force_complete();  // 类型 < 0：置「强制完成」（读取点在绘制期）
                forced = true;
            }
            switch (record.kind) {
                case 0: {
                    paint_interval(machine, canvas_a, record.a_start, record.a_count, now, forced);
                    paint_interval(machine, canvas_b, record.b_start, record.b_count, now, forced);
                    TextureRecord* texture = machine.texture_mut(record.texture_id);
                    if (texture == nullptr) break;
                    double progress = command_progress(record, now, forced);
                    progress = std::clamp(progress, 0.0, 1.0);
                    if (progress >= 1.0) record.duration = 0;
                    else machine.set_command_animating(true);
                    Surface target(texture->width, texture->height);
                    target.rgba = texture->rgba;
                    composite_fade(target, canvas_a, canvas_b, progress);
                    texture->rgba = std::move(target.rgba);
                    texture->revision++;
                    texture->non_file = true;
                    break;
                }
                case 1: {
                    if (first_frame) {
                        paint_interval(machine, canvas_a, record.a_start, record.a_count, now, forced);
                        TextureRecord* texture = machine.texture_mut(record.texture_id);
                        if (texture != nullptr) {
                            Surface target(texture->width, texture->height);
                            target.rgba = texture->rgba;
                            composite_plain(target, canvas_a, 0, 0);
                            apply_alpha_factor(target, record.mask_flag == 1 ? 0 : 255);
                            texture->rgba = std::move(target.rgba);
                    texture->revision++;
                            texture->non_file = true;
                        }
                    } else {
                        // 之后各帧只记「已消费」，零绘制请求。
                        paint_interval(machine, canvas_a, record.a_start, record.a_count, now, forced);
                    }
                    if (record.duration != 0) {
                        machine.set_command_animating(true);
                        record.duration = 0;
                        TextureRecord* texture = machine.texture_mut(record.texture_id);
                        if (texture != nullptr) {
                            Surface target(texture->width, texture->height);
                            target.rgba = texture->rgba;
                            apply_alpha_factor(target, record.mask_flag == 1 ? 255 : 0);
                            texture->rgba = std::move(target.rgba);
                    texture->revision++;
                        }
                    }
                    break;
                }
                case 2: {
                    paint_interval(machine, canvas_a, record.a_start, record.a_count, now, forced);
                    paint_interval(machine, canvas_b, record.b_start, record.b_count, now, forced);
                    DrawItem* rect_item = machine.draw_item_mut(record.key);
                    if (rect_item == nullptr) {
                        record.duration = 0;   // 缺同键绘制项：中止整档
                        abort_command_pass = true;
                        break;
                    }
                    const int left = rect_item->left + static_cast<int>(std::lround(rect_item->aux[0]));
                    const int top = rect_item->top + static_cast<int>(std::lround(rect_item->aux[1]));
                    const int right = rect_item->right + static_cast<int>(std::lround(rect_item->aux[0]));
                    const int bottom = rect_item->bottom + static_cast<int>(std::lround(rect_item->aux[1]));
                    TextureRecord* texture = machine.texture_mut(record.texture_id);
                    if (texture == nullptr) break;
                    Surface target(texture->width, texture->height);
                    target.rgba = texture->rgba;
                    if (record.wipe_type >= 0) {
                        for (std::uint32_t y = 0; y < target.height; ++y) {
                            for (std::uint32_t x = 0; x < target.width; ++x) {
                                const bool inside = static_cast<int>(x) >= left &&
                                                    static_cast<int>(x) < right &&
                                                    static_cast<int>(y) >= top &&
                                                    static_cast<int>(y) < bottom;
                                const bool use_first =
                                    inside && wipe_uses_first(record.wipe_type, static_cast<int>(x),
                                                              static_cast<int>(y), left, top,
                                                              right, bottom, record.split, now,
                                                              record.timestamp, record.delay,
                                                              record.duration);
                                const Surface& source = use_first ? canvas_a : canvas_b;
                                const std::uint32_t src_x = std::min(x, source.width - 1);
                                const std::uint32_t src_y = std::min(y, source.height - 1);
                                *target.pixel(static_cast<int>(x), static_cast<int>(y)) =
                                    *source.pixel(static_cast<int>(src_x), static_cast<int>(src_y));
                            }
                        }
                    }
                    texture->rgba = std::move(target.rgba);
                    texture->revision++;
                    texture->non_file = true;
                    // 到期 ⇒ 持续时间清 0；动画中 ⇒ 置「有动画」。
                    const auto elapsed = now - record.timestamp - static_cast<std::uint32_t>(record.delay);
                    if (record.duration > 0 &&
                        elapsed < static_cast<std::uint32_t>(record.duration)) {
                        machine.set_command_animating(true);
                    } else {
                        record.duration = 0;
                    }
                    break;
                }
                case 3: {
                    paint_interval(machine, canvas_a, record.a_start, record.a_count, now, forced);
                    TextureRecord* texture = machine.texture_mut(record.texture_id);
                    if (texture == nullptr) break;
                    double progress = command_progress(record, now, forced);
                    if (progress >= 1.0) {
                        progress = 1.0;
                        record.duration = 0;
                    } else {
                        machine.set_command_animating(true);
                    }
                    const double length = lerp_double(record.from[0], record.to[0], progress);
                    const double param_a = lerp_double(record.from[1], record.to[1], progress);
                    const double param_b = lerp_double(record.from[2], record.to[2], progress);
                    Surface target(texture->width, texture->height);
                    target.rgba = texture->rgba;
                    blur_into(target, canvas_a, record.variant, length, param_a, param_b);
                    texture->rgba = std::move(target.rgba);
                    texture->revision++;
                    texture->non_file = true;
                    break;
                }
                default:
                    break;
            }
            if (abort_command_pass) break;
        }
    }

    // 收尾扫描：四张同键表里「有效位 = 1 且第 16 位 = 0」的条目，按有符号键升序；
    // 同键平键序 = 辅助表 A → 辅助表 B → 网格 → 绘制项（辅助表暂不产生像素）。
    if (!abort_command_pass) {
        for (const auto key : keys) {
            if (DrawItem* item = machine.draw_item_mut(key)) {
                if ((item->flags & (1u << 16)) != 0) {
                    item->flags &= ~(1u << 16);
                    continue;
                }
            }
            if (MeshRecord* mesh = machine.mesh_mut(key)) {
                if ((mesh->flags & (1u << 16)) != 0) {
                    mesh->flags &= ~(1u << 16);
                    continue;
                }
            }
            draw_mesh(machine, key, layer, now, machine.forced_complete());
            draw_item(machine, key, layer, now, machine.forced_complete());
        }
    }

    // 场景混合：tint = 场景混合颜色（逐通道乘法）；模式 = 0 正常 / 1 加 / 2 乘 / 3 屏。
    // ★ spec 把模式格的像素分支留给平台（`RES-RENDER-BLEND-PIXEL`）⇒ 平台侧映射，
    //   只有 0 = normal 与本实现的约定一致，其余按常见枚举顺序（与 emulator-new 的 Pixi 表同口径）。
    Bytes canvas(static_cast<std::size_t>(width) * height * 4, 0);
    const auto blend = machine.blend_color();
    const bool tinted = blend != kNoColor;
    const int tint_r = tinted ? static_cast<int>((blend >> 16) & 0xFFu) : 255;
    const int tint_g = tinted ? static_cast<int>((blend >> 8) & 0xFFu) : 255;
    const int tint_b = tinted ? static_cast<int>(blend & 0xFFu) : 255;
    const int mode = machine.blend_mode();
    for (std::size_t i = 0; i + 3 < layer.rgba.size(); i += 4) {
        const int sr = layer.rgba[i + 0] * tint_r / 255;
        const int sg = layer.rgba[i + 1] * tint_g / 255;
        const int sb = layer.rgba[i + 2] * tint_b / 255;
        const int sa = layer.rgba[i + 3];
        const int dr = canvas[i + 0];
        const int dg = canvas[i + 1];
        const int db = canvas[i + 2];
        int rr = sr;
        int rg = sg;
        int rb = sb;
        switch (mode) {
            case 1: rr = sr + dr; rg = sg + dg; rb = sb + db; break;
            case 2: rr = sr * dr / 255; rg = sg * dg / 255; rb = sb * db / 255; break;
            case 3:
                rr = 255 - (255 - sr) * (255 - dr) / 255;
                rg = 255 - (255 - sg) * (255 - dg) / 255;
                rb = 255 - (255 - sb) * (255 - db) / 255;
                break;
            default:
                rr = (sr * sa + dr * (255 - sa)) / 255;
                rg = (sg * sa + dg * (255 - sa)) / 255;
                rb = (sb * sa + db * (255 - sa)) / 255;
                break;
        }
        canvas[i + 0] = clamp_byte(rr);
        canvas[i + 1] = clamp_byte(rg);
        canvas[i + 2] = clamp_byte(rb);
        canvas[i + 3] = clamp_byte(sa + canvas[i + 3] * (255 - sa) / 255);
    }

    machine.end_render();
    return canvas;
}

void save_bmp_rgba(const std::string& path, const Bytes& rgba, std::uint32_t width, std::uint32_t height) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw CoreError("bmp-open-failed:" + path);
    const std::uint32_t image_size = width * height * 4;
    const std::uint32_t file_size = 14 + 40 + image_size;
    auto put16 = [&](std::uint16_t v) { out.put(static_cast<char>(v & 0xFF)); out.put(static_cast<char>((v >> 8) & 0xFF)); };
    auto put32 = [&](std::uint32_t v) {
        out.put(static_cast<char>(v & 0xFF)); out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF)); out.put(static_cast<char>((v >> 24) & 0xFF));
    };
    out.put('B'); out.put('M');
    put32(file_size); put16(0); put16(0); put32(14 + 40);
    put32(40); put32(width); put32(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));
    put16(1); put16(32); put32(0); put32(image_size); put32(2835); put32(2835); put32(0); put32(0);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out.put(static_cast<char>(rgba[i + 2]));
        out.put(static_cast<char>(rgba[i + 1]));
        out.put(static_cast<char>(rgba[i + 0]));
        out.put(static_cast<char>(rgba[i + 3]));
    }
}

}  // namespace core
