#include "core/lzss.h"

#include <array>

namespace core {
namespace {
constexpr std::size_t kWindow = 4096;
constexpr std::size_t kRingMask = kWindow - 1;
}

Bytes decompress_lzss(ByteSpan packed, std::size_t out_length) {
    std::array<std::uint8_t, kWindow> ring{};
    std::size_t r = 0xFEE;
    Bytes out(out_length);
    std::size_t o = 0;
    std::size_t p = 0;

    const auto need = [&](std::size_t n) {
        if (p + n > packed.size()) {
            throw CoreError("lzss-truncated:" + std::to_string(p) + "+" + std::to_string(n) +
                            ">" + std::to_string(packed.size()));
        }
    };

    while (o < out_length) {
        need(1);
        const std::uint8_t flags = packed[p++];
        for (int bit = 0; bit < 8 && o < out_length; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                need(1);
                const std::uint8_t b = packed[p++];
                out[o++] = b;
                ring[r] = b;
                r = (r + 1) & kRingMask;
            } else {
                need(2);
                const std::uint8_t b1 = packed[p++];
                const std::uint8_t b2 = packed[p++];
                const std::size_t pos = b1 | ((static_cast<std::size_t>(b2) & 0xF0u) << 4);
                const std::size_t len = (b2 & 0x0Fu) + 3;
                for (std::size_t i = 0; i < len && o < out_length; ++i) {
                    const std::uint8_t b = ring[(pos + i) & kRingMask];
                    out[o++] = b;
                    ring[r] = b;
                    r = (r + 1) & kRingMask;
                }
            }
        }
    }
    return out;
}

}  // namespace core
