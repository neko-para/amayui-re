#pragma once

#include "core/bytes.h"

#include <cstdint>

namespace core {

struct DecodedAgf {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t bpp = 0;
    Bytes rgba;  // top-down RGBA8
};

// AGF/ACGF 读取侧；失败抛 CoreError（带稳定 code 前缀）。
DecodedAgf decode_agf(ByteSpan bytes);

}  // namespace core
