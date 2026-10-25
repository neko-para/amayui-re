#pragma once

#include "core/bytes.h"

#include <cstddef>

namespace core {

// ALF 目录区的 LZSS：4 KiB 环形窗口，位置是窗口内绝对写位置，窗口初值 0，
// 写指针起点 0xFEE（见 data/spec/chapters/20-alf.md）。
Bytes decompress_lzss(ByteSpan packed, std::size_t out_length);

}  // namespace core
