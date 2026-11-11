// emulator-lift · 黄金图比较器 / PNG 编码的实现（契约见 `runtime/golden_image.h`）
//
// ★ 这里的每一层判据都对应一种**真实的错**（⛔ 不是为了"更严格"）：
//   · 块均值：S1 那类"整层 sprite / Live2D 角色丢掉"会让一批 16×16 块整体突变；
//   · 全图均值：整幅 gamma / 偏色 / 全帧差一档亮度的错；
//   · 超阈像素比例：零散噪点（驱动 / 采样抖动）的兜底。
//   ⇒ 三层是**与**关系；`Report` 逐项留痕，红的时候能直接看出是哪一层、哪个块。
#include "runtime/golden_image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <png.h>

namespace lift {
namespace golden {

namespace {

// 单像素在 R/G/B 上的差（`RgbaImage` 是 PNG 的原生 RGBA 字节序 ⇒ 每像素 4 字节、前 3 个是 R/G/B）。
struct PixelDiff {
    std::uint32_t channel_sum = 0;  // |ΔR| + |ΔG| + |ΔB|
    std::uint32_t max_channel = 0;  // max(|ΔR|, |ΔG|, |ΔB|)
};

inline PixelDiff diffPixel(const std::uint8_t* a, const std::uint8_t* b) {
    PixelDiff d;
    for (std::size_t c = 0; c < 3; ++c) {
        const std::uint32_t av = a[c];
        const std::uint32_t bv = b[c];
        const std::uint32_t delta = av > bv ? av - bv : bv - av;
        d.channel_sum += delta;
        d.max_channel = std::max(d.max_channel, delta);
    }
    return d;
}

}  // namespace

bool compare(const png::RgbaImage& actual, const png::RgbaImage& golden, const Tolerance& tolerance,
             Report* out) {
    if (out == nullptr) return false;
    Report r;

    // ---- 尺寸：宽高一致 + 像素数自洽 ⇒ 否则判红（⛔ 不拿"缩放到一样大"糊过去）----
    const std::uint64_t expected = static_cast<std::uint64_t>(actual.width) * actual.height * 4u;
    r.width = actual.width;
    r.height = actual.height;
    r.size_ok = actual.width != 0 && actual.height != 0 && actual.width == golden.width &&
                actual.height == golden.height && actual.rgba.size() == expected &&
                golden.rgba.size() == expected;
    if (r.size_ok) {
        const std::uint32_t side = tolerance.block_side == 0 ? 16u : tolerance.block_side;
        const std::uint32_t blocks_x = (actual.width + side - 1u) / side;
        const std::uint32_t blocks_y = (actual.height + side - 1u) / side;
        const std::uint64_t block_count = static_cast<std::uint64_t>(blocks_x) * blocks_y;

        std::vector<double> block_max_sum(static_cast<std::size_t>(block_count), 0.0);
        std::vector<std::uint32_t> block_pixels(static_cast<std::size_t>(block_count), 0u);
        std::uint64_t channel_sum = 0;
        std::uint64_t pixel_count = 0;

        for (std::uint32_t y = 0; y < actual.height; ++y) {
            const std::uint32_t by = y / side;
            const std::size_t row = static_cast<std::size_t>(y) * actual.width * 4u;
            for (std::uint32_t x = 0; x < actual.width; ++x) {
                const std::size_t at = row + static_cast<std::size_t>(x) * 4u;
                const PixelDiff d = diffPixel(actual.rgba.data() + at, golden.rgba.data() + at);
                channel_sum += d.channel_sum;
                ++pixel_count;
                if (d.max_channel != 0) ++r.differing_pixels;
                if (d.max_channel > tolerance.outlier_delta) ++r.outlier_pixels;
                r.max_channel_diff = std::max(r.max_channel_diff, d.max_channel);
                const std::size_t block = static_cast<std::size_t>(by) * blocks_x + (x / side);
                block_max_sum[block] += static_cast<double>(d.max_channel);
                ++block_pixels[block];
            }
        }

        r.blocks = static_cast<std::uint32_t>(block_count);
        for (std::uint32_t by = 0; by < blocks_y; ++by) {
            for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                const std::size_t block = static_cast<std::size_t>(by) * blocks_x + bx;
                if (block_pixels[block] == 0) continue;  // 理论上不会：blocks_x/y 是按覆盖算的
                const double mean = block_max_sum[block] / block_pixels[block];
                if (mean > tolerance.block_mean) ++r.failing_blocks;
                if (mean > r.block_mean_worst) {
                    r.block_mean_worst = mean;
                    r.worst_block_x = bx * side;
                    r.worst_block_y = by * side;
                }
            }
        }

        r.global_mean = pixel_count == 0 ? 0.0 : static_cast<double>(channel_sum) / (pixel_count * 3u);
        r.outlier_ratio =
            pixel_count == 0 ? 0.0 : static_cast<double>(r.outlier_pixels) / static_cast<double>(pixel_count);
        r.block_ok = r.failing_blocks == 0;
        r.global_ok = r.global_mean <= tolerance.global_mean;
        r.outlier_ok = r.outlier_ratio <= tolerance.max_outlier_ratio;
    }

    *out = r;
    return r.pass();
}

bool encodeRgba8(const png::RgbaImage& image, std::vector<std::uint8_t>* out, std::string* why) {
    if (out == nullptr) return false;
    out->clear();
    const std::uint64_t expected = static_cast<std::uint64_t>(image.width) * image.height * 4u;
    if (image.width == 0 || image.height == 0 || image.rgba.size() != expected) {
        if (why) *why = "encodeRgba8：宽高 / 像素数不自洽";
        return false;
    }

    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    img.width = image.width;
    img.height = image.height;
    img.format = PNG_FORMAT_RGBA;

    // 简化 API 的两步走：先问长度，再写进调用方的缓冲（⇒ 不落文件、不需要临时目录）。
    png_alloc_size_t size = 0;
    if (png_image_write_to_memory(&img, nullptr, &size, 0, image.rgba.data(), 0, nullptr) == 0 ||
        size == 0) {
        if (why) *why = std::string("encodeRgba8：测量 PNG 长度失败：") + img.message;
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    if (png_image_write_to_memory(&img, out->data(), &size, 0, image.rgba.data(), 0, nullptr) == 0) {
        if (why) *why = std::string("encodeRgba8：写 PNG 失败：") + img.message;
        out->clear();
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    return true;
}

}  // namespace golden
}  // namespace lift
