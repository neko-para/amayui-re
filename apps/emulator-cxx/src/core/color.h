// 颜色 / 像素的小工具（**纯函数**，不含引擎状态）。
//
// ★ 为什么要有这个头：`clamp_byte` 原先在 `scene_raster.cpp` / `text_raster.cpp` /
//   `vm_ops_render.cpp` 各有一份逐字节相同的副本；`lerp_argb` 在 `vm_timing.cpp`（生产路径的
//   颜色窗推进）与 `scene_raster.cpp`（软件 oracle）各有一份。后者尤其危险 ——
//   两边必须实现**同一条**插值规则（`CORE-TIMING-WINDOW`：逐通道、截断、越界外推后再夹），
//   各写一份迟早会漂，而 oracle 漂了就没有对照价值了。
//
// ⛔ 这里只放"输入 → 输出"的纯变换；不读场景、不写记录、不判脏。
#pragma once

#include <algorithm>
#include <cstdint>

namespace core {

// 通道值夹到 0..255。
inline std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// ARGB 逐通道插值（截断）。`t` 可以越出 [0,1]：规范只判上界，负延迟 / 未夹取的进度会外推。
inline std::uint32_t lerp_argb(std::uint32_t from, std::uint32_t to, double t) {
    const auto channel = [&](int shift) {
        const double a = static_cast<double>((from >> shift) & 0xFFu);
        const double b = static_cast<double>((to >> shift) & 0xFFu);
        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(a + (b - a) * t), 0, 255));
    };
    return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// 一次 src-over 合成：`dst` 是 RGBA8 的四字节，`r/g/b/alpha` 是**未夹取的**源通道
//（alpha 允许超出 8 位：调用方传什么就按什么算，⛔ 不要先 mask 成 0..255，那会改结果）。
// 口径：逐通道 `(src * a + dst * (255 - a)) / 255`，截断；alpha 也按同一式合成。
// ★ 这条规则同时被「文字写进纹理」与「软件场景合成」用 —— 两处必须逐位一致。
inline void blend_argb_over(std::uint8_t* dst, int r, int g, int b, int alpha) {
    if (alpha <= 0) return;
    dst[0] = clamp_byte((r * alpha + dst[0] * (255 - alpha)) / 255);
    dst[1] = clamp_byte((g * alpha + dst[1] * (255 - alpha)) / 255);
    dst[2] = clamp_byte((b * alpha + dst[2] * (255 - alpha)) / 255);
    dst[3] = clamp_byte(alpha + dst[3] * (255 - alpha) / 255);
}

}  // namespace core
