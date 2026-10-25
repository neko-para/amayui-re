#include "core/agf.h"

#include "core/lzss.h"

#include <algorithm>

namespace core {
namespace {
std::uint16_t read_u16(ByteSpan bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) throw CoreError("agf-truncated");
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

ByteSpan slice(ByteSpan bytes, std::size_t offset, std::size_t length, const char* code) {
    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw CoreError(std::string(code) + ":" + std::to_string(offset) + "+" +
                        std::to_string(length) + ">" + std::to_string(bytes.size()));
    }
    return bytes.subspan(offset, length);
}

Bytes read_section(
    ByteSpan bytes,
    std::size_t offset,
    std::uint32_t unpacked,
    std::uint32_t packed,
    const char* code) {
    const ByteSpan raw = slice(bytes, offset, packed, code);
    if (unpacked == packed) return Bytes(raw.begin(), raw.end());
    try {
        return decompress_lzss(raw, unpacked);
    } catch (const std::exception& e) {
        throw CoreError(std::string(code) + ":lzss:" + e.what());
    }
}

std::size_t stride_for(std::uint32_t width, std::uint16_t bpp) {
    return 4 * ((static_cast<std::size_t>(bpp) * width + 31) / 32);
}

bool magic_at(ByteSpan bytes, std::size_t offset, const char* magic) {
    for (std::size_t i = 0; i < 4; ++i) {
        if (offset + i >= bytes.size() || bytes[offset + i] != static_cast<std::uint8_t>(magic[i])) {
            return false;
        }
    }
    return true;
}
}  // namespace

DecodedAgf decode_agf(ByteSpan bytes) {
    if (bytes.size() < 24) throw CoreError("agf-too-short");
    if (bytes[0] == 0x42 && bytes[1] == 0x4d) throw CoreError("agf-bmp-unsupported");

    const auto version = read_u32(bytes, 4);
    if (version != 1 && version != 2) throw CoreError("agf-version");
    if (read_u32(bytes, 8) != 0) throw CoreError("agf-unknown0");

    const auto meta_unpacked = read_u32(bytes, 12);
    if (read_u32(bytes, 16) != meta_unpacked) throw CoreError("agf-meta-size:repeated");
    const auto meta_packed = read_u32(bytes, 20);
    const Bytes meta = read_section(bytes, 24, meta_unpacked, meta_packed, "agf-meta-size");
    if (meta.size() < 32) throw CoreError("agf-meta-size:decoded");

    DecodedAgf image;
    image.width = read_u32(meta, 20);
    image.height = read_u32(meta, 24);
    if (image.width == 0 || image.height == 0) throw CoreError("agf-zero-size");
    image.bpp = read_u16(meta, 30);
    if (image.bpp != 8 && image.bpp != 24 && image.bpp != 32) throw CoreError("agf-bpp");

    const std::size_t body_header = 24 + meta_packed;
    slice(bytes, body_header, 12, "agf-body-size");
    const auto body_unpacked = read_u32(bytes, body_header + 4);
    const auto body_packed = read_u32(bytes, body_header + 8);
    const Bytes body = read_section(bytes, body_header + 12, body_unpacked, body_packed, "agf-body-size");

    const std::size_t stride = stride_for(image.width, image.bpp);
    const std::size_t need = stride * image.height;
    if (body.size() != need) throw CoreError("agf-body-size:body");

    Bytes alpha;
    if (version == 2) {
        const std::size_t acif = body_header + 12 + body_packed;
        if (!magic_at(bytes, acif, "ACIF")) throw CoreError("agf-acif-missing");
        slice(bytes, acif, 36, "agf-acif-size");
        const auto acif_version = read_u32(bytes, acif + 4);
        if (acif_version != 1 && acif_version != 2) throw CoreError("agf-acif-version");
        if (read_u32(bytes, acif + 8) != 0) throw CoreError("agf-acif-reserved");
        const auto alpha_unpacked = read_u32(bytes, acif + 12);
        if (read_u32(bytes, acif + 16) != image.width ||
            read_u32(bytes, acif + 20) != image.height) {
            throw CoreError("agf-acif-dimensions");
        }
        if (read_u32(bytes, acif + 24) != alpha_unpacked ||
            read_u32(bytes, acif + 28) != alpha_unpacked) {
            throw CoreError("agf-acif-size:repeated");
        }
        const auto alpha_packed = read_u32(bytes, acif + 32);
        alpha = read_section(bytes, acif + 36, alpha_unpacked, alpha_packed, "agf-acif-size");
        if (alpha.size() != static_cast<std::size_t>(image.width) * image.height) {
            throw CoreError("agf-alpha-size");
        }
    }

    Bytes palette(1024, 0);
    if (image.bpp == 8) {
        if (meta.size() < 56) throw CoreError("agf-meta-size:palette");
        const std::size_t n = std::min<std::size_t>(1024, meta.size() - 56);
        std::copy_n(meta.begin() + 56, n, palette.begin());
    }

    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::size_t src_row = static_cast<std::size_t>(image.height - 1 - y) * stride;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t d = (static_cast<std::size_t>(y) * image.width + x) * 4;
            if (image.bpp == 8) {
                const std::size_t index = body[src_row + x];
                const std::size_t p = index * 4;
                image.rgba[d] = palette[p + 2];
                image.rgba[d + 1] = palette[p + 1];
                image.rgba[d + 2] = palette[p];
                image.rgba[d + 3] = alpha.empty() ? 255 : alpha[static_cast<std::size_t>(y) * image.width + x];
            } else if (image.bpp == 24) {
                const std::size_t s = src_row + x * 3;
                image.rgba[d] = body[s + 2];
                image.rgba[d + 1] = body[s + 1];
                image.rgba[d + 2] = body[s];
                image.rgba[d + 3] = alpha.empty() ? 255 : alpha[static_cast<std::size_t>(y) * image.width + x];
            } else {
                const std::size_t s = src_row + x * 4;
                image.rgba[d] = body[s + 2];
                image.rgba[d + 1] = body[s + 1];
                image.rgba[d + 2] = body[s];
                image.rgba[d + 3] = body[s + 3];
            }
        }
    }
    return image;
}

}  // namespace core
