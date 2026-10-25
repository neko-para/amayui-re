#include "core/save_bmp.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace core {
namespace {

void put_u32(Bytes& out, std::size_t offset, std::uint32_t value) {
    out[offset + 0] = static_cast<std::uint8_t>(value);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    out[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t get_u32(ByteSpan bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t row_bytes_24(std::uint32_t width) {
    return ((width * 24u + 31u) / 32u) * 4u;  // 4 字节对齐
}

std::uint8_t channel_of(std::uint32_t pixel, std::uint32_t mask) {
    if (mask == 0) return 0;
    std::uint32_t shift = 0;
    while (((mask >> shift) & 1u) == 0u && shift < 32u) ++shift;
    const auto value = (pixel & mask) >> shift;
    const auto bits = static_cast<std::uint32_t>(std::popcount(mask));
    // 归一到 8 位（1/4/5/8 位通道都覆盖）。
    if (bits >= 8) return static_cast<std::uint8_t>(value >> (bits - 8));
    return static_cast<std::uint8_t>(value * 255u / ((1u << bits) - 1u));
}

}  // namespace

Bytes encode_thumb_bmp(const SurfacePixels& surface) {
    const std::uint32_t width = surface.width;
    const std::uint32_t height = surface.height;
    const std::uint32_t row = row_bytes_24(width);
    const std::uint32_t pixels_bytes = row * height;
    Bytes out(54u + pixels_bytes, 0u);
    out[0] = 'B';
    out[1] = 'M';
    put_u32(out, 2, pixels_bytes + 40u);  // ★ 声明长度 = 像素字节 + 40（不是 +54）
    put_u32(out, 10, 54u);
    put_u32(out, 14, 40u);
    put_u32(out, 18, width);
    put_u32(out, 22, height);
    std::uint16_t one = 1;
    std::memcpy(out.data() + 26, &one, 2);  // 平面数
    std::uint16_t depth24 = 24;
    std::memcpy(out.data() + 28, &depth24, 2);
    // +30 压缩 / +34 biSizeImage / +38,+42 每米像素 / +46 调色板计数 全 0（Buf 已清零）
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src = surface.pixels.data() + static_cast<std::size_t>(y) * surface.stride_bytes;
        std::uint8_t* dst = out.data() + 54u + static_cast<std::size_t>(y) * row;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* pixel = src + static_cast<std::size_t>(x) * surface.bytes_per_pixel;
            std::uint32_t raw = 0;
            for (std::uint32_t i = 0; i < surface.bytes_per_pixel && i < 4; ++i) {
                raw |= static_cast<std::uint32_t>(pixel[i]) << (8u * i);
            }
            dst[x * 3 + 0] = channel_of(raw, surface.b_mask);
            dst[x * 3 + 1] = channel_of(raw, surface.g_mask);
            dst[x * 3 + 2] = channel_of(raw, surface.r_mask);
        }
        // 行末填充字节保持 0（缓冲清零）
    }
    return out;
}

bool decode_thumb_bmp(ByteSpan bytes, ThumbImage& image) {
    if (bytes.size() < 54) return false;
    if (bytes[0] != 'B' || bytes[1] != 'M') return false;
    if (get_u32(bytes, 14) < 40) return false;
    const std::int32_t width = static_cast<std::int32_t>(get_u32(bytes, 18));
    const std::int32_t height = static_cast<std::int32_t>(get_u32(bytes, 22));
    if (width <= 0 || height == 0) return false;
    const std::uint32_t depth = static_cast<std::uint32_t>(bytes[28]) | (static_cast<std::uint32_t>(bytes[29]) << 8);
    const bool bottom_up = height > 0;
    const auto abs_height = static_cast<std::uint32_t>(height > 0 ? height : -height);
    std::uint32_t palette_entries = 0;
    if (depth == 1 || depth == 4 || depth == 8) {
        const auto given = get_u32(bytes, 46);
        palette_entries = given != 0 ? given : (1u << depth);
        if (bytes.size() < 54u + palette_entries * 4u) return false;
    } else if (depth != 24 && depth != 32) {
        return false;  // 16 位与其它位深都被拒（唯一的内容判据是位深集合）
    }
    const std::uint32_t bpp = depth / 8u;
    const std::uint32_t row = depth < 8u ? ((static_cast<std::uint32_t>(width) * depth + 31u) / 32u) * 4u
                                         : static_cast<std::uint32_t>(width) * bpp;
    const std::uint32_t pixel_bytes = row * abs_height;
    const std::uint32_t offset = 54u + palette_entries * 4u;
    if (bytes.size() < static_cast<std::size_t>(offset) + pixel_bytes) return false;

    image.width = static_cast<std::uint32_t>(width);
    image.height = abs_height;
    image.rgba.assign(static_cast<std::size_t>(width) * abs_height * 4u, 0u);
    for (std::uint32_t y = 0; y < abs_height; ++y) {
        // 行指针反向：BMP 行序自下而上 ⇒ 翻正成 top-down。
        const std::uint32_t source_row = bottom_up ? (abs_height - 1u - y) : y;
        const std::uint8_t* line = bytes.data() + offset + static_cast<std::size_t>(source_row) * row;
        std::uint8_t* dst = image.rgba.data() + static_cast<std::size_t>(y) * width * 4u;
        for (std::int32_t x = 0; x < width; ++x) {
            std::uint8_t r = 0, g = 0, b = 0;
            if (depth == 24 || depth == 32) {
                const std::uint8_t* pixel = line + static_cast<std::size_t>(x) * bpp;
                b = pixel[0];
                g = pixel[1];
                r = pixel[2];
            } else {
                const std::uint32_t bit = static_cast<std::uint32_t>(x) * depth;
                const std::uint32_t byte_index = bit / 8u;
                const std::uint32_t shift = 8u - depth - (bit % 8u);
                const std::uint32_t index = (line[byte_index] >> shift) & ((1u << depth) - 1u);
                const std::uint8_t* entry = bytes.data() + 54u + index * 4u;
                b = entry[0];
                g = entry[1];
                r = entry[2];
            }
            dst[static_cast<std::size_t>(x) * 4u + 0] = r;
            dst[static_cast<std::size_t>(x) * 4u + 1] = g;
            dst[static_cast<std::size_t>(x) * 4u + 2] = b;
            dst[static_cast<std::size_t>(x) * 4u + 3] = 255u;
        }
    }
    return true;
}

}  // namespace core
